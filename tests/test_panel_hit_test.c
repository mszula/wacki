/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * tests/test_panel_hit_test.c — verb-panel hit-test (production PanelHitTest).
 *
 * PanelHitTest maps mouse coords to a panel verb. The HUD verb-bar at the
 * bottom of the screen has 6 button cells at panel-local (300,20),
 * (345,20), (390,20), (435,20), (480,20), (525,20); each cell is 40x40
 * pixels. When the mouse falls inside a cell, g_hover_panel_verb is set
 * to g_panel_verb_tab[i]. Outside any cell — or when the panel is hidden
 * — g_hover_panel_verb resets to 0x26 (the "neutral verb" sentinel).
 *
 * These tests cover:
 *   - default "no hit" sentinel
 *   - panel-hidden short-circuit (komnata-flags bit 0 clear)
 *   - null AnimAsset / null draw-offset arrays
 *   - mouse above panel top
 *   - direct hits on each of 6 cells → matching verb
 *   - boundary conditions (exact pixel edges)
 */

#include "test.h"
#include "wacki.h"
#include "test_engine_stubs.h"

#include <stdint.h>
#include <string.h>

extern void PanelHitTest(void);

extern AnimAsset *g_panel_asset;
extern uint16_t   g_komnata_flags;
extern uint16_t   g_hover_panel_verb;
extern uint16_t   g_panel_verb_tab[6];
extern uint16_t   g_held_item;
extern int16_t    g_mouse_x;
extern int16_t    g_mouse_y;

/* Panel base position — we put it at (0, 0) screen-space so that
 * panel-local coords == screen coords (simplifies arithmetic in tests).
 * Real game uses panel base from the asset header; testing the math
 * matters more than testing a specific layout. */
static uint16_t s_drawX[1] = { 0 };
static uint16_t s_drawY[1] = { 0 };

static AnimAsset s_panel = {
    .frame_count = 1,
    .off_drawX   = s_drawX,
    .off_drawY   = s_drawY,
};

static void reset_panel(int16_t panel_x, int16_t panel_y)
{
    s_drawX[0] = (uint16_t)panel_x;
    s_drawY[0] = (uint16_t)panel_y;
    g_panel_asset = &s_panel;
    g_komnata_flags = 1;             /* bit 0 = panel visible */
    g_hover_panel_verb = 0x26;
    g_held_item = 0x26;

    /* Pre-populate the verb table with distinct values so we can
     * tell which slot was hit. */
    g_panel_verb_tab[0] = 0x01;             /* eg "give" */
    g_panel_verb_tab[1] = 0x02;             /* "pick up" */
    g_panel_verb_tab[2] = 0x03;             /* "use" */
    g_panel_verb_tab[3] = 0x04;             /* "open" */
    g_panel_verb_tab[4] = 0x05;             /* "look at" */
    g_panel_verb_tab[5] = 0x06;             /* "talk to" */
}

/* ---- default no-hit ------------------------------------------------- */

TEST(panel_default_neutral_when_mouse_far_below)
{
    reset_panel(/*x=*/0, /*y=*/0);
    /* Mouse well inside panel rect vertically (y=100) but far left
     * (x=10 — first button starts at panel-local 300). */
    g_mouse_x = 10;
    g_mouse_y = 100;
    PanelHitTest();
    ASSERT_EQ(g_hover_panel_verb, 0x26);
}

/* ---- panel-hidden short-circuit ------------------------------------- */

TEST(panel_hidden_returns_sentinel_even_on_button)
{
    /* Mouse lands on button 0 (panel-local 300..339, 20..59) but
     * the panel-visible bit is clear → must return 0x26 sentinel,
     * never reach the button table. */
    reset_panel(0, 0);
    g_komnata_flags = 0;             /* bit 0 clear */
    g_mouse_x = 320;
    g_mouse_y = 40;
    PanelHitTest();
    ASSERT_EQ(g_hover_panel_verb, 0x26);
}

TEST(panel_komnata_flags_other_bits_set_but_bit0_clear)
{
    /* bit 0 must be set; other bits alone don't enable the panel. */
    reset_panel(0, 0);
    g_komnata_flags = 0xFFFE;        /* every bit BUT 0 */
    g_mouse_x = 320;
    g_mouse_y = 40;
    PanelHitTest();
    ASSERT_EQ(g_hover_panel_verb, 0x26);
}

/* ---- null asset / null offset arrays -------------------------------- */

TEST(panel_null_asset_safely_returns)
{
    reset_panel(0, 0);
    g_panel_asset = NULL;
    g_mouse_x = 320;
    g_mouse_y = 40;
    PanelHitTest();                         /* must not crash */
    ASSERT_EQ(g_hover_panel_verb, 0x26);
}

