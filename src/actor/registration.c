/* src/actor/registration.c — (kind, id) → Entity dispatch table.
 *
 * The central look-up that lets the script VM go from a (kind, id) pair
 * (encoded into every SPAWN / LOAD_ASSET / REG_VERB_MASK call) to the
 * runtime pointer it represents. Used by:
 *
 *   - op 0x0B TIMER_SET, op 0x0F SET_ENTITY_ANIM  (main VM)
 *   - op 0x23 SWAP_ATLAS_BY_ID                    (per-entity VM)
 *   - op 0x2F REG_VERB_MASK                       (mask click chain)
 *   - op 0x2D LOAD_ASSET                          (registers; SPAWN finds)
 *
 * Scan is LIFO so the most-recent registration shadows earlier ones —
 * scene loaders rely on this to re-key (kind=1, id=N) to the new
 * scene's asset without needing to explicitly purge the old.
 *
 * The "Except" variant takes a skip-list and finds the highest-LIFO
 * entry that ISN'T in the list. Used by ScriptCallDestroyEnt when
 * tearing down all (kind, id) entries while preserving protected
 * actor click payloads — without it, the LIFO winner would be returned
 * forever and the destroy loop would never terminate.
 *
 * Wildcard unregister (id == 0xFFFF) drops every entry of the given
 * kind. Used by "tear down all of kind K" flows (op 0x50/0x51 SUBANIM).
 */

#include "wacki.h"
#include "internal.h"

#include <stdint.h>
#include <stddef.h>

/* Storage. Sized at 500 to cover one fully-populated scene's asset +
 * sprite + mask registrations with margin; never observed near full. */
struct UpdateRegEntry g_update_table[ACTOR_UPDATE_TABLE_SIZE];
int                   g_update_table_count = 0;

#define ID_WILDCARD 0xFFFFu

void *FindUpdateRegistration(uint16_t kind, uint16_t id)
{
    for (int i = g_update_table_count - 1; i >= 0; --i) {
        if (g_update_table[i].kind == kind && g_update_table[i].id == id) {
            return g_update_table[i].e;
        }
    }
    return NULL;
}

void *FindUpdateRegistrationExcept(uint16_t kind, uint16_t id,
                                   Entity *const *skip, int nskip)
{
    for (int i = g_update_table_count - 1; i >= 0; --i) {
        if (g_update_table[i].kind != kind || g_update_table[i].id != id) {
            continue;
        }
        int skipped = 0;
        for (int j = 0; j < nskip; ++j) {
            if (g_update_table[i].e == skip[j]) {
                skipped = 1;
                break;
            }
        }
        if (skipped) continue;
        return g_update_table[i].e;
    }
    return NULL;
}

void RegisterEntityForUpdate(Entity *e, uint16_t kind, uint16_t id)
{
    if (g_update_table_count >= ACTOR_UPDATE_TABLE_SIZE) return;
    g_update_table[g_update_table_count].e    = e;
    g_update_table[g_update_table_count].kind = kind;
    g_update_table[g_update_table_count].id   = id;
    ++g_update_table_count;
}

void UnregisterEntityForUpdate(uint16_t kind, uint16_t id)
{
    int w = 0;
    for (int r = 0; r < g_update_table_count; ++r) {
        int drop = (g_update_table[r].kind == kind) &&
                   (id == ID_WILDCARD || g_update_table[r].id == id);
        if (!drop) g_update_table[w++] = g_update_table[r];
    }
    g_update_table_count = w;
}

/* Single-entry removal (oldest match wins). Returns 1 if removed.
 *
 * Used by ScriptCallDestroyEnt's kind=1 asset unregister to preserve a
 * freshly-loaded asset when the destroy targets an older load of the
 * same id (e.g. liana state replacement: load Liana_b → load Liana.wyc
 * → destroy id=5 must drop Liana_b, leave Liana.wyc for spawn). */
int UnregisterFirstKindIdMatch(uint16_t kind, uint16_t id)
{
    for (int i = 0; i < g_update_table_count; ++i) {
        if (g_update_table[i].kind == kind && g_update_table[i].id == id) {
            for (int j = i; j < g_update_table_count - 1; ++j) {
                g_update_table[j] = g_update_table[j + 1];
            }
            --g_update_table_count;
            return 1;
        }
    }
    return 0;
}

/* Remove a specific entity pointer (only the first match) from the table.
 *
 * Used by ScriptCallDestroyEnt's per-entity destruction path so that
 * duplicates (e.g. Fjej's actor click payload at kind=4 id=2 AND a
 * script-spawned mask click payload also kind=4 id=2) don't get
 * co-destroyed when we want to remove only ONE.
 *
 * Returns 1 if removed, 0 if not found. */
int UnregisterEntityByPtr(Entity *e)
{
    if (!e) return 0;
    for (int r = 0; r < g_update_table_count; ++r) {
        if (g_update_table[r].e == e) {
            for (int j = r; j < g_update_table_count - 1; ++j) {
                g_update_table[j] = g_update_table[j + 1];
            }
            --g_update_table_count;
            return 1;
        }
    }
    return 0;
}
