/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/vm/main.c — Wacki bytecode VM and .scr loader.
 *
 * Bytecode format (instruction stream is a u16 array; pc is u16 *):
 *   word0   = [op:u8 at +0][len:u8 at +1]
 *   word1   = first u16 operand (a0) — in the same dword as the header
 *   word2.. = additional operand dwords (4 bytes each)
 * Total instruction size = len * 4 bytes (len is in DWORDS).
 *
 * The opcode table covers all 78 opcodes the engine emits. Where an
 * opcode calls into a subsystem we haven't ported (entity walker,
 * DirectSound mixer, palette fader), we trace via [script] log and
 * pc-advance so the VM keeps running deterministically.
 *
 * .scr file layout (text-tagged):
 *   [etap]N
 *   [komnata]M
 *   <binary bytecode block>
 *   ...
 * Other tags found in shipped scripts: [rozmowa], [animacja],
 * [sampl]. */
#include "wacki.h"
#include "wacki/log.h"
#include "opcodes.h"
#include "parser.h"

#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <SDL.h>

/* ---- Engine globals ---------------------------------------------- *
 *
 * Only those the VM reads or writes; the rest live in their owner
 * modules. */
uint32_t g_script_vars[0x129];              /* register file */
/* g_return_reg aliases g_script_vars[4] — the original engine put
 * the return register at +0x10 from g_script_vars (= index 4). Scripts
 * read the return value via op 0x04 IF_EQ a0=4, so the write target
 * MUST be var[4], not a separate variable. The macro keeps call sites
 * readable. */
#define g_return_reg (*(uint16_t *)&g_script_vars[4])
/* g_cursor_speed must be uint16_t — the original perspective formula
 * reads `(uint)g_cursor_speed` (zero-extend from 16 bits). An earlier
 * port declared it as uint8_t, which truncated op 0x40 invocations
 * with a0 > 255 (e.g. reg_id=0x0109=265 in the "wsiądź do auta" climb
 * sequence dropped to 0x09=9). With cs=9 the perspective z went
 * negative for nearly every Y, clamped to 0, and the renderer fell
 * through to natural-size blit — Ebek rendered HUGE during the climb
 * instead of shrinking with perspective. */
uint16_t g_cursor_speed     = 0x78;
uint16_t g_perspective_min  = 4;
uint16_t g_perspective_step = 7;
uint16_t g_active_actor     = 0;            /* 0 = Ebek, 1 = Fjej */
uint16_t g_anim1_done       = 0;
uint16_t g_anim2_done       = 0;
int      g_script_running   = 0;            /* !=0 inside VM */
int      g_script_reentry   = 0;            /* reentrancy counter */
uint32_t g_timer_baselines[16];

extern uint32_t g_tick_counter;             /* ms since boot */

/* Engine helpers used by various opcodes — kept as externs; many
 * resolve to stubs in stubs.c until the relevant subsystem is
 * ported. */
extern void  ProcessGameFrameTick(void);
extern void  PaletteFadeStep(int delta);
extern void  PrintTextOnScreen(uint16_t hx, uint16_t hy, const char *text);
extern uint32_t g_frame_delta_ms;           /* real wall-clock ms */
extern uint16_t g_frame_delta_ticks;        /* 10 ms ticks */

/* Stub-side script bridges (defined in stubs.c if missing). They take the
 * raw operand words and the active register id, and may pc-advance the
 * interpreter via side effects. All return void. */
extern void ScriptCallSoundPlay (uint16_t id, uint16_t a, uint32_t b, uint16_t c);
extern void ScriptCallSoundStop (void);
extern void ScriptCallPalLoad   (uint16_t fade_step, uint32_t selector, int with_fade);
extern int  ScriptCallPalFadeStep(void);   /* returns 1 when fade complete */
extern void ScriptCallBgMaskSetup(const char *name);     /* op 0x2C */
extern void ScriptCallRegMaskList(uint16_t id, uint32_t click_ptr, int verb_table);  /* op 0x2E/0x2F */
extern void ScriptCallLoadAsset (const char *name, uint16_t id);
extern void ScriptCallSpawnEntity(uint16_t id, uint16_t flags,
                                  uint32_t click_payload_addr,
                                  uint32_t script_addr);
extern void ScriptCallEnableEnt (uint16_t id, int enable);
extern void ScriptCallHideEnt   (uint16_t id);
extern void ScriptCallShowEnt   (uint16_t id);
extern int  actor_perspective_scale(Entity *a);   /* walker.c — depth ramp */
extern const void *xlat_binary_ptr(uint32_t addr);
extern int         PeLoaderContainsVA(uint32_t va);
extern const char *xlat_asset_name(uint32_t addr);
extern void ScriptCallWalkMode  (uint16_t id, int mode);
extern void ScriptCallWalkTo    (uint16_t id, uint16_t target, int mode);
extern void ScriptCallAttachProp(uint16_t actor, uint16_t prop, int keep);
extern void ScriptCallShowText  (uint16_t actor, const char *text);
extern int  ScriptCallDialogBegin(uint16_t actor, const char *a, const uint8_t *b, uint32_t c);
extern void ScriptCallDialogEnd  (const char *result);

/* Register-file accessors (var_get/var_set) and bytecode scanners
 * (skip_to_endif, find_label) live in src/vm/parser.{c,h} (included
 * above). Local aliases keep the inline call sites readable. */
#define var_get        vm_var_get
#define var_set        vm_var_set
#define skip_to_endif  vm_skip_to_endif
#define find_label     vm_find_label

/* ---- RunScriptInterpreter — 78 opcodes --------------------------- */
#define VM_CALL_STACK_DEPTH  10
#define VM_LOOP_SLOTS        10

/* Minimum perspective scale (0..0xA0 ramp) at which op 0x0E is allowed to
 * reveal a hidden actor for a scripted action (the klatka2 skarpeta climb,
 * bound at the rope base ~120%). Below this the actor is at/behind the
 * horizon and a hide+rebind means "vanish into depth" — it must stay
 * hidden, not pop back as a stuck, tiny sprite. See OP_SET_ENTITY_SCRIPT. */
#define ACTOR_REVEAL_MIN_SCALE  0x40

