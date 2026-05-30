/*
 * stubs.c — definitions for every global and helper that the rest of the
 * engine references through `extern` but whose full implementation we
 * deferred from the Ghidra reverse. Each stub:
 *
 * • carries a one-line note with the original FUN_* / DAT_* address
 * • has minimal-but-typed behaviour (no-ops, sane defaults)
 * • exists so that the engine LINKS cleanly and the SDL build runs
 *
 * If you ever fully port the binary, replace each stub with the
 * corresponding decompiled body.
 */
#include "wacki.h"
#include <SDL.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* =========== globals ====================================================
 * Only the ones not owned by another module live here. See game.c
 * (g_tick_counter, g_lmb_handled, g_stage_table, …), script.c
 * (g_active_actor, g_perspective_*) and assets.c (g_persp_band_count).
 */
uint32_t  g_entity_state[0x11C];        /* DAT_00449D28 */
uint32_t  g_scene_snapshot[0x1E];       /* DAT_00443332 */
int16_t   g_persp_profile[0x22*2];      /* DAT_0044E5F8 */

/* g_next_cd_check removed — rule #7. */

/* T22 phase A/B staging globals removed in T42:
 * - g_pending_komnata: replaced by in-place LoadKomnataScene call
 * - g_komnata_loaded_by_op20: outer-loop guard no longer needed
 * after play_first_scene_demo collapsed to single play_demo_scene
 * call (T22 phase B). */

/* Scene transitions (ScriptGoToKomnata) → src/scene/navigation.c
 * Actor walking (ActorWalkToBlocking, ActorWalkBothBlocking) →
 * src/scene/actor_walk.c
 * Stage descriptors (BuildStageTable, LoadActorWalkAnims, the
 * g_stage_table/g_stage_va_table/g_actor_walk_anim globals,
 * and g_default_world_state) → src/scene/stage.c */

/* LoadKomnata moved to src/scene/komnata.c. */

/* ActorWalkToBlocking — 1:1 with op 0x10/0x11/0x12 wait-for-walk
 * loop from Ghidra @ RunScriptInterpreter 0x00407820 case 0x10:
 *
 * if (actor[+0x22] != tx || actor[+0x24] != ty) {
 * DAT_0044e6a4 = idx; // swap active to this actor
 * DAT_0044e5ac = 1; // synthesize click pending
 * DAT_0044e5a4 = 1; // synthesize walker-bind flag
 * DAT_0044e570 = -1; // walk-target id reset
 * actor[+0x4C] = 0; actor[+0x50] = 0;
 * UpdateActorMovement(tx, ty); // binds walker via standard path
 * DAT_0044e6a4 = saved_active;
 * do {
 * DAT_0044e5ac = 0; DAT_0044e5a4 = 0;
 * ProcessGameFrameTick;
 * } while (actor[+0x4C] != 0 || actor[+0x50] != 0 || DAT_0044e570 != -1);
 * }
 *
 * EACH per-entity VM tick inside ProcessGameFrameTick advances the
 * walker via op 0x15/0x16's step loop — step size = walker bytecode's
 * step operand (× perspective scale). We just bind the walker through
 * BindActorWalker (which plants the path immediately, see #fix-2) and
 * pump ProcessGameFrameTick + SDL_Delay until +0x4C/+0x50 are zero.
 *
 * Previous port-only impl stepped 1 pixel per ProcessGameFrameTick
 * call with no SDL_Delay → spin loop pegged the CPU, walker traversed
 * 1000+ px/sec → actor teleported off-screen before the verb-script
 * could finish; QUIT events were also never honoured → game unkillable
 * during any verb-script walk. */
extern int  BindActorWalker(int actor_idx, int target_x, int target_y);
extern int  PlatformShouldQuit(void);

/* DAT_0044E448 — komnata flag bits (set from komnata table entry[+4]
 * inside / LoadKomnata):
 * bit 0 = panel visible (read by PanelHitTest)
 * bit 1 = actors active (read by actor.c — UpdateActorMovement gate)
 * bit 2 = link the kind=3/4 default entities (cursor + krazek)
 * Default is 2 (actors-only) so the menu/cutscene path doesn't render
 * the panel; LoadKomnata raises bit 0 for in-game rooms. */
