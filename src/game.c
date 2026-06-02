/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* src/game.c — top-level game state machine, scene runner, click
 * dispatcher, per-frame tick.
 *
 * Entry points: InitializeGameSubsystems (boot), RunMainGameLoop
 * (outer menu loop), RunGameStageLoop (per-stage loop with
 * cutscenes and frame ticks), RunMenuScene (one SceneDef-driven
 * menu / dialog), LoadStage, ProcessGameFrameTick (per-tick driver),
 * DispatchClickEvent (verb/object routing). */
#include "wacki.h"
#include "wacki/log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <SDL.h>

/* Portable key codes — match SDL_Keycode for ASCII keys (matches the
 * original where Win32 VK_ESCAPE = 0x1B happens to equal ASCII ESC). */
#define VK_ESCAPE 0x1B
#define VK_SPACE  0x20
#define VK_F12    0x7B

/* Forward decl: stubs.c owns ScriptObj for the portable build. */
struct ScriptObj { const uint8_t *start; const uint8_t *end; uint32_t size; uint8_t *buf; };

/* ---- shared state ------------------------------------------------------- */
StageDef *g_stage = NULL;                       /* g_actor_walk_anim_table */
uint16_t  g_cur_etap    = 0;                    /* */
uint16_t  g_cur_komnata = 0;                    /* g_cur_komnata */
/* g_game_over_code is a macro alias for g_script_vars[14] — see wacki.h.
 * g_completed_stages is similarly aliased to g_script_vars[17] (Fix #21).
 * No separate storage; the values live inside the script_vars array so
 * the SET_VAR / VAR_OR opcodes (op 0x0D / 0x0A, used by scripts) and the
 * post-loop switch / SelTloRefreshButtons see the SAME memory. */
int       g_save_request = 0;                   /* g_script_vars */
uint32_t  g_tick_counter = 0;                   /* g_tick_counter */
uint8_t   g_lmb_handled = 0;                    /* g_lmb_handled */

extern Entity *g_actor[2];                      /* g_actor/728 */

/* ---- T2 phase B: scene/walkability state promoted to globals -------------- *
 * Previously locals inside play_demo_scene; promoted so ProcessGameFrameTick
 * Inner can run the click handler + hotspot scan without scene-locals
 * threading. Set by play_demo_scene at scene-load top, cleared at end. */
const DemoScene *g_current_scene = NULL;            /* hotspot + walk-bounds */
const uint8_t *g_walk_fld_pixels = NULL;            /* .fld walkability bitmap (1bpp) */
int            g_walk_fld_w = 0,    g_walk_fld_h = 0;
int            g_walk_fld_ox = 0,   g_walk_fld_oy = 0;
int            g_walk_fld_stride = 0;
int            g_walk_x0 = 0, g_walk_x1 = 0;        /* fallback bbox (no .fld) */
int            g_walk_y0 = 0, g_walk_y1 = 0;
/* g_scene_quit — set by OpenOptionsMenu when Pytanie TAK confirms a
 * mid-game quit. play_demo_scene's main loop polls it as a quit
 * signal. Verb-driven exits (op 0x20 → LoadKomnataScene) are the
 * normal path; this is the user-cancellation latch. */
int            g_scene_quit = 0;

/* T-trail: scene BG published so ProcessGameFrameTickInner can repaint
 * underneath entities every frame — RestorePrevFrameRects
 * at top of (PGFT). Without this, blocking-wait pumps from
 * inside scripts (op 0x09 SHOW_TEXT, dialog runner, op 0x10/0x11/0x12 walks)
 * tick + render entities without clearing the prior frame → sprite trails.
 * Set by LoadKomnataScene (T22 phase B). */
void      *g_scene_bg_raw  = NULL;
uint32_t   g_scene_bg_size = 0;
/* T22 phase B — fld_asset hoisted from play_demo_scene local to global
 * so LoadKomnataScene can free old + load new in-place during op 0x20
 * transitions (no play_demo_scene unwinding). */
