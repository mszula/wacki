/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/scene/play_loop.c — per-room gameplay loop (play_demo_scene).
 *
 * Drives one komnata's gameplay from entry to exit. Sets up the
 * scene (palette, panel, atlases, persistent actors, entry script),
 * then runs a per-frame loop until the user quits or a script raises
 * g_game_over_code.
 *
 * Per-frame work:
 *   - Exit polls (PlatformShouldQuit, g_scene_quit, g_game_over_code)
 *   - update_actor_perspective_scale  — entity[+0x58] scale_pct
 *   - ProcessGameFrameTickInner       — BG paint + entity tick + HUD + cursor
 *   - FlushFrameToPrimary + TickMenuMusic
 *   - Keyboard handlers (SPACE / ESC)
 *   - F5 / F9 quicksave / quickload latches
 *   - F3 stats dump
 *   - F12 pause-menu (Pytanie quit-confirm)
 *   - Frame pacing (33 ms ≈ 30 fps; bypassed by --no-pacing)
 *
 * The original engine did all of this inline inside RunGameStageLoop;
 * this port path is a single-iteration komnata loop that runs until
 * the actor leaves the stage. Subsequent komnata transitions happen
 * via op 0x20 → ScriptGoToKomnata → LoadKomnataScene without
 * unwinding play_demo_scene. */

#include "wacki.h"
#include "wacki/log.h"
#include "wacki/platform/input.h"
#include <SDL.h>

#include <stdint.h>
#include <stdio.h>

/* ---- constants ---------------------------------------------------- */

#define ROOM_PALETTE_FILENAME       "paleta.pal"
#define DEFAULT_PANEL_FILENAME      "panel.wyc"

/* g_komnata_flags bit 0 = "panel visible this komnata". */
#define KOMNATA_FLAG_PANEL_VISIBLE  0x01u

/* Initial actor spawn frame (idle pose) + scene-default positions. */
#define ACTOR_INIT_FRAME            11
#define ACTOR_EBEK_INIT_X           380
#define ACTOR_EBEK_INIT_Y           375
#define ACTOR_FJEJ_INIT_X           300
#define ACTOR_FJEJ_INIT_Y           380
#define ACTOR_VERB_EBEK             1
#define ACTOR_VERB_FJEJ             2

/* Entry-script bytecode PE VA — loads floor cursor atlases then
 * tail-calls the actor position chain. */
#define ACTOR_ENTRY_SCRIPT_VA       0x004251C8u

/* "Neutral / no verb" sentinel — the engine's convention for enter_
 * script / room init calls. Replicated here to keep this module
 * independent of game.c's title-constants block. */
#define SCENE_NEUTRAL_VERB          0x26

/* Perspective scale clamps for the +0x58 scale_pct write. */
#define PERSPECTIVE_BASELINE_Y      400
#define PERSPECTIVE_SCALE_MIN_PCT   30
#define PERSPECTIVE_SCALE_MAX_PCT   160

/* g_game_over_code value for "user-confirmed quit to main menu". */
#define GAME_OVER_USER_QUIT         2

/* F12 Pytanie return code: TAK ("Yes" → confirm quit). */
#define PYTANIE_RC_TAK              3

/* QuickSave / QuickLoad slot + the marker name persist_audio_opts
 * writes so the user can tell quicksaves apart in the Load menu. */
#define QUICK_SAVE_SLOT             0
#define QUICK_SAVE_DISPLAY_NAME     "Quick"

/* Frame pacing target — 33 ms ≈ 30 fps. */
#define TARGET_FRAME_DELAY_MS       33

/* Win32 VK_ constants the original engine uses for keyboard polling.
 * Defined verbatim here so play_loop.c stays independent of game.c's
 * top-of-file block. */
#define VK_ESCAPE 0x1B
#define VK_SPACE  0x20

/* ---- externs ------------------------------------------------------ */

