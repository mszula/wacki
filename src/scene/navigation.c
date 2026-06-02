/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* src/scene/navigation.c — komnata (room) transitions from script.
 *
 * Script opcode 0x20 GO_EXIT calls ScriptGoToKomnata to switch the
 * current room. The work itself is delegated to LoadKomnataScene in
 * game.c, which freezes the actor walkers, frees the old BG / FLD
 * assets, runs the new room's enter script, and installs the new
 * BG / FLD / music. Actor entities themselves are preserved across
 * transitions (their atlas slots stay valid via the intern table),
 * so they re-appear in the new room at script-set coordinates without
 * needing to be re-spawned.
 */

#include "wacki.h"
#include "wacki/log.h"

#include <stdint.h>
#include <stdio.h>

/* ScriptGoToKomnata — entry point from op 0x20.
 *
 * T22 phase B — full in-place sync transition. Delegates to
 * LoadKomnataScene (game.c) which:
 * 1. Walker-freezes both g_actor entities (+0x4C/+0x50 = 0, +0x3A
 * bits 0,2 cleared) partial reset.
 * 2. Frees the old BG raw + FLD asset.
 * 3. Calls LoadKomnata(id) (preserves actors via T4, runs new
 * komnata's enter_script).
 * 4. Loads new BG + FLD + music into g_scene_* / g_walk_* globals.
 *
 * play_demo_scene's main loop sees the new state via globals and
 * continues running without unwinding. Verb-script post-op-0x20
 * SET_ENTITY_XY executes against persistent actor entities in the
 * NEW komnata's coordinate system. */
extern void LoadKomnataScene(uint16_t id);
void ScriptGoToKomnata(uint16_t id)
{
    if (id == 0) return;
    LOG_TRACE("script", "go-to-komnata %u (sync via LoadKomnataScene)", id);
    LoadKomnataScene(id);
}