AnimAsset *g_scene_fld_asset = NULL;

/* ---- forward decls for things we use ------------------------------------ */
extern void InstallPalette(const uint8_t *rgb, uint16_t first);
extern void PaletteFadeInOut(uint16_t pct, const uint8_t *pal,
                             uint16_t first, uint32_t flags,
                             void *cb);
extern void *g_dialogues_obj;
extern void *g_scripts_obj;
extern void *g_items_obj;

/* g_stage_table is defined in stubs.c. */

/* ---- PreloadCommonAssets ----------------------------------------- *
 *
 * Loaded once at startup. Holds the universally-shared sprites
 * resident in memory between stage swaps. */
FontHandle *g_default_font = NULL;     /* "Futura.30" bitmap font */

extern AnimAsset *g_items_atlas;       /* przedm.wyc */

/* T4 step 1: actor atlas singletons. Loaded once at startup, persist
 * across all scene transitions. The original engine spawns Ebek/Fjej
 * once at game start with these atlases bound; verb scripts post-
 * GO_EXIT can reposition the SAME actor entity without atlas reload. */
AnimAsset *g_ebek_atlas = NULL;        /* ebek.wyc — Ebek sprite frames */
AnimAsset *g_fjej_atlas = NULL;        /* fjej.wyc — Fjej sprite frames */
AnimAsset *g_ebfj_atlas = NULL;        /* ebfj.wyc — actor-portrait atlas
                                        * (4 frames: 0/1 = Ebek/Fjej
                                        * active w/ frame, 2/3 = inactive) */

/* T31 — cursor state atlases. Loaded by PreloadCommonAssets into
 * g_cursor_atlas[], then indexed by cursor state (0..7).
 *
 * Naming note: in the original PE these slots are loaded with
 * olowek1.wyc / kaseta.wyc / magnes1*.wyc / drzwi1*.wyc — but they're
 * NOT the puzzle items. They're CURSOR SPRITE ATLASES with cursor-
 * shaped frames (the names just happened to be assigned to the slots
 * in PreloadCommonAssets). The 8-state cursor anim table maps states
 * 0..7 to indices into the slot array. */
AnimAsset *g_cursor_atlas[8]    = {0};
uint8_t    g_cursor_state       = 0;
uint16_t   g_cursor_frame       = 0;
uint16_t   g_cursor_frame_acc   = 0;

extern void BuildStageTable(void);                /* stubs.c — T26 */

/* ---- ProcessGameFrameTick ---------------------------------------- */

extern void ScreenshotToPcxAutoIncrement(void);  /* extracted from 'P' branch */
extern void ScreenshotToBmpAutoIncrement(void);  /* 'B' branch */
extern void FlushQueuedClicks(void);

/* ProcessGameFrameTick — original per-tick sequence:
 *
 *   1. Pump events (PeekMessage / WaitMessage if backgrounded)
 *   2. Screenshot keys ('P' = PCX, 'B' = BMP)
 *   3. Composite + blit the frame
 *   4. Cursor sprite update (inventory pickup)
 *   5. Bottom-panel hit-test → g_hover_panel_verb
 *   6. Mouse hit-test → g_hover_scene_verb
 *   7. UpdateActorMovement — drive g_actor[] walkers per cursor
 *   8. Per-entity VM tick (ExecEntityScript)
 *   9. Prop / dialogue tick (deferred)
 *  10. EntityRenderAll with z-sort
 *  11. Drain click_queue — FlushQueuedClicks → DispatchClickEvent
 *
 * Blocking script ops (op 0x12 ANIM_BOTH_WAIT, op 0x14 WAIT_MS,
 * op 0x15 WAIT_ENTITY) loop on this; without driving the walker +
 * render here, those waits visually freeze the scene. */
