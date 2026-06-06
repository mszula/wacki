/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/menu/main_menu.c — title screen, main menu, RunMainGameLoop.
 *
 * Entry point is RunMainGameLoop, called from main.c. It drives the
 * outermost engine loop:
 *
 *   1. LoadSaveStateOrInitialize → ApplySavedSettings → install title
 *      palette.
 *   2. If --start-stage was set (dev flow), enter the chapter-select
 *      map directly and let the user pick a stage; bail when done.
 *   3. OUTER loop: play the title intro AVI then drive the INNER menu
 *      re-entry loop. INNER loop: show the title menu and dispatch the
 *      click rc — Load / New / Maluch (prologue) / Quit / Credits.
 *
 * The title menu's SceneDef sits on the Tlo.wyc mask with 5 buttons
 * (Load / New / Maluch / Quit / Credits). HandleMainMenuClick handles
 * one-time INIT (load palettes, start BGM), per-frame title-logo
 * flipbook ticks, and the click-to-rc dispatch.
 *
 * Cinematics: play_bomb_explosion (Quit→TAK), play_fiacik_intro +
 * play_loading_screen (Maluch→Prologue), play_film_intro (New→film). */

#include "wacki.h"
#include "wacki/log.h"
#include <SDL.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Win32 keyboard constant used by HasPendingKey / WaitForKey. */
#define VK_ESCAPE                       0x1B

/* ---- Title + main-menu constants ---------------------------------- */

#define TITLE_PALETTE_FILENAME          "Tlo.pal"
#define TITLE_MASK_FILENAME             "Tlo.wyc"
#define TITLE_BLEND_PALETTE_FILENAME    "menu.pal"
#define TITLE_INTRO_AVI                 "Dane_10.dta"
#define CREDITS_AVI                     "Dane_12.dta"
#define MAIN_MENU_BGM_FILENAME          "Dane_01.dta"

/* Title-screen button triggers (Tlo.wyc mask). Frames in the .wyc are
 * laid out so that hover frame = def frame + TITLE_HOVER_FRAME_OFFSET.
 * MALUCH (0x14) is the "click the Maluch to start a new game" button. */
#define TITLE_BTN_LOAD                  0x12
#define TITLE_BTN_NEW                   0x13
#define TITLE_BTN_MALUCH                0x14
#define TITLE_BTN_QUIT                  0x15
#define TITLE_BTN_CREDITS               0x16
#define TITLE_HOVER_FRAME_OFFSET        5
#define TITLE_BUTTON_COUNT              5

/* HandleMainMenuClick trigger sentinel: 0 = INIT (one-time per-frame
 * setup pass when the title is first entered). */
#define MAIN_MENU_TRIGGER_INIT          0

/* HandleMainMenuClick return codes. RunMainGameLoop's switch dispatches
 * on them; BACK_TO_MAIN keeps the inner loop running. */
#define MAIN_MENU_RC_NONE               0
#define MAIN_MENU_RC_BACK_TO_MAIN       2   /* Load cancel → re-enter title */
#define MAIN_MENU_RC_QUIT_CONFIRM_A     4
#define MAIN_MENU_RC_QUIT_CONFIRM_B     8
#define MAIN_MENU_RC_LOAD_SAVE          5
#define MAIN_MENU_RC_NEW_GAME           6
#define MAIN_MENU_RC_PROLOGUE           7
#define MAIN_MENU_RC_CREDITS            9

/* WACKI-logo flipbook in the title's mask atlas. Frames 10..(count-1)
 * form the animated logo; DOORS_FIRST..DOORS_LAST are the "doors
 * closing" pose during which the Maluch-click latch is NOT honoured
 * (let the doors finish before transitioning). */
#define MAIN_MENU_ANIM_FIRST_FRAME      10
#define MAIN_MENU_ANIM_DOORS_FIRST      0x0F
#define MAIN_MENU_ANIM_DOORS_LAST       0x12
#define MAIN_MENU_ANIM_TICKS_PER_FRAME  6

/* s_menu_flags bit 0 — set on every HandleMainMenuClick call and
 * cleared once the rc has switched to a "leave the menu" code. */
#define MAIN_MENU_FLAG_LATCH            0x01u

/* Pytanie (Y/N) quit-confirm — the Pytanie SceneDef HandleMainMenuClick
 * uses for "really quit?" lives here too (a separate, otherwise-identical
 * SceneDef sits in src/menu/options.c for the in-game F12 / opszyns
 * cascade). */
#define PYTANIE_TRIGGER_TAK             0x12
#define PYTANIE_TRIGGER_NIE             0x13
#define PYTANIE_RC_TAK                  3
#define PYTANIE_RC_NIE                  4
#define PYTANIE_FRAME_NONE              0xFFFF

