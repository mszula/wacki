/* src/hud/inventory.c — player inventory + panel page rotation.
 *
 * The player's inventory is a flat array of 60 verb_ids (`uint16_t`),
 * shared storage with the save-game scene snapshot (so saving a game
 * also captures inventory contents). Slot value 0x26 means "empty";
 * any other value in the verb-id range 0x29..0xB6 is the panel verb
 * for the item held there.
 *
 * Slots are presented six-at-a-time on the HUD verb panel. The current
 * page index (`g_panel_page_idx`) selects which six slots are visible.
 * Page navigation:
 *
 *   - PagePrev: scroll left (does nothing on page 0)
 *   - PageNext: scroll to the next page that contains at least one
 *     non-empty slot, capped at page 9 (= 10 pages × 6 = 60 slots)
 *   - PageCollapse: after removing an item, drop back to the rightmost
 *     non-empty page so the panel doesn't show a blank row
 *
 * Add/Remove/Has are straightforward: linear scan over the 60 slots.
 * Drop is special — it parks the item entity off-screen and strips its
 * bytecode so the renderer + per-entity VM stop touching it.
 *
 * Dialog mode reuses the same storage: op 0x19 fills 6 slots with
 * dialog choice verbs, op 0x1A page-swaps them onto the panel, and the
 * user-click then routes through the standard DispatchClickEvent flow.
 */

#include "wacki.h"

#include <stdint.h>
#include <string.h>

extern AnimAsset *g_items_atlas;
extern Entity   *FindEntityByVerbId(uint16_t verb);

/* Globals owned by other modules but referenced here. */
extern uint16_t g_settings_anim_active;
extern uint16_t g_panel_verb_tab[6];
extern uint32_t g_scene_snapshot[0x1E];     /* backing storage for inventory */

#define INVENTORY_SLOT_COUNT       60
#define INVENTORY_PAGE_SIZE        6
#define INVENTORY_LAST_PAGE_INDEX  9              /* page 0..9 → 60 slots */
#define INVENTORY_EMPTY_SLOT       0x26

#define ITEM_VERB_FIRST            0x29
#define ITEM_VERB_RANGE            0x8E           /* 142 distinct items */

/* Entity-state layout (per-item, 8-byte stride indexed by item-0x29):
 *   +0  u16  panel_verb_id  (what shows up in inventory slot)
 *   +2  u16  in_inventory   (0 = no, 0xFFFF = yes)
 *   +4  u16  state_a
 *   +6  u16  state_b */
#define ENTITY_STATE_STRIDE_U16    4              /* 8 bytes = 4 u16s */
#define ENTITY_STATE_OFF_VERB      0
#define ENTITY_STATE_OFF_IN_INV    1
#define ENTITY_STATE_IN_INV_YES    0xFFFFu

#define PANEL_VISIBLE_BIT          0x0001

/* Drop-an-item parks the item entity off-screen at this coordinate so
 * it stops drawing without needing to be destroyed. */
#define INV_DROP_OFFSCREEN         1000

/* Entity field offsets used by InventoryDropItem. */
#define ENT_OFF_DRAWN_X            0x0A
#define ENT_OFF_ANCHOR_X           0x22
#define ENT_OFF_BYTECODE_SLOT      0x2C

/* ---- module state -------------------------------------------------- */

uint16_t g_panel_page_idx                 = 0;
uint16_t g_panel_verb_tab_backup[6]       = {
    0x26, 0x26, 0x26, 0x26, 0x26, 0x26,
};
uint8_t  g_panel_redraw                   = 0;

/* ---- helpers ------------------------------------------------------- */

/* Returns 1 if a verb_id is in the item-verb range, 0 otherwise. */
static int is_item_verb(uint16_t verb)
{
    return (uint16_t)(verb - ITEM_VERB_FIRST) < ITEM_VERB_RANGE;
}

/* ---- public API ---------------------------------------------------- */

uint16_t *Inventory(void)
{
    return (uint16_t *)g_scene_snapshot;
}

void ResetInventory(void)
{
    uint16_t *inv = Inventory();
    for (int i = 0; i < INVENTORY_SLOT_COUNT; ++i) inv[i] = INVENTORY_EMPTY_SLOT;
    g_panel_page_idx = 0;
}

void PanelPageSwap(void)
{
    if (!(g_settings_anim_active & PANEL_VISIBLE_BIT) || !g_items_atlas) return;

    uint16_t *inv       = Inventory();
    int       page_base = g_panel_page_idx * INVENTORY_PAGE_SIZE;
    for (int i = 0; i < INVENTORY_PAGE_SIZE; ++i) {
        g_panel_verb_tab_backup[i] = g_panel_verb_tab[i];
        g_panel_verb_tab[i]        = inv[page_base + i];
    }
    g_panel_redraw = 1;
}