uint16_t  g_settings_anim_active = 2;   /* DAT_0044E448 — komnata flags
 * (T121: u16 not u8 — high bits
 * 8-15 needed by
 * ScriptCallBgMaskSetup perspective
 * band count `(flags & 0xff02) << 1`). */
uint16_t  g_active_target_y = 0;        /* DAT_0044E5A8 */
uint16_t  g_selected_save_slot = 0;
int       g_cd_drive_letter_present = 1;/* DAT_00475A54 */

void     *g_dialogues_obj = NULL;
void     *g_scripts_obj   = NULL;
void     *g_items_obj     = NULL;
AnimAsset *g_panel_cursor = NULL;       /* DAT_0044E698 */
AnimAsset *g_panel_asset  = NULL;       /* DAT_00453744 — stage panel (panel.wyc) */
AnimAsset *g_items_atlas  = NULL;       /* DAT_0044E6AC — przedm.wyc icons */
Entity   *g_actor[2]      = { NULL, NULL };

/* g_hover_scene_verb — written by ClickHitTest; read by cursor-state
 * machine to pick an icon. 0x26 = no hover. */
uint16_t  g_hover_scene_verb  = 0x26;

/* Panel hit-test + panel globals (g_panel_verb_tab, g_hover_panel_verb,
 * g_panel_cursor_redirect, g_panel_cursor_redirect2) moved to
 * src/hud/panel.c. */

/* LoadItemNamesTable + ItemHoverDwellTick + per-item WAV name table
 * moved to src/hud/items.c. */


/* Inventory + panel page rotation (Inventory, ResetInventory,
 * PanelPageSwap, InventoryPage*, InventoryAddItem, InventoryRemoveItem,
 * InventoryDropItem, InventoryHasItem, InventorySetPageForItem) plus
 * the inventory-side globals (g_panel_page_idx,
 * g_panel_verb_tab_backup, g_panel_redraw) moved to
 * src/hud/inventory.c. */

/* =========== version / file helpers ===================================== */

/* — GetFileVersionInfo probe; portable build can't probe a
 * .dll version, so always claim a sufficiently new one. */
uint32_t GetDllPackedVersion(const char *dll) { (void)dll; return 0x00500004; }

/* =========== blit-row helpers used by the original Win32 BlitSprite ====
 * The portable graphics.c already inlines these, so these are placeholders
 * to satisfy any remaining `extern` refs in legacy code paths. */
void BlitColorKeyRow(uint8_t *d, const uint8_t *s, uint16_t n)
{ for (uint16_t i=0;i<n;++i) if (s[i]) d[i]=s[i]; }
void BlitTranslucentRow(uint8_t *d, const uint8_t *s, uint16_t n, const uint8_t *xlate)
{ for (uint16_t i=0;i<n;++i) if (s[i]) d[i] = xlate ? xlate[(d[i]<<8)|s[i]] : (uint8_t)((d[i]+s[i])>>1); }
void OptimiseRectList(void *src, uint16_t count, void **out, uint32_t *outc)
{ *out = src; *outc = count; }
void RestoreSurfaceIfLost(void *o) { (void)o; }
void RestoreLostSurfaceArea(int16_t x,int16_t y,int16_t w,int16_t h)
{ (void)x;(void)y;(void)w;(void)h; }

/* =========== AVI playback (no-op shims) ================================= */
typedef struct AviPlayer { int opened; } AviPlayer;
void *NewAviPlayer(void *p)         { AviPlayer *a=(AviPlayer*)p; if(a) a->opened=0; return p; }
void  DestroyAviPlayer(void *p)     { (void)p; }
void  StartAviPlayback(void *p)     { (void)p; }
int   PollAviPlayback(void *p)      { (void)p; return 0x20D; /* "done" */ }
void  StopAviPlayback(void *p)      { (void)p; }
int   OpenAviCutscene(void *p, const char *path, void *owner)
{ (void)p; (void)owner; fprintf(stderr, "[avi] open(%s) ok\n", path?path:"(null)"); return 1; }

/* =========== DirectSound version checker (no-op shims) ================== */
typedef struct DSoundVerChecker { int dummy; } DSoundVerChecker;
void  DSoundVer_Init   (DSoundVerChecker *self) { (void)self; }
int   DSoundVer_IsBad  (DSoundVerChecker *self) { (void)self; return 0; }
short DSoundVer_Confirm(DSoundVerChecker *self) { (void)self; return 4; /* IDRETRY */ }
void  DSoundVer_Free   (DSoundVerChecker *self) { (void)self; }