/* Forward decl for PaintHudOverlay use. */
void paint_anim_button_at(AnimAsset *atlas, uint16_t frame,
                          int16_t base_x, int16_t base_y, int paint);
/* Forward decl for ProcessGameFrameTickInner use. */
void HandleSceneInput(void);
int paint_rawb_pic(const void *blob, uint32_t size, int as_overlay);

/* Externs into sibling TUs. */
extern void PaintHudOverlay(void);
extern void ProcessGameFrameTickInner(void);
extern void PaintCursor(void);
extern void UpdateCursorState(void);
extern int  PreloadCommonAssets(void);
extern int  is_walkable_at(int sx, int sy);
extern int  ClickHitTest(int16_t mouse_x, int16_t mouse_y, uint16_t *out_verb);
extern int  g_no_pacing;                /* main.c — --no-pacing flag */
extern const void *PeLoaderRead(uint32_t va);

/* ------------------------------------------------------------------------- *
 * DispatchClickEvent
 *
 * SCUMM-style verb/noun dispatcher. The per-stage descriptor at
 * g_actor_walk_anim_table (= the stage-table entry for g_cur_etap)
 * carries two tables of 6-byte entries:
 *
 * +0x04 verb_table = { u16 verb_id; u32 script_ptr; } *
 * +0x08 object_table = { u16 obj_id; u32 script_ptr; } *
 *
 * - terminator is the first entry with id == 0
 * - entries are 6 bytes (the 2 bytes between u16 and u32 are unused)
 *
 * Flow: search verb_table for verb_id, run that script — if it returns
 * non-zero, *also* search object_table for obj_id and run that script.
 *
 * The original reads both tables out of static PE memory as absolute
 * pointers. The port resolves them through PeLoaderRead, then xlats
 * the script pointer either to a manually-embedded blob in
 * binary_data.c or back to PE memory. */
uint32_t g_stage_va = 0;            /* original PE VA of current stage def */
uint16_t g_held_item = 0x26;        /* currently held inventory item
                                     * (0x26 = neutral / nothing held —
                                     * the post-dispatch reset in
                                     * RunGameStageLoop writes 0x26). */
extern const void *xlat_binary_ptr(uint32_t addr);

/* Read 6-byte entry id+ptr from PE memory at table_va + idx*6. */
int read_dispatch_entry(uint32_t table_va, int idx,
                               uint16_t *out_id, uint32_t *out_script_va)
{
    extern const void *PeLoaderRead(uint32_t va);
    const uint8_t *p = (const uint8_t *)PeLoaderRead(table_va + (uint32_t)idx * 6u);
    if (!p) return 0;
    *out_id        = (uint16_t)(p[0] | (p[1] << 8));
    *out_script_va = (uint32_t)(p[2] | (p[3] << 8) | (p[4] << 16) | (p[5] << 24));
    return 1;
}

/* ---- LoadStage --------------------------------------------------- */

extern uint32_t g_stage_va_table[5];                 /* stubs.c — T26 */
extern void     LoadActorWalkAnims(uint32_t stage_va); /* stubs.c */

