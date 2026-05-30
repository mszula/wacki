/*
 * stubs.c — definitions for every global and helper that the rest of the
 * engine references through `extern` but whose full implementation we
 * deferred from the Ghidra reverse. Each stub:
 *
 *   • carries a one-line note with the original FUN_* / DAT_* address
 *   • has minimal-but-typed behaviour (no-ops, sane defaults)
 *   • exists so that the engine LINKS cleanly and the SDL build runs
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
 * Only the ones not owned by another module live here.  See game.c
 * (g_tick_counter, g_lmb_handled, g_stage_table, …), script.c
 * (g_active_actor, g_perspective_*) and assets.c (g_persp_band_count).
 */
uint32_t  g_entity_state[0x11C];        /* DAT_00449D28 */
uint32_t  g_scene_snapshot[0x1E];       /* DAT_00443332 */
int16_t   g_persp_profile[0x22*2];      /* DAT_0044E5F8 */

/* g_next_cd_check removed — rule #7. */

/* T22 phase A/B staging globals removed in T42:
 *   - g_pending_komnata: replaced by in-place LoadKomnataScene call
 *   - g_komnata_loaded_by_op20: outer-loop guard no longer needed
 *     after play_first_scene_demo collapsed to single play_demo_scene
 *     call (T22 phase B). */

/* Scene transitions (ScriptGoToKomnata) → src/scene/navigation.c
 * Actor walking (ActorWalkToBlocking, ActorWalkBothBlocking) →
 *     src/scene/actor_walk.c
 * Stage descriptors (BuildStageTable, LoadActorWalkAnims, the
 *     g_stage_table/g_stage_va_table/g_actor_walk_anim globals,
 *     and g_default_world_state) → src/scene/stage.c */

/* LoadKomnata — 1:1 port of FUN_00402A50 @ 0x00402A50.
 *
 *   piVar2 = *DAT_0044A19C;                         // komnata table base
 *   walk entries (14 bytes each) until index == id
 *   if not found: DAT_0044E5DC = 0; return;
 *   DAT_0044E588 = id;                              // g_cur_komnata
 *   DAT_0044E448 = entry[+4];                        // flags
 *   palette fade-out (deferred — see comment below)
 *   FUN_00405F80() + FUN_00402DB0()                  // clear lists
 *   FUN_00402990(name)                               // name-keyed init
 *   FindScriptByStageAndRoom(scripts, etap, name)    // locate Wacky.scr section
 *   FUN_00409970(scripts)                            // parse [sampl] tags
 *   load pal_NN_NN.pal                               // per-komnata palette
 *   if (flags & 1) link kind=3 walk-behind initial entity
 *   if (flags & 2) link kind=2/3/4 default entities (cursor, krazek)
 *   RunScriptInterpreter(0x26, 0x26, entry[+6])      // enter script
 *   palette fade-in
 *   ProcessGameFrameTick × 2
 *   RunScriptInterpreter(0x26, 0x26, entry[+10])     // secondary script
 *
 * Most palette / FUN_0040xxxx side-effects are deferred — what matters
 * for our port is: locate name, clear lists, run enter_script. */
extern uint32_t g_stage_va;
extern void EntityListClearAll(void);
extern void VisibleMasksReset(void);
extern void ResetFrameSfxState(void);
extern const void *xlat_binary_ptr(uint32_t addr);

const char *LoadKomnata(uint16_t id)
{
    if (id == 0 || !g_stage_va) return NULL;
    /* Stage descriptor: +0 = komnata array base VA */
    extern const void *PeLoaderRead(uint32_t va);
    const uint8_t *sd = (const uint8_t *)PeLoaderRead(g_stage_va);
    if (!sd) return NULL;
    uint32_t komnata_arr_va = (uint32_t)(sd[0] | (sd[1] << 8) |
                                         (sd[2] << 16) | (sd[3] << 24));
    const uint8_t *karr = (const uint8_t *)PeLoaderRead(komnata_arr_va);
    if (!karr) return NULL;

    /* T105 fix — walk komnata table 1:1 with FUN_00402A50 @ 0x00402A55:
     *   piVar5 = table_base;  iVar6 = 0;
     *   do {
     *       if (*piVar5 == 0 && piVar5[1] == 0) { iVar6 = 0; bail; }
     *       ++iVar6;  piVar5 += 14;
     *   } while (iVar6 < param_1);
     *   ++iVar6;                              // post-loop inc
     *   idx = param_1 - 1;                    // 0-based entry to use
     *
     * Walk scans entries 0..(param_1-1), checking each for NULL+0
     * sentinel mid-walk. If hit, abort. Otherwise use index (param_1-1).
     *
     * Earlier port: `for (i<=idx+1 && i<16; ++i)` — bounded by fixed
     * 16-entry sanity cap. Stages 2-5 with >16 rooms would silently
     * fail to find them. */
    int idx = (int)id - 1;     /* 1-based → 0-based */
    int found = 0;
    for (int i = 0; i < (int)id; ++i) {
        uint32_t name_va = (uint32_t)(karr[i*14 + 0] | (karr[i*14 + 1] << 8) |
                                      (karr[i*14 + 2] << 16) | (karr[i*14 + 3] << 24));
        uint16_t flags   = (uint16_t)(karr[i*14 + 4] | (karr[i*14 + 5] << 8));
        if (!name_va && flags == 0) {             /* terminator */
            fprintf(stderr, "[load-komnata] terminator hit at i=%d, requested id=%u\n",
                    i, id);
            return NULL;
        }
        if (i == idx) { found = 1; /* keep walking to verify no terminator before idx */ }
    }
    if (!found) {
        fprintf(stderr, "[load-komnata] id=%u not in stage table\n", id);
        return NULL;
    }
    uint32_t name_va   = (uint32_t)(karr[idx*14 + 0] | (karr[idx*14 + 1] << 8) |
                                    (karr[idx*14 + 2] << 16) | (karr[idx*14 + 3] << 24));
    uint16_t flags     = (uint16_t)(karr[idx*14 + 4] | (karr[idx*14 + 5] << 8));
    uint32_t enter_va  = (uint32_t)(karr[idx*14 + 6] | (karr[idx*14 + 7] << 8) |
                                    (karr[idx*14 + 8] << 16) | (karr[idx*14 + 9] << 24));
    uint32_t second_va = (uint32_t)(karr[idx*14 + 10] | (karr[idx*14 + 11] << 8) |
                                    (karr[idx*14 + 12] << 16) | (karr[idx*14 + 13] << 24));
    const char *name = (const char *)PeLoaderRead(name_va);

    g_cur_komnata = id;
    g_stats.total_komnata_loads++;                /* T56 */
    fprintf(stderr, "[load-komnata] %u '%s' flags=0x%04X enter=0x%08X second=0x%08X\n",
            id, name ? name : "(null)", flags, enter_va, second_va);

    /* Clear lists — port mirror of FUN_00405F80 + FUN_00402DB0. */
    EntityListClearAll();
    VisibleMasksReset();
    ResetFrameSfxState();
    /* T132 — original FUN_00402DB0 calls FUN_00410D20 (sound queue reset)
     * as part of room reset. Without this, positional sources from the
     * previous komnata leak into the new one's aggregate pan. */
    SoundQueueReset();

    /* T107 — partial port of FUN_00402DB0 (room reset). The original
     * does a consolidated clear that we previously split across multiple
     * code paths; the bits below explicitly cover the gaps so non-op-0x2C
     * komnaty (= no explicit mask asset) don't carry stale state from the
     * previous room:
     *
     * 1. Perspective band count — original FUN_00402DB0 unconditionally
     *    `DAT_0044A200 = 0` then `= 4 if (flags & 2)`. Earlier port only
     *    reset it inside ScriptCallBgMaskSetup (which is called only when
     *    the room has a mask asset). Rooms without one inherited band
     *    count from the previous komnata.
     *
     * 2. Actor walker state — clear walk-remaining (+0x4C/+0x50) and
     *    walker-busy flag (+0x3A bit 0) on both g_actor[]. Op 0x15 path
     *    plant later re-sets them. Without this, an actor mid-walk when
     *    the player exited the room would resume walking at room entry.
     *
     * 3. Actor scale_pct — original sets `+0x16 = 100` (= 1.0×). Without
     *    this, an actor whose script set their scale in a previous room
     *    keeps that scale until UpdateActorMovement re-computes from
     *    anchor Y. Brief visual glitch on room entry.
     *
     * Cursor entity link (DAT_0045147C / DAT_00451480) is skipped — see
     * T106 (deferred). Click queue head, panel verb-tab init, label
     * strings — all done at engine boot, no need to repeat. */
    extern int g_persp_band_count;
    g_persp_band_count = (flags & 2) ? 4 : 0;
    /* Reset perspective globals (1:1 with FUN_00402DB0 top):
     *   DAT_0044a198 = 0x78;   // g_cursor_speed   = 120
     *   DAT_00449878 = 4;       // g_perspective_min  = 4
     *   DAT_0044987c = 7;       // g_perspective_step = 7
     * Without this an op 0x40 SET_PERSPECTIVE call from a prior komnata's
     * action cinematic (which biases perspective so ACTIVE actor scales
     * toward 0) persists into the next komnata. First UpdateActorMovement
     * tick after scene-load recomputes +0x58 with stale globals — actor
     * visibly jumps ~5% between T107 hardcoded 100 and stale-perspective
     * computed value. */
    extern uint16_t g_cursor_speed;
    extern uint16_t g_perspective_min;
    extern uint16_t g_perspective_step;
    g_cursor_speed     = 0x78;
    g_perspective_min  = 4;
    g_perspective_step = 7;
    extern Entity *g_actor[2];
    for (int i = 0; i < 2; ++i) {
        Entity *a = g_actor[i];
        if (!a) continue;
        uint8_t *eb = (uint8_t *)a;
        *(uint32_t *)(eb + 0x4C) = 0;       /* walk_dx_remaining */
        *(uint32_t *)(eb + 0x50) = 0;       /* walk_dy_remaining */
        *(uint16_t *)(eb + 0x3A) &= (uint16_t)~5u;   /* clear bits 0+2 */
        *(uint16_t *)(eb + 0x32) = 0;       /* pc */
        *(uint16_t *)(eb + 0x3C) = 0;       /* delay countdown */
        /* scale_pct lives in +0x58 in our entity layout (T3 walker port);
         * original was +0x16 in 32-bit struct. UpdateActorMovement
         * re-computes from anchor Y, so just reset to 100. */
        *(uint16_t *)(eb + 0x58) = 100;
    }

    /* Find this komnata's section in Wacky.scr + parse [sampl] tags —
     * 1:1 with FUN_00402A50's FindScriptByStageAndRoom + FUN_00409970
     * sequence. Replaces hand-transcribed g_frame_sfx[] table for any
     * asset mentioned in the parsed [komnata]N section. */
    extern void *g_scripts_obj;
    if (g_scripts_obj && name) {
        char etap_str[2] = { (char)('0' + g_cur_etap), 0 };
        if (FindScriptByStageAndRoom(g_scripts_obj, etap_str, name)) {
            /* ScriptObj.start / .end are private to script.c; expose
             * via accessor — see ScriptObjGetSection() decl. */
            extern const uint8_t *ScriptObjGetSectionStart(void *self);
            extern const uint8_t *ScriptObjGetSectionEnd  (void *self);
            ResetDynamicSfxTable();
            const uint8_t *ss = ScriptObjGetSectionStart(g_scripts_obj);
            const uint8_t *se = ScriptObjGetSectionEnd  (g_scripts_obj);
            if (ss && se) ParseSamplTagsForKomnata(ss, se);
        }
    }

    /* Panel page-swap — 1:1 with FUN_00402A50 @ 0x00402D76 calling
     * FUN_004071F0 right before enter_script. Loads page[0]'s 6 slots
     * into the panel verb table so the room starts with the inventory
     * front page on the bar. */
    g_settings_anim_active = flags;     /* T121: full u16 from komnata
                                         * entry[+4] (was truncated to u8). */
    PanelPageSwap();

    /* Run enter_script (1:1 with `RunScriptInterpreter(0x26, 0x26, ptr)`). */
    if (enter_va) {
        const uint8_t *bc = (const uint8_t *)xlat_binary_ptr(enter_va);
        if (bc) RunScriptInterpreter(0x26, 0x26, (uint8_t *)bc);
    }

    /* TWO frame ticks between enter_va and second_va — 1:1 with
     * FUN_00402A50:
     *   RunScriptInterpreter(enter_va);
     *   palette fade-in;
     *   ProcessGameFrameTick();        // <-- this
     *   ProcessGameFrameTick();        // <-- and this
     *   RunScriptInterpreter(second_va);
     *
     * These ticks let EntityRenderAll process one-shot BG-blit entities
     * (spawn flags = 0x0060 → flag-0x40/0x20 branch in FUN_00406040
     * paints the atlas to the backbuffer + clears 0x20 + FlushFrameToPrimary).
     * Without them, second_va's `op 0x31 destroy id=6` removes the BG
     * entity from the render list BEFORE the renderer ever saw it →
     * komnata 5 (magaz3j) renders the prior scene's framebuffer because
     * its real BG (magaz3c.wyc spawned with flags=0x60) was never blitted.
     *
     * Stage-1 komnaty have second_va = 0 so this ran as no-op previously;
     * stage 2 komnata 5 is the first to actually use second_va, which is
     * why the bug surfaced only here. */
    if (second_va) {
        extern void ProcessGameFrameTick(void);
        ProcessGameFrameTick();
        ProcessGameFrameTick();
        const uint8_t *bc = (const uint8_t *)xlat_binary_ptr(second_va);
        if (bc) RunScriptInterpreter(0x26, 0x26, (uint8_t *)bc);
    }

    return name;
}

