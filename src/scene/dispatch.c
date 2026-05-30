/* src/scene/dispatch.c — click-event → bytecode dispatch.
 *
 * DispatchClickEvent is the routing point between the scene-input
 * layer (HandleSceneInput / click queue) and the script VM. Given an
 * (obj_id, verb_id) pair, it walks the stage's verb dispatch table to
 * find the matching bytecode entry, then calls RunScriptInterpreter
 * with the (this_id, that_id) pair swapped for the obj-script
 * convention.
 */

#include "wacki.h"

#include <stdint.h>
#include <stdio.h>

extern int   read_dispatch_entry(uint32_t table_va, int idx,
                                 uint16_t *out_id, uint32_t *out_spv);
extern const void *xlat_binary_ptr(uint32_t addr);

static const uint8_t *find_dispatch_script(uint32_t table_va, uint16_t want_id)
{
    if (!table_va) return NULL;
    for (int i = 0; i < 256; ++i) {
        uint16_t id; uint32_t spv;
        if (!read_dispatch_entry(table_va, i, &id, &spv)) return NULL;
        if (id == want_id) {
            if (!spv) return NULL;
            return (const uint8_t *)xlat_binary_ptr(spv);
        }
        if (id == 0) return NULL;   /* terminator */
    }
    return NULL;
}

void DispatchClickEvent(uint16_t obj_id, uint16_t verb_id)
{
    extern const void *PeLoaderRead(uint32_t va);
    g_stats.total_clicks++;                    /* T56 */
    if (!g_stage_va) return;

    /* The per-stage struct's dispatch table pointers live at +4 / +8 of
     * the stage descriptor in PE memory. Read them through PeLoaderRead. */
    const uint8_t *sd = (const uint8_t *)PeLoaderRead(g_stage_va);
    if (!sd) return;
    uint32_t verb_tab_va = (uint32_t)(sd[4] | (sd[5] << 8) | (sd[6] << 16) | (sd[7] << 24));
    uint32_t obj_tab_va  = (uint32_t)(sd[8] | (sd[9] << 8) | (sd[10]<< 16) | (sd[11]<< 24));

    int continue_after = 1;
    const uint8_t *verb_script = find_dispatch_script(verb_tab_va, verb_id);
    const uint8_t *obj_script  = find_dispatch_script(obj_tab_va,  obj_id);
    if (verb_script || obj_script) {
        extern uint8_t g_dialog_active;        /* T20b debug */
        fprintf(stderr, "[dispatch] obj=0x%04X verb=0x%04X%s%s%s\n",
                obj_id, verb_id,
                verb_script ? " V" : "",
                obj_script  ? " O" : "",
                g_dialog_active ? " [dlg-active]" : "");
    }
    /* 1:1 with original FUN_004094A0 — the THIS/THAT args SWAP between
     * verb-script and object-script calls:
     *
     *   verb_script: RunScriptInterpreter(local_14, param_2, ...);
     *                = (param_1, param_2)
     *                = (held_item, hover_verb)
     *                → this=held_item, that=hover_verb
     *
     *   obj_script:  RunScriptInterpreter(local_18, param_1, ...);
     *                = (param_2, param_1)
     *                = (hover_verb, held_item)
     *                → this=hover_verb, that=held_item     ← SWAP
     *
     * Earlier port called both with the same (obj_id, verb_id) order
     * — object scripts saw wrong this_id, breaking ops 0x00 (skip-if-
     * neq-this) and 0x21 (return-is-item-this). */
    if (verb_script)
        continue_after = RunScriptInterpreter(obj_id, verb_id, (uint8_t *)verb_script);
    if (continue_after && obj_script)
        RunScriptInterpreter(verb_id, obj_id, (uint8_t *)obj_script);
}
