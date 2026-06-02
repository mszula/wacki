/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tests/test_save_slot.c — LoadSaveSlot field-copy + T102 order.
 *
 * save_io covers the file-I/O half (read + write + atomic rename).
 * This file covers the SLOT RESTORE half: LoadSaveSlot pours a slot's
 * stored state back into live globals (g_script_vars, g_entity_state,
 * g_scene_snapshot) and updates g_cur_etap/g_cur_komnata.
 *
 * Critical regression test for T102 (round 31): LoadStage MUST be called
 * BEFORE the memcpy block. Earlier port had LoadStage after, which
 * clobbered the just-restored script_vars with stage defaults.
 *
 * Our LoadStage stub in test_engine_stubs.c is a no-op (returns 1) so
 * the test can't directly verify the call-ORDER — but we CAN verify
 * that after LoadSaveSlot:
 *   - g_cur_etap matches slot.etap_id (set BEFORE LoadStage call)
 *   - g_cur_komnata matches slot.stage_indicator (set AFTER LoadStage)
 *   - g_script_vars/entity_state/scene_snapshot match slot contents
 *
 * Reference: src/save.c LoadSaveSlot lines 72-95.
 */

#include "test.h"
#include "wacki.h"

#include <stdint.h>
#include <string.h>

extern WackiSaveFile g_save;
extern int  LoadSaveSlot(uint16_t idx);
extern int  QuickSaveToSlot(uint16_t idx);
extern int  QuickLoadFromSlot(uint16_t idx);

/* Engine globals (script.c + test_engine_stubs.c). */
extern uint32_t g_script_vars[0x129];
extern uint16_t g_cur_etap;
extern uint16_t g_cur_komnata;
extern uint32_t g_entity_state[0x11C];
extern uint32_t g_scene_snapshot[0x1E];

/* ---- LoadSaveSlot field copy ---------------------------------------- */

TEST(load_slot_copies_script_vars)
{
    /* Pre-fill slot 3 with a known pattern of script_vars values.
     * After LoadSaveSlot(3), g_script_vars should match. */
    memset(&g_save, 0, sizeof g_save);
    WackiSlot *s = &g_save.slots[3];
    s->stage_indicator = 7;
    s->etap_id         = 2;
    for (int i = 0; i < 0x129; ++i) s->script_vars[i] = (uint32_t)(0xA000 + i);

    /* Scrub live state. */
    memset(g_script_vars, 0, sizeof g_script_vars);

    int rc = LoadSaveSlot(3);
    ASSERT_EQ(rc, 1);

    /* Spot-check several indices. */
    ASSERT_EQ(g_script_vars[0],     0xA000u);
    ASSERT_EQ(g_script_vars[1],     0xA001u);
    ASSERT_EQ(g_script_vars[100],   0xA000u + 100);
    ASSERT_EQ(g_script_vars[0x128], 0xA000u + 0x128);
}

TEST(load_slot_copies_entity_state)
{
    memset(&g_save, 0, sizeof g_save);
    WackiSlot *s = &g_save.slots[2];
    s->stage_indicator = 5;
    s->etap_id         = 1;
    for (int i = 0; i < 0x11C; ++i) s->entity_state[i] = (uint32_t)(0xB000 + i);

    memset(g_entity_state, 0, sizeof g_entity_state);

    ASSERT_EQ(LoadSaveSlot(2), 1);
    ASSERT_EQ(g_entity_state[0],      0xB000u);
    ASSERT_EQ(g_entity_state[0x11Bu - 1], 0xB000u + (0x11B - 1));
}

TEST(load_slot_copies_scene_snapshot)
{
    memset(&g_save, 0, sizeof g_save);
    WackiSlot *s = &g_save.slots[1];
    s->stage_indicator = 3;
    s->etap_id         = 1;
    for (int i = 0; i < 0x1E; ++i) s->scene_snapshot[i] = (uint32_t)(0xC000 + i);

    memset(g_scene_snapshot, 0, sizeof g_scene_snapshot);

    ASSERT_EQ(LoadSaveSlot(1), 1);
    ASSERT_EQ(g_scene_snapshot[0],  0xC000u);
    ASSERT_EQ(g_scene_snapshot[29], 0xC000u + 29);
}

