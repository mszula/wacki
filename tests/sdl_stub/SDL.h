/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tests/sdl_stub/SDL.h — minimal SDL.h replacement for the test build.
 *
 * Production source files that `#include <SDL.h>` and get linked into
 * the test binary (script.c, stubs.c) need the SDL types + a handful
 * of function declarations to compile. The test binary's actual SDL
 * usage is zero — every function is stubbed as a no-op in
 * tests/test_engine_stubs.c. Types are kept opaque-ish (forward decls
 * only) so we never depend on SDL internals.
 *
 * This header is picked up via `-I tests/sdl_stub` which comes BEFORE
 * any real SDL include path in TEST_CFLAGS. Production builds use the
 * system SDL2 headers from `sdl2-config --cflags`.
 */
#ifndef WACKI_TEST_SDL_STUB_H
#define WACKI_TEST_SDL_STUB_H

#include <stdint.h>

/* ---- functions actually called by script.c / stubs.c ---------------- */

void     SDL_Delay(uint32_t ms);
uint32_t SDL_GetTicks(void);
const char *SDL_GetError(void);

/* ---- types referenced by stubs.c debug-screenshot path -------------- *
 * The screenshot path (DebugScreenshot in stubs.c, called from no test)
 * uses these types but tests never reach the code. Keep them as
 * forward decls / minimal struct shells so the compiler is happy. */

typedef struct SDL_Surface       SDL_Surface;
typedef struct SDL_PixelFormat   SDL_PixelFormat;
typedef struct SDL_Palette       SDL_Palette;

/* SDL_Color is a value type used in arrays — needs concrete layout. */
typedef struct SDL_Color {
    uint8_t r, g, b, a;
} SDL_Color;

/* SDL pixel format constants — only INDEX8 is referenced. */
#define SDL_PIXELFORMAT_INDEX8 ((uint32_t)0x13000001u)

/* Internal layout used by the screenshot path: `s->format->palette`.
 * Provide minimal nested struct stubs so stubs.c compiles. The
 * concrete layout is irrelevant — tests never read these fields. */
struct SDL_PixelFormat {
    SDL_Palette *palette;
};
struct SDL_Surface {
    SDL_PixelFormat *format;
};

/* Debug-screenshot function stubs (called by stubs.c
 * DebugScreenshot path — tests never reach it). */
SDL_Surface *SDL_CreateRGBSurfaceWithFormatFrom(
    void *pixels, int width, int height,
    int depth, int pitch, uint32_t format);
int  SDL_SetPaletteColors(SDL_Palette *palette, const SDL_Color *colors,
                           int firstcolor, int ncolors);
int  SDL_SaveBMP(SDL_Surface *surface, const char *file);
void SDL_FreeSurface(SDL_Surface *surface);

/* ---- audio types + functions used by audio/sfx.c when it's linked
 * into the test binary. Concrete typedefs because mixer_internal.h
 * uses them in field types (Uint8 *, Uint32, SDL_AudioDeviceID), and
 * SDL_LockAudioDevice / SDL_UnlockAudioDevice need declarations so
 * sfx.c compiles. Implementations are no-op stubs in
 * tests/test_engine_stubs.c. */
typedef uint8_t  Uint8;
typedef uint32_t Uint32;
typedef uint32_t SDL_AudioDeviceID;

void SDL_LockAudioDevice  (SDL_AudioDeviceID dev);
void SDL_UnlockAudioDevice(SDL_AudioDeviceID dev);

#endif
