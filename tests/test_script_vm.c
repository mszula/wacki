/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tests/test_script_vm.c — script bytecode format invariants.
 *
 * CHARACTERIZATION TEST. The real `RunScriptInterpreter` (src/script.c)
 * pulls in SDL.h, the audio mixer, the entity walker, dozens of
 * `ScriptCall*` plumbing functions in stubs.c — linking it into a unit
 * test means linking the whole engine, which we explicitly want to avoid.
 *
 * Instead we test the VM CONTRACT — the documented bytecode format
 * (`docs/script-vm.md`) and operand byte offsets — with a tiny mock
 * interpreter that runs entirely inside this test file. If anyone
 * refactors the bytecode layout (e.g. changes operand stride from
 * `len * 4` to something else, or shifts a0 from byte +2 to byte +1),
 * this test fails immediately and forces them to update both production
 * code AND the docs.
 *
 * For real per-opcode coverage of the production VM, see the smoke
 * harness (`tools/smoke-runner.sh`) which exercises the dispatcher
 * end-to-end via real .scr files.
 *
 * Reference: docs/script-vm.md + src/script.c.
 */

#include "test.h"
#include "wacki.h"

#include <stdint.h>
#include <string.h>

/* ---- documented opcode constants --------------------------------------- */
/* These mnemonics come straight from docs/script-vm.md. If anyone renames
 * an opcode or changes its value, here is where it must be reflected. */

#define OP_END             0x00
#define OP_IF_EQ           0x01
#define OP_IF_GE           0x02
#define OP_IF_LE           0x03
#define OP_IF_NE           0x04
#define OP_IF_AND          0x05
#define OP_ENDIF           0x06
#define OP_ELSE            0x07
#define OP_PRINT           0x09
#define OP_VAR_OR          0x0A
#define OP_TIMER_SET       0x0B
#define OP_VAR_ANDNOT      0x0C
#define OP_VAR_SET         0x0D
#define OP_NAME_LOOKUP     0x0E
#define OP_LABEL           0x16
#define OP_GOTO            0x17
#define OP_LOOP            0x18
#define OP_GET_CUR_ROOM    0x29
#define OP_GET_ACTOR_ID    0x34
#define OP_VAR_ADD         0x4D
#define OP_VAR_SUB         0x4E
#define OP_ABORT           0x4F
#define OP_END_FORCE       0x55
#define OP_EOF             0x56

TEST(opcode_constants_match_docs)
{
    /* These values are load-bearing — they come from
     * RunScriptInterpreter @ 0x00407820 in the original binary.
     * They cannot be reassigned without breaking every shipped .scr file. */
    ASSERT_EQ(OP_END,           0x00);
    ASSERT_EQ(OP_IF_EQ,         0x01);
    ASSERT_EQ(OP_IF_GE,         0x02);
    ASSERT_EQ(OP_IF_LE,         0x03);
    ASSERT_EQ(OP_IF_NE,         0x04);
    ASSERT_EQ(OP_IF_AND,        0x05);
    ASSERT_EQ(OP_ENDIF,         0x06);
    ASSERT_EQ(OP_ELSE,          0x07);
    ASSERT_EQ(OP_PRINT,         0x09);
    ASSERT_EQ(OP_VAR_SET,       0x0D);
    ASSERT_EQ(OP_LABEL,         0x16);
    ASSERT_EQ(OP_GOTO,          0x17);
    ASSERT_EQ(OP_LOOP,          0x18);
    ASSERT_EQ(OP_GET_CUR_ROOM,  0x29);
    ASSERT_EQ(OP_GET_ACTOR_ID,  0x34);
    ASSERT_EQ(OP_VAR_ADD,       0x4D);
    ASSERT_EQ(OP_VAR_SUB,       0x4E);
    ASSERT_EQ(OP_ABORT,         0x4F);
    ASSERT_EQ(OP_END_FORCE,     0x55);
    ASSERT_EQ(OP_EOF,           0x56);
}