/* Entity-state table layout (FULL_RESET only zeroes the in_inventory_
 * flag field per entry, preserving panel_verb_id). */
#define ENTITY_STATE_ENTRY_COUNT        0x8E
#define ENTITY_STATE_FIELDS_PER_ENTRY   4
#define ENTITY_STATE_INVENTORY_FIELD    1

/* Dev-mode --start-stage clamps (DEV_PICK_FINALE = 5 in wacki.h). */
#define DEV_START_STAGE_MIN             1
#define DEV_START_STAGE_MAX             5

/* play_bomb_explosion / play_fiacik_intro / play_loading_screen timing. */
#define BOMB_FRAME_DELAY_MS             100
#define BOMB_AUDIO_FUSE_FRAME           8     /* start bum.wav from this frame */
#define BOMB_TAIL_DELAY_MS              800
#define FIACIK_FRAME_DELAY_MS           80    /* ~12 fps */
#define FIACIK_TAIL_DELAY_MS            200
#define FIACIK_BUTTONS_TO_PAINT         5     /* def_anim frames 0..4 */
/* play_film_intro — film.wyc is a 20-frame raw projector animation at
 * (260,326), the same slot as the static film icon (Tlo.wyc frame 1), so
 * it animates the "film" button. Sound markers from Wacky.scr [animacja]
 * film.wyc: Proj1.wav (0,15) then Proj2.wav (14,). */
#define FILM_FRAME_DELAY_MS             80
#define FILM_PROJ2_FRAME                14
#define FILM_TAIL_DELAY_MS              200
#define LOADING_SCREEN_HOLD_MS          1500
#define LOADING_SCREEN_FRAME_DELAY_MS   33

/* ---- module state ------------------------------------------------- */

static uint8_t s_menu_flags    = 0;
static int     s_anim_delay    = 0;
static int     s_anim_frame    = MAIN_MENU_ANIM_FIRST_FRAME;
static int     s_save_request  = 0;

/* DEV (--start-stage N or WACKI_START_STAGE=N): if set, RunMainGameLoop
 * skips the intro AVI + main menu and jumps straight to the chapter-
 * select map with stages 1..(N-1) marked completed. 0 = normal flow.
 * Set from main.c CLI parsing. */
int g_dev_start_stage = 0;

/* ---- externs ------------------------------------------------------ */

extern AnimAsset       *g_menu_asset_10;
extern uint8_t         *g_back_shadow;
extern void             LoadSaveStateOrInitialize(void);
extern void             SnapshotBackbufferForMenu(void);
extern void             paint_anim_button_at(AnimAsset *atlas, uint16_t frame,
                                             int16_t override_dx,
                                             int16_t override_dy,
                                             int use_override);
extern int              paint_rawb_pic(const void *blob, uint32_t size,
                                       int as_overlay);
extern SceneDef         g_load_menu_scene;
extern SceneDef         g_sel_tlo_scene;
extern int              s_chapter_pick;
extern int              SelTloRefreshButtons(void);
extern uint16_t         g_selected_save_slot;

/* ---- HandlePytanieClick ------------------------------------------- */

/* on_click for the "Pytanie?" quit-confirmation SceneDef. TAK = quit,
 * NIE = keep playing. */
static int HandlePytanieClick(int trigger)
{
    if (trigger == PYTANIE_TRIGGER_TAK) return PYTANIE_RC_TAK;
    if (trigger == PYTANIE_TRIGGER_NIE) return PYTANIE_RC_NIE;
    return MAIN_MENU_RC_NONE;
}

/* ---- HandleMainMenuClick helpers ---------------------------------- */

/* Install the Tlo.pal primary + load menu.pal into the secondary
 * (blend-target) palette buffer. The original engine cross-fades
 * between the two on entry; the port doesn't have the blend pipeline
 * wired so we just free menu.pal to avoid a leak. */
static void install_main_menu_palettes(void)
{
    void    *pal = NULL;
    uint32_t psz = 0;
    if (LoadFileFromDta(TITLE_PALETTE_FILENAME, &pal, &psz) && pal) {
        InstallPalette((uint8_t *)pal, 0);
        xfree(pal);
    }
    if (LoadFileFromDta(TITLE_BLEND_PALETTE_FILENAME, &pal, &psz) && pal) {
        xfree(pal);
    }
}

