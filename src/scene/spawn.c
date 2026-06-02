/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* src/scene/spawn.c — entity spawn (op 0x30) + actor pre-spawn.
 *
 * Two entry points:
 *
 *   - SpawnActorEntity: dedicated path for pre-spawning the two
 *     controllable actors (Ebek, Fjej) at game start. Builds the
 *     render entity, binds the atlas, and registers the (kind=2, id)
 *     + (kind=4, id) pair so FindEntityByVerbId can resolve verb-1 /
 *     verb-2 to the actor entities.
 *
 *   - ScriptCallSpawnEntity: op 0x30 dispatch — generic spawn path
 *     called by enter_scripts to populate scenes with props and NPCs.
 *     Allocates the entity, binds the atlas + bytecode resolved via
 *     the PE loader, optionally wires up a click payload from a click
 *     pool, and registers + links into the appropriate lists.
 */

#include "wacki.h"
#include "wacki/log.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern Entity *g_actor[2];
extern Entity *g_render_list_head;
extern Entity *g_click_list_head;
extern Entity *AllocEntity(uint16_t w, uint16_t h, uint16_t kind, uint16_t flags);
extern void    LinkEntityToList(Entity **head, Entity *e, int position);
extern void    RegisterEntityForUpdate(Entity *e, uint16_t kind, uint16_t id);
extern void   *FindUpdateRegistration(uint16_t kind, uint16_t id);
extern const void *xlat_binary_ptr(uint32_t addr);
extern const char *xlat_asset_name(uint32_t addr);
extern void   *xmalloc(uint32_t sz);

/* ---- constants ---------------------------------------------------- */

#define ASSET_KIND              1
#define ACTOR_KIND              2
#define CLICK_PAYLOAD_KIND      4

#define CLICK_KIND_SPRITE       1   /* descriptor.kind for op-0x30 sprites */

/* Spawn flag bits. */
#define SPAWN_FLAG_DOUBLED      0x0004   /* sprite drawn at 2× */
#define SPAWN_FLAG_NO_DOUBLE    0x0510   /* presence forces DOUBLED off */
#define SPAWN_FLAG_PRESET_BIT   0x0800
#define SPAWN_FLAG_ALPHA_PLANE  0x2000

#define ASSET_FLAG22_ALPHA_BIT  0x01

#define SPAWN_ALLOC_ALPHA       1        /* AllocEntity flags arg for alpha-plane */

/* Set on the actor entity at spawn so the per-entity VM applies the
 * foot-anchor compensation on its first tick. */
#define STATE_ANIM_ACTIVE_BIT   ESTATE_ANIM_ACTIVE   /* = 0x02 */

/* Entity byte +0x20 — a state byte set when SPAWN_FLAG_PRESET_BIT is
 * passed through op 0x30. The byte sits in the alpha-plane area. */
#define ENT_OFF_SPAWN_STATE     0x20
#define SPAWN_STATE_PRESET_VAL  1

/* Actor verb-table cache size: enough for the two pre-spawned actors. */
#define ACTOR_VERB_TAB_MAX      8

/* ---- helpers ------------------------------------------------------ */

/* Build a kind=1 sprite click payload referencing `owner`. Verb data
 * comes either from a 1-entry static table (actor spawn) or from a
 * script-supplied PE-resolved table (op 0x30 spawn). */
static Entity *build_sprite_click_payload(Entity *owner,
                                          const void *verb_table,
                                          uint16_t cached_verb)
{
    Entity *m = (Entity *)xmalloc(sizeof *m);
    if (!m) return NULL;
    memset(m, 0, sizeof *m);

    EOFF(m, CLICK_OFF_KIND,            uint16_t) = CLICK_KIND_SPRITE;
    EOFF(m, CLICK_OFF_OWNER_SLOT,      uint32_t) = ent_ptr_intern(owner);
    EOFF(m, CLICK_OFF_VERB_TABLE_SLOT, uint32_t) = verb_table
                                                   ? ent_ptr_intern((void *)verb_table)
                                                   : 0;
    EOFF(m, CLICK_OFF_CACHED_VERB,     uint16_t) = cached_verb;
    return m;
}

/* Pre-compensate the drawn position for an actor's initial frame so
 * it renders correctly before any walker tick fires. Mirrors the
 * post-exec foot-anchor block in ExecEntityScript (×1 path — actors
 * spawn with neither PERSPECTIVE nor DOUBLED flags set). */
static void preset_actor_drawn_position(Entity *e, AnimAsset *atlas,
                                        int16_t init_x, int16_t init_y,
                                        uint16_t init_frame)
{
    int16_t hot_x = 0, hot_y = 0;
    if (atlas->off_drawX && init_frame < atlas->frame_count) {
        hot_x = (int16_t)atlas->off_drawX[init_frame];
    }
    if (atlas->off_drawY && init_frame < atlas->frame_count) {
        hot_y = (int16_t)atlas->off_drawY[init_frame];
    }
    EOFF(e, ENT_OFF_DRAWN_X, int16_t) = (int16_t)(init_x + hot_x);
    EOFF(e, ENT_OFF_DRAWN_Y, int16_t) = (int16_t)(init_y + hot_y);

    /* foot_y for z-sort (= bottom edge of drawn sprite). */
    uint16_t sh = (atlas->off_heights && init_frame < atlas->frame_count)
                  ? atlas->off_heights[init_frame] : 0;
    EOFF(e, ENT_OFF_FOOT_Y, int16_t) =
        (int16_t)(EOFF(e, ENT_OFF_DRAWN_Y, int16_t) + (int16_t)sh);
}

