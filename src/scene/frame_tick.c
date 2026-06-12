/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/scene/frame_tick.c — per-frame game tick.
 *
 * Two-level dispatch:
 *
 *   ProcessGameFrameTickInner does the actual per-frame work:
 *     refresh frame deltas, run the per-entity VM (EntityWalkerTick),
 *     paint the scene + HUD + cursor, advance the dialog state, etc.
 *
 *   ProcessGameFrameTick wraps Inner with the queued-click drain
 *     (FlushQueuedClicks) and the platform quit-event poll. Splitting
 *     the inner body lets blocking wait pumps (e.g. WAIT_MS) call the
 *     inner work without re-entering the quit-event loop.
 */

#include "wacki.h"
#include <SDL.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern const DemoScene *g_current_scene;
extern const void      *g_scene_bg_raw;
extern uint32_t    g_scene_bg_size;
extern uint16_t    g_hover_scene_verb;

extern void  PaintHudOverlay(void);
extern void  PaintCursor(void);
extern void  UpdateCursorState(void);
extern void  TickSpeechBalloon(void);
extern void  FlushQueuedClicks(void);
extern int   paint_rawb_pic(const void *blob, uint32_t size, int as_overlay);
extern void  ScreenshotToBmpAutoIncrement(void);
extern void  ScreenshotToPcxAutoIncrement(void);
extern void  HandleSceneInput(void);
extern void  EntityWalkerTick(Entity *head);
extern void  PerActorWaypointAdvanceTick(void);
extern void  EntityRenderAll(Entity *head);
extern int   ClickHitTest(int16_t mouse_x, int16_t mouse_y, uint16_t *out_verb);
extern int16_t s_mouse_x, s_mouse_y;
extern Entity *g_render_list_head;

/* ---- constants ---------------------------------------------------- */

/* Frame-delta clamps so a pause / debugger break doesn't blow up the
 * per-entity VM's decrement-by-delta countdowns. */
#define MAX_REAL_MS_PER_FRAME           50
#define MIN_REAL_MS_PER_FRAME            1
#define MAX_TICKS_PER_FRAME             50      /* matches engine clamp */
#define MIN_TICKS_PER_FRAME              1

/* The engine's mmtimer fires every 10 ms; g_frame_delta_ticks counts in
 * those 10 ms units. The tick accumulator emits floor((carry+dt)/10)
 * each frame and carries the remainder to avoid integer drift at frame
 * rates that aren't a clean multiple of 10. */
#define MS_PER_TICK                     10

/* Debug screenshot key codes (matched case-insensitively). */
#define DEBUG_KEY_BMP                  'B'
#define DEBUG_KEY_PCX                  'P'
#define KEY_STATE_LOW_BYTE_MASK         0xFF

/* Scene-hover sentinel verb when no entity is under the cursor. */
#define NEUTRAL_VERB                    0x26

/* g_game_over_code value used to break the outer loops on clean quit. */
#define GAME_OVER_QUIT                  99

/* ---- helpers ------------------------------------------------------- */

/* Refresh the frame-delta globals at the top of each tick. Updates
 * both `g_frame_delta_ms` (real wall-clock) and `g_frame_delta_ticks`
 * (10 ms units, carrying the sub-tick remainder across frames). */
static void refresh_frame_deltas(void)
{
    static uint32_t s_last_real_ms  = 0;
    static uint32_t s_tick_carry_ms = 0;

    uint32_t now_ms = SDL_GetTicks();
    if (s_last_real_ms == 0) s_last_real_ms = now_ms;

    uint32_t dt = now_ms - s_last_real_ms;
    s_last_real_ms = now_ms;
    if (dt > MAX_REAL_MS_PER_FRAME) dt = MAX_REAL_MS_PER_FRAME;
    g_frame_delta_ms = dt ? dt : MIN_REAL_MS_PER_FRAME;
    g_tick_counter  += dt;

    s_tick_carry_ms += dt;
    uint32_t ticks   = s_tick_carry_ms / MS_PER_TICK;
    s_tick_carry_ms %= MS_PER_TICK;
    if (ticks > MAX_TICKS_PER_FRAME) ticks = MAX_TICKS_PER_FRAME;
    g_frame_delta_ticks = (uint16_t)(ticks ? ticks : MIN_TICKS_PER_FRAME);
}

/* Capture debug screenshot if the user pressed B (BMP) or P (PCX). */
static void handle_debug_screenshot_keys(void)
{
    uint8_t k = g_key_state & KEY_STATE_LOW_BYTE_MASK;
    if (k == DEBUG_KEY_PCX || k == (DEBUG_KEY_PCX | 0x20)) ScreenshotToPcxAutoIncrement();
    if (k == DEBUG_KEY_BMP || k == (DEBUG_KEY_BMP | 0x20)) ScreenshotToBmpAutoIncrement();
}

/* Repaint the scene's background from the cached .pic blob, then
 * overlay any one-shot BG atlas captured by an op-0x60 spawn flag. */
static void repaint_scene_background(void)
{
    if (!g_current_scene) return;
    if (g_scene_bg_raw) paint_rawb_pic(g_scene_bg_raw, g_scene_bg_size, 0);
    PaintSceneBgAtlasIfAny();
}

/* Run the per-frame hit-tests that feed g_hover_panel_verb /
 * g_hover_scene_verb (used by HandleSceneInput + UpdateCursorState). */
static void run_per_frame_hit_tests(void)
{
    PanelHitTest();
    ItemHoverDwellTick();

    uint16_t hover_verb = NEUTRAL_VERB;
    (void)ClickHitTest(s_mouse_x, s_mouse_y, &hover_verb);
    g_hover_scene_verb = hover_verb;
}

/* Drive everything that participates in the per-frame VM tick:
 * per-entity scripts, waypoint advance, deferred click drain,
 * speech balloon timer. */
static void run_entity_vm_passes(void)
{
    EntityWalkerTick(g_render_list_head);
    PerActorWaypointAdvanceTick();

    /* g_lmb_handled mirrors g_lmb_clicked for UpdateActorMovement's
     * perspective-scale update. HandleSceneInput will have cleared
     * g_lmb_clicked earlier so this is usually 0; only matters if
     * HandleSceneInput was skipped (e.g. no current scene). */
    g_lmb_handled = g_lmb_clicked;
    UpdateActorMovement(s_mouse_x, s_mouse_y);
    g_lmb_handled = 0;

    FlushQueuedClicks();
    TickSpeechBalloon();
}

/* Composite the frame: render entities, paint HUD, draw cursor. */
static void paint_frame(void)
{
    EntityRenderAll(g_render_list_head);
    PaintHudOverlay();
    UpdateCursorState();
    PaintCursor();
}

/* ---- public API ---------------------------------------------------- */

void ProcessGameFrameTickInner(void)
{
    PumpEvents();
    refresh_frame_deltas();
    handle_debug_screenshot_keys();
    repaint_scene_background();
    RestorePrevFrameRects();
    run_per_frame_hit_tests();

    /* HandleSceneInput runs BEFORE the entity VM passes so it can
     * consume g_lmb_clicked first. The previous ordering (input at
     * tail of the tick) let the snapshot below see the click flag
     * uncon­sumed and rebound the walker on every blocking-wait pump. */
    HandleSceneInput();

    run_entity_vm_passes();
    paint_frame();
}

void ProcessGameFrameTick(void)
{
    ProcessGameFrameTickInner();
    FlushFrameToPrimary();

    if (PlatformShouldQuit()) {
        g_game_over_code = GAME_OVER_QUIT;
    }
}
