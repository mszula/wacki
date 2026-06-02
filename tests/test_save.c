/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tests/test_save.c — Wacki.sav wire format.
 *
 * The .sav format is on-disk wire format — any reorder / resize of
 * fields would break reading old saves. The struct layout is enforced
 * at compile time (`wacki_slot_size_check` in wacki.h), but here we
 * pin:
 *   - The save magic = "SAVE" little-endian
 *   - Total file size = 0x1E0C0 = 122 560 bytes
 *   - Settings byte layout
 *   - Slot layout / offsets
 *   - Constants used by WriteSaveFile / LoadSaveStateOrInitialize
 *
 * Reference: src/save.c + include/wacki.h save constants.
 */

#include "test.h"
#include "wacki.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---- constants pinning -------------------------------------------------- */

TEST(slot_size_constant)
{
    ASSERT_EQ(WACKI_SLOT_SIZE, 0x3012);
    ASSERT_EQ(sizeof(WackiSlot), WACKI_SLOT_SIZE);
}

TEST(save_file_total_size)
{
    /* 4 (magic) + 8 (settings) + 10 × 0x3012 (slots) = 0x1E0C0. */
    ASSERT_EQ(WACKI_SAVE_SIZE, 0x1E0C0);
    ASSERT_EQ(sizeof(WackiSaveFile), WACKI_SAVE_SIZE);
}

TEST(slot_count_is_ten)
{
    ASSERT_EQ(WACKI_SAVE_SLOTS, 10);
}

TEST(save_magic_is_ascii_SAVE_le)
{
    uint32_t m = WACKI_SAVE_MAGIC;
    const char *p = (const char *)&m;
    ASSERT_EQ(p[0], 'S');
    ASSERT_EQ(p[1], 'A');
    ASSERT_EQ(p[2], 'V');
    ASSERT_EQ(p[3], 'E');
}

/* ---- field offsets inside a slot --------------------------------------- */

TEST(slot_field_offsets)
{
    /* These are the offsets the loader uses when restoring a slot.
     * If anyone resizes script_vars[] / entity_state[] / etc., either
     * the slot grows past 0x3012 (compile error) or fields shift. */
    ASSERT_EQ(offsetof(WackiSlot, stage_indicator),         0x0000);
    ASSERT_EQ(offsetof(WackiSlot, etap_id),                 0x0002);
    ASSERT_EQ(offsetof(WackiSlot, name),                    0x0004);
    ASSERT_EQ(offsetof(WackiSlot, script_vars),             0x0022);

    /* entity_state immediately follows 0x129 × 4 = 0x4A4 bytes of
     * script_vars; verify by arithmetic. */
    ASSERT_EQ(offsetof(WackiSlot, entity_state),
              offsetof(WackiSlot, script_vars) + 0x129 * 4);
}

TEST(save_file_header_offsets)
{
    ASSERT_EQ(offsetof(WackiSaveFile, magic),    0x0000);
    ASSERT_EQ(offsetof(WackiSaveFile, settings), 0x0004);
    ASSERT_EQ(offsetof(WackiSaveFile, slots),    0x000C);
}

/* ---- WackiSettings byte layout ----------------------------------------- */

TEST(settings_struct_is_8_bytes)
{
    ASSERT_EQ(sizeof(WackiSettings), 8);
}

TEST(settings_field_order)
{
    /* The order must match the on-disk byte stream the original engine
     * wrote — video_mode, sound_on, music_on, pad, voice_on, subtitles,
     * dialogues, pad. */
    WackiSettings s = { 0 };
    s.video_mode = 1;
    s.sound_on = 2;
    s.music_on = 3;
    s.voice_on = 5;
    s.subtitles_on = 6;
    s.dialogues_on = 7;

    const uint8_t *b = (const uint8_t *)&s;
    ASSERT_EQ(b[0], 1);
    ASSERT_EQ(b[1], 2);
    ASSERT_EQ(b[2], 3);
    /* b[3] = pad0 = 0 */
    ASSERT_EQ(b[3], 0);
    ASSERT_EQ(b[4], 5);
    ASSERT_EQ(b[5], 6);
    ASSERT_EQ(b[6], 7);
    /* b[7] = pad1 = 0 */
    ASSERT_EQ(b[7], 0);
}

/* ---- default slot name -------------------------------------------------- */

TEST(default_slot_name_is_pusty)
{
    /* "Pusty" is the empty-slot label shown in the original load menu. */
    ASSERT_STREQ(WACKI_DEFAULT_SLOT_NAME, "Pusty");
}

SUITE(save)
{
    RUN_TEST(slot_size_constant);
    RUN_TEST(save_file_total_size);
    RUN_TEST(slot_count_is_ten);
    RUN_TEST(save_magic_is_ascii_SAVE_le);
    RUN_TEST(slot_field_offsets);
    RUN_TEST(save_file_header_offsets);
    RUN_TEST(settings_struct_is_8_bytes);
    RUN_TEST(settings_field_order);
    RUN_TEST(default_slot_name_is_pusty);
}
