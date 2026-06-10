/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform_portmaster.c — PortMaster (Anbernic & friends) gamepad glue.
 *
 * On these handhelds standard SDL2 exposes the controls as a real
 * SDL_GameController (PortMaster's launcher feeds the per-device button
 * map via SDL_GAMECONTROLLERCONFIG). This module owns the controller
 * handle and maps it onto the engine's existing software-cursor + click
 * model — the PortMaster counterpart of platform_miyoo.c, which does the
 * equivalent for the Miyoo's keysym-based buttons.
 *
 *   left stick / d-pad   → move the software cursor
 *   A (south)            → left click   (walk / interact)
 *   B (east)             → right click  (HandleSceneInput toggles actor)
 *   START                → pause menu
 *   L1 / R1              → quickload / quicksave
 *
 * platform_sdl.c calls platform_pad_open() once at init, routes
 * SDL_CONTROLLER* events through platform_pad_handle_event(), and folds
 * platform_pad_read_motion() into its per-frame virtual-cursor poll.
 * Linked only for TARGET=portmaster (see Makefile). */

#include "wacki.h"
#include "wacki/log.h"

#include <SDL.h>

#include <stdint.h>

#ifdef WACKI_PS2
/* SDL2-PS2's joystick backend only padRead()s the pad — it never puts the
 * DualShock into analog (DUALSHOCK) mode, so padRead returns no stick bytes
 * and the SDL axes read 0. Request analog mode ourselves via libpad once the
 * pad is stable. (-lpadx, already linked.) */
#include <libpad.h>
static int s_ps2_analog_set = 0;
static void ps2_pad_ensure_analog(void)
{
    if (s_ps2_analog_set) return;
    if (padGetState(0, 0) != PAD_STATE_STABLE) return;
    if (padSetMainMode(0, 0, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK) == 1)
        s_ps2_analog_set = 1;            /* takes a few frames to apply */
}
#endif

extern uint8_t g_lmb_clicked;
extern uint8_t g_rmb_clicked;
extern uint8_t g_quicksave_request;
extern uint8_t g_quickload_request;
extern uint8_t g_pause_menu_request;

/* Analog-stick cursor: past the deadzone, full deflection moves
 * PAD_ANALOG_MAX_PX per tick, scaled linearly by how far the stick is
 * pushed. The caller carries the sub-pixel remainder so gentle pushes
 * still creep the cursor (fine aiming on a point-and-click). */
#define PAD_ANALOG_MAX_PX     9
#define PAD_ANALOG_DEADZONE   8000   /* of 32767 */

/* First opened controller. NULL until a pad shows up. */
static SDL_GameController *s_pad = NULL;

/* Init the controller subsystem (separately, so a backend without it
 * stays non-fatal) and adopt the first available pad. */
void platform_pad_open(void)
{
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        LOG_INFO("platform", "no game-controller subsystem: %s",
                 SDL_GetError());
        return;
    }
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i) &&
            (s_pad = SDL_GameControllerOpen(i)) != NULL) {
            LOG_INFO("platform", "game controller: %s",
                     SDL_GameControllerName(s_pad));
            return;
        }
    }
}

/* Handle one SDL_CONTROLLER* event. Returns 1 if consumed. Button layout
 * matches the Miyoo mapping so muscle memory carries across handhelds. */
int platform_pad_handle_event(const SDL_Event *ev)
{
    switch (ev->type) {
    case SDL_CONTROLLERBUTTONDOWN:
        switch (ev->cbutton.button) {
        case SDL_CONTROLLER_BUTTON_A:             g_lmb_clicked        = 1; break;
        case SDL_CONTROLLER_BUTTON_B:             g_rmb_clicked        = 1; break;
        case SDL_CONTROLLER_BUTTON_START:         g_pause_menu_request = 1; break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  g_quickload_request  = 1; break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: g_quicksave_request  = 1; break;
        default: return 0;
        }
        return 1;

    case SDL_CONTROLLERDEVICEADDED:
        /* Hot-plug: adopt a pad if we don't have one yet. */
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

/* Fold the pad into the caller's per-frame cursor poll: the d-pad adds to
 * the discrete dx/dy (sharing the keyboard's accel ramp) and the left
 * stick sets the proportional ax/ay (px/tick). No-op without a pad. */
void platform_pad_read_motion(int *dx, int *dy, float *ax, float *ay)
{
    if (!s_pad) return;
#ifdef WACKI_PS2
    ps2_pad_ensure_analog();
#endif

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
}
