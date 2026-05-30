/* src/scene/dispatch.c — click-event → bytecode dispatch.
 *
 * DispatchClickEvent is the routing point between the scene-input
 * layer (HandleSceneInput / click queue) and the script VM. Given an
 * (obj_id, verb_id) pair, it walks two per-stage dispatch tables:
 *
 *   - the VERB table: looks up `verb_id`. If found, runs the verb
 *     script with (this=obj_id, that=verb_id).
 *   - the OBJ table: looks up `obj_id`. If found, runs the object
 *     script with (this=verb_id, that=obj_id) — note the swap, so
 *     object scripts can treat `this` as the verb the user clicked.
 *
 * Either or both may run on a single click. A non-zero return from
 * the verb script suppresses the object script.
 */

#include "wacki.h"

#include <stdint.h>
#include <stdio.h>

extern int   read_dispatch_entry(uint32_t table_va, int idx,
                                 uint16_t *out_id, uint32_t *out_spv);
extern const void *xlat_binary_ptr(uint32_t addr);
extern const void *PeLoaderRead(uint32_t va);
extern uint8_t g_dialog_active;

/* ---- constants ---------------------------------------------------- */

/* Dispatch tables are bounded length walks rather than indexed
 * lookups — the engine scans entries 0..N-1 for a match against the
 * requested id. 256 is well above any observed table size. */
#define DISPATCH_TABLE_MAX_ENTRIES  256

/* Stage descriptor field offsets (declared here in addition to
 * scene/stage.c so the dispatch path doesn't need to import a header). */
#define STAGE_OFF_VERB_TABLE_VA      4
#define STAGE_OFF_OBJ_TABLE_VA       8

/* Sentinel ids in a dispatch entry: the table is terminated by id=0. */
#define DISPATCH_TERMINATOR_ID       0

/* ---- helpers ------------------------------------------------------ */

/* Read a u32 little-endian from a byte buffer. */
static uint32_t read_u32_le(const uint8_t *p)
{
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

/* Locate the bytecode for the dispatch entry matching `want_id` in the
 * table at PE VA `table_va`. Returns NULL on miss / unmappable / null
 * table_va / table terminator. */
static const uint8_t *find_dispatch_script(uint32_t table_va, uint16_t want_id)
{
    if (!table_va) return NULL;

    for (int i = 0; i < DISPATCH_TABLE_MAX_ENTRIES; ++i) {
        uint16_t id;
        uint32_t spv;
        if (!read_dispatch_entry(table_va, i, &id, &spv)) return NULL;

        if (id == want_id) {
            return spv ? (const uint8_t *)xlat_binary_ptr(spv) : NULL;
        }
        if (id == DISPATCH_TERMINATOR_ID) return NULL;
    }
    return NULL;
}

/* ---- public entry point ------------------------------------------- */

void DispatchClickEvent(uint16_t obj_id, uint16_t verb_id)
{
    g_stats.total_clicks++;
    if (!g_stage_va) return;

    /* The per-stage dispatch table VAs live at fixed offsets inside
     * the stage descriptor in PE memory. */
    const uint8_t *sd = (const uint8_t *)PeLoaderRead(g_stage_va);
    if (!sd) return;

    uint32_t verb_tab_va = read_u32_le(sd + STAGE_OFF_VERB_TABLE_VA);
    uint32_t obj_tab_va  = read_u32_le(sd + STAGE_OFF_OBJ_TABLE_VA);

    const uint8_t *verb_script = find_dispatch_script(verb_tab_va, verb_id);
    const uint8_t *obj_script  = find_dispatch_script(obj_tab_va,  obj_id);

    if (verb_script || obj_script) {
        fprintf(stderr, "[dispatch] obj=0x%04X verb=0x%04X%s%s%s\n",
                obj_id, verb_id,
                verb_script ? " V" : "",
                obj_script  ? " O" : "",
                g_dialog_active ? " [dlg-active]" : "");
    }

    /* Verb script runs first with (this=obj, that=verb). A non-zero
     * return suppresses the object script. */
    int continue_after = 1;
    if (verb_script) {
        continue_after = RunScriptInterpreter(obj_id, verb_id,
                                              (uint8_t *)verb_script);
    }

    /* Object script (this/that swap so `this` is the user's clicked
     * verb and `that` is the object — convenient for object scripts
     * that branch on verb). */
    if (continue_after && obj_script) {
        RunScriptInterpreter(verb_id, obj_id, (uint8_t *)obj_script);
    }
}
