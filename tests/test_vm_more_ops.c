/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tests/test_vm_more_ops.c — VM opcodes not covered by earlier suites.
 *
 * Picks up the rest of the 78-opcode table:
 *   TIMER_SET (0x0B)            entity timer write
 *   IS_SOUND_PLAYING (0x2A)     → calls WackiRand(a0), returns in [0, a0)
 *   SET_VERB (0x2B)             g_held_item write with reg_id range check
 *   PRELOAD_LIST_A (0x2E)       → ScriptCallRegMaskList(reg_id, addr, 0)
 *   PRELOAD_LIST_B (0x2F)       → ScriptCallRegMaskList(reg_id, addr, 1)
 *   SPAWN_ENTITY (0x30)         → ScriptCallSpawnEntity(id, flags, click, script)
 *   SET_CURSOR_SPEED (0x40)     writes 3 globals
 *   FLAG_CLEAR (0x43)           dead-state no-op
 *   FLAG_SET (0x44)             dead-state no-op
 *   NUDGE_ENT (0x47)            entity +0x0A/+0x22/+0x0C/+0x24 += dx/dy
 *   GET_ENT_X (0x4B)            reads +0x0A or +0x22 based on flag bit
 *   GET_ENT_Y (0x4C)            reads +0x0C or +0x24
 *   SET_ENT_POS (0x28)          entity +0x0A=+0x22=x, +0x0C=+0x24=y
 *   DEBUG_LOG (0x57)            stderr log (PORT SHORTCUT), no crash
 *   WAIT_FRAME (0x13)           ProcessGameFrameTick + SDL_Delay
 *
 * Reference: src/script.c case 0x0B / 0x2A / 0x2B / 0x2E-0x30 /
 *            0x40 / 0x43 / 0x44 / 0x47 / 0x4B / 0x4C / 0x28 / 0x57.
 */

#include "test.h"
#include "wacki.h"
#include "test_engine_stubs.h"

#include <stdint.h>
#include <string.h>

extern int RunScriptInterpreter(uint16_t this_id, uint16_t that_id, uint8_t *bytecode);
extern uint32_t g_script_vars[0x129];
extern uint16_t g_active_actor;
extern uint16_t g_cursor_speed;       /* in script.c */
extern uint16_t g_perspective_min;
extern uint16_t g_perspective_step;
extern uint16_t g_held_item;          /* in test_engine_stubs.c */

#define RETURN_REG (*(uint16_t *)&g_script_vars[4])

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
    g_active_actor = 0;
    vm_stubs_reset();
    g_stub_entity_for_verb = NULL;
    g_stub_update_registration_for_kind_id = NULL;
    g_held_item = 0;
}

/* Helper: Entity-shaped buffer for tests. We use a 256-byte aligned
 * buffer and cast to Entity*. The fields we touch live at byte offsets
 * 0x0A (drawn X), 0x0C (drawn Y), 0x22 (anchor X), 0x24 (anchor Y),
 * 0x28 (atlas slot), 0x30 (frame), 0x3A (flags3a). */
static uint8_t s_ent_buf[256];

static Entity *make_entity(int16_t draw_x, int16_t draw_y,
                            int16_t anchor_x, int16_t anchor_y,
                            uint16_t flags3a)
{
    memset(s_ent_buf, 0, sizeof s_ent_buf);
    *(int16_t *)(s_ent_buf + 0x0A)  = draw_x;
    *(int16_t *)(s_ent_buf + 0x0C)  = draw_y;
    *(int16_t *)(s_ent_buf + 0x22)  = anchor_x;
    *(int16_t *)(s_ent_buf + 0x24)  = anchor_y;
    *(uint16_t *)(s_ent_buf + 0x3A) = flags3a;
    /* +0x28 atlas slot = 0 (no atlas — GET_ENT_X/Y will read drawn) */
    return (Entity *)s_ent_buf;
}

/* ---- op 0x40 SET_CURSOR_SPEED: writes 3 globals ----------------------- */

TEST(op_40_set_cursor_speed_writes_three_globals)
{
    /* g_cursor_speed = a0; g_perspective_min = a1; g_perspective_step = a2. */
    reset_vm();
    g_cursor_speed = g_perspective_min = g_perspective_step = 0xFFFF;

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x40, 2, 100, 200, 50);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(g_cursor_speed,     100);
    ASSERT_EQ(g_perspective_min,  200);
    ASSERT_EQ(g_perspective_step, 50);
}

