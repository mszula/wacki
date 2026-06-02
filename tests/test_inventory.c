/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tests/test_inventory.c — REAL Inventory state machine.
 *
 * Now that stubs.c is linked, we test the PRODUCTION
 * InventoryAddItem/RemoveItem/HasItem/DropItem/PageNext/PagePrev/
 * PageCollapse/SetPageForItem/ResetInventory implementations directly.
 *
 * Inventory storage: 60-slot uint16_t array, accessed via Inventory()
 * which returns (uint16_t *)g_scene_snapshot. Empty slot sentinel = 0x26.
 * Valid item verb_ids = 0x29..0xB6.
 *
 * Reference: src/stubs.c lines 839-1082.
 */

#include "test.h"
#include "wacki.h"

#include <stdint.h>
#include <string.h>

/* Production API. */
extern uint16_t *Inventory(void);
extern void     ResetInventory(void);
extern int      InventoryAddItem(uint16_t item_verb);
extern int      InventoryRemoveItem(uint16_t item_verb);
extern int      InventoryHasItem(uint16_t item_verb);
extern void     InventoryDropItem(uint16_t item_verb);
extern int      InventoryPageNext(void);
extern int      InventoryPagePrev(void);
extern void     InventoryPageCollapse(void);
extern void     InventorySetPageForItem(uint16_t item_verb);

extern uint32_t g_scene_snapshot[0x1E];
extern uint32_t g_entity_state[0x11C];
extern uint16_t g_panel_page_idx;     /* defined in stubs.c */

/* ---- ResetInventory: fills all 60 slots with 0x26 sentinel --------- */

TEST(reset_inventory_fills_60_slots_with_sentinel)
{
    /* Scribble before reset to verify ResetInventory clears. */
    uint16_t *inv = Inventory();
    for (int i = 0; i < 60; ++i) inv[i] = (uint16_t)i;

    ResetInventory();

    for (int i = 0; i < 60; ++i) ASSERT_EQ(inv[i], 0x26);
    ASSERT_EQ(g_panel_page_idx, 0);
}

/* ---- AddItem: writes panel_verb from entity_state into first empty slot */

TEST(add_item_writes_to_first_empty_slot)
{
    ResetInventory();
    /* Set up entity_state[item-0x29]*4 (= +0 panel_verb_id) for item 0x40.
     * entity_state is a uint32_t array but accessed as uint16_t.
     * Per stubs.c: es[idx * 4 + 0] = panel_verb_id.
     * For item 0x40: idx = 0x40 - 0x29 = 0x17. es[0x17*4 + 0] = 0x40. */
    uint16_t *es = (uint16_t *)g_entity_state;
    es[(0x40 - 0x29) * 4 + 0] = 0x40;        /* panel verb */
    es[(0x40 - 0x29) * 4 + 1] = 0;            /* in_inventory marker (gets set) */

    int rc = InventoryAddItem(0x40);
    ASSERT_EQ(rc, 1);

    uint16_t *inv = Inventory();
    ASSERT_EQ(inv[0], 0x40);                  /* first slot now holds panel_verb */
    /* in_inventory marker set to 0xFFFF. */
    ASSERT_EQ(es[(0x40 - 0x29) * 4 + 1], 0xFFFF);
}

TEST(add_item_out_of_range_returns_zero)
{
    ResetInventory();
    /* Below range: < 0x29. */
    ASSERT_EQ(InventoryAddItem(0x10), 0);
    /* Above range: >= 0x29 + 0x8E = 0xB7. */
    ASSERT_EQ(InventoryAddItem(0xC0), 0);

    /* Inventory still empty. */
    uint16_t *inv = Inventory();
    for (int i = 0; i < 60; ++i) ASSERT_EQ(inv[i], 0x26);
}

TEST(add_item_duplicate_reuses_existing_slot)
{
    ResetInventory();
    uint16_t *es = (uint16_t *)g_entity_state;
    es[(0x50 - 0x29) * 4 + 0] = 0x50;

    InventoryAddItem(0x50);
    InventoryAddItem(0x50);   /* duplicate */

    uint16_t *inv = Inventory();
    /* First slot has 0x50, second remains empty. */
    ASSERT_EQ(inv[0], 0x50);
    ASSERT_EQ(inv[1], 0x26);
}

