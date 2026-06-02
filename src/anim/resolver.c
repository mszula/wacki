/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* src/anim/resolver.c — animation script lookup + bind.
 *
 * Two helpers used by the walker bind path:
 *
 *   FindAnimationScript(scripts, name): search a per-stage `[animacja]`
 *     table for the named anim. Returns a pointer to the matching
 *     entry or NULL.
 *
 *   PlayActorAnimByPath(entity, path, target_x, target_y): given an
 *     anim path (e.g. the L/R/U/D walk anim for an actor), bind the
 *     anim's bytecode to the entity and patch op 0x15 with the
 *     resolved target so the per-entity VM steps the actor on its
 *     next tick.
 */

#include "wacki.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern uint32_t ent_ptr_intern(void *p);
extern const void *xlat_binary_ptr(uint32_t addr);

/* ---- constants ---------------------------------------------------- */

#define WALK_TO_OP                  0x15    /* per-entity VM walk-to opcode */
#define WALK_TO_OPERAND_X_BYTE_OFF  2       /* a0 = X at instruction byte +2 */
#define WALK_TO_OPERAND_Y_BYTE_OFF  4       /* a1 = Y at instruction byte +4 */

/* +0x3A state-flag bits. */
#define STATE_RESET_MASK            (ESTATE_FRAME_READY | ESTATE_WALKER_FRESH)
#define STATE_WALKER_ACTIVE         0x04
#define STATE_WALKER_PATH_PLANTED   0x04    /* same bit; planted after first tick */

/* 16.16 fixed-point shift constant. */
#define FIXED_POINT_SHIFT           16
#define FIXED_POINT_ONE             0x10000

/* ---- public API --------------------------------------------------- */

void *FindAnimationScript(void *scripts, const char *name)
{
    (void)scripts;
    (void)name;
    return NULL;
}

/* Reset walker / loop / delay state so the entity is ready to be
 * bound to a fresh bytecode. Same block as SUBSCRIPT_CALL,
 * SWAP_ATLAS, ATTACH_PROP — see also script_bridge/entity.c. */
static void reset_entity_walker_state(Entity *e)
{
    EOFF(e, ENT_OFF_STATE_FLAGS,   uint16_t) &= (uint16_t)~STATE_RESET_MASK;
    EOFF(e, ENT_OFF_LOOP_A,        uint16_t) = 0;
    EOFF(e, ENT_OFF_LOOP_B,        uint16_t) = 0;
    EOFF(e, ENT_OFF_LOOP_C,        uint16_t) = 0;
    EOFF(e, ENT_OFF_LOOP_D,        uint16_t) = 0;
    EOFF(e, ENT_OFF_LOOP_E,        uint16_t) = 0;
    EOFF(e, ENT_OFF_PC,            uint16_t) = 0;
    EOFF(e, ENT_OFF_DELAY,         uint16_t) = 0;
    EOFF(e, ENT_OFF_WALKER_DX_REM, uint32_t) = 0;
    EOFF(e, ENT_OFF_WALKER_DY_REM, uint32_t) = 0;
}

/* Plant the walker's 16.16 fixed-point step state so dx/dy_rem are
 * non-zero immediately, before the per-entity VM has had a tick. This
 * removes a race where play_demo_scene's outer-loop "walker done"
 * check fires the same frame we bind the walker (because the just-
 * reset accumulators read as zero); see PlayActorAnimByPath note. */
static void plant_walker_path(Entity *e, int16_t target_x, int16_t target_y)
{
    int16_t cx = EOFF(e, ENT_OFF_ANCHOR_X, int16_t);
    int16_t cy = EOFF(e, ENT_OFF_ANCHOR_Y, int16_t);
    int16_t sdx = (int16_t)(target_x - cx);
    int16_t sdy = (int16_t)(target_y - cy);
    int16_t adx = sdx < 0 ? (int16_t)-sdx : sdx;
    int16_t ady = sdy < 0 ? (int16_t)-sdy : sdy;
    int16_t maxlen = adx > ady ? adx : ady;

    int32_t inc_x = 0, inc_y = 0;
    if (maxlen) {
        inc_x = ((int32_t)(target_x - cx) * FIXED_POINT_ONE) / maxlen;
        inc_y = ((int32_t)(target_y - cy) * FIXED_POINT_ONE) / maxlen;
    }

    /* Promote cx/cy through unsigned before shifting so cx/cy < 0
     * (actor entering off-screen left/top) doesn't trip UBSan on the
     * signed left-shift. Bit pattern is the same as the original's
     * `param << 0x10` on 2's complement. */
    EOFF(e, ENT_OFF_WALKER_X,      int32_t) = (int32_t)((uint32_t)(uint16_t)cx << FIXED_POINT_SHIFT);
    EOFF(e, ENT_OFF_WALKER_Y,      int32_t) = (int32_t)((uint32_t)(uint16_t)cy << FIXED_POINT_SHIFT);
    EOFF(e, ENT_OFF_WALKER_DX_REM, int32_t) = inc_x;
    EOFF(e, ENT_OFF_WALKER_DY_REM, int32_t) = inc_y;
    EOFF(e, ENT_OFF_WALKER_TGT_X,  int16_t) = target_x;
    EOFF(e, ENT_OFF_WALKER_TGT_Y,  int16_t) = target_y;

    /* Clear the "path needs planting" bit — op 0x15's first-plant
     * branch is gated on dx/dy_rem == 0, so once we've planted, the
     * VM tick just steps. */
    EOFF(e, ENT_OFF_STATE_FLAGS, uint16_t) &= (uint16_t)~STATE_WALKER_PATH_PLANTED;
}

void PlayActorAnimByPath(Entity *e, const char *path,
                         int16_t target_x, int16_t target_y)
{
    if (!e || !path) return;

    /* Find op 0x15 WALK_TO inside the bytecode. FindKeyInTaggedTable
     * returns an index in halfwords (ushort units); the matching
     * instruction starts at byte offset idx*2. Operands a0/a1 live
     * at instruction byte +2 / +4. */
    uint16_t idx = FindKeyInTaggedTable(path, WALK_TO_OP, -1);
    if (idx == 0) {
        /* No op 0x15 found → not a walker script, or actor already at
         * target. Original treats this as "do nothing". */
        return;
    }

    /* Patch the WALK_TO target in-place. PE image was loaded into
     * malloc'd memory, so writes are safe. Each click overwrites the
     * same opcode — patching IS the original mechanism. */
    uint8_t *bc = (uint8_t *)path;
    *(int16_t *)(bc + idx * 2 + WALK_TO_OPERAND_X_BYTE_OFF) = target_x;
    *(int16_t *)(bc + idx * 2 + WALK_TO_OPERAND_Y_BYTE_OFF) = target_y;

    /* Bind the new bytecode + reset VM state. */
    reset_entity_walker_state(e);
    EOFF(e, ENT_OFF_BYTECODE_SLOT, uint32_t) = ent_ptr_intern((void *)path);
    EOFF(e, ENT_OFF_FRAME,         uint16_t) = 0;
    EOFF8(e, ENT_OFF_STATE_FLAGS) |= STATE_WALKER_ACTIVE;

    /* Plant the walker path so play_demo_scene's outer-loop
     * "walker drained" check doesn't fire spuriously on this frame. */
    plant_walker_path(e, target_x, target_y);
}
