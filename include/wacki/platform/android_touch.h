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
 * Direct tapping on the game canvas still works (drag to aim, lift to click).
 * On Android we own ALL touch (SDL touch→mouse synthesis is disabled), so the
 * canvas tap path lives here too — it needs the letterbox geometry the draw
 * step caches anyway.
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

#endif /* WACKI_PLATFORM_ANDROID_TOUCH_H */
