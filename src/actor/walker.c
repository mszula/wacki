/* src/actor/walker.c — actor motion + waypoint path-finding.
 *
 * Drives the two controllable actors (Ebek, Fjej):
 *
 *   - Per-frame work (UpdateActorMovement): refresh perspective scale
 *     so actors shrink as they walk into scene depth.
 *
 *   - On-click binding (BindActorWalker / BindActorWalkerDirect):
 *     given a target click position, choose a directional walk anim
 *     (L/R/U/D) and patch op 0x15 in the walker bytecode so the
 *     per-entity VM steps the actor toward the target on its next tick.
 *     If a straight-line path is blocked by scene geometry, fall back
 *     to a waypoint chain via the scene's perspective bands.
 *
 *   - Per-frame waypoint advance (PerActorWaypointAdvanceTick): when
 *     a waypoint-routed walker reaches the current leg's target,
 *     bind the next leg. Continues until the walker is on the final
 *     (originally-requested) target leg.
 *
 * The waypoint graph (ActorWaypoints) is built once per scene from
 * the FILD body's perspective bands. Nodes are bands, edges connect
 * band pairs whose straight line is fully walkable. Per-click work
 * adds source/target pseudo-nodes (band IDs WP_BAND_SOURCE /
 * WP_BAND_TARGET), then runs a BFS from the target frontier and
 * picks the source-connected band with minimum total cost.
 */

#include "wacki.h"
#include "internal.h"

#include <stdint.h>
#include <stddef.h>

extern Entity        *g_actor[2];
extern const char    *g_actor_walk_anim[2][6];
extern uint16_t       g_active_actor;
extern uint16_t       g_cursor_speed;
extern uint16_t       g_perspective_min;
extern uint16_t       g_perspective_step;
extern uint16_t       g_settings_anim_active;
extern int16_t        g_persp_profile[];
extern int            g_persp_band_count;

extern int            is_walkable_at(int sx, int sy);
extern void           PlayActorAnimByPath(Entity *e, const char *path,
                                          int16_t target_x, int16_t target_y);

/* ---- entity field offsets used here -------------------------------- */
#define ENT_OFF_FLAGS1          0x08
#define ENT_OFF_ANCHOR_X        0x22
#define ENT_OFF_ANCHOR_Y        0x24
#define ENT_OFF_SCALE_PCT       0x58
#define ENT_OFF_WALKER_DX_REM   0x4C
#define ENT_OFF_WALKER_DY_REM   0x50

#define EFLAGS1_HIDDEN          0x80

/* ---- settings flag bits at g_settings_anim_active ----------------- */
#define ANIM_ACTIVE_ACTORS_ALIVE 0x02

/* Perspective defaults: any change indicates a script-modified
 * perspective bias (used to mute the partner-actor's scale update so
 * the partner doesn't balloon when the active actor is shrunk for a
 * cinematic action shot). */
#define PERSP_DEFAULT_CURSOR_SPEED   0x78
#define PERSP_DEFAULT_MIN            4
#define PERSP_DEFAULT_STEP           7

#define PERSP_BASELINE_Y             400
#define ACTOR_MAX_SCALE              0xA0

/* Script var index that holds the per-actor "frozen" bit. */
#define SCRIPT_VAR_ACTOR_FLAGS_BASE  1
#define SCRIPT_ACTOR_FROZEN_BIT      0x8000

/* ---- direction codes (index into g_actor_walk_anim[actor][..]) ---- */
enum {
    WALK_DIR_LEFT  = 0,
    WALK_DIR_RIGHT = 1,
    WALK_DIR_UP    = 2,
    WALK_DIR_DOWN  = 3,
};

/* ---- waypoint graph parameters ------------------------------------ */
#define WP_MAX_BANDS         0x22          /* 34 bands */
#define WP_MAX_EDGES         0x200         /* 512 edges */
#define WP_BAND_SOURCE       0xFE          /* pseudo-node: actor's start position */
#define WP_BAND_TARGET       0xFF          /* pseudo-node: click destination */

/* The first 4 band slots are reserved for the perimeter that wraps the
 * partner actor when both actors are on the floor (so the active actor
 * routes around the partner). Slots 4..N-1 hold the scene's perspective
 * bands and are the only ones we pre-build edges between in SceneInit. */
#define WP_FIRST_SCENE_BAND  4

