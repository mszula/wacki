/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tests/test_real_vm.c — production RunScriptInterpreter.
 *
 * Unlike test_script_vm.c (mock interpreter for characterization),
 * THIS file links the REAL `RunScriptInterpreter` from src/script.c.
 * Hand-crafted bytecode is fed through the production dispatcher and
 * post-state of g_script_vars / g_return_reg is verified against the
 * expected contract.
 *
 * Why both files exist:
 *   - `test_script_vm` characterizes the bytecode FORMAT (operand
 *     layout, opcode constants, mock dispatch). Survives any refactor.
 *   - `test_real_vm` exercises the actual production VM with
 *     ALL its quirks (this/that swap for a0=0x27/0x28, 1:1 Ghidra
 *     branch semantics, edge cases around op 0x55/0x56 termination).
 *     Catches regressions in `src/script.c` itself.
 *
 * Reference: src/script.c RunScriptInterpreter @ 0x00407820.
 */

#include "test.h"
#include "wacki.h"
#include "test_engine_stubs.h"

#include <stdint.h>
#include <string.h>

/* RunScriptInterpreter prototype. */
extern int RunScriptInterpreter(uint16_t this_id, uint16_t that_id, uint8_t *bytecode);
extern uint32_t g_script_vars[0x129];
extern uint16_t g_active_actor;
/* g_return_reg = (uint16_t)&g_script_vars[4]. Access via macro. */
#define RETURN_REG (*(uint16_t *)&g_script_vars[4])

/* ---- bytecode emitter (matches src/script.c format) -------------------- *
 *
 * Each instruction = `[op:u8 +0][len:u8 +1][operands]`, where len is
 * in DWORDS. Total instruction size = len * 4 bytes.
 *
 *   word[0]  = op | (len << 8)
 *   word[1]  = a0  (first u16 operand)
 *   word[2]  = a1  (low u16 of operand dword 1; also lo16 of i32_at4)
 *   word[3]  = a2  (high u16 of operand dword 1; also hi16 of i32_at4)
 *
 * Helpers below produce bytecode in a uint16_t buffer caller owns. */

static size_t emit(uint16_t *buf, size_t pos, uint8_t op, uint8_t len,
                    uint16_t a0, uint16_t a1, uint16_t a2)
{
    buf[pos + 0] = (uint16_t)op | (uint16_t)((uint16_t)len << 8);
    buf[pos + 1] = a0;
    if (len >= 2) {
        buf[pos + 2] = a1;
        buf[pos + 3] = a2;
    }
    return pos + (size_t)len * 2;
}

/* For ops that read a 32-bit immediate from byte +4 (i32_at4 in script.c). */
static size_t emit_imm32(uint16_t *buf, size_t pos, uint8_t op, uint8_t len,
                          uint16_t a0, uint32_t imm32)
{
    buf[pos + 0] = (uint16_t)op | (uint16_t)((uint16_t)len << 8);
    buf[pos + 1] = a0;
    buf[pos + 2] = (uint16_t)(imm32 & 0xFFFF);
    buf[pos + 3] = (uint16_t)((imm32 >> 16) & 0xFFFF);
    return pos + (size_t)len * 2;
}

static void reset_vm(void)
{
    memset(g_script_vars, 0, sizeof g_script_vars);
    g_active_actor = 0;
    vm_stubs_reset();
}

/* ---- arithmetic ops ---------------------------------------------------- */

TEST(var_set_writes_register)
{
    /* op 0x0D VAR_SET: vars[a0] = i32_at4 (32-bit imm at byte +4). */
    reset_vm();
    uint16_t prog[32] = { 0 };
    size_t p = 0;
    p = emit_imm32(prog, p, 0x0D, 2, 10, 0x12345678u);
    p = emit_imm32(prog, p, 0x4F, 1, 0,  0); /* ABORT */
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(g_script_vars[10], 0x12345678u);
}

TEST(var_add_increments_register)
{
    reset_vm();
    g_script_vars[7] = 100;
    uint16_t prog[16] = { 0 };
    size_t p = 0;
    p = emit_imm32(prog, p, 0x4D, 2, 7, 25); /* VAR_ADD vars[7] += 25 */
    p = emit_imm32(prog, p, 0x4F, 1, 0, 0);  /* ABORT */
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(g_script_vars[7], 125);
}