/* ActorWalkToBlocking — 1:1 with op 0x10/0x11/0x12 wait-for-walk
 * loop from Ghidra @ RunScriptInterpreter 0x00407820 case 0x10:
 *
 *   if (actor[+0x22] != tx || actor[+0x24] != ty) {
 *     DAT_0044e6a4 = idx;                  // swap active to this actor
 *     DAT_0044e5ac = 1;                    // synthesize click pending
 *     DAT_0044e5a4 = 1;                    // synthesize walker-bind flag
 *     DAT_0044e570 = -1;                   // walk-target id reset
 *     actor[+0x4C] = 0; actor[+0x50] = 0;
 *     UpdateActorMovement(tx, ty);         // binds walker via standard path
 *     DAT_0044e6a4 = saved_active;
 *     do {
 *       DAT_0044e5ac = 0; DAT_0044e5a4 = 0;
 *       ProcessGameFrameTick();
 *     } while (actor[+0x4C] != 0 || actor[+0x50] != 0 || DAT_0044e570 != -1);
 *   }
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
 * inside FUN_00402A50 / LoadKomnata):
 *   bit 0 = panel visible (read by FUN_00407260 PanelHitTest)
 *   bit 1 = actors active (read by actor.c — UpdateActorMovement gate)
 *   bit 2 = link the kind=3/4 default entities (cursor + krazek)
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

/* FUN_0040F8D0 — GetFileVersionInfo() probe; portable build can't probe a
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

/* =========== scripts ==================================================== */
void *FindAnimationScript(void *scripts, const char *name)
{ (void)scripts; (void)name; return NULL; }
/* PlayActorAnimByPath — 1:1 port of the anim-bind tail of FUN_004061D0
 * (UpdateActorMovement @ 0x004061D0, lines ~1054-1064 of decompile):
 *
 *   uVar9 = FindKeyInTaggedTable(pcVar12, '\x15', -1);
 *   sVar11 = (short)uVar9;
 *   if (sVar11 != 0) {
 *       FUN_00402500(entity);                              // full reset
 *       entity[+0x2C] = pcVar12;                            // bind script
 *       pcVar12[uVar9*2 + 2] = psVar3[2];                   // patch target X
 *       pcVar12[uVar9*2 + 4] = psVar3[3];                   // patch target Y
 *       entity[+0x3A] |= 4;                                 // walker-active flag
 *   }
 *
 * FindKeyInTaggedTable returns idx in ushort units. The op 0x15 WALK_TO_X
 * lives at byte offset `idx*2` in the bytecode; its first operand (a0 = X)
 * starts at +2, second (a1 = Y) at +4. We write the click target into
 * those byte positions BEFORE binding so the per-entity VM walker sees
 * the fresh destination on its next tick.
 *
 * Patching IS the original mechanism — bytecode lives in writable PE
 * memory and gets clobbered each click. Different clicks overwrite the
 * same op 0x15. */
void  PlayActorAnimByPath(Entity *e, const char *path, int16_t x, int16_t y)
{
    if (!e || !path) return;
    uint16_t idx = FindKeyInTaggedTable(path, 0x15, -1);
    if (idx == 0) {
        /* No op 0x15 found → either not a walker script or already at
         * target (original treats this as "do nothing"). */
        return;
    }
    /* Patch in-place — PE image is malloc'd, so memory is writable. */
    uint8_t *bc = (uint8_t *)path;
    *(int16_t *)(bc + idx * 2 + 2) = x;
    *(int16_t *)(bc + idx * 2 + 4) = y;

    /* FUN_00402500 reset (same block used by op 0x0E / 0x0F / 0x33). */
    uint8_t *eb = (uint8_t *)e;
    *(uint16_t *)(eb + 0x3A) &= (uint16_t)~5u;
    *(uint16_t *)(eb + 0x38) = 0;
    *(uint16_t *)(eb + 0x36) = 0;
    *(uint16_t *)(eb + 0x34) = 0;
    *(uint16_t *)(eb + 0x32) = 0;
    *(uint16_t *)(eb + 0x3C) = 0;
    *(uint16_t *)(eb + 0x40) = 0;
    *(uint16_t *)(eb + 0x42) = 0;
    *(uint32_t *)(eb + 0x4C) = 0;
    *(uint32_t *)(eb + 0x50) = 0;

    /* Bind walker bytecode + set walker-active bit 4 of +0x3A. */
    extern uint32_t ent_ptr_intern(void *p);
    *(uint32_t *)(eb + 0x2C) = ent_ptr_intern((void *)path);
    *(uint16_t *)(eb + 0x30) = 0;                /* frame = 0 */
    eb[0x3A] |= 4;                                /* walker-active flag */

    /* PORT SHORTCUT (refer FUN_004012E0 case 0x15 first-plant branch):
     * Plant the walker path right here so +0x4C/+0x50 are non-zero before
     * play_demo_scene's main loop polls "walker done" (`wdx == 0 && wdy ==
     * 0`) for the pending-scene-exit case. Without this, the same frame
     * that calls BindActorWalker also passes the done-check (since
     * FUN_00402500 just zero'd them) → scene transitions fire on click
     * without the actor ever stepping toward the exit.
     *
     * Original engine doesn't hit this race: PGFT polls walker state
     * AFTER per-entity VM has had a tick (op 0x15 plants path on first
     * fire). Our port's scene-exit check sits in play_demo_scene's outer
     * loop and runs AFTER ProcessGameFrameTickInner — which now executes
     * the per-entity VM, but BindActorWalker runs INSIDE PGFT Inner (via
     * HandleSceneInput at the tail). Result: VM has already ticked this
     * frame BEFORE the walker was bound. Path stays unplanted until next
     * frame's VM tick → check fires too early.
     *
     * Planting here at bind time matches the post-tick state and removes
     * the race. Op 0x15's first-plant branch is gated on dxr/dyr == 0
     * (no path), so once we've planted, it just steps. */
    int16_t cx = *(int16_t *)(eb + 0x22);        /* anchor X (foot) */
    int16_t cy = *(int16_t *)(eb + 0x24);        /* anchor Y (foot) */
    int16_t sdx = (int16_t)(x - cx);
    int16_t sdy = (int16_t)(y - cy);
    int16_t adx = sdx < 0 ? (int16_t)-sdx : sdx;
    int16_t ady = sdy < 0 ? (int16_t)-sdy : sdy;
    int16_t maxlen = adx > ady ? adx : ady;
    int32_t inc_x = 0, inc_y = 0;
    if (maxlen) {
        inc_x = ((int32_t)(x - cx) * 0x10000) / maxlen;
        inc_y = ((int32_t)(y - cy) * 0x10000) / maxlen;
    }
    /* Shift via uint32 to avoid signed-shift UB when cx/cy < 0 (actor
     * off-screen left/top). Bit pattern is identical to original's
     * `param_1 << 0x10` @ FUN_00401150 on 2's complement; UBSan's
     * shift-out-of-bounds abort variant fires for cx=-80 (rob4 enters
     * komnata 5 from off-screen left). */
    *(int32_t *)(eb + 0x44) = (int32_t)((uint32_t)(uint16_t)cx << 16);
    *(int32_t *)(eb + 0x48) = (int32_t)((uint32_t)(uint16_t)cy << 16);
    *(int32_t *)(eb + 0x4C) = inc_x;
    *(int32_t *)(eb + 0x50) = inc_y;
    *(int16_t *)(eb + 0x54) = x;
    *(int16_t *)(eb + 0x56) = y;
    /* Clear bit 2 of +0x3A — op 0x15 plant branch does the same, signals
     * "path is planted, no need to re-plant on first tick". */
    *(uint16_t *)(eb + 0x3A) &= (uint16_t)~4u;
    (void)cx; (void)cy; (void)inc_x; (void)inc_y;
}
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
void DrawPlaceholderScreen(const char *wanted_file)
{
    if (wanted_file)
        fprintf(stderr, "[asset] missing: %s\n", wanted_file);
}
/* ScreenshotToBmpAutoIncrement — dump current shadow buffer + palette as
 * BMP file (wac00000.bmp, wac00001.bmp, ...). Press B in game to capture. */
void ScreenshotToBmpAutoIncrement(void)
{
    static int s_shot_idx = 0;
    static uint32_t s_last_shot_ms = 0;
    uint32_t now = SDL_GetTicks();
    if (now - s_last_shot_ms < 500) return;     /* debounce key repeat */
    s_last_shot_ms = now;
    if (!g_back_shadow) return;
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(
        g_back_shadow, g_screen_w, g_screen_h, 8, g_screen_w,
        SDL_PIXELFORMAT_INDEX8);
    if (!s) { fprintf(stderr, "[debug] screenshot: SDL_CreateSurface failed: %s\n", SDL_GetError()); return; }
    SDL_Color colors[256];
    for (int i = 0; i < 256; ++i) {
        colors[i].r = g_palette_rgb[i*3 + 0];
        colors[i].g = g_palette_rgb[i*3 + 1];
        colors[i].b = g_palette_rgb[i*3 + 2];
        colors[i].a = 255;
    }
    SDL_SetPaletteColors(s->format->palette, colors, 0, 256);
    char path[64];
    snprintf(path, sizeof path, "wac%05d.bmp", s_shot_idx++);
    if (SDL_SaveBMP(s, path) == 0)
        fprintf(stderr, "[debug] screenshot saved -> %s\n", path);
    else
        fprintf(stderr, "[debug] screenshot: SDL_SaveBMP(%s) failed: %s\n",
                path, SDL_GetError());
    SDL_FreeSurface(s);
}
void ScreenshotToPcxAutoIncrement(void) { ScreenshotToBmpAutoIncrement(); }
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
 * periodic timer whose ISR (FUN_00403E40) does `INC DAT_0044E454`. That
 * counter is sampled in FUN_004024D0 into DAT_0044E578 = (now - prev)
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
 *   case 0x48 (full fade): DAT_004549E0 = 0; DAT_00455000 = step;
 *                          load target into DAT_00451DC8;
 *                          zero DAT_00454A00 work buffer.
 *   case 0x49 (step):      if (progress < 100) {
 *                              progress += step;
 *                              FUN_004140e0(work, target, out, progress%);
 *                              FUN_00412d10(out, 0);   // install
 *                              return 0;
 *                          }
 *                          // else return previous value
 *   case 0x4A (instant?):  similar to 0x48 but without progress reset.
 *
 * FUN_004140e0 linearly interpolates each palette byte between source
 * (DAT_00454A00, snapshot of pal at fade start) and target
 * (DAT_00451DC8) by progress/100, writing result to DAT_00454D00.
 *
 * Port state mirrors:
 *   g_palette_rgb       = DAT_00454D00 / live pal (256 entries × 3 RGB)
 *   s_pal_fade_source   = DAT_00454A00 (snapshot at fade start)
 *   s_pal_fade_target   = DAT_00451DC8 (loaded by case 0x48)
 *   s_pal_fade_progress = DAT_004549E0 (0..100)
 *   s_pal_fade_step     = DAT_00455000 (per-step advance) */
extern uint8_t g_palette_rgb[256*3];

static uint8_t  s_pal_fade_source [256*3];
static uint8_t  s_pal_fade_target [256*3];
static int      s_pal_fade_progress = 100;   /* 100 = no fade in flight */
static int      s_pal_fade_step     = 4;
static int      s_pal_fade_active   = 0;

void ScriptCallPalLoad(uint16_t fade_step, uint32_t selector, int with_fade)
{
    fprintf(stderr, "[script] pal load sel=0x%x fade=%d step=%u\n",
            selector, with_fade, fade_step);

    /* Snapshot CURRENT palette as fade source (DAT_00454A00). */
    memcpy(s_pal_fade_source, g_palette_rgb, sizeof s_pal_fade_source);

    /* Load fade target (DAT_00451DC8). */
    if (selector == 0) {
        /* Target = current → fade to itself (no visible change). */
        memcpy(s_pal_fade_target, g_palette_rgb, sizeof s_pal_fade_target);
    } else if (selector == 1) {
        memset(s_pal_fade_target, 0xFF, sizeof s_pal_fade_target);  /* white */
    } else if (selector == 2) {
        memset(s_pal_fade_target, 0x00, sizeof s_pal_fade_target);  /* black */
    } else if (selector == 3) {
        memset(s_pal_fade_target, 0x80, sizeof s_pal_fade_target);  /* gray */
    } else {
        const char *name = (const char *)xlat_binary_ptr(selector);
        if (name && *name) {
            void *pal = NULL; uint32_t psz = 0;
            if (LoadFileFromDta(name, &pal, &psz) && pal) {
                size_t cpy = psz < sizeof s_pal_fade_target
                             ? psz : sizeof s_pal_fade_target;
                memcpy(s_pal_fade_target, pal, cpy);
                xfree(pal);
            } else {
                fprintf(stderr, "[script] pal-load '%s' missing\n", name);
                /* Failed load → target = current (no fade). */
                memcpy(s_pal_fade_target, g_palette_rgb, sizeof s_pal_fade_target);
            }
        }
    }
    s_pal_fade_progress = 0;
    s_pal_fade_step     = fade_step ? (int)fade_step : 4;
    s_pal_fade_active   = 1;
    /* with_fade == 0 (case 0x4A) → script wants snap-to-target. Apply
     * immediately so scripts that don't poll PalFadeStep still see the
     * target take effect. */
    if (!with_fade) {
        memcpy(g_palette_rgb, s_pal_fade_target, sizeof g_palette_rgb);
        InstallPalette(g_palette_rgb, 0);
        s_pal_fade_progress = 100;
        s_pal_fade_active   = 0;
    }
}

/* ScriptCallPalFadeStep — 1:1 with case 0x49:
 *   if (progress < 100) {
 *       progress += step;
 *       FUN_004140e0(source, target, work, progress);
 *       FUN_00412d10(work, 0);
 *       return 0;
 *   }
 *   return previous_value (= 1 when fade not active).
 *
 * Returns 1 when fade complete (poll-loop exit condition). */
int ScriptCallPalFadeStep(void)
{
    if (!s_pal_fade_active || s_pal_fade_progress >= 100) {
        return 1;
    }
    s_pal_fade_progress += s_pal_fade_step;
    if (s_pal_fade_progress > 100) s_pal_fade_progress = 100;
    /* Linear interpolation: out = source + (target - source) * progress / 100. */
    for (int i = 0; i < (int)sizeof g_palette_rgb; ++i) {
        int s = s_pal_fade_source[i];
        int t = s_pal_fade_target[i];
        g_palette_rgb[i] = (uint8_t)(s + ((t - s) * s_pal_fade_progress) / 100);
    }
    InstallPalette(g_palette_rgb, 0);
    if (s_pal_fade_progress >= 100) {
        s_pal_fade_active = 0;
        return 1;
    }
    return 0;
}

void ScriptCallDestroyEnt(uint16_t id, int also_unreg_asset);  /* fwd decl */

