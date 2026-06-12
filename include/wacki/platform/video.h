/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * include/wacki/platform/video.h — platform video-output HAL.
 *
 * The engine renders into a flat 8-bpp shadow buffer + a 256×3 RGB palette.
 * How that reaches the screen is platform-specific; the shared SDL platform
 * layer (platform_sdl.c, which also owns the cross-platform input/event pump)
 * calls these and never #ifdefs on the video backend:
 *
 *   desktop / handheld  src/platform/sdl/video_sdl.c  (SDL window + renderer +
 *                                                      streaming ARGB texture)
 *   PS2                 src/platform_ps2.c            (gsKit: a PSMT8 texture +
 *                                                      hardware CLUT — an SDL
 *                                                      window would fight the
 *                                                      GS gsKit drives)
 */
#ifndef WACKI_PLATFORM_VIDEO_H
#define WACKI_PLATFORM_VIDEO_H

#include <stdint.h>

/* The SDL_Init subsystem flags this platform needs. The SDL video backend
 * wants VIDEO|EVENTS|AUDIO; PS2 owns the GS (gsKit) + audsrv directly, so it
 * only needs EVENTS|TIMER for input + timing. Returned as plain unsigned to
 * keep this header SDL-free; PlatformInit passes it straight to SDL_Init. */
unsigned plat_video_sdl_init_flags(void);

/* Bring up the display (window + renderer + streaming texture on SDL; the GS
 * via gsKit on PS2). Returns 1 on success, 0 on failure. Never called in
 * headless mode. */
int  plat_video_init(int w, int h, const char *title);

/* Present one frame: expand the 8-bpp `shadow` through `palette_rgb`
 * (256 × 3-byte RGB entries) and show it. */
void plat_video_present(const uint8_t *shadow, const uint8_t *palette_rgb,
                        int w, int h);

/* Tear down the display (the SDL_Quit / IOP side stays with PlatformShutdown). */
void plat_video_shutdown(void);

/* Toggle windowed / fullscreen at runtime and persist the choice. A no-op
 * where there's no windowing to toggle (handheld / PS2). */
void plat_video_toggle_fullscreen(void);

/* Show a native message box parented to the game window. A no-op where there
 * is no SDL display (PS2); the headless-mode logging path is handled by the
 * caller before this is reached. */
void plat_video_message_box(const char *title, const char *body);

#endif /* WACKI_PLATFORM_VIDEO_H */