int LoadStage(uint16_t stage)
{
    if (stage == 0) return 0;
    int idx = stage - 1;
    if (idx >= 5 || !g_stage_table[idx]) return 0;
    g_stage    = g_stage_table[idx];
    g_cur_etap = stage;
    /* T26: also propagate the raw PE VA so subsequent LoadKomnata /
     * DispatchClickEvent / LoadActorWalkAnims pick the right stage
     * descriptor. play_demo_scene's hard-coded stage-1 fallback VA
     * still applies for the demo entry path. */
    g_stage_va = g_stage_va_table[idx];
    LoadActorWalkAnims(g_stage_va);

    /* ResetInventory — clear g_inventory + page=0. Original calls
     * this before loading the new stage's panel + palette so any
     * held-from-previous-stage items get dropped. */
    ResetInventory();

    /* find Wacky.scr section for "[etap]N" */
    char buf[2] = { (char)('0' + g_cur_etap), 0 };
    FindScriptByStageAndRoom(g_scripts_obj, buf, "[komnata]");

    /* Load stage panel. The asset has 6 frames (one per button slot)
     * sharing a single (panel_x, panel_y) origin on its 0th frame,
     * used by PanelHitTest to convert cursor coords into panel-local
     * space.
 *
 * T27: each stage has its own panel.wyc / panel2.wyc / panel3.wyc /
 * panel4.wyc (stage 5 = credits, panel=NULL). Free any previously
 * loaded panel before reload to avoid leaking the old asset.
 * Note: ebek.wyc + fjej.wyc are the SAME filename across stages
 * 1-4 (verified via BuildStageTable log), so g_ebek_atlas /
 * g_fjej_atlas remain singletons loaded once in PreloadCommonAssets.
 *
 * Stage 5 (Monter finale = ACME assembly + end credits) has
 * panel_wyc/ebek_wyc/fjej_wyc all NULL — it's a cutscene-only
 * "stage" with no HUD and no playable actors. Without the
 * NULL-side branch below, g_panel_asset / g_actor[0] / g_actor[1]
 * kept their stage 4 values, so PaintHudOverlay + EntityRenderAll
 * still drew the panel and Ebek/Fjej during the finale.
 * actor-list management gated on komnata flag
 * g_komnata_flags & 2 — when the new stage's atlas slot is NULL, the
 * actor must be unlinked from the render list. */
    if (g_stage->panel_wyc) {
        if (g_panel_asset) {
            FreeAsset(g_panel_asset);
            g_panel_asset = NULL;
        }
        g_panel_asset = LoadAssetFromDtaBase(g_stage->panel_wyc);
        if (!g_panel_asset) return 0;
    } else if (g_panel_asset) {
        FreeAsset(g_panel_asset);
        g_panel_asset = NULL;
    }
    {
        extern void UnlinkEntity(Entity *e);
        if (!g_stage->ebek_wyc && g_actor[0]) {
            UnlinkEntity(g_actor[0]);
            g_actor[0] = NULL;
        }
        if (!g_stage->fjej_wyc && g_actor[1]) {
            UnlinkEntity(g_actor[1]);
            g_actor[1] = NULL;
        }
    }
    /* Clear "HUD visible" bit immediately when the new stage has no
 * panel — needed BEFORE play_demo_scene runs (which clears it again
 * later) because LoadKomnata (called from LoadKomnataScene inside
 * play_demo_scene's prologue) executes 2 embedded ProcessGameFrameTick
 * iterations as part of its enter_va + second_va run. Those ticks
 * call PaintHudOverlay which gates portrait + pasek paints on this
 * bit only (not on g_panel_asset). Leaving the bit at the previous
 * stage's value = 1 would draw stale portraits + health bar for two
 * frames before play_demo_scene's `&= ~1u` later in the prologue —
 * visible as the "HUD flash" right after picking the ACME button. */
    if (!g_stage->panel_wyc) g_komnata_flags &= ~1u;
    /* load palette */
    if (g_stage->paleta_pal) {
        void *pal = NULL; uint32_t n;
        if (!LoadFileFromDta(g_stage->paleta_pal, &pal, &n)) return 0;
        memcpy(g_palette_rgb, pal, sizeof g_palette_rgb);
    }
    /* enter the start room */
    g_cur_komnata = g_stage->start_komnata;
    PaletteFadeInOut(100, g_palette_rgb, 0, 0, NULL);
    return 1;
}

/* ---- RunMenuScene ------------------------------------------------ *
 *
 * Condensed, faithful implementation; the original was much more
 * verbose. */
extern AnimAsset *LoadAssetFromDtaBase(const char *);

