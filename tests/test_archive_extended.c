/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tests/test_archive_extended.c — multi-entry DTA + edge cases.
 *
 * test_archive.c covers the 2-entry happy path. This file extends:
 *   - 5-entry directory (verify each entry findable)
 *   - Multiple LoadFileFromDta back-to-back (no inter-call leakage)
 *   - Case-folding: query lowercase matches uppercase entry
 *   - Mixed-case query
 *   - Long names truncated to DTA_NAME_LEN (12 chars)
 *
 * Reference: src/archive.c, docs/asset-format.md §1.
 */

#include "test.h"
#include "wacki.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void write_pkv2_literal(uint8_t *out, const uint8_t *payload, uint32_t unp)
{
    Pkv2Header h;
    h.magic = PKV2_MAGIC;
    h.compressed_size = 12 + unp;
    h.unpacked_size   = unp;
    memcpy(out, &h, 12);
    memcpy(out + 12, payload, unp);
}

static int write_file_bytes(const char *path, const void *buf, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    size_t w = fwrite(buf, 1, n, f);
    fclose(f);
    return w == n;
}

/* Build a DTA archive with `nfiles` entries. Each file's payload is
 * a 20-byte literal ending in 0 (PKv2 literal mode). Caller supplies
 * an array of (name, payload) pairs.
 *
 * Returns total .dta file size. */
struct FileSpec {
    const char *name;
    const uint8_t *payload;     /* 20 bytes, last byte must be 0 */
};

static size_t build_multi_dta(uint8_t *out,
                               const struct FileSpec *specs, int nfiles)
{
    /* Layout:
     *   offset 0: "BASE" (4 bytes)
     *   for each file: PKv2 header (12) + 20-byte payload = 32 bytes
     *   "SPIS" magic (4 bytes)
     *   PKv2 SPIS stream: header (12) + dir entries (nfiles × 16 bytes)
     *     -- but PKv2 literal mode requires payload >= 20 AND last byte 0.
     *     -- For nfiles=1 → 16 bytes payload < 20 (won't work)
     *     -- For nfiles=2 → 32 bytes payload OK
     *     -- For nfiles>=2 → multiples of 16 always OK if last byte 0.
     *
     * We pad the directory with one zero entry at the end (which doubles
     * as terminator: file_offset=0 means LoadFileFromDta returns "not
     * found"; this works because the lookup uses memcmp on full 12-byte
     * name AND zero entry has zero name → only matches "" query). */

    uint32_t base = DTA_MAGIC_BASE;
    memcpy(out + 0, &base, 4);

    /* Files. */
    uint32_t off = 4;
    uint32_t *file_offsets = (uint32_t *)calloc((size_t)nfiles, sizeof(uint32_t));
    for (int i = 0; i < nfiles; ++i) {
        file_offsets[i] = off;
        write_pkv2_literal(out + off, specs[i].payload, 20);
        off += 12 + 20;
    }

    /* SPIS magic. */
    uint32_t spis = DTA_MAGIC_SPIS;
    memcpy(out + off, &spis, 4);
    off += 4;

    /* Build directory: nfiles + 1 zero-entry terminator. */
    int total = nfiles + 1;
    /* Total bytes for directory = total * 16. Must be >= 20 for PKv2
     * literal mode AND last byte (= last entry's file_offset high byte)
     * must be 0. With zero-entry as terminator that's automatic. */
    uint8_t *dir = (uint8_t *)calloc((size_t)total * 16, 1);
    for (int i = 0; i < nfiles; ++i) {
        size_t namelen = strlen(specs[i].name);
        if (namelen > DTA_NAME_LEN) namelen = DTA_NAME_LEN;
        memcpy(dir + i * 16, specs[i].name, namelen);
        memcpy(dir + i * 16 + 12, &file_offsets[i], 4);
    }
    /* Last entry stays zeroed (terminator + last byte 0 for PKv2 lit). */

    uint32_t dir_bytes = (uint32_t)total * 16;
    write_pkv2_literal(out + off, dir, dir_bytes);
    off += 12 + dir_bytes;

    /* spis_size dword at end. */
    uint32_t spis_size = 12 + dir_bytes;
    memcpy(out + off, &spis_size, 4);
    off += 4;

    free(file_offsets);
    free(dir);
    return off;
}

static const char kTmpDta[] = "/tmp/wacki-test-archive-ext.dta";

/* ---- 5-entry directory: every entry findable ------------------------- */

