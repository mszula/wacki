/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* src/util/screenshot.c — debug screenshot + missing-asset placeholder.
 *
 * Pressing 'B' in-game captures the current shadow back-buffer into a
 * BMP file (wac00000.bmp, wac00001.bmp, ...). The 8-bit indexed
 * surface is built from g_back_shadow with the live palette applied,
 * then SDL_SaveBMP writes it out. A 500 ms debounce prevents
 * accidental key-repeat from filling the directory with duplicates.
 *
 * DrawPlaceholderScreen is the "asset missing" hook — earlier ports
 * used to draw a coloured test-card to alert the user, but the engine
 * now just logs to stderr and leaves the back-buffer alone.
 */

#include "wacki.h"
#include "wacki/log.h"
#include <SDL.h>

#include <stdint.h>
#include <stdio.h>

extern uint8_t *g_back_shadow;
extern uint8_t  g_palette_rgb[256 * 3];

void DrawPlaceholderScreen(const char *wanted_file)
{
    if (wanted_file)
        LOG_TRACE("asset", "missing: %s", wanted_file);
}
/* ScreenshotToBmpAutoIncrement — dump current shadow buffer + palette as
 * BMP file (wac00000.bmp, wac00001.bmp, ...). Press B in game to capture. */
void ScreenshotToBmpAutoIncrement(void)
{
    static int s_shot_idx = 0;
    static uint32_t s_last_shot_ms = 0;
    uint32_t now = SDL_GetTicks();
    if (now - s_last_shot_ms < 500) return;     /* debounce key repeat */
    s_last_shot_ms = now;
    if (!g_back_shadow) return;
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(
        g_back_shadow, g_screen_w, g_screen_h, 8, g_screen_w,
        SDL_PIXELFORMAT_INDEX8);
    if (!s) { LOG_DEBUG("debug", "screenshot: SDL_CreateSurface failed: %s", SDL_GetError()); return; }
    SDL_Color colors[256];
    for (int i = 0; i < 256; ++i) {
        colors[i].r = g_palette_rgb[i*3 + 0];
        colors[i].g = g_palette_rgb[i*3 + 1];
        colors[i].b = g_palette_rgb[i*3 + 2];
        colors[i].a = 255;
    }
    SDL_SetPaletteColors(s->format->palette, colors, 0, 256);
    char path[64];
    snprintf(path, sizeof path, "wac%05d.bmp", s_shot_idx++);
    if (SDL_SaveBMP(s, path) == 0)
        LOG_DEBUG("debug", "screenshot saved -> %s", path);
    else
        LOG_DEBUG("debug", "screenshot: SDL_SaveBMP(%s) failed: %s", path, SDL_GetError());
    SDL_FreeSurface(s);
}
void ScreenshotToPcxAutoIncrement(void) { ScreenshotToBmpAutoIncrement(); }
