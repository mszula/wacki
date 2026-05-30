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

/* ------------------------------------------------------------------------- *
 * HandlePytanieClick (the on_click cb for the
 * "Pytanie?" quit-confirmation SceneDef at ).
 *
 * trigger 0 → 0 (init, nothing to do)
 * trigger 0x12 → 3 (TAK / Yes → caller treats 3 as "quit confirmed")
 * trigger 0x13 → 4 (NIE / No → caller loops back to the main menu)
 * anything else → 0
 */
static int HandlePytanieClick(int trigger)
{
    if (trigger == 0x12) return 3;
    if (trigger == 0x13) return 4;
    return 0;
}

static int HandleMainMenuClick(int trigger)
{
    int rc = 0;
    s_menu_flags |= 1;

    switch (trigger) {
    case 0: {                              /* INIT (per ) */
        /* @ 0x0040B100 case 0:
 * - clear back-buffer to colour 0
 * - load Tlo.pal as primary palette
 * - load menu.pal as secondary palette buffer
 * - reset animation frame counter to 10 (first logo frame)
 */
        FlipBuffersClearWith(0);
        FlushFrameToPrimary();

        void *pal = NULL; uint32_t psz = 0;
        if (LoadFileFromDta("Tlo.pal", &pal, &psz) && pal) {
            InstallPalette((uint8_t *)pal, 0);
            xfree(pal);
        }
        if (LoadFileFromDta("menu.pal", &pal, &psz) && pal) {
            /* original engine stores this at fade_target_buf for blending —
 * we don't have the blend pipeline yet, but free to avoid leak */
            xfree(pal);
        }
        s_anim_frame   = 10;
        s_anim_delay   = 0;
        s_save_request = 0;
        /* 0 in HandleMainMenuClick:
 * path = BuildAssetPath("Dane_01.dta", NULL)
 * handle = (&g_persp_band_count, path, 1) // open looped
 * (&g_persp_band_count, handle) // start
 * Dane_01.dta on this build is a plain RIFF/WAVE on disk. */
        PlayMenuMusic("Dane_01.dta", 1);
        break;
    }
    /* cb is only called with a non-(-1) trigger when a button was clicked
 * (RunMenuScene already filters). The original gated each case on
 * g_panel_cursor_redirect2 (LMB flag) which the host engine cleared *before*
 * calling the cb (per RunMenuScene @ 0x0040B5E0) — so the gate was
 * effectively always false there too. The visible action is reached
 * via the secondary g_lmb_handled flag, which is the saved click
 * state. We simply act on the trigger unconditionally here. */
    case 0x12: {                            /* Load game submenu */
        /* Reuse the same g_load_menu_scene used by opszyns dispatcher.
 * LoadSlotClick handles slot pick → LoadSaveSlot + LoadKomnataScene
 * → returns 3 (committed). Cancel returns 4. Anything else stays
 * in the load menu loop. After commit we hand control back to
 * the outer menu loop via rc=5, which routes through
 * RunGameStageLoop(0x10) (skip intro, use restored g_cur_etap /
 * g_cur_komnata / script_vars from the loaded slot). */
        extern SceneDef g_load_menu_scene;
        /* Snapshot main-menu title.pic backbuffer so Load's overlay
 * margins show the title art instead of an uninitialised buffer
 * (palette index 0 in the new palette can be white/garbage). */
        SnapshotBackbufferForMenu();
        int r = RunMenuScene(1, &g_load_menu_scene);
        rc = (r == 3) ? 5 : 2;     /* 3=committed → start gameplay; else
 * back to main menu */
        break;
    }
    case 0x13:   rc = 6;  s_menu_flags &= ~1u; break;   /* New game (film) */
    case 0x14:   s_save_request = 1;            break;   /* Maluch — start */
    case 0x15:   rc = 8;                        break;   /* Quit */
    case 0x16:   rc = 9;                        break;   /* Credits */
    }

    /*'s trailing block:
 * if ( && (anim_frame_index < 0xF || 0x12 < anim_frame_index)) {
 * &= ~1;
 * i.e. once the Maluch click latch (s_save_request) is set AND the
 * background flipbook is OUTSIDE the "doors closing" frames 15..18,
 * actually return 7 to the caller (= RunMainGameLoop case 7 = prologue
 * / play the game). The frame gate gives the title animation time to
 * finish its rotation pose before transitioning. */
    if (s_save_request && (s_anim_frame < 0xF || s_anim_frame > 0x12)) {
        rc = 7;
        s_menu_flags &= ~1u;
        s_anim_delay = 1;
    }

    if (trigger > 0)
        fprintf(stderr, "[menu] click trigger=0x%02X rc=%d\n", trigger, rc);

    /* --- trailing block: title-animation tick ----
 * Every (s_anim_delay -> 0) ticks, advance the animation frame and
 * draw it from the mask atlas (= asset 1,10 = sel_guz.wyc). Frames
 * 10..(count-1) form the animated WACKI logo. */
    AnimAsset *a = g_menu_asset_10;
    if (a && s_anim_delay < 1) {
        if (s_anim_frame >= a->frame_count) s_anim_frame = 10;
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
            /* Colour-keyed paint (mode 0) so the frame's index-0 pixels
 * stay transparent — otherwise they'd overwrite the button
 * underneath with white (= Tlo.pal[0]) and the WACKI logo
 * "halo" would punch a hole into the rest of the screen. */
            BlitSpriteToBackbuffer(dx, dy, 0, 0, w, h, w, h,
                                   a->pixel_ptrs[s_anim_frame], 0);
        }
        ++s_anim_frame;
        /* ~10 fps for the WACKI logo flipbook: at the 60 fps menu pacing
 * (SDL_Delay(16) below) a delay of 6 ticks → one new frame every
 * ~100 ms. The original used a hard-coded reset value of 6 here
 * (anim_delay_counter ← 6 on every advance). */
        s_anim_delay = 6;
    } else {
        --s_anim_delay;
    }
    /* Original: any non-zero return from the cb stops the music handle
 * (`(&g_persp_band_count, ); = 0xffff;`).
 * Mirror that so navigating away from the title kills the music. */
    if (rc != 0) StopMenuMusic();
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