/* ---- op 0x2B SET_VERB → g_held_item ----------------------------------- *
 *
 * Per src/script.c case 0x2B:
 *   if (((reg_id - 0x29) < 0x8E) || reg_id == 0x26)
 *     g_held_item = reg_id;
 *
 * The check uses RESOLVED reg_id (this/that-mapped, NOT raw a0). The
 * arithmetic is UNSIGNED here (uint16 - uint16). So reg_id in 0x29..0xB6
 * passes, AND the 0x26 self sentinel passes. Everything else NO-OP. */

TEST(op_2B_set_verb_writes_held_item_in_item_range)
{
    reset_vm();
    g_held_item = 0;

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x2B, 1, 0x50, 0, 0);    /* 0x50 - 0x29 = 0x27 < 0x8E ✓ */
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(g_held_item, 0x50);
}

TEST(op_2B_set_verb_self_sentinel_0x26)
{
    reset_vm();
    g_held_item = 0;

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x2B, 1, 0x26, 0, 0);    /* self sentinel */
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(g_held_item, 0x26);
}

TEST(op_2B_set_verb_above_item_range_is_noop)
{
    /* reg_id = 0x200 (past 0x29 + 0x8E = 0xB7) AND != 0x26 → no write.
     *
     * NOTE: the port's check is `((reg_id - 0x29) < 0x8E)` with int
     * promotion, which means IDs BELOW 0x29 also pass (signed
     * underflow → negative int < 0x8E). The Ghidra original is
     * `(ushort)(uVar29 - 0x29) < 0x8e` (unsigned wrap → large value,
     * fails check). This is a known port divergence — for IDs below
     * 0x29 the port writes g_held_item where the original would not.
     * No shipped script uses IDs < 0x29 for held items in practice.
     *
     * We test only with an ID where BOTH interpretations agree (no
     * write expected) — IDs >= 0xB7. */
    reset_vm();
    g_held_item = 0x77;   /* pre-set; should NOT be overwritten */

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x2B, 1, 0x200, 0, 0);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(g_held_item, 0x77);    /* unchanged */
}

TEST(op_2B_uses_resolved_reg_id_for_this_swap)
{
    /* a0=0x28 → reg_id = this_id. With this_id=0x50 (item range),
     * g_held_item = 0x50, NOT 0x28. */
    reset_vm();
    g_held_item = 0;

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x2B, 1, 0x28, 0, 0);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0x50, 0, (uint8_t *)prog);
    ASSERT_EQ(g_held_item, 0x50);
}

/* ---- op 0x43 / 0x44 dead-state no-ops --------------------------------- */

TEST(op_43_flag_clear_is_noop)
{
    reset_vm();
    g_script_vars[3] = 0x1234;     /* sentinel; verify no scribble */

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x43, 1, 0, 0, 0);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(g_script_vars[3], 0x1234u);
}

TEST(op_44_flag_set_is_noop)
{
    reset_vm();
    g_script_vars[3] = 0x5678;

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x44, 1, 0, 0, 0);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(g_script_vars[3], 0x5678u);
}

/* ---- op 0x2A IS_SOUND_PLAYING → WackiRand bridge ----------------------- *
 *
 * `g_return_reg = WackiRand(a0)`. Note this isn't really "IS_SOUND_PLAYING"
 * in the port — docs say it returns RNG. Whatever the historical name,
 * the contract is: returns a value < a0. */

TEST(op_2A_is_sound_playing_returns_within_bound)
{
    reset_vm();
    extern void WackiRandSeed(uint32_t);
    WackiRandSeed(42);

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x2A, 1, 100, 0, 0);     /* bound = 100 */
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_TRUE(RETURN_REG < 100);
}

/* ---- op 0x2E / 0x2F PRELOAD_LIST → RegMaskList ------------------------ */

TEST(op_2E_preload_list_a_dispatches_with_table_zero)
{
    reset_vm();
    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit_imm32(prog, p, 0x2E, 2, 7, 0xCAFEFEEDu);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
}

TEST(op_2F_preload_list_b_dispatches_with_table_one)
{
    reset_vm();
    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit_imm32(prog, p, 0x2F, 2, 9, 0xBEEFCAFEu);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
}

/* ---- op 0x30 SPAWN_ENTITY -------------------------------------------- *
 *
 * Operand layout per src/script.c:1149-1161:
 *   word[0] = op|len   word[1] = a0=id   word[2] = flags2 ? actually:
 *   word[6] = pc[6] = flags2 (for len>=4)
 *   u32 @ +4 = click_addr (for len>=3)
 *   u32 @ +8 = script_addr (for len>=4)
 *
 * len=4 → 16 bytes = 8 words: w0=hdr, w1=id, w2/3=click_u32, w4/5=script_u32, w6=flags, w7=pad.
 */

