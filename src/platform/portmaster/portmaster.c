/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform/portmaster/portmaster.c — PortMaster (Anbernic & friends)
 * platform hooks.
 *
 * PortMaster uses the shared SDL_GameController glue (sdl/gamepad_sdl.c) and
 * the rest of the SDL family unchanged; the only platform-variant behavior is
 * the display default: standard SDL2 drives a KMSDRM panel with no window
 * manager, so the 640×480 canvas is shown desktop-fullscreen (letterboxed by
 * SDL_RenderSetLogicalSize). This is the target's one hooks provider, so it
 * also supplies the no-ops for the behaviors it doesn't need. */

#include "wacki/platform/system.h"   /* plat_restore_system_volume */
#include "wacki/platform/input.h"    /* plat_handle_platform_key, plat_pad_read_extra */
#include "wacki/platform/video.h"    /* plat_apply_video_prefs */

void plat_apply_video_prefs(void)
{
    extern int g_fullscreen;
    g_fullscreen = 1;   /* KMSDRM has no WM — always cover the display */
}

void plat_restore_system_volume(void)            { }
int  plat_handle_platform_key(int sym)           { (void)sym; return 0; }
void plat_pad_read_extra(float *ax, float *ay)   { (void)ax; (void)ay; }
