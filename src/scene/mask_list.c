/* src/scene/mask_list.c — REG_VERB_MASK click-mask registration.
 *
 * Implements the back-end of opcodes 0x2E and 0x2F (mask list /
 * verb list). For each frame of an asset registered as (kind=1, id),
 * builds:
 *
 * - a kind=3 mask entity (carries the per-frame pixel data + draw
 * position; links into g_render_list_head for walk-behind blits
 * when not a verb-only hotspot)
 * - a kind=4 click payload (carries the verb_id from the script's
 * click pool; links into g_click_list_head for hit-test)
 *
 * The mask flag at +0x14 selects the per-pixel test mode (8bpp pixel,
 * 1bpp packed, bbox-only). The flag value depends on the asset's
 * flag_22 (visible vs hit-only) AND whether the frame has pixel data:
 *
 * asset->flag_22 bit 1 set → 0x8001 (8bpp pixel test)
 * else, has pixel data → 0x8002 (1bpp packed test)
 * else (bbox-only) → 0x8004
 *
 * The legacy VisibleMasks* stubs remain as no-ops for API stability;
 * walk-behind masks now live in g_render_list_head and z-sort with
 * everything else, so the "parallel visible-mask list" the older
 * port maintained is no longer needed.
 */

#include "wacki.h"

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

/* 1:1 with RunScriptInterpreter cases 0x2e and 0x2f, both of which
 * call (id, click_ptr, target_table) where target_table is
 * &DAT_0044e6d8 (op 0x2E, "mask" verb-list)
 * &DAT_0044e700 (op 0x2F, "verb" mask-list)
 *
 * : walks every frame of the asset
 * registered as kind=1 with the given id; for each frame, allocates a
 * kind=3 click-mask entity via , links into the target
 * table, and (if click_ptr is non-null) wraps it with a kind=4 click
 * payload entity that holds the verb id from click_ptr+(idx+1)*2.
 *
 * The bit 0x80 of mask_entity[+0x15] = `! (puVar3[0x11] & 2)` makes the
 * mask invisible (hit-test only). The original engine reads these masks
 * from DispatchClickEvent ( family).
 *
 * Our port doesn't yet have the click-region click-dispatch wired up,
 * so we log the registration and stop. Once the click handler is ported,
 * this stub needs to actually build the mask entities. */
/* g_visible_masks parallel list REMOVED. ScriptCallRegMaskList now
 * allocates proper kind=3 mask + kind=4 payload click entities linked
 * into g_click_list_head ClickHitTest walks
 * only the unified click list. */
void VisibleMasksReset(void) { /* no-op kept for API stability */ }
int  VisibleMasksCount(void) { return 0; }
int  VisibleMaskGet(int i, AnimAsset **a, uint16_t *f)
{ (void)i; if (a) *a = NULL; if (f) *f = 0; return 0; }
int  VisibleMaskGetEx(int i, AnimAsset **a, uint16_t *f,
                      uint16_t *id, const uint8_t **cp)
{ (void)i; if (a) *a = NULL; if (f) *f = 0;
  if (id) *id = 0; if (cp) *cp = NULL; return 0; }

/* ScriptCallRegMaskList —
 *
 * asset = (1, id);
 * if (!asset || asset->frame_count == 0) return;
 * uint16_t pool_idx = 0;
 * for (uint16_t frame = 0; frame < asset->frame_count; ++frame) {
 * mask = (w[f], h[f], drawX[f], drawY[f], pixels[f]);
 * RegisterEntityForUpdate(mask, 3, id);
 * LinkEntityToList(target_table, mask, 0);
 * if (asset->flag_22 & 2) mask->+0x14 = 0x8001; // VISIBLE
 * else mask->+0x15 |= 0x80; // HIDDEN
 * if (click_ptr) {
 * payload = malloc(0x14); memset(payload, 0, 0x14);
 * payload->+0x12 = click_ptr[pool_idx + 1]; // verb_id for this frame
 * payload->+0x0A = mask; // owner
 * payload->+0x08 = 2; // kind=2 click
 * LinkEntityToList(&click_list, payload, 0);
 * RegisterEntityForUpdate(payload, 4, id);
 * if (pool_idx < click_ptr[0] - 1) ++pool_idx;
 * }
 * }
 *
 * `verb_table` flag selects the mask-list head (E6D8 for op 0x2E, E700
 * for op 0x2F). Our port doesn't render kind=3 masks separately yet
 * (walk-behind alpha-plane render is deferred — see review B5/C7).
 * Hit-test correctness only needs the kind=4 payload chain. */
