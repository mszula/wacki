/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tests/test_vm_game_over.c — var[14] = g_game_over_code alias.
 *
 * CRITICAL REGRESSION TEST for Bug 2 fix (round 32).
 *
 * The original engine has DAT_004498B8 (g_game_over_code) at PE offset
 * 0x004498B8, which is byte offset 0x38 (= 14 * 4) inside the
 * g_script_vars[] array starting at DAT_00449880. The two share the
 * same memory.
 *
 * Scripts trigger end-of-stage / death / chapter-select via the main-VM
 * SET_VAR opcode (op 0x0D) with var index 14:
 *   val=1 → death          (Dane_14.dta cutscene)
 *   val=3 → chapter-select (sel_tlo.pic UI)
 *   val=4 → stage-end death cutscene + return to menu
 *
 * Confirmed: 9 such writes exist in WACKI.EXE bytecode (4×val=1,
 * 4×val=3, 1×val=4).
 *
 * Pre-fix the port had `g_game_over_code` as a SEPARATE int — scripts
 * wrote to g_script_vars[14] which nobody read; the post-loop handler
 * checked its own zero g_game_over_code. End-of-stage cutscene +
 * chapter-select NEVER FIRED. After fix the two are the SAME memory
 * via macro:
 *
 *   #define g_game_over_code  (*(int *)&g_script_vars[14])
 *
 * If anyone "tidies up" the macro into a separate variable, this test
 * fails immediately and prevents the regression from shipping.
 *
 * Reference: include/wacki.h:551-570, src/script.c:218-228.
 */

#include "test.h"
#include "wacki.h"
#include "test_engine_stubs.h"

#include <stdint.h>
#include <string.h>

extern int RunScriptInterpreter(uint16_t this_id, uint16_t that_id, uint8_t *bytecode);
extern uint32_t g_script_vars[0x129];

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
    vm_stubs_reset();
}

/* ---- var[14] alias: VAR_SET 14 writes g_game_over_code -------------- */

TEST(var_14_set_to_death_visible_via_game_over_macro)
{
    /* Script: VAR_SET 14, 1; END_FORCE
     * Expected: g_game_over_code == 1 (death). */
    reset_vm();
    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit_imm32(prog, p, 0x0D, 2, 14, 1);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(g_game_over_code, 1);
}

TEST(var_14_set_to_3_chapter_select)
{
    reset_vm();
    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit_imm32(prog, p, 0x0D, 2, 14, 3);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(g_game_over_code, 3);
}

TEST(var_14_set_to_4_stage_end)
{
    reset_vm();
    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit_imm32(prog, p, 0x0D, 2, 14, 4);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(g_game_over_code, 4);
}

/* ---- inverse direction: writing g_game_over_code visible via var[14] - */

TEST(game_over_macro_write_visible_via_var_14)
{
    /* Writing to g_game_over_code directly must update g_script_vars[14]
     * (since they're the same memory). */
    reset_vm();
    g_game_over_code = 7;
    ASSERT_EQ(g_script_vars[14], 7u);

    g_game_over_code = 0;
    ASSERT_EQ(g_script_vars[14], 0u);
}

/* ---- VAR_ADD / VAR_SUB on var[14] also propagate -------------------- */

TEST(var_add_to_14_propagates_to_game_over)
{
    reset_vm();
    g_script_vars[14] = 2;
    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit_imm32(prog, p, 0x4D, 2, 14, 1);    /* VAR_ADD 14 += 1 → 3 */
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(g_game_over_code, 3);
}

/* ---- VAR_OR on var[14] preserves alias ------------------------------ */

TEST(var_or_on_14_propagates)
{
    reset_vm();
    g_script_vars[14] = 0;
    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit_imm32(prog, p, 0x0A, 2, 14, 5);    /* VAR_OR 14 |= 5 */
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(g_game_over_code, 5);
}

/* ---- var[14] writes don't affect var[13] or var[15] (neighbour spill) */

TEST(var_14_write_does_not_spill_to_neighbours)
{
    reset_vm();
    g_script_vars[13] = 0xAAAA;
    g_script_vars[15] = 0xBBBB;

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit_imm32(prog, p, 0x0D, 2, 14, 0x12345678u);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(g_script_vars[14], 0x12345678u);
    /* Neighbours untouched. */
    ASSERT_EQ(g_script_vars[13], 0xAAAAu);
    ASSERT_EQ(g_script_vars[15], 0xBBBBu);
}

/* ---- g_return_reg = (uint16_t *)&g_script_vars[4] alias ------------- *
 *
 * Same pattern: g_return_reg overlays the low 16 bits of var[4]. Used
 * by query ops (0x29, 0x34, 0x22, 0x21, 0x49 etc.). Verify the alias
 * is bidirectional. */

TEST(return_reg_aliases_var_4_low_word)
{
    reset_vm();
    g_script_vars[4] = 0xAABB1234u;
    /* g_return_reg = uint16_t at addr of var[4] = low word = 0x1234 (on LE). */
    uint16_t *return_reg_p = (uint16_t *)&g_script_vars[4];
    ASSERT_EQ(*return_reg_p, 0x1234);

    *return_reg_p = 0xDEAD;
    /* High 16 bits of var[4] should be untouched (0xAABB). */
    ASSERT_EQ(g_script_vars[4] & 0xFFFF, 0xDEADu);
    ASSERT_EQ((g_script_vars[4] >> 16) & 0xFFFF, 0xAABBu);
}

TEST(get_cur_room_writes_low_word_of_var_4)
{
    /* op 0x29 GET_CUR_ROOM writes g_return_reg = g_cur_komnata.
     * Verify it writes the low 16 bits of var[4]. */
    reset_vm();
    extern uint16_t g_cur_komnata;
    g_cur_komnata = 42;
    g_script_vars[4] = 0xFFFF0000u;     /* pre-fill */

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x29, 1, 0, 0, 0);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    /* Low 16 bits = 42, high 16 bits untouched (= 0xFFFF). */
    ASSERT_EQ(g_script_vars[4] & 0xFFFFu, 42u);
    ASSERT_EQ((g_script_vars[4] >> 16) & 0xFFFFu, 0xFFFFu);
}

SUITE(vm_game_over)
{
    RUN_TEST(var_14_set_to_death_visible_via_game_over_macro);
    RUN_TEST(var_14_set_to_3_chapter_select);
    RUN_TEST(var_14_set_to_4_stage_end);
    RUN_TEST(game_over_macro_write_visible_via_var_14);
    RUN_TEST(var_add_to_14_propagates_to_game_over);
    RUN_TEST(var_or_on_14_propagates);
    RUN_TEST(var_14_write_does_not_spill_to_neighbours);
    RUN_TEST(return_reg_aliases_var_4_low_word);
    RUN_TEST(get_cur_room_writes_low_word_of_var_4);
}