/* Paint the LOLDING (krazek.pic) holding screen and flush it. Used as
 * a "something's happening" indicator during slow loads — the engine
 * is single-threaded so the painted frame stays on screen until the
 * next FlushFrameToPrimary call (typically inside the next game loop).
 *
 * paint_rawb_pic with as_overlay=0 calls MergePalette(krazek_pal) which
 * blends krazek.pic's palette into whatever is currently installed.
 * That mutation persists past this function and would poison the menu's
 * Tlo.pal — sprite colour ramps would shift, giving the title screen a
 * "broken palette" look once the load completes. To keep the merge for
 * LOLDING itself (its disc/text use krazek.pic's entries) but not for
 * what comes after, we re-install Tlo.pal AFTER the flush: the user
 * keeps seeing the LOLDING frame because it's already on the primary
 * surface, and the next flush (first menu iteration) will colour the
 * back buffer with clean Tlo.pal again. */
static void paint_lolding_holding_screen(void)
{
    void    *bg_raw  = NULL;
    uint32_t bg_size = 0;
    if (!LoadFileFromDta("krazek.pic", &bg_raw, &bg_size)) return;
    FlipBuffersClearWith(0);
    paint_rawb_pic(bg_raw, bg_size, 0);
    FlushFrameToPrimary();
    xfree(bg_raw);

    /* Roll back the palette merge that paint_rawb_pic just did so the
     * menu's first paint runs with a clean Tlo.pal. */
    install_main_menu_palettes();
}

/* INIT-trigger handler — runs once when the title is first entered:
 * install the menu palettes FIRST so the LOLDING holding screen uses
 * the correct colour ramp, then paint LOLDING so the user sees a
 * meaningful "still loading" indicator during the heavy mask-atlas
 * load that follows. Without this, on a slow SD card (Miyoo, ~1.5 s
 * for Tlo.wyc) the user would see the previous scene's residue —
 * the intro AVI's end-palette typically left index 0 as saturated
 * blue, and after the palette fix it came out as white. LOLDING is
 * a clearer signal that the engine is doing something. */
static void enter_main_menu(void)
{
    install_main_menu_palettes();
    paint_lolding_holding_screen();
    s_anim_frame   = MAIN_MENU_ANIM_FIRST_FRAME;
    s_anim_delay   = 0;
    s_save_request = 0;
    PlayMenuMusic(MAIN_MENU_BGM_FILENAME, 1);
}

/* TITLE_BTN_LOAD handler — runs the Load sub-menu and translates its
 * rc into the appropriate main-menu rc. */
static int dispatch_load_submenu(void)
{
    /* Snapshot the title-screen backbuffer so the Load overlay's
     * margins show the title art instead of an uninitialised buffer
     * (palette index 0 in the new palette can be white/garbage). */
    SnapshotBackbufferForMenu();
    int r = RunMenuScene(1, &g_load_menu_scene);
    return (r == 3) ? MAIN_MENU_RC_LOAD_SAVE : MAIN_MENU_RC_BACK_TO_MAIN;
}

/* Returns true once the Maluch-click latch is set AND the title
 * flipbook is OUTSIDE the "doors closing" frames. When both hold,
 * HandleMainMenuClick returns rc=PROLOGUE so RunMainGameLoop starts
 * a new playthrough. */
static int maluch_latch_ready_to_fire(void)
{
    return s_save_request &&
           (s_anim_frame <  MAIN_MENU_ANIM_DOORS_FIRST ||
            s_anim_frame >  MAIN_MENU_ANIM_DOORS_LAST);
}

/* Advance the title-screen WACKI-logo flipbook by one frame (or wait
 * for the per-frame tick countdown). Frames 10..(count-1) form the
 * animated logo painted with colour-key 0 so the buttons underneath
 * remain visible. */
static void tick_title_animation(void)
{
    AnimAsset *a = g_menu_asset_10;
    if (a && s_anim_delay < 1) {
        if (s_anim_frame >= a->frame_count)
            s_anim_frame = MAIN_MENU_ANIM_FIRST_FRAME;
        if (s_anim_frame < a->frame_count && a->pixel_ptrs[s_anim_frame]) {
            uint16_t w  = a->off_widths [s_anim_frame];
            uint16_t h  = a->off_heights[s_anim_frame];
            uint16_t dx = a->off_drawX  [s_anim_frame];
            uint16_t dy = a->off_drawY  [s_anim_frame];
            static int once = 0;
            if (once < 5) {
                LOG_TRACE("anim", "frame=%d at (%u,%u) %ux%u", s_anim_frame, dx, dy, w, h);
                ++once;
            }
            BlitSpriteToBackbuffer(dx, dy, 0, 0, w, h, w, h,
                                   a->pixel_ptrs[s_anim_frame], 0);
        }
        ++s_anim_frame;
        s_anim_delay = MAIN_MENU_ANIM_TICKS_PER_FRAME;
    } else {
        --s_anim_delay;
    }
}

