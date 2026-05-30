/*
 * platform_sdl.c — portable platform layer (SDL2).
 *
 * Replaces the original DirectDraw / WndProc / WaitMessage stack.
 * The engine renders into a flat 8-bpp uint8_t shadow buffer; this layer
 * uploads that buffer to a streaming SDL_Texture using a palette LUT.
 */
#include "wacki.h"
#include <SDL.h>
#include <string.h>
#include <stdio.h>

static SDL_Window   *s_win  = NULL;
static SDL_Renderer *s_ren  = NULL;
static SDL_Texture  *s_tex  = NULL;
static int           s_w = 0, s_h = 0;
static int           s_quit = 0;
static uint32_t      s_pixels32[640 * 480];

extern int         g_headless;                  /* T45 — main.c */
extern int         g_scale_factor;               /* T54 — main.c */
extern const char *g_scale_mode;                 /* T54 — main.c */

/* Typed-char ring buffer for inline-edit (save-slot rename). Populated
 * by SDL_TEXTINPUT (printable chars) + SDL_KEYDOWN (Backspace 0x08,
 * Enter 0x0D). Drained by PlatformPollTypedChar — returns 0 when empty. */
#define TYPED_QUEUE_SZ 32
static uint8_t s_typed_q[TYPED_QUEUE_SZ];
static int     s_typed_head = 0, s_typed_tail = 0;

void PlatformPushTypedChar(uint8_t c)
{
    int next = (s_typed_head + 1) % TYPED_QUEUE_SZ;
    if (next == s_typed_tail) return;     /* full → drop */
    s_typed_q[s_typed_head] = c;
    s_typed_head = next;
}

uint8_t PlatformPollTypedChar(void)
{
    if (s_typed_head == s_typed_tail) return 0;
    uint8_t c = s_typed_q[s_typed_tail];
    s_typed_tail = (s_typed_tail + 1) % TYPED_QUEUE_SZ;
    return c;
}

void PlatformSetTextInput(int on)
{
    if (g_headless) return;
    if (on) SDL_StartTextInput();
    else    SDL_StopTextInput();
    /* Drop any stale chars queued before/after the toggle. */
    s_typed_head = s_typed_tail = 0;
}

int PlatformInit(int w, int h, const char *title)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 0;
    }
    s_w = w; s_h = h;

    if (g_headless) {
        /* T45: no window/renderer/texture. Just init SDL so PumpEvents
 * still drives the event queue (dummy driver). Audio device
 * (dummy too) keeps the mixer callback firing — that way smoke
 * tests still exercise PlayDialogLine + TickMenuMusic etc. */
        fprintf(stderr, "[platform] SDL ready (headless): %dx%d, video=%s\n",
                w, h, SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?");
        return 1;
    }

    /* T54 — HiDPI scaling. The framebuffer stays 640×480, but the SDL
 * window can be enlarged Nx + SDL_RenderSetLogicalSize handles the
 * upscale via SDL_HINT_RENDER_SCALE_QUALITY. */
    int sf = g_scale_factor > 0 ? g_scale_factor : 1;
    int win_w = w * sf, win_h = h * sf;
    if (g_scale_mode && *g_scale_mode) {
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, g_scale_mode);
    }

    s_win = SDL_CreateWindow(title,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        win_w, win_h, SDL_WINDOW_SHOWN);
    if (!s_win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 0; }
    s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!s_ren) {
        /* try software fallback */
        s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!s_ren) { fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); return 0; }
    /* Logical size = native framebuffer; SDL upscales at present time. */
    SDL_RenderSetLogicalSize(s_ren, w, h);
    s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_ARGB8888,
                              SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!s_tex) { fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError()); return 0; }
    /* T31 v2 — hide the OS cursor; PaintCursor blits olowek/kaseta/
 * magnes/drzwi sprite at mouse pos every frame (
 * DirectDraw build where the GDI cursor was hidden and 
 * drew the sprite via the same blit path as scene entities).
 *
 * Initial call here covers the common case (cursor already over the
 * window at launch). PlatformPumpEvents re-asserts SDL_DISABLE on
 * every poll to defeat macOS Cocoa restoring the arrow on focus-loss
 * / mouse-leave / re-enter — those events fire AFTER startup so a
 * one-shot disable doesn't stick. The repeated call is a no-op when
 * the cursor is already hidden (SDL caches the state internally). */
    SDL_ShowCursor(SDL_DISABLE);
    fprintf(stderr, "[platform] SDL ready: %dx%d window (%dx scale, %s filter), renderer=%s\n",
            win_w, win_h, sf, g_scale_mode ? g_scale_mode : "nearest",
            SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?");

    /* Black initial frame so the window is never garbage. */
    memset(s_pixels32, 0, (size_t)w * h * 4);
    SDL_UpdateTexture(s_tex, NULL, s_pixels32, w * 4);
    SDL_RenderClear(s_ren);
    SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
    SDL_RenderPresent(s_ren);
    SDL_PumpEvents();
    return 1;
}

void PlatformShutdown(void)
{
    if (s_tex) { SDL_DestroyTexture(s_tex); s_tex = NULL; }
    if (s_ren) { SDL_DestroyRenderer(s_ren); s_ren = NULL; }
    if (s_win) { SDL_DestroyWindow(s_win);   s_win = NULL; }
    SDL_Quit();
}

