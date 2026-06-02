/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* src/actor/list.c — entity render / click list management.
 *
 * The engine maintains two parallel linked lists:
 *
 * render list — every drawable entity, walked once per tick to
 * advance per-entity scripts (EntityWalkerTick) and
 * again to paint into the back buffer (EntityRenderAll).
 * click list — clickable masks + sprite payloads, walked by the
 * hit-test (ClickHitTest) to resolve cursor → verb_id.
 *
 * The original engine stored next/prev pointers INSIDE Entity at bytes
 * +0/+4. On a 64-bit host that would overflow into flags1/flags2 and
 * corrupt the entity. Instead we keep side-band parallel arrays
 * (g_render_list_tbl, g_click_list_tbl) with the same observable
 * semantics. The "head" globals are kept up-to-date so external code
 * that took `&g_render_list_head` as a list identity (e.g.
 * LinkEntityToList) still routes correctly.
 *
 * Scene transitions: EntityListClearAll preserves the two actor
 * entities (g_actor[0]/[1]) and their click payloads across scene
 * boundaries — actors are spawned once at game start, not per-scene.
 * Walker state on the actors is explicitly reset so mid-walk state from
 * the previous scene doesn't conflict with the new scene's entry chain.
 */

#include "wacki.h"
#include "entity_offsets.h"
#include "internal.h"

#include <stddef.h>
#include <stdint.h>

/* Globals exposed via wacki.h — the linked-list "identity" handles used
 * by the rest of the engine. */
Entity *g_render_list_head = NULL;
Entity *g_click_list_head  = NULL;

#define ENT_LIST_CAP 256

/* Side-band storage. Tables are file-static; the rest of the engine
 * goes through Entity*ListFirst / At / Count for read access and
 * Link / Unlink / ClearAll for mutation. */
static struct ent_list_tbl {
    Entity *entities[ENT_LIST_CAP];
    int     count;
} g_render_list_tbl, g_click_list_tbl;

static int ent_list_index(Entity *e, const Entity *const *arr, int n)
{
    for (int i = 0; i < n; ++i) {
        if (arr[i] == e) return i;
    }
    return -1;
}

void LinkEntityToList(Entity **head, Entity *e, int position)
{
    /* Routing: which side-band table does this `head` identity refer to? */
    struct ent_list_tbl *t = (head == &g_render_list_head)
                             ? &g_render_list_tbl
                             : &g_click_list_tbl;
    if (t->count >= ENT_LIST_CAP) return;

    if (position < 0)             position = 0;
    if (position > t->count)      position = t->count;

    for (int i = t->count; i > position; --i) {
        t->entities[i] = t->entities[i - 1];
    }
    t->entities[position] = e;
    ++t->count;
    if (position == 0) *head = e;
}

void UnlinkEntity(Entity *e)
{
    if (!e) return;
    for (int pass = 0; pass < 2; ++pass) {
        struct ent_list_tbl *t  = pass ? &g_click_list_tbl : &g_render_list_tbl;
        Entity             **hp = pass ? &g_click_list_head : &g_render_list_head;

        int idx = ent_list_index(e, (const Entity *const *)t->entities, t->count);
        if (idx < 0) continue;

        for (int i = idx; i < t->count - 1; ++i) {
            t->entities[i] = t->entities[i + 1];
        }
        --t->count;
        *hp = t->count ? t->entities[0] : NULL;
    }
}

/* ---- iterators ----------------------------------------------------- */

Entity *EntityListFirst(int click_list)
{
    struct ent_list_tbl *t = click_list ? &g_click_list_tbl : &g_render_list_tbl;
    return t->count ? t->entities[0] : NULL;
}

Entity *EntityListAt(int click_list, int idx)
{
    struct ent_list_tbl *t = click_list ? &g_click_list_tbl : &g_render_list_tbl;
    return (idx >= 0 && idx < t->count) ? t->entities[idx] : NULL;
}

int EntityListCount(int click_list)
{
    return click_list ? g_click_list_tbl.count : g_render_list_tbl.count;
}

/* ---- scene-transition clear --------------------------------------- *
 *
 * Preserve across scene boundary:
 * - render list: actor entries (g_actor[0]/[1])
 * - click list: click payloads owned by an actor
 * - update table: actor entries (kind=2) + their click payloads (kind=4)
 * - intern table: NOT reset (surviving entities' slots stay valid)
 *
 * The kind=4 id=1/2 click payload filter is owner-based, NOT id-based.
 * Per-scene masks (e.g. komnata 4 wejw_lpg.msk) register at id=1/2 too;
 * a naive id-only filter would carry stale door-exit hitboxes from
 * komnata 4 into komnata 5 and the cursor would report "exit available"
 * over empty floor.
 */

/* The click payload's owner slot (= CLICK_OFF_OWNER_SLOT in
 * entity_offsets.h) is an intern handle for the sprite/mask that the
 * payload "belongs to" — looked up via ent_ptr_resolve to compare
 * against g_actor[0]/[1]. */
