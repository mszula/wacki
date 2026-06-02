/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tests/test_graphics.c — graphics primitives that don't need SDL.
 *
 * Covers the pure-function part of src/graphics.c:
 *   - DepackRleFrame   (FUN_00410cb0) — RLE decoder for ANIM kind=3 frames
 *   - InstallPalette   (FUN_00412D10) — palette copy + LUT rebuild
 *   - SetAlphaTint                    — tint encoding (0x808080 = identity)
 *
 * Skips the actual blit path which writes into g_back_shadow and would
 * pull half the engine in. Coverage for the blitter is best done via
 * the existing smoke runner (`tools/smoke-runner.sh`).
 *
 * Reference: src/graphics.c.
 */

#include "test.h"
#include "wacki.h"

#include <string.h>

/* ---- DepackRleFrame: RLE decoder for ANIM kind=3 frames ----------------- */
/* Stream rules (from src/graphics.c:342+):
 *   header[0] = fill_value
 *   header[1] = marker_A → next byte = count-1; emit count × fill_value
 *   header[2] = marker_B → next byte = count-1; next byte = value; emit count × value
 *   else → emit byte once
 * Loop until dst_len bytes written. */

TEST(rle_literal_passthrough)
{
    /* fill=0, A=0xAA, B=0xBB. Stream is plain literals (none match A/B). */
    const uint8_t src[] = { 0x00, 0xAA, 0xBB,
                            0x10, 0x20, 0x30, 0x40, 0x50 };
    uint8_t dst[5] = { 0 };
    DepackRleFrame(src, dst, 5);
    ASSERT_EQ(dst[0], 0x10);
    ASSERT_EQ(dst[1], 0x20);
    ASSERT_EQ(dst[2], 0x30);
    ASSERT_EQ(dst[3], 0x40);
    ASSERT_EQ(dst[4], 0x50);
}

