/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * assets-explorer/src/render.c — asset → RGBA (see render.h).
 *
 * Paletted assets are 8bpp; we map each index through a viewer-held 256-colour
 * palette. .pal triplets are stored R,G,B (the engine's present path proves it:
 * video_sdl.c maps e[0]→R,e[1]→G,e[2]→B; docs §7's "BGR" was wrong). ANIM frames
 * come from the engine's real loaders —
 * kind 3 (RLE) is decoded with the engine's DepackRleFrame, kind 2 is already
 * flat 8bpp. */

#include "render.h"

#include "wacki/types.h"   /* AnimAsset */
#include "wacki/api.h"     /* DepackRleFrame */

#include <stdlib.h>
#include <string.h>

#define PAL_BYTES        768
#define PIC_HEADER_BYTES 0x308
#define ANIM_KIND_RAW    2     /* flat 8bpp frames */
#define ANIM_KIND_RICH   3     /* RLE-compressed frames */
#define MAX_DIM          8192  /* sanity ceiling on any single image axis */

static uint8_t s_pal[PAL_BYTES];
static int     s_has_pal = 0;

void viewer_set_palette(const uint8_t *pal768)
{
    if (pal768) {
        memcpy(s_pal, pal768, PAL_BYTES);
        s_has_pal = 1;
    } else {
        for (int i = 0; i < 256; ++i)
            s_pal[i * 3] = s_pal[i * 3 + 1] = s_pal[i * 3 + 2] = (uint8_t)i;
        s_has_pal = 0;
    }
}

int viewer_has_palette(void) { return s_has_pal; }

/* One paletted index → RGBA. .pal triplets are R,G,B — verified against the
 * engine's own present path (video_sdl.c: e[0]→R, e[1]→G, e[2]→B). The docs'
 * "BGR" claim (§7) was wrong, same as the PIC header (§6). */
static void put_indexed(uint8_t idx, int transparent_idx0, uint8_t *out)
{
    if (transparent_idx0 && idx == 0) {
        out[0] = out[1] = out[2] = out[3] = 0;
        return;
    }
    out[0] = s_pal[idx * 3 + 0];   /* R */
    out[1] = s_pal[idx * 3 + 1];   /* G */
    out[2] = s_pal[idx * 3 + 2];   /* B */
    out[3] = 255;
}

static int alloc_img(ViewImage *out, int w, int h)
{
    if (w <= 0 || h <= 0 || w > MAX_DIM || h > MAX_DIM) return 0;
    out->rgba = (uint8_t *)malloc((size_t)w * (size_t)h * 4);
    if (!out->rgba) return 0;
    out->w = w;
    out->h = h;
    return 1;
}

static void blit_indexed(const uint8_t *px, int w, int h,
                         int transparent_idx0, ViewImage *out)
{
    size_t n = (size_t)w * (size_t)h;
    for (size_t i = 0; i < n; ++i)
        put_indexed(px[i], transparent_idx0, out->rgba + i * 4);
}

int view_render_anim_frame(struct AnimAsset *a, int f, ViewImage *out)
{
    if (!a || !a->pixel_ptrs || f < 0 || f >= a->frame_count) return 0;
    /* Only the two pixel kinds; a 1bpp mask atlas (kind 0) isn't a sprite. */
    if (a->kind != ANIM_KIND_RAW && a->kind != ANIM_KIND_RICH) return 0;

    int w = a->off_widths[f];
    int h = a->off_heights[f];
    if (!alloc_img(out, w, h)) return 0;

    const uint8_t *src = a->pixel_ptrs[f];
    if (a->kind == ANIM_KIND_RICH) {
        uint8_t *tmp = (uint8_t *)malloc((size_t)w * (size_t)h);
        if (!tmp) { free(out->rgba); out->rgba = NULL; return 0; }
        DepackRleFrame(src, tmp, w * h);
        blit_indexed(tmp, w, h, 1, out);
        free(tmp);
    } else {
        blit_indexed(src, w, h, 1, out);
    }
    return 1;
}

int view_render_anim_sheet(struct AnimAsset *a, ViewImage *out)
{
    if (!a || a->frame_count == 0) return 0;
    int n  = a->frame_count;
    int mw = a->max_w, mh = a->max_h;
    if (mw <= 0 || mh <= 0) return 0;

    /* Square-ish grid; each frame gets a max_w×max_h cell, top-left aligned. */
    int cols = 1;
    while (cols * cols < n) ++cols;
    int rows = (n + cols - 1) / cols;
    int sw = cols * mw, sh = rows * mh;
    if (!alloc_img(out, sw, sh)) return 0;
    memset(out->rgba, 0, (size_t)sw * sh * 4);          /* transparent grid */

    for (int i = 0; i < n; ++i) {
        ViewImage f = {0};
        if (!view_render_anim_frame(a, i, &f)) continue;
        int cx = (i % cols) * mw;
        int cy = (i / cols) * mh;
        for (int y = 0; y < f.h && y < mh; ++y)
            memcpy(out->rgba + ((size_t)(cy + y) * sw + cx) * 4,
                   f.rgba + (size_t)y * f.w * 4, (size_t)f.w * 4);
        view_image_free(&f);
    }
    return 1;
}

int view_render_mask(struct AnimAsset *a, int f, ViewImage *out)
{
    if (!a || !a->pixel_ptrs || f < 0 || f >= a->frame_count) return 0;
    int w = a->off_widths[f], h = a->off_heights[f];
    if (w <= 0 || h <= 0) return 0;
    if (!alloc_img(out, w, h)) return 0;

    const uint8_t *px   = a->pixel_ptrs[f];
    const uint8_t *base = (const uint8_t *)a->raw_buffer;

    /* Byte span of this frame (pixel_ptrs are in-order offsets into
     * raw_buffer); clamp into the buffer so a malformed table can't
     * overread. */
    long span;
    if (f + 1 < a->frame_count) span = (long)(a->pixel_ptrs[f + 1] - px);
    else                        span = (long)(base + a->raw_size - px);
    if (span <= 0 || span > (long)a->raw_size) span = (long)a->raw_size;

    int  stride1 = (w + 7) / 8;
    long sz1 = (long)stride1 * h;          /* 1bpp byte count */
    long sz8 = (long)w * h;                /* 8bpp byte count */
    int  is_1bpp = labs(span - sz1) <= labs(span - sz8);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            long bi  = is_1bpp ? (long)y * stride1 + x / 8 : (long)y * w + x;
            int  set = 0;
            if (bi < span)
                set = is_1bpp ? (px[bi] & (0x80 >> (x & 7))) != 0 : px[bi] != 0;
            uint8_t *o = out->rgba + ((size_t)y * w + x) * 4;
            if (set) { o[0] = 120; o[1] = 220; o[2] = 140; o[3] = 255; }  /* region */
            else     { o[0] = 24;  o[1] = 24;  o[2] = 30;  o[3] = 255; }  /* backdrop */
        }
    }
    return 1;
}

