/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/scene/komnata.c — room (komnata) loader.
 *
 * LoadKomnata is the engine's "switch to a different room" routine.
 * It walks the current stage's komnata table for the requested id,
 * then performs a full room reset:
 *
 *   1. Look up name + flags + enter/secondary script addresses in
 *      the stage's komnata table.
 *   2. Tear down the previous room (entity lists, visible masks,
 *      frame SFX state, positional sound queue).
 *   3. Reset perspective globals + actor walker state so the
 *      previous room's biases don't leak into the new one.
 *   4. Parse the new room's [sampl] tags from Wacky.scr.
 *   5. Page-swap the inventory panel.
 *   6. Run the enter script (RunScriptInterpreter at 0x26/0x26).
 *   7. If a secondary script is registered, pump two frame ticks
 *      (so one-shot BG-blit entities paint to the back-buffer)
 *      then run it too.
 *
 * Returns the resolved komnata name, or NULL if lookup failed.
 */

#include "wacki.h"
#include "wacki/log.h"

#include <stdint.h>
#include <stdio.h>

extern uint32_t       g_stage_va;
extern void          *g_scripts_obj;
extern int            g_persp_band_count;
extern uint16_t       g_cursor_speed;
extern uint16_t       g_perspective_min;
extern uint16_t       g_perspective_step;
extern Entity        *g_actor[2];

extern void  EntityListClearAll(void);
extern void  VisibleMasksReset(void);
extern void  ResetFrameSfxState(void);
extern void  SoundQueueReset(void);
extern void  ResetDynamicSfxTable(void);
extern void  ParseSamplTagsForKomnata(const uint8_t *start, const uint8_t *end);
extern void  ProcessGameFrameTick(void);
extern const void *xlat_binary_ptr(uint32_t addr);
extern const void *PeLoaderRead(uint32_t va);
extern const uint8_t *ScriptObjGetSectionStart(void *self);
extern const uint8_t *ScriptObjGetSectionEnd  (void *self);
extern int   FindScriptByStageAndRoom(void *self, const char *etap, const char *komnata);

/* ---- constants ---------------------------------------------------- */

/* Komnata table entry layout: 14 bytes per entry, packed:
 *   +0  u32 name VA
 *   +4  u16 flags
 *   +6  u32 enter-script VA
 *   +10 u32 secondary-script VA */
#define KOMNATA_ENTRY_SIZE          14
#define KOMNATA_OFF_NAME_VA          0
#define KOMNATA_OFF_FLAGS            4
#define KOMNATA_OFF_ENTER_VA         6
#define KOMNATA_OFF_SECOND_VA       10

/* Komnata flag bits. */
#define KOMNATA_FLAG_HAS_MASK       0x0001  /* (flags & 1) → use BG mask asset */
#define KOMNATA_FLAG_HAS_PERIMETER  0x0002  /* (flags & 2) → reserved perim bands */

/* Perspective baseline restored at room entry — overrides any op 0x40
 * SET_PERSPECTIVE bias from the previous room's cinematic. */
#define PERSP_DEFAULT_CURSOR_SPEED  0x78
#define PERSP_DEFAULT_MIN              4
#define PERSP_DEFAULT_STEP             7

/* Perspective band count: 0 normally, 4 reserved slots when the
 * komnata has a perimeter (partner-obstacle pathfinding). */
#define PERSP_PERIM_BAND_COUNT         4

/* Default actor scale (100 % = unity). UpdateActorMovement re-derives
 * it from anchor-Y on the first tick, but resetting here removes a
 * brief visual glitch on room entry. */
#define ACTOR_DEFAULT_SCALE          100

/* Walker state-bit reset mask: clear "frame ready" + "walker fresh". */
#define WALKER_RESET_BITS           (ESTATE_FRAME_READY | ESTATE_WALKER_FRESH)

/* Neutral (this, that) verb pair for enter-script invocation. */
#define ENTER_SCRIPT_THIS_VERB     0x26
#define ENTER_SCRIPT_THAT_VERB     0x26

/* Two frame ticks run UNCONDITIONALLY after the enter script, before the
 * optional secondary script — mirrors the original FUN_00402a50, which
 * pumps two ProcessGameFrameTick() between enter_va and secondary_va
 * regardless of whether secondary_va exists. They give one-shot BG-blit
 * entities (op 0x30 spawn with flags=0x0060) a render pass to paint
 * themselves onto the BG (via paint_oneshot_bg → SaveSceneBgAtlas) so
 * LoadKomnataScene's post-load PaintSceneBgAtlasIfAny can re-apply them
 * over the freshly-painted .pic — AND so a later secondary_va op 0x31
 * destroy can't drop them before they paint.
 *
 * NOTE: an earlier port gated these ticks on (second_va != 0). That left
 * secondary-less rooms whose enter script spawns a one-shot overlay with
 * the overlay never painted. Concretely: korlab5 (lab corridor, no
 * secondary) re-rendered its opened airlock CLOSED on re-entry — the
 * open-door overlay (id 51, flags 0x0060) is spawned in the var[0xDC]!=0
 * branch but was never blitted, while the same branch's verb-mask still
 * made the door passable and var[0xDC] kept the toggle "open". */