TEST(panel_null_off_drawX_safely_returns)
{
    reset_panel(0, 0);
    s_panel.off_drawX = NULL;               /* missing X table */
    g_mouse_x = 320;
    g_mouse_y = 40;
    PanelHitTest();
    ASSERT_EQ(g_hover_panel_verb, 0x26);
    s_panel.off_drawX = s_drawX;            /* restore */
}

TEST(panel_null_off_drawY_safely_returns)
{
    reset_panel(0, 0);
    s_panel.off_drawY = NULL;
    g_mouse_x = 320;
    g_mouse_y = 40;
    PanelHitTest();
    ASSERT_EQ(g_hover_panel_verb, 0x26);
    s_panel.off_drawY = s_drawY;
}

/* ---- mouse above panel ---------------------------------------------- */

TEST(panel_mouse_at_panel_top_returns_sentinel)
{
    /* Production check is `panel_y >= mouse_y` (>= so equal Y bails). */
    reset_panel(0, 50);
    g_mouse_x = 320;
    g_mouse_y = 50;                         /* exactly at panel top */
    PanelHitTest();
    ASSERT_EQ(g_hover_panel_verb, 0x26);
}

TEST(panel_mouse_above_panel_returns_sentinel)
{
    reset_panel(0, 50);
    g_mouse_x = 320;
    g_mouse_y = 10;                         /* above panel */
    PanelHitTest();
    ASSERT_EQ(g_hover_panel_verb, 0x26);
}

/* ---- direct hits on each of 6 buttons ------------------------------- */

TEST(panel_button_0_hit_returns_verb_tab_0)
{
    /* Button 0: panel-local x in (300, 340), y in (20, 60). Open
     * intervals — production uses strict `<` on both edges. */
    reset_panel(0, 0);
    g_mouse_x = 320;                        /* in (300, 340) */
    g_mouse_y = 40;                         /* in (20, 60) */
    PanelHitTest();
    ASSERT_EQ(g_hover_panel_verb, 0x01);
}

TEST(panel_button_1_hit)
{
    reset_panel(0, 0);
    g_mouse_x = 365;                        /* in (345, 385) */
    g_mouse_y = 40;
    PanelHitTest();
    ASSERT_EQ(g_hover_panel_verb, 0x02);
}

TEST(panel_button_2_hit)
{
    reset_panel(0, 0);
    g_mouse_x = 410;                        /* in (390, 430) */
    g_mouse_y = 40;
    PanelHitTest();
    ASSERT_EQ(g_hover_panel_verb, 0x03);
}

TEST(panel_button_3_hit)
{
    reset_panel(0, 0);
    g_mouse_x = 455;                        /* in (435, 475) */
    g_mouse_y = 40;
    PanelHitTest();
    ASSERT_EQ(g_hover_panel_verb, 0x04);
}

TEST(panel_button_4_hit)
{
    reset_panel(0, 0);
    g_mouse_x = 500;                        /* in (480, 520) */
    g_mouse_y = 40;
    PanelHitTest();
    ASSERT_EQ(g_hover_panel_verb, 0x05);
}

TEST(panel_button_5_hit)
{
    reset_panel(0, 0);
    g_mouse_x = 545;                        /* in (525, 565) */
    g_mouse_y = 40;
    PanelHitTest();
    ASSERT_EQ(g_hover_panel_verb, 0x06);
}

/* ---- boundary tests — strict open intervals ------------------------- */

TEST(panel_button_left_edge_excluded)
{
    /* Production: `btn_x[i] < local_x` — strict <, so exactly at
     * left edge (x=300) is NOT a hit. */
    reset_panel(0, 0);
    g_mouse_x = 300;
    g_mouse_y = 40;
    PanelHitTest();
    ASSERT_EQ(g_hover_panel_verb, 0x26);
}

TEST(panel_button_right_edge_excluded)
{
    /* Same on the right: `local_x < btn_x[i] + 0x28` means x=340 (=300+0x28)
     * is NOT a hit. */
    reset_panel(0, 0);
    g_mouse_x = 340;
    g_mouse_y = 40;
    PanelHitTest();
    ASSERT_EQ(g_hover_panel_verb, 0x26);
}

TEST(panel_button_top_edge_excluded)
{
    reset_panel(0, 0);
    g_mouse_x = 320;
    g_mouse_y = 20;                         /* exactly at btn_y */
    PanelHitTest();
    ASSERT_EQ(g_hover_panel_verb, 0x26);
}

TEST(panel_button_bottom_edge_excluded)
{
    reset_panel(0, 0);
    g_mouse_x = 320;
    g_mouse_y = 60;                         /* = 20 + 0x28 */
    PanelHitTest();
    ASSERT_EQ(g_hover_panel_verb, 0x26);
}

TEST(panel_just_inside_top_left_corner_hits)
{
    /* (301, 21) is the smallest "strictly inside" pixel for button 0. */
    reset_panel(0, 0);
    g_mouse_x = 301;
    g_mouse_y = 21;
    PanelHitTest();
    ASSERT_EQ(g_hover_panel_verb, 0x01);
}

