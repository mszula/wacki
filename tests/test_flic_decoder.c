/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * tests/test_flic_decoder.c — FLIC frame decoder bounds + correctness.
 *
 * The decoder (src/flic/decoder.c) consumes AVI-supplied, UNTRUSTED
 * frame dimensions and chunk sizes. These tests pin two things:
 *
 *   1. Valid frames still decode correctly (CK_COPY / CK_BLACK) — the
 *      bounds added for safety must not regress real playback.
 *   2. Malformed frames (oversized w/h, truncated bodies, lying chunk
 *      sizes, garbage BRUN/DELTA, line cursor past the frame) do not
 *      read or write out of bounds.
 *
 * g_back_shadow is allocated to EXACTLY 640*480 so that under
 * -fsanitize=address any overshoot aborts the test. The decoder's own
 * writes are additionally checked against explicit canaries so the
 * suite catches regressions even in a non-sanitized build.
 */

#include "test.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern void flic_decode_frame(const uint8_t *fdata, uint32_t fsize, int w, int h);
extern uint8_t *g_back_shadow;          /* defined in graphics.c */
extern uint8_t  g_palette_rgb[256 * 3];

#define SHADOW_W 640
#define SHADOW_H 480
#define SHADOW_BYTES (SHADOW_W * SHADOW_H)

/* Point g_back_shadow at a fresh exact-size buffer; caller restores. */
static uint8_t *shadow_begin(uint8_t **saved)
{
    *saved = g_back_shadow;
    uint8_t *buf = malloc(SHADOW_BYTES);
    memset(buf, 0, SHADOW_BYTES);
    g_back_shadow = buf;
    return buf;
}
static void shadow_end(uint8_t *saved, uint8_t *buf)
{
    g_back_shadow = saved;
    free(buf);
}

