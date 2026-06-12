/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/scene/scene_input.c — HandleSceneInput + click-dispatch helpers.
 *
 * Pulled out of game.c (was part of the RunGameStageLoop main-loop click
 * block). Drives the per-frame click routing:
 *
 *   - RMB toggles the active actor (Ebek ↔ Fjej).
 *   - LMB below HUD_PANEL_TOP_Y is a scene click — either a verb
 *     dispatch (hover hit) or a free-walk bind.
 *   - LMB at/above HUD_PANEL_TOP_Y is a panel click — verb-bar pickup,
 *     OPCJE button, page nav, HUD-entity dispatch, or empty-slot
 *     diagnostic.
 *
 * The dispatch into the bytecode VM happens via DispatchClickEvent;
 * the verb-script wired to (this, that) decides whether the actor
 * needs to walk first (op 0x10/0x11/0x12 walk-to + blocking wait).
 *
 * Re-entry guard: DispatchClickEvent can fire scripts that hit blocking
 * waits (op 0x09 SHOW_TEXT, op 0x52 DIALOG_BEGIN). Those waits pump
 * ProcessGameFrameTickInner which calls back here. Without the guard
 * the same click re-fires repeatedly. */

#include "wacki.h"
#include "wacki/log.h"

#include <stdint.h>
#include <stdio.h>

/* ---- externs ------------------------------------------------------ */

extern uint8_t  g_dialog_active;
extern const DemoScene *g_current_scene;

extern int  is_walkable_at(int sx, int sy);
extern int  ClickHitTest(int16_t mouse_x, int16_t mouse_y, uint16_t *out_verb);
extern void DispatchClickEvent(uint16_t obj_id, uint16_t verb_id);
extern int  BindActorWalker(int actor_idx, int target_x, int target_y);
extern void OpenOptionsMenu(void);

/* ---- constants ---------------------------------------------------- */

/* HUD layout — the panel sits along the bottom of the screen and
 * carries six panel-bar buttons plus three special hit-regions:
 *   OPCJE          ([230..280) × [430..455))   → opens options menu
 *   page-prev (▲)  ([600..630) × [412..442))   → InventoryPagePrev
 *   page-next (▼)  ([599..629) × [443..473))   → InventoryPageNext
 *
 * The Y boundary between "panel click" and "scene click" is 400. */
#define HUD_PANEL_TOP_Y                400

#define OPCJE_BTN_X0                   230
#define OPCJE_BTN_X1                   280
#define OPCJE_BTN_Y0                   430
#define OPCJE_BTN_Y1                   455

#define PAGE_PREV_BTN_X0               600
#define PAGE_PREV_BTN_X1               630
#define PAGE_PREV_BTN_Y0               412
#define PAGE_PREV_BTN_Y1               442

#define PAGE_NEXT_BTN_X0               599
#define PAGE_NEXT_BTN_X1               629
#define PAGE_NEXT_BTN_Y0               443
#define PAGE_NEXT_BTN_Y1               473

/* Scene-input verb codes. */
#define SCENE_NEUTRAL_VERB             0x26
#define SCENE_USE_ON_ITEM_VERB         0x0F
#define SCENE_PICKUP_TARGET_VAR_IDX    0x0F   /* g_script_vars[0x0F] holds target verb */

/* Free-walk "nearest walkable" search bounds: spiral out by 2 px steps
 * until something within 200 px of the click is walkable. */
#define WALKABLE_SEARCH_MAX_RADIUS     200
#define WALKABLE_SEARCH_STEP_PX        2
#define WALKABLE_SEARCH_DIST_SENTINEL  (1 << 30)

/* Click-dispatch verb ids that also switch the active actor. */
#define ACTOR_VERB_EBEK                1
#define ACTOR_VERB_FJEJ                2

/* ---- helpers ------------------------------------------------------ */

static inline int point_in_rect(int px, int py,
                                int x0, int x1, int y0, int y1)
{
    return px >= x0 && px < x1 && py >= y0 && py < y1;
}

/* Squared distance to the nearest walkable pixel within
 * WALKABLE_SEARCH_MAX_RADIUS of (tx, ty). Writes the best point to
 * (*btx, *bty) when found. Returns 1 on hit, 0 if the entire search
 * region is non-walkable. */
static int find_nearest_walkable(int tx, int ty, int *btx, int *bty)
{
    int best_d = WALKABLE_SEARCH_DIST_SENTINEL;
    *btx = tx;  *bty = ty;
    for (int r = 1;
         r <= WALKABLE_SEARCH_MAX_RADIUS && best_d == WALKABLE_SEARCH_DIST_SENTINEL;
         r += WALKABLE_SEARCH_STEP_PX)
    {
        for (int dy = -r; dy <= r; ++dy) {
            for (int dx = -r; dx <= r; ++dx) {
                if (dx * dx + dy * dy > r * r) continue;
                int cx = tx + dx, cy = ty + dy;
                if (is_walkable_at(cx, cy)) {
                    int d = dx * dx + dy * dy;
                    if (d < best_d) {
                        best_d = d;
                        *btx = cx;
                        *bty = cy;
                    }
                }
            }
        }
    }
    return best_d != WALKABLE_SEARCH_DIST_SENTINEL;
}

