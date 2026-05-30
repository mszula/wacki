/* src/script_bridge/entity.c — script-side entity manipulation bridges.
 *
 * Opcode bridges that operate on an existing entity (asset, sprite, or
 * mask) by its (kind, id) registration:
 *
 * ScriptCallLoadAsset (op 0x2D): load an atlas from a DTA file and
 * register it under (kind=1, id) for subsequent SPAWN / mask
 * lookups.
 * ScriptCallDestroyEnt (op 0x31 / 0x32): remove an entity from the
 * lists + free its storage. Op 0x31 also unregisters the asset.
 * ScriptCallEnableEnt (op via reg_id): set bit 0x80 of flags1 to
 * enable/disable the entity.
 * ScriptCallHideEnt / ShowEnt (ops 0x3E / 0x3F): flip flag bit 0x80.
 * ScriptCallWalkMode (op 0x35 / 0x37): set the entity's walker mode.
 * ScriptCallWalkTo (op 0x38 / 0x3A): bind a walker to a target.
 * ScriptCallAttachProp (op 0x3B / 0x3C): attach a prop to an actor's
 * hand for the next animation cycle.
 */

#include "wacki.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern Entity *g_render_list_head;
extern Entity *g_click_list_head;
extern Entity *g_actor[2];
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

/* ----:
 * LoadAssetFromDtaBase(name) + RegisterEntityForUpdate(asset, kind=1, id)
 * The asset itself is the "table-look-up payload"; the actual entity is
 * spawned later by opcode 0x30 (which finds the asset via (1,id)).
 */
void ScriptCallLoadAsset(const char *name, uint16_t id)
{
    if (!name) return;
    /* → → : if an asset
 * already occupies this slot, freeing it stops all wavs in its
 * SampleTable list ( per entry). We don't yet free
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

/* — DESTROY entities matching (id) across kinds
 * 2, 3, 4 (render/click/mask). Originally invoked by opcodes 0x31 + 0x32:
 *
 * case 0x31: (id, '\x01'); // also unregisters kind 1
 * case 0x32: (id, '\0');
 *
 * The original walks the update table, finds each matching entity,
 * calls (unlink + free), then loops until no more match
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
 * prologue calling (id, 1, -1)
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
    /* NOTE (Bug 3 fix #17 — refer ): protect actor
 * entities + their click payloads from script destroy. Scripts collide
 * actor IDs (1=Ebek, 2=Fjej) with regular mask/asset IDs via `op
 * 0x2C/0x2D load-asset id=N`; e.g. kiosk21 enter_script loads
 * `kiosk21.msk` as id=2, then `op 0x31 DESTROY id=2` unloads the mask
 * AND (in original ) would also wipe Fjej's click payload. We skip
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

/* @ Ghidra case 0x3e:
 * if (iVar20) *(byte *)(iVar20 + 8) |= 0x80;
 *
 * Earlier port iterated kinds 2/3/4 via FindUpdateRegistration — too
 * broad, would hide click-payload and walk-behind entries that the
 * original leaves alone. The proper lookup is FindEntityByVerbId
 * which returns the single owner render entity.
 *
 * NOTE:
 * Original verb scripts call op 0x3E on BOTH controllable actors during
 * many action sequences:
 * - active actor (the one doing the action): hide so a spawned
 * action-overlay sprite shows cleanly at the actor's anchor
 * without the idle pose double-rendering underneath
 * - partner actor: also hide, to keep the cinematic frame focused
 * on the action (original "stage clearing" convention)
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
/* @ Ghidra case 0x3f:
 * if (iVar20) *(ushort *)(iVar20 + 8) &= 0xff7f;
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

/* ScriptCallWalkMode (op 0x35/0x37) —
 * Despite the legacy "WalkMode" name in our port, the original is a
 * SAVE-STATE call: snapshot one field of entity `id` into a 20-slot
 * table keyed by (id, mode). (WalkTo) restores from it.
 *
 * find slot where (slot.id == id && slot.mode == mode), or append;
 * } */
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

/* ScriptCallWalkTo (op 0x38/0x3A) —
 * RESTORE-STATE: searches the save table for (target_id, mode) slot,
 * then writes its data back into entity `verb_id`.
 * } */
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
                /* reset on asset swap. */
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

/* ScriptCallAttachProp
 *
 * (binder): writes new atlas ptr into entity[+0x28] and
 * resets the per-entity script pc + delay counters. is the
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
        /* — re-bind atlas + reset per-script state:
 * e[+0x3C] = 0; // sub-delay accumulator */
        uint8_t *eb = (uint8_t *)e;
        *(uint32_t *)(eb + 0x28) = ent_ptr_intern((void *)a);
        *(uint16_t *)(eb + 0x32) = 0;
        *(uint16_t *)(eb + 0x36) = 0;
        *(uint16_t *)(eb + 0x3C) = 0;
    }
    if (!keep) {
        /* — full reset (op 0x3B path). Mirrors the inline
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
