/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * tests/test_engine_stubs.c — engine externs the test binary needs.
 *
 * Linked production sources (TEST_ENGINE_SRCS in Makefile):
 *   depack.c archive.c graphics.c pe_loader.c heap.c cygio.c
 *   assets.c font.c save.c binary_data.c timer.c script.c
 *   stubs.c  ← linked via tests/sdl_stub/SDL.h (declares SDL types +
 *               stubs we provide below for SDL_Delay / SDL_GetTicks /
 *               SDL_Create*Surface / SDL_SaveBMP / etc.)
 *
 * NOT linked (deeply coupled — actor.c pulls graphics tick state,
 * game.c is the main loop, audio.c is SDL_AudioDevice, flic.c is
 * AVI decoder, platform_sdl.c is the window/event layer, main.c is
 * the entry point):
 *
 *   actor.c game.c audio.c flic.c platform_sdl.c main.c
 *
 * This file provides:
 *   1. SDL function stubs (debug screenshot path in stubs.c) — no-ops.
 *   2. Engine globals defined in actor.c/game.c (g_render_list_head,
 *      g_cur_etap, g_tick_counter, etc.) — zero-init.
 *   3. Function stubs for ~30 actor.c/game.c/audio.c entry points.
 *
 * Capture infrastructure for VM dispatch tests is GONE — those tests
 * now exercise production stubs.c implementations and verify state
 * changes (s_sound_queue contents, g_panel_verb_tab rotation, etc.)
 * directly. The few remaining capture counters are for symbols only
 * actor.c/game.c provide.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "wacki.h"
#include "test_engine_stubs.h"

/* SDL stub types — pulled in via tests/sdl_stub/SDL.h (which production
 * stubs.c #includes via <SDL.h>). */
#include "SDL.h"

/* ---- SDL function stubs (for stubs.c's debug screenshot path) ------- */

void     SDL_Delay(uint32_t ms)                                       { (void)ms; }
uint32_t SDL_GetTicks(void)                                           { return 0; }
const char *SDL_GetError(void)                                        { return "(test stub)"; }

SDL_Surface *SDL_CreateRGBSurfaceWithFormatFrom(
    void *pixels, int w, int h, int depth, int pitch, uint32_t format)
{
    (void)pixels; (void)w; (void)h; (void)depth; (void)pitch; (void)format;
    return NULL;
}
int  SDL_SetPaletteColors(SDL_Palette *p, const SDL_Color *c, int first, int n)
{ (void)p; (void)c; (void)first; (void)n; return 0; }
int  SDL_SaveBMP(SDL_Surface *s, const char *file)
{ (void)s; (void)file; return -1; }
void SDL_FreeSurface(SDL_Surface *s) { (void)s; }

/* ---- platform_sdl.c stubs ------------------------------------------- */

void PlatformPresent(const uint8_t *shadow, const uint8_t *palette_rgb,
                      int w, int h)
{ (void)shadow; (void)palette_rgb; (void)w; (void)h; }

void PlatformPumpEvents(void) {}
void PumpEvents(void) {}

/* Headless: graphics.c FadeOutToBlack early-returns on this, so the fade
 * loop never runs under tests (and no frames are presented anyway). */
int g_headless = 1;

/* Settable: PlatformShouldQuit return value. Wait loops in script.c
 * (op 0x14/0x15/0x26/0x3D) check this each iteration and break if
 * non-zero. */
int g_stub_should_quit = 0;
int  PlatformShouldQuit(void) { return g_stub_should_quit; }

/* ---- audio.c stubs -------------------------------------------------- *
 *
 * src/audio/sfx.c IS linked into the test binary (so tests can hit
 * ParseSamplTagsForKomnata, ResetDynamicSfxTable, TriggerFrameSfx,
 * StopAllSfxForAsset etc.). Its dependencies on audio.c — the mixer
 * device, channel array, lock/unlock — are stubbed here as no-ops.
 *
 * Audio device + channels: zero-init so sfx.c's replay-guard reads
 * find inactive channels.
 *
 * mixer_ensure_open returns 0 → PlaySfxPannedAndGetChannel bails before
 * touching the device, which is exactly what we want under tests. */