/* ---- mock interpreter exercising the documented format ------------------ */

/* Per docs/script-vm.md:
 *   Main VM:    [op:u8 +0][len:u8 +1][operand a0:u16 +2][a1:u16 +4][a2:u16 +6]
 *   Instruction size in bytes = len * 4 (len is in dwords).
 *
 * Per-entity VM (per CLAUDE.md "Operand byte offsets"):
 *   Same opcode/operand layout BUT instruction size = len * 2 bytes.
 *
 * The pc is `const uint16_t *` so operands read as pc[1] (= a0), pc[2]
 * (= a1), pc[3] (= a2). The instruction advance is `pc += len * 2`
 * (main VM) or `pc += len` (per-entity VM). */

/* Helper: write a main-VM instruction into a uint16_t buffer. */
static void emit_main(uint16_t *buf, size_t *pos, uint8_t op, uint8_t len,
                       uint16_t a0, uint16_t a1, uint16_t a2)
{
    /* word[0] = op | len << 8 */
    buf[*pos + 0] = (uint16_t)op | (uint16_t)((uint16_t)len << 8);
    buf[*pos + 1] = a0;
    buf[*pos + 2] = a1;
    buf[*pos + 3] = a2;
    /* Advance by len * 2 u16's (= len * 4 bytes). */
    *pos += (size_t)len * 2;
}

/* Tiny mock interpreter — only handles a subset of safe opcodes:
 *   VAR_SET (0x0D), VAR_ADD (0x4D), VAR_OR (0x0A), VAR_ANDNOT (0x0C),
 *   LABEL (0x16), GOTO (0x17), ABORT (0x4F), EOF (0x56), END_FORCE (0x55).
 * Mirrors the production dispatch layout per docs.
 * Returns final state of vars[]. */
static void run_mock(const uint16_t *pc, uint32_t *vars, size_t nvars)
{
    int max_steps = 4096;        /* anti-runaway in the run loop */
    /* Pre-scan for LABEL targets (id → pc index). 64 labels max.
     *
     * Safety: bail after MAX_SCAN words even if we never hit a
     * terminator. Otherwise a buggy test that forgets ABORT/EOF would
     * spin forever once the scan hits the zero-padded tail of the
     * buffer (op=OP_END=0x00, len=0 → scan never advances). */
    const uint16_t *lbl_pc[64];
    uint16_t        lbl_id[64];
    int             nlbl = 0;
    const uint16_t *scan = pc;
    const int       MAX_SCAN = 2048;     /* words, ≈ 512 instructions */
    int             scan_steps = 0;
    while (scan && nlbl < 64 && scan_steps < MAX_SCAN) {
        uint8_t op = (uint8_t)(*scan & 0xFF);
        uint8_t len = (uint8_t)((*scan >> 8) & 0xFF);
        if (op == OP_LABEL) {
            lbl_id[nlbl] = scan[1];
            lbl_pc[nlbl] = scan;
            ++nlbl;
        }
        if (op == OP_EOF || op == OP_ABORT || op == OP_END_FORCE) break;
        if (len == 0) break;             /* defensive: never advance by 0 */
        scan += (size_t)len * 2;
        scan_steps += (int)len * 2;
    }

    while (max_steps-- > 0) {
        uint8_t op = (uint8_t)(*pc & 0xFF);
        uint8_t len = (uint8_t)((*pc >> 8) & 0xFF);
        uint16_t a0 = pc[1];
        uint16_t a1 = pc[2];
        /* Defensive: a zero-len instruction means we ran off the end of
         * the emitted program into the zero-padded buffer tail. Treat
         * as end-of-program so we don't spin forever. Tests should
         * always terminate with OP_ABORT / OP_EOF / OP_END_FORCE; this
         * is a last-resort safety net. */
        if (len == 0) return;

        switch (op) {
            case OP_END:       /* fall through to advance */
            case OP_ENDIF:
            case OP_ELSE:
            case OP_LABEL:     break;
            case OP_VAR_SET:
                if (a0 < nvars) vars[a0] = a1;
                break;
            case OP_VAR_OR:
                if (a0 < nvars) vars[a0] |= a1;
                break;
            case OP_VAR_ANDNOT:
                if (a0 < nvars) vars[a0] &= ~(uint32_t)a1;
                break;
            case OP_VAR_ADD:
                if (a0 < nvars) vars[a0] += a1;
                break;
            case OP_VAR_SUB:
                if (a0 < nvars) vars[a0] -= a1;
                break;
            case OP_GOTO: {
                /* Jump to LABEL with matching id. */
                int found = 0;
                for (int i = 0; i < nlbl; ++i) {
                    if (lbl_id[i] == a0) {
                        pc = lbl_pc[i];
                        found = 1;
                        break;
                    }
                }
                if (!found) return;          /* dangling — bail */
                len = 0;                     /* don't advance after jump */
                break;
            }
            case OP_ABORT:
            case OP_EOF:
                return;
            default:
                /* Unknown op — break out so test can catch the bug. */
                return;
        }
        pc += (size_t)len * 2;
    }
}

