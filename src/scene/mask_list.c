/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/scene/mask_list.c — REG_VERB_MASK click-mask registration.
 *
 * Implements the back-end of opcodes 0x2E and 0x2F (mask list / verb
 * list). For each frame of an asset registered as (kind=1, id), builds:
 *
 *   - a kind=3 mask entity (carries the per-frame pixel data + draw
 *     position; links into g_render_list_head for walk-behind blits
 *     when not a verb-only hotspot)
 *   - a kind=4 click payload (carries the verb_id from the script's
 *     click pool; links into g_click_list_head for hit-test)
 *
 * The mask hit-test flag at +0x14 selects the per-pixel test mode:
 *
 *   asset->flag_22 bit 1 set        → 0x8001 (8bpp pixel test)
 *   else, has pixel data            → 0x8002 (1bpp packed test)
 *   else (bbox-only)                → 0x8004
 *
 * The legacy VisibleMasks* stubs remain as no-ops for API stability;
 * walk-behind masks now live in g_render_list_head and z-sort with
 * everything else.
 */

#include "wacki.h"
#include "wacki/log.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern Entity *g_click_list_head;
extern Entity *g_render_list_head;
extern void   *xmalloc(uint32_t sz);
extern void    LinkEntityToList(Entity **head, Entity *e, int position);
extern void    RegisterEntityForUpdate(Entity *e, uint16_t kind, uint16_t id);
extern void   *FindUpdateRegistration(uint16_t kind, uint16_t id);
extern const void *xlat_binary_ptr(uint32_t addr);

/* Legacy compat no-ops — older code may still call these even though
 * walk-behind masks now z-sort through the unified render list. */
void VisibleMasksReset(void)               { /* no-op kept for API stability */ }
int  VisibleMasksCount(void)               { return 0; }
int  VisibleMaskGet(int i, AnimAsset **a, uint16_t *f)
{
    (void)i;
    if (a) *a = NULL;
    if (f) *f = 0;
    return 0;
}
int  VisibleMaskGetEx(int i, AnimAsset **a, uint16_t *f,
                      uint16_t *id, const uint8_t **cp)
{
    (void)i;
    if (a) *a = NULL;  if (f) *f = 0;
    if (id) *id = 0;   if (cp) *cp = NULL;
    return 0;
}

/* ---- module constants --------------------------------------------- */

/* (kind, id) registrations used here. */
#define ASSET_KIND          1
#define MASK_KIND           3
#define CLICK_PAYLOAD_KIND  4

/* Click descriptor kind (at +0x08): masks register as CLICK_KIND_MASK
 * so ClickHitTest dispatches into the mask pixel-test path. */
#define CLICK_KIND_MASK     2

/* Hit-test flag values written to the mask entity at +0x14. */
#define HIT_FLAG_8BPP       0x8001    /* 8bpp pixel test */
#define HIT_FLAG_1BPP       0x8002    /* 1bpp packed shape */
#define HIT_FLAG_BBOX       0x8004    /* bbox-only */

/* asset->flag_22 bit 1 = "visible asset" → switches mask to 8bpp test. */
#define ASSET_FLAG22_VISIBLE_BIT  0x02

/* Link a freshly-built mask into the render-list head (so it
 * participates in z-sort and walk-behind paint). Verb-table builds
 * (op 0x2F) call us with `verb_table = 1` — those are invisible
 * hotspots that must NOT render, so we skip the link. */
#define VERB_TABLE_HOTSPOT  1

/* ---- helpers ------------------------------------------------------- */

/* Pick the hit-test flag for a mask entity based on the asset's
 * flag_22 visibility bit and whether the frame carries pixel data. */
static uint16_t pick_mask_hit_flag(const AnimAsset *a, const uint8_t *px)
{
    if (a->flag_22 & ASSET_FLAG22_VISIBLE_BIT) return HIT_FLAG_8BPP;
    if (px)                                    return HIT_FLAG_1BPP;
    return HIT_FLAG_BBOX;
}

/* Build the kind=3 mask Entity used by ClickHitTest (for the mask
 * pixel test) and by EntityRenderAll (for walk-behind blit). The
 * caller owns the returned pointer; NULL on alloc failure. */
static Entity *build_mask_entity(const AnimAsset *a, uint16_t frame,
                                 int16_t dx, int16_t dy,
                                 uint16_t w, uint16_t h, const uint8_t *px)
{
    Entity *mask = (Entity *)xmalloc(sizeof *mask);
    if (!mask) return NULL;
    memset(mask, 0, sizeof *mask);

    EOFF(mask, CLICK_OFF_KIND,           uint16_t) = CLICK_KIND_MASK;
    EOFF(mask, ENT_OFF_DRAWN_X,          int16_t)  = dx;
    EOFF(mask, ENT_OFF_DRAWN_Y,          int16_t)  = dy;
    EOFF(mask, ENT_OFF_WIDTH,            uint16_t) = w;
    EOFF(mask, ENT_OFF_HEIGHT,           uint16_t) = h;
    EOFF(mask, ENT_OFF_CLICK_FOOT_Y,     int16_t)  = (int16_t)(dy + h);
    EOFF(mask, ENT_OFF_PIXELS_SLOT,      uint16_t) = pick_mask_hit_flag(a, px);
    EOFF(mask, ENT_OFF_PIXEL_SLOT_ALT,   uint32_t) = px ? ent_ptr_intern((void *)px) : 0;
    EOFF(mask, ENT_OFF_ATLAS_SLOT,       uint32_t) = ent_ptr_intern((void *)a);
    EOFF(mask, ENT_OFF_FRAME,            uint16_t) = frame;

    /* Z-sort key — same byte as foot_y used by cmp_entity_y. Without
     * this the mask falls back to anchor-Y which is also zero on a
     * freshly-allocated mask and ends up at the back of the stack. */
    EOFF(mask, ENT_OFF_FOOT_Y,           int16_t) = (int16_t)(dy + h);

    return mask;
}