TEST(var_sub_decrements_register)
{
    reset_vm();
    g_script_vars[3] = 50;
    uint16_t prog[16] = { 0 };
    size_t p = 0;
    p = emit_imm32(prog, p, 0x4E, 2, 3, 30); /* VAR_SUB vars[3] -= 30 */
    p = emit_imm32(prog, p, 0x4F, 1, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(g_script_vars[3], 20);
}

TEST(var_or_sets_bits)
{
    reset_vm();
    g_script_vars[1] = 0xF0;
    uint16_t prog[16] = { 0 };
    size_t p = 0;
    p = emit_imm32(prog, p, 0x0A, 2, 1, 0x0F);  /* VAR_OR vars[1] |= 0x0F */
    p = emit_imm32(prog, p, 0x4F, 1, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(g_script_vars[1], 0xFF);
}

TEST(var_andnot_clears_bits)
{
    reset_vm();
    g_script_vars[2] = 0xFF;
    uint16_t prog[16] = { 0 };
    size_t p = 0;
    p = emit_imm32(prog, p, 0x0C, 2, 2, 0xAA);  /* VAR_ANDNOT vars[2] &= ~0xAA */
    p = emit_imm32(prog, p, 0x4F, 1, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(g_script_vars[2], 0x55);
}

/* ---- this/that swap on a0 = 0x27 / 0x28 ------------------------------- *
 *
 * Critical Ghidra @ 0x00407820 line 218-223 behavior: when a0 is 0x27
 * or 0x28 the register index resolves to this_id / that_id rather than
 * raw a0. Earlier port had this inverted and broke every script that
 * pivoted on the active actor. Lock the contract here. */

TEST(reg_id_remap_a0_0x28_to_this_id)
{
    /* VAR_SET with a0=0x28 → vars[this_id], not vars[0x28]. */
    reset_vm();
    uint16_t prog[16] = { 0 };
    size_t p = 0;
    p = emit_imm32(prog, p, 0x0D, 2, 0x28, 0xCAFEBABEu);
    p = emit_imm32(prog, p, 0x4F, 1, 0, 0);
    (void)p;

    /* this_id=5 → expect vars[5] written, NOT vars[0x28]. */
    RunScriptInterpreter(5, 0, (uint8_t *)prog);
    ASSERT_EQ(g_script_vars[5], 0xCAFEBABEu);
    ASSERT_EQ(g_script_vars[0x28], 0u);
}

TEST(reg_id_remap_a0_0x27_to_that_id)
{
    /* a0=0x27 → vars[that_id]. */
    reset_vm();
    uint16_t prog[16] = { 0 };
    size_t p = 0;
    p = emit_imm32(prog, p, 0x0D, 2, 0x27, 0x99);
    p = emit_imm32(prog, p, 0x4F, 1, 0, 0);
    (void)p;

    RunScriptInterpreter(3, 17, (uint8_t *)prog);
    ASSERT_EQ(g_script_vars[17], 0x99);
    ASSERT_EQ(g_script_vars[0x27], 0u);
}

TEST(reg_id_no_remap_for_other_a0)
{
    /* a0=42 (not 0x27, not 0x28) → vars[42]. */
    reset_vm();
    uint16_t prog[16] = { 0 };
    size_t p = 0;
    p = emit_imm32(prog, p, 0x0D, 2, 42, 0xBEEF);
    p = emit_imm32(prog, p, 0x4F, 1, 0, 0);
    (void)p;

    RunScriptInterpreter(5, 17, (uint8_t *)prog);
    ASSERT_EQ(g_script_vars[42], 0xBEEF);
}

/* ---- query opcodes: GET_CUR_ROOM, GET_ACTOR_ID, IS_ITEM, GET_HELD ----- */

TEST(get_cur_room_returns_cur_komnata)
{
    /* op 0x29 GET_CUR_ROOM → g_return_reg = g_cur_komnata. */
    reset_vm();
    extern uint16_t g_cur_komnata;
    g_cur_komnata = 7;

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x29, 1, 0, 0, 0);
    p = emit(prog, p, 0x4F, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(RETURN_REG, 7);
}

TEST(get_actor_id_returns_active_actor_plus_one)
{
    /* op 0x34 → g_return_reg = g_active_actor + 1. */
    reset_vm();
    g_active_actor = 1; /* Fjej */

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x34, 1, 0, 0, 0);
    p = emit(prog, p, 0x4F, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(RETURN_REG, 2);    /* g_active_actor 1 + 1 */
}

TEST(is_item_returns_2_for_item_id_range)
{
    /* op 0x21: g_return_reg = ((this_id - 0x29) < 0x8E) ? 2 : 1.
     *
     * The arithmetic is SIGNED (this_id is uint16_t but `- 0x29` and the
     * `< 0x8E` comparison promote to int). So values < 0x29 underflow to
     * negative ints and STILL pass `< 0x8E` → return 2. That's 1:1 with
     * the original engine; this test pins both well-defined branches:
     *   - this_id in [0, 0xB6]   → returns 2
     *   - this_id in [0xB7, INF) → returns 1
     */
    reset_vm();
    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x21, 1, 0, 0, 0);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);   /* END_FORCE — actually halts */
    (void)p;

    /* this_id=0x50 (mid item range) → 0x27 < 0x8E → 2 */
    RunScriptInterpreter(0x50, 0, (uint8_t *)prog);
    ASSERT_EQ(RETURN_REG, 2);

    /* this_id=0x200 (past item range) → 0x1D7 (471) NOT < 0x8E (142) → 1 */
    reset_vm();
    RunScriptInterpreter(0x200, 0, (uint8_t *)prog);
    ASSERT_EQ(RETURN_REG, 1);
}

TEST(get_held_item_returns_that_id)
{
    /* op 0x22 GET_HELD_ITEM → g_return_reg = that_id. */
    reset_vm();
    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x22, 1, 0, 0, 0);
    p = emit(prog, p, 0x4F, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0xBEEF, (uint8_t *)prog);
    ASSERT_EQ(RETURN_REG, 0xBEEF);
}