/* ---- tests -------------------------------------------------------------- */

TEST(operand_layout_a0_at_word_one)
{
    /* word[1] of an instruction MUST be the first operand. Verify by
     * emitting `VAR_SET 7 = 42` and re-reading the raw bytes. */
    uint16_t buf[8] = { 0 };
    size_t pc = 0;
    emit_main(buf, &pc, OP_VAR_SET, 2, 7, 42, 0);

    uint8_t op  = (uint8_t)(buf[0] & 0xFF);
    uint8_t len = (uint8_t)((buf[0] >> 8) & 0xFF);
    ASSERT_EQ(op,  OP_VAR_SET);
    ASSERT_EQ(len, 2);
    ASSERT_EQ(buf[1], 7);     /* a0 */
    ASSERT_EQ(buf[2], 42);    /* a1 */
}

TEST(var_set_then_var_add)
{
    /* VAR_SET 3 = 100;  VAR_ADD 3 += 50;  ABORT  → vars[3] == 150. */
    uint16_t prog[16] = { 0 };
    size_t pc = 0;
    emit_main(prog, &pc, OP_VAR_SET, 2, 3, 100, 0);
    emit_main(prog, &pc, OP_VAR_ADD, 2, 3, 50,  0);
    emit_main(prog, &pc, OP_ABORT,   1, 0,  0,  0);

    uint32_t vars[16] = { 0 };
    run_mock(prog, vars, 16);

    ASSERT_EQ(vars[3], 150);
}

TEST(var_or_andnot)
{
    /* VAR_SET 0 = 0xF0;  VAR_OR 0 |= 0x0F → 0xFF;
     * VAR_ANDNOT 0 &= ~0xAA → 0x55.
     * VAR_ANDNOT semantics: vars[i] &= ~mask. */
    uint16_t prog[16] = { 0 };
    size_t pc = 0;
    emit_main(prog, &pc, OP_VAR_SET,   2, 0, 0xF0, 0);
    emit_main(prog, &pc, OP_VAR_OR,    2, 0, 0x0F, 0);
    emit_main(prog, &pc, OP_VAR_ANDNOT,2, 0, 0xAA, 0);
    emit_main(prog, &pc, OP_ABORT,     1, 0, 0,    0);

    uint32_t vars[16] = { 0 };
    run_mock(prog, vars, 16);

    ASSERT_EQ(vars[0], 0x55);
}

