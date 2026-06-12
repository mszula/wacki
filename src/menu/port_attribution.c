/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/menu/port_attribution.c — author credits for this C/SDL2 port.
 *
 * Two surfaces:
 *
 *   paint_title_attribution_footer
 *       Wired into the title-screen SceneDef as `.after_paint`. Paints
 *       a half-resolution, white, two-piece footer on a single baseline
 *       ~20 px from the screen bottom:
 *           left half  — author + build version (centred in left half)
 *           right half — repo URL               (centred in right half)
 *
 *   play_port_attribution_screen
 *       Standalone screen shown after Dane_12.dta (the in-game Credits
 *       AVI). Clears the back buffer, centres three lines (author, repo
 *       URL, build version) vertically and horizontally, holds for ~4 s
 *       or until the user clicks/keys through.
 *
 * The half-size footer is implemented as a software 2× downscale of a
 * Futura.30 render (the game ships only one font, 30 px tall). The
 * downscale uses a "3-of-4 subpixels lit" threshold instead of the
 * default 2-of-4 majority — thinner strokes that blend into the title
 * art rather than competing with it. */

#include "wacki.h"

#include <SDL.h>

#include <stdint.h>
#include <string.h>

/* ---- text content ------------------------------------------------- */

/* The author of this C/SDL2 port of Wacki (the same binary serves
 * Linux/macOS/Windows desktop hosts and the Miyoo Mini Plus handheld —
 * the attribution belongs to the port, not any single target). Two
 * lines: author + source-repo URL.
 *
 * Written in Futura.30 glyph indices, not CP-1250: "ł" is 0xEE in the
 * Futura.30 atlas (see the cp1250→Futura table in src/text/balloon.c).
 * Plain ASCII for the rest. Storing the string pre-translated lets us
 * skip a runtime CP-1250 translation pass for this static asset.
 *
 * String-concat split around \xEE so the C parser doesn't greedily
 * extend the hex escape into the following 'a' (\x has no fixed
 * length — \xEEa = 0xEEa = "hex escape out of range"). */
#define PORT_ATTRIBUTION_AUTHOR     "Port: Mateusz Szu" "\xEE" "a, 2026"
#define PORT_ATTRIBUTION_LINE1      ((const uint8_t *)PORT_ATTRIBUTION_AUTHOR)
#define PORT_ATTRIBUTION_LINE2      ((const uint8_t *)"github.com/mszula/wacki")

/* Build version baked in at compile time (Makefile -DWACKI_VERSION,
 * e.g. "v0.1.0"). Falls back to "unknown" if a build path forgets to
 * pass it — same fallback as main.c's startup log, so the two surfaces
 * never disagree. Shown on its own centred line on the post-Credits
 * screen and appended to the signature on the title footer, so a player
 * can read back exactly which build they're running. WACKI_VERSION is a
 * string literal, so it concatenates with the author literal at compile
 * time — the comma chars / dots / dashes it contains are all in
 * Futura.30. */
#ifndef WACKI_VERSION
#define WACKI_VERSION "unknown"
#endif
#define PORT_ATTRIBUTION_VERSION    ((const uint8_t *)WACKI_VERSION)

/* ---- layout constants --------------------------------------------- */

/* Hold time for the post-Credits standalone attribution screen. */
#define PORT_ATTRIBUTION_HOLD_MS        4000

/* Futura.30 cell height — line-to-line vertical advance for the
 * full-size render (post-Credits standalone screen). */
#define PORT_ATTRIBUTION_LINE_H         30

/* Half-resolution footer metrics. We downscale Futura.30 2× per axis
 * (30 px cell → 15 px) to produce the "much smaller" footer text on
 * the title screen — there's no smaller font shipped with the game,
 * and an embedded mini bitmap font wouldn't cover Polish ł / lower-
 * case URL chars without extra glyph data. */
#define PORT_ATTRIBUTION_LINE_H_SMALL   15

