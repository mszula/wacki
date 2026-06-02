/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* src/script_bridge/entity.c — script-side entity manipulation bridges.
 *
 * Opcode bridges that operate on an existing entity (asset, sprite, or
 * mask) by its (kind, id) registration:
 *
 *   ScriptCallLoadAsset  (op 0x2D): load an atlas from a DTA file and
 *                                   register it under (kind=1, id) for
 *                                   subsequent SPAWN / mask lookups.
 *   ScriptCallDestroyEnt (op 0x31 / 0x32): remove an entity from the
 *                                   lists + free its storage. Op 0x31
 *                                   also unregisters the asset.
 *   ScriptCallEnableEnt  (legacy):  routes to DestroyEnt.
 *   ScriptCallHideEnt    (op 0x3E): set the hidden flag on flags1.
 *   ScriptCallShowEnt    (op 0x3F): clear hidden + wait-for-enable flags.
 *   ScriptCallWalkMode   (op 0x35 / 0x37): SAVE state into 20-slot table.
 *   ScriptCallWalkTo     (op 0x38 / 0x3A): RESTORE state from the table.
 *   ScriptCallAttachProp (op 0x3B / 0x3C): attach a prop atlas to an
 *                                   actor for the next animation cycle.
 */

#include "wacki.h"
#include "wacki/log.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern Entity *g_render_list_head;
extern Entity *g_click_list_head;
extern Entity *g_actor[2];
extern uint16_t g_active_actor;

extern void    LinkEntityToList(Entity **head, Entity *e, int position);
extern void    UnlinkEntity(Entity *e);
extern void    RegisterEntityForUpdate(Entity *e, uint16_t kind, uint16_t id);
extern void    UnregisterEntityForUpdate(uint16_t kind, uint16_t id);
extern int     UnregisterFirstKindIdMatch(uint16_t kind, uint16_t id);
extern int     UnregisterEntityByPtr(Entity *e);
extern void   *FindUpdateRegistration(uint16_t kind, uint16_t id);
extern void   *FindUpdateRegistrationExcept(uint16_t kind, uint16_t id,
                                            Entity *const *skip, int nskip);
extern Entity *FindEntityByVerbId(uint16_t verb);
extern void    FreeEntity(Entity *e);
extern void    xfree(void *p);
extern int     BindActorWalker(int actor_idx, int target_x, int target_y);
extern const void *xlat_binary_ptr(uint32_t addr);
extern void   *ent_ptr_resolve(uint32_t slot);
extern void    StopAllSfxForAsset(const char *asset_name);

/* ---- module constants --------------------------------------------- */

#define ASSET_KIND              1       /* update-table kind for AnimAsset registrations */
#define MAX_PROTECTED_DESTROY   8       /* per-kind cap of actor-protected entries */

/* (kind, id) pairs the destroy walk iterates. Click payloads (kind=4)
 * referencing actors at id=1/2 require special handling — see notes
 * inline. */
#define DESTROY_KIND_FIRST      2
#define DESTROY_KIND_LAST       4

#define CLICK_KIND              4
#define ACTOR_ID_EBEK           1
#define ACTOR_ID_FJEJ           2

/* Hide/Show flag bits on entity[+0x08]. */
#define ENT_FLAG_HIDDEN_BIT     0x0080
#define ENT_FLAG_SHOW_MASK      0x2080  /* HIDDEN | WAIT_FOR_ENABLE */

/* ScriptCallWalkMode/WalkTo backing-store sizing. */
#define SAVE_SLOT_MAX           20
#define SAVE_MODE_XY_PACK       1       /* data = low: X, high: Y */
#define SAVE_MODE_ATLAS         2       /* data = atlas intern slot */

/* Walker-state reset clears state bits 0 + 2 (frame-ready + walker-fresh). */
#define WALKER_RESET_BITS       (ESTATE_FRAME_READY | ESTATE_WALKER_FRESH)

/* ---- helpers ------------------------------------------------------ */