/* Dispatch an item-combine click: the held item is being used on the
 * target verb. Original wires this from the same path as world-on-item
 * — both routes set var[0x0F] = target verb and fire the verb-0x0F
 * script with this=held_item. */
static void dispatch_item_combine(uint16_t held, uint16_t target_verb)
{
    g_script_vars[SCENE_PICKUP_TARGET_VAR_IDX] = target_verb;
    LOG_TRACE("panel", "use-on-item: held=0x%04X target=0x%04X "
                    "(var[0x0F]=0x%04X)", held, target_verb, target_verb);
    g_lmb_handled = 0;
    DispatchClickEvent(held, SCENE_USE_ON_ITEM_VERB);
    g_lmb_handled = 0;
}

/* Handle a click on the panel verb-bar (top row of the panel). Picks
 * up a verb into g_held_item, or routes as item-combine if a held
 * item is already set. */
static void handle_panel_verb_click(void)
{
    if (g_dialog_active) {
        LOG_TRACE("dlg-click", "panel verb=0x%04X "
                        "(held=0x%04X) dialog-active", g_hover_panel_verb, g_held_item);
    }
    if (g_held_item != SCENE_NEUTRAL_VERB &&
        g_held_item != g_hover_panel_verb)
    {
        uint16_t held = g_held_item;
        g_held_item   = SCENE_NEUTRAL_VERB;
        dispatch_item_combine(held, g_hover_panel_verb);
    } else {
        g_held_item = g_hover_panel_verb;
        LOG_TRACE("panel", "picked up verb=0x%04X (held_item set)%s", g_held_item, g_dialog_active
                    ? " [dlg-active: should this dispatch instead?]"
                    : "");
    }
}

/* Handle a click on the bottom panel (Y >= HUD_PANEL_TOP_Y). Routes
 * to one of: verb-bar pickup, OPCJE menu, page navigation, HUD entity
 * dispatch, or empty-slot diagnostic. */
static void handle_panel_click(int have_hover, uint16_t hover_verb)
{
    if (g_hover_panel_verb != SCENE_NEUTRAL_VERB) {
        handle_panel_verb_click();
        return;
    }

    if (point_in_rect(g_mouse_x, g_mouse_y,
                      OPCJE_BTN_X0, OPCJE_BTN_X1,
                      OPCJE_BTN_Y0, OPCJE_BTN_Y1))
    {
        LOG_TRACE("opt", "OPCJE clicked at (%d,%d) → opszyns", g_mouse_x, g_mouse_y);
        OpenOptionsMenu();
        return;
    }

    if (point_in_rect(g_mouse_x, g_mouse_y,
                      PAGE_PREV_BTN_X0, PAGE_PREV_BTN_X1,
                      PAGE_PREV_BTN_Y0, PAGE_PREV_BTN_Y1))
    {
        if (InventoryPagePrev()) {
            PanelPageSwap();
            LOG_TRACE("panel", "page-prev → page=%u", g_panel_page_idx);
        }
        return;
    }

    if (point_in_rect(g_mouse_x, g_mouse_y,
                      PAGE_NEXT_BTN_X0, PAGE_NEXT_BTN_X1,
                      PAGE_NEXT_BTN_Y0, PAGE_NEXT_BTN_Y1))
    {
        if (InventoryPageNext()) {
            PanelPageSwap();
            LOG_TRACE("panel", "page-next → page=%u", g_panel_page_idx);
        }
        return;
    }

    if (have_hover && hover_verb != SCENE_NEUTRAL_VERB) {
        uint16_t held = g_held_item;
        g_held_item   = SCENE_NEUTRAL_VERB;
        LOG_TRACE("panel", "HUD verb=0x%04X at (%d,%d) — dispatch", hover_verb, g_mouse_x, g_mouse_y);
        g_lmb_handled = 0;
        DispatchClickEvent(held, hover_verb);
        g_lmb_handled = 0;
        return;
    }

    LOG_TRACE("scene", "panel click at (%d,%d) — empty slot", g_mouse_x, g_mouse_y);
}

/* Switch the active actor based on the dispatched verb id (1 = Ebek,
 * 2 = Fjej). Logs the transition when it actually changes. */
static void maybe_switch_active_actor(uint16_t verb)
{
    if (verb == ACTOR_VERB_EBEK) {
        if (g_active_actor != 0) {
            LOG_TRACE("active", "dispatch → Ebek (was %s)", g_active_actor ? "Fjej" : "Ebek");
        }
        g_active_actor = 0;
    } else if (verb == ACTOR_VERB_FJEJ) {
        if (g_active_actor != 1) {
            LOG_TRACE("active", "dispatch → Fjej (was %s)", g_active_actor ? "Fjej" : "Ebek");
        }
        g_active_actor = 1;
    }
}

