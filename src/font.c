/*
 * font.c — "Futura.30" bitmap font: parse + raster.
 *
 * Original addresses:
 * ParseFutFontFile 0x00413690
 * RenderTextLineToBuffer 0x00413970
 * FindKeyInTaggedTable 0x00401240
 *
 * Format (all multi-byte values are big-endian!):
 * +0 DWORD magic = 0x000003F3
 * +0x18 DWORD sub-magic = 0x000003E9 (1bpp) or 0x000003EA (color)
 * +0x60..0x90 character-cell descriptor:
 * +0x62 byte cell width
 * +0x6E word baseline
 * +0x70 byte flags (bit 0x40 = color font)
 * +0x72 word ascent
 * +0x74 word advance
 * +0x7A byte first_char
 * +0x7B byte last_char
 * +0x80 word glyph stride
 * +0x82..0x89 offsets to per-glyph width/Y-offset tables (BE DWORD)
 * +0x90 byte plane_count (color only)
 * +0x9A.. (per-plane offsets)
 * Glyph bitmaps live at file_base + 0x20 + each offset.
 */
#include "wacki.h"
#include <string.h>

extern void *xmalloc(uint32_t sz);

/* 0x38-byte parsed font descriptor — 1:1 with ParseFutFontFile output.
 * Original struct layout (Ghidra `psVar7[N]` byte offsets):
 * +0x00 advance (BE16) — default cursor step when advance_tab is NULL
 * +0x02 baseline (BE16)
 * +0x04 glyph_stride (BE16) — bytes per row in shared plane bitmap
 * +0x06 cell_width (BE16)
 * +0x08 first_char (u8)
 * +0x09 last_char (u8)
 * +0x0A plane[0..7] — up to 8 plane pointers
 * +0x2A width_tab — 4 bytes per glyph: BE16 bit_offset + BE16 bbox_width
 * +0x2E advance_tab — 2 bytes per glyph: BE16 cursor advance (was
 * mislabelled `yoff_tab` in earlier port — actually the post-
 * render cursor step; T101 fix renamed)
 * +0x32 kern_tab — 2 bytes per glyph: BE16 signed pre-render kern shift
 * +0x36 plane_count (u16) */
struct FontHandle {
    uint16_t advance;       /* +0x00 default advance */
    uint16_t baseline;      /* +0x02 */
    uint16_t glyph_stride;  /* +0x04 */
    uint16_t cell_width;    /* +0x06 */
    uint8_t  first_char;    /* +0x08 */
    uint8_t  last_char;     /* +0x09 */
    uint8_t *plane[8];      /* +0x0A..+0x29 */
    uint8_t *width_tab;     /* +0x2A — 4 B/glyph: BE16 bit_offset + BE16 bbox_width */
    uint8_t *advance_tab;   /* +0x2E — 2 B/glyph: BE16 cursor advance
 * (was mislabelled `yoff_tab`) */
    uint8_t *kern_tab;      /* +0x32 — 2 B/glyph: BE16 signed kern */
    uint16_t plane_count;   /* +0x36 */
};

static inline uint32_t be32(const uint8_t *p)
{ return ((uint32_t)p[0]<<24) | ((uint32_t)p[1]<<16) |
         ((uint32_t)p[2]<<8)  |  (uint32_t)p[3]; }
static inline uint16_t be16(const uint8_t *p)
{ return (uint16_t)((p[0]<<8) | p[1]); }

/* ------------------------------------------------------------------------- *
 * ParseFutFontFile — 0x00413690
 * ------------------------------------------------------------------------- */
FontHandle *ParseFutFontFile(const uint8_t *raw)
{
    if (be32(raw) != 0x3F3) return NULL;
    uint32_t sub = be32(raw + 0x18);
    if (sub != 0x3E9 && sub != 0x3EA) return NULL;
    if (raw[0x62] != 0x0C)            return NULL;     /* cell-width sentinel */

    FontHandle *f = (FontHandle *)xmalloc(sizeof *f);
    if (!f) return NULL;
    memset(f, 0, sizeof *f);

    f->advance      = be16(raw + 0x72);
    f->baseline     = (uint16_t)((raw[0x6E]<<8) | raw[0x6F]);
    f->glyph_stride = be16(raw + 0x80);
    f->cell_width   = be16(raw + 0x74);
    f->first_char   = raw[0x7A];
    f->last_char    = raw[0x7B];

    const uint8_t *cell_base = raw + 0x20;
    if (!(raw[0x70] & 0x40)) {
        /* 1-bpp */
        f->plane_count = 1;
        uint32_t off = be32(raw + 0x7C);
        f->plane[0] = (uint8_t *)cell_base + off;
    } else {
        f->plane_count = raw[0x90];
        for (int i = 0; i < f->plane_count; ++i) {
            uint32_t off = be32(raw + 0x9A + i*4);
            if (off) f->plane[i] = (uint8_t *)cell_base + off;
        }
    }
    f->width_tab   = (uint8_t *)cell_base + be32(raw + 0x82);
    f->advance_tab = (uint8_t *)cell_base + be32(raw + 0x86);   /* T101: was yoff_tab */
    f->kern_tab    = (uint8_t *)cell_base + be32(raw + 0x8A);
    return f;
}

