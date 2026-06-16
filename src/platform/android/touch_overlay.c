/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform/android/touch_overlay.c — Android on-screen touch overlay
 * (see include/wacki/platform/android_touch.h).
 *
 * Fills the letterbox pillarbox bars with a virtual joystick (left, drives the
 * cursor) + left/right click buttons (right). Semi-transparent so it doesn't
 * fight the artwork. Owns all touch on Android: controls in the bars, plus
 * drag-to-aim / tap-to-click on the game canvas (with the letterbox→logical
 * coordinate conversion the draw step's cached geometry provides).
 *
 * All tunables (sizes, alpha, cursor speed, deadzone) are constants below —
 * touch feel wants on-device tuning. */

#include "wacki.h"          /* g_mouse_x/y, g_lmb_clicked, g_rmb_clicked, WACKI_SCREEN_* */
#include "wacki/platform/android_touch.h"

#include <SDL.h>
#include <math.h>

/* ---- tunables ---------------------------------------------------- */
#define BAR_MIN_PX        96      /* hide controls if a side bar is narrower */
#define STICK_BAR_FRAC    0.42f   /* stick radius vs bar width … */
#define STICK_H_FRAC      0.17f   /* … and vs output height (min wins) */
#define STICK_CY_FRAC     0.60f   /* stick vertical centre (thumb reach) */
#define KNOB_FRAC         0.50f   /* knob radius vs stick radius */
#define LMB_BAR_FRAC      0.42f
#define LMB_H_FRAC        0.16f
#define LMB_CY_FRAC       0.62f
#define RMB_FRAC          0.64f   /* RMB radius vs LMB radius */
#define STICK_DEADZONE    0.18f
#define STICK_SPEED       7.0f    /* cursor px/frame at full deflection */
#define TAP_MS            350u    /* canvas press↔release under this = a tap */
#define TAP_SLOP_PX       18      /* … with movement under this */

/* overlay paint (RGBA, low alpha = subtle over the black bars) */
#define A_STICK_BASE      40
#define A_STICK_KNOB      95
#define A_LMB             64
#define A_RMB             48

/* ---- geometry (renderer output pixels), cached on draw ----------- */
static int   s_ow = 0, s_oh = 0;
static int   s_have_geom = 0;
static float s_scale = 1.0f;
static int   s_gx0, s_gy0, s_gw, s_gh;          /* game canvas rect */
static int   s_controls_on = 0;
static int   s_stick_cx, s_stick_cy, s_stick_r;
static int   s_lmb_cx, s_lmb_cy, s_lmb_r;
static int   s_rmb_cx, s_rmb_cy, s_rmb_r;

/* ---- virtual stick + per-finger state --------------------------- */
static float s_def_x = 0.0f, s_def_y = 0.0f;    /* deflection -1..1 */
static int   s_stick_on = 0;
static float s_cur_x = 0.0f, s_cur_y = 0.0f;    /* sub-pixel cursor mirror */

enum { ROLE_NONE = 0, ROLE_STICK, ROLE_LMB, ROLE_RMB, ROLE_GAME };
typedef struct {
    SDL_FingerID id;
    int          used;
    int          role;
    uint32_t     t0;          /* ROLE_GAME: press time */
    int          sx, sy;      /* ROLE_GAME: press point (output px) */
    int          moved;
} Finger;
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
        free_slot->moved = 0;
        return free_slot;
    }
    return NULL;
}
static void finger_release(SDL_FingerID id)
{
    for (int i = 0; i < MAX_FINGERS; ++i)
        if (s_fingers[i].used && s_fingers[i].id == id) { s_fingers[i].used = 0; return; }
}

static int in_circle(int px, int py, int cx, int cy, int r)
{
    long dx = px - cx, dy = py - cy;
    return dx * dx + dy * dy <= (long)r * r;
}

/* Map an output-pixel point inside the game canvas to logical 640×480. Returns
 * 0 if the point is outside the canvas (i.e. in a bar). */
