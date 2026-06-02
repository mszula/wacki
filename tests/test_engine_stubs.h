/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tests/test_engine_stubs.h — capture-stub interface for VM tests.
 *
 * Each ScriptCall* / FindEntityByVerbId / etc. stub in
 * test_engine_stubs.c bumps a counter and stores last-arg values.
 * Tests call vm_stubs_reset() before exercising the VM, then read the
 * g_stub / g_stub_last globals to verify dispatch happened as expected.
 *
 * See test_engine_stubs.c for the actual stub implementations.
 */
#ifndef WACKI_TEST_ENGINE_STUBS_H
#define WACKI_TEST_ENGINE_STUBS_H

#include <stdint.h>

void vm_stubs_reset(void);

/* Settable: return value of InventoryHasItem (op 0x23 reads this). */
extern uint16_t g_stub_inventory_has;

/* Settable: pointer returned by FindEntityByVerbId. Tests can supply
 * an Entity-shaped buffer to exercise opcodes that read/write +0x0A,
 * +0x22, +0x3A, etc. (NUDGE_ENT, GET_ENT_X/Y, etc.). NULL = "no match"
 * (the default). */
struct Entity;
extern struct Entity *g_stub_entity_for_verb;

/* Settable: pointer returned by FindUpdateRegistration. Same idea but
 * for kind/id lookups (TIMER_SET 0x0B, etc.). */
extern void *g_stub_update_registration_for_kind_id;

/* Settable: ActorWalkBothBlocking / ActorWalkToBlocking can capture
 * their last call's params (already there) AND optionally pre-set
 * `g_actor[0]`/`g_actor[1]` so RESET_ACTORS (0x33) can find entities
 * to operate on. The g_actor[] symbols themselves are defined in this
 * file (the engine's real g_actor lives in actor.c which isn't linked). */
struct Entity;
extern struct Entity *g_actor[2];

/* Per-call counters. */
struct vm_stub_counters {
    int process_frame;
    int palette_fade_step;
    int print_text;
    int sound_play;
    int sound_stop;
    int pal_load;
    int pal_fade_step;
    int bg_mask_setup;
    int reg_mask_list;
    int load_asset;
    int spawn_entity;
    int enable_ent;
    int hide_ent;
    int show_ent;
    int walk_mode;
    int walk_to;
    int attach_prop;
    int show_text;
    int dialog_begin;
    int dialog_end;
    int destroy_ent;
    int find_entity;
    int find_update_registration;
    int walk_both_blocking;
    int paint_rawb;
    int inv_add;
    int inv_remove;
    int inv_drop;
    int inv_page_next;
    int inv_page_prev;
    int inv_page_collapse;
    int inv_set_page_for_item;
    int panel_page_swap;
};

/* Last-args. Always read AFTER checking the corresponding counter > 0. */
struct vm_stub_last_args {
    int palette_fade_delta;
    uint16_t sound_play_id, sound_play_a, sound_play_c;
    uint32_t sound_play_b;
    uint16_t pal_load_step;
    uint32_t pal_load_selector;
    int      pal_load_with_fade;
    const char *bg_mask_name;
    uint16_t reg_mask_id;
    uint32_t reg_mask_click;
    int      reg_mask_verb_table;
    const char *load_asset_name;
    uint16_t load_asset_id;
    uint16_t spawn_id, spawn_flags;
    uint32_t spawn_click_addr, spawn_script_addr;
    uint16_t enable_ent_id;
    int      enable_ent_flag;
    uint16_t hide_ent_id, show_ent_id;
    uint16_t walk_mode_id; int walk_mode_mode;
    uint16_t walk_to_id, walk_to_target; int walk_to_mode;
    uint16_t attach_prop_actor, attach_prop_prop; int attach_prop_keep;
    uint16_t show_text_actor;
    const char *show_text_text;
    uint16_t dialog_begin_actor;
    const char *dialog_end_result;
    uint16_t destroy_ent_id; int destroy_ent_flag;
    uint16_t find_entity_verb_id;
    uint16_t find_update_kind, find_update_id;
    int16_t walk_both_x, walk_both_y; int walk_both_dir;
    uint16_t inv_add_id, inv_remove_id, inv_drop_id, inv_set_page_id;
};

/* Settable: return value for InventoryPageNext / InventoryPagePrev.
 * Op 0x1C / 0x1D guard `if (Inventory*) PanelPageSwap()` — so 0 means
 * "no page swap" and non-zero means "swap." Tests can flip this to
 * exercise both paths. */
extern int g_stub_inv_page_next_rc;
extern int g_stub_inv_page_prev_rc;

/* Settable: PlatformShouldQuit() return value. Wait loops in op
 * 0x14/0x15/0x26/0x3D break on non-zero. */
extern int g_stub_should_quit;

/* ProcessGameFrameTick call counter — game.c's real function isn't
 * linked, so our no-op stub counts invocations instead. */
extern int g_stub_process_frame_calls;

/* Inject entities into the click list so production FindEntityByVerbId
 * (in stubs.c — walks EntityListCount/At) can find them. */
struct Entity;
void test_set_click_list(struct Entity **list, int n);
void test_prepare_entity_with_verb(struct Entity *e, uint16_t verb_id);

/* Convenience: prepare entity + add as single-element click list in one
 * call. Most tests want exactly this — set up an entity that
 * FindEntityByVerbId(verb) will return, run the op, verify field changes. */
void test_inject_entity_for_verb(struct Entity *e, uint16_t verb_id);

/* Register an entity in the production update table so
 * FindUpdateRegistration(kind, id) finds it. Used by op 0x0B TIMER_SET,
 * op 0x0F SET_ENTITY_ANIM, op 0x3D WAIT_KIND2_FRAME, op 0x50/0x51
 * SUBANIM tests. */
void test_inject_entity_for_update(struct Entity *e, uint16_t kind, uint16_t id);

extern struct vm_stub_counters g_stub;
extern struct vm_stub_last_args g_stub_last;

#endif
