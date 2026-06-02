/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* src/scene/hit_test.c — scene click resolver + verb_id reverse lookup.
 *
 * Two public helpers:
 *
 *   - ClickHitTest(mx, my, *out_verb): walk the click list, run a
 *     per-kind hit-test (8bpp pixel, 1bpp packed, or bbox-only) against
 *     the mouse, pick the winning verb_id by foot_y depth or short-
 *     circuit to a kind=2 first-match. Returns 1 on hit + verb in
 *     `*out_verb`; out_verb defaults to NEUTRAL_VERB on miss.
 *
 *   - FindEntityByVerbId(target_verb): reverse lookup — given a verb id,
 *     find the OWNER entity (the sprite or mask whose click payload
 *     carries that verb). Used by opcodes 0x15/0x26/0x28 etc.
 *
 * Both walk g_click_list_head via EntityListAt/Count. Sprite payloads
 * (kind=1) carry a verb table indexed by current frame; mask payloads
 * (kind=2) pre-bake the verb at registration time.
 */

#include "wacki.h"

#include <stdint.h>

extern Entity *g_click_list_head;
extern int     EntityListCount(int click_list);
extern Entity *EntityListAt   (int click_list, int idx);

/* ---- module constants --------------------------------------------- */

#define NEUTRAL_VERB                  0x26

/* Iterator selector for EntityListAt/Count. */
#define CLICK_LIST_INDEX              1

/* Click descriptor kinds at +0x08. */
#define CLICK_KIND_SPRITE             1
#define CLICK_KIND_MASK               2

/* Mask flag bits at owner+0x14. The low byte (after XOR with HITTABLE)
 * selects the per-pixel test mode. */
#define MASK_FLAG_ALWAYS_HIT          0x0010
#define MASK_FLAG_HITTABLE            0x8000
#define MASK_TEST_8BPP                1
#define MASK_TEST_1BPP_PACKED         2
#define MASK_TEST_BBOX                4
#define MASK_TEST_TERMINATE           8

/* 1bpp mask: MSB packing (each byte stores 8 pixels left-to-right). */
#define BPP1_HIGH_BIT                 0x80
#define BPP1_BITS_PER_BYTE            8
#define BPP1_BIT_INDEX_MASK           0x07

/* ---- helpers ------------------------------------------------------- */

/* Pixel test for a kind=1 click descriptor: 8bpp pixel at (mx, my) on
 * the owner's atlas frame. Caller has already done the bbox check.
 * Also returns the current frame so the verb-table lookup can use it. */
static int pixel_hit_sprite(Entity *owner, int16_t mouse_x, int16_t mouse_y,
                            int16_t ox, int16_t oy, uint16_t ow,
                            uint16_t *out_frame)
{
    *out_frame = 0;
    AnimAsset *atlas =
        (AnimAsset *)ent_ptr_resolve(EOFF(owner, ENT_OFF_ATLAS_SLOT, uint32_t));
    if (!atlas || !atlas->frame_count) return 0;

    uint16_t cur_frame = EOFF(owner, ENT_OFF_FRAME, uint16_t);
    if (cur_frame >= atlas->frame_count) cur_frame = 0;
    *out_frame = cur_frame;

    const uint8_t *px = atlas->pixel_ptrs ? atlas->pixel_ptrs[cur_frame] : NULL;
    if (!px) return 0;

    int lx = mouse_x - ox;
    int ly = mouse_y - oy;
    return px[ly * (int)ow + lx] != 0;
}

/* Pixel test for a kind=2 mask click descriptor. The mask flag at
 * owner+0x14 selects the test mode; HITTABLE bit (or ALWAYS_HIT override)
 * must be set or the mask doesn't fire at all. */
