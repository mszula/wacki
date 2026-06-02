/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tests/test_entity_layout.c — struct layout invariants.
 *
 * THE MOST CRITICAL TEST IN THE WHOLE SUITE.
 *
 * The script VM (RunScriptInterpreter + per-entity FUN_004012E0) writes
 * to RAW byte offsets via `*(T *)((uint8_t *)e + N)`. Those offsets are
 * load-bearing — they are the ORIGINAL 32-bit engine's struct layout.
 * If anyone "innocently" reorders a field, adds a field in the wrong
 * spot, or changes a pad, the VM starts corrupting random fields.
 *
 * These checks run at COMPILE TIME (_Static_assert) so they cannot
 * regress silently. At test time we just smoke-check that the file
 * compiles and the tests exist.
 *
 * Reference: include/wacki.h Entity comment + lessons learned in
 * CLAUDE.md (64-bit pointer traps section).
 */

#include "test.h"
#include "wacki.h"

#include <stddef.h>   /* offsetof */

/* ============ Entity ===================================================== */
/* Script-byte range — these MUST match the original 32-bit engine layout. */

_Static_assert(offsetof(Entity, flags1)      == 0x08,
               "Entity.flags1 must be at byte +0x08 (script writes here)");
_Static_assert(offsetof(Entity, flags2)      == 0x09,
               "Entity.flags2 must be at byte +0x09");
_Static_assert(offsetof(Entity, cur_anim_id) == 0x0A,
               "Entity.cur_anim_id must be at byte +0x0A");
_Static_assert(offsetof(Entity, cur_anim_y)  == 0x0C,
               "Entity.cur_anim_y must be at byte +0x0C");
_Static_assert(offsetof(Entity, width)       == 0x0E,
               "Entity.width must be at byte +0x0E");
_Static_assert(offsetof(Entity, height)      == 0x10,
               "Entity.height must be at byte +0x10");

/* Trailing zone — native pointers MUST live past byte 0xE0 so that script
 * writes in the +0x00..+0x66 range cannot corrupt them. See #100 fix in
 * CLAUDE.md (pixels collision bug). */
_Static_assert(offsetof(Entity, pixels)      == 0xE0,
               "Entity.pixels native pointer must be past script-byte zone (+0xE0)");
_Static_assert(offsetof(Entity, kind)        == 0xE8,
               "Entity.kind port-internal must follow pixels");
_Static_assert(offsetof(Entity, group_flags) == 0xEA,
               "Entity.group_flags port-internal at +0xEA");

/* ============ WackiSlot / WackiSaveFile ================================== */
/* These are wire format — must exactly match the original .sav file on disk. */

_Static_assert(sizeof(WackiSlot) == WACKI_SLOT_SIZE,
               "WackiSlot must be exactly 0x3012 bytes (matches Wacki.sav layout)");
_Static_assert(WACKI_SLOT_SIZE == 0x3012,
               "WACKI_SLOT_SIZE constant must be 0x3012");
_Static_assert(WACKI_SAVE_SLOTS == 10,
               "WACKI_SAVE_SLOTS must be 10");
_Static_assert(WACKI_SAVE_SIZE == 0x1E0C0,
               "WACKI_SAVE_SIZE must be 0x1E0C0 (= 4 + 8 + 10 * 0x3012)");

/* Field-by-field offsets inside WackiSlot — these are read by
 * LoadSaveSlot when restoring g_script_vars / g_entity_state etc.
 * If anyone changes the array sizes the offsets break. */
_Static_assert(offsetof(WackiSlot, stage_indicator)         == 0x0000,
               "WackiSlot.stage_indicator at +0x00");
_Static_assert(offsetof(WackiSlot, etap_id)                 == 0x0002,
               "WackiSlot.etap_id at +0x02");
_Static_assert(offsetof(WackiSlot, name)                    == 0x0004,
               "WackiSlot.name at +0x04");
_Static_assert(offsetof(WackiSlot, script_vars)             == 0x0022,
               "WackiSlot.script_vars at +0x22 (after 0x1E name bytes)");

/* WackiSaveFile total layout — magic (4) + settings (8) + slots (10 × 0x3012). */
_Static_assert(offsetof(WackiSaveFile, magic)    == 0x0000,
               "WackiSaveFile.magic at +0x00");
_Static_assert(offsetof(WackiSaveFile, settings) == 0x0004,
               "WackiSaveFile.settings at +0x04");
