/*
 * game.c — top-level game state machine, scene runner, click dispatcher,
 * per-frame tick.
 *
 * Original addresses:
 * InitializeGameSubsystems 0x00403A30
 * PreloadCommonAssets 0x00403790
 * RunMainGameLoop 0x0040BBF0
 * RunGameStageLoop 0x0040BEA0
 * RunMenuScene 0x0040B5E0
 * LoadStage 0x00403320
 * ProcessGameFrameTick 0x004025C0
 * DispatchClickEvent 0x004094A0
 * ScreenshotPCX/BMP (debug 'P' / 'B' inside ProcessGameFrameTick)
 */
#include "wacki.h"
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
struct DemoScene;
const struct DemoScene *g_current_scene = NULL;     /* hotspot + walk-bounds */
const uint8_t *g_walk_fld_pixels = NULL;            /* .fld walkability bitmap (1bpp) */
int            g_walk_fld_w = 0,    g_walk_fld_h = 0;
int            g_walk_fld_ox = 0,   g_walk_fld_oy = 0;
int            g_walk_fld_stride = 0;
int            g_walk_x0 = 0, g_walk_x1 = 0;        /* fallback bbox (no .fld) */
int            g_walk_y0 = 0, g_walk_y1 = 0;
/* g_scene_quit — set by OpenOptionsMenu when Pytanie TAK confirms quit
 * mid-game. play_demo_scene's main loop polls it as a quit signal.
 * T35 removed g_pending_scene_exit (hotspot-click out-signal) entirely
 * after verb-driven exits via op 0x20 + LoadKomnataScene now work. */
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
extern void *g_items_obj;                       /* */
extern AnimAsset *g_panel_cursor;               /* g_panel_cursor (Krazek.pic) */

/* g_stage_table is defined in stubs.c — see PTR_PTR_00442FA8 in the binary. */

/* ------------------------------------------------------------------------- *
 * PreloadCommonAssets — 0x00403790
 *
 * Loaded once at startup. Holds the universally-shared sprites resident
 * in memory between stage swaps.
 * ------------------------------------------------------------------------- */
FontHandle *g_default_font = NULL;     /* — "Futura.30" bitmap font */

extern AnimAsset *g_items_atlas;       /* g_items_atlas — przedm.wyc */

/* T4 step 1: actor atlas singletons. Loaded once at startup, persist
 * across all scene transitions. Original engine spawns Ebek/Fjej once
 * at game start with these atlases bound; verb scripts post-GO_EXIT
 * can reposition the SAME actor entity without atlas reload. */
AnimAsset *g_ebek_atlas = NULL;        /* ebek.wyc — Ebek sprite frames */
AnimAsset *g_fjej_atlas = NULL;        /* fjej.wyc — Fjej sprite frames */
AnimAsset *g_ebfj_atlas = NULL;        /* g_ebfj_atlas — ebfj.wyc actor-portrait
 atlas (4 frames: 0/1 = Ebek/Fjej
 active w/ frame, 2/3 = inactive). */

/* T31 — cursor state atlases. PreloadCommonAssets which
 * loaded these into g_cursor_atlas..0x004514A4 then 's state
 * table @ indexed them per cursor state (0..7).
 *
 * Naming note: in the original PE these slots are loaded with
 * olowek1.wyc / kaseta.wyc / magnes1*.wyc / drzwi1*.wyc — but they're
 * NOT the puzzle items. They're CURSOR SPRITE ATLASES with cursor-
 * shaped frames (the names just happened to be assigned to the slots
 * in PreloadCommonAssets; in RE we see them used as cursor only by
 * ). The 8-state cursor anim table maps states 0..7 to
 * indices into the slot array. */
AnimAsset *g_cursor_atlas[8] = {0};    /* g_cursor_atlas..0x004514A4 */
uint8_t    g_cursor_state    = 0;      /* g_script_running */
uint16_t   g_cursor_frame    = 0;      /* cursor_state_struct + 0x30 */
uint16_t   g_cursor_frame_acc = 0;     /* cursor_state_struct + 0x3C accumulator */

extern void BuildStageTable(void);                /* stubs.c — T26 */

/* PreloadCommonAssets moved to src/scene/preload.c. */

/* ------------------------------------------------------------------------- *
 * ProcessGameFrameTick — 0x004025C0
 * ------------------------------------------------------------------------- */
extern void ScreenshotToPcxAutoIncrement(void);  /* extracted from RE'd 'P' branch */
extern void ScreenshotToBmpAutoIncrement(void);  /* 'B' branch */
/* UpdateAllEntities removed — its responsibilities are split between
 * EntityWalkerTick (per-entity VM ticks) and EntityRenderAll (z-sorted
 * render), both wired into ProcessGameFrameTick. */
extern void FlushQueuedClicks(void);

/* ProcessGameFrameTick —
 *
 * Original sequence:
 * PeekMessage / WaitMessage if backgrounded
 * screenshot keys ('P' PCX, 'B' BMP)
 * — composite/blit frame
 * — cursor sprite update (inventory pickup)
 * — bottom-panel hit-test (sets g_hover_panel_verb verb)
 * — mouse hit-test (sets g_hover_scene_verb hover_verb)
 * UpdateActorMovement — drives g_actor[] walkers per cursor
 * — per-entity VM tick (ExecEntityScript)
 * / 00406EB0— prop/dialogue tick (deferred)
 * — EntityRenderAll with z-sort
 * drain click_queue — FlushQueuedClicks → DispatchClickEvent
 *
 * Blocking script ops (op 0x12 ANIM_BOTH_WAIT, op 0x14 WAIT_MS, op 0x15
 * WAIT_ENTITY) loop on this; without driving the walker + render here,
 * those waits visually freeze the scene. */
/* Forward decl for PaintHudOverlay use. */
void paint_anim_button_at(AnimAsset *atlas, uint16_t frame,
                          int16_t base_x, int16_t base_y, int paint);
/* Forward decl for ProcessGameFrameTickInner use. */
void HandleSceneInput(void);
int paint_rawb_pic(const void *blob, uint32_t size, int as_overlay);

/* Functions moved to other TUs but still called from game.c. */
extern void PaintHudOverlay(void);
extern void ProcessGameFrameTickInner(void);
extern void PaintCursor(void);
extern void UpdateCursorState(void);
extern int  PreloadCommonAssets(void);
extern int  is_walkable_at(int sx, int sy);
extern int  ClickHitTest(int16_t mouse_x, int16_t mouse_y, uint16_t *out_verb);
extern int  g_no_pacing;                /* main.c — --no-pacing flag */
extern const void *PeLoaderRead(uint32_t va);

/* Cursor state + paint (UpdateCursorState, PaintCursor) moved to src/hud/cursor.c. */

/* PaintHudOverlay moved to src/scene/hud_paint.c. */

/* ProcessGameFrameTick(Inner) moved to src/scene/frame_tick.c. */

/* ------------------------------------------------------------------------- *
 * DispatchClickEvent
 *
 * SCUMM-style verb/noun dispatcher. The per-stage descriptor at
 * g_actor_walk_anim_table (= the entry from PTR_PTR_00442FA8[etap-1]) carries two
 * tables of 6-byte entries:
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
 * Original reads both tables out of static PE memory (g_actor_walk_anim_table =
 * absolute pointer). The port resolves them through PeLoaderRead, then
 * xlats the script pointer either to a manually-embedded blob in
 * binary_data.c or back to PE memory.
 * ------------------------------------------------------------------------- */
uint32_t g_stage_va = 0;            /* g_actor_walk_anim_table — original VA of current stage def */
uint16_t g_held_item = 0x26;        /* g_held_item — currently held inventory item
 * (0x26 = neutral / nothing held; see
 * RunGameStageLoop @ 0x0040C0C6 where the
 * post-dispatch reset writes 0x26). */
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

/* find_dispatch_script + DispatchClickEvent moved to src/scene/dispatch.c. */

/* ------------------------------------------------------------------------- *
 * LoadStage — 0x00403320
 * ------------------------------------------------------------------------- */
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
 * descriptor. play_demo_scene's hard-coded stage-1 assignment
 * (0x00428220) is kept as a fallback for the demo entry path. */
    g_stage_va = g_stage_va_table[idx];
    LoadActorWalkAnims(g_stage_va);

    /*:
 *; // ResetInventory — clear g_inventory + page=0
 * Original calls this before loading the new stage's panel + palette
 * so any held-from-previous-stage items get dropped. */
    ResetInventory();

    /* find Wacky.scr section for "[etap]N" */
    char buf[2] = { (char)('0' + g_cur_etap), 0 };
    FindScriptByStageAndRoom(g_scripts_obj, buf, "[komnata]");

    /* load stage panel —:
 * The asset has 6 frames (one per button slot) sharing a single
 * (panel_x, panel_y) origin on its 0th frame, used by PanelHitTest
 * to convert cursor coords into panel-local space.
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
 * g_settings_anim_active & 2 — when the new stage's atlas slot is NULL, the
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
    if (!g_stage->panel_wyc) g_settings_anim_active &= ~1u;
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

/* ------------------------------------------------------------------------- *
 * RunMenuScene — 0x0040B5E0
 *
 * A condensed, faithful implementation; the original was much more verbose.
 * ------------------------------------------------------------------------- */
extern AnimAsset *LoadAssetFromDtaBase(const char *);

/* ------------------------------------------------------------------------- *
 * HandleMainMenuClick — port of (the on_click callback for the
 * main menu's SceneDef at ).
 *
 * Param 1 is a trigger value:
 * 0 INIT — load palettes (Tlo.pal/menu.pal) and start CD-audio
 * playback of Dane_01.dta (the menu music).
 * 0x12 Load-game submenu (returns 2/3/5)
 * 0x13 New game (returns 6)
 * 0x14 Options/save toggle (sets g_save_request)
 * 0x15 Quit (returns 8)
 * 0x16 Credits (returns 9)
 *
 * Per-frame work runs regardless of trigger: advance the title-animation
 * frame and blit it. We don't yet have CD-audio or the full animation
 * pipeline, so this is reduced to the dispatch-only logic.
 */
static uint8_t s_menu_flags    = 0;   /* */
static int     s_anim_delay    = 0;   /* anim_delay_counter (anim delay counter) */
static int     s_anim_frame    = 10;  /* anim_frame_index (anim frame index) */
static int     s_save_request  = 0;   /* */

/* Forward decls for the menu-BG snapshot used by RunMenuScene overlay
 * branch (see comments at the definitions below near paint_slot_list).
 * Declared early because RunMenuScene at 0x0040B5E0 lives above the
 * save/load menu code that owns them. */
#define SAVE_THUMB_W 126
#define SAVE_THUMB_H 78
static uint8_t g_save_thumb_pending[SAVE_THUMB_W * SAVE_THUMB_H];
static uint8_t g_menu_bg_snapshot[WACKI_SCREEN_W * WACKI_SCREEN_H];
static int     g_menu_bg_snapshot_valid = 0;

/* Snapshot whatever is currently in the backbuffer so the next
 * overlay RunMenuScene can paint its .pic on top of it (margins
 * stay coherent). Called before opening sub-fullscreen overlay
 * menus (OpenOptionsMenu, main-menu Load). No-op until shadow is
 * allocated. */
static void SnapshotBackbufferForMenu(void)
{
    extern uint8_t *g_back_shadow;
    if (!g_back_shadow) return;
    memcpy(g_menu_bg_snapshot, g_back_shadow,
           (size_t)WACKI_SCREEN_W * WACKI_SCREEN_H);
    g_menu_bg_snapshot_valid = 1;
}

/* "asset (1, 10)" hook used by HandleMainMenuClick — set in RunMenuScene
 * to the mask atlas asset (matching the engine's RegisterEntityForUpdate
 * call at 0x0040B5E0). */
AnimAsset *g_menu_asset_10 = NULL;

/* ---- Title + main-menu constants ---------------------------------- */

/* Title-screen filenames (Tlo.pal background palette, Tlo.wyc button
 * mask, menu.pal blended-fade buffer the original cross-fades into,
 * Dane_01.dta looping menu BGM). */
#define TITLE_PALETTE_FILENAME          "Tlo.pal"
#define TITLE_MASK_FILENAME             "Tlo.wyc"
#define TITLE_BLEND_PALETTE_FILENAME    "menu.pal"
#define TITLE_INTRO_AVI                 "Dane_10.dta"
#define CREDITS_AVI                     "Dane_12.dta"
#define MAIN_MENU_BGM_FILENAME          "Dane_01.dta"

/* Title-screen button triggers (Tlo.wyc mask). Frames in the .wyc are
 * laid out so that hover frame = def frame + TITLE_HOVER_FRAME_OFFSET.
 * Note MALUCH (0x14) is the "click the Maluch to start a new game"
 * button — an earlier RE annotation called it "Options" but the
 * dispatch (s_save_request → rc=PROLOGUE) makes it the prologue start. */
#define TITLE_BTN_LOAD                  0x12
#define TITLE_BTN_NEW                   0x13
#define TITLE_BTN_MALUCH                0x14
#define TITLE_BTN_QUIT                  0x15
#define TITLE_BTN_CREDITS               0x16
#define TITLE_HOVER_FRAME_OFFSET        5
#define TITLE_BUTTON_COUNT              5

/* The engine's "neutral / no verb" sentinel — sprinkled through every
 * SceneDef-walking helper and through HandleSceneInput. 0x26 was the
 * original engine's default fall-through value for verb dispatch. */
#define SCENE_NEUTRAL_VERB              0x26

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
#define MAIN_MENU_RC_HARD_QUIT          99

/* WACKI-logo flipbook in the title's mask atlas (sel_guz.wyc). Frames
 * 10..(count-1) form the animated logo; FRAME_DOORS_FIRST..DOORS_LAST
 * are the "doors closing" pose during which the Maluch-click latch
 * is NOT honoured (let the doors finish before transitioning). */
#define MAIN_MENU_ANIM_FIRST_FRAME      10
#define MAIN_MENU_ANIM_DOORS_FIRST      0x0F
#define MAIN_MENU_ANIM_DOORS_LAST       0x12
#define MAIN_MENU_ANIM_TICKS_PER_FRAME  6

/* s_menu_flags bit 0 — set on every HandleMainMenuClick call and
 * cleared once the rc has switched to a "leave the menu" code. */
#define MAIN_MENU_FLAG_LATCH            0x01u

/* Pytanie (Y/N) quit-confirm. */
#define PYTANIE_TRIGGER_TAK             0x12
#define PYTANIE_TRIGGER_NIE             0x13
#define PYTANIE_RC_TAK                  3
#define PYTANIE_RC_NIE                  4
#define PYTANIE_FRAME_NONE              0xFFFF

/* RunGameStageLoop flag bits. */
#define STAGE_LOAD_FLAG_FULL_RESET      0x02
#define STAGE_LOAD_FLAG_SAVE_LOAD       0x10

/* Entity-state table layout (flag-2 FULL RESET only zeroes the
 * in_inventory_flag field per entry, preserving panel_verb_id). */
#define ENTITY_STATE_ENTRY_COUNT        0x8E
#define ENTITY_STATE_FIELDS_PER_ENTRY   4
#define ENTITY_STATE_INVENTORY_FIELD    1

/* g_game_over_code progress signals + the "user-confirmed quit to main
 * menu" sentinel set by ESC / F12→TAK / OPCJE→Quit. Anything other than
 * NONE / DEATH / CHAPTER_PICK / STAGE_END_AVI is treated as a user-quit
 * intent by the dev start-stage flow. */
#define GAME_OVER_NONE                  0
#define GAME_OVER_DEATH                 1
#define GAME_OVER_USER_QUIT             2
#define GAME_OVER_CHAPTER_PICK          3
#define GAME_OVER_STAGE_END_AVI         4

/* Dev-mode --start-stage clamps + the "Monter finale" pick value. */
#define DEV_START_STAGE_MIN             1
#define DEV_START_STAGE_MAX             5
#define DEV_PICK_FINALE                 5

/* ------------------------------------------------------------------------- *
 * HandlePytanieClick — on_click for the "Pytanie?" quit-confirmation
 * SceneDef. The two buttons are TAK (yes, quit) and NIE (no, keep
 * playing); the caller treats PYTANIE_RC_TAK as "quit confirmed". */
static int HandlePytanieClick(int trigger)
{
    if (trigger == PYTANIE_TRIGGER_TAK) return PYTANIE_RC_TAK;
    if (trigger == PYTANIE_TRIGGER_NIE) return PYTANIE_RC_NIE;
    return MAIN_MENU_RC_NONE;
}

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

/* INIT-trigger handler — runs once when the title is first entered:
 * clear the backbuffer, install both palettes, reset the flipbook
 * counters, and start the looping menu BGM. */
static void enter_main_menu(void)
{
    FlipBuffersClearWith(0);
    FlushFrameToPrimary();
    install_main_menu_palettes();
    s_anim_frame   = MAIN_MENU_ANIM_FIRST_FRAME;
    s_anim_delay   = 0;
    s_save_request = 0;
    PlayMenuMusic(MAIN_MENU_BGM_FILENAME, 1);
}

/* TITLE_BTN_LOAD handler — runs the Load-game sub-menu and translates
 * its rc into the appropriate main-menu rc. */
static int dispatch_load_submenu(void)
{
    extern SceneDef g_load_menu_scene;
    /* Snapshot the title-screen backbuffer so the Load overlay's
     * margins show the title art instead of an uninitialised buffer
     * (palette index 0 in the new palette can be white/garbage). */
    SnapshotBackbufferForMenu();
    int r = RunMenuScene(1, &g_load_menu_scene);
    return (r == 3) ? MAIN_MENU_RC_LOAD_SAVE : MAIN_MENU_RC_BACK_TO_MAIN;
}