static int HandleMainMenuClick(int trigger)
{
    int rc = MAIN_MENU_RC_NONE;
    s_menu_flags |= MAIN_MENU_FLAG_LATCH;

    switch (trigger) {
    case MAIN_MENU_TRIGGER_INIT:
        enter_main_menu();
        break;

    case TITLE_BTN_LOAD:
        rc = dispatch_load_submenu();
        break;

    case TITLE_BTN_NEW:
        rc = MAIN_MENU_RC_NEW_GAME;
        s_menu_flags &= ~MAIN_MENU_FLAG_LATCH;
        break;

    case TITLE_BTN_MALUCH:
        s_save_request = 1;
        break;

    case TITLE_BTN_QUIT:
        rc = MAIN_MENU_RC_QUIT_CONFIRM_B;
        break;

    case TITLE_BTN_CREDITS:
        rc = MAIN_MENU_RC_CREDITS;
        break;
    }

    /* Maluch-latch trailing block: once s_save_request is set AND the
     * title flipbook is outside the "doors closing" pose, return rc=
     * PROLOGUE so RunMainGameLoop starts a new playthrough. */
    if (maluch_latch_ready_to_fire()) {
        rc = MAIN_MENU_RC_PROLOGUE;
        s_menu_flags &= ~MAIN_MENU_FLAG_LATCH;
        s_anim_delay = 1;
    }

    if (trigger > MAIN_MENU_TRIGGER_INIT) {
        LOG_TRACE("menu", "click trigger=0x%02X rc=%d", trigger, rc);
    }

    tick_title_animation();

    /* Any non-zero rc means we're leaving the menu — stop the BGM so
     * it doesn't bleed into the next scene. */
    if (rc != MAIN_MENU_RC_NONE) StopMenuMusic();
    return rc;
}

/* ---- cinematics --------------------------------------------------- */

/* play_bomb_explosion — visual port of the bomb cutscene fired on
 * Quit→TAK from the title menu. bomba.wyc is a 20-frame fullscreen
 * raw atlas; frame 0 holds the menu with a lit fuse, mid-frames bloom
 * the fireball, frame 19 fades to white. */
static void play_bomb_explosion(void)
{
    StopMenuMusic();

    AnimAsset *a = LoadAssetFromDtaBase("bomba.wyc");
    int started_fuse = 0, played_bang = 0;
    if (a && a->frame_count > 0) {
        for (uint16_t f = 0; f < a->frame_count; ++f) {
            if (PlatformShouldQuit()) break;
            PumpEvents();
            /* bomba.wyc frames are fullscreen at (0,0) — paint raw. */
            paint_anim_button_at(a, f, 0, 0, 1);
            FlushFrameToPrimary();
            /* Audio: frame 0 fuse (lont.wav), BOMB_AUDIO_FUSE_FRAME
             * bang (bum.wav). The second PlayMenuMusic implicitly
             * stops lont via its StopMenuMusic call so the bang
             * cleanly cuts the fuse hiss. */
            if (!started_fuse) {
                PlayMenuMusic("lont.wav", 0);
                started_fuse = 1;
            }
            if (!played_bang && f >= BOMB_AUDIO_FUSE_FRAME) {
                PlayMenuMusic("bum.wav", 0);
                played_bang = 1;
            }
            TickMenuMusic();
            EnginePaceFrame(BOMB_FRAME_DELAY_MS);
        }
        FreeAsset(a);
    }
    SDL_Delay(BOMB_TAIL_DELAY_MS);
    StopMenuMusic();
}

/* play_fiacik_intro — Maluch driving across the title screen before
 * the prologue. Re-renders the menu cleanly first (no hover overlay)
 * then snapshots the cleaned shadow and restores under each fiacik
 * frame so there's no per-frame ghosting. */