#include "../src/audio/mixer_internal.h"

struct MixChannel  s_mix[MIX_CHANNEL_COUNT];

/* Defined in audio.c, used by sfx.c gates. With mixer_ensure_open
 * returning 0 these never gate anything meaningful — sfx_handle_end_
 * frames and the parser don't read them at all — but the symbols
 * need to resolve at link time. */
int g_audio_sfx_enabled   = 1;
int g_audio_sound_enabled = 1;

int  mixer_ensure_open(void)                          { return 0; }
int  mixer_load_wav(const char *n, Uint8 **buf, Uint32 *len)
{ (void)n; if (buf) *buf = NULL; if (len) *len = 0; return 0; }
void mixer_assign(int idx, Uint8 *buf, Uint32 len, int loop,
                  const char *name)
{ (void)idx; (void)buf; (void)len; (void)loop; (void)name; }
void mixer_stop_channel(int idx)                      { (void)idx; }

/* Audio-output HAL — sfx.c's MIX_DEV_LOCK routes here now (was the SDL
 * device lock). No real device under tests, so they're inert. */
int  plat_audio_is_open(void)                         { return 0; }
void plat_audio_lock(void)                            { }
void plat_audio_unlock(void)                          { }

void StopMenuMusic(void)                              {}
uint32_t PlayDialogLine(const char *wav)              { (void)wav; return 0; }
void StopDialogLine(void)                             {}
int  IsDialogLinePlaying(void)                       { return 0; }

/* From menu/options.c — render.c reads the Grafika tint toggle. Tests
 * exercise the tint math directly, so report the effect as enabled. */
int  GraphicsAlphaFxEnabled(void)                    { return 1; }

/* ---- game.c stubs --------------------------------------------------- */

/* From game.c — save.c LoadSaveSlot calls this. Tests don't exercise
 * the slot-restore path; stub returns 1. */
int LoadStage(uint16_t stage)         { (void)stage; return 1; }

/* From game.c — stubs.c LoadKomnata calls these. */
void LoadKomnataScene(uint16_t id)    { (void)id; }
/* ParseSamplTagsForKomnata is now provided by the real audio/sfx.c
 * linked into TEST_ENGINE_SRCS — tests can exercise the [sampl] parser
 * for real (see tests/test_sampl_parser.c). */

/* ScriptGoToKomnata — stubs.c provides production version. Tests that
 * want to capture the call check g_cur_komnata side-effect instead.
 * (Settable captures retained for legacy test references, but they're
 * no longer wired to the production function.) */
uint16_t g_stub_go_to_komnata_id = 0;
int g_stub_go_to_komnata_calls = 0;

/* From game.c — stubs.c calls this from FlushQueuedClicks. Capture last
 * 16 (obj, verb) dispatches so tests can verify the per-frame drain. */
#define DISPATCH_CAPTURE_MAX 16
uint16_t g_stub_dispatch_obj [DISPATCH_CAPTURE_MAX];
uint16_t g_stub_dispatch_verb[DISPATCH_CAPTURE_MAX];
int      g_stub_dispatch_count = 0;
void DispatchClickEvent(uint16_t obj_id, uint16_t verb_id)
{
    if (g_stub_dispatch_count < DISPATCH_CAPTURE_MAX) {
        g_stub_dispatch_obj [g_stub_dispatch_count] = obj_id;
        g_stub_dispatch_verb[g_stub_dispatch_count] = verb_id;
    }
    ++g_stub_dispatch_count;
}

/* ---- actor.c is NOW LINKED ----------------------------------------- *
 *
 * Production AllocEntity, LinkEntityToList, EntityListAt/Count,
 * FindUpdateRegistration*, BindActorWalker, ent_ptr_intern/resolve,
 * g_render_list_head, g_click_list_head — all from actor.c.
 *
 * Test entity injection now uses production EntityListClearAll +
 * LinkEntityToList on the real g_click_list_head. */

extern Entity *g_click_list_head;
extern Entity *g_render_list_head;
extern Entity *AllocEntity(uint16_t w, uint16_t h, uint16_t kind, uint16_t flags);
extern void    LinkEntityToList(Entity **head, Entity *e, int position);
extern void    EntityListClearAll(void);