/* HandleMainMenuClick — on_click callback for the main menu's
 * SceneDef. Trigger values:
 *   0     INIT — load palettes (Tlo.pal / menu.pal) and start
 *               playback of Dane_01.dta (the menu music)
 *   0x12  Load-game submenu       (returns 2 / 3 / 5)
 *   0x13  New game                (returns 6)
 *   0x14  Options/save toggle      (sets g_save_request)
 *   0x15  Quit                    (returns 8)
 *   0x16  Credits                 (returns 9)
 *
 * Per-frame work runs regardless of trigger: advance the title-
 * animation frame and blit it. We don't yet have the full original
 * animation pipeline, so the implementation is dispatch-only. */

/* Forward decls for the menu-BG snapshot used by RunMenuScene's
 * overlay branch (definitions sit further down near
 * paint_slot_list). */
/* Non-static so src/menu/slot_picker.c can read it from the save-
 * commit path (CapturePendingThumbnail writes here before opszyns
 * opens). */
uint8_t g_save_thumb_pending[SAVE_THUMB_W * SAVE_THUMB_H];

extern void SnapshotBackbufferForMenu(void);

/* "asset (kind=1, id=10)" hook used by HandleMainMenuClick — set by
 * RunMenuScene to the active menu's mask atlas so the on_click
 * handler can fetch it for its title-animation block. */
AnimAsset *g_menu_asset_10 = NULL;



/* play_demo_scene — single-call scene loop (T22 phase B). Initial
 * komnata is loaded via LoadKomnataScene at prologue; subsequent
 * transitions happen IN-PLACE via ScriptGoToKomnata (op 0x20) without
 * unwinding the main loop. Returns NULL on quit (ESC / F12 TAK). */
extern const char *play_demo_scene(const DemoScene *scene); /* scene/play_loop.c */

/* HandleSceneInput externs (forward decls so the helper can call them). */
extern int  BindActorWalker(int actor_idx, int target_x, int target_y);
extern void DispatchClickEvent(uint16_t obj_id, uint16_t verb_id);

/* Run the embedded enter_script for each scene through the bytecode
 * VM. Spawns the per-room NPCs (drut/barstoi/pies in maluch,
 * babcia/deska/domofon in klatka2, pijaki/ptak in kiosk21,
 * dziewczynki/hustawki in plac) so EntityRenderAll has actual
 * entities to draw. */
extern Entity *g_render_list_head;
extern void    EntityWalkerTick(Entity *head);
extern void    EntityRenderAll (Entity *head);

/* ---- RunGameStageLoop -------------------------------------------- *
 *
 * Original control flow:
 *   1. Zero script_vars + entity_state (FULL_RESET path)
 *   2. ResetInventory
 *   3. Clear entity / mask lists, reset panel
 *   4. if (stage && !(flags & 0x10)) PlaySceneCutsceneAvi(intro_avi)
 *   5. LoadKomnata(g_cur_komnata)
 *   6. Per-frame loop:
 *      - dispatch clicks (use-on-item + actor-toggle branch)
 *      - save UI if requested
 *      - SPACE mid-frame actor toggle
 *      - ProcessGameFrameTick
 *      - game-over handling (cases 1/3/4)
 *   7. Exit when exit_signal is set.
 *
 * Flag bits:
 *   0x02 = FULL RESET (new game): zero vars + ResetInventory + LoadStage
 *   0x10 = SKIP INTRO AVI (came from save load)
 *   0x11 combos used after F12 menu / save UI */

/* Forward externs for chapter-select state used by the chapter-pick
 * epilogue (defined in src/menu/chapter_select.c). */
extern SceneDef g_sel_tlo_scene;
extern int      s_chapter_pick;
extern int      SelTloRefreshButtons(void);

/* Entity-state table layout — FULL_RESET only zeroes the in_inventory_
 * flag field per entry, preserving the panel_verb_id identity mapping
 * seeded by PreloadCommonAssets. */
#define ENTITY_STATE_ENTRY_COUNT        0x8E
#define ENTITY_STATE_FIELDS_PER_ENTRY   4
#define ENTITY_STATE_INVENTORY_FIELD    1