extern AnimAsset      *g_ebek_atlas;
extern AnimAsset      *g_fjej_atlas;
extern Entity         *SpawnActorEntity(uint16_t id, AnimAsset *atlas,
                                        uint16_t init_frame,
                                        int16_t init_x, int16_t init_y);
extern const void     *xlat_binary_ptr(uint32_t addr);
extern void            ProcessGameFrameTickInner(void);
extern void            LoadKomnataScene(uint16_t id);
extern int             g_no_pacing;
extern SceneDef       *opt_get_pytanie_scene(void);
extern void            SnapshotBackbufferForMenu(void);  /* menu_loop.c */

/* Scene-state globals defined in game.c. */
extern int                    g_walk_x0;
extern int                    g_walk_x1;
extern int                    g_walk_y0;
extern int                    g_walk_y1;
extern const DemoScene       *g_current_scene;
extern int                    g_scene_quit;
extern AnimAsset             *g_scene_fld_asset;
extern void                  *g_scene_bg_raw;
extern uint32_t               g_scene_bg_size;
extern const uint8_t         *g_walk_fld_pixels;

/* ---- setup helpers ------------------------------------------------ */

/* Stage palette load — every paletted blit reads from the installed
 * 256×3 palette, so this must happen before any BG/sprite paint. */
static void install_room_palette(void)
{
    void    *pal = NULL;
    uint32_t psz = 0;
    if (LoadFileFromDta(ROOM_PALETTE_FILENAME, &pal, &psz) && pal) {
        InstallPalette((uint8_t *)pal, 0);
        xfree(pal);
    }
}

/* Load the HUD panel asset (singleton, shared across stage-1 komnaty).
 * Stage 5 (Monter finale) sets g_stage->panel_wyc = NULL → no HUD;
 * LoadStage already free'd + NULLed g_panel_asset, so we must not
 * load a default panel here or the ACME ending would have a HUD. */
static void prepare_panel_asset(void)
{
    int stage_has_panel = (g_stage && g_stage->panel_wyc) || !g_stage;
    if (stage_has_panel && !g_panel_asset) {
        g_panel_asset = LoadAssetFromDtaBase(DEFAULT_PANEL_FILENAME);
    }
    if (stage_has_panel) g_komnata_flags |=  KOMNATA_FLAG_PANEL_VISIBLE;
    else                 g_komnata_flags &= ~KOMNATA_FLAG_PANEL_VISIBLE;
}

/* Pick the per-actor atlas to spawn from — NULL for stages where the
 * actor isn't present (Monter stage 5 has neither). */
static void resolve_actor_atlases(AnimAsset *out[2])
{
    out[0] = (g_stage && !g_stage->ebek_wyc) ? NULL : g_ebek_atlas;
    out[1] = (g_stage && !g_stage->fjej_wyc) ? NULL : g_fjej_atlas;
}

/* Spawn Ebek (id=1) + Fjej (id=2) as entity-backed actors so the
 * original scripts can position them via op 0x28 SET_ENTITY_XY. Actor
 * entities PERSIST across scene transitions (preserved by EntityList
 * ClearAll); we only spawn on the FIRST scene. */
static void spawn_persistent_actors_if_needed(AnimAsset *atlases[2])
{
    if (!g_actor[0] && atlases[0]) {
        g_actor[0] = SpawnActorEntity(ACTOR_VERB_EBEK, atlases[0],
                                      ACTOR_INIT_FRAME,
                                      ACTOR_EBEK_INIT_X, ACTOR_EBEK_INIT_Y);
    }
    if (!g_actor[1] && atlases[1]) {
        g_actor[1] = SpawnActorEntity(ACTOR_VERB_FJEJ, atlases[1],
                                      ACTOR_INIT_FRAME,
                                      ACTOR_FJEJ_INIT_X, ACTOR_FJEJ_INIT_Y);
    }
}

/* Run the original actor entry chain. The script loads the floor
 * cursor atlases then conditionally positions Ebek/Fjej via op 0x28
 * based on var[6] bit 0 (= "in scene transition" flag). Call args
 * (this/that) = SCENE_NEUTRAL_VERB matches the engine's convention for
 * all enter_script / room init calls. */
