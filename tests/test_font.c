/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tests/test_font.c — Futura.30 parser + tagged-table walker.
 *
 * Coverage:
 *   - FindKeyInTaggedTable (FUN_00401240) — used by .scr loader to
 *     locate sections like `[komnata]N` by walking length-prefixed
 *     records terminated with '!'. Pure function, fully testable.
 *   - ParseFutFontFile (FUN_00413690) — Big-Endian header parser for
 *     the Futura.30 bitmap font. Hand-built fixture exercises the
 *     1bpp path (most common in the original game).
 *
 * Reference: src/font.c.
 */

#include "test.h"
#include "wacki.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* External symbols from font.c (not in wacki.h public API). */
struct FontHandle;
typedef struct FontHandle FontHandle;
extern FontHandle *ParseFutFontFile(const uint8_t *raw);
extern uint16_t FindKeyInTaggedTable(const char *table, char tag, int16_t key);

/* ---- FindKeyInTaggedTable ---------------------------------------------- *
 * Format:
 *   each record = [tag:u8 at +0][len:u8 at +1][key:i16 at +2][pad to len*2]
 *   advance by `len * 2` bytes between records
 *   idx accumulates `len` per record
 *   stop on tag == '!'
 *
 * Returns idx of the FIRST matching record (tag matches AND
 * (key == -1 OR record's key matches)). Returns 0 on miss. */

TEST(tag_table_finds_first_match)
{
    /* Three records: tag='A' key=1 (skipped), tag='B' key=42 (skipped because
     * different tag), tag='A' key=42 (MATCH).
     *
     * Each record stride = len * 2. We use len=2 so records are 4 bytes:
     *   [tag][len=2][key_lo][key_hi]
     */
    uint8_t table[] = {
        /* record 0: tag='A' len=2 key=1 (skipped) */
        'A', 2, 0x01, 0x00,
        /* record 1: tag='B' len=2 key=42 (wrong tag) */
        'B', 2, 0x2A, 0x00,
        /* record 2: tag='A' len=2 key=42 (MATCH — idx=4) */
        'A', 2, 0x2A, 0x00,
        /* terminator */
        '!', 0, 0, 0,
    };
    uint16_t idx = FindKeyInTaggedTable((const char *)table, 'A', 42);
    /* idx accumulates `len` per record. Records 0 and 1 advance idx by 2 each
     * → idx == 4 at the matching record. */
    ASSERT_EQ(idx, 4);
}

TEST(tag_table_wildcard_key_matches_first_with_tag)
{
    /* key == -1 → match any key, just need the tag. First 'X' record wins. */
    uint8_t table[] = {
        'Y', 2, 0,   0,
        'X', 2, 0x55, 0,
        'X', 2, 0x77, 0,
        '!', 0, 0,   0,
    };
    uint16_t idx = FindKeyInTaggedTable((const char *)table, 'X', -1);
    ASSERT_EQ(idx, 2);   /* second record, idx accumulated by 2 from first */
}

TEST(tag_table_miss_returns_zero)
{
    uint8_t table[] = {
        'A', 2, 0, 0,
        'B', 2, 0, 0,
        '!', 0, 0, 0,
    };
    uint16_t idx = FindKeyInTaggedTable((const char *)table, 'Z', 5);
    ASSERT_EQ(idx, 0);
}

TEST(tag_table_empty_returns_zero)
{
    uint8_t table[] = { '!', 0, 0, 0 };
    uint16_t idx = FindKeyInTaggedTable((const char *)table, 'A', 1);
    ASSERT_EQ(idx, 0);
}

/* ---- ParseFutFontFile -------------------------------------------------- *
 * The font has a 32-byte cell_base origin at raw+0x20. All plane / table
 * offsets are relative to cell_base. We build a fixture with:
 *   - magic 0x000003F3 at +0x00 (BE)
 *   - sub-magic 0x000003E9 at +0x18 (BE, 1bpp variant)
 *   - cell-width sentinel 0x0C at +0x62
 *   - flag byte 0x00 at +0x70 (NOT color font → 1bpp path)
 *   - baseline = 0x001E at +0x6E (BE16)
 *   - advance = 0x0008 at +0x72 (BE16)
 *   - cell_width = 0x000C at +0x74 (BE16)
 *   - first_char = 0x20 at +0x7A
 *   - last_char  = 0x7F at +0x7B
 *   - plane[0] offset = 0x0100 (BE32) at +0x7C
 *   - glyph_stride = 0x0040 (BE16) at +0x80
 *   - width_tab offset   = 0x0010 (BE32) at +0x82
 *   - advance_tab offset = 0x0020 (BE32) at +0x86
 *   - kern_tab offset    = 0x0030 (BE32) at +0x8A
 *
 * Buffer must include cell_base (raw + 0x20) + the largest pointed-to
 * offset; plane[0] at offset 0x100 means buffer >= 0x20 + 0x100 = 0x120. */