/* Storage for tests that want a static entity buffer (vs production
 * AllocEntity'd buffer). The injection helper wires it into the real
 * click list via LinkEntityToList. */
static uint8_t  s_click_desc_buf[256];
static uint16_t s_click_verb_tab[2];

void test_set_click_list(Entity **list, int n)
{
    /* Reset production click list, then link supplied entities back in. */
    EntityListClearAll();
    for (int i = 0; i < n && i < 16; ++i) {
        LinkEntityToList(&g_click_list_head, list[i], /*position=*/0);
    }
}

/* Helper: prepare an entity buffer so production FindEntityByVerbId
 * matches `verb_id` against it via the kind=2 cached-verb path:
 *   +0x08 click_kind = 2
 *   +0x0e verb_table slot = 0 (NULL → fall to cached)
 *   +0x12 cached verb_id = `verb_id`
 *
 * Test injects via test_set_click_list. */
void test_prepare_entity_with_verb(Entity *e, uint16_t verb_id)
{
    /* Configures `e` itself as a kind=2 click descriptor — but production
     * FindEntityByVerbId returns NULL for kind=2 (per stubs.c:1996-2002).
     * Use test_inject_entity_for_verb() instead, which sets up the
     * kind=1 owner chain so FindEntityByVerbId returns `e`. */
    uint8_t *eb = (uint8_t *)e;
    *(uint16_t *)(eb + 0x08) = 2;
    *(uint32_t *)(eb + 0x0E) = 0;
    *(uint16_t *)(eb + 0x12) = verb_id;
}

/* Internal storage for the kind=1 click descriptor chain — buffer
 * already declared at file top (256 bytes). */

extern void RegisterEntityForUpdate(Entity *e, uint16_t kind, uint16_t id);

void test_inject_entity_for_update(Entity *e, uint16_t kind, uint16_t id)
{
    /* Production RegisterEntityForUpdate populates the update table that
     * FindUpdateRegistration walks. Tests need this for op 0x0B/0x0F/
     * 0x3D/0x50/0x51 paths. */
    RegisterEntityForUpdate(e, kind, id);
}

void test_inject_entity_for_verb(Entity *e, uint16_t verb_id)
{
    /* Production FindEntityByVerbId (stubs.c:1966-2006) walks the click
     * list and for kind=1 entries returns the OWNER (resolved via
     * ent_ptr_intern slot at +0x0a). For verb matching it reads the
     * verb_table at +0x0e:
     *   vt[0]    = count
     *   vt[1..]  = verb_id per frame (or vt[count] if frame >= count)
     *
     * Set up a click descriptor (kind=1) whose owner = `e` and verb
     * table contains the requested `verb_id`. */
    memset(s_click_desc_buf, 0, sizeof s_click_desc_buf);
    *(uint16_t *)(s_click_desc_buf + 0x08) = 1;                /* click_kind = 1 */
    *(uint32_t *)(s_click_desc_buf + 0x0A) = ent_ptr_intern(e);/* owner slot */

    s_click_verb_tab[0] = 1;                                   /* count */
    s_click_verb_tab[1] = verb_id;                             /* verb */
    *(uint32_t *)(s_click_desc_buf + 0x0E) =
        ent_ptr_intern(s_click_verb_tab);

    /* NOTE: do NOT touch e[+0x30] (frame index). With count=1, the
     * production verb-lookup picks vt[1]=verb_id regardless of frame
     * (frame >= count → vt[count]=vt[1]; frame < count → vt[idx+1]=vt[1]).
     * Tests that need a specific frame set it themselves. */

    Entity *list[1] = { (Entity *)s_click_desc_buf };
    test_set_click_list(list, 1);
}

/* FindUpdateRegistration / Except — production actor.c provides. */
/* BindActorWalker, ActorWaypointsSceneInit — production actor.c provides. */
/* ActorWalkToBlocking / ActorWalkBothBlocking — production stubs.c provides. */

/* g_actor[2] is now defined by src/stubs.c (linked into TEST_ENGINE_SRCS),
 * so the test-side definition would clash at link time. Kept as a
 * note for future maintainers — don't re-add. */

