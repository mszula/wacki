/*
 * script.c — Wacki bytecode VM and .scr loader.
 *
 * Original addresses:
 *   RunScriptInterpreter        0x00407820
 *   LoadScriptFile              0x00409690
 *   FindScriptByStageAndRoom    0x004097E0
 *   FindTagInScript             0x00409840
 *   ScanTextTag                 0x00409760
 *
 * Bytecode format (instruction stream is a u16 array; pc is u16 *):
 *   word0   = [op:u8 at +0][len:u8 at +1]
 *   word1   = first u16 operand (a0 / "uVar31")           — in same dword as header
 *   word2.. = additional operand dwords (4 bytes each)
 *
 * Total instruction size = len * 4 bytes (len is in DWORDS).
 *
 * Pre-rewrite the file had a stub for ~20 opcodes; this revision is the
 * full 78-opcode table mapped from RunScriptInterpreter @ 0x00407820. Where
 * an opcode calls into a subsystem we haven't ported yet (entity walker,
 * DirectSound mixer, palette fader), we trace [script] log it and pc-
 * advance so the VM keeps running deterministically.
 *
 * .scr file layout (text-tagged):
 *   [etap]N
 *   [komnata]M
 *   <binary bytecode block>
 *   ...
 *   Other tags found in shipped scripts: [rozmowa], [animacja], [sampl].
 */
#include "wacki.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <SDL.h>

/* ========================================================================= *
 * Engine globals — 1:1 with WACKI.EXE .data layout (only those the VM reads
 * or writes; the rest live in their owner module).
 * ========================================================================= */
uint32_t g_script_vars[0x129];              /* DAT_00449880 — register file */
/* g_return_reg ALIAS — DAT_00449890 in the original is at offset 0x10
 * from DAT_00449880, which is var[4] in our register file (4*4 bytes).
 * Scripts read the return value via op 0x04 IF_EQ a0=4 etc., so the
 * write target MUST be var[4], not a separate variable. The macro keeps
 * call sites readable. */
#define g_return_reg (*(uint16_t *)&g_script_vars[4])
/* DAT_0044A198 is `undefined2` (uint16_t, 2 bytes) per Ghidra type info.
 * Original perspective formula reads `(uint)DAT_0044a198` (zero-extend
 * from 16 bits). Earlier port declared as uint8_t — op 0x40 invocations
 * with a0 > 255 (e.g. verb-action's 0x40 with reg_id=0x0109=265 at
 * 0x0042740C, for the "wsiądź do auta" climb sequence) silently truncated
 * to 0x09=9. With cs=9, perspective z went negative for nearly every
 * Y, clamped to 0 → renderer fell through `if (scale && scale != 100)`
 * to natural-size blit → Ebek rendered HUGE during the climb instead
 * of shrinking with perspective + disappearing behind horyzont.msk
 * walk-behind. Fix: store full uint16_t to match original. */
uint16_t g_cursor_speed = 0x78;             /* DAT_0044A198 (was uint8_t — bug) */
uint16_t g_perspective_min  = 4;            /* DAT_00449878 */
uint16_t g_perspective_step = 7;            /* DAT_0044987C */
uint16_t g_active_actor = 0;                /* DAT_0044E6A4 — 0=Ebek, 1=Fjej  */
/* g_dialogue_mode removed — was misnamed and unused. The misnamed
 * placeholder for DAT_0044988C (= scene hover verb) is not needed in
 * our port because ClickHitTest returns the hover via out-parameter
 * instead of a global. */
uint16_t g_anim1_done = 0;                  /* DAT_00449884 */
uint16_t g_anim2_done = 0;                  /* DAT_00449888 */
int      g_script_running = 0;              /* DAT_00451470 — !=0 inside VM   */
int      g_script_reentry = 0;              /* DAT_00455008 — reentrancy ctr  */
uint32_t g_timer_baselines[16];             /* DAT_004548A0[]                 */

extern uint32_t g_tick_counter;             /* DAT_0044E454 — ms since boot   */

/* engine helpers used by various opcodes — kept as externs; many resolve to
 * stubs in stubs.c until the relevant subsystem is ported. */
extern void  ProcessGameFrameTick(void);
extern void  PaletteFadeStep(int delta);
extern void  PrintTextOnScreen(uint16_t hx, uint16_t hy, const char *text);
extern uint32_t g_frame_delta_ms;           /* real wall-clock ms             */
extern uint16_t g_frame_delta_ticks;        /* DAT_0044E578 — 10 ms ticks      */

/* Stub-side script bridges (defined in stubs.c if missing). They take the
 * raw operand words and the active register id, and may pc-advance the
 * interpreter via side effects. All return void. */
extern void ScriptCallSoundPlay (uint16_t id, uint16_t a, uint32_t b, uint16_t c);
extern void ScriptCallSoundStop (void);
/* ScriptCallAnimActor removed — ops 0x10/0x11/0x12 inline ActorWalkToBlocking. */
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
extern const void *xlat_binary_ptr(uint32_t addr);
extern int         PeLoaderContainsVA(uint32_t va);
extern const char *xlat_asset_name(uint32_t addr);
extern void ScriptCallWalkMode  (uint16_t id, int mode);
extern void ScriptCallWalkTo    (uint16_t id, uint16_t target, int mode);
extern void ScriptCallAttachProp(uint16_t actor, uint16_t prop, int keep);
extern void ScriptCallShowText  (uint16_t actor, const char *text);
extern int  ScriptCallDialogBegin(uint16_t actor, const char *a, const uint8_t *b, uint32_t c);
extern void ScriptCallDialogEnd  (const char *result);

/* ScriptObj + LoadScriptFile + FindScriptByStageAndRoom +
 * ScriptObjFindSection moved to src/vm/script_obj.c. */

/* Register-file accessors (var_get/var_set) and bytecode scanners
 * (skip_to_endif, find_label) moved to src/vm/parser.{c,h}. Local
 * aliases keep the inline call sites readable. */
#include "vm/parser.h"
#define var_get        vm_var_get
#define var_set        vm_var_set
#define skip_to_endif  vm_skip_to_endif
#define find_label     vm_find_label

