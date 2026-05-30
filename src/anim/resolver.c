/* src/anim/resolver.c — animation script lookup + bind.
 *
 * Two helpers used by the walker bind path:
 *
 * FindAnimationScript(scripts, name): search a per-stage `[animacja]`
 * table for the named anim. Returns a pointer to the matching
 * entry or NULL.
 *
 * PlayActorAnimByPath(entity, path, target_x, target_y): given an
 * anim path (e.g. the L/R/U/D walk anim for an actor), bind the
 * anim's bytecode to the entity and patch op 0x15 with the
 * resolved target so the per-entity VM steps the actor on its
 * next tick.
 */

#include "wacki.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern uint32_t ent_ptr_intern(void *p);
extern const void *xlat_binary_ptr(uint32_t addr);

/* =========== scripts ==================================================== */
void *FindAnimationScript(void *scripts, const char *name)
{ (void)scripts; (void)name; return NULL; }
/* PlayActorAnimByPath
 * (UpdateActorMovement @ 0x004061D0, lines ~1054-1064 of decompile):
 * (entity); // full reset
 *
 * FindKeyInTaggedTable returns idx in ushort units. The op 0x15 WALK_TO_X
 * lives at byte offset `idx*2` in the bytecode; its first operand (a0 = X)
 * starts at +2, second (a1 = Y) at +4. We write the click target into
 * those byte positions BEFORE binding so the per-entity VM walker sees
 * the fresh destination on its next tick.
 *
 * Patching IS the original mechanism — bytecode lives in writable PE
 * memory and gets clobbered each click. Different clicks overwrite the
 * same op 0x15. */
void  PlayActorAnimByPath(Entity *e, const char *path, int16_t x, int16_t y)
{
    if (!e || !path) return;
    uint16_t idx = FindKeyInTaggedTable(path, 0x15, -1);
    if (idx == 0) {
        /* No op 0x15 found → either not a walker script or already at
 * target (original treats this as "do nothing"). */
        return;
    }
    /* Patch in-place — PE image is malloc'd, so memory is writable. */
    uint8_t *bc = (uint8_t *)path;
    *(int16_t *)(bc + idx * 2 + 2) = x;
    *(int16_t *)(bc + idx * 2 + 4) = y;

    /* reset (same block used by op 0x0E / 0x0F / 0x33). */
    uint8_t *eb = (uint8_t *)e;
    *(uint16_t *)(eb + 0x3A) &= (uint16_t)~5u;
    *(uint16_t *)(eb + 0x38) = 0;
    *(uint16_t *)(eb + 0x36) = 0;
    *(uint16_t *)(eb + 0x34) = 0;
    *(uint16_t *)(eb + 0x32) = 0;
    *(uint16_t *)(eb + 0x3C) = 0;
    *(uint16_t *)(eb + 0x40) = 0;
    *(uint16_t *)(eb + 0x42) = 0;
    *(uint32_t *)(eb + 0x4C) = 0;
    *(uint32_t *)(eb + 0x50) = 0;

    /* Bind walker bytecode + set walker-active bit 4 of +0x3A. */
    extern uint32_t ent_ptr_intern(void *p);
    *(uint32_t *)(eb + 0x2C) = ent_ptr_intern((void *)path);
    *(uint16_t *)(eb + 0x30) = 0;                /* frame = 0 */
    eb[0x3A] |= 4;                                /* walker-active flag */

    /* NOTE:
 * Plant the walker path right here so +0x4C/+0x50 are non-zero before
 * play_demo_scene's main loop polls "walker done" (`wdx == 0 && wdy ==
 * 0`) for the pending-scene-exit case. Without this, the same frame
 * that calls BindActorWalker also passes the done-check (since
 * just zero'd them) → scene transitions fire on click
 * without the actor ever stepping toward the exit.
 *
 * Original engine doesn't hit this race: PGFT polls walker state
 * AFTER per-entity VM has had a tick (op 0x15 plants path on first
 * fire). Our port's scene-exit check sits in play_demo_scene's outer
 * loop and runs AFTER ProcessGameFrameTickInner — which now executes
 * the per-entity VM, but BindActorWalker runs INSIDE PGFT Inner (via
 * HandleSceneInput at the tail). Result: VM has already ticked this
 * frame BEFORE the walker was bound. Path stays unplanted until next
 * frame's VM tick → check fires too early.
 *
 * Planting here at bind time matches the post-tick state and removes
 * the race. Op 0x15's first-plant branch is gated on dxr/dyr == 0
 * (no path), so once we've planted, it just steps. */
    int16_t cx = *(int16_t *)(eb + 0x22);        /* anchor X (foot) */
    int16_t cy = *(int16_t *)(eb + 0x24);        /* anchor Y (foot) */
    int16_t sdx = (int16_t)(x - cx);
    int16_t sdy = (int16_t)(y - cy);
    int16_t adx = sdx < 0 ? (int16_t)-sdx : sdx;
    int16_t ady = sdy < 0 ? (int16_t)-sdy : sdy;
    int16_t maxlen = adx > ady ? adx : ady;
    int32_t inc_x = 0, inc_y = 0;
    if (maxlen) {
        inc_x = ((int32_t)(x - cx) * 0x10000) / maxlen;
        inc_y = ((int32_t)(y - cy) * 0x10000) / maxlen;
    }
    /* Shift via uint32 to avoid signed-shift UB when cx/cy < 0 (actor
 * off-screen left/top). Bit pattern is identical to original's
 * `param_1 << 0x10` @ on 2's complement; UBSan's
 * shift-out-of-bounds abort variant fires for cx=-80 (rob4 enters
 * komnata 5 from off-screen left). */
    *(int32_t *)(eb + 0x44) = (int32_t)((uint32_t)(uint16_t)cx << 16);
    *(int32_t *)(eb + 0x48) = (int32_t)((uint32_t)(uint16_t)cy << 16);
    *(int32_t *)(eb + 0x4C) = inc_x;
    *(int32_t *)(eb + 0x50) = inc_y;
    *(int16_t *)(eb + 0x54) = x;
    *(int16_t *)(eb + 0x56) = y;
    /* Clear bit 2 of +0x3A — op 0x15 plant branch does the same, signals
 * "path is planted, no need to re-plant on first tick". */
    *(uint16_t *)(eb + 0x3A) &= (uint16_t)~4u;
    (void)cx; (void)cy; (void)inc_x; (void)inc_y;
}