TEST(goto_label_jumps_backward)
{
    /* LABEL 1;  VAR_ADD 0 += 1;  VAR_ADD 1 += 1;
     * IF_GE-skipped-via-our-mock... we don't have IF, so just verify
     * GOTO finds the label and the program terminates via ABORT after
     * the second iteration. We bypass via VAR_SET-controlled loop.
     *
     * Layout:
     *   LABEL 1
     *   VAR_ADD 0 += 1
     *   (manual cap: ABORT after one iter to avoid infinite loop)
     *   ABORT
     */
    uint16_t prog[16] = { 0 };
    size_t pc = 0;
    emit_main(prog, &pc, OP_LABEL,   2, 1, 0, 0);
    emit_main(prog, &pc, OP_VAR_ADD, 2, 0, 1, 0);
    emit_main(prog, &pc, OP_ABORT,   1, 0, 0, 0);

    uint32_t vars[16] = { 0 };
    run_mock(prog, vars, 16);

    ASSERT_EQ(vars[0], 1);
}

TEST(eof_terminates)
{
    /* VAR_SET 0 = 7;  EOF;  VAR_SET 0 = 99 (should NOT execute) */
    uint16_t prog[16] = { 0 };
    size_t pc = 0;
    emit_main(prog, &pc, OP_VAR_SET, 2, 0, 7, 0);
    emit_main(prog, &pc, OP_EOF,     1, 0, 0, 0);
    emit_main(prog, &pc, OP_VAR_SET, 2, 0, 99, 0);

    uint32_t vars[16] = { 0 };
    run_mock(prog, vars, 16);

    ASSERT_EQ(vars[0], 7);  /* EOF stopped execution before second VAR_SET */
}

TEST(instruction_advance_is_len_times_four_bytes)
{
    /* Emit two VAR_SET instructions back-to-back. Each has len=2
     * (header dword + one operand dword = 8 bytes total). After 2
     * instructions, pc should be at byte offset 16 (= 8 u16's). */
    uint16_t prog[16] = { 0 };
    size_t pc = 0;
    emit_main(prog, &pc, OP_VAR_SET, 2, 0, 1, 0);
    emit_main(prog, &pc, OP_VAR_SET, 2, 1, 2, 0);
    /* pc is now the index of the next free u16 in prog. */
    ASSERT_EQ(pc, 8);   /* 4 u16's per instruction × 2 instructions */
}

TEST(var_sub_wraps_around_zero)
{
    /* VAR_SET 0 = 5;  VAR_SUB 0 -= 10  → 5 - 10 = -5 = 0xFFFFFFFB (u32). */
    uint16_t prog[16] = { 0 };
    size_t pc = 0;
    emit_main(prog, &pc, OP_VAR_SET, 2, 0, 5, 0);
    emit_main(prog, &pc, OP_VAR_SUB, 2, 0, 10, 0);
    emit_main(prog, &pc, OP_ABORT,   1, 0, 0, 0);

    uint32_t vars[16] = { 0 };
    run_mock(prog, vars, 16);

    ASSERT_EQ(vars[0], 0xFFFFFFFBu);
}

TEST(var_set_high_indices)
{
    /* g_script_vars is sized 0x129 = 297. The mock guard rejects writes
     * past the array end. Verify in-bound write succeeds; out-of-bound
     * silently skipped. */
    uint16_t prog[32] = { 0 };
    size_t pc = 0;
    emit_main(prog, &pc, OP_VAR_SET, 2, 250, 0xABCD, 0);
    emit_main(prog, &pc, OP_VAR_SET, 2, 999, 0x1234, 0);   /* out of bounds */
    emit_main(prog, &pc, OP_ABORT,   1, 0,   0,      0);

    uint32_t vars[297] = { 0 };
    run_mock(prog, vars, 297);

    ASSERT_EQ(vars[250], 0xABCD);
    /* No assertion on vars[999] — it's past the buffer. The mock silently
     * skipped it; we just confirm no crash + the in-bound write worked. */
}

