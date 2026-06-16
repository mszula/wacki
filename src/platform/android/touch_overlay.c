/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform/android/touch_overlay.c — Android full-window touch layout.
 *
 * The engine canvas is 640×480 (4:3); a phone held landscape is far wider. We
 * DON'T let SDL letterbox the canvas (video_sdl.c skips SDL_RenderSetLogicalSize
 * on Android) — instead this module owns the whole window: the game texture is
 * drawn into a centred 4:3 rect, and the side panels left over become real,
 * touchable control surfaces:
 *
 *   - left panel:  virtual joystick → drives the cursor (precise, analog)
 *   - right panel: big circle = left click (walk/use), small = right click
 *
 * Why full-window instead of controls-in-the-bars: with SDL's logical-size
 * letterbox, the emulator (BlueStacks) maps touch to the rendered 4:3 canvas and
 * saturates anything in the bars to the canvas edge — the bars are unreachable.
 * Rendering full-window (no sub-canvas) makes the whole window the touch
 * surface, so the panels are addressable. This also unifies the mapping across
 * emulator and real hardware.
 *
 * All geometry here is in WINDOW pixels (no logical-size transform is active on
 * Android). Game-area touch maps window-px → 640×480 via the game rect. */

#include "wacki.h"          /* g_mouse_x/y, g_lmb_clicked, g_rmb_clicked, WACKI_SCREEN_* */
#include "wacki/platform/android_touch.h"

#include <SDL.h>
#include <math.h>

/* control sizing/placement as fractions of the panel width / window height,
 * so they scale with the device (wide phone → big panels, 16:9 emu → ~200px). */
#define STICK_PANEL_FRAC  0.42f
#define LMB_PANEL_FRAC    0.40f
#define RMB_PANEL_FRAC    0.26f
#define BTN_GAP_FRAC      0.12f      /* vertical gap LMB↔RMB, of panel width */
#define STICK_DEADZONE    0.18f
#define STICK_SPEED       6.0f      /* logical px / tick at full deflection */
#define TAP_MS            350u
#define TAP_SLOP_FRAC     0.02f     /* of window width */

#define A_STICK_BASE  46
#define A_STICK_KNOB  100
#define A_LMB         70
#define A_RMB         54

/* ---- layout, recomputed each frame from the renderer output size ---- */
static int s_have_geom = 0;
static int s_win_w = 0, s_win_h = 0;
static SDL_Rect s_game = { 0, 0, 0, 0 };
static int s_panel_l = 0, s_panel_r = 0;
static int s_stick_cx, s_stick_cy, s_stick_r, s_knob_r;
static int s_lmb_cx, s_lmb_cy, s_lmb_r;
static int s_rmb_cx, s_rmb_cy, s_rmb_r;
static int s_tap_slop = 8;

/* ---- runtime state ---- */
static float s_def_x = 0.0f, s_def_y = 0.0f;   /* stick deflection, -1..1 */
static int   s_stick_on = 0;
static float s_cur_x = 0.0f, s_cur_y = 0.0f;   /* stick-driven cursor accumulator */

enum { ROLE_NONE = 0, ROLE_STICK, ROLE_LMB, ROLE_RMB, ROLE_GAME };
typedef struct {
    SDL_FingerID id; int used; int role;
    uint32_t t0; int sx, sy, moved;            /* ROLE_GAME tap detection (window px) */
} Finger;
#define MAX_FINGERS 8
static Finger s_fingers[MAX_FINGERS];

static int clampi(int v, int lo, int hi){ return v < lo ? lo : (v > hi ? hi : v); }

static Finger *finger_get(SDL_FingerID id, int create)
{
    Finger *free_slot = NULL;
    for (int i = 0; i < MAX_FINGERS; ++i) {
        if (s_fingers[i].used && s_fingers[i].id == id) return &s_fingers[i];
        if (!s_fingers[i].used && !free_slot) free_slot = &s_fingers[i];
    }
    if (create && free_slot) {
        free_slot->used = 1; free_slot->id = id; free_slot->role = ROLE_NONE;
        free_slot->moved = 0;
        return free_slot;
    }
    return NULL;
}