#define ACTOR_KIND_SPRITE 2     /* update_table.kind for actor entity */
#define ACTOR_KIND_CLICK  4     /* update_table.kind for actor click payload */
#define ACTOR_ID_EBEK     1
#define ACTOR_ID_FJEJ     2

#define MAX_PROTECTED_CLICK_PAYLOADS 8

void EntityListClearAll(void)
{
    extern Entity *g_actor[2];
    extern Entity *g_speech_balloon;

    /* Snapshot which click payloads we want to keep — kind=4 entries
 * whose owner pointer resolves to one of the two actor entities. */
    Entity *keep_click[MAX_PROTECTED_CLICK_PAYLOADS];
    int     keep_n = 0;
    for (int r = 0;
         r < g_update_table_count && keep_n < MAX_PROTECTED_CLICK_PAYLOADS;
         ++r)
    {
        if (g_update_table[r].kind != ACTOR_KIND_CLICK) continue;
        if (g_update_table[r].id   != ACTOR_ID_EBEK &&
            g_update_table[r].id   != ACTOR_ID_FJEJ) continue;

        Entity *e = g_update_table[r].e;
        if (!e) continue;

        uint32_t owner_slot = EOFF(e, CLICK_OFF_OWNER_SLOT, uint32_t);
        void    *owner      = owner_slot ? ent_ptr_resolve(owner_slot) : NULL;
        if (owner != g_actor[0] && owner != g_actor[1]) continue;

        keep_click[keep_n++] = e;
    }

    /* Render list: keep only actor entries. */
    int w = 0;
    for (int r = 0; r < g_render_list_tbl.count; ++r) {
        Entity *e = g_render_list_tbl.entities[r];
        if (e == g_actor[0] || e == g_actor[1]) {
            g_render_list_tbl.entities[w++] = e;
        }
    }
    g_render_list_tbl.count = w;
    g_render_list_head      = w ? g_render_list_tbl.entities[0] : NULL;

    /* Click list: keep actor-owned payloads (snapshotted above). */
    w = 0;
    for (int r = 0; r < g_click_list_tbl.count; ++r) {
        Entity *e = g_click_list_tbl.entities[r];
        for (int i = 0; i < keep_n; ++i) {
            if (e == keep_click[i]) {
                g_click_list_tbl.entities[w++] = e;
                break;
            }
        }
    }
    g_click_list_tbl.count = w;
    g_click_list_head      = w ? g_click_list_tbl.entities[0] : NULL;

    /* Update table: keep actor entries (kind=2) and their click
 * payloads (kind=4). Same owner-check reasoning as keep_click[]:
 * id=1/2 also gets used by per-scene masks, so we have to
 * disambiguate by entity identity, not just the id field. */
    w = 0;
    for (int r = 0; r < g_update_table_count; ++r) {
        int keep = 0;
        if (g_update_table[r].kind == ACTOR_KIND_SPRITE &&
            (g_update_table[r].e == g_actor[0] ||
             g_update_table[r].e == g_actor[1]))
        {
            keep = 1;
        } else if (g_update_table[r].kind == ACTOR_KIND_CLICK) {
            for (int i = 0; i < keep_n; ++i) {
                if (g_update_table[r].e == keep_click[i]) {
                    keep = 1;
                    break;
                }
            }
        }
        if (keep) g_update_table[w++] = g_update_table[r];
    }
    g_update_table_count = w;

    /* Walker-state sync: clear mid-walk state on the surviving actors
     * so the new scene's entry-chain starts from a known idle state.
     * Anchor (ANCHOR_X / ANCHOR_Y) and atlas (ATLAS_SLOT) are
     * intentionally left untouched. */
    for (int i = 0; i < 2; ++i) {
        Entity *a = g_actor[i];
        if (!a) continue;

        EOFF(a, ENT_OFF_PC,            uint16_t) = 0;
        EOFF(a, ENT_OFF_LOOP_A,        uint16_t) = 0;
        EOFF(a, ENT_OFF_LOOP_B,        uint16_t) = 0;
        EOFF(a, ENT_OFF_LOOP_C,        uint16_t) = 0;
        EOFF8(a, ENT_OFF_STATE_FLAGS) &= (uint8_t)~(
            ESTATE_FRAME_READY | ESTATE_WALKER_FRESH);
        EOFF(a, ENT_OFF_DELAY,         uint16_t) = 0;
        EOFF(a, ENT_OFF_DELAY_RESET,   uint16_t) = 0;
        EOFF(a, ENT_OFF_LOOP_D,        uint16_t) = 0;
        EOFF(a, ENT_OFF_LOOP_E,        uint16_t) = 0;
        EOFF(a, ENT_OFF_WALKER_X,      uint32_t) = 0;
        EOFF(a, ENT_OFF_WALKER_Y,      uint32_t) = 0;
        EOFF(a, ENT_OFF_WALKER_DX_REM, uint32_t) = 0;
        EOFF(a, ENT_OFF_WALKER_DY_REM, uint32_t) = 0;
    }

    /* Speech balloon (kind=1 entity) is per-scene scenery, not persisted. */
    g_speech_balloon = NULL;
}