TEST(rle_marker_a_emits_run_of_fill)
{
    /* fill=0, A=0xAA. Stream: AA 04 → emit 5 × 0 (count = 4 + 1). */
    const uint8_t src[] = { 0x00, 0xAA, 0xBB,
                            0xAA, 0x04, 0x11 };
    uint8_t dst[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    DepackRleFrame(src, dst, 6);
    ASSERT_EQ(dst[0], 0x00);
    ASSERT_EQ(dst[1], 0x00);
    ASSERT_EQ(dst[2], 0x00);
    ASSERT_EQ(dst[3], 0x00);
    ASSERT_EQ(dst[4], 0x00);
    /* 6th byte = literal 0x11 (next stream byte). */
    ASSERT_EQ(dst[5], 0x11);
}

TEST(rle_marker_b_emits_run_of_value)
{
    /* fill=0, A=0xAA, B=0xBB. Stream: BB 02 7C → emit 3 × 0x7C. */
    const uint8_t src[] = { 0x00, 0xAA, 0xBB,
                            0xBB, 0x02, 0x7C, 0x99 };
    uint8_t dst[4] = { 0 };
    DepackRleFrame(src, dst, 4);
    ASSERT_EQ(dst[0], 0x7C);
    ASSERT_EQ(dst[1], 0x7C);
    ASSERT_EQ(dst[2], 0x7C);
    ASSERT_EQ(dst[3], 0x99);   /* next literal */
}

TEST(rle_respects_dst_len_clamp)
{
    /* Even if a run would emit more than dst_len, the decoder must stop. */
    const uint8_t src[] = { 0x00, 0xAA, 0xBB,
                            0xAA, 0xFF };       /* would emit 256 × 0 */
    uint8_t dst[8];
    memset(dst, 0xCD, sizeof dst);
    DepackRleFrame(src, dst, 4);   /* only 4 bytes of output */
    ASSERT_EQ(dst[0], 0x00);
    ASSERT_EQ(dst[1], 0x00);
    ASSERT_EQ(dst[2], 0x00);
    ASSERT_EQ(dst[3], 0x00);
    /* Bytes 4-7 must be untouched (guard). */
    ASSERT_EQ(dst[4], 0xCD);
    ASSERT_EQ(dst[5], 0xCD);
    ASSERT_EQ(dst[6], 0xCD);
    ASSERT_EQ(dst[7], 0xCD);
}

TEST(rle_handles_zero_length_dst)
{
    /* dst_len <= 0 → early return, no crash. */
    const uint8_t src[] = { 0x00, 0xAA, 0xBB, 0x11 };
    uint8_t dst[4] = { 0x55, 0x55, 0x55, 0x55 };
    DepackRleFrame(src, dst, 0);
    /* All output untouched. */
    ASSERT_EQ(dst[0], 0x55);
    ASSERT_EQ(dst[1], 0x55);
    ASSERT_EQ(dst[2], 0x55);
    ASSERT_EQ(dst[3], 0x55);
}

TEST(rle_null_input_does_not_crash)
{
    uint8_t dst[8] = { 0 };
    DepackRleFrame(NULL, dst, 8);
    DepackRleFrame((const uint8_t *)"abc", NULL, 8);
    /* If we reach here without segfault, the test passes. */
    ASSERT_TRUE(1);
}

/* ---- InstallPalette: copies bytes into g_palette_rgb -------------------- */
/* WARNING: this also triggers RebuildAlphaQuantLuts which is heavy
 * (4096-entry inverse search). Keep palette sizes small in tests. */

TEST(install_palette_copies_full)
{
    uint8_t test_pal[256 * 3];
    for (int i = 0; i < 256; ++i) {
        test_pal[i*3 + 0] = (uint8_t)(i);
        test_pal[i*3 + 1] = (uint8_t)(i ^ 0xFF);
        test_pal[i*3 + 2] = (uint8_t)(i + 0x80);
    }
    InstallPalette(test_pal, 0);

    /* Spot-check entries 0, 64, 255. */
    ASSERT_EQ(g_palette_rgb[0],         0);
    ASSERT_EQ(g_palette_rgb[1],         0xFF);
    ASSERT_EQ(g_palette_rgb[2],         0x80);
    ASSERT_EQ(g_palette_rgb[64*3 + 0],  64);
    ASSERT_EQ(g_palette_rgb[64*3 + 1],  64 ^ 0xFF);
    ASSERT_EQ(g_palette_rgb[64*3 + 2],  (uint8_t)(64 + 0x80));
    ASSERT_EQ(g_palette_rgb[255*3 + 0], 255);
}

TEST(install_palette_partial_starts_at_first)
{
    /* Install only entries 128..255 — leaving 0..127 untouched. */
    uint8_t before[256 * 3];
    memcpy(before, g_palette_rgb, sizeof before);

    uint8_t patch[128 * 3];
    memset(patch, 0xAB, sizeof patch);
    InstallPalette(patch, 128);

    /* Entries 0..127 unchanged. */
    for (int i = 0; i < 128 * 3; ++i)
        ASSERT_EQ(g_palette_rgb[i], before[i]);
    /* Entries 128..255 all 0xAB. */
    for (int i = 128 * 3; i < 256 * 3; ++i)
        ASSERT_EQ(g_palette_rgb[i], 0xAB);
}

TEST(install_palette_out_of_range_is_no_op)
{
    uint8_t before[256 * 3];
    memcpy(before, g_palette_rgb, sizeof before);

    uint8_t patch[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    InstallPalette(patch, 256);   /* first == 256 → out of range */
    InstallPalette(NULL,  0);     /* null source */

    for (int i = 0; i < 256 * 3; ++i)
        ASSERT_EQ(g_palette_rgb[i], before[i]);
}

/* ---- SetAlphaTint: encoding sanity ------------------------------------- */
/* The tint param is a single uint32_t (R at low byte) used by the
 * alpha-plane scaled blit. 0x808080 = neutral identity. We can't easily
 * inspect the internal cached tint without exposing it; this test just
 * verifies SetAlphaTint doesn't crash on edge values. */

TEST(set_alpha_tint_accepts_full_range)
{
    SetAlphaTint(0x000000);
    SetAlphaTint(0x808080);     /* identity */
    SetAlphaTint(0xFFFFFF);
    SetAlphaTint(0xFF00FF);
    /* Survival = pass. */
    ASSERT_TRUE(1);
}

/* ---- BlitSpriteToBackbuffer + PaintImageToBackbuffer ------------------ *
 *
 * These write into the global `g_back_shadow` (lazily allocated by
 * ensure_shadow on first call). Tests verify destination pixels match
 * source after color-key (mode 0) and opaque (mode 1) blits. */

extern uint8_t  *g_back_shadow;
extern uint16_t  g_screen_w;
extern uint16_t  g_screen_h;

void BlitSpriteToBackbuffer(uint16_t dx, uint16_t dy,
                            uint16_t sx, uint16_t sy,
                            uint16_t cw, uint16_t ch,
                            uint16_t pw, uint16_t ph,
                            uint8_t *src, int16_t mode);

TEST(blit_sprite_color_key_skips_zero_pixels)
{
    /* 4×2 source, color 0 is "transparent". Backbuffer pre-filled with
     * 0x55 sentinel. After mode-0 blit at (10, 10), only non-zero
     * source pixels overwrite the sentinel. */
    uint8_t src[8] = {
        0x10, 0x00, 0x20, 0x30,        /* row 0: skip col 1 */
        0x00, 0x40, 0x00, 0x50,        /* row 1: skip cols 0, 2 */
    };

    /* Force shadow allocation by triggering any blit. */
    uint8_t init_src[2] = { 0, 0 };
    BlitSpriteToBackbuffer(0, 0, 0, 0, 2, 1, 2, 1, init_src, 0);
    ASSERT_NOT_NULL(g_back_shadow);

    /* Fill shadow with sentinel. */
    memset(g_back_shadow, 0x55, (size_t)g_screen_w * g_screen_h);

    /* Blit at (10, 10), full 4×2 sprite. */
    BlitSpriteToBackbuffer(10, 10, 0, 0, 4, 2, 4, 2, src, 0);

    /* Row 0 at y=10. */
    uint8_t *row0 = g_back_shadow + 10 * g_screen_w + 10;
    ASSERT_EQ(row0[0], 0x10);    /* opaque */
    ASSERT_EQ(row0[1], 0x55);    /* transparent — sentinel kept */
    ASSERT_EQ(row0[2], 0x20);
    ASSERT_EQ(row0[3], 0x30);

    /* Row 1 at y=11. */
    uint8_t *row1 = g_back_shadow + 11 * g_screen_w + 10;
    ASSERT_EQ(row1[0], 0x55);
    ASSERT_EQ(row1[1], 0x40);
    ASSERT_EQ(row1[2], 0x55);
    ASSERT_EQ(row1[3], 0x50);
}

TEST(blit_sprite_mode_1_opaque_memcpy)
{
    /* mode=1: even zero pixels overwrite. */
    uint8_t src[4] = { 0x00, 0xAA, 0x00, 0xBB };

    uint8_t init_src[2] = { 0, 0 };
    BlitSpriteToBackbuffer(0, 0, 0, 0, 2, 1, 2, 1, init_src, 0);
    memset(g_back_shadow, 0x55, (size_t)g_screen_w * g_screen_h);

    BlitSpriteToBackbuffer(5, 5, 0, 0, 4, 1, 4, 1, src, 1);

    uint8_t *row = g_back_shadow + 5 * g_screen_w + 5;
    ASSERT_EQ(row[0], 0x00);     /* mode 1 overwrites zero */
    ASSERT_EQ(row[1], 0xAA);
    ASSERT_EQ(row[2], 0x00);
    ASSERT_EQ(row[3], 0xBB);
}

TEST(blit_sprite_clips_to_screen_right_edge)
{
    /* Blit a 10-pixel wide sprite starting at x = screen_w - 5. Only
     * 5 pixels should land on-screen; no read past source bounds. */
    uint8_t src[10];
    for (int i = 0; i < 10; ++i) src[i] = (uint8_t)(0xA0 + i);

    uint8_t init_src[2] = { 0, 0 };
    BlitSpriteToBackbuffer(0, 0, 0, 0, 2, 1, 2, 1, init_src, 0);
    memset(g_back_shadow, 0x00, (size_t)g_screen_w * g_screen_h);

    int16_t dx = (int16_t)(g_screen_w - 5);
    BlitSpriteToBackbuffer((uint16_t)dx, 0, 0, 0, 10, 1, 10, 1, src, 1);

    /* Verify first 5 pixels of the clipped region match source. */
    uint8_t *row = g_back_shadow + dx;
    for (int i = 0; i < 5; ++i)
        ASSERT_EQ(row[i], 0xA0 + i);
}

TEST(blit_sprite_null_source_safe)
{
    /* src=NULL → early return, no crash. */
    BlitSpriteToBackbuffer(0, 0, 0, 0, 4, 4, 4, 4, NULL, 0);
    ASSERT_TRUE(1);
}

TEST(paint_image_to_backbuffer_copies_pixels)
{
    /* PaintImageToBackbuffer is an opaque memcpy variant (no color-key). */
    uint8_t src[16];
    for (int i = 0; i < 16; ++i) src[i] = (uint8_t)(0x10 + i);

    /* Ensure shadow exists. */
    uint8_t init_src[2] = { 0, 0 };
    BlitSpriteToBackbuffer(0, 0, 0, 0, 2, 1, 2, 1, init_src, 0);
    memset(g_back_shadow, 0x77, (size_t)g_screen_w * g_screen_h);

    PaintImageToBackbuffer(100, 100, 4, 4, src);

    /* Verify 4×4 block at (100, 100). */
    for (int r = 0; r < 4; ++r) {
        uint8_t *row = g_back_shadow + (100 + r) * g_screen_w + 100;
        for (int c = 0; c < 4; ++c) {
            ASSERT_EQ(row[c], (uint8_t)(0x10 + r * 4 + c));
        }
    }
}

/* ---- edge cases: RLE marker collision, clipping, palette boundaries -- */

TEST(rle_marker_a_and_b_same_byte_a_wins)
{
    /* Edge case: fill=0, marker_A==marker_B==0xAA. The decoder checks
     * `if (b == marker_A)` FIRST, then `else if (b == marker_B)`. So
     * when both markers are the same byte, the marker_A branch always
     * fires — pin this contract so a refactor doesn't accidentally
     * reorder the branches. */
    const uint8_t src[] = { 0x00, 0xAA, 0xAA,
                            0xAA, 0x03, 0x99 };  /* AA → fill (marker_A path) */
    uint8_t dst[5] = { 0 };
    DepackRleFrame(src, dst, 5);
    /* Marker_A branch: count=4, emit 4 × fill (= 0x00). 5th byte = 0x99 literal. */
    ASSERT_EQ(dst[0], 0x00);
    ASSERT_EQ(dst[1], 0x00);
    ASSERT_EQ(dst[2], 0x00);
    ASSERT_EQ(dst[3], 0x00);
    ASSERT_EQ(dst[4], 0x99);
}

TEST(blit_sprite_negative_dx_left_clip)
{
    /* dx = -3, sprite 8px wide → first 3 pixels clipped, 5 pixels
     * rendered at x=0..4. The leftmost RENDERED pixel = source[3]. */
    uint8_t src[8] = { 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7 };

    uint8_t init_src[2] = { 0, 0 };
    BlitSpriteToBackbuffer(0, 0, 0, 0, 2, 1, 2, 1, init_src, 0);
    memset(g_back_shadow, 0x00, (size_t)g_screen_w * g_screen_h);

    /* dx = -3 as uint16 = 0xFFFD. BlitSpriteToBackbuffer signed-casts
     * the dx parameter internally, so we pass it as uint16 (cast). */
    BlitSpriteToBackbuffer((uint16_t)(int16_t)-3, 50, 0, 0, 8, 1, 8, 1, src, 1);

    /* Pixels at x=0..4 should match source[3..7]. */
    uint8_t *row = g_back_shadow + 50 * g_screen_w;
    ASSERT_EQ(row[0], 0xA3);
    ASSERT_EQ(row[1], 0xA4);
    ASSERT_EQ(row[2], 0xA5);
    ASSERT_EQ(row[3], 0xA6);
    ASSERT_EQ(row[4], 0xA7);
}

TEST(blit_sprite_negative_dy_top_clip)
{
    /* dy = -2, sprite 4 rows tall → first 2 rows clipped, 2 rows rendered. */
    uint8_t src[16];
    for (int i = 0; i < 16; ++i) src[i] = (uint8_t)(0xB0 + i);

    uint8_t init_src[2] = { 0, 0 };
    BlitSpriteToBackbuffer(0, 0, 0, 0, 2, 1, 2, 1, init_src, 0);
    memset(g_back_shadow, 0x00, (size_t)g_screen_w * g_screen_h);

    BlitSpriteToBackbuffer(20, (uint16_t)(int16_t)-2, 0, 0, 4, 4, 4, 4, src, 1);

    /* Row 0 of shadow at y=0 should match source row 2 = bytes 8..11. */
    uint8_t *row0 = g_back_shadow + 0 * g_screen_w + 20;
    ASSERT_EQ(row0[0], 0xB8);
    ASSERT_EQ(row0[1], 0xB9);
    ASSERT_EQ(row0[2], 0xBA);
    ASSERT_EQ(row0[3], 0xBB);
    /* Row 1 of shadow at y=1 should match source row 3 = bytes 12..15. */
    uint8_t *row1 = g_back_shadow + 1 * g_screen_w + 20;
    ASSERT_EQ(row1[0], 0xBC);
    ASSERT_EQ(row1[3], 0xBF);
}

TEST(install_palette_first_255_writes_only_one_entry)
{
    /* InstallPalette(rgb, 255) → count = 256 - 255 = 1 entry. */
    uint8_t before[256 * 3];
    memcpy(before, g_palette_rgb, sizeof before);

    uint8_t one[3] = { 0xAB, 0xCD, 0xEF };
    InstallPalette(one, 255);

    /* Entries 0..254 unchanged. */
    for (int i = 0; i < 254 * 3; ++i) ASSERT_EQ(g_palette_rgb[i], before[i]);
    /* Entry 255 = new bytes. */
    ASSERT_EQ(g_palette_rgb[255 * 3 + 0], 0xAB);
    ASSERT_EQ(g_palette_rgb[255 * 3 + 1], 0xCD);
    ASSERT_EQ(g_palette_rgb[255 * 3 + 2], 0xEF);
}

SUITE(graphics)
{
    RUN_TEST(rle_literal_passthrough);
    RUN_TEST(rle_marker_a_emits_run_of_fill);
    RUN_TEST(rle_marker_b_emits_run_of_value);
    RUN_TEST(rle_respects_dst_len_clamp);
    RUN_TEST(rle_handles_zero_length_dst);
    RUN_TEST(rle_null_input_does_not_crash);
    RUN_TEST(install_palette_copies_full);
    RUN_TEST(install_palette_partial_starts_at_first);
    RUN_TEST(install_palette_out_of_range_is_no_op);
    RUN_TEST(set_alpha_tint_accepts_full_range);
    RUN_TEST(blit_sprite_color_key_skips_zero_pixels);
    RUN_TEST(blit_sprite_mode_1_opaque_memcpy);
    RUN_TEST(blit_sprite_clips_to_screen_right_edge);
    RUN_TEST(blit_sprite_null_source_safe);
    RUN_TEST(paint_image_to_backbuffer_copies_pixels);
    RUN_TEST(rle_marker_a_and_b_same_byte_a_wins);
    RUN_TEST(blit_sprite_negative_dx_left_clip);
    RUN_TEST(blit_sprite_negative_dy_top_clip);
    RUN_TEST(install_palette_first_255_writes_only_one_entry);
}