int view_render_pic(const void *blob, uint32_t sz, ViewImage *out)
{
    if (!blob || sz < PIC_HEADER_BYTES + 1) return 0;
    const uint8_t *b = (const uint8_t *)blob;
    /* Leading 4 bytes are the "RAWB" magic; width is u16 @4, height u16 @6
     * (the engine's paint_rawb_pic reads it the same way). Pixels are flat
     * 8bpp at 0x308. NOTE: docs/asset-format.md §6 wrongly listed w@0/h@2. */
    int w = b[4] | (b[5] << 8);
    int h = b[6] | (b[7] << 8);
    if ((uint64_t)PIC_HEADER_BYTES + (uint64_t)w * (uint64_t)h > sz) return 0;
    if (!alloc_img(out, w, h)) return 0;
    blit_indexed(b + PIC_HEADER_BYTES, w, h, 0, out);   /* opaque background */
    return 1;
}

int view_render_pal(const void *blob, uint32_t sz, int cell, ViewImage *out)
{
    if (!blob || sz < PAL_BYTES) return 0;
    if (cell < 2) cell = 18;
    const uint8_t *p = (const uint8_t *)blob;
    int w = 16 * cell, h = 16 * cell;
    if (!alloc_img(out, w, h)) return 0;

    for (int cy = 0; cy < 16; ++cy) {
        for (int cx = 0; cx < 16; ++cx) {
            int idx = cy * 16 + cx;
            uint8_t r = p[idx * 3 + 0], g = p[idx * 3 + 1], bl = p[idx * 3 + 2];
            for (int y = 0; y < cell; ++y) {
                for (int x = 0; x < cell; ++x) {
                    uint8_t *o = out->rgba + ((size_t)(cy * cell + y) * w
                                             + (cx * cell + x)) * 4;
                    if (x == 0 || y == 0) {        /* 1px grid line */
                        o[0] = o[1] = o[2] = 40; o[3] = 255;
                    } else {
                        o[0] = r; o[1] = g; o[2] = bl; o[3] = 255;
                    }
                }
            }
        }
    }
    return 1;
}

void view_image_free(ViewImage *img)
{
    if (img && img->rgba) { free(img->rgba); img->rgba = NULL; }
}
