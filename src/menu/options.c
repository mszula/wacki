/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* src/menu/options.c — in-game options menus.
 *
 *   opszyns.pic — top-level options dispatcher (OpszynsClick)
 *   Solund.pic  — music / subtitles / voice / dialogues / extra toggles
 *   Grafika.pic — graphics flags
 *   Pytanie.pic — Y/N quit-confirm (in-game F12 / opszyns→Quit)
 *
 * OpenOptionsMenu is the entry point — invoked from the panel-click
 * router (scene_input.c) when the user clicks the OPCJE region of the
 * HUD. It snapshots the gameplay backbuffer, captures the save-slot
 * thumbnail, then runs the opszyns dispatcher.
 *
 * Settings persistence: ApplySavedSettings hydrates the s_opt_* statics
 * from g_save.settings on boot; persist_audio_opts writes them back on
 * any sub-menu commit.
 *
 * Field mapping between WackiSettings and the s_opt_* statics:
 *   music_on     ↔ s_opt_music     (Solund 0x12)
 *   sound_on     ↔ sfx mixer gate  (no Solund button — see save.c default)
 *   voice_on     ↔ s_opt_voice     (Solund 0x14)
 *   subtitles_on ↔ s_opt_subtitles (Solund 0x13)
 *   dialogues_on ↔ s_opt_dialogues (Solund 0x15)
 *   video_mode   ↔ s_opt_gfx1      (Grafika 0x12) */

#include "wacki.h"
#include "wacki/log.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- common return codes shared with RunMenuScene ----------------- */

/* GAME_OVER_USER_QUIT (2) is defined in game.c's title constants block
 * — referenced from OpszynsClick's Pytanie cascade. */
#define GAME_OVER_USER_QUIT             2

/* PYTANIE_RC_TAK (3) — `Pytanie?` confirm-button return code. Replicated
 * here to keep this module free of game.c constant dependencies. */
#define PYTANIE_RC_TAK                  3

/* ---- module state ------------------------------------------------- */

/* Solund.pic toggle flags (mirror WackiSettings fields). */
static uint8_t s_opt_music     = 1;
static uint8_t s_opt_subtitles = 1;
static uint8_t s_opt_voice     = 1;
static uint8_t s_opt_dialogues = 1;
static uint8_t s_opt_extra     = 0;   /* semantic still under RE */

/* Grafika.pic toggle flags. */
static uint8_t s_opt_gfx1 = 1;        /* video_mode */
static uint8_t s_opt_gfx2 = 1;        /* fast-transitions flag */

/* SceneDefs are defined further down; forward-declared here so the
 * apply / refresh helpers see them. */
static SceneDef g_solund_scene;
static SceneDef g_grafika_scene;
static SceneDef g_pytanie_scene;
static SceneDef g_opszyns_scene;

/* External SceneDefs from sibling modules. */
extern SceneDef g_save_menu_scene;
extern SceneDef g_load_menu_scene;

/* Engine-side hooks. */
extern void AudioSetVoiceEnabled(int on);     /* audio.c */
extern void SnapshotBackbufferForMenu(void);  /* game.c */
extern uint8_t  g_save_thumb_pending[SAVE_THUMB_W * SAVE_THUMB_H];
extern uint8_t *g_back_shadow;
extern int      g_scene_quit;                 /* game.c — Pytanie TAK signal */

/* Gameplay area is 640×400 (HUD panel covers rows 400..480). */
#define GAMEPLAY_AREA_HEIGHT_PX     400

/* ---- ApplySavedSettings + persist_audio_opts ---------------------- */

/* Pull from g_save.settings (loaded by LoadSaveStateOrInitialize) into
 * the in-memory opt flags + the audio mixer state. Called once at boot
 * AFTER LoadSaveStateOrInitialize so a fresh game uses persisted prefs
 * (or save.c defaults if no save file exists). */
void ApplySavedSettings(void)
{
    s_opt_music     = g_save.settings.music_on     ? 1 : 0;
    s_opt_voice     = g_save.settings.voice_on     ? 1 : 0;
    s_opt_subtitles = g_save.settings.subtitles_on ? 1 : 0;
    s_opt_dialogues = g_save.settings.dialogues_on ? 1 : 0;
    s_opt_gfx1      = g_save.settings.video_mode   ? 1 : 0;
    uint8_t sfx_on  = g_save.settings.sound_on     ? 1 : 0;

    AudioSetMusicEnabled(s_opt_music);
    AudioSetSfxEnabled  (sfx_on);
    AudioSetVoiceEnabled(s_opt_voice);
    g_subtitles_on  = s_opt_subtitles;
    g_dialogues_on  = s_opt_dialogues;
    LOG_TRACE("opt", "applied saved settings: music=%d voice=%d "
                    "subs=%d dialogs=%d sfx=%d gfx1=%d", s_opt_music, s_opt_voice, s_opt_subtitles, s_opt_dialogues, sfx_on, s_opt_gfx1);
}