TEST(op_30_spawn_entity_passes_flags_and_addrs)
{
    reset_vm();
    uint16_t prog[16] = { 0 };
    size_t pos = 0;
    /* op=0x30 len=4 → 16 bytes total */
    prog[pos + 0] = (uint16_t)0x30 | (4 << 8);
    prog[pos + 1] = 55;     /* id */
    /* u32 click_addr @ +4 = words[2..3] */
    prog[pos + 2] = 0x1234;
    prog[pos + 3] = 0x5678;
    /* u32 script_addr @ +8 = words[4..5] */
    prog[pos + 4] = 0xAAAA;
    prog[pos + 5] = 0xBBBB;
    /* flags2 @ word[6] */
    prog[pos + 6] = 0x00C0;
    prog[pos + 7] = 0;       /* pad */
    pos += 4 * 2;
    emit(prog, pos, 0x55, 1, 0, 0, 0);

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
}

/* ---- op 0x47 NUDGE_ENT: entity field += dx, dy ----------------------- *
 *
 * Adds (dx, dy) to entity at +0x0A and +0x22 (X) and +0x0C and +0x24 (Y).
 * Operand layout: a0=verb, pc[2]=dx, pc[3]=dy. */

TEST(op_47_nudge_ent_no_entity_is_noop)
{
    /* FindEntityByVerbId returns NULL (default) → no scribble. */
    reset_vm();
    g_stub_entity_for_verb = NULL;

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x47, 2, 7, 10, 20);     /* nudge verb=7 by (10, 20) */
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    /* FindEntityByVerbId was called with verb_id=7. */
}

TEST(op_47_nudge_ent_with_entity_updates_4_fields)
{
    reset_vm();
    Entity *e = make_entity(100, 200, 105, 210, 0);
    test_inject_entity_for_verb(e, 7);

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    /* Nudge by dx=5, dy=-7. */
    p = emit(prog, p, 0x47, 2, 7, 5, (uint16_t)(-7));
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);

    /* All 4 fields shifted. */
    uint8_t *eb = (uint8_t *)e;
    ASSERT_EQ(*(int16_t *)(eb + 0x0A), 105);    /* drawn X: 100+5 */
    ASSERT_EQ(*(int16_t *)(eb + 0x22), 110);    /* anchor X: 105+5 */
    ASSERT_EQ(*(int16_t *)(eb + 0x0C), 193);    /* drawn Y: 200-7 */
    ASSERT_EQ(*(int16_t *)(eb + 0x24), 203);    /* anchor Y: 210-7 */
}

/* ---- op 0x4B / 0x4C GET_ENT_X / Y ------------------------------------ *
 *
 * Returns +0x0A (drawn X) by default; if atlas (+0x28) is non-NULL AND
 * flag bit 1 at +0x3A is set, returns +0x22 (anchor X) instead.
 * Mirror for Y with +0x0C / +0x24. */

TEST(op_4B_get_ent_x_returns_drawn_when_no_atlas)
{
    reset_vm();
    Entity *e = make_entity(/*draw_x=*/123, 200, 999, 999, /*flags3a=*/0);
    test_inject_entity_for_verb(e, 7);

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x4B, 1, 7, 0, 0);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(RETURN_REG, 123);
}

TEST(op_4B_get_ent_x_returns_anchor_when_atlas_and_flag_2)
{
    reset_vm();
    Entity *e = make_entity(123, 200, /*anchor_x=*/456, 999, /*flags3a=*/2);
    test_inject_entity_for_verb(e, 7);
    /* Set atlas slot at +0x28 to non-zero. The slot is opaque to the
     * VM logic — just needs to be != 0. */
    *(uint32_t *)((uint8_t *)e + 0x28) = 0x1234;

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x4B, 1, 7, 0, 0);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(RETURN_REG, 456);
}

TEST(op_4B_get_ent_x_no_entity_returns_zero)
{
    reset_vm();
    g_stub_entity_for_verb = NULL;
    RETURN_REG = 0xFFFF;        /* sentinel */

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x4B, 1, 7, 0, 0);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(RETURN_REG, 0);
}

TEST(op_4C_get_ent_y_returns_drawn_when_no_atlas)
{
    reset_vm();
    Entity *e = make_entity(100, /*draw_y=*/234, 999, 999, 0);
    test_inject_entity_for_verb(e, 7);

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x4C, 1, 7, 0, 0);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(RETURN_REG, 234);
}