static int win_to_logical(int px, int py, int *lx, int *ly)
{
    if (s_scale <= 0.0f) return 0;
    int x = (int)((px - s_gx0) / s_scale);
    int y = (int)((py - s_gy0) / s_scale);
    if (x < 0 || y < 0 || x >= WACKI_SCREEN_W || y >= WACKI_SCREEN_H) return 0;
    *lx = x; *ly = y;
    return 1;
}

static void recompute(int ow, int oh)
{
    s_ow = ow; s_oh = oh;
    const float lw = (float)WACKI_SCREEN_W, lh = (float)WACKI_SCREEN_H;
    float sx = ow / lw, sy = oh / lh;
    s_scale = sx < sy ? sx : sy;
    s_gw = (int)(lw * s_scale); s_gh = (int)(lh * s_scale);
    s_gx0 = (ow - s_gw) / 2;    s_gy0 = (oh - s_gh) / 2;

    int left_bar  = s_gx0;
    int right_bar = ow - (s_gx0 + s_gw);
    s_controls_on = (left_bar >= BAR_MIN_PX && right_bar >= BAR_MIN_PX);

    /* left bar → joystick */
    int sr = (int)((left_bar * STICK_BAR_FRAC < oh * STICK_H_FRAC)
                   ? left_bar * STICK_BAR_FRAC : oh * STICK_H_FRAC);
    s_stick_r  = sr;
    s_stick_cx = left_bar / 2;
    s_stick_cy = (int)(oh * STICK_CY_FRAC);

    /* right bar → LMB (big) + RMB (small, above it) */
    int rcx = s_gx0 + s_gw + right_bar / 2;
    int lr  = (int)((right_bar * LMB_BAR_FRAC < oh * LMB_H_FRAC)
                    ? right_bar * LMB_BAR_FRAC : oh * LMB_H_FRAC);
    s_lmb_r  = lr;
    s_lmb_cx = rcx;
    s_lmb_cy = (int)(oh * LMB_CY_FRAC);
    s_rmb_r  = (int)(lr * RMB_FRAC);
    s_rmb_cx = rcx;
    s_rmb_cy = s_lmb_cy - s_lmb_r - s_rmb_r - (int)(oh * 0.03f);

    s_have_geom = 1;
}

static void stick_set(int px, int py)
{
    if (s_stick_r <= 0) return;
    float dx = (float)(px - s_stick_cx) / s_stick_r;
    float dy = (float)(py - s_stick_cy) / s_stick_r;
    float m  = sqrtf(dx * dx + dy * dy);
    if (m > 1.0f) { dx /= m; dy /= m; }     /* clamp to the rim */
    s_def_x = dx; s_def_y = dy;
}

/* ---- drawing ----------------------------------------------------- */
static void fill_circle(SDL_Renderer *ren, int cx, int cy, int r,
                        Uint8 R, Uint8 G, Uint8 B, Uint8 A)
{
    if (r <= 0) return;
    SDL_SetRenderDrawColor(ren, R, G, B, A);
    for (int dy = -r; dy <= r; ++dy) {
        int dx = (int)floor(sqrt((double)r * r - (double)dy * dy));
        SDL_RenderDrawLine(ren, cx - dx, cy + dy, cx + dx, cy + dy);
    }
}