/* Mirror s_opt_* → WackiSettings → Wacki.sav. Called on every toggle
 * inside SolundClick / GrafikaClick so options survive across sessions.
 * `extra` and `sound_on` are NOT persisted — extra has no struct field,
 * sound_on has no in-game toggle so it stays as whatever was loaded. */
static void persist_audio_opts(void)
{
    g_save.settings.music_on     = s_opt_music     ? 1 : 0;
    g_save.settings.voice_on     = s_opt_voice     ? 1 : 0;
    g_save.settings.subtitles_on = s_opt_subtitles ? 1 : 0;
    g_save.settings.dialogues_on = s_opt_dialogues ? 1 : 0;
    g_save.settings.video_mode   = s_opt_gfx1      ? 1 : 0;
    WriteSaveFile();
}

/* ---- Solund.pic constants + helpers ------------------------------- */

#define SOLUND_BTN_MUSIC            0x12
#define SOLUND_BTN_SUBTITLES        0x13
#define SOLUND_BTN_VOICE            0x14
#define SOLUND_BTN_DIALOGUES        0x15
#define SOLUND_BTN_EXTRA            0x16
#define SOLUND_BTN_EXIT             0x17

#define SOLUND_RC_KEEP_OPEN         0
#define SOLUND_RC_EXIT              3

#define SOLUND_SLOT_MUSIC           0
#define SOLUND_SLOT_SUBTITLES       1
#define SOLUND_SLOT_VOICE           2
#define SOLUND_SLOT_DIALOGUES       3
#define SOLUND_SLOT_EXTRA           4
#define SOLUND_SLOT_EXIT            5
#define SOLUND_BUTTON_COUNT         6

/* Solund.wyc atlas: 24 frames in four contiguous bands of 6:
 *   [0..5]    OFF hover    (slot N → frame N + base)
 *   [6..11]   OFF def
 *   [12..17]  ON  hover
 *   [18..23]  ON  def */
#define SOLUND_OFF_HOVER_BASE       0
#define SOLUND_OFF_DEF_BASE         6
#define SOLUND_ON_HOVER_BASE        12
#define SOLUND_ON_DEF_BASE          18

/* Write the def/hover frames for one toggle slot from its on/off state. */
static void apply_solund_toggle_visual(int slot, int is_on)
{
    g_solund_scene.buttons[slot].hover_anim =
        (uint16_t)(slot + (is_on ? SOLUND_ON_HOVER_BASE
                                 : SOLUND_OFF_HOVER_BASE));
    g_solund_scene.buttons[slot].def_anim =
        (uint16_t)(slot + (is_on ? SOLUND_ON_DEF_BASE
                                 : SOLUND_OFF_DEF_BASE));
}

/* Sync all 5 toggle slots' visuals from the current s_opt_* flags. */
static void refresh_solund_toggle_visuals(void)
{
    apply_solund_toggle_visual(SOLUND_SLOT_MUSIC,     s_opt_music);
    apply_solund_toggle_visual(SOLUND_SLOT_SUBTITLES, s_opt_subtitles);
    apply_solund_toggle_visual(SOLUND_SLOT_VOICE,     s_opt_voice);
    apply_solund_toggle_visual(SOLUND_SLOT_DIALOGUES, s_opt_dialogues);
    apply_solund_toggle_visual(SOLUND_SLOT_EXTRA,     s_opt_extra);
}

/* Re-apply every option's effect to its subsystem (used on EXIT to make
 * sure mixer / global gate state matches the flags before persisting). */
static void reassert_audio_option_state(void)
{
    AudioSetMusicEnabled(s_opt_music);
    AudioSetVoiceEnabled(s_opt_voice);
    g_subtitles_on = s_opt_subtitles;
    g_dialogues_on = s_opt_dialogues;
}