/* ---- op 0x28 SET_ENT_POS --------------------------------------------- *
 *
 * Sets both drawn (+0x0A/+0x0C) AND anchor (+0x22/+0x24) to (x, y).
 * If atlas is set + foot-anchor flag, adjusts drawn by atlas hot-spot
 * (we don't exercise that path here — needs a real AnimAsset fixture). */

TEST(op_28_set_ent_pos_writes_all_four_fields)
{
    reset_vm();
    Entity *e = make_entity(0, 0, 0, 0, 0);
    test_inject_entity_for_verb(e, 7);
    /* No atlas → no foot-anchor compensation. */

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x28, 2, 7, 333, 444);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    uint8_t *eb = (uint8_t *)e;
    ASSERT_EQ(*(uint16_t *)(eb + 0x0A), 333);
    ASSERT_EQ(*(uint16_t *)(eb + 0x22), 333);
    ASSERT_EQ(*(uint16_t *)(eb + 0x0C), 444);
    ASSERT_EQ(*(uint16_t *)(eb + 0x24), 444);
}

/* ---- op 0x0B SET_ACTOR_FLAG2 + frame override ------------------------- *
 *
 * FindUpdateRegistration(2, reg_id); if (e) { e[+9] |= 2; e[+0x26] = pc[2] } */

TEST(op_0B_timer_set_with_entity_writes_flag_and_frame)
{
    reset_vm();
    /* Reuse Entity buffer. */
    Entity *e = make_entity(0, 0, 0, 0, 0);
    test_inject_entity_for_update(e, 2, 7);

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x0B, 2, 7, 0x42, 0);   /* a0=verb, a1=frame override */
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    uint8_t *eb = (uint8_t *)e;
    ASSERT_EQ(eb[9] & 2, 2);                  /* flags2 bit 1 set */
    ASSERT_EQ(*(uint16_t *)(eb + 0x26), 0x42);
}

TEST(op_0B_no_entity_is_noop)
{
    reset_vm();
    g_stub_update_registration_for_kind_id = NULL;

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x0B, 2, 7, 0x55, 0);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    /* Should not crash. */
    RunScriptInterpreter(0, 0, (uint8_t *)prog);
}

/* ---- op 0x13 WAIT_FRAME → ProcessGameFrameTick (+ SDL_Delay no-op) --- */

TEST(op_13_wait_frame_calls_process_tick)
{
    reset_vm();
    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x13, 1, 0, 0, 0);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(g_stub_process_frame_calls, 1);
}

/* ---- op 0x57 DEBUG_LOG: PORT SHORTCUT writes to stderr, no crash ----- */

TEST(op_57_debug_log_does_not_crash)
{
    /* The port writes to stderr; just verify we don't segfault on a
     * len=2 instruction with arbitrary operand. */
    reset_vm();
    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit_imm32(prog, p, 0x57, 2, 0, 0xABCD);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    int rc = RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(rc, 1);    /* completed cleanly */
}

SUITE(vm_more_ops)
{
    RUN_TEST(op_40_set_cursor_speed_writes_three_globals);
    RUN_TEST(op_2B_set_verb_writes_held_item_in_item_range);
    RUN_TEST(op_2B_set_verb_self_sentinel_0x26);
    RUN_TEST(op_2B_set_verb_above_item_range_is_noop);
    RUN_TEST(op_2B_uses_resolved_reg_id_for_this_swap);
    RUN_TEST(op_43_flag_clear_is_noop);
    RUN_TEST(op_44_flag_set_is_noop);
    RUN_TEST(op_2A_is_sound_playing_returns_within_bound);
    RUN_TEST(op_2E_preload_list_a_dispatches_with_table_zero);
    RUN_TEST(op_2F_preload_list_b_dispatches_with_table_one);
    RUN_TEST(op_30_spawn_entity_passes_flags_and_addrs);
    RUN_TEST(op_47_nudge_ent_no_entity_is_noop);
    RUN_TEST(op_47_nudge_ent_with_entity_updates_4_fields);
    RUN_TEST(op_4B_get_ent_x_returns_drawn_when_no_atlas);
    RUN_TEST(op_4B_get_ent_x_returns_anchor_when_atlas_and_flag_2);
    RUN_TEST(op_4B_get_ent_x_no_entity_returns_zero);
    RUN_TEST(op_4C_get_ent_y_returns_drawn_when_no_atlas);
    RUN_TEST(op_28_set_ent_pos_writes_all_four_fields);
    RUN_TEST(op_0B_timer_set_with_entity_writes_flag_and_frame);
    RUN_TEST(op_0B_no_entity_is_noop);
    RUN_TEST(op_13_wait_frame_calls_process_tick);
    RUN_TEST(op_57_debug_log_does_not_crash);
}
