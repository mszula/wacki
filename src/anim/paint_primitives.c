/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* src/anim/paint_primitives.c — general-purpose paint helpers.
 *
 * Two public APIs:
 *   paint_rawb_pic       — render a "RAWB" .pic blob into the back
 *                          buffer (centred, palette-merging or color-
 *                          keyed depending on as_overlay).
 *   paint_anim_button_at — blit one frame of an ANIM atlas at its
 *                          embedded hot-spot (or an explicit override).
 *
 * Used by the menu loop (background + button sprites), the scene
 * loader (.pic backgrounds), the HUD painter (panel sprite), the
 * cinematics (bomba / fiacik / krazek), and the script VM's op-0x1A
 * paint_pic. They live in their own TU so menu, scene, vm, hud, and
 * cinematic modules can all reach them without dragging in the rest
 * of game.c.
 *
 * The kind=3 RLE-decode path needs a per-frame scratch buffer sized to
 * the asset's max bounding box; we keep one growing pool here so
 * callers don't have to manage it. */

#include "wacki.h"

#include <stdint.h>
#include <stdlib.h>

/* ---- constants ---------------------------------------------------- */

/* RAWB header layout: 'RAWB' (4) + w (2) + h (2) + palette[256*3] (768)
 * + pixels (w*h). Total fixed prelude = 776 bytes. */
#define RAWB_HEADER_BYTES           776
#define RAWB_HEADER_OFF_W           4
#define RAWB_HEADER_OFF_H           6
#define RAWB_HEADER_OFF_PALETTE     8
#define RAWB_HEADER_OFF_PIXELS      776

/* Gameplay area height — .pic centring uses this for the vertical
 * offset (rows below this are the HUD panel). */
#define GAMEPLAY_AREA_HEIGHT_PX     400

#define PALETTE_SIZE                256

/* ANIM atlas kind values shared with assets.c. */
#define ANIM_KIND_RICH              3   /* RLE-compressed frames */

/* ---- MergePalette ------------------------------------------------- *
 *
 * Fold a .pic file's embedded palette into the live one, preserving
 * entries the .pic has black (0,0,0) for. The shipped scene .pic files
 * fill only ~70 of the 256 indices with real colours; the other ~180
 * are (0,0,0) placeholders. If we naïvely InstallPalette the .pic
 * header, sprite-shared indices (paleta.pal's earth/green/orange
 * ramps) get overwritten with black and the corresponding sprite
 * pixels render BLACK on the user's screen.
 *
 * The original engine never installs the .pic palette directly — it
 * loads the .pic as a kind=3 entity and the renderer leaves the live
 * palette alone. Our port paints .pic pixels directly so it MUST keep
 * the .pic palette merged in (kiosk21 fills 11 new indices at
 * 162..172, plac fills 16 more) while still preserving paleta.pal
 * entries where the .pic has black. */
static void MergePalette(const uint8_t *src256_rgb)
{
    extern uint8_t g_palette_rgb[PALETTE_SIZE * 3];
    for (int i = 0; i < PALETTE_SIZE; ++i) {
        uint8_t r = src256_rgb[i * 3 + 0];
        uint8_t g = src256_rgb[i * 3 + 1];
        uint8_t b = src256_rgb[i * 3 + 2];
        if (r | g | b) {
            g_palette_rgb[i * 3 + 0] = r;
            g_palette_rgb[i * 3 + 1] = g;
            g_palette_rgb[i * 3 + 2] = b;
        }
    }
}

/* ---- paint_rawb_pic ----------------------------------------------- */

/* Decode a .pic ("RAWB") backbuffer:
 *   +0  u32 magic   = 'RAWB'
 *   +4  u16 width   (LE)
 *   +6  u16 height  (LE)
 *   +8  u8  palette[256*3]
 *  +776 u8  pixels[w*h]
 *
 * The image is centred via dx = (640 - w) / 2, dy = (400 - h) / 2
 * (clipped to ≥ 0). For fullscreen 640×480 that's (0, 0); for a
 * 344×319 Pytanie dialog that's (148, 40).
 *
 * `as_overlay`:
 *   0 — fullscreen scene background: install the embedded palette
 *       (merged) and paint opaque.
 *   1 — dialog overlay: keep current palette + color-key index 0 so
 *       the underlying menu shows through (matches the original's
 *       alpha-blend mode 0 with transparent-on-0).
 *
 * Returns 1 if it painted, 0 otherwise. */