int InventoryPagePrev(void)
{
    if (g_panel_page_idx == 0) return 0;
    --g_panel_page_idx;
    return 1;
}

int InventoryPageNext(void)
{
    if (g_panel_page_idx >= INVENTORY_LAST_PAGE_INDEX) return 0;

    uint16_t *inv    = Inventory();
    int       cursor = (g_panel_page_idx + 1) * INVENTORY_PAGE_SIZE;
    while (cursor < INVENTORY_SLOT_COUNT) {
        if (inv[cursor] != INVENTORY_EMPTY_SLOT) {
            ++g_panel_page_idx;
            return 1;
        }
        ++cursor;
    }
    return 0;
}

void InventoryPageCollapse(void)
{
    if (g_panel_page_idx == 0) return;

    uint16_t *inv = Inventory();
    while (g_panel_page_idx != 0) {
        int base  = g_panel_page_idx * INVENTORY_PAGE_SIZE;
        int empty = 1;
        for (int i = 0; i < INVENTORY_PAGE_SIZE; ++i) {
            if (inv[base + i] != INVENTORY_EMPTY_SLOT) { empty = 0; break; }
        }
        if (!empty) return;
        --g_panel_page_idx;
    }
}

void InventorySetPageForItem(uint16_t item_verb)
{
    if (!is_item_verb(item_verb)) return;

    uint16_t *inv = Inventory();
    for (int i = 0; i < INVENTORY_SLOT_COUNT; ++i) {
        if (inv[i] == item_verb) {
            g_panel_page_idx = (uint16_t)(i / INVENTORY_PAGE_SIZE);
            return;
        }
    }
}

int InventoryAddItem(uint16_t item_verb)
{
    if (!is_item_verb(item_verb)) return 0;

    uint16_t *inv  = Inventory();
    int       slot = -1;

    /* Prefer the existing slot if the item is already on hand. */
    for (int i = 0; i < INVENTORY_SLOT_COUNT; ++i) {
        if (inv[i] == item_verb) { slot = i; break; }
    }
    /* Otherwise the first empty slot. */
    if (slot < 0) {
        for (int i = 0; i < INVENTORY_SLOT_COUNT; ++i) {
            if (inv[i] == INVENTORY_EMPTY_SLOT) { slot = i; break; }
        }
    }
    if (slot < 0) return 0;                         /* inventory full */

    int       idx           = item_verb - ITEM_VERB_FIRST;
    uint16_t *es            = (uint16_t *)g_entity_state;
    uint16_t  panel_verb_id = es[idx * ENTITY_STATE_STRIDE_U16 + ENTITY_STATE_OFF_VERB];

    es[idx * ENTITY_STATE_STRIDE_U16 + ENTITY_STATE_OFF_IN_INV] = ENTITY_STATE_IN_INV_YES;
    inv[slot] = panel_verb_id;
    return 1;
}

int InventoryRemoveItem(uint16_t item_verb)
{
    if (!is_item_verb(item_verb)) return 0;

    uint16_t *inv = Inventory();
    for (int i = 0; i < INVENTORY_SLOT_COUNT; ++i) {
        if (inv[i] == item_verb) {
            int tail = INVENTORY_SLOT_COUNT - 1;
            if (i < tail) {
                memmove(&inv[i], &inv[i + 1],
                        (size_t)(tail - i) * sizeof(uint16_t));
            }
            inv[tail] = INVENTORY_EMPTY_SLOT;
            return 1;
        }
    }
    return 0;
}

void InventoryDropItem(uint16_t item_verb)
{
    Entity *e = FindEntityByVerbId(item_verb);
    if (!e) return;

    uint8_t *eb = (uint8_t *)e;
    *(uint32_t *)(eb + ENT_OFF_BYTECODE_SLOT) = 0;
    *(int16_t  *)(eb + ENT_OFF_ANCHOR_X)      = INV_DROP_OFFSCREEN;
    *(int16_t  *)(eb + ENT_OFF_DRAWN_X)       = INV_DROP_OFFSCREEN;
}

int InventoryHasItem(uint16_t item_verb)
{
    uint16_t *inv = Inventory();
    for (int i = 0; i < INVENTORY_SLOT_COUNT; ++i) {
        if (inv[i] == item_verb) return 1;
    }
    return 0;
}