/* 1:1 with RunScriptInterpreter case 0x2c (BG mask setup, NOT a scene
 * transition):
 *
 *    FUN_004093e0(0, '\x01');                    // DESTROY id=0 (kinds 2/3/4)
 *    asset = LoadAssetFromDtaBase(name);
 *    if (asset) {
 *        RegisterEntityForUpdate(asset, 1, 0);   // kind=1 asset reg, id=0
 *        e = FUN_00405880(off_widths[0], off_heights[0], off_drawX[0],
 *                         off_drawY[0], pixel_ptrs[0]);
 *        if (e) {
 *            RegisterEntityForUpdate(e, 3, 0);   // kind=3 mask entity
 *            FUN_00405fe0(&DAT_0044e6b0, e, 1);  // link into mask list (tail)
 *        }
 *    }
 *
 * Used by enter_scripts to install the room's .fld walkable-area mask
 * (maluch.fld, klatka2.fld, kiosk.fld, plac.fld). The kind=3 mask entity
 * is consumed by click-region detection (mouse cursor over walkable
 * floor → free-walk, otherwise → exit hotspot or NPC interaction).
 *
 * Our port doesn't yet have the click-region detection wired to scripts,
 * but the DESTROY-id=0-then-load semantics still matter: without it,
 * each scene's mask asset stacks in the registration table and confuses
 * future FindUpdateRegistration(1, 0) lookups. So we do the destroy +
 * the load + the kind=1/3 registrations; the kind=3 entity itself is
 * skipped (it has no render path since masks are click-only). */
void ScriptCallBgMaskSetup(const char *name)
{
    extern void *FindUpdateRegistration(uint16_t kind, uint16_t id);
    extern void  UnregisterEntityForUpdate(uint16_t kind, uint16_t id);
    extern int   g_persp_band_count;                   /* DAT_0044A200 */
    extern uint16_t g_settings_anim_active;            /* DAT_0044E448 (T121) */
    /* DESTROY id=0 across kinds 2,3,4 + unregister kind=1 asset reg
     * (param_2='\x01' to original FUN_004093e0). */
    ScriptCallDestroyEnt(0, 1);
    /* Reset perspective band count — 1:1 with original case 0x2c (line 893):
     *   DAT_0044A200 = (DAT_0044E448 & 0xff02) << 1;
     * Without this reset, FILD-asset bands accumulate across scene
     * changes (LoadAssetFromDtaBase ADDS to band count, never clears).
     * The mask `& 0xff02` extracts bits 1 + 8-15 of komnata flags; the
     * <<1 shift moves them up by one. For komnata_flags=0x03 (panel +
     * actors) this evaluates to 0x04, providing a per-room baseline. */
    g_persp_band_count = (int)((g_settings_anim_active & 0xff02) << 1);
    if (!name) {
        fprintf(stderr, "[script] bg-mask-setup name=NULL\n");
        return;
    }
    AnimAsset *a = LoadAssetFromDtaBase(name);
    if (!a) {
        fprintf(stderr, "[script] bg-mask-setup '%s' FAILED\n", name);
        return;
    }
    RegisterEntityForUpdate((Entity *)a, 1, 0);

    /* Publish walk-area FLD globals from this asset's first frame.
     * The bg-mask-setup file IS the room's walkability bitmap in the
     * original (the same kind=1 asset id=0 backs both walk-behind
     * rendering and is_walkable_at look-ups). Previously only
     * LoadKomnataScene step 4 published these globals — synthesizing
     * the filename as `<pic-basename>.fld`. That works when pic and
     * fld share a basename (foto3.pic/foto3.fld, …) but breaks in
     * stage-2 komnata 5: pic = magaz3j.pic (a 1×1 palette stub), fld =
     * magaz3.fld (bez `j`). Synth name `magaz3j.fld` didn't exist →
     * walkability empty → every click "unreachable". Publishing here
     * makes the script-named FLD authoritative; step 4's synth load
     * skips when this already populated globals (see game.c). */
    if (a->frame_count > 0 && a->off_widths && a->off_heights &&
        a->off_drawX && a->off_drawY && a->pixel_ptrs && a->pixel_ptrs[0]) {
        extern const uint8_t *g_walk_fld_pixels;
        extern uint16_t g_walk_fld_w, g_walk_fld_h, g_walk_fld_stride;
        extern int16_t  g_walk_fld_ox, g_walk_fld_oy;
        g_walk_fld_pixels = a->pixel_ptrs[0];
        g_walk_fld_w      = a->off_widths [0];
        g_walk_fld_h      = a->off_heights[0];
        g_walk_fld_ox     = (int16_t)a->off_drawX[0];
        g_walk_fld_oy     = (int16_t)a->off_drawY[0];
        g_walk_fld_stride = (uint16_t)((g_walk_fld_w + 7) / 8);
        fprintf(stderr, "[fld] %s: %ux%u @ (%d,%d) stride=%u (via bg-mask-setup)\n",
                name, g_walk_fld_w, g_walk_fld_h, g_walk_fld_ox, g_walk_fld_oy,
                g_walk_fld_stride);
        /* Rebuild per-actor waypoint graphs — 1:1 with RunScriptInterpreter
         * op 0x2C tail @ 0x00408AA0 which calls FUN_00404600(g_actor_wp[0])
         * then REP MOVSD-copies actor 0 struct to actor 1. The graph holds
         * scene perspective bands (from FILD body) + pre-built edges
         * between them; BindActorWalker consults it via wp_find_path. */
        extern void ActorWaypointsSceneInit(int actor_idx);
        ActorWaypointsSceneInit(0);
        ActorWaypointsSceneInit(1);
    }

    /* Synthesize the kind=3 walk-behind entity — 1:1 with FUN_00402C46
     * (op 0x2C body):
     *   piVar22 = FUN_00405880(w[0], h[0], drawX[0], drawY[0], pixels[0]);
     *   RegisterEntityForUpdate(piVar22, 3, 0);
     *   FUN_00405fe0(&DAT_0044e6b0, piVar22, 1);
     *
     * The original links to DAT_0044E6B0 (walk-behind list) which is used
     * for foot-fall validation by FUN_00406510 (clamps walker into
     * non-obstacle areas). For visible occlusion of actors, we ADDITIONALLY
     * link the entity into the main render list (g_render_list_head) with
     * a foot_y at the bottom of the sprite — so when Z-sort runs, the
     * walk-behind entity renders ABOVE any actor whose foot_y is less
     * than the walk-behind's bottom edge.
     *
     * Asset visibility check (FUN_004076E0 logic): if asset->kind & 2 == 0
     * (= mask file, no visible pixels), the entity stays HIDDEN; only
     * kind=2/3 visible .wyc files get on-screen as walk-behind. */
    if (a->frame_count > 0 && a->off_widths && a->off_heights &&
        a->off_drawX && a->off_drawY && a->pixel_ptrs && a->pixel_ptrs[0])
    {
        uint16_t w  = a->off_widths [0];
        uint16_t h  = a->off_heights[0];
        int16_t  dx = (int16_t)a->off_drawX[0];
        int16_t  dy = (int16_t)a->off_drawY[0];
        Entity *wb = (Entity *)xmalloc(sizeof *wb);
        if (wb) {
            memset(wb, 0, sizeof *wb);
            uint8_t *eb = (uint8_t *)wb;
            /* T32 — walk-behind alpha-plane: if asset's flag_22 bit 0 is
             * set (alpha-plane source per wacki.h AnimAsset comment),
             * tag the entity with flag 0x100 so EntityRenderAll routes
             * to BlitAlphaScaled mode 2 instead of plain color-key blit.
             * This gives translucent edge rendering for walk-behind props
             * (semi-transparent foliage, glass etc.) — 1:1 with original
             * FUN_004076E0 logic that branches on asset[+0x16] bit 0. */
            uint16_t fl = 1;                                  /* visible */
            if (a->flag_22 & 1) fl |= 0x100;                  /* alpha-plane */
            *(uint16_t *)(eb + 0x08) = fl;
            *(int16_t  *)(eb + 0x0A) = dx;                    /* drawX */
            *(int16_t  *)(eb + 0x0C) = dy;                    /* drawY */
            *(uint16_t *)(eb + 0x0E) = w;                     /* width */
            *(uint16_t *)(eb + 0x10) = h;                     /* height */
            *(int16_t  *)(eb + 0x26) = (int16_t)(dy + h);     /* foot_y (Z-sort) */
            *(uint32_t *)(eb + 0x28) = ent_ptr_intern((void *)a);
            *(uint16_t *)(eb + 0x30) = 0;                     /* frame 0 */
            RegisterEntityForUpdate(wb, 3, 0);
            /* Only put into the main render list when the asset is a
             * visible .wyc (kind=2 or kind=3, not kind=0 mask). */
            if (a->kind != 0) {
                extern Entity *g_render_list_head;
                extern void    LinkEntityToList(Entity **head, Entity *e, int position);
                LinkEntityToList(&g_render_list_head, wb, 1);
            }
        }
    }

    fprintf(stderr, "[script] bg-mask-setup '%s' (asset id=0, kind=%u, frames=%u)\n",
            name, a->kind, a->frame_count);
}

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
extern void   *FindUpdateRegistration(uint16_t kind, uint16_t id);  /* FUN_00405D80 */
extern const void *xlat_binary_ptr(uint32_t);

/* ------------------------------------------------------------------------- *
 * SpawnActorEntity — 1:1 with op 0x30 SPAWN code path used for Ebek/Fjej.
 *
 * Original engine pre-spawns both actors at game start with their atlas
 * (ebek.wyc / fjej.wyc) bound and verb_id = 1/2 in the click payload, so
 *   - op 0x28 SET_ENTITY_XY id=1  → FUN_00404C30(1) finds Ebek's click
 *     entity → returns its owner render entity → moves it.
 *   - op 0x28 SET_ENTITY_XY id=2  → same for Fjej.
 *   - DispatchClickEvent + verb_table searches resolve actor verb_ids.
 *
 * Returns the spawned render entity (= g_actor[idx]). Owns:
 *   - kind=2 render entity registered (kind=2, id) in update table,
 *     linked to render list
 *   - kind=4 click entity (offset+8 = 1 in click list), bound to
 *     a tiny 1-entry verb table { count=1, verb_id }, linked to
 *     click list + registered (kind=4, id) in update table          */
extern Entity *AllocEntity(uint16_t w, uint16_t h, uint16_t kind, uint16_t flags);

Entity *SpawnActorEntity(uint16_t id, AnimAsset *atlas, uint16_t init_frame,
                         int16_t init_x, int16_t init_y)
{
    if (!atlas) return NULL;
    uint16_t w = atlas->max_w, h = atlas->max_h;
    /* alpha-plane flag = 0 (actors aren't alpha) */
    Entity *e = AllocEntity(w, h, 1, 0);
    if (!e) return NULL;
    uint8_t *eb = (uint8_t *)e;
    /* +0x08 flags1/flags2 — initialised by AllocEntity (kind/flags) */
    /* Set walk-with-perspective bit (flag 0x40 of +0x08 is for scaling,
     * flag 4 of +0x08 is "mirror") — actors are normally upright,
     * left-facing native, so just leave flags as default-set. */
    *(uint32_t *)(eb + 0x2C) = 0;                /* no per-entity script */
    *(uint32_t *)(eb + 0x28) = ent_ptr_intern((void *)atlas);
    *(uint16_t *)(eb + 0x30) = init_frame;
    *(uint16_t *)(eb + 0x22) = (uint16_t)init_x;  /* anchor X (foot) */
    *(uint16_t *)(eb + 0x24) = (uint16_t)init_y;  /* anchor Y (foot) */
    /* Set FLAG_2 (bit 1 of +0x3A) so op 0x28 + post-exec see "foot
     * anchor active" — without this, drawn position doesn't apply
     * frame's hot_x/y compensation. */
    *(uint8_t *)(eb + 0x3A) |= 2;
    /* Pre-compensate the drawn position so the actor renders correctly
     * on frame 0 BEFORE any walker tick fires. Mirrors the post-exec
     * foot-anchor compensation in ExecEntityScript:
     *   drawn = anchor + atlas->off_draw[frame]
     * (×1 path since flags & 0x400 / 4 are not set on actors at spawn). */
    int16_t hot_x = 0, hot_y = 0;
    if (atlas->off_drawX && init_frame < atlas->frame_count)
        hot_x = (int16_t)atlas->off_drawX[init_frame];
    if (atlas->off_drawY && init_frame < atlas->frame_count)
        hot_y = (int16_t)atlas->off_drawY[init_frame];
    *(int16_t *)(eb + 0x0A) = (int16_t)(init_x + hot_x);
    *(int16_t *)(eb + 0x0C) = (int16_t)(init_y + hot_y);
    /* foot_y for z-sort (= bottom edge of drawn sprite). */
    uint16_t sh = (atlas->off_heights && init_frame < atlas->frame_count)
                 ? atlas->off_heights[init_frame] : 0;
    *(int16_t *)(eb + 0x26) = (int16_t)(*(int16_t *)(eb + 0x0C) + (int16_t)sh);

    RegisterEntityForUpdate(e, 2, id);
    LinkEntityToList(&g_render_list_head, e, 0);

    /* Click entity (1:1 with FUN_004076E0 kind=4 payload allocation) —
     * stores a tiny verb table {count=1, verb_id=id} so FUN_00404C30
     * resolves the actor by its id. The verb table is per-actor static
     * memory; we use a static array indexed by id. */
    static uint16_t s_actor_verb_tab[8][2];   /* {count, verb_id} pairs */
    if (id < 8) {
        s_actor_verb_tab[id][0] = 1;          /* count */
        s_actor_verb_tab[id][1] = id;         /* verb_id at frame 0 */
    }
    /* Click payload — full Entity alloc (B31 safety, see RegMaskList). */
    Entity *m = (Entity *)xmalloc(sizeof *m);
    if (m) {
        memset(m, 0, sizeof *m);
        *(uint32_t *)((uint8_t *)m + 0x0e) =
            ent_ptr_intern((void *)(id < 8 ? s_actor_verb_tab[id] : NULL));
        *(uint32_t *)((uint8_t *)m + 0x0a) = ent_ptr_intern((void *)e);
        *(uint16_t *)((uint8_t *)m + 8) = 1;  /* click kind=1 (sprite) */
        *(uint16_t *)((uint8_t *)m + 0x12) = id;  /* cached verb_id */
        LinkEntityToList(&g_click_list_head, m, 0);
        RegisterEntityForUpdate(m, 4, id);
    }
    fprintf(stderr, "[actor] spawn id=%u atlas=%s frame=%u at (%d,%d)\n",
            id, atlas->name, init_frame, init_x, init_y);
    return e;
}

