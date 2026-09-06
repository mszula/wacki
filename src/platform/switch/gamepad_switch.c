/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform/switch/gamepad_switch.c — Joy-Con / Pro Controller input.
 *
 * Switch uses its own gamepad file instead of the shared gamepad_sdl.c so
 * the Nintendo button layout remapping stays entirely out of shared code.
 *
 * SDL names buttons by PHYSICAL POSITION in the Xbox diamond:
 *   SDL A = south,  SDL B = east,  SDL X = west,  SDL Y = north
 * Nintendo's physical labels at those positions:
 *   south=B, east=A, west=Y, north=X
 *
 * Desired mapping (physical labels):
 *   physical A (east  / SDL B) → left click
 *   physical B (south / SDL A) → right click
 *   physical X (north / SDL Y) → toggle aspect mode
 *   MINUS      (SDL BACK)      → cycle touch mode
 *   PLUS       (SDL START)     → pause menu
 *   L          (SDL LSHOULDER) → quickload
 *   R          (SDL RSHOULDER) → quicksave */

#include "wacki.h"
#include "wacki/log.h"
#include "wacki/platform/input.h"
#include "sdl_internal.h"
#include <SDL.h>

static SDL_GameController *s_pad = NULL;

#define PAD_ANALOG_MAX_PX   9
#define PAD_ANALOG_DEADZONE 8000

void platform_pad_open(void)
{
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        LOG_INFO("platform", "SDL_INIT_GAMECONTROLLER: %s", SDL_GetError());
        return;
    }
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) {
            s_pad = SDL_GameControllerOpen(i);
            if (s_pad) {
                LOG_INFO("platform", "controller: %s",
                         SDL_GameControllerName(s_pad));
                return;
            }
        }
    }
}

int platform_pad_handle_event(const SDL_Event *ev)
{
    switch (ev->type) {
    case SDL_CONTROLLERBUTTONDOWN:
        switch (ev->cbutton.button) {
        /* physical A = east position = SDL B */
        case SDL_CONTROLLER_BUTTON_B:             g_lmb_clicked        = 1; break;
        /* physical B = south position = SDL A */
        case SDL_CONTROLLER_BUTTON_A:             g_rmb_clicked        = 1; break;
        /* physical X = north position = SDL Y → aspect mode */
        case SDL_CONTROLLER_BUTTON_Y:             platform_video_toggle_aspect_mode(); break;
        /* MINUS = SDL BACK → cycle touch mode */
        case SDL_CONTROLLER_BUTTON_BACK:          platform_touch_cycle_mode();         break;
        /* PLUS = SDL START → pause */
        case SDL_CONTROLLER_BUTTON_START:         g_pause_menu_request = 1; break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  g_quickload_request  = 1; break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: g_quicksave_request  = 1; break;
        default: return 0;
        }
        return 1;

    case SDL_CONTROLLERDEVICEADDED:
        if (!s_pad && SDL_IsGameController(ev->cdevice.which))
            s_pad = SDL_GameControllerOpen(ev->cdevice.which);
        return 1;

    case SDL_CONTROLLERDEVICEREMOVED:
        if (s_pad && ev->cdevice.which ==
            SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(s_pad))) {
            SDL_GameControllerClose(s_pad);
            s_pad = NULL;
        }
        return 1;

    default:
        return 0;
    }
}

void platform_pad_read_motion(int *dx, int *dy, float *ax, float *ay)
{
    if (!s_pad) return;
    *dx += SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)
         - SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    *dy += SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN)
         - SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_DPAD_UP);

    int sx = SDL_GameControllerGetAxis(s_pad, SDL_CONTROLLER_AXIS_LEFTX);
    int sy = SDL_GameControllerGetAxis(s_pad, SDL_CONTROLLER_AXIS_LEFTY);
    if (sx > PAD_ANALOG_DEADZONE || sx < -PAD_ANALOG_DEADZONE)
        *ax = (float)sx / 32767.0f * PAD_ANALOG_MAX_PX;
    if (sy > PAD_ANALOG_DEADZONE || sy < -PAD_ANALOG_DEADZONE)
        *ay = (float)sy / 32767.0f * PAD_ANALOG_MAX_PX;

    plat_pad_read_extra(ax, ay);
}

int plat_pad_menu_nav(int *up, int *down, int *confirm)
{
    *up = *down = *confirm = 0;
    if (!s_pad) return 0;
    SDL_GameControllerUpdate();
    int sy = SDL_GameControllerGetAxis(s_pad, SDL_CONTROLLER_AXIS_LEFTY);
    int u = SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_DPAD_UP)   || sy < -PAD_ANALOG_DEADZONE;
    int d = SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) || sy >  PAD_ANALOG_DEADZONE;
    /* physical A (east/SDL B) = confirm */
    int c = SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_B);
    static int pu = 0, pd = 0, pc = 0;
    if (u && !pu) *up = 1;
    if (d && !pd) *down = 1;
    if (c && !pc) *confirm = 1;
    pu = u; pd = d; pc = c;
    return 1;
}

void plat_input_flush(void)
{
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    g_lmb_clicked = 0;
    g_rmb_clicked = 0;
}