static int SolundClick(int trigger)
{
    int toggled = 1;
    switch (trigger) {
    case SOLUND_BTN_MUSIC:
        s_opt_music ^= 1;
        AudioSetMusicEnabled(s_opt_music);
        break;
    case SOLUND_BTN_SUBTITLES:
        s_opt_subtitles ^= 1;
        g_subtitles_on = s_opt_subtitles;
        break;
    case SOLUND_BTN_VOICE:
        s_opt_voice ^= 1;
        AudioSetVoiceEnabled(s_opt_voice);
        break;
    case SOLUND_BTN_DIALOGUES:
        s_opt_dialogues ^= 1;
        g_dialogues_on = s_opt_dialogues;
        break;
    case SOLUND_BTN_EXTRA:
        s_opt_extra ^= 1;
        LOG_TRACE("opt", "extra (fade_color_index) = %d", s_opt_extra);
        break;
    case SOLUND_BTN_EXIT:
        reassert_audio_option_state();
        persist_audio_opts();
        return SOLUND_RC_EXIT;
    default:
        toggled = 0;
        break;     /* idle per-frame call (trigger=-1) */
    }

    refresh_solund_toggle_visuals();

    if (toggled) {
        LOG_TRACE("opt", "music=%d subs=%d voice=%d dialog=%d extra=%d", s_opt_music, s_opt_subtitles, s_opt_voice, s_opt_dialogues, s_opt_extra);
    }
    return SOLUND_RC_KEEP_OPEN;
}

static SceneDef g_solund_scene = {
    .background_pic = "Solund.pic",
    .mask_file      = "Solund.wyc",
    .on_click       = SolundClick,
    .button_count   = SOLUND_BUTTON_COUNT,
    .flags          = SCENE_FLAG_REDRAW,
    .buttons = {
        { SOLUND_BTN_MUSIC,
          SOLUND_OFF_DEF_BASE   + SOLUND_SLOT_MUSIC,
          SOLUND_OFF_HOVER_BASE + SOLUND_SLOT_MUSIC },
        { SOLUND_BTN_SUBTITLES,
          SOLUND_OFF_DEF_BASE   + SOLUND_SLOT_SUBTITLES,
          SOLUND_OFF_HOVER_BASE + SOLUND_SLOT_SUBTITLES },
        { SOLUND_BTN_VOICE,
          SOLUND_OFF_DEF_BASE   + SOLUND_SLOT_VOICE,
          SOLUND_OFF_HOVER_BASE + SOLUND_SLOT_VOICE },
        { SOLUND_BTN_DIALOGUES,
          SOLUND_OFF_DEF_BASE   + SOLUND_SLOT_DIALOGUES,
          SOLUND_OFF_HOVER_BASE + SOLUND_SLOT_DIALOGUES },
        { SOLUND_BTN_EXTRA,
          SOLUND_OFF_DEF_BASE   + SOLUND_SLOT_EXTRA,
          SOLUND_OFF_HOVER_BASE + SOLUND_SLOT_EXTRA },
        { SOLUND_BTN_EXIT,
          SOLUND_OFF_DEF_BASE   + SOLUND_SLOT_EXIT,
          SOLUND_OFF_HOVER_BASE + SOLUND_SLOT_EXIT },
    },
};

/* ---- Grafika.pic constants + handler ------------------------------ */

/* Grafika.wyc has a 12-frame redraw atlas:
 *   button[0] ON  → def=0 hover=6     OFF → def=3 hover=9
 *   button[1] ON  → def=1 hover=7     OFF → def=4 hover=10
 *   button[2] exit (static)           def=8 hover=2 */
#define GRAFIKA_BTN_FLAG1               0x12
#define GRAFIKA_BTN_FLAG2               0x13
#define GRAFIKA_BTN_EXIT                0x14
#define GRAFIKA_RC_KEEP_OPEN            0
#define GRAFIKA_RC_EXIT                 3

#define GRAFIKA_F1_DEF_ON               0
#define GRAFIKA_F1_DEF_OFF              3
#define GRAFIKA_F1_HOVER_ON             6
#define GRAFIKA_F1_HOVER_OFF            9
#define GRAFIKA_F2_DEF_ON               1
#define GRAFIKA_F2_DEF_OFF              4
#define GRAFIKA_F2_HOVER_ON             7
#define GRAFIKA_F2_HOVER_OFF            10
#define GRAFIKA_EXIT_DEF                8
#define GRAFIKA_EXIT_HOVER              2