/* Mirror the FULL_RESET (flag 0x02) cleanup: zero script vars, clear
 * each entity_state[i].in_inventory_flag, reset the inventory state.
 * Used by both RunGameStageLoop's flag-2 branch and the dev-flow
 * apply_full_reset in main_menu.c (which keeps a private copy to stay
 * decoupled from game.c). */
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

/* ---- RunGameStageLoop constants + helpers ------------------------- */

/* Stage-1 fallback: PE VA of stage 1's StageDef + its etap id. Used
 * when RunGameStageLoop was entered without FULL_RESET (so LoadStage(1)
 * hasn't run) AND without SAVE_LOAD (so LoadSaveSlot didn't restore
 * g_stage_va). Without the fallback, DispatchClickEvent can't find
 * the verb tables and clicks silently no-op. */
#define STAGE_FALLBACK_VA               0x00428220u
#define STAGE_FALLBACK_ETAP             1
#define KOMNATA_FALLBACK                1

/* Stage-end AVI filenames. DEATH and CREDITS_STING are global to the
 * RunGameStageLoop epilogue; per-stage outros + transitions live on
 * the StageDef as alt_avi / alt3_avi. */
#define DEATH_AVI                       "Dane_14.dta"
#define CREDITS_STING_AVI               "Dane_13.dta"

/* `g_completed_stages` is a 32-bit bitmask of finished etaps. We only
 * set a bit if g_cur_etap fits in the mask. */
#define COMPLETED_STAGES_BITMASK_MAX    32

/* Initialize stage state on entry. FULL_RESET (flag 0x02) zeroes script
 * vars + ResetInventory + LoadStage(1); SAVE_LOAD (flag 0x10) trusts
 * LoadSaveSlot's prior g_stage_va / g_cur_komnata restore but falls
 * back to stage-1 defaults if either was missed. */
static void prepare_stage_state(uint8_t flags)
{
    if (flags & STAGE_LOAD_FLAG_FULL_RESET) {
        apply_full_reset();
        LoadStage(STAGE_FALLBACK_ETAP);
    }
    if (flags & STAGE_LOAD_FLAG_SAVE_LOAD) {
        if (!g_stage_va) {
            g_stage_va  = STAGE_FALLBACK_VA;
            g_cur_etap  = STAGE_FALLBACK_ETAP;
        }
        if (g_cur_komnata == 0) g_cur_komnata = KOMNATA_FALLBACK;
    }
}

/* Demo-fallback: if neither flag bit ran, set g_stage_va to stage 1 so
 * DispatchClickEvent finds the verb tables. */
static void ensure_stage_va_default(void)
{
    if (!g_stage_va) {
        g_stage_va = STAGE_FALLBACK_VA;
        g_cur_etap = STAGE_FALLBACK_ETAP;
    }
}

/* Drive one stage's gameplay loop: load the start komnata, run
 * play_demo_scene until it returns, then clear entity list + silence
 * any looping per-room SFX. */
static void run_initial_komnata_scene(uint16_t cur_komnata)
{
    LoadKomnataScene(cur_komnata);
    if (!g_current_scene) {
        LOG_TRACE("scene", "RunGameStageLoop: LoadKomnataScene(%u) yielded no scene", cur_komnata);
        return;
    }

    (void)play_demo_scene((const DemoScene *)g_current_scene);

    extern void EntityListClearAll(void);
    extern void ResetFrameSfxState(void);
    EntityListClearAll();
    /* Silence any looping (N,M) SFX still mixing — the original frees
     * every per-komnata asset on gameplay exit; SampleTable destructors
     * stop their wavs. Without this, a [sampl] WAV (N,M) loop started
     * mid-scene keeps playing into the menu after ESC / Pytanie TAK. */
    ResetFrameSfxState();
}

