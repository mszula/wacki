/* src/actor/render.c — entity z-sorted render walk + walk-behind mask.
 *
 * Once per frame, after the per-entity VM has updated every entity's
 * position/frame, EntityRenderAll:
 *
 * 1. Collects all entities into a working pointer array
 * 2. Sorts them by foot_y (lower foot = drawn first = behind)
 * 3. Walks back-to-front blitting each into the back buffer
 *
 * Special cases the walk handles inline:
 *
 * - One-shot BG blit (flags 0x40 | 0x20): paints opaquely and clears
 * bit 0x20 so subsequent frames skip. Used for room-background
 * sprites in komnaty whose .pic is a 1×1 palette stub.
 *
 * - Walk-behind mask (.msk asset, kind=0, flag_22 bit 1 clear):
 * restores scene background pixels through the mask shape, but
 * only over pixels we know an actor wrote. Achieves the "actor
 * walks behind kiosk" effect without needing per-pixel depth.
 *
 * - Perspective-scaled sprites (+0x58 ≠ 0/100): blit at scaled size,
 * and recompute drawn position from anchor + scaled atlas hot-spot
 * so the foot stays put across scale changes.
 *
 * - Alpha-plane sprites (flags 0x100): use the dedicated alpha-plane
 * blit path with tint blending.
 *
 * The actor-paint mask (s_actor_paint_mask) is a 1-byte-per-pixel
 * scratch buffer that records every pixel an actor (g_actor[0]/[1])
 * or "sky" entity wrote this frame. Walk-behind only restores BG
 * over those pixels — static foreground props with foot_y below the
 * walkable area are not affected.
 */

#include "wacki.h"
#include "internal.h"

#include <SDL.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern Entity *g_actor[2];

/* ---- entity primary-flag bits at +0x08 ----------------------------- */
#define EFLAG_HIDDEN          0x0080   /* HIDE_ENTITY sets, SHOW clears */
#define EFLAG_FADE_OR_BG      0x0040   /* fade-out OR one-shot BG paint */
#define EFLAG_ONESHOT_BG_PEND 0x0020   /* paired with EFLAG_FADE_OR_BG */
#define EFLAG_DOUBLED         0x0004   /* sprite drawn at 2× (flip alt) */
#define EFLAG_ALPHA_PLANE     0x0100   /* alpha-plane blit */
#define EFLAG_SKY             0x2000   /* genuine sky entity (walk-behind) */
#define EFLAG_PENDING_FREE    0x8000

/* ---- entity state-flag bits at +0x3A ------------------------------- */
#define ESTATE_FOOT_ANCHORED  0x02

/* ---- field offsets used here ---------------------------------------- */
#define ENT_OFF_DRAWN_X       0x0A
#define ENT_OFF_DRAWN_Y       0x0C
#define ENT_OFF_ANCHOR_X      0x22
#define ENT_OFF_ANCHOR_Y      0x24
#define ENT_OFF_FOOT_Y        0x26
#define ENT_OFF_ATLAS_SLOT    0x28
#define ENT_OFF_FRAME         0x30
#define ENT_OFF_STATE_FLAGS   0x3A
#define ENT_OFF_SCALE_PCT     0x58

/* ---- z-sort settings ------------------------------------------------ */
#define MAX_RENDER_ENTITIES   128
#define BG_HEADER_SIZE        776     /* .pic header bytes before pixel data */
#define SCENE_BG_WIDTH_OFFSET   4
#define SCENE_BG_HEIGHT_OFFSET  6

/* ---- one-shot BG persistence heuristic ----------------------------- */
/* HUD panel atlases (small button strips at the bottom of the screen)
 * also use the one-shot blit path, but we must NOT cache them as the
 * persistent scene background. Heuristic: only cache when the sprite
 * spans at least half the play-area width AND its top is high enough
 * to suggest it covers the upper part of the screen. */
#define BG_PERSIST_MIN_WIDTH  320
#define BG_PERSIST_TOP_LIMIT  200
#define BG_PERSIST_BOT_LIMIT  380

