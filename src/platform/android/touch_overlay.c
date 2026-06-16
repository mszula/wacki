/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform/android/touch_overlay.c — Android on-screen touch overlay +
 * canvas touch mapping (see include/wacki/platform/android_touch.h).
 *
 * Owns ALL touch on Android. Measured on-device (BlueStacks): the app's touch
 * surface is the GAME WINDOW itself — the emulator draws the letterbox bars but
 * they're outside the touch area, so a touch normalizes to the canvas. Hence
 * logical = nx·640 / ny·480 (NOT SDL's window-letterbox transform, which is
 * what made clicks drift). Game-area touch → cursor + tap-click; the joystick +
 * buttons (hugging the canvas edges, the only touchable spot — the bars can't
 * host controls) drive the cursor / latches.
 *
 * NOTE: this assumes touch is normalized to the canvas, which holds on the SDL
 * Android surface here. On a device where the surface includes the letterbox
 * bars the canvas-edge controls + mapping would need the window transform — see
 * git history for the SDL_RenderWindowToLogical variant. */

#include "wacki.h"          /* g_mouse_x/y, g_lmb_clicked, g_rmb_clicked, WACKI_SCREEN_* */
#include "wacki/platform/android_touch.h"

#include <SDL.h>
#include <math.h>

/* ---- control geometry, in LOGICAL (640×480) px — edge-hugging ---- */
#define STICK_CX   48
#define STICK_CY   240
#define STICK_R    44
#define KNOB_R     22
#define LMB_CX     592
#define LMB_CY     280
#define LMB_R      42
#define RMB_CX     592
#define RMB_CY     190
#define RMB_R      27

#define STICK_DEADZONE  0.18f
#define STICK_SPEED     6.0f
#define TAP_MS          350u
#define TAP_SLOP        10      /* logical px */

#define A_STICK_BASE  46
#define A_STICK_KNOB  100
#define A_LMB         70
#define A_RMB         54

static int   s_have_geom = 0;
static float s_def_x = 0.0f, s_def_y = 0.0f;
static int   s_stick_on = 0;
static float s_cur_x = 0.0f, s_cur_y = 0.0f;

enum { ROLE_NONE = 0, ROLE_STICK, ROLE_LMB, ROLE_RMB, ROLE_GAME };
typedef struct {
    SDL_FingerID id; int used; int role;
    uint32_t t0; int sx, sy, moved;     /* ROLE_GAME tap detection */
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
    long dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= (long)r * r;
}

static void to_logical(float nx, float ny, int *lx, int *ly)
{
    int x = (int)(nx * WACKI_SCREEN_W), y = (int)(ny * WACKI_SCREEN_H);
    *lx = clampi(x, 0, WACKI_SCREEN_W - 1);
    *ly = clampi(y, 0, WACKI_SCREEN_H - 1);
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

/* ---- input (we own all touch; synth is disabled) ---------------- */
int wacki_overlay_owns_touch(void) { return 1; }

void wacki_overlay_finger_down(SDL_FingerID id, float nx, float ny)
{
    if (!s_have_geom) return;
    int lx, ly;
    to_logical(nx, ny, &lx, &ly);
    Finger *f = finger_get(id, 1);
    if (!f) return;

    if (in_circle(lx, ly, STICK_CX, STICK_CY, STICK_R)) {
        f->role = ROLE_STICK;
        if (!s_stick_on) { s_cur_x = g_mouse_x; s_cur_y = g_mouse_y; }
        s_stick_on = 1;
        stick_set(lx, ly);
    } else if (in_circle(lx, ly, LMB_CX, LMB_CY, LMB_R)) {
        f->role = ROLE_LMB; g_lmb_clicked = 1;
    } else if (in_circle(lx, ly, RMB_CX, RMB_CY, RMB_R)) {
        f->role = ROLE_RMB; g_rmb_clicked = 1;
    } else {
        /* game canvas: move the cursor under the finger, arm a tap. */
        f->role = ROLE_GAME; f->t0 = SDL_GetTicks();
        f->sx = lx; f->sy = ly; f->moved = 0;
        g_mouse_x = (int16_t)lx; g_mouse_y = (int16_t)ly;
    }
}

void wacki_overlay_finger_motion(SDL_FingerID id, float nx, float ny)
{
    Finger *f = finger_get(id, 0);
    if (!f) return;
    int lx, ly;
    to_logical(nx, ny, &lx, &ly);
    if (f->role == ROLE_STICK) {
        stick_set(lx, ly);
    } else if (f->role == ROLE_GAME) {
        if (abs(lx - f->sx) > TAP_SLOP || abs(ly - f->sy) > TAP_SLOP) f->moved = 1;
        g_mouse_x = (int16_t)lx; g_mouse_y = (int16_t)ly;
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