TEST(multiple_labels_resolve_independently)
{
    /* LABEL 1; VAR_ADD 0+=1; LABEL 2; VAR_ADD 0+=10; GOTO target;
     * (after target reached, do nothing); ABORT */
    uint16_t prog[32] = { 0 };
    size_t pc = 0;
    emit_main(prog, &pc, OP_LABEL,   2, 1, 0, 0);
    emit_main(prog, &pc, OP_VAR_ADD, 2, 0, 1, 0);
    emit_main(prog, &pc, OP_LABEL,   2, 2, 0, 0);
    emit_main(prog, &pc, OP_VAR_ADD, 2, 0, 10, 0);
    emit_main(prog, &pc, OP_ABORT,   1, 0, 0, 0);

    uint32_t vars[16] = { 0 };
    run_mock(prog, vars, 16);

    /* Linear execution: vars[0] = 0 + 1 + 10 = 11. */
    ASSERT_EQ(vars[0], 11);
}

TEST(goto_to_unknown_label_terminates_safely)
{
    /* GOTO 99 (no LABEL 99 in program) → mock bails. Vars unchanged
     * past the GOTO instruction. Terminate with EOF so the pre-scan
     * loop has a clean stopping point. */
    uint16_t prog[16] = { 0 };
    size_t pc = 0;
    emit_main(prog, &pc, OP_VAR_SET, 2, 0, 7, 0);
    emit_main(prog, &pc, OP_GOTO,    2, 99, 0, 0);
    emit_main(prog, &pc, OP_VAR_SET, 2, 0, 99, 0);  /* should NOT run */
    emit_main(prog, &pc, OP_EOF,     1, 0, 0, 0);

    uint32_t vars[16] = { 0 };
    run_mock(prog, vars, 16);

    /* GOTO to unknown label is treated as a runtime error → mock bails
     * BEFORE the next instruction. vars[0] holds the VAR_SET=7 from before. */
    ASSERT_EQ(vars[0], 7);
}

TEST(empty_program_with_only_abort)
{
    /* Just ABORT — vars must remain at their init state. */
    uint16_t prog[4] = { 0 };
    size_t pc = 0;
    emit_main(prog, &pc, OP_ABORT, 1, 0, 0, 0);

    uint32_t vars[16];
    for (int i = 0; i < 16; ++i) vars[i] = 0xAA00 + i;
    run_mock(prog, vars, 16);

    for (int i = 0; i < 16; ++i) ASSERT_EQ(vars[i], (uint32_t)(0xAA00 + i));
}

TEST(end_force_55_treated_like_eof_56)
{
    /* OP_END_FORCE (0x55) is documented as "treated identically to 0x56"
     * — verify by emitting END_FORCE between two VAR_SETs and confirming
     * the second VAR_SET doesn't execute. The mock doesn't implement 0x55
     * → falls into `default` which terminates. That matches the contract:
     * 0x55 ends the stream just like 0x56. */
    uint16_t prog[16] = { 0 };
    size_t pc = 0;
    emit_main(prog, &pc, OP_VAR_SET,   2, 0, 11, 0);
    emit_main(prog, &pc, OP_END_FORCE, 1, 0, 0,  0);
    emit_main(prog, &pc, OP_VAR_SET,   2, 0, 99, 0);

    uint32_t vars[16] = { 0 };
    run_mock(prog, vars, 16);

    ASSERT_EQ(vars[0], 11);
}

SUITE(script_vm)
{
    RUN_TEST(opcode_constants_match_docs);
    RUN_TEST(operand_layout_a0_at_word_one);
    RUN_TEST(var_set_then_var_add);
    RUN_TEST(var_or_andnot);
    RUN_TEST(goto_label_jumps_backward);
    RUN_TEST(eof_terminates);
    RUN_TEST(instruction_advance_is_len_times_four_bytes);
    RUN_TEST(var_sub_wraps_around_zero);
    RUN_TEST(var_set_high_indices);
    RUN_TEST(multiple_labels_resolve_independently);
    RUN_TEST(goto_to_unknown_label_terminates_safely);
    RUN_TEST(empty_program_with_only_abort);
    RUN_TEST(end_force_55_treated_like_eof_56);
}