/* Animation resolver (FindAnimationScript, PlayActorAnimByPath)
 * moved to src/anim/resolver.c. */
void  PlayAnimation(uint16_t anim, uint16_t frame)
{ (void)anim; (void)frame; }
void  PrintTextOnScreen(uint16_t hx, uint16_t hy, const char *text)
{ (void)hx; (void)hy; if(text) fprintf(stderr, "[text] %s\n", text); }
void  PaletteFadeStep(int delta) { (void)delta; }
void  PaletteFadeInOut(uint16_t pct, const uint8_t *pal,
                       uint16_t first, uint32_t flags, void *cb)
{ (void)pct;(void)flags;(void)cb; if(pal) InstallPalette(pal, first); }
void  SetPalette(const uint8_t *pal, uint16_t first) { InstallPalette(pal, first); }

/* =========== placeholder rendering ======================================
 * If the engine is missing a background asset it would otherwise leave the
 * back-buffer untouched (black). Paint a recognisable test card and the
 * name of the file we *would* have loaded so the user sees activity. */
/* DrawPlaceholderScreen — no-op kept only as a compatibility hook.
 * (The test-card "mosaic" the early-port used has been removed at the
 * user's request: when an asset is missing the engine now just leaves the
 * back-buffer in its previous state and logs to stderr.) */
extern uint8_t *g_back_shadow;
extern uint8_t  g_palette_rgb[256*3];
/* DrawPlaceholderScreen + Screenshot helpers moved to
 * src/util/screenshot.c. */
/* UpdateAllEntities removed — was a no-op placeholder. Its responsibilities
 * are now split between EntityWalkerTick (per-entity VM ticks) and
 * EntityRenderAll (z-sorted blit), both wired into ProcessGameFrameTick. */
/* Deferred click-event queue (EnqueueClickEvent, FlushQueuedClicks)
 * moved to src/scene/click_queue.c. */
/* cd_watchdog_dispatcher REMOVED — rule #7. */

/* ------------------------------------------------------------------------- *
 * Script-VM ↔ subsystem bridges. These mirror the FUN_xxx calls that
 * RunScriptInterpreter makes per-opcode. Until the full entity / dialogue /
 * walker subsystems are ported, most of these are observable no-ops; the
 * sound ones forward to the real audio.c so PLAY_SOUND opcodes in shipped
 * scripts can actually play their WAVs out of Dane_02.dta.
 * ------------------------------------------------------------------------- */

/* Frame delta in milliseconds — read by code that genuinely wants real
 * wall-clock ms (held-item ghost interp, speech-balloon dismiss timer,
 * etc.). Recomputed every frame from the MM timer in the original; we
 * default to ~16 ms (60 fps) which matches our SDL pacing. */
uint32_t g_frame_delta_ms = 16;

/* Frame delta in 10 ms TICKS — 1:1 with DAT_0044E578 in the PE. The
 * original game arms timeSetEvent (call site at 0x00403D84) with a 10 ms
 * periodic timer whose ISR does `INC DAT_0044E454`. That
 * counter is sampled in into DAT_0044E578 = (now - prev)
 * 10 ms units. EVERY in-PE site that reads DAT_0044E578 (cursor anim
 * accumulator, entity VM frame countdown +0x3C, dialog/prop timer, op
 * 0x14 WAIT_MS countdown, op 0x26/0x3D anim-frame waits, dialog choice
 * dismiss) expects this unit, not real milliseconds. Driving them with
 * g_frame_delta_ms (real ms) makes everything animate ~10× too fast.
 *
 * Updated in lockstep with g_frame_delta_ms inside the PGFT Inner /
 * EntityWalkerTick dt blocks; an accumulator carries the sub-10 ms
 * remainder so we don't drift over time at frame rates that aren't a
 * clean multiple of 10 (e.g. 16 ms @ 60 fps emits 2/1/2/1/… ticks). */
uint16_t g_frame_delta_ticks = 1;

/* WackiRand / WackiRandSeed moved to src/util/rng.c. */