_Static_assert(offsetof(WackiSaveFile, slots)    == 0x000C,
               "WackiSaveFile.slots[] at +0x0C (after magic + settings)");
_Static_assert(sizeof(WackiSaveFile) == WACKI_SAVE_SIZE,
               "WackiSaveFile total size must match WACKI_SAVE_SIZE");

_Static_assert(sizeof(WackiSettings) == 8,
               "WackiSettings must be 8 bytes (matches original layout)");

/* ============ DtaIndexEntry / DtaFileHeader / Pkv2Header ================= */
/* Wire format for .dta archive directory + compressed file header. */

_Static_assert(sizeof(DtaIndexEntry) == DTA_DIR_ENTRY_SZ,
               "DtaIndexEntry must be 16 bytes");
_Static_assert(DTA_DIR_ENTRY_SZ == 16,
               "DTA_DIR_ENTRY_SZ must be 16");
_Static_assert(DTA_NAME_LEN == 12,
               "DTA_NAME_LEN must be 12");
_Static_assert(offsetof(DtaIndexEntry, name)        == 0,
               "DtaIndexEntry.name at +0");
_Static_assert(offsetof(DtaIndexEntry, file_offset) == 12,
               "DtaIndexEntry.file_offset at +12");

_Static_assert(sizeof(DtaFileHeader) == 12,
               "DtaFileHeader must be 12 bytes");
_Static_assert(sizeof(Pkv2Header)    == 12,
               "Pkv2Header must be 12 bytes");

/* ============ Magic constants ============================================ */
/* These are little-endian dword views of 4-byte ASCII tags. */

_Static_assert(DTA_MAGIC_BASE    == 0x45534142u, "'BASE' = 0x45534142");
_Static_assert(DTA_MAGIC_SPIS    == 0x53495053u, "'SPIS' = 0x53495053");
_Static_assert(PKV2_MAGIC        == 0x32764B50u, "'PKv2' = 0x32764B50");
_Static_assert(ASSET_MAGIC_ANIM  == 0x4D494E41u, "'ANIM' = 0x4D494E41");
_Static_assert(ASSET_MAGIC_MASK  == 0x4B53414Du, "'MASK' = 0x4B53414D");
_Static_assert(ASSET_MAGIC_FILD  == 0x444C4946u, "'FILD' = 0x444C4946");
_Static_assert(WACKI_SAVE_MAGIC  == 0x45564153u, "'SAVE' = 0x45564153");

/* ============ Runtime smoke tests ======================================== */

TEST(entity_size_at_least_e0)
{
    /* Pixels native pointer must be at +0xE0; sizeof(Entity) must reach it. */
    ASSERT_TRUE(sizeof(Entity) >= 0xE0 + sizeof(uint8_t *));
}

TEST(wacki_settings_default_bytes)
{
    /* Confirm zero-init yields zero bytes — assumed by LoadSaveStateOrInitialize. */
    WackiSettings s = { 0, 0, 0, 0, 0, 0, 0, 0 };
    const uint8_t *b = (const uint8_t *)&s;
    for (size_t i = 0; i < sizeof s; ++i) {
        ASSERT_EQ(b[i], 0);
    }
}

TEST(magic_strings_are_ascii)
{
    /* "BASE" should be readable as bytes B-A-S-E in little-endian dword view. */
    uint32_t base = DTA_MAGIC_BASE;
    ASSERT_EQ(((const char *)&base)[0], 'B');
    ASSERT_EQ(((const char *)&base)[1], 'A');
    ASSERT_EQ(((const char *)&base)[2], 'S');
    ASSERT_EQ(((const char *)&base)[3], 'E');

    uint32_t pkv2 = PKV2_MAGIC;
    ASSERT_EQ(((const char *)&pkv2)[0], 'P');
    ASSERT_EQ(((const char *)&pkv2)[1], 'K');
    ASSERT_EQ(((const char *)&pkv2)[2], 'v');
    ASSERT_EQ(((const char *)&pkv2)[3], '2');

    uint32_t save = WACKI_SAVE_MAGIC;
    ASSERT_EQ(((const char *)&save)[0], 'S');
    ASSERT_EQ(((const char *)&save)[1], 'A');
    ASSERT_EQ(((const char *)&save)[2], 'V');
    ASSERT_EQ(((const char *)&save)[3], 'E');
}

SUITE(entity_layout)
{
    RUN_TEST(entity_size_at_least_e0);
    RUN_TEST(wacki_settings_default_bytes);
    RUN_TEST(magic_strings_are_ascii);
}