/* ------------------------------------------------------------------------- *
 * RenderTextLineToBuffer — 0x00413970
 *
 * target_desc[0] = stride (= screen width when drawing direct)
 * [1] = pixels (uint8 *)
 * [2] = font (FontHandle *)
 * [3] = x cursor (in/out)
 * [4] = color_base palette index
 * text = NUL-terminated byte string (each byte ∈ [first_char..last_char]).
 *
 * Composed glyphs use the colour `color_base` (1-bpp) or
 * `color_base + plane_bitmap` (multi-plane).
 * ------------------------------------------------------------------------- */
void RenderTextLineToBuffer(TextRenderTarget *t, const uint8_t *text)
{
    if (!t || !text) return;
    FontHandle *f = t->font;
    if (!f)        return;

    uint16_t stride = t->stride;
    uint8_t *pixels = t->pixels;
    int16_t  x      = (int16_t)t->x;
    uint8_t  cbase  = t->color_base;

    /* NOTE — original RenderTextLineToBuffer @ 0x00413970:
 * target.y_advance (+0x02) = bottom of render area
 * target.y_baseline (+0x0E) = top of render area within target
 * font[+2] (our `baseline` field — misnamed; actually CELL HEIGHT)
 * lines = min(y_advance - y_baseline, font.baseline)
 * row0 = pixels + stride * y_baseline
 *
 * Our TextRenderTarget doesn't carry y_baseline/y_advance — use
 * y_baseline=0 (top of buffer) and lines=font.baseline (full glyph
 * cell height, Futura.30 = 30 rows). Both callers (speech balloon:
 * e->pixels + i*30*max_w already at line origin; save menu:
 * g_back_shadow + ry*stride at top of slot row) expect this. */
    uint16_t lines = f->baseline ? f->baseline : 30;

    uint8_t *row0 = pixels;

    /* T101 — proper Futura.30 layout per :
 * width_tab[idx*4+0] = bit_offset into the shared plane[] bitmap
 * (= where THIS glyph's bits start, in bits)
 * width_tab[idx*4+2] = bbox_width (pixels to draw for this glyph)
 * advance_tab[idx*2] = post-render cursor step (typically > bbox_w
 * to add right-side bearing)
 * kern_tab[idx*2] = signed pre-render cursor adjust
 *
 * Earlier port read `width_tab[idx*4+0]` as glyph width (it's actually
 * the BIT OFFSET) and stepped cursor by it — which produced text that
 * was both rendered from wrong source bits AND spaced wrongly. */
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

        /* Clip glyph_w to remaining stride space so we don't write past
 * the back-buffer edge. */
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
                /* Color font: bits combine across planes → palette index
 * `cbase - 1 + bit_mask`. Each plane contributes one bit
 * of the result index (plane 0 = LSB). */
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
        /* Cursor step after render — use advance_tab if present, else
 * default font.advance (= em width). */
        {
            uint16_t step;
            if (f->advance_tab) step = be16(&f->advance_tab[idx*2]);
            else                step = f->advance;
            x += (int16_t)step;
        }
    }
    t->x = (uint16_t)(x < 0 ? 0 : x);
}

/* ------------------------------------------------------------------------- *
 * MeasureTextLine —
 *
 * uVar7 = 0;
 * for each char c in text:
 * idx = clamp(c, first_char, last_char) - first_char
 * if (kern_tab) uVar7 += be16(kern_tab[idx*2]);
 * if (uVar7 < 0) uVar7 = 0;
 * if (advance_tab) uVar7 += be16(advance_tab[idx*2]); // psVar7[0x17]
 * else uVar7 += font.advance; // default em
 *
 * Returns pixel width of `text` rendered with `font`. Used by op 0x09
 * SHOW_TEXT to compute speech balloon width.
 *
 * T101 fix — earlier port read `width_tab[idx*4]` (= bit_offset into the
 * shared bitmap plane, NOT a width!) which gave wrong measurements that
 * cascaded into wrong bubble sizes + clipped multi-line text. Original
 * advance comes from `psVar7[0x17]` (`advance_tab`) read with idx*2. */
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

/* ------------------------------------------------------------------------- *
 * FindKeyInTaggedTable — 0x00401240
 * Walk a "tagged" table terminated by an entry whose first char is '!'.
 * ------------------------------------------------------------------------- */
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
