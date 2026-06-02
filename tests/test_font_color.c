/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tests/test_font_color.c — Futura.30 color font path (sub-magic 0x3EA).
 *
 * test_font.c covers the 1bpp path (sub-magic 0x3E9, raw[0x70] & 0x40 == 0).
 * The COLOR variant (sub-magic 0x3EA) takes a different branch in
 * ParseFutFontFile:
 *
 *   plane_count = raw[0x90];
 *   for i in 0..plane_count:
 *     off = be32(raw + 0x9A + i*4);
 *     if off: plane[i] = cell_base + off;
 *
 * Multi-plane fonts encode each color bit in a separate bitmap plane;
 * the menu text in Wacki uses this format.
 *
 * Reference: src/font.c lines 88-100.
 */

#include "test.h"
#include "wacki.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct FontHandle;
typedef struct FontHandle FontHandle;
extern FontHandle *ParseFutFontFile(const uint8_t *raw);

/* Layout-matching struct so tests can cast the opaque FontHandle pointer.
 * MUST stay in sync with src/font.c struct FontHandle (lines 46-59). */
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
};

static void write_be16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v & 0xFF; }
static void write_be32(uint8_t *p, uint32_t v) {
    p[0] = (v >> 24) & 0xFF; p[1] = (v >> 16) & 0xFF;
    p[2] = (v >> 8)  & 0xFF; p[3] =  v        & 0xFF;
}

/* ---- color font with 4 planes ----------------------------------------- */

TEST(parse_color_font_with_3_planes)
{
    uint8_t raw[0x300];
    memset(raw, 0, sizeof raw);

    /* Common header (same as 1bpp test). */
    write_be32(raw + 0x00, 0x000003F3u);
    write_be32(raw + 0x18, 0x000003EAu);   /* sub-magic = COLOR */
    raw[0x62] = 0x0C;                       /* cell-width sentinel */
    write_be16(raw + 0x6E, 0x001E);         /* baseline */
    raw[0x70] = 0x40;                       /* color font flag */
    write_be16(raw + 0x72, 0x0008);         /* advance */
    write_be16(raw + 0x74, 0x000C);         /* cell_width */
    raw[0x7A] = 0x20;
    raw[0x7B] = 0x7F;
    write_be16(raw + 0x80, 0x0040);
    write_be32(raw + 0x82, 0x00000010u);    /* width_tab */
    write_be32(raw + 0x86, 0x00000020u);    /* advance_tab */
    write_be32(raw + 0x8A, 0x00000030u);    /* kern_tab */

    /* Color path: plane_count at raw[0x90]. */
    raw[0x90] = 3;
    /* plane offsets at +0x9A + i*4 (BE32). */
    write_be32(raw + 0x9A + 0*4, 0x00000100u);   /* plane[0] @ cell_base + 0x100 */
    write_be32(raw + 0x9A + 1*4, 0x00000180u);   /* plane[1] @ cell_base + 0x180 */
    write_be32(raw + 0x9A + 2*4, 0x00000200u);   /* plane[2] @ cell_base + 0x200 */

    FontHandle *f = ParseFutFontFile(raw);
    ASSERT_NOT_NULL(f);

    struct test_fonthandle *fh = (struct test_fonthandle *)f;

    ASSERT_EQ(fh->plane_count, 3);

    /* Each plane pointer = raw + 0x20 (cell_base) + offset. */
    ASSERT_EQ((uintptr_t)fh->plane[0], (uintptr_t)(raw + 0x20 + 0x100));
    ASSERT_EQ((uintptr_t)fh->plane[1], (uintptr_t)(raw + 0x20 + 0x180));
    ASSERT_EQ((uintptr_t)fh->plane[2], (uintptr_t)(raw + 0x20 + 0x200));

    /* width_tab / advance_tab / kern_tab still resolved from same fields. */
    ASSERT_EQ((uintptr_t)fh->width_tab,   (uintptr_t)(raw + 0x20 + 0x10));
    ASSERT_EQ((uintptr_t)fh->advance_tab, (uintptr_t)(raw + 0x20 + 0x20));
    ASSERT_EQ((uintptr_t)fh->kern_tab,    (uintptr_t)(raw + 0x20 + 0x30));

    free(f);
}

TEST(parse_color_font_zero_offset_plane_skipped)
{
    /* If a plane's offset is 0, the loader leaves `plane[i] = NULL`.
     * Common for sparse fonts where only some color planes are populated. */
    uint8_t raw[0x300];
    memset(raw, 0, sizeof raw);

    write_be32(raw + 0x00, 0x000003F3u);
    write_be32(raw + 0x18, 0x000003EAu);
    raw[0x62] = 0x0C;
    raw[0x70] = 0x40;                       /* color */
    raw[0x90] = 2;                          /* 2 planes */
    /* plane[0] offset = 0x100; plane[1] offset = 0 (skipped). */
    write_be32(raw + 0x9A + 0*4, 0x00000100u);
    write_be32(raw + 0x9A + 1*4, 0x00000000u);

    FontHandle *f = ParseFutFontFile(raw);
    ASSERT_NOT_NULL(f);

    struct test_fonthandle *fh = (struct test_fonthandle *)f;
    ASSERT_EQ(fh->plane_count, 2);
    ASSERT_EQ((uintptr_t)fh->plane[0], (uintptr_t)(raw + 0x20 + 0x100));
    ASSERT_NULL(fh->plane[1]);

    free(f);
}

TEST(parse_color_font_sub_magic_3EA_accepted)
{
    /* Both sub-magics 0x3E9 (1bpp) and 0x3EA (color) are accepted; verify
     * 0x3EA passes the loader's sub-magic check. */
    uint8_t raw[0x200];
    memset(raw, 0, sizeof raw);
    write_be32(raw + 0x00, 0x000003F3u);
    write_be32(raw + 0x18, 0x000003EAu);   /* color */
    raw[0x62] = 0x0C;
    raw[0x70] = 0x40;                       /* color font */
    raw[0x90] = 1;
    write_be32(raw + 0x9A, 0x00000050u);    /* plane[0] offset */

    FontHandle *f = ParseFutFontFile(raw);
    ASSERT_NOT_NULL(f);

    free(f);
}

TEST(parse_rejects_color_subfix_when_flag_not_set)
{
    /* Edge case: sub-magic 0x3EA but raw[0x70] & 0x40 == 0 → falls into
     * the 1bpp branch (which only sets plane[0] from raw+0x7C). This
     * is allowed by the loader — it accepts both sub-magics and gates
     * the color path purely on the byte flag, not the magic. */
    uint8_t raw[0x200];
    memset(raw, 0, sizeof raw);
    write_be32(raw + 0x00, 0x000003F3u);
    write_be32(raw + 0x18, 0x000003EAu);
    raw[0x62] = 0x0C;
    raw[0x70] = 0x00;                       /* NOT color */
    write_be32(raw + 0x7C, 0x00000060u);    /* 1bpp plane offset */

    FontHandle *f = ParseFutFontFile(raw);
    ASSERT_NOT_NULL(f);

    struct test_fonthandle *fh = (struct test_fonthandle *)f;
    ASSERT_EQ(fh->plane_count, 1);
    ASSERT_EQ((uintptr_t)fh->plane[0], (uintptr_t)(raw + 0x20 + 0x60));

    free(f);
}

SUITE(font_color)
{
    RUN_TEST(parse_color_font_with_3_planes);
    RUN_TEST(parse_color_font_zero_offset_plane_skipped);
    RUN_TEST(parse_color_font_sub_magic_3EA_accepted);
    RUN_TEST(parse_rejects_color_subfix_when_flag_not_set);
}