/* Bottom offset for the small footer — distance from the screen
 * bottom edge to the line's top edge. Tuned so the text sits a touch
 * above the menu's button block without crowding the bottom edge. */
#define PORT_ATTRIBUTION_BOTTOM_OFFSET  25

/* Half-size pixel gap between the three footer pieces (signature,
 * version, URL). The whole three-piece group is centred on screen, so
 * this is just the breathing room between adjacent pieces. */
#define PORT_ATTRIBUTION_FOOTER_GAP     28

/* Threshold for the 2× downscale: emit a destination pixel when at
 * least N of 4 source subpixels are lit. 2 = majority (bolder);
 * 3 = thinner; 4 = thinnest (often broken strokes). */
#define ATTRIBUTION_DOWNSCALE_THRESHOLD 3

/* ---- engine surfaces consumed ------------------------------------- */

extern FontHandle *g_default_font;
extern uint8_t     g_palette_rgb[256 * 3];
extern uint8_t    *g_back_shadow;


/* ---- helpers ------------------------------------------------------- */

/* Find the brightest palette index. We use the highest-luminance entry
 * as our "white" for the small footer text so it pops off any palette.
 * Walked once per paint pass — palette is 256 entries, trivial cost
 * vs. the actual text rasterisation. */
static int find_palette_white_index(void)
{
    int best = 0xFF;
    int best_sum = -1;
    for (int i = 0; i < 256; ++i) {
        int r = g_palette_rgb[i * 3 + 0];
        int g = g_palette_rgb[i * 3 + 1];
        int b = g_palette_rgb[i * 3 + 2];
        int s = r + g + b;
        if (s > best_sum) { best_sum = s; best = i; }
    }
    return best;
}

/* Scratch buffer for the half-size text rasteriser — sized for one
 * full-width Futura.30 line. Static so per-frame footer paints don't
 * malloc/free 19 KB on every iteration (heap fragmentation cost on
 * Miyoo). */
static uint8_t s_attribution_scratch[WACKI_SCREEN_W * PORT_ATTRIBUTION_LINE_H];

/* Paint a single attribution line to the back-shadow buffer at full
 * size (Futura.30 native 30 px). `color` is the palette index — the
 * standalone post-Credits screen overrides two slots to guaranteed
 * black/white before calling this, so it can ignore whatever palette
 * the credits AVI left behind. */
static void paint_line_full(int x, int y, uint8_t color, const uint8_t *text)
{
    if (!g_default_font || !g_back_shadow || !text) return;
    if (y < 0 || y >= WACKI_SCREEN_H) return;
    TextRenderTarget t = {
        .stride     = WACKI_SCREEN_W,
        .x          = (uint16_t)x,
        .color_base = color,
        .pixels     = g_back_shadow + (size_t)y * WACKI_SCREEN_W,
        .font       = g_default_font,
    };
    RenderTextLineToBuffer(&t, text);
}

/* Paint a single attribution line at HALF size — 2× nearest-pair
 * decimation with a threshold from ATTRIBUTION_DOWNSCALE_THRESHOLD.
 * The threshold preserves stroke legibility at this aggressive
 * downscale; for the footer use-case where text is just-readable at
 * 15 px it's the difference between "elegant" and "muddy". */
