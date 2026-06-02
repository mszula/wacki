/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tests/test_pe_loader_malformed.c — PE loader defensive checks.
 *
 * src/pe_loader.c has multiple safety bails for malformed PE files.
 * Each should fail with rc=0 (no crash, no mapping). Verifies that
 * a corrupt WACKI.EXE doesn't crash the engine — it just fails to
 * load and the caller decides what to do.
 *
 * Each test builds an otherwise-valid mini PE on /tmp, mutates one
 * field to break the corresponding check, calls PeLoaderInit, expects 0.
 *
 * Reference: src/pe_loader.c lines 29-135.
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

static const char kTmpPe[] = "/tmp/wacki-test-pe-malformed.exe";

/* Reuse the minimal-PE builder. Returns 0x200-byte file. Caller can
 * mutate specific offsets BEFORE writing to disk to break specific
 * safety checks. */
static void build_minimal_pe(uint8_t *out, uint32_t image_base)
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
}

static int write_pe(const uint8_t *buf, size_t n)
{
    FILE *f = fopen(kTmpPe, "wb");
    if (!f) return 0;
    size_t w = fwrite(buf, 1, n, f);
    fclose(f);
    return w == n;
}

/* ---- file too small (< 0x200) ---------------------------------------- */

TEST(file_smaller_than_0x200_rejected)
{
    /* PeLoaderInit's first check: `if (fsz_l < 0x200) return 0`.
     * Write a 0x100-byte file. */
    uint8_t small[0x100];
    memset(small, 0, sizeof small);
    small[0] = 'M'; small[1] = 'Z';

    write_pe(small, sizeof small);
    PeLoaderFree();
    ASSERT_EQ(PeLoaderInit(kTmpPe), 0);
    ASSERT_EQ(PeLoaderLoaded(), 0);

    remove(kTmpPe);
}

/* ---- e_lfanew out of range ------------------------------------------- */

TEST(e_lfanew_pointing_past_eof_rejected)
{
    uint8_t buf[0x200];
    build_minimal_pe(buf, 0x00400000u);
    /* Set e_lfanew to 0x10000 (way past 0x200 file size). */
    uint32_t bad_e = 0x10000;
    memcpy(buf + 0x3C, &bad_e, 4);

    write_pe(buf, sizeof buf);
    PeLoaderFree();
    ASSERT_EQ(PeLoaderInit(kTmpPe), 0);

    remove(kTmpPe);
}

/* ---- missing PE signature -------------------------------------------- */

TEST(missing_pe_signature_rejected)
{
    uint8_t buf[0x200];
    build_minimal_pe(buf, 0x00400000u);
    /* Corrupt the "PE\0\0" signature. */
    buf[0x80] = 'X'; buf[0x81] = 'X';

    write_pe(buf, sizeof buf);
    PeLoaderFree();
    ASSERT_EQ(PeLoaderInit(kTmpPe), 0);

    remove(kTmpPe);
}

/* ---- optional header too small --------------------------------------- */

TEST(optional_header_too_small_rejected)
{
    uint8_t buf[0x200];
    build_minimal_pe(buf, 0x00400000u);
    /* Set SizeOfOptionalHeader to < 0x60. */
    uint16_t bad_size = 0x40;
    memcpy(buf + 0x94, &bad_size, 2);

    write_pe(buf, sizeof buf);
    PeLoaderFree();
    ASSERT_EQ(PeLoaderInit(kTmpPe), 0);

    remove(kTmpPe);
}

/* ---- section table out of range -------------------------------------- */

TEST(too_many_sections_rejected)
{
    /* sec_off + num_sections * 40 > fsz → bail.
     * sec_off = pe_off (0x80) + 0x18 + opt_header_size (0x60) = 0xF8.
     * num_sections × 40 + 0xF8 > 0x200 → num_sections > (0x200 - 0xF8)/40
     * = 0x108 / 40 = 6.6 → 7 or more breaks. */
    uint8_t buf[0x200];
    build_minimal_pe(buf, 0x00400000u);
    uint16_t big_nsec = 100;
    memcpy(buf + 0x86, &big_nsec, 2);

    write_pe(buf, sizeof buf);
    PeLoaderFree();
    ASSERT_EQ(PeLoaderInit(kTmpPe), 0);

    remove(kTmpPe);
}