static void run_actor_entry_chain(void)
{
    const uint8_t *entry_script =
        (const uint8_t *)xlat_binary_ptr(ACTOR_ENTRY_SCRIPT_VA);
    if (entry_script) {
        LOG_INFO("actor", "running entry chain @ 0x%08X", ACTOR_ENTRY_SCRIPT_VA);
        RunScriptInterpreter(SCENE_NEUTRAL_VERB, SCENE_NEUTRAL_VERB,
                             (uint8_t *)entry_script);
    }
}

/* Publish scene walk-bounds + the scene pointer to the globals
 * HandleSceneInput / is_walkable_at read from. */
static void publish_scene_walk_bounds(const DemoScene *scene)
{
    g_walk_x0 = scene->walk_x0;
    g_walk_x1 = scene->walk_x1;
    g_walk_y0 = scene->walk_y0;
    g_walk_y1 = scene->walk_y1;
    g_current_scene = scene;
}

/* ---- per-frame helpers ------------------------------------------- */

/* Recompute the perspective scale_pct at entity[+0x58] each frame for
 * both actors, matching the prologue of the original UpdateActorMovement.
 * The walker itself doesn't write +0x58. */
static void update_actor_perspective_scale(void)
{
    extern int g_actor_scale_frozen[2];
    for (int i = 0; i < 2; ++i) {
        if (!g_actor[i]) continue;
        if (g_actor_scale_frozen[i]) continue;   /* action/climb holds its scale */
        uint8_t *eb = (uint8_t *)g_actor[i];
        int16_t anchor_y = EOFF(eb, ENT_OFF_ANCHOR_Y, int16_t);
        int     scale_pct =
            (int)g_cursor_speed
            - ((PERSPECTIVE_BASELINE_Y - anchor_y) * (int)g_perspective_min)
              / (int)g_perspective_step;
        if (scale_pct < PERSPECTIVE_SCALE_MIN_PCT) scale_pct = PERSPECTIVE_SCALE_MIN_PCT;
        if (scale_pct > PERSPECTIVE_SCALE_MAX_PCT) scale_pct = PERSPECTIVE_SCALE_MAX_PCT;
        EOFF(eb, ENT_OFF_SCALE_PCT, uint16_t) = (uint16_t)scale_pct;
    }
}

/* Drain SPACE (toggle active actor) / ESC (user-confirmed quit) from
 * the platform key queue.
 *
 * The SPACE → toggle binding is gated on plat_input_has_keyboard(): on a
 * handheld every hardware button is mapped by firmware to some keyboard
 * scancode, unpredictably (Miyoo Mini Plus volume buttons triggered the actor
 * toggle on one user's device, presumably aliased onto the A-face scancode),
 * and the PS2 has no keyboard at all — so SPACE isn't a safe binding there.
 * The B button (RMB → toggle in HandleSceneInput) covers the same gesture on
 * those targets. ESC stays everywhere — it's the universal quit. */
static void handle_gameplay_keys(int *quit)
{
    if (!HasPendingKey()) return;
    uint16_t k = WaitForKey();
    if (k == VK_ESCAPE) {
        g_game_over_code = GAME_OVER_USER_QUIT;
        *quit = 1;
    }
    else if (k == VK_SPACE && plat_input_has_keyboard()) {
        g_active_actor ^= 1;
        LOG_TRACE("scene", "active actor → %s", g_active_actor ? "Fjej" : "Ebek");
    }
}

/* F5 quicksave / F9 quickload latches (set by the platform_sdl key
 * handler, consumed + cleared here). */
static void handle_quicksave_quickload_requests(void)
{
    if (g_quicksave_request) {
        g_quicksave_request = 0;
        WackiSlot *qs = &g_save.slots[QUICK_SAVE_SLOT];
        memset(qs->name, 0, sizeof qs->name);
        snprintf(qs->name, sizeof qs->name, QUICK_SAVE_DISPLAY_NAME);
        QuickSaveToSlot(QUICK_SAVE_SLOT);
    }
    if (g_quickload_request) {
        g_quickload_request = 0;
        if (QuickLoadFromSlot(QUICK_SAVE_SLOT)) {
            LoadKomnataScene(g_cur_komnata);
        }
    }
}