/* ent_ptr_intern / resolve — production actor.c provides. */

/* ---- font.c integration --------------------------------------------- */

/* g_default_font — stubs.c speech balloon path references. */
struct FontHandle;
typedef struct FontHandle FontHandle;
FontHandle *g_default_font = NULL;

/* ---- game.c globals ------------------------------------------------- */

WackiStats g_stats;
uint16_t  g_cur_etap;
uint16_t  g_cur_komnata;
uint32_t  g_tick_counter;
uint8_t   g_lmb_handled;
uint8_t   g_lmb_clicked;
uint16_t  g_held_item    = 0x26;          /* default = empty/sentinel */
uint32_t  g_stage_va     = 0;
/* g_panel_verb_tab is now defined by src/hud/panel.c (in TEST_ENGINE_SRCS). */

/* ProcessGameFrameTick — game.c provides. Wait loops in script.c
 * (op 0x14/0x15/0x26/0x3D) call it; stub is no-op + counter. */
int g_stub_process_frame_calls = 0;
void ProcessGameFrameTick(void) { ++g_stub_process_frame_calls; }

/* paint_rawb_pic — game.c provides. op 0x54 SHOW_PICTURE calls it. */
int paint_rawb_pic(const void *blob, uint32_t size, int as_overlay)
{ (void)blob; (void)size; (void)as_overlay; return 1; }

/* is_walkable_at — game.c provides. actor.c walker checks walkability. */
int is_walkable_at(int sx, int sy) { (void)sx; (void)sy; return 1; }

/* TriggerFrameSfx — real implementation linked from audio/sfx.c. */

/* Scene BG raw — game.c provides. actor.c walk-behind path references. */
const void *g_scene_bg_raw = NULL;
uint32_t    g_scene_bg_size = 0;

/* Walkability bitmap globals (set by op 0x2C BG MASK SETUP path). */
const uint8_t *g_walk_fld_pixels = NULL;
uint16_t       g_walk_fld_w      = 0;
uint16_t       g_walk_fld_h      = 0;
uint16_t       g_walk_fld_stride = 0;
uint16_t       g_walk_fld_ox     = 0;
uint16_t       g_walk_fld_oy     = 0;

/* Mouse coordinates — stubs.c speech balloon uses them. */
int16_t g_mouse_x = 0;
int16_t g_mouse_y = 0;

/* g_default_world_state is in stubs.c (production). */

/* ---- legacy capture state for actor.c-tested ops -------------------- *
 *
 * VM dispatch tests that call PRODUCTION ScriptCall* (now linked from
 * stubs.c) can no longer use capture counters — they verify state
 * changes directly (e.g. s_sound_queue[] contents, g_panel_verb_tab[]).
 *
 * What remains is settable values some tests need to inject. */

uint16_t g_stub_inventory_has = 0;
struct Entity *g_stub_entity_for_verb = NULL;
void *g_stub_update_registration_for_kind_id = NULL;
int g_stub_inv_page_next_rc = 0;
int g_stub_inv_page_prev_rc = 0;

/* g_stub.* counters retained as zeroes — tests reference them but
 * since production ScriptCall* don't increment them, those tests have
 * been rewritten or deleted. Keeping the struct so test files don't
 * break on header references. */
struct vm_stub_counters g_stub = { 0 };
struct vm_stub_last_args g_stub_last = { 0 };

void vm_stubs_reset(void)
{
    memset(&g_stub,      0, sizeof g_stub);
    memset(&g_stub_last, 0, sizeof g_stub_last);
    g_stub_inventory_has = 0;
    g_stub_entity_for_verb = NULL;
    g_stub_update_registration_for_kind_id = NULL;
    g_stub_inv_page_next_rc = 0;
    g_stub_inv_page_prev_rc = 0;
    g_stub_should_quit = 0;
    g_stub_process_frame_calls = 0;
    g_stub_go_to_komnata_id = 0;
    g_stub_go_to_komnata_calls = 0;
    /* Clear injected click list — prevents previous test's entity from
     * leaking into the next test's FindEntityByVerbId calls. */
    EntityListClearAll();
}
