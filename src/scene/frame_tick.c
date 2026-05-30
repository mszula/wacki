/* src/scene/frame_tick.c — per-frame game tick.
 *
 * Two-level dispatch:
 *
 * ProcessGameFrameTickInner does the actual per-frame work:
 * refresh frame deltas, run the per-entity VM (EntityWalkerTick),
 * paint the scene + HUD + cursor, advance the dialog state, etc.
 *
 * ProcessGameFrameTick wraps Inner with the queued-click drain
 * (FlushQueuedClicks) and the platform quit-event poll. Splitting
 * the inner body lets blocking wait pumps (e.g. WAIT_MS) call the
 * inner work without re-entering the quit-event loop.
 */

#include "wacki.h"
#include <SDL.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern const char *g_current_scene;
extern const void *g_scene_bg_raw;
extern uint32_t    g_scene_bg_size;

extern void  PaintHudOverlay(void);
extern void  PaintCursor(void);
extern void  UpdateCursorState(void);
extern void  TickSpeechBalloon(void);
extern void  FlushQueuedClicks(void);
extern int   PlatformShouldQuit(void);
extern int   paint_rawb_pic(const void *blob, uint32_t size, int as_overlay);
extern void  ScreenshotToBmpAutoIncrement(void);
extern void  ScreenshotToPcxAutoIncrement(void);
extern void  HandleSceneInput(void);

/* ProcessGameFrameTickInner — the per-frame "shared body" of the game
 * tick: per-entity VM, panel hit-test, click hover, walker update,
 * deferred-click flush, speech balloon, entity render, frame-delta.
 *
 * Does NOT call FlushFrameToPrimary or check PlatformShouldQuit — those
 * are the wrapper's responsibility (ProcessGameFrameTick below).
 *
 * Called by:
 * - ProcessGameFrameTick (blocking-wait pumps + RunGameStageLoop outer)
 * - play_demo_scene main loop (T2 refactor — replaces the inlined
 * EntityWalkerTick + FlushQueuedClicks + EntityRenderAll + frame delta
 * block, so the scene's BG paint happens BEFORE this and overlay
 * paint (panel, inventory, held-item) happens AFTER, with the scene's
 * own FlushFrameToPrimary at the very end of the frame). */
