/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tests/test_ent_ptr_intern.c — 64-bit pointer slot table.
 *
 * Entity struct stores 4-byte slot IDs in pointer-typed fields (+0x28
 * atlas, +0x2C bytecode, +0x14 pixels, etc.) to preserve byte-faithful
 * 32-bit binary layout. On 64-bit hosts, real pointers are 8 bytes —
 * so each pointer is interned through ent_ptr_intern() into a small
 * table; the returned slot ID fits in 32 bits, and ent_ptr_resolve()
 * round-trips back to the real pointer.
 *
 * Why this matters: bug #99/#100 — earlier ports wrote
 *   `*(uint32_t *)(eb + 0x28) = (uint32_t)(uintptr_t)atlas_ptr;`
 * which TRUNCATES the upper 32 bits of a 64-bit pointer, then later
 * dereferences a bogus 32-bit address. The slot table is the fix.
 *
 * Refactor-safety: every pointer-in-uint32 slot MUST go through this
 * intern path. A refactor that introduces a fresh cast bypassing
 * ent_ptr_intern silently breaks ALL entity-asset lookups on 64-bit.
 *
 * Contract (1:1 with src/actor.c:31-41):
 *   - slot 0 reserved for NULL
 *   - ent_ptr_intern(NULL) → 0 (the reserved sentinel)
 *   - same pointer interned twice → same slot (deduplication)
 *   - ent_ptr_resolve(0) → NULL
 *   - ent_ptr_resolve(invalid) → NULL (no OOB access)
 *   - round-trip: resolve(intern(p)) == p for any non-NULL p
 *   - table capacity = 1024 slots
 *
 * Reference: src/actor.c:27-41.
 */

#include "test.h"
#include "wacki.h"
#include "test_engine_stubs.h"

#include <stdint.h>

extern uint32_t ent_ptr_intern (void *p);
extern void    *ent_ptr_resolve(uint32_t slot);

/* ---- NULL sentinel slot --------------------------------------------- */

TEST(intern_null_returns_slot_zero)
{
    /* NULL maps to slot 0 — this is the reserved sentinel that lets
     * uninitialised entity fields (which are memset to 0) read back
     * as NULL via resolve. */
    ASSERT_EQ(ent_ptr_intern(NULL), 0);
}

TEST(resolve_slot_zero_returns_null)
{
    ASSERT_NULL(ent_ptr_resolve(0));
}

TEST(resolve_null_round_trip_via_intern)
{
    /* intern(NULL) → 0; resolve(0) → NULL. End-to-end. */
    uint32_t slot = ent_ptr_intern(NULL);
    ASSERT_EQ(slot, 0);
    ASSERT_NULL(ent_ptr_resolve(slot));
}

/* ---- non-NULL pointer round-trip ------------------------------------ */

TEST(intern_returns_nonzero_slot_for_real_pointer)
{
    static int dummy;
    uint32_t slot = ent_ptr_intern(&dummy);
    ASSERT_NE(slot, 0);                            /* not the NULL sentinel */
}

TEST(round_trip_recovers_original_pointer)
{
    static int dummy_a;
    uint32_t slot = ent_ptr_intern(&dummy_a);
    void *back = ent_ptr_resolve(slot);
    ASSERT_EQ((uintptr_t)back, (uintptr_t)&dummy_a);
}

TEST(distinct_pointers_get_distinct_slots)
{
    /* Two different pointers must NOT collide on the same slot
     * (otherwise resolve would alias them). */
    static int dummy_a, dummy_b;
    uint32_t slot_a = ent_ptr_intern(&dummy_a);
    uint32_t slot_b = ent_ptr_intern(&dummy_b);
    ASSERT_NE(slot_a, slot_b);
    ASSERT_EQ((uintptr_t)ent_ptr_resolve(slot_a), (uintptr_t)&dummy_a);
    ASSERT_EQ((uintptr_t)ent_ptr_resolve(slot_b), (uintptr_t)&dummy_b);
}

/* ---- deduplication: same pointer → same slot ------------------------ */

