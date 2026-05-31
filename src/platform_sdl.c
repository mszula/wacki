/* src/platform_sdl.c — portable platform layer (SDL2).
 *
 * Replaces the original DirectDraw / WndProc / WaitMessage stack. The
 * engine renders into a flat 8-bpp uint8_t shadow buffer; this layer
 * uploads that buffer to a streaming SDL_Texture via a palette LUT
 * and pumps SDL events into the engine's input + key-latch globals.
 *
 * Headless mode (--headless) keeps SDL_Init alive (dummy video +
 * audio drivers) so the event queue + mixer callback still run, but
 * skips window/renderer/texture creation so CI smoke tests work
 * without a display.
 *
 * Public API (declared in wacki.h):
 *   PlatformInit / PlatformShutdown
 *   PlatformPresent  — upload + present one frame
 *   PlatformPumpEvents — drain SDL events, update input globals
 *   PlatformShouldQuit — set by SDL_QUIT / WINDOWCLOSE / ESC
 *   PlatformSetTextInput / PlatformPollTypedChar / PushTypedChar
 *   PlatformShowMessageBox
 */

#include "wacki.h"

#include <SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- constants ---------------------------------------------------- */

/* Typed-char ring buffer for inline-edit (save-slot rename). Populated
 * by SDL_TEXTINPUT (printable chars) + SDL_KEYDOWN (Backspace / Enter).
 * Drained by PlatformPollTypedChar — returns 0 when empty. */
#define TYPED_QUEUE_SZ              32

/* ASCII control codes the inline-edit handler expects. */
#define ASCII_BACKSPACE             0x08
#define ASCII_ENTER                 0x0D

/* ARGB8888 pixel layout (matches SDL_PIXELFORMAT_ARGB8888): alpha is
 * the top byte, then R / G / B descending. */
#define ARGB_BYTES_PER_PIXEL        4
#define ARGB_ALPHA_OPAQUE_SHIFTED   (0xFFu << 24)
#define ARGB_R_SHIFT                16
#define ARGB_G_SHIFT                8

/* UTF-8 lead/continuation bytes start at 0x80 — used to drop multi-
 * byte sequences from SDL_TEXTINPUT (the save-slot name field is
 * single-byte latin-1, accepts only space + '0'..'Z'). */
#define UTF8_MULTIBYTE_MARK         0x80

/* PALETTE_BYTES_PER_ENTRY mirrors the 3-byte RGB entries in the .PAL
 * file — kept local so this module doesn't depend on the alpha-blit
 * module's copy. */
#define PALETTE_BYTES_PER_ENTRY     3

/* Default scale factor when --scale wasn't given (1× = native 640×480
 * window). */
#define DEFAULT_SCALE_FACTOR        1

/* ---- module state ------------------------------------------------- */

static SDL_Window   *s_win  = NULL;
static SDL_Renderer *s_ren  = NULL;
static SDL_Texture  *s_tex  = NULL;
static int           s_w = 0, s_h = 0;
static int           s_quit = 0;
static uint32_t      s_pixels32[640 * 480];

static uint8_t       s_typed_q[TYPED_QUEUE_SZ];
static int           s_typed_head = 0, s_typed_tail = 0;

extern int         g_headless;
extern int         g_scale_factor;
extern const char *g_scale_mode;

/* ---- typed-char ring buffer -------------------------------------- */

