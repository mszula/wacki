/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tests/test_vm_misc_ops.c — remaining VM ops.
 *
 * Covers:
 *   0x0E SET_ENTITY_SCRIPT  → reset entity state + bind bytecode
 *   0x14 WAIT_MS            → ProcessGameFrameTick loop (a0 ticks)
 *   0x15 WAIT_ENT_IDLE      → loop until entity[+0x4C]==0 && +0x50==0
 *   0x26 WAIT_ANIM_FRAME    → loop until entity[+0x30] == target
 *   0x27 SET_OBJ_PROP       → write to tagged-table key '\x15' slot
 *   0x50 SUBANIM_HIDE       → walks click list, toggle owner flag
 *   0x51 SUBANIM_SET_PARAM  → walks click list, write owner +0x12
 *   0x54 SHOW_PICTURE       → LoadFileFromDta + wait loop (with NULL name)
 *
 * Reference: src/script.c case 0x0E / 0x14 / 0x15 / 0x26 / 0x27 /
 *            0x50 / 0x51 / 0x54.
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

static uint8_t s_ent_buf[256];

static Entity *make_entity_clear(void)
{
    memset(s_ent_buf, 0, sizeof s_ent_buf);
    return (Entity *)s_ent_buf;
}

static void reset_vm(void)
{
    memset(g_script_vars, 0, sizeof g_script_vars);
    vm_stubs_reset();
    g_stub_entity_for_verb = NULL;
    g_stub_update_registration_for_kind_id = NULL;
}

/* ---- op 0x14 WAIT_MS — drives ProcessGameFrameTick in a loop --------- */

TEST(op_14_wait_ms_pumps_frames_until_zero)
{
    /* Production sets g_frame_delta_ticks = 1 → WAIT N decrements by 1
     * per tick → exactly N process_frame calls. */
    reset_vm();
    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x14, 1, 10, 0, 0);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(g_stub_process_frame_calls, 10);
}

TEST(op_14_wait_ms_zero_completes_immediately)
{
    /* a0=0 → while (left) never enters → no ticks. */
    reset_vm();
    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x14, 1, 0, 0, 0);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(g_stub_process_frame_calls, 0);
}

/* ---- op 0x15 WAIT_ENT_IDLE — waits until entity walker fields zero --- */

TEST(op_15_wait_ent_idle_with_no_entity_no_op)
{
    /* FindEntityByVerbId returns NULL → wait loop never entered. */
    reset_vm();
    g_stub_entity_for_verb = NULL;

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x15, 1, 7, 0, 0);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(g_stub_process_frame_calls, 0);
}

TEST(op_15_wait_ent_idle_exits_when_already_idle)
{
    /* Entity exists with +0x4C == 0 AND +0x50 == 0 → loop condition
     * `(eb[+0x4C] || eb[+0x50])` is false on first iteration → loop
     * never enters. */
    reset_vm();
    Entity *e = make_entity_clear();    /* all fields 0 */
    test_inject_entity_for_verb(e, 7);

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x15, 1, 7, 0, 0);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(g_stub_process_frame_calls, 0);
}

/* ---- op 0x26 WAIT_ANIM_FRAME — wait until entity[+0x30] == target ---- */

TEST(op_26_wait_anim_frame_exits_when_already_at_target)
{
    reset_vm();
    Entity *e = make_entity_clear();
    *(uint16_t *)((uint8_t *)e + 0x30) = 5;   /* already at frame 5 */
    test_inject_entity_for_verb(e, 7);

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x26, 2, 7, 5, 0);      /* wait verb=7 to frame 5 */
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(g_stub_process_frame_calls, 0);
}

TEST(op_26_wait_anim_frame_no_entity_skipped)
{
    reset_vm();
    g_stub_entity_for_verb = NULL;

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x26, 2, 7, 5, 0);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(g_stub_process_frame_calls, 0);
}

/* ---- op 0x3D WAIT_KIND2_FRAME — same as 0x26 but via update table --- */

TEST(op_3D_wait_kind2_frame_exits_when_already_at_target)
{
    reset_vm();
    Entity *e = make_entity_clear();
    *(uint16_t *)((uint8_t *)e + 0x30) = 3;
    test_inject_entity_for_update(e, 2, 7);

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x3D, 2, 7, 3, 0);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    /* FindUpdateRegistration(2, 7) called once, then target match → exit. */
    ASSERT_EQ(g_stub_process_frame_calls, 0);
}

/* ---- op 0x0E SET_ENTITY_SCRIPT — reset state + bind bytecode -------- */