#pragma pack(push, 1)
typedef struct WPEdge {
    uint8_t from;
    uint8_t to;
    int32_t dist;
} WPEdge;
#pragma pack(pop)

typedef struct ActorWaypoints {
    WPEdge   edges[WP_MAX_EDGES];

    int16_t  band_x[WP_MAX_BANDS];
    int16_t  band_y[WP_MAX_BANDS];

    int16_t  bfs_state[WP_MAX_BANDS];          /* -1 = unvisited, ≥0 = BFS level */
    int32_t  bfs_dist[WP_MAX_BANDS];           /* -1 = unreached */
    uint8_t  bfs_back[WP_MAX_BANDS];           /* predecessor on shortest path */
    uint8_t  selected_path[WP_MAX_BANDS];      /* chosen hop chain */

    uint16_t edge_count;
    uint16_t baseline_count;                    /* edge_count snapshot after SceneInit */
    int16_t  path_index;                        /* current leg into selected_path */
    int16_t  final_x, final_y;                  /* original click destination */
    uint8_t  path_active;
} ActorWaypoints;

static ActorWaypoints s_wp[2];
static int            s_wp_path_len[2] = { 0, 0 };

/* =================================================================== *
 * UpdateActorMovement.
 *
 * Once-per-frame refresh of each actor's perspective scale, called
 * from ProcessGameFrameTickInner with the current target. (target_x,
 * target_y) used to drive an auto-walker-bind path; that path was
 * removed because spurious mid-pump clicks would rebind the walker to
 * the cursor in the middle of a long blocking animation. The dedicated
 * click handler (HandleSceneInput) is now the sole entry point for
 * starting a walk.
 *
 * Perspective scaling: actors farther up the screen (lower Y) are
 * conceptually farther from the camera and so are drawn smaller.
 * The formula is a linear ramp keyed off (PERSP_BASELINE_Y - anchor_y),
 * clamped to [0, ACTOR_MAX_SCALE]. Scripts can override the baseline /
 * step values via op 0x40 SET_PERSPECTIVE — the "partner" actor's
 * scale is then frozen so a cinematic depth shot of one actor doesn't
 * accidentally inflate the other.
 * =================================================================== */
void UpdateActorMovement(int16_t target_x, int16_t target_y)
{
    (void)target_x;
    (void)target_y;

    if (!(g_settings_anim_active & ANIM_ACTIVE_ACTORS_ALIVE)) return;

    int persp_script_modified =
        (g_cursor_speed     != PERSP_DEFAULT_CURSOR_SPEED) ||
        (g_perspective_min  != PERSP_DEFAULT_MIN) ||
        (g_perspective_step != PERSP_DEFAULT_STEP);

    for (int i = 0; i < 2; ++i) {
        Entity *a = g_actor[i];
        if (!a) continue;
        if (a->flags1 & EFLAGS1_HIDDEN) continue;

        int is_partner = ((int)(g_active_actor & 1u)) != i;
        if (is_partner && persp_script_modified) {
            continue;                       /* keep partner's scale frozen */
        }

        int anchor_y = EOFF(a, ENT_OFF_ANCHOR_Y, int16_t);
        int z = (int)g_cursor_speed -
                ((PERSP_BASELINE_Y - anchor_y) * (int)g_perspective_min) /
                (int)g_perspective_step;
        if (z < 0)              z = 0;
        if (z > ACTOR_MAX_SCALE) z = ACTOR_MAX_SCALE;
        EOFF(a, ENT_OFF_SCALE_PCT, int16_t) = (int16_t)z;

        if (g_script_vars[SCRIPT_VAR_ACTOR_FLAGS_BASE + i] & SCRIPT_ACTOR_FROZEN_BIT) {
            continue;
        }
    }
}

/* =================================================================== *
 * Walkability line tracing — Bresenham-style 16.16 fixed-point.
 *
 * Both helpers walk the line from (sx, sy) to (tx, ty) one pixel at a
 * time; the starting pixel is not checked (assumed to be the actor's
 * own anchor, which they're standing on).
 *
 *   line_reaches: returns 1 iff every intermediate pixel is walkable
 *   line_clip:    same walk, but also returns the last walkable pixel
 * =================================================================== */
