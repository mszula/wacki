/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform/sdl/video_sdl.c — video-output HAL, SDL backend.
 *
 * Adds g_aspect_mode support ("stretch" / "4:3"):
 *   "4:3"    — SDL_RenderSetLogicalSize(640,480): SDL letterboxes automatically
 *              when output AR differs (e.g. Switch 1280×720 → black bars).
 *   "stretch"— SDL_RenderSetLogicalSize disabled (0,0); texture blitted with
 *              explicit full-window SDL_Rect → edge-to-edge, no bars.
 *              SDL's RenderSetLogicalSize always preserves AR (per its docs)
 *              so disproportionate fill requires bypassing it entirely.
 *
 * g_aspect_mode is defined in src/config.c so it's available for all targets
 * (including PS2 which doesn't link this file). */

#include "wacki.h"
#include "wacki/log.h"
#include "wacki/platform/video.h"
#include "sdl_internal.h"
#ifdef __APPLE__
#include "wacki/platform/macos.h"
#endif
#ifdef __ANDROID__
#include "wacki/platform/android_touch.h"
#endif

#include <SDL.h>
#include <stdint.h>
#include <string.h>

#define ARGB_BYTES_PER_PIXEL      4
#define ARGB_ALPHA_OPAQUE_SHIFTED (0xFFu << 24)
#define ARGB_R_SHIFT              16
#define ARGB_G_SHIFT              8
#define PALETTE_BYTES_PER_ENTRY   3
#define DEFAULT_SCALE_FACTOR      1

static SDL_Window   *s_win = NULL;
static SDL_Renderer *s_ren = NULL;
static SDL_Texture  *s_tex = NULL;
static uint32_t      s_pixels32[640 * 480];
static int           s_fb_w = 0, s_fb_h = 0;

/* Defined in src/config.c — available for all targets. */
extern char g_aspect_mode[16];

/* 1 when logical-size scaling is disabled (stretch mode).
 * platform_video_get_present_state() exposes this to platform_sdl.c so
 * handle_mouse_motion can manually rescale coordinates in that mode. */
static int g_stretch_active = 0;

static void apply_aspect_mode(int w, int h)
{
    if (!s_ren) return;
#ifdef __ANDROID__
    (void)w; (void)h; /* Android overlay manages its own layout */
    return;
#else
    if (g_aspect_mode[0] == '4') {
        g_stretch_active = 0;
        SDL_RenderSetLogicalSize(s_ren, w, h);
    } else {
        /* stretch: disable logical scaling so SDL stops rescaling mouse
         * coords; present() blits to explicit full-window rect instead. */
        g_stretch_active = 1;
        SDL_RenderSetLogicalSize(s_ren, 0, 0);
    }
#endif
}

void platform_video_get_present_state(int *stretch_active,
                                      int *win_w, int *win_h,
                                      int *fb_w,  int *fb_h)
{
    *stretch_active = g_stretch_active;
    *fb_w = s_fb_w; *fb_h = s_fb_h;
    if (s_win) SDL_GetWindowSize(s_win, win_w, win_h);
    else { *win_w = s_fb_w; *win_h = s_fb_h; }
}

void platform_video_toggle_aspect_mode(void)
{
    if (g_aspect_mode[0] == '4')
        strncpy(g_aspect_mode, "stretch", 15);
    else
        strncpy(g_aspect_mode, "4:3", 15);
    g_aspect_mode[15] = '\0';
    apply_aspect_mode(s_fb_w, s_fb_h);
    LOG_INFO("platform", "aspect_mode=%s", g_aspect_mode);
    extern void ConfigSave(void);
    ConfigSave();
}

unsigned plat_video_sdl_init_flags(void)
{
    return SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_AUDIO;
}

int plat_video_init(int w, int h, const char *title)
{
    plat_apply_video_prefs();

    s_fb_w = w; s_fb_h = h;

    int sf    = g_scale_factor > 0 ? g_scale_factor : DEFAULT_SCALE_FACTOR;
    int win_w = w * sf, win_h = h * sf;
    if (g_scale_mode && *g_scale_mode)
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, g_scale_mode);

    Uint32 win_flags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
    if (g_fullscreen) win_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

    s_win = SDL_CreateWindow(title,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        win_w, win_h, win_flags);
    if (!s_win) { LOG_INFO("log", "SDL_CreateWindow: %s", SDL_GetError()); return 0; }

    s_ren = SDL_CreateRenderer(s_win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!s_ren)
        s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_SOFTWARE);
    if (!s_ren) { LOG_INFO("log", "SDL_CreateRenderer: %s", SDL_GetError()); return 0; }

#ifndef __ANDROID__
    apply_aspect_mode(w, h);