/* Returns true once the Maluch-click latch is set AND the title
 * flipbook is OUTSIDE the "doors closing" frames — when both hold,
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
                fprintf(stderr, "[anim] frame=%d at (%u,%u) %ux%u\n",
                        s_anim_frame, dx, dy, w, h);
                ++once;
            }
            BlitSpriteToBackbuffer(dx, dy, 0, 0, w, h, w, h,
                                   a->pixel_ptrs[s_anim_frame], 0);
        }
        ++s_anim_frame;
        /* ~10 fps at the 60 fps menu pacing — one new frame every
         * MAIN_MENU_ANIM_TICKS_PER_FRAME (≈100 ms). */
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
     * PROLOGUE so RunMainGameLoop starts a new playthrough. The frame
     * gate gives the title animation time to finish its rotation. */
    if (maluch_latch_ready_to_fire()) {
        rc = MAIN_MENU_RC_PROLOGUE;
        s_menu_flags &= ~MAIN_MENU_FLAG_LATCH;
        s_anim_delay = 1;
    }

    if (trigger > MAIN_MENU_TRIGGER_INIT) {
        fprintf(stderr, "[menu] click trigger=0x%02X rc=%d\n", trigger, rc);
    }

    tick_title_animation();

    /* Any non-zero rc means we're leaving the menu — stop the BGM so
     * it doesn't bleed into the next scene. */
    if (rc != MAIN_MENU_RC_NONE) StopMenuMusic();
    return rc;
}

/* Decode a .pic ("RAWB") backbuffer:
 * +0 uint32 magic = 'RAWB'
 * +4 uint16 width (LE)
 * +6 uint16 height (LE)
 * +8 uint8 palette[256*3]
 * +776 uint8 pixels[w*h]
 *
 * → : the image is centered using
 * drawX = (640 - w) / 2, drawY = (400 - h) / 2 (clipped to ≥ 0).
 * For fullscreen 640×480 that's (0, 0); for the 344×319 Pytanie dialog
 * that's (148, 40), matching the on-disk button coords (194,287) / (354,292).
 *
 * `as_overlay`:
 * 0 — fullscreen scene background: install the embedded palette
 * and paint opaque (the original raw-copy path).
 * 1 — dialog overlay: keep current palette and color-key index 0
 * so the underlying menu shows through (matches 
 * mode 0 — alpha-blend with transparent-on-0).
 * Returns 1 if it painted, 0 otherwise.
 */
/* MergePalette — fold a .pic file's embedded palette into the live one,
 * preserving entries the .pic has black (0,0,0) for.
 *
 * Why the merge (and not a straight InstallPalette like the original):
 *
 * The shipped scene .pic files only fill ~70 of 256 palette indices
 * with real colours; the other ~180 are (0,0,0) — placeholder. The
 * .pic's BG pixels only use those ~70 "filled" indices. But the SPRITE
 * assets loaded into the same scene (drut, barstoi, pies, pijaki,
 * kufle, …) use a much wider range that paleta.pal sets up — including
 * the earth-tone / green ramp at indices 32..47 (greens 32-39, yellow
 * 40, oranges 41-46, red 47), the pink/violet ramp at 23-25, the
 * teal/blue ramp at 53-55, etc. If we naïvely InstallPalette the
 * .pic header, those sprite colours get overwritten with (0,0,0) and
 * the corresponding sprite pixels render BLACK on the user's screen.
 *
 * The original engine never installs the .pic palette directly — it
 * loads the .pic as a kind=3 entity and its renderer
 * leaves the live palette alone. So in practice paleta.pal stays the
 * active palette across all scenes, and per-room .pic palettes are
 * only there as a hint of which indices the artist actually used.
 *
 * Our port doesn't go through the entity-rendered BG path; we paint
 * the .pic pixels directly. So we MUST keep the .pic palette merged
 * in for any indices it sets that paleta.pal doesn't (kiosk21 fills
 * 11 new indices at 162..172, plac fills 16 more), while still
 * preserving paleta.pal entries where the .pic has black.
 *
 * This is a one-line merge: for each of the 256 RGB triplets in the
 * .pic palette, only overwrite g_palette_rgb if the .pic entry is
 * non-black. */
static void MergePalette(const uint8_t *src256_rgb)
{
    extern uint8_t g_palette_rgb[256*3];
    for (int i = 0; i < 256; ++i) {
        uint8_t r = src256_rgb[i*3 + 0];
        uint8_t g = src256_rgb[i*3 + 1];
        uint8_t b = src256_rgb[i*3 + 2];
        if (r | g | b) {
            g_palette_rgb[i*3 + 0] = r;
            g_palette_rgb[i*3 + 1] = g;
            g_palette_rgb[i*3 + 2] = b;
        }
    }
}

int paint_rawb_pic(const void *blob, uint32_t size, int as_overlay)
{
    if (size <= 776) return 0;
    const uint8_t *p = (const uint8_t *)blob;
    if (p[0]!='R' || p[1]!='A' || p[2]!='W' || p[3]!='B') return 0;
    uint16_t w = (uint16_t)(p[4] | (p[5] << 8));
    uint16_t h = (uint16_t)(p[6] | (p[7] << 8));
    if ((uint32_t)w * h + 776u > size) return 0;
    int dx = (WACKI_SCREEN_W - (int)w) / 2;
    int dy = (400              - (int)h) / 2;
    if (dx < 0) dx = 0;
    if (dy < 0) dy = 0;
    if (!as_overlay) {
        /* Fullscreen bg: MERGE palette (not overwrite), then opaque blit.
 * See MergePalette comment for the why — naïve install kills
 * the sprite-shared earth/green/orange colour ramps. */
        MergePalette(p + 8);
        PaintImageToBackbuffer((uint16_t)dx, (uint16_t)dy, w, h, p + 776);
    } else {
        /* Dialog overlay: color-key 0. Merge the .pic's palette so the
 * UI-color indices used by paint_slot_list (0x12 for slot text,
 * 0x01 for inset bg, 0xFE for inline-edit) resolve to whatever
 * the .pic's artist intended — otherwise 0x12 picks up whichever
 * palette happens to be active (red in menu.pal, dark blue in
 * gameplay palette), and the menu looks different depending on
 * where it was opened from. MergePalette only overwrites
 * non-black entries, so the underlying gameplay snapshot in the
 * margins keeps most of its colours. */
        MergePalette(p + 8);
        BlitSpriteToBackbuffer((uint16_t)dx, (uint16_t)dy, 0, 0,
                               w, h, w, h, (uint8_t *)(p + 776), 0);
    }
    return 1;
}

/* Per-asset scratch buffer for RLE-decoded frames (kind=3 rich ANIM).
 * Sized to the asset's max bounding box (max_w * max_h). Released by the
 * caller via paint_anim_release. */
static uint8_t *s_rle_scratch  = NULL;
static int      s_rle_scratch_sz = 0;

static uint8_t *get_rle_scratch(int sz)
{
    if (sz <= s_rle_scratch_sz) return s_rle_scratch;
    free(s_rle_scratch);
    s_rle_scratch    = (uint8_t *)malloc((size_t)sz);
    s_rle_scratch_sz = s_rle_scratch ? sz : 0;
    return s_rle_scratch;
}

/* Blit one frame of an ANIM atlas using its embedded hot-spot.
 * kind=3 ("rich") frames are RLE-compressed → decode into the scratch
 * buffer first, then blit raw. kind=2 ("passive") frames are already raw. */
void paint_anim_button_at(AnimAsset *atlas, uint16_t frame,
                          int16_t override_dx, int16_t override_dy,
                          int use_override)
{
    if (!atlas || frame >= atlas->frame_count || !atlas->pixel_ptrs) return;
    uint16_t w  = atlas->off_widths [frame];
    uint16_t h  = atlas->off_heights[frame];
    uint16_t dx = use_override ? (uint16_t)override_dx : atlas->off_drawX[frame];
    uint16_t dy = use_override ? (uint16_t)override_dy : atlas->off_drawY[frame];
    uint8_t *px = atlas->pixel_ptrs[frame];
    if (!px || w == 0 || h == 0) return;

    if (atlas->kind == 3) {
        int need = (int)w * (int)h;
        uint8_t *scratch = get_rle_scratch(need);
        if (!scratch) return;
        DepackRleFrame(px, scratch, need);
        px = scratch;
    }
    /* mode 0 = colour-key 0 (transparent) */
    BlitSpriteToBackbuffer(dx, dy, 0, 0, w, h, w, h, px, 0);
}

static void paint_anim_button(AnimAsset *atlas, uint16_t frame)
{
    paint_anim_button_at(atlas, frame, 0, 0, 0);
}

/* ------------------------------------------------------------------------- *
 * RunMenuScene — 0x0040B5E0
 *
 * Faithful to the RE'd binary:
 * scene->background_pic = optional full-screen .pic ("RAWB") — NULL means
 * "use whatever is already on screen" (the menu
 * overlays the last AVI frame).
 * scene->mask_file = ANIM atlas of clickable buttons (.wyc).
 * scene->buttons[] = (id, key, anim_frame) triples.
 * scene->flags = bitmask (SCENE_FLAG_*).
 *
 * Per frame we paint the background (if any), then each button sprite at its
 * own hot-spot, then present and poll input. ESC quits unless
 * SCENE_FLAG_DISABLE_ESC is set; clicks invoke on_click.
 * ------------------------------------------------------------------------- */
/* Hit-test: which scene button (if any) does (mx,my) fall on?
 *
 * Each button has two atlas frames: def_anim (always drawn) and hover_anim
 * (drawn on top when hovered). A def_anim of 0xFFFF means "no rest sprite"
 * (used by Pytanie — the dialog only shows TAK/NIE highlighted on hover);
 * in that case fall back to hover_anim's rect so the cursor still hits. */
static int hit_test_buttons(SceneDef *scene, AnimAsset *atlas,
                            int mx, int my)
{
    if (!atlas || !atlas->pixel_ptrs) return -1;
    for (int i = 0; i < scene->button_count; ++i) {
        /* Test both def_anim and hover_anim rects — some buttons store a
 * 1x1 placeholder in def_anim (e.g. Grafika.wyc exit at frame 8,
 * pos (0,0) size 1x1) and the actual clickable rect lives only in
 * the hover frame. Original uses the .msk mask for
 * pixel-perfect hit detection; we approximate with the union of
 * the two sprite bounding boxes. */
        uint16_t a_def = scene->buttons[i].def_anim;
        uint16_t a_hov = scene->buttons[i].hover_anim;
        for (int pass = 0; pass < 2; ++pass) {
            uint16_t a = (pass == 0) ? a_def : a_hov;
            if (a >= atlas->frame_count) continue;
            int w = atlas->off_widths[a], h = atlas->off_heights[a];
            if (w < 2 || h < 2) continue;       /* skip 1x1 placeholders */
            int x = atlas->off_drawX[a], y = atlas->off_drawY[a];
            if (mx >= x && mx < x + w && my >= y && my < y + h)
                return i;
        }
    }
    return -1;
}

extern int16_t s_mouse_x;       /* main.c — set from SDL_MOUSEMOTION */
extern int16_t s_mouse_y;

/* ---- RunMenuScene constants + helpers ----------------------------- */

/* Menu loop runs ~60 fps. SDL_Delay 16 ms / frame leaves headroom
 * for the per-tick callback + paint pass to complete. */
#define MENU_FRAME_DELAY_MS         16

/* ESC inside a menu returns this rc (= MAIN_MENU_RC_QUIT_CONFIRM_A).
 * The outer caller treats it as "user wants to back out". Scenes that
 * set SCENE_FLAG_DISABLE_ESC opt out of the binding. */
#define MENU_ESC_RC                 MAIN_MENU_RC_QUIT_CONFIRM_A

/* .pic RAWB header layout — width and height live at offsets 4..7 as
 * little-endian 16-bit values. */
#define RAWB_HEADER_OFF_WIDTH       4
#define RAWB_HEADER_OFF_HEIGHT      6

/* A .pic shorter than this y-threshold AND not full-width is treated
 * as a sub-fullscreen overlay (Pytanie quit-confirm, Solund options,
 * etc.) — see paint_menu_background. */
#define MENU_OVERLAY_HEIGHT_THRESH  400

/* Load the SceneDef's BG .pic + button mask atlas. Returns 1 if bg
 * was loaded, 0 otherwise; writes the atlas pointer to *out_buttons. */
static int load_menu_scene_assets(SceneDef *scene,
                                  void **out_bg_raw, uint32_t *out_bg_size,
                                  AnimAsset **out_buttons)
{
    int bg_loaded = 0;
    *out_bg_raw   = NULL;
    *out_bg_size  = 0;
    *out_buttons  = NULL;

    if (scene->background_pic) {
        bg_loaded = LoadFileFromDta(scene->background_pic,
                                    out_bg_raw, out_bg_size);
    }
    if (scene->mask_file) {
        *out_buttons = LoadAssetFromDtaBase(scene->mask_file);
    }
    /* The mask atlas is "entity (kind=1, id=10)" in the original — the
     * on_click handler fetches it via the asset hook for its title-
     * animation block. */
    g_menu_asset_10 = *out_buttons;
    return bg_loaded;
}

/* Decide whether a .pic is a sub-fullscreen overlay (Pytanie-sized)
 * vs a full-screen scene background. */
static int rawb_is_overlay(const void *bg_raw)
{
    const uint8_t *bp = (const uint8_t *)bg_raw;
    int bg_w = bp[RAWB_HEADER_OFF_WIDTH]
             | (bp[RAWB_HEADER_OFF_WIDTH + 1] << 8);
    int bg_h = bp[RAWB_HEADER_OFF_HEIGHT]
             | (bp[RAWB_HEADER_OFF_HEIGHT + 1] << 8);
    return bg_w < WACKI_SCREEN_W || bg_h < MENU_OVERLAY_HEIGHT_THRESH;
}

/* Paint the menu's BG layer. Full-screen scenes draw the .pic
 * directly; overlay scenes restore the captured gameplay snapshot
 * (or clear to colour 0 if no snapshot exists) so cursor trails /
 * closed-submenu remnants don't accumulate in the margin. */
static void paint_menu_background(const void *bg_raw, uint32_t bg_size)
{
    int overlay = rawb_is_overlay(bg_raw);
    extern uint8_t *g_back_shadow;
    if (overlay) {
        if (g_menu_bg_snapshot_valid && g_back_shadow) {
            memcpy(g_back_shadow, g_menu_bg_snapshot,
                   (size_t)WACKI_SCREEN_W * WACKI_SCREEN_H);
        } else {
            FlipBuffersClearWith(0);
        }
    }
    paint_rawb_pic(bg_raw, bg_size, overlay);
}

/* Walk the SceneDef's button table for whichever sprite the mouse is
 * currently over. Returns (button_idx, button_id) via out params;
 * idx = -1 when nothing is hovered (and id falls back to NEUTRAL). */
static void resolve_menu_hover(SceneDef *scene, AnimAsset *buttons,
                               int *out_idx, uint16_t *out_id)
{
    int idx = (buttons && (s_mouse_x | s_mouse_y))
            ? hit_test_buttons(scene, buttons, s_mouse_x, s_mouse_y)
            : -1;
    *out_idx = idx;
    *out_id  = (idx >= 0) ? scene->buttons[idx].id : SCENE_NEUTRAL_VERB;
}

/* Fire the SceneDef's on_click handler with the hovered button's id
 * (or -1 when nothing is hovered — the cb's per-frame logic still
 * runs that way, just no switch-case matches). Returns the cb's rc. */
static int run_menu_click_callback(SceneDef *scene,
                                   int hover_btn, uint16_t hover_id)
{
    int trigger = -1;
    if (g_lmb_clicked && hover_btn >= 0) {
        trigger = (int)hover_id;
    }
    g_lmb_clicked = 0;
    return scene->on_click(trigger);
}

/* Paint the SceneDef's button layer: every button's def sprite, then
 * the hovered button's hover sprite on top. */
static void paint_menu_buttons(SceneDef *scene, AnimAsset *buttons,
                               int hover_btn)
{
    if (!buttons) return;
    for (int i = 0; i < scene->button_count; ++i) {
        paint_anim_button(buttons, scene->buttons[i].def_anim);
    }
    if (hover_btn >= 0) {
        paint_anim_button(buttons, scene->buttons[hover_btn].hover_anim);
    }
}

/* Paint the cursor on top of the menu. Menus have no scene-hover-verb
 * and no held item, so the cursor state machine settles on state 0
 * (default arrow); the call still drives frame advance so the cursor
 * is animated the moment we re-enter gameplay. */
static void paint_menu_cursor(void)
{
    g_hover_scene_verb = SCENE_NEUTRAL_VERB;
    g_hover_panel_verb = SCENE_NEUTRAL_VERB;
    UpdateCursorState();
    PaintCursor();
}

/* Poll ESC / platform-quit. ESC sets the menu-back rc unless the
 * scene opted out via SCENE_FLAG_DISABLE_ESC. */
static int poll_menu_keyboard_quit(SceneDef *scene)
{
    if (HasPendingKey()) {
        uint16_t k = WaitForKey();
        if (k == VK_ESCAPE && !(scene->flags & SCENE_FLAG_DISABLE_ESC)) {
            return MENU_ESC_RC;
        }
    }
    if (PlatformShouldQuit()) return MAIN_MENU_RC_HARD_QUIT;
    return MAIN_MENU_RC_NONE;
}

