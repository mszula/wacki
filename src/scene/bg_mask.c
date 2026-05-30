/* src/scene/bg_mask.c — scene background mask setup (op 0x2C).
 *
 * BG mask setup is the engine's "destroy everything at id=0 and re-load
 * the room's walkable-area mask + walk-behind layer". Called by every
 * room's enter_script with the room's .fld filename. Three phases:
 *
 * 1. Destroy any kind=2/3/4 entities registered with id=0 (clearing
 * the old room's mask state without nuking actors who are at
 * different ids).
 * 2. Load the new .fld asset and register it as (kind=1, id=0) for
 * subsequent lookups.
 * 3. Allocate a kind=3 mask entity from the asset's frame 0 and link
 * it into the render list (so it participates in z-sort) and the
 * walkability table.
 *
 * The FLD body also seeds the scene's perspective bands which the
 * waypoint pathfinder uses.
 */

#include "wacki.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern Entity *g_render_list_head;
extern void   *xmalloc(uint32_t sz);
extern void    LinkEntityToList(Entity **head, Entity *e, int position);
extern void    RegisterEntityForUpdate(Entity *e, uint16_t kind, uint16_t id);
extern void    ScriptCallDestroyEnt(uint16_t id, int also_unreg_asset);

/* (BG mask setup, NOT a scene
 * transition):
 *
 * (0, '\x01'); // DESTROY id=0 (kinds 2/3/4)
 * asset = LoadAssetFromDtaBase(name);
 * if (asset) {
 * RegisterEntityForUpdate(asset, 1, 0); // kind=1 asset reg, id=0
 * e = (off_widths[0], off_heights[0], off_drawX[0],
 * off_drawY[0], pixel_ptrs[0]);
 * if (e) {
 * RegisterEntityForUpdate(e, 3, 0); // kind=3 mask entity
 * (&walkability_head, e, 1); // link into mask list (tail)
 * }
 * }
 *
 * Used by enter_scripts to install the room's .fld walkable-area mask
 * (maluch.fld, klatka2.fld, kiosk.fld, plac.fld). The kind=3 mask entity
 * is consumed by click-region detection (mouse cursor over walkable
 * floor → free-walk, otherwise → exit hotspot or NPC interaction).
 *
 * Our port doesn't yet have the click-region detection wired to scripts,
 * but the DESTROY-id=0-then-load semantics still matter: without it,
 * each scene's mask asset stacks in the registration table and confuses
 * future FindUpdateRegistration(1, 0) lookups. So we do the destroy +
 * the load + the kind=1/3 registrations; the kind=3 entity itself is
 * skipped (it has no render path since masks are click-only). */
