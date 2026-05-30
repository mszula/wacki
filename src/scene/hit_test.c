/* src/scene/hit_test.c — scene click resolver + verb_id reverse lookup.
 *
 * Two helpers:
 *
 * - ClickHitTest(mouse, *out_verb): walk the click list, run per-kind
 * hit-test (8bpp pixel, 1bpp packed, or bbox-only) against the
 * mouse, pick the winning verb_id by foot_y depth or short-circuit
 * to a kind=2 first-match. Returns 1 on hit + verb in `*out_verb`;
 * out_verb defaults to NEUTRAL_VERB (0x26) on miss.
 *
 * - FindEntityByVerbId(target_verb): reverse lookup — given a
 * verb_id, find the OWNER entity (the sprite or mask whose click
 * payload carries that verb). Used by opcodes 0x15/0x26/0x28 etc.
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

#define NEUTRAL_VERB                  0x26

/* Click descriptor field offsets. */
#define CLICK_OFF_KIND                0x08
#define CLICK_OFF_OWNER_SLOT          0x0A
#define CLICK_OFF_VERB_TABLE_SLOT     0x0E
#define CLICK_OFF_CACHED_VERB         0x12

/* Owner-entity field offsets (relevant to hit-test). */
#define OWNER_OFF_DRAWN_X             0x0A
#define OWNER_OFF_DRAWN_Y             0x0C
#define OWNER_OFF_WIDTH               0x0E
#define OWNER_OFF_HEIGHT              0x10
#define OWNER_OFF_FOOT_Y              0x12
#define OWNER_OFF_MASK_FLAG           0x14
#define OWNER_OFF_PIXEL_SLOT          0x16
#define OWNER_OFF_ATLAS_SLOT          0x28
#define OWNER_OFF_FRAME               0x30

/* Click descriptor kinds. */
#define CLICK_KIND_SPRITE  1
#define CLICK_KIND_MASK    2

/* Mask flag bits — see ANALYSIS.md for the full table. */
#define MASK_FLAG_ALWAYS_HIT      0x0010
#define MASK_FLAG_HITTABLE        0x8000
#define MASK_TEST_8BPP            1
#define MASK_TEST_1BPP_PACKED     2
#define MASK_TEST_BBOX            4
#define MASK_TEST_TERMINATE       8


/* ------------------------------------------------------------------------- *
 * ClickHitTest —
 *
 * Original code:
 *
 * void (void) {
 * uVar7 = 0; sVar4 = 0;
 * if (g_click_list_head != NULL) {
 * short mx = cursor_state_struct+0x22; // mouse X
 * short my = cursor_state_struct+0x24; // mouse Y
 * g_hover_scene_verb = 0x26; // default verb
 * piVar9 = g_click_list_head; // click list head
 * do {
 * puVar8 = piVar9[+0x0A]; // owner entity
 * uVar6 = 0;
 * if (piVar9[+0x08] == 1) { // kind=1 (sprite click, op 0x30 SPAWN)
 * synth_desc(&, puVar8, current_frame);
 * puVar8 = &;
 * uVar6 = puVar8[+0x30]; // current frame
 * goto TEST;
 * } else if (piVar9[+0x08] == 2) { // kind=2 (mask, op 0x2F REG_VERB_MASK)
 * goto TEST; // owner = kind=3 mask entity = test desc
 * }
 * continue;
 * TEST:
 * if ((puVar8, mx, my)) {
 * if (piVar9[+0x0E]) { // verb-id table present
 * count = *(u16*)(piVar9[+0x0E]);
 * idx = min(uVar6, count - 1);
 * piVar9[+0x12] = *(u16*)(piVar9[+0x0E] + 2 + idx*2);
 * }
 * if (uVar7 == 0) {
 * uVar7 = piVar9[+0x12]; // first hit
 * sVar4 = puVar8[+0x12]; // foot_y
 * if (piVar9[+0x08] == 2) { // kind=2 → final (don't keep searching)
 * if (uVar7 == 0) { g_hover_scene_verb = 0x26; return; }
 * goto SET;
 * }
 * } else if (sVar4 < puVar8[+0x12]) { // deeper hit
 * uVar7 = piVar9[+0x12];
 * sVar4 = puVar8[+0x12];
 * if (piVar9[+0x08] == 2) goto SET;
 * }
 * }
 * piVar9 = piVar9->next;
 * } while (piVar9);
 * }
 * if (uVar7 != 0) SET: g_hover_scene_verb = uVar7;
 * }
 *
 * Output: returns the verb_id to dispatch on click. The caller passes
 * this to DispatchClickEvent as its 2nd arg (= "target verb"), with
 * the 1st arg being the currently-held inventory item (or 0x26). */
extern Entity *g_click_list_head;
extern int     EntityListCount(int kind);  /* kind=1 → click list */
extern Entity *EntityListAt(int kind, int idx);