static int line_reaches(int sx, int sy, int tx, int ty)
{
    if (sx == tx && sy == ty) return 1;

    int dx = tx - sx, dy = ty - sy;
    int abs_dx = dx < 0 ? -dx : dx;
    int abs_dy = dy < 0 ? -dy : dy;
    int maxlen = abs_dx > abs_dy ? abs_dx : abs_dy;
    if (maxlen == 0) return 1;

    int64_t cx_fp = (int64_t)sx << 16;
    int64_t cy_fp = (int64_t)sy << 16;
    int64_t inc_x = ((int64_t)dx << 16) / maxlen;
    int64_t inc_y = ((int64_t)dy << 16) / maxlen;

    for (int step = 0; step < maxlen; ++step) {
        cx_fp += inc_x;
        cy_fp += inc_y;
        int nx = (int)(cx_fp >> 16);
        int ny = (int)(cy_fp >> 16);
        if (!is_walkable_at(nx, ny)) return 0;
    }
    return 1;
}

static int line_clip(int sx, int sy, int tx, int ty,
                     int *out_clip_x, int *out_clip_y)
{
    int dx = tx - sx, dy = ty - sy;
    int abs_dx = dx < 0 ? -dx : dx;
    int abs_dy = dy < 0 ? -dy : dy;
    int maxlen = abs_dx > abs_dy ? abs_dx : abs_dy;

    if (maxlen == 0) {
        if (out_clip_x) *out_clip_x = sx;
        if (out_clip_y) *out_clip_y = sy;
        return 1;
    }

    int64_t cx_fp = (int64_t)sx << 16;
    int64_t cy_fp = (int64_t)sy << 16;
    int64_t inc_x = ((int64_t)dx << 16) / maxlen;
    int64_t inc_y = ((int64_t)dy << 16) / maxlen;

    int last_x = sx, last_y = sy;
    int reached = 1;
    for (int step = 0; step < maxlen; ++step) {
        cx_fp += inc_x;
        cy_fp += inc_y;
        int nx = (int)(cx_fp >> 16);
        int ny = (int)(cy_fp >> 16);
        if (!is_walkable_at(nx, ny)) { reached = 0; break; }
        last_x = nx;
        last_y = ny;
    }
    if (out_clip_x) *out_clip_x = last_x;
    if (out_clip_y) *out_clip_y = last_y;
    return reached;
}

/* =================================================================== *
 * Waypoint graph building.
 *
 * SceneInit copies the scene's perspective bands into the waypoint
 * slots starting at WP_FIRST_SCENE_BAND, then exhaustively adds edges
 * between every band pair whose straight line is walkable. The first
 * four slots stay zero by default — they're populated on the fly when
 * the BFS needs a partner-actor obstacle perimeter, which our port
 * doesn't currently exercise.
 * =================================================================== */
static void wp_add_edges_from(ActorWaypoints *wp, int16_t cx, int16_t cy,
                              int start_band, uint8_t from_id)
{
    int total = g_persp_band_count;
    if (total > WP_MAX_BANDS) total = WP_MAX_BANDS;

    for (int idx = start_band; idx < total; ++idx) {
        if (wp->edge_count >= WP_MAX_EDGES) break;

        int16_t bx = wp->band_x[idx], by = wp->band_y[idx];
        if (!line_reaches(cx, cy, bx, by)) continue;

        int dx = bx - cx, dy = by - cy;
        int manhattan = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);

        wp->edges[wp->edge_count].from = from_id;
        wp->edges[wp->edge_count].to   = (uint8_t)idx;
        wp->edges[wp->edge_count].dist = manhattan;
        wp->edge_count++;
    }
}

void ActorWaypointsSceneInit(int actor_idx)
{
    if (actor_idx < 0 || actor_idx > 1) return;
    ActorWaypoints *wp = &s_wp[actor_idx];

    wp->edge_count  = 0;
    wp->path_index  = -1;
    wp->path_active = 0;

    /* Perimeter slots stay zero — populated only when a partner-actor
     * obstacle perimeter is active (not currently exercised). */
    for (int i = 0; i < WP_FIRST_SCENE_BAND; ++i) {
        wp->band_x[i] = 0;
        wp->band_y[i] = 0;
    }

    int n = g_persp_band_count;
    if (n > WP_MAX_BANDS) n = WP_MAX_BANDS;
    for (int b = 0; b < n; ++b) {
        wp->band_x[b] = g_persp_profile[b * 2 + 0];
        wp->band_y[b] = g_persp_profile[b * 2 + 1];
    }

    /* Pre-build edges between all scene bands so per-click work only
     * adds source/target pseudo-edges. */
    if (WP_FIRST_SCENE_BAND < n - 1) {
        for (int i = WP_FIRST_SCENE_BAND; i < n - 1; ++i) {
            wp_add_edges_from(wp, wp->band_x[i], wp->band_y[i],
                              i + 1, (uint8_t)i);
        }
    }
    wp->baseline_count = wp->edge_count;
}