static void play_fiacik_intro(void)
{
    StopMenuMusic();
    PlayMenuMusic("fiacik.wav", 0);

    /* RunMenuScene's cleanup already freed the mask atlas and NULL'd
     * g_menu_asset_10 — re-load Tlo.wyc locally for the clean redraw. */
    AnimAsset *bg = LoadAssetFromDtaBase("Tlo.wyc");
    if (bg && bg->pixel_ptrs) {
        int bgf = s_anim_frame;
        if (bgf < MAIN_MENU_ANIM_FIRST_FRAME) bgf = MAIN_MENU_ANIM_FIRST_FRAME;
        if (bgf >= bg->frame_count) bgf = bg->frame_count - 1;
        if (bg->pixel_ptrs[bgf]) {
            BlitSpriteToBackbuffer(
                bg->off_drawX[bgf], bg->off_drawY[bgf], 0, 0,
                bg->off_widths[bgf], bg->off_heights[bgf],
                bg->off_widths[bgf], bg->off_heights[bgf],
                bg->pixel_ptrs[bgf], 1);                  /* opaque wipe */
        }
        /* def_anim only for buttons 0..4 (no hover overlay). */
        for (uint16_t i = 0; i < FIACIK_BUTTONS_TO_PAINT; ++i) {
            paint_anim_button_at(bg, i, 0, 0, 0);
        }
    }

    /* The prologue script in the original spawns two entities: mal_back
     * (113×74 orange patch over the Maluch button so the Fiat can drive
     * across without leaving the icon) and fiacik (the Fiat itself). */
    AnimAsset *mal_back = LoadAssetFromDtaBase("mal_back.wyc");
    if (mal_back) {
        paint_anim_button_at(mal_back, 0, 0, 0, 0);
        FreeAsset(mal_back);
    }

    /* Snapshot the now-clean menu shadow so we can restore it under
     * each fiacik frame — equivalent to the engine's RestorePrevFrame
     * Rects, no per-frame ghosting. */
    size_t shadow_bytes = (size_t)WACKI_SCREEN_W * WACKI_SCREEN_H;
    uint8_t *snapshot = (uint8_t *)malloc(shadow_bytes);
    if (snapshot && g_back_shadow) memcpy(snapshot, g_back_shadow, shadow_bytes);

    AnimAsset *a = LoadAssetFromDtaBase("fiacik.wyc");
    if (a && a->frame_count > 0) {
        for (uint16_t f = 0; f < a->frame_count; ++f) {
            if (PlatformShouldQuit()) break;
            PumpEvents();
            if (snapshot && g_back_shadow) {
                memcpy(g_back_shadow, snapshot, shadow_bytes);
            }
            paint_anim_button_at(a, f, 0, 0, 0);   /* honour atlas hot-spot */
            FlushFrameToPrimary();
            TickMenuMusic();
            EnginePaceFrame(FIACIK_FRAME_DELAY_MS);
        }
        FreeAsset(a);
    }
    if (bg) FreeAsset(bg);
    free(snapshot);
    SDL_Delay(FIACIK_TAIL_DELAY_MS);
    StopMenuMusic();
}

/* play_film_intro — the projector cutscene fired when the player clicks
 * the film-strip icon (New, 0x13). 1:1 with the original's New click
 * script (RunScriptInterpreter @ DAT_00427b40), which loads film.wyc,
 * spawns it and plays it once to its last frame. film.wyc is a 20-frame
 * raw atlas anchored at (260,326) — the exact slot of the static film
 * icon (Tlo.wyc frame 1) — so it animates that button in place. After it
 * finishes, RunMainGameLoop's outer loop replays the title intro AVI
 * (same as the original's case-6 break). */
static void play_film_intro(void)
{
    StopMenuMusic();
    PlayMenuMusic("Proj1.wav", 0);

    /* RunMenuScene's cleanup already freed the mask atlas and NULL'd
     * g_menu_asset_10 — re-load Tlo.wyc locally for the clean backdrop
     * (current lava frame + the 5 button rest sprites, no hover). */
    AnimAsset *bg = LoadAssetFromDtaBase("Tlo.wyc");
    if (bg && bg->pixel_ptrs) {
        int bgf = s_anim_frame;
        if (bgf < MAIN_MENU_ANIM_FIRST_FRAME) bgf = MAIN_MENU_ANIM_FIRST_FRAME;
        if (bgf >= bg->frame_count) bgf = bg->frame_count - 1;
        if (bg->pixel_ptrs[bgf]) {
            BlitSpriteToBackbuffer(
                bg->off_drawX[bgf], bg->off_drawY[bgf], 0, 0,
                bg->off_widths[bgf], bg->off_heights[bgf],
                bg->off_widths[bgf], bg->off_heights[bgf],
                bg->pixel_ptrs[bgf], 1);                  /* opaque wipe */
        }
        for (uint16_t i = 0; i < FIACIK_BUTTONS_TO_PAINT; ++i) {
            paint_anim_button_at(bg, i, 0, 0, 0);
        }
    }

    /* Snapshot the clean menu shadow so we can restore it under each
     * film frame — film.wyc is colour-keyed (index 0 transparent), so
     * without the restore the projector's transparent pixels would let
     * the previous frame bleed through. */
    size_t shadow_bytes = (size_t)WACKI_SCREEN_W * WACKI_SCREEN_H;
    uint8_t *snapshot = (uint8_t *)malloc(shadow_bytes);
    if (snapshot && g_back_shadow) memcpy(snapshot, g_back_shadow, shadow_bytes);

    AnimAsset *a = LoadAssetFromDtaBase("film.wyc");
    int played_proj2 = 0;
    if (a && a->frame_count > 0) {
        for (uint16_t f = 0; f < a->frame_count; ++f) {
            if (PlatformShouldQuit()) break;
            PumpEvents();
            if (snapshot && g_back_shadow) {
                memcpy(g_back_shadow, snapshot, shadow_bytes);
            }
            paint_anim_button_at(a, f, 0, 0, 0);   /* honour atlas hot-spot */
            FlushFrameToPrimary();
            /* Proj1.wav rolls from frame 0; Proj2.wav takes over at
             * FILM_PROJ2_FRAME (PlayMenuMusic stops the previous clip). */
            if (!played_proj2 && f >= FILM_PROJ2_FRAME) {
                PlayMenuMusic("Proj2.wav", 0);
                played_proj2 = 1;
            }
            TickMenuMusic();
            EnginePaceFrame(FILM_FRAME_DELAY_MS);
        }
        FreeAsset(a);
    }
    if (bg) FreeAsset(bg);
    free(snapshot);
    SDL_Delay(FILM_TAIL_DELAY_MS);
    StopMenuMusic();
}

