/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform/android/touch_overlay.c — Android on-screen touch overlay
 * (see include/wacki/platform/android_touch.h).
 *
 * A semi-transparent virtual joystick (drives the cursor) + left/right click
 * buttons, for players who prefer not to tap the canvas directly. Direct
 * tapping still works via SDL's touch→mouse synthesis; this only claims the
 * control circles and suppresses the synth there (wacki_overlay_owns_touch).
 *
 * The controls live in the engine's LOGICAL 640×480 space (over the game's
 * edges), NOT in the letterbox bars: emulators (BlueStacks) normalize touch to
 * the canvas, so the bars receive no usable touch — a control there is
 * untouchable. Placing them in logical space means they're drawn and hit-tested
 * through the exact same present transform as the game, so the touch lands where
 * the control is drawn on every device. */

#include "wacki.h"          /* g_mouse_x/y, g_lmb_clicked, g_rmb_clicked, WACKI_SCREEN_* */
#include "wacki/platform/android_touch.h"

#include <SDL.h>
#include <math.h>

/* ---- control geometry, in LOGICAL (640×480) px ------------------ *
 * Hugging the left/right edges at mid-height to stay clear of the bottom HUD
 * panel. Tunable; tell me if any overlaps a hotspot. */
#define STICK_CX   56
#define STICK_CY   240
#define STICK_R    46
#define KNOB_R     23
#define LMB_CX     584
#define LMB_CY     280
#define LMB_R      42
#define RMB_CX     584
#define RMB_CY     188
#define RMB_R      27

#define STICK_DEADZONE  0.18f
#define STICK_SPEED     6.0f    /* cursor logical-px/frame at full deflection */

/* alpha (over the game — subtle but visible) */
#define A_STICK_BASE  46
#define A_STICK_KNOB  100
#define A_LMB         70
#define A_RMB         54

static SDL_Renderer *s_ren = NULL;
static int   s_win_w = 0, s_win_h = 0;
static int   s_have_geom = 0;

static float s_def_x = 0.0f, s_def_y = 0.0f;
static int   s_stick_on = 0;
static float s_cur_x = 0.0f, s_cur_y = 0.0f;
static int   s_control_fingers = 0;

enum { ROLE_NONE = 0, ROLE_STICK, ROLE_LMB, ROLE_RMB };
typedef struct { SDL_FingerID id; int used; int role; } Finger;
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
    if (create && free_slot) { free_slot->used = 1; free_slot->id = id; free_slot->role = ROLE_NONE; return free_slot; }
    return NULL;
}

static int in_circle(int x, int y, int cx, int cy, int r)
{
    long dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= (long)r * r;
}

/* Touch (window-normalized) → engine logical px, via SDL's own present
 * transform (the same one that makes direct game taps land correctly). */
static int touch_to_logical(float nx, float ny, int *lx, int *ly)
{
    if (!s_ren || s_win_w <= 0) return 0;
    float fx = 0.0f, fy = 0.0f;
    SDL_RenderWindowToLogical(s_ren, (int)(nx * s_win_w), (int)(ny * s_win_h), &fx, &fy);
    *lx = (int)fx; *ly = (int)fy;
    return 1;
}

static void stick_set(int lx, int ly)
{
    float dx = (float)(lx - STICK_CX) / STICK_R;
    float dy = (float)(ly - STICK_CY) / STICK_R;
    float m  = sqrtf(dx * dx + dy * dy);
    if (m > 1.0f) { dx /= m; dy /= m; }
    s_def_x = dx; s_def_y = dy;
}

/* ---- drawing (logical coords; logical-size letterbox stays active) --- */
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
    if (!ren) return;
    s_ren = ren;
    SDL_Window *win = SDL_RenderGetWindow(ren);
    if (win) SDL_GetWindowSize(win, &s_win_w, &s_win_h);
    if (s_win_w <= 0) SDL_GetRendererOutputSize(ren, &s_win_w, &s_win_h);
    s_have_geom = 1;

    SDL_BlendMode prev_bm;
    SDL_GetRenderDrawBlendMode(ren, &prev_bm);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    Uint8 pr, pg, pb, pa;
    SDL_GetRenderDrawColor(ren, &pr, &pg, &pb, &pa);

    fill_circle(ren, STICK_CX, STICK_CY, STICK_R, A_STICK_BASE);
    fill_circle(ren, STICK_CX + (int)(s_def_x * STICK_R),
                     STICK_CY + (int)(s_def_y * STICK_R), KNOB_R, A_STICK_KNOB);
    fill_circle(ren, LMB_CX, LMB_CY, LMB_R, A_LMB);
    fill_circle(ren, RMB_CX, RMB_CY, RMB_R, A_RMB);

    SDL_SetRenderDrawColor(ren, pr, pg, pb, pa);
    SDL_SetRenderDrawBlendMode(ren, prev_bm);
}

/* ---- input: control circles only (game area = SDL synth) -------- */
int wacki_overlay_owns_touch(void) { return s_control_fingers > 0; }

void wacki_overlay_finger_down(SDL_FingerID id, float nx, float ny)
{
    if (!s_have_geom) return;
    int lx, ly;
    if (!touch_to_logical(nx, ny, &lx, &ly)) return;
    Finger *f = finger_get(id, 1);
    if (!f) return;

    if (in_circle(lx, ly, STICK_CX, STICK_CY, STICK_R)) {
        f->role = ROLE_STICK; ++s_control_fingers;
        if (!s_stick_on) { s_cur_x = g_mouse_x; s_cur_y = g_mouse_y; }
        s_stick_on = 1;
        stick_set(lx, ly);
    } else if (in_circle(lx, ly, LMB_CX, LMB_CY, LMB_R)) {
        f->role = ROLE_LMB; ++s_control_fingers; g_lmb_clicked = 1;
    } else if (in_circle(lx, ly, RMB_CX, RMB_CY, RMB_R)) {
        f->role = ROLE_RMB; ++s_control_fingers; g_rmb_clicked = 1;
    }
    /* else: game-area touch → role NONE, SDL synth handles it. */
}

void wacki_overlay_finger_motion(SDL_FingerID id, float nx, float ny)
{
    Finger *f = finger_get(id, 0);
    if (!f || f->role != ROLE_STICK) return;
    int lx, ly;
    if (touch_to_logical(nx, ny, &lx, &ly)) stick_set(lx, ly);
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