/* =================================================================== *
 * Per-click BFS Dijkstra over the waypoint graph.
 *
 * Restores the edge list to its post-SceneInit baseline, adds source
 * (start position) and target (clicked position) pseudo-edges, runs a
 * BFS from the target frontier filling bfs_dist[], then picks the
 * source-connected band with minimum total cost. The reconstructed
 * hop chain lives in selected_path[]; path_index = 0 → walker starts
 * at the source-adjacent band and progresses toward the target.
 *
 * Returns the path length (0 = no path found; caller falls back to the
 * line_clip result).
 * =================================================================== */
static int wp_find_path(ActorWaypoints *wp,
                        int16_t source_x, int16_t source_y,
                        int16_t target_x, int16_t target_y)
{
    int n = g_persp_band_count;
    if (n > WP_MAX_BANDS) n = WP_MAX_BANDS;

    /* Reset to baseline + add the per-click pseudo-edges. */
    wp->edge_count = wp->baseline_count;
    wp_add_edges_from(wp, source_x, source_y, WP_FIRST_SCENE_BAND, WP_BAND_SOURCE);
    wp_add_edges_from(wp, target_x, target_y, 0,                   WP_BAND_TARGET);

    if (n == 0) {
        wp->path_index = -1;
        return 0;
    }

    /* Initial BFS state: all bands unvisited. */
    for (int b = 0; b < n; ++b) {
        wp->bfs_state[b] = -1;
        wp->bfs_dist [b] = -1;
        wp->bfs_back [b] = WP_BAND_TARGET;
    }

    /* Seed frontier: bands directly connected to the target. */
    for (uint16_t e = 0; e < wp->edge_count; ++e) {
        WPEdge *ed   = &wp->edges[e];
        int     band = -1;
        if      (ed->from == WP_BAND_TARGET) band = ed->to;
        else if (ed->to   == WP_BAND_TARGET) band = ed->from;
        if (band >= 0 && band < n) {
            wp->bfs_state[band] = 1;
            wp->bfs_back [band] = WP_BAND_TARGET;
            wp->bfs_dist [band] = ed->dist;
        }
    }

    /* Expand frontier level by level. */
    int level   = 0;
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int b = 0; b < n; ++b) {
            if (wp->bfs_state[b] != level + 1) continue;

            for (uint16_t e = 0; e < wp->edge_count; ++e) {
                WPEdge *ed    = &wp->edges[e];
                int     other = -1;
                if      (ed->from == b) other = ed->to;
                else if (ed->to   == b) other = ed->from;
                if (other < 0 || other >= n) continue;

                int32_t new_dist = wp->bfs_dist[b] + ed->dist;
                if (wp->bfs_state[other] == -1) {
                    wp->bfs_state[other] = level + 2;
                    wp->bfs_back [other] = (uint8_t)b;
                    wp->bfs_dist [other] = new_dist;
                    changed = 1;
                } else if ((uint32_t)new_dist < (uint32_t)wp->bfs_dist[other]) {
                    wp->bfs_dist [other] = new_dist;
                    wp->bfs_back [other] = (uint8_t)b;
                }
            }
        }
        level++;
    }

    /* Pick the source-connected band with the lowest total cost. */
    uint32_t best_total = 0xFFFFFFFFu;
    int      best_band  = -1;
    for (uint16_t e = 0; e < wp->edge_count; ++e) {
        WPEdge *ed   = &wp->edges[e];
        int     band = -1;
        if      (ed->from == WP_BAND_SOURCE) band = ed->to;
        else if (ed->to   == WP_BAND_SOURCE) band = ed->from;
        if (band < 0 || band >= n) continue;
        if (wp->bfs_dist[band] < 0) continue;

        uint32_t total = (uint32_t)wp->bfs_dist[band] + (uint32_t)ed->dist;
        if (total < best_total) {
            best_total = total;
            best_band  = band;
        }
    }
    if (best_band < 0) {
        wp->path_index = -1;
        return 0;
    }

    /* Walk the predecessor chain: selected_path[0] = source-side band,
     * selected_path[last] = target-side band. */
    int idx = 0;
    int cur = best_band;
    while (cur != WP_BAND_TARGET && idx < WP_MAX_BANDS) {
        wp->selected_path[idx++] = (uint8_t)cur;
        cur = wp->bfs_back[cur];
    }
    wp->path_index = 0;
    return idx;
}