int paint_rawb_pic(const void *blob, uint32_t size, int as_overlay)
{
    if (size <= RAWB_HEADER_BYTES) return 0;
    const uint8_t *p = (const uint8_t *)blob;
    if (p[0] != 'R' || p[1] != 'A' || p[2] != 'W' || p[3] != 'B') return 0;

    uint16_t w = (uint16_t)(p[RAWB_HEADER_OFF_W]
                          | (p[RAWB_HEADER_OFF_W + 1] << 8));
    uint16_t h = (uint16_t)(p[RAWB_HEADER_OFF_H]
                          | (p[RAWB_HEADER_OFF_H + 1] << 8));
    if ((uint32_t)w * h + RAWB_HEADER_BYTES > size) return 0;

    int dx = (WACKI_SCREEN_W          - (int)w) / 2;
    int dy = (GAMEPLAY_AREA_HEIGHT_PX - (int)h) / 2;
    if (dx < 0) dx = 0;
    if (dy < 0) dy = 0;

    const uint8_t *pal_src   = p + RAWB_HEADER_OFF_PALETTE;
    const uint8_t *pixel_src = p + RAWB_HEADER_OFF_PIXELS;

    if (!as_overlay) {
        /* Fullscreen bg: MERGE palette (not overwrite), then opaque
         * blit. See MergePalette for why a straight install kills the
         * sprite-shared colour ramps. */
        MergePalette(pal_src);
        PaintImageToBackbuffer((uint16_t)dx, (uint16_t)dy, w, h, pixel_src);
    } else {
        /* Dialog overlay: color-key 0. Merge the .pic's palette so the
         * UI-colour indices the slot-text renderer expects (0x12 slot
         * text, 0x01 inset bg, 0xFE inline-edit) resolve to whatever
         * the .pic's artist intended — otherwise they pick up whichever
         * palette happens to be active. */
        MergePalette(pal_src);
        BlitSpriteToBackbuffer((uint16_t)dx, (uint16_t)dy, 0, 0,
                               w, h, w, h, (uint8_t *)pixel_src, 0);
    }
    return 1;
}

/* ---- paint_anim_button_at ----------------------------------------- */

/* Per-asset scratch buffer for RLE-decoded frames. Grows on demand. */
static uint8_t *s_rle_scratch    = NULL;
static int      s_rle_scratch_sz = 0;

static uint8_t *get_rle_scratch(int sz)
{
    if (sz <= s_rle_scratch_sz) return s_rle_scratch;
    free(s_rle_scratch);
    s_rle_scratch    = (uint8_t *)malloc((size_t)sz);
    s_rle_scratch_sz = s_rle_scratch ? sz : 0;
    return s_rle_scratch;
}

/* Blit one frame of an ANIM atlas using its embedded hot-spot.
 * kind=3 ("rich") frames are RLE-compressed → decode into the scratch
 * buffer first then blit raw. kind=2 ("passive") frames are already
 * raw 8bpp.
 *
 * `use_override` selects between (atlas->off_drawX[frame],
 * atlas->off_drawY[frame]) and the explicit (override_dx, override_dy)
 * passed by the caller — used by the cinematics + HUD panel to paint
 * at a known screen position rather than the atlas's hot-spot. */
void paint_anim_button_at(AnimAsset *atlas, uint16_t frame,
                          int16_t override_dx, int16_t override_dy,
                          int use_override)
{
    if (!atlas || frame >= atlas->frame_count || !atlas->pixel_ptrs) return;
    uint16_t w  = atlas->off_widths [frame];
    uint16_t h  = atlas->off_heights[frame];
    uint16_t dx = use_override ? (uint16_t)override_dx : atlas->off_drawX[frame];
    uint16_t dy = use_override ? (uint16_t)override_dy : atlas->off_drawY[frame];
    uint8_t *px = atlas->pixel_ptrs[frame];
    if (!px || w == 0 || h == 0) return;

    if (atlas->kind == ANIM_KIND_RICH) {
        int      need    = (int)w * (int)h;
        uint8_t *scratch = get_rle_scratch(need);
        if (!scratch) return;
        DepackRleFrame(px, scratch, need);
        px = scratch;
    }
    /* mode 0 = colour-key 0 (transparent). */
    BlitSpriteToBackbuffer(dx, dy, 0, 0, w, h, w, h, px, 0);
}