/* ---- 1:1 with opcode 0x30 SPAWN_ENTITY @ RunScriptInterpreter:
 *
 *   asset = FUN_00405D80(1, id)               // find loaded atlas by id
 *   if (asset) {
 *       flags = arg3   (16-bit operand)
 *       if (flags & 0x510) flags &= ~4
 *       w = asset->max_w; h = asset->max_h
 *       if (flags & 4) { w <<= 1; h <<= 1; }
 *       e = AllocEntity(w, h, 1, has_alpha_plane?)
 *       e->flags2 |= flags
 *       e->script_bytecode = arg2 (dword2)
 *       e->current_anim    = asset (dword?+0xa = piVar22[10])
 *       RegisterEntityForUpdate(e, 2, id)
 *       LinkEntityToList(&render_list_head, e, 0)
 *       if (click_payload) {
 *           m = alloc(0x14, 1)
 *           m->payload = arg1 (dword1)
 *           m->owner   = e
 *           m->flags   = 1
 *           LinkEntityToList(&click_list_head, m, 0)
 *           RegisterEntityForUpdate(m, 4, id)
 *       }
 *   }
 */
void ScriptCallSpawnEntity(uint16_t id, uint16_t flags,
                           uint32_t click_payload_addr,
                           uint32_t script_addr)
{
    AnimAsset *asset = (AnimAsset *)FindUpdateRegistration(1, id);
    if (!asset) {
        fprintf(stderr, "[script] spawn id=%u: no asset registered (skipping)\n", id);
        return;
    }
    if (flags & 0x510) flags &= ~4u;
    /* Alpha-plane gate — 1:1 with original case 0x30:
     *   if ((asset->flag_22 & 1) || (flags & 0x2000) || (flags & 4))
     *       alloc_flags = 1;
     * Earlier port used `asset->kind == 3` which collapsed bits 0 and 1
     * of the raw flag — would over-trigger alpha for visible-only assets
     * (kind=3 with flag bit 0 clear). */
    uint16_t alloc_flags = 0;
    if ((asset->flag_22 & 1) || (flags & 0x2000) || (flags & 4)) alloc_flags = 1;
    uint16_t w = asset->max_w, h = asset->max_h;
    if (flags & 4) { w = (uint16_t)(w << 1); h = (uint16_t)(h << 1); }
    Entity *e = AllocEntity(w, h, 1, alloc_flags);
    if (!e) return;
    /* 1:1 with the original op 0x30 SPAWN body:
     *   *(ushort *)(piVar22 + 2) = *(ushort *)(piVar22 + 2) | uVar29;
     *   if ((uVar29 & 0x800) != 0) *(byte *)(piVar22 + 8) = 1;
     *
     * `+ 2` on an int* in the original = byte offset 8 (the 16-bit flags).
     * `+ 8` on an int* = byte offset 0x20 (an unrelated state byte). */
    *(uint16_t *)((uint8_t *)e + 8) |= flags;
    if (flags & 0x800) ((uint8_t *)e)[0x20] = 1;
    /* Visibility is gated by EntityRenderAll directly: bit 0x80 = hidden
     * (op 0x3E), bit 0x2000 = "wait-for-enable" (alpha-plane spawn whose
     * per-entity script will fill its private pixel buffer). Neither is
     * forced here — the SPAWN simply OR's the script's flags as the
     * original does. */
    /* Bind script bytecode + asset via slot-ID intern table (Entity stores
     * 4-byte slot IDs at the original 32-bit pointer offsets; real C
     * pointers don't fit in 4 bytes on 64-bit).
     *
     * 1:1 with original case 0x30 (Ghidra line ~960):
     *   piVar22[0xb] = iVar3;   // entity[+0x2C] = script_addr
     *   piVar22[10]  = iVar17;  // entity[+0x28] = asset_ptr
     *
     * No frame / anchor pre-init — AllocEntity already zeroed the buffer,
     * and the per-entity script (bound at +0x2C) drives position via
     * op 0x07 SET_ANCHOR on its first tick. Earlier port pre-set
     * +0x22/+0x24 to drawX[0]/drawY[0] which DOUBLED the apparent offset
     * for static (script-less) entities — divergence from original. */
    extern const void *xlat_binary_ptr(uint32_t);
    const void *bc = xlat_binary_ptr(script_addr);
    *(uint32_t *)((uint8_t *)e + 0x2C) = bc ? ent_ptr_intern((void *)bc) : 0;
    *(uint32_t *)((uint8_t *)e + 0x28) = ent_ptr_intern((void *)asset);

    extern void RegisterEntityForUpdate(Entity *, uint16_t, uint16_t);
    extern void LinkEntityToList(Entity **, Entity *, int);
    extern Entity *g_render_list_head;
    extern Entity *g_click_list_head;
    RegisterEntityForUpdate(e, 2, id);
    LinkEntityToList(&g_render_list_head, e, 0);

    if (click_payload_addr) {
        /* Click payload — full Entity alloc (B31 safety). */
        Entity *m = (Entity *)xmalloc(sizeof *m);
        if (m) {
            memset(m, 0, sizeof *m);
            const void *payload = xlat_binary_ptr(click_payload_addr);
            *(uint32_t *)((uint8_t *)m + 0x0e) = payload ? ent_ptr_intern((void *)payload) : 0;
            *(uint32_t *)((uint8_t *)m + 0x0a) = ent_ptr_intern((void *)e);
            *(uint16_t *)((uint8_t *)m + 8) = 1;
            LinkEntityToList(&g_click_list_head, m, 0);
            RegisterEntityForUpdate(m, 4, id);
        }
    }
    fprintf(stderr, "[script] spawn id=%u asset=%p script=0x%08x flags=0x%04x → %p\n",
            id, (void *)asset, script_addr, flags, (void *)e);
}

/* ---- 1:1 with opcode 0x2d in RunScriptInterpreter:
 *    LoadAssetFromDtaBase(name) + RegisterEntityForUpdate(asset, kind=1, id)
 * The asset itself is the "table-look-up payload"; the actual entity is
 * spawned later by opcode 0x30 (which finds the asset via FUN_00405D80(1,id)).
 */
void ScriptCallLoadAsset(const char *name, uint16_t id)
{
    if (!name) return;
    /* 1:1 with FUN_00405A60 → FUN_00401010 → FUN_0040A150: if an asset
     * already occupies this slot, freeing it stops all wavs in its
     * SampleTable list (FUN_0040D460 per entry). We don't yet free
     * the AnimAsset itself (leak — TBD), but we MUST stop any looping
     * SFX it owned so they don't keep playing after the script switched
     * away. Without this, e.g. marsz.wav (rakieta.wyc (1,464) range)
     * keeps looping when zuw1b.wyc takes over the id=7 slot before
     * frame 464 is ticked. */
    extern void *FindUpdateRegistration(uint16_t kind, uint16_t id);
    AnimAsset *prev = (AnimAsset *)FindUpdateRegistration(1, id);
    if (prev && prev->name[0]) {
        extern void StopAllSfxForAsset(const char *asset_name);
        StopAllSfxForAsset(prev->name);
    }
    AnimAsset *a = LoadAssetFromDtaBase(name);
    if (!a) {
        fprintf(stderr, "[script] load-asset '%s' id=%u FAILED\n", name, id);
        return;
    }
    RegisterEntityForUpdate((Entity *)a, 1, id);
    fprintf(stderr, "[script] load-asset '%s' id=%u (kind=%u frames=%u)\n",
            name, id, a->kind, a->frame_count);
}

/* 1:1 with FUN_004093e0 — DESTROY entities matching (id) across kinds
 * 2, 3, 4 (render/click/mask). Originally invoked by opcodes 0x31 + 0x32:
 *
 *   case 0x31:  FUN_004093e0(id, '\x01');     // also unregisters kind 1
 *   case 0x32:  FUN_004093e0(id, '\0');
 *
 * The original walks the update table, finds each matching entity,
 * calls FUN_00406020 (unlink + free), then loops until no more match
 * (handles duplicate IDs across kinds). For our port the side-band
 * list keeps the canonical entity pointers — we unlink them and free
 * the storage. */
extern void UnlinkEntity(Entity *e);
void ScriptCallDestroyEnt(uint16_t id, int also_unreg_asset)
{
    extern void *FindUpdateRegistration(uint16_t kind, uint16_t id);
    int total_killed = 0;
    int total_skipped = 0;
    extern void UnregisterEntityForUpdate(uint16_t kind, uint16_t id);
    extern Entity *g_actor[2];
    if (also_unreg_asset) {
        /* Drop ONLY the OLDEST kind=1 asset registration matching id —
         * 1:1 with FUN_004093E0 prologue calling FUN_00405E70(id, 1, -1)
         * which scans from START and removes FIRST match. Critical when
         * script does `load id=N (asset_A)` + `load id=N (asset_B)` then
         * `destroy id=N (unreg=1)` — both entries exist, original drops
         * only asset_A leaving asset_B for subsequent spawn. Earlier port
         * removed ALL matches → followup spawn id=N silently fails ("no
         * asset registered"); manifests as missing assets after puzzle
         * state-change scripts (stage 4 LewyBrz liana state replacement,
         * stage X Y mask replacement, …). */
        extern int UnregisterFirstKindIdMatch(uint16_t kind, uint16_t id);
        UnregisterFirstKindIdMatch(1, id);
    }
    /* PORT SHORTCUT (Bug 3 fix #17 — refer FUN_004093E0): protect actor
     * entities + their click payloads from script destroy. Scripts collide
     * actor IDs (1=Ebek, 2=Fjej) with regular mask/asset IDs via `op
     * 0x2C/0x2D load-asset id=N`; e.g. kiosk21 enter_script loads
     * `kiosk21.msk` as id=2, then `op 0x31 DESTROY id=2` unloads the mask
     * AND (in original 1:1) would also wipe Fjej's click payload. We skip
     * the actor entries here so subsequent `op 0x0E SET_SCRIPT` /
     * `op 0x28 SET_ENTITY_XY` calls can still find Fjej via
     * FindEntityByVerbId. Original WACKI must store actor click payloads
     * in a separate ID space (TBD via deeper RE).
     *
     * Use FindUpdateRegistrationExcept to skip already-inspected protected
     * entries (otherwise the next find returns the same protected entry
     * forever — infinite loop). */
    extern void *FindUpdateRegistrationExcept(uint16_t k, uint16_t id,
                                              Entity *const *skip, int n);
    extern void *ent_ptr_resolve(uint32_t slot);
    extern int UnregisterEntityByPtr(Entity *e);
    for (uint16_t k = 2; k <= 4; ++k) {
        Entity *protected[8]; int nprot = 0;
        for (;;) {
            Entity *e = (Entity *)FindUpdateRegistrationExcept(k, id, protected, nprot);
            if (!e) break;
            int is_actor = (e == g_actor[0] || e == g_actor[1]);
            int is_actor_click = 0;
            if (!is_actor && k == 4 && (id == 1 || id == 2)) {
                uint32_t owner_slot = *(uint32_t *)((uint8_t *)e + 0x0a);
                if (owner_slot) {
                    void *owner = ent_ptr_resolve(owner_slot);
                    if (owner == g_actor[0] || owner == g_actor[1])
                        is_actor_click = 1;
                }
            }
            if (is_actor || is_actor_click) {
                if (nprot < 8) protected[nprot++] = e;
                ++total_skipped;
                continue;
            }
            /* CRITICAL: use UnregisterEntityByPtr (single entity by ptr) not
             * UnregisterEntityForUpdate(k, id) — the latter wipes ALL entries
             * matching kind+id, which would also remove the actor's click
             * payload (registered at same kind=4 id=2 as the script-spawned
             * mask click payload). Then per-actor protection becomes useless
             * because the actor was already wiped from update_table before
             * we got to check it. */
            UnregisterEntityByPtr(e);
            UnlinkEntity(e);
            xfree(e);
            ++total_killed;
        }
    }
    fprintf(stderr, "[script] destroy id=%u killed=%d skipped=%d (asset_unreg=%d)\n",
            id, total_killed, total_skipped, also_unreg_asset);
}
/* Legacy name kept for callers in game.c — now a destroy alias. The
 * `enable` argument maps to: 1 (op 0x31) = destroy + unreg asset,
 * 0 (op 0x32) = destroy only. */
void ScriptCallEnableEnt(uint16_t id, int enable)
{
    ScriptCallDestroyEnt(id, enable ? 1 : 0);
}

/* 1:1 with op 0x3E HIDE_ENTITY @ Ghidra case 0x3e:
 *   iVar20 = FUN_00404C30(verb_id);
 *   if (iVar20) *(byte *)(iVar20 + 8) |= 0x80;
 *
 * Earlier port iterated kinds 2/3/4 via FindUpdateRegistration — too
 * broad, would hide click-payload and walk-behind entries that the
 * original leaves alone. The proper lookup is FindEntityByVerbId
 * which returns the single owner render entity.
 *
 * PORT SHORTCUT (refer FUN_00404C30 + flag-set, original case 0x3E):
 * Original verb scripts call op 0x3E on BOTH controllable actors during
 * many action sequences:
 *   - active actor (the one doing the action): hide so a spawned
 *     action-overlay sprite shows cleanly at the actor's anchor
 *     without the idle pose double-rendering underneath
 *   - partner actor: also hide, to keep the cinematic frame focused
 *     on the action (original "stage clearing" convention)
 *
 * The partner-hide is the user-reported "druga postać znika a nie
 * powinna" bug — modern adventure-game UX expects the partner to stay
 * on screen at all times. The active-actor hide is REQUIRED — without
 * it the user sees both the idle actor sprite AND the spawned action
 * sprite overlapping ("double-character" artifact during verb actions
 * like pick-up / drink / push).
 *
 * Compromise: ALLOW hide on the active actor (g_active_actor side),
 * SUPPRESS hide on the partner. g_active_actor is set by RMB toggle
 * and by op 0x34's `that_arg` dispatch — so during a verb script it
 * already names the actor performing the action. The corresponding
 * op 0x3F SHOW_ENTITY clears the hide flag on the active side at the
 * end of the action; SHOW on the partner is a no-op (already visible). */