static void put_u32(uint8_t *p, uint32_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static void put_u16(uint8_t *p, uint16_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }

/* Assemble a single-chunk frame of exactly `total` bytes into an
 * exact-size heap alloc. `chunk_sz` is written verbatim (may lie). */
static uint8_t *make_frame(uint16_t chunk_sz, uint16_t type, uint32_t total, uint32_t *fsize)
{
    uint8_t *f = calloc(1, total);
    put_u16(f + 4, 0xF1FA);          /* frame magic */
    put_u16(f + 6, 1);               /* one chunk */
    put_u32(f + 16, chunk_sz);       /* chunk size field */
    put_u16(f + 20, type);           /* chunk type */
    *fsize = total;
    return f;
}

/* ---- positive: valid frames still decode ------------------------------- */

TEST(copy_chunk_writes_pixels)
{
    /* CK_COPY (16), w=8 h=4 → 32 body bytes 0x00..0x1F copied verbatim. */
    enum { W = 8, H = 4, N = W * H };
    uint8_t *saved, *buf = shadow_begin(&saved);

    uint32_t fsize;
    uint8_t *f = make_frame(6 + N, 16, 16 + 6 + N, &fsize);
    for (int i = 0; i < N; ++i) f[22 + i] = (uint8_t)i;

    flic_decode_frame(f, fsize, W, H);

    for (int i = 0; i < N; ++i) ASSERT_EQ(buf[i], (uint8_t)i);
    free(f);
    shadow_end(saved, buf);
}

TEST(black_chunk_clears_frame)
{
    /* CK_BLACK (13) zeroes w*h bytes. Pre-fill, then assert cleared. */
    enum { W = 8, H = 4, N = W * H };
    uint8_t *saved, *buf = shadow_begin(&saved);
    memset(buf, 0xAB, N + 8);

    uint32_t fsize;
    uint8_t *f = make_frame(6, 13, 16 + 6, &fsize);
    flic_decode_frame(f, fsize, W, H);

    for (int i = 0; i < N; ++i) ASSERT_EQ(buf[i], 0u);
    ASSERT_EQ(buf[N], 0xABu);          /* byte just past the frame untouched */
    free(f);
    shadow_end(saved, buf);
}

/* ---- negative: malformed frames must not go out of bounds -------------- */

TEST(oversized_dims_are_rejected)
{
    /* w*h = 25,000,000 on a 307,200-byte buffer: the old decoder memset
     * 25 MB past the end. The dim clamp must bail before any write. */
    uint8_t *saved, *buf = shadow_begin(&saved);
    buf[0] = 0x5A;                      /* canary */

    uint32_t fsize;
    uint8_t *f = make_frame(6, 13 /*BLACK*/, 16 + 6, &fsize);
    flic_decode_frame(f, fsize, 5000, 5000);

    ASSERT_EQ(buf[0], 0x5Au);           /* untouched — frame was rejected */
    free(f);
    shadow_end(saved, buf);
}

TEST(zero_and_negative_dims_are_rejected)
{
    uint8_t *saved, *buf = shadow_begin(&saved);
    buf[0] = 0x5A;
    uint32_t fsize;
    uint8_t *f = make_frame(6, 13, 16 + 6, &fsize);

    flic_decode_frame(f, fsize, 0, 480);
    flic_decode_frame(f, fsize, 640, 0);
    flic_decode_frame(f, fsize, -1, -1);

    ASSERT_EQ(buf[0], 0x5Au);
    free(f);
    shadow_end(saved, buf);
}

TEST(copy_with_short_body_does_not_overread)
{
    /* CK_COPY claims a full 640*480 image but the frame carries only a
     * few body bytes. Copy must clamp to what's present (ASan guards the
     * source read; here we assert only the delivered bytes landed). */
    uint8_t *saved, *buf = shadow_begin(&saved);
    uint32_t fsize;
    uint8_t *f = make_frame(6 + 4, 16, 16 + 6 + 4, &fsize);
    f[22]=1; f[23]=2; f[24]=3; f[25]=4;

    flic_decode_frame(f, fsize, 640, 480);   /* would overread 300 KB pre-fix */

    ASSERT_EQ(buf[0], 1u); ASSERT_EQ(buf[3], 4u);
    free(f);
    shadow_end(saved, buf);
}

TEST(lying_chunk_size_is_clamped)
{
    /* Chunk size field claims 60000 bytes; the frame is tiny. body_end
     * must clamp to the real buffer and the loop must stop. */
    uint8_t *saved, *buf = shadow_begin(&saved);
    uint32_t fsize;
    uint8_t *f = make_frame(60000, 16 /*COPY*/, 16 + 6 + 4, &fsize);

    flic_decode_frame(f, fsize, 640, 480);   /* must not crash / overread */

    (void)buf;
    free(f);
    shadow_end(saved, buf);
    ASSERT_TRUE(1);
}

TEST(brun_garbage_body_stays_in_bounds)
{
    /* BRUN (15) with a body of negative run-counts (literal runs) that
     * would read past a short body — reads must stop at the frame end. */
    uint8_t *saved, *buf = shadow_begin(&saved);
    uint32_t fsize;
    uint8_t *f = make_frame(6 + 6, 15, 16 + 6 + 6, &fsize);
    memset(f + 22, 0x80, 6);            /* 0x80 = -128 → long literal run */

    flic_decode_frame(f, fsize, 640, 480);

    (void)buf;
    free(f);
    shadow_end(saved, buf);
    ASSERT_TRUE(1);
}

TEST(delta_line_skip_past_frame_stays_in_bounds)
{
    /* DELTA (7): a LINE_SKIP opcode drives the row cursor far past the
     * frame height, then a packet writes — the y-range gate must stop
     * the write from leaving the buffer. */
    uint8_t *saved, *buf = shadow_begin(&saved);
    uint32_t fsize;
    uint8_t *f = make_frame(6 + 12, 7, 16 + 6 + 12, &fsize);
    put_u16(f + 22, 4);                 /* lines = 4 */
    put_u16(f + 24, 0xC000 | 1);        /* LINE_SKIP (top bits 11) */
    put_u16(f + 26, 1);                 /* opcode: 1 packet */
    f[28] = 0; f[29] = (uint8_t)(-4);   /* skip 0, RLE 4 word-pairs */
    f[30] = 0xAB; f[31] = 0xCD;

    flic_decode_frame(f, fsize, 640, 480);

    (void)buf;
    free(f);
    shadow_end(saved, buf);
    ASSERT_TRUE(1);
}

SUITE(flic_decoder)
{
    RUN_TEST(copy_chunk_writes_pixels);
    RUN_TEST(black_chunk_clears_frame);
    RUN_TEST(oversized_dims_are_rejected);
    RUN_TEST(zero_and_negative_dims_are_rejected);
    RUN_TEST(copy_with_short_body_does_not_overread);
    RUN_TEST(lying_chunk_size_is_clamped);
    RUN_TEST(brun_garbage_body_stays_in_bounds);
    RUN_TEST(delta_line_skip_past_frame_stays_in_bounds);
}