void ScriptCallRegMaskList(uint16_t id, uint32_t click_ptr, int verb_table)
{
    extern void *FindUpdateRegistration(uint16_t kind, uint16_t id);
    extern uint32_t ent_ptr_intern(void *p);
    extern void LinkEntityToList(Entity **head, Entity *e, int position);
    AnimAsset *a = (AnimAsset *)FindUpdateRegistration(1, id);
    if (!a) {
        fprintf(stderr, "[script] reg-mask-list id=%u click=0x%08x — no asset registered\n",
                id, click_ptr);
        return;
    }
    extern const void *xlat_binary_ptr(uint32_t addr);
    const uint16_t *pool = (const uint16_t *)xlat_binary_ptr(click_ptr);
    uint16_t pool_count = pool ? pool[0] : 0;
    uint16_t pool_idx = 0;
    int spawned = 0;

    for (uint16_t f = 0; f < a->frame_count; ++f) {
        if (!a->off_widths || !a->off_heights || !a->off_drawX || !a->off_drawY)
            break;
        uint16_t w  = a->off_widths [f];
        uint16_t h  = a->off_heights[f];
        int16_t  dx = (int16_t)a->off_drawX[f];
        int16_t  dy = (int16_t)a->off_drawY[f];
        uint8_t *px = a->pixel_ptrs ? a->pixel_ptrs[f] : NULL;
        if (w == 0 || h == 0) continue;

        /* mask alloc — Entity-sized buffer used as a
 * hit-test descriptor. Port-side layout mirrors my Entity
 * convention (drawX@+0x0A, etc.) so ClickHitTest's existing
 * field reads work uniformly. */
        Entity *mask = (Entity *)xmalloc(sizeof *mask);
        if (!mask) break;
        memset(mask, 0, sizeof *mask);
        uint8_t *mb = (uint8_t *)mask;
        *(uint16_t *)(mb + 0x08) = 2;                    /* kind for hit-test */
        *(int16_t  *)(mb + 0x0A) = dx;                   /* drawX */
        *(int16_t  *)(mb + 0x0C) = dy;                   /* drawY */
        *(uint16_t *)(mb + 0x0E) = w;                    /* width */
        *(uint16_t *)(mb + 0x10) = h;                    /* height */
        *(int16_t  *)(mb + 0x12) = (int16_t)(dy + h);    /* foot_y */
        /* Hit-test flag ( + ):
 * seeds: +0x14 = 0x8002 if pixels else 0x8004.
 * visibility branch (puVar3[0x11] & 2):
 * bit clear: OR 0x80 onto +0x15 — no-op (0x8000 already set).
 * Net: keep 0x8002 / 0x8004 (1bpp / bbox-only).
 * bit set (visible asset, flag_22 bit 1): overwrite +0x14 =
 * 0x8001 → 8bpp pixel test mode.
 * Uses asset->flag_22 directly (the raw rich_flag from the asset
 * header) — not a->kind, which collapses bits 0 and 1.
 * reads +0x14 low byte to pick the test mode
 * (1=8bpp, 2=1bpp packed, 4=bbox-only). */
        uint16_t hit_flag;
        if (a->flag_22 & 2)         hit_flag = 0x8001;  /* 8bpp test */
        else if (px)                hit_flag = 0x8002;  /* 1bpp packed test */
        else                        hit_flag = 0x8004;  /* bbox-only test */
        *(uint16_t *)(mb + 0x14) = hit_flag;
        *(uint32_t *)(mb + 0x16) = px ? ent_ptr_intern((void *)px) : 0;
        /* Asset ref kept on the mask for renderers (deferred). */
        *(uint32_t *)(mb + 0x28) = ent_ptr_intern((void *)a);
        *(uint16_t *)(mb + 0x30) = f;                    /* frame */
        /* Walk-behind sort key — foot_y at +0x26 (1:1 with cmp_entity_y
 * convention). Without this the mask would fall into the
 * "+0x26 == 0 → fallback to +0x24" branch (anchor Y is also 0
 * for un-positioned masks) and end up at the back of the z-stack. */
        *(int16_t *)(mb + 0x26) = (int16_t)(dy + h);

        RegisterEntityForUpdate(mask, 3, id);
        /* T-walk-behind: link MASK-table (E6D8) entries into the render
 * list so they participate in z-sort. Without this, walk-behind
 * masks (tree, building cut-outs, etc.) never render → actor
 * walks IN FRONT of objects they should walk BEHIND.
 *
 * Original walks the ONE render-list head
 * = ALL entities including walk-behind masks. The hit-test E6D8
 * pool is a SEPARATE bookkeeping. Our port previously only put
 * sprite spawns in g_render_list_head and left masks for hit-test
 * only — actor "walks in front of tree" regression.
 *
 * Verb-table (E700) entries are clickable hotspots, NOT walk-
 * behind layers — those don't render either way (hotspots are
 * invisible). Skip linking them so we don't double-render hotspot
 * rectangles over the BG. */
        if (!verb_table) {
            extern Entity *g_render_list_head;
            LinkEntityToList(&g_render_list_head, mask, 0);
        }

        /* Click payload (kind=4 update, kind=2 hit-test mode at +8).
 *
 * Allocate full Entity (sizeof = 236 B post-T10) instead of 0x14
 * raw bytes. The original engine alloc'd ~20 bytes for click
 * payloads via (0x14), but on 64-bit hosts our Entity
 * struct trailing zone (native pointers at byte 0xE0+) requires
 * the full size — casting a 20-byte buffer to Entity* and
 * accidentally accessing `e->pixels` / `e->kind` would corrupt
 * the heap. Tiny memory cost (~6 payloads per scene = ~1.4 KB).
 *
 * UAF safety (B35 false-positive analysis, T12):
 * The mask (kind=3, id) holds the pixel data. Payload (kind=4, id)
 * references the mask via byte +0x0A (intern slot). When the script
 * destroys this id via op 0x31/0x32 → ScriptCallDestroyEnt iterates
 * kinds 2-4 with matching id and destroys BOTH the mask AND the
 * payload atomically (see stubs.c:2049-2063). There is NO window
 * in which the payload's mask-ref points at freed memory:
 * - destroy(kind=3, id) frees mask buffer
 * - destroy(kind=4, id) — payload also unlinked + freed
 * The intern table slot for the dead mask is NOT recycled until
 * EntityListClearAll (scene change), so even a stale ptr-resolve
 * call between these two destroys would return the (just-freed)
 * mask pointer rather than an unrelated entity. */
        if (pool && pool_count > 0) {
            Entity *pld = (Entity *)xmalloc(sizeof *pld);
            if (pld) {
                memset(pld, 0, sizeof *pld);
                uint16_t verb_id = pool[pool_idx + 1];   /* per-frame verb */
                *(uint32_t *)((uint8_t *)pld + 0x0a) = ent_ptr_intern((void *)mask);
                *(uint32_t *)((uint8_t *)pld + 0x0e) = 0;        /* no shared verb table */
                *(uint16_t *)((uint8_t *)pld + 0x12) = verb_id;  /* direct verb_id */
                *(uint16_t *)((uint8_t *)pld + 0x08) = 2;        /* kind=2 click */
                LinkEntityToList(&g_click_list_head, pld, 0);
                RegisterEntityForUpdate(pld, 4, id);
                if (pool_idx < (uint16_t)(pool_count - 1)) ++pool_idx;
            }
        }
        ++spawned;
    }

    fprintf(stderr, "[script] reg-mask-list id=%u asset=%s frames=%u spawned=%d "
                    "click=0x%08X pool[count=%u] table=%s\n",
            id, a->name, a->frame_count, spawned,
            click_ptr, pool_count,
            verb_table ? "verb(E700)" : "mask(E6D8)");
}
