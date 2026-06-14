/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform/sdl/gamepad_sdl.c — SDL_GameController → cursor glue.
 *
 * Shared by every pad-driven target — PortMaster (Anbernic & friends), the
 * PS2's DualShock, the Vita — wherever standard SDL2 exposes the controls as
 * a real SDL_GameController. This module owns the controller handle and maps
 * it onto the engine's existing software-cursor + click model (the keysym-
 * button counterpart for the Miyoo lives in miyoo/miyoo.c).
 *
 *   left stick / d-pad   → move the software cursor
 *   A (south)            → left click   (walk / interact)
 *   B (east)             → right click  (HandleSceneInput toggles actor)
 *   START                → pause menu
 *   L1 / R1              → quickload / quicksave
 *
 * platform_sdl.c calls platform_pad_open() once at init, routes
 * SDL_CONTROLLER* events through platform_pad_handle_event(), and folds
 * platform_pad_read_motion() into its per-frame virtual-cursor poll. Linked
 * for the pad targets (see Makefile); not desktop or miyoo. */

#include "wacki.h"
#include "wacki/log.h"
#include "wacki/platform/input.h"   /* plat_pad_read_extra */
#include "sdl_internal.h"           /* platform_pad_* declarations */

#include <SDL.h>

#include <stdint.h>


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
 * stick sets the proportional ax/ay (px/tick). On PS2 a USB mouse adds its
 * relative motion + clicks on top, so it works with or without a pad. */
void platform_pad_read_motion(int *dx, int *dy, float *ax, float *ay)
{
    if (s_pad) {
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

    /* Platform extras folded on top of the SDL pad read — on PS2 the DualShock
     * is kicked into analog mode and the USB HID mouse delta + clicks are
     * added; a no-op elsewhere. */
    plat_pad_read_extra(ax, ay);
}

/* Edge-triggered menu navigation — see wacki/platform/input.h. Drives the PS2
 * boot-time video-mode picker, which runs before the main event loop, so it
 * refreshes the controller state itself (SDL_GameControllerUpdate) instead of
 * relying on the per-frame event pump. */
int plat_pad_menu_nav(int *up, int *down, int *confirm)
{
    *up = *down = *confirm = 0;
    if (!s_pad) return 0;                      /* no pad — caller uses a default */

    SDL_GameControllerUpdate();                /* poll fresh state, no event loop */

    int sy = SDL_GameControllerGetAxis(s_pad, SDL_CONTROLLER_AXIS_LEFTY);
    int u = SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_DPAD_UP)
            || sy < -PAD_ANALOG_DEADZONE;
    int d = SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN)
            || sy >  PAD_ANALOG_DEADZONE;
    int c = SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_A);

    /* Fire on the press edge so a held direction/button advances once. */
    static int p_u = 0, p_d = 0, p_c = 0;
    if (u && !p_u) *up = 1;
    if (d && !p_d) *down = 1;
    if (c && !p_c) *confirm = 1;
    p_u = u; p_d = d; p_c = c;
    return 1;
}

/* Discard input queued during a pre-game modal — see wacki/platform/input.h.
 * The picker polls the pad with SDL_GameControllerUpdate, which also POSTS the
 * button events to the queue; the confirming X would otherwise reach the game's
 * first pump as a click and skip the intro. Drop the whole queue + clear the
 * click latches. */
void plat_input_flush(void)
{
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    g_lmb_clicked = 0;
    g_rmb_clicked = 0;
}
