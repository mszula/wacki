/* src/hud/panel.c — verb-panel cursor hit-test.
 *
 * The HUD verb panel sits across the bottom of the screen with six
 * buttons in its top row. PanelHitTest maps the current mouse position
 * to whichever button it's hovering and writes the corresponding verb
 * into g_hover_panel_verb. The cursor-redirect handshake then lets the
 * mouse-down handler "promote" a hovered verb into the player's held
 * item (the cursor-as-verb pickup flow).
 *
 * Panel geometry: 6 buttons at panel-local (300, 20), (345, 20), ...,
 * (525, 20). Each cell is 40×40 px (0x28). The check uses STRICT-LESS
 * comparisons (open intervals) on both edges — exact pixel-edge
 * coordinates miss.
 *
 * Visibility gate: `g_settings_anim_active` bit 0 selects whether the
 * panel is shown for the current komnata. Cutscene rooms clear the
 * bit; gameplay rooms set it.
 */

#include "wacki.h"

#include <stdint.h>

extern AnimAsset *g_panel_asset;            /* stage panel.wyc handle */
extern int16_t    s_mouse_x;
extern int16_t    s_mouse_y;
extern uint16_t   g_held_item;

/* Verb panel state — defined here, exposed via wacki.h. */
uint16_t g_panel_verb_tab[6] = {
    0x26, 0x26, 0x26, 0x26, 0x26, 0x26,
};
uint16_t g_hover_panel_verb        = 0x26;
uint8_t  g_panel_cursor_redirect   = 0;
uint8_t  g_panel_cursor_redirect2  = 0;

/* Panel layout. */
#define PANEL_BUTTON_COUNT      6
#define PANEL_BUTTON_SIZE       0x28           /* 40 px square */
#define PANEL_NEUTRAL_VERB      0x26

/* Six panel-local button origins (top-left corners). All six sit in
 * the top row at panel-local Y = 20, X-strided by 45 px starting from
 * X = 300. Verified from the original engine's raw button table. */
static const int16_t s_btn_x[PANEL_BUTTON_COUNT] = {
    300, 345, 390, 435, 480, 525,
};
static const int16_t s_btn_y[PANEL_BUTTON_COUNT] = {
     20,  20,  20,  20,  20,  20,
};

#define PANEL_VISIBLE_BIT       0x0001
extern uint16_t g_settings_anim_active;

void PanelHitTest(void)
{
    g_hover_panel_verb = PANEL_NEUTRAL_VERB;

    if (!(g_settings_anim_active & PANEL_VISIBLE_BIT)) return;
    if (!g_panel_asset ||
        !g_panel_asset->off_drawX ||
        !g_panel_asset->off_drawY) return;

    int16_t panel_x = (int16_t)g_panel_asset->off_drawX[0];
    int16_t panel_y = (int16_t)g_panel_asset->off_drawY[0];
    if (panel_y >= s_mouse_y) return;       /* mouse above panel top */

    int local_x = (int16_t)(s_mouse_x - panel_x);

    for (int i = 0; i < PANEL_BUTTON_COUNT; ++i) {
        if (s_btn_x[i] < local_x && local_x < s_btn_x[i] + PANEL_BUTTON_SIZE) {
            int local_y = (int16_t)(s_mouse_y - panel_y);
            if (s_btn_y[i] < local_y && local_y < s_btn_y[i] + PANEL_BUTTON_SIZE) {
                g_hover_panel_verb = g_panel_verb_tab[i];
                if (g_panel_cursor_redirect) {
                    g_panel_cursor_redirect2 = 0;
                    g_panel_cursor_redirect  = 0;
                    g_held_item              = g_hover_panel_verb;
                }
            }
        }
    }
}
