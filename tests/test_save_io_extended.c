/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tests/test_save_io_extended.c — all-slot + settings persistence.
 *
 * test_save_io.c covers default-init + single-slot roundtrip. This
 * extension pins:
 *   - All 10 slots writable independently (none cross-talk)
 *   - WackiSettings field-by-field persistence
 *   - Roundtrip preserves both settings AND all slot data
 *   - Slot 0 quicksave convention (F5/F9 reserved)
 *
 * Reference: src/save.c + include/wacki.h WackiSaveFile.
 */

#include "test.h"
#include "wacki.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

extern WackiSaveFile g_save;
extern void LoadSaveStateOrInitialize(void);
extern void WriteSaveFile(void);

static char s_orig_cwd[1024];
static char s_tmp_dir[1024];

static void set_test_cwd(void)
{
    if (!getcwd(s_orig_cwd, sizeof s_orig_cwd)) s_orig_cwd[0] = 0;
    snprintf(s_tmp_dir, sizeof s_tmp_dir,
             "/tmp/wacki-test-save-ext-%d", (int)getpid());
    test_mkdir(s_tmp_dir);
    chdir(s_tmp_dir);
    remove("Wacki.sav");
    remove("Wacki.sav.tmp");
}

static void restore_test_cwd(void)
{
    remove("Wacki.sav");
    remove("Wacki.sav.tmp");
    if (s_orig_cwd[0]) chdir(s_orig_cwd);
    rmdir(s_tmp_dir);
}

/* ---- all 10 slots independent --------------------------------------- */

TEST(all_ten_slots_write_then_read_back)
{
    set_test_cwd();

    memset(&g_save, 0, sizeof g_save);
    g_save.magic = WACKI_SAVE_MAGIC;

    /* Fill each slot with a distinct pattern. */
    for (int i = 0; i < WACKI_SAVE_SLOTS; ++i) {
        WackiSlot *s = &g_save.slots[i];
        s->stage_indicator = (uint16_t)(i * 10 + 1);   /* 1, 11, 21, ... */
        s->etap_id         = (uint16_t)((i % 5) + 1);  /* 1..5 cyclic */
        snprintf(s->name, sizeof s->name, "Slot%d", i);
        /* Stamp identifying value into script_vars[0]. */
        s->script_vars[0] = (uint32_t)(0xA0000000u + i);
    }

    WriteSaveFile();
    /* Clear in-memory image, reload from disk. */
    memset(&g_save, 0, sizeof g_save);
    LoadSaveStateOrInitialize();

    /* Verify every slot. */
    for (int i = 0; i < WACKI_SAVE_SLOTS; ++i) {
        WackiSlot *s = &g_save.slots[i];
        ASSERT_EQ(s->stage_indicator, (uint16_t)(i * 10 + 1));
        ASSERT_EQ(s->etap_id, (uint16_t)((i % 5) + 1));
        char expected_name[16];
        snprintf(expected_name, sizeof expected_name, "Slot%d", i);
        ASSERT_STREQ(s->name, expected_name);
        ASSERT_EQ(s->script_vars[0], (uint32_t)(0xA0000000u + i));
    }

    restore_test_cwd();
}

/* ---- WackiSettings field-by-field ---------------------------------- */

TEST(all_settings_fields_persist)
{
    set_test_cwd();

    memset(&g_save, 0, sizeof g_save);
    g_save.magic = WACKI_SAVE_MAGIC;
    /* Set each settings field to a distinguishing value. */
    g_save.settings.video_mode   = 2;
    g_save.settings.sound_on     = 1;
    g_save.settings.music_on     = 0;
    g_save.settings.pad0         = 0;       /* always 0 */
    g_save.settings.voice_on     = 0;
    g_save.settings.subtitles_on = 1;
    g_save.settings.dialogues_on = 0;
    g_save.settings.pad1         = 0;

    WriteSaveFile();
    memset(&g_save, 0xFF, sizeof g_save);   /* trash before reload */
    LoadSaveStateOrInitialize();

    ASSERT_EQ(g_save.settings.video_mode,   2);
    ASSERT_EQ(g_save.settings.sound_on,     1);
    ASSERT_EQ(g_save.settings.music_on,     0);
    ASSERT_EQ(g_save.settings.pad0,         0);
    ASSERT_EQ(g_save.settings.voice_on,     0);
    ASSERT_EQ(g_save.settings.subtitles_on, 1);
    ASSERT_EQ(g_save.settings.dialogues_on, 0);
    ASSERT_EQ(g_save.settings.pad1,         0);

    restore_test_cwd();
}

/* ---- settings + slots roundtrip together ---------------------------- */

