/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* src/font.c — "Futura.30" bitmap font: parse + raster.
 *
 * .fut file format (all multi-byte values are BIG-ENDIAN):
 *   +0     DWORD magic     = 0x000003F3
 *   +0x18  DWORD sub-magic = 0x000003E9 (1bpp) or 0x000003EA (colour)
 *   +0x60..0x90 character-cell descriptor:
 *     +0x62 byte  cell width
 *     +0x6E word  baseline
 *     +0x70 byte  flags (bit 0x40 = colour font)
 *     +0x72 word  ascent
 *     +0x74 word  advance
 *     +0x7A byte  first_char
 *     +0x7B byte  last_char
 *     +0x80 word  glyph stride
 *     +0x82..0x89 offsets to per-glyph width / advance / kern tables
 *                 (BE DWORD)
 *     +0x90 byte  plane_count (colour only)
 *     +0x9A..     per-plane offsets
 *   Glyph bitmaps live at file_base + 0x20 + each offset. */

#include "wacki.h"
#include <string.h>

extern void *xmalloc(uint32_t sz);

/* 0x38-byte parsed font descriptor. */
struct FontHandle {
    uint16_t advance;       /* +0x00 default cursor advance when
                             *       advance_tab is NULL */
    uint16_t baseline;      /* +0x02 */
    uint16_t glyph_stride;  /* +0x04 bytes per row in plane bitmap */
    uint16_t cell_width;    /* +0x06 */
    uint8_t  first_char;    /* +0x08 */
    uint8_t  last_char;     /* +0x09 */
    uint8_t *plane[8];      /* +0x0A..+0x29 up to 8 plane pointers */
    uint8_t *width_tab;     /* +0x2A — 4 B/glyph: BE16 bit_offset +
                             *         BE16 bbox_width */
    uint8_t *advance_tab;   /* +0x2E — 2 B/glyph: BE16 cursor advance */
    uint8_t *kern_tab;      /* +0x32 — 2 B/glyph: BE16 signed kern */
    uint16_t plane_count;   /* +0x36 */
};

static inline uint32_t be32(const uint8_t *p)
{ return ((uint32_t)p[0]<<24) | ((uint32_t)p[1]<<16) |
         ((uint32_t)p[2]<<8)  |  (uint32_t)p[3]; }
static inline uint16_t be16(const uint8_t *p)
{ return (uint16_t)((p[0]<<8) | p[1]); }

/* ---- .fut font header layout -------------------------------------- */

/* Magic values in the .fut header. All multi-byte values are BIG-ENDIAN. */
#define FUT_MAGIC                       0x3F3
#define FUT_SUB_MAGIC_A                 0x3E9
#define FUT_SUB_MAGIC_B                 0x3EA

/* Sentinel that confirms the header layout we know how to parse. The
 * byte at FUT_OFF_CELL_WIDTH_SENTINEL must equal FUT_CELL_WIDTH_OK. */
#define FUT_OFF_CELL_WIDTH_SENTINEL     0x62
#define FUT_CELL_WIDTH_OK               0x0C

/* Fixed header field offsets. */
#define FUT_OFF_MAGIC                   0x00
#define FUT_OFF_SUB_MAGIC               0x18
#define FUT_OFF_CELL_BASE               0x20   /* all later offsets relative to this */
#define FUT_OFF_BASELINE_BE16           0x6E   /* baseline = (raw[0x6E]<<8) | raw[0x6F] */
#define FUT_OFF_FLAGS_BYTE              0x70   /* bit 0x40 = multi-plane font */
#define FUT_OFF_ADVANCE                 0x72
#define FUT_OFF_CELL_WIDTH              0x74
#define FUT_OFF_FIRST_CHAR              0x7A
#define FUT_OFF_LAST_CHAR               0x7B
#define FUT_OFF_PLANE0_OFFSET           0x7C   /* 1-bpp font: single plane */
#define FUT_OFF_GLYPH_STRIDE            0x80
#define FUT_OFF_WIDTH_TAB_OFFSET        0x82
#define FUT_OFF_ADVANCE_TAB_OFFSET      0x86
#define FUT_OFF_KERN_TAB_OFFSET         0x8A
#define FUT_OFF_PLANE_COUNT             0x90   /* multi-plane only */
#define FUT_OFF_PLANE_TABLE             0x9A   /* multi-plane: 4 bytes per plane */
#define FUT_FLAG_MULTI_PLANE            0x40
#define FUT_PLANE_OFFSET_ENTRY_BYTES    4

/* Fallback cell height used when `font.baseline` is zero. Futura.30
 * (the only font the shipped game uses) has a 30-row glyph cell. */
#define FUTURA_30_CELL_HEIGHT           30

/* Decode the .fut flags byte: 1-bpp (default) or multi-plane. */
static int fut_is_multi_plane(const uint8_t *raw)
{
    return (raw[FUT_OFF_FLAGS_BYTE] & FUT_FLAG_MULTI_PLANE) != 0;
}