TEST(load_slot_sets_cur_etap_and_komnata)
{
    memset(&g_save, 0, sizeof g_save);
    g_save.slots[5].stage_indicator = 11;
    g_save.slots[5].etap_id         = 4;

    g_cur_etap = g_cur_komnata = 0;
    ASSERT_EQ(LoadSaveSlot(5), 1);
    ASSERT_EQ(g_cur_etap,    4);
    ASSERT_EQ(g_cur_komnata, 11);
}

/* ---- LoadSaveSlot rejects invalid / empty slots --------------------- */

TEST(load_slot_out_of_range_returns_zero)
{
    memset(&g_save, 0, sizeof g_save);
    ASSERT_EQ(LoadSaveSlot(10), 0);    /* slot index >= WACKI_SAVE_SLOTS */
    ASSERT_EQ(LoadSaveSlot(99), 0);
}

TEST(load_slot_empty_returns_zero)
{
    /* Empty slot has stage_indicator == 0. Loader should reject. */
    memset(&g_save, 0, sizeof g_save);
    g_save.slots[4].stage_indicator = 0;
    g_save.slots[4].etap_id         = 0;

    /* Pre-fill live state with sentinels — should NOT be overwritten. */
    g_script_vars[0]  = 0xDEADBEEFu;
    g_entity_state[0] = 0xCAFEBABEu;

    int rc = LoadSaveSlot(4);
    ASSERT_EQ(rc, 0);
    /* Sentinels preserved. */
    ASSERT_EQ(g_script_vars[0],  0xDEADBEEFu);
    ASSERT_EQ(g_entity_state[0], 0xCAFEBABEu);
}

/* ---- T102 order check (indirect) ----------------------------------- */

TEST(load_slot_t102_etap_set_before_komnata)
{
    /* After LoadSaveSlot completes both should be set. The actual order
     * matters only when LoadStage is non-trivial — our stub is no-op so
     * we can only confirm both end up correct. */
    memset(&g_save, 0, sizeof g_save);
    g_save.slots[6].stage_indicator = 99;
    g_save.slots[6].etap_id         = 3;

    g_cur_etap = g_cur_komnata = 0xFFFF;
    ASSERT_EQ(LoadSaveSlot(6), 1);
    ASSERT_EQ(g_cur_etap,    3);
    ASSERT_EQ(g_cur_komnata, 99);
}

/* ---- QuickSaveToSlot / QuickLoadFromSlot --------------------------- */

TEST(quicksave_refuses_when_not_in_game)
{
    /* T53: refuse QuickSave when g_cur_etap == 0 or g_cur_komnata == 0
     * (i.e. still in menu — saving here would write stage_indicator=0,
     * which LoadSaveSlot then rejects on reload). */
    g_cur_etap    = 0;
    g_cur_komnata = 0;
    memset(&g_save, 0, sizeof g_save);

    int rc = QuickSaveToSlot(0);
    ASSERT_EQ(rc, 0);
    /* Slot 0 stage_indicator should remain 0. */
    ASSERT_EQ(g_save.slots[0].stage_indicator, 0);
}

TEST(quicksave_out_of_range_returns_zero)
{
    g_cur_etap = 1; g_cur_komnata = 1;
    ASSERT_EQ(QuickSaveToSlot(99), 0);
}

TEST(quickload_empty_slot_returns_zero)
{
    memset(&g_save, 0, sizeof g_save);
    int rc = QuickLoadFromSlot(7);
    ASSERT_EQ(rc, 0);
}

TEST(quickload_out_of_range_returns_zero)
{
    ASSERT_EQ(QuickLoadFromSlot(50), 0);
}

SUITE(save_slot)
{
    RUN_TEST(load_slot_copies_script_vars);
    RUN_TEST(load_slot_copies_entity_state);
    RUN_TEST(load_slot_copies_scene_snapshot);
    RUN_TEST(load_slot_sets_cur_etap_and_komnata);
    RUN_TEST(load_slot_out_of_range_returns_zero);
    RUN_TEST(load_slot_empty_returns_zero);
    RUN_TEST(load_slot_t102_etap_set_before_komnata);
    RUN_TEST(quicksave_refuses_when_not_in_game);
    RUN_TEST(quicksave_out_of_range_returns_zero);
    RUN_TEST(quickload_empty_slot_returns_zero);
    RUN_TEST(quickload_out_of_range_returns_zero);
}
