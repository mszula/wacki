/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tests/test_vm_walk_anim.c — actor walk + anim ops smoke tests.
 *
 * Production stubs.c now linked → ScriptCallWalkMode/WalkTo/AttachProp
 * are exercised end-to-end. The previous dispatch-counter tests were
 * removed; what remains here are smoke tests verifying the VM
 * dispatches without crashing + op 0x33 RESET_ACTORS field clearing
 * (which manipulates entity bytes directly and is fully testable).
 *
 * Reference: src/script.c case 0x10-0x12, 0x33, 0x35-0x3A, 0x3B-0x3C.
 */

#include "test.h"
#include "wacki.h"
#include "test_engine_stubs.h"

#include <stdint.h>
#include <string.h>

extern int RunScriptInterpreter(uint16_t this_id, uint16_t that_id, uint8_t *bytecode);
extern uint32_t g_script_vars[0x129];
extern Entity *g_actor[2];

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

static void reset_vm(void)
{
    memset(g_script_vars, 0, sizeof g_script_vars);
    vm_stubs_reset();
    g_actor[0] = g_actor[1] = NULL;
}

/* ---- op 0x10 / 0x11 / 0x12 ANIM_ACTOR smoke ------------------------- */

TEST(op_10_anim_actor1_completes)
{
    reset_vm();
    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x10, 2, 100, 200, 0);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    int rc = RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(rc, 1);
}

TEST(op_11_anim_actor2_completes)
{
    reset_vm();
    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x11, 2, 50, 250, 0);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    int rc = RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(rc, 1);
}

TEST(op_12_anim_both_completes)
{
    reset_vm();
    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x12, 2, 33, 44, 1);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    int rc = RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(rc, 1);
}

/* ---- op 0x35-0x3C walk/attach dispatches (production stubs.c) ------- */

TEST(op_35_walk_mode1_completes)
{
    reset_vm();
    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x35, 1, 7, 0, 0);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    int rc = RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(rc, 1);
}

TEST(op_38_walkto_completes)
{
    reset_vm();
    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x38, 2, 11, 0x55, 0);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    int rc = RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(rc, 1);
}

TEST(op_3B_attach_prop_completes)
{
    reset_vm();
    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x3B, 2, 5, 17, 0);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    int rc = RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(rc, 1);
}

/* ---- op 0x33 RESET_ACTORS — clears walker fields on g_actor[0..1] --- *
 *
 * This is fully testable because it manipulates Entity bytes directly. */

static uint8_t s_actor0[256];
static uint8_t s_actor1[256];

TEST(op_33_reset_actors_clears_walker_state)
{
    reset_vm();
    memset(s_actor0, 0, sizeof s_actor0);
    memset(s_actor1, 0, sizeof s_actor1);
    *(uint16_t *)(s_actor0 + 0x3A) = 0xFFFF;
    *(uint16_t *)(s_actor0 + 0x32) = 0x1234;
    *(uint32_t *)(s_actor0 + 0x4C) = 0xDEADBEEFu;
    *(uint32_t *)(s_actor0 + 0x50) = 0xCAFEBABEu;
    *(uint16_t *)(s_actor1 + 0x3A) = 0x00FF;
    *(uint32_t *)(s_actor1 + 0x4C) = 0xAAAAAAAAu;

    g_actor[0] = (Entity *)s_actor0;
    g_actor[1] = (Entity *)s_actor1;

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x33, 1, 0, 0, 0);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);

    ASSERT_EQ(*(uint16_t *)(s_actor0 + 0x3A) & 5u, 0u);
    ASSERT_EQ(*(uint16_t *)(s_actor0 + 0x32), 0);
    ASSERT_EQ(*(uint32_t *)(s_actor0 + 0x4C), 0u);
    ASSERT_EQ(*(uint32_t *)(s_actor0 + 0x50), 0u);
    ASSERT_EQ(*(uint16_t *)(s_actor1 + 0x3A) & 5u, 0u);
    ASSERT_EQ(*(uint32_t *)(s_actor1 + 0x4C), 0u);
}

TEST(op_33_reset_actors_handles_null_actors)
{
    reset_vm();
    g_actor[0] = g_actor[1] = NULL;

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x33, 1, 0, 0, 0);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    int rc = RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(rc, 1);
}

SUITE(vm_walk_anim)
{
    RUN_TEST(op_10_anim_actor1_completes);
    RUN_TEST(op_11_anim_actor2_completes);
    RUN_TEST(op_12_anim_both_completes);
    RUN_TEST(op_35_walk_mode1_completes);
    RUN_TEST(op_38_walkto_completes);
    RUN_TEST(op_3B_attach_prop_completes);
    RUN_TEST(op_33_reset_actors_clears_walker_state);
    RUN_TEST(op_33_reset_actors_handles_null_actors);
}
