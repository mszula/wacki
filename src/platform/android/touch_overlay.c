/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform/android/touch_overlay.c — Android on-screen touch overlay
 * (see include/wacki/platform/android_touch.h).
 *
 * Fills the letterbox pillarbox bars with a virtual joystick (left, drives the
 * cursor) + left/right click buttons (right), semi-transparent. The GAME AREA
 * is NOT handled here — SDL's built-in touch→mouse synthesis maps those touches
 * through the renderer's real present transform (exact on every device). This
 * module only claims the control zones in the bars and, while a finger is on a
 * control, tells the SDL layer to suppress the synth there
 * (wacki_overlay_owns_touch) so a bar touch doesn't also drag/click the cursor.
 *
 * Geometry is in WINDOW pixels (where SDL_FINGER* events are normalized);
 * drawing happens after the logical-size letterbox is disabled, in renderer-
 * OUTPUT pixels, scaled by output/window. */

#include "wacki.h"          /* g_mouse_x/y, g_lmb_clicked, g_rmb_clicked, WACKI_SCREEN_* */
#include "wacki/platform/android_touch.h"

#include <SDL.h>
#include <math.h>

/* ---- tunables ---------------------------------------------------- */
#define BAR_MIN_PX        96      /* hide controls if a side bar is narrower */
#define STICK_BAR_FRAC    0.42f
#define STICK_H_FRAC      0.17f
#define STICK_CY_FRAC     0.60f
#define KNOB_FRAC         0.50f
#define LMB_BAR_FRAC      0.42f
#define LMB_H_FRAC        0.16f
#define LMB_CY_FRAC       0.62f
#define RMB_FRAC          0.64f
#define STICK_DEADZONE    0.18f
#define STICK_SPEED       7.0f    /* cursor px/frame at full deflection */

/* overlay paint (RGBA, low alpha = subtle over the black bars) */
#define A_STICK_BASE      40
#define A_STICK_KNOB      95
#define A_LMB             64
#define A_RMB             48

/* ---- geometry (WINDOW pixels), cached on draw ------------------- */
static int   s_win_w = 0, s_win_h = 0;
static float s_kx = 1.0f, s_ky = 1.0f;          /* window→output draw scale */
static int   s_have_geom = 0;
static int   s_gx0, s_gw;                         /* canvas rect (window px) */
static int   s_controls_on = 0;
static int   s_stick_cx, s_stick_cy, s_stick_r;
static int   s_lmb_cx, s_lmb_cy, s_lmb_r;
static int   s_rmb_cx, s_rmb_cy, s_rmb_r;

/* ---- virtual stick + per-finger state --------------------------- */
static float s_def_x = 0.0f, s_def_y = 0.0f;    /* deflection -1..1 */
static int   s_stick_on = 0;
static float s_cur_x = 0.0f, s_cur_y = 0.0f;    /* sub-pixel cursor mirror */
static int   s_control_fingers = 0;              /* fingers on a control zone */

enum { ROLE_NONE = 0, ROLE_STICK, ROLE_LMB, ROLE_RMB };
typedef struct { SDL_FingerID id; int used; int role; } Finger;
#define MAX_FINGERS 8
static Finger s_fingers[MAX_FINGERS];

/* ---- helpers ----------------------------------------------------- */
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
        return free_slot;
    }
    return NULL;
}

static int in_circle(int px, int py, int cx, int cy, int r)
{
    long dx = px - cx, dy = py - cy;
    return dx * dx + dy * dy <= (long)r * r;
}

static void recompute(SDL_Renderer *ren, int ww, int wh, int ow, int oh)
{
    s_win_w = ww; s_win_h = wh;
    s_kx = ww > 0 ? (float)ow / ww : 1.0f;
    s_ky = wh > 0 ? (float)oh / wh : 1.0f;

    /* Canvas rect in WINDOW px, from SDL's present transform. */
    int ax = 0, ay = 0, bx = 0, by = 0;
    SDL_RenderLogicalToWindow(ren, 0.0f, 0.0f, &ax, &ay);
    SDL_RenderLogicalToWindow(ren, (float)WACKI_SCREEN_W, (float)WACKI_SCREEN_H, &bx, &by);
    s_gx0 = ax; s_gw = bx - ax;

    int left_bar  = s_gx0;
    int right_bar = ww - (s_gx0 + s_gw);
    s_controls_on = (left_bar >= BAR_MIN_PX && right_bar >= BAR_MIN_PX);

    s_stick_r  = (int)((left_bar * STICK_BAR_FRAC < wh * STICK_H_FRAC)
                       ? left_bar * STICK_BAR_FRAC : wh * STICK_H_FRAC);
    s_stick_cx = left_bar / 2;
    s_stick_cy = (int)(wh * STICK_CY_FRAC);

    int rcx = s_gx0 + s_gw + right_bar / 2;
    int lr  = (int)((right_bar * LMB_BAR_FRAC < wh * LMB_H_FRAC)
                    ? right_bar * LMB_BAR_FRAC : wh * LMB_H_FRAC);
    s_lmb_r  = lr;
    s_lmb_cx = rcx;
    s_lmb_cy = (int)(wh * LMB_CY_FRAC);
    s_rmb_r  = (int)(lr * RMB_FRAC);
    s_rmb_cx = rcx;
    s_rmb_cy = s_lmb_cy - s_lmb_r - s_rmb_r - (int)(wh * 0.03f);

    s_have_geom = 1;
}

