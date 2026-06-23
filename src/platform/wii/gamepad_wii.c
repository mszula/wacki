/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform/wii/gamepad_wii.c — Wiimote button glue (SDL_Joystick).
 *
 * SDL2-wii exposes the Wiimote as an SDL_Joystick (not SDL_GameController),
 * so the standard gamepad_sdl.c path (which opens SDL_GameController) finds
 * nothing on Wii. This file implements the identical platform_pad_* interface
 * declared in sdl_internal.h, but via SDL_Joystick.
 *
 * IR + A + B are already handled automatically by SDL2-wii's mouse synthesis
 * (IR → SDL_MOUSEMOTION, A → LMB, B → RMB) — this file only cares about the
 * remaining face buttons: +, −, 1, 2.
 *
 * Button index mapping (SDL2-wii, Wiimote held sideways or pointing at TV):
 *   SDL button 0  = WPAD_BUTTON_2   → pause menu
 *   SDL button 1  = WPAD_BUTTON_1   → toggle aspect mode
 *   SDL button 4  = WPAD_BUTTON_MINUS → quickload
 *   SDL button 10 = WPAD_BUTTON_PLUS  → quicksave
 *
 * NOTE: These indices are based on SDL1/libogc documentation and may need
 * adjustment after hardware testing with SDL2-wii. Constants are named so
 * that fixing one number fixes the whole mapping — see WII_BTN_* below.
 *
 * Classic Controller (untested — no hardware available):
 * SDL2-wii may expose the Classic Controller as a separate joystick index
 * or as additional axes/buttons on the same Wiimote joystick. If it appears
 * as joystick index 1, opening it in platform_pad_open() below should be
 * sufficient to forward its events here. The button mapping will almost
 * certainly need adjustment. Marked TODO for future testing. */

#include "wacki.h"
#include "wacki/log.h"
#include "wacki/platform/input.h"
#include "sdl_internal.h"

#include <SDL.h>

/* Wiimote SDL_Joystick button indices — adjust if hardware testing shows
 * different values. Named constants so a single-line fix corrects all uses. */
#define WII_BTN_2     0    /* WPAD_BUTTON_2 — pause menu               */
#define WII_BTN_1     1    /* WPAD_BUTTON_1 — toggle aspect mode        */
#define WII_BTN_MINUS 4    /* WPAD_BUTTON_MINUS — quickload             */
#define WII_BTN_PLUS  10   /* WPAD_BUTTON_PLUS — quicksave              */

static SDL_Joystick *s_wiimote = NULL;

void platform_pad_open(void)
{
    if (SDL_InitSubSystem(SDL_INIT_JOYSTICK) != 0) {
        LOG_INFO("platform", "SDL_INIT_JOYSTICK failed: %s", SDL_GetError());
        return;
    }

    int n = SDL_NumJoysticks();
    LOG_INFO("platform", "%d joystick(s) found", n);

    if (n > 0) {
        s_wiimote = SDL_JoystickOpen(0);
        if (s_wiimote)
            LOG_INFO("platform", "joystick 0: %s", SDL_JoystickName(s_wiimote));
    }

    /* TODO: Classic Controller — if SDL2-wii exposes it as joystick index 1,
     * open it here. Button mapping will need verification on hardware. */
}

int platform_pad_handle_event(const SDL_Event *ev)
{
    if (ev->type != SDL_JOYBUTTONDOWN) return 0;

    switch (ev->jbutton.button) {
    case WII_BTN_2:     g_pause_menu_request = 1; return 1;
    case WII_BTN_1:     platform_video_toggle_aspect_mode(); return 1;
    case WII_BTN_MINUS: g_quickload_request  = 1; return 1;
    case WII_BTN_PLUS:  g_quicksave_request  = 1; return 1;
    default:            return 0;
    }
}

/* Wiimote IR drives the cursor via SDL mouse synthesis — no analog motion
 * to report here. plat_pad_read_extra is also a no-op (no USB mouse on Wii
 * in the same sense as PS2). */
void platform_pad_read_motion(int *dx, int *dy, float *ax, float *ay)
{
    (void)dx; (void)dy; (void)ax; (void)ay;
}

int plat_pad_menu_nav(int *up, int *down, int *confirm)
{
    *up = *down = *confirm = 0;
    if (!s_wiimote) return 0;

    SDL_JoystickUpdate();
    /* D-pad on Wiimote (held pointing at TV): hat 0 */
    SDL_Hat hat = SDL_JoystickGetHat(s_wiimote, 0);
    int u = (hat & SDL_HAT_UP)   != 0;
    int d = (hat & SDL_HAT_DOWN) != 0;
    int c = SDL_JoystickGetButton(s_wiimote, WII_BTN_2); /* 2 confirms */

    static int p_u = 0, p_d = 0, p_c = 0;
    if (u && !p_u) *up = 1;
    if (d && !p_d) *down = 1;
    if (c && !p_c) *confirm = 1;
    p_u = u; p_d = d; p_c = c;
    return 1;
}

void plat_input_flush(void)
{
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    g_lmb_clicked = 0;
    g_rmb_clicked = 0;
}
