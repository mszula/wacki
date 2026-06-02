/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tests/test_save_io.c — Wacki.sav file I/O roundtrip.
 *
 * src/save.c uses fopen("Wacki.sav", "rb"/"wb") with a relative path
 * baked in via WACKI_SAVE_FILE. To test without polluting the project
 * directory we chdir into a temp dir before each test and restore
 * the original cwd after.
 *
 * Coverage:
 *   - LoadSaveStateOrInitialize sets correct defaults when no file exists
 *     (sound_on=1, music_on=1, voice_on=1 — port default: SFX on at
 *     first launch since there's no in-game UI to toggle sound_on)
 *   - "Pusty" name on every slot
 *   - WriteSaveFile + LoadSaveStateOrInitialize roundtrip preserves bytes
 *   - Atomic write: Wacki.sav.tmp doesn't leak on success
 *   - Existing valid file is loaded (magic check)
 *
 * Reference: src/save.c.
 */

#include "test.h"
#include "wacki.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>           /* chdir, getcwd */
#include <sys/stat.h>         /* mkdir */

/* Externs from save.c — these are not in wacki.h. */
extern WackiSaveFile g_save;
extern void LoadSaveStateOrInitialize(void);
extern void WriteSaveFile(void);

/* save.c also pulls extern uint32_t g_script_vars[0x129], extern uint16_t
 * g_active_actor / g_cur_etap / g_cur_komnata, extern uint32_t
 * g_entity_state[0x11C], extern uint32_t g_scene_snapshot[0x1E], extern
 * int LoadStage(...) — these are referenced by LoadSaveSlot /
 * QuickSaveToSlot / QuickLoadFromSlot, which we do NOT test here. But the
 * linker still needs the symbols. Stubs live in test_engine_stubs.c. */

/* ---- helpers ------------------------------------------------------------ */

static char s_orig_cwd[1024];
static char s_tmp_dir[1024];

static void set_test_cwd(void)
{
    /* Make a fresh per-test temp dir under /tmp; chdir into it. */
    if (!getcwd(s_orig_cwd, sizeof s_orig_cwd)) s_orig_cwd[0] = 0;
    snprintf(s_tmp_dir, sizeof s_tmp_dir,
             "/tmp/wacki-test-save-%d", (int)getpid());
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

/* ---- tests -------------------------------------------------------------- */

TEST(load_with_no_file_zeroes_then_sets_defaults)
{
    set_test_cwd();

    /* Scribble random data into g_save first — defaults must overwrite. */
    memset(&g_save, 0xAB, sizeof g_save);
    LoadSaveStateOrInitialize();

    /* Magic */
    ASSERT_EQ(g_save.magic, WACKI_SAVE_MAGIC);

    /* Port default: sound_on=1 so SFX work out of the box (the port has
     * no in-game UI to toggle sound_on, unlike the original engine's
     * Solund→Sound button). */
    ASSERT_EQ(g_save.settings.video_mode,   1);
    ASSERT_EQ(g_save.settings.sound_on,     1);
    ASSERT_EQ(g_save.settings.music_on,     1);
    ASSERT_EQ(g_save.settings.voice_on,     1);
    ASSERT_EQ(g_save.settings.subtitles_on, 1);
    ASSERT_EQ(g_save.settings.dialogues_on, 1);
    ASSERT_EQ(g_save.settings.pad0,         0);
    ASSERT_EQ(g_save.settings.pad1,         0);

    /* Every slot's name is "Pusty" + NUL. */
    for (int i = 0; i < WACKI_SAVE_SLOTS; ++i) {
        ASSERT_STREQ(g_save.slots[i].name, WACKI_DEFAULT_SLOT_NAME);
        /* stage_indicator = 0 → slot is "empty" */
        ASSERT_EQ(g_save.slots[i].stage_indicator, 0);
        ASSERT_EQ(g_save.slots[i].etap_id, 0);
    }

    restore_test_cwd();
}

TEST(write_then_load_roundtrip_preserves_state)
{
    set_test_cwd();

    /* Build a save state by hand and write. */
    memset(&g_save, 0, sizeof g_save);
    g_save.magic = WACKI_SAVE_MAGIC;
    g_save.settings.sound_on = 1;
    g_save.settings.music_on = 0;
    g_save.slots[3].stage_indicator = 42;
    g_save.slots[3].etap_id         = 2;
    memcpy(g_save.slots[3].name, "MyTestSlot", 10);
    /* Put a marker in script_vars[5] so we can verify it survives. */
    g_save.slots[3].script_vars[5] = 0xDEADBEEFu;

    WriteSaveFile();

    /* Clear in-memory image, then load from disk. */
    memset(&g_save, 0, sizeof g_save);
    LoadSaveStateOrInitialize();

    ASSERT_EQ(g_save.magic, WACKI_SAVE_MAGIC);
    ASSERT_EQ(g_save.settings.sound_on, 1);
    ASSERT_EQ(g_save.settings.music_on, 0);
    ASSERT_EQ(g_save.slots[3].stage_indicator, 42);
    ASSERT_EQ(g_save.slots[3].etap_id, 2);
    ASSERT_STREQ(g_save.slots[3].name, "MyTestSlot");
    ASSERT_EQ(g_save.slots[3].script_vars[5], 0xDEADBEEFu);

    /* Other slots stayed empty. */
    ASSERT_EQ(g_save.slots[0].stage_indicator, 0);
    ASSERT_EQ(g_save.slots[7].stage_indicator, 0);

    restore_test_cwd();
}

TEST(write_does_not_leak_tmp_file)
{
    set_test_cwd();

    memset(&g_save, 0, sizeof g_save);
    g_save.magic = WACKI_SAVE_MAGIC;
    WriteSaveFile();

    /* After a successful WriteSaveFile, Wacki.sav.tmp must NOT exist
     * (T131 atomic rename) — but Wacki.sav must. */
    FILE *fp_real = fopen("Wacki.sav", "rb");
    ASSERT_NOT_NULL(fp_real);
    fclose(fp_real);

    FILE *fp_tmp = fopen("Wacki.sav.tmp", "rb");
    ASSERT_NULL(fp_tmp);
    if (fp_tmp) fclose(fp_tmp);

    restore_test_cwd();
}

TEST(loading_corrupt_magic_falls_back_to_defaults)
{
    set_test_cwd();

    /* Create a Wacki.sav with the right size but WRONG magic. */
    FILE *fp = fopen("Wacki.sav", "wb");
    ASSERT_NOT_NULL(fp);
    uint8_t garbage[WACKI_SAVE_SIZE];
    memset(garbage, 0xCC, sizeof garbage);
    /* Stamp WRONG magic. */
    uint32_t bad_magic = 0xDEADBEEFu;
    memcpy(garbage, &bad_magic, 4);
    fwrite(garbage, 1, sizeof garbage, fp);
    fclose(fp);

    LoadSaveStateOrInitialize();

    /* Should fall back to defaults — magic restored to SAVE. */
    ASSERT_EQ(g_save.magic, WACKI_SAVE_MAGIC);
    ASSERT_STREQ(g_save.slots[0].name, WACKI_DEFAULT_SLOT_NAME);

    restore_test_cwd();
}

TEST(loading_short_file_falls_back)
{
    set_test_cwd();

    /* File exists but is too short — loader must reject. */
    FILE *fp = fopen("Wacki.sav", "wb");
    ASSERT_NOT_NULL(fp);
    uint32_t magic = WACKI_SAVE_MAGIC;
    fwrite(&magic, 1, 4, fp);              /* only 4 bytes */
    fclose(fp);

    /* Pre-scribble to detect reset. */
    memset(&g_save, 0x55, sizeof g_save);
    LoadSaveStateOrInitialize();

    ASSERT_EQ(g_save.magic, WACKI_SAVE_MAGIC);
    /* Defaults must be installed (port default: sound_on = 1). */
    ASSERT_EQ(g_save.settings.sound_on, 1);

    restore_test_cwd();
}

TEST(write_produces_exactly_save_size_bytes)
{
    set_test_cwd();

    memset(&g_save, 0, sizeof g_save);
    g_save.magic = WACKI_SAVE_MAGIC;
    WriteSaveFile();

    FILE *fp = fopen("Wacki.sav", "rb");
    ASSERT_NOT_NULL(fp);
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fclose(fp);

    ASSERT_EQ(sz, (long)WACKI_SAVE_SIZE);

    restore_test_cwd();
}

SUITE(save_io)
{
    RUN_TEST(load_with_no_file_zeroes_then_sets_defaults);
    RUN_TEST(write_then_load_roundtrip_preserves_state);
    RUN_TEST(write_does_not_leak_tmp_file);
    RUN_TEST(loading_corrupt_magic_falls_back_to_defaults);
    RUN_TEST(loading_short_file_falls_back);
    RUN_TEST(write_produces_exactly_save_size_bytes);
}