static int GrafikaClick(int trigger)
{
    int toggled = 1;
    switch (trigger) {
    case GRAFIKA_BTN_FLAG1: s_opt_gfx1 ^= 1; break;
    case GRAFIKA_BTN_FLAG2: s_opt_gfx2 ^= 1; break;
    case GRAFIKA_BTN_EXIT:
        /* Persist video_mode (mapped from s_opt_gfx1) before leaving. */
        persist_audio_opts();
        return GRAFIKA_RC_EXIT;
    default: toggled = 0; break;
    }
    /* The toggle's "siur" indicator is baked into the DEF frame so it
     * stays visible even when the cursor isn't hovering. */
    g_grafika_scene.buttons[0].def_anim   =
        (uint16_t)(s_opt_gfx1 ? GRAFIKA_F1_DEF_ON   : GRAFIKA_F1_DEF_OFF);
    g_grafika_scene.buttons[0].hover_anim =
        (uint16_t)(s_opt_gfx1 ? GRAFIKA_F1_HOVER_ON : GRAFIKA_F1_HOVER_OFF);
    g_grafika_scene.buttons[1].def_anim   =
        (uint16_t)(s_opt_gfx2 ? GRAFIKA_F2_DEF_ON   : GRAFIKA_F2_DEF_OFF);
    g_grafika_scene.buttons[1].hover_anim =
        (uint16_t)(s_opt_gfx2 ? GRAFIKA_F2_HOVER_ON : GRAFIKA_F2_HOVER_OFF);

    g_grafika_scene.flags |= SCENE_FLAG_REDRAW;
    if (toggled) {
        LOG_TRACE("opt-gfx", "flag1=%d flag2=%d", s_opt_gfx1, s_opt_gfx2);
    }
    return GRAFIKA_RC_KEEP_OPEN;
}

static SceneDef g_grafika_scene = {
    .background_pic = "Grafika.pic",
    .mask_file      = "Grafika.wyc",
    .on_click       = GrafikaClick,
    .button_count   = 3,
    .flags          = SCENE_FLAG_MOUSE_ONLY,
    .buttons = {
        { GRAFIKA_BTN_FLAG1, GRAFIKA_F1_DEF_ON, GRAFIKA_F1_HOVER_ON },
        { GRAFIKA_BTN_FLAG2, GRAFIKA_F2_DEF_ON, GRAFIKA_F2_HOVER_ON },
        { GRAFIKA_BTN_EXIT,  GRAFIKA_EXIT_DEF,  GRAFIKA_EXIT_HOVER  },
    },
};

/* ---- Pytanie.pic constants + handler ------------------------------ */

#define PYTANIE_BTN_TAK                 0x12
#define PYTANIE_BTN_NIE                 0x13
#define PYTANIE_RC_NIE                  4
#define PYTANIE_FRAME_NONE              0xFFFF

static int PytanieClick(int trigger)
{
    if (trigger == PYTANIE_BTN_TAK) return PYTANIE_RC_TAK;
    if (trigger == PYTANIE_BTN_NIE) return PYTANIE_RC_NIE;
    return 0;
}

static SceneDef g_pytanie_scene = {
    .background_pic = "Pytanie.pic",
    .mask_file      = "Pytanie.wyc",
    .on_click       = PytanieClick,
    .button_count   = 2,
    .flags          = SCENE_FLAG_MOUSE_ONLY,
    .buttons = {
        { PYTANIE_BTN_TAK, PYTANIE_FRAME_NONE, 0 },
        { PYTANIE_BTN_NIE, PYTANIE_FRAME_NONE, 1 },
    },
};

/* Public access for the F12 pause-menu cascade in play_demo_scene. */
SceneDef *opt_get_pytanie_scene(void) { return &g_pytanie_scene; }

/* ---- opszyns.pic dispatcher --------------------------------------- */

/* Triggers the opszyns.wyc mask emits per button. Polish naming
 * convention: "Sejw" is the SAVE picker, "Load" is the LOAD picker. */
#define OPSZYNS_BTN_GRAFIKA             0x12
#define OPSZYNS_BTN_SOLUND              0x13
#define OPSZYNS_BTN_SAVE                0x14
#define OPSZYNS_BTN_LOAD                0x15
#define OPSZYNS_BTN_QUIT                0x16
#define OPSZYNS_BTN_EXIT                0x17

#define OPSZYNS_RC_KEEP_OPEN            0
#define OPSZYNS_RC_CLOSE                3
#define SUBMENU_RC_COMMITTED            3

#define OPSZYNS_FRAME_NONE              0xFFFF
#define OPSZYNS_BUTTON_COUNT            6

/* Sub-menu helper: run a "commit-propagates-close" child (SAVE / LOAD).
 * Returns CLOSE if the child committed, KEEP_OPEN otherwise. */