void PlatformPresent(const uint8_t *shadow,
                     const uint8_t *palette_rgb, int w, int h)
{
    if (g_headless) return;                /* T45 — no render in headless */

    static int call_no = 0;
    ++call_no;
    if (!s_tex)    { if (call_no==1) fprintf(stderr,"[present] s_tex null!\n");    return; }
    if (!shadow)   { if (call_no==1) fprintf(stderr,"[present] shadow null!\n");    return; }
    if (!palette_rgb){if (call_no==1)fprintf(stderr,"[present] palette null!\n"); return; }

    int n = w * h;
    if (n > (int)(sizeof s_pixels32 / sizeof s_pixels32[0]))
        n = sizeof s_pixels32 / sizeof s_pixels32[0];
    for (int i = 0; i < n; ++i) {
        uint8_t idx = shadow[i];
        const uint8_t *e = palette_rgb + idx * 3;
        s_pixels32[i] = (0xFFu << 24) | ((uint32_t)e[0] << 16) |
                        ((uint32_t)e[1] << 8) | e[2];
    }
    SDL_UpdateTexture(s_tex, NULL, s_pixels32, w * 4);
    SDL_RenderClear(s_ren);
    SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
    SDL_RenderPresent(s_ren);
}

void PlatformPumpEvents(void)
{
    /* T31 v2 — re-assert cursor hide every pump. On macOS Cocoa
 * SDL_ShowCursor(SDL_DISABLE) called only at init is unreliable: the
 * OS restores the arrow when the SDL window loses focus, when the
 * mouse leaves & re-enters, and (sometimes) after a SDL_RenderPresent
 * triggers a re-layout. Re-asserting it here on every PollEvent loop
 * is cheap (SDL early-outs when the state already matches) and 100%
 * reliable across Cocoa / X11 / Win32. */
    if (!g_headless) SDL_ShowCursor(SDL_DISABLE);
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT:
            s_quit = 1;
            break;
        case SDL_WINDOWEVENT:
            /* T140 — some window managers (X11 / Wayland) dispatch a
 * SDL_WINDOWEVENT_CLOSE instead of SDL_QUIT when the user
 * closes the window via the [×] button. Treat both as a
 * quit request so the main loop unwinds + flushes saves. */
            if (ev.window.event == SDL_WINDOWEVENT_CLOSE) s_quit = 1;
            break;
        case SDL_KEYDOWN:
            g_key_state = (uint16_t)(ev.key.keysym.sym & 0xFF);
            if (ev.key.keysym.sym == SDLK_ESCAPE) s_quit = 1;
            /* T53 — quicksave / quickload latches. The play_demo_scene
 * main loop polls these once per frame and clears them. */
            if (ev.key.keysym.sym == SDLK_F5) g_quicksave_request = 1;
            if (ev.key.keysym.sym == SDLK_F9) g_quickload_request = 1;
            /* T56 — F3 stats dump (logs to stderr). */
            if (ev.key.keysym.sym == SDLK_F3) g_stats_dump_request = 1;
            /* T24 — F12 opens Pytanie quit-confirmation menu. */
            if (ev.key.keysym.sym == SDLK_F12) g_pause_menu_request = 1;
            /* Inline-edit (save-slot rename): queue Backspace / Enter
 * as typed-char events so the edit loop sees them alongside
 * SDL_TEXTINPUT printable chars. */
            if (ev.key.keysym.sym == SDLK_BACKSPACE) PlatformPushTypedChar(0x08);
            if (ev.key.keysym.sym == SDLK_RETURN ||
                ev.key.keysym.sym == SDLK_KP_ENTER) PlatformPushTypedChar(0x0D);
            break;
        case SDL_TEXTINPUT:
            /* Push the printable bytes (latin-1 single-byte chars only —
 * the original engine's slot name field is 30 bytes single-byte
 * and only accepts space + '0'..'Z'). Drop UTF-8
 * multi-byte sequences. */
            for (const char *p = ev.text.text; *p; ++p) {
                uint8_t c = (uint8_t)*p;
                if (c >= 0x80) continue;     /* skip UTF-8 lead/continuation */
                PlatformPushTypedChar(c);
            }
            break;
        case SDL_KEYUP:
            g_key_state &= 0xFF00;
            break;
        case SDL_MOUSEMOTION: {
            extern int16_t s_mouse_x, s_mouse_y;
            s_mouse_x = (int16_t)ev.motion.x;
            s_mouse_y = (int16_t)ev.motion.y;
            break;
        }
        case SDL_MOUSEBUTTONDOWN:
            if (ev.button.button == SDL_BUTTON_LEFT)  g_lmb_clicked = 1;
            if (ev.button.button == SDL_BUTTON_RIGHT) g_rmb_clicked = 1;
            break;
        }
    }
}

int PlatformShouldQuit(void) { return s_quit; }

void PlatformShowMessageBox(const char *title, const char *body)
{
    if (g_headless) {
        /* T45: no GUI dialog; log to stderr so CI runs still see fatal
 * messages (CD-rom missing, archive missing etc.). */
        fprintf(stderr, "[msgbox] %s: %s\n",
                title ? title : "(null)", body ? body : "(null)");
        return;
    }
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, title, body, s_win);
}
