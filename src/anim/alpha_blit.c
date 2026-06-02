/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* src/anim/alpha_blit.c — alpha-plane scaled blit with RGB12
 * quantization.
 *
 * Used by per-entity VM for entities with flag 0x100 + 0x400 (alpha-
 * plane + perspective scaled). Mode 0 = nearest-neighbor (also handled
 * by the existing BlitSpriteScaledColorKey path). Mode 1 = 1D
 * horizontal box filter with RGB12 quantization. Mode 2 = 2D box
 * filter (full alpha).
 *
 * The kernel runs the box-filter on each dst pixel: collect every
 * non-transparent source pixel that maps to it, average the RGB12-
 * encoded colour (R/G/B each truncated to 4 bits), apply the active
 * tint, and look up the result in a 4096-entry inverse LUT to get the
 * nearest palette index. The LUT is rebuilt from scratch every
 * InstallPalette (~1.66 ms on modern CPUs); the original 1997 build
 * cached it to disk as `Wacki.444`, dropped here as a perf
 * optimization not needed today.
 *
 * Public API (RebuildAlphaQuantLuts / SetAlphaTint / BlitAlphaScaled /
 * BlitAlphaScaledToBackbuffer) declared in wacki.h. */

#include "wacki.h"

#include <stdint.h>
#include <stdlib.h>

/* ---- RGB12 alpha-quant LUT constants ------------------------------ */

/* 8-bpp palette size + bytes per .PAL entry (R, G, B). */
#define PALETTE_SIZE                256
#define PALETTE_BYTES_PER_ENTRY     3

/* RGB12 = three 4-bit channels packed into 12 bits → 16^3 = 4096 keys.
 * Nibble layout per original: R is LOW, G MIDDLE, B HIGH (NOT a
 * typical RGB565-style ordering). */
#define RGB12_KEY_COUNT             4096
#define RGB12_NIBBLE_MASK           0xF
#define RGB12_NIBBLE_BITS           4
#define RGB12_R_SHIFT               0
#define RGB12_G_SHIFT               4
#define RGB12_B_SHIFT               8
#define RGB12_MAX_CHANNEL           0xF

/* 8-bit palette colours convert to RGB12 nibbles by >>4. */
#define RGB888_TO_NIBBLE_SHIFT      4

/* Brute-force inverse-LUT distance weights. From the original engine's
 * inner loop — approximate BT.601 perceived luminance squared:
 *   R diff × 900   (≈ 0.30² × 10000)
 *   G diff × 3481  (≈ 0.59² × 10000)
 *   B diff × 121   (≈ 0.11² × 10000)
 * G dominates, B is weakest. */
#define COLOR_DISTANCE_WEIGHT_R     900
#define COLOR_DISTANCE_WEIGHT_G     3481
#define COLOR_DISTANCE_WEIGHT_B     121

#define COLOR_DISTANCE_SENTINEL     0x7FFFFFFF

/* Index 0 is the engine's "transparent / colour-key" entry, so the
 * inverse-LUT search starts at palette index 1. */
#define PALETTE_NONZERO_FIRST       1

/* Tint multiplier — RGB-encoded uint32 with R in LOW byte. 0x80 per
 * channel = identity (no tint); the multiply is fixed-point Q1.7. */
#define TINT_IDENTITY               0x808080u
#define TINT_NEUTRAL_CHANNEL        0x80
#define TINT_CHANNEL_MASK           0xFF
#define TINT_R_SHIFT                0
#define TINT_G_SHIFT                8
#define TINT_B_SHIFT                16
#define TINT_SCALE_SHIFT            7    /* log2(0x80) — fixed-point Q1.7 */

/* ---- BlitAlphaScaled constants ------------------------------------ */

/* Maximum destination dimension supported by the alpha-scaled blitter.
 * Matches the original engine's hard guard; LUT_LEN is +1 so the
 * static tables can be indexed from 0..MAX inclusive. */
#define ALPHA_SCALED_MAX_DIM        0x400
#define ALPHA_SCALED_LUT_LEN        (ALPHA_SCALED_MAX_DIM + 1)

/* BlitAlphaScaled mode codes:
 *   NEAREST = original nearest-neighbor with x-step LUT
 *   BOX_1D  = 1D horizontal box filter + RGB12 quantization
 *   BOX_2D  = 2D box filter + RGB12 quantization */
#define BLIT_MODE_NEAREST           0
#define BLIT_MODE_BOX_1D            1
#define BLIT_MODE_BOX_2D            2

/* ---- module state ------------------------------------------------- */

/* Palette → RGB12 nibble triplets (byte order R/G/B/0). */
static uint8_t  s_pal_rgb12[PALETTE_SIZE][4];

/* RGB12 → nearest palette index. */
static uint8_t  s_rgb12_to_pal[RGB12_KEY_COUNT];