int RunMenuScene(int transition_mode, SceneDef *scene)
{
    (void)transition_mode;

    void      *bg_raw  = NULL;
    uint32_t   bg_size = 0;
    AnimAsset *buttons = NULL;
    int bg_loaded = load_menu_scene_assets(scene, &bg_raw, &bg_size, &buttons);

    if (!bg_loaded) FlipBuffersClearWith(0);

    fprintf(stderr,
            "[menu] entered: bg='%s' mask='%s' atlas-frames=%d btns=%d\n",
            scene->background_pic ? scene->background_pic : "(none)",
            scene->mask_file      ? scene->mask_file      : "(none)",
            buttons ? buttons->frame_count : 0,
            scene->button_count);

    /* INIT the on_click handler once with the INIT trigger so HandleMain
     * MenuClick can do its first-frame setup (palette install, BGM). */
    if (scene->on_click) scene->on_click(MAIN_MENU_TRIGGER_INIT);

    int rc = MAIN_MENU_RC_NONE;
    do {
        PumpWin32Messages();

        int      hover_btn;
        uint16_t hover_id;
        resolve_menu_hover(scene, buttons, &hover_btn, &hover_id);

        if (bg_loaded) paint_menu_background(bg_raw, bg_size);

        if (scene->on_click) {
            int r = run_menu_click_callback(scene, hover_btn, hover_id);
            if (r > 0) rc = r;
        }

        paint_menu_buttons(scene, buttons, hover_btn);
        if (scene->after_paint) scene->after_paint();
        paint_menu_cursor();
        FlushFrameToPrimary();
        TickMenuMusic();

        int quit_rc = poll_menu_keyboard_quit(scene);
        if (quit_rc != MAIN_MENU_RC_NONE) rc = quit_rc;

        SDL_Delay(MENU_FRAME_DELAY_MS);
    } while (rc == MAIN_MENU_RC_NONE);

    if (buttons) FreeAsset(buttons);
    if (bg_raw)  xfree(bg_raw);
    g_menu_asset_10 = NULL;
    return rc;
}

/* ------------------------------------------------------------------------- *
 * play_bomb_explosion — visual port of the bomb cutscene. The original
 * Quit-TAK ( with &). The real script is a Wacky.scr
 * bytecode sequence that drives the menu's bomb icon — fuse burns, then
 * BOOM, then fade-out — before the process exits.
 *
 * The fullscreen flipbook for this is **bomba.wyc**: 20 frames of raw
 * 640×480 (kind=2), each at (0,0). Frame 0 holds the menu title with the
 * bomb's fuse lit; mid-frames show the blooming fireball; frame 19 is a
 * fade-to-white. We play it linearly with the wybuch.wav SFX once the
 * fuse is roughly through (start audio at ~frame 8 instead of frame 0
 * so the bang lines up with the visible explosion).
 * ------------------------------------------------------------------------- */
static void play_bomb_explosion(void)
{
    StopMenuMusic();

    AnimAsset *a = LoadAssetFromDtaBase("bomba.wyc");
    int started_fuse = 0, played_bang = 0;
    if (a && a->frame_count > 0) {
        for (uint16_t f = 0; f < a->frame_count; ++f) {
            if (PlatformShouldQuit()) break;
            PumpWin32Messages();
            /* bomba.wyc frames are fullscreen at (0,0) — paint raw. */
            paint_anim_button_at(a, f, 0, 0, 1);
            FlushFrameToPrimary();
            /* Sound timing:
 * frame 0: start lont.wav (burning fuse, 1.30 s mono) — runs
 * until wybuch overrides it.
 * frame 8: start wybuch.wav (explosion, 2.53 s) — implicitly
 * stops lont via PlayMenuMusic's StopMenuMusic call,
 * so the bang cleanly cuts the fuse hiss.
 * Both SFX live inside Dane_02.dta; PlayMenuMusic's .dta
 * fallback handles loading. */
            if (!started_fuse) {
                PlayMenuMusic("lont.wav", 0);
                started_fuse = 1;
            }
            if (!played_bang && f >= 8) {
                /* Wacky.scr [animacja]Bomba.wyc → [sampl] Bum.wav (7,)
 * means "play bum.wav from frame 7 onwards" — that's the
 * actual asset (wybuch.wav is the in-game level-explosion,
 * different SFX). */
                PlayMenuMusic("bum.wav", 0);
                played_bang = 1;
            }
            TickMenuMusic();
            SDL_Delay(100);                     /* ~10 fps */
        }
        FreeAsset(a);
    }
    SDL_Delay(800);                             /* let the bang fully tail */
    StopMenuMusic();
}

/* ------------------------------------------------------------------------- *
 * RunMainGameLoop — 0x0040BBF0
 *
 *'s two-loop structure:
 * // either way: inner loop CONTINUES (rc2==4 → menu)
 *
 * Crucially: NIE from Pytanie does NOT replay the intro — control returns
 * to the inner do/while which re-enters RunMenuScene with the main menu.
 * Only "New game" (case 6) breaks the inner loop and the outer re-plays AVI.
 * ------------------------------------------------------------------------- */
extern void LoadSaveStateOrInitialize(void);

/* fwd-decl — defined after RunMainGameLoop so it can see all helpers.
 * T39 removed play_first_scene_demo (body inlined into RunGameStageLoop). */
static void play_fiacik_intro(void);
static void play_loading_screen(void);

/* DEV (Fix #22 / Bug 7 helper): if set via --start-stage N or
 * WACKI_START_STAGE=N CLI/env, RunMainGameLoop skips the intro AVI +
 * main menu and jumps straight into stage N gameplay with stages
 * 1..(N-1) marked completed. 0 = normal flow. */
int g_dev_start_stage = 0;

/* ---- RunMainGameLoop constants ------------------------------------ */

/* Title/main-menu constants moved to the top of the file (above
 * HandlePytanieClick + HandleMainMenuClick) so both early click
 * handlers and the late RunMainGameLoop builder share one source of
 * truth. See the "Title + main-menu constants" block above. */

