/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tests/test_update_registration.c — kind/id dispatch table.
 *
 * src/actor.c holds the central update-registration table — a 500-slot
 * array of (Entity*, kind, id) triples. Every op that needs to find a
 * registered asset / sprite / mask consults FindUpdateRegistration:
 *
 *   - op 0x0B TIMER_SET, op 0x0F SET_ENTITY_ANIM (main VM)
 *   - op 0x23 SWAP_ATLAS_BY_ID (per-entity VM)
 *   - op 0x2F REG_VERB_MASK (ScriptCallRegMaskList)
 *   - op 0x2D LOAD_ASSET (registers; later spawns look it up)
 *
 * LIFO scan (last-registered wins) — original FUN_00405D80 walks
 * backwards so a re-registration shadows the prior entry. Critical for
 * scene transitions: the new scene's assets register over old ones
 * without an explicit unregister.
 *
 * Except variant skips a passed-in protect-list — used by
 * ScriptCallDestroyEnt (Bug 3 fix #17) to avoid re-finding actor click
 * payloads that should survive a destroy-all.
 *
 * Refactor-safety: this table is the lingua franca between subsystems.
 * Any rework that changes scan direction breaks scene transitions; any
 * change to UnregisterEntityForUpdate's 0xFFFF wildcard breaks the
 * "destroy all of kind K" flow used by op 0x50/0x51 SUBANIM.
 *
 * Reference: src/actor.c:51 (Find), :65 (FindExcept), :1518 (Register),
 *            :1529 (Unregister).
 */

#include "test.h"
#include "wacki.h"
#include "test_engine_stubs.h"

#include <stdint.h>
#include <string.h>

extern void *FindUpdateRegistration(uint16_t kind, uint16_t id);
extern void *FindUpdateRegistrationExcept(uint16_t kind, uint16_t id,
                                          Entity *const *skip, int nskip);
extern void  RegisterEntityForUpdate(Entity *e, uint16_t kind, uint16_t id);
extern void  UnregisterEntityForUpdate(uint16_t kind, uint16_t id);

/* Test entities (the table stores Entity * — pointer identity is what
 * matters; we use distinct addresses). */
static int s_e0, s_e1, s_e2, s_e3;
#define E0 ((Entity *)&s_e0)
#define E1 ((Entity *)&s_e1)
#define E2 ((Entity *)&s_e2)
#define E3 ((Entity *)&s_e3)

static void reset(void)
{
    /* No public reset, but UnregisterEntityForUpdate with 0xFFFF
     * wipes a whole kind. Clear kinds we touch in this suite (0..6). */
    for (uint16_t k = 0; k <= 6; ++k)
        UnregisterEntityForUpdate(k, 0xFFFF);
}

/* ---- basic register + find ------------------------------------------ */

TEST(reg_find_finds_registered_entry)
{
    reset();
    RegisterEntityForUpdate(E0, /*kind=*/1, /*id=*/42);
    void *p = FindUpdateRegistration(1, 42);
    ASSERT_EQ((uintptr_t)p, (uintptr_t)E0);
}

TEST(reg_find_returns_null_on_miss)
{
    reset();
    RegisterEntityForUpdate(E0, 1, 42);

    /* Right kind, wrong id. */
    ASSERT_NULL(FindUpdateRegistration(1, 99));
    /* Right id, wrong kind. */
    ASSERT_NULL(FindUpdateRegistration(2, 42));
    /* Both wrong. */
    ASSERT_NULL(FindUpdateRegistration(5, 100));
}

TEST(reg_find_distinguishes_kind_when_id_collides)
{
    /* Critical: (kind, id) is the composite key, NOT id alone.
     * The original engine ships assets where kind=1 and kind=3 use
     * id=0 simultaneously (LoadAsset + walk-behind mask in same scene). */
    reset();
    RegisterEntityForUpdate(E0, /*kind=*/1, /*id=*/0);
    RegisterEntityForUpdate(E1, /*kind=*/3, /*id=*/0);

    ASSERT_EQ((uintptr_t)FindUpdateRegistration(1, 0), (uintptr_t)E0);
    ASSERT_EQ((uintptr_t)FindUpdateRegistration(3, 0), (uintptr_t)E1);
}

/* ---- LIFO scan: latest registration wins ---------------------------- */

TEST(reg_lifo_latest_registration_wins)
{
    /* Two entries with the same (kind, id). FindUpdateRegistration
     * walks BACKWARDS, so the second registration shadows the first. */
    reset();
    RegisterEntityForUpdate(E0, 1, 7);            /* older */
    RegisterEntityForUpdate(E1, 1, 7);            /* newer — wins */

    void *p = FindUpdateRegistration(1, 7);
    ASSERT_EQ((uintptr_t)p, (uintptr_t)E1);
}

TEST(reg_lifo_three_registrations)
{
    reset();
    RegisterEntityForUpdate(E0, 2, 5);
    RegisterEntityForUpdate(E1, 2, 5);
    RegisterEntityForUpdate(E2, 2, 5);            /* most recent — wins */

    void *p = FindUpdateRegistration(2, 5);
    ASSERT_EQ((uintptr_t)p, (uintptr_t)E2);
}

/* ---- Except variant ------------------------------------------------- */

TEST(reg_except_skips_protected_entity)
{
    /* Two entries with same (kind, id). FindExcept skipping the LIFO
     * winner returns the older entry. */
    reset();
    RegisterEntityForUpdate(E0, 1, 7);            /* older */
    RegisterEntityForUpdate(E1, 1, 7);            /* newer */

    Entity *skip[] = { E1 };
    void *p = FindUpdateRegistrationExcept(1, 7, skip, 1);
    ASSERT_EQ((uintptr_t)p, (uintptr_t)E0);
}

TEST(reg_except_returns_null_if_all_skipped)
{
    /* All matches are in the skip list → NULL. */
    reset();
    RegisterEntityForUpdate(E0, 1, 7);
    RegisterEntityForUpdate(E1, 1, 7);
    Entity *skip[] = { E0, E1 };
    void *p = FindUpdateRegistrationExcept(1, 7, skip, 2);
    ASSERT_NULL(p);
}

TEST(reg_except_with_empty_skip_equals_plain_find)
{
    /* nskip=0 must behave like FindUpdateRegistration. */
    reset();
    RegisterEntityForUpdate(E0, 4, 2);
    RegisterEntityForUpdate(E1, 4, 2);            /* LIFO winner */

    void *p_plain  = FindUpdateRegistration(4, 2);
    void *p_except = FindUpdateRegistrationExcept(4, 2, NULL, 0);
    ASSERT_EQ((uintptr_t)p_plain, (uintptr_t)p_except);
    ASSERT_EQ((uintptr_t)p_plain, (uintptr_t)E1);
}

TEST(reg_except_skip_entries_not_in_table_is_safe)
{
    /* Entity in skip list that was never registered → ignored, normal
     * LIFO winner returned. */
    reset();
    RegisterEntityForUpdate(E0, 1, 9);
    Entity *skip[] = { E1 };                       /* E1 not registered */
    void *p = FindUpdateRegistrationExcept(1, 9, skip, 1);
    ASSERT_EQ((uintptr_t)p, (uintptr_t)E0);
}

/* ---- Unregister ------------------------------------------------------ */

TEST(reg_unregister_removes_specific_entry)
{
    reset();
    RegisterEntityForUpdate(E0, 1, 3);
    UnregisterEntityForUpdate(1, 3);
    ASSERT_NULL(FindUpdateRegistration(1, 3));
}

TEST(reg_unregister_keeps_other_kinds)
{
    /* Unregister (kind=1, id=3) must leave (kind=2, id=3) intact. */
    reset();
    RegisterEntityForUpdate(E0, 1, 3);
    RegisterEntityForUpdate(E1, 2, 3);

    UnregisterEntityForUpdate(1, 3);
    ASSERT_NULL(FindUpdateRegistration(1, 3));
    ASSERT_EQ((uintptr_t)FindUpdateRegistration(2, 3), (uintptr_t)E1);
}

TEST(reg_unregister_wildcard_0xFFFF_drops_all_of_kind)
{
    /* `id == 0xFFFF` is the original's `-1` shortcut: drop every entry
     * with the given kind. Used by "tear down all of kind K" flows. */
    reset();
    RegisterEntityForUpdate(E0, 5, 1);
    RegisterEntityForUpdate(E1, 5, 2);
    RegisterEntityForUpdate(E2, 5, 3);
    RegisterEntityForUpdate(E3, 6, 1);             /* different kind */

    UnregisterEntityForUpdate(5, 0xFFFF);
    ASSERT_NULL(FindUpdateRegistration(5, 1));
    ASSERT_NULL(FindUpdateRegistration(5, 2));
    ASSERT_NULL(FindUpdateRegistration(5, 3));
    /* Other kind preserved. */
    ASSERT_EQ((uintptr_t)FindUpdateRegistration(6, 1), (uintptr_t)E3);
}

TEST(reg_unregister_unmatched_is_safe)
{
    /* Unregistering a (kind, id) that was never registered is a no-op. */
    reset();
    RegisterEntityForUpdate(E0, 1, 1);
    UnregisterEntityForUpdate(99, 99);             /* no match */
    ASSERT_EQ((uintptr_t)FindUpdateRegistration(1, 1), (uintptr_t)E0);
}

TEST(reg_unregister_with_duplicates_removes_all_matching)
{
    /* If the same (kind, id) was registered twice (LIFO shadowing),
     * UnregisterEntityForUpdate drops BOTH — the swap-remove walk
     * doesn't stop at the first hit. After unregister, FindUpdateRegistration
     * must return NULL. */
    reset();
    RegisterEntityForUpdate(E0, 1, 5);
    RegisterEntityForUpdate(E1, 1, 5);

    UnregisterEntityForUpdate(1, 5);
    ASSERT_NULL(FindUpdateRegistration(1, 5));
}

/* ---- Stress: many entries don't blow the table ---------------------- */

TEST(reg_stress_lookup_still_works_after_many_registrations)
{
    /* Register 100 distinct (kind, id) pairs and verify each can be
     * found. Sanity check that the LIFO scan handles a populated table. */
    reset();
    static int storage[100];
    for (int i = 0; i < 100; ++i) {
        RegisterEntityForUpdate((Entity *)&storage[i],
                                /*kind=*/(uint16_t)((i % 6) + 1),
                                /*id=*/(uint16_t)i);
    }
    for (int i = 0; i < 100; ++i) {
        void *p = FindUpdateRegistration((uint16_t)((i % 6) + 1),
                                         (uint16_t)i);
        ASSERT_EQ((uintptr_t)p, (uintptr_t)&storage[i]);
    }
}

SUITE(update_registration)
{
    RUN_TEST(reg_find_finds_registered_entry);
    RUN_TEST(reg_find_returns_null_on_miss);
    RUN_TEST(reg_find_distinguishes_kind_when_id_collides);
    RUN_TEST(reg_lifo_latest_registration_wins);
    RUN_TEST(reg_lifo_three_registrations);
    RUN_TEST(reg_except_skips_protected_entity);
    RUN_TEST(reg_except_returns_null_if_all_skipped);
    RUN_TEST(reg_except_with_empty_skip_equals_plain_find);
    RUN_TEST(reg_except_skip_entries_not_in_table_is_safe);
    RUN_TEST(reg_unregister_removes_specific_entry);
    RUN_TEST(reg_unregister_keeps_other_kinds);
    RUN_TEST(reg_unregister_wildcard_0xFFFF_drops_all_of_kind);
    RUN_TEST(reg_unregister_unmatched_is_safe);
    RUN_TEST(reg_unregister_with_duplicates_removes_all_matching);
    RUN_TEST(reg_stress_lookup_still_works_after_many_registrations);
}