/* ---- implausibly large virtual extent -------------------------------- */

TEST(virtual_extent_above_0x10000000_rejected)
{
    /* max_va_end > 0x10000000 → bail. Set section VA to high value
     * so that VA + VirtualSize > 0x10000000. */
    uint8_t buf[0x200];
    build_minimal_pe(buf, 0x00400000u);
    uint32_t huge_va = 0x20000000u;
    memcpy(buf + 0xF8 + 0x0C, &huge_va, 4);

    write_pe(buf, sizeof buf);
    PeLoaderFree();
    ASSERT_EQ(PeLoaderInit(kTmpPe), 0);

    remove(kTmpPe);
}

/* ---- virtual extent below minimum ------------------------------------ */

TEST(virtual_extent_below_0x1000_rejected)
{
    /* max_va_end < 0x1000 → bail. Set VA + VirtualSize < 0x1000.
     * VA = 0x100, vsize = 0x40 → max_va_end = 0x140 < 0x1000. */
    uint8_t buf[0x200];
    build_minimal_pe(buf, 0x00400000u);
    uint32_t small_va = 0x100;
    memcpy(buf + 0xF8 + 0x0C, &small_va, 4);

    write_pe(buf, sizeof buf);
    PeLoaderFree();
    ASSERT_EQ(PeLoaderInit(kTmpPe), 0);

    remove(kTmpPe);
}

/* ---- section pointer past EOF — skipped but doesn't bail ------------- *
 *
 * Per src/pe_loader.c: "if ((size_t)rptr + (size_t)rsize > fsz) {
 *   fprintf(stderr, ...); continue; }"
 *
 * So a section with bogus rptr SKIPS that section but continues
 * loading the rest. With only 1 section (skipped), the image gets
 * allocated with VA range from section.VA but no bytes copied. Test
 * verifies: PE load returns 1, but reading from that VA gets the
 * calloc'd zero bytes. */

TEST(section_pointer_past_eof_skipped_load_succeeds)
{
    uint8_t buf[0x200];
    build_minimal_pe(buf, 0x00400000u);
    /* Set RawPointerToData to 0x10000 (way past 0x200 file size). */
    uint32_t bad_rptr = 0x10000;
    memcpy(buf + 0xF8 + 0x14, &bad_rptr, 4);

    write_pe(buf, sizeof buf);
    PeLoaderFree();
    /* Load succeeds (returns 1) — the bad section just gets skipped. */
    ASSERT_EQ(PeLoaderInit(kTmpPe), 1);

    /* Reading from VA = ImageBase + 0x1000 returns the zeroed image
     * bytes (section wasn't copied → calloc-zero). */
    extern const void *PeLoaderRead(uint32_t va);
    const uint8_t *p = (const uint8_t *)PeLoaderRead(0x00401000u);
    ASSERT_NOT_NULL(p);
    /* First few bytes should be zero (calloc default). */
    ASSERT_EQ(p[0], 0);
    ASSERT_EQ(p[1], 0);

    PeLoaderFree();
    remove(kTmpPe);
}

/* ---- NULL path argument ---------------------------------------------- */

TEST(null_path_rejected)
{
    PeLoaderFree();
    ASSERT_EQ(PeLoaderInit(NULL), 0);
}

SUITE(pe_loader_malformed)
{
    RUN_TEST(file_smaller_than_0x200_rejected);
    RUN_TEST(e_lfanew_pointing_past_eof_rejected);
    RUN_TEST(missing_pe_signature_rejected);
    RUN_TEST(optional_header_too_small_rejected);
    RUN_TEST(too_many_sections_rejected);
    RUN_TEST(virtual_extent_above_0x10000000_rejected);
    RUN_TEST(virtual_extent_below_0x1000_rejected);
    RUN_TEST(section_pointer_past_eof_skipped_load_succeeds);
    RUN_TEST(null_path_rejected);
}
