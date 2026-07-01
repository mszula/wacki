/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform/sdl/sdl_internal.h — cross-file declarations within the SDL
 * platform family. NOT a public engine header.
 */
#ifndef WACKI_PLATFORM_SDL_INTERNAL_H
#define WACKI_PLATFORM_SDL_INTERNAL_H

#include <SDL.h>

/* gamepad_sdl.c / gamepad_switch.c */
void platform_pad_open(void);
int  platform_pad_handle_event(const SDL_Event *ev);
void platform_pad_read_motion(int *dx, int *dy, float *ax, float *ay);

/* video_sdl.c — aspect mode (stretch vs 4:3 letterbox).
 * g_aspect_mode is defined in src/config.c (available for all targets). */
void platform_video_toggle_aspect_mode(void);
void platform_video_get_present_state(int *stretch_active,
                                      int *win_w, int *win_h,
                                      int *fb_w,  int *fb_h);

/* platform_sdl.c — touch mode cycle (absolute / relative / off).
 * g_touch_mode is defined in src/config.c (available for all targets). */
void platform_touch_cycle_mode(void);

#endif /* WACKI_PLATFORM_SDL_INTERNAL_H */