/* ---- viewport off-screen guard (generous on the right edge so
 * mid-flight off-screen sprites still render their tail) ------- */
#define VIEWPORT_RIGHT_PAD     200
#define VIEWPORT_BOTTOM_PAD    200

extern void  *g_scene_bg_raw;
extern uint32_t g_scene_bg_size;
extern int16_t g_walk_fld_oy;

/* ---- depth-sort comparator ----------------------------------------- *
 *
 * Sorts entities by their cached foot_y at +0x26 (= drawn_y + height),
 * computed by the per-entity VM post-exec block. Lower foot = farther
 * from camera = drawn first.
 *
 * Fallback to the anchor Y at +0x24 when +0x26 hasn't been set yet
 * (freshly-spawned entities before their first VM tick, or entities
 * carrying the "no foot bake" flag). Without the fallback, fresh
 * sprites would all sort to position 0 and overdraw incorrectly. */
static int cmp_entity_y(const void *a, const void *b)
{
    Entity *ea = *(Entity *const *)a;
    Entity *eb = *(Entity *const *)b;

    int ay = (int)EOFF(ea, ENT_OFF_FOOT_Y, int16_t);
    int by = (int)EOFF(eb, ENT_OFF_FOOT_Y, int16_t);
    if (ay == 0) ay = (int)EOFF(ea, ENT_OFF_ANCHOR_Y, int16_t);
    if (by == 0) by = (int)EOFF(eb, ENT_OFF_ANCHOR_Y, int16_t);
    return ay - by;
}

/* ---- actor-paint scratch mask --------------------------------------- *
 *
 * 1 byte per backbuffer pixel; set to 1 when an actor (or sky entity)
 * writes a non-transparent pixel this frame. Walk-behind masks consult
 * this so they only restore BG pixels where an actor has been drawn —
 * static scene props are unaffected. */
static uint8_t *s_actor_paint_mask = NULL;
static int      s_actor_paint_sz   = 0;

static void ensure_actor_paint_mask(void)
{
    int need = (int)g_screen_w * g_screen_h;
    if (need > s_actor_paint_sz) {
        free(s_actor_paint_mask);
        s_actor_paint_mask = (uint8_t *)malloc((size_t)need);
        s_actor_paint_sz   = s_actor_paint_mask ? need : 0;
    }
    if (s_actor_paint_mask) {
        memset(s_actor_paint_mask, 0, (size_t)s_actor_paint_sz);
    }
}

/* Stamp the actor-paint mask over a rect (clipped to the screen). */
static void stamp_paint_mask(int x0, int y0, int w, int h)
{
    if (!s_actor_paint_mask) return;
    int x1 = x0 + w;
    int y1 = y0 + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)g_screen_w) x1 = (int)g_screen_w;
    if (y1 > (int)g_screen_h) y1 = (int)g_screen_h;
    for (int y = y0; y < y1; ++y) {
        uint8_t *row = s_actor_paint_mask + (size_t)y * g_screen_w;
        for (int x = x0; x < x1; ++x) row[x] = 1;
    }
}

/* ---- one-shot BG sprite (flag 0x40 | 0x20) -------------------------- *
 *
 * Some komnaty ship a 1×1 stub .pic and rely on a regular entity
 * spawn to paint the actual background — this fires once on the first
 * tick after the entity is spawned, then clears its own pending bit so
 * subsequent frames don't repaint. We also stash the atlas pixels via
 * SaveSceneBgAtlas so the per-frame BG repaint can find them.
 *
 * The size heuristic prevents HUD panel strips (which also use the
 * flag-0x40|0x20 path) from being cached as the scene BG. */