TEST(add_item_fills_60_slots_then_returns_zero)
{
    ResetInventory();
    uint16_t *es = (uint16_t *)g_entity_state;
    /* Fill 60 distinct items. Item ids 0x29..(0x29+60-1) = 0x29..0x64.
     * Each one's panel_verb_id stored at es[(id-0x29)*4]. */
    for (int i = 0; i < 60; ++i) {
        uint16_t id = (uint16_t)(0x29 + i);
        es[i * 4 + 0] = id;
        es[i * 4 + 1] = 0;
        ASSERT_EQ(InventoryAddItem(id), 1);
    }

    /* 61st add should fail (no empty slot). */
    uint16_t id_61 = (uint16_t)(0x29 + 60);
    es[60 * 4 + 0] = id_61;
    ASSERT_EQ(InventoryAddItem(id_61), 0);
}

/* ---- HasItem: linear scan over 60 slots ----------------------------- */

TEST(has_item_returns_one_when_present)
{
    ResetInventory();
    uint16_t *inv = Inventory();
    inv[15] = 0x55;                          /* place item 0x55 in slot 15 */

    ASSERT_EQ(InventoryHasItem(0x55), 1);
    ASSERT_EQ(InventoryHasItem(0x40), 0);    /* not present */
}

TEST(has_item_returns_zero_for_empty_inventory)
{
    ResetInventory();
    /* Every slot is 0x26. HasItem for any non-0x26 should return 0. */
    ASSERT_EQ(InventoryHasItem(0x30), 0);
    /* HasItem(0x26) — every slot matches 0x26 → returns 1. (Edge case
     * documented behaviour: scripts shouldn't query 0x26 sentinel.) */
    ASSERT_EQ(InventoryHasItem(0x26), 1);
}

/* ---- RemoveItem: shifts subsequent slots left ----------------------- */

TEST(remove_item_shifts_subsequent_slots_and_fills_last)
{
    ResetInventory();
    uint16_t *inv = Inventory();
    inv[0] = 0x30;
    inv[1] = 0x40;
    inv[2] = 0x50;
    inv[3] = 0x60;

    int rc = InventoryRemoveItem(0x40);
    ASSERT_EQ(rc, 1);

    /* Slots 1..58 shifted left, slot 59 = 0x26. */
    ASSERT_EQ(inv[0], 0x30);
    ASSERT_EQ(inv[1], 0x50);                  /* was inv[2] */
    ASSERT_EQ(inv[2], 0x60);                  /* was inv[3] */
    ASSERT_EQ(inv[59], 0x26);
}

TEST(remove_item_not_in_inventory_returns_zero)
{
    ResetInventory();
    Inventory()[0] = 0x30;

    ASSERT_EQ(InventoryRemoveItem(0x99), 0);

    /* Inventory unchanged. */
    ASSERT_EQ(Inventory()[0], 0x30);
}

TEST(remove_item_out_of_range_returns_zero)
{
    ResetInventory();
    ASSERT_EQ(InventoryRemoveItem(0x10), 0);
    ASSERT_EQ(InventoryRemoveItem(0xFF), 0);
}

/* ---- PageNext / PagePrev / PageCollapse ----------------------------- */

TEST(page_prev_from_zero_returns_zero)
{
    ResetInventory();
    g_panel_page_idx = 0;

    ASSERT_EQ(InventoryPagePrev(), 0);
    ASSERT_EQ(g_panel_page_idx, 0);
}

TEST(page_prev_decrements_when_non_zero)
{
    ResetInventory();
    g_panel_page_idx = 3;

    ASSERT_EQ(InventoryPagePrev(), 1);
    ASSERT_EQ(g_panel_page_idx, 2);

    ASSERT_EQ(InventoryPagePrev(), 1);
    ASSERT_EQ(g_panel_page_idx, 1);
}

TEST(page_next_advances_when_next_page_has_items)
{
    ResetInventory();
    uint16_t *inv = Inventory();
    /* Put item on page 1 (slot 6). */
    inv[6] = 0x40;
    g_panel_page_idx = 0;

    ASSERT_EQ(InventoryPageNext(), 1);
    ASSERT_EQ(g_panel_page_idx, 1);
}