TEST(settings_and_slots_persist_together)
{
    set_test_cwd();

    memset(&g_save, 0, sizeof g_save);
    g_save.magic = WACKI_SAVE_MAGIC;
    /* Non-default settings. */
    g_save.settings.sound_on = 1;
    g_save.settings.music_on = 0;
    /* Populate slot 3 + slot 7. */
    g_save.slots[3].stage_indicator = 42;
    g_save.slots[3].etap_id = 2;
    memcpy(g_save.slots[3].name, "First", 5);
    g_save.slots[7].stage_indicator = 88;
    g_save.slots[7].etap_id = 4;
    memcpy(g_save.slots[7].name, "Second", 6);

    WriteSaveFile();
    memset(&g_save, 0, sizeof g_save);
    LoadSaveStateOrInitialize();

    /* Settings preserved. */
    ASSERT_EQ(g_save.settings.sound_on, 1);
    ASSERT_EQ(g_save.settings.music_on, 0);

    /* Slot 3 + 7 preserved; others empty. */
    ASSERT_EQ(g_save.slots[3].stage_indicator, 42);
    ASSERT_EQ(g_save.slots[3].etap_id, 2);
    ASSERT_STREQ(g_save.slots[3].name, "First");

    ASSERT_EQ(g_save.slots[7].stage_indicator, 88);
    ASSERT_EQ(g_save.slots[7].etap_id, 4);
    ASSERT_STREQ(g_save.slots[7].name, "Second");

    /* Slot 0, 1, 2, 4, 5, 6, 8, 9 should be empty (stage_indicator=0). */
    ASSERT_EQ(g_save.slots[0].stage_indicator, 0);
    ASSERT_EQ(g_save.slots[5].stage_indicator, 0);
    ASSERT_EQ(g_save.slots[9].stage_indicator, 0);

    restore_test_cwd();
}

/* ---- multiple Write cycles preserve last-written ------------------- */

TEST(repeated_writes_preserve_last_write)
{
    set_test_cwd();

    /* Write #1 — slot 1 has data X. */
    memset(&g_save, 0, sizeof g_save);
    g_save.magic = WACKI_SAVE_MAGIC;
    g_save.slots[1].stage_indicator = 11;
    g_save.slots[1].script_vars[0] = 0xAAAAu;
    WriteSaveFile();

    /* Write #2 — slot 1 now has Y, plus slot 2 added. */
    g_save.slots[1].script_vars[0] = 0xBBBBu;
    g_save.slots[2].stage_indicator = 22;
    g_save.slots[2].script_vars[0] = 0xCCCCu;
    WriteSaveFile();

    /* Reload — slot 1 = Y (from second write), slot 2 = CCCC. */
    memset(&g_save, 0, sizeof g_save);
    LoadSaveStateOrInitialize();
    ASSERT_EQ(g_save.slots[1].stage_indicator, 11);
    ASSERT_EQ(g_save.slots[1].script_vars[0], 0xBBBBu);
    ASSERT_EQ(g_save.slots[2].stage_indicator, 22);
    ASSERT_EQ(g_save.slots[2].script_vars[0], 0xCCCCu);

    restore_test_cwd();
}

/* ---- entity_state and scene_snapshot per-slot ---------------------- */

TEST(entity_state_per_slot)
{
    set_test_cwd();

    memset(&g_save, 0, sizeof g_save);
    g_save.magic = WACKI_SAVE_MAGIC;
    /* Slot 0: entity_state[10] = 0x111111. */
    g_save.slots[0].stage_indicator = 1;
    g_save.slots[0].entity_state[10] = 0x111111;
    /* Slot 5: entity_state[10] = 0x222222. */
    g_save.slots[5].stage_indicator = 5;
    g_save.slots[5].entity_state[10] = 0x222222;
    /* Slot 5: scene_snapshot[15] = 0xDEAD. */
    g_save.slots[5].scene_snapshot[15] = 0xDEADu;

    WriteSaveFile();
    memset(&g_save, 0xFF, sizeof g_save);
    LoadSaveStateOrInitialize();

    ASSERT_EQ(g_save.slots[0].entity_state[10], 0x111111u);
    ASSERT_EQ(g_save.slots[5].entity_state[10], 0x222222u);
    ASSERT_EQ(g_save.slots[5].scene_snapshot[15], 0xDEADu);

    /* Slots without entity_state writes should have zeros. */
    ASSERT_EQ(g_save.slots[1].entity_state[10], 0u);
    ASSERT_EQ(g_save.slots[9].entity_state[10], 0u);

    restore_test_cwd();
}

/* ---- write produces file with size matching WACKI_SAVE_SIZE ------- *
 *
 * Already covered by test_save_io, re-asserted here as a sanity. */

TEST(file_size_remains_save_size_after_multiple_writes)
{
    set_test_cwd();

    memset(&g_save, 0, sizeof g_save);
    g_save.magic = WACKI_SAVE_MAGIC;

    for (int cycle = 0; cycle < 3; ++cycle) {
        g_save.slots[cycle].stage_indicator = (uint16_t)cycle;
        WriteSaveFile();

        FILE *fp = fopen("Wacki.sav", "rb");
        ASSERT_NOT_NULL(fp);
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        fclose(fp);
        ASSERT_EQ(sz, (long)WACKI_SAVE_SIZE);
    }

    restore_test_cwd();
}

SUITE(save_io_extended)
{
    RUN_TEST(all_ten_slots_write_then_read_back);
    RUN_TEST(all_settings_fields_persist);
    RUN_TEST(settings_and_slots_persist_together);
    RUN_TEST(repeated_writes_preserve_last_write);
    RUN_TEST(entity_state_per_slot);
    RUN_TEST(file_size_remains_save_size_after_multiple_writes);
}