void ScriptCallHideEnt(uint16_t id)
{
    extern Entity *FindEntityByVerbId(uint16_t verb);
    extern Entity *g_actor[2];
    extern uint16_t g_active_actor;
    Entity *e = FindEntityByVerbId(id);
    if (!e) {
        fprintf(stderr, "[hide] id=0x%04X — no entity (active=%u)\n",
                id, (unsigned)g_active_actor);
        return;
    }
    int target_idx = (e == g_actor[0]) ? 0 : (e == g_actor[1]) ? 1 : -1;
    int partner_idx = (int)((g_active_actor & 1u) ^ 1u);
    int suppressed = (target_idx == partner_idx);
    fprintf(stderr, "[hide] id=0x%04X target=%s active=%s → %s\n",
            id,
            target_idx == 0 ? "Ebek" : target_idx == 1 ? "Fjej" : "non-actor",
            (g_active_actor & 1u) ? "Fjej" : "Ebek",
            suppressed ? "SUPPRESSED" : "HIDDEN");
    if (suppressed) return;
    ((uint8_t *)e)[8] |= 0x80;
}
/* 1:1 with op 0x3F SHOW_ENTITY @ Ghidra case 0x3f:
 *   iVar20 = FUN_00404C30(verb_id);
 *   if (iVar20) *(ushort *)(iVar20 + 8) &= 0xff7f;
 *
 * We additionally clear the "wait-for-enable" 0x2000 bit, which is set
 * on alpha-plane spawns: those entities should become visible the
 * moment a script flips them. */
void ScriptCallShowEnt(uint16_t id)
{
    extern Entity *FindEntityByVerbId(uint16_t verb);
    extern Entity *g_actor[2];
    extern uint16_t g_active_actor;
    Entity *e = FindEntityByVerbId(id);
    int target_idx = e ? ((e == g_actor[0]) ? 0 :
                          (e == g_actor[1]) ? 1 : -1) : -2;
    fprintf(stderr, "[show] id=0x%04X target=%s active=%s\n",
            id,
            target_idx == 0 ? "Ebek" : target_idx == 1 ? "Fjej" :
            target_idx == -1 ? "non-actor" : "no-entity",
            (g_active_actor & 1u) ? "Fjej" : "Ebek");
    if (e) {
        uint16_t *f = (uint16_t *)((uint8_t *)e + 8);
        *f &= (uint16_t)~0x2080u;
    }
}

/* ScriptCallWalkMode (op 0x35/0x37) — 1:1 port of FUN_00409270 @ 0x00409270.
 * Despite the legacy "WalkMode" name in our port, the original is a
 * SAVE-STATE call: snapshot one field of entity `id` into a 20-slot
 * table keyed by (id, mode). FUN_00409340 (WalkTo) restores from it.
 *
 *   if (save_count < 20) {
 *     find slot where (slot.id == id && slot.mode == mode), or append;
 *     e = FUN_00404C30(id);                       // by verb_id
 *     if (e) {
 *       if (mode == 1) slot.data = (e[+0x22], e[+0x24]);  // anchor pos
 *       if (mode == 2) slot.data = e[+0x28];               // asset slot
 *       slot.id = id; slot.mode = mode;
 *       if (was_append) ++save_count;
 *     }
 *   }                                                                   */
#define SAVE_SLOT_MAX 20
static struct {
    uint16_t id;
    uint16_t mode;
    uint32_t data;     /* mode 1: low=X high=Y, mode 2: asset slot */
} g_save_slots[SAVE_SLOT_MAX];
static int g_save_slot_count = 0;

void ScriptCallWalkMode(uint16_t id, int mode)
{
    extern Entity *FindEntityByVerbId(uint16_t verb_id);
    if (g_save_slot_count >= SAVE_SLOT_MAX) return;
    /* Find existing (id, mode) slot or append. */
    int slot = -1;
    for (int i = 0; i < g_save_slot_count; ++i) {
        if (g_save_slots[i].id == id && g_save_slots[i].mode == (uint16_t)mode) {
            slot = i; break;
        }
    }
    int appending = (slot < 0);
    if (slot < 0) slot = g_save_slot_count;
    Entity *e = FindEntityByVerbId(id);
    if (!e) return;
    uint8_t *eb = (uint8_t *)e;
    if (mode == 1) {
        uint16_t x = *(uint16_t *)(eb + 0x22);
        uint16_t y = *(uint16_t *)(eb + 0x24);
        g_save_slots[slot].data = ((uint32_t)y << 16) | x;
    } else if (mode == 2) {
        g_save_slots[slot].data = *(uint32_t *)(eb + 0x28);
    }
    g_save_slots[slot].id   = id;
    g_save_slots[slot].mode = (uint16_t)mode;
    if (appending) ++g_save_slot_count;
}

/* ScriptCallWalkTo (op 0x38/0x3A) — 1:1 port of FUN_00409340 @ 0x00409340.
 * RESTORE-STATE: searches the save table for (target_id, mode) slot,
 * then writes its data back into entity `verb_id`.
 *
 *   for slot in save_table[0..save_count]:
 *     if (slot.id == target_id && slot.mode == mode) {
 *       e = FUN_00404C30(verb_id);
 *       if (e) {
 *         if (mode == 1) { e[+0x22] = saved_X; e[+0x24] = saved_Y; }
 *         if (mode == 2) { FUN_00402500(e); e[+0x28] = saved_asset; }
 *       }
 *     }                                                                 */
void ScriptCallWalkTo(uint16_t verb_id, uint16_t target_id, int mode)
{
    extern Entity *FindEntityByVerbId(uint16_t verb);
    for (int i = 0; i < g_save_slot_count; ++i) {
        if (g_save_slots[i].id == target_id &&
            g_save_slots[i].mode == (uint16_t)mode)
        {
            Entity *e = FindEntityByVerbId(verb_id);
            if (!e) return;
            uint8_t *eb = (uint8_t *)e;
            if (mode == 1) {
                *(uint16_t *)(eb + 0x22) = (uint16_t)(g_save_slots[i].data & 0xFFFF);
                *(uint16_t *)(eb + 0x24) = (uint16_t)(g_save_slots[i].data >> 16);
            } else if (mode == 2) {
                /* FUN_00402500 reset on asset swap. */
                *(uint16_t *)(eb + 0x3A) &= (uint16_t)~5u;
                *(uint16_t *)(eb + 0x32) = 0;
                *(uint16_t *)(eb + 0x34) = 0;
                *(uint16_t *)(eb + 0x36) = 0;
                *(uint16_t *)(eb + 0x38) = 0;
                *(uint16_t *)(eb + 0x3C) = 0;
                *(uint16_t *)(eb + 0x40) = 0;
                *(uint16_t *)(eb + 0x42) = 0;
                *(uint32_t *)(eb + 0x4C) = 0;
                *(uint32_t *)(eb + 0x50) = 0;
                *(uint32_t *)(eb + 0x28) = g_save_slots[i].data;
            }
            return;
        }
    }
}

/* ScriptCallAttachProp — 1:1 port of op 0x3B / 0x3C @ 0x00408DFA / 0x00408E40.
 *
 *   case 0x3B: e = FindEntityByVerbId(reg_id);
 *              a = FindUpdateRegistration(1, prop);
 *              if (e && a) FUN_00407600(e, a);     // bind atlas
 *              if (e)      FUN_00402500(e);         // reset state
 *              break;
 *   case 0x3C: same as 0x3B but WITHOUT the reset (keep=1).
 *
 * FUN_00407600 (binder): writes new atlas ptr into entity[+0x28] and
 * resets the per-entity script pc + delay counters. FUN_00402500 is the
 * "reset state" inlined elsewhere — zero +0x32-0x3C, clear flags &~5. */
extern Entity *FindEntityByVerbId(uint16_t verb_id);
extern void   *FindUpdateRegistration(uint16_t kind, uint16_t id);
extern uint32_t ent_ptr_intern(void *p);

void ScriptCallAttachProp(uint16_t actor, uint16_t prop, int keep)
{
    Entity *e = FindEntityByVerbId(actor);
    if (!e) return;
    void   *a = FindUpdateRegistration(1, prop);   /* asset slot, kind=1 */
    if (a) {
        /* FUN_00407600 — re-bind atlas + reset per-script state:
         *   e[+0x28] = asset;
         *   e[+0x32] = 0;     // pc
         *   e[+0x36] = 0;     // delay
         *   e[+0x3C] = 0;     // sub-delay accumulator */
        uint8_t *eb = (uint8_t *)e;
        *(uint32_t *)(eb + 0x28) = ent_ptr_intern((void *)a);
        *(uint16_t *)(eb + 0x32) = 0;
        *(uint16_t *)(eb + 0x36) = 0;
        *(uint16_t *)(eb + 0x3C) = 0;
    }
    if (!keep) {
        /* FUN_00402500 — full reset (op 0x3B path). Mirrors the inline
         * reset used by op 0x1E / 0x23 / 0x33. */
        uint8_t *eb = (uint8_t *)e;
        *(uint16_t *)(eb + 0x3A) &= ~5u;
        *(uint16_t *)(eb + 0x38) = 0;
        *(uint16_t *)(eb + 0x36) = 0;
        *(uint16_t *)(eb + 0x34) = 0;
        *(uint16_t *)(eb + 0x3C) = 0;
        *(uint16_t *)(eb + 0x42) = 0;
        *(uint16_t *)(eb + 0x40) = 0;
        *(uint16_t *)(eb + 0x32) = 0;
        *(uint32_t *)(eb + 0x50) = 0;
        *(uint32_t *)(eb + 0x4C) = 0;
    }
    fprintf(stderr, "[script] attach-prop actor=0x%04X prop=0x%04X keep=%d %s\n",
            actor, prop, keep, a ? "ok" : "no-asset");
}

/* Most-recent SHOW_TEXT line for overlay rendering. Auto-dismiss
 * duration derived 1:1 from Ghidra op 0x09 (RunScriptInterpreter @
 * 0x00407820, case 9):
 *
 *   DAT_00455004 = (short)text_chars*10 - 0x7d20 + (lines+4)*0x19
 *
 * which is the wait-loop counter ticked down by DAT_0044E578 per
 * ProcessGameFrameTick. We mirror it as g_speech_dismiss_ticks. */
char     g_speech_text[256]      = {0};
uint16_t g_speech_actor          = 0;
uint32_t g_speech_tick           = 0;
uint16_t g_speech_dismiss_ticks  = 0;

/* Active speech balloon entity (kind=1 with manual pixel buffer).
 * NULL = no active balloon. EntityListClearAll resets via the same
 * path as g_actor[]. */
Entity *g_speech_balloon = NULL;

/* Speaker animation unbind state — mirrors the original op 0x09 epilogue
 * (RunScriptInterpreter case 9, line ~480 of FUN_00407820):
 *
 *   if (local_164 < dlg_count) {
 *       FUN_00401210(slot[local_164].speaker, slot[local_164].DATA);
 *   }
 *
 * The original re-binds the speaker entity to the dialog slot's `data`
 * bytecode (idle animation) AFTER the balloon dismisses. Without it the
 * speaker stays frozen in the "talking" frame.
 *
 * Port stores the bind args here at op 0x09 time; TickSpeechBalloon
 * applies them when the timer hits zero (= original wait-loop exit). */
uint16_t g_speech_unbind_speaker = 0;
uint32_t g_speech_unbind_data    = 0;

/* T117 — Polish-diacritic → Futura-glyph translation table (1:1 with
 * FUN_0040C740 init @ 0x0040C740). The original engine maps 18 CP-1250
 * Polish characters to custom Futura.30 glyph slots @ indices 0xC2..0xFB.
 * Identity for all other bytes. Source pairs lifted from
 * DAT_00445E40 (source CP-1250) + DAT_00445E58 (target Futura slot) in PE.
 *
 * Without this LUT, op 0x09 SHOW_TEXT would either:
 *   (a) render Polish chars as wrong glyphs (whatever lives at CP-1250
 *       codepoint in Futura.30 — possibly garbage), or
 *   (b) render them as blanks (if outside font's first..last_char range).
 *
 * Called once at engine boot (PreloadCommonAssets tail) so it's ready
 * before any op 0x09 fires. */
static uint8_t g_text_translation_lut[256];
static int     g_text_lut_built = 0;

void TextTranslationLutInit(void)
{
    if (g_text_lut_built) return;
    /* Identity mapping for all 256 bytes (1:1 z FUN_0040C740 head). */
    for (int i = 0; i < 256; ++i) g_text_translation_lut[i] = (uint8_t)i;
    /* Override 18 entries — Polish diacritics → Futura slots. Table
     * lifted byte-for-byte from PE: source @ 0x00445E40, target @
     * 0x00445E58 (each 18 bytes). */
    static const uint8_t src[18] = {
        0xA5, 0xC6, 0xCA, 0xA3, 0xD1, 0xD3, 0x8C, 0xAF, 0x8F,   /* Ą Ć Ę Ł Ń Ó Ś Ż Ź */
        0xB9, 0xE6, 0xEA, 0xB3, 0xF1, 0xF3, 0x9C, 0x9F, 0xBF    /* ą ć ę ł ń ó ś ź ż */
    };
    static const uint8_t dst[18] = {
        0xC2, 0xCA, 0xCB, 0xCE, 0xCF, 0xD3, 0xD4, 0xDB, 0xDA,
        0xE2, 0xEA, 0xEB, 0xEE, 0xEF, 0xF3, 0xF4, 0xFA, 0xFB
    };
    for (int i = 0; i < 18; ++i) g_text_translation_lut[src[i]] = dst[i];
    g_text_lut_built = 1;
}

/* T117 — translate input text via LUT into output buffer. 1:1 with
 * FUN_0040C780 @ 0x0040C780. Stops on NUL byte in TRANSLATED stream
 * (an override mapping a char to 0x00 would terminate early — the
 * Polish-diacritic LUT never does this so we're safe in practice). */
static void translate_script_text(const char *in, char *out, size_t out_sz)
{
    if (!in || !out || out_sz < 1) { if (out_sz) out[0] = 0; return; }
    TextTranslationLutInit();
    size_t n = 0;
    while (n + 1 < out_sz) {
        uint8_t c = (uint8_t)in[n];
        if (c == 0) break;
        uint8_t t = g_text_translation_lut[c];
        if (t == 0) break;       /* mirrors original early-out on translated NUL */
        out[n++] = (char)t;
    }
    out[n] = 0;
}

