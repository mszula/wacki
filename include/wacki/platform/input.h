/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * include/wacki/platform/input.h — platform input-capability HAL.
 *
 * The raw input/event pump (PlatformPumpEvents, the virtual cursor, the
 * keyboard/mouse/gamepad handlers) is cross-platform and lives in
 * src/platform/sdl/platform_sdl.c — every target drives the cursor through the same path,
 * so it needs no per-platform split. What *does* vary is input *capability*:
 * gameplay code must not assume a real keyboard exists. That query lives here
 * so scene/gameplay code stays platform-agnostic.
 *
 * Implementation: src/platform/sdl/platform_sdl.c (the SDL input layer).
 */
#ifndef WACKI_PLATFORM_INPUT_H
#define WACKI_PLATFORM_INPUT_H

/* Whether the platform has a real, reliable keyboard. Desktop = yes. On the
 * handhelds (Miyoo / PortMaster) every hardware button is mapped by firmware
 * to some keyboard scancode — unpredictably: a volume key aliased onto the
 * SPACE/A scancode toggled the actor on one user's device. On the PS2 there is
 * no keyboard at all (DualShock + USB mouse only). So gameplay keybindings
 * beyond the universal ESC must gate on this — the alternate gesture (the B
 * button → RMB → toggle) covers the same action on those targets. */
int plat_input_has_keyboard(void);

/* A platform-specific hardware-button keydown (handhelds map buttons to
 * firmware keysyms). Returns 1 if it fired a click/menu latch, 0 otherwise.
 * Real only on the Miyoo (its keysym button map); a no-op (0) elsewhere. */
int plat_handle_platform_key(int sym);

/* Per-frame platform extras folded into the gamepad cursor read: on PS2 the
 * USB HID mouse delta is added to (*ax,*ay) and its buttons fire clicks, plus
 * the DualShock is kicked into analog mode. A no-op everywhere else. Called
 * from the SDL gamepad read (gamepad_sdl.c). */
void plat_pad_read_extra(float *ax, float *ay);

/* Edge-triggered controller navigation for a simple on-device menu (the PS2
 * boot-time video-mode picker). Writes 1 to *up / *down / *confirm on the press
 * EDGE of D-pad up/down (or the left stick) and X (south button); held buttons
 * fire once. Pumps the controller state itself, so it works before the main
 * event loop is running. Returns 1 when a controller is connected, 0 when none
 * — the caller then proceeds with a default instead of soft-locking. Generic
 * (any pad target); implemented in gamepad_sdl.c. */
int plat_pad_menu_nav(int *up, int *down, int *confirm);

/* Discard any input queued during a pre-game modal (the PS2 video-mode picker)
 * and clear the click latches. Without this the button that confirmed the modal
 * (X) stays in the SDL event queue and the game's first pump reads it as a click
 * — which skips the (click-skippable) intro cutscene straight to the menu.
 * Implemented in gamepad_sdl.c. */
void plat_input_flush(void);

#endif /* WACKI_PLATFORM_INPUT_H */