/* Build the title-screen SceneDef (Tlo.wyc mask + HandleMainMenuClick). */
static SceneDef make_title_scene(void)
{
    SceneDef s = {
        .background_pic = NULL,
        .mask_file      = TITLE_MASK_FILENAME,
        .on_click       = HandleMainMenuClick,
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

/* Build the Pytanie Y/N quit-confirm SceneDef. */
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

/* Mirror RunGameStageLoop's flag-2 FULL RESET cleanup: zero script vars,
 * clear each entity_state[i].in_inventory_flag (preserving the
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

/* Mask of "stage prior to N is completed" bits used to seed the
 * dev-mode chapter-select map. (1<<(N-1)) - 1 = bits 0..N-2. */
static uint32_t dev_completed_mask_for_stage(int n)
{
    return (uint32_t)((1u << (n - 1)) - 1u);
}

/* g_game_over_code values 0/1/3/4 mean "stage progressed normally" —
 * anything else (2 = ESC/F12 quit, 99 = hard-quit, unknown codes)
 * means the user is done with the dev session. */
static int game_over_is_progress_signal(int code)
{
    return code == GAME_OVER_NONE
        || code == GAME_OVER_DEATH
        || code == GAME_OVER_CHAPTER_PICK
        || code == GAME_OVER_STAGE_END_AVI;
}

/* Forward-declared because g_sel_tlo_scene + helpers live further down
 * the file (in the sel_tlo.pic chapter-select section). */
extern SceneDef g_sel_tlo_scene;
extern int      s_chapter_pick;
extern int      SelTloRefreshButtons(void);

/* DEV --start-stage N: skip menu+intro, show chapter-select map with
 * stages 1..(N-1) marked completed. User picks stage from the map,
 * then that stage runs normally. Loop so that returning from one
 * stage re-shows the map (e.g. ESC out of stage 2 → back to map).
 * Returns 1 if the dev flow handled the run (RunMainGameLoop should
 * return), 0 if dev mode is off and the normal flow should proceed. */
static int run_dev_start_stage_flow(void)
{
    if (g_dev_start_stage < DEV_START_STAGE_MIN ||
        g_dev_start_stage > DEV_START_STAGE_MAX) return 0;

    int N = g_dev_start_stage;
    apply_full_reset();
    g_completed_stages = dev_completed_mask_for_stage(N);
    fprintf(stderr, "[wacki] dev-start: chapter-select map, "
                    "completed_mask=0x%X (stages 1..%d done)\n",
            g_completed_stages, N - 1);

    while (!PlatformShouldQuit()) {
        (void)SelTloRefreshButtons();
        s_chapter_pick = 0;
        RunMenuScene(0, &g_sel_tlo_scene);
        if (PlatformShouldQuit()) return 1;

        if (s_chapter_pick < 1 || s_chapter_pick > DEV_PICK_FINALE) {
            fprintf(stderr, "[wacki] dev-start: no stage picked — exit\n");
            return 1;
        }
        fprintf(stderr, "[wacki] dev-start: stage %d picked from map\n",
                s_chapter_pick);
        if (!LoadStage((uint16_t)s_chapter_pick)) {
            fprintf(stderr, "[wacki] dev-start: LoadStage(%d) failed\n",
                    s_chapter_pick);
            continue;
        }

        int played_stage = s_chapter_pick;
        RunGameStageLoop(STAGE_LOAD_FLAG_SAVE_LOAD);

        /* Stage 5 (Monter finale) is terminal — the original returns
         * to the main-menu loop after the credits sting. In dev mode
         * the title was skipped on startup, so the equivalent
         * terminal action is to return from the dev loop entirely. */
        if (played_stage == DEV_PICK_FINALE) {
            fprintf(stderr, "[wacki] dev-start: Monter finale complete "
                            "→ exit (= main menu in normal flow)\n");
            return 1;
        }

        /* Loop back to the map only on stage-progress signals; on an
         * explicit user quit (game_over_code 2, 99, or any unknown
         * value) bail back to the OS instead of bouncing to sel_tlo. */
        if (!game_over_is_progress_signal(g_game_over_code)) {
            fprintf(stderr, "[wacki] dev-start: game_over=%d → exit\n",
                    g_game_over_code);
            return 1;
        }

        /* Re-mark all stages prior to N as completed so the map shows
         * progress even if the played stage cleared a bit via script. */
        g_completed_stages |= dev_completed_mask_for_stage(N);
    }
    return 1;
}

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
 * outer / inner loop should keep running, 0 if the caller should
 * break out of the inner loop (back to outer's intro replay). */
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

    case MAIN_MENU_RC_LOAD_SAVE: {
        /* LoadSaveSlot restores g_cur_komnata + g_script_vars +
         * g_entity_state from Wacki.sav slot N. */
        extern uint16_t g_selected_save_slot;
        LoadSaveSlot(g_selected_save_slot);
        RunGameStageLoop(STAGE_LOAD_FLAG_SAVE_LOAD);
        return 1;
    }

    case MAIN_MENU_RC_NEW_GAME:
        /* Film-reel button — the original runs an intro script which
         * plays the credits/film cutscene. The VM isn't wired to
         * assets here, so break the inner loop and let the outer
         * replay the title intro AVI (Dane_10.dta). */
        return 0;

    case MAIN_MENU_RC_PROLOGUE:
        /* [etap]1 [komnata]init prologue:
         *   1. fiacik.wyc Maluch driving across the title
         *   2. load.pic / load.wyc "Lołding" progress bar
         *   3. hand off to RunGameStageLoop(FULL_RESET). */
        play_fiacik_intro();
        play_loading_screen();
        RunGameStageLoop(STAGE_LOAD_FLAG_FULL_RESET);
        return 0;

    case MAIN_MENU_RC_CREDITS:
        PlaySceneCutsceneAvi(CREDITS_AVI);
        return 1;
    }
    return 1;
}

void RunMainGameLoop(void)
{
    LoadSaveStateOrInitialize();

    /* Push Wacki.sav settings into in-memory s_opt_* + audio mixer.
     * Without this the saved prefs were loaded into g_save.settings
     * but never had any effect — opszyns always opened with all-on
     * regardless of what the user toggled last session. */
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

/* ------------------------------------------------------------------------- *
 * play_fiacik_intro — Maluch driving away across the title screen.
 *
 *scr [etap]1 [komnata]init line:
 * [animacja] fiacik.wyc
 * [sampl] fiacik.wav (0,20)
 *
 * fiacik.wyc is a 10-frame raw atlas (kind=2). Each frame has its own
 * (drawX, drawY) hot-spot — frame 0 at (201,244), frame 5 at (16,247),
 * frame 9 at (429,404) (off-screen end-marker). The Maluch slides from
 * the right edge of the title to the left. fiacik.wav (engine sound, ~1s)
 * plays alongside the visible motion.
 *
 * The animation overlays the title screen (we keep the WACKI logo
 * underneath, just paint the car on top with color-key 0). This matches
 * the bomb-explosion structure user already recognised.
 * ------------------------------------------------------------------------- */
static void play_fiacik_intro(void)
{
    StopMenuMusic();
    PlayMenuMusic("fiacik.wav", 0);

    /* The menu loop exited with the HOVER overlay still composited onto
 * the shadow — Maluch's hover frame 7 is 105×60 @ (213,241), which
 * sticks out 3 px to the LEFT of mal_back's 113×74 patch @ (216,228).
 * That sliver would survive mal_back and show up as the "yellow trash"
 * the user reported.
 *
 * IMPORTANT: RunMenuScene's cleanup did `FreeAsset(buttons); g_menu_
 * asset_10 = NULL;` BEFORE we got here — so we have to re-load Tlo.wyc
 * locally; relying on g_menu_asset_10 silently no-ops the re-render
 * and leaves the hover sprite on the shadow.
 *
 * Re-render the menu cleanly first (Tlo flipbook fullscreen wipe +
 * def_anim buttons only, no hover) so the snapshot starts from the
 * exact same state as the engine's pre-fiacik tick. Equivalent to the
 * original which despawns the hover entity before the script runs. */
    AnimAsset *bg = LoadAssetFromDtaBase("Tlo.wyc");
    if (bg && bg->pixel_ptrs) {
        int bgf = s_anim_frame;
        if (bgf < 10) bgf = 10;
        if (bgf >= bg->frame_count) bgf = bg->frame_count - 1;
        if (bg->pixel_ptrs[bgf])
            BlitSpriteToBackbuffer(
                bg->off_drawX[bgf], bg->off_drawY[bgf], 0, 0,
                bg->off_widths[bgf], bg->off_heights[bgf],
                bg->off_widths[bgf], bg->off_heights[bgf],
                bg->pixel_ptrs[bgf], 1);                  /* opaque wipe */
        /* def_anim only for buttons 0..4 (no hover overlay) */
        for (uint16_t i = 0; i < 5; ++i)
            paint_anim_button(bg, (uint16_t)i);
    }

    /* Per prologue script @ 0x00427af8 the original engine spawns TWO
 * entities for this animation:
 * LOAD_ASSET id=0x01 → mal_back.wyc (113×74 orange patch @216,228)
 * LOAD_ASSET id=0x3C → fiacik.wyc
 * mal_back covers the def_anim "yellow car" button so the Fiat can
 * drive across without leaving the icon behind. */
    AnimAsset *mal_back = LoadAssetFromDtaBase("mal_back.wyc");
    if (mal_back) {
        paint_anim_button_at(mal_back, 0, 0, 0, 0);   /* atlas hot-spot = 216,228 */
        FreeAsset(mal_back);
    }

    /* Snapshot the (now button-erased) menu shadow so we can restore it
 * cleanly under each fiacik frame. equivalent of the engine's
 * RestorePrevFrameRects — no per-frame ghosting. */
    extern uint8_t *g_back_shadow;
    size_t shadow_bytes = (size_t)WACKI_SCREEN_W * WACKI_SCREEN_H;
    uint8_t *snapshot = (uint8_t *)malloc(shadow_bytes);
    if (snapshot && g_back_shadow) memcpy(snapshot, g_back_shadow, shadow_bytes);

    AnimAsset *a = LoadAssetFromDtaBase("fiacik.wyc");
    if (a && a->frame_count > 0) {
        for (uint16_t f = 0; f < a->frame_count; ++f) {
            if (PlatformShouldQuit()) break;
            PumpWin32Messages();
            /* Restore the clean menu under us before painting this frame. */
            if (snapshot && g_back_shadow)
                memcpy(g_back_shadow, snapshot, shadow_bytes);
            paint_anim_button_at(a, f, 0, 0, 0);   /* honour atlas hot-spot */
            FlushFrameToPrimary();
            TickMenuMusic();
            SDL_Delay(80);                        /* ~12 fps */
        }
        FreeAsset(a);
    }
    if (bg) FreeAsset(bg);
    free(snapshot);
    SDL_Delay(200);                               /* let engine note tail */
    StopMenuMusic();
}

/* ------------------------------------------------------------------------- *
 * play_loading_screen — the "LOLDING" screen between Maluch and the scene.
 *
 * Per LoadStage @ 0x00403320: `g_panel_cursor = krazek.pic` (a 203×220 RAWB
 * of a vinyl-CD shape with the text "LOLDING" baked in, preloaded by
 * PreloadCommonAssets). The engine paints it as a centred overlay on top
 * of whatever is on screen during every stage transition:
 * ( (screen_w - pic_w) / 2,
 * (screen_h - pic_h) / 2,
 * pic_w, pic_h,
 * pic_pixels);
 * FlushFrameToPrimary;
 *
 * No animation — it's a static drop while assets stream. We hold it for
 * ~1.5 s as the visible "loading time" placeholder.
 *
 * NOTE: load.pic / load.wyc are the save-slot menu — completely different
 * assets. Easy to confuse by filename. The Wacki "loading" overlay is
 * krazek.pic (Polish-pun text "LOLDING" on a CD/krążek).
 * ------------------------------------------------------------------------- */
static void play_loading_screen(void)
{
    void *bg_raw = NULL; uint32_t bg_size = 0;
    int   bg_ok  = LoadFileFromDta("krazek.pic", &bg_raw, &bg_size);
    if (!bg_ok) return;

    /* Paint it centred (paint_rawb_pic does the math + uses color-key 0
 * so the corners are transparent — the menu shows through where the
 * CD shape doesn't cover). */
    uint32_t start = SDL_GetTicks();
    while (SDL_GetTicks() - start < 1500) {
        if (PlatformShouldQuit()) break;
        PumpWin32Messages();
        FlipBuffersClearWith(0);                  /* black underneath */
        paint_rawb_pic(bg_raw, bg_size, 0);       /* fullscreen-ish; small RAWB centers itself */
        FlushFrameToPrimary();
        SDL_Delay(33);
        if (HasPendingKey()) {
            uint16_t k = WaitForKey();
            if (k == VK_ESCAPE) break;
        }
    }
    xfree(bg_raw);
}

/* ------------------------------------------------------------------------- *
 * Scene descriptor — minimal data needed to render and walk one room.
 *
 * The original engine derives most of this from the per-room script section
 * + .fld file at load time (see LoadStage / FindScriptByStageAndRoom). For
 * the demo we hard-code a small table per room — once the script VM is
 * wired into the entity system we replace this with the real lookup.
 * ------------------------------------------------------------------------- */
typedef struct DemoScene {
    const char  *name;            /* scene id (= "maluch.pic" etc, matches PE komnata name string) */
    const char  *bg_pic;          /* fullscreen RAWB — same as name in stage 1 */
    const char  *fld_file;        /* walkable mask asset (e.g. "maluch.fld") */
    const char  *music_wav;       /* per-room loop, from Wacky.scr [sampl] */
    int          walk_x0, walk_y0, walk_x1, walk_y1;  /* fallback bbox if .fld load fails */
    /* T35 (shipped): hotspots[] / n_hotspots retired. Verb-driven exits
 * via op 0x20 GO_EXIT → LoadKomnataScene (T22 phase B) now work
 * after the skip_to_endif 0x55 false-terminator bug was fixed. */
} DemoScene;

/* Stage-1 scene table — (4 rooms,
 * 14 bytes each, terminated by all-zero entry). Each room's walkable bbox
 * is read from its .fld file header (FILD parser in assets.c). Hotspots
 * are taken from per-room .msk files (the original engine binds them to
 * the room's enter_script bytecode; we wire them statically until the
 * full per-room script execution is up).
 *
 * Room indices (per dispatch):
 */
/* T28 retired (Fix #26) — hardcoded s_demo_scenes[] / find_demo_scene
 * removed. LoadKomnataScene now ALWAYS synthesizes the DemoScene from
 * the komnata name + g_cur_etap + g_cur_komnata (Fix #24):
 * Walk box is FLD-derived (off_drawX/Y/widths/heights[0]) — the
 * old hardcoded walk_x0..y1 values match FLD output exactly for stage 1
 * (verified maluch=628x79@(6,315), kiosk=628x112@(6,281), etc.).
 * Note: original cycles music variants 'a'/'b'/'c' randomly per komnata
 * load; synth always picks 'a' — small fidelity gap, not user-visible. */

/* T22 phase B — LoadKomnataScene: full synchronous komnata transition.
 *
 * 1. Walker-freeze both g_actor entities (+0x4C/+0x50 = 0, +0x3A bits
 * 0,2 cleared) partial reset. Prevents
 * leftover walker step from overwriting post-transition
 * SET_ENTITY_XY in verb script.
 * 2. Free the old BG raw blob and FLD asset (if any).
 * 3. Call LoadKomnata(id) for the script-level setup: EntityListClearAll
 * preserves actors (T4), runs new room's enter_script.
 * 4. Find the DemoScene record for the new komnata name and load its
 * BG + FLD into the g_scene_* / g_walk_* globals.
 * 5. Start the per-room music loop.
 *
 * Called from:
 * - ScriptGoToKomnata (op 0x20 in script.c) — verb-driven transition
 * - F9 quickload in play_demo_scene main loop — save-driven transition
 * - play_demo_scene prologue — initial entry into the first komnata
 *
 * After this returns, play_demo_scene's per-frame loop sees the new
 * scene's BG/FLD/entities live in the globals and continues running
 * without unwinding. The legacy g_pending_komnata polling has been
 * fully removed (T42); LoadKomnataScene is the single entry-point. */
/* ---- LoadKomnataScene helpers ------------------------------------ */

/* Stage descriptor offsets for the per-actor anim-table pointers. */
#define STAGE_OFF_ACTOR_ANIM_PTR_BASE  0x0C
#define ANIM_TABLE_IDLE_SLOT_OFFSET    0x14   /* entry [5] = idle bytecode */
#define WALKER_STATE_RESET_MASK        (ESTATE_FRAME_READY | ESTATE_WALKER_FRESH)

#define FLD_BITS_PER_BYTE              8

/* Walker name buffer size for the synthesised DemoScene. */
#define SCENE_NAME_BUFFER_SIZE         64

/* Music filename template uses 2-digit decimals; allow plenty of room. */
#define MUSIC_NAME_FMT                 "Tlo_%u_%ua.wav"
#define FLD_EXTENSION                  ".fld"
#define FLD_EXTENSION_BYTES            5      /* ".fld" + NUL */

/* Reset all per-entity VM scratch state on an actor and rebind its
 * idle bytecode (entry [5] in the per-stage anim table). Without this
 * the previous room's walker bytecode + patched op 0x15 target stay
 * bound, and the next VM tick re-plants the path toward the OLD
 * scene's exit — actor "drifts diagonally" on room entry. */
static void reset_actor_for_komnata(Entity *a, const uint8_t *sd, int actor_idx)
{
    uint8_t *eb = (uint8_t *)a;

    EOFF(a, ENT_OFF_STATE_FLAGS, uint16_t)   &= (uint16_t)~WALKER_STATE_RESET_MASK;
    EOFF(a, ENT_OFF_LOOP_A,        uint16_t) = 0;
    EOFF(a, ENT_OFF_LOOP_B,        uint16_t) = 0;
    EOFF(a, ENT_OFF_LOOP_C,        uint16_t) = 0;
    EOFF(a, ENT_OFF_LOOP_D,        uint16_t) = 0;
    EOFF(a, ENT_OFF_LOOP_E,        uint16_t) = 0;
    EOFF(a, ENT_OFF_PC,            uint16_t) = 0;
    EOFF(a, ENT_OFF_DELAY,         uint16_t) = 0;
    EOFF(a, ENT_OFF_WALKER_DX_REM, uint32_t) = 0;
    EOFF(a, ENT_OFF_WALKER_DY_REM, uint32_t) = 0;
    EOFF(a, ENT_OFF_WALKER_TGT_X,  uint16_t) = 0;
    EOFF(a, ENT_OFF_WALKER_TGT_Y,  uint16_t) = 0;
    (void)eb;

    if (!sd) return;

    /* Rebind idle bytecode (anim_tab[+0x14] = entry [5]). */
    int      anim_ptr_off = STAGE_OFF_ACTOR_ANIM_PTR_BASE + actor_idx * 4;
    uint32_t anim_tab_va  = *(const uint32_t *)(sd + anim_ptr_off);
    const uint8_t *anim_tab = (const uint8_t *)PeLoaderRead(anim_tab_va);
    if (!anim_tab) return;

    uint32_t bc_va = *(const uint32_t *)(anim_tab + ANIM_TABLE_IDLE_SLOT_OFFSET);
    const void *bc = xlat_binary_ptr(bc_va);
    if (!bc) return;

    EOFF(a, ENT_OFF_BYTECODE_SLOT, uint32_t) = ent_ptr_intern((void *)bc);
}

/* Drop the previous scene's BG raw blob + walkability asset so the new
 * room starts with cleared scene-state. */
static void free_previous_scene_assets(void)
{
    extern void FreeAsset(AnimAsset *a);

    if (g_scene_bg_raw) {
        xfree(g_scene_bg_raw);
        g_scene_bg_raw = NULL;
    }
    g_scene_bg_size = 0;

    if (g_scene_fld_asset) {
        FreeAsset(g_scene_fld_asset);
        g_scene_fld_asset = NULL;
    }
    g_walk_fld_pixels = NULL;
    g_walk_fld_w  = g_walk_fld_h = 0;
    g_walk_fld_ox = g_walk_fld_oy = g_walk_fld_stride = 0;

    FreeSceneBgAtlas();
    StopMenuMusic();
}

/* Build a synthesised DemoScene from the resolved komnata name, the
 * current stage / komnata indices, and the standard naming convention:
 *   bg_pic    = <name>
 *   fld_file  = <name without .pic>.fld
 *   music_wav = Tlo_<stage>_<komnata>a.wav
 * Returns a pointer to a static buffer (stable across the call,
 * overwritten by the next LoadKomnataScene). */
static const DemoScene *synthesise_demo_scene(const char *name)
{
    static DemoScene s_synth;
    static char      s_synth_name [SCENE_NAME_BUFFER_SIZE];
    static char      s_synth_fld  [SCENE_NAME_BUFFER_SIZE];
    static char      s_synth_music[SCENE_NAME_BUFFER_SIZE];

    snprintf(s_synth_name, sizeof s_synth_name, "%s", name);

    /* Derive ".fld" filename by replacing the .pic extension. */
    const char *dot  = strrchr(name, '.');
    size_t      base = dot ? (size_t)(dot - name) : strlen(name);
    if (base >= sizeof s_synth_fld) {
        base = sizeof s_synth_fld - FLD_EXTENSION_BYTES;
    }
    memcpy(s_synth_fld, name, base);
    memcpy(s_synth_fld + base, FLD_EXTENSION, FLD_EXTENSION_BYTES);

    snprintf(s_synth_music, sizeof s_synth_music, MUSIC_NAME_FMT,
             (unsigned)g_cur_etap, (unsigned)g_cur_komnata);

    s_synth = (DemoScene){
        .name      = s_synth_name,
        .bg_pic    = s_synth_name,
        .fld_file  = s_synth_fld,
        .music_wav = s_synth_music,
        .walk_x0   = 0, .walk_y0 = 0,
        .walk_x1   = 0, .walk_y1 = 0,
    };
    return &s_synth;
}

/* Load the .pic background blob for the new scene, if present. */
static void load_scene_background(const DemoScene *s)
{
    if (!s->bg_pic) return;

    void    *bg    = NULL;
    uint32_t bg_sz = 0;
    if (LoadFileFromDta(s->bg_pic, &bg, &bg_sz) && bg) {
        g_scene_bg_raw  = bg;
        g_scene_bg_size = bg_sz;
    }
}

/* Fallback walkability bitmap loader. The script-driven bg-mask-setup
 * call (op 0x2C inside LoadKomnata) is the authoritative source;
 * this only fires when the script never called it (komnaty whose
 * .pic and .fld share a basename). */
static void load_scene_walkability_fallback(const DemoScene *s)
{
    if (!s->fld_file || g_walk_fld_pixels) return;

    AnimAsset *fld = LoadAssetFromDtaBase(s->fld_file);
    g_scene_fld_asset = fld;

    if (fld && fld->pixel_ptrs && fld->pixel_ptrs[0]) {
        g_walk_fld_pixels = fld->pixel_ptrs[0];
        g_walk_fld_w      = fld->off_widths [0];
        g_walk_fld_h      = fld->off_heights[0];
        g_walk_fld_ox     = (int16_t)fld->off_drawX[0];
        g_walk_fld_oy     = (int16_t)fld->off_drawY[0];
        g_walk_fld_stride = (g_walk_fld_w + FLD_BITS_PER_BYTE - 1) / FLD_BITS_PER_BYTE;
        fprintf(stderr, "[fld] %s: %dx%d @ (%d,%d) stride=%d\n",
                s->fld_file, g_walk_fld_w, g_walk_fld_h,
                g_walk_fld_ox, g_walk_fld_oy, g_walk_fld_stride);
    } else {
        fprintf(stderr, "[fld] %s: load failed — falling back to bbox\n",
                s->fld_file);
    }
}

void LoadKomnataScene(uint16_t id)
{
    extern Entity *g_actor[2];
    if (id == 0) return;

    /* --- Step 1: reset both actors -------------------------------- */
    const uint8_t *sd = g_stage_va
                        ? (const uint8_t *)PeLoaderRead(g_stage_va)
                        : NULL;
    for (int i = 0; i < 2; ++i) {
        if (g_actor[i]) reset_actor_for_komnata(g_actor[i], sd, i);
    }

    /* --- Step 2: drop previous scene's assets --------------------- */
    free_previous_scene_assets();

    /* --- Step 3: run the script-level load ----------------------- */
    const char *name = LoadKomnata(id);
    if (!name) {
        fprintf(stderr, "[scene] LoadKomnataScene(%u) — LoadKomnata failed\n", id);
        return;
    }

    /* --- Step 4: build a DemoScene + load BG + walkability fallback */
    const DemoScene *s = synthesise_demo_scene(name);
    g_current_scene = (const struct DemoScene *)s;
    g_walk_x0 = s->walk_x0;  g_walk_x1 = s->walk_x1;
    g_walk_y0 = s->walk_y0;  g_walk_y1 = s->walk_y1;

    load_scene_background(s);
    load_scene_walkability_fallback(s);

    /* --- Step 5: music + clean BG repaint ------------------------ */
    if (s->music_wav) PlayMenuMusic(s->music_wav, 1);

    /* LoadKomnata runs the enter script + two embedded ProcessGameFrameTick
     * calls + the secondary script. Those embedded ticks call
     * EntityRenderAll, painting entities at their CURRENT positions.
     * The outer ProcessGameFrameTickInner that called us is mid-tick
     * and will call EntityRenderAll again at slightly advanced
     * positions — repainting the BG here wipes the embedded paint so
     * the outer pass renders a clean single frame. */
    if (g_scene_bg_raw) paint_rawb_pic(g_scene_bg_raw, g_scene_bg_size, 0);
    PaintSceneBgAtlasIfAny();

    fprintf(stderr, "[scene] LoadKomnataScene(%u) → '%s'\n", id, name);
}

/* play_demo_scene — single-call scene loop (T22 phase B). Initial
 * komnata is loaded via LoadKomnataScene at prologue; subsequent
 * transitions happen IN-PLACE via ScriptGoToKomnata (op 0x20) without
 * unwinding the main loop. Returns NULL on quit (ESC / F12 TAK). */
static const char *play_demo_scene(const DemoScene *scene);

/* HandleSceneInput externs (forward decls so the helper can call them). */
extern int  BindActorWalker(int actor_idx, int target_x, int target_y);
extern void DispatchClickEvent(uint16_t obj_id, uint16_t verb_id);

/* ===== Options menu — opszyns.pic + Solund.pic =========================== *
 *
 * The HUD panel.wyc has an "OPCJE" label baked into frame 0 at panel-local
 * (~160..400, 16..55). The original game's RunGameStageLoop @ 0x0040C0CC
 * launches RunMenuScene(opszyns.pic) when g_script_vars != 0, but no code
 * path in the disassembly ever sets g_script_vars — the wiring was lost.
 *
 * We add the missing piece: when the user clicks the OPCJE region of the
 * panel (and not an inventory slot), open the opszyns menu. The opszyns
 * dispatcher maps button IDs 0x12..0x17 to sub-menus
 * (Pytanie / Solund / Grafika / save / load / exit). We port the Solund
 * sub-menu (sound options) in full since the user's stated need was
 * music on/off toggles. Other sub-menus log + no-op for now.
 *
 * Audio toggles wired through AudioSet{Music,Sfx,Sound}Enabled in audio.c.
 *
 * Sub-menu globals — mirror speech_text_attr..fade_progress layout from
 * LoadSaveStateOrInitialize @ 0x0040a597. Init to "all on" so a fresh
 * game (no Wacki.sav) plays with full audio. */
/* T103 fix — correct Solund mapping per Ghidra plate comment on
 * LoadSaveStateOrInitialize @ 0x0040A5C0:
 * speech_x_offset — video_mode (Grafika menu, NOT Solund)
 * speech_y_offset — sfx (sound effects on/off — NOT in Solund)
 * speech_text_attr — music_on (Solund 0x12)
 * speech_color_index — ? (Solund 0x16; default 0 in original)
 * fade_target — voice_on (Solund 0x14) — gates dialog line audio
 * fade_step — subtitles (Solund 0x13) — gates op 0x09 SHOW_TEXT
 * fade_progress — dialogues (Solund 0x15) — gates op 0x52/0x53
 *
 * Earlier port mis-labelled 0x13/0x14/0x15 as samplerate/stereo/sfx
 * (in-session only, no Wacki.sav persistence) — those names don't appear
 * in original. Player couldn't disable subtitles / voice / dialogues
 * independently. */
static uint8_t s_opt_music      = 1;      /* speech_text_attr */
static uint8_t s_opt_subtitles  = 1;      /* fade_step — gates op 0x09 SHOW_TEXT */
static uint8_t s_opt_voice      = 1;      /* fade_target — gates PlayDialogLine */
static uint8_t s_opt_dialogues  = 1;      /* fade_progress — gates op 0x52/0x53 */
static uint8_t s_opt_extra      = 0;      /* speech_color_index — default 0 per original;
 * semantic under RE (possibly an
 * audio-quality / sfx-master flag) */

/* T34 — persistence bridge between Solund/Grafika static opt-flags and
 * the on-disk WackiSettings struct in g_save.settings. The original 1997
 * engine kept these as separate globals (speech_x_offset..22) AND wrote them
 * into Wacki.sav settings header; on boot it sampled the .sav header back
 * into the globals so per-session options stuck.
 *
 * Mapping (WackiSettings → s_opt_*):
 * music_on → s_opt_music
 * sound_on → s_opt_sound
 * voice_on → s_opt_sfx (voice = sound effects; closest match)
 * subtitles_on, dialogues_on — no Solund toggle; left at WackiSettings default
 * video_mode — Grafika.pic but maps to s_opt_gfx1 (graphics flag 1)
 *
 * Samplerate / stereo aren't reflected in Wacki.sav settings (the original
 * stored those elsewhere — DAT_0044E5xx audio init block); for our port
 * they live only in this session's s_opt_* statics. */
extern WackiSaveFile g_save;
extern void WriteSaveFile(void);

/* Forward refs to the Grafika.pic static toggles — defined further down
 * (next to GrafikaClick). Declared up here so the persistence helpers
 * below can see them without reorganising the whole options block. */
static uint8_t s_opt_gfx1, s_opt_gfx2;

void ApplySavedSettings(void)
{
    /* Pull from g_save.settings (loaded by LoadSaveStateOrInitialize)
 * into the in-memory opt flags + the audio mixer state. Called once
 * at boot AFTER LoadSaveStateOrInitialize so a fresh game uses
 * persisted prefs (or defaults if no save file).
 *
 * T103 fix — correct field mapping per Ghidra plate comment:
 * music_on, voice_on, subtitles_on, dialogues_on come from saved
 * WackiSettings (offsets 2/4/5/6). `sfx` lives at offset 1 in the
 * on-disk block — currently named `sound_on` in
 * our WackiSettings struct (semantic drift; mapping preserved). */
    s_opt_music      = g_save.settings.music_on     ? 1 : 0;
    s_opt_voice      = g_save.settings.voice_on     ? 1 : 0;
    s_opt_subtitles  = g_save.settings.subtitles_on ? 1 : 0;
    s_opt_dialogues  = g_save.settings.dialogues_on ? 1 : 0;
    s_opt_gfx1       = g_save.settings.video_mode   ? 1 : 0;
    /* `sound_on` in WackiSettings = speech_y_offset = sfx flag per Ghidra
 * (offset 1 of the 7-byte settings block). Wire it to the sfx
 * mixer toggle (audio.c AudioSetSfxEnabled). */
    uint8_t sfx_on   = g_save.settings.sound_on     ? 1 : 0;

    /* Push to audio mixer + global gate flags. */
    AudioSetMusicEnabled(s_opt_music);
    AudioSetSfxEnabled  (sfx_on);
    AudioSetVoiceEnabled(s_opt_voice);
    g_subtitles_on  = s_opt_subtitles;
    g_dialogues_on  = s_opt_dialogues;
    fprintf(stderr, "[opt] applied saved settings: music=%d voice=%d "
                    "subs=%d dialogs=%d sfx=%d gfx1=%d\n",
            s_opt_music, s_opt_voice, s_opt_subtitles, s_opt_dialogues,
            sfx_on, s_opt_gfx1);
}

static void persist_audio_opts(void)
{
    /* Mirror s_opt_* → WackiSettings → Wacki.sav. Called on every toggle
 * inside SolundClick / GrafikaClick so options survive across sessions. */
    g_save.settings.music_on     = s_opt_music     ? 1 : 0;
    g_save.settings.voice_on     = s_opt_voice     ? 1 : 0;
    g_save.settings.subtitles_on = s_opt_subtitles ? 1 : 0;
    g_save.settings.dialogues_on = s_opt_dialogues ? 1 : 0;
    g_save.settings.video_mode   = s_opt_gfx1      ? 1 : 0;
    /* sfx flag lives at WackiSettings.sound_on (= speech_y_offset = offset 1). */
    /* Note: extra is not persisted in WackiSettings —
 * lives only in this session. */
    WriteSaveFile();
}

/* ---- Solund.pic audio-options constants + helpers ----------------- */

/* Triggers the Solund.wyc mask emits per button. Buttons 0x12..0x16 are
 * toggle slots (5 of them); 0x17 is the exit button. */
#define SOLUND_BTN_MUSIC                0x12
#define SOLUND_BTN_SUBTITLES            0x13
#define SOLUND_BTN_VOICE                0x14
#define SOLUND_BTN_DIALOGUES            0x15
#define SOLUND_BTN_EXTRA                0x16
#define SOLUND_BTN_EXIT                 0x17

/* SolundClick return codes. 0 = idle / per-frame redraw; 3 = exit
 * (caller propagates to the opszyns dispatcher). */
#define SOLUND_RC_KEEP_OPEN             0
#define SOLUND_RC_EXIT                  3

/* Button slot indices in g_solund_scene.buttons[]. */
#define SOLUND_SLOT_MUSIC               0
#define SOLUND_SLOT_SUBTITLES           1
#define SOLUND_SLOT_VOICE               2
#define SOLUND_SLOT_DIALOGUES           3
#define SOLUND_SLOT_EXTRA               4
#define SOLUND_SLOT_EXIT                5
#define SOLUND_BUTTON_COUNT             6
#define SOLUND_TOGGLE_BUTTON_COUNT      5

/* Solund.wyc atlas layout — 24 frames in four contiguous bands of 6:
 *   [0..5]    = OFF hover  (slot N → frame N + SOLUND_OFF_HOVER_BASE)
 *   [6..11]   = OFF def
 *   [12..17]  = ON  hover
 *   [18..23]  = ON  def
 * The exit slot has no "active" state, so it stays at OFF frames. */
#define SOLUND_OFF_HOVER_BASE           0
#define SOLUND_OFF_DEF_BASE             6
#define SOLUND_ON_HOVER_BASE            12
#define SOLUND_ON_DEF_BASE              18

/* SolundClick — Solund.pic SceneDef has 6 buttons (ids 0x12..0x17).
 * Per the dispatch-table mapping:
 *   MUSIC      → music_on             (audio mixer gate)
 *   SUBTITLES  → subtitles            (op 0x09 SHOW_TEXT gate)
 *   VOICE      → voice_on             (PlayDialogLine gate)
 *   DIALOGUES  → dialogues            (op 0x52 / 0x53 gate)
 *   EXTRA      → extra                (semantic still under RE)
 *   EXIT       → return SOLUND_RC_EXIT + persist to Wacki.sav
 *
 * Each toggle flips its flag and applies immediately to the relevant
 * subsystem; exit also persists via persist_audio_opts. */
extern SceneDef g_solund_scene;
extern void AudioSetVoiceEnabled(int on);    /* audio.c */

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

/* Sync all 5 toggle slots' visuals from the current s_opt_* flags.
 * Runs on every SolundClick call (including idle / per-frame trigger=-1)
 * so the on/off indicators stay in sync when the scene is re-entered. */
static void refresh_solund_toggle_visuals(void)
{
    apply_solund_toggle_visual(SOLUND_SLOT_MUSIC,     s_opt_music);
    apply_solund_toggle_visual(SOLUND_SLOT_SUBTITLES, s_opt_subtitles);
    apply_solund_toggle_visual(SOLUND_SLOT_VOICE,     s_opt_voice);
    apply_solund_toggle_visual(SOLUND_SLOT_DIALOGUES, s_opt_dialogues);
    apply_solund_toggle_visual(SOLUND_SLOT_EXTRA,     s_opt_extra);
}

/* Re-apply every option's effect to its subsystem (used on EXIT to
 * make sure mixer / global gate state matches the flags before
 * persisting to Wacki.sav). */
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
        /* speech_color_index semantic still under RE — log only. */
        fprintf(stderr, "[opt] extra (fade_color_index) = %d\n", s_opt_extra);
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
        fprintf(stderr, "[opt] music=%d subs=%d voice=%d dialog=%d extra=%d\n",
                s_opt_music, s_opt_subtitles, s_opt_voice,
                s_opt_dialogues, s_opt_extra);
    }
    return SOLUND_RC_KEEP_OPEN;
}