/* Resolve the per-plane bitmap pointers from the header. The single-
 * plane path uses a fixed offset; the multi-plane path reads N table
 * entries (one BE32 per plane). */
static void fut_resolve_planes(FontHandle *f, const uint8_t *raw,
                               const uint8_t *cell_base)
{
    if (!fut_is_multi_plane(raw)) {
        f->plane_count = 1;
        uint32_t off = be32(raw + FUT_OFF_PLANE0_OFFSET);
        f->plane[0] = (uint8_t *)cell_base + off;
        return;
    }

    f->plane_count = raw[FUT_OFF_PLANE_COUNT];
    for (int i = 0; i < f->plane_count; ++i) {
        uint32_t off = be32(raw + FUT_OFF_PLANE_TABLE +
                            i * FUT_PLANE_OFFSET_ENTRY_BYTES);
        if (off) f->plane[i] = (uint8_t *)cell_base + off;
    }
}

/* ---- ParseFutFontFile -------------------------------------------- *
 *
 * Decode a .fut font file into a FontHandle. Returns NULL on magic
 * mismatch, cell-width-sentinel mismatch, or OOM. */
FontHandle *ParseFutFontFile(const uint8_t *raw)
{
    if (be32(raw + FUT_OFF_MAGIC) != FUT_MAGIC) return NULL;
    uint32_t sub = be32(raw + FUT_OFF_SUB_MAGIC);
    if (sub != FUT_SUB_MAGIC_A && sub != FUT_SUB_MAGIC_B) return NULL;
    if (raw[FUT_OFF_CELL_WIDTH_SENTINEL] != FUT_CELL_WIDTH_OK) return NULL;

    FontHandle *f = (FontHandle *)xmalloc(sizeof *f);
    if (!f) return NULL;
    memset(f, 0, sizeof *f);

    f->advance      = be16(raw + FUT_OFF_ADVANCE);
    f->baseline     = (uint16_t)((raw[FUT_OFF_BASELINE_BE16    ] << 8)
                               |  raw[FUT_OFF_BASELINE_BE16 + 1]);
    f->glyph_stride = be16(raw + FUT_OFF_GLYPH_STRIDE);
    f->cell_width   = be16(raw + FUT_OFF_CELL_WIDTH);
    f->first_char   = raw[FUT_OFF_FIRST_CHAR];
    f->last_char    = raw[FUT_OFF_LAST_CHAR];

    const uint8_t *cell_base = raw + FUT_OFF_CELL_BASE;
    fut_resolve_planes(f, raw, cell_base);

    f->width_tab   = (uint8_t *)cell_base + be32(raw + FUT_OFF_WIDTH_TAB_OFFSET);
    f->advance_tab = (uint8_t *)cell_base + be32(raw + FUT_OFF_ADVANCE_TAB_OFFSET);
    f->kern_tab    = (uint8_t *)cell_base + be32(raw + FUT_OFF_KERN_TAB_OFFSET);
    return f;
}

/* ---- RenderTextLineToBuffer -------------------------------------- *
 *
 * Renders `text` (NUL-terminated byte string, each byte ∈
 * [first_char..last_char]) into `t->pixels` at cursor x = `t->x`,
 * advancing the cursor per-glyph. Composed glyphs use the colour
 * `color_base` (1-bpp) or `color_base + plane_bitmap` (multi-plane). */