/* play_loading_screen — the "LOLDING" screen between Maluch and the
 * first gameplay scene. krazek.pic is a 203×220 RAWB of a vinyl-CD
 * shape with the text "LOLDING" baked in; paint_rawb_pic centres it
 * and colour-keys index 0 so the corners are transparent.
 *
 * Previously held the screen up for LOADING_SCREEN_HOLD_MS (1.5 s)
 * BEFORE calling LoadStage. On the original 1997 PC where stage
 * loading itself took multiple seconds this was an artistic beat;
 * on modern hosts (especially Miyoo where load is sub-100 ms) it
 * was 1.5 s of dead time and *then* the player saw the partially-
 * rendered scene flash up because LOLDING had already been torn
 * down.
 *
 * New behaviour: paint LOLDING, present it once, return immediately.
 * The caller invokes RunGameStageLoop next — that loads assets, runs
 * the enter_script and only then calls FlushFrameToPrimary again,
 * which is the moment LOLDING gets replaced by the first real
 * gameplay frame. Single-threaded engine means nothing else
 * redraws the screen in between, so LOLDING stays visible for the
 * full duration of the load. Minimum-hold of LOADING_SCREEN_MIN_HOLD_MS
 * keeps a brief pause so even sub-100 ms loads don't flash. */
#define LOADING_SCREEN_MIN_HOLD_MS  400
static void play_loading_screen(void)
{
    void *bg_raw = NULL;
    uint32_t bg_size = 0;
    if (!LoadFileFromDta("krazek.pic", &bg_raw, &bg_size)) return;

    PumpEvents();
    FlipBuffersClearWith(0);
    paint_rawb_pic(bg_raw, bg_size, 0);
    FlushFrameToPrimary();
    xfree(bg_raw);

    /* Brief minimum hold so very fast loaders still register the
     * screen visually. Anything past this point happens while
     * LOLDING is still on display — LoadStage runs synchronously
     * and doesn't redraw until it's done. */
    SDL_Delay(LOADING_SCREEN_MIN_HOLD_MS);
    PumpEvents();
    if (PlatformShouldQuit()) return;
}

/* Port-attribution helpers live in src/menu/port_attribution.c —
 * referenced here as externs so the title SceneDef can wire the
 * footer paint, and the Credits-AVI handler can chain the standalone
 * "Port: ..." screen. */
extern void paint_title_attribution_footer(void);
extern void play_port_attribution_screen(void);

/* ---- title-menu SceneDef builders + helpers ----------------------- */

static SceneDef make_title_scene(void)
{
    SceneDef s = {
        .background_pic = NULL,
        .mask_file      = TITLE_MASK_FILENAME,
        .on_click       = HandleMainMenuClick,
        .after_paint    = paint_title_attribution_footer,
        .button_count   = TITLE_BUTTON_COUNT,
        .flags          = SCENE_FLAG_FORCE_CB,
        .buttons = {
            { .id = TITLE_BTN_LOAD,    .def_anim = 0,
              .hover_anim = TITLE_HOVER_FRAME_OFFSET + 0 },
            { .id = TITLE_BTN_NEW,     .def_anim = 1,
              .hover_anim = TITLE_HOVER_FRAME_OFFSET + 1 },
            { .id = TITLE_BTN_MALUCH,  .def_anim = 2,
              .hover_anim = TITLE_HOVER_FRAME_OFFSET + 2 },
            { .id = TITLE_BTN_QUIT,    .def_anim = 3,
              .hover_anim = TITLE_HOVER_FRAME_OFFSET + 3 },
            { .id = TITLE_BTN_CREDITS, .def_anim = 4,
              .hover_anim = TITLE_HOVER_FRAME_OFFSET + 4 },
        },
    };
    return s;
}