static int pixel_hit_mask(Entity *owner, int16_t mouse_x, int16_t mouse_y,
                          int16_t ox, int16_t oy, uint16_t ow)
{
    uint16_t mask_flag  = EOFF(owner, ENT_OFF_PIXELS_SLOT, uint16_t);
    int      always_hit = (mask_flag & MASK_FLAG_ALWAYS_HIT) != 0;

    /* HIDE'd masks (HITTABLE cleared) don't fire unless ALWAYS_HIT
     * forces them through. */
    if (!always_hit && (mask_flag & MASK_FLAG_HITTABLE) == 0) return 0;
    if (always_hit) return 1;

    const uint8_t *px =
        (const uint8_t *)ent_ptr_resolve(EOFF(owner, ENT_OFF_PIXEL_SLOT_ALT, uint32_t));
    int lx = mouse_x - ox;
    int ly = mouse_y - oy;

    switch (mask_flag ^ MASK_FLAG_HITTABLE) {
    case MASK_TEST_8BPP:
        return px && px[ly * (int)ow + lx] != 0;

    case MASK_TEST_1BPP_PACKED: {
        if (!px) return 0;
        int     stride = (ow + 7) / BPP1_BITS_PER_BYTE;
        uint8_t byte   = px[ly * stride + lx / BPP1_BITS_PER_BYTE];
        return (byte & (BPP1_HIGH_BIT >> (lx & BPP1_BIT_INDEX_MASK))) != 0;
    }

    case MASK_TEST_BBOX:
        return 1;

    case MASK_TEST_TERMINATE:
    default:
        /* TERMINATE explicitly says "this mask never hits" — original
         * also cleared a global continue flag, but our sequential
         * walk handles that naturally by simply skipping the entry. */
        return 0;
    }
}

/* Resolve the verb id for a click descriptor that just matched the
 * pixel test. kind=1 uses the verb table indexed by current frame;
 * kind=2 carries the verb pre-baked at +0x12.
 *
 * Mutates the descriptor's cache slot at +0x12 so subsequent
 * FindEntityByVerbId calls can use the cached value. */
static uint16_t resolve_hit_verb(Entity *desc, uint16_t kind, uint16_t cur_frame)
{
    uint8_t        *cb = (uint8_t *)desc;
    uint16_t        verb = EOFF(desc, CLICK_OFF_CACHED_VERB, uint16_t);
    const uint16_t *vt = (const uint16_t *)ent_ptr_resolve(
        EOFF(desc, CLICK_OFF_VERB_TABLE_SLOT, uint32_t));

    if (vt && kind == CLICK_KIND_SPRITE) {
        uint16_t count = vt[0];
        if (count) {
            uint16_t idx = (cur_frame < count) ? cur_frame
                                               : (uint16_t)(count - 1);
            verb = vt[idx + 1];
            *(uint16_t *)(cb + CLICK_OFF_CACHED_VERB) = verb;
        }
    }
    return verb;
}

/* ---- FindEntityByVerbId ------------------------------------------- *
 *
 * Reverse lookup: walk the click list and return the OWNER entity
 * whose payload would resolve to `target_verb` at the current frame.
 * Only kind=1 (sprite) payloads return an owner — kind=2 mask payloads
 * match against the verb but return NULL since the "owner" is a mask
 * desc rather than something callers like op 0x28 SET_ENTITY_XY can
 * use. */
Entity *FindEntityByVerbId(uint16_t target_verb)
{
    int nclk = EntityListCount(CLICK_LIST_INDEX);
    for (int i = 0; i < nclk; ++i) {
        Entity *c = EntityListAt(CLICK_LIST_INDEX, i);
        if (!c) continue;

        uint8_t  *cb         = (uint8_t *)c;
        uint16_t  click_kind = EOFF(c, CLICK_OFF_KIND, uint16_t);
        const uint16_t *vt   = (const uint16_t *)ent_ptr_resolve(
            EOFF(c, CLICK_OFF_VERB_TABLE_SLOT, uint32_t));

        uint16_t verb;
        if (!vt) {
            verb = EOFF(c, CLICK_OFF_CACHED_VERB, uint16_t);  /* kind=2 cached path */
        } else {
            uint16_t count = vt[0];
            uint16_t idx   = 0;
            if (click_kind == CLICK_KIND_SPRITE) {
                Entity *owner = (Entity *)ent_ptr_resolve(
                    EOFF(c, CLICK_OFF_OWNER_SLOT, uint32_t));
                if (owner) {
                    uint16_t frame = EOFF(owner, ENT_OFF_FRAME, uint16_t);
                    if (count && frame >= count) {
                        verb = vt[count];           /* last entry */
                        goto matched;
                    }
                    idx = frame;
                }
            }
            verb = count ? vt[idx + 1] : 0;
        }
    matched:
        if (verb == target_verb) {
            /* Only sprite payloads return an owner. */
            if (click_kind == CLICK_KIND_SPRITE) {
                return (Entity *)ent_ptr_resolve(
                    EOFF(c, CLICK_OFF_OWNER_SLOT, uint32_t));
            }
            return NULL;
        }
        (void)cb;
    }
    return NULL;
}