TEST(page_next_returns_zero_when_no_more_items)
{
    ResetInventory();
    g_panel_page_idx = 0;
    /* All slots empty (0x26). Page next should fail. */

    ASSERT_EQ(InventoryPageNext(), 0);
    ASSERT_EQ(g_panel_page_idx, 0);
}

TEST(page_next_capped_at_9)
{
    ResetInventory();
    g_panel_page_idx = 9;

    ASSERT_EQ(InventoryPageNext(), 0);
    ASSERT_EQ(g_panel_page_idx, 9);
}

TEST(page_collapse_walks_back_to_non_empty_page)
{
    ResetInventory();
    uint16_t *inv = Inventory();
    /* Put item on page 0 only. */
    inv[0] = 0x30;
    g_panel_page_idx = 3;     /* on an empty page */

    InventoryPageCollapse();

    /* Walked back to page 0 (the only non-empty page). */
    ASSERT_EQ(g_panel_page_idx, 0);
}

TEST(page_collapse_zero_is_no_op)
{
    ResetInventory();
    g_panel_page_idx = 0;
    InventoryPageCollapse();
    ASSERT_EQ(g_panel_page_idx, 0);
}

TEST(page_collapse_stops_at_first_non_empty_page)
{
    ResetInventory();
    uint16_t *inv = Inventory();
    /* Item on page 2 only. */
    inv[12] = 0x30;
    g_panel_page_idx = 5;

    InventoryPageCollapse();
    ASSERT_EQ(g_panel_page_idx, 2);
}

/* ---- SetPageForItem ----------------------------------------------- */

TEST(set_page_for_item_finds_correct_page)
{
    ResetInventory();
    uint16_t *inv = Inventory();
    inv[14] = 0x50;                          /* page 2 (14/6 = 2) */
    g_panel_page_idx = 0;

    InventorySetPageForItem(0x50);
    ASSERT_EQ(g_panel_page_idx, 2);
}

TEST(set_page_for_item_out_of_range_is_noop)
{
    ResetInventory();
    g_panel_page_idx = 5;

    InventorySetPageForItem(0x10);            /* below range */
    ASSERT_EQ(g_panel_page_idx, 5);

    InventorySetPageForItem(0xFF);            /* above range */
    ASSERT_EQ(g_panel_page_idx, 5);
}

TEST(set_page_for_item_not_present_no_op)
{
    ResetInventory();
    g_panel_page_idx = 4;

    InventorySetPageForItem(0x70);            /* not in inventory */
    ASSERT_EQ(g_panel_page_idx, 4);           /* unchanged */
}

SUITE(inventory)
{
    RUN_TEST(reset_inventory_fills_60_slots_with_sentinel);
    RUN_TEST(add_item_writes_to_first_empty_slot);
    RUN_TEST(add_item_out_of_range_returns_zero);
    RUN_TEST(add_item_duplicate_reuses_existing_slot);
    RUN_TEST(add_item_fills_60_slots_then_returns_zero);
    RUN_TEST(has_item_returns_one_when_present);
    RUN_TEST(has_item_returns_zero_for_empty_inventory);
    RUN_TEST(remove_item_shifts_subsequent_slots_and_fills_last);
    RUN_TEST(remove_item_not_in_inventory_returns_zero);
    RUN_TEST(remove_item_out_of_range_returns_zero);
    RUN_TEST(page_prev_from_zero_returns_zero);
    RUN_TEST(page_prev_decrements_when_non_zero);
    RUN_TEST(page_next_advances_when_next_page_has_items);
    RUN_TEST(page_next_returns_zero_when_no_more_items);
    RUN_TEST(page_next_capped_at_9);
    RUN_TEST(page_collapse_walks_back_to_non_empty_page);
    RUN_TEST(page_collapse_zero_is_no_op);
    RUN_TEST(page_collapse_stops_at_first_non_empty_page);
    RUN_TEST(set_page_for_item_finds_correct_page);
    RUN_TEST(set_page_for_item_out_of_range_is_noop);
    RUN_TEST(set_page_for_item_not_present_no_op);
}
