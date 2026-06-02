/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* src/menu/chapter_select.c — sel_tlo.pic chapter-select UI.
 *
 * Shown when game_over_code == 3 (player finished a stage and needs to
 * pick the next chapter to play). Four stage buttons (id=0x12..0x15,
 * frames 0..3 def / 5..8 hover) plus one ACME-complete green button
 * (id=0x16, frames 4 def / 9 hover) that appears only when all four
 * stages are done.
 *
 * Completed stages are made non-clickable by patching their button id
 * to SCENE_NEUTRAL_VERB and their frames to FRAME_NONE. The ACME
 * button is hidden by downgrading button_count from ALL (5) to
 * PARTIAL (4) until completion.
 *
 * SelTloClick stores the picked stage in `s_chapter_pick` (1..4 = stage
 * 1..4, 5 = Monter finale) and returns SEL_TLO_RC_PICK_MADE so
 * RunMenuScene exits; the caller (RunGameStageLoop game_over branch /
 * RunMainGameLoop dev-flow) reads s_chapter_pick and dispatches the
 * actual LoadStage call. */

#include "wacki.h"
#include "wacki/log.h"

#include <stdint.h>
#include <stdio.h>

int s_chapter_pick = 0;              /* 1..5 = picked stage, 0 = none */

/* ---- constants ---------------------------------------------------- */

/* Trigger ids the sel_guz.wyc mask emits when buttons are clicked.
 * Stages 1..4 are a contiguous 0x12..0x15 range; ACME-complete is 0x16. */
#define SEL_TLO_TRIGGER_STAGE_FIRST   0x12   /* stage 1 */
#define SEL_TLO_TRIGGER_STAGE_LAST    0x15   /* stage 4 */
#define SEL_TLO_TRIGGER_ACME          0x16   /* ACME complete → finale */
#define SEL_TLO_TRIGGER_NEUTRAL       0x26   /* SCENE_NEUTRAL_VERB */

/* Frame layout in sel_guz.wyc: stage button N has def=N, hover=N+5;
 * ACME-complete (button 4) has def=4, hover=9. */
#define SEL_TLO_HOVER_FRAME_OFFSET    5
#define SEL_TLO_FRAME_NONE            0xFFFF

/* RunMenuScene return codes. SelTloClick returns PICK_MADE on a stage
 * pick; the caller reads s_chapter_pick (1..5) to dispatch. */
#define SEL_TLO_RC_KEEP_OPEN          0
#define SEL_TLO_RC_PICK_MADE          3

/* s_chapter_pick values — 1..4 = stage 1..4, 5 = Monter finale. */
#define SEL_TLO_PICK_FINALE_STAGE     5

/* Button slot layout in g_sel_tlo_scene.buttons. */
#define SEL_TLO_STAGE_BUTTON_COUNT    4
#define SEL_TLO_BUTTON_COUNT_PARTIAL  4   /* ACME hidden */
#define SEL_TLO_BUTTON_COUNT_ALL      5   /* ACME exposed */

/* Scene flag bits OR'd into g_sel_tlo_scene.flags. FORCE_CB drives the
 * trigger==0 per-tick path; DISABLE_ESC stops ESC from quitting; KEEP
 * IMAGE preserves the painted background between frames. */
#define SEL_TLO_SCENE_FLAGS           0x34

/* ---- click handler ----------------------------------------------- */

static int SelTloClick(int trigger)
{
    /* trigger==0 is the per-frame callback path enabled by FORCE_CB —
     * the original wires the ACME-assembly animation through it (load
     * Tlo.pal, blit successive sel_guz frames starting at 10). The
     * animation itself isn't ported; completion state is conveyed by
     * each stage button going invisible (def/hover=SEL_TLO_FRAME_NONE)
     * once its bit in g_completed_stages flips. */
    if (trigger == 0) return SEL_TLO_RC_KEEP_OPEN;

    /* Stage buttons 1..4. Only the not-yet-completed ones dispatch;
     * the refresh pass below makes completed slots neutral, but we
     * double-check here for safety against stale state. */
    if (trigger >= SEL_TLO_TRIGGER_STAGE_FIRST &&
        trigger <= SEL_TLO_TRIGGER_STAGE_LAST)
    {
        int idx = trigger - SEL_TLO_TRIGGER_STAGE_FIRST;
        if ((g_completed_stages & (1u << idx)) == 0) {
            s_chapter_pick = idx + 1;
            LOG_TRACE("chapter-select", "picked stage %d", s_chapter_pick);
            return SEL_TLO_RC_PICK_MADE;
        }
        LOG_TRACE("chapter-select", "stage %d already completed — ignore", idx + 1);
        return SEL_TLO_RC_KEEP_OPEN;
    }

    /* ACME-complete (stage 5 Monter finale). Hit-test only fires when
     * SelTloRefreshButtons has promoted button_count to ALL — all four
     * stages done. The finale loads Dane_12.dta intro, Dane_11.dta
     * gameplay, Dane_13.dta end-credits sting. */
    if (trigger == SEL_TLO_TRIGGER_ACME) {
        s_chapter_pick = SEL_TLO_PICK_FINALE_STAGE;
        LOG_TRACE("chapter-select", "ACME complete — start finale (stage 5)");
        return SEL_TLO_RC_PICK_MADE;
    }
    return SEL_TLO_RC_KEEP_OPEN;
}