/* Build the kind=4 click payload that ClickHitTest walks. References
 * the mask via the owner-slot intern; verb id comes from the script's
 * click pool. */
static Entity *build_click_payload(Entity *mask, uint16_t verb_id)
{
    Entity *pld = (Entity *)xmalloc(sizeof *pld);
    if (!pld) return NULL;
    memset(pld, 0, sizeof *pld);

    EOFF(pld, CLICK_OFF_KIND,             uint16_t) = CLICK_KIND_MASK;
    EOFF(pld, CLICK_OFF_OWNER_SLOT,       uint32_t) = ent_ptr_intern(mask);
    EOFF(pld, CLICK_OFF_VERB_TABLE_SLOT,  uint32_t) = 0;
    EOFF(pld, CLICK_OFF_CACHED_VERB,      uint16_t) = verb_id;
    return pld;
}

/* ---- public entry point ------------------------------------------- *
 *
 * NOTE: we allocate full sizeof(Entity) for the click payload (~236 B
 * post-T10) even though the original used a ~20-byte buffer. Casting
 * a 20-byte buffer to Entity* and then accessing the trailing-zone
 * native-pointer fields (e->pixels etc.) would corrupt the heap on
 * 64-bit hosts. The memory overhead is trivial (~6 payloads/scene).
 *
 * Memory safety: the mask (kind=3, id) holds the pixel data; the
 * payload (kind=4, id) references the mask through an intern slot.
 * ScriptCallDestroyEnt iterates kinds 2-4 with matching id and frees
 * mask + payload atomically, so the payload's mask-ref never points
 * at freed memory.                                                   */
void ScriptCallRegMaskList(uint16_t id, uint32_t click_ptr, int verb_table)
{
    AnimAsset *a = (AnimAsset *)FindUpdateRegistration(ASSET_KIND, id);
    if (!a) {
        LOG_TRACE("script", "reg-mask-list id=%u click=0x%08x — no asset registered", id, click_ptr);
        return;
    }
    if (!a->off_widths || !a->off_heights || !a->off_drawX || !a->off_drawY) {
        return;
    }

    /* The verb-mask pool is raw PE data at an arbitrary (possibly odd) byte
     * offset — the same misaligned-u16 hazard as EOFF (see entity_offsets.h),
     * but it's a plain pointer deref, not an entity slot, so that fix doesn't
     * cover it. Read its u16s via memcpy so the R5900 can't trap on `lhu`. */
    const uint8_t  *pool       = (const uint8_t *)xlat_binary_ptr(click_ptr);
    uint16_t        pool_count = 0;
    if (pool) memcpy(&pool_count, pool, sizeof pool_count);
    uint16_t        pool_idx   = 0;
    int             spawned    = 0;

    for (uint16_t f = 0; f < a->frame_count; ++f) {
        uint16_t w  = a->off_widths [f];
        uint16_t h  = a->off_heights[f];
        if (w == 0 || h == 0) continue;

        int16_t  dx = (int16_t)a->off_drawX[f];
        int16_t  dy = (int16_t)a->off_drawY[f];
        uint8_t *px = a->pixel_ptrs ? a->pixel_ptrs[f] : NULL;

        Entity *mask = build_mask_entity(a, f, dx, dy, w, h, px);
        if (!mask) break;

        RegisterEntityForUpdate(mask, MASK_KIND, id);

        /* Op 0x2E mask list → link into render list so the z-sort
         * walks the mask for walk-behind blits. Op 0x2F verb table
         * entries are invisible hotspots — never render them. */
        if (verb_table != VERB_TABLE_HOTSPOT) {
            LinkEntityToList(&g_render_list_head, mask, 0);
        }

        if (pool && pool_count > 0) {
            uint16_t verb_id;
            memcpy(&verb_id, pool + (size_t)(pool_idx + 1) * 2, sizeof verb_id);
            Entity  *pld     = build_click_payload(mask, verb_id);
            if (pld) {
                LinkEntityToList(&g_click_list_head, pld, 0);
                RegisterEntityForUpdate(pld, CLICK_PAYLOAD_KIND, id);
                if (pool_idx < (uint16_t)(pool_count - 1)) ++pool_idx;
            }
        }
        ++spawned;
    }

    LOG_TRACE("script", "reg-mask-list id=%u asset=%s frames=%u spawned=%d "
            "click=0x%08X pool[count=%u] table=%s", id, a->name, a->frame_count, spawned, click_ptr, pool_count, verb_table ? "verb(hotspot)" : "mask(render)");
}
