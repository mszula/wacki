/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tests/test_pe_loader_lifecycle.c — Init/Free/Init cycle.
 *
 * The PE loader holds module-level state (g_pe_image / size / base).
 * Tests pin that:
 *   - Init replaces previous state on second call
 *   - Free clears state completely
 *   - PeLoaderRead / PeLoaderContainsVA reflect current state
 *   - Multiple Init/Free cycles don't leak
 *
 * test_pe_loader.c + test_pe_loader_malformed.c cover single-call
 * happy/sad paths. This file pins SEQUENCE behavior.
 *
 * Reference: src/pe_loader.c lines 29-172.
 */

#include "test.h"
#include "wacki.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int   PeLoaderInit(const char *exe_path);
extern void  PeLoaderFree(void);
extern int   PeLoaderLoaded(void);
extern const void *PeLoaderRead(uint32_t va);
extern int   PeLoaderContainsVA(uint32_t va);

static const char kTmpPe1[] = "/tmp/wacki-test-pe-cycle-1.exe";
static const char kTmpPe2[] = "/tmp/wacki-test-pe-cycle-2.exe";

/* Mini PE32 builder with caller-controlled image_base and section bytes. */
static void build_pe(uint8_t *out, uint32_t image_base, uint8_t marker_byte)
{
    memset(out, 0, 0x200);
    out[0x00] = 'M'; out[0x01] = 'Z';
    uint32_t e_lfanew = 0x80;
    memcpy(out + 0x3C, &e_lfanew, 4);
    out[0x80] = 'P'; out[0x81] = 'E';
    uint16_t machine = 0x014C, nsec = 1, opt_hdr_size = 0x60, chars = 0x0103;
    memcpy(out + 0x84, &machine, 2);
    memcpy(out + 0x86, &nsec, 2);
    memcpy(out + 0x94, &opt_hdr_size, 2);
    memcpy(out + 0x96, &chars, 2);
    uint16_t opt_magic = 0x010B;
    memcpy(out + 0x98, &opt_magic, 2);
    memcpy(out + 0xB4, &image_base, 4);
    memcpy(out + 0xF8, ".data", 5);
    uint32_t vsize = 0x40, va = 0x1000, rsize = 0x40, rptr = 0x140;
    memcpy(out + 0xF8 + 0x08, &vsize, 4);
    memcpy(out + 0xF8 + 0x0C, &va, 4);
    memcpy(out + 0xF8 + 0x10, &rsize, 4);
    memcpy(out + 0xF8 + 0x14, &rptr, 4);
    /* Section content: 64 bytes of marker_byte (distinguishes which
     * PE image is currently mapped). */
    memset(out + 0x140, marker_byte, 0x40);
}

static int write_pe(const char *path, uint32_t image_base, uint8_t marker)
{
    uint8_t buf[0x200];
    build_pe(buf, image_base, marker);
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    size_t w = fwrite(buf, 1, 0x200, f);
    fclose(f);
    return w == 0x200;
}

/* ---- Free clears all state ----------------------------------------- */

TEST(free_after_init_clears_loaded_state)
{
    write_pe(kTmpPe1, 0x00400000u, 0xAA);

    PeLoaderFree();
    ASSERT_EQ(PeLoaderInit(kTmpPe1), 1);
    ASSERT_EQ(PeLoaderLoaded(), 1);

    /* Free clears g_pe_image → Loaded = 0, Read returns NULL. */
    PeLoaderFree();
    ASSERT_EQ(PeLoaderLoaded(), 0);
    ASSERT_NULL(PeLoaderRead(0x00401000u));
    ASSERT_EQ(PeLoaderContainsVA(0x00401000u), 0);

    remove(kTmpPe1);
}

TEST(free_without_init_is_safe)
{
    PeLoaderFree();   /* multiple free calls — no double-free crash */
    PeLoaderFree();
    PeLoaderFree();
    ASSERT_EQ(PeLoaderLoaded(), 0);
}

/* ---- Init replaces previous mapping --------------------------------- */

