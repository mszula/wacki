/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
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
#include "wacki/log.h"
#include <SDL.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* =========== globals ====================================================
 * Only the ones not owned by another module live here. See game.c
 * (g_tick_counter, g_lmb_handled, g_stage_table, …), script.c
 * (g_active_actor, g_perspective_*) and assets.c (g_persp_band_count).
 */
uint32_t  g_entity_state[0x11C];        /* g_entity_state */
uint32_t  g_scene_snapshot[0x1E];       /* g_inventory */
/* g_persp_profile is defined in src/assets.c (canonical storage). */

/* ActorWalkToBlocking — implements ops 0x10/0x11/0x12 wait-for-walk.
 *
 * EACH per-entity VM tick inside ProcessGameFrameTick advances the
 * walker via op 0x15/0x16's step loop — step size = walker bytecode's
 * step operand (× perspective scale). We bind the walker through
 * BindActorWalker (which plants the path immediately) and pump
 * ProcessGameFrameTick + SDL_Delay until +0x4C/+0x50 are zero.
 *
 * An earlier port-only impl stepped 1 pixel per ProcessGameFrameTick
 * with no SDL_Delay — spin loop pegged the CPU, walker traversed
 * 1000+ px/sec, actor teleported off-screen before the verb-script
 * could finish, and QUIT events were never honoured (game unkillable
 * during any verb-script walk). */
extern int  BindActorWalker(int actor_idx, int target_x, int target_y);

/* Komnata flag bitfield — set from komnata table entry[+4] inside
 * LoadKomnata. Bits read by the engine:
 *   bit 0 — panel visible    (PanelHitTest, HUD draw)
 *   bit 1 — actors active    (actor walker / UpdateActorMovement)
 *   bit 2 — link default kind=3/4 entities (cursor + krazek)
 *   bits 8-15 — perspective band count, shifted by ScriptCallBgMaskSetup
 *               as `(flags & 0xff02) << 1` (so the u16 width matters).
 * Default 2 = actors-only, no panel — matches menu / cutscene boot
 * state. LoadKomnata raises bit 0 for in-game rooms. */
uint16_t  g_komnata_flags = 2;
uint16_t  g_selected_save_slot = 0;

void     *g_dialogues_obj = NULL;
void     *g_scripts_obj   = NULL;
void     *g_items_obj     = NULL;
AnimAsset *g_panel_asset  = NULL;       /* stage panel (panel.wyc) */
AnimAsset *g_items_atlas  = NULL;       /* przedm.wyc inventory icons */
Entity   *g_actor[2]      = { NULL, NULL };

/* g_hover_scene_verb — written by ClickHitTest; read by cursor-state
 * machine to pick an icon. 0x26 = no hover. */
uint16_t  g_hover_scene_verb  = 0x26;

/* =========== version / file helpers ===================================== */

/* GetFileVersionInfo probe — the portable build can't probe a
 * Windows .dll version, so always claim a sufficiently new one. */
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
{ (void)p; (void)owner; LOG_TRACE("avi", "open(%s) ok", path?path:"(null)"); return 1; }

/* =========== DirectSound version checker (no-op shims) ================== */
typedef struct DSoundVerChecker { int dummy; } DSoundVerChecker;
void  DSoundVer_Init   (DSoundVerChecker *self) { (void)self; }
int   DSoundVer_IsBad  (DSoundVerChecker *self) { (void)self; return 0; }
short DSoundVer_Confirm(DSoundVerChecker *self) { (void)self; return 4; /* IDRETRY */ }
void  DSoundVer_Free   (DSoundVerChecker *self) { (void)self; }

void  PlayAnimation(uint16_t anim, uint16_t frame)
{ (void)anim; (void)frame; }
void  PrintTextOnScreen(uint16_t hx, uint16_t hy, const char *text)
{ (void)hx; (void)hy; if(text) LOG_INFO("text", "%s", text); }
void  PaletteFadeStep(int delta) { (void)delta; }
void  PaletteFadeInOut(uint16_t pct, const uint8_t *pal,
                       uint16_t first, uint32_t flags, void *cb)
{ (void)pct;(void)flags;(void)cb; if(pal) InstallPalette(pal, first); }
void  SetPalette(const uint8_t *pal, uint16_t first) { InstallPalette(pal, first); }

extern uint8_t *g_back_shadow;
extern uint8_t  g_palette_rgb[256*3];

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

/* Frame delta in 10 ms TICKS.
 *
 * The original game arms a 10 ms timeSetEvent ISR that increments
 * g_tick_counter; g_frame_delta_ticks holds (now - prev) ticks per
 * frame. EVERY in-PE site that reads it (cursor anim accumulator,
 * entity VM frame countdown +0x3C, dialog/prop timer, op 0x14
 * WAIT_MS, op 0x26/0x3D anim-frame waits, dialog-choice dismiss)
 * expects this unit, not real milliseconds. Driving them with
 * g_frame_delta_ms (real ms) makes everything animate ~10× too
 * fast.
 *
 * Updated in lockstep with g_frame_delta_ms inside the PGFT Inner /
 * EntityWalkerTick dt blocks; an accumulator carries the sub-10 ms
 * remainder so we don't drift at frame rates that aren't a clean
 * multiple of 10 (e.g. 16 ms @ 60 fps emits 2/1/2/1/… ticks). */
uint16_t g_frame_delta_ticks = 1;

/* Palette fade machinery
 *
 * case 0x48 (full fade): fade_progress_alt = 0; fade_step_alt = step;
 * load target into fade_target_buf;
 * zero fade_source_snapshot work buffer.
 * case 0x49 (step): if (progress < 100) {
 * (work, target, out, progress%);
 * (out, 0); // install
 * // else return previous value
 * case 0x4A (instant?): similar to 0x48 but without progress reset.
 *
 * linearly interpolates each palette byte between source
 * (fade_source_snapshot, snapshot of pal at fade start) and target
 * by progress/100, writing result to live_palette.
 *
 * Port state mirrors:
 * s_pal_fade_step = fade_step_alt (per-step advance) */

void ScriptCallDestroyEnt(uint16_t id, int also_unreg_asset);  /* fwd decl */

extern Entity *AllocEntity(uint16_t w, uint16_t h, uint16_t kind, uint16_t flags);
extern void    LinkEntityToList(Entity **head, Entity *e, int position);
extern Entity *g_render_list_head;
extern Entity *g_click_list_head;
extern void    RegisterEntityForUpdate(Entity *e, uint16_t kind, uint16_t id);
extern void   *FindUpdateRegistration(uint16_t kind, uint16_t id);
extern const void *xlat_binary_ptr(uint32_t);