TEST(inventory_has_via_op_0x23)
{
    /* op 0x23 GET_STATE → g_return_reg = InventoryHasItem(reg_id).
     * Production InventoryHasItem walks Inventory()=g_scene_snapshot. */
    extern uint32_t g_scene_snapshot[0x1E];
    reset_vm();
    /* Place verb 7 in inventory slot 0 (uint16 view of g_scene_snapshot). */
    ((uint16_t *)g_scene_snapshot)[0] = 7;

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x23, 1, 7, 0, 0);
    p = emit(prog, p, 0x4F, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(RETURN_REG, 1);

    /* Clear inventory → returns 0. */
    reset_vm();
    memset(g_scene_snapshot, 0, sizeof g_scene_snapshot);
    p = 0;
    p = emit(prog, p, 0x23, 1, 7, 0, 0);
    p = emit(prog, p, 0x4F, 1, 0, 0, 0);
    (void)p;
    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(RETURN_REG, 0);
}

/* ---- termination opcodes: ABORT, EOF, END_FORCE ----------------------- *
 *
 * Per src/script.c:
 *   - op 0x4F ABORT       → sets `result = 0` but does NOT halt execution
 *                          (the script continues until 0x55/0x56). The
 *                          docs entry "returns 0" describes the return
 *                          value of RunScriptInterpreter, not flow control.
 *   - op 0x55 END_FORCE   → pc = NULL → loop exits
 *   - op 0x56 EOF         → pc = NULL → loop exits
 */

TEST(abort_sets_return_to_zero_does_not_halt)
{
    /* ABORT sets the function's return value to 0, but execution
     * CONTINUES until 0x55/0x56. */
    reset_vm();
    uint16_t prog[16] = { 0 };
    size_t p = 0;
    p = emit_imm32(prog, p, 0x0D, 2, 5, 0x11); /* vars[5] = 0x11 */
    p = emit(prog, p, 0x4F, 1, 0, 0, 0);       /* ABORT → result=0 */
    p = emit_imm32(prog, p, 0x0D, 2, 5, 0x22); /* still runs */
    p = emit(prog, p, 0x55, 1, 0, 0, 0);       /* END_FORCE */

    int rc = RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(rc, 0);                          /* ABORT made return 0 */
    ASSERT_EQ(g_script_vars[5], 0x22);         /* both VAR_SETs ran */
}

TEST(no_abort_returns_one)
{
    /* Without ABORT, RunScriptInterpreter returns 1. */
    reset_vm();
    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit_imm32(prog, p, 0x0D, 2, 3, 0x77);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    int rc = RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(g_script_vars[3], 0x77);
}

TEST(eof_halts_execution)
{
    reset_vm();
    uint16_t prog[16] = { 0 };
    size_t p = 0;
    p = emit_imm32(prog, p, 0x0D, 2, 5, 0x33);
    p = emit(prog, p, 0x56, 1, 0, 0, 0);        /* EOF → pc=NULL */
    p = emit_imm32(prog, p, 0x0D, 2, 5, 0x44);

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(g_script_vars[5], 0x33);
}

TEST(end_force_halts_like_eof)
{
    /* op 0x55 END_FORCE → pc = NULL, same as 0x56. */
    reset_vm();
    uint16_t prog[16] = { 0 };
    size_t p = 0;
    p = emit_imm32(prog, p, 0x0D, 2, 5, 0x55);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);        /* END_FORCE */
    p = emit_imm32(prog, p, 0x0D, 2, 5, 0x66);  /* should NOT run */

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(g_script_vars[5], 0x55);
}

SUITE(real_vm)
{
    RUN_TEST(var_set_writes_register);
    RUN_TEST(var_add_increments_register);
    RUN_TEST(var_sub_decrements_register);
    RUN_TEST(var_or_sets_bits);
    RUN_TEST(var_andnot_clears_bits);
    RUN_TEST(reg_id_remap_a0_0x28_to_this_id);
    RUN_TEST(reg_id_remap_a0_0x27_to_that_id);
    RUN_TEST(reg_id_no_remap_for_other_a0);
    RUN_TEST(get_cur_room_returns_cur_komnata);
    RUN_TEST(get_actor_id_returns_active_actor_plus_one);
    RUN_TEST(is_item_returns_2_for_item_id_range);
    RUN_TEST(get_held_item_returns_that_id);
    RUN_TEST(inventory_has_via_op_0x23);
    RUN_TEST(abort_sets_return_to_zero_does_not_halt);
    RUN_TEST(no_abort_returns_one);
    RUN_TEST(eof_halts_execution);
    RUN_TEST(end_force_halts_like_eof);
}
