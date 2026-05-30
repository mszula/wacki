/* src/scene/spawn.c — entity spawn (op 0x30) + actor pre-spawn.
 *
 * Two entry points:
 *
 * - SpawnActorEntity: dedicated path for pre-spawning the two
 * controllable actors (Ebek, Fjej) at game start. Builds the
 * render entity, binds the atlas, and registers the (kind=2, id)
 * + (kind=4, id) pair so FindEntityByVerbId can resolve verb-1 /
 * verb-2 to the actor entities.
 *
 * - ScriptCallSpawnEntity: op 0x30 dispatch — generic spawn path
 * called by enter_scripts to populate scenes with props and NPCs.
 * Allocates the entity, binds the atlas + bytecode resolved via
 * the PE loader, optionally wires up a click payload from a click
 * pool, and registers + links into the appropriate lists.
 */

#include "wacki.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern Entity *g_actor[2];
extern Entity *g_render_list_head;
extern Entity *g_click_list_head;
extern Entity *AllocEntity(uint16_t w, uint16_t h, uint16_t kind, uint16_t flags);
extern void    LinkEntityToList(Entity **head, Entity *e, int position);
extern void    RegisterEntityForUpdate(Entity *e, uint16_t kind, uint16_t id);
extern void   *FindUpdateRegistration(uint16_t kind, uint16_t id);
extern const void *xlat_binary_ptr(uint32_t addr);
extern const char *xlat_asset_name(uint32_t addr);
extern void   *xmalloc(uint32_t sz);

Entity *SpawnActorEntity(uint16_t id, AnimAsset *atlas, uint16_t init_frame,
                         int16_t init_x, int16_t init_y)
{
    if (!atlas) return NULL;
    uint16_t w = atlas->max_w, h = atlas->max_h;
    /* alpha-plane flag = 0 (actors aren't alpha) */
    Entity *e = AllocEntity(w, h, 1, 0);
    if (!e) return NULL;
    uint8_t *eb = (uint8_t *)e;
    /* +0x08 flags1/flags2 — initialised by AllocEntity (kind/flags) */
    /* Set walk-with-perspective bit (flag 0x40 of +0x08 is for scaling,
 * flag 4 of +0x08 is "mirror") — actors are normally upright,
 * left-facing native, so just leave flags as default-set. */
    *(uint32_t *)(eb + 0x2C) = 0;                /* no per-entity script */
    *(uint32_t *)(eb + 0x28) = ent_ptr_intern((void *)atlas);
    *(uint16_t *)(eb + 0x30) = init_frame;
    *(uint16_t *)(eb + 0x22) = (uint16_t)init_x;  /* anchor X (foot) */
    *(uint16_t *)(eb + 0x24) = (uint16_t)init_y;  /* anchor Y (foot) */
    /* Set FLAG_2 (bit 1 of +0x3A) so op 0x28 + post-exec see "foot
 * anchor active" — without this, drawn position doesn't apply
 * frame's hot_x/y compensation. */
    *(uint8_t *)(eb + 0x3A) |= 2;
    /* Pre-compensate the drawn position so the actor renders correctly
 * on frame 0 BEFORE any walker tick fires. Mirrors the post-exec
 * foot-anchor compensation in ExecEntityScript:
 * drawn = anchor + atlas->off_draw[frame]
 * (×1 path since flags & 0x400 / 4 are not set on actors at spawn). */
    int16_t hot_x = 0, hot_y = 0;
    if (atlas->off_drawX && init_frame < atlas->frame_count)
        hot_x = (int16_t)atlas->off_drawX[init_frame];
    if (atlas->off_drawY && init_frame < atlas->frame_count)
        hot_y = (int16_t)atlas->off_drawY[init_frame];
    *(int16_t *)(eb + 0x0A) = (int16_t)(init_x + hot_x);
    *(int16_t *)(eb + 0x0C) = (int16_t)(init_y + hot_y);
    /* foot_y for z-sort (= bottom edge of drawn sprite). */
    uint16_t sh = (atlas->off_heights && init_frame < atlas->frame_count)
                 ? atlas->off_heights[init_frame] : 0;
    *(int16_t *)(eb + 0x26) = (int16_t)(*(int16_t *)(eb + 0x0C) + (int16_t)sh);

    RegisterEntityForUpdate(e, 2, id);
    LinkEntityToList(&g_render_list_head, e, 0);

    /* Click entity ( kind=4 payload allocation) —
 * stores a tiny verb table {count=1, verb_id=id} so 
 * resolves the actor by its id. The verb table is per-actor static
 * memory; we use a static array indexed by id. */
    static uint16_t s_actor_verb_tab[8][2];   /* {count, verb_id} pairs */
    if (id < 8) {
        s_actor_verb_tab[id][0] = 1;          /* count */
        s_actor_verb_tab[id][1] = id;         /* verb_id at frame 0 */
    }
    /* Click payload — full Entity alloc (B31 safety, see RegMaskList). */
    Entity *m = (Entity *)xmalloc(sizeof *m);
    if (m) {
        memset(m, 0, sizeof *m);
        *(uint32_t *)((uint8_t *)m + 0x0e) =
            ent_ptr_intern((void *)(id < 8 ? s_actor_verb_tab[id] : NULL));
        *(uint32_t *)((uint8_t *)m + 0x0a) = ent_ptr_intern((void *)e);
        *(uint16_t *)((uint8_t *)m + 8) = 1;  /* click kind=1 (sprite) */
        *(uint16_t *)((uint8_t *)m + 0x12) = id;  /* cached verb_id */
        LinkEntityToList(&g_click_list_head, m, 0);
        RegisterEntityForUpdate(m, 4, id);
    }
    fprintf(stderr, "[actor] spawn id=%u atlas=%s frame=%u at (%d,%d)\n",
            id, atlas->name, init_frame, init_x, init_y);
    return e;
}