static void paint_oneshot_bg(Entity *e, uint16_t flags, AnimAsset *atlas)
{
    if (!atlas || !atlas->frame_count || !atlas->pixel_ptrs) return;

    uint16_t f = EOFF(e, ENT_OFF_FRAME, uint16_t);
    if (f >= atlas->frame_count) f = 0;

    uint8_t *px = atlas->pixel_ptrs[f];
    if (!px) return;

    uint16_t fw = atlas->off_widths [f];
    uint16_t fh = atlas->off_heights[f];
    int16_t  bx = (int16_t)EOFF(e, ENT_OFF_DRAWN_X, int16_t);
    int16_t  by = (int16_t)EOFF(e, ENT_OFF_DRAWN_Y, int16_t);

    PaintImageToBackbuffer(bx, by, fw, fh, px);
    EOFF(e, 8, uint16_t) = (uint16_t)(flags & ~(uint16_t)EFLAG_ONESHOT_BG_PEND);
    FlushFrameToPrimary();

    int top_high_enough = (by < BG_PERSIST_TOP_LIMIT) ||
                          ((by + fh) >= BG_PERSIST_BOT_LIMIT);
    if (fw >= BG_PERSIST_MIN_WIDTH && top_high_enough) {
        SaveSceneBgAtlas(bx, by, fw, fh, px);
    }
}

/* ---- walk-behind mask: 1bpp shape → BG-through-mask blit ------------ *
 *
 * Mask assets carry their shape as 1bpp packed pixels (MSB-first,
 * stride = (w + 7) / 8). The blit walks the mask shape, and for every
 * "set" bit copies the BG pixel from the same global (x, y) onto the
 * backbuffer — but ONLY if the actor-paint mask shows an actor wrote
 * there this frame. That gives us the "actor walks behind kiosk"
 * effect without any per-pixel depth buffer.
 *
 * Skipped at non-trivial scale because perspective-scaled masks would
 * need additional sampling plumbing; no shipped mask asset uses a
 * scale flag, so this is safe. */
static void paint_walkbehind_mask(int16_t dx, int16_t dy,
                                  uint16_t fw, uint16_t fh,
                                  const uint8_t *mask_px)
{
    if (!g_scene_bg_raw || g_scene_bg_size <= BG_HEADER_SIZE) return;

    const uint8_t *bg        = (const uint8_t *)g_scene_bg_raw;
    uint16_t       bg_w      = (uint16_t)(bg[SCENE_BG_WIDTH_OFFSET]  | (bg[SCENE_BG_WIDTH_OFFSET  + 1] << 8));
    uint16_t       bg_h      = (uint16_t)(bg[SCENE_BG_HEIGHT_OFFSET] | (bg[SCENE_BG_HEIGHT_OFFSET + 1] << 8));
    const uint8_t *bg_pixels = bg + BG_HEADER_SIZE;
    int            stride    = (fw + 7) / 8;

    for (int my = 0; my < (int)fh; ++my) {
        int gy = dy + my;
        if (gy < 0 || gy >= (int)bg_h) continue;
        if (gy >= (int)g_screen_h) break;

        const uint8_t *row_mask  = mask_px + (size_t)my * stride;
        const uint8_t *bg_row    = bg_pixels + (size_t)gy * bg_w;
        uint8_t       *dst_row   = g_back_shadow + (size_t)gy * g_screen_w;
        const uint8_t *paint_row = s_actor_paint_mask
                                 ? s_actor_paint_mask + (size_t)gy * g_screen_w
                                 : NULL;

        for (int mx = 0; mx < (int)fw; ++mx) {
            int gx = dx + mx;
            if (gx < 0) continue;
            if (gx >= (int)bg_w || gx >= (int)g_screen_w) break;

            uint8_t bit = (uint8_t)(row_mask[mx >> 3] & (0x80u >> (mx & 7)));
            if (!bit) continue;
            if (paint_row && !paint_row[gx]) continue;
            dst_row[gx] = bg_row[gx];
        }
    }
}

/* ---- main render walk ---------------------------------------------- */

