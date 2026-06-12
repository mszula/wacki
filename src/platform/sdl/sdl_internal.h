/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform/sdl/sdl_internal.h — cross-file declarations within the SDL
 * platform family (mirrors src/platform/ps2/ps2_internal.h). NOT a public
 * engine header; it just gives platform_sdl.c (the input/event pump) and
 * gamepad_sdl.c (the controller glue) a single declaration point for the pad
 * interface instead of ad-hoc local externs.
 */
#ifndef WACKI_PLATFORM_SDL_INTERNAL_H
#define WACKI_PLATFORM_SDL_INTERNAL_H

#include <SDL.h>

/* gamepad_sdl.c — the shared SDL_GameController → cursor glue, driven by the
 * input/event pump in platform_sdl.c. Runtime no-ops where no pad is open. */
void platform_pad_open(void);
int  platform_pad_handle_event(const SDL_Event *ev);
void platform_pad_read_motion(int *dx, int *dy, float *ax, float *ay);

#endif /* WACKI_PLATFORM_SDL_INTERNAL_H */