#endif

    s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_ARGB8888,
                              SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!s_tex) { LOG_INFO("log", "SDL_CreateTexture: %s", SDL_GetError()); return 0; }

    extern unsigned char wacki_icon_bmp[];
    extern unsigned int  wacki_icon_bmp_len;
    SDL_RWops *rw = SDL_RWFromConstMem(wacki_icon_bmp, (int)wacki_icon_bmp_len);
    if (rw) {
        SDL_Surface *icon = SDL_LoadBMP_RW(rw, 1);
        if (icon) { SDL_SetWindowIcon(s_win, icon); SDL_FreeSurface(icon); }
    }

#ifdef __APPLE__
    PlatformSetupMacMenu();
#endif
    SDL_ShowCursor(SDL_DISABLE);

    LOG_INFO("platform", "SDL ready: %dx%d (%dx scale, fullscreen=%d, aspect=%s)",
             win_w, win_h, sf, g_fullscreen, g_aspect_mode);

    memset(s_pixels32, 0, (size_t)w * h * ARGB_BYTES_PER_PIXEL);
    SDL_UpdateTexture(s_tex, NULL, s_pixels32, w * ARGB_BYTES_PER_PIXEL);
    SDL_RenderClear(s_ren);
    SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
    SDL_RenderPresent(s_ren);
    SDL_PumpEvents();
    return 1;
}

void plat_video_present(const uint8_t *shadow, const uint8_t *pal, int w, int h)
{
    if (!s_tex || !shadow || !pal) return;

    void *pixels = NULL; int pitch = 0;
    if (SDL_LockTexture(s_tex, NULL, &pixels, &pitch) == 0 && pixels) {
        uint32_t *out = (uint32_t *)pixels;
        int stride = pitch / ARGB_BYTES_PER_PIXEL;
        for (int y = 0; y < h; ++y) {
            uint32_t      *row = out + (size_t)y * stride;
            const uint8_t *src = shadow + (size_t)y * w;
            for (int x = 0; x < w; ++x) {
                const uint8_t *e = pal + src[x] * PALETTE_BYTES_PER_ENTRY;
                row[x] = ARGB_ALPHA_OPAQUE_SHIFTED
                       | ((uint32_t)e[0] << ARGB_R_SHIFT)
                       | ((uint32_t)e[1] << ARGB_G_SHIFT)
                       |  (uint32_t)e[2];
            }
        }
        SDL_UnlockTexture(s_tex);
    } else {
        int n = w * h < (int)(sizeof s_pixels32 / 4) ? w * h : (int)(sizeof s_pixels32 / 4);
        for (int i = 0; i < n; ++i) {
            const uint8_t *e = pal + shadow[i] * PALETTE_BYTES_PER_ENTRY;
            s_pixels32[i] = ARGB_ALPHA_OPAQUE_SHIFTED
                          | ((uint32_t)e[0] << ARGB_R_SHIFT)
                          | ((uint32_t)e[1] << ARGB_G_SHIFT)
                          |  (uint32_t)e[2];
        }
        SDL_UpdateTexture(s_tex, NULL, s_pixels32, w * ARGB_BYTES_PER_PIXEL);
    }

    SDL_SetRenderDrawColor(s_ren, 0, 0, 0, 255);
    SDL_RenderClear(s_ren);

#ifdef __ANDROID__
    wacki_overlay_compute_layout(s_ren);
    SDL_Rect gr = wacki_overlay_game_rect();
    SDL_RenderCopy(s_ren, s_tex, NULL, gr.w > 0 ? &gr : NULL);
    wacki_overlay_draw(s_ren);
#else
    if (g_stretch_active && s_win) {
        int ww = w, wh = h;
        SDL_GetWindowSize(s_win, &ww, &wh);
        SDL_Rect dest = {0, 0, ww, wh};
        SDL_RenderCopy(s_ren, s_tex, NULL, &dest);
    } else {
        SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
    }
#endif
    SDL_RenderPresent(s_ren);
}

void plat_video_shutdown(void)
{
    if (s_tex) { SDL_DestroyTexture(s_tex);  s_tex = NULL; }
    if (s_ren) { SDL_DestroyRenderer(s_ren); s_ren = NULL; }
    if (s_win) { SDL_DestroyWindow(s_win);   s_win = NULL; }
}

void plat_video_toggle_fullscreen(void)
{
    if (!s_win) return;
    g_fullscreen = !g_fullscreen;
    SDL_SetWindowFullscreen(s_win, g_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    LOG_INFO("platform", "fullscreen=%d", g_fullscreen);
    extern void ConfigSave(void);
    ConfigSave();
}

void plat_video_message_box(const char *title, const char *body)
{
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, title, body, s_win);
}