/* FindEntityByVerbId —
 *
 * undefined4 *(ushort param_1) {
 * for (puVar4 = g_click_list_head; puVar4; puVar4 = (undefined4*)*puVar4) {
 * ushort *puVar1 = *(ushort**)((int)puVar4 + 0xe); // verb table ptr
 * ushort uVar2;
 * if (puVar1 == NULL) {
 * uVar2 = *(ushort*)((int)puVar4 + 0x12); // cached verb
 * } else {
 * uint uVar3;
 * if (*(short*)(puVar4 + 2) == 1) { // kind=1 click
 * uVar2 = *(ushort*)(*(int*)((int)puVar4 + 10) + 0x30); // owner frame
 * uVar3 = (uint)uVar2;
 * if (*puVar1 <= uVar2) { // clamp to count-1
 * uVar2 = puVar1[*puVar1 - 1 + 1];
 * goto MATCH;
 * }
 * } else {
 * uVar3 = 0;
 * }
 * uVar2 = puVar1[uVar3 + 1]; // verb_id @ frame
 * }
 * MATCH:
 * if (uVar2 == param_1) goto FOUND;
 * }
 * puVar4 = NULL;
 * FOUND:
 * if (puVar4 && *(short*)(puVar4 + 2) == 1)
 * return *(undefined4*)((int)puVar4 + 10); // owner entity
 * return NULL;
 * }
 *
 * Returns the OWNER render entity (kind=1 click → +0x0A) whose
 * verb_id matches `target_verb`. Used by op 0x15/0x26/0x28 etc. */
Entity *FindEntityByVerbId(uint16_t target_verb)
{
    int nclk = EntityListCount(1);
    for (int i = 0; i < nclk; ++i) {
        Entity *c = EntityListAt(1, i);
        if (!c) continue;
        uint8_t *cb = (uint8_t *)c;
        uint16_t click_kind = *(uint16_t *)(cb + 8);
        const uint16_t *vt = (const uint16_t *)ent_ptr_resolve(*(uint32_t *)(cb + 0x0e));
        uint16_t verb;
        if (!vt) {
            verb = *(uint16_t *)(cb + 0x12);            /* cached (kind=2 path) */
        } else {
            uint16_t count = vt[0];
            uint16_t idx = 0;
            if (click_kind == 1) {                      /* kind=1 (sprite) */
                Entity *owner = (Entity *)ent_ptr_resolve(*(uint32_t *)(cb + 0x0a));
                if (owner) {
                    uint16_t frame = *(uint16_t *)((uint8_t *)owner + 0x30);
                    if (count && frame >= count) {
                        verb = vt[count];               /* last entry */
                        goto matched;
                    }
                    idx = frame;
                }
            }
            verb = count ? vt[idx + 1] : 0;
        }
    matched:
        if (verb == target_verb) {
            /* Only kind=1 returns an owner entity.
 * kind=2 click matches → returns NULL (caller's "entity"
 * is the mask, not useful for op 0x28 SET_ENTITY_XY etc.). */
            if (click_kind == 1) {
                return (Entity *)ent_ptr_resolve(*(uint32_t *)(cb + 0x0a));
            }
            return NULL;
        }
    }
    return NULL;
}

