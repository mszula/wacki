/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * assets-explorer/src/stubs.c — minimal definitions for the handful of engine symbols
 * that assets.c / graphics.c / font.c reference but the viewer deliberately
 * does NOT link (no scene, no VM, no game loop, no real platform layer).
 *
 * g_headless = 1 forces every stray present/pace path in graphics.c into its
 * no-op branch, so these never actually run — they exist only to satisfy the
 * linker. The viewer renders frames itself (render.c), it never drives the
 * engine's FlushFrameToPrimary path. */

#include "wacki/api.h"
#include "wacki/globals.h"

#include <stddef.h>
#include <stdint.h>

int   g_headless    = 1;     /* force headless: present/pace become no-ops */
void *g_scripts_obj = NULL;  /* no Wacky.scr → no per-anim sound-trigger table */

/* assets.c looks up a per-anim sound table by name; with no scripts loaded
 * there's nothing to find. */
void *FindAnimationScript(void *scripts_obj, const char *name)
{
    (void)scripts_obj;
    (void)name;
    return NULL;
}

/* InstallPalette calls this to rebuild the RGB12 alpha-plane LUTs. The viewer's
 * Phase-1 render path is plain paletted→RGBA (no alpha-plane box filter), so
 * the LUTs are unused. */
void RebuildAlphaQuantLuts(void) { }

/* graphics.c's present/pump/quit/pace hooks — never reached under g_headless. */
void PlatformPresent(const uint8_t *shadow, const uint8_t *palette_rgb, int w, int h)
{
    (void)shadow; (void)palette_rgb; (void)w; (void)h;
}
int  PlatformShouldQuit(void)        { return 0; }
void PumpEvents(void)                { }
void EnginePaceFrame(uint32_t target_ms) { (void)target_ms; }