static void paint_line_small(int dst_x, int dst_y,
                             uint8_t color,
                             const uint8_t *text)
{
    if (!g_default_font || !g_back_shadow || !text) return;

    int src_w_full = MeasureTextLine(g_default_font, text);
    if (src_w_full <= 0) return;
    if (src_w_full > WACKI_SCREEN_W) src_w_full = WACKI_SCREEN_W;

    /* Clear only the columns we'll use — saves zeroing 19 KB when the
     * text is shorter than the screen. */
    for (int row = 0; row < PORT_ATTRIBUTION_LINE_H; ++row) {
        memset(s_attribution_scratch + row * WACKI_SCREEN_W, 0,
               (size_t)src_w_full);
    }

    /* Render Futura.30 at full size into the scratch buffer. Any
     * non-zero glyph pixel will become a "lit" marker for downscale. */
    TextRenderTarget rt = {
        .stride     = WACKI_SCREEN_W,
        .x          = 0,
        .color_base = 0xFF,        /* marker — distinct from "no pixel" 0 */
        .pixels     = s_attribution_scratch,
        .font       = g_default_font,
    };
    RenderTextLineToBuffer(&rt, text);

    int dst_w = src_w_full / 2;
    int dst_h = PORT_ATTRIBUTION_LINE_H_SMALL;
    if (dst_x < 0) dst_x = 0;
    if (dst_y < 0 || dst_y + dst_h > WACKI_SCREEN_H) return;
    if (dst_x + dst_w > WACKI_SCREEN_W) dst_w = WACKI_SCREEN_W - dst_x;
    if (dst_w <= 0) return;

    for (int dy = 0; dy < dst_h; ++dy) {
        const uint8_t *r0 = s_attribution_scratch + (size_t)(dy * 2) * WACKI_SCREEN_W;
        const uint8_t *r1 = s_attribution_scratch + (size_t)(dy * 2 + 1) * WACKI_SCREEN_W;
        uint8_t *out = g_back_shadow + (size_t)(dst_y + dy) * WACKI_SCREEN_W + dst_x;
        for (int dx = 0; dx < dst_w; ++dx) {
            int sx = dx * 2;
            int hits = (r0[sx]     != 0)
                     + (r0[sx + 1] != 0)
                     + (r1[sx]     != 0)
                     + (r1[sx + 1] != 0);
            if (hits >= ATTRIBUTION_DOWNSCALE_THRESHOLD) out[dx] = color;
        }
    }
}

/* ---- public API --------------------------------------------------- */

/* SceneDef.after_paint hook for the title screen — runs every frame
 * after buttons + hover sprite are painted, so the attribution text
 * sits on top of the title art (and underneath the cursor). One
 * half-resolution white baseline ~20 px from the screen bottom holds
 * three pieces in reading order — signature, build version, repo URL —
 * each separated by a fixed gap and the whole group centred as a unit,
 * so they stay balanced no matter how wide each piece is. The version
 * sits right next to the signature it belongs to. */
void paint_title_attribution_footer(void)
{
    if (!g_default_font || !g_back_shadow) return;

    uint8_t white = (uint8_t)find_palette_white_index();

    int y = WACKI_SCREEN_H - PORT_ATTRIBUTION_BOTTOM_OFFSET
                           - PORT_ATTRIBUTION_LINE_H_SMALL;

    /* Half-size widths — measure Futura at full size then divide by 2
     * (matches paint_line_small's 2x downscale ratio). */
    int wa = MeasureTextLine(g_default_font, PORT_ATTRIBUTION_LINE1)   / 2;
    int wv = MeasureTextLine(g_default_font, PORT_ATTRIBUTION_VERSION) / 2;
    int wu = MeasureTextLine(g_default_font, PORT_ATTRIBUTION_LINE2)   / 2;

    /* Centre the signature + version + URL group as one unit. */
    int gap   = PORT_ATTRIBUTION_FOOTER_GAP;
    int total = wa + gap + wv + gap + wu;
    int x     = (WACKI_SCREEN_W - total) / 2;
    if (x < 0) x = 0;

    paint_line_small(x, y, white, PORT_ATTRIBUTION_LINE1);
    x += wa + gap;
    paint_line_small(x, y, white, PORT_ATTRIBUTION_VERSION);
    x += wv + gap;
    paint_line_small(x, y, white, PORT_ATTRIBUTION_LINE2);
}