void PlatformPushTypedChar(uint8_t c)
{
    int next = (s_typed_head + 1) % TYPED_QUEUE_SZ;
    if (next == s_typed_tail) return;     /* queue full → drop */
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

/* ---- init / shutdown --------------------------------------------- */

int PlatformInit(int w, int h, const char *title)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 0;
    }
    s_w = w;
    s_h = h;

    if (g_headless) {
        /* T45: no window/renderer/texture. SDL stays initialised (dummy
         * video + audio drivers) so PumpEvents still drives the event
         * queue and the mixer callback still fires — CI smoke tests
         * exercise PlayDialogLine + TickMenuMusic etc. */
        const char *drv = SDL_GetCurrentVideoDriver();
        fprintf(stderr, "[platform] SDL ready (headless): %dx%d, video=%s\n",
                w, h, drv ? drv : "?");
        return 1;
    }

    /* T54 — HiDPI scaling. The framebuffer stays w×h; the SDL window
     * can be enlarged Nx and SDL_RenderSetLogicalSize handles the
     * upscale via SDL_HINT_RENDER_SCALE_QUALITY. */
    int sf    = g_scale_factor > 0 ? g_scale_factor : DEFAULT_SCALE_FACTOR;
    int win_w = w * sf;
    int win_h = h * sf;
    if (g_scale_mode && *g_scale_mode) {
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, g_scale_mode);
    }

    s_win = SDL_CreateWindow(title,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        win_w, win_h, SDL_WINDOW_SHOWN);
    if (!s_win) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 0;
    }

    s_ren = SDL_CreateRenderer(s_win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!s_ren) {
        /* Try software fallback before bailing. */
        s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!s_ren) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        return 0;
    }

    SDL_RenderSetLogicalSize(s_ren, w, h);
    s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_ARGB8888,
                              SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!s_tex) {
        fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError());
        return 0;
    }

    /* T31 v2 — hide the OS cursor; PaintCursor blits the olowek /
     * kaseta / magnes / drzwi sprite at mouse pos every frame
     * (matches the original DirectDraw build where the GDI cursor was
     * hidden and the engine drew its own sprite via the scene-blit
     * path).
     *
     * The initial call here covers the common case (cursor over the
     * window at launch). PlatformPumpEvents re-asserts SDL_DISABLE on
     * every poll to defeat macOS Cocoa restoring the arrow on focus-
     * loss / mouse-leave / re-enter. The repeated call is cheap (SDL
     * early-outs when the state already matches). */
    SDL_ShowCursor(SDL_DISABLE);

    const char *drv = SDL_GetCurrentVideoDriver();
    fprintf(stderr,
            "[platform] SDL ready: %dx%d window (%dx scale, %s filter), renderer=%s\n",
            win_w, win_h, sf,
            g_scale_mode ? g_scale_mode : "nearest",
            drv ? drv : "?");

    /* Black initial frame so the window is never garbage. */
    memset(s_pixels32, 0, (size_t)w * h * ARGB_BYTES_PER_PIXEL);
    SDL_UpdateTexture(s_tex, NULL, s_pixels32, w * ARGB_BYTES_PER_PIXEL);
    SDL_RenderClear(s_ren);
    SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
    SDL_RenderPresent(s_ren);
    SDL_PumpEvents();
    return 1;
}

void PlatformShutdown(void)
{
    if (s_tex) { SDL_DestroyTexture (s_tex); s_tex = NULL; }
    if (s_ren) { SDL_DestroyRenderer(s_ren); s_ren = NULL; }
    if (s_win) { SDL_DestroyWindow  (s_win); s_win = NULL; }
    SDL_Quit();
}

/* ---- presentation ------------------------------------------------ */

/* Convert one (palette index, palette[256×3]) frame into the streaming
 * ARGB8888 texture and present it. Caller passes the shadow buffer +
 * the live palette explicitly so this module doesn't reach into
 * graphics.c. */
void PlatformPresent(const uint8_t *shadow,
                     const uint8_t *palette_rgb, int w, int h)
{
    if (g_headless) return;
    if (!s_tex || !shadow || !palette_rgb) return;

    int n = w * h;
    int cap = (int)(sizeof s_pixels32 / sizeof s_pixels32[0]);
    if (n > cap) n = cap;

    for (int i = 0; i < n; ++i) {
        uint8_t        idx = shadow[i];
        const uint8_t *e   = palette_rgb + idx * PALETTE_BYTES_PER_ENTRY;
        s_pixels32[i] = ARGB_ALPHA_OPAQUE_SHIFTED
                      | ((uint32_t)e[0] << ARGB_R_SHIFT)
                      | ((uint32_t)e[1] << ARGB_G_SHIFT)
                      |  (uint32_t)e[2];
    }
    SDL_UpdateTexture(s_tex, NULL, s_pixels32, w * ARGB_BYTES_PER_PIXEL);
    SDL_RenderClear(s_ren);
    SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
    SDL_RenderPresent(s_ren);
}

/* ---- event pump -------------------------------------------------- */

/* Per-event-type handlers — each takes the SDL_Event and returns
 * nothing. PlatformPumpEvents dispatches on ev.type. */