TEST(second_init_replaces_first_mapping)
{
    /* PE 1: image_base 0x00400000, .data filled with 0xAA. */
    write_pe(kTmpPe1, 0x00400000u, 0xAA);
    /* PE 2: image_base 0x10000000, .data filled with 0xBB. */
    write_pe(kTmpPe2, 0x10000000u, 0xBB);

    PeLoaderFree();

    /* Init #1: PE1 mapped. */
    ASSERT_EQ(PeLoaderInit(kTmpPe1), 1);
    const uint8_t *p1 = (const uint8_t *)PeLoaderRead(0x00401000u);
    ASSERT_NOT_NULL(p1);
    ASSERT_EQ(p1[0], 0xAA);
    ASSERT_EQ(PeLoaderContainsVA(0x00400000u), 1);
    ASSERT_EQ(PeLoaderContainsVA(0x10000000u), 0);   /* PE2 not yet mapped */

    /* Init #2: PE2 replaces. PE1's VA range no longer valid. */
    ASSERT_EQ(PeLoaderInit(kTmpPe2), 1);
    /* PE2's VA range = [0x10000000, 0x10001040). */
    ASSERT_EQ(PeLoaderContainsVA(0x10000000u), 1);
    ASSERT_EQ(PeLoaderContainsVA(0x10001000u), 1);
    /* PE1's range no longer mapped (image_base changed). */
    ASSERT_EQ(PeLoaderContainsVA(0x00400000u), 0);

    const uint8_t *p2 = (const uint8_t *)PeLoaderRead(0x10001000u);
    ASSERT_NOT_NULL(p2);
    ASSERT_EQ(p2[0], 0xBB);

    PeLoaderFree();
    remove(kTmpPe1);
    remove(kTmpPe2);
}

TEST(init_without_free_does_not_leak_visible_state)
{
    /* Implementation detail: Init without prior Free should still
     * replace the mapping (no assertion the old image is freed —
     * could be a memory leak in production, but visible state is
     * the new mapping). */
    write_pe(kTmpPe1, 0x00400000u, 0xCC);
    write_pe(kTmpPe2, 0x00500000u, 0xDD);

    PeLoaderFree();
    ASSERT_EQ(PeLoaderInit(kTmpPe1), 1);
    ASSERT_EQ(PeLoaderInit(kTmpPe2), 1);   /* no Free between */

    /* Now reading from PE2's VA should succeed. */
    const uint8_t *p = (const uint8_t *)PeLoaderRead(0x00501000u);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(p[0], 0xDD);

    PeLoaderFree();
    remove(kTmpPe1);
    remove(kTmpPe2);
}

/* ---- Failed Init leaves state clean -------------------------------- */

TEST(failed_init_after_successful_init_keeps_previous_state)
{
    /* When the SECOND PeLoaderInit fails (e.g. nonexistent file), what
     * happens to the FIRST mapping?
     *
     * Reading src/pe_loader.c: on failure paths, `g_pe_image` /
     * `g_pe_image_size` / `g_pe_image_base` are NOT cleared — they
     * keep whatever the previous successful Init set. This is the
     * conservative behavior: a failed load doesn't trash the working
     * state. */
    write_pe(kTmpPe1, 0x00400000u, 0xEE);

    PeLoaderFree();
    ASSERT_EQ(PeLoaderInit(kTmpPe1), 1);

    /* Failed init — but old state should remain... */
    ASSERT_EQ(PeLoaderInit("/tmp/wacki-pe-truly-no-exist.exe"), 0);

    /* Verify old mapping still readable. (Note: this depends on the
     * loader's implementation. If a future refactor adds an explicit
     * clear on failure, this test will fail and the behavior change
     * needs to be a deliberate decision.) */
    const uint8_t *p = (const uint8_t *)PeLoaderRead(0x00401000u);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(p[0], 0xEE);

    PeLoaderFree();
    remove(kTmpPe1);
}

/* ---- Free then Init cycle works (no stale state) ------------------- */

TEST(multiple_free_init_cycles)
{
    write_pe(kTmpPe1, 0x00400000u, 0x11);
    write_pe(kTmpPe2, 0x10000000u, 0x22);

    for (int cycle = 0; cycle < 3; ++cycle) {
        PeLoaderFree();
        ASSERT_EQ(PeLoaderInit(kTmpPe1), 1);
        const uint8_t *p = (const uint8_t *)PeLoaderRead(0x00401000u);
        ASSERT_NOT_NULL(p);
        ASSERT_EQ(p[0], 0x11);

        PeLoaderFree();
        ASSERT_EQ(PeLoaderInit(kTmpPe2), 1);
        p = (const uint8_t *)PeLoaderRead(0x10001000u);
        ASSERT_NOT_NULL(p);
        ASSERT_EQ(p[0], 0x22);
    }

    PeLoaderFree();
    remove(kTmpPe1);
    remove(kTmpPe2);
}

SUITE(pe_loader_lifecycle)
{
    RUN_TEST(free_after_init_clears_loaded_state);
    RUN_TEST(free_without_init_is_safe);
    RUN_TEST(second_init_replaces_first_mapping);
    RUN_TEST(init_without_free_does_not_leak_visible_state);
    RUN_TEST(failed_init_after_successful_init_keeps_previous_state);
    RUN_TEST(multiple_free_init_cycles);
}