static int in_circle(int x, int y, int cx, int cy, int r)
{
    if (r <= 0) return 0;
    long dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= (long)r * r;
}

static int in_rect(int x, int y, const SDL_Rect *r)
{
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

/* ---- layout ---- */
void wacki_overlay_compute_layout(SDL_Renderer *ren)
{
    if (!ren) return;
    SDL_GetRendererOutputSize(ren, &s_win_w, &s_win_h);
    if (s_win_w <= 0 || s_win_h <= 0) return;

    /* game = 640×480 (4:3) fitted to full height, centred; clamp to width. */
    int gh = s_win_h;
    int gw = (int)((long)gh * WACKI_SCREEN_W / WACKI_SCREEN_H);
    if (gw > s_win_w) { gw = s_win_w; gh = (int)((long)gw * WACKI_SCREEN_H / WACKI_SCREEN_W); }
    s_game.w = gw; s_game.h = gh;
    s_game.x = (s_win_w - gw) / 2;
    s_game.y = (s_win_h - gh) / 2;
    s_panel_l = s_game.x;
    s_panel_r = s_win_w - (s_game.x + gw);

    s_stick_cx = s_panel_l / 2;
    s_stick_cy = s_win_h / 2;
    s_stick_r  = (int)(s_panel_l * STICK_PANEL_FRAC);
    s_knob_r   = s_stick_r / 2;

    /* LMB centred vertically (mirrors the stick on the left); RMB stacked just
     * above it with a gap so the two never overlap regardless of panel width. */
    int rcx  = s_game.x + gw + s_panel_r / 2;
    s_lmb_cx = rcx; s_lmb_r = (int)(s_panel_r * LMB_PANEL_FRAC); s_lmb_cy = s_win_h / 2;
    s_rmb_cx = rcx; s_rmb_r = (int)(s_panel_r * RMB_PANEL_FRAC);
    s_rmb_cy = s_lmb_cy - s_lmb_r - s_rmb_r - (int)(s_panel_r * BTN_GAP_FRAC);

    s_tap_slop = (int)(s_win_w * TAP_SLOP_FRAC);
    s_have_geom = 1;
}

SDL_Rect wacki_overlay_game_rect(void) { return s_game; }

static void stick_set(int px, int py)
{
    if (s_stick_r <= 0) return;
    float dx = (float)(px - s_stick_cx) / s_stick_r;
    float dy = (float)(py - s_stick_cy) / s_stick_r;
    float m  = sqrtf(dx * dx + dy * dy);
    if (m > 1.0f) { dx /= m; dy /= m; }
    s_def_x = dx; s_def_y = dy;
}

/* ---- drawing (window px) ---- */
static void fill_circle(SDL_Renderer *ren, int cx, int cy, int r, Uint8 A)
{
    if (r <= 0) return;
    SDL_SetRenderDrawColor(ren, 255, 255, 255, A);
    for (int dy = -r; dy <= r; ++dy) {
        int dx = (int)floor(sqrt((double)r * r - (double)dy * dy));
        SDL_RenderDrawLine(ren, cx - dx, cy + dy, cx + dx, cy + dy);
    }
}

void wacki_overlay_draw(SDL_Renderer *ren)
{
    if (!ren || !s_have_geom) return;

    SDL_BlendMode prev_bm;
    SDL_GetRenderDrawBlendMode(ren, &prev_bm);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    Uint8 pr, pg, pb, pa;
    SDL_GetRenderDrawColor(ren, &pr, &pg, &pb, &pa);

    fill_circle(ren, s_stick_cx, s_stick_cy, s_stick_r, A_STICK_BASE);
    fill_circle(ren, s_stick_cx + (int)(s_def_x * s_stick_r),
                     s_stick_cy + (int)(s_def_y * s_stick_r), s_knob_r, A_STICK_KNOB);
    fill_circle(ren, s_lmb_cx, s_lmb_cy, s_lmb_r, A_LMB);
    fill_circle(ren, s_rmb_cx, s_rmb_cy, s_rmb_r, A_RMB);

    SDL_SetRenderDrawColor(ren, pr, pg, pb, pa);
    SDL_SetRenderDrawBlendMode(ren, prev_bm);
}

/* ---- input (we own all touch; synth is disabled) ---- */
int wacki_overlay_owns_touch(void) { return 1; }

static void game_set_cursor(int px, int py)
{
    if (s_game.w <= 0 || s_game.h <= 0) return;
    int gx = (px - s_game.x) * WACKI_SCREEN_W / s_game.w;
    int gy = (py - s_game.y) * WACKI_SCREEN_H / s_game.h;
    g_mouse_x = (int16_t)clampi(gx, 0, WACKI_SCREEN_W - 1);
    g_mouse_y = (int16_t)clampi(gy, 0, WACKI_SCREEN_H - 1);
}

void wacki_overlay_finger_down(SDL_FingerID id, float nx, float ny)
{
    if (!s_have_geom) return;
    int px = (int)(nx * s_win_w), py = (int)(ny * s_win_h);
    Finger *f = finger_get(id, 1);
    if (!f) return;

    if (in_circle(px, py, s_stick_cx, s_stick_cy, s_stick_r)) {
        f->role = ROLE_STICK;
        if (!s_stick_on) { s_cur_x = g_mouse_x; s_cur_y = g_mouse_y; }
        s_stick_on = 1;
        stick_set(px, py);
    } else if (in_circle(px, py, s_lmb_cx, s_lmb_cy, s_lmb_r)) {
        f->role = ROLE_LMB; g_lmb_clicked = 1;
    } else if (in_circle(px, py, s_rmb_cx, s_rmb_cy, s_rmb_r)) {
        f->role = ROLE_RMB; g_rmb_clicked = 1;
    } else if (in_rect(px, py, &s_game)) {
        f->role = ROLE_GAME; f->t0 = SDL_GetTicks();
        f->sx = px; f->sy = py; f->moved = 0;
        game_set_cursor(px, py);
    } else {
        f->role = ROLE_NONE;          /* empty panel space — ignore */
    }
}

void wacki_overlay_finger_motion(SDL_FingerID id, float nx, float ny)
{
    Finger *f = finger_get(id, 0);
    if (!f) return;
    int px = (int)(nx * s_win_w), py = (int)(ny * s_win_h);
    if (f->role == ROLE_STICK) {
        stick_set(px, py);
    } else if (f->role == ROLE_GAME) {
        if (abs(px - f->sx) > s_tap_slop || abs(py - f->sy) > s_tap_slop) f->moved = 1;
        game_set_cursor(px, py);
    }
}

void wacki_overlay_finger_up(SDL_FingerID id, float nx, float ny)
{
    (void)nx; (void)ny;
    Finger *f = finger_get(id, 0);
    if (!f) return;
    if (f->role == ROLE_STICK) { s_stick_on = 0; s_def_x = s_def_y = 0.0f; }
    else if (f->role == ROLE_GAME && !f->moved && (SDL_GetTicks() - f->t0) <= TAP_MS)
        g_lmb_clicked = 1;
    f->used = 0;
}

void wacki_overlay_tick(void)
{
    if (!s_stick_on) return;
    float m = sqrtf(s_def_x * s_def_x + s_def_y * s_def_y);
    if (m < STICK_DEADZONE) return;
    s_cur_x += s_def_x * STICK_SPEED;
    s_cur_y += s_def_y * STICK_SPEED;
    s_cur_x = (float)clampi((int)s_cur_x, 0, WACKI_SCREEN_W - 1);
    s_cur_y = (float)clampi((int)s_cur_y, 0, WACKI_SCREEN_H - 1);
    g_mouse_x = (int16_t)s_cur_x;
    g_mouse_y = (int16_t)s_cur_y;
}