/* F12 pause-menu (Pytanie quit-confirm). TAK → quit-to-main-menu,
 * NIE → fall through and keep playing. */
static void handle_pause_menu_request(int *quit, const char **next_scene)
{
    if (!g_pause_menu_request) return;
    g_pause_menu_request = 0;
    /* Capture the live gameplay frame so the Pytanie overlay's margins
     * show the current scene, not a stale snapshot from an earlier menu.
     * Mirrors OpenOptionsMenu / the Load sub-menu, which snapshot first. */
    SnapshotBackbufferForMenu();
    int rc = RunMenuScene(1, opt_get_pytanie_scene());
    LOG_TRACE("scene", "F12 Pytanie rc=%d", rc);
    if (rc == PYTANIE_RC_TAK) {
        g_game_over_code = GAME_OVER_USER_QUIT;
        *quit = 1;
        *next_scene = NULL;
    }
}

/* End-of-scene asset cleanup. Actor atlases are singletons (lifetime =
 * whole game) — DO NOT free them here; LoadStage frees the panel
 * asset on stage change. */
static void cleanup_scene_assets(void)
{
    if (g_scene_fld_asset) {
        FreeAsset(g_scene_fld_asset);
        g_scene_fld_asset = NULL;
    }
    if (g_scene_bg_raw) {
        xfree(g_scene_bg_raw);
        g_scene_bg_raw = NULL;
    }
    g_scene_bg_size = 0;
    StopMenuMusic();
    /* Clear scene/walkability globals so HandleSceneInput called from
     * blocking-wait pumps (menu, intro AVI) doesn't read stale FLD
     * pointers. */
    g_current_scene   = NULL;
    g_walk_fld_pixels = NULL;
}

/* ---- public entry point ------------------------------------------ */

/* Run one komnata's gameplay loop. Setup → per-frame loop → cleanup.
 * Returns the next-scene name if the script requested a transition
 * (currently always NULL — komnata transitions happen via op 0x20
 * ScriptGoToKomnata + LoadKomnataScene in-place, not by unwinding).
 *
 * Non-static so RunGameStageLoop (game.c) can call it. */
const char *play_demo_scene(const DemoScene *scene)
{
    install_room_palette();

    LoadKomnataScene(g_cur_komnata);

    prepare_panel_asset();
    AnimAsset *atlases[2];
    resolve_actor_atlases(atlases);
    LOG_TRACE("scene", "initial entry: panel=%d ebek=%d fjej=%d", g_panel_asset ? g_panel_asset->frame_count : 0, atlases[0]    ? atlases[0]   ->frame_count : 0, atlases[1]    ? atlases[1]   ->frame_count : 0);

    spawn_persistent_actors_if_needed(atlases);
    run_actor_entry_chain();
    publish_scene_walk_bounds(scene);

    const char *next_scene = NULL;
    int         quit       = 0;
    g_scene_quit = 0;

    while (!quit) {
        if (PlatformShouldQuit()) break;
        if (g_scene_quit) { g_scene_quit = 0; quit = 1; break; }
        if (g_game_over_code) { quit = 1; break; }

        update_actor_perspective_scale();

        ProcessGameFrameTickInner();
        FlushFrameToPrimary();
        TickMenuMusic();

        handle_gameplay_keys(&quit);
        handle_quicksave_quickload_requests();
        if (g_stats_dump_request) {
            g_stats_dump_request = 0;
            StatsDump();
        }
        handle_pause_menu_request(&quit, &next_scene);

        if (!g_no_pacing) EnginePaceFrame(TARGET_FRAME_DELAY_MS);
    }

    cleanup_scene_assets();
    return next_scene;
}