/* ---- ClickHitTest -------------------------------------------------- *
 *
 * The mouse → verb_id pipeline:
 *   1. Walk the click list
 *   2. For each desc, run kind-specific pixel test against (mx, my)
 *   3. Resolve the hit verb (kind=1 frame-indexed, kind=2 cached)
 *   4. Pick winner by foot_y depth, kind=2 short-circuits
 *
 * Returns 1 on hit (verb stored in *out_verb); 0 on miss (out_verb is
 * still written to NEUTRAL_VERB so callers don't see stale values). */
int ClickHitTest(int16_t mouse_x, int16_t mouse_y, uint16_t *out_verb)
{
    uint16_t hit_verb   = 0;
    int16_t  hit_foot_y = 0;
    int      any_hit    = 0;

    int nclk = EntityListCount(CLICK_LIST_INDEX);
    for (int i = 0; i < nclk; ++i) {
        Entity *c = EntityListAt(CLICK_LIST_INDEX, i);
        if (!c) continue;

        uint16_t kind = EOFF(c, CLICK_OFF_KIND, uint16_t);
        if (kind != CLICK_KIND_SPRITE && kind != CLICK_KIND_MASK) continue;

        Entity *owner = (Entity *)ent_ptr_resolve(
            EOFF(c, CLICK_OFF_OWNER_SLOT, uint32_t));
        if (!owner) continue;

        /* Bbox prune against owner's drawn rectangle. */
        int16_t  ox = EOFF(owner, ENT_OFF_DRAWN_X, int16_t);
        int16_t  oy = EOFF(owner, ENT_OFF_DRAWN_Y, int16_t);
        uint16_t ow = EOFF(owner, ENT_OFF_WIDTH,   uint16_t);
        uint16_t oh = EOFF(owner, ENT_OFF_HEIGHT,  uint16_t);
        if (mouse_x < ox || mouse_x >= ox + (int)ow ||
            mouse_y < oy || mouse_y >= oy + (int)oh) continue;

        /* Kind-specific pixel test. */
        int      hit       = 0;
        uint16_t cur_frame = 0;
        if (kind == CLICK_KIND_SPRITE) {
            hit = pixel_hit_sprite(owner, mouse_x, mouse_y, ox, oy, ow,
                                   &cur_frame);
        } else {
            hit = pixel_hit_mask(owner, mouse_x, mouse_y, ox, oy, ow);
        }
        if (!hit) continue;

        /* Resolve the verb and pick depth winner. */
        uint16_t verb   = resolve_hit_verb(c, kind, cur_frame);
        int16_t  foot_y = EOFF(owner, ENT_OFF_CLICK_FOOT_Y, int16_t);
        any_hit = 1;

        if (hit_verb == 0) {
            hit_verb   = verb;
            hit_foot_y = foot_y;
            if (kind == CLICK_KIND_MASK) {
                /* kind=2 first hit short-circuits everything else. */
                *out_verb = (verb == 0) ? NEUTRAL_VERB : verb;
                return any_hit;
            }
        } else if (foot_y > hit_foot_y) {
            hit_verb   = verb;
            hit_foot_y = foot_y;
            if (kind == CLICK_KIND_MASK) {
                *out_verb = verb;
                return any_hit;
            }
        }
    }

    *out_verb = hit_verb ? hit_verb : NEUTRAL_VERB;
    return any_hit;
}