/* Current tint state. */
static uint32_t s_tint_color = TINT_IDENTITY;
static uint32_t s_tint_r = TINT_NEUTRAL_CHANNEL;
static uint32_t s_tint_g = TINT_NEUTRAL_CHANNEL;
static uint32_t s_tint_b = TINT_NEUTRAL_CHANNEL;

/* ---- LUT-build helpers -------------------------------------------- */

/* Convert the live g_palette_rgb (256 × R/G/B bytes) into the per-
 * palette-entry RGB12 nibble triplet used by the quant kernels. */
static void rebuild_rgb12_palette(void)
{
    for (int i = 0; i < PALETTE_SIZE; ++i) {
        s_pal_rgb12[i][0] =
            g_palette_rgb[i * PALETTE_BYTES_PER_ENTRY + 0] >> RGB888_TO_NIBBLE_SHIFT;
        s_pal_rgb12[i][1] =
            g_palette_rgb[i * PALETTE_BYTES_PER_ENTRY + 1] >> RGB888_TO_NIBBLE_SHIFT;
        s_pal_rgb12[i][2] =
            g_palette_rgb[i * PALETTE_BYTES_PER_ENTRY + 2] >> RGB888_TO_NIBBLE_SHIFT;
        s_pal_rgb12[i][3] = 0;
    }
}

/* Pack three RGB12 nibbles into the inverse-LUT key. */
static inline int pack_rgb12_key(int r, int g, int b)
{
    return (r << RGB12_R_SHIFT)
         | (g << RGB12_G_SHIFT)
         | (b << RGB12_B_SHIFT);
}

/* Brute-force nearest-palette-index lookup for a single RGB12 triple. */
static uint8_t find_nearest_palette_index(int r, int g, int b)
{
    int best_idx  = PALETTE_NONZERO_FIRST;
    int best_dist = COLOR_DISTANCE_SENTINEL;
    for (int p = PALETTE_NONZERO_FIRST; p < PALETTE_SIZE; ++p) {
        int dr = r - s_pal_rgb12[p][0];
        int dg = g - s_pal_rgb12[p][1];
        int db = b - s_pal_rgb12[p][2];
        int dist = dr * dr * COLOR_DISTANCE_WEIGHT_R
                 + dg * dg * COLOR_DISTANCE_WEIGHT_G
                 + db * db * COLOR_DISTANCE_WEIGHT_B;
        if (dist < best_dist) {
            best_dist = dist;
            best_idx  = p;
        }
        if (dist == 0) break;
    }
    return (uint8_t)best_idx;
}

/* Apply the active tint multiplier to one RGB12 sample triple,
 * clamping each channel to RGB12_MAX_CHANNEL. */
static inline void apply_tint_clamped(int *r, int *g, int *b)
{
    if (s_tint_color == TINT_IDENTITY) return;
    *r = (*r * (int)s_tint_r) >> TINT_SCALE_SHIFT;
    *g = (*g * (int)s_tint_g) >> TINT_SCALE_SHIFT;
    *b = (*b * (int)s_tint_b) >> TINT_SCALE_SHIFT;
    if (*r > RGB12_MAX_CHANNEL) *r = RGB12_MAX_CHANNEL;
    if (*g > RGB12_MAX_CHANNEL) *g = RGB12_MAX_CHANNEL;
    if (*b > RGB12_MAX_CHANNEL) *b = RGB12_MAX_CHANNEL;
}

/* Common tail for the 1D / 2D box-filter samplers: divide each running
 * sum by the contributing-pixel count, mask back to a nibble, apply
 * the tint, and look up the result in the inverse LUT. n==0 → fully
 * transparent → return palette idx 0 (colour-key). */
static uint8_t finalize_sample(int sum_r, int sum_g, int sum_b, int n)
{
    if (n == 0) return 0;
    if (n > 1) {
        sum_r = (sum_r / n) & RGB12_NIBBLE_MASK;
        sum_g = (sum_g / n) & RGB12_NIBBLE_MASK;
        sum_b = (sum_b / n) & RGB12_NIBBLE_MASK;
    }
    apply_tint_clamped(&sum_r, &sum_g, &sum_b);
    return s_rgb12_to_pal[pack_rgb12_key(sum_r, sum_g, sum_b)];
}

/* Accumulate one pixel into the running RGB12 sums (no-op for
 * transparent index-0 pixels). */
static inline void accumulate_pixel(uint8_t v,
                                    int *sum_r, int *sum_g, int *sum_b, int *n)
{
    if (!v) return;
    *sum_r += s_pal_rgb12[v][0];
    *sum_g += s_pal_rgb12[v][1];
    *sum_b += s_pal_rgb12[v][2];
    ++*n;
}

/* ---- LUT-build / tint public API --------------------------------- */

