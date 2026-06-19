/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * include/wacki/globals.h — engine-wide extern globals + aliases.
 *
 * Pulled out of include/wacki.h. The umbrella header still includes
 * us, so call sites continue to write `#include "wacki.h"`.
 *
 * What lives here: every global that crosses TU boundaries — screen
 * buffer, palette, save state, script registers, panel state, frame
 * timers, audio toggles, scene flags, F-key request latches, etc.
 *
 * Function declarations live in include/wacki/api.h; type definitions
 * live in include/wacki/types.h. */

#ifndef WACKI_GLOBALS_H
#define WACKI_GLOBALS_H

#include <stdint.h>
#include "wacki/types.h"

/* ---- screen buffer + palette ------------------------------------- */

extern char      g_data_root[260];
extern uint8_t   g_palette_rgb[256*3];
extern uint16_t  g_screen_w, g_screen_h;
extern uint8_t  *g_back_shadow;          /* 320×240×8bpp paletted shadow */
extern uint16_t  g_screen_w_dim, g_screen_h_dim;

/* ---- script register file + scene state -------------------------- */

extern uint32_t  g_script_vars[0x129];
extern uint32_t  g_entity_state[0x11C];
extern uint16_t  g_active_actor;
extern uint16_t  g_cur_etap;
extern uint16_t  g_cur_komnata;

/* Current room's looping background-music tracks, resolved from the
 * komnata's Wacky.scr room-level [sampl] (see FindKomnataBgMusic). The
 * original LAYERS all of them at once, each looped; the port plays each on
 * its own looping channel. Set by LoadKomnata, consumed by LoadKomnataScene.
 * count==0 = no room music (a room with no room-level [sampl] plays silent,
 * as in the original). */
extern char      g_scene_bg_tracks[KOMNATA_BG_MUSIC_MAX_TRACKS][KOMNATA_BG_MUSIC_NAME_MAX];
extern int       g_scene_bg_track_count;

extern StageDef *g_stage;
extern StageDef *g_stage_table[5];

/* Original PE virtual address of the current stage's per-stage
 * table. Set by LoadStage / play_demo_scene. Read by
 * DispatchClickEvent to walk the per-stage verb_table (+4) and
 * object_table (+8) in PE memory via PeLoaderRead. Stage 1 =
 * 0x00428220, stage 2 = 0x004310A0, etc. — indexed by g_cur_etap.
 * 0 = no stage (DispatchClick noop). */
extern uint32_t g_stage_va;

/* ---- font + held-item -------------------------------------------- */

extern FontHandle *g_default_font;       /* "Futura.30" */

/* Currently-held inventory item id. 0 = nothing held (treat as 0x26
 * = "look at" verb). Set when the user clicks an item in the bottom
 * panel; consumed by DispatchClickEvent as `this_id`. */
extern uint16_t g_held_item;

/* ---- game-over signal aliases ------------------------------------ *
 *
 * g_game_over_code aliases g_script_vars[14] — both names refer to
 * the SAME dword (the original binary kept these at the same PE
 * address; forking them in the port stalls end-of-stage / death /
 * chapter-select because scripts write var[14] but nothing reads a
 * separate int).
 *
 * Scripts trigger transitions via SET_VAR (op 0x0D) on var index 14:
 *   val=1  → death (Dane_14.dta cutscene)
 *   val=3  → chapter-select UI (sel_tlo.pic)
 *   val=4  → stage-end death cutscene, then return to menu
 *
 * g_completed_stages aliases g_script_vars[17] — bitfield, bit i set
 * = stage (i+1) completed. Scripts set it via op 0x0A VAR_OR var[17]
 * imm=1<<stage in each stage's ending bytecode (right before
 * var[14]=3). */
#define g_game_over_code    (*(int      *)&g_script_vars[14])
#define g_completed_stages  (*(uint32_t *)&g_script_vars[17])

/* ---- save + stats ------------------------------------------------ */

extern int           g_save_request;
extern WackiSaveFile g_save;
extern WackiStats    g_stats;

/* ---- input latches ----------------------------------------------- */

extern uint8_t   g_lmb_clicked;
extern uint8_t   g_rmb_clicked;
extern uint8_t   g_lmb_handled;
extern uint16_t  g_key_state;
/* T53 — F5 / F9 latches. PlatformPumpEvents sets these on F5/F9
 * key-down; the play_demo_scene main loop consumes + clears them per
 * frame. */