/* Clear an entity's walker / loop / delay state so it stops mid-action
 * and is ready to be re-bound to a new bytecode or atlas. Used by
 * SUBSCRIPT_CALL, SWAP_ATLAS, ATTACH_PROP, and the WalkTo restore path. */
static void reset_entity_walker_state(Entity *e)
{
    EOFF(e, ENT_OFF_STATE_FLAGS,   uint16_t) &= (uint16_t)~WALKER_RESET_BITS;
    EOFF(e, ENT_OFF_LOOP_A,        uint16_t) = 0;
    EOFF(e, ENT_OFF_LOOP_B,        uint16_t) = 0;
    EOFF(e, ENT_OFF_LOOP_C,        uint16_t) = 0;
    EOFF(e, ENT_OFF_LOOP_D,        uint16_t) = 0;
    EOFF(e, ENT_OFF_LOOP_E,        uint16_t) = 0;
    EOFF(e, ENT_OFF_DELAY,         uint16_t) = 0;
    EOFF(e, ENT_OFF_PC,            uint16_t) = 0;
    EOFF(e, ENT_OFF_WALKER_DX_REM, uint32_t) = 0;
    EOFF(e, ENT_OFF_WALKER_DY_REM, uint32_t) = 0;
}

/* Is the (kind, id) entry pointing at an entity we must protect across
 * a destroy walk (the two controllable actors and their click payloads)? */
static int is_protected_actor_entry(Entity *e, uint16_t k, uint16_t id)
{
    if (e == g_actor[0] || e == g_actor[1]) return 1;
    if (k != CLICK_KIND) return 0;
    if (id != ACTOR_ID_EBEK && id != ACTOR_ID_FJEJ) return 0;

    uint32_t owner_slot = EOFF(e, CLICK_OFF_OWNER_SLOT, uint32_t);
    if (!owner_slot) return 0;
    void *owner = ent_ptr_resolve(owner_slot);
    return owner == g_actor[0] || owner == g_actor[1];
}

/* ---- script bridges ----------------------------------------------- */

void ScriptCallLoadAsset(const char *name, uint16_t id)
{
    if (!name) return;

    /* If an asset already occupies this slot, freeing it stops all wavs
     * in its SampleTable. We don't yet free the AnimAsset itself (TBD),
     * but we MUST stop any looping SFX it owned so they don't keep
     * playing after the script switches away. */
    AnimAsset *prev = (AnimAsset *)FindUpdateRegistration(ASSET_KIND, id);
    if (prev && prev->name[0]) {
        StopAllSfxForAsset(prev->name);
    }

    AnimAsset *a = LoadAssetFromDtaBase(name);
    if (!a) {
        LOG_TRACE("script", "load-asset '%s' id=%u FAILED", name, id);
        return;
    }
    RegisterEntityForUpdate((Entity *)a, ASSET_KIND, id);
    LOG_TRACE("script", "load-asset '%s' id=%u (kind=%u frames=%u)", name, id, a->kind, a->frame_count);
}

