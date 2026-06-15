/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform/android/hooks_android.c — Android platform hooks provider.
 *
 * Android reuses the whole SDL family unchanged (video/audio/input/save/file
 * under src/platform/sdl/) plus the SDL_GameController glue; the only
 * platform-variant behaviour is the display default and the absence of a
 * keyboard. As with the other targets this is the one hooks provider for the
 * build, so it also supplies the no-ops for the behaviours Android doesn't
 * need. Keeping these here is what lets the shared sdl/ files stay free of an
 * Android #ifdef (the OS-family bring-up that genuinely belongs to the SDL
 * core — the writable-cwd chdir + Back-button trap — lives in system_sdl.c's
 * __ANDROID__ branch, matching the existing __APPLE__ / _WIN32 pattern). */

#include "wacki/platform/system.h"   /* plat_restore_system_volume */
#include "wacki/platform/input.h"    /* plat_handle_platform_key, plat_pad_read_extra */
#include "wacki/platform/video.h"    /* plat_apply_video_prefs */

void plat_apply_video_prefs(void)
{
    extern int g_fullscreen;
    g_fullscreen = 1;   /* the SDL Android backend always covers the display */
}

void plat_restore_system_volume(void)            { }
int  plat_handle_platform_key(int sym)           { (void)sym; return 0; }
void plat_pad_read_extra(float *ax, float *ay)   { (void)ax; (void)ay; }

/* No reliable keyboard on a touch device, so gameplay keybindings beyond the
 * universal ESC / Back gate on this (see play_loop.c) — the two-finger-tap →
 * RMB gesture covers the actor toggle, the pause menu covers save/load. A
 * physical/Bluetooth keyboard, if attached, still drives text input fine; this
 * only governs whether gameplay may *assume* one is present. */
int  plat_input_has_keyboard(void)               { return 0; }