/* Solund.pic SceneDef. The five toggle slots start at OFF frames
 * (def=N+OFF_DEF_BASE, hover=N+OFF_HOVER_BASE); SolundClick's first
 * non-toggle entry re-syncs them to the current flag state. The exit
 * slot has no active state and stays at OFF frames. */
SceneDef g_solund_scene = {
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

/* =================== Grafika.pic — graphics options ===================== *
 *
 * — 3 buttons:
 * 0x12 toggle speech_x_offset (graphics flag 1 — read by entity VM
 * + ; meaning TBD)
 * 0x13 toggle speech_y_offset (fast-transitions flag — controls 100-tick
 * vs 4-tick fade speed in LoadKomnata + game-over fade)
 * 0x14 exit (return 3)
 *
 * Atlas (Grafika.wyc) frame layout — 12 frames per redraw.
 * (Ghidra decompile of ):
 * button[2] (exit) — static def=8, hover=2.
 * scene.flags |= 1; // SCENE_FLAG_REDRAW every call
 *
 * EARLIER PORT BUG: def_anim and hover_anim were swapped, so the "ON
 * state" indicator (the small visual cue baked into frames 0/1 — what
 * the user calls "siur") only flashed while the cursor was over the
 * button instead of staying visible whenever the toggle was ON. */
/* (Definitions below — forward-declared near the audio toggles so the
 * persist_audio_opts helper can see them. Defaults = 1, set here in the
 * normal flow; ApplySavedSettings re-hydrates from Wacki.sav at boot.) */
static uint8_t s_opt_gfx1 = 1;       /* speech_x_offset */
static uint8_t s_opt_gfx2 = 1;       /* speech_y_offset — fast transitions */
extern SceneDef g_grafika_scene;
static int GrafikaClick(int trigger)
{
    int toggled = 1;
    switch (trigger) {
    case 0x12: s_opt_gfx1 ^= 1; break;
    case 0x13: s_opt_gfx2 ^= 1; break;
    case 0x14:
        /* T34 — persist video_mode (mapped from s_opt_gfx1) before
 * leaving the menu so the setting survives a restart. */
        persist_audio_opts();
        return 3;            /* exit → caller (opszyns) propagates */
    default: toggled = 0; break;
    }
    /*+: def shows the ON/OFF indicator
 * sprite at rest (always painted), hover overlays the highlight sprite
 * on top. Earlier port had def/hover swapped → "siur" indicator was
 * only visible on mouseover. */
    g_grafika_scene.buttons[0].def_anim   = (uint16_t)(s_opt_gfx1 ? 0 : 3);
    g_grafika_scene.buttons[0].hover_anim = (uint16_t)(s_opt_gfx1 ? 6 : 9);
    g_grafika_scene.buttons[1].def_anim   = (uint16_t)(s_opt_gfx2 ? 1 : 4);
    g_grafika_scene.buttons[1].hover_anim = (uint16_t)(s_opt_gfx2 ? 7 : 10);
    /* Force redraw bit ( ` |= 1` in
 * — the original sets SCENE_FLAG_REDRAW on every call
 * so the def-sprite repaint sees the new frame indices. */
    g_grafika_scene.flags |= SCENE_FLAG_REDRAW;
    if (toggled) {
        fprintf(stderr, "[opt-gfx] flag1=%d flag2=%d\n", s_opt_gfx1, s_opt_gfx2);
    }
    return 0;
}

/* Initial frames match flag=ON
 * (s_opt_gfx1=1, s_opt_gfx2=1 defaults). Original handler @ 0x0040ADC0:
 * button[0]: ON → def=0 hover=6 / OFF → def=3 hover=9
 * button[1]: ON → def=1 hover=7 / OFF → def=4 hover=10
 * button[2] exit (static, no toggle): def=8 hover=2. */
SceneDef g_grafika_scene = {
    .background_pic = "Grafika.pic",
    .mask_file      = "Grafika.wyc",
    .on_click       = GrafikaClick,
    .button_count   = 3,
    .flags          = SCENE_FLAG_MOUSE_ONLY,
    .buttons = { { 0x12, 0, 6 }, { 0x13, 1, 7 }, { 0x14, 8, 2 } },
};

/* =================== Pytanie.pic — quit confirmation ==================== *
 *
 * — 2 buttons:
 * 0x12 "Tak" (Yes) → return 3
 * 0x13 "Nie" (No) → return 4
 *
 * Caller semantics differ by entry point:
 * - opszyns dispatcher case 5 (button 0x16): if Pytanie returns 3,
 * set g_game_over_code = 2 and return 3 (exit opszyns + signal quit).
 * - F12 in-game menu (RunGameStageLoop @ 0x0040BFAF): if Pytanie returns
 * 3, CONTINUE the game; else set quit flags. Inverted semantics —
 * Pytanie text presumably reads "Continue playing?" in that context.
 *
 * We just port the handler faithfully and let callers interpret. */
static int PytanieClick(int trigger)
{
    if (trigger == 0x12) return 3;
    if (trigger == 0x13) return 4;
    return 0;
}

/* */
static SceneDef g_pytanie_scene = {
    .background_pic = "Pytanie.pic",
    .mask_file      = "Pytanie.wyc",
    .on_click       = PytanieClick,
    .button_count   = 2,
    .flags          = SCENE_FLAG_MOUSE_ONLY,
    .buttons = { { 0x12, 0xFFFF, 0 }, { 0x13, 0xFFFF, 1 } },
};

/* =================== Sejw.pic + Load.pic — save/load slot lists ========= *
 *
 * Two SceneDefs share the same button layout (12 buttons, ids 0x12..0x1d):
 * - id 0x12 = Anuluj (cancel) → return 4
 * - id 0x13 = Zapisz / Wczytaj (commit) → return 3 + perform action
 * - id 0x14..0x1d = 10 slot rows → select slot, stay open
 *
 * SceneDef → handler binding (verified from PE hex dumps of 0x00445B78 +
 * 0x00445BD8, opcode disassembly + Polish-word semantics — Sejw = "save"):
 * "Sejw.pic" + "Load.wyc" → 0x0040a960 (write current state to slot)
 * "Load.pic" + "Load.wyc" → 0x0040a6b0 (read slot into current state)
 *
 * The earlier port had the SceneDef variable names swapped (g_sejw_scene
 * carried bg="Load.pic", g_load_scene carried bg="Sejw.pic"); fixed in
 * this rewrite — names now match what they do.
 *
 * Original also supports inline slot-name renaming (typing
 * with the keyboard, backspace, etc.) before commit. We omit that —
 * a default name is auto-generated from etap/komnata + a "slot N" suffix,
 * which is sufficient for the user to distinguish slots in the load
 * menu. Renaming UX can be added later if requested.
 *
 * Renderer integration: the slot row layout comes from the Load.wyc atlas
 * (loaded by RunMenuScene into g_menu_asset_10). Hover frames 2..11
 * correspond to slot rows 0..9 — drawX/drawY of each gives the row
 * position. We paint slot names every frame inside the on_click cb's
 * idle branch (trigger == -1), so they refresh as the user selects
 * different slots. Buttons in this menu have def_anim = 0xFFFF (no
 * default sprite), so the text isn't overdrawn except by the actively-
 * hovered hover sprite (acceptable — the highlight indicates focus). */

/* ---- slot-list UI constants -------------------------------------- *
 *
 * Button trigger codes (verb ids attached to each SceneDef button).
 * Used by both SaveSlotClick and LoadSlotClick. */
#define SLOT_BTN_CANCEL              0x12
#define SLOT_BTN_COMMIT              0x13
#define SLOT_BTN_FIRST_SLOT_TRIGGER  0x14   /* slot 0 → 0x14, slot 9 → 0x1d */

/* SceneDef button-frame index for slot N (Load.wyc atlas frames 2..11). */
#define SLOT_BUTTON_FRAME_OFFSET     2

/* Inline-edit keyboard handling. */
#define KEY_ENTER                    0x0D
#define KEY_BACKSPACE                0x08
#define KEY_PRINTABLE_FIRST          0x20   /* ' ' */
#define KEY_PRINTABLE_LAST           0x7F   /* exclusive — DEL excluded */
#define EDIT_NAME_MAX_CHARS          20

/* Slot-row text layout. */
#define SLOT_ROW_INSET_PX            4      /* fill+text inset from row border */
#define SLOT_ROW_TEXT_X_INSET_PX     4      /* extra text X offset past the fill rect */
#define SLOT_ROW_BG_FILL_COLOR       0x01   /* indexed-palette grey/brown */
#define SLOT_ROW_TEXT_COLOR          0x12   /* normal slot-name colour */
#define SLOT_ROW_EDIT_TEXT_COLOR     0xFD   /* highlight palette slot used while editing */

/* Cursor blink rate: ~2 Hz (toggle every 500 ms). */
#define EDIT_CURSOR_BLINK_MS         500

/* RGB for the yellow we force into palette slot 0xFD while editing — see
 * the comment above the palette mutation for why we override here. */
#define EDIT_CURSOR_PALETTE_R        0xFF
#define EDIT_CURSOR_PALETTE_G        0xE0
#define EDIT_CURSOR_PALETTE_B        0x00

/* Click-handler return codes. */
#define CLICK_RET_STAY               0      /* stay on this menu */
#define CLICK_RET_LOAD_COMPLETED     3      /* close menu + resume gameplay */
#define CLICK_RET_USER_CANCELLED     4      /* close menu w/o action */

/* Thumbnail preview pane is at frame 1's draw rect, inset 3 px from the
 * top-left corner. */
#define SLOT_THUMB_FRAME_INDEX       1
#define SLOT_THUMB_INSET_PX          3

static int s_slot_selected = -1;        /* 0..9 = chosen slot; -1 = none */
/* Inline-edit (Save menu only)'s keyboard-input
 * branch ( tracks the slot, the local string buffer
 * accumulates typed chars + Backspace/Enter). When >= 0, paint_slot_list
 * shows that slot with a trailing '_' cursor and bypasses the s_slot_
 * selected highlight logic for it. */
static int  s_edit_slot = -1;
static char s_edit_buf[30];     /* matches WackiSlot.name capacity */
static int  s_edit_len = 0;

/* Hover-frame index per slot row in Load.wyc. Slots 0..9 map to button
 * indices 2..11 in the SceneDef → hover frames 2..11. */
static uint16_t slot_hover_frame(int slot) { return (uint16_t)(slot + 2); }

/* Compose a display string for a slot's slot-row
 * paint: just the slot's stored name field. Empty (never-saved) slots
 * already carry WACKI_DEFAULT_SLOT_NAME ("Pusty") from
 * LoadSaveStateOrInitialize, so they render as "Pusty" naturally. No
 * row prefix — the original lays slots out vertically and the user
 * identifies them by position. */
static void slot_display_name(int slot, char *out, size_t out_sz)
{
    const WackiSlot *s = &g_save.slots[slot];
    if (s->stage_indicator == 0 && !s->name[0]) {
        snprintf(out, out_sz, "%s", WACKI_DEFAULT_SLOT_NAME);
    } else if (s->name[0]) {
        snprintf(out, out_sz, "%s", s->name);
    } else {
        snprintf(out, out_sz, "etap %u k%u",
                 (unsigned)s->etap_id, (unsigned)s->stage_indicator);
    }
}

/* Paint slot names + "*" marker on the selected one. Called every frame
 * from inside the slot-menu cb's idle path, so the text refreshes when
 * the selection changes or after a save/load completes. Reads
 * g_menu_asset_10 (set by RunMenuScene from scene->mask_file). */
/* g_save_thumb_pending — captured backbuffer thumbnail (126×78 indexed)
 * sampled before opszyns menu opens. SaveSlotClick commit copies this
 * into the slot's world_default_snapshot field. Without pre-capture,
 * the save would store the menu image instead of the gameplay scene.
 *
 * g_menu_bg_snapshot — full-size backbuffer snapshot captured before
 * menu paints. Used by RunMenuScene to restore the gameplay scene
 * under sub-fullscreen overlay .pics (opszyns / Sejw / Load / Pytanie)
 * every frame — prevents cursor trails in the margins AND keeps
 * gameplay visible through transparent areas. Marked invalid when no
 * snapshot was captured for the current menu scope (e.g. main-menu
 * Load reached before gameplay starts).
 *
 * Storage for both is declared at the top of this file; only repeated
 * here as a forward-decl comment so the menu code is readable. */

/* Externs used by all the slot-list helpers below. */
extern AnimAsset *g_menu_asset_10;
extern FontHandle *g_default_font;
extern uint8_t *g_back_shadow;

/* Blit the thumbnail preview for the currently-selected slot (or slot
 * 0 if nothing is selected) into the frame-1 box of the slot-list
 * atlas. The thumbnail source is the slot's saved
 * world_default_snapshot — captured scene for filled slots, TV-test
 * pattern for empty slots. */
static void paint_slot_thumbnail(AnimAsset *atlas)
{
    if (atlas->frame_count <= SLOT_THUMB_FRAME_INDEX) return;

    int16_t tx = (int16_t)(atlas->off_drawX[SLOT_THUMB_FRAME_INDEX]
                           + SLOT_THUMB_INSET_PX);
    int16_t ty = (int16_t)(atlas->off_drawY[SLOT_THUMB_FRAME_INDEX]
                           + SLOT_THUMB_INSET_PX);
    int show_slot = (s_slot_selected >= 0 && s_slot_selected < WACKI_SAVE_SLOTS)
                  ? s_slot_selected : 0;
    const uint8_t *thumb_src = g_save.slots[show_slot].world_default_snapshot;

    if (tx >= 0 && ty >= 0 &&
        tx + SAVE_THUMB_W <= WACKI_SCREEN_W &&
        ty + SAVE_THUMB_H <= WACKI_SCREEN_H)
    {
        PaintImageToBackbuffer((uint16_t)tx, (uint16_t)ty,
                               SAVE_THUMB_W, SAVE_THUMB_H, thumb_src);
    }
}

/* Compute the inner fill rectangle (background colour) for slot row
 * `slot`. Returns 0 if the row geometry is out of bounds. */
static int slot_row_fill_rect(AnimAsset *atlas, int slot,
                              int *rx, int *ry, int *rw, int *rh)
{
    uint16_t f = slot_hover_frame(slot);
    if (f >= atlas->frame_count) return 0;

    int x = (int)atlas->off_drawX[f] + SLOT_ROW_INSET_PX;
    int y = (int)atlas->off_drawY[f] + SLOT_ROW_INSET_PX;
    int w = (int)atlas->off_widths [f] - 2 * SLOT_ROW_INSET_PX;
    int h = (int)atlas->off_heights[f] - 2 * SLOT_ROW_INSET_PX;

    if (x < 0 || y < 0 || x >= WACKI_SCREEN_W || y >= WACKI_SCREEN_H) return 0;
    if (w <= 0 || h <= 0) return 0;
    if (x + w > WACKI_SCREEN_W) w = WACKI_SCREEN_W - x;
    if (y + h > WACKI_SCREEN_H) h = WACKI_SCREEN_H - y;

    *rx = x;  *ry = y;  *rw = w;  *rh = h;
    return 1;
}

/* Fill the slot row's inner rectangle with the configured background
 * colour. The sejw.pic palette has slot 0x01 = a visible grey/brown
 * that contrasts with the row border; using any other colour leaks
 * transparency through to the underlying menu image. */
static void fill_slot_row_bg(int rx, int ry, int rw, int rh)
{
    for (int yy = 0; yy < rh; ++yy) {
        uint8_t *row = g_back_shadow + (size_t)(ry + yy) * WACKI_SCREEN_W + rx;
        memset(row, SLOT_ROW_BG_FILL_COLOR, (size_t)rw);
    }
}

/* Force a known-bright yellow into the high palette slot the editing
 * text uses. The sejw.pic palette doesn't normally have a bright
 * colour at 0xFD; without this the editing text comes out near-black
 * and unreadable. The next scene's InstallPalette refresh will
 * overwrite this slot, so the mutation is non-persistent. */
static void install_edit_cursor_palette(void)
{
    extern uint8_t g_palette_rgb[256 * 3];
    g_palette_rgb[SLOT_ROW_EDIT_TEXT_COLOR * 3 + 0] = EDIT_CURSOR_PALETTE_R;
    g_palette_rgb[SLOT_ROW_EDIT_TEXT_COLOR * 3 + 1] = EDIT_CURSOR_PALETTE_G;
    g_palette_rgb[SLOT_ROW_EDIT_TEXT_COLOR * 3 + 2] = EDIT_CURSOR_PALETTE_B;
}

/* Compose the line of text for slot `slot` — either the display name
 * or, if the slot is in inline-edit mode, the editing buffer plus a
 * blinking cursor. Returns the colour-base index for the text render. */
static uint8_t compose_slot_row_text(int slot, char *out, size_t out_sz)
{
    if (slot != s_edit_slot) {
        slot_display_name(slot, out, out_sz);
        return SLOT_ROW_TEXT_COLOR;
    }

    install_edit_cursor_palette();
    int cursor_on = ((SDL_GetTicks() / EDIT_CURSOR_BLINK_MS) & 1u) == 0;
    snprintf(out, out_sz, "%.*s%c",
             s_edit_len, s_edit_buf,
             cursor_on ? '_' : ' ');
    return SLOT_ROW_EDIT_TEXT_COLOR;
}

/* Paint slot row `slot`: fill the inner rectangle then render the
 * display name (or editing buffer) into it. */
static void paint_slot_row(AnimAsset *atlas, int slot)
{
    int rx, ry, rw, rh;
    if (!slot_row_fill_rect(atlas, slot, &rx, &ry, &rw, &rh)) return;

    fill_slot_row_bg(rx, ry, rw, rh);

    char    line[40];
    uint8_t color_base = compose_slot_row_text(slot, line, sizeof line);

    TextRenderTarget t = {
        .stride     = WACKI_SCREEN_W,
        .x          = (uint16_t)(rx + SLOT_ROW_TEXT_X_INSET_PX),
        .color_base = color_base,
        .pixels     = g_back_shadow + (size_t)ry * WACKI_SCREEN_W,
        .font       = g_default_font,
    };
    RenderTextLineToBuffer(&t, (const uint8_t *)line);
}

static void paint_slot_list(void)
{
    AnimAsset *atlas = g_menu_asset_10;
    if (!atlas || !g_default_font || !g_back_shadow) return;
    if (!atlas->off_drawX || !atlas->off_drawY) return;

    paint_slot_thumbnail(atlas);
    for (int i = 0; i < WACKI_SAVE_SLOTS; ++i) {
        paint_slot_row(atlas, i);
    }
}

/* SaveSlotClick — (Sejw.pic handler).
 *
 * Inline-edit flow (matches original):
 * - 1st click on a slot row → select it (s_slot_selected = i).
 * - 2nd click on the same slot → enter inline-edit (s_edit_slot = i,
 * seed buffer with the slot's current name, start text-input).
 * - In edit: typed chars append to s_edit_buf, Backspace pops the last
 * char, Enter commits the rename and exits edit mode.
 * - Click on a different slot during edit → commit current edit, then
 * select the new slot.
 * - Zapisz (0x13) commits the name + writes the save.
 * - Anuluj (0x12) cancels — name unchanged. */
static void end_edit_commit(void)
{
    if (s_edit_slot < 0) return;
    WackiSlot *s = &g_save.slots[s_edit_slot];
    memset(s->name, 0, sizeof s->name);
    if (s_edit_len > 0) {
        size_t n = (size_t)s_edit_len;
        if (n > sizeof s->name - 1) n = sizeof s->name - 1;
        memcpy(s->name, s_edit_buf, n);
        s->name[n] = 0;
    }
    PlatformSetTextInput(0);
    s_edit_slot = -1;
    s_edit_len = 0;
}

static void begin_edit(int slot)
{
    s_edit_slot = slot;
    /* Seed buffer with the slot's current name. Empty/default ("Pusty")
 * slots start with an empty buffer so typing replaces the placeholder. */
    const WackiSlot *s = &g_save.slots[slot];
    if (s->stage_indicator == 0 || strcmp(s->name, WACKI_DEFAULT_SLOT_NAME) == 0) {
        s_edit_len = 0;
        s_edit_buf[0] = 0;
    } else {
        size_t n = strnlen(s->name, sizeof s->name);
        if (n >= sizeof s_edit_buf) n = sizeof s_edit_buf - 1;
        memcpy(s_edit_buf, s->name, n);
        s_edit_buf[n] = 0;
        s_edit_len = (int)n;
    }
    PlatformSetTextInput(1);
}

/* Commit a save to the selected slot. Shared between the Zapisz button
 * (trigger 0x13) and the Enter-in-edit-mode path. Returns 3 on success
 * (= close menu), 0 if no slot selected, 4 if outside gameplay. */
static int save_commit_selected(void)
{
    end_edit_commit();
    if (s_slot_selected < 0) {
        fprintf(stderr, "[save-menu] commit ignored (no slot selected)\n");
        return 0;
    }
    if (g_cur_etap == 0 || g_cur_komnata == 0) {
        fprintf(stderr, "[save-menu] cannot save outside gameplay\n");
        return 4;
    }
    WackiSlot *s = &g_save.slots[s_slot_selected];
    /* Auto-name fallback when the user committed without typing
 * anything (e.g. pressed Zapisz with empty buffer). Matches what
 * the original would store after an empty rename. */
    if (!s->name[0] || strcmp(s->name, WACKI_DEFAULT_SLOT_NAME) == 0) {
        snprintf(s->name, sizeof s->name, "etap %u k%u",
                 (unsigned)g_cur_etap, (unsigned)g_cur_komnata);
    }
    memcpy(s->world_default_snapshot, g_save_thumb_pending,
           sizeof s->world_default_snapshot);
    QuickSaveToSlot((uint16_t)s_slot_selected);
    fprintf(stderr, "[save-menu] saved slot %d (%s)\n",
            s_slot_selected, s->name);
    s_slot_selected = -1;
    return 3;
}

/* Trigger code → slot index conversion (or -1 if not a slot trigger). */
static int slot_index_from_trigger(int trigger)
{
    int slot = trigger - SLOT_BTN_FIRST_SLOT_TRIGGER;
    if (slot < 0 || slot >= WACKI_SAVE_SLOTS) return -1;
    return slot;
}

/* Drain typed characters into the edit buffer. Returns 1 if the user
 * pressed Enter (signalling commit-now), 0 otherwise. */
static int process_typed_chars(void)
{
    uint8_t c;
    while ((c = PlatformPollTypedChar()) != 0) {
        if (c == KEY_ENTER) return 1;

        if (c == KEY_BACKSPACE) {
            if (s_edit_len > 0) s_edit_buf[--s_edit_len] = 0;
        } else if (c >= KEY_PRINTABLE_FIRST && c < KEY_PRINTABLE_LAST) {
            if (s_edit_len < EDIT_NAME_MAX_CHARS) {
                s_edit_buf[s_edit_len++] = (char)c;
                s_edit_buf[s_edit_len]   = 0;
            }
        }
    }
    return 0;
}

static int SaveSlotClick(int trigger)
{
    int slot = slot_index_from_trigger(trigger);

    if (slot >= 0) {
        /* Single click selects + enters edit mode (slight deviation from
         * the original "click then click again" UX — user prefers
         * immediate typing). Switching to another slot mid-edit commits
         * the prior edit first. */
        if (s_edit_slot >= 0 && s_edit_slot != slot) end_edit_commit();
        s_slot_selected = slot;
        if (s_edit_slot != slot) begin_edit(slot);
        fprintf(stderr, "[save-menu] slot %d editing\n", slot);
        return CLICK_RET_STAY;
    }

    if (trigger == SLOT_BTN_CANCEL) {
        /* Drop any in-progress edit without committing. */
        PlatformSetTextInput(0);
        s_edit_slot     = -1;
        s_edit_len      = 0;
        s_slot_selected = -1;
        return CLICK_RET_USER_CANCELLED;
    }

    if (trigger == SLOT_BTN_COMMIT) {
        return save_commit_selected();
    }

    /* Idle frame — drain typed chars (Enter commits + closes), then
     * repaint slot rows so the editing slot's cursor blinks. */
    if (s_edit_slot >= 0 && process_typed_chars()) {
        return save_commit_selected();
    }
    paint_slot_list();
    return CLICK_RET_STAY;
}

/* LoadSlotClick (Load.pic handler). Reads the selected slot via
 * LoadSaveSlot then triggers LoadKomnataScene to re-enter the room
 * the save was made in. */
extern void LoadKomnataScene(uint16_t id);
static int LoadSlotClick(int trigger)
{
    int slot = slot_index_from_trigger(trigger);

    if (slot >= 0) {
        s_slot_selected = slot;
        fprintf(stderr, "[load-menu] slot %d selected\n", slot);
        return CLICK_RET_STAY;
    }

    if (trigger == SLOT_BTN_CANCEL) {
        s_slot_selected = -1;
        return CLICK_RET_USER_CANCELLED;
    }

    if (trigger == SLOT_BTN_COMMIT) {
        if (s_slot_selected < 0) {
            fprintf(stderr, "[load-menu] commit ignored (no slot selected)\n");
            return CLICK_RET_STAY;
        }
        if (g_save.slots[s_slot_selected].stage_indicator == 0) {
            fprintf(stderr, "[load-menu] slot %d is empty\n", s_slot_selected);
            return CLICK_RET_STAY;
        }
        if (LoadSaveSlot((uint16_t)s_slot_selected)) {
            LoadKomnataScene(g_cur_komnata);
            fprintf(stderr, "[load-menu] loaded slot %d → etap %u k%u\n",
                    s_slot_selected,
                    (unsigned)g_cur_etap, (unsigned)g_cur_komnata);
            s_slot_selected = -1;
            return CLICK_RET_LOAD_COMPLETED;
        }
        return CLICK_RET_STAY;
    }

    paint_slot_list();
    return CLICK_RET_STAY;
}

/* (bg="Sejw.pic"). Handler
 * @ 0x0040a960 = SAVE slot picker. */
static SceneDef g_save_menu_scene = {
    .background_pic = "Sejw.pic",
    .mask_file      = "Load.wyc",
    .on_click       = SaveSlotClick,
    .button_count   = 12,
    .flags          = SCENE_FLAG_FORCE_CB,   /* cb fires every frame for
 * idle paint_slot_list */
    .buttons = {
        { 0x12, 0xFFFF,  0 }, { 0x13, 0xFFFF,  1 }, { 0x14, 0xFFFF,  2 },
        { 0x15, 0xFFFF,  3 }, { 0x16, 0xFFFF,  4 }, { 0x17, 0xFFFF,  5 },
        { 0x18, 0xFFFF,  6 }, { 0x19, 0xFFFF,  7 }, { 0x1a, 0xFFFF,  8 },
        { 0x1b, 0xFFFF,  9 }, { 0x1c, 0xFFFF, 10 }, { 0x1d, 0xFFFF, 11 },
    },
    /* paint slot text AFTER hover sprite so it isn't obscured */
    .after_paint = paint_slot_list,
};

/* (bg="Load.pic"). Handler
 * @ 0x0040a6b0 = LOAD slot picker. */
SceneDef g_load_menu_scene = {
    .background_pic = "Load.pic",
    .mask_file      = "Load.wyc",
    .on_click       = LoadSlotClick,
    .button_count   = 12,
    .flags          = SCENE_FLAG_FORCE_CB,
    .buttons = {
        { 0x12, 0xFFFF,  0 }, { 0x13, 0xFFFF,  1 }, { 0x14, 0xFFFF,  2 },
        { 0x15, 0xFFFF,  3 }, { 0x16, 0xFFFF,  4 }, { 0x17, 0xFFFF,  5 },
        { 0x18, 0xFFFF,  6 }, { 0x19, 0xFFFF,  7 }, { 0x1a, 0xFFFF,  8 },
        { 0x1b, 0xFFFF,  9 }, { 0x1c, 0xFFFF, 10 }, { 0x1d, 0xFFFF, 11 },
    },
    .after_paint = paint_slot_list,
};

/* ---- opszyns.pic options-menu constants --------------------------- */

/* Triggers the opszyns.wyc mask emits per button. Each routes to a
 * sub-menu (or to exit-self). The Polish naming convention: "Sejw" is
 * the SAVE picker, "Load" is the LOAD picker — an earlier port comment
 * had them reversed. */
#define OPSZYNS_BTN_GRAFIKA             0x12  /* Grafika.pic */
#define OPSZYNS_BTN_SOLUND              0x13  /* Solund.pic  */
#define OPSZYNS_BTN_SAVE                0x14  /* Sejw.pic    */
#define OPSZYNS_BTN_LOAD                0x15  /* Load.pic    */
#define OPSZYNS_BTN_QUIT                0x16  /* Pytanie.pic */
#define OPSZYNS_BTN_EXIT                0x17  /* close opszyns */

/* OpszynsClick / sub-menu return codes. 0 = stay open, 3 = close +
 * propagate (e.g. save committed, quit confirmed). */
#define OPSZYNS_RC_KEEP_OPEN            0
#define OPSZYNS_RC_CLOSE                3
#define SUBMENU_RC_COMMITTED            3

/* opszyns.wyc atlas — no def frames (slots use FRAME_NONE), one hover
 * frame per button slot in the same order as the buttons[] array. */
#define OPSZYNS_FRAME_NONE              0xFFFF
#define OPSZYNS_BUTTON_COUNT            6

/* Sub-menu helper: run one of opszyns's "commit-propagates-close"
 * children (SAVE / LOAD). Returns CLOSE if the child committed,
 * KEEP_OPEN otherwise. */
static int run_commit_propagating_submenu(SceneDef *scene)
{
    int rc = RunMenuScene(1, scene);
    return rc == SUBMENU_RC_COMMITTED ? OPSZYNS_RC_CLOSE
                                      : OPSZYNS_RC_KEEP_OPEN;
}

/* OpszynsClick — opszyns.pic dispatcher. Maps the clicked button id
 * to its sub-menu (or to exit-self). A non-zero return propagates up
 * to OpenOptionsMenu, which closes the options overlay. */
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
        /* If save committed, propagate the close so the player ends
         * up back in-game instead of hanging in the options menu. */
        return run_commit_propagating_submenu(&g_save_menu_scene);

    case OPSZYNS_BTN_LOAD:
        /* Same propagation for load — a successful load must drop the
         * player into the loaded scene, not back into options. */
        return run_commit_propagating_submenu(&g_load_menu_scene);

    case OPSZYNS_BTN_QUIT: {
        int rc = RunMenuScene(1, &g_pytanie_scene);
        if (rc == PYTANIE_RC_TAK) {
            g_game_over_code = GAME_OVER_USER_QUIT;
            fprintf(stderr, "[opt] Pytanie: quit confirmed → game_over=%d\n",
                    GAME_OVER_USER_QUIT);
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

/* opszyns.pic SceneDef — 6 buttons; no def frame (FRAME_NONE), one
 * hover frame per slot in slot order. */
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

/* Capture g_back_shadow (640×400) downsampled → 126×78 indexed pixels
 * into the pending buffer. Nearest-neighbor sampling. Called right
 * before opszyns menu paints, so the captured image is the gameplay
 * scene (not the menu overlay). */
static void CapturePendingThumbnail(void)
{
    extern uint8_t *g_back_shadow;
    if (!g_back_shadow) return;
    const int src_w = WACKI_SCREEN_W;
    for (int y = 0; y < SAVE_THUMB_H; ++y) {
        int sy = (y * 400) / SAVE_THUMB_H;
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
 * panel. RunGameStageLoop @ 0x0040C0CC behaviour
 * (which read g_script_vars and called RunMenuScene(opszyns)). The
 * original's flag-setter is missing from the disassembly, so we wire
 * the panel click directly here. */
static void OpenOptionsMenu(void)
{
    /* Capture thumbnail BEFORE the menu paints over the backbuffer. */
    CapturePendingThumbnail();
    int rc = RunMenuScene(0, &g_opszyns_scene);
    fprintf(stderr, "[opt] opszyns closed rc=%d\n", rc);
    /* If Pytanie confirmed quit (rc=3 from dispatcher case 0x16), signal
 * scene-loop break so play_demo_scene returns to the menu. */
    if (rc == 3 && g_game_over_code == 2) {
        g_scene_quit = 1;
    }
}

/* =================== sel_tlo.pic — chapter-select UI ==================== *
 *
 * Shown when game_over_code == 3 (player finished a stage and needs to
 * pick the next chapter to play). The screen displays 4 stage buttons;
 * each is either "available" (id=0x12..0x15, def=0..3, hover=5..8) or
 * "already completed" (id=0x26 = neutral / non-clickable, def/hover=0xFFFF).
 * Completion bit comes from `g_completed_stages` (= in PE).
 *
 * Click handler stores the picked stage in `s_chapter_pick` (1..4). The
 * RunGameStageLoop game_over branch reads it and calls LoadStage(picked)
 * to continue gameplay in the chosen chapter.
 *
 * Full AVI sequence is wired in the game_over_code==3 branch (see below):
 * 1. PlaySceneCutsceneAvi(g_stage->alt_avi) — outro of just-finished
 * 2. RunMenuScene(g_sel_tlo_scene) — user picks chapter
 * 3. LoadStage(picked) — load new stage
 * 4. PlaySceneCutsceneAvi(<stashed alt3_avi>) — transition into next
 *
 * RunGameStageLoop @ 0x0040C298 case 3 ordering. */
int s_chapter_pick = 0;              /* 1..4 = picked stage, 0 = none */
extern SceneDef g_sel_tlo_scene;

/* ---- sel_tlo.pic chapter-select constants ------------------------- */

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

/* RunMenuScene return codes. SelTloClick returns 3 ("exit menu") on a
 * picked stage; the caller reads s_chapter_pick (1..5) to dispatch. */
#define SEL_TLO_RC_KEEP_OPEN          0
#define SEL_TLO_RC_PICK_MADE          3

/* s_chapter_pick values — 1..4 = stage 1..4, 5 = Monter finale. */
#define SEL_TLO_PICK_FINALE_STAGE     5

/* Button slot layout in g_sel_tlo_scene.buttons. */
#define SEL_TLO_STAGE_BUTTON_COUNT    4
#define SEL_TLO_ACME_BUTTON_SLOT      4
#define SEL_TLO_BUTTON_COUNT_PARTIAL  4   /* ACME hidden */
#define SEL_TLO_BUTTON_COUNT_ALL      5   /* ACME exposed */

/* Scene flag bits OR'd into g_sel_tlo_scene.flags. FORCE_CB drives the
 * trigger==0 per-tick path; DISABLE_ESC stops ESC from quitting; KEEP
 * IMAGE preserves the painted background between frames. */
#define SEL_TLO_SCENE_FLAGS           0x34

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
            fprintf(stderr, "[chapter-select] picked stage %d\n", s_chapter_pick);
            return SEL_TLO_RC_PICK_MADE;
        }
        fprintf(stderr, "[chapter-select] stage %d already completed — ignore\n",
                idx + 1);
        return SEL_TLO_RC_KEEP_OPEN;
    }

    /* ACME-complete (stage 5 Monter finale). Hit-test only fires when
     * SelTloRefreshButtons has promoted button_count to ALL — all four
     * stages done. The finale loads Dane_12.dta intro, Dane_11.dta
     * gameplay, Dane_13.dta end-credits sting. */
    if (trigger == SEL_TLO_TRIGGER_ACME) {
        s_chapter_pick = SEL_TLO_PICK_FINALE_STAGE;
        fprintf(stderr, "[chapter-select] ACME complete — start finale (stage 5)\n");
        return SEL_TLO_RC_PICK_MADE;
    }
    return SEL_TLO_RC_KEEP_OPEN;
}

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
 * unlock states. Mirrors the original's pre-RunMenuScene button rebuild. */
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

/* is_walkable_at moved to src/scene/walkability.c. */

/* ---- HandleSceneInput constants ----------------------------------- */

/* HUD layout — the panel sits along the bottom of the screen and
 * carries six panel-bar buttons plus three special hit-regions:
 *   OPCJE          ([230..280) × [430..455))   → opens options menu
 *   page-prev (▲)  ([600..630) × [412..442))   → InventoryPagePrev
 *   page-next (▼)  ([599..629) × [443..473))   → InventoryPageNext
 *
 * The Y boundary between "panel click" and "scene click" is 400. */
#define HUD_PANEL_TOP_Y                400

#define OPCJE_BTN_X0                   230
#define OPCJE_BTN_X1                   280
#define OPCJE_BTN_Y0                   430
#define OPCJE_BTN_Y1                   455

#define PAGE_PREV_BTN_X0               600
#define PAGE_PREV_BTN_X1               630
#define PAGE_PREV_BTN_Y0               412
#define PAGE_PREV_BTN_Y1               442

#define PAGE_NEXT_BTN_X0               599
#define PAGE_NEXT_BTN_X1               629
#define PAGE_NEXT_BTN_Y0               443
#define PAGE_NEXT_BTN_Y1               473

/* Scene-input verb codes. SCENE_NEUTRAL_VERB lives in the title block
 * up top (shared with menu/cursor code). */
#define SCENE_USE_ON_ITEM_VERB         0x0F
#define SCENE_PICKUP_TARGET_VAR_IDX    0x0F   /* g_script_vars[0x0F] holds target verb */

/* Free-walk "nearest walkable" search bounds: spiral out by 2 px steps
 * until something within 200 px of the click is walkable. */
#define WALKABLE_SEARCH_MAX_RADIUS     200
#define WALKABLE_SEARCH_STEP_PX        2
#define WALKABLE_SEARCH_DIST_SENTINEL  (1 << 30)

/* Click-dispatch verb ids that also switch the active actor. */
#define ACTOR_VERB_EBEK                1
#define ACTOR_VERB_FJEJ                2

/* ---- HandleSceneInput helpers ------------------------------------- */

static inline int point_in_rect(int px, int py,
                                int x0, int x1, int y0, int y1)
{
    return px >= x0 && px < x1 && py >= y0 && py < y1;
}

/* Squared distance to the nearest walkable pixel within
 * WALKABLE_SEARCH_MAX_RADIUS of (tx, ty). Writes the best point to
 * (*btx, *bty) when found. Returns 1 on hit, 0 if the entire search
 * region is non-walkable. */
static int find_nearest_walkable(int tx, int ty, int *btx, int *bty)
{
    int best_d = WALKABLE_SEARCH_DIST_SENTINEL;
    *btx = tx;  *bty = ty;
    for (int r = 1;
         r <= WALKABLE_SEARCH_MAX_RADIUS && best_d == WALKABLE_SEARCH_DIST_SENTINEL;
         r += WALKABLE_SEARCH_STEP_PX)
    {
        for (int dy = -r; dy <= r; ++dy) {
            for (int dx = -r; dx <= r; ++dx) {
                if (dx * dx + dy * dy > r * r) continue;
                int cx = tx + dx, cy = ty + dy;
                if (is_walkable_at(cx, cy)) {
                    int d = dx * dx + dy * dy;
                    if (d < best_d) {
                        best_d = d;
                        *btx = cx;
                        *bty = cy;
                    }
                }
            }
        }
    }
    return best_d != WALKABLE_SEARCH_DIST_SENTINEL;
}

/* Dispatch an item-combine click: the held item is being used on the
 * target verb. Original wires this from the same path as world-on-item
 * — both routes set var[0x0F] = target verb and fire the verb-0x0F
 * script with this=held_item. */
static void dispatch_item_combine(uint16_t held, uint16_t target_verb)
{
    g_script_vars[SCENE_PICKUP_TARGET_VAR_IDX] = target_verb;
    fprintf(stderr, "[panel] use-on-item: held=0x%04X target=0x%04X "
                    "(var[0x0F]=0x%04X)\n",
            held, target_verb, target_verb);
    g_lmb_handled = 0;
    DispatchClickEvent(held, SCENE_USE_ON_ITEM_VERB);
    g_lmb_handled = 0;
}

/* Handle a click on the panel verb-bar (top row of the panel). Picks
 * up a verb into g_held_item, or routes as item-combine if a held
 * item is already set. */
static void handle_panel_verb_click(void)
{
    extern uint8_t g_dialog_active;
    if (g_dialog_active) {
        fprintf(stderr, "[dlg-click] panel verb=0x%04X "
                        "(held=0x%04X) dialog-active\n",
                g_hover_panel_verb, g_held_item);
    }
    if (g_held_item != SCENE_NEUTRAL_VERB &&
        g_held_item != g_hover_panel_verb)
    {
        uint16_t held = g_held_item;
        g_held_item   = SCENE_NEUTRAL_VERB;
        dispatch_item_combine(held, g_hover_panel_verb);
    } else {
        g_held_item = g_hover_panel_verb;
        fprintf(stderr,
                "[panel] picked up verb=0x%04X (held_item set)%s\n",
                g_held_item,
                g_dialog_active
                    ? " [dlg-active: should this dispatch instead?]"
                    : "");
    }
}

/* Handle a click on the bottom panel (Y >= HUD_PANEL_TOP_Y). Routes
 * to one of: verb-bar pickup, OPCJE menu, page navigation, HUD entity
 * dispatch, or empty-slot diagnostic. */
static void handle_panel_click(int have_hover, uint16_t hover_verb)
{
    if (g_hover_panel_verb != SCENE_NEUTRAL_VERB) {
        handle_panel_verb_click();
        return;
    }

    if (point_in_rect(s_mouse_x, s_mouse_y,
                      OPCJE_BTN_X0, OPCJE_BTN_X1,
                      OPCJE_BTN_Y0, OPCJE_BTN_Y1))
    {
        fprintf(stderr, "[opt] OPCJE clicked at (%d,%d) → opszyns\n",
                s_mouse_x, s_mouse_y);
        OpenOptionsMenu();
        return;
    }

    if (point_in_rect(s_mouse_x, s_mouse_y,
                      PAGE_PREV_BTN_X0, PAGE_PREV_BTN_X1,
                      PAGE_PREV_BTN_Y0, PAGE_PREV_BTN_Y1))
    {
        if (InventoryPagePrev()) {
            PanelPageSwap();
            fprintf(stderr, "[panel] page-prev → page=%u\n", g_panel_page_idx);
        }
        return;
    }

    if (point_in_rect(s_mouse_x, s_mouse_y,
                      PAGE_NEXT_BTN_X0, PAGE_NEXT_BTN_X1,
                      PAGE_NEXT_BTN_Y0, PAGE_NEXT_BTN_Y1))
    {
        if (InventoryPageNext()) {
            PanelPageSwap();
            fprintf(stderr, "[panel] page-next → page=%u\n", g_panel_page_idx);
        }
        return;
    }

    if (have_hover && hover_verb != SCENE_NEUTRAL_VERB) {
        uint16_t held = g_held_item;
        g_held_item   = SCENE_NEUTRAL_VERB;
        fprintf(stderr, "[panel] HUD verb=0x%04X at (%d,%d) — dispatch\n",
                hover_verb, s_mouse_x, s_mouse_y);
        g_lmb_handled = 0;
        DispatchClickEvent(held, hover_verb);
        g_lmb_handled = 0;
        return;
    }

    fprintf(stderr, "[scene] panel click at (%d,%d) — empty slot\n",
            s_mouse_x, s_mouse_y);
}

/* Switch the active actor based on the dispatched verb id (1 = Ebek,
 * 2 = Fjej). Logs the transition when it actually changes. */
static void maybe_switch_active_actor(uint16_t verb)
{
    if (verb == ACTOR_VERB_EBEK) {
        if (g_active_actor != 0) {
            fprintf(stderr, "[active] dispatch → Ebek (was %s)\n",
                    g_active_actor ? "Fjej" : "Ebek");
        }
        g_active_actor = 0;
    } else if (verb == ACTOR_VERB_FJEJ) {
        if (g_active_actor != 1) {
            fprintf(stderr, "[active] dispatch → Fjej (was %s)\n",
                    g_active_actor ? "Fjej" : "Ebek");
        }
        g_active_actor = 1;
    }
}

/* Dispatch a click that resolved to a scene entity (have_hover &&
 * hover_verb != NEUTRAL). Also handles the use-on-item routing when
 * an item is held + the hover lands on a panel slot. */
static void handle_scene_entity_click(uint16_t hover_verb, int active_actor)
{
    fprintf(stderr, "[click] verb=0x%04X at (%d,%d) — dispatch (%s)\n",
            hover_verb, s_mouse_x, s_mouse_y,
            active_actor ? "Fjej" : "Ebek");

    uint16_t this_arg    = g_held_item;
    uint16_t that_arg    = hover_verb;
    int      held_active = (g_held_item != SCENE_NEUTRAL_VERB);
    g_held_item = SCENE_NEUTRAL_VERB;

    if (held_active && g_hover_panel_verb != SCENE_NEUTRAL_VERB) {
        that_arg = SCENE_USE_ON_ITEM_VERB;
        g_script_vars[SCENE_PICKUP_TARGET_VAR_IDX] = g_hover_panel_verb;
        fprintf(stderr, "[click] use-on-item: held=0x%04X target=0x%04X "
                        "(var[0x0F]=0x%04X)\n",
                this_arg, g_hover_panel_verb, g_hover_panel_verb);
    }

    maybe_switch_active_actor(that_arg);

    int both_neutral = (that_arg == SCENE_NEUTRAL_VERB &&
                        this_arg == SCENE_NEUTRAL_VERB);
    if (!both_neutral) {
        g_lmb_handled = 0;
        DispatchClickEvent(this_arg, that_arg);
        g_lmb_handled = 0;
    }
}

/* Handle a free-walk click (no hover, scene click): bind the active
 * actor's walker to the clicked position (with nearest-walkable
 * fallback if the click landed on non-walkable scenery). */
static void handle_free_walk_click(int active_actor)
{
    int tx = s_mouse_x;
    int ty = s_mouse_y;
    int found_walkable = is_walkable_at(tx, ty);
    if (!found_walkable) {
        int btx, bty;
        if (find_nearest_walkable(tx, ty, &btx, &bty)) {
            tx = btx;
            ty = bty;
            found_walkable = 1;
        }
    }

    if (!found_walkable) {
        fprintf(stderr, "[scene] click (%d,%d) unreachable — ignoring\n",
                s_mouse_x, s_mouse_y);
        return;
    }
    if (g_actor[active_actor]) {
        fprintf(stderr, "[scene] %s walk → (%d,%d)\n",
                active_actor ? "Fjej" : "Ebek", tx, ty);
        BindActorWalker(active_actor, tx, ty);
    }
}

/* HandleSceneInput — RMB toggle + hotspot scan + LMB click dispatch.
 *
 * Extracted from the original RunGameStageLoop main-loop click block,
 * lifted to file-scope so ProcessGameFrameTickInner can drive scene
 * input without scene-locals threading.
 *
 * Reads globals: g_current_scene, walk-fld state, s_mouse_x/y,
 * g_lmb_clicked, g_rmb_clicked, g_hover_panel_verb, g_held_item,
 * g_actor[], g_active_actor, g_panel_verb_tab.
 *
 * Writes globals: g_held_item, g_active_actor, g_lmb_clicked (consumed),
 * g_rmb_clicked (consumed), g_lmb_handled (set/cleared around
 * DispatchClickEvent, matching the original engine's invariant).
 *
 * NO rising-edge debounce on g_lmb_clicked / g_rmb_clicked — SDL_MOUSE
 * BUTTONDOWN fires per-press (not per-hold), so the click flag IS the
 * press event. A previous port had an `s_last_lmb` static which could
 * deadlock: if a walk-loop consumed g_lmb_clicked mid-walk and the user
 * clicked again, the new click would set g_lmb_clicked=1 but the outer
 * HandleSceneInput end set s_last_lmb=1, locking future clicks.
 *
 * Re-entry guard: DispatchClickEvent can fire scripts that hit blocking
 * waits (op 0x09 SHOW_TEXT, op 0x52 DIALOG_BEGIN). Those waits pump
 * ProcessGameFrameTickInner which calls back here. Without the guard
 * the same click re-fires repeatedly. */
void HandleSceneInput(void)
{
    static int s_reentry_depth = 0;

    if (!g_current_scene) return;
    if (s_reentry_depth > 0) return;
    ++s_reentry_depth;

    /* RMB → toggle active actor. */
    if (g_rmb_clicked) {
        g_active_actor ^= 1;
        fprintf(stderr, "[scene] RMB → active actor = %s\n",
                g_active_actor ? "Fjej" : "Ebek");
        g_rmb_clicked = 0;
    }

    /* Panel + scene hit-tests. PGFT Inner already runs these before us in
     * the normal flow, but other callers may not have — idempotent. */
    PanelHitTest();
    uint16_t hover_verb = SCENE_NEUTRAL_VERB;
    int have_hover = ClickHitTest((int16_t)s_mouse_x, (int16_t)s_mouse_y,
                                  &hover_verb);
    g_hover_scene_verb = hover_verb;

    if (g_lmb_clicked) {
        /* CONSUME the click immediately, before any dispatch or walker
         * bind. Without this, blocking-wait pumps inside DispatchClick
         * Event call PGFT Inner which would snapshot g_lmb_handled =
         * g_lmb_clicked (still 1!) and UpdateActorMovement would auto-
         * bind the walker to the current mouse pos, clobbering the
         * verb-script's own walker. */
        g_lmb_clicked = 0;
        g_lmb_handled = 1;

        int active_actor = g_active_actor & 1;

        if (s_mouse_y >= HUD_PANEL_TOP_Y) {
            handle_panel_click(have_hover, hover_verb);
        } else if (have_hover) {
            handle_scene_entity_click(hover_verb, active_actor);
            /* NO auto-walk-to-mouse here. The original engine, on a
             * verb-resolving click, clears g_lmb_handled around
             * DispatchClickEvent and never falls through to Update
             * ActorMovement's walker-bind path. The verb script itself
             * walks the actor (op 0x10/0x11/0x12 walk-to + blocking
             * wait) if it needs to approach the target. A previous
             * port-only BindActorWalker() here clobbered whatever path
             * the verb script just set. */
        } else {
            handle_free_walk_click(active_actor);
        }
    }
    --s_reentry_depth;
}

/* ------------------------------------------------------------------------- *
 * T39: play_first_scene_demo removed (inlined into RunGameStageLoop).
 * What follows is the externs/helpers that body used.
 * ------------------------------------------------------------------------- */
/* Run the embedded enter_script for each scene through the bytecode VM.
 * Spawns the per-room NPCs (drut/barstoi/pies in maluch, babcia/deska/
 * domofon in klatka2, pijaki/ptak in kiosk21, dziewczynki/hustawki in
 * plac) so EntityRenderAll has actual entities to draw. */
extern Entity *g_render_list_head;
extern void    EntityWalkerTick(Entity *head);
extern void    EntityRenderAll (Entity *head);

/* T130 — s_entry_dir global retired (was kept as no-op stub since T22B).
 * Replaced all writes (HandleSceneInput, play_demo_scene prologue, etc.)
 * with explicit comment annotations referencing T22B; verb-driven
 * exits via LoadKomnataScene handle the role this global had. */

/* T39 (shipped): play_first_scene_demo removed. Its body was inlined
 * into RunGameStageLoop, the only caller after T22 phase B. */

/* ---- play_demo_scene constants + helpers -------------------------- */

#define ROOM_PALETTE_FILENAME       "paleta.pal"
#define DEFAULT_PANEL_FILENAME      "panel.wyc"

/* g_settings_anim_active bit 0 = "panel visible this komnata". The
 * original reads it from the komnata-table entry; we raise it directly
 * because play_demo_scene is a port shortcut over the original entry. */
#define KOMNATA_FLAG_PANEL_VISIBLE  0x01u

/* Initial actor spawn frame (idle pose) + scene-default positions. */
#define ACTOR_INIT_FRAME            11
#define ACTOR_EBEK_INIT_X           380
#define ACTOR_EBEK_INIT_Y           375
#define ACTOR_FJEJ_INIT_X           300
#define ACTOR_FJEJ_INIT_Y           380

/* Entry-script bytecode lives at fixed PE VA — loads the floor cursor
 * atlases (ids 0x64/0x65) then tail-calls the actor position chain. */
#define ACTOR_ENTRY_SCRIPT_VA       0x004251C8u

/* Perspective scale clamps for the +0x58 scale_pct write. Anchored at
 * Y=PERSPECTIVE_BASELINE_Y (= floor line); above that, actors shrink
 * by g_perspective_min/g_perspective_step. */
#define PERSPECTIVE_BASELINE_Y      400
#define PERSPECTIVE_SCALE_MIN_PCT   30
#define PERSPECTIVE_SCALE_MAX_PCT   160

/* GAME_OVER_USER_QUIT (2) + PYTANIE_RC_TAK (3) are defined earlier in
 * the file — RunMainGameLoop constants + play_demo_scene's F12 handler
 * both consume them, and so does OpszynsClick further up. */

/* QuickSave / QuickLoad live in slot 0 with the literal name "Quick"
 * so the user can tell quicksaves apart from named saves in the Load
 * menu list. */
#define QUICK_SAVE_SLOT             0
#define QUICK_SAVE_DISPLAY_NAME     "Quick"

/* Frame pacing target — 33 ms ≈ 30 fps. Bypassed by --no-pacing for CI. */
#define TARGET_FRAME_DELAY_MS       33

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
    if (stage_has_panel) g_settings_anim_active |=  KOMNATA_FLAG_PANEL_VISIBLE;
    else                 g_settings_anim_active &= ~KOMNATA_FLAG_PANEL_VISIBLE;
}

/* Pick the per-actor atlas to spawn from — NULL for stages where the
 * actor isn't present (Monter stage 5 has neither). */
static void resolve_actor_atlases(AnimAsset *out[2])
{
    extern AnimAsset *g_ebek_atlas, *g_fjej_atlas;
    out[0] = (g_stage && !g_stage->ebek_wyc) ? NULL : g_ebek_atlas;
    out[1] = (g_stage && !g_stage->fjej_wyc) ? NULL : g_fjej_atlas;
}

/* Spawn Ebek (id=1) + Fjej (id=2) as entity-backed actors so the
 * original scripts can position them via op 0x28 SET_ENTITY_XY. Actor
 * entities PERSIST across scene transitions (preserved by EntityList
 * ClearAll); we only spawn on the FIRST scene. NULL atlas → skip. */
static void spawn_persistent_actors_if_needed(AnimAsset *atlases[2])
{
    extern Entity *SpawnActorEntity(uint16_t id, AnimAsset *atlas,
                                    uint16_t init_frame,
                                    int16_t init_x, int16_t init_y);
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

/* Run the original actor entry chain. The script loads the floor cursor
 * atlases then conditionally positions Ebek/Fjej via op 0x28 based on
 * var[6] bit 0 (= "in scene transition" flag, default 0). Call args
 * (this/that) = SCENE_NEUTRAL_VERB matches the engine's convention for
 * all enter_script / room init calls — an earlier port passed (0, 0)
 * which collided with op 0x00 (skip-if-not-this) on reg_id=0. */
static void run_actor_entry_chain(void)
{
    const uint8_t *entry_script =
        (const uint8_t *)xlat_binary_ptr(ACTOR_ENTRY_SCRIPT_VA);
    if (entry_script) {
        fprintf(stderr, "[actor] running entry chain @ 0x%08X\n",
                ACTOR_ENTRY_SCRIPT_VA);
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
    g_current_scene = (const struct DemoScene *)scene;
}

/* Recompute the perspective scale_pct at entity[+0x58] each frame for
 * both actors, matching the prologue of the original UpdateActorMovement.
 * The walker itself doesn't write +0x58. */
static void update_actor_perspective_scale(void)
{
    for (int i = 0; i < 2; ++i) {
        if (!g_actor[i]) continue;
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
 * the platform key queue. Sets *quit on ESC and raises g_game_over_code
 * to the "user quit" sentinel so the dev `--start-stage` loop can bail
 * back to the OS instead of looping back to chapter-select. */
static void handle_gameplay_keys(int *quit)
{
    if (!HasPendingKey()) return;
    uint16_t k = WaitForKey();
    if (k == VK_ESCAPE) {
        g_game_over_code = GAME_OVER_USER_QUIT;
        *quit = 1;
    } else if (k == VK_SPACE) {
        g_active_actor ^= 1;
        fprintf(stderr, "[scene] active actor → %s\n",
                g_active_actor ? "Fjej" : "Ebek");
    }
}

/* F5 quicksave / F9 quickload latches (set by the platform_sdl key
 * handler, consumed + cleared here). Quicksave stamps slot 0's name
 * to "Quick" so it shows up distinctly in the Load menu list. */
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
            /* In-place scene rebuild for the loaded komnata. LoadKomnata
             * Scene preserves persistent actors, frees old BG/FLD, and
             * runs the new komnata's enter_script. */
            LoadKomnataScene(g_cur_komnata);
        }
    }
}

/* F12 pause-menu (Pytanie quit-confirmation). Mirrors the original
 * F12 branch — TAK → quit-to-main-menu, NIE → fall through and keep
 * playing. */
static void handle_pause_menu_request(int *quit, const char **next_scene)
{
    if (!g_pause_menu_request) return;
    g_pause_menu_request = 0;
    extern int RunMenuScene(int, SceneDef *);
    extern SceneDef g_pytanie_scene;
    int rc = RunMenuScene(1, &g_pytanie_scene);
    fprintf(stderr, "[scene] F12 Pytanie rc=%d\n", rc);
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

/* Heavy lifting for one scene. Runs the per-frame gameplay loop until
 * the user quits (ESC / F12→TAK / OPCJE→Quit), a script raises
 * g_game_over_code, or the platform requests shutdown.
 *
 * Setup:                  install_room_palette → LoadKomnataScene →
 *                         prepare_panel_asset → resolve_actor_atlases →
 *                         spawn_persistent_actors_if_needed →
 *                         run_actor_entry_chain → publish_scene_walk_bounds.
 *
 * Per-frame (in order):   exit polls, update_actor_perspective_scale,
 *                         ProcessGameFrameTickInner + FlushFrameToPrimary,
 *                         TickMenuMusic, key handlers, F5/F9, F3 stats,
 *                         F12 pause, frame pacing.
 *
 * The original engine did all of this inline inside RunGameStageLoop;
 * this port path is a single-iteration komnata loop that runs until
 * the actor leaves the stage. Subsequent komnata transitions happen
 * via op 0x20 → ScriptGoToKomnata → LoadKomnataScene without
 * unwinding this function. */
static const char *play_demo_scene(const DemoScene *scene)
{
    install_room_palette();

    LoadKomnataScene(g_cur_komnata);

    prepare_panel_asset();
    AnimAsset *atlases[2];
    resolve_actor_atlases(atlases);
    fprintf(stderr, "[scene] initial entry: panel=%d ebek=%d fjej=%d\n",
            g_panel_asset ? g_panel_asset->frame_count : 0,
            atlases[0]    ? atlases[0]   ->frame_count : 0,
            atlases[1]    ? atlases[1]   ->frame_count : 0);

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

        /* BG paint + entity composite + HUD overlay + cursor live inside
         * PGFT Inner — must run every tick (including blocking-wait
         * pumps inside scripts) or sprite trails will appear. */
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

        if (!g_no_pacing) SDL_Delay(TARGET_FRAME_DELAY_MS);
    }

    cleanup_scene_assets();
    return next_scene;
}

/* ------------------------------------------------------------------------- *
 * RunGameStageLoop —
 *
 * Original control flow (decoded from ):
 *
 * zero script_vars + entity_state;
 *; // ResetInventory
 *;; // clear lists + reset panel
 * if (stage && !(flags & 0x10)) PlaySceneCutsceneAvi(intro_avi);
 * (g_cur_komnata); // LoadKomnata
 * // click dispatch with use-on-item / actor-toggle branch
 * // save UI if requested
 * // SPACE-mid-frame toggle
 * ProcessGameFrameTick;
 * // game-over handling (cases 1/3/4)
 * if (exit_signal) return;
 *
 * Flags:
 * bit 1 (0x02) = FULL RESET (new game): zero vars + ResetInventory + LoadStage
 * bit 4 (0x10) = SKIP INTRO AVI (came from save load)
 * bit 0+4 (0x11) combos used after F12 menu / save UI
 *
 * T39 (shipped): play_first_scene_demo legacy entry inlined here.
 * Previously RunGameStageLoop delegated to play_first_scene_demo which
 * was a thin wrapper after T22 phase B. Inlining removes the
 * indirection and pulls the actor-walk-anim setup + initial DemoScene
 * lookup directly into the canonical RunGameStageLoop body. */

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
        fprintf(stderr,
                "[scene] RunGameStageLoop: LoadKomnataScene(%u) yielded no scene\n",
                cur_komnata);
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
        fprintf(stderr,
                "[game-over=%d] stage %u completed (g_completed_stages=0x%X)\n",
                GAME_OVER_STAGE_END_AVI,
                (unsigned)g_cur_etap, g_completed_stages);
    }
}