static SceneDef make_pytanie_scene(void)
{
    SceneDef s = {
        .background_pic = "Pytanie.pic",
        .mask_file      = "Pytanie.wyc",
        .on_click       = HandlePytanieClick,
        .button_count   = 2,
        .flags          = SCENE_FLAG_MOUSE_ONLY,
        .buttons = {
            { .id = PYTANIE_TRIGGER_TAK,
              .def_anim = PYTANIE_FRAME_NONE, .hover_anim = 0 },
            { .id = PYTANIE_TRIGGER_NIE,
              .def_anim = PYTANIE_FRAME_NONE, .hover_anim = 1 },
        },
    };
    return s;
}

/* Install the Tlo.pal background palette. Done here because we haven't
 * ported the original InitializeStage that does it on engine boot. */
static void install_title_palette(void)
{
    void    *pal = NULL;
    uint32_t psz = 0;
    if (LoadFileFromDta(TITLE_PALETTE_FILENAME, &pal, &psz) && pal) {
        InstallPalette((uint8_t *)pal, 0);
        xfree(pal);
    }
}

/* Mirror RunGameStageLoop's flag-2 FULL RESET cleanup: zero script
 * vars, clear each entity_state[i].in_inventory_flag (preserving the
 * panel_verb_id identity mapping), reset the inventory state. */
static void apply_full_reset(void)
{
    memset(g_script_vars, 0, sizeof g_script_vars);
    uint16_t *es = (uint16_t *)g_entity_state;
    for (int idx = 0; idx < ENTITY_STATE_ENTRY_COUNT; ++idx) {
        es[idx * ENTITY_STATE_FIELDS_PER_ENTRY +
           ENTITY_STATE_INVENTORY_FIELD] = 0;
    }
    ResetInventory();
}

/* (1<<(N-1)) - 1 = bits 0..N-2 = "stages 1..N-1 completed" bitmask. */
static uint32_t dev_completed_mask_for_stage(int n)
{
    return (uint32_t)((1u << (n - 1)) - 1u);
}

/* g_game_over_code values 0/1/3/4 mean "stage progressed normally";
 * anything else (2 = ESC/F12 quit, 99 = hard-quit, unknown codes)
 * means the user is done with the dev session. */
static int game_over_is_progress_signal(int code)
{
    return code == GAME_OVER_NONE
        || code == GAME_OVER_DEATH
        || code == GAME_OVER_CHAPTER_PICK
        || code == GAME_OVER_STAGE_END_AVI;
}

/* ---- dev start-stage flow ----------------------------------------- */

/* DEV --start-stage N: skip menu+intro, show chapter-select map with
 * stages 1..(N-1) marked completed. User picks a stage from the map,
 * then that stage runs normally. Loop so returning from one stage
 * re-shows the map (e.g. ESC out of stage 2 → back to map). Returns
 * 1 if the dev flow handled the run (RunMainGameLoop should return),
 * 0 if dev mode is off and the normal flow should proceed. */
static int run_dev_start_stage_flow(void)
{
    if (g_dev_start_stage < DEV_START_STAGE_MIN ||
        g_dev_start_stage > DEV_START_STAGE_MAX) return 0;

    int N = g_dev_start_stage;
    apply_full_reset();
    g_completed_stages = dev_completed_mask_for_stage(N);
    LOG_INFO("wacki", "dev-start: chapter-select map, "
                    "completed_mask=0x%X (stages 1..%d done)", g_completed_stages, N - 1);

    while (!PlatformShouldQuit()) {
        (void)SelTloRefreshButtons();
        s_chapter_pick = 0;
        RunMenuScene(0, &g_sel_tlo_scene);
        if (PlatformShouldQuit()) return 1;

        if (s_chapter_pick < 1 || s_chapter_pick > DEV_PICK_FINALE) {
            LOG_INFO("wacki", "dev-start: no stage picked — exit");
            return 1;
        }
        LOG_INFO("wacki", "dev-start: stage %d picked from map", s_chapter_pick);
        if (!LoadStage((uint16_t)s_chapter_pick)) {
            LOG_INFO("wacki", "dev-start: LoadStage(%d) failed", s_chapter_pick);
            continue;
        }

        int played_stage = s_chapter_pick;
        RunGameStageLoop(STAGE_LOAD_FLAG_SAVE_LOAD);

        if (played_stage == DEV_PICK_FINALE) {
            LOG_INFO("wacki", "dev-start: Monter finale complete "
                            "→ exit (= main menu in normal flow)");
            return 1;
        }

        if (!game_over_is_progress_signal(g_game_over_code)) {
            LOG_INFO("wacki", "dev-start: game_over=%d → exit", g_game_over_code);
            return 1;
        }

        g_completed_stages |= dev_completed_mask_for_stage(N);
    }
    return 1;
}