int RunMenuScene(int transition_mode, SceneDef *scene)
{
    (void)transition_mode;

    void      *bg_raw  = NULL; uint32_t bg_size = 0;
    AnimAsset *buttons = NULL;
    int        bg_loaded = 0;

    if (scene->background_pic)
        bg_loaded = LoadFileFromDta(scene->background_pic, &bg_raw, &bg_size);
    if (scene->mask_file)
        buttons = LoadAssetFromDtaBase(scene->mask_file);

    /* @ 0x0040B5E0: the mask atlas is registered as
 * entity (kind=1, id=10) so the on_click handler can fetch it via
 * (1, 10) for its background animation block. */
    g_menu_asset_10 = buttons;

    if (!bg_loaded) FlipBuffersClearWith(0);

    fprintf(stderr, "[menu] entered: bg='%s' mask='%s' atlas-frames=%d btns=%d\n",
            scene->background_pic ? scene->background_pic : "(none)",
            scene->mask_file      ? scene->mask_file      : "(none)",
            buttons ? buttons->frame_count : 0,
            scene->button_count);

    /* INIT the on_click handler once with trigger 0 (case 0 in
 * HandleMainMenuClick: load Tlo.pal/menu.pal, start CD music). */
    if (scene->on_click) scene->on_click(0);

    int rc = 0;
    do {
        /* Pump events first so g_lmb_clicked / g_key_state reflect this
 * tick's input before we run the click+animation callback. */
        PumpWin32Messages();

        /* Hover detection:
 * walk the buttons, find the one whose def_anim rect contains
 * the mouse cursor, and use its .id as the "hover key" passed to
 * the on_click callback every frame. (Original engine stores this
 * in g_hover_scene_verb and resets to 0x26 when nothing is hovered;
 * here we just pass it directly to cb.) */
        int hover_btn = (buttons && (s_mouse_x | s_mouse_y))
                      ? hit_test_buttons(scene, buttons, s_mouse_x, s_mouse_y)
                      : -1;
        uint16_t hover_id = (hover_btn >= 0)
                          ? scene->buttons[hover_btn].id : 0x26;

        /* Compose the frame into the shadow buffer (bottom → top):
 * 1. background .pic (if scene has one)
 * 2. cb — paints the animated full-screen WACKI flipbook
 * 3. default-state button sprites for every button
 * 4. hover-state sprite for the hovered button (if any)
 */
        if (bg_loaded) {
            /* Treat sub-fullscreen .pics as transparent overlays (Pytanie
 * dialog), fullscreen ones as opaque scene backgrounds. The
 * RAWB header at +4/+6 stores the size; quick peek to decide. */
            const uint8_t *bp = (const uint8_t *)bg_raw;
            int bg_w = bp[4] | (bp[5] << 8);
            int bg_h = bp[6] | (bp[7] << 8);
            int overlay = (bg_w < WACKI_SCREEN_W || bg_h < 400);
            /* Sub-fullscreen overlay bg leaves margins around the .pic.
 * Without restoring something, cursor trails / closed
 * inner-menu remnants accumulate. If a gameplay snapshot
 * was captured (OpenOptionsMenu), restore it under the
 * overlay so margins show the gameplay scene (and the
 * cursor's previous position gets wiped each frame). For
 * scenes without snapshot (main menu Load reached BEFORE
 * gameplay starts), fall back to clear-with-0. */
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

        /* Click → fire cb with the hovered button's id (trigger). When
 * nothing is hovered, pass -1 so the cb's per-frame logic still
 * runs (animation tick) but no switch-case matches. */
        if (scene->on_click) {
            int trigger = -1;
            if (g_lmb_clicked && hover_btn >= 0) {
                trigger = (int)hover_id;
                /* g_panel_cursor_redirect2 is the "this-frame LMB" flag the cb cases
 * gate on. We can't model that without the whole
 * entity system, but firing trigger on the click frame is
 * the same observable behaviour. */
            }
            g_lmb_clicked = 0;
            int r = scene->on_click(trigger);
            if (r > 0) rc = r;
        }
        if (buttons) {
            /* Default-state buttons (always drawn) */
            for (int i = 0; i < scene->button_count; ++i)
                paint_anim_button(buttons, scene->buttons[i].def_anim);
            /* Hover overlay (one sprite at most, on top of everything) */
            if (hover_btn >= 0)
                paint_anim_button(buttons,
                                  scene->buttons[hover_btn].hover_anim);
        }
        /* Optional overlay pass — runs AFTER button + hover sprites.
 * Save/Load menus use this to repaint the slot text on top of
 * the row-highlight hover sprite (which otherwise covers it). */
        if (scene->after_paint) scene->after_paint();
        /* T31 v2 — cursor sprite on top of the menu. In the menu there's no
 * scene hover-verb (no clickable scene objects) and no held item, so
 * the state machine settles on state 0 = olowek (default arrow). The
 * call still drives frame advance for the unused anim states so the
 * cursor is animated the moment we re-enter gameplay. */
        g_hover_scene_verb = 0x26;
        g_hover_panel_verb = 0x26;
        UpdateCursorState();
        PaintCursor();
        /* Single flush per iteration — once everything is on the shadow */
        FlushFrameToPrimary();
        /* Top up the music queue so the loop is seamless. */
        TickMenuMusic();

        if (HasPendingKey()) {
            uint16_t k = WaitForKey();
            if (k == VK_ESCAPE && !(scene->flags & SCENE_FLAG_DISABLE_ESC))
                rc = 4;
        }
        if (PlatformShouldQuit()) rc = 99;
        SDL_Delay(16);                        /* ~60 fps pacing */
    } while (rc == 0);

    if (buttons) FreeAsset(buttons);
    if (bg_raw)  xfree(bg_raw);
    g_menu_asset_10 = NULL;       /* invalidated after free */
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

void RunMainGameLoop(void)
{
    LoadSaveStateOrInitialize();

    /* T34 — push Wacki.sav settings into in-memory s_opt_* + audio mixer
 * (music/sfx/sound enable flags). Without this the saved prefs were
 * loaded into g_save.settings but never had any effect — opszyns
 * always opened with all-on regardless of what the user toggled
 * last session. */
    ApplySavedSettings();

    /* Tlo.pal — installed here because we haven't ported the original
 * InitializeStage that does it on engine boot. */
    {
        void *pal = NULL; uint32_t psz = 0;
        if (LoadFileFromDta("Tlo.pal", &pal, &psz) && pal) {
            InstallPalette((uint8_t *)pal, 0);
            xfree(pal);
        }
    }

    /* flags=4 (FORCE_CB only).
 * Buttons {id, def_anim, hover_anim}: 0x12 Load, 0x13 New, 0x14 Options,
 * 0x15 Quit, 0x16 Credits. */
    SceneDef title = {
        .background_pic = NULL,
        .mask_file      = "Tlo.wyc",
        .on_click       = HandleMainMenuClick,
        .button_count   = 5,
        .flags          = SCENE_FLAG_FORCE_CB,
        .buttons = {
            { .id = 0x12, .def_anim = 0, .hover_anim = 5 },
            { .id = 0x13, .def_anim = 1, .hover_anim = 6 },
            { .id = 0x14, .def_anim = 2, .hover_anim = 7 },
            { .id = 0x15, .def_anim = 3, .hover_anim = 8 },
            { .id = 0x16, .def_anim = 4, .hover_anim = 9 },
        },
    };

    /* DEV --start-stage N: skip menu+intro, show chapter-select MAP
 * (sel_tlo.pic) with stages 1..N marked as completed. User picks
 * stage from the map, then that stage runs normally. Lets us test
 * any stage without playing through earlier ones to trigger the
 * map. Loop so that returning from one stage re-shows the map
 * (e.g. ESC out of stage 2 → back to map). */
    if (g_dev_start_stage >= 1 && g_dev_start_stage <= 5) {
        int N = g_dev_start_stage;
        /* Mirror RunGameStageLoop's flag-2 FULL RESET cleanup. */
        memset(g_script_vars, 0, sizeof g_script_vars);
        {
            uint16_t *es = (uint16_t *)g_entity_state;
            for (int idx = 0; idx < 0x8E; ++idx)
                es[idx * 4 + 1] = 0;       /* in_inventory_flag */
        }
        ResetInventory();
        /* Mark stages 1..(N-1) as completed. Intent declared in main.c:
 * "--start-stage N jumps straight into stage N gameplay." For N=4
 * that's stages 1,2,3 done + stage 4 available on the map; for
 * N=5 all 4 stages done → map shows assembled ACME + green
 * "finale" button (id=0x16) enabled. Previous mask (1<<N)-1 had
 * stage N itself marked done, making it unclickable. */
        g_completed_stages = (uint32_t)((1u << (N - 1)) - 1u);
        fprintf(stderr, "[wacki] dev-start: chapter-select map, completed_mask=0x%X (stages 1..%d done)\n",
                g_completed_stages, N - 1);

        while (!PlatformShouldQuit()) {
            extern SceneDef g_sel_tlo_scene;
            extern int  SelTloRefreshButtons(void);
            extern int  s_chapter_pick;
            (void)SelTloRefreshButtons();
            s_chapter_pick = 0;
            RunMenuScene(0, &g_sel_tlo_scene);
            if (PlatformShouldQuit()) return;
            /* Pick: 1..4 = stage button (id 0x12..0x15), 5 = ACME-complete
 * green button (id 0x16, "Monter" finale = Dane_12/11/13). */
            if (s_chapter_pick < 1 || s_chapter_pick > 5) {
                fprintf(stderr, "[wacki] dev-start: no stage picked — exit\n");
                return;
            }
            fprintf(stderr, "[wacki] dev-start: stage %d picked from map\n",
                    s_chapter_pick);
            if (!LoadStage((uint16_t)s_chapter_pick)) {
                fprintf(stderr, "[wacki] dev-start: LoadStage(%d) failed\n",
                        s_chapter_pick);
                continue;
            }
            int played_stage = s_chapter_pick;
            RunGameStageLoop(0x10);   /* flag 0x10 = SAVE-LOAD: skip intro,
 * use pre-loaded stage state */
            /* Stage 5 (Monter finale) is terminal — after Dane_11.dta
 * + Dane_13.dta credits sting plays in case 4 (alt_avi +
 * alt3_avi), the original RunGameStageLoop returns to
 * RunMainGameLoop's inner loop which immediately re-enters
 * RunMenuScene(title) (= main menu). In dev mode the title
 * loop was skipped on startup, so the equivalent terminal
 * action is to return from the dev loop entirely — back to
 * WinMain / OS. Without this guard the dev loop bounces
 * back to RunMenuScene(sel_tlo) and shows the assembled-
 * ACME map again instead of the menu the user expects. */
            if (played_stage == 5) {
                fprintf(stderr, "[wacki] dev-start: Monter finale complete → exit (= main menu in normal flow)\n");
                return;
            }
            /* Loop back to the map ONLY for stage-progress signals:
 * Everything else — explicit quit (2 from F12+TAK / ESC /
 * opszyns→Quit), hard-quit (99), or any other unknown code
 * — means "user wants out of the dev session". Return to the
 * OS instead of bouncing back to sel_tlo. In normal flow
 * (non-dev), RunMainGameLoop's `> 4` check passes 2 through
 * to the title-screen loop; here in dev mode the title was
 * skipped so we treat 2 as terminal. */
            if (g_game_over_code != 0 &&
                g_game_over_code != 1 &&
                g_game_over_code != 3 &&
                g_game_over_code != 4) {
                fprintf(stderr, "[wacki] dev-start: game_over=%d → exit\n",
                        g_game_over_code);
                return;
            }
            /* Re-mark all stages prior to N as completed so map shows
 * progress (the played stage may set its own bit via script). */
            g_completed_stages |= (uint32_t)((1u << (N - 1)) - 1u);
        }
        return;
    }

    /* OUTER loop: each iteration plays the intro AVI then runs the
 * menu-re-entry inner loop. */
    int outer = 1;
    while (outer) {
        if (PlatformShouldQuit()) return;
        PlaySceneCutsceneAvi("Dane_10.dta");

        int inner = 1;
        while (inner) {
            int rc = RunMenuScene(1, &title);
            if (PlatformShouldQuit() || rc == 99) return;

            switch (rc) {
            case 4: case 8: {           /* Pytanie? quit-confirm */
                SceneDef q = {
                    .background_pic = "Pytanie.pic",
                    .mask_file      = "Pytanie.wyc",
                    .on_click       = HandlePytanieClick,
                    .button_count   = 2,
                    .flags          = SCENE_FLAG_MOUSE_ONLY,
                    .buttons = {
                        { .id = 0x12, .def_anim = 0xFFFF, .hover_anim = 0 },
                        { .id = 0x13, .def_anim = 0xFFFF, .hover_anim = 1 },
                    },
                };
                int rc2 = RunMenuScene(1, &q);
                if (rc2 == 3) {         /* TAK → bomb + exit */
                    play_bomb_explosion();
                    return;
                }
                /* NIE (rc2 == 4) → fall through: inner loop continues,
 * which re-enters RunMenuScene(title) — same as original
 * after Pytanie returns without bVar1=false. */
                break;
            }
            case 5: {                   /* Start at selected save slot */
                /* @ 0x0040BC4A:
 *
 * LoadSaveSlot restores g_cur_komnata (g_cur_komnata) +
 * g_script_vars (script_vars) + g_entity_state (entity_state)
 * from Wacki.sav slot N. */
                extern uint16_t g_selected_save_slot;
                LoadSaveSlot(g_selected_save_slot);
                RunGameStageLoop(0x10);
                break;
            }
            case 6:                     /* "New game" (film-reel button) */
                /* Original: FindScriptByStageAndRoom + RunScriptInterpreter
 * (0, 0, &); bVar1=false;
 * The intro script plays the credits/film cutscene. We
 * don't have the VM wired to assets yet — fall back to
 * the simplest faithful behaviour: break the inner loop
 * so outer re-plays the title intro AVI (Dane_10.dta),
 * which is what the user observed as "film reel works
 * and plays the intro". */
                inner = 0;
                break;
            case 7: {                   /* Prologue — Maluch "start game" */
                /* Original: RunScriptInterpreter(0, 0, &)
 * then RunGameStageLoop(2).
 *
 * Per Wacky.scr [etap]1 [komnata]init the prologue runs
 * these three steps:
 * 1. [animacja] fiacik.wyc / [sampl] fiacik.wav (0,20)
 * → in-place menu animation of the Maluch driving
 * left across the title (structurally identical
 * to the bomb explosion).
 * 2. Show load.pic + load.wyc progress bar ("Lołding").
 * 3. Hand off to RunGameStageLoop(2) — gameplay loop
 * proper. Until the script VM is wired to the
 * entity walker, the loop body still delegates to
 * play_first_scene_demo (port shortcut tracked
 * under C4/B1). */
                play_fiacik_intro();
                play_loading_screen();
                RunGameStageLoop(2);          /* flag 2 = full reset, new game */
                inner = 0;                    /* back to title on return */
                break;
            }
            case 9:                     /* "Alt-menu" — Credits set */
                PlaySceneCutsceneAvi("Dane_12.dta");
                break;
            }
            if (g_game_over_code > 4) return;
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
void LoadKomnataScene(uint16_t id)
{
    extern Entity *g_actor[2];
    extern void FreeAsset(AnimAsset *a);
    if (id == 0) return;

    /* Step 1 — full actor reset ( room-reset block
 * @ 0x00402DB0). The original calls to clear all per-
 * entity VM scratch state (+0x32 pc, +0x34/0x36/0x38 loop counters,
 * +0x3C delay, +0x40/0x42 osc counters, +0x4C/0x50 walker remainder,
 * bits 0+2 of +0x3A) and then re-binds +0x2C to entry [5] idle
 * bytecode (= anim_tab[+0x14]). Without this, the previous room's
 * walker bytecode + patched op 0x15 target stay bound; next per-
 * entity VM tick re-plants the path from the new spawn position
 * toward the OLD scene's target → actor "drifts diagonally" toward
 * the previous exit. */
    extern uint32_t g_stage_va;
    extern const void *PeLoaderRead(uint32_t va);
    extern const void *xlat_binary_ptr(uint32_t va);
    extern uint32_t ent_ptr_intern(void *p);
    const uint8_t *sd = g_stage_va
        ? (const uint8_t *)PeLoaderRead(g_stage_va) : NULL;
    for (int i = 0; i < 2; ++i) {
        if (!g_actor[i]) continue;
        uint8_t *eb = (uint8_t *)g_actor[i];
        /* reset — full per-entity VM scratch clear. */
        *(uint16_t *)(eb + 0x3A) &= (uint16_t)~5u;  /* bits 0 + 2 cleared */
        *(uint16_t *)(eb + 0x38) = 0;
        *(uint16_t *)(eb + 0x36) = 0;
        *(uint16_t *)(eb + 0x34) = 0;
        *(uint16_t *)(eb + 0x3C) = 0;
        *(uint16_t *)(eb + 0x42) = 0;
        *(uint16_t *)(eb + 0x40) = 0;
        *(uint16_t *)(eb + 0x32) = 0;
        *(uint32_t *)(eb + 0x50) = 0;
        *(uint32_t *)(eb + 0x4C) = 0;
        /* Also clear walker target so a subsequent op 0x15 replant
 * (after dxr/dyr both zeroed above) doesn't carry a stale target
 * from the previous scene. */
        *(uint16_t *)(eb + 0x54) = 0;
        *(uint16_t *)(eb + 0x56) = 0;
        /* Bind entry [5] idle bytecode ( line
 * = anim_tab[+0x14] = entry [5]). */
        if (sd) {
            uint32_t anim_tab_va =
                *(const uint32_t *)(sd + (i ? 0x10 : 0x0C));
            const uint8_t *anim_tab =
                (const uint8_t *)PeLoaderRead(anim_tab_va);
            if (anim_tab) {
                uint32_t bc_va = *(const uint32_t *)(anim_tab + 0x14);
                const void *bc = xlat_binary_ptr(bc_va);
                if (bc)
                    *(uint32_t *)(eb + 0x2C) = ent_ptr_intern((void *)bc);
            }
        }
    }

    /* Step 2 — free old per-scene assets (BG raw blob + FLD asset). */
    if (g_scene_bg_raw)   { xfree(g_scene_bg_raw);   g_scene_bg_raw   = NULL; }
    g_scene_bg_size = 0;
    if (g_scene_fld_asset){ FreeAsset(g_scene_fld_asset); g_scene_fld_asset = NULL; }
    g_walk_fld_pixels = NULL;
    g_walk_fld_w = g_walk_fld_h = 0;
    g_walk_fld_ox = g_walk_fld_oy = g_walk_fld_stride = 0;
    /* Drop prior scene's BG atlas copy — next komnata captures its own. */
    FreeSceneBgAtlas();
    StopMenuMusic();

    /* Step 3 — script-level load. Returns komnata name string. */
    const char *name = LoadKomnata(id);
    if (!name) {
        fprintf(stderr, "[scene] LoadKomnataScene(%u) — LoadKomnata failed\n", id);
        return;
    }

    /* Step 4 — synthesize the DemoScene from the komnata name + current
 * stage (Fix #26 — hardcoded s_demo_scenes[] removed):
 * walk_x0..y1 are left at 0; the actual walk box comes from FLD
 * (off_drawX/Y/widths/heights[0]) loaded below — g_walk_x0..y1
 * fallback is only used when no FLD is present. */
    static DemoScene s_synth;
    static char s_synth_name[64], s_synth_fld[64], s_synth_music[64];
    snprintf(s_synth_name, sizeof s_synth_name, "%s", name);
    const char *dot = strrchr(name, '.');
    size_t base = dot ? (size_t)(dot - name) : strlen(name);
    if (base >= sizeof s_synth_fld) base = sizeof s_synth_fld - 5;
    memcpy(s_synth_fld, name, base);
    memcpy(s_synth_fld + base, ".fld", 5);
    snprintf(s_synth_music, sizeof s_synth_music,
             "Tlo_%u_%ua.wav", (unsigned)g_cur_etap, (unsigned)g_cur_komnata);
    s_synth = (DemoScene){
        .name = s_synth_name, .bg_pic = s_synth_name,
        .fld_file = s_synth_fld, .music_wav = s_synth_music,
        .walk_x0 = 0, .walk_y0 = 0, .walk_x1 = 0, .walk_y1 = 0,
    };
    const DemoScene *s = &s_synth;
    g_current_scene = (const struct DemoScene *)s;
    g_walk_x0 = s->walk_x0; g_walk_x1 = s->walk_x1;
    g_walk_y0 = s->walk_y0; g_walk_y1 = s->walk_y1;

    void *bg = NULL; uint32_t bg_sz = 0;
    if (s->bg_pic && LoadFileFromDta(s->bg_pic, &bg, &bg_sz) && bg) {
        g_scene_bg_raw  = bg;
        g_scene_bg_size = bg_sz;
    }

    /* Walkability FLD — only synthesize "<pic-basename>.fld" and load if
 * bg-mask-setup (op 0x2C, runs inside LoadKomnata step 3) didn't
 * already publish g_walk_fld_pixels. The script-named FLD is the
 * authoritative source ( game, which has no
 * separate "step 4 synth fld load" — that was a port shortcut for
 * komnaty where pic/fld share a basename). Step 4 stays only as a
 * fallback for scenes where the script never calls bg-mask-setup. */
    if (s->fld_file && !g_walk_fld_pixels) {
        AnimAsset *fld = LoadAssetFromDtaBase(s->fld_file);
        g_scene_fld_asset = fld;
        if (fld && fld->pixel_ptrs && fld->pixel_ptrs[0]) {
            g_walk_fld_pixels = fld->pixel_ptrs[0];
            g_walk_fld_w  = fld->off_widths [0];
            g_walk_fld_h  = fld->off_heights[0];
            g_walk_fld_ox = (int16_t)fld->off_drawX[0];
            g_walk_fld_oy = (int16_t)fld->off_drawY[0];
            g_walk_fld_stride = (g_walk_fld_w + 7) / 8;
            fprintf(stderr, "[fld] %s: %dx%d @ (%d,%d) stride=%d\n",
                    s->fld_file, g_walk_fld_w, g_walk_fld_h,
                    g_walk_fld_ox, g_walk_fld_oy, g_walk_fld_stride);
        } else {
            fprintf(stderr, "[fld] %s: load failed — falling back to bbox\n",
                    s->fld_file);
        }
    }

    /* Step 5 — music. */
    if (s->music_wav) PlayMenuMusic(s->music_wav, 1);

    /* Step 6 — clean BG repaint. LoadKomnata runs enter_va + 2 embedded
 * frame ticks + second_va inside step 3. The
 * embedded ticks process EntityRenderAll which paints both the BG
 * sprite (flag-0x60 one-shot) AND any other entities (HUD panel
 * buttons, walk-behinds) to the backbuffer. When control returns
 * here, the outer ProcessGameFrameTickInner that called us is
 * mid-iteration — it already passed its top-of-tick BG paint and
 * is about to call EntityRenderAll again. Without this reset, the
 * second EntityRenderAll paints entities AGAIN over the embedded
 * tick's earlier paint, but at slightly advanced walker positions
 * → 1-frame ghost trail visible right after entering the komnata.
 * Repainting the BG here wipes the embedded tick's entity paint;
 * the outer EntityRenderAll then paints fresh at current positions. */
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

/* SolundClick — Solund.pic SceneDef
 * has 6 buttons (ids 0x12..0x17). Per Ghidra mapping (T103):
 * 0x12 → music_on
 * 0x13 → subtitles — gates op 0x09 SHOW_TEXT
 * 0x14 → voice_on — gates PlayDialogLine
 * 0x15 → dialogues — gates op 0x52/0x53
 * 0x16 → extra — semantic under RE
 * 0x17 → exit
 *
 * Each toggle flips its flag + applies immediately to the relevant
 * subsystem (audio mixer / global gate). Exit triggers Wacki.sav write
 * via persist_audio_opts. */
extern SceneDef g_solund_scene;
extern void AudioSetVoiceEnabled(int on);    /* audio.c */
static int SolundClick(int trigger)
{
    int toggled = 1;
    switch (trigger) {
    case 0x12:
        s_opt_music ^= 1;
        AudioSetMusicEnabled(s_opt_music);
        break;
    case 0x13:
        s_opt_subtitles ^= 1;
        g_subtitles_on = s_opt_subtitles;
        break;
    case 0x14:
        s_opt_voice ^= 1;
        AudioSetVoiceEnabled(s_opt_voice);
        break;
    case 0x15:
        s_opt_dialogues ^= 1;
        g_dialogues_on = s_opt_dialogues;
        break;
    case 0x16:
        s_opt_extra ^= 1;
        /* speech_color_index semantic still under RE — log only for now. */
        fprintf(stderr, "[opt] extra (fade_color_index) = %d\n", s_opt_extra);
        break;
    case 0x17:
        /* Exit — return code 3 → caller (opszyns dispatcher) propagates.
 * Re-assert all toggles + persist to Wacki.sav. */
        AudioSetMusicEnabled(s_opt_music);
        AudioSetVoiceEnabled(s_opt_voice);
        g_subtitles_on = s_opt_subtitles;
        g_dialogues_on = s_opt_dialogues;
        persist_audio_opts();
        return 3;
    default: toggled = 0; break;       /* idle per-frame call (trigger=-1) */
    }
    /* Refresh the SceneDef button frames so on/off state shows visually.
 * Atlas layout (24 frames): 0..5 = OFF hover, 6..11 = OFF def,
 * 12..17 = ON hover, 18..23 = ON def. Buttons 0..4 (5 toggles) get
 * the ON/OFF treatment; button 5 (exit at id 0x17) stays at def=11,
 * hover=5 — there's no toggled "exit-active" state.
 *
 * Block runs on EVERY call (including idle/default trigger=-1) so the
 * on/off state stays in sync with s_opt_* flags even when scene is
 * re-entered. Original reaches via default fall-through. */
    /* T103: button index mapping (post-fix Solund semantics):
 * buttons[5] = exit (0x17, no toggle visual) */
    g_solund_scene.buttons[0].hover_anim = (uint16_t)(s_opt_music      ? 12 : 0);
    g_solund_scene.buttons[0].def_anim   = (uint16_t)(s_opt_music      ? 18 : 6);
    g_solund_scene.buttons[1].hover_anim = (uint16_t)(s_opt_subtitles  ? 13 : 1);
    g_solund_scene.buttons[1].def_anim   = (uint16_t)(s_opt_subtitles  ? 19 : 7);
    g_solund_scene.buttons[2].hover_anim = (uint16_t)(s_opt_voice      ? 14 : 2);
    g_solund_scene.buttons[2].def_anim   = (uint16_t)(s_opt_voice      ? 20 : 8);
    g_solund_scene.buttons[3].hover_anim = (uint16_t)(s_opt_dialogues  ? 15 : 3);
    g_solund_scene.buttons[3].def_anim   = (uint16_t)(s_opt_dialogues  ? 21 : 9);
    g_solund_scene.buttons[4].hover_anim = (uint16_t)(s_opt_extra      ? 16 : 4);
    g_solund_scene.buttons[4].def_anim   = (uint16_t)(s_opt_extra      ? 22 : 10);
    if (toggled) {
        fprintf(stderr, "[opt] music=%d subs=%d voice=%d dialog=%d extra=%d\n",
                s_opt_music, s_opt_subtitles, s_opt_voice,
                s_opt_dialogues, s_opt_extra);
    }
    return 0;
}

/* Solund.pic scene def —
 * Button layout (per dispatch table & graphics):
 * id 0x12 (music) def=6 (on) / hover=0 (off label)
 * id 0x13 (samplerate) def=7 / hover=1
 * id 0x14 (stereo) def=8 / hover=2
 * id 0x15 (sfx) def=9 / hover=3
 * id 0x16 (sound) def=10 / hover=4
 * id 0x17 (exit) def=11 / hover=5
 * Initial def/hover values are recomputed by SolundClick on first
 * non-toggle entry to reflect the current flag state. */
SceneDef g_solund_scene = {
    .background_pic = "Solund.pic",
    .mask_file      = "Solund.wyc",
    .on_click       = SolundClick,
    .button_count   = 6,
    .flags          = SCENE_FLAG_REDRAW,
    .buttons = {
        { 0x12, 6, 0 }, { 0x13, 7, 1 }, { 0x14, 8, 2 },
        { 0x15, 9, 3 }, { 0x16, 10, 4 }, { 0x17, 11, 5 },
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

static void paint_slot_list(void)
{
    extern AnimAsset *g_menu_asset_10;
    extern FontHandle *g_default_font;
    extern uint8_t *g_back_shadow;
    AnimAsset *atlas = g_menu_asset_10;
    if (!atlas || !g_default_font || !g_back_shadow) return;
    if (!atlas->off_drawX || !atlas->off_drawY) return;

    /* Thumbnail preview pane — init
 * branch: blits 126×78 indexed pixels at (off_drawX[1]+3,
 * off_drawY[1]+3). Frame 1 of Load.wyc atlas defines the preview
 * box position. Source: ALWAYS the selected slot's
 * world_default_snapshot — for filled slots that's the captured
 * scene at save time, for empty slots that's the TV-test-pattern
 * default copied from g_default_world_state at LoadSaveStateOrInit.
 * No selection → fall back to slot 0's content (matches original
 * init branch which always paints slot 0). */
    if (atlas->frame_count > 1) {
        int16_t tx = (int16_t)(atlas->off_drawX[1] + 3);
        int16_t ty = (int16_t)(atlas->off_drawY[1] + 3);
        int show_slot = (s_slot_selected >= 0 &&
                         s_slot_selected < WACKI_SAVE_SLOTS)
                      ? s_slot_selected : 0;
        const WackiSlot *s = &g_save.slots[show_slot];
        const uint8_t *thumb_src = s->world_default_snapshot;
        if (tx >= 0 && ty >= 0 &&
            tx + SAVE_THUMB_W <= WACKI_SCREEN_W &&
            ty + SAVE_THUMB_H <= WACKI_SCREEN_H) {
            PaintImageToBackbuffer((uint16_t)tx, (uint16_t)ty,
                                   SAVE_THUMB_W, SAVE_THUMB_H, thumb_src);
        }
    }


    for (int i = 0; i < WACKI_SAVE_SLOTS; ++i) {
        uint16_t f = slot_hover_frame(i);
        if (f >= atlas->frame_count) continue;
        /* call @ 0x0040AA75: original fills a
 * `(w-8) × (h-8)` rectangle at `(drawX[f]+4, drawY[f]+4)` with
 * bg color 0x01, then renders text 0x12 into it. The 4-pixel
 * inset matches the slot row's frame border. */
        int16_t rx = (int16_t)(atlas->off_drawX[f] + 4);
        int16_t ry = (int16_t)(atlas->off_drawY[f] + 4);
        int rw = (int)atlas->off_widths[f]  - 8;
        int rh = (int)atlas->off_heights[f] - 8;
        if (rx < 0 || ry < 0 || rx >= WACKI_SCREEN_W || ry >= WACKI_SCREEN_H) continue;
        if (rw <= 0 || rh <= 0) continue;
        if (rx + rw > WACKI_SCREEN_W) rw = WACKI_SCREEN_W - rx;
        if (ry + rh > WACKI_SCREEN_H) rh = WACKI_SCREEN_H - ry;
        /* Fill bg rectangle: PUSH 0x01 = bg color
 * index. sejw.pic palette has 0x01 = visible (some grey/brown
 * highlight color). DON'T try other indices — palette is
 * scene-specific and merged unpredictably across transitions. */
        for (int yy = 0; yy < rh; ++yy) {
            uint8_t *row = g_back_shadow + (size_t)(ry + yy) * WACKI_SCREEN_W + rx;
            memset(row, 0x01, (size_t)rw);
        }
        char line[40];
        uint8_t color_base = 0x12;      /* PUSH 0x12. */
        if (i == s_edit_slot) {
            /* Editing this slot — render the buffer + blinking cursor
 * in YELLOW. Original used 0xFE / 0xFD but
 * both come out near-black under the merged Sejw.pic
 * palette in our port. Force a known-bright yellow into
 * a high palette slot (0xFD) right before rendering — the
 * next scene's InstallPalette refresh will overwrite it,
 * so this is non-persistent. NOTE (refer
 * inline-edit branch). */
            extern uint8_t g_palette_rgb[256*3];
            g_palette_rgb[0xFD * 3 + 0] = 0xFF;  /* R */
            g_palette_rgb[0xFD * 3 + 1] = 0xE0;  /* G */
            g_palette_rgb[0xFD * 3 + 2] = 0x00;  /* B */
            int cursor_on = ((SDL_GetTicks() / 500u) & 1u) == 0;
            snprintf(line, sizeof line, "%.*s%c",
                     s_edit_len, s_edit_buf,
                     cursor_on ? '_' : ' ');
            color_base = 0xFD;
        } else {
            slot_display_name(i, line, sizeof line);
        }

        /* NOTE — small extra left text inset (+4 px past the bg
 * rect edge). The original starts text flush at rx; in our port
 * the font advance is slightly different so a flush start visually
 * crowds the slot border. Keeps the bg rect at the original +4
 * position; only the text origin shifts. */
        TextRenderTarget t = {
            .stride     = WACKI_SCREEN_W,
            .x          = (uint16_t)(rx + 4),
            .color_base = color_base,
            .pixels     = g_back_shadow + (size_t)ry * WACKI_SCREEN_W,
            .font       = g_default_font,
        };
        RenderTextLineToBuffer(&t, (const uint8_t *)line);
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

static int SaveSlotClick(int trigger)
{
    /* Slot row click — single click selects AND enters edit mode straight
 * away (deviates from the original's "click to select, click again to
 * edit" — user prefers immediate typing). Clicking a different slot
 * mid-edit commits the previous edit first. */
    if (trigger >= 0x14 && trigger <= 0x14 + WACKI_SAVE_SLOTS - 1) {
        int slot = trigger - 0x14;
        if (s_edit_slot >= 0 && s_edit_slot != slot) {
            end_edit_commit();
        }
        s_slot_selected = slot;
        if (s_edit_slot != slot) {
            begin_edit(slot);
        }
        fprintf(stderr, "[save-menu] slot %d editing\n", slot);
    } else if (trigger == 0x12) {
        /* Anuluj — drop any in-progress edit without committing. */
        PlatformSetTextInput(0);
        s_edit_slot = -1;
        s_edit_len = 0;
        s_slot_selected = -1;
        return 4;
    } else if (trigger == 0x13) {
        /* Zapisz button — same path as Enter-in-edit. */
        return save_commit_selected();
    } else {
        /* Idle frame: consume typed chars if we're in edit mode, then
 * keep the slot list painted on the BG. Enter commits the
 * save AND closes the menu (= Zapisz button). */
        if (s_edit_slot >= 0) {
            uint8_t c;
            int commit_now = 0;
            while ((c = PlatformPollTypedChar()) != 0) {
                if (c == 0x0D) {              /* Enter — save + close */
                    commit_now = 1;
                    break;
                } else if (c == 0x08) {       /* Backspace */
                    if (s_edit_len > 0) {
                        s_edit_buf[--s_edit_len] = 0;
                    }
                } else if (c >= 0x20 && c < 0x7F) {
                    /* 20-char cap — slot row pixel width can't fit much
 * more without overflowing into the thumbnail/buttons. */
                    enum { EDIT_MAX_CHARS = 20 };
                    if (s_edit_len < EDIT_MAX_CHARS) {
                        s_edit_buf[s_edit_len++] = (char)c;
                        s_edit_buf[s_edit_len] = 0;
                    }
                }
            }
            if (commit_now) return save_commit_selected();
        }
        paint_slot_list();
    }
    return 0;
}

/* LoadSlotClick — (Load.pic handler).
 * Loads selected slot via LoadSaveSlot, then triggers LoadKomnataScene
 * for the loaded komnata so the caller (opszyns dispatcher) returns to
 * the running game in the right room. */
extern void LoadKomnataScene(uint16_t id);    /* in this file */
static int LoadSlotClick(int trigger)
{
    if (trigger >= 0x14 && trigger <= 0x14 + WACKI_SAVE_SLOTS - 1) {
        s_slot_selected = trigger - 0x14;
        fprintf(stderr, "[load-menu] slot %d selected\n", s_slot_selected);
    } else if (trigger == 0x12) {
        s_slot_selected = -1;
        return 4;
    } else if (trigger == 0x13) {
        /* Wczytaj — read slot into globals + re-enter the loaded komnata.
 * LoadSaveSlot returns 0 for empty/invalid slots. */
        if (s_slot_selected < 0) {
            fprintf(stderr, "[load-menu] commit ignored (no slot selected)\n");
            return 0;
        }
        if (g_save.slots[s_slot_selected].stage_indicator == 0) {
            fprintf(stderr, "[load-menu] slot %d is empty\n", s_slot_selected);
            return 0;
        }
        if (LoadSaveSlot((uint16_t)s_slot_selected)) {
            LoadKomnataScene(g_cur_komnata);
            fprintf(stderr, "[load-menu] loaded slot %d → etap %u k%u\n",
                    s_slot_selected,
                    (unsigned)g_cur_etap, (unsigned)g_cur_komnata);
            s_slot_selected = -1;
            return 3;       /* exit menu, resume game in loaded state */
        }
        return 0;
    } else {
        paint_slot_list();
    }
    return 0;
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

/* OpszynsClick — (dispatcher).
 * Maps clicked button id (trigger) to its sub-menu. Button id mapping
 * verified from disassembly @ 0x0040b04a..0x0040b09f:
 *
 * trigger sub-menu SceneDef VA handler VA purpose
 * ------------------------------------------------------------
 * 0x12 0x00445C38 0x0040adc0 Grafika.pic
 * 0x13 0x00445C60 0x0040ae90 Solund.pic
 * 0x14 0x00445BD8 0x0040a960 Sejw.pic (SAVE slots)
 * 0x15 0x00445B78 0x0040a6b0 Load.pic (LOAD slots)
 * 0x16 0x00445B58 0x0040a690 Pytanie.pic — if returns
 * 3 → set g_game_over=2
 * 0x17 (no sub-menu) exit opszyns → return 3
 *
 * Polish naming note: "Sejw" (the SceneDef file name) = save (Polish gaming
 * slang for "save game"). Earlier port comment mis-annotated 0x14 as
 * "load slots" and 0x15 as "save slots" — actually reversed: Sejw.pic is
 * the SAVE picker, Load.pic is the LOAD picker.
 */
static int OpszynsClick(int trigger)
{
    int rc;
    switch (trigger) {
    case 0x12:
        RunMenuScene(1, &g_grafika_scene);
        return 0;
    case 0x13:
        RunMenuScene(1, &g_solund_scene);
        return 0;
    case 0x14:
        /* If save committed (returns 3), propagate exit code so opszyns
 * also closes and the player is back in-game. Returning 0 here
 * left the options menu hanging open after a successful save. */
        rc = RunMenuScene(1, &g_save_menu_scene);    /* Sejw.pic = SAVE */
        return rc == 3 ? 3 : 0;
    case 0x15:
        /* Same propagation for load — commit must drop the player into
 * the loaded scene, not into opszyns. */
        rc = RunMenuScene(1, &g_load_menu_scene);    /* Load.pic = LOAD */
        return rc == 3 ? 3 : 0;
    case 0x16:
        rc = RunMenuScene(1, &g_pytanie_scene);
        /* @ 0x0040b0a4-b8: if Pytanie returned 3
 * (= "Tak"), set game_over_code = 2 + propagate exit. */
        if (rc == 3) {
            g_game_over_code = 2;
            fprintf(stderr, "[opt] Pytanie: quit confirmed → game_over=2\n");
            return 3;
        }
        return 0;
    case 0x17:
        return 3;       /* exit back to game */
    default:
        return 0;
    }
}

/* opszyns.pic scene def —
 * 6 buttons with no def_anim (0xFFFF), each shows a hover sprite when
 * the mouse is over the button's hot rect. */
static SceneDef g_opszyns_scene = {
    .background_pic = "opszyns.pic",
    .mask_file      = "opszyns.wyc",
    .on_click       = OpszynsClick,
    .button_count   = 6,
    .flags          = SCENE_FLAG_REDRAW | SCENE_FLAG_FADE,
    .buttons = {
        { 0x12, 0xFFFF, 0 }, { 0x13, 0xFFFF, 1 }, { 0x14, 0xFFFF, 2 },
        { 0x15, 0xFFFF, 3 }, { 0x16, 0xFFFF, 4 }, { 0x17, 0xFFFF, 5 },
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

static int SelTloClick(int trigger)
{
    /* trigger==0 is the per-frame CB dispatch enabled by SCENE_FLAG_FORCE_CB.
 * Original (HandleAltMenuClick @ 0x0040B3D0): on this path it loads
 * Tlo.pal, sets up the ACME-assembly per-tick animation counters
 * (, ), and per-frame blits successive
 * sel_guz frames (starting at frame 10) to compose ACME. PORT
 * SHORTCUT (refer ): animation itself not yet ported —
 * the assembled-ACME state is conveyed visually by buttons 0..3
 * going invisible (def/hover=0xFFFF) once their bit in
 * g_completed_stages flips. The 5th button (id=0x16, def=4, hover=9)
 * remains as the explicit "click to start finale" cue. */
    if (trigger == 0) return 0;

    /* Buttons 0x12..0x15 = stages 1..4. */
    if (trigger >= 0x12 && trigger <= 0x15) {
        int idx = trigger - 0x12;
        /* Only allow clicking a stage that's NOT yet completed (bit i
 * of g_completed_stages clear). Original makes them neutral
 * (id=0x26) so they don't dispatch, but we double-check here
 * for safety in case the menu is entered with a stale state. */
        if ((g_completed_stages & (1u << idx)) == 0) {
            s_chapter_pick = idx + 1;       /* 1..4 */
            fprintf(stderr, "[chapter-select] picked stage %d\n", s_chapter_pick);
            return 3;       /* exit menu */
        }
        fprintf(stderr, "[chapter-select] stage %d already completed — ignore\n",
                idx + 1);
        return 0;
    }
    /* Button 0x16 — "ACME complete" green button at the bottom of the
 * map. Hit-test only fires when SelTloRefreshButtons has promoted
 * button_count back to 5 (i.e. all 4 stages bits set).
 * HandleAltMenuClick @ 0x0040B47C: = 5, return 8. The
 * caller (RunGameStageLoop / dev loop) then runs LoadStage(5) which
 * launches the "Monter" finale (intro=Dane_12.dta, alt=Dane_11.dta,
 * alt3=Dane_13.dta — end credits sting). */
    if (trigger == 0x16) {
        s_chapter_pick = 5;
        fprintf(stderr, "[chapter-select] ACME complete — start finale (stage 5)\n");
        return 3;
    }
    return 0;
}

/* Button slots 0..3 get patched
 * per-call based on `g_completed_stages`; button 4 (id=0x16 "ACME-complete"
 * green finale button) has static frames straight from the binary
 * (+0x2C: 16 00 04 00 09 00). Static button_count = 5 per binary +0x0C;
 * SelTloRefreshButtons downgrades to 4 when not all stages done — 
 * with RunGameStageLoop @ 0x0040C3FA write to g_default_world_state. */
SceneDef g_sel_tlo_scene = {
    .background_pic = "sel_tlo.pic",
    /* @ 0x00445D3C: mask = "sel_guz.wyc" (NOT sel_tlo.wyc —
 * that filename doesn't exist in the .DTA archive; earlier port had
 * a typo). Without the correct mask, buttons have no hit-test
 * rectangles → clicks ignored → map appears non-functional. */
    .mask_file      = "sel_guz.wyc",
    .on_click       = SelTloClick,
    .button_count   = 5,
    /* 0x34 = SCENE_FLAG_FORCE_CB | SCENE_FLAG_DISABLE_ESC | SCENE_FLAG_KEEP_IMAGE.
 * +0x10. Previous flags=0 suppressed the per-tick CB
 * dispatch needed for the original's ACME-assembly animation
 * (HandleAltMenuClick trigger==0 path — animation itself still
 * NOTE, see SelTloClick). */
    .flags          = 0x34,
    .buttons = {
        { 0x12, 0,    5 },   /* stage 1 — patched per game_over fire */
        { 0x13, 1,    6 },   /* stage 2 */
        { 0x14, 2,    7 },   /* stage 3 */
        { 0x15, 3,    8 },   /* stage 4 */
        { 0x16, 4,    9 },   /* ACME-complete green button — static frames */
    },
};

/* Patch SceneDef buttons to reflect current completion state. Called
 * right before RunMenuScene(sel_tlo) so the user sees correct lock/
 * unlock states. Mirrors RunGameStageLoop @ 0x0040C2C8-2EE button
 * rebuild loop. */
int SelTloRefreshButtons(void)
{
    int all_done = 1;
    for (int i = 0; i < 4; ++i) {
        if (g_completed_stages & (1u << i)) {
            /* Completed — neutral / non-clickable. */
            g_sel_tlo_scene.buttons[i].id         = 0x26;
            g_sel_tlo_scene.buttons[i].def_anim   = 0xFFFF;
            g_sel_tlo_scene.buttons[i].hover_anim = 0xFFFF;
        } else {
            /* Available — normal id + frames. */
            g_sel_tlo_scene.buttons[i].id         = (uint16_t)(0x12 + i);
            g_sel_tlo_scene.buttons[i].def_anim   = (uint16_t)i;
            g_sel_tlo_scene.buttons[i].hover_anim = (uint16_t)(i + 5);
            all_done = 0;
        }
    }
    /* button[4] (ACME-complete green button, id=0x16) is exposed only
 * when all 4 stages finished. @ 0x0040C3FA:
 * if (bVar22) g_default_world_state = 5; // all done → 5 buttons live
 * else g_default_world_state = 4; // some pending → 4 buttons
 * `button_count` IS g_default_world_state (SceneDef +0x0C). With count=4 the
 * 5th slot stays in memory but never gets hit-tested, painted, or
 * dispatched — so the green graphic disappears until ACME is full. */
    g_sel_tlo_scene.button_count = all_done ? 5 : 4;
    return all_done;
}

/* is_walkable_at moved to src/scene/walkability.c. */

/* HandleSceneInput — RMB toggle + hotspot scan + LMB click dispatch.
 *
 *'s main-loop click block (lines ~1794-2023
 * pre-T2 phase B), extracted to file-scope so ProcessGameFrameTick
 * Inner can drive scene input without scene-locals threading.
 *
 * Reads globals: g_current_scene, walk-fld state, s_mouse_x/y,
 * g_lmb_clicked, g_rmb_clicked, g_hover_panel_verb, g_held_item,
 * g_actor[], g_active_actor, g_panel_verb_tab.
 *
 * Writes globals: g_held_item, g_active_actor, g_lmb_clicked (consumed),
 * g_rmb_clicked (consumed), g_lmb_handled (set/cleared around
 * DispatchClickEvent — matches RunGameStageLoop @ 0x0040C0BF/C0CB). */
void HandleSceneInput(void)
{
    extern int16_t s_mouse_x, s_mouse_y;
    extern uint8_t g_rmb_clicked;
    extern uint16_t g_hover_panel_verb;
    extern uint16_t g_panel_verb_tab[6];
    static int s_reentry_depth = 0;
    /* Note: NO rising-edge debounce on g_lmb_clicked / g_rmb_clicked.
 * SDL_MOUSEBUTTONDOWN fires per-press (not per-hold) — the click
 * flag IS the press event. Original engine works the same way
 * (Win32 WM_LBUTTONDOWN). A previous port had `s_last_lmb`
 * static which could DEADLOCK: if walk-loop consumed g_lmb_clicked
 * mid-walk and user clicked again, the new click would set
 * g_lmb_clicked=1 but outer HandleSceneInput end sets
 * s_last_lmb=1, locking future clicks (rising edge never fires
 * since user keeps clicking → g_lmb_clicked stays 1 →
 * s_last_lmb stays 1). Removed. */

    if (!g_current_scene) return;

    /* Re-entry guard — DispatchClickEvent can fire scripts that hit
 * blocking waits (op 0x09 SHOW_TEXT, op 0x52 DIALOG_BEGIN). Those
 * waits pump ProcessGameFrameTickInner which calls back into us.
 * Without the guard, the same click re-fires repeatedly: dialog
 * starts → wait pumps PGFT → HandleSceneInput sees same g_lmb_clicked
 * → re-dispatches → infinite recursion. Original RunGameStageLoop's
 * `g_lmb_handled = 0` around DispatchClickEvent serves the same
 * purpose, but here we use a depth counter to be safe with the
 * full set of script-blocking paths. */
    if (s_reentry_depth > 0) return;
    ++s_reentry_depth;

    /* RMB → toggle active actor. SDL_MOUSEBUTTONDOWN fires once per
 * press; consume the flag to avoid double-toggle within the same
 * press event. No rising-edge debounce — see s_reentry_depth
 * comment above. */
    if (g_rmb_clicked) {
        g_active_actor ^= 1;
        fprintf(stderr, "[scene] RMB → active actor = %s\n",
                g_active_actor ? "Fjej" : "Ebek");
        g_rmb_clicked = 0;
    }

    /* Panel hit-test + mask hit-test — already done by PGFT Inner BEFORE
 * us in the new flow. But Inner runs AFTER this in play_demo_scene's
 * current order, and other callers may not have run them yet. Call
 * them defensively — idempotent. */
    PanelHitTest();
    extern int ClickHitTest(int16_t mx, int16_t my, uint16_t *out_verb);
    extern uint16_t g_hover_scene_verb;            /* T31 v2 — g_hover_scene_verb */
    uint16_t hover_verb = 0x26;
    int have_hover = ClickHitTest((int16_t)s_mouse_x, (int16_t)s_mouse_y,
                                  &hover_verb);
    g_hover_scene_verb = hover_verb;

    int a = g_active_actor & 1;

    if (g_lmb_clicked) {
        /* T-input-order: CONSUME the click immediately, before any dispatch
 * or walker bind. Without this, blocking-wait pumps inside Dispatch
 * ClickEvent (op 0x09 SHOW_TEXT, op 0x10..0x12 walker waits, op 0x52
 * dialog) call ProcessGameFrameTick → PGFT Inner snapshots g_lmb_
 * handled = g_lmb_clicked (still 1!) → UpdateActorMovement auto-
 * binds walker to current mouse pos, clobbering the verb-script's
 * own walker. @ 0x40C037 where
 * g_panel_cursor_redirect2 (= g_lmb_clicked) is cleared at the end of the
 * click-handling block (line 0x40C0E1, written here as the same
 * pre-dispatch invariant). */
        g_lmb_clicked = 0;
        g_lmb_handled = 1;
        if (s_mouse_y >= 400) {
            /* Panel click — pick up verb from hover panel slot.
 *
 * Item-combine path: if a held_item is already set AND user
 * clicks a different inventory slot (hover_panel_verb != 0x26),
 * route this as use-on-item (verb 0x0F) instead of overwriting
 * the held verb. @ 0x0040C081-C0A0:
 *
 * Original wires this from the same dispatch path as world-on-
 * item — both routes set var[0x0F] = target verb and fire
 * verb-0x0F script with this=held_item. */
            if (g_hover_panel_verb != 0x26) {
                /* T20b debug — log panel click w/dialog-mode annotation
 * so we can see if dialog choice clicks are routing
 * correctly. */
                extern uint8_t g_dialog_active;
                if (g_dialog_active) {
                    fprintf(stderr, "[dlg-click] panel verb=0x%04X "
                                    "(held=0x%04X) dialog-active\n",
                            g_hover_panel_verb, g_held_item);
                }
                if (g_held_item != 0x26 && g_held_item != g_hover_panel_verb) {
                    /* item-on-item combine */
                    uint16_t this_arg   = g_held_item;
                    uint16_t target_verb = g_hover_panel_verb;
                    g_held_item = 0x26;
                    extern uint32_t g_script_vars[0x129];
                    g_script_vars[0x0F] = target_verb;
                    fprintf(stderr, "[panel] use-on-item: held=0x%04X target=0x%04X "
                            "(var[0x0F]=0x%04X)\n",
                            this_arg, target_verb, target_verb);
                    g_lmb_handled = 0;
                    DispatchClickEvent(this_arg, 0x0F);
                    g_lmb_handled = 0;
                } else {
                    /* Plain pick-up. */
                    g_held_item = g_hover_panel_verb;
                    fprintf(stderr, "[panel] picked up verb=0x%04X (held_item set)%s\n",
                            g_held_item,
                            g_dialog_active ? " [dlg-active: should this dispatch instead?]" : "");
                }
            } else if (s_mouse_x >= 230 && s_mouse_x < 280 &&
                       s_mouse_y >= 430 && s_mouse_y < 455) {
                /* OPCJE button — guziki#1.wyc frame 0 at (230,430) size
 * 50x25 (parsed from atlas header). The earlier port used
 * the much-larger panel.wyc frame 1 OPCJE label overlay
 * as a hit region — that covered actor portraits + half
 * the inventory area. Original wiring missing from
 * RunGameStageLoop @ 0x0040C0CC (g_script_vars never set
 * in code), so we trigger the menu directly here. */
                fprintf(stderr, "[opt] OPCJE clicked at (%d,%d) → opszyns\n",
                        s_mouse_x, s_mouse_y);
                OpenOptionsMenu();
            } else if (s_mouse_x >= 600 && s_mouse_x < 630 &&
                       s_mouse_y >= 412 && s_mouse_y < 442) {
                /* ▲ page-prev arrow — guziki#1.wyc frame 2 at (600,412)
 * size 30x30. Original spawns this as an entity in the
 * click on it would run an entity verb-script that calls
 * op 0x1D (InventoryPagePrev + PanelPageSwap). The port's
 * entity click-list doesn't currently catch panel-area
 * entities, so we wire the hit-region directly. */
                if (InventoryPagePrev()) {
                    PanelPageSwap();
                    fprintf(stderr, "[panel] page-prev → page=%u\n",
                            g_panel_page_idx);
                }
            } else if (s_mouse_x >= 599 && s_mouse_x < 629 &&
                       s_mouse_y >= 443 && s_mouse_y < 473) {
                /* ▼ page-next arrow — guziki#1.wyc frame 4 at (599,443)
 * size 30x30. Same wiring story as page-prev. */
                if (InventoryPageNext()) {
                    PanelPageSwap();
                    fprintf(stderr, "[panel] page-next → page=%u\n",
                            g_panel_page_idx);
                }
            } else if (have_hover && hover_verb != 0x26) {
                /* HUD entity click — masks registered on the panel (e.g.
 * `fjbezoku.msk` glasses on Fjej portrait → verb 0x67)
 * fall here. Dispatch through the regular verb-script
 * route so the click triggers the entity's handler (which
 * typically hides the asset + adds item to inventory). */
                uint16_t this_arg = g_held_item;
                uint16_t that_arg = hover_verb;
                g_held_item = 0x26;
                fprintf(stderr, "[panel] HUD verb=0x%04X at (%d,%d) — dispatch\n",
                        hover_verb, s_mouse_x, s_mouse_y);
                g_lmb_handled = 0;
                DispatchClickEvent(this_arg, that_arg);
                g_lmb_handled = 0;
            } else {
                fprintf(stderr, "[scene] panel click at (%d,%d) — empty slot\n",
                        s_mouse_x, s_mouse_y);
            }
        } else if (have_hover) {
            fprintf(stderr, "[click] verb=0x%04X at (%d,%d) — dispatch (%s)\n",
                    hover_verb, s_mouse_x, s_mouse_y, a ? "Fjej" : "Ebek");
            uint16_t this_arg = g_held_item;
            uint16_t that_arg = hover_verb;
            int held_active = (g_held_item != 0x26);
            g_held_item = 0x26;
            if (held_active && g_hover_panel_verb != 0x26) {
                that_arg = 0x0F;
                /* T8: write side-channel = hover_panel_verb
 * (= script var[0x0F] since g_script_vars + 0xF*4 = 0x498BC).
 * The use-on-item verb-0x0F script reads var[0x0F] to know
 * the original target. @ 0x40C08F:
 * g_pickup_target_verb = (uint)g_hover_panel_verb; */
                extern uint32_t g_script_vars[0x129];
                g_script_vars[0x0F] = g_hover_panel_verb;
                fprintf(stderr, "[click] use-on-item: held=0x%04X target=0x%04X "
                        "(var[0x0F]=0x%04X)\n",
                        this_arg, g_hover_panel_verb, g_hover_panel_verb);
            }
            if      (that_arg == 1) {
                if (g_active_actor != 0)
                    fprintf(stderr, "[active] dispatch → Ebek (was %s)\n",
                            g_active_actor ? "Fjej" : "Ebek");
                g_active_actor = 0;
            }
            else if (that_arg == 2) {
                if (g_active_actor != 1)
                    fprintf(stderr, "[active] dispatch → Fjej (was %s)\n",
                            g_active_actor ? "Fjej" : "Ebek");
                g_active_actor = 1;
            }
            if (that_arg == 0x26 && this_arg == 0x26) {
                /* both neutral — short-circuit */
            } else {
                g_lmb_handled = 0;
                DispatchClickEvent(this_arg, that_arg);
                g_lmb_handled = 0;
            }
            /* NO auto-walk-to-mouse here. @
 * 0x0040C037..C0CB: when a click resolves to a verb, the
 * original CLEARS g_lmb_handled around DispatchClickEvent and
 * never falls through to UpdateActorMovement's walker-bind
 * path. The verb script itself handles walking (op 0x10/0x11/
 * 0x12 actor walk-to + blocking wait) if the interaction
 * needs the actor to approach the target.
 *
 * Previous port-only BindActorWalker(a, mouse_x, mouse_y)
 * here CLOBBERED whatever path the verb script just set —
 * actor either froze in place, slid to mouse pos leaving a
 * trail of frames, or ended up off-screen depending on what
 * the verb script tried to do. Removed. */
        } else {
            /* Free-walk — clamp to .fld walkable mask. If click is on a
 * non-walkable cell AND nearest-walkable search fails within
 * 200 px, IGNORE the click (port shortcut: original engine
 * doesn't enforce this, but walking actor into a wall or
 * off-screen is worse UX than nothing). */
            int tx = s_mouse_x, ty = s_mouse_y;
            int found_walkable = is_walkable_at(tx, ty);
            if (!found_walkable) {
                int best_d = 1 << 30, btx = tx, bty = ty;
                for (int r = 1; r <= 200 && best_d == (1 << 30); r += 2)
                    for (int dy = -r; dy <= r; ++dy)
                        for (int dx = -r; dx <= r; ++dx) {
                            if (dx*dx + dy*dy > r*r) continue;
                            int cx = tx + dx, cy = ty + dy;
                            if (is_walkable_at(cx, cy)) {
                                int d = dx*dx + dy*dy;
                                if (d < best_d) { best_d = d; btx = cx; bty = cy; }
                            }
                        }
                if (best_d != (1 << 30)) {
                    tx = btx; ty = bty;
                    found_walkable = 1;
                }
            }
            if (!found_walkable) {
                fprintf(stderr, "[scene] click (%d,%d) unreachable — ignoring\n",
                        s_mouse_x, s_mouse_y);
            } else if (g_actor[a]) {
                fprintf(stderr, "[scene] %s walk → (%d,%d)\n",
                        a ? "Fjej" : "Ebek", tx, ty);
                BindActorWalker(a, tx, ty);
            }
        }
        /* Note: g_lmb_clicked was already cleared at top of this block
 * to prevent blocking-wait pumps inside DispatchClickEvent from
 * re-snapshotting it into g_lmb_handled. */
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

/* Heavy lifting for one scene. */
static const char *play_demo_scene(const DemoScene *scene)
{
    /* Stage palette first so all paletted blits look right. */
    void *pal = NULL; uint32_t psz = 0;
    if (LoadFileFromDta("paleta.pal", &pal, &psz) && pal) {
        InstallPalette((uint8_t *)pal, 0);
        xfree(pal);
    }

    /* T22 phase B — bg/fld/music load delegated to LoadKomnataScene.
 * Called once here for the initial komnata; subsequent transitions
 * happen via op 0x20 → ScriptGoToKomnata → LoadKomnataScene without
 * unwinding this function. The play_first_scene_demo outer komnata
 * loop is now single-iteration; play_demo_scene runs until ESC/F12. */
    extern uint16_t g_cur_komnata;
    LoadKomnataScene(g_cur_komnata);

    /* Publish the panel to the global PanelHitTest reads.
 * Loaded once — same panel.wyc shared across all stage-1 komnaty.
 * (Stage 2-5 panel reload happens in LoadStage per T27.)
 *
 * Stage 5 (Monter finale): g_stage->panel_wyc == NULL → no HUD.
 * LoadStage already free'd + NULLed g_panel_asset; do NOT load a
 * default panel.wyc here, otherwise PaintHudOverlay paints the
 * panel during the ACME ending. */
    int stage_has_panel = (g_stage && g_stage->panel_wyc) || !g_stage;
    if (stage_has_panel && !g_panel_asset) {
        g_panel_asset = LoadAssetFromDtaBase("panel.wyc");
    }
    AnimAsset *panel = g_panel_asset;
    /* Mark panel visible — komnata flag bit 0 ( reads from
 * the komnata table entry; play_demo_scene is a port shortcut so we
 * raise it directly). For NULL-panel stages keep bit 0 cleared so
 * PaintHudOverlay's portrait + inventory paths stay gated off
 * regardless of any leftover g_items_atlas/g_panel_asset state. */
    if (stage_has_panel) g_settings_anim_active |=  1;
    else                 g_settings_anim_active &= ~1u;
    /* T4 step 1: actor atlases are singletons from PreloadCommonAssets.
 * Previously LoadAssetFromDtaBase per-scene leaked a fresh AnimAsset
 * each scene change while old actor[+0x28] intern slot still pointed
 * at the prior copy. Singleton globals eliminate that leak and pave
 * the way for actor entity persistence (T4 unblocker).
 *
 * Stage 5 (Monter): g_stage->ebek_wyc / fjej_wyc both NULL → the
 * actors are not part of this stage at all (
 * actor-list management gated on komnata flag 0x44e448 & 2).
 * Force the per-slot atlas to NULL so the spawn block below skips. */
    extern AnimAsset *g_ebek_atlas, *g_fjej_atlas;
    AnimAsset *atlases[2] = {
        (g_stage && !g_stage->ebek_wyc) ? NULL : g_ebek_atlas,
        (g_stage && !g_stage->fjej_wyc) ? NULL : g_fjej_atlas,
    };
    fprintf(stderr, "[scene] initial entry: panel=%d ebek=%d fjej=%d\n",
            panel       ? panel       ->frame_count : 0,
            atlases[0]  ? atlases[0]  ->frame_count : 0,
            atlases[1]  ? atlases[1]  ->frame_count : 0);

    /* Two actors ( @ 0x004061D0): both visible
 * in every scene, but only the active one responds to
 * clicks. SPACE toggles between them. */

    /* Spawn Ebek (id=1) + Fjej (id=2) as entity-backed actors so the
 * original scripts can position them via op 0x28 SET_ENTITY_XY
 * (which uses to look up the render entity by
 * verb_id in the click list). game's
 * pre-spawn of g_actor / actor entities. */
    extern Entity *SpawnActorEntity(uint16_t id, AnimAsset *atlas,
                                    uint16_t init_frame,
                                    int16_t init_x, int16_t init_y);
    /* T4 step 4: actor entities PERSIST across scene transitions
 * (preserved by EntityListClearAll). Spawn only on FIRST scene;
 * subsequent scenes re-use existing g_actor[i] entities. The
 * persistent actor's anchor/scale/walker state may be carried
 * across — entry-chain (s_entry_dir block) re-positions if
 * scene transition was a hotspot click.
 *
 * For NULL-atlas stages (Monter) atlases[i] is NULL, so spawn is
 * skipped and LoadStage's UnlinkEntity leaves the render list
 * actor-free for the finale. */
    if (!g_actor[0] && atlases[0])
        g_actor[0] = SpawnActorEntity(1, atlases[0], 11, 380, 375);
    if (!g_actor[1] && atlases[1])
        g_actor[1] = SpawnActorEntity(2, atlases[1], 11, 300, 380);

    /* Run the original actor entry script — 0x004251C8 LOADs the
 * floor cursor atlases (ids 0x64/0x65) then tail-calls 0x00423E28
 * which conditionally positions Ebek/Fjej via op 0x28 based on
 * var[6] bit 0 (= "in scene transition" flag, default 0).
 *
 * On a fresh scene entry (var[6] bit 0 clear, which is our case)
 * the ELSE branch fires:
 * op 0x28 id=1 (380, 375) ← Ebek
 * op 0x28 id=2 (300, 380) ← Fjej
 * Our op 0x28 implementation in script.c finds the entity via
 * FindUpdateRegistration(kind=2, id) which now resolves both
 * actors thanks to the SpawnActorEntity call above.
 *
 * Call args: this/that = 0x26 (neutral marker).
 * which uses 0x26 for all enter_script / room init calls. Earlier
 * port passed (0, 0) — for op 0x00 (skip-if-not-this), reg_id = 0
 * would compare equal to this_id = 0 → take body. With 0x26 we
 * match the original's neutral-marker convention. */
    {
        const uint8_t *entry_script = (const uint8_t *)xlat_binary_ptr(0x004251C8u);
        if (entry_script) {
            fprintf(stderr, "[actor] running entry chain @ 0x004251C8\n");
            RunScriptInterpreter(0x26, 0x26, (uint8_t *)entry_script);
        }
    }

    /* T22 phase B — s_entry_dir + k_entry_pos workaround retired.
 * With sync op 0x20 + LoadKomnataScene + persistent actors (T4),
 * the verb-script post-GO_EXIT SET_ENTITY_XY runs against the
 * actor entity directly (in the new komnata's coord system).
 * No need for the C-side teleport approximation. Actors keep
 * their pre-transition position (set by initial spawn or by
 * verb-script writes) — the walker freeze in LoadKomnataScene
 * stops any in-flight step from corrupting that position. */
    /* T130 — s_entry_dir global retired. */
    extern uint16_t g_active_actor;            /* script.c — g_active_actor */

    extern int16_t s_mouse_x, s_mouse_y;
    const int walk_x0 = scene->walk_x0, walk_x1 = scene->walk_x1;
    const int walk_y0 = scene->walk_y0, walk_y1 = scene->walk_y1;
    /* T2 phase B: publish walk bounds + scene to globals for HandleSceneInput. */
    g_walk_x0 = walk_x0; g_walk_x1 = walk_x1;
    g_walk_y0 = walk_y0; g_walk_y1 = walk_y1;
    g_current_scene = (const struct DemoScene *)scene;
    /* walk_first/walk_last (frame 5..15 = walk-step poses) removed in T3 —
 * per-entity VM walker handles frame cycling via op 0x06/0x12 in the
 * walker bytecode. Manual frame_tick logic gone. */

    /* T22 phase B — .fld walkability is loaded by LoadKomnataScene into
 * the g_walk_fld_* globals + g_scene_fld_asset (no per-scene local
 * variable anymore). HandleSceneInput / is_walkable_at read globals
 * directly. The IS_WALKABLE/FLD_AT macros below are no longer needed
 * since the actual is_walkable_at function in game.c top reads the
 * globals. */
    (void)walk_x0; (void)walk_x1; (void)walk_y0; (void)walk_y1;

    /* Quit-out signals — set from ESC keyboard, F12 Pytanie TAK,
 * or OpenOptionsMenu Pytanie cascade. Local `quit` polled by
 * the while-condition; g_scene_quit polled at top of each tick. */
    const char *next_scene = NULL;
    int quit = 0;
    g_scene_quit = 0;
    while (!quit) {
        if (PlatformShouldQuit()) break;
        if (g_scene_quit) { g_scene_quit = 0; quit = 1; break; }   /* opszyns TAK */
        /* Bug 2 fix #19 — exit the main loop when a script sets
 * g_game_over_code (= var[14] alias, see Fix #16): 1=death,
 * 3=chapter-select after stage end, 4=stage-end death AVI. 
 * with RunMainGameLoop @ 0x0040BDBD which reads g_game_over_code at
 * the top of each iter and bails to the post-loop handler. */
        if (g_game_over_code) { quit = 1; break; }
        /* T2 NOTE: PumpWin32Messages removed here — ProcessGameFrameTickInner
 * below does it. Avoids double event consumption that could drop
 * input events between the two pumps. */

        /* S3-phase-2: per-tick re-sync from local actor_fx/fy arrays
 * REMOVED — walker now reads from entity[+0x22/+0x24] directly,
 * and writes back to those same fields at end of step. Single
 * source of truth = entity. Click targets live in entity
 * [+0x54/+0x56]. */


        /* --- Per-actor scale_pct + scene-exit polling (T3 FULL) ---------- *
 * After T3 the per-entity VM walker (op 0x15/0x16 in
 * ) handles actor motion via entity[+0x44..+0x56]
 * fixed-point stepper. EntityWalkerTick (called below) ticks the
 * walker each frame. play_demo_scene no longer runs a parallel
 * manual walker.
 *
 * What we still do here:
 * 1. Compute perspective scale_pct from anchor Y and write to
 * entity[+0x58] each frame.
 * at the top of UpdateActorMovement ( lines 1-12
 * of decompile). Walker doesn't write +0x58 itself.
 * 2. Poll walker completion (entity[+0x4C/+0x50] == 0) for the
 * pending-scene-transition case — when actor walks to an
 * exit hotspot and arrives, trigger the scene change.
 *
 * What we no longer do:
 * - actor_walking[] gate, actor_frame[]/actor_frame_tick[]
 * cycling, actor_facing_right[] — all moved into walker
 * bytecode / BindActorWalker.
 * - Direct writes to entity[+0x22/+0x24/+0x0A/+0x0C/+0x30] —
 * all driven by the per-entity VM tick + post-exec block. */
        for (int i = 0; i < 2; ++i) {
            if (!g_actor[i]) continue;
            uint8_t *eb = (uint8_t *)g_actor[i];
            int16_t anchor_y = *(int16_t *)(eb + 0x24);
            int scale_pct = (int)g_cursor_speed
                          - ((400 - anchor_y) * (int)g_perspective_min)
                            / (int)g_perspective_step;
            if (scale_pct < 30)  scale_pct = 30;
            if (scale_pct > 160) scale_pct = 160;
            *(uint16_t *)(eb + 0x58) = (uint16_t)scale_pct;

            /* T35: walker-done scene-exit branch retired. Verb-driven
 * exits (op 0x20 GO_EXIT → LoadKomnataScene per T22 phase B
 * + skip_to_endif 0x55 bug fix) handle all transitions
 * directly. This branch is now just per-frame scale_pct
 * update (above) — no walker-completion side-effect. */
        }


        /* --- Composite scene (back→front, ordered by foot_y) ----------- *
 * T-trail: BG paint moved INTO ProcessGameFrameTickInner (top of
 * tick, head `RestorePrevFrameRects`). Before
 * this fix, BG was painted here in play_demo_scene's main loop
 * only — blocking-wait pumps inside scripts (op 0x09 SHOW_TEXT,
 * dialog runner, op 0x10..0x12 walker waits, op 0x26/0x3D frame
 * waits) called ProcessGameFrameTick (Inner+Flush) without any
 * BG repaint, so entities composited on top of the prior frame's
 * shadow buffer → sprite trails everywhere. PGFT Inner now reads
 * g_scene_bg_raw/g_scene_bg_size globals published at the top
 * of play_demo_scene and clears the BG every tick. */
        ProcessGameFrameTickInner();

        /* T2 NOTE: TickSpeechBalloon removed here — ProcessGameFrameTick
 * Inner already ticks the speech balloon dismiss counter. */
        FlushFrameToPrimary();
        TickMenuMusic();                       /* loop seam for per-room WAV */

        /* --- Keyboard: SPACE toggles active actor, ESC exits ----------- */
        if (HasPendingKey()) {
            uint16_t k = WaitForKey();
            if (k == VK_ESCAPE) {
                /* PORT EXTENSION (original gameplay loop doesn't bind ESC).
 * Treat ESC as an explicit quit-to-main-menu signal — same
 * semantics as F12+TAK and opszyns→Quit. Setting
 * game_over_code=2 lets the dev `--start-stage` loop
 * distinguish "user quit" from "stage ended normally"
 * and bail out instead of looping back to the map. */
                g_game_over_code = 2;
                quit = 1;
            }
            else if (k == VK_SPACE) {
                g_active_actor ^= 1;
                fprintf(stderr, "[scene] active actor → %s\n",
                        g_active_actor ? "Fjej" : "Ebek");
            }
        }
        /* T53 — F5 quicksave / F9 quickload latches (set by platform_sdl
 * key handler, consumed + cleared here). */
        if (g_quicksave_request) {
            g_quicksave_request = 0;
            /* F5 path — stamp a "Quick" marker so the user can tell it
 * apart from named saves in the Load menu. The Save-menu
 * path sets its own name; QuickSaveToSlot no longer renames. */
            WackiSlot *qs = &g_save.slots[0];
            memset(qs->name, 0, sizeof qs->name);
            snprintf(qs->name, sizeof qs->name, "Quick");
            QuickSaveToSlot(0);
        }
        if (g_quickload_request) {
            g_quickload_request = 0;
            if (QuickLoadFromSlot(0)) {
                /* T22 phase B — trigger in-place scene rebuild for the
 * loaded komnata. LoadKomnataScene preserves persistent
 * actors (T4), frees old BG/FLD, runs new enter_script. */
                LoadKomnataScene(g_cur_komnata);
            }
        }
        /* T56 — F3 stats dump (logs to stderr). */
        if (g_stats_dump_request) {
            g_stats_dump_request = 0;
            StatsDump();
        }
        /* T24 — F12 opens Pytanie quit-confirmation menu (
 * RunGameStageLoop @ 0x40BFxx F12 branch). RunMenuScene returns
 * the trigger from PytanieClick: 3 = TAK (exit), 4 = NIE
 * (continue). On TAK we set quit so the scene unwinds back to
 * the main menu via play_first_scene_demo's outer loop. */
        if (g_pause_menu_request) {
            g_pause_menu_request = 0;
            extern int RunMenuScene(int, SceneDef *);
            extern SceneDef g_pytanie_scene;
            int rc = RunMenuScene(1, &g_pytanie_scene);
            fprintf(stderr, "[scene] F12 Pytanie rc=%d\n", rc);
            if (rc == 3) {                          /* TAK — quit to menu */
                /*→Quit Pytanie cascade @ game.c:2906
 * (case 0x16: g_game_over_code = 2). Both paths are
 * semantically "user-confirmed quit to main menu", so
 * they raise the same code. Needed for the dev
 * `--start-stage` loop to bail back to the OS instead
 * of returning to the chapter-select map. */
                g_game_over_code = 2;
                quit = 1;
                next_scene = NULL;
            }
            /* rc == 4 (NIE) → fall through, keep playing. */
        }
        /* T138 — honor `--no-pacing` flag in main gameplay loop too.
 * Earlier the flag only affected FLIC playback; CI runs still
 * waited 33ms/frame in gameplay. Now `./wacki --headless
 * --no-pacing` exercises the engine as fast as possible. */
        extern int g_no_pacing;
        if (!g_no_pacing) SDL_Delay(33);
    }

    /* T4 step 1: actor atlases are singletons — DO NOT free here (they
 * live for the whole game in g_ebek_atlas / g_fjej_atlas).
 * T22 phase B: panel/fld/bg_raw moved to globals, freed below. */
    (void)atlases;
    (void)panel;          /* g_panel_asset singleton, freed in LoadStage on stage change */
    if (g_scene_fld_asset) { FreeAsset(g_scene_fld_asset); g_scene_fld_asset = NULL; }
    if (g_scene_bg_raw)    { xfree(g_scene_bg_raw); g_scene_bg_raw = NULL; }
    g_scene_bg_size = 0;
    StopMenuMusic();
    /* T2 phase B: clear scene/walkability globals so HandleSceneInput
 * called from blocking-wait pumps (menu, intro AVI) doesn't try
 * to read stale fld pointers. */
    g_current_scene = NULL;
    g_walk_fld_pixels = NULL;
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

void RunGameStageLoop(uint8_t flags)
{
    g_game_over_code = 0;

    /* FULL RESET branch — (flags & 2) block @
 * 0x0040BEFA..0x0040BF35. The original DOES NOT memset all of
 * g_entity_state — it only zeroes the in_inventory_flag (+2) of each
 * entry, preserving the panel_verb_id (+0) identity mapping seeded by
 * PreloadCommonAssets. Earlier port wiped the whole block, which made
 * InventoryAddItem read panel_verb_id=0 and ultimately PaintHudOverlay
 * skip every slot. */
    if (flags & 0x02) {
        memset(g_script_vars,  0, sizeof g_script_vars);
        {
            uint16_t *es = (uint16_t *)g_entity_state;
            for (int idx = 0; idx < 0x8E; ++idx) {
                es[idx * 4 + 1] = 0;        /* in_inventory_flag only */
            }
        }
        ResetInventory();
        LoadStage(1);
    }

    /* SAVE-LOAD branch (flag 0x10): g_cur_komnata is already set by
 * LoadSaveSlot. Skip intro AVI, jump straight into play. */
    if (flags & 0x10) {
        /* Make sure stage state is sane — if g_stage_va wasn't restored
 * by LoadSaveSlot, fall back to stage 1. */
        if (!g_stage_va) { g_stage_va = 0x00428220; g_cur_etap = 1; }
        if (g_cur_komnata == 0) g_cur_komnata = 1;
    }

    /* T39 inlined body (was play_first_scene_demo). Single-call
 * play_demo_scene drives the gameplay loop until ESC / F12 TAK /
 * PlatformShouldQuit / g_scene_quit. All komnata transitions
 * happen in-place via LoadKomnataScene (T22 phase B). */
    extern uint32_t g_tick_counter;
    extern void EntityListClearAll(void);
    extern void LoadActorWalkAnims(uint32_t stage_va);

    g_stats.boot_tick = g_tick_counter;             /* T56 — playthrough timer */
    /* Stage-1 demo fallback — if RunGameStageLoop was entered without
 * flags 0x02 (so LoadStage(1) wasn't called) or 0x10 (save-load),
 * set g_stage_va explicitly so DispatchClickEvent finds the verb
 * tables. */
    if (!g_stage_va) {
        g_stage_va = 0x00428220;
        g_cur_etap = 1;
    }
    LoadActorWalkAnims(g_stage_va);

    uint16_t cur_komnata = (g_cur_komnata != 0) ? g_cur_komnata : 1;
    g_cur_komnata = cur_komnata;        /* play_demo_scene prologue reads */

    /* Initial scene: call LoadKomnataScene which handles ANY stage via
 * the komnata table (LoadKomnata) + DemoScene synth fallback for
 * komnaty not in s_demo_scenes[]. Previously hardcoded k_names[]
 * only covered stage 1's 4 komnaty — stage 3's start_komnata=6
 * (or any stage 2+ entry) hit "no DemoScene" + bailed without
 * running play_demo_scene. */
    LoadKomnataScene(cur_komnata);
    if (g_current_scene) {
        (void)play_demo_scene((const DemoScene *)g_current_scene);
        EntityListClearAll();
        /* Stop any looping (N,M) SFX still running — the original frees
 * every per-komnata asset on gameplay exit ( chain),
 * each SampleTable destructor stops its wavs.
 * Without this, e.g. a [sampl] WAV (N,M) loop started mid-scene
 * keeps mixing into the menu after the user backs out via ESC /
 * Pytanie TAK / death cutscene path. ResetFrameSfxState walks
 * g_sfx_state[] and silences active channels (same call used by
 * LoadKomnata between rooms). */
        extern void ResetFrameSfxState(void);
        ResetFrameSfxState();
    } else {
        fprintf(stderr, "[scene] RunGameStageLoop: LoadKomnataScene(%u) yielded no scene\n",
                cur_komnata);
    }

    /* Post-loop game-over handling — switch on
 * g_game_over_code at the bottom of each iteration. */
    if (g_game_over_code) {
        switch (g_game_over_code) {
        case 1:                                   /* death */
            PlaySceneCutsceneAvi("Dane_14.dta");
            break;
        case 3: {                                 /* chapter-select UI —
 *
 * Order of operations matches original exactly:
 * 1. Refresh sel_tlo buttons (1st pass) — computes all_done
 * 2. If !all_done AND g_stage non-null: play current stage's
 * alt_avi (outro), stash alt3_avi for post-pick playback
 * 3. Refresh buttons again (2nd pass, original is redundant
 * here but we mirror it for fidelity)
 * 4. If all_done: play Dane_13.dta (credits) + bail
 * 5. RunMenuScene(sel_tlo) → user picks stage
 * 6. LoadStage(picked)
 * 7. Refresh buttons (3rd pass post-load)
 * 8. If still !all_done: play stashed alt3_avi (= "transition
 * into picked stage") */

            /* Step 1: first pass — sets g_sel_tlo_scene buttons + tells
 * us whether all 4 are completed. */
            int all_done = SelTloRefreshButtons();

            /* Step 2: outro of just-finished stage + stash alt3 for later.
 *
 * 0x0040C2EE: PlaySceneCutsceneAvi(stage+0x2A) = alt_avi,
 * local_1c = stage+0x2E = alt3_avi. */
            const char *stashed_alt3 = NULL;
            if (!all_done && g_stage) {
                if (g_stage->alt_avi) {
                    fprintf(stderr, "[game-over=3] outro AVI: %s\n",
                            g_stage->alt_avi);
                    PlaySceneCutsceneAvi(g_stage->alt_avi);
                }
                stashed_alt3 = g_stage->alt3_avi;
            }

            /* Step 3: second pass — redundant with step 1 but matches
 * the original's belt-and-braces double-rebuild. State could
 * conceivably have changed during the AVI (script via op
 * dispatches?) so re-checking is safe. */
            all_done = SelTloRefreshButtons();

            /* Step 4: all stages done → play Dane_13.dta credits sting,
 * then DROP into the menu so the user sees the assembled
 * ACME on the map and can click the green "finale" button
 * (id=0x16). @ 0x0040C3F4:
 * LoadStage((ushort));
 * — note no early bail. The previous port version `break`ed
 * here, which meant the player never saw the green button
 * and the Monter stage 5 finale was unreachable from a
 * regular playthrough. */
            if (all_done) {
                fprintf(stderr, "[game-over=3] all stages done → Dane_13.dta + map with green button\n");
                PlaySceneCutsceneAvi("Dane_13.dta");
            }

            /* Step 5: chapter-select menu. With button_count=5 (set by
 * SelTloRefreshButtons when all_done) the ACME-complete green
 * button is now hit-testable; SelTloClick writes
 * s_chapter_pick=5 on hit. */
            s_chapter_pick = 0;
            RunMenuScene(0, &g_sel_tlo_scene);

            /* Step 6: load picked stage. NOTE — original
 * stores the pick in via a separate click
 * handler; our SelTloClick writes s_chapter_pick directly
 * (1-based). 5 = Monter finale (intro=Dane_12, alt=Dane_11,
 * alt3=Dane_13). */
            if (s_chapter_pick >= 1 && s_chapter_pick <= 5) {
                fprintf(stderr, "[game-over=3] LoadStage(%d)\n", s_chapter_pick);
                LoadStage((uint16_t)s_chapter_pick);
            }

            /* Step 7: third pass — post-load button rebuild. */
            all_done = SelTloRefreshButtons();

            /* Step 8: intro AVI of newly-loaded stage. The original
 * plays the alt3_avi stashed from BEFORE the load — that's
 * the prev stage's "going to X" transition. Skipped on the
 * finale path because stashed_alt3 stays NULL (set only
 * when !all_done in step 2). */
            if (!all_done && stashed_alt3) {
                fprintf(stderr, "[game-over=3] transition AVI: %s\n",
                        stashed_alt3);
                PlaySceneCutsceneAvi(stashed_alt3);
            }
            break;
        }
        case 4:                                   /* stage-end —
 * @ 0x0040C328:
 *;;
 * // fall through to common epilogue:
 *
 * Plays BOTH outro (alt_avi) and transition (alt3_avi)
 * back-to-back without the chapter-select menu between
 * them. Used when stage-end is "automatic" (no player
 * choice required). */
            if (g_stage) {
                if (g_stage->alt_avi) {
                    fprintf(stderr, "[game-over=4] outro AVI: %s\n",
                            g_stage->alt_avi);
                    PlaySceneCutsceneAvi(g_stage->alt_avi);
                }
                if (g_stage->alt3_avi) {
                    fprintf(stderr, "[game-over=4] transition AVI: %s\n",
                            g_stage->alt3_avi);
                    PlaySceneCutsceneAvi(g_stage->alt3_avi);
                }
            }
            /* Mark current stage as completed so next chapter-select
 * shows it as done. Original sets the bit elsewhere (likely
 * via a script op that writes through a var-indirection
 * mapping — exact path still under RE); we set it here as
 * a defensive port shortcut so the user-visible behaviour
 * is correct regardless of trigger source. */
            if (g_cur_etap >= 1 && g_cur_etap <= 32) {
                g_completed_stages |= 1u << (g_cur_etap - 1);
                fprintf(stderr, "[game-over=4] stage %u completed "
                                "(g_completed_stages=0x%X)\n",
                        (unsigned)g_cur_etap, g_completed_stages);
            }
            break;
        }
    }
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
