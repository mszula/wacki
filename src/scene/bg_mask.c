/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* src/scene/bg_mask.c — scene background mask setup (op 0x2C).
 *
 * BG mask setup is the engine's "destroy everything at id=0 and re-load
 * the room's walkable-area mask + walk-behind layer". Called by every
 * room's enter_script with the room's .fld filename. Three phases:
 *
 *   1. Destroy any kind=2/3/4 entities registered with id=0 (clears
 *      the old room's mask state without nuking actors at different ids).
 *   2. Load the new .fld asset and register it as (kind=1, id=0).
 *   3. Allocate a kind=3 walk-behind entity from frame 0 and link it
 *      into the render list. Also publish the walkability bitmap
 *      globals and rebuild the per-actor waypoint graphs.
 *
 * The .fld file IS the room's walkability bitmap in the original — the
 * same kind=1 asset id=0 backs both walk-behind rendering and the
 * is_walkable_at look-ups.
 */

#include "wacki.h"
#include "wacki/log.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern Entity   *g_render_list_head;
extern int       g_persp_band_count;
extern uint16_t  g_komnata_flags;
extern const uint8_t *g_walk_fld_pixels;
extern int       g_walk_fld_w, g_walk_fld_h, g_walk_fld_stride;
extern int       g_walk_fld_ox, g_walk_fld_oy;

extern void   *xmalloc(uint32_t sz);
extern void    LinkEntityToList(Entity **head, Entity *e, int position);
extern void    RegisterEntityForUpdate(Entity *e, uint16_t kind, uint16_t id);
extern void    ScriptCallDestroyEnt(uint16_t id, int also_unreg_asset);
extern void    UnregisterEntityForUpdate(uint16_t kind, uint16_t id);
extern void   *FindUpdateRegistration(uint16_t kind, uint16_t id);
extern void    ActorWaypointsSceneInit(int actor_idx);

/* ---- constants ---------------------------------------------------- */

/* (kind, id) registrations. */
#define BG_MASK_ID                      0
#define ASSET_KIND                      1
#define WALK_BEHIND_MASK_KIND           3

/* Perspective band count rebuild from komnata flags. The formula
 * `(flags & 0xff02) << 1` extracts bits 1 + 8-15 and shifts them up
 * by one. For komnata flags 0x03 (panel + actors) this yields 0x04,
 * providing a per-room baseline; bits 8-15 carry stage-specific extras. */
#define BAND_COUNT_FLAGS_MASK           0xFF02

/* Walk-behind entity flag bits (the kind=3 entity registered for
 * z-sort + render). */
#define EFLAG_VISIBLE                   0x0001
#define EFLAG_ALPHA_PLANE_LOCAL         0x0100  /* matches EFLAG_ALPHA_PLANE */

/* asset->flag_22 bit 0 = alpha-plane source. */
#define ASSET_FLAG22_ALPHA_BIT          0x01

/* 1bpp stride: 8 pixels per byte. */
#define WALK_FLD_BITS_PER_BYTE          8

/* Tail position for LinkEntityToList — used when we want the walk-
 * behind to fall AFTER everything else already in the list (so the
 * z-sort can later override). */
#define LINK_TAIL_POSITION              1

/* ---- helpers ------------------------------------------------------- */

/* Does this asset's frame 0 carry all the metadata we need to use it
 * as a walkability bitmap + walk-behind sprite? */
static int asset_has_frame0_geometry(const AnimAsset *a)
{
    return a->frame_count > 0
        && a->off_widths && a->off_heights
        && a->off_drawX && a->off_drawY
        && a->pixel_ptrs && a->pixel_ptrs[0];
}

/* Publish the FLD walkability bitmap globals from this asset's frame 0
 * and rebuild the per-actor waypoint graphs. The bg-mask-setup file
 * IS the room's walkability bitmap, so publishing here makes the
 * script-named FLD authoritative (avoids LoadKomnataScene's
 * synthesized "<bg-basename>.fld" guess, which breaks when the .pic
 * and .fld have different basenames — magaz3j.pic vs magaz3.fld). */
static void publish_walkability_from_asset(const AnimAsset *a, const char *name)
{
    g_walk_fld_pixels = a->pixel_ptrs[0];
    g_walk_fld_w      = a->off_widths [0];
    g_walk_fld_h      = a->off_heights[0];
    g_walk_fld_ox     = (int16_t)a->off_drawX[0];
    g_walk_fld_oy     = (int16_t)a->off_drawY[0];
    g_walk_fld_stride = (uint16_t)((g_walk_fld_w + WALK_FLD_BITS_PER_BYTE - 1)
                                   / WALK_FLD_BITS_PER_BYTE);
    LOG_TRACE("fld", "%s: %ux%u @ (%d,%d) stride=%u (via bg-mask-setup)", name, g_walk_fld_w, g_walk_fld_h, g_walk_fld_ox, g_walk_fld_oy, g_walk_fld_stride);

    ActorWaypointsSceneInit(0);
    ActorWaypointsSceneInit(1);
}

/* Build the kind=3 walk-behind entity from frame 0 of `a` and link it
 * into the appropriate lists. Returns the entity (or NULL on alloc
 * failure). The entity is NOT linked into the render list when the
 * asset is a pure mask (kind==0, no visible pixels) — masks contribute
 * to walkability only, not to drawing. */
static void build_walk_behind_entity(const AnimAsset *a)
{
    uint16_t w  = a->off_widths [0];
    uint16_t h  = a->off_heights[0];
    int16_t  dx = (int16_t)a->off_drawX[0];
    int16_t  dy = (int16_t)a->off_drawY[0];

    Entity *wb = (Entity *)xmalloc(sizeof *wb);
    if (!wb) return;
    memset(wb, 0, sizeof *wb);

    uint16_t fl = EFLAG_VISIBLE;
    if (a->flag_22 & ASSET_FLAG22_ALPHA_BIT) fl |= EFLAG_ALPHA_PLANE_LOCAL;

    EOFF(wb, ENT_OFF_FLAGS1,      uint16_t) = fl;
    EOFF(wb, ENT_OFF_DRAWN_X,     int16_t)  = dx;
    EOFF(wb, ENT_OFF_DRAWN_Y,     int16_t)  = dy;
    EOFF(wb, ENT_OFF_WIDTH,       uint16_t) = w;
    EOFF(wb, ENT_OFF_HEIGHT,      uint16_t) = h;
    EOFF(wb, ENT_OFF_FOOT_Y,      int16_t)  = (int16_t)(dy + h);
    EOFF(wb, ENT_OFF_ATLAS_SLOT,  uint32_t) = ent_ptr_intern((void *)a);
    EOFF(wb, ENT_OFF_FRAME,       uint16_t) = 0;

    RegisterEntityForUpdate(wb, WALK_BEHIND_MASK_KIND, BG_MASK_ID);

    /* Pure masks (kind=0) contribute to walkability only, not to
     * rendering. Visible .wyc files (kind=2/3) get linked so the
     * z-sort can put them in front of actors. */
    if (a->kind != 0) {
        LinkEntityToList(&g_render_list_head, wb, LINK_TAIL_POSITION);
    }
}

/* ---- public entry point ------------------------------------------- */

void ScriptCallBgMaskSetup(const char *name)
{
    /* Tear down the previous room's (kind=2/3/4, id=0) entities plus
     * the prior asset registration (param `also_unreg_asset=1`). */
    ScriptCallDestroyEnt(BG_MASK_ID, 1);

    /* Reset the perspective band count from komnata flags. Without
     * this, FILD-asset bands accumulate across scene changes. */
    g_persp_band_count = (int)((g_komnata_flags & BAND_COUNT_FLAGS_MASK) << 1);

    if (!name) {
        LOG_TRACE("script", "bg-mask-setup name=NULL");
        return;
    }

    AnimAsset *a = LoadAssetFromDtaBase(name);
    if (!a) {
        LOG_TRACE("script", "bg-mask-setup '%s' FAILED", name);
        return;
    }
    RegisterEntityForUpdate((Entity *)a, ASSET_KIND, BG_MASK_ID);

    if (asset_has_frame0_geometry(a)) {
        publish_walkability_from_asset(a, name);
        build_walk_behind_entity(a);
    }

    LOG_TRACE("script", "bg-mask-setup '%s' (asset id=0, kind=%u, frames=%u)", name, a->kind, a->frame_count);
}