static void stick_set(int px, int py)
{
    if (s_stick_r <= 0) return;
    float dx = (float)(px - s_stick_cx) / s_stick_r;
    float dy = (float)(py - s_stick_cy) / s_stick_r;
    float m  = sqrtf(dx * dx + dy * dy);
    if (m > 1.0f) { dx /= m; dy /= m; }
    s_def_x = dx; s_def_y = dy;
}

/* ---- drawing ---------------------------------------------------- */
static void fill_circle(SDL_Renderer *ren, int cx, int cy, int r, Uint8 A)
{
    if (r <= 0) return;
    SDL_SetRenderDrawColor(ren, 255, 255, 255, A);
    for (int dy = -r; dy <= r; ++dy) {
        int dx = (int)floor(sqrt((double)r * r - (double)dy * dy));
        SDL_RenderDrawLine(ren, cx - dx, cy + dy, cx + dx, cy + dy);
    }
}
static void draw_control(SDL_Renderer *ren, int cx, int cy, int r, Uint8 A)
{
    fill_circle(ren, (int)(cx * s_kx), (int)(cy * s_ky), (int)(r * s_kx), A);
}

void wacki_overlay_draw(SDL_Renderer *ren)
{
    if (!ren) return;
    int ow = 0, oh = 0, ww = 0, wh = 0;
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    SDL_Window *win = SDL_RenderGetWindow(ren);
    if (win) SDL_GetWindowSize(win, &ww, &wh);
    if (ww <= 0 || wh <= 0) { ww = ow; wh = oh; }
    if (ow <= 0 || oh <= 0) return;
    recompute(ren, ww, wh, ow, oh);     /* logical size still active here */
    if (!s_controls_on) return;

    int lw = 0, lh = 0;
    SDL_RenderGetLogicalSize(ren, &lw, &lh);
    SDL_RenderSetLogicalSize(ren, 0, 0);
    SDL_BlendMode prev_bm;
    SDL_GetRenderDrawBlendMode(ren, &prev_bm);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    Uint8 pr, pg, pb, pa;
    SDL_GetRenderDrawColor(ren, &pr, &pg, &pb, &pa);

    draw_control(ren, s_stick_cx, s_stick_cy, s_stick_r, A_STICK_BASE);
    int kx = s_stick_cx + (int)(s_def_x * s_stick_r);
    int ky = s_stick_cy + (int)(s_def_y * s_stick_r);
    draw_control(ren, kx, ky, (int)(s_stick_r * KNOB_FRAC), A_STICK_KNOB);
    draw_control(ren, s_lmb_cx, s_lmb_cy, s_lmb_r, A_LMB);
    draw_control(ren, s_rmb_cx, s_rmb_cy, s_rmb_r, A_RMB);

    SDL_SetRenderDrawColor(ren, pr, pg, pb, pa);
    SDL_SetRenderDrawBlendMode(ren, prev_bm);
    SDL_RenderSetLogicalSize(ren, lw, lh);
}

/* ---- input: control zones only (game area = SDL synth) ---------- */
int wacki_overlay_owns_touch(void) { return s_control_fingers > 0; }

void wacki_overlay_finger_down(SDL_FingerID id, float nx, float ny)
{
    if (!s_have_geom || !s_controls_on) return;   /* no bars → synth handles all */
    int px = (int)(nx * s_win_w), py = (int)(ny * s_win_h);
    Finger *f = finger_get(id, 1);
    if (!f) return;

    if (in_circle(px, py, s_stick_cx, s_stick_cy, s_stick_r)) {
        f->role = ROLE_STICK; ++s_control_fingers;
        if (!s_stick_on) { s_cur_x = g_mouse_x; s_cur_y = g_mouse_y; }
        s_stick_on = 1;
        stick_set(px, py);
    } else if (in_circle(px, py, s_lmb_cx, s_lmb_cy, s_lmb_r)) {
        f->role = ROLE_LMB; ++s_control_fingers; g_lmb_clicked = 1;
    } else if (in_circle(px, py, s_rmb_cx, s_rmb_cy, s_rmb_r)) {
        f->role = ROLE_RMB; ++s_control_fingers; g_rmb_clicked = 1;
    }
    /* else: game-area touch → role stays NONE, not counted; SDL synth handles it. */
}

void wacki_overlay_finger_motion(SDL_FingerID id, float nx, float ny)
{
    if (!s_have_geom) return;
    Finger *f = finger_get(id, 0);
    if (f && f->role == ROLE_STICK)
        stick_set((int)(nx * s_win_w), (int)(ny * s_win_h));
}

void wacki_overlay_finger_up(SDL_FingerID id, float nx, float ny)
{
    (void)nx; (void)ny;
    Finger *f = finger_get(id, 0);
    if (!f) return;
    if (f->role != ROLE_NONE) {
        if (f->role == ROLE_STICK) { s_stick_on = 0; s_def_x = s_def_y = 0.0f; }
        if (s_control_fingers > 0) --s_control_fingers;
    }
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