/* Mark the current etap as completed in g_completed_stages. The
 * original sets this elsewhere (likely via a script-op write through
 * a var-indirection); we set it defensively in the epilogue so the
 * chapter-select map shows progress regardless of trigger source. */
static void mark_current_stage_completed(void)
{
    if (g_cur_etap >= 1 && g_cur_etap <= COMPLETED_STAGES_BITMASK_MAX) {
        g_completed_stages |= 1u << (g_cur_etap - 1);
        LOG_INFO("log", "[game-over=%d] stage %u completed (g_completed_stages=0x%X)", GAME_OVER_STAGE_END_AVI, (unsigned)g_cur_etap, g_completed_stages);
    }
}

/* Play one of the StageDef's per-stage AVIs if it's non-NULL. */
static void play_stage_avi_if(const char *avi, int code, const char *kind)
{
    if (!avi) return;
    LOG_INFO("log", "[game-over=%d] %s AVI: %s", code, kind, avi);
    PlaySceneCutsceneAvi(avi);
}

/* game_over_code == GAME_OVER_CHAPTER_PICK (3). Plays the just-finished
 * stage's outro, stashes its transition AVI, shows the chapter-select
 * map, loads the picked stage, then plays the stashed transition.
 *
 * 8-step order mirrors the original engine exactly. Step 4 (all_done →
 * credits sting + DROP into the map) intentionally does NOT bail so
 * the user can see the assembled ACME map and click the green finale
 * button — an earlier port `break`ed here and made stage 5 unreachable
 * from a regular playthrough. */
static void handle_game_over_chapter_pick(void)
{
    /* Step 1: refresh buttons; tells us whether all 4 stages are done. */
    int all_done = SelTloRefreshButtons();

    /* Step 2: outro of just-finished stage + stash alt3 for later. */
    const char *stashed_alt3 = NULL;
    if (!all_done && g_stage) {
        play_stage_avi_if(g_stage->alt_avi, GAME_OVER_CHAPTER_PICK, "outro");
        stashed_alt3 = g_stage->alt3_avi;
    }

    /* Step 3: re-refresh — belt-and-braces in case the AVI ran a script
     * op that flipped a completion bit. */
    all_done = SelTloRefreshButtons();

    /* Step 4: all stages done → credits sting, then drop into the map. */
    if (all_done) {
        LOG_INFO("log", "[game-over=%d] all stages done → %s + map with "
                        "green button", GAME_OVER_CHAPTER_PICK, CREDITS_STING_AVI);
        PlaySceneCutsceneAvi(CREDITS_STING_AVI);
    }

    /* Step 5: chapter-select menu. */
    s_chapter_pick = 0;
    RunMenuScene(0, &g_sel_tlo_scene);

    /* Step 6: load picked stage (1..4 = regular, 5 = Monter finale). */
    if (s_chapter_pick >= 1 && s_chapter_pick <= DEV_PICK_FINALE) {
        LOG_INFO("log", "[game-over=%d] LoadStage(%d)", GAME_OVER_CHAPTER_PICK, s_chapter_pick);
        LoadStage((uint16_t)s_chapter_pick);
    }

    /* Step 7: third pass — post-load button rebuild. */
    all_done = SelTloRefreshButtons();

    /* Step 8: intro AVI of newly-loaded stage (the prev stage's "going
     * to X" transition stashed in step 2). Skipped on finale path. */
    if (!all_done && stashed_alt3) {
        play_stage_avi_if(stashed_alt3, GAME_OVER_CHAPTER_PICK, "transition");
    }
}

/* game_over_code == GAME_OVER_STAGE_END_AVI (4). Plays both the outro
 * and transition AVIs back-to-back without the chapter-select menu —
 * used when stage-end is "automatic" (no player choice). */
static void handle_game_over_stage_end_avi(void)
{
    if (g_stage) {
        play_stage_avi_if(g_stage->alt_avi,
                          GAME_OVER_STAGE_END_AVI, "outro");
        play_stage_avi_if(g_stage->alt3_avi,
                          GAME_OVER_STAGE_END_AVI, "transition");
    }
    mark_current_stage_completed();
}