/* Dispatch a click that resolved to a scene entity (have_hover &&
 * hover_verb != NEUTRAL). Also handles the use-on-item routing when
 * an item is held + the hover lands on a panel slot. */
static void handle_scene_entity_click(uint16_t hover_verb, int active_actor)
{
    LOG_TRACE("click", "verb=0x%04X at (%d,%d) — dispatch (%s)", hover_verb, g_mouse_x, g_mouse_y, active_actor ? "Fjej" : "Ebek");

    uint16_t this_arg    = g_held_item;
    uint16_t that_arg    = hover_verb;
    int      held_active = (g_held_item != SCENE_NEUTRAL_VERB);
    g_held_item = SCENE_NEUTRAL_VERB;

    if (held_active && g_hover_panel_verb != SCENE_NEUTRAL_VERB) {
        that_arg = SCENE_USE_ON_ITEM_VERB;
        g_script_vars[SCENE_PICKUP_TARGET_VAR_IDX] = g_hover_panel_verb;
        LOG_TRACE("click", "use-on-item: held=0x%04X target=0x%04X "
                        "(var[0x0F]=0x%04X)", this_arg, g_hover_panel_verb, g_hover_panel_verb);
    }

    maybe_switch_active_actor(that_arg);

    int both_neutral = (that_arg == SCENE_NEUTRAL_VERB &&
                        this_arg == SCENE_NEUTRAL_VERB);
    if (!both_neutral) {
        g_lmb_handled = 0;
        DispatchClickEvent(this_arg, that_arg);
        g_lmb_handled = 0;
    }
}

/* Handle a free-walk click (no hover, scene click): bind the active
 * actor's walker to the clicked position (with nearest-walkable
 * fallback if the click landed on non-walkable scenery). */
static void handle_free_walk_click(int active_actor)
{
    int tx = g_mouse_x;
    int ty = g_mouse_y;
    int found_walkable = is_walkable_at(tx, ty);
    if (!found_walkable) {
        int btx, bty;
        if (find_nearest_walkable(tx, ty, &btx, &bty)) {
            tx = btx;
            ty = bty;
            found_walkable = 1;
        }
    }

    if (!found_walkable) {
        LOG_TRACE("scene", "click (%d,%d) unreachable — ignoring", g_mouse_x, g_mouse_y);
        return;
    }
    if (g_actor[active_actor]) {
        LOG_TRACE("scene", "%s walk → (%d,%d)", active_actor ? "Fjej" : "Ebek", tx, ty);
        BindActorWalker(active_actor, tx, ty);
    }
}

/* ---- public API --------------------------------------------------- */

/* HandleSceneInput — RMB toggle + hotspot scan + LMB click dispatch.
 *
 * NO rising-edge debounce on g_lmb_clicked / g_rmb_clicked — SDL_MOUSE
 * BUTTONDOWN fires per-press (not per-hold), so the click flag IS the
 * press event. A previous port had an `s_last_lmb` static which could
 * deadlock: if a walk-loop consumed g_lmb_clicked mid-walk and the user
 * clicked again, the new click would set g_lmb_clicked=1 but the outer
 * HandleSceneInput end set s_last_lmb=1, locking future clicks. */
void HandleSceneInput(void)
{
    static int s_reentry_depth = 0;

    if (!g_current_scene) return;
    if (s_reentry_depth > 0) return;
    ++s_reentry_depth;

    /* RMB → toggle active actor. */
    if (g_rmb_clicked) {
        g_active_actor ^= 1;
        LOG_TRACE("scene", "RMB → active actor = %s", g_active_actor ? "Fjej" : "Ebek");
        g_rmb_clicked = 0;
    }

    /* Panel + scene hit-tests. PGFT Inner already runs these before us
     * in the normal flow, but other callers may not have — idempotent. */
    PanelHitTest();
    uint16_t hover_verb = SCENE_NEUTRAL_VERB;
    int have_hover = ClickHitTest((int16_t)g_mouse_x, (int16_t)g_mouse_y,
                                  &hover_verb);
    g_hover_scene_verb = hover_verb;

    if (g_lmb_clicked) {
        /* CONSUME the click immediately, before any dispatch or walker
         * bind. Without this, blocking-wait pumps inside DispatchClick
         * Event call PGFT Inner which would snapshot g_lmb_handled =
         * g_lmb_clicked (still 1!) and UpdateActorMovement would auto-
         * bind the walker to the current mouse pos, clobbering the
         * verb-script's own walker. */
        g_lmb_clicked = 0;
        g_lmb_handled = 1;

        int active_actor = g_active_actor & 1;

        if (g_mouse_y >= HUD_PANEL_TOP_Y) {
            handle_panel_click(have_hover, hover_verb);
        } else if (have_hover) {
            handle_scene_entity_click(hover_verb, active_actor);
            /* NO auto-walk-to-mouse here. The verb script itself walks
             * the actor (op 0x10/0x11/0x12 walk-to + blocking wait) if
             * it needs to approach the target. */
        } else {
            handle_free_walk_click(active_actor);
        }
    }
    --s_reentry_depth;
}