/* Standalone post-Credits attribution screen. Shown for ~4 s after
 * Dane_12.dta (the Credits AVI) finishes — clears the back buffer,
 * centres the three attribution lines (author, repo URL, build
 * version) vertically and horizontally, flushes once, holds for the
 * timeout unless the user clicks/keys through.
 *
 * The credits AVI leaves an arbitrary palette installed; index 0 was
 * often saturated blue and 0x12 (our default attribution colour) some
 * other arbitrary entry, giving the user a "blue text on blue
 * background" non-screen. To stay palette-independent we force two
 * specific palette slots — 0 and 0xFF — to true black and true white
 * for the duration of the screen, clear with 0 and paint with 0xFF.
 *
 * The next user action — returning to the title menu — calls
 * install_main_menu_palettes which re-installs Tlo.pal in full and
 * blows away both overrides, so no explicit restore here. */
#define ATTRIBUTION_SCREEN_BG_INDEX  0
#define ATTRIBUTION_SCREEN_FG_INDEX  0xFF
void play_port_attribution_screen(void)
{
    if (!g_default_font || !g_back_shadow) return;

    g_palette_rgb[ATTRIBUTION_SCREEN_BG_INDEX * 3 + 0] = 0x00;
    g_palette_rgb[ATTRIBUTION_SCREEN_BG_INDEX * 3 + 1] = 0x00;
    g_palette_rgb[ATTRIBUTION_SCREEN_BG_INDEX * 3 + 2] = 0x00;
    g_palette_rgb[ATTRIBUTION_SCREEN_FG_INDEX * 3 + 0] = 0xFF;
    g_palette_rgb[ATTRIBUTION_SCREEN_FG_INDEX * 3 + 1] = 0xFF;
    g_palette_rgb[ATTRIBUTION_SCREEN_FG_INDEX * 3 + 2] = 0xFF;

    FlipBuffersClearWith(ATTRIBUTION_SCREEN_BG_INDEX);

    int total_h = 3 * PORT_ATTRIBUTION_LINE_H;
    int y_line1 = (WACKI_SCREEN_H - total_h) / 2;
    int y_line2 = y_line1 + PORT_ATTRIBUTION_LINE_H;
    int y_line3 = y_line2 + PORT_ATTRIBUTION_LINE_H;

    int w1 = MeasureTextLine(g_default_font, PORT_ATTRIBUTION_LINE1);
    int w2 = MeasureTextLine(g_default_font, PORT_ATTRIBUTION_LINE2);
    int w3 = MeasureTextLine(g_default_font, PORT_ATTRIBUTION_VERSION);
    int x1 = (WACKI_SCREEN_W - w1) / 2;
    int x2 = (WACKI_SCREEN_W - w2) / 2;
    int x3 = (WACKI_SCREEN_W - w3) / 2;
    if (x1 < 0) x1 = 0;
    if (x2 < 0) x2 = 0;
    if (x3 < 0) x3 = 0;

    paint_line_full(x1, y_line1, ATTRIBUTION_SCREEN_FG_INDEX, PORT_ATTRIBUTION_LINE1);
    paint_line_full(x2, y_line2, ATTRIBUTION_SCREEN_FG_INDEX, PORT_ATTRIBUTION_LINE2);
    paint_line_full(x3, y_line3, ATTRIBUTION_SCREEN_FG_INDEX, PORT_ATTRIBUTION_VERSION);
    FlushFrameToPrimary();

    /* Hold with a per-tick poll so user can dismiss with a click /
     * keypress instead of waiting out the full timeout. */
    uint32_t t0 = SDL_GetTicks();
    while (SDL_GetTicks() - t0 < PORT_ATTRIBUTION_HOLD_MS) {
        PumpEvents();
        if (PlatformShouldQuit()) break;
        if (g_lmb_clicked || g_rmb_clicked || (g_key_state & 0xFF)) {
            g_lmb_clicked = 0;
            g_rmb_clicked = 0;
            g_key_state  &= 0xFF00;
            break;
        }
        SDL_Delay(16);
    }
}