/* Forward decls — used both ways between BindActorWalker and the
 * waypoint advance tick. */
int BindActorWalker      (int actor_idx, int target_x, int target_y);
int BindActorWalkerDirect(int actor_idx, int target_x, int target_y);

/* =================================================================== *
 * Per-frame waypoint advance.
 *
 * When a waypoint-routed walker drains (both walker remainders zeroed),
 * advance to the next leg in selected_path[]. Once the index runs past
 * the last waypoint, bind the actor to the original (clicked) target
 * for the final approach.
 * =================================================================== */
void PerActorWaypointAdvanceTick(void)
{
    for (int i = 0; i < 2; ++i) {
        ActorWaypoints *wp = &s_wp[i];
        if (!wp->path_active || !g_actor[i]) continue;

        uint32_t wdx = EOFF(g_actor[i], ENT_OFF_WALKER_DX_REM, uint32_t);
        uint32_t wdy = EOFF(g_actor[i], ENT_OFF_WALKER_DY_REM, uint32_t);
        if (wdx != 0 || wdy != 0) continue;     /* walker still moving */

        wp->path_index++;

        int16_t next_x, next_y;
        int     is_final_leg = 0;
        if (wp->path_index >= s_wp_path_len[i]) {
            next_x = wp->final_x;
            next_y = wp->final_y;
            wp->path_active = 0;
            is_final_leg    = 1;
        } else {
            int     n    = g_persp_band_count;
            uint8_t band = wp->selected_path[wp->path_index];
            if (band >= n) {
                wp->path_active = 0;
                continue;
            }
            next_x = wp->band_x[band];
            next_y = wp->band_y[band];
        }

        int16_t cur_x = EOFF(g_actor[i], ENT_OFF_ANCHOR_X, int16_t);
        int16_t cur_y = EOFF(g_actor[i], ENT_OFF_ANCHOR_Y, int16_t);
        if (next_x == cur_x && next_y == cur_y) continue;

        if (is_final_leg) {
            BindActorWalker      (i, next_x, next_y);
        } else {
            BindActorWalkerDirect(i, next_x, next_y);
        }
    }
}

/* =================================================================== *
 * Direction selector for a vector (dx, dy).
 *
 * Picks left/right/up/down based on which axis dominates the magnitude
 * of the click vector. The result is an index into
 * g_actor_walk_anim[actor][..] which holds the walk anim path per
 * direction.
 * =================================================================== */
static int walk_direction_for(int dx, int dy)
{
    int abs_dx = dx < 0 ? -dx : dx;
    int abs_dy = dy < 0 ? -dy : dy;
    if (abs_dy < abs_dx) {
        return (dx > 0) ? WALK_DIR_RIGHT : WALK_DIR_LEFT;
    } else {
        return (dy > 0) ? WALK_DIR_DOWN : WALK_DIR_UP;
    }
}

/* Step from the clicked pixel toward the actor's anchor until we find
 * a walkable pixel. Used as Phase 1 of BindActorWalker when the click
 * lands on non-walkable scenery (the user wants the actor to walk
 * "as close as possible" to the click). */
static void backstep_to_walkable(int sx, int sy, int *tx_io, int *ty_io)
{
    int tx = *tx_io;
    int ty = *ty_io;

    int dx = sx - tx, dy = sy - ty;
    int abs_dx = dx < 0 ? -dx : dx;
    int abs_dy = dy < 0 ? -dy : dy;
    int maxlen = abs_dx > abs_dy ? abs_dx : abs_dy;
    if (maxlen <= 0) return;

    int64_t cx_fp = (int64_t)tx << 16;
    int64_t cy_fp = (int64_t)ty << 16;
    int64_t inc_x = ((int64_t)dx << 16) / maxlen;
    int64_t inc_y = ((int64_t)dy << 16) / maxlen;

    for (int s = 0; s < maxlen; ++s) {
        int nx = (int)(cx_fp >> 16);
        int ny = (int)(cy_fp >> 16);
        if (is_walkable_at(nx, ny)) {
            *tx_io = nx;
            *ty_io = ny;
            return;
        }
        cx_fp += inc_x;
        cy_fp += inc_y;
    }
}