TEST(intern_same_pointer_twice_returns_same_slot)
{
    /* Critical: re-interning the same pointer must reuse its slot —
     * otherwise the 1024-slot table fills up after a few scene loads
     * and intern starts returning 0 (NULL fallback) for live atlases. */
    static int dummy;
    uint32_t a = ent_ptr_intern(&dummy);
    uint32_t b = ent_ptr_intern(&dummy);
    ASSERT_EQ(a, b);
}

TEST(intern_three_pointers_two_dupes)
{
    /* intern(P), intern(Q), intern(P) → first and third share slot. */
    static int p, q;
    uint32_t s_p1 = ent_ptr_intern(&p);
    uint32_t s_q  = ent_ptr_intern(&q);
    uint32_t s_p2 = ent_ptr_intern(&p);

    ASSERT_EQ(s_p1, s_p2);                         /* same pointer → same slot */
    ASSERT_NE(s_p1, s_q);
}

/* ---- invalid slot resolution ---------------------------------------- */

TEST(resolve_high_invalid_slot_returns_null)
{
    /* Slot > table size must NOT read past the array. */
    ASSERT_NULL(ent_ptr_resolve(0xFFFFFFFF));
    ASSERT_NULL(ent_ptr_resolve(99999));
}

TEST(resolve_slot_beyond_high_water_returns_null)
{
    /* Even within the 1024-slot range, a slot that has never been
     * issued must return NULL (table[i] is uninitialised for those). */
    static int dummy;
    uint32_t live = ent_ptr_intern(&dummy);
    /* Any slot > `live` (and within array bounds) is past the high-water
     * mark — resolve must return NULL, not a stale pointer. */
    ASSERT_NULL(ent_ptr_resolve(live + 100));
}

/* ---- typical entity-field usage ------------------------------------- */

TEST(intern_simulates_entity_atlas_slot_usage)
{
    /* Simulate how Entity stores its atlas pointer at +0x28:
     *   *(uint32_t *)(eb + 0x28) = ent_ptr_intern(atlas);
     *   ...
     *   AnimAsset *a = (AnimAsset *)ent_ptr_resolve(*(uint32_t *)(eb + 0x28));
     *
     * This is the EXACT pattern used throughout actor.c / script.c —
     * pinning it here protects against a refactor regressing to raw
     * casts (#99/#100). */
    static AnimAsset atlas;
    uint8_t entity_buf[256] = { 0 };

    /* Write atlas slot into entity[+0x28]. */
    *(uint32_t *)(entity_buf + 0x28) = ent_ptr_intern(&atlas);

    /* Read it back as a typed pointer. */
    AnimAsset *back = (AnimAsset *)ent_ptr_resolve(*(uint32_t *)(entity_buf + 0x28));
    ASSERT_EQ((uintptr_t)back, (uintptr_t)&atlas);
}

TEST(intern_zero_init_entity_field_resolves_to_null)
{
    /* memset(entity, 0, sizeof) makes all slots = 0 = NULL sentinel.
     * Resolving an un-set field must give NULL, not a wild pointer
     * read out of bounds. */
    uint8_t entity_buf[256] = { 0 };
    void *atlas = ent_ptr_resolve(*(uint32_t *)(entity_buf + 0x28));
    ASSERT_NULL(atlas);
}

SUITE(ent_ptr_intern)
{
    RUN_TEST(intern_null_returns_slot_zero);
    RUN_TEST(resolve_slot_zero_returns_null);
    RUN_TEST(resolve_null_round_trip_via_intern);
    RUN_TEST(intern_returns_nonzero_slot_for_real_pointer);
    RUN_TEST(round_trip_recovers_original_pointer);
    RUN_TEST(distinct_pointers_get_distinct_slots);
    RUN_TEST(intern_same_pointer_twice_returns_same_slot);
    RUN_TEST(intern_three_pointers_two_dupes);
    RUN_TEST(resolve_high_invalid_slot_returns_null);
    RUN_TEST(resolve_slot_beyond_high_water_returns_null);
    RUN_TEST(intern_simulates_entity_atlas_slot_usage);
    RUN_TEST(intern_zero_init_entity_field_resolves_to_null);
}