void wacki_overlay_draw(SDL_Renderer *ren)
{
    if (!ren) return;
    int ow = 0, oh = 0;
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    if (ow <= 0 || oh <= 0) return;
    recompute(ow, oh);
    if (!s_controls_on) return;

    /* Draw in output-pixel space: drop the logical-size letterbox mapping so we
     * can paint into the bars (outside the 640×480 viewport), then restore. */
    int lw = 0, lh = 0;
    SDL_RenderGetLogicalSize(ren, &lw, &lh);
    SDL_RenderSetLogicalSize(ren, 0, 0);
    SDL_BlendMode prev_bm;
    SDL_GetRenderDrawBlendMode(ren, &prev_bm);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    /* Save the draw colour — fill_circle changes it to white, and the next
     * frame's SDL_RenderClear paints the bars with whatever colour is left set
     * (leaving white → solid white bars). */
    Uint8 pr, pg, pb, pa;
    SDL_GetRenderDrawColor(ren, &pr, &pg, &pb, &pa);

    /* joystick: base + knob at current deflection */
    fill_circle(ren, s_stick_cx, s_stick_cy, s_stick_r, 255, 255, 255, A_STICK_BASE);
    int kx = s_stick_cx + (int)(s_def_x * s_stick_r);
    int ky = s_stick_cy + (int)(s_def_y * s_stick_r);
    fill_circle(ren, kx, ky, (int)(s_stick_r * KNOB_FRAC), 255, 255, 255, A_STICK_KNOB);

    /* click buttons: big LMB, smaller RMB */
    fill_circle(ren, s_lmb_cx, s_lmb_cy, s_lmb_r, 255, 255, 255, A_LMB);
    fill_circle(ren, s_rmb_cx, s_rmb_cy, s_rmb_r, 255, 255, 255, A_RMB);

    SDL_SetRenderDrawColor(ren, pr, pg, pb, pa);
    SDL_SetRenderDrawBlendMode(ren, prev_bm);
    SDL_RenderSetLogicalSize(ren, lw, lh);
}

/* ---- input ------------------------------------------------------- */
void wacki_overlay_finger_down(SDL_FingerID id, float nx, float ny)
{
    if (!s_have_geom) return;
    int px = (int)(nx * s_ow), py = (int)(ny * s_oh);
    Finger *f = finger_get(id, 1);
    if (!f) return;

    if (s_controls_on) {
        if (in_circle(px, py, s_stick_cx, s_stick_cy, s_stick_r)) {
            f->role = ROLE_STICK;
            if (!s_stick_on) { s_cur_x = g_mouse_x; s_cur_y = g_mouse_y; }
            s_stick_on = 1;
            stick_set(px, py);
            return;
        }
        if (in_circle(px, py, s_lmb_cx, s_lmb_cy, s_lmb_r)) {
            f->role = ROLE_LMB; g_lmb_clicked = 1; return;
        }
        if (in_circle(px, py, s_rmb_cx, s_rmb_cy, s_rmb_r)) {
            f->role = ROLE_RMB; g_rmb_clicked = 1; return;
        }
    }

    /* Game canvas: move the cursor under the finger (drag to aim), arm a tap. */
    int lx, ly;
    if (win_to_logical(px, py, &lx, &ly)) {
        f->role = ROLE_GAME; f->t0 = SDL_GetTicks();
        f->sx = px; f->sy = py; f->moved = 0;
        g_mouse_x = (int16_t)lx; g_mouse_y = (int16_t)ly;
    } else {
        f->role = ROLE_NONE;   /* empty bar space → swallow, do nothing */
    }
}

void wacki_overlay_finger_motion(SDL_FingerID id, float nx, float ny)
{
    if (!s_have_geom) return;
    Finger *f = finger_get(id, 0);
    if (!f) return;
    int px = (int)(nx * s_ow), py = (int)(ny * s_oh);

    if (f->role == ROLE_STICK) {
        stick_set(px, py);
    } else if (f->role == ROLE_GAME) {
        int dx = px - f->sx, dy = py - f->sy;
        if (dx * dx + dy * dy > TAP_SLOP_PX * TAP_SLOP_PX) f->moved = 1;
        int lx, ly;
        if (win_to_logical(px, py, &lx, &ly)) {
            g_mouse_x = (int16_t)lx; g_mouse_y = (int16_t)ly;
        }
    }
}

void wacki_overlay_finger_up(SDL_FingerID id, float nx, float ny)
{
    (void)nx; (void)ny;
    Finger *f = finger_get(id, 0);
    if (f) {
        if (f->role == ROLE_STICK) {
            s_stick_on = 0; s_def_x = s_def_y = 0.0f;
        } else if (f->role == ROLE_GAME) {
            if (!f->moved && (SDL_GetTicks() - f->t0) <= TAP_MS) g_lmb_clicked = 1;
        }
    }
    finger_release(id);
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