/* Play one of the StageDef's per-stage AVIs if it's non-NULL. */
static void play_stage_avi_if(const char *avi, int code, const char *kind)
{
    if (!avi) return;
    fprintf(stderr, "[game-over=%d] %s AVI: %s\n", code, kind, avi);
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
        fprintf(stderr, "[game-over=%d] all stages done → %s + map with "
                        "green button\n",
                GAME_OVER_CHAPTER_PICK, CREDITS_STING_AVI);
        PlaySceneCutsceneAvi(CREDITS_STING_AVI);
    }

    /* Step 5: chapter-select menu. */
    s_chapter_pick = 0;
    RunMenuScene(0, &g_sel_tlo_scene);

    /* Step 6: load picked stage (1..4 = regular, 5 = Monter finale). */
    if (s_chapter_pick >= 1 && s_chapter_pick <= DEV_PICK_FINALE) {
        fprintf(stderr, "[game-over=%d] LoadStage(%d)\n",
                GAME_OVER_CHAPTER_PICK, s_chapter_pick);
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

/* ------------------------------------------------------------------------- *
 * InitializeGameSubsystems — 0x00403A30
 * ------------------------------------------------------------------------- */
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
        snprintf(buf, sizeof buf, "%s/Dane_02.dta", g_cd_path);
        opened = OpenDtaArchiveFile(buf);
    }
    if (!opened) {
        fprintf(stderr, "\nBrak pliku bazy : Dane_02.dta (na CD: %s)\n", g_cd_path);
        PlatformShowMessageBox("Wacki",
            "Nie znaleziono Dane_02.dta — sprawd\xC5\xBA p\xC5\x82yt\xC4\x99 CD.");
        return 0;
    }
    fprintf(stderr, "[init] mounted archive Dane_02.dta\n");

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
        fprintf(stderr, "[init] some resident assets missing — continuing\n");

    extern int InitializeMmTimer(void *);
    static uint8_t mmt[32];
    InitializeMmTimer(mmt);
    return 1;
}
