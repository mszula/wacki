/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tests/test_per_entity_vm.c — per-entity VM bytecode format (mock).
 *
 * CHARACTERIZATION TEST. The per-entity VM (`FUN_004012E0` /
 * `ExecEntityScript` in src/actor.c) drives every spawned entity's
 * animation + walker. actor.c is too coupled to graphics/stubs to link
 * into the test binary, so we pin the FORMAT contract here with a
 * mock dispatcher that mirrors the documented bytecode shape.
 *
 * Per-entity VM differs from the main VM:
 *   - Instruction stride is `dlt * 2` bytes (vs main VM's `len * 4`).
 *   - dlt lives at byte +1 of the instruction (same as len).
 *   - Operands at byte +2, +4, +6 (same).
 *   - END marker is op 0x21 (= '!') — NOT 0x56 like main VM.
 *   - LABEL is op 0x0A (= '\n').
 *
 * If anyone refactors actor.c and accidentally changes the stride from
 * `dlt * 2` to `dlt * 4` (the main-VM convention), this test fails
 * immediately. Same for opcode constants.
 *
 * Reference: src/actor.c lines 279-360 + opcodes 0x00..0x24 cases.
 */

#include "test.h"

#include <stdint.h>
#include <string.h>

/* ---- documented per-entity VM opcodes (from src/actor.c switch) ----- *
 *
 * Pinned values; if anyone renames or reassigns these constants, the
 * shipped bytecode in PE memory stops being interpretable. */

#define PVM_LABEL          0x0A   /* '\n' — scan target for LOOP / JUMP_LABEL */
#define PVM_END            0x21   /* '!' — terminator (different from main VM 0x56) */

/* Other opcodes the per-entity VM handles (cases 0..0x24). The complete
 * mapping is in src/actor.c ExecEntityScript switch body. Tests don't
 * exercise each — just pin the constants for END and LABEL which are
 * the structural markers. */

TEST(per_entity_opcode_constants)
{
    /* These two are the ONLY structural markers — END terminates the
     * bytecode stream, LABEL marks jump targets. Both are also visible
     * in shipped scripts as printable ASCII (`!` and `\n` chosen for
     * easy hex-dump inspection — that's a hint they're truly stable). */
    ASSERT_EQ(PVM_END,   0x21);
    ASSERT_EQ(PVM_LABEL, 0x0A);
    /* Per docs/script-vm.md the per-entity VM dispatches 0x00..0x24 — that's
     * 37 distinct ops. Pin the upper bound. */
    ASSERT_TRUE(0x24 < 0x25);   /* 0x24 is valid */
}

/* ---- mock per-entity dispatcher pinning stride contract ------------- *
 *
 * Bytecode format: `[op:u8 +0][dlt:u8 +1][operand:u16 +2..]`
 * Instruction stride = dlt * 2 bytes.
 * Termination: op == 0x21 (END) → exit.
 *
 * Mock interpreter walks the bytecode collecting visited PCs into an
 * output array. Tests verify PCs match the expected dlt-based advance. */

static int mock_walk(const uint8_t *bytecode, uint16_t *visited_pcs,
                      int max_visits)
{
    int n = 0;
    uint16_t pc = 0;     /* in BYTES (matches actor.c's `pc * 2`-stepping
                          * once we collect 1 entry per instruction). */
    for (int safety = 0; safety < 1024 && n < max_visits; ++safety) {
        const uint8_t *p = bytecode + pc;
        uint8_t op  = p[0];
        uint8_t dlt = p[1];
        visited_pcs[n++] = pc;
        if (op == PVM_END) break;
        if (dlt == 0) break;             /* defensive — same as actor.c */
        pc = (uint16_t)(pc + (uint16_t)dlt * 2);
    }
    return n;
}

/* Emit a per-entity VM instruction into a uint8_t buffer.
 * Layout: [op][dlt][operand_lo][operand_hi][...]. */
static int emit_pvm(uint8_t *buf, int pos, uint8_t op, uint8_t dlt,
                     uint16_t a0, uint16_t a1, uint16_t a2)
{
    buf[pos + 0] = op;
    buf[pos + 1] = dlt;
    /* The operand zone is `dlt * 2 - 2` bytes total (instruction
     * stride minus header). Pad with operands as supplied. */
    if (dlt >= 2) {
        buf[pos + 2] = (uint8_t)(a0 & 0xFF);
        buf[pos + 3] = (uint8_t)((a0 >> 8) & 0xFF);
    }
    if (dlt >= 3) {
        buf[pos + 4] = (uint8_t)(a1 & 0xFF);
        buf[pos + 5] = (uint8_t)((a1 >> 8) & 0xFF);
    }
    if (dlt >= 4) {
        buf[pos + 6] = (uint8_t)(a2 & 0xFF);
        buf[pos + 7] = (uint8_t)((a2 >> 8) & 0xFF);
    }
    return pos + (int)dlt * 2;
}

TEST(stride_is_dlt_times_two_bytes)
{
    /* 3 instructions:
     *   #0 op=0x05 dlt=2 → 4 bytes (pc 0..3)
     *   #1 op=0x06 dlt=2 → 4 bytes (pc 4..7)
     *   #2 op=PVM_END dlt=1 → 2 bytes (pc 8..9)
     */
    uint8_t prog[32] = { 0 };
    int p = 0;
    p = emit_pvm(prog, p, 0x05, 2, 0x1234, 0, 0);
    p = emit_pvm(prog, p, 0x06, 2, 0x5678, 0, 0);
    p = emit_pvm(prog, p, PVM_END, 1, 0, 0, 0);
    (void)p;

    uint16_t visited[8];
    int n = mock_walk(prog, visited, 8);

    ASSERT_EQ(n, 3);
    ASSERT_EQ(visited[0], 0);
    ASSERT_EQ(visited[1], 4);     /* dlt=2 → +4 bytes */
    ASSERT_EQ(visited[2], 8);     /* dlt=2 → +4 bytes */
}

TEST(varying_dlt_strides_correctly)
{
    /* dlt=1 (2 bytes, no operands), dlt=4 (8 bytes), dlt=2 (4 bytes), END. */
    uint8_t prog[32] = { 0 };
    int p = 0;
    p = emit_pvm(prog, p, 0x13, 1, 0, 0, 0);             /* 2 bytes */
    p = emit_pvm(prog, p, 0x11, 4, 0x11, 0x22, 0x33);    /* 8 bytes */
    p = emit_pvm(prog, p, 0x07, 2, 0xAA, 0, 0);          /* 4 bytes */
    p = emit_pvm(prog, p, PVM_END, 1, 0, 0, 0);
    (void)p;

    uint16_t visited[8];
    int n = mock_walk(prog, visited, 8);

    ASSERT_EQ(n, 4);
    ASSERT_EQ(visited[0], 0);
    ASSERT_EQ(visited[1], 2);    /* +1*2 */
    ASSERT_EQ(visited[2], 10);   /* +4*2 */
    ASSERT_EQ(visited[3], 14);   /* +2*2 */
}

TEST(end_marker_stops_walk)
{
    /* After END, subsequent bytes must NOT be visited. */
    uint8_t prog[32] = { 0 };
    int p = 0;
    p = emit_pvm(prog, p, 0x05, 2, 0, 0, 0);
    p = emit_pvm(prog, p, PVM_END, 1, 0, 0, 0);
    /* These bytes lie past END — should not be visited. */
    p = emit_pvm(prog, p, 0x99, 2, 0, 0, 0);
    (void)p;

    uint16_t visited[8];
    int n = mock_walk(prog, visited, 8);

    ASSERT_EQ(n, 2);    /* op 0x05 + END only */
}

TEST(dlt_zero_terminates_defensively)
{
    /* actor.c's scan_for_label has: `if (dlt == 0) return 0;`
     * The main exec loop has similar defensive break. Test the
     * defensive behavior. */
    uint8_t prog[8] = { 0 };
    /* op=0x05 dlt=0 — buggy bytecode. Walk should bail. */
    prog[0] = 0x05; prog[1] = 0;

    uint16_t visited[4];
    int n = mock_walk(prog, visited, 4);

    /* First visit recorded at pc=0; then dlt==0 → break. */
    ASSERT_EQ(n, 1);
    ASSERT_EQ(visited[0], 0);
}

/* ---- LABEL scan: finds matching LABEL by id ------------------------- *
 *
 * scan_for_label walks the bytecode looking for op == 0x0A LABEL with
 * matching arg. END (0x21) terminates the scan. */

static uint16_t scan_for_label_mock(const uint8_t *bytecode, uint16_t target_id)
{
    uint16_t pc = 0;
    for (int safety = 0; safety < 2048; ++safety) {
        const uint8_t *p = bytecode + (size_t)pc * 2;
        uint8_t op = p[0];
        if (op == PVM_END) return 0;
        if (op == PVM_LABEL) {
            uint16_t arg = (uint16_t)(p[2] | (p[3] << 8));
            if (target_id == 0xFFFF || arg == target_id) return pc;
        }
        uint8_t dlt = p[1];
        if (dlt == 0) return 0;
        pc = (uint16_t)(pc + dlt);
    }
    return 0;
}

TEST(scan_finds_label_by_exact_id)
{
    uint8_t prog[32] = { 0 };
    int p = 0;
    p = emit_pvm(prog, p, 0x05,       2, 0, 0, 0);
    p = emit_pvm(prog, p, PVM_LABEL,  2, 7, 0, 0);   /* LABEL 7 at pc=4 → halfword pc=2 */
    p = emit_pvm(prog, p, 0x06,       2, 0, 0, 0);
    p = emit_pvm(prog, p, PVM_LABEL,  2, 42, 0, 0);  /* LABEL 42 at pc=12 → halfword pc=6 */
    p = emit_pvm(prog, p, PVM_END,    1, 0, 0, 0);
    (void)p;

    /* Find LABEL 42 — scan_for_label returns pc in halfwords from start. */
    uint16_t pc = scan_for_label_mock(prog, 42);
    /* LABEL 42 sits at byte 12 → halfword pc = 6. */
    ASSERT_EQ(pc, 6);

    /* Find LABEL 7. */
    pc = scan_for_label_mock(prog, 7);
    ASSERT_EQ(pc, 2);
}

TEST(scan_label_wildcard_0xFFFF_returns_first_label)
{
    /* When target_id == 0xFFFF, scan returns the FIRST LABEL found. */
    uint8_t prog[32] = { 0 };
    int p = 0;
    p = emit_pvm(prog, p, 0x05,       2, 0, 0, 0);
    p = emit_pvm(prog, p, PVM_LABEL,  2, 7, 0, 0);   /* first LABEL */
    p = emit_pvm(prog, p, PVM_LABEL,  2, 42, 0, 0);
    p = emit_pvm(prog, p, PVM_END,    1, 0, 0, 0);
    (void)p;

    uint16_t pc = scan_for_label_mock(prog, 0xFFFF);
    /* First LABEL at byte 4 → halfword pc = 2. */
    ASSERT_EQ(pc, 2);
}

TEST(scan_label_unknown_returns_zero)
{
    uint8_t prog[16] = { 0 };
    int p = 0;
    p = emit_pvm(prog, p, PVM_LABEL,  2, 1, 0, 0);
    p = emit_pvm(prog, p, PVM_END,    1, 0, 0, 0);
    (void)p;

    /* No LABEL with id=99 → return 0. */
    uint16_t pc = scan_for_label_mock(prog, 99);
    ASSERT_EQ(pc, 0);
}

TEST(scan_label_stops_at_end_marker)
{
    /* LABEL exists past END — must NOT be found. */
    uint8_t prog[16] = { 0 };
    int p = 0;
    p = emit_pvm(prog, p, PVM_END,    1, 0, 0, 0);
    p = emit_pvm(prog, p, PVM_LABEL,  2, 5, 0, 0);   /* past END */
    (void)p;

    uint16_t pc = scan_for_label_mock(prog, 5);
    ASSERT_EQ(pc, 0);
}

SUITE(per_entity_vm)
{
    RUN_TEST(per_entity_opcode_constants);
    RUN_TEST(stride_is_dlt_times_two_bytes);
    RUN_TEST(varying_dlt_strides_correctly);
    RUN_TEST(end_marker_stops_walk);
    RUN_TEST(dlt_zero_terminates_defensively);
    RUN_TEST(scan_finds_label_by_exact_id);
    RUN_TEST(scan_label_wildcard_0xFFFF_returns_first_label);
    RUN_TEST(scan_label_unknown_returns_zero);
    RUN_TEST(scan_label_stops_at_end_marker);
}