/* ---- SceneDef + button-state refresh ------------------------------ */

/* Stage buttons 0..3 are patched per-fire from g_completed_stages;
 * button 4 (ACME-complete green button) has the static frames listed
 * below straight from the binary. button_count starts at ALL; the
 * refresh pass downgrades to PARTIAL when not all stages are done. */
SceneDef g_sel_tlo_scene = {
    .background_pic = "sel_tlo.pic",
    /* sel_guz.wyc (NOT sel_tlo.wyc — that filename doesn't exist in
     * the .DTA archive; an earlier port had a typo). Without the
     * correct mask, buttons have no hit-test rectangles → clicks
     * ignored → map appears non-functional. */
    .mask_file      = "sel_guz.wyc",
    .on_click       = SelTloClick,
    .button_count   = SEL_TLO_BUTTON_COUNT_ALL,
    .flags          = SEL_TLO_SCENE_FLAGS,
    .buttons = {
        { SEL_TLO_TRIGGER_STAGE_FIRST + 0, 0,
          SEL_TLO_HOVER_FRAME_OFFSET + 0 },   /* stage 1 — patched per fire */
        { SEL_TLO_TRIGGER_STAGE_FIRST + 1, 1,
          SEL_TLO_HOVER_FRAME_OFFSET + 1 },   /* stage 2 */
        { SEL_TLO_TRIGGER_STAGE_FIRST + 2, 2,
          SEL_TLO_HOVER_FRAME_OFFSET + 2 },   /* stage 3 */
        { SEL_TLO_TRIGGER_STAGE_FIRST + 3, 3,
          SEL_TLO_HOVER_FRAME_OFFSET + 3 },   /* stage 4 */
        { SEL_TLO_TRIGGER_ACME, 4,
          SEL_TLO_HOVER_FRAME_OFFSET + 4 },   /* ACME-complete — static frames */
    },
};

/* Patch SceneDef buttons to reflect current completion state. Called
 * right before RunMenuScene(sel_tlo) so the user sees correct lock/
 * unlock states. Mirrors the original's pre-RunMenuScene button rebuild.
 * Returns 1 if all four stages are completed (button_count promoted to
 * ALL → ACME button live), 0 otherwise. */
int SelTloRefreshButtons(void)
{
    int all_done = 1;
    for (int i = 0; i < SEL_TLO_STAGE_BUTTON_COUNT; ++i) {
        if (g_completed_stages & (1u << i)) {
            /* Completed — neutral / non-clickable. */
            g_sel_tlo_scene.buttons[i].id         = SEL_TLO_TRIGGER_NEUTRAL;
            g_sel_tlo_scene.buttons[i].def_anim   = SEL_TLO_FRAME_NONE;
            g_sel_tlo_scene.buttons[i].hover_anim = SEL_TLO_FRAME_NONE;
        } else {
            /* Available — normal id + frames. */
            g_sel_tlo_scene.buttons[i].id         =
                (uint16_t)(SEL_TLO_TRIGGER_STAGE_FIRST + i);
            g_sel_tlo_scene.buttons[i].def_anim   = (uint16_t)i;
            g_sel_tlo_scene.buttons[i].hover_anim =
                (uint16_t)(i + SEL_TLO_HOVER_FRAME_OFFSET);
            all_done = 0;
        }
    }
    /* ACME-complete (slot 4, id=SEL_TLO_TRIGGER_ACME) is exposed only
     * when all four stages are finished. With count=PARTIAL the 5th
     * slot stays in memory but is never hit-tested, painted, or
     * dispatched — so the green graphic disappears until ACME is
     * complete. */
    g_sel_tlo_scene.button_count = all_done
        ? SEL_TLO_BUTTON_COUNT_ALL
        : SEL_TLO_BUTTON_COUNT_PARTIAL;
    return all_done;
}
