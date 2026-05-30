/* src/scene/komnata.c — room (komnata) loader.
 *
 * LoadKomnata is the engine's main "switch to a different room"
 * routine. It walks the current stage's komnata table for the
 * requested id, then performs a full room reset:
 *
 * 1. Look up the komnata's name + flags + enter/secondary script
 * addresses in the stage descriptor.
 * 2. Tear down the previous room (entity lists, visible masks,
 * frame SFX state, positional sound queue).
 * 3. Reset perspective globals and actor walker state so the
 * previous room's biases don't leak into the new one.
 * 4. Parse the new room's [sampl] tags from Wacky.scr.
 * 5. Page-swap the inventory panel.
 * 6. Run the enter script (RunScriptInterpreter at 0x26/0x26).
 * 7. If a secondary script is registered, pump two frame ticks
 * (lets one-shot BG-blit entities paint to the back-buffer)
 * then run it too.
 *
 * Returns the resolved komnata name pointer, or NULL if the lookup
 * failed (id out of range, stage table missing, terminator hit before
 * idx).
 */

#include "wacki.h"

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

/* LoadKomnata —
 *
 * walk entries (14 bytes each) until index == id
 * palette fade-out (deferred — see comment below)
 * + // clear lists
 * (name) // name-keyed init
 * FindScriptByStageAndRoom(scripts, etap, name) // locate Wacky.scr section
 * (scripts) // parse [sampl] tags
 * load pal_NN_NN.pal // per-komnata palette
 * if (flags & 1) link kind=3 walk-behind initial entity
 * if (flags & 2) link kind=2/3/4 default entities (cursor, krazek)
 * RunScriptInterpreter(0x26, 0x26, entry[+6]) // enter script
 * palette fade-in
 * ProcessGameFrameTick × 2
 * RunScriptInterpreter(0x26, 0x26, entry[+10]) // secondary script
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

    /* T105 fix — walk komnata table:
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

    /* Clear lists — port mirror of + . */
    EntityListClearAll();
    VisibleMasksReset();
    ResetFrameSfxState();
    /* T132 — original calls (sound queue reset)
 * as part of room reset. Without this, positional sources from the
 * previous komnata leak into the new one's aggregate pan. */
    SoundQueueReset();

    /* T107 — partial port of (room reset). The original
 * does a consolidated clear that we previously split across multiple
 * code paths; the bits below explicitly cover the gaps so non-op-0x2C
 * komnaty (= no explicit mask asset) don't carry stale state from the
 * previous room:
 *
 * 1. Perspective band count — original unconditionally
 * `perspective_band_count_alt = 0` then `= 4 if (flags & 2)`. Earlier port only
 * reset it inside ScriptCallBgMaskSetup (which is called only when
 * the room has a mask asset). Rooms without one inherited band
 * count from the previous komnata.
 *
 * 2. Actor walker state — clear walk-remaining (+0x4C/+0x50) and
 * walker-busy flag (+0x3A bit 0) on both g_actor[]. Op 0x15 path
 * plant later re-sets them. Without this, an actor mid-walk when
 * the player exited the room would resume walking at room entry.
 *
 * 3. Actor scale_pct — original sets `+0x16 = 100` (= 1.0×). Without
 * this, an actor whose script set their scale in a previous room
 * keeps that scale until UpdateActorMovement re-computes from
 * anchor Y. Brief visual glitch on room entry.
 *
 * Cursor entity link (cursor_state_struct / ) is skipped — see
 * T106 (deferred). Click queue head, panel verb-tab init, label
 * strings — all done at engine boot, no need to repeat. */
    extern int g_persp_band_count;
    g_persp_band_count = (flags & 2) ? 4 : 0;
    /* Reset perspective globals ( top):
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
 *'s FindScriptByStageAndRoom + 
 * sequence. Replaces hand-transcribed g_frame_sfx[] table for any
 * asset mentioned in the parsed [komnata]N section. */
    extern void *g_scripts_obj;
    if (g_scripts_obj && name) {
        char etap_str[2] = { (char)('0' + g_cur_etap), 0 };
        if (FindScriptByStageAndRoom(g_scripts_obj, etap_str, name)) {
            /* ScriptObj.start / .end are private to script.c; expose
 * via accessor — see ScriptObjGetSection decl. */
            extern const uint8_t *ScriptObjGetSectionStart(void *self);
            extern const uint8_t *ScriptObjGetSectionEnd  (void *self);
            ResetDynamicSfxTable();
            const uint8_t *ss = ScriptObjGetSectionStart(g_scripts_obj);
            const uint8_t *se = ScriptObjGetSectionEnd  (g_scripts_obj);
            if (ss && se) ParseSamplTagsForKomnata(ss, se);
        }
    }

    /* Panel page-swap — calling
 * right before enter_script. Loads page[0]'s 6 slots
 * into the panel verb table so the room starts with the inventory
 * front page on the bar. */
    g_settings_anim_active = flags;     /* T121: full u16 from komnata
 * entry[+4] (was truncated to u8). */
    PanelPageSwap();

    /* Run enter_script ( 0x26, ptr)`). */
    if (enter_va) {
        const uint8_t *bc = (const uint8_t *)xlat_binary_ptr(enter_va);
        if (bc) RunScriptInterpreter(0x26, 0x26, (uint8_t *)bc);
    }

    /* TWO frame ticks between enter_va and second_va
 * :
 * palette fade-in;
 * ProcessGameFrameTick; // <-- this
 * ProcessGameFrameTick; // <-- and this
 *
 * These ticks let EntityRenderAll process one-shot BG-blit entities
 * (spawn flags = 0x0060 → flag-0x40/0x20 branch in 
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
