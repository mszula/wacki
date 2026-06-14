/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/scene/komnata_scene.c — LoadKomnataScene + scene-load helpers.
 *
 * Higher-level wrapper around LoadKomnata (the script-level load in
 * scene/komnata.c). LoadKomnata runs the new room's enter_script;
 * LoadKomnataScene handles the per-frame engine state:
 *
 *   1. Reset both actors' per-entity VM scratch + rebind their idle
 *      bytecode (entry [5] in the per-stage anim table).
 *   2. Drop the previous scene's assets (BG raw blob, FLD asset,
 *      atlas-BG copy, looping music).
 *   3. Run the script-level LoadKomnata for entity setup + enter_script.
 *   4. Synthesise a DemoScene from the resolved room name, then load
 *      the .pic background and the .fld walkability bitmap (the script
 *      may have already set walkability via op 0x2C — see fallback).
 *   5. Start the per-room music loop and repaint the clean BG.
 *
 * Called from:
 *   - ScriptGoToKomnata (op 0x20 in script.c)        — verb-driven
 *   - F9 quickload in play_demo_scene main loop      — save-driven
 *   - play_demo_scene prologue                        — initial entry */

#include "wacki.h"
#include "wacki/log.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- constants ---------------------------------------------------- */

/* Stage descriptor offsets for the per-actor anim-table pointers. */
#define STAGE_OFF_ACTOR_ANIM_PTR_BASE  0x0C
#define ANIM_TABLE_IDLE_SLOT_OFFSET    0x14   /* entry [5] = idle bytecode */
#define WALKER_STATE_RESET_MASK        (ESTATE_FRAME_READY | ESTATE_WALKER_FRESH)

#define FLD_BITS_PER_BYTE              8

/* Synthesised-DemoScene name buffer size + the music name template
 * (Tlo_<etap>_<komnata>a.wav). */
#define SCENE_NAME_BUFFER_SIZE         64
#define MUSIC_NAME_FMT                 "Tlo_%u_%ua.wav"
#define FLD_EXTENSION                  ".fld"
#define FLD_EXTENSION_BYTES            5      /* ".fld" + NUL */

/* ---- externs ------------------------------------------------------ */

extern uint32_t          ent_ptr_intern(void *p);
extern AnimAsset        *LoadAssetFromDtaBase(const char *name);
extern void              PaintSceneBgAtlasIfAny(void);
extern void              FreeSceneBgAtlas(void);
extern int               paint_rawb_pic(const void *blob, uint32_t size,
                                        int as_overlay);
extern const void       *xlat_binary_ptr(uint32_t addr);

extern int               g_walk_x0, g_walk_x1;
extern int               g_walk_y0, g_walk_y1;
extern const DemoScene  *g_current_scene;
extern AnimAsset        *g_scene_fld_asset;
extern void             *g_scene_bg_raw;
extern uint32_t          g_scene_bg_size;
extern const uint8_t    *g_walk_fld_pixels;
extern int               g_walk_fld_w, g_walk_fld_h;
extern int               g_walk_fld_ox, g_walk_fld_oy;
extern int               g_walk_fld_stride;
extern const void       *PeLoaderRead(uint32_t va);

/* ---- helpers ------------------------------------------------------ */

/* Reset all per-entity VM scratch state on an actor and rebind its
 * idle bytecode (entry [5] in the per-stage anim table). Without this
 * the previous room's walker bytecode + patched op 0x15 target stay
 * bound, and the next VM tick re-plants the path toward the OLD
 * scene's exit — actor "drifts diagonally" on room entry. */
static void reset_actor_for_komnata(Entity *a, const uint8_t *sd, int actor_idx)
{
    EOFF(a, ENT_OFF_STATE_FLAGS,   uint16_t) &= (uint16_t)~WALKER_STATE_RESET_MASK;
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
        LOG_TRACE("fld", "%s: %dx%d @ (%d,%d) stride=%d", s->fld_file, g_walk_fld_w, g_walk_fld_h, g_walk_fld_ox, g_walk_fld_oy, g_walk_fld_stride);
    } else {
        LOG_TRACE("fld", "%s: load failed — falling back to bbox", s->fld_file);
    }
}

/* ---- public API --------------------------------------------------- */

void LoadKomnataScene(uint16_t id)
{
    if (id == 0) return;

    /* Hold the screen on the last good frame for the whole load: LoadKomnata's
     * settling ticks (Step 3) paint the new room's entities while the shadow
     * still holds the OLD background — the new BG isn't painted until Step 5.
     * Presenting those ticks flashes "new entities on old background" for a
     * frame or two (very visible at the PS2's vsync-locked 30 fps). The outer
     * game tick presents the clean scene once we return. */
    g_present_suppressed = 1;

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
        g_present_suppressed = 0;
        LOG_TRACE("scene", "LoadKomnataScene(%u) — LoadKomnata failed", id);
        return;
    }

    /* --- Step 4: build a DemoScene + load BG + walkability fallback */
    const DemoScene *s = synthesise_demo_scene(name);
    g_current_scene = s;
    g_walk_x0 = s->walk_x0;  g_walk_x1 = s->walk_x1;
    g_walk_y0 = s->walk_y0;  g_walk_y1 = s->walk_y1;

    load_scene_background(s);
    load_scene_walkability_fallback(s);

    /* --- Step 5: music + clean BG repaint ------------------------ */
    if (s->music_wav) PlayMenuMusic(s->music_wav, 1);

    /* LoadKomnata runs the enter script + two embedded ProcessGame
     * FrameTick calls. Those embedded ticks call EntityRenderAll,
     * painting entities at their CURRENT positions. The outer PGFT
     * Inner that called us is mid-tick and will call EntityRenderAll
     * again at slightly advanced positions — repainting the BG here
     * wipes the embedded paint so the outer pass renders a clean
     * single frame. */
    if (g_scene_bg_raw) paint_rawb_pic(g_scene_bg_raw, g_scene_bg_size, 0);
    PaintSceneBgAtlasIfAny();

    /* New scene composited clean (new BG painted; the embedded entity paint is
     * wiped and re-rendered by the outer tick) — resume presenting. */
    g_present_suppressed = 0;
    LOG_TRACE("scene", "LoadKomnataScene(%u) → '%s'", id, name);
}
