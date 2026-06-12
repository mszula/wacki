/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/scene/hud_paint.c — per-frame HUD overlay paint.
 *
 * Called once per frame from ProcessGameFrameTickInner, after the
 * scene render but before the cursor and back-buffer flush. Composites
 * five things in order:
 *
 *   1. Verb panel (panel.wyc): the bottom-of-screen verb bar.
 *   2. Inventory item icons: one per non-empty panel slot.
 *   3. Actor portraits + active-frame indicator (ebfj.wyc).
 *   4. Fjej glasses-on/off portrait overlay (script-var driven, since
 *      the entity lifetime for the script-spawned glasses sprite is
 *      currently broken).
 *   5. Health bar (pasek#N.wyc) re-painted above the panel.
 *   6. Held-item drag-ghost: the icon trailing the cursor.
 */

#include "wacki.h"
#include <SDL.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern AnimAsset *g_ebfj_atlas;
extern AnimAsset *g_panel_asset;
extern AnimAsset *g_items_atlas;
extern uint16_t   g_panel_verb_tab[6];
extern uint16_t   g_active_actor;
extern uint32_t   g_frame_delta_ms;

/* Forward decls for helpers still owned by game.c. */
extern void paint_anim_button_at(AnimAsset *atlas, uint16_t frame,
                                 int16_t base_x, int16_t base_y, int paint);

/* ---- constants ---------------------------------------------------- */

/* Panel layout. */
#define PANEL_SLOT_COUNT            6
#define PANEL_TOP_Y                 400   /* base Y for paint_anim_button_at */
#define PANEL_BUTTON_SIZE         0x28    /* 40 px cells (used elsewhere) */

/* Inventory verb-id range. */
#define ITEM_VERB_FIRST           0x29
#define NEUTRAL_VERB              0x26

/* Panel visibility bit on g_komnata_flags. */
#define PANEL_VISIBLE_BIT         0x0001

/* Actor portrait atlas — 4 frames: 0/1 active, 2/3 inactive. */
#define ACTOR_PORTRAIT_MIN_FRAMES   4
#define PORTRAIT_INACTIVE_BASE      2     /* OR with (active ^ 1) → 2 or 3 */
#define ACTOR_INDEX_MASK            0x01u

/* Glasses-on/off portrait — driven by a script var. */
#define GLASSES_SCRIPT_VAR_INDEX  0x67
#define GLASSES_TAKEN_VALUE         1
#define GLASSES_DEFAULT_X         172
#define GLASSES_DEFAULT_Y         427

/* Health-bar entity uses asset names starting with "pasek". */
#define HEALTH_BAR_NAME_PREFIX     "pasek"
#define HEALTH_BAR_NAME_PREFIX_LEN  5
#define HEALTH_BAR_DEFAULT_X        7
#define HEALTH_BAR_DEFAULT_Y      403

/* Held-item drag ghost. */
#define HELD_ITEM_GHOST_MISSING   0xFFFF
#define HELD_ITEM_GHOST_X_OFFSET  0x22    /* cursor offset where the icon trails */
#define HELD_ITEM_GHOST_Y_OFFSET     7
#define HELD_ITEM_GHOST_STEP_MAX    64    /* per-frame ghost step cap */

/* Per-slot panel-button origins (top-left), in panel-local coords. */
static const int16_t s_btn_x[PANEL_SLOT_COUNT] = { 300, 345, 390, 435, 480, 525 };
static const int16_t s_btn_y[PANEL_SLOT_COUNT] = {  20,  20,  20,  20,  20,  20 };

/* ---- helpers ------------------------------------------------------- */

static int panel_is_visible(void)
{
    return (g_komnata_flags & PANEL_VISIBLE_BIT) != 0;
}

/* Blit one frame of an atlas at the given screen coords. Skips
 * gracefully on missing tables / zero dimensions. */
static void blit_atlas_frame(AnimAsset *atlas, uint16_t frame,
                             int16_t dx, int16_t dy)
{
    if (!atlas || !atlas->pixel_ptrs ||
        !atlas->off_widths || !atlas->off_heights) return;
    if (frame >= atlas->frame_count) return;

    uint8_t *px = atlas->pixel_ptrs[frame];
    uint16_t fw = atlas->off_widths [frame];
    uint16_t fh = atlas->off_heights[frame];
    if (!px || !fw || !fh) return;

    BlitSpriteToBackbuffer((uint16_t)dx, (uint16_t)dy, 0, 0,
                           fw, fh, fw, fh, px, 0);
}

/* Paint the inventory item icons over the panel slots. */
static void paint_inventory_icons(AnimAsset *panel)
{
    if (!g_items_atlas || !panel_is_visible() ||
        !panel || !panel->off_drawX || !panel->off_drawY) return;

    int16_t panel_x = (int16_t)panel->off_drawX[0];
    int16_t panel_y = (int16_t)panel->off_drawY[0];

    for (int i = 0; i < PANEL_SLOT_COUNT; ++i) {
        uint16_t verb = g_panel_verb_tab[i];
        if (verb == NEUTRAL_VERB) continue;

        uint16_t idx = (uint16_t)(verb - ITEM_VERB_FIRST);
        if (idx >= g_items_atlas->frame_count) continue;

        int16_t dx = (int16_t)(panel_x + s_btn_x[i]);
        int16_t dy = (int16_t)(panel_y + s_btn_y[i]);
        blit_atlas_frame(g_items_atlas, idx, dx, dy);
    }
}

/* Paint the two actor portraits (inactive first so the active one
 * with its frame ends up on top). */
static void paint_actor_portraits(void)
{
    if (!g_ebfj_atlas ||
        g_ebfj_atlas->frame_count < ACTOR_PORTRAIT_MIN_FRAMES ||
        !panel_is_visible()) return;

    unsigned active   = g_active_actor & ACTOR_INDEX_MASK;
    unsigned f_inact  = (active ^ ACTOR_INDEX_MASK) | PORTRAIT_INACTIVE_BASE;
    unsigned f_active = active;

    for (int pass = 0; pass < 2; ++pass) {
        unsigned f = (pass == 0) ? f_inact : f_active;
        uint16_t dx = g_ebfj_atlas->off_drawX ? g_ebfj_atlas->off_drawX[f] : 0;
        uint16_t dy = g_ebfj_atlas->off_drawY ? g_ebfj_atlas->off_drawY[f] : 0;
        blit_atlas_frame(g_ebfj_atlas, (uint16_t)f, (int16_t)dx, (int16_t)dy);
    }
}

/* Paint the glasses-on Fjej portrait overlay when var[0x67] == 1.
 * Cached load on first use so the asset stays resident (~2 KB). */
static void paint_glasses_overlay(void)
{
    static AnimAsset *s_fjbezoku       = NULL;
    static int        s_load_attempted = 0;

    if (!s_load_attempted) {
        s_load_attempted = 1;
        s_fjbezoku = LoadAssetFromDtaBase("fjbezoku.wyc");
    }
    if (!s_fjbezoku) return;
    if (g_script_vars[GLASSES_SCRIPT_VAR_INDEX] != GLASSES_TAKEN_VALUE) return;
    if (s_fjbezoku->frame_count == 0) return;

    uint16_t dx = s_fjbezoku->off_drawX ? s_fjbezoku->off_drawX[0]
                                        : GLASSES_DEFAULT_X;
    uint16_t dy = s_fjbezoku->off_drawY ? s_fjbezoku->off_drawY[0]
                                        : GLASSES_DEFAULT_Y;
    blit_atlas_frame(s_fjbezoku, 0, (int16_t)dx, (int16_t)dy);
}

/* Walk the render list looking for the health-bar entity and re-paint
 * it AFTER the panel (the entity-render pass drew it before the panel
 * overlay, so the panel covered it). */
static void paint_health_bar(void)
{
    if (!panel_is_visible()) return;

    int n = EntityListCount(0);
    for (int i = 0; i < n; ++i) {
        Entity *e = EntityListAt(0, i);
        if (!e) continue;

        AnimAsset *atlas =
            (AnimAsset *)ent_ptr_resolve(EOFF(e, ENT_OFF_ATLAS_SLOT, uint32_t));
        if (!atlas || !atlas->name[0]) continue;
        if (strncmp(atlas->name, HEALTH_BAR_NAME_PREFIX,
                    HEALTH_BAR_NAME_PREFIX_LEN) != 0) continue;

        uint16_t frame = EOFF(e, ENT_OFF_FRAME, uint16_t);
        if (atlas->frame_count == 0) break;
        if (frame >= atlas->frame_count) frame = 0;

        uint16_t dx = atlas->off_drawX ? atlas->off_drawX[frame]
                                       : HEALTH_BAR_DEFAULT_X;
        uint16_t dy = atlas->off_drawY ? atlas->off_drawY[frame]
                                       : HEALTH_BAR_DEFAULT_Y;
        blit_atlas_frame(atlas, frame, (int16_t)dx, (int16_t)dy);
        break;
    }
}

/* Advance the held-item ghost position by one frame-tick toward
 * (target_x, target_y) using a step-cap'd Bresenham. */
static void step_ghost_toward(int16_t *ghost_x, int16_t *ghost_y,
                              int16_t target_x, int16_t target_y)
{
    int steps = (int)g_frame_delta_ms;
    if (steps > HELD_ITEM_GHOST_STEP_MAX) steps = HELD_ITEM_GHOST_STEP_MAX;

    for (int i = 0;
         i < steps && (*ghost_x != target_x || *ghost_y != target_y);
         ++i)
    {
        int dx     = (int)target_x - (int)*ghost_x;
        int dy     = (int)target_y - (int)*ghost_y;
        int abs_dx = dx < 0 ? -dx : dx;
        int abs_dy = dy < 0 ? -dy : dy;
        int stepx  = 0, stepy = 0;

        if (abs_dx >= abs_dy) {
            stepx = (dx > 0) ? 1 : -1;
            if (abs_dy > 0) {
                int r = abs_dy;
                int d = abs_dx ? abs_dx : 1;
                if (((i * r) % d) < (((i + 1) * r) % d)) {
                    stepy = (dy > 0) ? 1 : (dy < 0 ? -1 : 0);
                }
            }
        } else {
            stepy = (dy > 0) ? 1 : -1;
            if (abs_dx > 0) {
                int r = abs_dx;
                int d = abs_dy ? abs_dy : 1;
                if (((i * r) % d) < (((i + 1) * r) % d)) {
                    stepx = (dx > 0) ? 1 : (dx < 0 ? -1 : 0);
                }
            }
        }
        *ghost_x = (int16_t)(*ghost_x + stepx);
        *ghost_y = (int16_t)(*ghost_y + stepy);
    }
}

/* Paint the held-item drag ghost that trails the cursor. */
static void paint_held_item_ghost(void)
{
    static int16_t  s_ghost_x    = 0;
    static int16_t  s_ghost_y    = 0;
    static uint16_t s_ghost_item = HELD_ITEM_GHOST_MISSING;

    if (g_held_item == NEUTRAL_VERB || !g_items_atlas) {
        s_ghost_item = HELD_ITEM_GHOST_MISSING;
        return;
    }
    uint16_t idx = (uint16_t)(g_held_item - ITEM_VERB_FIRST);
    if (idx >= g_items_atlas->frame_count) return;

    int16_t target_x = (int16_t)(g_mouse_x - HELD_ITEM_GHOST_X_OFFSET);
    int16_t target_y = (int16_t)(g_mouse_y - HELD_ITEM_GHOST_Y_OFFSET);

    if (g_held_item != s_ghost_item) {
        s_ghost_x    = target_x;
        s_ghost_y    = target_y;
        s_ghost_item = g_held_item;
    } else if ((s_ghost_x != target_x || s_ghost_y != target_y) &&
               g_frame_delta_ms != 0) {
        step_ghost_toward(&s_ghost_x, &s_ghost_y, target_x, target_y);
    }

    blit_atlas_frame(g_items_atlas, idx, s_ghost_x, s_ghost_y);
}

/* ---- public entry point ------------------------------------------- */

void PaintHudOverlay(void)
{
    AnimAsset *panel = g_panel_asset;
    if (panel) paint_anim_button_at(panel, 0, 0, PANEL_TOP_Y, 1);

    paint_inventory_icons(panel);
    paint_actor_portraits();
    paint_glasses_overlay();
    paint_health_bar();
    paint_held_item_ghost();
}