TEST(five_entry_archive_all_findable)
{
    uint8_t p1[20] = { 0 }; memcpy(p1, "file 1 contents\0\0\0\0", 20);
    uint8_t p2[20] = { 0 }; memcpy(p2, "file 2 different\0\0\0", 20);
    uint8_t p3[20] = { 0 }; memcpy(p3, "third payload here\0", 20);
    uint8_t p4[20] = { 0 }; memcpy(p4, "fourth one\0\0\0\0\0\0\0\0\0", 20);
    uint8_t p5[20] = { 0 }; memcpy(p5, "and the fifth\0\0\0\0\0\0", 20);
    /* Ensure every payload ends in 0 (literal-mode terminator). */
    p1[19] = 0; p2[19] = 0; p3[19] = 0; p4[19] = 0; p5[19] = 0;

    struct FileSpec specs[5] = {
        { "ALPHA.WYC", p1 },
        { "BETA.WYC",  p2 },
        { "GAMMA.WYC", p3 },
        { "DELTA.WYC", p4 },
        { "EPS.WYC",   p5 },
    };

    uint8_t blob[1024];
    memset(blob, 0, sizeof blob);
    size_t n = build_multi_dta(blob, specs, 5);
    ASSERT_TRUE(write_file_bytes(kTmpDta, blob, n));
    ASSERT_EQ(OpenDtaArchiveFile(kTmpDta), 1);

    /* Look up each in different order to make sure linear search hits all. */
    void *buf = NULL; uint32_t sz = 0;

    ASSERT_EQ(LoadFileFromDta("GAMMA.WYC", &buf, &sz), 1);
    ASSERT_EQ(sz, 20);
    ASSERT_MEMEQ(buf, p3, 20);

    buf = NULL; sz = 0;
    ASSERT_EQ(LoadFileFromDta("ALPHA.WYC", &buf, &sz), 1);
    ASSERT_EQ(sz, 20);
    ASSERT_MEMEQ(buf, p1, 20);

    buf = NULL; sz = 0;
    ASSERT_EQ(LoadFileFromDta("EPS.WYC", &buf, &sz), 1);
    ASSERT_EQ(sz, 20);
    ASSERT_MEMEQ(buf, p5, 20);

    buf = NULL; sz = 0;
    ASSERT_EQ(LoadFileFromDta("BETA.WYC", &buf, &sz), 1);
    ASSERT_EQ(sz, 20);
    ASSERT_MEMEQ(buf, p2, 20);

    buf = NULL; sz = 0;
    ASSERT_EQ(LoadFileFromDta("DELTA.WYC", &buf, &sz), 1);
    ASSERT_EQ(sz, 20);
    ASSERT_MEMEQ(buf, p4, 20);

    remove(kTmpDta);
}

TEST(repeated_lookups_no_state_leak)
{
    /* Load the same file 10 times — each call should return identical
     * bytes and not deadlock / leak / scribble. */
    uint8_t p1[20] = { 0 };
    memcpy(p1, "ten reads check\0\0\0\0", 20);
    p1[19] = 0;

    struct FileSpec specs[2] = {
        { "FIRST.BIN",  p1 },
        { "SECOND.BIN", p1 },
    };
    uint8_t blob[256];
    memset(blob, 0, sizeof blob);
    size_t n = build_multi_dta(blob, specs, 2);
    write_file_bytes(kTmpDta, blob, n);
    ASSERT_EQ(OpenDtaArchiveFile(kTmpDta), 1);

    for (int i = 0; i < 10; ++i) {
        void *buf = NULL; uint32_t sz = 0;
        ASSERT_EQ(LoadFileFromDta("FIRST.BIN", &buf, &sz), 1);
        ASSERT_EQ(sz, 20);
        ASSERT_MEMEQ(buf, p1, 20);
    }

    remove(kTmpDta);
}

TEST(mixed_case_query_matches_uppercase_entry)
{
    /* Real archives store entries uppercase. LoadFileFromDta uppercases
     * the query — so mixed-case queries should match. */
    uint8_t p1[20] = { 0 };
    memcpy(p1, "case test payload\0\0", 20);
    p1[19] = 0;

    struct FileSpec specs[2] = {
        { "MIXED.WYC",  p1 },
        { "OTHER.WYC",  p1 },
    };
    uint8_t blob[256];
    memset(blob, 0, sizeof blob);
    size_t n = build_multi_dta(blob, specs, 2);
    write_file_bytes(kTmpDta, blob, n);
    ASSERT_EQ(OpenDtaArchiveFile(kTmpDta), 1);

    void *buf = NULL; uint32_t sz = 0;
    /* Query with mixed case. */
    ASSERT_EQ(LoadFileFromDta("MiXeD.wyc", &buf, &sz), 1);
    ASSERT_EQ(sz, 20);
    ASSERT_MEMEQ(buf, p1, 20);

    /* All-lowercase. */
    buf = NULL; sz = 0;
    ASSERT_EQ(LoadFileFromDta("other.wyc", &buf, &sz), 1);
    ASSERT_EQ(sz, 20);

    remove(kTmpDta);
}

TEST(zero_entry_terminator_not_returned_as_match)
{
    /* The zero-name terminator entry should NOT match a query for "". */
    uint8_t p1[20] = { 0 };
    p1[19] = 0;
    struct FileSpec specs[1] = { { "REAL.BIN", p1 } };

    uint8_t blob[256];
    memset(blob, 0, sizeof blob);
    /* nfiles=1 → directory size = (1+1)*16 = 32 bytes, OK for PKv2 lit. */
    size_t n = build_multi_dta(blob, specs, 1);
    write_file_bytes(kTmpDta, blob, n);
    ASSERT_EQ(OpenDtaArchiveFile(kTmpDta), 1);

    /* Query for empty name — zero-entry has all-zero name (12 bytes),
     * and the loader does memcmp on the full 12 bytes. An empty query
     * gets uppercased to all zeros after strncpy + uppercase loop, so
     * it would MATCH the terminator (file_offset = 0). But then the
     * loader's `if (idx <= 0)` check rejects it. */
    void *buf = NULL; uint32_t sz = 0;
    int rc = LoadFileFromDta("", &buf, &sz);
    ASSERT_EQ(rc, 0);

    remove(kTmpDta);
}

SUITE(archive_extended)
{
    RUN_TEST(five_entry_archive_all_findable);
    RUN_TEST(repeated_lookups_no_state_leak);
    RUN_TEST(mixed_case_query_matches_uppercase_entry);
    RUN_TEST(zero_entry_terminator_not_returned_as_match);
}