/* ---- title menu dispatch ----------------------------------------- */

/* Pytanie Y/N quit-confirm cascade — runs the menu, bombs + returns 1
 * on TAK, returns 0 on NIE. Caller breaks out of the menu re-entry
 * loop on TAK. */
static int prompt_quit_with_bomb(void)
{
    SceneDef q = make_pytanie_scene();
    int rc = RunMenuScene(1, &q);
    if (rc == PYTANIE_RC_TAK) {
        play_bomb_explosion();
        return 1;
    }
    return 0;
}

/* Dispatch one return code from RunMenuScene(title). Returns 1 if the
 * inner loop should keep running, 0 if the caller should break out
 * (back to outer's intro replay). */
static int dispatch_main_menu_rc(int rc, int *should_return)
{
    *should_return = 0;
    switch (rc) {
    case MAIN_MENU_RC_QUIT_CONFIRM_A:
    case MAIN_MENU_RC_QUIT_CONFIRM_B:
        if (prompt_quit_with_bomb()) {
            *should_return = 1;
            return 0;
        }
        return 1;

    case MAIN_MENU_RC_LOAD_SAVE:
        /* LoadSaveSlot restores g_cur_komnata + g_script_vars +
         * g_entity_state from Wacki.sav slot N. */
        LoadSaveSlot(g_selected_save_slot);
        RunGameStageLoop(STAGE_LOAD_FLAG_SAVE_LOAD);
        return 1;

    case MAIN_MENU_RC_NEW_GAME:
        /* Film-reel button (0x13). The original's click script
         * (RunScriptInterpreter @ DAT_00427b40) plays film.wyc once,
         * then case 6 breaks the inner loop so the title intro AVI
         * replays. Mirror that: projector cutscene, then return 0. */
        play_film_intro();
        return 0;

    case MAIN_MENU_RC_PROLOGUE:
        /* [etap]1 [komnata]init prologue:
         *   1. fiacik.wyc Maluch driving across the title
         *   2. krazek.pic "Lołding" progress bar
         *   3. hand off to RunGameStageLoop(FULL_RESET). */
        play_fiacik_intro();
        play_loading_screen();
        RunGameStageLoop(STAGE_LOAD_FLAG_FULL_RESET);
        return 0;

    case MAIN_MENU_RC_CREDITS:
        PlaySceneCutsceneAvi(CREDITS_AVI);
        /* Tail the Credits AVI with a standalone screen crediting
         * this C/SDL2 port — same context (it's a credits sequence)
         * so it doesn't feel out of place; centred text, ~4 s hold,
         * user can click through. */
        play_port_attribution_screen();
        return 1;
    }
    return 1;
}

/* ---- public entry point ------------------------------------------ */

void RunMainGameLoop(void)
{
    LoadSaveStateOrInitialize();

    /* Push Wacki.sav settings into in-memory s_opt_* + audio mixer
     * (options.c). Without this the saved prefs were loaded into
     * g_save.settings but never had any effect. */
    ApplySavedSettings();
    install_title_palette();

    if (run_dev_start_stage_flow()) return;

    SceneDef title = make_title_scene();

    /* OUTER: each iteration plays the title intro then drives the menu
     * re-entry inner loop. INNER: each iteration shows the title menu
     * and dispatches one click. */
    while (1) {
        if (PlatformShouldQuit()) return;
        PlaySceneCutsceneAvi(TITLE_INTRO_AVI);

        int inner = 1;
        while (inner) {
            int rc = RunMenuScene(1, &title);
            if (PlatformShouldQuit() || rc == MAIN_MENU_RC_HARD_QUIT) return;

            int should_return = 0;
            inner = dispatch_main_menu_rc(rc, &should_return);
            if (should_return) return;
            if (g_game_over_code > GAME_OVER_STAGE_END_AVI) return;
        }
    }
}