/* Positional sound queue (SoundQueueReset, SoundQueueEnqueue,
 * SoundQueueMixForListener) + sound script bridges
 * (ScriptCallSoundPlay, ScriptCallSoundStop) moved to
 * src/audio/sound_queue.c. */

/* Palette fade machinery — 1:1 port of cases 0x48/0x49/0x4A.
 *
 * case 0x48 (full fade): DAT_004549E0 = 0; DAT_00455000 = step;
 * load target into DAT_00451DC8;
 * zero DAT_00454A00 work buffer.
 * case 0x49 (step): if (progress < 100) {
 * progress += step;
 * (work, target, out, progress%);
 * (out, 0); // install
 * return 0;
 * }
 * // else return previous value
 * case 0x4A (instant?): similar to 0x48 but without progress reset.
 *
 * linearly interpolates each palette byte between source
 * (DAT_00454A00, snapshot of pal at fade start) and target
 * by progress/100, writing result to DAT_00454D00.
 *
 * Port state mirrors:
 * g_palette_rgb = DAT_00454D00 / live pal (256 entries × 3 RGB)
 * s_pal_fade_source = DAT_00454A00 (snapshot at fade start)
 * s_pal_fade_target = DAT_00451DC8 (loaded by case 0x48)
 * s_pal_fade_progress = DAT_004549E0 (0..100)
 * s_pal_fade_step = DAT_00455000 (per-step advance) */
/* Palette fade (ScriptCallPalLoad + ScriptCallPalFadeStep) moved to
 * src/script_bridge/palette.c. */

void ScriptCallDestroyEnt(uint16_t id, int also_unreg_asset);  /* fwd decl */

/* BG mask setup (ScriptCallBgMaskSetup) moved to
 * src/scene/bg_mask.c. */

/* Click hit-test (FindEntityByVerbId, ClickHitTest) moved to
 * src/scene/hit_test.c.
 *
 * Mask-list registration (ScriptCallRegMaskList) and the
 * VisibleMasks* compatibility stubs moved to
 * src/scene/mask_list.c. */

extern Entity *AllocEntity(uint16_t w, uint16_t h, uint16_t kind, uint16_t flags);
extern void    LinkEntityToList(Entity **head, Entity *e, int position);
extern Entity *g_render_list_head;
extern Entity *g_click_list_head;
extern void    RegisterEntityForUpdate(Entity *e, uint16_t kind, uint16_t id);
extern void   *FindUpdateRegistration(uint16_t kind, uint16_t id);  /* */
extern const void *xlat_binary_ptr(uint32_t);

/* ------------------------------------------------------------------------- *
 * SpawnActorEntity — 1:1 with op 0x30 SPAWN code path used for Ebek/Fjej.
 *
 * Original engine pre-spawns both actors at game start with their atlas
 * (ebek.wyc / fjej.wyc) bound and verb_id = 1/2 in the click payload, so
 * - op 0x28 SET_ENTITY_XY id=1 → (1) finds Ebek's click
 * entity → returns its owner render entity → moves it.
 * - op 0x28 SET_ENTITY_XY id=2 → same for Fjej.
 * - DispatchClickEvent + verb_table searches resolve actor verb_ids.
 *
 * Returns the spawned render entity (= g_actor[idx]). Owns:
 * - kind=2 render entity registered (kind=2, id) in update table,
 * linked to render list
 * - kind=4 click entity (offset+8 = 1 in click list), bound to
 * a tiny 1-entry verb table { count=1, verb_id }, linked to
 * click list + registered (kind=4, id) in update table */
extern Entity *AllocEntity(uint16_t w, uint16_t h, uint16_t kind, uint16_t flags);

/* SpawnActorEntity + ScriptCallSpawnEntity moved to
 * src/scene/spawn.c. */

/* Asset / entity script bridges (ScriptCallLoadAsset, DestroyEnt,
 * EnableEnt, HideEnt, ShowEnt, WalkMode, WalkTo, AttachProp) moved
 * to src/script_bridge/entity.c. */

/* Speech balloon globals (g_speech_text/actor/tick/dismiss_ticks) and
 * text rendering (TextTranslationLutInit,
 * ScriptCallShowText, TickSpeechBalloon, ScriptCallDialogEnd, plus the
 * balloon-state globals) moved to src/text/balloon.c. */