#define POST_ENTER_SETUP_TICKS         2

/* ---- helpers ------------------------------------------------------ */

/* Read a u32 little-endian from a byte buffer. Used for PE-VA fields
 * in the komnata table. */
static uint32_t read_u32_le(const uint8_t *p)
{
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

/* Read a u16 little-endian. */
static uint16_t read_u16_le(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* Look up the komnata table entry for `id` (1-based). Returns 0 on
 * miss (id past the terminator or stage table unavailable), 1 on hit.
 * On hit, fills the four out-fields from the matching 14-byte slot. */
static int find_komnata_entry(uint16_t id,
                              uint32_t *out_name_va,
                              uint16_t *out_flags,
                              uint32_t *out_enter_va,
                              uint32_t *out_second_va)
{
    if (id == 0 || !g_stage_va) return 0;

    const uint8_t *sd = (const uint8_t *)PeLoaderRead(g_stage_va);
    if (!sd) return 0;

    uint32_t komnata_arr_va = read_u32_le(sd);
    const uint8_t *karr = (const uint8_t *)PeLoaderRead(komnata_arr_va);
    if (!karr) return 0;

    int idx = (int)id - 1;
    /* Walk entries 0..(id-1) to verify no terminator (NULL name + zero
     * flags) sits before the requested index. Stages 2-5 may have
     * >16 rooms — we don't cap at any fixed bound. */
    for (int i = 0; i < (int)id; ++i) {
        const uint8_t *e = karr + i * KOMNATA_ENTRY_SIZE;
        uint32_t       name_va = read_u32_le(e + KOMNATA_OFF_NAME_VA);
        uint16_t       flags   = read_u16_le(e + KOMNATA_OFF_FLAGS);
        if (!name_va && flags == 0) {
            LOG_INFO("load-komnata", "terminator hit at i=%d, requested id=%u", i, id);
            return 0;
        }
    }

    const uint8_t *e = karr + idx * KOMNATA_ENTRY_SIZE;
    *out_name_va   = read_u32_le(e + KOMNATA_OFF_NAME_VA);
    *out_flags     = read_u16_le(e + KOMNATA_OFF_FLAGS);
    *out_enter_va  = read_u32_le(e + KOMNATA_OFF_ENTER_VA);
    *out_second_va = read_u32_le(e + KOMNATA_OFF_SECOND_VA);
    return 1;
}

/* Tear down the previous room — entity lists, mask cache, per-frame
 * SFX state, positional sound queue. */
static void clear_previous_room(void)
{
    EntityListClearAll();
    VisibleMasksReset();
    ResetFrameSfxState();
    SoundQueueReset();
}

/* Reset perspective globals to their defaults so a script-bias from
 * the previous komnata doesn't leak in. */
static void reset_perspective_baseline(void)
{
    g_cursor_speed     = PERSP_DEFAULT_CURSOR_SPEED;
    g_perspective_min  = PERSP_DEFAULT_MIN;
    g_perspective_step = PERSP_DEFAULT_STEP;
}

/* Clear mid-walk state on the actors and reset their scale so a
 * room-entry render doesn't carry over the previous room's
 * perspective bias. */
static void reset_actor_walker_state(void)
{
    /* A held action/climb scale (g_actor_scale_frozen) is per-komnata and
     * is only otherwise cleared by the next walk — drop it on room entry so
     * a freeze from the previous room (e.g. an interrupted climb) can't
     * carry a stuck scale into a scene where the actor never walks. */
    extern int g_actor_scale_frozen[2];

    for (int i = 0; i < 2; ++i) {
        g_actor_scale_frozen[i] = 0;

        Entity *a = g_actor[i];
        if (!a) continue;

        EOFF(a, ENT_OFF_WALKER_DX_REM, uint32_t) = 0;
        EOFF(a, ENT_OFF_WALKER_DY_REM, uint32_t) = 0;
        EOFF(a, ENT_OFF_STATE_FLAGS,   uint16_t) &= (uint16_t)~WALKER_RESET_BITS;
        EOFF(a, ENT_OFF_PC,            uint16_t) = 0;
        EOFF(a, ENT_OFF_DELAY,         uint16_t) = 0;
        EOFF(a, ENT_OFF_SCALE_PCT,     uint16_t) = ACTOR_DEFAULT_SCALE;
    }
}

/* Parse [sampl] tags from Wacky.scr so frame-driven SFX work for the
 * assets mentioned in the script. Two sections feed the table:
 *
 *   1. [etap] N [komnata] init     — per-stage default block. Holds
 *                                    actor-wide entries (e.g. Ebek's
 *                                    idle frame 95 → Muzik05..09
 *                                    headphone-rock random pool) that
 *                                    apply across every komnata in the
 *                                    stage.
 *   2. [etap] N [komnata] <name>   — per-room block with prop-specific
 *                                    sampls (kioskarz, ptak, etc.)
 *
 * Both populate the same g_dynamic_sfx table — the init pass runs
 * first so room-specific entries can override identical (asset,
 * frame_start) triples if needed (no shipped script does, but the
 * order keeps it well-defined). */
static void parse_komnata_sampl_tags(const char *name)
{
    if (!g_scripts_obj || !name) return;

    char etap_str[2] = { (char)('0' + g_cur_etap), 0 };

    ResetDynamicSfxTable();

    /* Pass 1: [komnata] init (per-stage defaults). The original
     * engine searches both sections at trigger time; we flatten by
     * parsing both into the dynamic_sfx table. */
    if (FindScriptByStageAndRoom(g_scripts_obj, etap_str, "init")) {
        const uint8_t *is = ScriptObjGetSectionStart(g_scripts_obj);
        const uint8_t *ie = ScriptObjGetSectionEnd  (g_scripts_obj);
        if (is && ie) ParseSamplTagsForKomnata(is, ie);
    }

    /* Pass 2: this specific komnata. */
    if (!FindScriptByStageAndRoom(g_scripts_obj, etap_str, name)) return;
    const uint8_t *ss = ScriptObjGetSectionStart(g_scripts_obj);
    const uint8_t *se = ScriptObjGetSectionEnd  (g_scripts_obj);
    if (!ss || !se) return;
    ParseSamplTagsForKomnata(ss, se);

    /* Resolve this room's looping BG music from its room-level [sampl]
     * tracks. The original LAYERS all of them simultaneously, each looped
     * (FUN_00409970 chains a node per [sampl]; FUN_00401100 fires them all) —
     * so collect the full set; LoadKomnataScene plays each on its own looping
     * channel. Pre-cleared in free_previous_scene_assets, so "no [sampl]" →
     * stays silent. */
    int ntrk = FindKomnataBgMusic(ss, se, g_scene_bg_tracks,
                                  KOMNATA_BG_MUSIC_MAX_TRACKS);
    g_scene_bg_track_count = ntrk;
    LOG_TRACE("music", "room '%s': %d layered BG track(s)", name, ntrk);
}

/* Invoke an enter/secondary script via RunScriptInterpreter with
 * the standard (this=0x26, that=0x26) neutral verb pair. */
static void run_enter_script(uint32_t va)
{
    if (!va) return;
    const uint8_t *bc = (const uint8_t *)xlat_binary_ptr(va);
    if (bc) {
        RunScriptInterpreter(ENTER_SCRIPT_THIS_VERB,
                             ENTER_SCRIPT_THAT_VERB,
                             (uint8_t *)bc);
    }
}

/* ---- public entry point ------------------------------------------- */

const char *LoadKomnata(uint16_t id)
{
    uint32_t name_va, enter_va, second_va;
    uint16_t flags;
    if (!find_komnata_entry(id, &name_va, &flags, &enter_va, &second_va)) {
        LOG_INFO("load-komnata", "id=%u not in stage table", id);
        return NULL;
    }

    const char *name = (const char *)PeLoaderRead(name_va);
    g_cur_komnata = id;
    g_stats.total_komnata_loads++;
    LOG_INFO("load-komnata", "%u '%s' flags=0x%04X enter=0x%08X second=0x%08X", id, name ? name : "(null)", flags, enter_va, second_va);

    /* --- 1: tear down the previous room ---------------------------- */
    clear_previous_room();

    /* --- 2: reset perspective + actor walker state ----------------- */
    g_persp_band_count = (flags & KOMNATA_FLAG_HAS_PERIMETER)
                         ? PERSP_PERIM_BAND_COUNT
                         : 0;
    reset_perspective_baseline();
    reset_actor_walker_state();

    /* --- 3: parse [sampl] tags for the new room -------------------- */
    parse_komnata_sampl_tags(name);

    /* --- 4: page-swap the inventory panel + run scripts ------------ */
    g_komnata_flags = flags;
    PanelPageSwap();
    run_enter_script(enter_va);

    /* Settle the enter script's one-shot BG entities onto the BG before
     * the scene is presented — unconditional, matching FUN_00402a50.
     * (See POST_ENTER_SETUP_TICKS for why this must not be gated on
     * second_va.) */
    for (int i = 0; i < POST_ENTER_SETUP_TICKS; ++i) ProcessGameFrameTick();

    if (second_va) {
        run_enter_script(second_va);
    }
    return name;
}