/* ========================================================================= *
 * RunScriptInterpreter — 0x00407820, 78 opcodes mapped from Ghidra.
 * ========================================================================= */
#define VM_CALL_STACK_DEPTH  10
#define VM_LOOP_SLOTS        10

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

    /* Dialog choice accumulator — 1:1 with Ghidra's stack-local
     * local_124 / auStack_122 / uVar13. Up to 6 entries, each:
     *   { u16 speaker_id, u32 dialog_ptr, u32 dialog_data }
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
            fprintf(stderr, "[script] bogus op=0x%02x len=%u at pc=%p — END\n",
                    op, len, (void *)pc);
            pc = NULL;
            if (call_sp > 0) { --call_sp; base = call_base_ret[call_sp];
                               pc = call_pc_ret[call_sp]; }
            continue;
        }
        /* fprintf(stderr, "[vm] op=0x%02x len=%u pc=%p\n", op, len, (const void*)pc); */
        uint16_t a0 = pc[1];                    /* first u16 operand (uVar31) */
        uint16_t a1 = (len >= 2) ? pc[2] : 0;   /* low u16 of operand dword 1 */
        uint16_t a2 = (len >= 2) ? pc[3] : 0;   /* high u16 of operand dword 1 */
        uint32_t i32_at4 = 0;
        if (len >= 2) memcpy(&i32_at4, ((const uint8_t *)pc) + 4, 4);
        int advanced = 0;

        /* reg_id = which register the opcode operates on.
         *
         * 1:1 with Ghidra 0x00407820 line 218-223 (the `uVar29 =
         * (ushort)puVar15` derivation that precedes the switch):
         *
         *   uVar31 = local_158[1];                  // = a0 (1st operand u16)
         *   puVar15 = local_144;                     //   = this_id
         *   if ((uVar31 != 0x28) && (puVar15 = local_140, uVar31 != 0x27)) {
         *     puVar15 = unaff_EDI;                   //   = a0
         *   }
         *   uVar29 = (ushort)puVar15;
         *
         * I.e. when the FIRST OPERAND of any instruction is 0x27 or 0x28,
         * the register index is re-mapped to that_id or this_id. This is
         * how scripts address the "current" or "calling" entity without
         * hard-coding its id.
         *
         * Earlier this port had `reg_id = a0` always and then set reg_id
         * inside the case 0x27/0x28 (dead code — `break` followed).  That
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
        case 0x00: {                            /* SKIP_TO_END
             * 1:1 with Ghidra case 0 (line 213): `if (uVar29 != uVar23)
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
        case 0x01:                              /* IF_NE   (skip if var==imm) */
        case 0x02:                              /* IF_GT   (skip if var<=imm) */
        case 0x03:                              /* IF_LT   (skip if var>=imm) */
        case 0x04:                              /* IF_EQ   (skip if var!=imm) */
        case 0x05: {                            /* IF_ALL_BITS_SET (skip if (var&imm)!=imm) */
            /* Per Ghidra 0x004078d1: each case has `if (CONDITION) { skip }`,
             * so `take` = !CONDITION. Earlier this port had the senses
             * inverted — every IF that should have taken its body skipped
             * it, and every IF that should have skipped took it. That
             * scrambled which conditional sprites a scene's enter_script
             * spawned (e.g. klatka2 dropped babcia1b, kept skarpeta on
             * fresh-game state, etc.). Verified against decompile case
             * bodies 1-5 in RunScriptInterpreter @ 0x00407820. */
            uint32_t v   = var_get(reg_id);
            uint32_t imm = i32_at4;
            int take = 0;
            switch (op) {
            case 0x01: take = (v != imm); break;
            case 0x02: take = ((int32_t)v >  (int32_t)imm); break;
            case 0x03: take = ((int32_t)v <  (int32_t)imm); break;
            case 0x04: take = (v == imm); break;
            case 0x05: take = ((v & imm) == imm); break;   /* unchanged — was correct */
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
        case 0x07:                              /* ELSE (skip true-branch) */
            pc = skip_to_endif(pc);
            if (pc) {
                uint8_t mlen = ((const uint8_t *)pc)[1];
                pc = mlen ? (pc + mlen * 2) : NULL;
            }
            advanced = 1;
            break;

        case 0x17: {                            /* JUMP_LABEL — 1:1 with
            * Ghidra case 0x17:
            *   if (base == NULL) goto FALLTHROUGH;
            *   walk base looking for op 0x16 with arg == a0;
            *   if found: pc = found_address;
            *   if not found: FALLTHROUGH (puVar16 is at 0x56 marker
            *                  but the LAB_00408609 check rejects it). */
            const uint16_t *t = find_label(base, a0);
            if (t) { pc = t; advanced = 1; }
            /* base==NULL or label not found → fall through to default
             * advance (pc += len*2). Earlier port set pc = NULL when
             * base was NULL — that ended the script instead of just
             * skipping this opcode. */
            break;
        }
        case 0x18: {                            /* LOOP_COUNTED — 1:1 with
            * Ghidra case 0x18: counter slot = number of 0x18 opcodes
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
            * not 1:1 with original. Now matches. */
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
        case 0x0A: var_set(reg_id, var_get(reg_id) |  i32_at4); break;
        case 0x0C: var_set(reg_id, var_get(reg_id) & ~i32_at4); break;
        case 0x0D: var_set(reg_id, i32_at4); break;
        case 0x4D: var_set(reg_id, var_get(reg_id) + i32_at4); break;
        case 0x4E: var_set(reg_id, var_get(reg_id) - i32_at4); break;

        /* ---- query-and-store-in-return-register ---------------------- */
        case 0x21: g_return_reg = (uint16_t)(((this_id - 0x29) < 0x8E) ? 2 : 1); break;
        case 0x22: g_return_reg = that_id; break;
        case 0x23: {
            /* 1:1 with Ghidra @ case 0x23:
             *   uVar26 = FUN_00405650(uVar29);
             *   _DAT_00449890 = (ushort)(uVar26 & 0xff);
             *
             * = InventoryHasItem(reg_id) → store result in var[4] = g_return_reg.
             * Used by scripts as a precondition: `if (has(item)) { ... }`. */
            g_return_reg = (uint16_t)InventoryHasItem(reg_id);
            break;
        }
        case 0x29: g_return_reg = g_cur_komnata; /* 1:1 with Ghidra case 0x29: returns DAT_0044e588 (current komnata id). Was wrongly reading g_script_vars[0]. */ break;
        case 0x2A: g_return_reg = (uint16_t)WackiRand((uint16_t)a0); /* 1:1 FUN_00410F50(bound=a0) */ break;
        case 0x34: g_return_reg = (uint16_t)(g_active_actor + 1); break;

        /* ---- wait / pacing ------------------------------------------- */
        case 0x13: ProcessGameFrameTick(); SDL_Delay(33); break;  /* T-anim-speed */
        case 0x14: {                            /* WAIT — a0 in 10 ms TICKS.
            * Original FUN_00407820 case 0x14 wait loop @ 0x00408366:
            *
            *     uint16_t left = a0;
            *     do {
            *         FUN_004025C0();                 // PGFT
            *         uint16_t dt = DAT_0044E578;     // ticks elapsed
            *         if (left < dt) { left = 0; break; }
            *         left -= dt;
            *     } while (left);
            *
            * DAT_0044E578 is in 10 ms units (timeSetEvent 10ms period at
            * 0x00403D84), so the operand a0 is ALSO ticks — a script that
            * says "WAIT 100" waits 100*10 = 1000 ms = 1 s. Earlier port
            * treated a0 as raw ms which ran waits 10× too fast. */
            uint32_t left = a0;
            while (left) {
                ProcessGameFrameTick();
                if (PlatformShouldQuit() || g_game_over_code) break;
                uint32_t step = g_frame_delta_ticks ? g_frame_delta_ticks : 3;
                left = (left > step) ? left - step : 0;
                SDL_Delay(33);  /* T-anim-speed: match main loop pacing */
            }
            break;
        }
        case 0x15: {                            /* WAIT_ENTITY — 1:1 with
            * Ghidra case 0x15:
            *   iVar20 = FUN_00404C30(uVar29);            // find by verb_id
            *   if (iVar20 != 0) {
            *     while (entity[+0x4C] || entity[+0x50])  // walking?
            *       ProcessGameFrameTick();
            *   }
            * Entity fields +0x4C / +0x50 = walk_dx_remaining / walk_dy_remaining
            * (set by FUN_00401150 in WALK_TO op, decremented per step). */
            extern Entity *FindEntityByVerbId(uint16_t verb_id);
            Entity *e = FindEntityByVerbId(reg_id);
            if (e) {
                uint8_t *eb = (uint8_t *)e;
                int safety = 2000;
                while (safety-- > 0 &&
                       (*(uint32_t *)(eb + 0x4C) || *(uint32_t *)(eb + 0x50))) {
                    ProcessGameFrameTick();
                    if (PlatformShouldQuit() || g_game_over_code) break;
                    SDL_Delay(33);  /* T-anim-speed: match main loop pacing */
                }
            }
            break;
        }
        case 0x26: {                            /* WAIT_ANIM_FRAME — 1:1
             * with Ghidra case 0x26:
             *   e = FUN_00404C30(verb_id);
             *   if (e) while (e[+0x30] != target) ProcessGameFrameTick();
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
                    SDL_Delay(33);  /* T-anim-speed: match main loop pacing */
                }
            }
            break;
        }
        case 0x3D: {                            /* WAIT_KIND2_FRAME — 1:1 with
             * Ghidra case 0x3D:
             *   e = FindUpdateRegistration(2, reg_id);   // kind=2 sprite slot
             *   if (e) {
             *       target = a1;
             *       while (e->+0x30 != target) ProcessGameFrameTick();
             *   }
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
                    SDL_Delay(33);  /* T-anim-speed: match main loop pacing */
                }
            }
            break;
        }

        /* ---- actor walk-to + blocking wait — 1:1 with Ghidra ops
         *      0x10 (actor 0), 0x11 (actor 1), 0x12 (both, with 50-tick
         *      head-start). Original case 0x10:
         *
         *        if (actor.+0x22 != uVar31 ||              // = a0 (X)
         *            actor.+0x24 != local_158[2]) {        // = a1 (Y)
         *          ...
         *          UpdateActorMovement(uVar31, local_158[2]);
         *          // wait loop on actor walker fields
         *        }
         *
         * Confirmed via concrete bytecode: verb 7 (exit-left) at
         * 0x00427BB0 encodes `12 02 1E 00 72 01 00 00` = op 0x12 walking
         * actors to (30, 370) — that's the left-exit target with Y on
         * the walkable floor. Target reads as (a0=30, a1=370). a2 = 0
         * here is the "wait mode" flag for op 0x12.
         *
         * History note: earlier port read (a0, a1) → fix #68 thought
         * "off by one halfword" and changed to (a1, a2) — that was the
         * wrong direction. Reverted to (a0, a1).
         *
         * Bytecode layout: `[op:1][len:1] [X:2] [Y:2] [mode:2]`
         * len=2 means 8 bytes total (len*4). For op 0x10/0x11 a2 is
         * unused; for op 0x12 a2 = wait-mode flag (0 = wait for both,
         * else wait for actor 0 only). */
        case 0x10:                              /* walk Ebek */
            ActorWalkToBlocking(0, (int16_t)a0, (int16_t)a1);
            break;
        case 0x11:                              /* walk Fjej */
            ActorWalkToBlocking(1, (int16_t)a0, (int16_t)a1);
            break;
        case 0x12: {                            /* walk both, 50-tick init
             * 1:1 with original case 0x12 (Ghidra @ line 582):
             *   setup actor 0 walker  (target = a0, a1)
             *   wait 0x32 ticks
             *   setup actor 1 walker  (same target)
             *   if (a2 == 0) wait for BOTH; else wait for actor 0 only
             *
             * a2 = mode flag (0 = wait both, 1 = wait actor 0). */
            extern void ActorWalkBothBlocking(int16_t, int16_t, int);
            ActorWalkBothBlocking((int16_t)a0, (int16_t)a1, (int)a2);
            break;
        }
        case 0x33: {                            /* RESET_BOTH_ACTORS —
            * 1:1 with Ghidra case 0x33:
            *   FUN_00402500(g_actor[0]);                       // reset state
            *   DAT_0044E570 = -1;                              // walk-target id reset
            *   g_actor[0].+0x2C = *(int*)(stage+0xC) + 0x10;   // idle bytecode
            *   FUN_00402500(g_actor[1]);
            *   DAT_0044E5C0 = -1;
            *   g_actor[1].+0x2C = *(int*)(stage+0x10) + 0x10;
            * The +0xC / +0x10 offsets into the stage struct point at
            * per-actor animation tables; offset +0x10 inside that table
            * is the "idle" bytecode entry. */
            extern Entity *g_actor[2];
            for (int ai = 0; ai < 2; ++ai) {
                Entity *e = g_actor[ai];
                if (!e) continue;
                uint8_t *eb = (uint8_t *)e;
                /* FUN_00402500 reset */
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
        case 0x09: {                            /* SHOW_TEXT — 1:1 with
            * Ghidra case 0x09 (lines 352-496): allocates a text-balloon
            * entity, renders multi-line wrapped text into it, positions
            * relative to actor's foot, waits for click/timeout.
            *
            * Operand layout per Ghidra: `*(char **)(local_158 + 2)` reads
            * a DWORD at byte +4 of the instruction → a binary pointer
            * into .data where the string lives.
            *
            * 1:1 prefix (lines 353-371 of FUN_00407820): search
            * dlg_speaker[] for an entry matching reg_id. If found at
            * index N (N < dlg_count), bind dlg_ptr[N] as a per-entity
            * script on the speaker entity (= FUN_00401210). This makes
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
                    /* FUN_00401210 — find entity by verb_id, reset state,
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
                         * to re-bind when this balloon dismisses (1:1
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
            /* Block until balloon dismisses — 1:1 with original case 9
             * wait loop (Ghidra @ FUN_00407820 lines ~462-475):
             *
             *   DAT_0044E5AC = 0;
             *   do {
             *     if (DAT_00455004 == 0) break;
             *     DAT_0044E5A4 = DAT_0044E5AC;
             *     ProcessGameFrameTick();
             *     if (DAT_00455004 < DAT_0044E578) DAT_00455004 = 0;
             *     else DAT_00455004 -= DAT_0044E578;
             *   } while (DAT_0044E5AC == '\0');
             *
             * = pump frames until: dismiss timer (DAT_00455004) hits 0,
             *   OR user click (DAT_0044E5AC != 0).
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
                        /* Force-dismiss: simulate DAT_0044E5AC click by
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
        case 0x0B: {                            /* SET_ACTOR_FLAG2 + frame override —
            * 1:1 with Ghidra case 0x0b (lines 501-507): lookup kind=2
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
        case 0x0E: {                            /* SET_ENTITY_SCRIPT
            * 1:1 port of FUN_00401210 @ 0x00401210:
            *
            *   iVar1 = FUN_00404C30(verb_id);            // find by verb
            *   if (iVar1) {
            *     FUN_00402500(iVar1);                   // reset state
            *     entity[+0x2C] = bytecode_ptr;           // bind new script
            *     entity[+0x30] = 0;                     // frame = 0
            *   }
            *
            * Used by 0x00423E28 ELSE branch to bind Ebek's idle script
            * (0x004230A8) and Fjej's (0x00423280). Earlier mis-port
            * treated this as "destroy dialog balloon" → wiped both
            * actors right after SpawnActorEntity. */
            extern Entity *FindEntityByVerbId(uint16_t verb_id);
            Entity *e = FindEntityByVerbId(reg_id);
            if (e) {
                uint8_t *eb = (uint8_t *)e;
                /* FUN_00402500 reset (same block as op 0x33) */
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
            }
            break;
        }
        case 0x0F: {                            /* SET_ENTITY_ANIM —
            * 1:1 with Ghidra case 0x0f (lines 522-530):
            *
            *   iVar20 = FUN_00405dc0(2, uVar29);   // kind=2 entity by id
            *   if (iVar20) {
            *     FUN_00402500(iVar20);              // FULL state reset
            *     entity[+0x30] = 0;                 // frame = 0
            *     entity[+0x2C] = *(dword)(pc+4);    // bind new bytecode
            *   }
            *
            * FUN_00402500 (full reset) clears +0x32, +0x34, +0x36, +0x38,
            * +0x3A&~5, +0x3C, +0x40, +0x42, +0x4C, +0x50. Earlier port
            * missed +0x40 (osc counter A), +0x42 (osc B), +0x4C (walker
            * dx_remaining), +0x50 (walker dy_remaining) — residual walker
            * state from the previous anim could trigger spurious motion
            * after rebind. */
            extern void *FindUpdateRegistration(uint16_t kind, uint16_t id);
            Entity *e = (Entity *)FindUpdateRegistration(2, reg_id);
            if (e) {
                uint8_t *eb = (uint8_t *)e;
                /* Full FUN_00402500 reset (matches op 0x0E, 0x33 reset blocks). */
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
        case 0x19: {                            /* QUEUE_DIALOG
             * 1:1 with Ghidra case 0x19 (line 745):
             *
             *   if (uVar13 < 6) {
             *     iVar20 = 0;
             *     if (local_124 != uVar29) {            // slot 0 not match
             *       puVar15 = &local_124;
             *       do {
             *         if (uVar13 <= iVar20) break;       // scan up to current
             *         puVar15 += 5; iVar20++;            // 10-byte stride
             *       } while (*puVar15 != uVar29);        // until match
             *     }
             *     // iVar20 = matching slot OR uVar13 (append)
             *     slot[iVar20].speaker = uVar29;
             *     slot[iVar20].ptr     = *(u32)(pc+4);
             *     slot[iVar20].data    = *(u32)(pc+8);
             *     uVar13++;                              // ALWAYS, not just on append
             *   }
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
                ++dlg_count;                /* 1:1 — always grow the high-water */
                /* T20b debug — trace dialog choice queue. The verb_id
                 * here is what op 0x1A will later add to the panel
                 * inventory slot; ptr/data reference Gadki.scr sections
                 * for the response audio. */
                fprintf(stderr, "[dlg] op 0x19 QUEUE slot=%d verb=0x%04X "
                                "ptr=0x%08X data=0x%08X (count→%d)\n",
                        slot, reg_id, ptr_v, data_v, dlg_count);
            }
            break;
        }
        /* ---- dialog page-swap ops — 1:1 with RunScriptInterpreter
         * @ 0x00408704..0x004087F7 in the original. All five ops drive
         * the same panel verb-table rotation via FUN_004071F0
         * (= PanelPageSwap), gated on different pre-conditions:
         *
         *   case 0x1A: FUN_00405730(dlg_count) + InventorySetPageForItem(reg)
         *              + PanelPageSwap()
         *      The dialog accumulator (built by op 0x19) gets committed
         *      into the inventory slots starting at slot 0; page slid
         *      to where the speaker's verb landed; then panel refreshed.
         *      Effectively "open dialog menu". FUN_00405730 here takes
         *      the accumulator count, not an item_verb — it copies the
         *      6-slot stack-local into the inventory. We approximate by
         *      writing the accumulated speakers + dlg_ptr values into
         *      inventory[0..n-1] and zeroing the rest.
         *
         *   case 0x1B: FUN_004057C0(reg) + FUN_004056D0 + PanelPageSwap()
         *      "Close dialog" — original removes the choice marker
         *      (FUN_004057C0), collapses to first non-empty page
         *      (FUN_004056D0 = InventoryPageCollapse), refreshes panel.
         *
         *   case 0x1C: if (InventoryPageNext()) PanelPageSwap()
         *      Page-down (panel right arrow).
         *
         *   case 0x1D: if (InventoryPagePrev()) PanelPageSwap()
         *      Page-up (panel left arrow).
         *
         *   case 0x1F: FUN_00405850(reg)
         *      Drop / consume currently-held item. FUN_00405850 is a
         *      small helper that clears the inventory slot holding the
         *      given verb. Stubbed for now — needs porting alongside
         *      the full pickup/drop pipeline.
         *
         * Without a full dialog UI, these ops still move data correctly;
         * the panel verbs change but won't visually render dialog text
         * until the panel-label render path is ported (see D2 in REVIEW). */
        case 0x1A: {
            /* 1:1 with Ghidra case 0x1a (line 765):
             *   FUN_00405730((uint)puVar15);    // InventoryAddItem(reg_id)
             *   FUN_00405680(uVar29);            // InventorySetPageForItem(reg_id)
             *   FUN_004071F0();                  // PanelPageSwap()
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
                fprintf(stderr, "[dlg] op 0x1A COMMIT verb=0x%04X (dlg_count=%d) "
                                "panel_tab=[%04X %04X %04X %04X %04X %04X]\n",
                        reg_id, dlg_count,
                        g_panel_verb_tab[0], g_panel_verb_tab[1], g_panel_verb_tab[2],
                        g_panel_verb_tab[3], g_panel_verb_tab[4], g_panel_verb_tab[5]);
            }
            break;
        }
        case 0x1B: {
            /* 1:1 with Ghidra @ 0x0040871A:
             *   FUN_004057C0(reg_id);
             *   FUN_004056D0();             // InventoryPageCollapse
             *   FUN_004071F0();             // PanelPageSwap
             */
            (void)InventoryRemoveItem(reg_id);
            InventoryPageCollapse();
            PanelPageSwap();
            fprintf(stderr, "[dlg] op 0x1B CLOSE verb=0x%04X\n", reg_id);
            break;
        }
        case 0x1C: {
            /* 1:1 with Ghidra @ 0x004087E0:
             *   if (FUN_004055D0() != 0) FUN_004071F0();   // page-next
             */
            if (InventoryPageNext()) PanelPageSwap();
            break;
        }
        case 0x1D: {
            /* 1:1 with Ghidra @ 0x004087F7:
             *   if (FUN_004055B0() != 0) FUN_004071F0();   // page-prev
             */
            if (InventoryPagePrev()) PanelPageSwap();
            break;
        }
        case 0x1F: {
            /* 1:1 with Ghidra @ 0x00408726:
             *   FUN_00405850(reg_id);   // InventoryDropItem
             */
            InventoryDropItem(reg_id);
            break;
        }

        /* ---- exits / scene transitions ------------------------------- */
        case 0x20: {                            /* GO_EXIT — 1:1 with
            * Ghidra case 0x20:
            *   if (uVar31 == 0x26) target = DAT_0044E588;   // 0x26 = self
            *   else                target = uVar31;
            *   FUN_00402A50(target);
            *
            * 0x26 special-case = "reload current komnata" (used by
            * scripts that want a state refresh without changing room). */
            uint16_t target = (a0 == 0x26) ? g_cur_komnata : a0;
            ScriptGoToKomnata(target);
            break;
        }
        case 0x2B: {
            /* 1:1 with Ghidra case 0x2b:
             *   if (((ushort)(uVar29 - 0x29) < 0x8e) || (uVar29 == 0x26))
             *     DAT_0044E5E8 = uVar29;       // g_held_item
             *
             * uVar29 = RESOLVED reg_id (this/that-mapped), NOT raw a0.
             * Earlier port read raw `a0` — when scripts use 0x28 (this)
             * or 0x27 (that) as operand, port tested 0x28/0x27 against
             * 0x29 range (both < 0x29 so unsigned underflow → fails)
             * and never wrote held_item. With reg_id the resolved
             * verb_id (e.g. 1 = Ebek, 2 = Fjej) gets compared properly. */
            if (((reg_id - 0x29) < 0x8E) || reg_id == 0x26)
                g_held_item = reg_id;
            break;
        }
        case 0x2C: {                            /* BG MASK SETUP — 1:1 with
                                                 * Ghidra case 0x2c steps 1-7:
                                                 * destroys old id=0 entity,
                                                 * loads .fld/.msk by name,
                                                 * registers as kind=1 id=0,
                                                 * links kind=3 mask entity.
                                                 *
                                                 * PORT SHORTCUT (steps 8-9):
                                                 * Original ALSO calls
                                                 * FUN_00404600(0x44E750) to
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
        case 0x2D: {                            /* LOAD_ASSET */
            const char *name = NULL;
            if (PeLoaderContainsVA(i32_at4))
                name = xlat_asset_name(i32_at4);
            else name = (const char *)(pc + 2);
            ScriptCallLoadAsset(name, reg_id);
            break;
        }
        case 0x2E: {                            /* REGISTER_MASK_E6D8 — per
                                                 * Ghidra FUN_004076e0(id, ptr,
                                                 * &DAT_0044e6d8); registers
                                                 * per-frame click hotspots
                                                 * into the "mask" verb table.
                                                 * Asset must already be
                                                 * loaded via op 0x2D. */
            uint32_t click_addr = i32_at4;
            ScriptCallRegMaskList(reg_id, click_addr, 0);
            break;
        }
        case 0x2F: {                            /* REGISTER_MASK_E700 — same
                                                 * as 0x2E but target table is
                                                 * &DAT_0044e700 (the verb-list
                                                 * mask table). */
            uint32_t click_addr = i32_at4;
            ScriptCallRegMaskList(reg_id, click_addr, 1);
            break;
        }
        case 0x30: {                            /* SPAWN_ENTITY (16 bytes) */
            /* operand layout per Ghidra dump:
             *   uVar29 = local_158[6]        // halfword @ +0xc = flags
             *   iVar20 = *(int *)(local_158 + 2)  // dword @ +4 = click_payload addr
             *   iVar3  = *(int *)(local_158 + 4)  // dword @ +8 = script bytecode addr
             *   uVar31 = a0                  // id
             */
            uint16_t flags2 = (len >= 4) ? pc[6] : 0;
            uint32_t click_addr = 0, script_addr = 0;
            if (len >= 3) memcpy(&click_addr,  ((const uint8_t *)pc) + 4, 4);
            if (len >= 4) memcpy(&script_addr, ((const uint8_t *)pc) + 8, 4);
            ScriptCallSpawnEntity(reg_id, flags2, click_addr, script_addr);
            break;
        }
        /* 1:1 with Ghidra case 0x31/0x32:
         *   case 0x31: FUN_004093E0(reg_id, '\x01'); // destroy + unreg asset
         *   case 0x32: FUN_004093E0(reg_id, '\x00'); // destroy only
         * Op naming `EnableEnt` was legacy misleading — both DESTROY; the
         * `1` arg only controls whether the kind=1 asset slot is also
         * cleared. Use the proper-named call. */
        case 0x31: { extern void ScriptCallDestroyEnt(uint16_t, int);
                     ScriptCallDestroyEnt(reg_id, 1); break; }
        case 0x32: { extern void ScriptCallDestroyEnt(uint16_t, int);
                     ScriptCallDestroyEnt(reg_id, 0); break; }

        /* ---- walk / actor positioning -------------------------------- */
        case 0x35: ScriptCallWalkMode(reg_id, 1); break;
        case 0x37: ScriptCallWalkMode(reg_id, 2); break;
        case 0x38: ScriptCallWalkTo(reg_id, a1, 1); break;
        case 0x3A: ScriptCallWalkTo(reg_id, reg_id, 2); break;
        case 0x3B: ScriptCallAttachProp(reg_id, a1, 0); break;
        case 0x3C: ScriptCallAttachProp(reg_id, a1, 1); break;
        case 0x3E: ScriptCallHideEnt(reg_id); break;
        case 0x3F: ScriptCallShowEnt(reg_id); break;
        case 0x47: {                            /* MOVE_ENTITY_DELTA —
            * 1:1 with Ghidra case 0x47 (lines 1108-1119): adds (dx,dy)
            * to the entity's draw AND raw positions.
            *
            *   e = FUN_00404c30(reg_id);   // FindEntityByVerbId
            *   if (e) {
            *     e->draw_x += local_158[2];   (+10)
            *     e->raw_x  += local_158[2];   (+0x22)
            *     e->draw_y += local_158[3];   (+0xc)
            *     e->raw_y  += local_158[3];   (+0x24)
            *   }
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
        case 0x4B: {                            /* QUERY_ENTITY_X
             * 1:1 with Ghidra case 0x4b:
             *
             *   g_return_reg = 0;
             *   e = FUN_00404c30(reg_id);
             *   if (e) {
             *       atlas = e[+0x28];
             *       g_return_reg = e[+0x0a];                      // drawn X
             *       if (atlas != 0 && (e[+0x3a] & 2) != 0)
             *           g_return_reg = e[+0x22];                  // anchor X
             *   }
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
        case 0x4C: {                            /* QUERY_ENTITY_Y — mirror of 0x4B.
             * Same +0x28 atlas test, returns +0x24 (anchor Y) or +0x0C
             * (drawn Y). 1:1 with Ghidra case 0x4c. */
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
         * (FUN_00404C30) which walks the click list. Earlier port used
         * FindUpdateRegistration(2, reg_id) — wrong lookup. */
        case 0x27: {                            /* SET_TAGGED_FIELD — 1:1
             * with Ghidra case 0x27:
             *
             *   ptr  = *(char**)(local_158+2);       // dword @ +4
             *   idx  = FindKeyInTaggedTable(ptr, '\x15', -1);
             *   if (idx != 0) {
             *     ((short*)ptr)[idx*1 + 1] = local_158[4];   // X
             *     ((short*)ptr)[idx*1 + 2] = local_158[5];   // Y
             *   }
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
        case 0x28: {                            /* SET_ENTITY_XY —
            * 1:1 with Ghidra case 0x28 (lines 855-875): writes (x, y)
            * into the entity matching `reg_id` (which after the pre-
            * dispatch alias is normally this_id for op 0x28's a0==0x28
            * special case, but can be other ids if the script uses the
            * raw form).
            *
            *   e = FUN_00404C30(reg_id);   // FindEntityByVerbId
            *   if (e) {
            *     e->draw_x = local_158[2];   (+10)
            *     e->raw_x  = local_158[2];   (+0x22)
            *     e->draw_y = local_158[3];   (+0xc)
            *     e->raw_y  = local_158[3];   (+0x24)
            *     // (+ attached-prop offset path for bit 2 of +0x3a)
            *   }
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
                /* Foot-anchor compensation — 1:1 with original case 0x28
                 * @ Ghidra line 866-873:
                 *   iVar3 = entity[+0x28];                  // atlas ptr
                 *   if (iVar3 && (entity[+0x3a] & 2)) {     // FLAG_2 = foot-anchor active
                 *       drawn_x += atlas->x_tab[frame];
                 *       drawn_y += atlas->y_tab[frame];
                 *   }
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
        case 0x24: {                            /* TAILCALL
             * 1:1 with original case 0x24 (Ghidra @ 0x00407820, line ~817):
             *   param_3 = *(ushort **)(local_158 + 2);  // base = dword@+4
             *
             * End-of-iteration: `pc = base` (not advanced) because op was
             * 0x24/0x25. No stack push. Effectively a jump replacing the
             * current script with the target function — the call frame
             * stays where it was. */
            uint32_t addr; memcpy(&addr, ((const uint8_t *)pc) + 4, 4);
            const void *xlt = xlat_binary_ptr(addr);
            if (!xlt) {
                /* Unknown binary address — would segfault. Treat as END. */
                fprintf(stderr, "[script] TAILCALL to unresolved 0x%08x — END\n", addr);
                pc = NULL;
            } else {
                base = (const uint16_t *)xlt;
                pc   = base;
            }
            advanced = 1;
            break;
        }
        case 0x25: {                            /* CALL_SUB
             * 1:1 with original case 0x25 (Ghidra @ 0x00407820, line ~820):
             *   if (bVar2 < 10) {
             *     puVar15 = *(ushort **)(local_158 + 2);  // new_base = dword@+4
             *     auStack_84[bVar2] = param_3;             // save base
             *     aiStack_ac[bVar2] = (int)local_158;      // save pc (raw)
             *     bVar2++;
             *     param_3 = puVar15;                        // base = new
             *   }
             *   // (else: param_3 unchanged, end-of-iteration sets pc = base
             *   //  → effectively re-enters the current script at offset 0)
             *
             * End-of-iteration code (line ~1382):
             *   if (op != 0x24 && op != 0x25) pc += len*4;
             *   else pc = base;                           // jump
             *
             * On END (op 0x56) in callee:
             *   pc = saved_pc + (byte[saved_pc+1] * 4)    // = saved_pc + len*4
             *   base = saved_base
             *
             * Port stores the post-call return address eagerly (saves
             * pc + len*2 ushorts = saved_pc + len*4 bytes) — equivalent.
             * On stack overflow we mirror the original: do NOT push, but
             * still update pc = base (restart current script). In practice
             * 10-deep recursion never fires for shipped scripts. */
            uint32_t addr; memcpy(&addr, ((const uint8_t *)pc) + 4, 4);
            const void *xlt = xlat_binary_ptr(addr);
            if (!xlt) {
                fprintf(stderr, "[script] CALL_SUB to unresolved 0x%08x — skip\n", addr);
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
                fprintf(stderr, "[script] CALL_SUB stack overflow (depth=%d), "
                                "restarting current script\n", call_sp);
                pc = base;
                advanced = 1;
            }
            break;
        }

        /* ---- cursor / perspective ------------------------------------ */
        case 0x40:
            g_perspective_min  = a1;
            g_perspective_step = a2;
            g_cursor_speed     = a0;   /* 1:1 — uVar31 = full halfword */
            break;

        /* ---- sound --------------------------------------------------- *
         * PORT SHORTCUT (FUN_00410D30 sound-id table): The original engine
         * maintains a sound_id → asset-filename table that maps the
         * opcode's 32-bit `id` arg to a specific WAV in Dane_02.dta. That
         * table hasn't been RE'd from the binary yet; port's
         * `s_sound_table` in stubs.c is empty, so most positional sound
         * calls only end up in the queue (consumed by stereo pan calc
         * via SoundQueueMixForListener) without playing a WAV. The
         * positional queue itself + the stereo pan computation are 1:1
         * with the original FUN_00410DA0; only the id→asset mapping is
         * stubbed. See TASKS-2 T36 polish notes. */
        case 0x41: {
            /* 1:1 with Ghidra case 0x41:
             *   FUN_00410D30(reg_id, local_158[2], *(u32)(local_158 + 4),
             *                local_158[3]);
             *
             * local_158 is ushort*. local_158[2] = a1 (ushort at byte +4).
             * `local_158 + 4` is pointer arithmetic = byte offset +8. So
             * the u32 argument is at byte +8 of the instruction, NOT +4.
             * Earlier port passed i32_at4 (= u32 at byte +4) which fed
             * the wrong dword into the sound id slot. */
            uint32_t i32_at8 = 0;
            if (len >= 3) memcpy(&i32_at8, ((const uint8_t *)pc) + 8, 4);
            ScriptCallSoundPlay(reg_id, a1, i32_at8, a2);
            break;
        }
        case 0x42: ScriptCallSoundStop(); break;
        /* RE confirmed (T16 quick win, 2026-05-27): DAT_0044E6C4 has
         * TWO writers (op 0x43 sets 0 @ 0x408d69, op 0x44 sets 0x10 @
         * 0x408d77) and ZERO readers anywhere in the binary — verified
         * via Ghidra xrefs. The flag is dead state even in the original
         * game, so our no-op port is 1:1 correct. Both ops are kept as
         * dispatch-and-bail so scripts that contain them advance their
         * PC past the operand bytes correctly. */
        case 0x43: /* CLEAR_DAT_E6C4 — dead state, no-op 1:1 */ break;
        case 0x44: /* SET_DAT_E6C4_16 — dead state, no-op 1:1 */ break;

        /* ---- timers -------------------------------------------------- */
        case 0x45: if (a0 < 16) g_timer_baselines[a0] = g_tick_counter; break;
        case 0x46: if (a0 < 16) g_return_reg = (uint16_t)(g_tick_counter - g_timer_baselines[a0]); break;

        /* ---- palette ------------------------------------------------- */
        case 0x48: ScriptCallPalLoad(a0, i32_at4, 1); break;
        case 0x49: g_return_reg = (uint16_t)ScriptCallPalFadeStep(); break;
        case 0x4A: ScriptCallPalLoad(0,  i32_at4, 0); break;

        /* ---- subanim toggles ---------------------------------------- */
        case 0x50: {                            /* SUBANIM_HIDE_TOGGLE — 1:1
             * with Ghidra case 0x50:
             *
             *   asset  = FUN_00405D80(1, id);          // kind=1 by id
             *   frame  = local_158[2];                  // arg2
             *   if (asset && frame < asset->frame_count) {
             *     mask_e = walk DAT_0044E6D8 looking for entry
             *              whose +0x16 == asset->pixel_ptrs[frame];
             *     if (mask_e) {
             *       if (local_158[3] == 0) mask_e[+0x14] &= ~0x8000; // show
             *       else                   mask_e[+0x15] |=  0x80;    // hide
             *     }
             *   }                                                          */
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
            fprintf(stderr, "[op50] id=%u asset=%s frame=%u flag=%u → %s%s\n",
                    reg_id,
                    asset ? asset->name : "(none)",
                    frame, flag,
                    flag == 0 ? "HIDE" : "SHOW",
                    matched ? "" : " (NO MASK MATCHED)");
            break;
        }
        case 0x51: {                            /* SUBANIM_SET_PARAM — 1:1
             * with Ghidra case 0x51: same asset+frame lookup as 0x50,
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
         * 1:1 with RunScriptInterpreter @ 0x00407820 case 0x52 / 0x53:
         *   case 0x52: FUN_0040c6a0(state, entity, *(char**)(pc+4),
         *                           *(byte**)(pc+8), *(u32)(pc+12));
         *   case 0x53: FUN_00409dd0(DAT_0044E44C, state, *(char**)(pc+4));
         *
         * Both opcodes read DWORD POINTERS at offsets +4, +8, +12 — they
         * are NOT inline strings (pc+2 was wrong). Resolve through xlat
         * so the dialog name lives in PE memory (Gadki.scr keys etc.). */
        case 0x52: {
            /* 1:1 with FUN_0040C6A0 @ 0x0040C6A0 (DialogPush):
             *   FUN_0040c6a0(stack, entity, *(char**)(pc+4),
             *                *(byte**)(pc+8), *(u32)(pc+12));
             *
             * Args:
             *   pc+4  = char *dialog_name      → load asset, push to stack
             *   pc+8  = byte *opts_bytes       → horner-hashed into slot[+4]
             *   pc+12 = u32 talk_anim_va       → PE VA of mouth-cycle bytecode
             *                                     stored at slot[+0x0C]; bound
             *                                     to entity[+0x2C] when
             *                                     FUN_0040C570 ACTIVATE fires
             *
             * T108 fix — earlier port read pc+12 as `opts_cnt` (uint count)
             * which was a wrong-type label. Real meaning per Ghidra
             * FUN_0040C6A0 signature: it's a `param_4` undefined4 — actually
             * a PE VA pointing to talking-head animation bytecode. We now
             * resolve it via xlat_binary_ptr and thread it through
             * ScriptCallDialogBegin → DialogStackPush → slot.talk_anim_va,
             * then DialogActivateTopSpeaker binds it to the speaker
             * entity's bytecode pointer (instead of T20c's manual frame
             * 0↔1 toggle which was a port shortcut). */
            uint32_t opts_va = 0, talk_anim_va = 0;
            if (len >= 3) memcpy(&opts_va,      ((const uint8_t *)pc) + 8,  4);
            if (len >= 4) memcpy(&talk_anim_va, ((const uint8_t *)pc) + 12, 4);
            const char    *name = (const char *)xlat_binary_ptr(i32_at4);
            const uint8_t *opts = (const uint8_t *)xlat_binary_ptr(opts_va);
            ScriptCallDialogBegin(reg_id, name, opts, talk_anim_va);
            break;
        }
        case 0x53: {
            const char *result_key = (const char *)xlat_binary_ptr(i32_at4);
            ScriptCallDialogEnd(result_key);
            break;
        }
        case 0x54: {                            /* SHOW_PICTURE — 1:1 with
             * Ghidra case 0x54: FUN_00405500(*(char**)(local_158 + 2)).
             *
             *   FUN_00405500(name):
             *     LoadFileFromDta(name, &raw);
             *     pic = FUN_004053C0(w, h, raw+0x308, 0, 1);    // PIC builder
             *     local_8 = 0xFFFF;
             *     while (DAT_0044E5AC == 0 && uVar2 != 0) {
             *       FUN_00404110();             // pump events
             *       FUN_004024D0();             // mm-tick
             *       uVar2 -= DAT_0044E578;       // decrement by frame_delta
             *     }
             *     DAT_0044E5AC = 0;
             *     FreeAsset(pic);
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
                        extern void     PlatformPumpEvents(void);
                        extern int      PlatformShouldQuit(void);
                        uint32_t budget_ms = 0xFFFF * 16;   /* ~17 minutes */
                        while (budget_ms > 0) {
                            PlatformPumpEvents();
                            if (PlatformShouldQuit()) break;
                            if (g_lmb_clicked) {
                                g_lmb_clicked = 0;
                                break;
                            }
                            SDL_Delay(33);  /* T-anim-speed: match main loop pacing */
                            budget_ms -= 16;
                        }
                    }
                    xfree(raw);
                } else {
                    fprintf(stderr, "[script] op 0x54 show-picture '%s' not found\n", name);
                }
            }
            break;
        }

        /* ---- abort / end -------------------------------------------- */
        case 0x4F: result = 0; break;
        case 0x55:                              /* END_HARD */
        case 0x56:                              /* END (implicit) */
            pc = NULL; advanced = 1;
            break;

        /* ---- debug log --------------------------------------------- *
         * PORT SHORTCUT (FUN_004158A0): Original appends log line to
         * __tmp.txt on disk; port writes to stderr instead (no
         * filesystem spam during development). Behavioural delta is
         * cosmetic — debug-only path. */
        case 0x57:
            fprintf(stderr, "[script] log: %.*s\n", 64,
                    (const char *)(pc + 2));
            break;

        /* ---- implicit no-op markers (have no case in original) ------ */
        case 0x06:   /* ENDIF — consumed by 0x00..0x05/0x07 scan */
        case 0x08:
        case 0x16:   /* LABEL — consumed by find_label */
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
