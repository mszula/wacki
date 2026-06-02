/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tests/test_vm_wait_break.c — wait-loop break conditions.
 *
 * src/script.c has 4 wait-loop opcodes:
 *   0x14 WAIT_MS        — wait N ms ticks
 *   0x15 WAIT_ENT_IDLE  — wait until entity[+0x4C]==0 && +0x50==0
 *   0x26 WAIT_ANIM_FRAME — wait until entity[+0x30] == target
 *   0x3D WAIT_KIND2_FRAME — variant via FindUpdateRegistration
 *
 * All four contain `if (PlatformShouldQuit() || g_game_over_code) break;`
 * inside their loop body. This is a critical user-experience guarantee:
 * a buggy script that waits forever (or a death event mid-wait) must
 * not lock the game.
 *
 * These tests use the settable `g_stub_should_quit` to force the quit
 * condition mid-wait and verify the wait terminates early.
 *
 * Reference: src/script.c case 0x14 line 551, 0x15 line 575, 0x26 line
 * 596, 0x3D line 620.
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

static uint8_t s_ent_buf[256];
static Entity *make_busy_entity(void)
{
    /* Entity with non-zero walker fields (so WAIT_ENT_IDLE would
     * loop until they zero). */
    memset(s_ent_buf, 0, sizeof s_ent_buf);
    *(uint32_t *)(s_ent_buf + 0x4C) = 0x100000;     /* walk_dx_rem != 0 */
    *(uint32_t *)(s_ent_buf + 0x50) = 0x100000;     /* walk_dy_rem != 0 */
    return (Entity *)s_ent_buf;
}

static Entity *make_busy_anim_entity(uint16_t current_frame)
{
    /* Entity with current frame != target (so WAIT_ANIM_FRAME loops). */
    memset(s_ent_buf, 0, sizeof s_ent_buf);
    *(uint16_t *)(s_ent_buf + 0x30) = current_frame;
    return (Entity *)s_ent_buf;
}

static void reset_vm(void)
{
    memset(g_script_vars, 0, sizeof g_script_vars);
    vm_stubs_reset();
    g_stub_entity_for_verb = NULL;
    g_stub_update_registration_for_kind_id = NULL;
    g_stub_should_quit = 0;
}

/* ---- op 0x14 WAIT_MS respects PlatformShouldQuit -------------------- */

TEST(op_14_wait_ms_breaks_on_should_quit)
{
    /* WAIT 1000 ticks (would normally take ~333 process_frame calls).
     * Set should_quit=1 → loop breaks on FIRST iteration. */
    reset_vm();
    g_stub_should_quit = 1;

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x14, 1, 1000, 0, 0);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    /* First iter: ProcessGameFrameTick fires, then break check.
     * So exactly 1 process_frame call. */
    ASSERT_EQ(g_stub_process_frame_calls, 1);
}

TEST(op_14_wait_ms_breaks_on_game_over_code)
{
    /* WAIT 1000 ticks but g_game_over_code (= var[14]) set →
     * loop breaks after 1 iteration. */
    reset_vm();
    g_game_over_code = 1;   /* death */

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x14, 1, 1000, 0, 0);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(g_stub_process_frame_calls, 1);
}

TEST(op_14_wait_ms_continues_when_no_break)
{
    /* Production g_frame_delta_ticks=1 → WAIT N = N ProcessGameFrameTick
     * calls when no break conditions fire. */
    reset_vm();

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x14, 1, 9, 0, 0);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(g_stub_process_frame_calls, 9);
}

/* ---- op 0x15 WAIT_ENT_IDLE breaks on should_quit / game_over -------- */

TEST(op_15_wait_ent_idle_breaks_on_should_quit)
{
    /* Entity is BUSY (walker fields non-zero). Without break it would
     * loop until safety cap (2000). With should_quit=1, exits after 1. */
    reset_vm();
    Entity *e = make_busy_entity();
    test_inject_entity_for_verb(e, 7);
    g_stub_should_quit = 1;

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x15, 1, 7, 0, 0);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(g_stub_process_frame_calls, 1);
}

TEST(op_15_wait_ent_idle_breaks_on_game_over)
{
    reset_vm();
    Entity *e = make_busy_entity();
    test_inject_entity_for_verb(e, 7);
    g_game_over_code = 3;   /* chapter-select */

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x15, 1, 7, 0, 0);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(g_stub_process_frame_calls, 1);
}

/* ---- op 0x26 WAIT_ANIM_FRAME breaks --------------------------------- */

TEST(op_26_wait_anim_frame_breaks_on_should_quit)
{
    reset_vm();
    Entity *e = make_busy_anim_entity(/*current=*/5);
    test_inject_entity_for_verb(e, 7);
    g_stub_should_quit = 1;

    /* Wait for frame 99 — entity is at 5, would normally loop forever
     * (until safety cap). should_quit → exit after 1 tick. */
    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x26, 2, 7, 99, 0);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(g_stub_process_frame_calls, 1);
}

TEST(op_26_wait_anim_frame_safety_cap_2000_eventually_exits)
{
    /* Without ANY break condition (no should_quit, no game_over),
     * the safety cap of 2000 iterations in the wait loop prevents
     * an infinite hang. Tests the safety cap exists. */
    reset_vm();
    Entity *e = make_busy_anim_entity(5);
    test_inject_entity_for_verb(e, 7);
    /* No break conditions. */

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x26, 2, 7, 99, 0);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    /* Should complete (not hang) thanks to safety cap. */
    int rc = RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(rc, 1);
    /* Safety cap = 2000 → exactly 2000 process_frame calls. */
    ASSERT_EQ(g_stub_process_frame_calls, 2000);
}

/* ---- op 0x3D WAIT_KIND2_FRAME breaks -------------------------------- */

TEST(op_3D_wait_kind2_frame_breaks_on_should_quit)
{
    reset_vm();
    Entity *e = make_busy_anim_entity(/*current=*/3);
    test_inject_entity_for_update(e, 2, 7);
    g_stub_should_quit = 1;

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x3D, 2, 7, 99, 0);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(g_stub_process_frame_calls, 1);
}

TEST(op_3D_wait_kind2_frame_safety_cap_eventually_exits)
{
    reset_vm();
    Entity *e = make_busy_anim_entity(3);
    test_inject_entity_for_update(e, 2, 7);

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x3D, 2, 7, 99, 0);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    int rc = RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(g_stub_process_frame_calls, 2000);
}

SUITE(vm_wait_break)
{
    RUN_TEST(op_14_wait_ms_breaks_on_should_quit);
    RUN_TEST(op_14_wait_ms_breaks_on_game_over_code);
    RUN_TEST(op_14_wait_ms_continues_when_no_break);
    RUN_TEST(op_15_wait_ent_idle_breaks_on_should_quit);
    RUN_TEST(op_15_wait_ent_idle_breaks_on_game_over);
    RUN_TEST(op_26_wait_anim_frame_breaks_on_should_quit);
    RUN_TEST(op_26_wait_anim_frame_safety_cap_2000_eventually_exits);
    RUN_TEST(op_3D_wait_kind2_frame_breaks_on_should_quit);
    RUN_TEST(op_3D_wait_kind2_frame_safety_cap_eventually_exits);
}