void RenderTextLineToBuffer(TextRenderTarget *t, const uint8_t *text)
{
    if (!t || !text) return;
    FontHandle *f = t->font;
    if (!f)        return;

    uint16_t stride = t->stride;
    uint8_t *pixels = t->pixels;
    int16_t  x      = (int16_t)t->x;
    uint8_t  cbase  = t->color_base;

    /* The original target struct carries (y_baseline, y_advance) so
     * a sub-rect is renderable; ours doesn't, so we treat y_baseline
     * = 0 and lines = font.baseline (= full glyph cell height,
     * Futura.30 = 30 rows). The `baseline` field is misnamed in the
     * original — it's actually the CELL HEIGHT. Both callers (speech
     * balloon: e->pixels + i*30*max_w already at line origin; save
     * menu: g_back_shadow + ry*stride at top of slot row) expect
     * this. */
    uint16_t lines = f->baseline ? f->baseline : FUTURA_30_CELL_HEIGHT;

    uint8_t *row0 = pixels;

    /* T101 — proper Futura.30 layout:
     *   width_tab[idx*4+0] = bit_offset into the shared plane[]
     *                        bitmap (= where THIS glyph's bits start)
     *   width_tab[idx*4+2] = bbox_width (pixels to draw)
     *   advance_tab[idx*2] = post-render cursor step (typically >
     *                        bbox_w to add right-side bearing)
     *   kern_tab[idx*2]    = signed pre-render cursor adjust
     *
     * Earlier port read `width_tab[idx*4+0]` as glyph width (it's
     * actually the BIT OFFSET) and stepped cursor by it — producing
     * text rendered from wrong source bits AND spaced wrongly. */
    for (; *text; ++text) {
        uint8_t ch = *text;
        if (ch < f->first_char) ch = f->first_char;
        if (ch > f->last_char)  ch = f->last_char;
        uint8_t idx = (uint8_t)(ch - f->first_char);

        /* Kern (signed BE16) — shift cursor before render. Clamp to 0. */
        if (f->kern_tab) {
            int16_t kern = (int16_t)be16(&f->kern_tab[idx*2]);
            x += kern;
            if (x < 0) x = 0;
        }

        uint16_t bit_off = be16(&f->width_tab[idx*4 + 0]);
        uint16_t glyph_w = be16(&f->width_tab[idx*4 + 2]);

        uint16_t byte_off  = bit_off >> 3;     /* byte offset into row */
        uint8_t  start_mask = (uint8_t)(0x80u >> (bit_off & 7));

        /* Clip glyph_w to remaining stride space so we don't write
         * past the back-buffer edge. */
        if (x + glyph_w > stride) {
            if (x >= stride) goto advance;
            glyph_w = (uint16_t)(stride - x);
        }

        if (glyph_w == 0) goto advance;

        for (uint16_t row = 0; row < lines; ++row) {
            uint8_t *dst = row0 + (uint32_t)stride * row + (uint16_t)x;
            uint8_t  mask = start_mask;
            uint32_t row_base = (uint32_t)f->glyph_stride * row + byte_off;

            if (f->plane_count <= 1) {
                const uint8_t *gp = f->plane[0] + row_base;
                for (uint16_t col = 0; col < glyph_w; ++col) {
                    if (*gp & mask) dst[col] = cbase;
                    mask >>= 1;
                    if (!mask) { mask = 0x80; ++gp; }
                }
            } else {
                /* Colour font: bits combine across planes → palette
                 * index `cbase - 1 + bit_mask`. Each plane
                 * contributes one bit (plane 0 = LSB). */
                uint32_t plane_pos = row_base;
                for (uint16_t col = 0; col < glyph_w; ++col) {
                    uint8_t bits = 0;
                    for (int p = 0; p < f->plane_count && p < 8; ++p) {
                        if (f->plane[p] && (f->plane[p][plane_pos] & mask))
                            bits |= (uint8_t)(1u << p);
                    }
                    if (bits) dst[col] = (uint8_t)(cbase - 1 + bits);
                    mask >>= 1;
                    if (!mask) { mask = 0x80; ++plane_pos; }
                }
            }
        }

advance:
        /* Cursor step after render — use advance_tab if present,
         * else default font.advance (= em width). */
        {
            uint16_t step;
            if (f->advance_tab) step = be16(&f->advance_tab[idx*2]);
            else                step = f->advance;
            x += (int16_t)step;
        }
    }
    t->x = (uint16_t)(x < 0 ? 0 : x);
}

/* ---- MeasureTextLine --------------------------------------------- *
 *
 * Returns pixel width of `text` rendered with `font`. Used by op 0x09
 * SHOW_TEXT to compute speech balloon width.
 *
 * T101 fix — an earlier port read `width_tab[idx*4]` as the glyph
 * width, but it's actually the BIT OFFSET into the shared bitmap
 * plane. That gave wrong measurements which cascaded into wrong
 * bubble sizes and clipped multi-line text. The advance lives in
 * advance_tab, read with idx*2. */
int MeasureTextLine(FontHandle *f, const uint8_t *text)
{
    if (!f || !text) return 0;
    int x = 0;
    for (; *text; ++text) {
        uint8_t ch = *text;
        if (ch < f->first_char) ch = f->first_char;
        if (ch > f->last_char)  ch = f->last_char;
        uint8_t idx = (uint8_t)(ch - f->first_char);
        if (f->kern_tab) {
            int16_t kern = (int16_t)be16(&f->kern_tab[idx*2]);
            x += kern;
            if (x < 0) x = 0;
        }
        uint16_t step;
        if (f->advance_tab) step = be16(&f->advance_tab[idx*2]);
        else                step = f->advance;
        x += step;
    }
    return x;
}

/* ---- FindKeyInTaggedTable ---------------------------------------- *
 *
 * Walk a "tagged" table terminated by an entry whose first char is
 * '!'. */
uint16_t FindKeyInTaggedTable(const char *table, char tag, int16_t key)
{
    uint16_t idx = 0;
    const uint8_t *p = (const uint8_t *)table;
    while (*p != '!') {
        if (*p == (uint8_t)tag &&
            (key == -1 || *(int16_t *)(p + 2) == key))
            return idx;
        uint8_t len = p[1];
        p   += (size_t)len * 2;
        idx += len;
    }
    return 0;
}
