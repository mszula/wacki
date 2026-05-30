/* src/hud/cursor.c — mouse cursor state + paint.
 *
 * The cursor is a 16x16 indexed sprite drawn directly into the
 * back-buffer. UpdateCursorState picks which sprite to show based on
 * the current hover verb (panel verb if non-neutral, else scene verb)
 * and the held item; PaintCursor blits the chosen sprite at the
 * mouse position.
 *
 * The held-item ghost (a dimmed copy of the item icon following the
 * cursor when a player has "picked up" a verb) is also drawn here.
 */

#include "wacki.h"
#include <SDL.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* T31 v2 — UpdateCursorState: 1:1 with FUN_004067C0 state determination.
 *
 * Atlas mapping confirmed by user — names match cursor sprites:
 *   - olowek1.wyc (state 0/6) = default arrow cursor (ołówek = pencil)
 *   - kaseta.wyc  (state 1)   = loading anim cursor (cassette spinning)
 *   - magnes1a.wyc (state 2)  = held-item hover indicator
 *   - magnes1.wyc  (state 3/7)= held-item alternate
 *   - drzwi1l.wyc (state 4)   = exit-left cursor (door left)
 *   - drzwi1p.wyc (state 5)   = exit-right cursor (door right)
 *
 * Reads:
 *   g_held_item        — DAT_0044E5E8, 0x26 = no item
 *   g_hover_scene_verb — DAT_0044988C, scene hover (ClickHitTest result)
 *   g_hover_panel_verb — DAT_0044E450, panel hover (PanelHitTest result) */
void UpdateCursorState(void)
{
    extern uint8_t  g_cursor_state;
    extern uint16_t g_cursor_frame, g_cursor_frame_acc;
    extern uint16_t g_hover_panel_verb;        /* DAT_0044E450 */
    extern uint16_t g_hover_scene_verb;        /* DAT_0044988C */
    extern uint16_t g_held_item;
    extern AnimAsset *g_cursor_atlas[8];

    uint8_t prev_state = g_cursor_state;
    int has_item = (g_held_item != 0x26 &&
                    (uint16_t)(g_held_item - 0x29) < 0x8E);
    uint16_t sv = g_hover_scene_verb;
    uint16_t pv = g_hover_panel_verb;

    if (g_cursor_state == 2 || g_cursor_state == 7) {
        /* Held-item context — sticky until item dropped. */
        if (!has_item)                     g_cursor_state = 0;
        else if (sv != 0x26 || pv != 0x26) g_cursor_state = 7;
        else                               g_cursor_state = 2;
    } else if (!has_item) {
        if (sv == 0x26)                            g_cursor_state = 0;
        else if (sv == 7 || sv == 9 || sv == 10)   g_cursor_state = 4;
        else if (sv == 8)                          g_cursor_state = 5;
        else                                       g_cursor_state = 6;
    } else {
        g_cursor_state = 2;
    }

    /* Per-state lookup — 1:1 with DAT_00443400 in the PE. Each state has
     * its own (atlas-slot, step, period) tuple decoded from the 80-byte
     * table (10 bytes/entry: ptr_to_slot, step:i16, period:u16, clamp:
     * u16). Key insight: state 6 reuses atlas SLOT 0 (olowek) but with
     * its own period/step — that's how the "interactive-pencil wiggle"
     * works. State 7 reuses slot 3 (magnes1) for held-item-over-target.
     * The earlier port aliased state 6→0 and 7→3, collapsing those
     * distinct anim profiles and breaking the wiggle.
     *
     * Periods are in 10 ms TICKS — same unit as DAT_0044E578, accumulated
     * via g_frame_delta_ticks. State 6 (interactive pencil) uses period=4
     * → ~40 ms/frame ≈ 25 fps wiggle; states 1..5,7 use period=10 → ~100
     * ms/frame ≈ 10 fps cycle. Earlier port read these as raw ms which
     * was 10× too fast (strobe). */
    static const uint8_t state_slot         [8] = { 0, 1, 2, 3, 4, 5, 0, 3 };
    static const uint8_t state_period_ticks [8] = { 0, 10, 10, 10, 10, 10, 4, 10 };
    static const int8_t  state_step         [8] = { 0,  1,  1,  1,  1,  1, 1,  1 };

    if (g_cursor_state != prev_state) {
        g_cursor_frame = 0;
        g_cursor_frame_acc = 0;
    }
    uint8_t period = state_period_ticks[g_cursor_state & 7];
    int8_t  step   = state_step        [g_cursor_state & 7];
    if (period && step) {
        g_cursor_frame_acc += g_frame_delta_ticks;
        while (g_cursor_frame_acc >= period) {
            g_cursor_frame_acc -= period;
            AnimAsset *a = g_cursor_atlas[state_slot[g_cursor_state & 7]];
            if (a && a->frame_count) {
                int next = (int)g_cursor_frame + step;
                if (next < 0)                     next = a->frame_count - 1;
                if (next >= (int)a->frame_count)  next = 0;
                g_cursor_frame = (uint16_t)next;
            }
        }
    }
}

/* T31 v2 — PaintCursor: blit cursor sprite at mouse position. 1:1 tail
 * of FUN_004067C0 where cursor entity (DAT_0045147C) gets its draw
 * fields populated from atlas frame data. The atlas-slot is selected
 * via the same state→slot table UpdateCursorState uses (states 6/7
 * reuse slots 0/3). */
void PaintCursor(void)
{
    extern int16_t s_mouse_x, s_mouse_y;
    extern uint8_t g_cursor_state;
    extern uint16_t g_cursor_frame;
    extern AnimAsset *g_cursor_atlas[8];
    /* Mirror of the table in UpdateCursorState. */
    static const uint8_t state_slot[8] = { 0, 1, 2, 3, 4, 5, 0, 3 };

    AnimAsset *a = g_cursor_atlas[state_slot[g_cursor_state & 7]];
    if (!a || !a->frame_count || !a->pixel_ptrs) return;
    uint16_t fi = g_cursor_frame;
    if (fi >= a->frame_count) fi = 0;
    uint8_t *px = a->pixel_ptrs[fi];
    if (!px || !a->off_widths || !a->off_heights) return;
    uint16_t fw = a->off_widths [fi];
    uint16_t fh = a->off_heights[fi];
    int16_t  ox = a->off_drawX ? (int16_t)a->off_drawX[fi] : 0;
    int16_t  oy = a->off_drawY ? (int16_t)a->off_drawY[fi] : 0;
    int16_t dx = (int16_t)(s_mouse_x + ox);
    int16_t dy = (int16_t)(s_mouse_y + oy);
    if (fw == 0 || fh == 0) return;
    BlitSpriteToBackbuffer((uint16_t)dx, (uint16_t)dy, 0, 0,
                           fw, fh, fw, fh, px, 0);
}