/* Post-loop game-over dispatcher: 1=death AVI, 3=chapter-pick UI,
 * 4=stage-end double AVI + completion bit. Unknown codes are no-ops. */
static void run_game_over_epilogue(void)
{
    switch (g_game_over_code) {
    case GAME_OVER_DEATH:
        PlaySceneCutsceneAvi(DEATH_AVI);
        return;
    case GAME_OVER_CHAPTER_PICK:
        handle_game_over_chapter_pick();
        return;
    case GAME_OVER_STAGE_END_AVI:
        handle_game_over_stage_end_avi();
        return;
    }
}

void RunGameStageLoop(uint8_t flags)
{
    g_game_over_code = GAME_OVER_NONE;

    prepare_stage_state(flags);

    extern uint32_t g_tick_counter;
    extern void     LoadActorWalkAnims(uint32_t stage_va);

    g_stats.boot_tick = g_tick_counter;
    ensure_stage_va_default();
    LoadActorWalkAnims(g_stage_va);

    uint16_t cur_komnata = (g_cur_komnata != 0) ? g_cur_komnata
                                                : KOMNATA_FALLBACK;
    g_cur_komnata = cur_komnata;

    run_initial_komnata_scene(cur_komnata);

    if (g_game_over_code) run_game_over_epilogue();
}

/* ---- InitializeGameSubsystems ------------------------------------ */
int InitializeGameSubsystems(void)
{
    if (InitializeDirectSound() != 0) {
        PlatformShowMessageBox("Wacki",
            "Program wymaga Direct Sound w wersji 5.0 lub nowszej.");
        return 0;
    }

    /* main archive — try local pwd first, then CD path */
    int opened = OpenDtaArchiveFile("Dane_02.dta");
    if (!opened) {
        char buf[280];
        snprintf(buf, sizeof buf, "%s/Dane_02.dta", g_data_root);
        opened = OpenDtaArchiveFile(buf);
    }
    if (!opened) {
        LOG_INFO("log", "\nBrak pliku bazy : Dane_02.dta (na CD: %s)", g_data_root);
        PlatformShowMessageBox("Wacki",
            "Nie znaleziono Dane_02.dta — sprawd\xC5\xBA p\xC5\x82yt\xC4\x99 CD.");
        return 0;
    }
    LOG_INFO("init", "mounted archive Dane_02.dta");

    g_items_obj     = malloc(sizeof(struct ScriptObj));
    g_scripts_obj   = malloc(sizeof(struct ScriptObj));
    g_dialogues_obj = malloc(sizeof(struct ScriptObj));
    if (g_items_obj)     memset(g_items_obj,     0, sizeof(struct ScriptObj));
    if (g_scripts_obj)   memset(g_scripts_obj,   0, sizeof(struct ScriptObj));
    if (g_dialogues_obj) memset(g_dialogues_obj, 0, sizeof(struct ScriptObj));
    /* Item.scr has its OWN format ([N]filename per line, not the standard
 * [tag]<body> ScriptObj layout) — parsed by LoadItemNamesTable into
 * a fixed-width name table. The g_items_obj generic-ScriptObj load
 * stays for any code that still treats it as a ScriptObj, but the
 * voice-over uses the dedicated table. */
    LoadScriptFile(g_items_obj,     "Item.scr");
    LoadItemNamesTable();
    LoadScriptFile(g_scripts_obj,   "Wacky.scr");
    LoadScriptFile(g_dialogues_obj, "Gadki.scr");

    if (!PreloadCommonAssets())
        LOG_INFO("init", "some resident assets missing — continuing");

    extern int InitializeMmTimer(void *);
    static uint8_t mmt[32];
    InitializeMmTimer(mmt);
    return 1;
}
