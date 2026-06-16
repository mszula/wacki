/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * include/wacki/platform/android_touch.h — Android on-screen touch overlay.
 *
 * The engine renders a 640×480 (4:3) canvas; a phone held landscape is far
 * wider. Rather than let SDL letterbox the canvas (which makes the leftover bars
 * unreachable on emulators — the touch saturates to the canvas edge), the SDL
 * video HAL skips SDL_RenderSetLogicalSize on Android and this module owns the
 * whole window: the game is drawn into a centred 4:3 rect and the side panels
 * hold semi-transparent controls for players who find tapping imprecise:
 *
 *   - left panel:  a virtual joystick that drives the cursor (precise, analog)
 *   - right panel: a big button = left click (walk / use), a smaller one = right
 *                  click (switch actor)
 *
 * The overlay owns ALL touch (synth is disabled): a touch in the game rect maps
 * window-px → 640×480; a touch in a panel hits a control; empty panel space is
 * ignored. Implementation: src/platform/android/touch_overlay.c. The shared SDL
 * layer calls these under #ifdef __ANDROID__: video_sdl.c sizes + draws,
 * platform_sdl.c feeds SDL_FINGER* events + ticks the stick each frame. */
#ifndef WACKI_PLATFORM_ANDROID_TOUCH_H
#define WACKI_PLATFORM_ANDROID_TOUCH_H

#include <SDL.h>

/* Recompute the full-window layout (game rect + control geometry) from the
 * renderer output size. Call once per frame before draw / game_rect. */
void wacki_overlay_compute_layout(SDL_Renderer *ren);

/* The centred dest rect (window px) the game texture should be copied into.
 * Valid after wacki_overlay_compute_layout. */
SDL_Rect wacki_overlay_game_rect(void);

/* Draw the side-panel controls (called after the game texture, before present).
 * No-op when the panels are too narrow to hold controls. */
void wacki_overlay_draw(SDL_Renderer *ren);

/* SDL_FINGER* routing. Coordinates are SDL's window-normalized 0..1. */
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