void EntityRenderAll(Entity *head)
{
    (void)head;

    /* Collect → sort. The cap is comfortable: a fully-populated
 * komnata typically holds 30-50 entities. */
    Entity *list[MAX_RENDER_ENTITIES];
    int n = 0;
    int total = EntityListCount(/*click_list=*/0);
    for (int i = 0; i < total && n < MAX_RENDER_ENTITIES; ++i) {
        Entity *e = EntityListAt(/*click_list=*/0, i);
        if (e) list[n++] = e;
    }
    if (n > 1) qsort(list, n, sizeof(Entity *), cmp_entity_y);

    ensure_actor_paint_mask();

    for (int i = 0; i < n; ++i) {
        Entity  *e     = list[i];
        uint16_t flags = EOFF(e, 8, uint16_t);

        if (flags & EFLAG_HIDDEN) continue;

        if (flags & EFLAG_FADE_OR_BG) {
            /* Sub-branch: with EFLAG_ONESHOT_BG_PEND we still owe a
 * one-time backbuffer paint; otherwise this entity is in a
 * "stay invisible" state (genuine fade-out or post-paint
 * idle) and we skip render entirely. */
            if (flags & EFLAG_ONESHOT_BG_PEND) {
                AnimAsset *atlas = (AnimAsset *)ent_ptr_resolve(
                    EOFF(e, ENT_OFF_ATLAS_SLOT, uint32_t));
                paint_oneshot_bg(e, flags, atlas);
            }
            continue;
        }
        if (flags & EFLAG_PENDING_FREE) continue;

        AnimAsset *atlas = (AnimAsset *)ent_ptr_resolve(
            EOFF(e, ENT_OFF_ATLAS_SLOT, uint32_t));

        /* kind=1 entities (speech balloons from main-VM SHOW_TEXT)
 * have no atlas — pixels live directly on the Entity. Speech
 * balloons are dead code in the shipped game (audio-only
 * dialogue), but the branch is here in case future scripts
 * activate it. */
        if (!atlas) {
            if (!e->pixels || !e->width || !e->height) continue;
            int16_t bx = (int16_t)EOFF(e, ENT_OFF_DRAWN_X, int16_t);
            int16_t by = (int16_t)EOFF(e, ENT_OFF_DRAWN_Y, int16_t);
            BlitSpriteToBackbuffer((uint16_t)bx, (uint16_t)by,
                                   0, 0, e->width, e->height,
                                   e->width, e->height,
                                   e->pixels, 0);
            continue;
        }

        if (atlas->frame_count == 0) continue;
        uint16_t f = EOFF(e, ENT_OFF_FRAME, uint16_t);
        if (f >= atlas->frame_count) f = 0;

        uint8_t *px = atlas->pixel_ptrs ? atlas->pixel_ptrs[f] : NULL;
        if (!px) continue;

        uint16_t fw = atlas->off_widths [f];
        uint16_t fh = atlas->off_heights[f];

        /* Drawn position: per-entity VM has written this for sprites
 * with a script. Static scene props (drut, barstoi, pies) often
 * don't have a script and leave +0x0A/+0x0C at zero — fall back
 * to the atlas hot-spot which IS their scene position. */
        int16_t dx = (int16_t)EOFF(e, ENT_OFF_DRAWN_X, int16_t);
        int16_t dy = (int16_t)EOFF(e, ENT_OFF_DRAWN_Y, int16_t);
        if (dx == 0 && dy == 0 && atlas->off_drawX && atlas->off_drawY) {
            dx = (int16_t)atlas->off_drawX[f];
            dy = (int16_t)atlas->off_drawY[f];
        }

        /* Off-screen cull. Generous on right/bottom so mid-flight
 * tail-off-screen sprites still render their visible portion. */
        if ((int)dx + (int)fw <= 0) continue;
        if ((int)dy + (int)fh <= 0) continue;
        if (dx >= (int)g_screen_w + VIEWPORT_RIGHT_PAD)  continue;
        if (dy >= (int)g_screen_h + VIEWPORT_BOTTOM_PAD) continue;

        uint16_t scale = EOFF(e, ENT_OFF_SCALE_PCT, uint16_t);

        /* RLE-decode kind=3 frames into per-call scratch. */
        if (atlas->kind == 3) {
            int need = (int)fw * (int)fh;
            static uint8_t *s_scratch    = NULL;
            static int      s_scratch_sz = 0;
            if (need > s_scratch_sz) {
                free(s_scratch);
                s_scratch    = (uint8_t *)malloc((size_t)need);
                s_scratch_sz = s_scratch ? need : 0;
            }
            if (!s_scratch) continue;
            DepackRleFrame(px, s_scratch, need);
            px = s_scratch;
        }

        /* Walk-behind mask: 1bpp .msk assets blit the BG through the
 * mask shape instead of their own pixels. Only at trivial
 * scale (none of the shipped masks carry a scale flag). */
        if (atlas->kind == 0 && (atlas->flag_22 & 2) == 0 &&
            (scale == 0 || scale == 100)) {
            paint_walkbehind_mask(dx, dy, fw, fh, px);
            continue;
        }

        /* ---- the regular sprite blit -------------------------------- */
        int blit_w, blit_h;

        if (scale && scale != 100) {
            /* Perspective-scaled. Recompute drawn from anchor +
 * scaled atlas hot-spot for foot-anchored entities, so the
 * foot stays put even when the scale changes between the
 * VM-tick anchor-set and this render. */
            uint16_t dw = (uint16_t)((fw * scale) / 100);
            uint16_t dh = (uint16_t)((fh * scale) / 100);
            if (dw == 0) dw = 1;
            if (dh == 0) dh = 1;

            int has_foot_anchor =
                (EOFF(e, ENT_OFF_STATE_FLAGS, uint16_t) & ESTATE_FOOT_ANCHORED) &&
                atlas->off_drawX && atlas->off_drawY &&
                f < atlas->frame_count;
            if (has_foot_anchor) {
                int16_t ax = EOFF(e, ENT_OFF_ANCHOR_X, int16_t);
                int16_t ay = EOFF(e, ENT_OFF_ANCHOR_Y, int16_t);
                int16_t hx = (int16_t)atlas->off_drawX[f];
                int16_t hy = (int16_t)atlas->off_drawY[f];
                dx = (int16_t)(ax + ((int32_t)hx * scale) / 100);
                dy = (int16_t)(ay + ((int32_t)hy * scale) / 100);
            }

            if ((flags & EFLAG_ALPHA_PLANE) && !(flags & EFLAG_DOUBLED)) {
                BlitAlphaScaledToBackbuffer(dx, dy, fw, fh, dw, dh, px, 2);
            } else {
                BlitSpriteScaledColorKeyFlip(dx, dy, fw, fh, dw, dh, px,
                                             (flags & EFLAG_DOUBLED) ? 1 : 0);
            }
            blit_w = dw;
            blit_h = dh;
        } else {
            BlitSpriteToBackbuffer((uint16_t)dx, (uint16_t)dy, 0, 0,
                                   fw, fh, fw, fh, px, 0);
            blit_w = fw;
            blit_h = fh;
        }

        /* Walk-behind paint mask: stamp pixels we just drew that
 * walk-behind masks should be able to occlude later in the
 * walk.
 *
 * Actors: always marked (Ebek/Fjej walk behind buildings).
 * Sky entities: marked iff they sit fully above the walk
 * area AND carry EFLAG_SKY (set on SPAWN for
 * birds / airplane / ufo). The combined gate
 * prevents foreground props that happen to
 * lift above the walk area mid-action (e.g.
 * camera prop during use-camera anim) from
 * being marked. */
        int is_actor = (e == g_actor[0] || e == g_actor[1]);
        int entity_bottom_y = dy + blit_h;
        int is_sky = (g_walk_fld_oy > 0 &&
                      entity_bottom_y <= (int)g_walk_fld_oy &&
                      (flags & EFLAG_SKY));
        if (is_actor || is_sky) {
            stamp_paint_mask(dx, dy, blit_w, blit_h);
        }
    }
}
