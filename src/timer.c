/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/*
 * timer.c — multimedia timer wrapper, portable stub.
 *
 * The original used Win32 winmm: timeGetDevCaps + timeBeginPeriod +
 * timeSetEvent for periodic callbacks. In the SDL build we let SDL drive
 * frame pacing via VSYNC, so this file degrades to a placeholder.
 *
 * The full Win32 implementation is reproduced verbatim in the report's
 * §3.4 in case anyone wants to restore it.
 */
#include "wacki.h"
#include <SDL.h>
#include <string.h>

int InitializeMmTimer(void *self_) { (void)self_; return 1; }

int ArmPeriodicCallback(void *self_, uint32_t period_ms,
                        uint32_t flags, void (*fn)(void))
{
    (void)self_; (void)period_ms; (void)flags; (void)fn;
    return 0;
}

/* Deadline-aware frame pacer. Caps the engine at ~target_ms frame
 * time (1000/target_ms FPS) but never sleeps PAST the deadline — so
 * on a slow handheld where the frame work alone exceeds the budget
 * we don't tack on a full SDL_Delay on top of the over-budget work.
 *
 * The previous pattern (`SDL_Delay(33)` after every frame) added
 * 33 ms unconditionally, which on Cortex-A7 dropped sustained FPS
 * to ~15 even when the engine could have hit ~25. Deadline-aware
 * pacing gives the same 30-FPS cap on fast hosts and is essentially
 * free on slow ones. */
void EnginePaceFrame(uint32_t target_ms)
{
    static uint32_t s_next_deadline = 0;
    uint32_t        now             = SDL_GetTicks();

    if (s_next_deadline == 0) {
        s_next_deadline = now + target_ms;
        return;
    }

    if (now < s_next_deadline) {
        SDL_Delay(s_next_deadline - now);
        now = SDL_GetTicks();
    }
    /* If work overshot the deadline, restart counting from "now" so
     * we don't enter an "owing frames" catch-up state. */
    s_next_deadline = now + target_ms;
}