static void write_be16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v & 0xFF; }
static void write_be32(uint8_t *p, uint32_t v) {
    p[0] = (v >> 24) & 0xFF; p[1] = (v >> 16) & 0xFF;
    p[2] = (v >> 8)  & 0xFF; p[3] =  v        & 0xFF;
}

TEST(parse_minimal_1bpp_font)
{
    uint8_t raw[0x200];
    memset(raw, 0, sizeof raw);

    write_be32(raw + 0x00, 0x000003F3u);   /* main magic */
    write_be32(raw + 0x18, 0x000003E9u);   /* sub-magic = 1bpp */
    raw[0x62] = 0x0C;                       /* cell-width sentinel */
    write_be16(raw + 0x6E, 0x001E);         /* baseline */
    raw[0x70] = 0x00;                       /* not color → 1bpp */
    write_be16(raw + 0x72, 0x0008);         /* advance */
    write_be16(raw + 0x74, 0x000C);         /* cell_width */
    raw[0x7A] = 0x20;                       /* first_char */
    raw[0x7B] = 0x7F;                       /* last_char */
    write_be32(raw + 0x7C, 0x00000100u);    /* plane[0] offset (rel to cell_base) */
    write_be16(raw + 0x80, 0x0040);         /* glyph_stride */
    write_be32(raw + 0x82, 0x00000010u);    /* width_tab offset */
    write_be32(raw + 0x86, 0x00000020u);    /* advance_tab offset */
    write_be32(raw + 0x8A, 0x00000030u);    /* kern_tab offset */

    FontHandle *f = ParseFutFontFile(raw);
    ASSERT_NOT_NULL(f);

    /* Cast back to a struct we know the layout of (mirrors src/font.c). */
    struct test_fonthandle {
        uint16_t advance;
        uint16_t baseline;
        uint16_t glyph_stride;
        uint16_t cell_width;
        uint8_t  first_char;
        uint8_t  last_char;
        uint8_t *plane[8];
        uint8_t *width_tab;
        uint8_t *advance_tab;
        uint8_t *kern_tab;
        uint16_t plane_count;
    } *fh = (struct test_fonthandle *)f;

    ASSERT_EQ(fh->advance,       0x0008);
    ASSERT_EQ(fh->baseline,      0x001E);
    ASSERT_EQ(fh->glyph_stride,  0x0040);
    ASSERT_EQ(fh->cell_width,    0x000C);
    ASSERT_EQ(fh->first_char,    0x20);
    ASSERT_EQ(fh->last_char,     0x7F);
    ASSERT_EQ(fh->plane_count,   1);

    /* plane[0] = cell_base + 0x100 = raw + 0x20 + 0x100 = raw + 0x120 */
    ASSERT_EQ((uintptr_t)fh->plane[0],   (uintptr_t)(raw + 0x120));
    ASSERT_EQ((uintptr_t)fh->width_tab,  (uintptr_t)(raw + 0x30));
    ASSERT_EQ((uintptr_t)fh->advance_tab,(uintptr_t)(raw + 0x40));
    ASSERT_EQ((uintptr_t)fh->kern_tab,   (uintptr_t)(raw + 0x50));

    free(f);
}

TEST(parse_rejects_bad_main_magic)
{
    uint8_t raw[0x200];
    memset(raw, 0, sizeof raw);
    write_be32(raw + 0x00, 0xDEADBEEFu);   /* wrong main magic */
    write_be32(raw + 0x18, 0x000003E9u);
    raw[0x62] = 0x0C;

    FontHandle *f = ParseFutFontFile(raw);
    ASSERT_NULL(f);
}

TEST(parse_rejects_bad_sub_magic)
{
    uint8_t raw[0x200];
    memset(raw, 0, sizeof raw);
    write_be32(raw + 0x00, 0x000003F3u);
    write_be32(raw + 0x18, 0x0000FFFFu);   /* not 3E9 or 3EA */
    raw[0x62] = 0x0C;

    FontHandle *f = ParseFutFontFile(raw);
    ASSERT_NULL(f);
}

TEST(parse_rejects_bad_cell_sentinel)
{
    uint8_t raw[0x200];
    memset(raw, 0, sizeof raw);
    write_be32(raw + 0x00, 0x000003F3u);
    write_be32(raw + 0x18, 0x000003E9u);
    raw[0x62] = 0x0B;   /* should be 0x0C */

    FontHandle *f = ParseFutFontFile(raw);
    ASSERT_NULL(f);
}

SUITE(font)
{
    RUN_TEST(tag_table_finds_first_match);
    RUN_TEST(tag_table_wildcard_key_matches_first_with_tag);
    RUN_TEST(tag_table_miss_returns_zero);
    RUN_TEST(tag_table_empty_returns_zero);
    RUN_TEST(parse_minimal_1bpp_font);
    RUN_TEST(parse_rejects_bad_main_magic);
    RUN_TEST(parse_rejects_bad_sub_magic);
    RUN_TEST(parse_rejects_bad_cell_sentinel);
}