/* DEAD CODE IN SHIPPED GAME (verified empirically 2026-05-28).
 *
 * Op 0x09 SHOW_TEXT is fully implemented in WACKI.EXE (RunScriptInterpreter
 * case 9 @ 0x00407820) but no script in any of the 5 stages actually emits
 * the opcode. All in-game character dialogue goes through op 0x52/0x53
 * (DialogStackPush / DialogRunner reading [sampl] audio-only entries from
 * Gadki.scr — Polish voice acting, no subtitles).
 *
 * Likely a leftover from pre-dub development when text placeholders were
 * shown over speakers. Kept 1:1 for fidelity + in case any unreached
 * scripts in stages 4/5 actually fire it. If you ever see a "[say]" log
 * line, the kind=1 entity render path in EntityRenderAll will display it. */
void ScriptCallShowText(uint16_t actor, const char *text)
{
    extern uint32_t g_tick_counter;
    extern FontHandle *g_default_font;
    if (!text || !*text || !g_default_font) return;
    /* T103 — gate on g_subtitles_on (DAT_00455121). When disabled in
     * Solund menu, op 0x09 SHOW_TEXT no-ops (audio still plays via
     * separate path if voice_on is set; only the visible balloon is
     * suppressed). */
    if (!g_subtitles_on) {
        fprintf(stderr, "[say] suppressed (subtitles_on=0): %.60s\n", text);
        return;
    }
    /* T117 — translate raw CP-1250 input via Polish-diacritic LUT before
     * any layout/render. 1:1 with original op 0x09 dispatch calling
     * FUN_0040C780(text) → DAT_00475950 (translated buffer). */
    char translated[256];
    translate_script_text(text, translated, sizeof translated);
    text = translated;
    fprintf(stderr, "[say] actor=%u: %.120s\n", actor, text);

    /* 1:1 with op 0x09 (RunScriptInterpreter case 9):
     *
     *   1. FUN_0040C780 copies the text into work buffer (DAT_00475950).
     *   2. Split on '|' into up to 10 lines (local_d4[]).
     *   3. Measure each line width via FUN_004138B0(font, line),
     *      track max in uVar24, set total height = lines * font.advance.
     *   4. piVar22 = AllocEntity(maxW, totalH, kind=1, 1).
     *   5. Zero the pixel buffer.
     *   6. Set up render desc DAT_004549E8..:
     *        stride=maxW height=totalH font=DAT_0045500C color=0xFD
     *      and render each line centred at (maxW - line_w)/2.
     *   7. FUN_00404BC0(speaker_id) → speaker click entity.
     *      If kind=1/2: X = drawX + (w - text_w)/2, Y = drawY - text_h.
     *      Else: Y=0x50, X centred.
     *   8. Clamp X to [0, screen_w - text_w], Y to >= 0.
     *   9. FUN_00405FC0(DAT_0045147C, balloon) — link to render list.
     *  10. Wait loop ticks down DAT_00455004 by g_frame_delta_ms per
     *      ProcessGameFrameTick; exit when zero or DAT_0044E5AC click.
     *  11. Set balloon hidden bit (+0x09 & 0x80) on exit.                */

    /* --- 1: copy text + split on '|' --- */
    char buf[256];
    strncpy(buf, text, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    char *lines[10];
    int   line_count = 0;
    char *p = buf;
    while (line_count < 10) {
        lines[line_count++] = p;
        char *bar = strchr(p, '|');
        if (!bar) break;
        *bar = 0;
        p = bar + 1;
    }

    /* --- 2: measure widths via MeasureTextLine (= FUN_004138B0
     * 1:1 port — per-glyph width_tab + kern_tab sums). --- */
    int line_w[10] = {0};
    int max_w = 0;
    for (int i = 0; i < line_count; ++i) {
        line_w[i] = MeasureTextLine(g_default_font, (const uint8_t *)lines[i]);
        if (line_w[i] > max_w) max_w = line_w[i];
    }
    if (max_w < 32) max_w = 32;
    if (max_w > WACKI_SCREEN_W - 8) max_w = WACKI_SCREEN_W - 8;
    int total_h = line_count * 30;       /* Futura.30 advance ≈ 30 */
    if (total_h < 30) total_h = 30;

    /* --- 3: AllocEntity kind=1 with primary plane (group_flags=1) --- */
    /* Free any previous balloon — original op 0x09 keeps only one at
     * a time (the previous one is hidden via +0x09 |= 0x80 then GC'd). */
    if (g_speech_balloon) {
        UnlinkEntity(g_speech_balloon);
        if (g_speech_balloon->pixels) xfree(g_speech_balloon->pixels);
        xfree(g_speech_balloon);
        g_speech_balloon = NULL;
    }
    Entity *e = AllocEntity((uint16_t)max_w, (uint16_t)total_h, 1, 1);
    if (!e || !e->pixels) {
        if (e) { if (e->pixels) xfree(e->pixels); xfree(e); }
        return;
    }
    memset(e->pixels, 0, (size_t)max_w * (size_t)total_h);

    /* --- 4: render each line into the buffer ---
     * RenderTextLineToBuffer writes at td.pixels + stride*baseline.
     * Per-line Y advance via offsetting base pointer. Color 0xFD
     * (DAT_004549F8 from Ghidra).
     *
     * Native-pointer TextRenderTarget struct (replaces the original's
     * 32-bit uint32_t[5] descriptor) — see wacki.h. The legacy uint32_t
     * array truncated 64-bit pointers and crashed when malloc returned
     * addresses above the 4 GB boundary. */
    for (int i = 0; i < line_count; ++i) {
        int cx = (max_w - line_w[i]) / 2;
        if (cx < 0) cx = 0;
        TextRenderTarget td = {
            .stride     = (uint16_t)max_w,
            .x          = (uint16_t)cx,
            .color_base = 0xFD,
            .pixels     = e->pixels + (size_t)(i * 30) * max_w,
            .font       = g_default_font,
        };
        RenderTextLineToBuffer(&td, (const uint8_t *)lines[i]);
    }

    /* --- 5: position above speaker — 1:1 with case 9 FUN_00404BC0
     * lookup. Use FindEntityByVerbId; if found, position above its
     * draw rect. Else fallback: Y=0x50, X centred. */
    int bx, by;
    Entity *spk = FindEntityByVerbId(actor);
    if (spk) {
        uint8_t *sb = (uint8_t *)spk;
        int16_t sx_x = *(int16_t  *)(sb + 0x0A);
        int16_t sx_y = *(int16_t  *)(sb + 0x0C);
        uint16_t sx_w = *(uint16_t *)(sb + 0x0E);
        bx = (int)sx_x + ((int)sx_w - max_w) / 2;
        by = (int)sx_y - total_h;
    } else {
        bx = (WACKI_SCREEN_W - max_w) / 2;
        by = 0x50;
    }
    if (bx < 0) bx = 0;
    if (bx + max_w > WACKI_SCREEN_W) bx = WACKI_SCREEN_W - max_w;
    if (by < 0) by = 0;

    uint8_t *eb = (uint8_t *)e;
    *(int16_t *)(eb + 0x0A) = (int16_t)bx;
    *(int16_t *)(eb + 0x0C) = (int16_t)by;
    *(int16_t *)(eb + 0x22) = (int16_t)bx;          /* anchor mirror */
    *(int16_t *)(eb + 0x24) = (int16_t)by;
    /* foot_y at +0x26 = drawY + height for z-sort. */
    *(int16_t *)(eb + 0x26) = (int16_t)(by + total_h);

    /* --- 6: link to render list. --- */
    LinkEntityToList(&g_render_list_head, e, 0);
    g_speech_balloon = e;

    /* --- 7: dismissal timer (Ghidra: DAT_00455004 = chars*10 - 0x7D20
     * + (lines+4)*0x19; ticks down by g_frame_delta_ms each frame). --- */
    size_t n_chars = strlen(text);
    /* Dismiss-timer formula — 1:1 with Ghidra case 9:
     *
     *   DAT_00455004 = (short)pcVar5 * 10 + -0x7d20 + (lines+4) * 0x19;
     *
     * `pcVar5` is the NUL-terminator pointer (= buffer_start +
     * text_chars). The `(short)pcVar5 * 10 - 0x7D20` term resolves to
     * `text_chars * 10` IF the buffer starts at the original game's
     * fixed address `0x475950` (whose low word 0x5950 = 22864 satisfies
     * `22864 * 10 mod 65536 == 0x7D20`). That calibration cancels the
     * fixed buffer-base contribution leaving the char count.
     *
     * Our heap-allocated buffer doesn't match the original's address,
     * so we compute the semantic equivalent directly:
     *   dur (ms) = text_chars * 10 + (lines+4) * 25
     *
     * For 50 chars, 1 line: 500 + 125 = 625 ms.
     * For 200 chars, 2 lines: 2000 + 150 = 2150 ms.
     * Result is read by TickSpeechBalloon each frame and decremented by
     * g_frame_delta_ms (real ms) — when ≤ 0 the balloon is GC'd. */
    int dur = (int)n_chars * 10 + (line_count + 4) * 0x19;
    if (dur < 60)   dur = 60;       /* safety: ensure at least ~1s show */
    if (dur > 5000) dur = 5000;     /* cap absurdly long text at 5s */
    /* Keep legacy fields for the overlay-renderer transition window
     * — they'll be retired once the entity render path is the sole
     * draw site for kind=1 balloons. */
    g_speech_text[0]        = 0;             /* disable old overlay */
    g_speech_actor          = actor;
    g_speech_tick           = g_tick_counter;
    g_speech_dismiss_ticks  = (uint16_t)dur;
}

/* TickSpeechBalloon — drains the dismissal timer (Ghidra DAT_00455004
 * wait loop in case 9). Called once per ProcessGameFrameTick. */
void TickSpeechBalloon(void)
{
    extern uint32_t g_tick_counter;
    extern const void *xlat_binary_ptr(uint32_t);
    extern uint32_t ent_ptr_intern(void *);
    extern Entity *FindEntityByVerbId(uint16_t);
    if (!g_speech_balloon) return;
    if ((g_tick_counter - g_speech_tick) >= g_speech_dismiss_ticks) {
        /* Hide + unlink + free. Mirrors `balloon[+9] |= 0x80` then
         * normal entity GC; we GC explicitly since render list owns
         * the pointer. */
        UnlinkEntity(g_speech_balloon);
        if (g_speech_balloon->pixels) xfree(g_speech_balloon->pixels);
        xfree(g_speech_balloon);
        g_speech_balloon = NULL;

        /* Speaker animation UNBIND — 1:1 with original case 9 epilogue:
         *
         *   if (local_164 < dlg_count) {
         *       FUN_00401210(slot[local_164].speaker, slot[local_164].data);
         *   }
         *
         * Re-bind the speaker to the dlg_data idle-animation bytecode
         * so they return to neutral pose. Identical to op 0x0E body
         * (FUN_00401210). */
        if (g_speech_unbind_speaker != 0 && g_speech_unbind_data != 0) {
            Entity *sp = FindEntityByVerbId(g_speech_unbind_speaker);
            if (sp) {
                const void *bc = xlat_binary_ptr(g_speech_unbind_data);
                if (bc) {
                    uint8_t *eb = (uint8_t *)sp;
                    *(uint16_t *)(eb + 0x3A) &= (uint16_t)~5u;
                    *(uint16_t *)(eb + 0x38) = 0;
                    *(uint16_t *)(eb + 0x36) = 0;
                    *(uint16_t *)(eb + 0x34) = 0;
                    *(uint16_t *)(eb + 0x3C) = 0;
                    *(uint16_t *)(eb + 0x42) = 0;
                    *(uint16_t *)(eb + 0x40) = 0;
                    *(uint16_t *)(eb + 0x32) = 0;
                    *(uint32_t *)(eb + 0x50) = 0;
                    *(uint32_t *)(eb + 0x4C) = 0;
                    *(uint32_t *)(eb + 0x2C) = ent_ptr_intern((void *)bc);
                    *(uint16_t *)(eb + 0x30) = 0;
                }
            }
            g_speech_unbind_speaker = 0;
            g_speech_unbind_data    = 0;
        }
    }
}

/* ScriptCallDialogBegin — partial port of FUN_0040C6A0 (DialogPush) +
 * FUN_00409DD0 (DialogRunner) ops 0x52 / 0x53.
 *
 * Full original flow:
 *   op 0x52 = FUN_0040C6A0 — push dialog onto an internal stack with:
 *       slot.entity = speaker entity (looked up by reg_id)
 *       slot.atlas_backup = entity[+0x28]
 *       slot.script_backup = entity[+0x2C]
 *       slot.hash = horner-fold(opts bytes)
 *       slot.asset = LoadAssetFromDtaBase(dialog_name)
 *   op 0x53 = FUN_00409DD0 — run the interactive loop:
 *       FindSection(Gadki.scr, "[rozmowa]", dialog_name, "[animacja]")
 *       for each text line in section:
 *         allocate speech bubble, play audio, ProcessGameFrameTick wait
 *         while audio playing; process per-frame events; advance
 *       on user choice (panel click): return result_key
 *
 * Partial port goal: at least make the dialog NAME look up the section
 * in Gadki.scr and display the first text line as a speech balloon so
 * the user sees real dialog text instead of just the dialog name.
 *
 * The full interactive loop (with audio, options, multi-line scroll)
 * stays a stub until D2 is fully completed. */
extern int ScriptObjFindSection(void *self_, const char *tag,
                                const char *param, const char *altparam);
extern const uint8_t *ScriptObjGetSectionStart(void *self);
extern const uint8_t *ScriptObjGetSectionEnd  (void *self);

/* Extract the next [sampl] block from a [rozmowa]<name> section.
 *
 * Gadki.scr [rozmowa] sections have NO inline text — the audio file IS
 * the dialog content. Each line is structured as:
 *
 *   [sampl] fjgut04a.wav
 *   [fj] Optional spoken text (speaker = fj = Fjej)
 *   [sampl] fjgut04b.wav
 *   [eb] Another line (speaker = eb = Ebek)
 *   [nic]                                  ← silent / no text
 *
 * 1:1 with FUN_00409DD0 @ 0x00409DD0 outer loop, which:
 *   1. Finds next [sampl] tag
 *   2. Copies filename after it (skipping whitespace, until next ws)
 *   3. Plays the WAV
 *   4. Inside playback wait loop, scans forward for [eb]/[fj]/[nic]
 *      and renders a speech bubble per tag found
 *
 * Earlier port treated lines as inline text — bailed on the first '['
 * character. That's why `[dialog] section 'X' played 0 lines` —
 * the very first line of every section is `[sampl]`, instantly
 * terminating the scan.
 *
 * Returns 1 if a [sampl] block was extracted (out_wav populated; speaker
 * = 'e'/'f'/0 = none; out_text = matching speech bubble or empty).
 * Returns 0 at end of section. */
static int dialog_extract_line(const uint8_t **pss, const uint8_t *se,
                               char *out_wav, size_t out_wav_sz,
                               char *out_text, size_t out_text_sz,
                               char *out_speaker)
{
    const uint8_t *ss = *pss;
    if (out_wav)     out_wav[0] = 0;
    if (out_text)    out_text[0] = 0;
    if (out_speaker) *out_speaker = 0;

    /* ---- find next [sampl] tag --------------------------------------- */
    const char *sampl_tag = "[sampl]";
    const size_t sampl_len = 7;
    const uint8_t *sampl_at = NULL;
    while (ss + sampl_len <= se) {
        if (ss[0] == '[' && memcmp(ss, sampl_tag, sampl_len) == 0) {
            sampl_at = ss;
            break;
        }
        ++ss;
    }
    if (!sampl_at) { *pss = se; return 0; }

    /* Copy WAV filename right after [sampl] — skip ws, take until ws/EOL. */
    const uint8_t *p = sampl_at + sampl_len;
    while (p < se && (*p == ' ' || *p == '\t')) ++p;
    size_t n = 0;
    while (p < se && *p != ' ' && *p != '\t' &&
           *p != '\r' && *p != '\n' && n + 1 < out_wav_sz) {
        out_wav[n++] = (char)*p++;
    }
    out_wav[n] = 0;

    /* Lowercase for DTA lookup case-insensitivity (DTA names live as
     * sent in archive; the archive lookup is case-sensitive but assets
     * are stored lowercase. Original GetFileBySection does its own
     * normalisation; we mirror just enough of it for the .wav path). */
    for (size_t i = 0; i < n; ++i) {
        if (out_wav[i] >= 'A' && out_wav[i] <= 'Z') out_wav[i] += 32;
    }

    /* ---- find next speaker tag ([eb] / [fj] / [nic]) up to next [sampl] - */
    const uint8_t *block_end = se;
    {
        const uint8_t *q = p;
        while (q + sampl_len <= se) {
            if (q[0] == '[' && memcmp(q, sampl_tag, sampl_len) == 0) {
                block_end = q;
                break;
            }
            ++q;
        }
    }

    while (p < block_end) {
        /* Skip ws / newlines between tags. */
        while (p < block_end && (*p == ' ' || *p == '\t' ||
                                 *p == '\r' || *p == '\n')) ++p;
        if (p >= block_end) break;
        if (*p != '[') { ++p; continue; }

        /* Match [eb] / [fj] / [nic] */
        char sp = 0;
        size_t tag_len = 0;
        if (p + 4 <= block_end && memcmp(p, "[eb]", 4) == 0) {
            sp = 'e'; tag_len = 4;
        } else if (p + 4 <= block_end && memcmp(p, "[fj]", 4) == 0) {
            sp = 'f'; tag_len = 4;
        } else if (p + 5 <= block_end && memcmp(p, "[nic]", 5) == 0) {
            /* Silent block — no speaker, no text. Consume tag + bail. */
            p += 5;
            *pss = block_end;
            return 1;
        } else {
            /* Some other tag — skip it. */
            while (p < block_end && *p != ']') ++p;
            if (p < block_end) ++p;
            continue;
        }

        p += tag_len;
        /* Skip ws after the speaker tag. */
        while (p < block_end && (*p == ' ' || *p == '\t')) ++p;
        /* Copy text until EOL (or next tag). */
        size_t tn = 0;
        while (p < block_end && *p != '\r' && *p != '\n' &&
               *p != '[' && tn + 1 < out_text_sz) {
            out_text[tn++] = (char)*p++;
        }
        while (tn > 0 && (out_text[tn-1] == ' ' || out_text[tn-1] == '\t'))
            --tn;
        out_text[tn] = 0;
        if (out_speaker) *out_speaker = sp;
        break;
    }

    *pss = block_end;
    return 1;
}


/* Play a single dialog line as a speech bubble, then wait for it to
 * dismiss before returning. Mirrors the original per-line loop inside
 * FUN_00409DD0 (which also pumps audio + timer until the line is done). */
extern void ScriptCallShowText(uint16_t actor, const char *text);

/* dialog_make_audio_name — retired in T20b. The WAV name is now taken
 * straight from the [sampl] tag inside Gadki.scr (see dialog_extract_line),
 * not built from `<section>_idx.wav` which was a guess. Names like
 * `fj_gut04` → wav `fjgut04a.wav` don't follow the old pattern. */

static void dialog_play_line_indexed(uint16_t actor, const char *text,
                                     const char *dialog_base, int line_idx);

/* T20: retired — no producer after dialog_play_section_lines stopped
 * using placeholder fallback. Kept under #if 0 in case interactive
 * fallback comes back. */
#if 0
static void dialog_play_line(uint16_t actor, const char *text)
{
    dialog_play_line_indexed(actor, text, NULL, 0);
}
#endif

/* dialog_play_line_indexed — T6 / T20b: dialog with per-line audio.
 *
 * `wav_name` is the .wav filename extracted from a [sampl] tag (1:1 with
 * FUN_00409DD0 — it copies the name into DAT_004550b8 then calls
 * FUN_0040a110 to load + play). NULL or empty = silent line (text-only,
 * matches the [nic] tag case). The earlier port built names from
 * `<section_name><idx>.wav` which was an unverified guess — actual
 * WAV filenames in Gadki.scr [sampl] tags don't match that convention
 * (e.g. section `fj_gut04` → wav `fjgut04a.wav`).
 *
 * `text` can be empty for silent ([nic]) lines or [sampl]-only entries.
 * If both are absent, the function returns without action.
 *
 * Wait loop polls both: text dismiss-tick AND dialog audio playing flag. */
/* Forward decls — definitions follow further down (next to the
 * DialogStackPush/Pop block, which they share state with). */
static void DialogActivateTopSpeaker(void);
static void DialogRestoreTopSpeaker(void);
/* T108 — DialogTickMouth retired; mouth anim now driven by per-entity VM
 * via talk_anim bytecode bound in DialogActivateTopSpeaker. */

static void dialog_play_line_indexed(uint16_t actor, const char *text,
                                     const char *wav_name, int line_idx)
{
    /* Either we have audio, or we have text, or both — silent + textless
     * means there's nothing to do (this happens for orphan [sampl] entries
     * that point at missing files plus an empty bubble). */
    int has_text = text && *text;
    int has_audio = wav_name && *wav_name;
    if (!has_text && !has_audio) return;
    (void)line_idx;

    if (has_text) ScriptCallShowText(actor, text);

    int audio_started = 0;
    if (has_audio) {
        uint32_t len = PlayDialogLine(wav_name);
        if (len > 0) audio_started = 1;
    }

    /* T108 — bind the speaker entity to dialog atlas + mouth-cycle
     * bytecode via DialogActivateTopSpeaker. Per-entity VM
     * (ExecEntityScript) advances frames natively from the bytecode
     * each tick — no manual DialogTickMouth needed any more. On line
     * end DialogRestoreTopSpeaker swaps back to the actor's pre-dialog
     * idle anim. */
    DialogActivateTopSpeaker();

    extern Entity *g_speech_balloon;
    extern uint8_t g_lmb_clicked;
    extern uint16_t g_speech_dismiss_ticks;
    /* When audio is playing, allow up to ~30s safety (longer than any
     * realistic dialog line). Without audio, ~10s. Without text bubble
     * the loop simply waits on audio completion. We arm a grace period
     * (~5 frames ≈ 165 ms) before checking IsDialogLinePlaying so the
     * mixer has time to actually enqueue the WAV — without this the
     * audio-only path can break out on frame 0 because PlayDialogLine
     * returns synchronously but the audio thread hasn't transitioned
     * the channel into the "playing" state yet. */
    int safety = audio_started ? 1800 : 600;
    int audio_grace = audio_started ? 5 : 0;
    while (safety-- > 0) {
        if (has_text && !g_speech_balloon) break;
        if (!has_text && audio_grace == 0 && !IsDialogLinePlaying()) break;
        if (audio_grace > 0) --audio_grace;
        /* T108 — mouth animation is now bytecode-driven. The per-entity
         * VM tick (called inside ProcessGameFrameTick → EntityWalkerTick →
         * ExecEntityScript) cycles frames per the bound talk_anim
         * bytecode automatically. No manual DialogTickMouth needed. */
        ProcessGameFrameTick();
        extern int PlatformShouldQuit(void);
        if (PlatformShouldQuit()) break;
        if (g_game_over_code) break;
        if (g_lmb_clicked) {
            g_lmb_clicked = 0;
            g_speech_dismiss_ticks = 0;
            StopDialogLine();              /* T6: cancel mid-line speech */
            ProcessGameFrameTick();
            break;
        }
        /* T6 lip-sync: if audio is the timing source, wait until BOTH
         * (a) audio finishes AND (b) text dismiss-timer expires.
         * If audio finished but text still has time, that's fine —
         * the loop naturally exits when text expires. */
        if (audio_started && !IsDialogLinePlaying()) {
            /* Audio done. Fast-forward text dismiss so the line snaps
             * away on next tick (1:1 with original behavior where
             * FUN_0040A400 progress-poll returns 0 then text clears). */
            g_speech_dismiss_ticks = 0;
            ProcessGameFrameTick();
            break;
        }
        /* Throttle to ~60 fps so the loop doesn't spin CPU + so the
         * safety iteration counter matches a realistic time budget. */
        SDL_Delay(33);  /* T-anim-speed: match main loop pacing */
    }
    /* T20c — line over; flip atlas back to the speaker's original pose
     * so they don't stay frozen in talking-head form between lines. */
    DialogRestoreTopSpeaker();
}

/* T20b — Activate / restore the speaker's dialog atlas during line
 * playback. Original FUN_0040C570 toggles entity[+0x28] (atlas) and
 * entity[+0x2C] (bytecode) per-slot:
 *   ACTIVATE (mode 1): atlas → slot.asset (e.g. fjgadap.wyc),
 *                      bytecode → slot.talk_anim_va (mouth-cycling
 *                                                    bytecode ptr)
 *   RESTORE (mode 0):  atlas → slot.atlas_backup,
 *                      bytecode → slot.bytecode_backup
 *
 * Port shortcut: we don't read the original mouth-cycling bytecode
 * (param_4 of op 0x52) — that requires deeper RE of the slot[+0x0C]
 * pointer layout. Instead we:
 *   - Swap atlas only (visible "talking pose"). Entity VM continues
 *     running the backed-up bytecode, which references frame indices
 *     that may not all exist in the dialog atlas. We CLAMP frame
 *     index to atlas->frame_count in the renderer (existing safety
 *     net), so the worst case is a static talking face.
 *   - Drive mouth open/closed frame cycling manually: toggle frame 0
 *     ↔ 1 (or 0↔frame_count-1 if 2-frame atlas) every ~150 ms while
 *     audio is playing. Matches the visual cadence the original gets
 *     from its mouth-cycle bytecode. */
/* Definitions of DialogActivateTopSpeaker / DialogRestoreTopSpeaker /
 * DialogTickMouth moved AFTER the DialogStackSlot type and
 * s_dialog_stack array decl below — they need that struct visible. */

/* T21 — DialogPush/Pop stack (1:1 with FUN_0040C6A0 + FUN_0040C500).
 *
 * Original layout: each pushed slot is 0x18 bytes inside a dynamically
 * grown vector (FUN_00407570 grow helper). Layout per slot:
 *   +0x00  entity ptr           — speaker entity
 *   +0x04  opts_hash            — horner-folded opts bytes (used by some
 *                                  internal lookup; unused in our partial
 *                                  port but stored for fidelity)
 *   +0x08  loaded asset ptr     — LoadAssetFromDtaBase(dialog_name)
 *   +0x0C  opts count           — number of choice entries
 *   +0x10  entity[+0x28] backup — atlas at push time
 *   +0x14  entity[+0x2C] backup — bytecode at push time
 *
 * On pop (FUN_0040C500), for each pushed slot, in iteration order:
 *   - Restore entity[+0x28] to backup via FUN_00407600 (which also
 *     reallocs entity bitmap if the restored atlas needs more space).
 *   - Run FUN_00402500 walker-state reset on entity.
 *   - Clear entity[+0x30] (script pc) to 0.
 *   - Restore entity[+0x2C] from backup.
 *   - Free the loaded asset (FUN_00405A60 = FreeAsset).
 *
 * Original dialog stack lives in RunScriptInterpreter's local_15c (per-
 * invocation). Our port uses a process-global stack since
 * ScriptCallDialogBegin and ScriptCallDialogEnd are split across
 * RSI calls (the port's runner sequencer is more linear). Nested
 * dialogs ARE supported (push twice, pop pops the top one). */
typedef struct DialogStackSlot {
    Entity   *entity;            /* speaker entity (NULL if not resolved) */
    AnimAsset *asset;            /* loaded dialog asset (FreeAsset on pop) */
    uint32_t  opts_hash;         /* horner fold of opts bytes — port fidelity */
    uint32_t  talk_anim_va;      /* T108: PE VA of mouth-cycle bytecode
                                  * (= op 0x52's 4th arg / slot+0x0C in
                                  * the original 0x18-byte slot). Bound to
                                  * entity[+0x2C] by ACTIVATE, restored
                                  * from bytecode_backup by RESTORE. Earlier
                                  * field was misnamed `opts_count`. */
    uint32_t  atlas_backup;      /* entity[+0x28] at push time (intern slot) */
    uint32_t  bytecode_backup;   /* entity[+0x2C] at push time (intern slot) */
} DialogStackSlot;

#define DIALOG_STACK_MAX 8
static DialogStackSlot s_dialog_stack[DIALOG_STACK_MAX];
static int             s_dialog_stack_n = 0;

/* T108 — Dialog ACTIVATE/RESTORE helpers (1:1 with FUN_0040C570 @ 0x0040C570).
 * Per Ghidra:
 *
 *   void FUN_0040C570(stack, int mode) {
 *       for each slot in stack:
 *           if (mode == 0) {  // RESTORE
 *               FUN_00407600(entity, slot[+0x10]);   // restore atlas_backup
 *               FUN_00402500(entity);                 // walker reset
 *               entity[+0x2C] = slot[+0x14];          // restore bytecode_backup
 *           } else {            // ACTIVATE
 *               FUN_00407600(entity, slot[+0x08]);   // bind dialog asset
 *               FUN_00402500(entity);                 // walker reset
 *               entity[+0x2C] = slot[+0x0C];          // bind talk_anim bytecode
 *           }
 *           entity[+0x30] = 0;                        // reset frame pc
 *   }
 *
 * Earlier port (T20c) only swapped the atlas and drove a hardcoded
 * frame 0↔1 toggle via DialogTickMouth — that was a port shortcut
 * needed because we didn't yet have the bytecode pointer (op 0x52's
 * 4th arg, threaded through in T108). Now with talk_anim_va resolved
 * via xlat_binary_ptr we can bind real bytecode; the per-entity VM
 * cycles the mouth frames naturally via its standard frame-advance
 * timer, matching original cadence + animation curve exactly.
 *
 * DialogTickMouth retired — bytecode drives the animation now. */
extern uint32_t ent_ptr_intern(void *);
extern const void *xlat_binary_ptr(uint32_t addr);

static void DialogActivateTopSpeaker(void)
{
    if (s_dialog_stack_n == 0) return;
    DialogStackSlot *slot = &s_dialog_stack[s_dialog_stack_n - 1];
    Entity *e = slot->entity;
    if (!e || !slot->asset) return;
    uint8_t *eb = (uint8_t *)e;
    /* Atlas: bind loaded dialog asset (= slot[+0x08]). */
    *(uint32_t *)(eb + 0x28) = ent_ptr_intern(slot->asset);
    /* Walker reset — 1:1 with FUN_00402500 partial reset (clears walker
     * busy + delays so the new bytecode runs fresh from pc=0). */
    *(uint16_t *)(eb + 0x3A) &= (uint16_t)~5u;
    *(uint16_t *)(eb + 0x38) = 0;
    *(uint16_t *)(eb + 0x36) = 0;
    *(uint16_t *)(eb + 0x34) = 0;
    *(uint16_t *)(eb + 0x3C) = 0;
    *(uint16_t *)(eb + 0x42) = 0;
    *(uint16_t *)(eb + 0x40) = 0;
    *(uint16_t *)(eb + 0x32) = 0;
    *(uint32_t *)(eb + 0x50) = 0;
    *(uint32_t *)(eb + 0x4C) = 0;
    /* Bytecode: bind talk_anim_va — the mouth-cycle program from PE.
     * Per-entity VM (FUN_004012E0 / ExecEntityScript) advances frames
     * automatically per its op 0x06/op 0x08/etc. instructions. */
    if (slot->talk_anim_va) {
        const void *bc = xlat_binary_ptr(slot->talk_anim_va);
        if (bc) *(uint32_t *)(eb + 0x2C) = ent_ptr_intern((void *)bc);
    }
    /* Reset frame pc — VM starts from frame 0. */
    *(uint16_t *)(eb + 0x30) = 0;
}

static void DialogRestoreTopSpeaker(void)
{
    if (s_dialog_stack_n == 0) return;
    DialogStackSlot *slot = &s_dialog_stack[s_dialog_stack_n - 1];
    Entity *e = slot->entity;
    if (!e) return;
    uint8_t *eb = (uint8_t *)e;
    /* Restore atlas + bytecode from backups taken at push time. */
    *(uint32_t *)(eb + 0x28) = slot->atlas_backup;
    /* Walker reset (1:1 FUN_00402500 — same as ACTIVATE path). */
    *(uint16_t *)(eb + 0x3A) &= (uint16_t)~5u;
    *(uint16_t *)(eb + 0x38) = 0;
    *(uint16_t *)(eb + 0x36) = 0;
    *(uint16_t *)(eb + 0x34) = 0;
    *(uint16_t *)(eb + 0x3C) = 0;
    *(uint16_t *)(eb + 0x42) = 0;
    *(uint16_t *)(eb + 0x40) = 0;
    *(uint16_t *)(eb + 0x32) = 0;
    *(uint32_t *)(eb + 0x50) = 0;
    *(uint32_t *)(eb + 0x4C) = 0;
    *(uint32_t *)(eb + 0x2C) = slot->bytecode_backup;
    *(uint16_t *)(eb + 0x30) = 0;
}

/* T108 — DialogTickMouth retired entirely. The T20c port shortcut
 * (manual frame 0↔1 toggle every 150 ms) is no longer needed: per-entity
 * VM cycles mouth frames natively via the talk_anim bytecode bound by
 * DialogActivateTopSpeaker. Function deleted; remove the forward decl
 * up near dialog_play_line_indexed if linkage breaks. */

/* DialogStackPush — 1:1 with FUN_0040C6A0. Allocates asset, stores
 * speaker entity backups. Returns the stack depth after push, or
 * -1 on overflow / asset load failure (mirrors original: original
 * only commits the slot if LoadAssetFromDtaBase succeeded). */
static int DialogStackPush(Entity *speaker, const char *dialog_name,
                           const uint8_t *opts_bytes, uint32_t talk_anim_va)
{
    if (s_dialog_stack_n >= DIALOG_STACK_MAX) {
        fprintf(stderr, "[dialog] push: stack full (%d)\n", s_dialog_stack_n);
        return -1;
    }
    if (!speaker) {
        fprintf(stderr, "[dialog] push: NULL speaker — skip\n");
        return -1;
    }

    DialogStackSlot *slot = &s_dialog_stack[s_dialog_stack_n];
    slot->entity        = speaker;
    slot->talk_anim_va  = talk_anim_va;

    /* Horner fold (1:1 with FUN_0040C6A0 inner loop: `h = h*2 + b`). */
    uint32_t h = 0;
    if (opts_bytes) {
        for (const uint8_t *p = opts_bytes; *p; ++p)
            h = h * 2u + *p;
    }
    slot->opts_hash = h;

    /* Backup atlas + bytecode slots (intern handles — match script-byte
     * layout). Even if some are 0 (no current atlas) we still record. */
    uint8_t *eb = (uint8_t *)speaker;
    slot->atlas_backup    = *(uint32_t *)(eb + 0x28);
    slot->bytecode_backup = *(uint32_t *)(eb + 0x2C);

    /* Load dialog asset. Original commits the slot ONLY if load
     * succeeded; we mirror that: on failure, don't bump count. */
    AnimAsset *a = LoadAssetFromDtaBase(dialog_name);
    if (!a) {
        fprintf(stderr, "[dialog] push: asset '%s' load failed — skip\n",
                dialog_name ? dialog_name : "(null)");
        return -1;
    }
    slot->asset = a;

    ++s_dialog_stack_n;
    return s_dialog_stack_n;
}

/* DialogStackPop — 1:1 with FUN_0040C500. Iterates slots from BOTTOM
 * to TOP (matches original loop direction) restoring entity state +
 * freeing per-slot asset. Resets stack count to 0 (clear all). */
static void DialogStackPop(void)
{
    for (int i = 0; i < s_dialog_stack_n; ++i) {
        DialogStackSlot *slot = &s_dialog_stack[i];
        Entity *e = slot->entity;
        if (e) {
            uint8_t *eb = (uint8_t *)e;
            /* FUN_00407600 atlas restore — write atlas backup into
             * entity[+0x28]. We skip the size-realloc branch (original
             * checks new_atlas dims vs entity bitmap bytes and reallocs
             * via FUN_00405920); on restore the backup atlas was the
             * speaker's original, so dims match what the entity already
             * carries — no realloc needed in 99% of cases. If the dialog
             * asset was larger and the bitmap was grown during dialog,
             * the restored atlas now references a smaller frame range —
             * still works, just over-allocated buffer (benign). */
            *(uint32_t *)(eb + 0x28) = slot->atlas_backup;
            /* FUN_00402500 walker-state reset (1:1). */
            *(uint16_t *)(eb + 0x3A) &= (uint16_t)0xFFFAu;
            *(uint16_t *)(eb + 0x38) = 0;
            *(uint16_t *)(eb + 0x36) = 0;
            *(uint16_t *)(eb + 0x34) = 0;
            *(uint16_t *)(eb + 0x3C) = 0;
            *(uint16_t *)(eb + 0x42) = 0;
            *(uint16_t *)(eb + 0x40) = 0;
            *(uint16_t *)(eb + 0x32) = 0;
            *(uint32_t *)(eb + 0x50) = 0;
            *(uint32_t *)(eb + 0x4C) = 0;
            /* Clear pc + restore bytecode. */
            *(uint16_t *)(eb + 0x30) = 0;
            *(uint32_t *)(eb + 0x2C) = slot->bytecode_backup;
        }
        if (slot->asset) {
            FreeAsset(slot->asset);
            slot->asset = NULL;
        }
        slot->entity = NULL;
    }
    s_dialog_stack_n = 0;
}

/* T20 — 1:1 with Ghidra dialog flow:
 *   op 0x52 (FUN_0040C6A0) = PUSH only (entity backups + asset load)
 *   op 0x53 (FUN_00409DD0 + FUN_0040C500) = play [sampl] WAVs + pop
 *
 * Lines come from Gadki.scr [rozmowa]<result_key> section. Each entry
 * is a [sampl] tag with the WAV filename, optionally followed by [eb]/
 * [fj]/[nic] speaker + text for the speech bubble. result_key is the
 * op 0x53 arg, chosen by script's IF-chain on var[4]. */
static void dialog_play_section_lines(uint16_t actor, const char *section_name)
{
    if (!section_name || !*section_name) return;
    if (!g_dialogues_obj) return;
    if (!ScriptObjFindSection(g_dialogues_obj, "[rozmowa]",
                              section_name, "[animacja]"))
    {
        /* 1:1 with FUN_00409DD0: silent return if section missing. */
        fprintf(stderr, "[dialog] section '%s' not in Gadki.scr — skip\n",
                section_name);
        return;
    }
    const uint8_t *ss = ScriptObjGetSectionStart(g_dialogues_obj);
    const uint8_t *se = ScriptObjGetSectionEnd  (g_dialogues_obj);
    if (!ss || !se) return;

    /* Iterate over [sampl] blocks (1:1 with FUN_00409DD0 outer loop).
     * Each block: WAV filename + optional speaker text bubble. */
    char wav[64];
    char text[256];
    char speaker = 0;
    int played = 0;
    for (int i = 0; i < 64; ++i) {
        if (!dialog_extract_line(&ss, se, wav, sizeof wav,
                                 text, sizeof text, &speaker)) break;
        /* Speaker actor verb — Ebek=0x29? Fjej=0x2A? — placeholder
         * "actor" arg keeps the existing speech-balloon positioning
         * (centred on screen) since we don't yet thread the speaker
         * verb through DialogStackPush. T20c future: pass speaker
         * verb from g_dialog_stack[top].entity click-payload. */
        (void)speaker;
        fprintf(stderr, "[dialog] line %d wav='%s' speaker=%c text='%s'\n",
                i + 1, wav[0] ? wav : "(none)",
                speaker ? speaker : '-',
                text[0] ? text : "(none)");
        dialog_play_line_indexed(actor, text, wav, i + 1);
        ++played;
    }
    fprintf(stderr, "[dialog] section '%s' played %d sampl blocks\n",
            section_name, played);
}

/* T20b debug — set while a dialog is open (between op 0x52 push and
 * op 0x53 pop). HandleSceneInput + DispatchClickEvent annotate logs
 * with [dlg] tag when this is set, so we can see exactly which click
 * the user makes during the choice picker. */
uint8_t g_dialog_active = 0;

/* T103 — Solund-menu non-audio gates. Set by SolundClick + restored
 * by ApplySavedSettings on boot. */
uint8_t g_subtitles_on = 1;       /* DAT_00455121 mirror */
uint8_t g_dialogues_on = 1;       /* DAT_00455122 mirror — gates op 0x52/0x53 */

int ScriptCallDialogBegin(uint16_t actor, const char *dialog_name,
                          const uint8_t *opts, uint32_t talk_anim_va)
{
    /* T103 — gate on g_dialogues_on (DAT_00455122). Original op 0x52
     * @ 0x00408BC8 wraps the FUN_0040C6A0 call in `if (DAT_00455122 != 0)`.
     * If dialogues are disabled in Solund, we no-op the entire op. */
    if (!g_dialogues_on) {
        fprintf(stderr, "[dlg] op 0x52 BEGIN suppressed (dialogues_on=0)\n");
        return 0;
    }
    fprintf(stderr, "[dlg] op 0x52 BEGIN actor=0x%04X asset=%s talk_anim_va=0x%08X\n",
            actor, dialog_name ? dialog_name : "(null)", talk_anim_va);
    g_stats.total_dialogs++;                /* T56 */
    g_dialog_active = 1;

    if (!dialog_name || !*dialog_name) return 0;

    /* op 0x52 = PUSH only (1:1 FUN_0040C6A0). T108 — pass talk_anim_va
     * (= op 0x52's 4th arg / PE VA of mouth-cycle bytecode) so
     * DialogActivateTopSpeaker can bind it to entity[+0x2C] later. */
    Entity *speaker = FindEntityByVerbId(actor);
    DialogStackPush(speaker, dialog_name, opts, talk_anim_va);
    return 0;
}

void ScriptCallDialogEnd(const char *result)
{
    /* T103 — gate on g_dialogues_on, 1:1 z op 0x53 wrap @ 0x00408C28. */
    if (!g_dialogues_on) {
        fprintf(stderr, "[dlg] op 0x53 END suppressed (dialogues_on=0)\n");
        return;
    }
    /* op 0x53 = play lines from [rozmowa]<result> + pop (1:1
     * FUN_00409DD0 + FUN_0040C500). */
    fprintf(stderr, "[dlg] op 0x53 END result=%s (stack=%d) var[4]=0x%04X\n",
            result ? result : "(null)", s_dialog_stack_n,
            (unsigned)(g_script_vars[4] & 0xFFFF));

    /* Speaker actor verb from the top of dialog stack. The dialog
     * stack stores Entity*, but ScriptCallShowText needs a verb_id
     * for balloon positioning. For now pass actor=0 — balloon centres
     * on screen instead of speaker (acceptable until we extract the
     * verb from the entity's click payload). */
    dialog_play_section_lines(0, result);
    DialogStackPop();
    if (s_dialog_stack_n == 0) g_dialog_active = 0;
}