/* =================================================================== *
 * BindActorWalkerDirect — bind walker without path-finding.
 *
 * Picks a direction anim and patches op 0x15 in the bytecode with the
 * resolved target. Caller is responsible for ensuring (target_x,
 * target_y) is reachable (used by waypoint advance and by callers that
 * pre-clipped via line_clip).
 * =================================================================== */
int BindActorWalkerDirect(int actor_idx, int target_x, int target_y)
{
    if (actor_idx < 0 || actor_idx > 1) return 0;
    Entity *a = g_actor[actor_idx];
    if (!a) return 0;

    int16_t anchor_x = EOFF(a, ENT_OFF_ANCHOR_X, int16_t);
    int16_t anchor_y = EOFF(a, ENT_OFF_ANCHOR_Y, int16_t);
    int     dir      = walk_direction_for(target_x - anchor_x, target_y - anchor_y);

    const char *anim_path = g_actor_walk_anim[actor_idx][dir];
    if (!anim_path) return 0;

    PlayActorAnimByPath(a, anim_path, (int16_t)target_x, (int16_t)target_y);
    return 1;
}

/* =================================================================== *
 * BindActorWalker — full three-phase pathfinding entry point.
 *
 *   Phase 1: if the target pixel itself is non-walkable, walk the line
 *            from target toward source until we hit a walkable pixel.
 *            (User clicked on scenery — go as close as we can.)
 *
 *   Phase 2: walk the line from source to target, recording the last
 *            walkable pixel along the way. If we reach the target, no
 *            path-finding is needed.
 *
 *   Phase 3: if the line clipped early, run the BFS Dijkstra over the
 *            waypoint graph. Bind the walker to the first hop;
 *            PerActorWaypointAdvanceTick takes over from there.
 *
 *   Fallback: if Phase 3 finds no path either, just bind to the
 *             Phase-2 clip point (the actor walks as far as they can).
 * =================================================================== */
int BindActorWalker(int actor_idx, int target_x, int target_y)
{
    if (actor_idx < 0 || actor_idx > 1) return 0;
    Entity *a = g_actor[actor_idx];
    if (!a) return 0;

    int sx = (int)EOFF(a, ENT_OFF_ANCHOR_X, int16_t);
    int sy = (int)EOFF(a, ENT_OFF_ANCHOR_Y, int16_t);
    int tx = target_x;
    int ty = target_y;

    ActorWaypoints *wp = &s_wp[actor_idx];
    /* New bind = fresh intent — drop any in-flight waypoint chain. */
    wp->path_active = 0;
    wp->path_index  = -1;

    if (!is_walkable_at(tx, ty)) {
        backstep_to_walkable(sx, sy, &tx, &ty);
    }

    int clip_x = sx, clip_y = sy;
    int reached = line_clip(sx, sy, tx, ty, &clip_x, &clip_y);

    if (!reached && g_persp_band_count > 0) {
        int path_len = wp_find_path(wp, (int16_t)sx, (int16_t)sy,
                                        (int16_t)tx, (int16_t)ty);
        if (path_len > 0) {
            uint8_t band = wp->selected_path[wp->path_index];
            int     n    = g_persp_band_count;
            if (band < n) {
                wp->final_x               = (int16_t)tx;
                wp->final_y               = (int16_t)ty;
                wp->path_active           = 1;
                s_wp_path_len[actor_idx]  = path_len;
                return BindActorWalkerDirect(actor_idx,
                                             wp->band_x[band], wp->band_y[band]);
            }
        }
    }

    /* NOTE: scenes with walkable pixels under visible obstacles
     * (kind=3 walk-behind sprites flagged 0x8008) aren't blocked here
     * — our port only consults the single bg-mask-setup FLD bitmap.
     * BFS may route a "shortcut through a building" in those rare
     * scenes. Full fix requires porting the per-entity walkability
     * overlay chain; documented for future work. */
    return BindActorWalkerDirect(actor_idx, clip_x, clip_y);
}