/* Recompute both the palette → RGB12 forward table and the RGB12 →
 * palette inverse table whenever the palette changes. Called from
 * InstallPalette in graphics.c. */
void RebuildAlphaQuantLuts(void)
{
    rebuild_rgb12_palette();

    for (int rgb12 = 0; rgb12 < RGB12_KEY_COUNT; ++rgb12) {
        int r = (rgb12 >> RGB12_R_SHIFT) & RGB12_NIBBLE_MASK;
        int g = (rgb12 >> RGB12_G_SHIFT) & RGB12_NIBBLE_MASK;
        int b = (rgb12 >> RGB12_B_SHIFT) & RGB12_NIBBLE_MASK;
        s_rgb12_to_pal[rgb12] = find_nearest_palette_index(r, g, b);
    }

    /* Reset tint to identity on every palette swap. */
    s_tint_color = TINT_IDENTITY;
    s_tint_r = TINT_NEUTRAL_CHANNEL;
    s_tint_g = TINT_NEUTRAL_CHANNEL;
    s_tint_b = TINT_NEUTRAL_CHANNEL;
}

/* Set the tint multiplier as an RGB-encoded uint32 (R=LOW byte,
 * B=HIGH byte). TINT_IDENTITY = no tint; values < 0x80 darken the
 * channel, > 0x80 brighten it. */
void SetAlphaTint(uint32_t rgb)
{
    s_tint_color = rgb;
    s_tint_r = (rgb >> TINT_R_SHIFT) & TINT_CHANNEL_MASK;
    s_tint_g = (rgb >> TINT_G_SHIFT) & TINT_CHANNEL_MASK;
    s_tint_b = (rgb >> TINT_B_SHIFT) & TINT_CHANNEL_MASK;
}

/* ---- BlitAlphaScaled box-filter samplers ------------------------- */

/* Average non-transparent pixels across a `count`-wide source span,
 * then look up the resulting RGB12 triple in the inverse LUT. Used by
 * mode-1 (1D horizontal) alpha-scaled blits. */
static uint8_t sample_box_1d(const uint8_t *p, int count)
{
    int sum_r = 0, sum_g = 0, sum_b = 0, n = 0;
    if (count < 1) count = 1;
    for (int i = 0; i < count; ++i) {
        accumulate_pixel(p[i], &sum_r, &sum_g, &sum_b, &n);
    }
    return finalize_sample(sum_r, sum_g, sum_b, n);
}

/* Same as 1D but spans `height` rows × `width` cols around the source
 * pixel. `src_stride` is the row pitch. Used by mode-2 (2D box-filter)
 * alpha-scaled blits. */
static uint8_t sample_box_2d(const uint8_t *p, int width, int src_stride,
                             int height)
{
    int sum_r = 0, sum_g = 0, sum_b = 0, n = 0;
    if (width  < 1) width  = 1;
    if (height < 1) height = 1;
    for (int y = 0; y < height; ++y) {
        const uint8_t *row = p + y * src_stride;
        for (int x = 0; x < width; ++x) {
            accumulate_pixel(row[x], &sum_r, &sum_g, &sum_b, &n);
        }
    }
    return finalize_sample(sum_r, sum_g, sum_b, n);
}

/* ---- BlitAlphaScaled per-mode row paths ------------------------- */

/* Build a Bresenham-style x-step LUT: each dst column draws step[dx]
 * src pixels (either floor(src_w/dst_w) or that+1). Modes 1/2 read it
 * as a per-column box-filter width. */
static void build_x_step_lut(uint32_t *x_step, uint16_t src_w, uint16_t dst_w)
{
    uint32_t base = src_w / dst_w;
    uint32_t rem  = src_w % dst_w;
    uint32_t acc  = rem;
    uint32_t cur  = base;
    for (uint32_t i = 0; i < dst_w; ++i) {
        x_step[i] = cur;
        acc += rem;
        cur = base;
        if ((int32_t)acc >= (int32_t)dst_w) {
            acc -= dst_w;
            cur += 1;
        }
    }
}

/* Build a y-extra-row flag table: y_extra[dy] = 1 when the source row
 * pointer should advance an extra src_w for this dst row (Bresenham
 * remainder distribution). */
static void build_y_extra_table(uint8_t *y_extra,
                                uint16_t src_h, uint16_t dst_h)
{
    uint32_t rem = src_h % dst_h;
    uint32_t acc = rem;
    for (uint32_t i = 0; i < dst_h; ++i) {
        int extra = ((int32_t)acc >= (int32_t)dst_h);
        y_extra[i] = (uint8_t)extra;
        if (extra) acc -= dst_h;
        acc += rem;
    }
}

/* Mode-0 row: copy one src pixel per dst column, stepping src by the
 * per-column x-step value. */