static void handle_keydown(const SDL_Event *ev)
{
    SDL_Keycode sym = ev->key.keysym.sym;
    g_key_state = (uint16_t)(sym & 0xFF);

    if (sym == SDLK_ESCAPE) s_quit = 1;

    /* T53 — quicksave / quickload latches consumed by the play_demo_
     * scene main loop once per frame. */
    if (sym == SDLK_F5)  g_quicksave_request  = 1;
    if (sym == SDLK_F9)  g_quickload_request  = 1;
    /* T56 — F3 stats dump (logs to stderr). */
    if (sym == SDLK_F3)  g_stats_dump_request = 1;
    /* T24 — F12 opens the Pytanie quit-confirmation menu. */
    if (sym == SDLK_F12) g_pause_menu_request = 1;

    /* Inline-edit (save-slot rename): queue Backspace / Enter as
     * typed-char events so the edit loop sees them alongside the
     * SDL_TEXTINPUT printable chars. */
    if (sym == SDLK_BACKSPACE)
        PlatformPushTypedChar(ASCII_BACKSPACE);
    if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER)
        PlatformPushTypedChar(ASCII_ENTER);
}

static void handle_textinput(const SDL_Event *ev)
{
    /* Push the printable bytes (latin-1 single-byte only — the
     * original engine's slot-name field is 30 bytes single-byte and
     * only accepts space + '0'..'Z'). Drop UTF-8 multi-byte
     * sequences entirely. */
    for (const char *p = ev->text.text; *p; ++p) {
        uint8_t c = (uint8_t)*p;
        if (c >= UTF8_MULTIBYTE_MARK) continue;
        PlatformPushTypedChar(c);
    }
}

static void handle_mouse_motion(const SDL_Event *ev)
{
    extern int16_t s_mouse_x, s_mouse_y;
    s_mouse_x = (int16_t)ev->motion.x;
    s_mouse_y = (int16_t)ev->motion.y;
}

static void handle_mouse_button_down(const SDL_Event *ev)
{
    if (ev->button.button == SDL_BUTTON_LEFT)  g_lmb_clicked = 1;
    if (ev->button.button == SDL_BUTTON_RIGHT) g_rmb_clicked = 1;
}

void PlatformPumpEvents(void)
{
    /* T31 v2 — re-assert cursor hide every pump. On macOS Cocoa,
     * SDL_ShowCursor(SDL_DISABLE) called only at init is unreliable —
     * the OS restores the arrow on focus-loss / mouse-leave / re-
     * enter / sometimes after SDL_RenderPresent triggers a re-layout.
     * Re-asserting here is cheap and 100% reliable across Cocoa /
     * X11 / Win32. */
    if (!g_headless) SDL_ShowCursor(SDL_DISABLE);

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT:
            s_quit = 1;
            break;
        case SDL_WINDOWEVENT:
            /* T140 — some window managers (X11 / Wayland) dispatch
             * WINDOWEVENT_CLOSE instead of SDL_QUIT when the user
             * clicks [×]. Treat both as a quit request so the main
             * loop unwinds + flushes saves. */
            if (ev.window.event == SDL_WINDOWEVENT_CLOSE) s_quit = 1;
            break;
        case SDL_KEYDOWN:
            handle_keydown(&ev);
            break;
        case SDL_KEYUP:
            g_key_state &= 0xFF00;
            break;
        case SDL_TEXTINPUT:
            handle_textinput(&ev);
            break;
        case SDL_MOUSEMOTION:
            handle_mouse_motion(&ev);
            break;
        case SDL_MOUSEBUTTONDOWN:
            handle_mouse_button_down(&ev);
            break;
        }
    }
}

int PlatformShouldQuit(void) { return s_quit; }

/* ---- message box ------------------------------------------------- */

void PlatformShowMessageBox(const char *title, const char *body)
{
    if (g_headless) {
        /* No GUI dialog in headless mode — log to stderr so CI runs
         * still see fatal messages (CD-rom missing, archive missing
         * etc.). */
        fprintf(stderr, "[msgbox] %s: %s\n",
                title ? title : "(null)",
                body  ? body  : "(null)");
        return;
    }
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, title, body, s_win);
}