void ScriptCallDestroyEnt(uint16_t id, int also_unreg_asset)
{
    int total_killed  = 0;
    int total_skipped = 0;

    /* Op 0x31 (also_unreg_asset=1) drops the OLDEST kind=1 registration
     * matching id, not all matches. Scripts that do
     *   load id=N (asset_A) → load id=N (asset_B) → destroy id=N (unreg=1)
     * expect asset_A to go and asset_B to remain for subsequent spawns. */
    if (also_unreg_asset) {
        UnregisterFirstKindIdMatch(ASSET_KIND, id);
    }

    /* NOTE: scripts collide actor ids (1=Ebek, 2=Fjej) with regular
     * mask ids — e.g. `load id=2 (kiosk21.msk)` then `destroy id=2`
     * shouldn't wipe Fjej's click payload. We skip protected entries
     * so subsequent SET_SCRIPT / SET_ENTITY_XY calls still find them.
     *
     * Use FindUpdateRegistrationExcept to skip already-inspected
     * protected entries — without it the next find returns the same
     * entry forever (infinite loop).
     *
     * Use UnregisterEntityByPtr (single entity) rather than
     * UnregisterEntityForUpdate(k, id) — the latter wipes all entries
     * matching the kind+id, defeating the per-entity protection. */
    for (uint16_t k = DESTROY_KIND_FIRST; k <= DESTROY_KIND_LAST; ++k) {
        Entity *protected[MAX_PROTECTED_DESTROY];
        int     nprot = 0;
        for (;;) {
            Entity *e = (Entity *)FindUpdateRegistrationExcept(k, id, protected, nprot);
            if (!e) break;

            if (is_protected_actor_entry(e, k, id)) {
                if (nprot < MAX_PROTECTED_DESTROY) protected[nprot++] = e;
                ++total_skipped;
                continue;
            }

            /* Stop any looping [sampl] SFX this entity's atlas owns
             * before freeing the entity. Without this, the wav keeps
             * playing forever — sfx_handle_end_frames only fires on
             * the entity's own frame ticks, and once the entity is
             * gone nothing ever increments past frame_end. Triggered
             * by the user-reported "skateboard skid sound kept
             * looping after the boy crashed". */
            AnimAsset *atlas = (AnimAsset *)ent_ptr_resolve(
                EOFF(e, ENT_OFF_ATLAS_SLOT, uint32_t));
            if (atlas && atlas->name[0]) {
                StopAllSfxForAsset(atlas->name);
            }

            UnregisterEntityByPtr(e);
            UnlinkEntity(e);
            xfree(e);
            ++total_killed;
        }
    }
    LOG_TRACE("script", "destroy id=%u killed=%d skipped=%d (asset_unreg=%d)", id, total_killed, total_skipped, also_unreg_asset);
}

void ScriptCallEnableEnt(uint16_t id, int enable)
{
    /* Legacy name kept for game.c callers. The `enable` flag maps to:
     *   1 (op 0x31) = destroy + unregister asset
     *   0 (op 0x32) = destroy only */
    ScriptCallDestroyEnt(id, enable ? 1 : 0);
}

/* Direction-flag helper for the hide / show logging. */
static const char *actor_label_for_index(int idx)
{
    switch (idx) {
    case 0:  return "Ebek";
    case 1:  return "Fjej";
    case -1: return "non-actor";
    default: return "no-entity";
    }
}

void ScriptCallHideEnt(uint16_t id)
{
    /* NOTE: scripts call op 0x3E on BOTH controllable actors during
     * many action sequences:
     *   - active actor: hide so a spawned action-overlay sprite shows
     *     cleanly at the actor's anchor without the idle pose
     *     double-rendering underneath
     *   - partner actor: also hide in the original, to clear the
     *     cinematic frame; modern UX prefers the partner to remain
     *     visible (user-reported "druga postać znika"), so we suppress
     *     the partner-hide. */
    Entity *e = FindEntityByVerbId(id);
    if (!e) {
        LOG_TRACE("hide", "id=0x%04X — no entity (active=%u)", id, (unsigned)g_active_actor);
        return;
    }

    int target_idx  = (e == g_actor[0]) ? 0 : (e == g_actor[1]) ? 1 : -1;
    int partner_idx = (int)((g_active_actor & 1u) ^ 1u);
    int suppressed  = (target_idx == partner_idx);

    LOG_TRACE("hide", "id=0x%04X target=%s active=%s → %s", id, actor_label_for_index(target_idx), (g_active_actor & 1u) ? "Fjej" : "Ebek", suppressed ? "SUPPRESSED" : "HIDDEN");

    if (suppressed) return;
    EOFF8(e, ENT_OFF_FLAGS1) |= (uint8_t)ENT_FLAG_HIDDEN_BIT;
}

void ScriptCallShowEnt(uint16_t id)
{
    Entity *e = FindEntityByVerbId(id);
    int target_idx = e ? ((e == g_actor[0]) ? 0
                          : (e == g_actor[1]) ? 1 : -1)
                       : -2;
    LOG_INFO("show", "id=0x%04X target=%s active=%s", id, actor_label_for_index(target_idx), (g_active_actor & 1u) ? "Fjej" : "Ebek");

    if (e) {
        /* Clear both HIDDEN and the alpha-plane WAIT_FOR_ENABLE bit so
         * scripts can use op 0x3F to reveal pending entities. */
        EOFF(e, ENT_OFF_FLAGS1, uint16_t) &= (uint16_t)~ENT_FLAG_SHOW_MASK;
    }
}