static void blit_nearest_row(uint8_t *dst, const uint8_t *src,
                             const uint32_t *x_step, uint32_t dst_w)
{
    const uint32_t *step = x_step;
    for (uint32_t dx = 0; dx < dst_w; ++dx) {
        dst[dx] = *src;
        src += *step++;
    }
}

/* Mode-1 row: each dst pixel = 1D horizontal box-filter of step[dx]
 * src pixels via sample_box_1d. */
static void blit_box_1d_row(uint8_t *dst, const uint8_t *src,
                            const uint32_t *x_step, uint32_t dst_w)
{
    const uint32_t *step = x_step;
    for (uint32_t dx = 0; dx < dst_w; ++dx) {
        int sw = (int)*step;
        if (sw < 1) sw = 1;
        dst[dx] = sample_box_1d(src, sw);
        src += *step++;
    }
}

/* Mode-2 row: each dst pixel = 2D box-filter over step[dx] × sh src
 * pixels via sample_box_2d (`sh` already clamped by the caller). */
static void blit_box_2d_row(uint8_t *dst, const uint8_t *src,
                            const uint32_t *x_step, uint32_t dst_w,
                            int sh, uint16_t src_stride)
{
    const uint32_t *step = x_step;
    for (uint32_t dx = 0; dx < dst_w; ++dx) {
        int sw = (int)*step;
        if (sw == 0) sw = 1;
        dst[dx] = sample_box_2d(src, sw, (int)src_stride, sh);
        src += *step++;
    }
}

/* ---- BlitAlphaScaled driver + backbuffer wrapper ----------------- */

void BlitAlphaScaled(uint16_t src_w, uint16_t src_h, const uint8_t *src,
                     uint16_t dst_w, uint16_t dst_h, uint8_t *dst,
                     uint16_t mode)
{
    if (!src || !dst) return;
    if (!src_w || !src_h || !dst_w || !dst_h) return;
    if (dst_w > ALPHA_SCALED_MAX_DIM || dst_h > ALPHA_SCALED_MAX_DIM) return;

    static uint32_t x_step [ALPHA_SCALED_LUT_LEN];
    static uint8_t  y_extra[ALPHA_SCALED_LUT_LEN];
    build_x_step_lut    (x_step,  src_w, dst_w);
    build_y_extra_table (y_extra, src_h, dst_h);

    const uint8_t *srcrow       = src;
    uint8_t       *dstrow       = dst;
    int            src_step_row = (src_h / dst_h) * src_w;
    uint32_t       y_base       = src_h / dst_h;

    for (uint32_t dy = 0; dy < dst_h; ++dy) {
        switch (mode) {
        case BLIT_MODE_NEAREST:
            blit_nearest_row(dstrow, srcrow, x_step, dst_w);
            break;
        case BLIT_MODE_BOX_1D:
            blit_box_1d_row (dstrow, srcrow, x_step, dst_w);
            break;
        case BLIT_MODE_BOX_2D: {
            uint32_t sh = y_base + (y_extra[dy] ? 1 : 0);
            if (sh == 0) sh = 1;
            if (dy + sh > dst_h) sh = (uint32_t)(dst_h - dy);
            blit_box_2d_row(dstrow, srcrow, x_step, dst_w, (int)sh, src_w);
            break;
        }
        default:
            return;
        }
        srcrow += src_step_row;
        if (y_extra[dy]) srcrow += src_w;
        dstrow += dst_w;
    }
}

/* Convenience wrapper that allocates a scratch dst buffer of `dw × dh`,
 * runs BlitAlphaScaled, then color-key-blits to the shadow buffer
 * (palette idx 0 transparent). Used by EntityRenderAll for entities
 * with flag 0x100 + 0x400 (alpha + perspective scaled). */
void BlitAlphaScaledToBackbuffer(int16_t dx, int16_t dy,
                                 uint16_t sw, uint16_t sh,
                                 uint16_t dw, uint16_t dh,
                                 const uint8_t *src, uint16_t mode)
{
    if (!src || dw == 0 || dh == 0) return;

    /* Scratch buffer for the scaled output. Reuse across calls. */
    static uint8_t *s_alpha_scratch    = NULL;
    static size_t   s_alpha_scratch_sz = 0;
    size_t need = (size_t)dw * (size_t)dh;
    if (need > s_alpha_scratch_sz) {
        free(s_alpha_scratch);
        s_alpha_scratch    = (uint8_t *)malloc(need);
        s_alpha_scratch_sz = s_alpha_scratch ? need : 0;
    }
    if (!s_alpha_scratch) return;

    BlitAlphaScaled(sw, sh, src, dw, dh, s_alpha_scratch, mode);
    BlitSpriteToBackbuffer((uint16_t)dx, (uint16_t)dy, 0, 0, dw, dh,
                           dw, dh, s_alpha_scratch, 0);
}