static int run_commit_propagating_submenu(SceneDef *scene)
{
    int rc = RunMenuScene(1, scene);
    return rc == SUBMENU_RC_COMMITTED ? OPSZYNS_RC_CLOSE
                                      : OPSZYNS_RC_KEEP_OPEN;
}

static int OpszynsClick(int trigger)
{
    switch (trigger) {
    case OPSZYNS_BTN_GRAFIKA:
        RunMenuScene(1, &g_grafika_scene);
        return OPSZYNS_RC_KEEP_OPEN;

    case OPSZYNS_BTN_SOLUND:
        RunMenuScene(1, &g_solund_scene);
        return OPSZYNS_RC_KEEP_OPEN;

    case OPSZYNS_BTN_SAVE:
        return run_commit_propagating_submenu(&g_save_menu_scene);

    case OPSZYNS_BTN_LOAD:
        return run_commit_propagating_submenu(&g_load_menu_scene);

    case OPSZYNS_BTN_QUIT: {
        int rc = RunMenuScene(1, &g_pytanie_scene);
        if (rc == PYTANIE_RC_TAK) {
            g_game_over_code = GAME_OVER_USER_QUIT;
            LOG_TRACE("opt", "Pytanie: quit confirmed → game_over=%d", GAME_OVER_USER_QUIT);
            return OPSZYNS_RC_CLOSE;
        }
        return OPSZYNS_RC_KEEP_OPEN;
    }

    case OPSZYNS_BTN_EXIT:
        return OPSZYNS_RC_CLOSE;

    default:
        return OPSZYNS_RC_KEEP_OPEN;
    }
}

static SceneDef g_opszyns_scene = {
    .background_pic = "opszyns.pic",
    .mask_file      = "opszyns.wyc",
    .on_click       = OpszynsClick,
    .button_count   = OPSZYNS_BUTTON_COUNT,
    .flags          = SCENE_FLAG_REDRAW | SCENE_FLAG_FADE,
    .buttons = {
        { OPSZYNS_BTN_GRAFIKA, OPSZYNS_FRAME_NONE, 0 },
        { OPSZYNS_BTN_SOLUND,  OPSZYNS_FRAME_NONE, 1 },
        { OPSZYNS_BTN_SAVE,    OPSZYNS_FRAME_NONE, 2 },
        { OPSZYNS_BTN_LOAD,    OPSZYNS_FRAME_NONE, 3 },
        { OPSZYNS_BTN_QUIT,    OPSZYNS_FRAME_NONE, 4 },
        { OPSZYNS_BTN_EXIT,    OPSZYNS_FRAME_NONE, 5 },
    },
};

/* ---- thumbnail capture + entry point ------------------------------ */

/* Downsample g_back_shadow (640×400) → 126×78 indexed pixels into the
 * pending buffer. Nearest-neighbor sampling. Called right before
 * opszyns menu paints, so the captured image is the gameplay scene
 * (not the menu overlay). */
static void CapturePendingThumbnail(void)
{
    if (!g_back_shadow) return;
    const int src_w = WACKI_SCREEN_W;
    for (int y = 0; y < SAVE_THUMB_H; ++y) {
        int sy = (y * GAMEPLAY_AREA_HEIGHT_PX) / SAVE_THUMB_H;
        const uint8_t *src_row = g_back_shadow + (size_t)sy * src_w;
        uint8_t *dst_row = g_save_thumb_pending + (size_t)y * SAVE_THUMB_W;
        for (int x = 0; x < SAVE_THUMB_W; ++x) {
            int sx = (x * src_w) / SAVE_THUMB_W;
            dst_row[x] = src_row[sx];
        }
    }
    /* Also snapshot the full backbuffer for menu BG restore. */
    SnapshotBackbufferForMenu();
}

/* OpenOptionsMenu — invoked on click in the OPCJE region of the HUD
 * panel. The original engine's flag-setter for this is missing from the
 * disassembly, so we wire the panel click directly here. */
void OpenOptionsMenu(void)
{
    CapturePendingThumbnail();
    int rc = RunMenuScene(0, &g_opszyns_scene);
    LOG_TRACE("opt", "opszyns closed rc=%d", rc);
    /* If Pytanie confirmed quit (rc=CLOSE from QUIT case), signal scene-
     * loop break so play_demo_scene returns to the menu. */
    if (rc == OPSZYNS_RC_CLOSE && g_game_over_code == GAME_OVER_USER_QUIT) {
        g_scene_quit = 1;
    }
}