/* ---- public entry points ------------------------------------------ */

Entity *SpawnActorEntity(uint16_t id, AnimAsset *atlas, uint16_t init_frame,
                         int16_t init_x, int16_t init_y)
{
    if (!atlas) return NULL;

    Entity *e = AllocEntity(atlas->max_w, atlas->max_h, /*kind=*/1, /*flags=*/0);
    if (!e) return NULL;

    /* Bind atlas; no per-entity script (actors are driven by direct
     * walker bind from the click handler, not by an enter-time
     * bytecode). */
    EOFF(e, ENT_OFF_BYTECODE_SLOT, uint32_t) = 0;
    EOFF(e, ENT_OFF_ATLAS_SLOT,    uint32_t) = ent_ptr_intern(atlas);
    EOFF(e, ENT_OFF_FRAME,         uint16_t) = init_frame;
    EOFF(e, ENT_OFF_ANCHOR_X,      uint16_t) = (uint16_t)init_x;
    EOFF(e, ENT_OFF_ANCHOR_Y,      uint16_t) = (uint16_t)init_y;
    EOFF8(e, ENT_OFF_STATE_FLAGS) |= STATE_ANIM_ACTIVE_BIT;

    preset_actor_drawn_position(e, atlas, init_x, init_y, init_frame);

    RegisterEntityForUpdate(e, ACTOR_KIND, id);
    LinkEntityToList(&g_render_list_head, e, 0);

    /* Click payload: verb table is a tiny static array per-actor so
     * FindEntityByVerbId(id) resolves to this entity at frame 0. */
    static uint16_t s_actor_verb_tab[ACTOR_VERB_TAB_MAX][2];
    if (id < ACTOR_VERB_TAB_MAX) {
        s_actor_verb_tab[id][0] = 1;        /* count */
        s_actor_verb_tab[id][1] = id;       /* verb_id at frame 0 */
    }
    Entity *m = build_sprite_click_payload(
        e,
        id < ACTOR_VERB_TAB_MAX ? s_actor_verb_tab[id] : NULL,
        id);
    if (m) {
        LinkEntityToList(&g_click_list_head, m, 0);
        RegisterEntityForUpdate(m, CLICK_PAYLOAD_KIND, id);
    }

    LOG_INFO("actor", "spawn id=%u atlas=%s frame=%u at (%d,%d)", id, atlas->name, init_frame, init_x, init_y);
    return e;
}

void ScriptCallSpawnEntity(uint16_t id, uint16_t flags,
                           uint32_t click_payload_addr,
                           uint32_t script_addr)
{
    AnimAsset *asset = (AnimAsset *)FindUpdateRegistration(ASSET_KIND, id);
    if (!asset) {
        LOG_TRACE("script", "spawn id=%u: no asset registered (skipping)", id);
        return;
    }

    /* SPAWN_FLAG_NO_DOUBLE bits override the DOUBLED bit if set. */
    if (flags & SPAWN_FLAG_NO_DOUBLE) flags &= (uint16_t)~SPAWN_FLAG_DOUBLED;

    /* Alpha-plane gate: any of three conditions promotes the entity
     * to alpha allocation. We use asset->flag_22 directly because
     * asset->kind collapses bits 0 and 1 of the raw flag. */
    uint16_t alloc_flags = 0;
    if ((asset->flag_22 & ASSET_FLAG22_ALPHA_BIT) ||
        (flags & SPAWN_FLAG_ALPHA_PLANE) ||
        (flags & SPAWN_FLAG_DOUBLED))
    {
        alloc_flags = SPAWN_ALLOC_ALPHA;
    }

    uint16_t w = asset->max_w;
    uint16_t h = asset->max_h;
    if (flags & SPAWN_FLAG_DOUBLED) {
        w = (uint16_t)(w << 1);
        h = (uint16_t)(h << 1);
    }

    Entity *e = AllocEntity(w, h, /*kind=*/1, alloc_flags);
    if (!e) return;

    /* Op 0x30: OR script flags into entity flags1+flags2; if the
     * preset bit is set, also seed a byte at +0x20. */
    EOFF(e, ENT_OFF_FLAGS1, uint16_t) |= flags;
    if (flags & SPAWN_FLAG_PRESET_BIT) {
        EOFF8(e, ENT_OFF_SPAWN_STATE) = SPAWN_STATE_PRESET_VAL;
    }

    /* Bind asset + bytecode through the slot-ID intern table. No
     * frame/anchor pre-init — the per-entity script's first tick
     * sets position via SET_ANCHOR; pre-setting from atlas drawX/drawY
     * here would DOUBLE the offset for static (script-less) entities. */
    const void *bc = xlat_binary_ptr(script_addr);
    EOFF(e, ENT_OFF_BYTECODE_SLOT, uint32_t) = bc ? ent_ptr_intern((void *)bc) : 0;
    EOFF(e, ENT_OFF_ATLAS_SLOT,    uint32_t) = ent_ptr_intern(asset);

    RegisterEntityForUpdate(e, ACTOR_KIND, id);
    LinkEntityToList(&g_render_list_head, e, 0);

    if (click_payload_addr) {
        const void *payload = xlat_binary_ptr(click_payload_addr);
        Entity     *m       = build_sprite_click_payload(e, payload, 0);
        if (m) {
            LinkEntityToList(&g_click_list_head, m, 0);
            RegisterEntityForUpdate(m, CLICK_PAYLOAD_KIND, id);
        }
    }

    LOG_TRACE("script", "spawn id=%u asset=%p script=0x%08x flags=0x%04x → %p", id, (void *)asset, script_addr, flags, (void *)e);
}
