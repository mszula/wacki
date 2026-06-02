/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* src/actor/vm.c — per-entity script interpreter and walker tick.
 *
 * Every entity in the scene carries a small bytecode program that
 * drives its animation, position, and timing. The per-entity VM
 * dispatches one tick's worth of those instructions for every
 * registered entity each game frame:
 *
 * EntityWalkerTick
 * → walks the render list (kind=0)
 * → for each entity with an atlas bound: ExecEntityScript
 *
 * Bytecode format is `[op:u8 +0][dlt:u8 +1][operand:u16 +2]…` with a
 * stride of `dlt * 2` bytes (half of the main script VM's `len * 4`).
 * Opcodes 0x00..0x24 cover anchor moves, frame stepping, walker setup,
 * stops, oscillators, atlas swaps, and the click-event enqueue. END is
 * 0x21; LABEL targets are 0x0A.
 *
 * The frame-delta gate at the top of ExecEntityScript yields to the
 * next tick when delay is still positive — this is what makes
 * animation timing scale with frame pacing.
 *
 * Post-execution always runs a tidy-up block: mirrors anchor → drawn
 * position, applies oscillation offsets, stashes atlas frame W/H,
 * clamps scale, and applies foot-anchor compensation.
 */

#include "wacki.h"
#include "internal.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

extern uint32_t g_frame_delta_ms;
extern uint16_t g_frame_delta_ticks;

/* ---- opcode constants --------------------------------------------- */

enum {
    PVM_SET_ANCHOR_XY      = 0x00,
    PVM_SET_ANCHOR_X       = 0x01,
    PVM_SET_ANCHOR_Y       = 0x02,
    PVM_X_OSCILLATE        = 0x03,
    PVM_Y_OSCILLATE        = 0x04,
    PVM_SET_POS_FROM_FRAME = 0x05,
    PVM_SET_FRAME          = 0x06,
    PVM_IF_FRAME           = 0x07,
    PVM_FRAME_RANGE_CHECK  = 0x08,
    PVM_SET_DELAY          = 0x09,
    PVM_LABEL              = 0x0A,
    PVM_CLEAR_LOOP_CTRS    = 0x0B,
    PVM_LOOP_A             = 0x0C,
    PVM_LOOP_B             = 0x0D,
    PVM_LOOP_C             = 0x0E,
    PVM_SET_RAND_FRAME     = 0x0F,
    PVM_SET_DELAY_PAUSE    = 0x10,
    PVM_SET_RAND_DELAY     = 0x11,
    PVM_ADVANCE_FRAME      = 0x12,
    PVM_WAIT_FRAME_LABEL   = 0x13,
    PVM_RAND_GATE          = 0x14,
    PVM_WALK_TO_X          = 0x15,
    PVM_WALK_TO_XY         = 0x16,
    PVM_ADD_X              = 0x17,
    PVM_ADD_Y              = 0x18,
    PVM_JUMP_IF_BIT0       = 0x19,
    PVM_JUMP_IF_NOT_BIT0   = 0x1A,
    PVM_SET_FLAG_2         = 0x1B,
    PVM_CLEAR_FLAG_2       = 0x1C,
    PVM_STOP_TICK          = 0x1D,
    PVM_SUBSCRIPT_CALL     = 0x1E,
    PVM_STOP_RESET         = 0x1F,
    PVM_STOP_KEEP_PC       = 0x20,
    PVM_END                = 0x21,
    PVM_ENQUEUE_CLICK      = 0x22,
    PVM_SWAP_ATLAS         = 0x23,
    PVM_SET_FADE           = 0x24,
};

#define LABEL_WILDCARD          0xFFFFu
#define MAX_SCAN_ITERS          2048
#define MAX_INTERPRETER_ITERS   4096

/* Entity state-flag bits at +0x3A. */
#define ENT_FLAG_FRAME_READY    0x01
#define ENT_FLAG_ANIM_ACTIVE    0x02
#define ENT_FLAG_WALKER_FRESH   0x04

/* Entity primary-flag bits at +0x08/+0x09 (16-bit). */
#define ENT_PFLAG_DOUBLED       0x0004   /* sprite drawn at 2× */
#define ENT_PFLAG_FADING_OUT    0x0040   /* clears +0x26 when frame changes */
#define ENT_PFLAG_FOOT_BAKED    0x0020   /* set when +0x26 has been zeroed by fade-out */
#define ENT_PFLAG_NO_FOOT_BAKE  0x0200   /* skip the standard +0x26 = +0x0C + +0x10 bake */
#define ENT_PFLAG_PERSPECTIVE   0x0400   /* foot anchor uses scale_pct from +0x58 */

/* Scaling clamp: drawn scale_pct at +0x58 may not exceed 0xA0 (160%). */

/* Field offsets used by the post-exec block (the opcode bodies still
 * use raw constants — they read closer to the original bytecode that
 * way). */

extern const void *xlat_binary_ptr(uint32_t addr);
extern int         PeLoaderContainsVA(uint32_t va);
extern void       *FindUpdateRegistration(uint16_t kind, uint16_t id);
extern void        TriggerFrameSfx(const char *asset_name, int frame);
extern void        EnqueueClickEvent(uint16_t obj, uint16_t verb);

/* Resolve a PE-binary virtual address to a usable pointer in the host
 * address space. Some scripts reference oscillation / lookup tables by
 * their original PE VA — the PE loader maps the original image and
 * xlat_binary_ptr translates the address. Fallback to a raw cast for
 * addresses outside the loaded PE range (handful of edge cases). */
static const int16_t *resolve_pe_table(uint32_t addr)
{
    if (PeLoaderContainsVA(addr)) {
        return (const int16_t *)xlat_binary_ptr(addr);
    }
    return (const int16_t *)(uintptr_t)addr;
}

/* Scan one bytecode block for a LABEL (op 0x0A) whose argument matches
 * `target_id`. Returns the new pc (in halfwords from the base), or 0
 * if the label isn't found — callers treat 0 as "restart from top".
 *
 * `target_id == LABEL_WILDCARD` matches the first label encountered
 * regardless of argument (used by subroutine-entry lookup).
 *
 * Used by opcodes that jump by label: WAIT_FRAME_LABEL, RAND_GATE, the
 * LOOP_A/B/C family, and the conditional JUMP_IF_*_BIT0 pair. */
static uint16_t scan_for_label(const uint8_t *bytecode, uint16_t target_id)
{
    uint16_t pc = 0;
    for (int safety = 0; safety < MAX_SCAN_ITERS; ++safety) {
        const uint8_t *p  = bytecode + (size_t)pc * 2;
        uint8_t        op = p[0];

        if (op == PVM_END) return 0;
        if (op == PVM_LABEL) {
            uint16_t arg = (uint16_t)(p[2] | (p[3] << 8));
            if (target_id == LABEL_WILDCARD || arg == target_id) {
                return pc;
            }
        }

        uint8_t dlt = p[1];
        if (dlt == 0) return 0;        /* defensive — malformed bytecode */
        pc = (uint16_t)(pc + dlt);
    }
    return 0;
}

/* Reset the walker state of an entity to "idle, fresh, no path planted".
 * Shared by SUBSCRIPT_CALL and SWAP_ATLAS, which both nuke pending
 * walks and animation timers when they switch bytecodes or atlases. */
static void reset_entity_walker_state(Entity *e)
{
    EOFF(e, ENT_OFF_STATE_FLAGS, uint16_t) &=
        (uint16_t)~(ENT_FLAG_FRAME_READY | ENT_FLAG_WALKER_FRESH);
    EOFF(e, ENT_OFF_LOOP_A,        uint16_t) = 0;
    EOFF(e, ENT_OFF_LOOP_B,        uint16_t) = 0;
    EOFF(e, ENT_OFF_LOOP_C,        uint16_t) = 0;
    EOFF(e, ENT_OFF_LOOP_D,        uint16_t) = 0;
    EOFF(e, ENT_OFF_LOOP_E,        uint16_t) = 0;
    EOFF(e, ENT_OFF_DELAY,         uint16_t) = 0;
    EOFF(e, ENT_OFF_PC,            uint16_t) = 0;
    EOFF(e, ENT_OFF_WALKER_DX_REM, uint32_t) = 0;
    EOFF(e, ENT_OFF_WALKER_DY_REM, uint32_t) = 0;
}

/* ============================================================== *
 * Main interpreter.
 *
 * `keep_running` is the inner-loop continuation flag — opcodes that
 * stop execution for this tick (delay/pause family, STOP_*) clear it.
 *
 * `frame_changed` records whether the tick touched the displayed frame
 * (set by opcodes that move the entity or swap frames). Used by the
 * post-exec block to decide when foot-fade-on-touch fires.
 * ============================================================== */
static void ExecEntityScript(Entity *e)
{
    if (!e) return;
    AnimAsset *atlas =
        (AnimAsset *)ent_ptr_resolve(EOFF(e, ENT_OFF_ATLAS_SLOT, uint32_t));
    if (!atlas) return;

    const uint8_t *bytecode =
        (const uint8_t *)ent_ptr_resolve(EOFF(e, ENT_OFF_BYTECODE_SLOT, uint32_t));
    if (!bytecode) return;

    uint16_t pc            = EOFF(e, ENT_OFF_PC, uint16_t);
    int      keep_running  = 1;
    int      frame_changed = 0;
    const int16_t *osc_table_x = NULL;
    const int16_t *osc_table_y = NULL;

    /* Frame-delta gate. Decrement the per-entity delay counter by this
 * frame's tick budget; if it's still positive, skip the opcode loop
 * and jump straight to post-exec (entity yields this frame).
 *
 * Once the delay reaches zero, optionally reload it from the
 * "period" field at +0x3E — SET_DELAY (op 0x09) writes both fields
 * with the same value so an entity that needs a steady cadence
 * (e.g. blinking light) re-arms automatically. */
    int16_t delay = (int16_t)EOFF(e, ENT_OFF_DELAY, uint16_t);
    delay = (int16_t)(delay - (int16_t)g_frame_delta_ticks);
    EOFF(e, ENT_OFF_DELAY, uint16_t) = (uint16_t)delay;
    if (delay > 0) goto post_exec;
    {
        int16_t reset = (int16_t)EOFF(e, ENT_OFF_DELAY_RESET, uint16_t);
        EOFF(e, ENT_OFF_DELAY, uint16_t) = (uint16_t)(delay + reset);
        if (reset == 0) EOFF(e, ENT_OFF_DELAY, uint16_t) = 0;
    }

    /* Safety counter — bail after a fixed number of iterations to
 * guarantee we never wedge the whole game on a malformed script
 * (e.g. unresolved SUBSCRIPT_CALL with a backward jump and no
 * STOP). The original VM doesn't bound the loop; it relies on
 * every script having a reachable STOP_TICK, which doesn't hold
 * for the partially-ported subset. */
    int safety = MAX_INTERPRETER_ITERS;
    while (keep_running && safety-- > 0) {
        /* Re-read bytecode pointer at the top of each iteration:
 * SUBSCRIPT_CALL mutates +0x2C and the very next instruction
 * must come from the new program. Caching the pointer at
 * function entry would silently keep executing the OLD
 * bytecode after a tail-call. */
        bytecode = (const uint8_t *)ent_ptr_resolve(EOFF(e, ENT_OFF_BYTECODE_SLOT, uint32_t));
        if (!bytecode) break;

        const uint8_t *p       = bytecode + (size_t)pc * 2;
        uint8_t        op      = p[0];
        uint8_t        dlt     = p[1];
        uint16_t       arg     = (uint16_t)(p[2] | (p[3] << 8));
        uint16_t       next_pc = (uint16_t)(pc + dlt);

        switch (op) {

        case PVM_SET_ANCHOR_XY:
            EOFF(e, ENT_OFF_ANCHOR_X, uint16_t) = arg;
            EOFF(e, ENT_OFF_ANCHOR_Y, uint16_t) = (uint16_t)(p[4] | (p[5] << 8));
            frame_changed = 1;
            break;

        case PVM_SET_ANCHOR_X:
            EOFF(e, ENT_OFF_ANCHOR_X, uint16_t) = arg;
            frame_changed = 1;
            break;

        case PVM_SET_ANCHOR_Y:
            EOFF(e, ENT_OFF_ANCHOR_Y, uint16_t) = arg;
            frame_changed = 1;
            break;

        case PVM_X_OSCILLATE: {
            /* Operand at p+4 is a PE virtual address pointing to a small
 * table {count, offset[0], offset[1], ...}. Counter at +0x40
 * advances through the table; the post-exec block adds
 * `osc_table_x[counter]` to the drawn anchor X. */
            uint32_t tbl_addr;
            memcpy(&tbl_addr, p + 4, 4);
            osc_table_x = resolve_pe_table(tbl_addr);

            uint16_t counter = EOFF(e, ENT_OFF_LOOP_D, uint16_t);
            uint16_t first   = osc_table_x ? (uint16_t)osc_table_x[0] : 0;
            if ((int)counter < (int)first) {
                EOFF(e, ENT_OFF_LOOP_D, uint16_t) = counter + 1;
            } else {
                EOFF(e, ENT_OFF_LOOP_D, uint16_t) = 1;
            }
            break;
        }

        case PVM_Y_OSCILLATE: {
            uint32_t tbl_addr;
            memcpy(&tbl_addr, p + 4, 4);
            osc_table_y = resolve_pe_table(tbl_addr);

            uint16_t counter = EOFF(e, ENT_OFF_LOOP_E, uint16_t);
            uint16_t first   = osc_table_y ? (uint16_t)osc_table_y[0] : 0;
            if ((int)counter < (int)first) {
                EOFF(e, ENT_OFF_LOOP_E, uint16_t) = counter + 1;
            } else {
                EOFF(e, ENT_OFF_LOOP_E, uint16_t) = 1;
            }
            break;
        }

        case PVM_SET_POS_FROM_FRAME: {
            /* Use the current frame's per-frame anchor from the atlas
 * tables. NOTE: clamp frame to in-range; SWAP_ATLAS (0x23)
 * preserves +0x30 across atlas swap, so a stale high frame
 * index on a smaller new atlas could OOB the lookup. */
            uint16_t fid = EOFF(e, ENT_OFF_FRAME, uint16_t);
            if (atlas->frame_count && fid >= atlas->frame_count) {
                fid = (uint16_t)(atlas->frame_count - 1);
            }
            if (atlas->off_drawX) EOFF(e, ENT_OFF_ANCHOR_X, uint16_t) = atlas->off_drawX[fid];
            if (atlas->off_drawY) EOFF(e, ENT_OFF_ANCHOR_Y, uint16_t) = atlas->off_drawY[fid];
            break;
        }

        case PVM_IF_FRAME:
            /* Sets the "frame ready" bit when current frame == arg.
 * Used in combination with JUMP_IF_*_BIT0 to take a branch
 * when reaching a specific frame in an animation. */
            if (arg == EOFF(e, ENT_OFF_FRAME, uint16_t)) {
                EOFF8(e, ENT_OFF_STATE_FLAGS) |= ENT_FLAG_FRAME_READY;
            } else {
                EOFF(e, ENT_OFF_STATE_FLAGS, uint16_t) &= (uint16_t)~ENT_FLAG_FRAME_READY;
            }
            break;

        case PVM_FRAME_RANGE_CHECK: {
            /* Coerce `arg` into the current frame index iff the current
 * frame is inside [lo, hi]. Falls through to SET_FRAME, so
 * the effect is "if frame ∈ [lo, hi], keep it; otherwise
 * snap to `arg`". */
            uint16_t fid = EOFF(e, ENT_OFF_FRAME, uint16_t);
            uint16_t lo  = (uint16_t)(p[4] | (p[5] << 8));
            uint16_t hi  = (uint16_t)(p[6] | (p[7] << 8));
            if (lo <= fid && fid <= hi) arg = fid;
        }
        /* fallthrough */

        case PVM_SET_FRAME: {
            /* Clamp into the atlas's valid frame range, then fire the
 * per-frame SFX trigger.
 *
 * NOTE: the SFX gate has been simplified vs the original.
 * The original engine attaches a [sampl] table to each
 * asset at load time and only fires when that table is
 * non-NULL. The port keeps a scene-wide table keyed by
 * (asset_name, frame), so TriggerFrameSfx is called
 * unconditionally — it's a no-op when no entry matches. */
            uint16_t fc = atlas->frame_count;
            if (fc && arg >= fc) arg = (uint16_t)(fc - 1);
            EOFF(e, ENT_OFF_FRAME, uint16_t) = arg;
            frame_changed = 1;
            TriggerFrameSfx(atlas->name, (int)arg);
            break;
        }

        case PVM_SET_DELAY:
            /* Writes BOTH the current delay and the period so the
 * frame-delta gate at function top re-arms with the same
 * value after each yield. */
            EOFF(e, ENT_OFF_DELAY,       uint16_t) = arg;
            EOFF(e, ENT_OFF_DELAY_RESET, uint16_t) = arg;
            break;

        case PVM_CLEAR_LOOP_CTRS:
            EOFF(e, ENT_OFF_LOOP_A, uint16_t) = 0;
            EOFF(e, ENT_OFF_LOOP_B, uint16_t) = 0;
            EOFF(e, ENT_OFF_LOOP_C, uint16_t) = 0;
            break;

        case PVM_LOOP_A:
        case PVM_LOOP_B:
        case PVM_LOOP_C: {
            uint16_t off = (op == PVM_LOOP_A) ? ENT_OFF_LOOP_A
                         : (op == PVM_LOOP_B) ? ENT_OFF_LOOP_B
                                              : ENT_OFF_LOOP_C;
            uint16_t cnt = EOFF(e, off, uint16_t);
            if ((uint32_t)(cnt + 1) < arg) {
                /* Counter not yet at limit: jump back to the matching
 * label. NOTE: assign next_pc unconditionally — if the
 * label isn't found, scan_for_label returns 0 and the
 * loop wraps to the bytecode head. That's the intended
 * behavior for malformed scripts (matches the original);
 * silently skipping the jump would corrupt control flow
 * by continuing past the body. */
                int16_t  want  = (int16_t)(p[4] | (p[5] << 8));
                uint16_t found = scan_for_label(bytecode,
                                                want < 0 ? LABEL_WILDCARD : (uint16_t)want);
                next_pc = found;
                EOFF(e, off, uint16_t) = (uint16_t)(cnt + 1);
            } else {
                EOFF(e, off, uint16_t) = 0;
            }
            break;
        }

        case PVM_SET_RAND_FRAME: {
            /* Pick a random frame in [0, arg) and trigger the per-frame
 * SFX, same as PVM_SET_FRAME. */
            uint16_t fc = atlas->frame_count;
            if (fc && arg > fc) arg = fc;
            uint16_t f = (uint16_t)WackiRand(arg);
            EOFF(e, ENT_OFF_FRAME, uint16_t) = f;
            frame_changed = 1;
            TriggerFrameSfx(atlas->name, (int)f);
            break;
        }

        case PVM_SET_DELAY_PAUSE:
            EOFF(e, ENT_OFF_DELAY, uint16_t) = arg;
            /* fallthrough */
        case PVM_STOP_TICK:
            keep_running = 0;
            break;

        case PVM_SET_RAND_DELAY: {
            /* Pick a random delay in [0, arg) and yield this tick. */
            uint16_t d = (uint16_t)WackiRand(arg);
            EOFF(e, ENT_OFF_DELAY, uint16_t) = d;
            keep_running = 0;
            break;
        }

        case PVM_ADVANCE_FRAME: {
            /* arg < 0x80 → wrap to 0 on overflow (looping anim).
 * arg ≥ 0x80 → clamp to last frame (one-shot anim).
 * The bit selects which behavior — used by scripts that want
 * an idle animation to loop but a special-action animation
 * to "finish and stop". */
            EOFF8(e, ENT_OFF_STATE_FLAGS) |= ENT_FLAG_FRAME_READY;
            uint16_t f = (uint16_t)(EOFF(e, ENT_OFF_FRAME, uint16_t) + arg);
            EOFF(e, ENT_OFF_FRAME, uint16_t) = f;
            if (atlas->frame_count && f >= atlas->frame_count) {
                if (arg < 0x80) {
                    EOFF(e, ENT_OFF_FRAME, uint16_t) = 0;
                } else {
                    EOFF(e, ENT_OFF_FRAME, uint16_t) = (uint16_t)(atlas->frame_count - 1);
                }
                EOFF(e, ENT_OFF_STATE_FLAGS, uint16_t) &= (uint16_t)~ENT_FLAG_FRAME_READY;
            }
            frame_changed = 1;
            TriggerFrameSfx(atlas->name, (int)EOFF(e, ENT_OFF_FRAME, uint16_t));
            break;
        }

        case PVM_WAIT_FRAME_LABEL:
            next_pc = scan_for_label(bytecode, arg);
            break;

        case PVM_RAND_GATE:
            /* 50/50 jump: if the RNG returns 0, take the labeled branch. */
            if (WackiRand(2) == 0) {
                next_pc = scan_for_label(bytecode, arg);
            }
            break;

        case PVM_WALK_TO_X:
            /* WALK_TO_X reads Y from the operand at p+4..5; WALK_TO_XY
 * reuses the entity's current anchor Y. Both opcodes have
 * dlt=4 in every shipped script — reading p[4..7] is safe.
 * Falls through into WALK_TO_XY. */
        case PVM_WALK_TO_XY: {
            EOFF8(e, ENT_OFF_STATE_FLAGS) |= ENT_FLAG_FRAME_READY;

            /* If no path is planted yet, set up Bresenham state. */
            uint32_t dxr = EOFF(e, ENT_OFF_WALKER_DX_REM, uint32_t);
            uint32_t dyr = EOFF(e, ENT_OFF_WALKER_DY_REM, uint32_t);
            if (dxr == 0 && dyr == 0) {
                int16_t tx = (int16_t)arg;
                int16_t ty = (op == PVM_WALK_TO_X)
                             ? (int16_t)(p[4] | (p[5] << 8))
                             : (int16_t)EOFF(e, ENT_OFF_ANCHOR_Y, uint16_t);
                int16_t cx = (int16_t)EOFF(e, ENT_OFF_ANCHOR_X, uint16_t);
                int16_t cy = (int16_t)EOFF(e, ENT_OFF_ANCHOR_Y, uint16_t);
                int16_t sdx = tx - cx, sdy = ty - cy;
                int16_t adx = sdx < 0 ? -sdx : sdx;
                int16_t ady = sdy < 0 ? -sdy : sdy;
                int16_t maxlen = adx > ady ? adx : ady;

                int32_t inc_x = 0, inc_y = 0;
                if (maxlen) {
                    inc_x = (int32_t)((tx - cx) * 0x10000) / maxlen;
                    inc_y = (int32_t)((ty - cy) * 0x10000) / maxlen;
                }

                /* Promote cx/cy to uint16 before shifting so the
 * resulting accumulator has the same bit pattern as
 * the original — defined behavior for off-screen
 * entries where cx/cy may be negative. */
                EOFF(e, ENT_OFF_WALKER_X,        int32_t) = (int32_t)((uint32_t)(uint16_t)cx << 16);
                EOFF(e, ENT_OFF_WALKER_Y,        int32_t) = (int32_t)((uint32_t)(uint16_t)cy << 16);
                EOFF(e, ENT_OFF_WALKER_DX_REM,   int32_t) = inc_x;
                EOFF(e, ENT_OFF_WALKER_DY_REM,   int32_t) = inc_y;
                EOFF(e, ENT_OFF_WALKER_TGT_X,    int16_t) = tx;
                EOFF(e, ENT_OFF_WALKER_TGT_Y,    int16_t) = ty;
                EOFF(e, ENT_OFF_STATE_FLAGS,     uint16_t) &= (uint16_t)~ENT_FLAG_WALKER_FRESH;
            }

            /* Step count from the operand (with scale_pct applied for
 * perspective-doubled actors). */
            uint16_t step = (op == PVM_WALK_TO_X)
                            ? (uint16_t)(p[6] | (p[7] << 8))
                            : (uint16_t)(p[4] | (p[5] << 8));
            if (EOFF8(e, 9) & 4) {
                step = (uint16_t)((EOFF(e, ENT_OFF_SCALE_PCT, uint16_t) * step) / 100);
            }
            if ((int16_t)step == 0) step = 1;

            /* NOTE: aliasing-safe loop. The high half of the int32
 * accumulator at +0x44 / +0x48 is read separately as an
 * int16 at +0x46 / +0x4A — same memory aliased through two
 * types. Under `-fstrict-aliasing` the compiler may reorder
 * the int32 += with the subsequent int16 ==, causing the
 * walker to overshoot the target by 1 px per tick and then
 * never zero +0x4C/+0x50 → walker keeps stepping past.
 * The per-iteration local-variable copies force a fresh
 * read each iteration; `-fno-strict-aliasing` in the
 * Makefile is the belt-and-braces. */
            for (uint16_t k = 0; k < step; ++k) {
                int16_t pre_x = EOFF(e, ENT_OFF_WALKER_X_HI, int16_t);
                int16_t tgt_x = EOFF(e, ENT_OFF_WALKER_TGT_X, int16_t);
                if (pre_x == tgt_x) {
                    EOFF(e, ENT_OFF_WALKER_DX_REM, uint32_t) = 0;
                } else {
                    EOFF(e, ENT_OFF_WALKER_X, int32_t) += EOFF(e, ENT_OFF_WALKER_DX_REM, int32_t);
                }

                int16_t pre_y = EOFF(e, ENT_OFF_WALKER_Y_HI, int16_t);
                int16_t tgt_y = EOFF(e, ENT_OFF_WALKER_TGT_Y, int16_t);
                if (pre_y == tgt_y) {
                    EOFF(e, ENT_OFF_WALKER_DY_REM, uint32_t) = 0;
                } else {
                    EOFF(e, ENT_OFF_WALKER_Y, int32_t) += EOFF(e, ENT_OFF_WALKER_DY_REM, int32_t);
                }
            }

            /* Project the high halves back to the anchor. */
            EOFF(e, ENT_OFF_ANCHOR_X, uint16_t) = EOFF(e, ENT_OFF_WALKER_X_HI, uint16_t);
            EOFF(e, ENT_OFF_ANCHOR_Y, uint16_t) = EOFF(e, ENT_OFF_WALKER_Y_HI, uint16_t);

            if (EOFF(e, ENT_OFF_WALKER_DX_REM, uint32_t) == 0 &&
                EOFF(e, ENT_OFF_WALKER_DY_REM, uint32_t) == 0) {
                EOFF(e, ENT_OFF_STATE_FLAGS, uint16_t) &= (uint16_t)~ENT_FLAG_FRAME_READY;
            }
            /* Walker tick always counts as a frame-touch — the
 * walk-tail post-exec hooks need to fire on the moving
 * tick, not just the arrival tick. */
            frame_changed = 1;
            break;
        }

        case PVM_ADD_X:
            EOFF(e, ENT_OFF_ANCHOR_X, uint16_t) =
                (uint16_t)(EOFF(e, ENT_OFF_ANCHOR_X, uint16_t) + arg);
            break;

        case PVM_ADD_Y:
            EOFF(e, ENT_OFF_ANCHOR_Y, uint16_t) =
                (uint16_t)(EOFF(e, ENT_OFF_ANCHOR_Y, uint16_t) + arg);
            break;

        case PVM_JUMP_IF_BIT0:
            if (EOFF8(e, ENT_OFF_STATE_FLAGS) & ENT_FLAG_FRAME_READY) {
                next_pc = scan_for_label(bytecode, arg);
            }
            break;

        case PVM_JUMP_IF_NOT_BIT0:
            if (!(EOFF8(e, ENT_OFF_STATE_FLAGS) & ENT_FLAG_FRAME_READY)) {
                next_pc = scan_for_label(bytecode, arg);
            }
            break;

        case PVM_SET_FLAG_2:
            EOFF8(e, ENT_OFF_STATE_FLAGS) |= ENT_FLAG_ANIM_ACTIVE;
            break;

        case PVM_CLEAR_FLAG_2:
            EOFF(e, ENT_OFF_STATE_FLAGS, uint16_t) &= (uint16_t)~ENT_FLAG_ANIM_ACTIVE;
            break;

        case PVM_SUBSCRIPT_CALL: {
            /* Tail-call into a different bytecode block. NOTE: if the
 * target address isn't resolvable (subroutine not embedded
 * in our PE blob yet), terminate the tick and leave +0x2C
 * pointing at the current bytecode. Otherwise we'd reset
 * pc to 0 on the SAME bytecode and re-execute this opcode
 * forever → game hangs. */
            uint32_t addr;
            memcpy(&addr, p + 4, 4);
            const void *new_bc = xlat_binary_ptr(addr);
            if (!new_bc) {
                keep_running = 0;
                next_pc      = pc;
                break;
            }
            reset_entity_walker_state(e);
            EOFF(e, ENT_OFF_BYTECODE_SLOT, uint32_t) = ent_ptr_intern((void *)new_bc);
            next_pc = 0;
            break;
        }

        case PVM_STOP_RESET:
        case PVM_END:
            keep_running = 0;
            next_pc      = 0;
            break;

        case PVM_STOP_KEEP_PC:
            keep_running = 0;
            next_pc      = pc;
            break;

        case PVM_ENQUEUE_CLICK: {
            /* Push (obj, verb) onto the deferred click queue. The drain
 * happens at the tail of ProcessGameFrameTick, after this
 * walker has finished — running DispatchClickEvent inline
 * would mutate the entity list we're currently iterating. */
            uint16_t obj  = arg;
            uint16_t verb = (uint16_t)(p[4] | (p[5] << 8));
            EnqueueClickEvent(obj, verb);
            break;
        }

        case PVM_SWAP_ATLAS: {
            /* Swap to a different atlas registered at (kind=1, id=arg)
 * in the dispatch table. Resets walker + delay state; if
 * the lookup fails we just fall through to the reset (the
 * entity keeps its old atlas, which is the safest behavior). */
            AnimAsset *new_atlas = (AnimAsset *)FindUpdateRegistration(1, arg);
            if (new_atlas) {
                EOFF(e, ENT_OFF_ATLAS_SLOT, uint32_t) = ent_ptr_intern(new_atlas);
                EOFF(e, ENT_OFF_PC,         uint16_t) = 0;
                EOFF(e, ENT_OFF_LOOP_B,     uint16_t) = 0;
                EOFF(e, ENT_OFF_DELAY,      uint16_t) = 0;
            }
            reset_entity_walker_state(e);
            break;
        }

        case PVM_SET_FADE:
            /* Mark fading-out (bit 1 of the primary-flag byte) and seed
 * +0x26 with the script-supplied initial fade level. */
            EOFF8(e, 9) |= 2;
            EOFF(e, ENT_OFF_FOOT_Y, uint16_t) = arg;
            frame_changed = 1;
            break;

        default:
            /* Unknown opcode: advance without effect. */
            break;
        }

        pc = next_pc;
    }

    /* ============================================================== *
 * Post-exec tidy-up.
 *
 * Runs whether or not the opcode loop executed (the delay gate
 * branches straight here). Order matters: the foot-fade clear at
 * the very top runs BEFORE the standard foot-Y bake at the bottom,
 * so when both conditions hit the same tick, the standard bake
 * wins.
 * ============================================================== */
post_exec:
    /* Fading-out + touched-this-tick → zero foot_y so the entity
 * sinks below visible scenery before the fade pass culls it. */
    if (frame_changed && (EOFF(e, 8, uint16_t) & ENT_PFLAG_FADING_OUT)) {
        EOFF(e, ENT_OFF_FOOT_Y, uint16_t) = 0;
        EOFF(e, 8, uint16_t)             |= ENT_PFLAG_FOOT_BAKED;
    }
    EOFF(e, ENT_OFF_PC, uint16_t) = pc;

    /* Mirror anchor → drawn position (with optional oscillation). */
    {
        int16_t ax = (int16_t)EOFF(e, ENT_OFF_ANCHOR_X, uint16_t);
        int16_t ay = (int16_t)EOFF(e, ENT_OFF_ANCHOR_Y, uint16_t);
        EOFF(e, ENT_OFF_DRAWN_X, int16_t) = ax;
        EOFF(e, ENT_OFF_DRAWN_Y, int16_t) = ay;

        if (osc_table_x) {
            uint16_t idx = EOFF(e, ENT_OFF_LOOP_D, uint16_t);
            EOFF(e, ENT_OFF_DRAWN_X, int16_t) = (int16_t)(osc_table_x[idx] + ax);
        }
        if (osc_table_y) {
            uint16_t idx = EOFF(e, ENT_OFF_LOOP_E, uint16_t);
            EOFF(e, ENT_OFF_DRAWN_Y, int16_t) = (int16_t)(osc_table_y[idx] + ay);
        }
    }

    /* Stash the current frame's atlas width / height into the entity
 * so renderers + hit-tests can pick them up. Clamps the frame
 * index defensively — SWAP_ATLAS preserves +0x30 across the swap,
 * so a stale high index on a smaller new atlas could OOB. */
    {
        uint16_t fid = EOFF(e, ENT_OFF_FRAME, uint16_t);
        if (atlas->frame_count && fid >= atlas->frame_count) {
            fid = (uint16_t)(atlas->frame_count - 1);
        }
        if (atlas->off_widths)  EOFF(e, ENT_OFF_WIDTH,  uint16_t) = atlas->off_widths [fid];
        if (atlas->off_heights) EOFF(e, ENT_OFF_HEIGHT, uint16_t) = atlas->off_heights[fid];
    }

    /* Clamp scale to the engine's max. */
    if (EOFF(e, ENT_OFF_SCALE_PCT, uint16_t) > ENT_SCALE_MAX) {
        EOFF(e, ENT_OFF_SCALE_PCT, uint16_t) = ENT_SCALE_MAX;
    }

    /* Foot-anchor compensation: ANIM_ACTIVE entities want the drawn
 * position to reference their per-frame "hot point" so the foot
 * stays at the script's anchor while the body's silhouette cycles
 * through poses.
 *
 * perspective-scaled (ENT_PFLAG_PERSPECTIVE):
 * drawn += hot * scale_pct / 100
 * 2× doubled (ENT_PFLAG_DOUBLED):
 *
 * NOTE: the "perspective" path also fires whenever scale_pct is
 * actually being applied by the renderer — the original engine
 * gates this on ENT_PFLAG_PERSPECTIVE specifically, but actors
 * spawned via the path we've reproduced don't reliably get that
 * flag set. Treating any non-trivial scale_pct as perspective
 * keeps foot anchoring correct both for shrinking (climbing
 * distance) and growing (leftover scale from another actor's
 * action). */
    if (EOFF(e, ENT_OFF_STATE_FLAGS, uint16_t) & ENT_FLAG_ANIM_ACTIVE) {
        uint16_t fid   = EOFF(e, ENT_OFF_FRAME, uint16_t);
        uint16_t flags = EOFF(e, 8, uint16_t);
        if (atlas->off_drawX && atlas->off_drawY && fid < atlas->frame_count) {
            int16_t  hx      = (int16_t)atlas->off_drawX[fid];
            int16_t  hy      = (int16_t)atlas->off_drawY[fid];
            uint16_t scale58 = EOFF(e, ENT_OFF_SCALE_PCT, uint16_t);

            if (flags & ENT_PFLAG_PERSPECTIVE) {
                EOFF(e, ENT_OFF_DRAWN_X, int16_t) = (int16_t)(
                    EOFF(e, ENT_OFF_DRAWN_X, int16_t) + ((int32_t)hx * scale58) / 100);
                EOFF(e, ENT_OFF_DRAWN_Y, int16_t) = (int16_t)(
                    EOFF(e, ENT_OFF_DRAWN_Y, int16_t) + ((int32_t)hy * scale58) / 100);
            } else if (flags & ENT_PFLAG_DOUBLED) {
                EOFF(e, ENT_OFF_DRAWN_X, int16_t) =
                    (int16_t)(EOFF(e, ENT_OFF_DRAWN_X, int16_t) + hx * 2);
                EOFF(e, ENT_OFF_DRAWN_Y, int16_t) =
                    (int16_t)(EOFF(e, ENT_OFF_DRAWN_Y, int16_t) + hy * 2);
            } else {
                EOFF(e, ENT_OFF_DRAWN_X, int16_t) =
                    (int16_t)(EOFF(e, ENT_OFF_DRAWN_X, int16_t) + hx);
                EOFF(e, ENT_OFF_DRAWN_Y, int16_t) =
                    (int16_t)(EOFF(e, ENT_OFF_DRAWN_Y, int16_t) + hy);
            }
        }
    }

    /* Standard foot_y bake: ENT_PFLAG_NO_FOOT_BAKE entities (HUD,
 * speech balloons) opt out; everything else gets the conventional
 * "foot = drawn_y + height". This runs AFTER the top-of-post-exec
 * fade-out clear, so it wins when both fire on the same tick. */
    if (!(EOFF(e, 8, uint16_t) & ENT_PFLAG_NO_FOOT_BAKE)) {
        EOFF(e, ENT_OFF_FOOT_Y, int16_t) = (int16_t)(
            EOFF(e, ENT_OFF_HEIGHT, int16_t) + EOFF(e, ENT_OFF_DRAWN_Y, int16_t));
    }
}

/* ---- EntityWalkerTick ---------------------------------------------- *
 *
 * Once-per-frame driver. Walks the render list and executes one tick
 * of bytecode for every entity that has an atlas bound (an entity
 * without an atlas — most kind=4 click payloads, masks, etc. — has no
 * VM state, so the per-entity check skips them cheaply).
 *
 * `head` is accepted for API compatibility with the original engine's
 * call convention; the actual iteration uses the side-band entity
 * list so we don't rely on Entity's in-place next/prev pointers
 * (which would overflow on 64-bit hosts).
 *
 * Frame-delta globals (`g_frame_delta_ms`, `g_frame_delta_ticks`) are
 * refreshed by ProcessGameFrameTickInner before this runs, so we just
 * read them as-is. */
void EntityWalkerTick(Entity *head)
{
    (void)head;

    int n = EntityListCount(/*click_list=*/0);
    for (int i = 0; i < n; ++i) {
        Entity *e = EntityListAt(/*click_list=*/0, i);
        if (!e) continue;

        AnimAsset *a = (AnimAsset *)ent_ptr_resolve(EOFF(e, ENT_OFF_ATLAS_SLOT, uint32_t));
        if (a != NULL) {
            ExecEntityScript(e);
        }
    }
}
