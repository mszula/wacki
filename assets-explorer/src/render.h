/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * assets-explorer/src/render.h — turn a depacked asset into an RGBA image for preview.
 *
 * Uses the engine's real loaders/decoders (LoadAssetFromDtaBase, DepackRleFrame)
 * and maps 8bpp paletted pixels through a viewer-held palette. No SDL here —
 * main.c uploads the ViewImage to an SDL_Texture. */

#ifndef WACKI_VIEWER_RENDER_H
#define WACKI_VIEWER_RENDER_H

#include <stdint.h>

/* Tightly-packed RGBA8888, row-major, `w*h*4` bytes. Byte order R,G,B,A. */
typedef struct {
    uint8_t *rgba;
    int      w, h;
} ViewImage;

/* Install the active 256-colour palette. `pal768` is 768 bytes of R,G,B
 * triplets exactly as stored in a .pal asset. Pass NULL for a grayscale ramp
 * (so paletted assets are still legible before any .pal is chosen). */
void viewer_set_palette(const uint8_t *pal768);
int  viewer_has_palette(void);

struct AnimAsset;   /* forward decl — avoids pulling wacki.h into the UI TU */

/* Render frame `f` of an ANIM atlas (kind 2 raw / kind 3 RLE) to RGBA, with
 * palette index 0 transparent. Returns 1 on success, 0 if unrenderable
 * (e.g. a 1bpp mask atlas, kind 0). */
int  view_render_anim_frame(struct AnimAsset *a, int f, ViewImage *out);

/* Render every frame of an ANIM atlas into one RGBA grid (sprite-sheet),
 * cols = ceil(sqrt(frame_count)), each cell max_w×max_h. Returns 1/0. */
int  view_render_anim_sheet(struct AnimAsset *a, ViewImage *out);

/* Render a MASK / FILD frame as a green-on-dark region map. Auto-detects
 * 1bpp-packed vs 8bpp by the frame's byte span. Returns 1/0. */
int  view_render_mask(struct AnimAsset *a, int f, ViewImage *out);

/* Render a .pic background: u16 w @0, u16 h @2, 8bpp pixels @0x308. Opaque
 * (index 0 is a real colour in a background, not transparent). */
int  view_render_pic(const void *blob, uint32_t sz, ViewImage *out);

/* Render a .pal as a 16×16 swatch grid, `cell` px per swatch. */
int  view_render_pal(const void *blob, uint32_t sz, int cell, ViewImage *out);

void view_image_free(ViewImage *img);

#endif /* WACKI_VIEWER_RENDER_H */