extern uint8_t   g_quicksave_request;
extern uint8_t   g_quickload_request;
/* T56 — F3 stats dump latch. */
extern uint8_t   g_stats_dump_request;
/* T24 — F12 pause/exit confirmation latch (Pytanie.scr equivalent). */
extern uint8_t   g_pause_menu_request;

/* Virtual cursor position (engine-space 640×480). Driven by the SDL
 * mouse-motion handler + the d-pad/analog virtual cursor; read by the HUD,
 * hit-test, and cursor-paint paths. */
extern int16_t   g_mouse_x;
extern int16_t   g_mouse_y;

/* ---- display / CLI knobs (set by parse_cli_args + config) -------- */

extern int          g_headless;       /* --headless: skip window + present */
extern int          g_no_pacing;      /* --test-cutscenes: skip frame sleeps */
extern uint8_t      g_present_suppressed; /* hold the screen during a komnata load
                                           * (the embedded settling ticks paint new
                                           * entities on the old BG before the new
                                           * one is loaded — don't present that) */
extern int          g_scale_factor;   /* --scale N: window = 640×480 × N */
extern const char  *g_scale_mode;     /* --scaler: nearest|linear|best */
extern int          g_fullscreen;     /* --fullscreen / F11 */

/* ---- audio gates ------------------------------------------------- *
 *
 * Options-menu toggles. When music or the global sound flag flips
 * off mid-play, the music channel is stopped; when flipped back on,
 * the last-requested track resumes. SFX flag just gates new PlaySfx
 * calls. */
extern int g_audio_music_enabled;
extern int g_audio_sfx_enabled;
extern int g_audio_voice_enabled;
extern int g_audio_sound_enabled;
/* T103 — Solund-menu non-audio gates. Set/cleared by SolundClick. */
extern uint8_t g_subtitles_on;       /* gates op 0x09 SHOW_TEXT */
extern uint8_t g_dialogues_on;       /* gates op 0x52/0x53 */

/* ---- per-frame timers -------------------------------------------- */

extern uint32_t g_tick_counter;
/* Per-frame deltas. _ms is real wall-clock ms (held-item ghost
 * interp, speech-balloon dismiss timer); _ticks is 10 ms units
 * (cursor anim, entity VM +0x3C countdown, op 0x14 / op 0x26 / op
 * 0x3D wait loops). */
extern uint32_t g_frame_delta_ms;
extern uint16_t g_frame_delta_ticks;

/* ---- komnata flags + perspective --------------------------------- *
 *
 * Komnata flag bitfield — loaded from the komnata table at scene
 * entry. Low bits gate per-room features (bit 0 = panel visible,
 * bit 1 = actors alive / has perimeter bands); the high byte is
 * shifted by ScriptCallBgMaskSetup as `(flags & 0xff02) << 1`, so
 * the full uint16_t width is load-bearing. */
extern uint16_t  g_komnata_flags;
extern uint16_t  g_cursor_speed;
extern uint16_t  g_perspective_min;
extern uint16_t  g_perspective_step;

/* ---- script objects + atlases ------------------------------------ */

extern void *g_dialogues_obj;
extern void *g_scripts_obj;
extern void *g_items_obj;
extern AnimAsset *g_panel_asset;        /* stage panel.wyc atlas */
extern AnimAsset *g_items_atlas;        /* przedm.wyc inventory icons */
extern Entity    *g_actor[2];

/* ---- entity lists ------------------------------------------------ */

extern Entity *g_render_list_head;
extern Entity *g_click_list_head;

/* ---- panel verb selection ---------------------------------------- *
 *
 * See src/hud/panel.c for the hit-test that publishes hover state. */
extern uint16_t g_panel_verb_tab[6];
extern uint16_t g_hover_panel_verb;
extern uint16_t g_hover_scene_verb;       /* T31 v2 — cursor state */

/* Inventory page rotation. */
extern uint16_t  g_panel_page_idx;
extern uint16_t  g_panel_verb_tab_backup[6];
extern uint8_t   g_panel_redraw;

#endif /* WACKI_GLOBALS_H */