void ProcessGameFrameTickInner(void)
{
    PumpWin32Messages();                /* SDL event pump */

    /* Frame-delta from MM timer update. Done at
 * the TOP of PGFT Inner so that EntityWalkerTick (entity VM +0x3C
 * countdown), UpdateCursorState (cursor anim acc), the held-item
 * ghost interp and the speech-balloon dismiss timer all see fresh
 * deltas for THIS frame.
 *
 * uint32_t now = g_tick_counter;
 * int dt = now - ;
 * if (dt > 0x32) dt = 0x32;
 * g_frame_delta_ticks = dt;
 * = now;
 *
 * The original mmtimer (timeSetEvent @ 0x00403D84 with PUSH 0xa)
 * fires every 10 ms in real time and INC's g_tick_counter. 
 * samples (now - prev) into g_frame_delta_ticks, so g_frame_delta_ticks is in
 * 10 ms TICKS — not raw milliseconds. We mirror that with two deltas:
 * - g_frame_delta_ms : real ms (held-item ghost interp,
 * speech-balloon dismiss timer)
 * - g_frame_delta_ticks : 10 ms ticks ( g_frame_delta_ticks — cursor
 * anim acc, entity VM +0x3C countdown,
 * op 0x14/0x26/0x3D wait loops)
 *
 * Without the tick conversion every animation timer ran ~10× too
 * fast (cursor strobed, prop anims blurred, WAIT_MS finished in 1/10
 * the script-author-intended time). */
    {
        static uint32_t s_last_real_ms   = 0;
        static uint32_t s_tick_carry_ms  = 0;
        uint32_t now_ms = SDL_GetTicks();
        if (s_last_real_ms == 0) s_last_real_ms = now_ms;
        uint32_t dt = now_ms - s_last_real_ms;
        s_last_real_ms = now_ms;
        if (dt > 50) dt = 50;
        g_frame_delta_ms = dt ? dt : 1;
        g_tick_counter  += dt;
        /* Tick accumulator — emits floor((carry + dt) / 10) and keeps
 * the remainder so we don't drift at frame rates that aren't a
 * clean multiple of 10 (e.g. 16 ms emits 2/1/2/1/… ticks =
 * exactly 1.6 ticks/frame average, no integer truncation). */
        s_tick_carry_ms += dt;
        uint32_t ticks   = s_tick_carry_ms / 10;
        s_tick_carry_ms %= 10;
        if (ticks > 50) ticks = 50;             /* match PE clamp at 0x32 */
        g_frame_delta_ticks = (uint16_t)(ticks ? ticks : 1);
    }

    /* debug screenshot keys — SDL keysym is lowercase for letters */
    {
        uint8_t k = g_key_state & 0xFF;
        if (k == 'P' || k == 'p') ScreenshotToPcxAutoIncrement();
        if (k == 'B' || k == 'b') ScreenshotToBmpAutoIncrement();
    }

    /* T-trail: repaint scene background as the first thing per tick — 
 * with head where `RestorePrevFrameRects(uVar12)` runs
 * (gated on scene_quit_flag == 0). Original restored bg pixels under
 * the previous frame's entity dirty rects; we do the simpler thing
 * and repaint the whole BG. This MUST run inside PGFT so blocking-
 * wait pumps (op 0x09 SHOW_TEXT, dialog runner, op 0x10/0x11/0x12
 * walker waits) also clear the prior frame and don't smear sprites. */
    if (g_current_scene) {
        if (g_scene_bg_raw) paint_rawb_pic(g_scene_bg_raw, g_scene_bg_size, 0);
        /* Stub-pic komnaty (e.g. magaz3j.pic = 1×1) get their BG from a
 * kind=2 atlas captured via flag-0x60 one-shot blit. paint_rawb_pic
 * above does nothing for these (just sets palette), so we overlay
 * the saved atlas pixels here. Order: pic-paint first (palette
 * merge), then atlas overlay. For fullscreen-pic komnaty no atlas
 * is set so PaintSceneBgAtlasIfAny is a no-op. */
        PaintSceneBgAtlasIfAny();
    }

    RestorePrevFrameRects();

    /* Panel + cursor-hover hit-tests (read by HandleSceneInput below). */
    PanelHitTest();
    /* Item-name voice-over: hovering an inventory item for ~2 s plays
 * its name WAV via Item.scr's name table.
 * branch inside ProcessGameFrameTick @ 0x004028B4. */
    ItemHoverDwellTick();
    extern int ClickHitTest(int16_t mx, int16_t my, uint16_t *out_verb);
    extern int16_t s_mouse_x, s_mouse_y;
    {
        /* T31 v2: persist the scene hover verb into g_hover_scene_verb
 * (g_hover_scene_verb in the PE). UpdateCursorState reads it to pick
 * cursor state 0/4/5/6/2/7. tail which
 * writes g_hover_scene_verb = matched_verb (or 0x26 on miss). */
        extern uint16_t g_hover_scene_verb;
        uint16_t hover_verb = 0x26;
        (void)ClickHitTest(s_mouse_x, s_mouse_y, &hover_verb);
        g_hover_scene_verb = hover_verb;
    }

    /* T-input-order: HandleSceneInput runs BEFORE the snapshot+
 * UpdateActorMovement+EntityWalkerTick block.
 * where RunGameStageLoop's outer loop processes the click + clears
 * the click flag BEFORE entering ProcessGameFrameTick. Earlier
 * port had HandleSceneInput at the tail of PGFT Inner, which let
 * the snapshot (`g_lmb_handled = g_lmb_clicked`) see g_lmb_clicked
 * = 1 BEFORE HandleSceneInput consumed it → UpdateActorMovement
 * auto-bound walker to mouse pos (= click pos) ON EVERY VERB
 * CLICK, and re-bound on every subsequent blocking-wait pump
 * because g_lmb_clicked stayed 1 across the dispatch. Result was
 * triple walker bind per click + verb script's walk getting
 * clobbered. HandleSceneInput clears g_lmb_clicked at end of its
 * block, so the snapshot below sees 0 and UpdateActorMovement is
 * a no-op for click bind (still does perspective scale update). */
    HandleSceneInput();

    /* per-entity VM ticks — drives NPC animations, glizda crawl,
 * actor scripts bound by op 0x0E etc. */
    extern void EntityWalkerTick(Entity *head);
    extern Entity *g_render_list_head;
    EntityWalkerTick(g_render_list_head);

    /* Waypoint-routing advance — when path-finder bound walker to an
 * intermediate band (single-hop port shortcut for ), the
 * pending real target is set on s_wp_pending_*. Once walker drains
 * (+0x4C/+0x50 == 0), re-bind to pending target so actor continues
 * the second leg of the route. */
    extern void PerActorWaypointAdvanceTick(void);
    PerActorWaypointAdvanceTick();

    /* Snapshot click flag for UpdateActorMovement
 * RunGameStageLoop @ 0x0040C037 `g_lmb_handled = g_panel_cursor_redirect2`.
 * After T-input-order, g_lmb_clicked is normally 0 here (cleared
 * by HandleSceneInput above). Only set on the rare case where
 * HandleSceneInput was skipped (e.g., no g_current_scene). */
    g_lmb_handled = g_lmb_clicked;

    /* UpdateActorMovement target = current mouse position. Per-frame
 * perspective scale update is the main consumer now (the auto-
 * walker-bind path is dormant in normal use thanks to T-input-
 * order — HandleSceneInput cleared g_lmb_clicked before this). */
    UpdateActorMovement(s_mouse_x, s_mouse_y);
    g_lmb_handled = 0;
    FlushQueuedClicks();                /* drain op 0x22 → DispatchClickEvent */

    /* Speech-balloon timer (kind=1 entity; falls off when dismiss
 * counter expires). wait-loop fall-through. */
    extern void TickSpeechBalloon(void);
    TickSpeechBalloon();

    /* EntityRenderAll ordering inside
 * (right before the final flush). Repaints all
 * registered entities in z-order so blocking-wait loops (op 0x10/
 * 0x11/0x12 actor walks, op 0x26 wait-anim-frame, dialog runner)
 * see live updates. play_demo_scene's main loop has its own paint
 * pass and doesn't call ProcessGameFrameTick, so this only fires
 * during the legitimate "synchronous wait" paths the original
 * also pumps. */
    extern void EntityRenderAll(Entity *head);
    EntityRenderAll(g_render_list_head);

    /* HUD overlay paint (panel + inventory icons + held-item ghost) —
 * sits on top of scene entities. Reads globals only; idempotent. */
    PaintHudOverlay();

    /* T31 v2 — cursor sprite. tail of
 * . UpdateCursorState picks the slot (olowek/kaseta/
 * magnes/drzwi) from hover_scene_verb/hover_panel_verb/held_item,
 * PaintCursor blits the chosen frame at the mouse anchor on top of
 * everything (panel + held-item ghost are painted earlier above). */
    UpdateCursorState();
    PaintCursor();

    /* (debug crosshair removed — Fix #11 resolved the "kosmos" bug; anchor
 * was correct all along, rendering path was doubling hot-spot offset due
 * to wrongly-set flag 0x04 in BindActorWalker.) */

    /* Frame-delta update moved to TOP of PGFT Inner — see comment block
 * there. Doing it here (bottom) timestamped frames one tick behind
 * and meant EntityWalkerTick / UpdateCursorState read stale values. */

}

/* ProcessGameFrameTick — public API. Inner + final flush + quit poll.
 * Used by blocking-wait pumps where there's no caller-managed paint
 * order (just need a complete tick + present to screen). */
void ProcessGameFrameTick(void)
{
    ProcessGameFrameTickInner();
    FlushFrameToPrimary();              /* uploads shadow → SDL_Texture */
    /* graceful shutdown if user closed the window */
    if (PlatformShouldQuit()) {
        g_game_over_code = 99;          /* break outer loops */
    }
}