/* ---- save / restore state table (op 0x35/0x37 + 0x38/0x3A) -------- */

typedef struct SaveSlot {
    uint16_t id;
    uint16_t mode;
    uint32_t data;        /* mode 1: low=X high=Y, mode 2: atlas intern slot */
} SaveSlot;

static SaveSlot g_save_slots[SAVE_SLOT_MAX];
static int      g_save_slot_count = 0;

/* Find existing (id, mode) slot, or return -1 if append needed. */
static int find_save_slot(uint16_t id, uint16_t mode)
{
    for (int i = 0; i < g_save_slot_count; ++i) {
        if (g_save_slots[i].id == id && g_save_slots[i].mode == mode) return i;
    }
    return -1;
}

void ScriptCallWalkMode(uint16_t id, int mode)
{
    if (g_save_slot_count >= SAVE_SLOT_MAX) return;

    int slot = find_save_slot(id, (uint16_t)mode);
    int appending = (slot < 0);
    if (appending) slot = g_save_slot_count;

    Entity *e = FindEntityByVerbId(id);
    if (!e) return;

    if (mode == SAVE_MODE_XY_PACK) {
        uint16_t x = EOFF(e, ENT_OFF_ANCHOR_X, uint16_t);
        uint16_t y = EOFF(e, ENT_OFF_ANCHOR_Y, uint16_t);
        g_save_slots[slot].data = ((uint32_t)y << 16) | x;
    } else if (mode == SAVE_MODE_ATLAS) {
        g_save_slots[slot].data = EOFF(e, ENT_OFF_ATLAS_SLOT, uint32_t);
    }
    g_save_slots[slot].id   = id;
    g_save_slots[slot].mode = (uint16_t)mode;
    if (appending) ++g_save_slot_count;
}

void ScriptCallWalkTo(uint16_t verb_id, uint16_t target_id, int mode)
{
    int slot = find_save_slot(target_id, (uint16_t)mode);
    if (slot < 0) return;

    Entity *e = FindEntityByVerbId(verb_id);
    if (!e) return;

    if (mode == SAVE_MODE_XY_PACK) {
        EOFF(e, ENT_OFF_ANCHOR_X, uint16_t) = (uint16_t)(g_save_slots[slot].data & 0xFFFF);
        EOFF(e, ENT_OFF_ANCHOR_Y, uint16_t) = (uint16_t)(g_save_slots[slot].data >> 16);
    } else if (mode == SAVE_MODE_ATLAS) {
        reset_entity_walker_state(e);
        EOFF(e, ENT_OFF_ATLAS_SLOT, uint32_t) = g_save_slots[slot].data;
    }
}

/* ---- attach-prop --------------------------------------------------- */

void ScriptCallAttachProp(uint16_t actor, uint16_t prop, int keep)
{
    Entity *e = FindEntityByVerbId(actor);
    if (!e) return;

    void *a = FindUpdateRegistration(ASSET_KIND, prop);
    if (a) {
        /* Re-bind atlas + reset per-script timing state. */
        EOFF(e, ENT_OFF_ATLAS_SLOT, uint32_t) = ent_ptr_intern(a);
        EOFF(e, ENT_OFF_PC,         uint16_t) = 0;
        EOFF(e, ENT_OFF_LOOP_B,     uint16_t) = 0;
        EOFF(e, ENT_OFF_DELAY,      uint16_t) = 0;
    }
    if (!keep) {
        /* Op 0x3B path: full walker-state reset (same shape as op 0x1E
         * SUBSCRIPT_CALL and op 0x23 SWAP_ATLAS). */
        reset_entity_walker_state(e);
    }
    LOG_TRACE("script", "attach-prop actor=0x%04X prop=0x%04X keep=%d %s", actor, prop, keep, a ? "ok" : "no-asset");
}