int ClickHitTest(int16_t mouse_x, int16_t mouse_y, uint16_t *out_verb)
{
    uint16_t hit_verb = 0;
    int16_t  hit_foot_y = 0;
    int      any_hit = 0;

    /* ---- Pass 1: click list (op 0x30 SPAWN spawned kind=1 hit tests) ---- */
    int nclk = EntityListCount(1);    /* 1 = click list, see EntityListAt */
    for (int i = 0; i < nclk; ++i) {
        Entity *c = EntityListAt(1, i);
        if (!c) continue;
        uint8_t *cb = (uint8_t *)c;
        uint16_t kind = *(uint16_t *)(cb + 8);
        /* kind=1: owner is render sprite, synth desc from current frame.
 * kind=2: owner is mask desc — test directly. (Other kinds: skip.) */
        if (kind != 1 && kind != 2) continue;
        Entity *owner = (Entity *)ent_ptr_resolve(*(uint32_t *)(cb + 0x0a));
        if (!owner) continue;
        uint8_t *ob = (uint8_t *)owner;

        /* Pixel-test bounding box (the owner's drawn rect). */
        int16_t ox = *(int16_t  *)(ob + 0x0a);
        int16_t oy = *(int16_t  *)(ob + 0x0c);
        uint16_t ow = *(uint16_t *)(ob + 0x0e);
        uint16_t oh = *(uint16_t *)(ob + 0x10);
        if (mouse_x < ox || mouse_x >= ox + (int)ow ||
            mouse_y < oy || mouse_y >= oy + (int)oh) continue;

        /* Per-pixel test — mirrors switch on flags^0x8000:
 * case 1 (8bpp): byte != 0 → hit
 * case 2 (1bpp packed, stride = (w+7)&~7 / 8): bit set → hit
 * case 4: bbox hit always
 * kind=1 click (sprite SPAWN): owner is a kind=2 sprite entity
 * with asset[+0x28] → use 8bpp pixel test on asset frame.
 * kind=2 click (mask REG_VERB_MASK): owner is a kind=3 mask
 * entity holding pixel_ptr directly at +0x16 → use 1bpp test. */
        int hit = 0;
        uint16_t cur_frame = 0;
        if (kind == 1) {
            AnimAsset *atlas = (AnimAsset *)ent_ptr_resolve(*(uint32_t *)(ob + 0x28));
            if (atlas && atlas->frame_count) {
                cur_frame = *(uint16_t *)(ob + 0x30);
                if (cur_frame >= atlas->frame_count) cur_frame = 0;
                const uint8_t *px = atlas->pixel_ptrs ? atlas->pixel_ptrs[cur_frame] : NULL;
                if (px) {
                    int lx = mouse_x - ox;
                    int ly = mouse_y - oy;
                    hit = (px[ly * (int)ow + lx] != 0);
                }
            }
        } else {
            /* kind=2 mask click — dispatch on mask flag at +0x14 to match
 * . The mask Entity stores its
 * test-mode flag at +0x14 (set by ScriptCallRegMaskList per
 * 's visibility branch):
 * bit 0x10 = always-hit override (early goto case 4)
 * bit 0x8000 = HITTABLE — REQUIRED to fire; cleared by op 0x50
 * with arg2==0 (HIDE mask) and re-set with arg2!=0
 * (SHOW mask). Mirrors RunScriptInterpreter case
 * 0x50 @ 0x004080F1 (`AND [ESI+0x14], 0x7FFF`).
 * 0x8001 = 8bpp test (visible asset, kind=2/3 atlas pixels)
 * 0x8002 = 1bpp packed test (mask file with pixel data)
 * 0x8004 = bbox-only (mask file with no pixel data)
 * 0x8008 = TERMINATE — clears global "continue" flag (we
 * translate to: skip without hit; iteration continues
 * on neighbours, since our list-walk is sequential).
 *
 * Bug fix (T-RopeExit): previously the switch had a `default`
 * branch that ran the 1bpp test for any unknown low byte — so a
 * masked hotspot HIDE'd via op 0x50 (flag=0x0002, bit 0x8000
 * cleared) silently fell into that branch and still clicked.
 * Now we early-skip when bit 0x8000 is clear (unless bit 0x10
 * forces always-hit), exactly as does. */
            uint16_t mask_flag = *(uint16_t *)(ob + 0x14);
            int always_hit = (mask_flag & 0x0010) != 0;
            if (!always_hit && (mask_flag & 0x8000) == 0) continue;  /* HIDE'd */
            const uint8_t *px = (const uint8_t *)ent_ptr_resolve(*(uint32_t *)(ob + 0x16));
            int lx = mouse_x - ox;
            int ly = mouse_y - oy;
            if (always_hit) {
                hit = 1;
            } else switch (mask_flag ^ 0x8000) {
            case 1: /* 8bpp pixel != 0 → hit */
                if (px) hit = (px[ly * (int)ow + lx] != 0);
                break;
            case 2: /* 1bpp packed: bit set → hit */
                if (px) {
                    int stride = (ow + 7) / 8;
                    uint8_t byte = px[ly * stride + lx / 8];
                    hit = (byte & (0x80u >> (lx & 7))) != 0;
                }
                break;
            case 4: /* bbox-only — always hit if inside */
                hit = 1;
                break;
            case 8: /* TERMINATE — original clears mask_continue_flag then returns
 * 0; equivalent here is "this mask never hits". */
                hit = 0;
                break;
            default:
                /* Unknown / unflagged: no hit. Original has no
 * default arm — execution falls past the switch with
 * local_1=0 → return 0. */
                hit = 0;
                break;
            }
        }
        if (!hit) continue;

        /* Verb-id resolution:
 * - kind=1 click: payload[+0x0E] is a u16 verb-id pool table
 * {count, verb_id[0], verb_id[1], …}; frame indexes the table.
 * - kind=2 click: payload[+0x12] holds the verb_id directly,
 * pre-baked at registration time ( reads click
 * pool[pool_idx+1] when allocating per-frame payload). */
        uint16_t verb = *(uint16_t *)(cb + 0x12);  /* cached / direct */
        const uint16_t *vt = (const uint16_t *)ent_ptr_resolve(*(uint32_t *)(cb + 0x0e));
        if (vt && kind == 1) {
            uint16_t count = vt[0];
            if (count) {
                uint16_t idx = (cur_frame < count) ? cur_frame : (uint16_t)(count - 1);
                verb = vt[idx + 1];
                *(uint16_t *)(cb + 0x12) = verb;     /* cache mirror */
            }
        }

        int16_t foot_y = *(int16_t *)(ob + 0x12);
        any_hit = 1;
        if (hit_verb == 0) {
            hit_verb = verb;
            hit_foot_y = foot_y;
            if (kind == 2) {
                /* kind=2 first hit → final. */
                if (verb == 0) { *out_verb = 0x26; return any_hit; }
                *out_verb = verb;
                return any_hit;
            }
        } else if (foot_y > hit_foot_y) {
            hit_verb   = verb;
            hit_foot_y = foot_y;
            if (kind == 2) { *out_verb = verb; return any_hit; }
        }
    }

    *out_verb = hit_verb ? hit_verb : 0x26;
    return any_hit;
}
