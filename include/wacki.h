/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * include/wacki.h — umbrella header for the reconstructed Wacki engine.
 *
 * The engine runs on the SDL2 platform layer (src/platform_sdl.c).
 * Function and field names match the RE'd binary.
 *
 * The contents are split across three sibling headers — pull this
 * umbrella in and you get all three:
 *
 *   wacki/types.h           structs, enums, format magic numbers
 *   wacki/api.h             module function declarations
 *   wacki/platform/storage.h storage HAL (save / data-root / file I/O)
 *   wacki/globals.h         extern globals + macro aliases (g_script_vars
 *                           aliases for g_game_over_code /
 *                           g_completed_stages, etc.)
 *
 * Source modules can keep writing `#include "wacki.h"` and need not
 * care about the split. Code that wants a tighter dependency surface
 * (e.g. a header that only needs the type definitions) may include a
 * specific sibling directly. */

#ifndef WACKI_H
#define WACKI_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "wacki/types.h"
#include "wacki/api.h"
#include "wacki/platform/storage.h"
#include "wacki/globals.h"

#endif /* WACKI_H */