TEST(panel_just_inside_bottom_right_corner_hits)
{
    /* (339, 59) — last pixel before the strict `<` bound trips. */
    reset_panel(0, 0);
    g_mouse_x = 339;
    g_mouse_y = 59;
    PanelHitTest();
    ASSERT_EQ(g_hover_panel_verb, 0x01);
}

/* ---- gaps between buttons miss -------------------------------------- */

TEST(panel_gap_between_button_0_and_1_misses)
{
    /* Button 0 ends at 340 (exclusive), button 1 starts at 345 (exclusive).
     * x in [340..345] is gap. */
    reset_panel(0, 0);
    g_mouse_x = 343;
    g_mouse_y = 40;
    PanelHitTest();
    ASSERT_EQ(g_hover_panel_verb, 0x26);
}

/* ---- non-zero panel base offsets ------------------------------------ */

TEST(panel_with_non_zero_origin_translates_correctly)
{
    /* Place panel at (200, 100). Button 0 is now screen-space
     * (500..540, 120..160). */
    reset_panel(/*panel_x=*/200, /*panel_y=*/100);
    g_mouse_x = 520;                        /* = 200 + 320 (button 0 local) */
    g_mouse_y = 140;                        /* = 100 + 40 */
    PanelHitTest();
    ASSERT_EQ(g_hover_panel_verb, 0x01);
}

TEST(panel_with_non_zero_origin_button_5)
{
    reset_panel(200, 100);
    g_mouse_x = 745;                        /* = 200 + 545 */
    g_mouse_y = 140;
    PanelHitTest();
    ASSERT_EQ(g_hover_panel_verb, 0x06);
}

/* ---- verb-tab content propagates ------------------------------------ */

TEST(panel_verb_tab_change_reflects_in_hover_verb)
{
    /* Re-populating g_panel_verb_tab between PanelHitTest calls must
     * change the result (table is read by reference, not cached). */
    reset_panel(0, 0);
    g_panel_verb_tab[0] = 0x42;
    g_mouse_x = 320;
    g_mouse_y = 40;
    PanelHitTest();
    ASSERT_EQ(g_hover_panel_verb, 0x42);

    g_panel_verb_tab[0] = 0x99;
    PanelHitTest();
    ASSERT_EQ(g_hover_panel_verb, 0x99);
}

/* ---- always resets at function entry -------------------------------- */

TEST(panel_resets_hover_verb_on_each_call)
{
    /* Even if the previous call left a non-sentinel value in
     * g_hover_panel_verb, the next call must overwrite the slot
     * (line 517 unconditional `g_hover_panel_verb = 0x26;`). */
    reset_panel(0, 0);
    g_mouse_x = 320;
    g_mouse_y = 40;
    PanelHitTest();
    ASSERT_EQ(g_hover_panel_verb, 0x01);    /* hit verb 1 */

    /* Now move mouse off the panel — verb must reset to sentinel. */
    g_mouse_x = 10;
    g_mouse_y = 40;
    PanelHitTest();
    ASSERT_EQ(g_hover_panel_verb, 0x26);
}

SUITE(panel_hit_test)
{
    RUN_TEST(panel_default_neutral_when_mouse_far_below);
    RUN_TEST(panel_hidden_returns_sentinel_even_on_button);
    RUN_TEST(panel_komnata_flags_other_bits_set_but_bit0_clear);
    RUN_TEST(panel_null_asset_safely_returns);
    RUN_TEST(panel_null_off_drawX_safely_returns);
    RUN_TEST(panel_null_off_drawY_safely_returns);
    RUN_TEST(panel_mouse_at_panel_top_returns_sentinel);
    RUN_TEST(panel_mouse_above_panel_returns_sentinel);
    RUN_TEST(panel_button_0_hit_returns_verb_tab_0);
    RUN_TEST(panel_button_1_hit);
    RUN_TEST(panel_button_2_hit);
    RUN_TEST(panel_button_3_hit);
    RUN_TEST(panel_button_4_hit);
    RUN_TEST(panel_button_5_hit);
    RUN_TEST(panel_button_left_edge_excluded);
    RUN_TEST(panel_button_right_edge_excluded);
    RUN_TEST(panel_button_top_edge_excluded);
    RUN_TEST(panel_button_bottom_edge_excluded);
    RUN_TEST(panel_just_inside_top_left_corner_hits);
    RUN_TEST(panel_just_inside_bottom_right_corner_hits);
    RUN_TEST(panel_gap_between_button_0_and_1_misses);
    RUN_TEST(panel_with_non_zero_origin_translates_correctly);
    RUN_TEST(panel_with_non_zero_origin_button_5);
    RUN_TEST(panel_verb_tab_change_reflects_in_hover_verb);
    RUN_TEST(panel_resets_hover_verb_on_each_call);
}
