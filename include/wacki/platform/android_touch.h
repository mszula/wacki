/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * include/wacki/platform/android_touch.h — Android on-screen touch overlay.
 *
 * The engine renders a 640×480 (4:3) canvas; on a wide phone held landscape
 * that letterboxes to big black pillarbox bars left and right. This overlay
 * fills that wasted space with semi-transparent controls for players who find
 * tapping the tiny canvas imprecise:
 *
 *   - left bar:  a virtual joystick that drives the cursor (precise, analog)
 *   - right bar: a big button = left click (walk / use), a smaller one = right
 *                click (switch actor)
 *
 * Direct tapping/dragging on the game canvas still works — those touches go
 * through SDL's built-in touch→mouse synthesis (it maps through the renderer's
 * real present transform, so the cursor lands exactly under the finger on every
 * device). This module only claims the control zones in the bars and, while a
 * finger is on a control, has the SDL layer suppress the synth for that touch
 * (wacki_overlay_owns_touch).
 *
 * Implementation: src/platform/android/touch_overlay.c. The shared SDL layer
 * calls these under #ifdef __ANDROID__: video_sdl.c draws, platform_sdl.c feeds
 * SDL_FINGER* events + ticks the stick each frame. */
#ifndef WACKI_PLATFORM_ANDROID_TOUCH_H
#define WACKI_PLATFORM_ANDROID_TOUCH_H

#include <SDL.h>

/* Draw the overlay (called after the game texture, before present). Also
 * (re)caches the renderer output size + control geometry for the event
 * handlers. No-op visual when the bars are too narrow to hold controls. */
void wacki_overlay_draw(SDL_Renderer *ren);

/* SDL_FINGER* routing. Coordinates are SDL's window-normalized 0..1. Each
 * returns 1 when the touch was handled here (always 1 on Android — we own
 * touch). */
void wacki_overlay_finger_down(SDL_FingerID id, float nx, float ny);
void wacki_overlay_finger_motion(SDL_FingerID id, float nx, float ny);
void wacki_overlay_finger_up(SDL_FingerID id, float nx, float ny);

/* Per-frame: integrate the virtual stick deflection into the cursor. */
void wacki_overlay_tick(void);

/* Non-zero while a finger is on an on-screen control (joystick/buttons). The
 * SDL layer uses this to suppress the synthesized touch-mouse for that touch so
 * a bar press doesn't also drag/click the game cursor. */
int  wacki_overlay_owns_touch(void);

#endif /* WACKI_PLATFORM_ANDROID_TOUCH_H */