int RunScriptInterpreter(uint16_t this_id, uint16_t that_id,
                         uint8_t *bytecode)
{
    const uint16_t *pc = (const uint16_t *)bytecode;
    const uint16_t *call_pc_ret  [VM_CALL_STACK_DEPTH] = {0};
    const uint16_t *call_base_ret[VM_CALL_STACK_DEPTH] = {0};
    int             call_sp      = 0;
    uint16_t        loop_counters[VM_LOOP_SLOTS] = {0};
    int             result       = 1;
    const uint16_t *base         = pc;          /* current bytecode block */

    /* Dialog choice accumulator —'s stack-local
 * local_124 / auStack_122 / uVar13. Up to 6 entries, each:
 * { u16 speaker_id, u32 dialog_ptr, u32 dialog_data }
 * Populated by op 0x19 QUEUE_DIALOG, consumed by op 0x1A-0x1D
 * dialog-open variants. */
    uint16_t dlg_speaker[6] = {0};
    uint32_t dlg_ptr    [6] = {0};
    uint32_t dlg_data   [6] = {0};
    uint16_t dlg_count       = 0;

    g_script_running = 1;
    ++g_script_reentry;

    while (pc) {
        uint8_t op  = ((const uint8_t *)pc)[0];
        uint8_t len = ((const uint8_t *)pc)[1];
        /* Safety: opcodes outside 0x00..0x57 are invalid (we ran past the
 * end of a truncated embedded script, or hit a NULL-init region).
 * Terminate cleanly rather than crash on a bogus operand. Also
 * cap len at 32 dwords (128 bytes — biggest legitimate instruction
 * is opcode 0x52 at len=8). */
        if (op > 0x57 || len > 32) {
            LOG_TRACE("script", "bogus op=0x%02x len=%u at pc=%p — END", op, len, (void *)pc);
            pc = NULL;
            if (call_sp > 0) { --call_sp; base = call_base_ret[call_sp];
                               pc = call_pc_ret[call_sp]; }
            continue;
        }
        /* LOG_TRACE("vm", "op=0x%02x len=%u pc=%p", op, len, (const void*)pc); */
        uint16_t a0 = pc[1];                    /* first u16 operand (uVar31) */
        uint16_t a1 = (len >= 2) ? pc[2] : 0;   /* low u16 of operand dword 1 */
        uint16_t a2 = (len >= 2) ? pc[3] : 0;   /* high u16 of operand dword 1 */
        uint32_t i32_at4 = 0;
        if (len >= 2) memcpy(&i32_at4, ((const uint8_t *)pc) + 4, 4);
        int advanced = 0;

        /* reg_id = which register the opcode operates on.
 *
 * I.e. when the FIRST OPERAND of any instruction is 0x27 or 0x28,
 * the register index is re-mapped to that_id or this_id. This is
 * how scripts address the "current" or "calling" entity without
 * hard-coding its id.
 *
 * Earlier this port had `reg_id = a0` always and then set reg_id
 * inside the case 0x27/0x28 (dead code — `break` followed). That
 * left every var read/write keyed on raw a0 — so register-targeted
 * opcodes with operand 0x27/0x28 hit `g_script_vars[0x27..0x28]`
 * instead of the caller's this/that. Cluster of scripts that pivot
 * on the active actor (Ebek vs Fjej) were misrouted. */
        uint16_t reg_id;
        if      (a0 == 0x28) reg_id = this_id;
        else if (a0 == 0x27) reg_id = that_id;
        else                 reg_id = a0;

        switch (op) {
        /* ---- conditionals & control flow ----------------------------- */
        case OP_SKIP_TO_END: {                            /* SKIP_TO_END
 * (line 213): `if (uVar29 != uVar23)
 * skip-forward-past-endif`.
 *
 * `uVar29` is the RESOLVED reg_id (this/that-mapped via the
 * loop preamble at lines 218-223). `uVar23` is the caller's
 * `this_id` captured at function entry. The condition is
 * `resolved_reg_id != this_id`, NOT `raw_a0 != this_id`.
 *
 * Earlier port compared `a0` directly — when a0 == 0x28
 * (the "this" marker, most common case), `0x28 != this_id`
 * was true so the body was SKIPPED, opposite of intent. */
            if (reg_id != this_id) {
                pc = skip_to_endif(pc);
                /* Advance past the marker (6/7) we landed on — its own
 * `len` differs from the IF's, so the main loop's blanket
 * `pc += len*2` would mis-step. Re-read len from the new pc. */
                if (pc) {
                    uint8_t mlen = ((const uint8_t *)pc)[1];
                    pc = mlen ? (pc + mlen * 2) : NULL;
                }
                advanced = 1;
            }
            break;
        }
        case OP_IF_NE:                              /* IF_NE (skip if var==imm) */
        case OP_IF_GT:                              /* IF_GT (skip if var<=imm) */
        case OP_IF_LT:                              /* IF_LT (skip if var>=imm) */
        case OP_IF_EQ:                              /* IF_EQ (skip if var!=imm) */
        case OP_IF_ALL_BITS_SET: {                            /* IF_ALL_BITS_SET (skip if (var&imm)!=imm) */
            /* Each IF opcode has `if (CONDITION) { skip }` in the
             * original, so `take` = !CONDITION. An earlier port had
             * the senses inverted — every IF that should have taken
             * its body skipped it (and vice versa), which scrambled
             * which conditional sprites a scene's enter_script spawned
             * (e.g. klatka2 dropped babcia1b, kept skarpeta on a
             * fresh-game state). */
            uint32_t v   = var_get(reg_id);
            uint32_t imm = i32_at4;
            int take = 0;
            switch (op) {
            case OP_IF_NE: take = (v != imm); break;
            case OP_IF_GT: take = ((int32_t)v >  (int32_t)imm); break;
            case OP_IF_LT: take = ((int32_t)v <  (int32_t)imm); break;
            case OP_IF_EQ: take = (v == imm); break;
            case OP_IF_ALL_BITS_SET: take = ((v & imm) == imm); break;   /* unchanged — was correct */
            }
            if (!take) {
                pc = skip_to_endif(pc);
                if (pc) {
                    uint8_t mlen = ((const uint8_t *)pc)[1];
                    pc = mlen ? (pc + mlen * 2) : NULL;
                }
                advanced = 1;
            }
            break;
        }
        case OP_ELSE:                              /* ELSE (skip true-branch) */
            pc = skip_to_endif(pc);
            if (pc) {
                uint8_t mlen = ((const uint8_t *)pc)[1];
                pc = mlen ? (pc + mlen * 2) : NULL;
            }
            advanced = 1;
            break;

        case OP_JUMP_LABEL: {                            /* JUMP_LABEL
 * Ghidra case OP_JUMP_LABEL:
 * if (base == NULL) goto FALLTHROUGH;
 * walk base looking for op 0x16 with arg == a0;
 * if found: pc = found_address;
 * if not found: FALLTHROUGH (puVar16 is at 0x56 marker
 * but the check rejects it). */
            const uint16_t *t = find_label(base, a0);
            if (t) { pc = t; advanced = 1; }
            /* base==NULL or label not found → fall through to default
 * advance (pc += len*2). Earlier port set pc = NULL when
 * base was NULL — that ended the script instead of just
 * skipping this opcode. */
            break;
        }
        case OP_LOOP_COUNTED: {                            /* LOOP_COUNTED
 * Ghidra case OP_LOOP_COUNTED: counter slot = number of 0x18 opcodes
 * appearing BEFORE the current pc in this script. Up to 10
 * slots. Each increment compares against arg=a0 (limit); if
 * still under, jump to op 0x16 LABEL with matching index
 * (a2 = pc[2]); if reached, reset counter and fall through.
 *
 * Original walks param_3 → local_158 counting bytes 0x18,
 * we replicate exactly by walking the bytecode forward from
 * base to pc, counting opcode==0x18 bytes (each instruction
 * is `dlt * 2` bytes wide where dlt = bytecode[1]).
 *
 * Earlier port used `(pc-base) & 15` as slot index — was
 * not Now matches. */
            int slot = 0;
            if (base && base != pc) {
                const uint16_t *p = base;
                while (p && p < pc) {
                    uint8_t op_at  = ((const uint8_t *)p)[0];
                    uint8_t dlt_at = ((const uint8_t *)p)[1];
                    if (op_at == 0x18) ++slot;
                    if (dlt_at == 0) break;          /* malformed → bail */
                    p += dlt_at;                     /* uint16_t* +=
 * dlt = byte +=
 * dlt*2 */
                }
            }
            if (slot < 10) {
                ++loop_counters[slot];
                if (loop_counters[slot] < a0) {
                    /* Original reads `local_158[2]` (= a2, the operand
 * at +4) as the label index to jump to. */
                    const uint16_t *t = find_label(base, a2);
                    if (t) { pc = t; advanced = 1; }
                } else {
                    loop_counters[slot] = 0;
                }
            }
            break;
        }

        /* ---- variable arithmetic ------------------------------------- */
        case OP_VAR_OR: var_set(reg_id, var_get(reg_id) |  i32_at4); break;
        case OP_VAR_AND_NOT: var_set(reg_id, var_get(reg_id) & ~i32_at4); break;
        case OP_VAR_SET: var_set(reg_id, i32_at4); break;
        case OP_VAR_ADD: var_set(reg_id, var_get(reg_id) + i32_at4); break;
        case OP_VAR_SUB: var_set(reg_id, var_get(reg_id) - i32_at4); break;

        /* ---- query-and-store-in-return-register ---------------------- */
        case OP_RET_REG_DEFAULT: g_return_reg = (uint16_t)(((this_id - 0x29) < 0x8E) ? 2 : 1); break;
        case OP_RET_REG_THAT_ID: g_return_reg = that_id; break;
        case OP_INV_HAS_ITEM: {
            /* @ case OP_INV_HAS_ITEM:
 *
 * = InventoryHasItem(reg_id) → store result in var[4] = g_return_reg.
 * Used by scripts as a precondition: `if (has(item)) { ... }`. */
            g_return_reg = (uint16_t)InventoryHasItem(reg_id);
            break;
        }
        case OP_GET_CUR_ROOM: g_return_reg = g_cur_komnata; /*: returns g_cur_komnata (current komnata id). Was wrongly reading g_script_vars[0]. */ break;
        case OP_WACKI_RAND: g_return_reg = (uint16_t)WackiRand((uint16_t)a0); /* (bound=a0) */ break;
        case OP_GET_ACTIVE_ACTOR: g_return_reg = (uint16_t)(g_active_actor + 1); break;

        /* ---- wait / pacing ------------------------------------------- */
        case OP_FRAME_TICK: ProcessGameFrameTick(); EnginePaceFrame(33); break;  /* T-anim-speed */
        case OP_WAIT_MS: {                            /* WAIT — a0 in 10 ms TICKS.
             *
             * g_frame_delta_ticks is in 10 ms units (the original
             * runs a 10 ms timeSetEvent ISR), so the operand a0 is
             * ALSO ticks — `WAIT 100` waits 100*10 = 1000 ms = 1 s.
             * An earlier port treated a0 as raw ms which ran waits
             * 10× too fast. */
            uint32_t left = a0;
            while (left) {
                ProcessGameFrameTick();
                if (PlatformShouldQuit() || g_game_over_code) break;
                uint32_t step = g_frame_delta_ticks ? g_frame_delta_ticks : 3;
                left = (left > step) ? left - step : 0;
                EnginePaceFrame(33);
            }
            break;
        }
        case OP_WAIT_ENTITY_IDLE: {                            /* WAIT_ENTITY
 * Ghidra case OP_WAIT_ENTITY_IDLE:
 * ProcessGameFrameTick;
 * Entity fields +0x4C / +0x50 = walk_dx_remaining / walk_dy_remaining
 * (set by in WALK_TO op, decremented per step). */
            extern Entity *FindEntityByVerbId(uint16_t verb_id);
            Entity *e = FindEntityByVerbId(reg_id);
            if (e) {
                uint8_t *eb = (uint8_t *)e;
                int safety = 2000;
                while (safety-- > 0 &&
                       (*(uint32_t *)(eb + 0x4C) || *(uint32_t *)(eb + 0x50))) {
                    ProcessGameFrameTick();
                    if (PlatformShouldQuit() || g_game_over_code) break;
                    EnginePaceFrame(33);
                }
                if (safety <= 0) {
                    LOG_INFO("vm-wait", "OP_WAIT_ENTITY_IDLE id=0x%04X "
                                        "WATCHDOG — walk_dx=%u walk_dy=%u",
                             reg_id,
                             *(uint32_t *)(eb + 0x4C),
                             *(uint32_t *)(eb + 0x50));
                }
            }
            break;
        }
        case OP_WAIT_ANIM_FRAME: {                            /* WAIT_ANIM_FRAME —
 * with Ghidra case OP_WAIT_ANIM_FRAME:
 * if (e) while (e[+0x30] != target) ProcessGameFrameTick;
 *
 * Block script until entity's frame index reaches the target. */
            extern Entity *FindEntityByVerbId(uint16_t verb_id);
            Entity *e = FindEntityByVerbId(reg_id);
            uint16_t target = (len >= 2) ? pc[2] : 0;
            if (e) {
                uint8_t *eb = (uint8_t *)e;
                int safety = 2000;
                while (safety-- > 0 &&
                       *(uint16_t *)(eb + 0x30) != target) {
                    ProcessGameFrameTick();
                    if (PlatformShouldQuit() || g_game_over_code) break;
                    EnginePaceFrame(33);
                }
                if (safety <= 0) {
                    LOG_INFO("vm-wait", "OP_WAIT_ANIM_FRAME id=0x%04X "
                                        "WATCHDOG — target=%u got=%u",
                             reg_id, (unsigned)target,
                             (unsigned)*(uint16_t *)(eb + 0x30));
                }
            }
            break;
        }
        case OP_WAIT_KIND2_FRAME: {                            /* WAIT_KIND2_FRAME
 * Ghidra case OP_WAIT_KIND2_FRAME:
 *
 * Blocks the calling script until the kind=2 entity bound to
 * `reg_id` has its current frame index equal to `a1`. */
            extern void *FindUpdateRegistration(uint16_t kind, uint16_t id);
            void *e = FindUpdateRegistration(2, reg_id);
            if (e) {
                uint16_t target = a1;
                uint8_t *eb = (uint8_t *)e;
                int safety = 2000;
                while (safety-- > 0 && *(uint16_t *)(eb + 0x30) != target) {
                    ProcessGameFrameTick();
                    if (PlatformShouldQuit() || g_game_over_code) break;
                    EnginePaceFrame(33);
                }
                if (safety <= 0) {
                    LOG_INFO("vm-wait", "OP_WAIT_KIND2_FRAME id=0x%04X "
                                        "WATCHDOG — target=%u got=%u",
                             reg_id, (unsigned)target,
                             (unsigned)*(uint16_t *)(eb + 0x30));
                }
            }
            break;
        }

        /* ---- actor walk-to + blocking wait —
 * 0x10 (actor 0), 0x11 (actor 1), 0x12 (both, with 50-tick
 * head-start). Original case OP_WALK_EBEK:
 *
 * actor.+0x24 != local_158[2]) { // = a1 (Y)
 * ...
 * // wait loop on actor walker fields
 *
 * Confirmed via concrete bytecode: verb 7 (exit-left) encodes
 * `12 02 1E 00 72 01 00 00` = op 0x12 walking the actors to
 * (30, 370) — the left-exit target with Y on the walkable floor.
 * Target reads as (a0=30, a1=370); a2=0 is the "wait mode" flag
 * for op 0x12.
 *
 * An earlier fix attempt shifted the operand read to (a1, a2)
 * thinking it was off by one halfword — wrong direction; the
 * shipped fix reverted to (a0, a1).
 *
 * Bytecode layout: `[op:1][len:1] [X:2] [Y:2] [mode:2]`
 * len=2 means 8 bytes total (len*4). For op 0x10/0x11 a2 is
 * unused; for op 0x12 a2 = wait-mode flag (0 = return when EITHER
 * actor reaches the target — the go-to-exit case; else wait for both). */
        case OP_WALK_EBEK:                              /* walk Ebek */
            ActorWalkToBlocking(0, (int16_t)a0, (int16_t)a1);
            break;
        case OP_WALK_FJEJ:                              /* walk Fjej */
            ActorWalkToBlocking(1, (int16_t)a0, (int16_t)a1);
            break;
        case OP_WALK_BOTH: {                            /* walk both, 50-tick init
 * case 0x12 (Ghidra @ line 582):
 * setup actor 0 walker (target = a0, a1)
 * wait 0x32 ticks
 * setup actor 1 walker (same target)
 * if (a2 == 0) return when EITHER actor arrives; else wait for BOTH
 *
 * a2 = wait-mode flag (0 = wait either / go-to-exit, 1 = wait both). */
            extern void ActorWalkBothBlocking(int16_t, int16_t, int);
            ActorWalkBothBlocking((int16_t)a0, (int16_t)a1, (int)a2);
            break;
        }
        case OP_RESET_ACTORS: {                            /* RESET_BOTH_ACTORS —
 *:
 * (g_actor[0]); // reset state
 * g_actor[0].+0x2C = *(int*)(stage+0xC) + 0x10; // idle bytecode
 * (g_actor[1]);
 * = -1;
 * g_actor[1].+0x2C = *(int*)(stage+0x10) + 0x10;
 * The +0xC / +0x10 offsets into the stage struct point at
 * per-actor animation tables; offset +0x10 inside that table
 * is the "idle" bytecode entry. */
            extern Entity *g_actor[2];
            for (int ai = 0; ai < 2; ++ai) {
                Entity *e = g_actor[ai];
                if (!e) continue;
                uint8_t *eb = (uint8_t *)e;
                /* reset */
                *(uint16_t *)(eb + 0x3A) &= (uint16_t)~5u;
                *(uint16_t *)(eb + 0x38) = 0;
                *(uint16_t *)(eb + 0x36) = 0;
                *(uint16_t *)(eb + 0x34) = 0;
                *(uint16_t *)(eb + 0x3C) = 0;
                *(uint16_t *)(eb + 0x42) = 0;
                *(uint16_t *)(eb + 0x40) = 0;
                *(uint16_t *)(eb + 0x32) = 0;
                *(uint32_t *)(eb + 0x50) = 0;
                *(uint32_t *)(eb + 0x4C) = 0;
                /* Bytecode reset — read from PE stage descriptor. */
                if (g_stage_va) {
                    extern const void *PeLoaderRead(uint32_t va);
                    const uint8_t *sd = (const uint8_t *)PeLoaderRead(g_stage_va);
                    if (sd) {
                        uint32_t anim_tab_va =
                            *(const uint32_t *)(sd + (ai ? 0x10 : 0x0C));
                        const uint8_t *anim_tab =
                            (const uint8_t *)PeLoaderRead(anim_tab_va);
                        if (anim_tab) {
                            uint32_t bc_va = *(const uint32_t *)(anim_tab + 0x10);
                            const void *bc = xlat_binary_ptr(bc_va);
                            extern uint32_t ent_ptr_intern(void *p);
                            *(uint32_t *)(eb + 0x2C) =
                                bc ? ent_ptr_intern((void *)bc) : 0;
                        }
                    }
                }
            }
            break;
        }

        /* ---- text / dialogue ----------------------------------------- */
        case OP_SHOW_TEXT: {                            /* SHOW_TEXT
 * Ghidra case 0x09 (lines 352-496): allocates a text-balloon
 * entity, renders multi-line wrapped text into it, positions
 * relative to actor's foot, waits for click/timeout.
 *
 * Operand layout per Ghidra: `*(char **)(local_158 + 2)` reads
 * a DWORD at byte +4 of the instruction → a binary pointer
 * into .data where the string lives.
 *
 * prefix (lines 353-371 of ): search
 * dlg_speaker[] for an entry matching reg_id. If found at
 * index N (N < dlg_count), bind dlg_ptr[N] as a per-entity
 * script on the speaker entity (= ). This makes
 * the speaker animate (e.g. mouth move) while text shows.
 *
 * Without this, characters appear static during dialog
 * lines. With it, the bound per-entity script drives their
 * animation. */
            {
                int found_idx = -1;
                if (dlg_count > 0 && dlg_speaker[0] == reg_id) {
                    found_idx = 0;
                } else {
                    for (uint16_t i = 1; i < dlg_count; ++i) {
                        if (dlg_speaker[i] == reg_id) { found_idx = i; break; }
                    }
                }
                /* Reset unbind state for THIS op invocation — clear in
 * case the previous balloon's unbind was never applied
 * (e.g. stage switched mid-speech). */
                extern uint16_t g_speech_unbind_speaker;
                extern uint32_t g_speech_unbind_data;
                g_speech_unbind_speaker = 0;
                g_speech_unbind_data    = 0;
                if (found_idx >= 0) {
                    /* Find the entity by verb_id, reset its state,
                     * bind bytecode at +0x2C, set frame = 0. */
                    extern Entity *FindEntityByVerbId(uint16_t v);
                    Entity *sp = FindEntityByVerbId(dlg_speaker[found_idx]);
                    if (sp) {
                        const void *new_bc = xlat_binary_ptr(dlg_ptr[found_idx]);
                        if (new_bc) {
                            uint8_t *eb = (uint8_t *)sp;
                            *(uint16_t *)(eb + 0x3A) &= (uint16_t)~5u;
                            *(uint16_t *)(eb + 0x38) = 0;
                            *(uint16_t *)(eb + 0x36) = 0;
                            *(uint16_t *)(eb + 0x34) = 0;
                            *(uint16_t *)(eb + 0x3C) = 0;
                            *(uint16_t *)(eb + 0x42) = 0;
                            *(uint16_t *)(eb + 0x40) = 0;
                            *(uint16_t *)(eb + 0x32) = 0;
                            *(uint32_t *)(eb + 0x50) = 0;
                            *(uint32_t *)(eb + 0x4C) = 0;
                            extern uint32_t ent_ptr_intern(void *p);
                            *(uint32_t *)(eb + 0x2C) =
                                ent_ptr_intern((void *)new_bc);
                            *(uint16_t *)(eb + 0x30) = 0;
                        }
                        /* Stash the dlg_data ptr for TickSpeechBalloon
 * to re-bind when this balloon dismisses (
 * with original case 9 epilogue, line ~480). */
                        g_speech_unbind_speaker = dlg_speaker[found_idx];
                        g_speech_unbind_data    = dlg_data[found_idx];
                    }
                }
            }
            const char *text = NULL;
            if (PeLoaderContainsVA(i32_at4))
                text = (const char *)xlat_binary_ptr(i32_at4);
            ScriptCallShowText(reg_id, text);
            /* Block until balloon dismisses — case 9
 * wait loop (Ghidra @ lines ~462-475):
 * ProcessGameFrameTick;
 *
 * = pump frames until: dismiss timer hits 0,
 * OR user click (g_panel_cursor_redirect2 != 0).
 *
 * TickSpeechBalloon (called from ProcessGameFrameTick) drives
 * the dismiss timer and clears g_speech_balloon when expired.
 * We block here until that happens. Earlier port returned
 * immediately — script ran past op 0x09 while balloon was
 * still visible, breaking dialog sequencing. */
            {
                extern Entity *g_speech_balloon;
                extern uint8_t g_lmb_clicked;
                /* Safety cap (~10s real) so a buggy dismiss timer can't
 * hang the game forever. */
                int safety_ms = 10000;
                while (g_speech_balloon && safety_ms > 0) {
                    ProcessGameFrameTick();
                    if (g_lmb_clicked) {
                        g_lmb_clicked = 0;                  /* consume */
                        /* Force-dismiss: simulate g_panel_cursor_redirect2 click by
 * fast-forwarding the timer past the dismiss
 * threshold. TickSpeechBalloon will GC the
 * balloon on the next tick. */
                        extern uint16_t g_speech_dismiss_ticks;
                        g_speech_dismiss_ticks = 0;
                        ProcessGameFrameTick();
                        break;
                    }
                    safety_ms -= (int)g_frame_delta_ms;
                }
            }
            break;
        }
        case OP_SET_ACTOR_FLAG2: {                            /* SET_ACTOR_FLAG2 + frame override —
 * (lines 501-507): lookup kind=2
 * entity, set flags2 bit 1 (= "frame override active") AND
 * write local_158[2] into +0x26 (the override frame index).
 * Used by trigger frames (Edek blinking, etc.). */
            extern void *FindUpdateRegistration(uint16_t kind, uint16_t id);
            Entity *e = (Entity *)FindUpdateRegistration(2, reg_id);
            if (e) {
                uint8_t *eb = (uint8_t *)e;
                eb[9] |= 2;                                       /* flags2 |= 2 */
                if (len >= 2) *(uint16_t *)(eb + 0x26) = pc[2];   /* override frame */
            }
            break;
        }
        case OP_SET_ENTITY_SCRIPT: {                            /* SET_ENTITY_SCRIPT
             *
             * Used by the actor-bind ELSE branch to point Ebek's and
             * Fjej's per-entity VMs at their idle scripts. An earlier
             * mis-port treated this as "destroy dialog balloon" and
             * wiped both actors right after SpawnActorEntity. */
            extern Entity *FindEntityByVerbId(uint16_t verb_id);
            Entity *e = FindEntityByVerbId(reg_id);
            if (e) {
                uint8_t *eb = (uint8_t *)e;
                /* reset (same block as op 0x33) */
                *(uint16_t *)(eb + 0x3A) &= (uint16_t)~5u;
                *(uint16_t *)(eb + 0x38) = 0;
                *(uint16_t *)(eb + 0x36) = 0;
                *(uint16_t *)(eb + 0x34) = 0;
                *(uint16_t *)(eb + 0x3C) = 0;
                *(uint16_t *)(eb + 0x42) = 0;
                *(uint16_t *)(eb + 0x40) = 0;
                *(uint16_t *)(eb + 0x32) = 0;
                *(uint32_t *)(eb + 0x50) = 0;
                *(uint32_t *)(eb + 0x4C) = 0;
                /* Bind new script + reset frame. */
                const void *new_bc = xlat_binary_ptr(i32_at4);
                extern uint32_t ent_ptr_intern(void *p);
                *(uint32_t *)(eb + 0x2C) = new_bc ? ent_ptr_intern((void *)new_bc) : 0;
                *(uint16_t *)(eb + 0x30) = 0;

                /* Binding a fresh visible anim to a HIDDEN actor in the
                 * FOREGROUND un-hides it. klatka2 skarpeta climb: the verb
                 * script hides Fjej (op 0x3E) to drop the approach pose,
                 * then binds the climb anim here. Fjej runs the first climb
                 * segment itself (frames 60-71, anchor rising); without
                 * this it plays invisibly for the ~500 ms lead-in until the
                 * climb-overlay sprite takes over. During the overlay phase
                 * the script parks the actor off-screen (x≈1000 → culled),
                 * so this can't double-render the climb.
                 *
                 * Two gates keep this from misfiring — the original op 0x0E
                 * (FUN_00401210) does NOT touch hidden at all; this whole
                 * branch is a port-local compensation, so it must be narrow:
                 *
                 *  - was_hidden: op 0x0E is also the routine scene-setup op
                 *    that points each actor's per-entity VM at its idle
                 *    script, which runs on a VISIBLE actor every komnata.
                 *    The original only freezes scale by hiding, so an idle
                 *    rebind on a visible actor must leave its scale free or
                 *    it stops shrinking as it walks into depth.
                 *
                 *  - foreground: op 0x3E + op 0x0E is ALSO how a scene sends
                 *    an actor BEHIND THE HORIZON (hide, then rebind while
                 *    deep). There the actor's perspective scale is near zero
                 *    and it must STAY hidden — revealing it leaves a stuck,
                 *    tiny sprite that should have vanished into the distance.
                 *    The climb is bound at the rope base (~120%), well above
                 *    the threshold; a deep approach is far below it. */
                extern Entity *g_actor[2];
                int was_hidden = (*(uint16_t *)(eb + 8) & 0x0080u) != 0;
                if (was_hidden && (e == g_actor[0] || e == g_actor[1]) &&
                    actor_perspective_scale(e) >= ACTOR_REVEAL_MIN_SCALE) {
                    int aidx = (e == g_actor[1]) ? 1 : 0;
                    *(uint16_t *)(eb + 8) &= (uint16_t)~0x0080u;   /* clear EFLAG_HIDDEN */
                    /* Hold the perspective scale for the action: the climb
                     * anim drives the anchor UP the rope, which would
                     * otherwise shrink the actor (perspective reads rising
                     * Y as "deeper"). The original keeps a constant,
                     * natural-size climb (the actor is hidden, its scale
                     * frozen); freezing here matches it. Cleared when the
                     * actor next walks (BindActorWalker). */
                    extern int g_actor_scale_frozen[2];
                    g_actor_scale_frozen[aidx] = 1;
                }
            }
            break;
        }
        case OP_SET_ENTITY_ANIM: {                            /* SET_ENTITY_ANIM —
 * (lines 522-530):
 *
 * (full reset) clears +0x32, +0x34, +0x36, +0x38,
 * +0x3A&~5, +0x3C, +0x40, +0x42, +0x4C, +0x50. Earlier port
 * missed +0x40 (osc counter A), +0x42 (osc B), +0x4C (walker
 * dx_remaining), +0x50 (walker dy_remaining) — residual walker
 * state from the previous anim could trigger spurious motion
 * after rebind. */
            extern void *FindUpdateRegistration(uint16_t kind, uint16_t id);
            Entity *e = (Entity *)FindUpdateRegistration(2, reg_id);
            if (e) {
                uint8_t *eb = (uint8_t *)e;
                /* Full reset (matches op 0x0E, 0x33 reset blocks). */
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
                *(uint16_t *)(eb + 0x30) = 0;             /* frame = 0 */
                /* Bind new script (+0x2c). The operand is a binary
 * pointer to bytecode; resolve via xlat or leave as
 * slot=0 if not embedded. */
                const void *new_bc = xlat_binary_ptr(i32_at4);
                extern uint32_t ent_ptr_intern(void *p);
                *(uint32_t *)(eb + 0x2C) = new_bc ? ent_ptr_intern((void *)new_bc) : 0;
            }
            break;
        }
        case OP_QUEUE_DIALOG: {                            /* QUEUE_DIALOG
 * (line 745):
 *
 * Note the ALWAYS-increment behaviour: even if scan found an
 * existing slot, uVar13 still grows. The slot index becomes
 * a high-water mark (not a count of unique entries). Earlier
 * port only incremented on append — divergence. */
            if (dlg_count < 6) {
                int slot = dlg_count;
                for (int i = 0; i < dlg_count; ++i) {
                    if (dlg_speaker[i] == reg_id) { slot = i; break; }
                }
                uint32_t ptr_v = i32_at4;
                uint32_t data_v = 0;
                if (len >= 3) memcpy(&data_v, ((const uint8_t *)pc) + 8, 4);
                dlg_speaker[slot] = reg_id;
                dlg_ptr    [slot] = ptr_v;
                dlg_data   [slot] = data_v;
                ++dlg_count;                /* always grow the high-water */
                /* T20b debug — trace the dialog choice queue. The
                 * verb_id here is what op 0x1A will later add to the
                 * panel inventory slot; ptr/data reference Gadki.scr
                 * sections for the response audio. */
                LOG_TRACE("dlg", "op 0x19 QUEUE slot=%d verb=0x%04X "
                                "ptr=0x%08X data=0x%08X (count→%d)", slot, reg_id, ptr_v, data_v, dlg_count);
            }
            break;
        }
        /* ---- dialog page-swap ops ----
         *
         * All five ops drive the same panel verb-table rotation
         * (PanelPageSwap), gated on different pre-conditions:
 *
 * case OP_DIALOG_SHOW: (dlg_count) + InventorySetPageForItem(reg)
 * + PanelPageSwap
 * The dialog accumulator (built by op 0x19) gets committed
 * into the inventory slots starting at slot 0; page slid
 * to where the speaker's verb landed; then panel refreshed.
 * Effectively "open dialog menu". here takes
 * the accumulator count, not an item_verb — it copies the
 * 6-slot stack-local into the inventory. We approximate by
 * writing the accumulated speakers + dlg_ptr values into
 * inventory[0..n-1] and zeroing the rest.
 *
 * case OP_DIALOG_CLEAR: (reg) + + PanelPageSwap
 * "Close dialog" — original removes the choice marker
 *, collapses to first non-empty page
 * ( = InventoryPageCollapse), refreshes panel.
 *
 * case OP_DIALOG_CHOICE: if (InventoryPageNext) PanelPageSwap
 * Page-down (panel right arrow).
 *
 * case OP_DIALOG_CLOSE: if (InventoryPagePrev) PanelPageSwap
 * Page-up (panel left arrow).
 *
 * case OP_DIALOG_TBD (0x1F): hide/deactivate the named entity — find
 * it by verb id, clear its bytecode slot and move it off-screen
 * (X=1000). Used to make an item's world sprite vanish (e.g. on
 * pickup). Fully implemented by InventoryDropItem (legacy name) —
 * see hud/inventory.c.
 *
 * Without a full dialog UI, the 0x1A..0x1D panel ops still move data
 * correctly; the panel verbs change but won't visually render dialog
 * text until the panel-label render path is ported (see D2 in REVIEW). */
        case OP_DIALOG_SHOW: {
            /* (line 765):
 * ((uint)puVar15); // InventoryAddItem(reg_id)
 * (uVar29); // InventorySetPageForItem(reg_id)
 *; // PanelPageSwap
 *
 * BOTH calls use the resolved reg_id (`puVar15` and `uVar29`
 * are the same value — `uVar29 = (ushort)puVar15`). Earlier
 * port read the first arg as dlg_count by mistake. The arg
 * is a verb_id of the speaker / dialogue marker. Op 0x1A is
 * the "open dialog menu" verb: it stashes the speaker's
 * marker into inventory, sets the inventory page to that
 * speaker's slot, then refreshes the panel verb table. */
            (void)InventoryAddItem(reg_id);
            InventorySetPageForItem(reg_id);
            PanelPageSwap();
            /* T20b debug — trace choice commit so we can see whether the
 * verb is actually landing on the panel. The verb must be in
 * range [0x29..0xB7] for przedm.wyc to have a frame for it;
 * otherwise PaintHudOverlay skips paint. */
            {
                extern uint16_t g_panel_verb_tab[6];
                LOG_TRACE("dlg", "op 0x1A COMMIT verb=0x%04X (dlg_count=%d) "
                                "panel_tab=[%04X %04X %04X %04X %04X %04X]", reg_id, dlg_count, g_panel_verb_tab[0], g_panel_verb_tab[1], g_panel_verb_tab[2], g_panel_verb_tab[3], g_panel_verb_tab[4], g_panel_verb_tab[5]);
            }
            break;
        }
        case OP_DIALOG_CLEAR: {
            /*
 * (reg_id);
 *; // InventoryPageCollapse
 *; // PanelPageSwap
 */
            (void)InventoryRemoveItem(reg_id);
            InventoryPageCollapse();
            PanelPageSwap();
            LOG_TRACE("dlg", "op 0x1B CLOSE verb=0x%04X", reg_id);
            break;
        }
        case OP_DIALOG_CHOICE: {
            /*
 */
            if (InventoryPageNext()) PanelPageSwap();
            break;
        }
        case OP_DIALOG_CLOSE: {
            /*
 */
            if (InventoryPagePrev()) PanelPageSwap();
            break;
        }
        case OP_DIALOG_TBD:   /* 0x1F — hide/deactivate entity by id */
            InventoryDropItem(reg_id);
            break;

        /* ---- exits / scene transitions ------------------------------- */
        case OP_GO_EXIT: {                            /* GO_EXIT
 * Ghidra case OP_GO_EXIT:
 * if (uVar31 == 0x26) target = g_cur_komnata; // 0x26 = self
 * else target = uVar31;
 * (target);
 *
 * 0x26 special-case = "reload current komnata" (used by
 * scripts that want a state refresh without changing room). */
            uint16_t target = (a0 == 0x26) ? g_cur_komnata : a0;
            ScriptGoToKomnata(target);
            break;
        }
        case OP_SECONDARY_SCRIPT: {
            /*:
 * Earlier port read raw `a0` — when scripts use 0x28 (this)
 * or 0x27 (that) as operand, port tested 0x28/0x27 against
 * 0x29 range (both < 0x29 so unsigned underflow → fails)
 * and never wrote held_item. With reg_id the resolved
 * verb_id (e.g. 1 = Ebek, 2 = Fjej) gets compared properly. */
            if (((reg_id - 0x29) < 0x8E) || reg_id == 0x26)
                g_held_item = reg_id;
            break;
        }
        case OP_BG_MASK_SETUP: {                            /* BG MASK SETUP
 * Ghidra case 0x2c steps 1-7:
 * destroys old id=0 entity,
 * loads .fld/.msk by name,
 * registers as kind=1 id=0,
 * links kind=3 mask entity.
 *
 * NOTE (steps 8-9):
 * Original ALSO calls
 * (0x44E750) to
 * populate exit-reachability
 * graph + memcpy's 3502 B
 * snapshot 0x44E750→0x44F4F0.
 * Port skips — no consumer
 * (uses .fld walkability +
 * hotspots). Zob. TASKS old T9. */
            const char *name = NULL;
            if (PeLoaderContainsVA(i32_at4))
                name = xlat_asset_name(i32_at4);
            else name = (const char *)(pc + 2);
            ScriptCallBgMaskSetup(name);
            break;
        }
        case OP_LOAD_ASSET: {                            /* LOAD_ASSET */
            const char *name = NULL;
            if (PeLoaderContainsVA(i32_at4))
                name = xlat_asset_name(i32_at4);
            else name = (const char *)(pc + 2);
            ScriptCallLoadAsset(name, reg_id);
            break;
        }
        case OP_REG_MASK_LIST: {                            /* REGISTER_MASK_E6D8 — per
 * Ghidra (id, ptr,
 * &mask_list_head); registers
 * per-frame click hotspots
 * into the "mask" verb table.
 * Asset must already be
 * loaded via op 0x2D. */
            uint32_t click_addr = i32_at4;
            ScriptCallRegMaskList(reg_id, click_addr, 0);
            break;
        }
        case OP_REG_VERB_MASK: {                            /* REGISTER_MASK_E700 — same
 * as 0x2E but target table is
 * &verb_mask_list_head (the verb-list
 * mask table). */
            uint32_t click_addr = i32_at4;
            ScriptCallRegMaskList(reg_id, click_addr, 1);
            break;
        }
        case OP_SPAWN_ENTITY: {                            /* SPAWN_ENTITY (16 bytes) */
            /* operand layout per Ghidra dump:
 */
            uint16_t flags2 = (len >= 4) ? pc[6] : 0;
            uint32_t click_addr = 0, script_addr = 0;
            if (len >= 3) memcpy(&click_addr,  ((const uint8_t *)pc) + 4, 4);
            if (len >= 4) memcpy(&script_addr, ((const uint8_t *)pc) + 8, 4);
            ScriptCallSpawnEntity(reg_id, flags2, click_addr, script_addr);
            break;
        }
        /*/0x32:
 * case OP_DESTROY_ENT_A: (reg_id, '\x01'); // destroy + unreg asset
 * case OP_DESTROY_ENT_B: (reg_id, '\x00'); // destroy only
 * Op naming `EnableEnt` was legacy misleading — both DESTROY; the
 * `1` arg only controls whether the kind=1 asset slot is also
 * cleared. Use the proper-named call. */
        case OP_DESTROY_ENT_A: { extern void ScriptCallDestroyEnt(uint16_t, int);
                     ScriptCallDestroyEnt(reg_id, 1); break; }
        case OP_DESTROY_ENT_B: { extern void ScriptCallDestroyEnt(uint16_t, int);
                     ScriptCallDestroyEnt(reg_id, 0); break; }

        /* ---- walk / actor positioning -------------------------------- */
        case OP_WALK_MODE_A: ScriptCallWalkMode(reg_id, 1); break;
        case OP_WALK_MODE_B: ScriptCallWalkMode(reg_id, 2); break;
        case OP_WALK_TO_BY_ID: ScriptCallWalkTo(reg_id, a1, 1); break;
        case OP_WALK_TO_SELF: ScriptCallWalkTo(reg_id, reg_id, 2); break;
        case OP_ATTACH_PROP_A: ScriptCallAttachProp(reg_id, a1, 0); break;
        case OP_ATTACH_PROP_B: ScriptCallAttachProp(reg_id, a1, 1); break;
        case OP_HIDE_ENTITY: ScriptCallHideEnt(reg_id); break;
        case OP_SHOW_ENTITY: ScriptCallShowEnt(reg_id); break;
        case OP_MOVE_ENTITY_DELTA: {                            /* MOVE_ENTITY_DELTA —
 * (lines 1108-1119): adds (dx,dy)
 * to the entity's draw AND raw positions.
 *
 * Used for scripted nudges (an NPC stepping aside, prop
 * tipping over). Earlier port used FindUpdateRegistration(2,
 * reg_id) — wrong lookup table. The original walks the click
 * list to resolve verb_id → render entity. */
            extern Entity *FindEntityByVerbId(uint16_t verb_id);
            Entity *e = FindEntityByVerbId(reg_id);
            if (e) {
                int16_t dx = (int16_t)((len >= 2) ? pc[2] : 0);
                int16_t dy = (int16_t)((len >= 2) ? pc[3] : 0);
                uint8_t *eb = (uint8_t *)e;
                *(int16_t *)(eb + 0x0A) = (int16_t)(*(int16_t *)(eb + 0x0A) + dx);
                *(int16_t *)(eb + 0x22) = (int16_t)(*(int16_t *)(eb + 0x22) + dx);
                *(int16_t *)(eb + 0x0C) = (int16_t)(*(int16_t *)(eb + 0x0C) + dy);
                *(int16_t *)(eb + 0x24) = (int16_t)(*(int16_t *)(eb + 0x24) + dy);
            }
            break;
        }
        case OP_QUERY_ENTITY_X: {                            /* QUERY_ENTITY_X
 *:
 *
 * Tests +0x28 (atlas), NOT +0x2C (bytecode). Earlier port read
 * +0x2C — would pick anchor for any scripted entity even if it
 * had no atlas, and pick drawn for atlas-only static entities. */
            extern Entity *FindEntityByVerbId(uint16_t verb);
            Entity *e = FindEntityByVerbId(reg_id);
            if (e) {
                uint8_t *eb = (uint8_t *)e;
                uint16_t flags3a = *(uint16_t *)(eb + 0x3A);
                if ((flags3a & 2) && *(uint32_t *)(eb + 0x28))
                    g_return_reg = *(uint16_t *)(eb + 0x22);
                else
                    g_return_reg = *(uint16_t *)(eb + 0x0A);
            } else {
                g_return_reg = 0;
            }
            break;
        }
        case OP_QUERY_ENTITY_Y: {                            /* QUERY_ENTITY_Y — mirror of 0x4B.
 * Same +0x28 atlas test, returns +0x24 (anchor Y) or +0x0C
 * (drawn Y). */
            extern Entity *FindEntityByVerbId(uint16_t verb);
            Entity *e = FindEntityByVerbId(reg_id);
            if (e) {
                uint8_t *eb = (uint8_t *)e;
                uint16_t flags3a = *(uint16_t *)(eb + 0x3A);
                if ((flags3a & 2) && *(uint32_t *)(eb + 0x28))
                    g_return_reg = *(uint16_t *)(eb + 0x24);
                else
                    g_return_reg = *(uint16_t *)(eb + 0x0C);
            } else {
                g_return_reg = 0;
            }
            break;
        }
        /* 0x47 / 0x4B / 0x4C implemented above — share FindEntityByVerbId
 * which walks the click list. Earlier port used
 * FindUpdateRegistration(2, reg_id) — wrong lookup. */
        case OP_SET_TAGGED_FIELD: {                            /* SET_TAGGED_FIELD — 
 * with Ghidra case OP_SET_TAGGED_FIELD:
 *
 * Used by exit verb scripts (e.g. verb 7 mal_l) to plant the
 * actor's walk-out destination INSIDE the actor's per-entity
 * bytecode block. The bytecode then drives the walk via the
 * usual per-entity VM ops 0x15/0x16. */
            extern uint16_t FindKeyInTaggedTable(const char *t, char tag, int16_t k);
            uint8_t *blk = (uint8_t *)xlat_binary_ptr(i32_at4);
            if (blk) {
                uint16_t idx = FindKeyInTaggedTable((const char *)blk, '\x15', -1);
                if (idx != 0) {
                    uint16_t v1 = (len >= 3) ? pc[4] : 0;
                    uint16_t v2 = (len >= 3) ? pc[5] : 0;
                    /* The original writes BACK into PE-loaded data — our
 * PeLoaderRead returns the in-memory image, so this
 * mutates the same store (good — matches original
 * intent of "patch the actor's bytecode in place"). */
                    *(uint16_t *)(blk + 2 + (size_t)idx * 2) = v1;
                    *(uint16_t *)(blk + 4 + (size_t)idx * 2) = v2;
                }
            }
            break;
        }
        case OP_SET_ENTITY_XY: {                            /* SET_ENTITY_XY —
 * (lines 855-875): writes (x, y)
 * into the entity matching `reg_id` (which after the pre-
 * dispatch alias is normally this_id for op 0x28's a0==0x28
 * special case, but can be other ids if the script uses the
 * raw form).
 * // (+ attached-prop offset path for bit 2 of +0x3a)
 *
 * Used pervasively to plant actors / props at scripted spawn
 * points (Ebek/Fjej initial position, NPC initial pose, etc.).
 * Earlier port used FindUpdateRegistration(2, reg_id) — wrong:
 * the original walks the click list (FindEntityByVerbId) which
 * misses props registered ONLY in the update table BUT also
 * picks up entities reachable only via kind=1 click entry. */
            extern Entity *FindEntityByVerbId(uint16_t verb);
            Entity *e = FindEntityByVerbId(reg_id);
            if (e) {
                uint16_t x = (len >= 2) ? pc[2] : 0;
                uint16_t y = (len >= 2) ? pc[3] : 0;
                uint8_t *eb = (uint8_t *)e;
                *(uint16_t *)(eb + 0x22) = x;       /* anchor (foot) X */
                *(uint16_t *)(eb + 0x0A) = x;       /* drawn X — initial */
                *(uint16_t *)(eb + 0x24) = y;       /* anchor (foot) Y */
                *(uint16_t *)(eb + 0x0C) = y;       /* drawn Y — initial */
                /* Foot-anchor compensation — case 0x28
 * @ Ghidra line 866-873:
 * Without this the script-positioned actor renders with
 * foot at the requested (x,y) but the sprite isn't
 * offset by the atlas hot-spot — so a -37px foot-X
 * offset wouldn't shift the sprite left of the foot. */
                extern void *ent_ptr_resolve(uint32_t slot);
                AnimAsset *atlas = (AnimAsset *)ent_ptr_resolve(*(uint32_t *)(eb + 0x28));
                uint16_t flags3a = *(uint16_t *)(eb + 0x3A);
                if (atlas && (flags3a & 2) && atlas->off_drawX && atlas->off_drawY) {
                    uint16_t fid = *(uint16_t *)(eb + 0x30);
                    if (atlas->frame_count && fid < atlas->frame_count) {
                        int16_t ax = (int16_t)atlas->off_drawX[fid];
                        int16_t ay = (int16_t)atlas->off_drawY[fid];
                        *(int16_t *)(eb + 0x0A) = (int16_t)(x + ax);
                        *(int16_t *)(eb + 0x0C) = (int16_t)(y + ay);
                    }
                }
            }
            break;
        }

        /* ---- call / tail-call ---------------------------------------- */
        case OP_CALL_SUB: {                            /* TAILCALL
 * case 0x24 (, line ~817):
 *
 * End-of-iteration: `pc = base` (not advanced) because op was
 * 0x24/0x25. No stack push. Effectively a jump replacing the
 * current script with the target function — the call frame
 * stays where it was. */
            uint32_t addr; memcpy(&addr, ((const uint8_t *)pc) + 4, 4);
            const void *xlt = xlat_binary_ptr(addr);
            if (!xlt) {
                /* Unknown binary address — would segfault. Treat as END. */
                LOG_TRACE("script", "TAILCALL to unresolved 0x%08x — END", addr);
                pc = NULL;
            } else {
                base = (const uint16_t *)xlt;
                pc   = base;
            }
            advanced = 1;
            break;
        }
        case OP_TAILCALL: {                            /* CALL_SUB
 * bVar2++;
 * // → effectively re-enters the current script at offset 0)
 *
 * End-of-iteration code (line ~1382):
 * if (op != 0x24 && op != 0x25) pc += len*4;
 * else pc = base; // jump
 *
 * On END (op 0x56) in callee:
 * pc = saved_pc + (byte[saved_pc+1] * 4) // = saved_pc + len*4
 * base = saved_base
 *
 * Port stores the post-call return address eagerly (saves
 * pc + len*2 ushorts = saved_pc + len*4 bytes) — equivalent.
 * On stack overflow we mirror the original: do NOT push, but
 * still update pc = base (restart current script). In practice
 * 10-deep recursion never fires for shipped scripts. */
            uint32_t addr; memcpy(&addr, ((const uint8_t *)pc) + 4, 4);
            const void *xlt = xlat_binary_ptr(addr);
            if (!xlt) {
                LOG_TRACE("script", "CALL_SUB to unresolved 0x%08x — skip", addr);
                /* fall through past the call as a no-op */
            } else if (call_sp < VM_CALL_STACK_DEPTH) {
                call_pc_ret  [call_sp] = pc + len * 2;
                call_base_ret[call_sp] = base;
                ++call_sp;
                base = (const uint16_t *)xlt;
                pc   = base;
                advanced = 1;
            } else {
                /* Stack overflow recovery — mirrors original (no push,
 * pc = base). Loud about it because it's a script bug. */
                LOG_TRACE("script", "CALL_SUB stack overflow (depth=%d), "
                                "restarting current script", call_sp);
                pc = base;
                advanced = 1;
            }
            break;
        }

        /* ---- cursor / perspective ------------------------------------ */
        case OP_SET_PERSPECTIVE:
            g_perspective_min  = a1;
            g_perspective_step = a2;
            g_cursor_speed     = a0;   /* full halfword (was uint8_t — bug) */
            break;

        /* ---- alpha-plane tint sources (NOT sound) -------------------- *
         *
         * op 0x41/0x42 drive the original's positional colored-lighting
         * on alpha-plane sprites, despite the legacy "sound" opcode
         * names. op 0x41 pushes a tint source
         * (x=reg_id, y=a1, rgb=u32@+8, radius=a2); op 0x42 clears the
         * queue. The blend + apply happen in the render path
         * (AlphaTintForListener → SetAlphaTint, actor/render.c). Real
         * SFX are frame-driven via the [sampl] system. */
        case OP_SOUND_PLAY: {
            /* The rgb u32 is at instruction byte +8, NOT +4. */
            uint32_t rgb_at8 = 0;
            if (len >= 3) memcpy(&rgb_at8, ((const uint8_t *)pc) + 8, 4);
            ScriptCallSoundPlay(reg_id, a1, rgb_at8, a2);
            break;
        }
        case OP_SOUND_STOP: ScriptCallSoundStop(); break;
        /* RE confirmed (T16 quick win, 2026-05-27): has
 * TWO writers (op 0x43 sets 0 @ 0x408d69, op 0x44 sets 0x10 @
 * 0x408d77) and ZERO readers anywhere in the binary — verified
 * via Ghidra xrefs. The flag is dead state even in the original
 * game, so our no-op port is correct. Both ops are kept as
 * dispatch-and-bail so scripts that contain them advance their
 * PC past the operand bytes correctly. */
        case OP_NOP_E6C4_A: /* CLEAR_DAT_E6C4 — dead state, no-op */ break;
        case OP_NOP_E6C4_B: /* SET_DAT_E6C4_16 — dead state, no-op */ break;

        /* ---- timers -------------------------------------------------- */
        case OP_TIMER_RESET: if (a0 < 16) g_timer_baselines[a0] = g_tick_counter; break;
        case OP_TIMER_READ: if (a0 < 16) g_return_reg = (uint16_t)(g_tick_counter - g_timer_baselines[a0]); break;

        /* ---- palette ------------------------------------------------- */
        case OP_PAL_LOAD_FADE: ScriptCallPalLoad(a0, i32_at4, 1); break;
        case OP_PAL_FADE_STEP: g_return_reg = (uint16_t)ScriptCallPalFadeStep(); break;
        case OP_PAL_LOAD_INSTANT: ScriptCallPalLoad(0,  i32_at4, 0); break;

        /* ---- subanim toggles ---------------------------------------- */
        case OP_SUBANIM_HIDE_TOGGLE: {                            /* SUBANIM_HIDE_TOGGLE — 
 * with Ghidra case OP_SUBANIM_HIDE_TOGGLE:
 * whose +0x16 == asset->pixel_ptrs[frame];
 * } */
            extern void *FindUpdateRegistration(uint16_t kind, uint16_t id);
            extern int   EntityListCount(int);
            extern Entity *EntityListAt(int, int);
            extern void *ent_ptr_resolve(uint32_t slot);
            AnimAsset *asset = (AnimAsset *)FindUpdateRegistration(1, reg_id);
            uint16_t frame = (len >= 2) ? pc[2] : 0;
            uint16_t flag  = (len >= 2) ? pc[3] : 0;
            int matched = 0;
            if (asset && asset->pixel_ptrs && frame < asset->frame_count) {
                void *target_pix = asset->pixel_ptrs[frame];
                int nclk = EntityListCount(1);          /* click list */
                for (int i = 0; i < nclk; ++i) {
                    Entity *m = EntityListAt(1, i);
                    if (!m) continue;
                    uint8_t *mb = (uint8_t *)m;
                    if (*(uint16_t *)(mb + 8) != 2) continue; /* kind=2 click */
                    Entity *owner = (Entity *)ent_ptr_resolve(*(uint32_t *)(mb + 0x0a));
                    if (!owner) continue;
                    void *owner_pix =
                        ent_ptr_resolve(*(uint32_t *)((uint8_t *)owner + 0x16));
                    if (owner_pix != target_pix) continue;
                    uint8_t *ob = (uint8_t *)owner;
                    if (flag == 0) *(uint16_t *)(ob + 0x14) &= (uint16_t)~0x8000u;
                    else            ob[0x15] |= 0x80;
                    matched = 1;
                    break;
                }
            }
            LOG_TRACE("op50", "id=%u asset=%s frame=%u flag=%u → %s%s", reg_id, asset ? asset->name : "(none)", frame, flag, flag == 0 ? "HIDE" : "SHOW", matched ? "" : " (NO MASK MATCHED)");
            break;
        }
        case OP_SUBANIM_SET_PARAM: {                            /* SUBANIM_SET_PARAM — 
 * with Ghidra case OP_SUBANIM_SET_PARAM: same asset+frame lookup as 0x50,
 * then writes local_158[3] into mask's +0x12 (foot_y mirror
 * used for z-sort + hit-test bbox bottom). */
            extern void *FindUpdateRegistration(uint16_t kind, uint16_t id);
            extern int   EntityListCount(int);
            extern Entity *EntityListAt(int, int);
            extern void *ent_ptr_resolve(uint32_t slot);
            AnimAsset *asset = (AnimAsset *)FindUpdateRegistration(1, reg_id);
            uint16_t frame = (len >= 2) ? pc[2] : 0;
            uint16_t val   = (len >= 2) ? pc[3] : 0;
            if (asset && asset->pixel_ptrs && frame < asset->frame_count) {
                void *target_pix = asset->pixel_ptrs[frame];
                int nclk = EntityListCount(1);
                for (int i = 0; i < nclk; ++i) {
                    Entity *m = EntityListAt(1, i);
                    if (!m) continue;
                    uint8_t *mb = (uint8_t *)m;
                    if (*(uint16_t *)(mb + 8) != 2) continue;
                    Entity *owner = (Entity *)ent_ptr_resolve(*(uint32_t *)(mb + 0x0a));
                    if (!owner) continue;
                    void *owner_pix =
                        ent_ptr_resolve(*(uint32_t *)((uint8_t *)owner + 0x16));
                    if (owner_pix != target_pix) continue;
                    *(uint16_t *)((uint8_t *)owner + 0x12) = val;
                    break;
                }
            }
            break;
        }

        /* ---- dialogue subsystem -------------------------------------
         *
         * OP_DIALOG_PUSH and OP_DIALOG_PLAY both read DWORD POINTERS
         * at instruction offsets +4, +8, +12 — they are NOT inline
         * strings (an earlier port used pc+2 which was wrong). We
         * resolve through xlat so the dialog name lives in PE memory
         * (Gadki.scr keys etc.). */
        case OP_DIALOG_PUSH: {
            /* DialogPush args:
             *   pc+4  char *dialog_name → load asset, push to stack
             *   pc+8  byte *opts_bytes  → horner-hashed into slot[+4]
             *   pc+12 u32   talk_anim_va → PE VA of mouth-cycle
             *                              bytecode stored at
             *                              slot[+0x0C]; bound to
             *                              entity[+0x2C] on ACTIVATE
             *
             * T108 fix — an earlier port read pc+12 as `opts_cnt`
             * (uint count); it's actually a PE VA pointing to
             * talking-head animation bytecode. We resolve it via
             * xlat_binary_ptr and thread it through
             * ScriptCallDialogBegin → DialogStackPush →
             * slot.talk_anim_va, then DialogActivateTopSpeaker binds
             * it to the speaker entity's bytecode pointer (replacing
             * T20c's manual frame 0↔1 mouth toggle). */
            uint32_t opts_va = 0, talk_anim_va = 0;
            if (len >= 3) memcpy(&opts_va,      ((const uint8_t *)pc) + 8,  4);
            if (len >= 4) memcpy(&talk_anim_va, ((const uint8_t *)pc) + 12, 4);
            const char    *name = (const char *)xlat_binary_ptr(i32_at4);
            const uint8_t *opts = (const uint8_t *)xlat_binary_ptr(opts_va);
            ScriptCallDialogBegin(reg_id, name, opts, talk_anim_va);
            break;
        }
        case OP_DIALOG_PLAY: {
            const char *result_key = (const char *)xlat_binary_ptr(i32_at4);
            ScriptCallDialogEnd(result_key);
            break;
        }
        case OP_SHOW_PICTURE: {                            /* SHOW_PICTURE
 * Ghidra case OP_SHOW_PICTURE: (*(char**)(local_158 + 2)).
 *
 * (name):
 *; // pump events
 *; // mm-tick
 *
 * Loads + displays a .pic file fullscreen, waits up to ~0xFFFF
 * frame-deltas (or until user clicks LMB). */
            const char *name = (const char *)xlat_binary_ptr(i32_at4);
            if (name) {
                void    *raw = NULL;
                uint32_t sz  = 0;
                if (LoadFileFromDta(name, &raw, &sz) && raw) {
                    extern int paint_rawb_pic(const void *blob, uint32_t size, int as_overlay);
                    if (paint_rawb_pic(raw, sz, 0)) {
                        FlushFrameToPrimary();
                        /* Wait for LMB or timeout — pump events each frame
 * so SDL stays alive and the user can click out. */
                        extern uint8_t  g_lmb_clicked;
                        uint32_t budget_ms = 0xFFFF * 16;   /* ~17 minutes */
                        while (budget_ms > 0) {
                            PlatformPumpEvents();
                            if (PlatformShouldQuit()) break;
                            if (g_lmb_clicked) {
                                g_lmb_clicked = 0;
                                break;
                            }
                            EnginePaceFrame(33);
                            budget_ms -= 16;
                        }
                    }
                    xfree(raw);
                } else {
                    LOG_TRACE("script", "op 0x54 show-picture '%s' not found", name);
                }
            }
            break;
        }

        /* ---- abort / end -------------------------------------------- */
        case OP_RETURN_FALSE: result = 0; break;
        case OP_END_HARD:                              /* END_HARD */
        case OP_END:                              /* END (implicit) */
            pc = NULL; advanced = 1;
            break;

        /* ---- debug log --------------------------------------------- *
 * NOTE: Original appends log line to
 * __tmp.txt on disk; port writes to stderr instead (no
 * filesystem spam during development). Behavioural delta is
 * cosmetic — debug-only path. */
        case 0x57:
            LOG_TRACE("script", "log: %.*s", 64, (const char *)(pc + 2));
            break;

        /* ---- implicit no-op markers (have no case in original) ------ */
        case OP_ENDIF:   /* ENDIF — consumed by 0x00..0x05/0x07 scan */
        case 0x08:
        case OP_LABEL:   /* LABEL — consumed by find_label */
        case 0x1E:
        case 0x36:
        case 0x39:
        default:
            break;
        }

        if (!advanced) {
            /* Default advance: len*4 bytes = len*2 ushorts.
 * Defensive: malformed len=0 → terminate to avoid infinite loop. */
            if (!pc) break;
            if (len == 0) { pc = NULL; break; }
            pc += len * 2;
        }

        /* CALL_SUB / TAILCALL terminator: hitting end-of-block while the
 * call stack has frames → pop and resume the caller. */
        if (pc == NULL && call_sp > 0) {
            --call_sp;
            base = call_base_ret[call_sp];
            pc   = call_pc_ret  [call_sp];
        }
    }

    if (--g_script_reentry <= 0) {
        g_script_running   = 0;
        g_script_reentry   = 0;
    }
    return result;
}