/* ---- @ RunScriptInterpreter:
 *
 * asset = (1, id) // find loaded atlas by id
 * if (asset) {
 * flags = arg3 (16-bit operand)
 * if (flags & 0x510) flags &= ~4
 * w = asset->max_w; h = asset->max_h
 * if (flags & 4) { w <<= 1; h <<= 1; }
 * e = AllocEntity(w, h, 1, has_alpha_plane?)
 * e->flags2 |= flags
 * e->script_bytecode = arg2 (dword2)
 * e->current_anim = asset (dword?+0xa = piVar22[10])
 * RegisterEntityForUpdate(e, 2, id)
 * LinkEntityToList(&render_list_head, e, 0)
 * if (click_payload) {
 * m = alloc(0x14, 1)
 * m->payload = arg1 (dword1)
 * m->owner = e
 * m->flags = 1
 * LinkEntityToList(&click_list_head, m, 0)
 * RegisterEntityForUpdate(m, 4, id)
 * }
 * }
 */
void ScriptCallSpawnEntity(uint16_t id, uint16_t flags,
                           uint32_t click_payload_addr,
                           uint32_t script_addr)
{
    AnimAsset *asset = (AnimAsset *)FindUpdateRegistration(1, id);
    if (!asset) {
        fprintf(stderr, "[script] spawn id=%u: no asset registered (skipping)\n", id);
        return;
    }
    if (flags & 0x510) flags &= ~4u;
    /* Alpha-plane gate — case 0x30:
 * if ((asset->flag_22 & 1) || (flags & 0x2000) || (flags & 4))
 * alloc_flags = 1;
 * Earlier port used `asset->kind == 3` which collapsed bits 0 and 1
 * of the raw flag — would over-trigger alpha for visible-only assets
 * (kind=3 with flag bit 0 clear). */
    uint16_t alloc_flags = 0;
    if ((asset->flag_22 & 1) || (flags & 0x2000) || (flags & 4)) alloc_flags = 1;
    uint16_t w = asset->max_w, h = asset->max_h;
    if (flags & 4) { w = (uint16_t)(w << 1); h = (uint16_t)(h << 1); }
    Entity *e = AllocEntity(w, h, 1, alloc_flags);
    if (!e) return;
    /* op 0x30 SPAWN body:
 * *(ushort *)(piVar22 + 2) = *(ushort *)(piVar22 + 2) | uVar29;
 * if ((uVar29 & 0x800) != 0) *(byte *)(piVar22 + 8) = 1;
 *
 * `+ 2` on an int* in the original = byte offset 8 (the 16-bit flags).
 * `+ 8` on an int* = byte offset 0x20 (an unrelated state byte). */
    *(uint16_t *)((uint8_t *)e + 8) |= flags;
    if (flags & 0x800) ((uint8_t *)e)[0x20] = 1;
    /* Visibility is gated by EntityRenderAll directly: bit 0x80 = hidden
 * (op 0x3E), bit 0x2000 = "wait-for-enable" (alpha-plane spawn whose
 * per-entity script will fill its private pixel buffer). Neither is
 * forced here — the SPAWN simply OR's the script's flags as the
 * original does. */
    /* Bind script bytecode + asset via slot-ID intern table (Entity stores
 * 4-byte slot IDs at the original 32-bit pointer offsets; real C
 * pointers don't fit in 4 bytes on 64-bit).
 *
 * case 0x30 (Ghidra line ~960):
 * piVar22[0xb] = iVar3; // entity[+0x2C] = script_addr
 * piVar22[10] = iVar17; // entity[+0x28] = asset_ptr
 *
 * No frame / anchor pre-init — AllocEntity already zeroed the buffer,
 * and the per-entity script (bound at +0x2C) drives position via
 * op 0x07 SET_ANCHOR on its first tick. Earlier port pre-set
 * +0x22/+0x24 to drawX[0]/drawY[0] which DOUBLED the apparent offset
 * for static (script-less) entities — divergence from original. */
    extern const void *xlat_binary_ptr(uint32_t);
    const void *bc = xlat_binary_ptr(script_addr);
    *(uint32_t *)((uint8_t *)e + 0x2C) = bc ? ent_ptr_intern((void *)bc) : 0;
    *(uint32_t *)((uint8_t *)e + 0x28) = ent_ptr_intern((void *)asset);

    extern void RegisterEntityForUpdate(Entity *, uint16_t, uint16_t);
    extern void LinkEntityToList(Entity **, Entity *, int);
    extern Entity *g_render_list_head;
    extern Entity *g_click_list_head;
    RegisterEntityForUpdate(e, 2, id);
    LinkEntityToList(&g_render_list_head, e, 0);

    if (click_payload_addr) {
        /* Click payload — full Entity alloc (B31 safety). */
        Entity *m = (Entity *)xmalloc(sizeof *m);
        if (m) {
            memset(m, 0, sizeof *m);
            const void *payload = xlat_binary_ptr(click_payload_addr);
            *(uint32_t *)((uint8_t *)m + 0x0e) = payload ? ent_ptr_intern((void *)payload) : 0;
            *(uint32_t *)((uint8_t *)m + 0x0a) = ent_ptr_intern((void *)e);
            *(uint16_t *)((uint8_t *)m + 8) = 1;
            LinkEntityToList(&g_click_list_head, m, 0);
            RegisterEntityForUpdate(m, 4, id);
        }
    }
    fprintf(stderr, "[script] spawn id=%u asset=%p script=0x%08x flags=0x%04x → %p\n",
            id, (void *)asset, script_addr, flags, (void *)e);
}
