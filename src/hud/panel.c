/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/hud/panel.c — verb-panel cursor hit-test.
 *
 * The HUD verb panel sits across the bottom of the screen with six
 * buttons in its top row. PanelHitTest maps the current mouse position
 * to whichever button it's hovering and publishes the corresponding
 * verb into g_hover_panel_verb. Click → held_item promotion is done
 * by the panel-click router in src/scene/scene_input.c — this module
 * only does the hit-test.
 *
 * Geometry: 6 buttons at panel-local (300, 20), (345, 20), ...,
 * (525, 20). Each cell is 40×40 px. Open-interval comparisons on both
 * edges — pixel-edge coordinates intentionally miss.
 *
 * Visibility gate: g_komnata_flags bit 0 selects whether the
 * panel is shown for the current komnata (cutscene rooms clear it,
 * gameplay rooms set it). */

#include "wacki.h"

#include <stdint.h>

extern AnimAsset *g_panel_asset;
extern uint16_t   g_komnata_flags;

/* ---- constants ---------------------------------------------------- */

#define PANEL_BUTTON_COUNT      6
#define PANEL_BUTTON_SIZE       40              /* px, square */
#define PANEL_NEUTRAL_VERB      0x26
#define PANEL_VISIBLE_BIT       0x0001

/* Panel-local origins of the six verb buttons. All in the top row at
 * Y=20, X-strided by 45 px starting from X=300. Verified from the
 * original engine's button table. */
static const int16_t s_btn_x[PANEL_BUTTON_COUNT] = {
    300, 345, 390, 435, 480, 525,
};
#define PANEL_BUTTON_ROW_Y      20

/* ---- public state ----------------------------------------------- */

uint16_t g_panel_verb_tab[PANEL_BUTTON_COUNT] = {
    PANEL_NEUTRAL_VERB, PANEL_NEUTRAL_VERB, PANEL_NEUTRAL_VERB,
    PANEL_NEUTRAL_VERB, PANEL_NEUTRAL_VERB, PANEL_NEUTRAL_VERB,
};
uint16_t g_hover_panel_verb = PANEL_NEUTRAL_VERB;

/* ---- hit-test ---------------------------------------------------- */

void PanelHitTest(void)
{
    g_hover_panel_verb = PANEL_NEUTRAL_VERB;

    if (!(g_komnata_flags & PANEL_VISIBLE_BIT)) return;
    if (!g_panel_asset ||
        !g_panel_asset->off_drawX ||
        !g_panel_asset->off_drawY) return;

    int16_t panel_x = (int16_t)g_panel_asset->off_drawX[0];
    int16_t panel_y = (int16_t)g_panel_asset->off_drawY[0];
    if (panel_y >= s_mouse_y) return;       /* mouse above panel top */

    int local_x = s_mouse_x - panel_x;
    int local_y = s_mouse_y - panel_y;
    if (local_y <= PANEL_BUTTON_ROW_Y ||
        local_y >= PANEL_BUTTON_ROW_Y + PANEL_BUTTON_SIZE) return;

    for (int i = 0; i < PANEL_BUTTON_COUNT; ++i) {
        if (s_btn_x[i] < local_x && local_x < s_btn_x[i] + PANEL_BUTTON_SIZE) {
            g_hover_panel_verb = g_panel_verb_tab[i];
            return;
        }
    }
}
