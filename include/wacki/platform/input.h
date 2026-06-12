/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * include/wacki/platform/input.h — platform input-capability HAL.
 *
 * The raw input/event pump (PlatformPumpEvents, the virtual cursor, the
 * keyboard/mouse/gamepad handlers) is cross-platform and lives in
 * src/platform_sdl.c — every target drives the cursor through the same path,
 * so it needs no per-platform split. What *does* vary is input *capability*:
 * gameplay code must not assume a real keyboard exists. That query lives here
 * so scene/gameplay code stays platform-agnostic.
 *
 * Implementation: src/platform_sdl.c (the SDL input layer).
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

#endif /* WACKI_PLATFORM_INPUT_H */