TEST(op_0E_set_entity_script_clears_walker_state)
{
    reset_vm();
    Entity *e = make_entity_clear();
    uint8_t *eb = (uint8_t *)e;
    /* Pre-fill walker state. */
    *(uint16_t *)(eb + 0x3A) = 0xFFFF;
    *(uint16_t *)(eb + 0x38) = 0x99;
    *(uint16_t *)(eb + 0x32) = 0x44;
    *(uint32_t *)(eb + 0x4C) = 0xDEAD0000u;
    *(uint32_t *)(eb + 0x50) = 0xBEEFu;
    *(uint32_t *)(eb + 0x2C) = 0xCAFE;
    *(uint16_t *)(eb + 0x30) = 99;
    test_inject_entity_for_verb(e, 7);

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit_imm32(prog, p, 0x0E, 2, 7, 0xDEAD);   /* a0=verb, imm=bytecode VA (unresolvable) */
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);

    /* Verify reset block. */
    ASSERT_EQ(*(uint16_t *)(eb + 0x3A) & 5u, 0u);
    ASSERT_EQ(*(uint16_t *)(eb + 0x38), 0);
    ASSERT_EQ(*(uint16_t *)(eb + 0x36), 0);
    ASSERT_EQ(*(uint16_t *)(eb + 0x34), 0);
    ASSERT_EQ(*(uint16_t *)(eb + 0x32), 0);
    ASSERT_EQ(*(uint16_t *)(eb + 0x3C), 0);
    ASSERT_EQ(*(uint16_t *)(eb + 0x40), 0);
    ASSERT_EQ(*(uint16_t *)(eb + 0x42), 0);
    ASSERT_EQ(*(uint32_t *)(eb + 0x4C), 0u);
    ASSERT_EQ(*(uint32_t *)(eb + 0x50), 0u);
    /* frame reset */
    ASSERT_EQ(*(uint16_t *)(eb + 0x30), 0);
    /* bytecode VA unresolvable → +0x2C = 0 */
    ASSERT_EQ(*(uint32_t *)(eb + 0x2C), 0u);
}

TEST(op_0E_no_entity_is_noop)
{
    reset_vm();
    g_stub_entity_for_verb = NULL;

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit_imm32(prog, p, 0x0E, 2, 7, 0);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    int rc = RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(rc, 1);
}

/* ---- op 0x0F SET_ENTITY_ANIM — kind=2 lookup variant ----------------- */

TEST(op_0F_set_entity_anim_with_kind2_clears_state)
{
    reset_vm();
    Entity *e = make_entity_clear();
    uint8_t *eb = (uint8_t *)e;
    *(uint16_t *)(eb + 0x3A) = 0xFFFF;
    *(uint32_t *)(eb + 0x4C) = 0xAAAA;
    test_inject_entity_for_update(e, 2, 7);

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit_imm32(prog, p, 0x0F, 2, 7, 0);   /* unresolved bc → slot 0 */
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(*(uint16_t *)(eb + 0x3A) & 5u, 0u);
    ASSERT_EQ(*(uint32_t *)(eb + 0x4C), 0u);
}

/* ---- op 0x27 SET_OBJ_PROP — unresolved VA is no-op ------------------- */

TEST(op_27_set_obj_prop_with_unresolved_va_is_noop)
{
    /* No PE → xlat_binary_ptr(i32_at4) returns NULL → outer `if (blk)`
     * guard skips. Must complete without crash. */
    reset_vm();
    uint16_t prog[16] = { 0 };
    size_t p = 0;
    p = emit_imm32(prog, p, 0x27, 3, 0, 0xDEADBEEFu);   /* VA at +4 */
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    int rc = RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(rc, 1);
}

/* ---- op 0x50 / 0x51 SUBANIM — with no asset is no-op ----------------- */

TEST(op_50_subanim_hide_with_no_asset_is_noop)
{
    /* FindUpdateRegistration(1, reg_id) returns NULL → outer guard
     * skips the click-list scan. */
    reset_vm();
    g_stub_update_registration_for_kind_id = NULL;

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x50, 2, 7, 0, 1);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    int rc = RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(rc, 1);
}

TEST(op_51_subanim_set_param_with_no_asset_is_noop)
{
    reset_vm();
    g_stub_update_registration_for_kind_id = NULL;

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x51, 2, 7, 0, 0);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    int rc = RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(rc, 1);
}

/* ---- op 0x54 SHOW_PICTURE — NULL name path bails cleanly ------------- */

TEST(op_54_show_picture_with_null_name_logs_and_continues)
{
    /* No PE → xlat returns NULL → `name == NULL` → logs warning + skip.
     * No paint_rawb, no LMB wait loop. */
    reset_vm();

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit_imm32(prog, p, 0x54, 2, 0, 0xCAFEBABEu);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    int rc = RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(g_stub.paint_rawb, 0);
}

SUITE(vm_misc_ops)
{
    RUN_TEST(op_14_wait_ms_pumps_frames_until_zero);
    RUN_TEST(op_14_wait_ms_zero_completes_immediately);
    RUN_TEST(op_15_wait_ent_idle_with_no_entity_no_op);
    RUN_TEST(op_15_wait_ent_idle_exits_when_already_idle);
    RUN_TEST(op_26_wait_anim_frame_exits_when_already_at_target);
    RUN_TEST(op_26_wait_anim_frame_no_entity_skipped);
    RUN_TEST(op_3D_wait_kind2_frame_exits_when_already_at_target);
    RUN_TEST(op_0E_set_entity_script_clears_walker_state);
    RUN_TEST(op_0E_no_entity_is_noop);
    RUN_TEST(op_0F_set_entity_anim_with_kind2_clears_state);
    RUN_TEST(op_27_set_obj_prop_with_unresolved_va_is_noop);
    RUN_TEST(op_50_subanim_hide_with_no_asset_is_noop);
    RUN_TEST(op_51_subanim_set_param_with_no_asset_is_noop);
    RUN_TEST(op_54_show_picture_with_null_name_logs_and_continues);
}