void ScriptCallBgMaskSetup(const char *name)
{
    extern void *FindUpdateRegistration(uint16_t kind, uint16_t id);
    extern void  UnregisterEntityForUpdate(uint16_t kind, uint16_t id);
    extern int   g_persp_band_count;                   /* perspective_band_count_alt */
    extern uint16_t g_settings_anim_active;            /* g_settings_anim_active (T121) */
    /* DESTROY id=0 across kinds 2,3,4 + unregister kind=1 asset reg
 * (param_2='\x01' to original ). */
    ScriptCallDestroyEnt(0, 1);
    /* Reset perspective band count — case 0x2c (line 893):
 * perspective_band_count_alt = (g_settings_anim_active & 0xff02) << 1;
 * Without this reset, FILD-asset bands accumulate across scene
 * changes (LoadAssetFromDtaBase ADDS to band count, never clears).
 * The mask `& 0xff02` extracts bits 1 + 8-15 of komnata flags; the
 * <<1 shift moves them up by one. For komnata_flags=0x03 (panel +
 * actors) this evaluates to 0x04, providing a per-room baseline. */
    g_persp_band_count = (int)((g_settings_anim_active & 0xff02) << 1);
    if (!name) {
        fprintf(stderr, "[script] bg-mask-setup name=NULL\n");
        return;
    }
    AnimAsset *a = LoadAssetFromDtaBase(name);
    if (!a) {
        fprintf(stderr, "[script] bg-mask-setup '%s' FAILED\n", name);
        return;
    }
    RegisterEntityForUpdate((Entity *)a, 1, 0);

    /* Publish walk-area FLD globals from this asset's first frame.
 * The bg-mask-setup file IS the room's walkability bitmap in the
 * original (the same kind=1 asset id=0 backs both walk-behind
 * rendering and is_walkable_at look-ups). Previously only
 * LoadKomnataScene step 4 published these globals — synthesizing
 * the filename as `<pic-basename>.fld`. That works when pic and
 * fld share a basename (foto3.pic/foto3.fld, …) but breaks in
 * stage-2 komnata 5: pic = magaz3j.pic (a 1×1 palette stub), fld =
 * magaz3.fld (bez `j`). Synth name `magaz3j.fld` didn't exist →
 * walkability empty → every click "unreachable". Publishing here
 * makes the script-named FLD authoritative; step 4's synth load
 * skips when this already populated globals (see game.c). */
    if (a->frame_count > 0 && a->off_widths && a->off_heights &&
        a->off_drawX && a->off_drawY && a->pixel_ptrs && a->pixel_ptrs[0]) {
        extern const uint8_t *g_walk_fld_pixels;
        extern uint16_t g_walk_fld_w, g_walk_fld_h, g_walk_fld_stride;
        extern int16_t  g_walk_fld_ox, g_walk_fld_oy;
        g_walk_fld_pixels = a->pixel_ptrs[0];
        g_walk_fld_w      = a->off_widths [0];
        g_walk_fld_h      = a->off_heights[0];
        g_walk_fld_ox     = (int16_t)a->off_drawX[0];
        g_walk_fld_oy     = (int16_t)a->off_drawY[0];
        g_walk_fld_stride = (uint16_t)((g_walk_fld_w + 7) / 8);
        fprintf(stderr, "[fld] %s: %ux%u @ (%d,%d) stride=%u (via bg-mask-setup)\n",
                name, g_walk_fld_w, g_walk_fld_h, g_walk_fld_ox, g_walk_fld_oy,
                g_walk_fld_stride);
        /* Rebuild per-actor waypoint graphs —
 * op 0x2C tail @ 0x00408AA0 which calls (g_actor_wp[0])
 * then REP MOVSD-copies actor 0 struct to actor 1. The graph holds
 * scene perspective bands (from FILD body) + pre-built edges
 * between them; BindActorWalker consults it via wp_find_path. */
        extern void ActorWaypointsSceneInit(int actor_idx);
        ActorWaypointsSceneInit(0);
        ActorWaypointsSceneInit(1);
    }

    /* Synthesize the kind=3 walk-behind entity
 * (op 0x2C body):
 * piVar22 = (w[0], h[0], drawX[0], drawY[0], pixels[0]);
 * RegisterEntityForUpdate(piVar22, 3, 0);
 * (&walkability_head, piVar22, 1);
 *
 * The original links to walkability_head (walk-behind list) which is used
 * for foot-fall validation by (clamps walker into
 * non-obstacle areas). For visible occlusion of actors, we ADDITIONALLY
 * link the entity into the main render list (g_render_list_head) with
 * a foot_y at the bottom of the sprite — so when Z-sort runs, the
 * walk-behind entity renders ABOVE any actor whose foot_y is less
 * than the walk-behind's bottom edge.
 *
 * Asset visibility check ( logic): if asset->kind & 2 == 0
 * (= mask file, no visible pixels), the entity stays HIDDEN; only
 * kind=2/3 visible .wyc files get on-screen as walk-behind. */
    if (a->frame_count > 0 && a->off_widths && a->off_heights &&
        a->off_drawX && a->off_drawY && a->pixel_ptrs && a->pixel_ptrs[0])
    {
        uint16_t w  = a->off_widths [0];
        uint16_t h  = a->off_heights[0];
        int16_t  dx = (int16_t)a->off_drawX[0];
        int16_t  dy = (int16_t)a->off_drawY[0];
        Entity *wb = (Entity *)xmalloc(sizeof *wb);
        if (wb) {
            memset(wb, 0, sizeof *wb);
            uint8_t *eb = (uint8_t *)wb;
            /* T32 — walk-behind alpha-plane: if asset's flag_22 bit 0 is
 * set (alpha-plane source per wacki.h AnimAsset comment),
 * tag the entity with flag 0x100 so EntityRenderAll routes
 * to BlitAlphaScaled mode 2 instead of plain color-key blit.
 * This gives translucent edge rendering for walk-behind props
 * (semi-transparent foliage, glass etc.) —
 * logic that branches on asset[+0x16] bit 0. */
            uint16_t fl = 1;                                  /* visible */
            if (a->flag_22 & 1) fl |= 0x100;                  /* alpha-plane */
            *(uint16_t *)(eb + 0x08) = fl;
            *(int16_t  *)(eb + 0x0A) = dx;                    /* drawX */
            *(int16_t  *)(eb + 0x0C) = dy;                    /* drawY */
            *(uint16_t *)(eb + 0x0E) = w;                     /* width */
            *(uint16_t *)(eb + 0x10) = h;                     /* height */
            *(int16_t  *)(eb + 0x26) = (int16_t)(dy + h);     /* foot_y (Z-sort) */
            *(uint32_t *)(eb + 0x28) = ent_ptr_intern((void *)a);
            *(uint16_t *)(eb + 0x30) = 0;                     /* frame 0 */
            RegisterEntityForUpdate(wb, 3, 0);
            /* Only put into the main render list when the asset is a
 * visible .wyc (kind=2 or kind=3, not kind=0 mask). */
            if (a->kind != 0) {
                extern Entity *g_render_list_head;
                extern void    LinkEntityToList(Entity **head, Entity *e, int position);
                LinkEntityToList(&g_render_list_head, wb, 1);
            }
        }
    }

    fprintf(stderr, "[script] bg-mask-setup '%s' (asset id=0, kind=%u, frames=%u)\n",
            name, a->kind, a->frame_count);
}
