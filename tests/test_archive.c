/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tests/test_archive.c — DTA archive directory parse + per-file lookup.
 *
 * Builds a tiny but valid .dta archive in /tmp:
 *   - "BASE" magic header (4 bytes)
 *   - 1 file entry "MYFILE.BIN" (PKv2-literal-mode payload of 20 bytes)
 *   - SPIS section: PKv2-literal-mode directory of 2 entries (real file + 0)
 *   - trailing spis_size dword
 *
 * Then exercises OpenDtaArchiveFile + LoadFileFromDta and checks that
 * the named lookup returns the original payload byte-for-byte.
 *
 * This pins the archive wire format from end to end without depending on
 * any real DANE_NN.DTA. (`tools/dta-validate.sh` covers the real-data
 * integration test.)
 *
 * Reference: src/archive.c + docs/asset-format.md §1.
 */

#include "test.h"
#include "wacki.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- helpers ------------------------------------------------------------ */

/* Same literal-mode encoder as test_pkv2.c, kept local to avoid coupling. */
static void write_pkv2_literal(uint8_t *out, const uint8_t *payload, uint32_t unp)
{
    Pkv2Header h;
    h.magic = PKV2_MAGIC;
    h.compressed_size = 12 + unp;
    h.unpacked_size   = unp;
    memcpy(out, &h, 12);
    memcpy(out + 12, payload, unp);
}

/* Build the fixture in `out` (must be >= 128 bytes). Returns total file size. */
static size_t build_fixture_dta(uint8_t *out, const char *fname,
                                 const uint8_t *file_payload)
{
    /* file_payload is exactly 20 bytes ending in 0. */
    uint32_t base_off = 4;

    /* 1. "BASE" magic at offset 0. */
    uint32_t base = DTA_MAGIC_BASE;
    memcpy(out + 0, &base, 4);

    /* 2. File payload at offset 4: PKv2 header + 20 bytes literal. */
    write_pkv2_literal(out + 4, file_payload, 20);
    uint32_t after_file = 4 + 12 + 20;     /* = 36 */

    /* 3. "SPIS" magic. */
    uint32_t spis = DTA_MAGIC_SPIS;
    memcpy(out + after_file, &spis, 4);

    /* 4. Directory: 2 entries × 16 bytes = 32 bytes (entry[1] all-zero
     *    serves as the literal-mode terminator since its last byte is 0). */
    uint8_t dir[32];
    memset(dir, 0, sizeof dir);
    /* entry[0].name (uppercase, nul-padded to 12 bytes) */
    size_t namelen = strlen(fname);
    if (namelen > DTA_NAME_LEN) namelen = DTA_NAME_LEN;
    memcpy(dir, fname, namelen);
    /* entry[0].file_offset */
    memcpy(dir + 12, &base_off, 4);
    /* entry[1] stays all-zero. */

    write_pkv2_literal(out + after_file + 4, dir, 32);
    /* SPIS PKv2 stream is 12 + 32 = 44 bytes. */
    uint32_t after_spis = after_file + 4 + 44;     /* = 84 */

    /* 5. spis_size dword at end — equals compressed size of SPIS payload. */
    uint32_t spis_size = 44;
    memcpy(out + after_spis, &spis_size, 4);

    return after_spis + 4;          /* total = 88 bytes */
}

static const char kTmpDta[] = "/tmp/wacki-test-archive.dta";

static int write_file_bytes(const char *path, const void *buf, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    size_t w = fwrite(buf, 1, n, f);
    fclose(f);
    return w == n;
}

/* ---- tests -------------------------------------------------------------- */

TEST(open_mounts_directory_and_lookup_works)
{
    /* 20 bytes ending in 0 — the literal-mode terminator. */
    uint8_t payload[20];
    memcpy(payload, "Hello, dta archive!\0", 20);

    uint8_t blob[128];
    memset(blob, 0xCC, sizeof blob);
    size_t n = build_fixture_dta(blob, "MYFILE.BIN", payload);
    ASSERT_EQ(n, 88);
    ASSERT_TRUE(write_file_bytes(kTmpDta, blob, n));

    /* Mount + lookup. */
    ASSERT_EQ(OpenDtaArchiveFile(kTmpDta), 1);

    void *out_buf = NULL;
    uint32_t out_size = 0;
    int rc = LoadFileFromDta("MYFILE.BIN", &out_buf, &out_size);
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(out_size, 20);
    ASSERT_NOT_NULL(out_buf);
    ASSERT_MEMEQ(out_buf, payload, 20);

    /* Cleanup */
    remove(kTmpDta);
}

TEST(lookup_is_case_insensitive)
{
    /* The original engine uppercases the lookup key. So a lowercase
     * argument should still match an uppercase directory entry. */
    uint8_t payload[20];
    memcpy(payload, "case-fold check\n\0\0\0\0\0", 20);

    uint8_t blob[128];
    build_fixture_dta(blob, "EBEK.WYC", payload);
    ASSERT_TRUE(write_file_bytes(kTmpDta, blob, 88));

    ASSERT_EQ(OpenDtaArchiveFile(kTmpDta), 1);

    void *out_buf = NULL;
    uint32_t out_size = 0;
    /* Lowercase / mixed-case lookup should match. */
    int rc = LoadFileFromDta("ebek.wyc", &out_buf, &out_size);
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(out_size, 20);
    ASSERT_MEMEQ(out_buf, payload, 20);

    remove(kTmpDta);
}

TEST(lookup_miss_returns_zero)
{
    uint8_t payload[20];
    memcpy(payload, "fixture content    \0", 20);

    uint8_t blob[128];
    build_fixture_dta(blob, "REAL.BIN", payload);
    ASSERT_TRUE(write_file_bytes(kTmpDta, blob, 88));
    ASSERT_EQ(OpenDtaArchiveFile(kTmpDta), 1);

    void *out_buf = NULL;
    uint32_t out_size = 0;
    int rc = LoadFileFromDta("DOES_NOT.EXST", &out_buf, &out_size);
    ASSERT_EQ(rc, 0);

    remove(kTmpDta);
}

TEST(open_nonexistent_path_returns_zero)
{
    /* The retry-with-uppercase path also fails → return 0, not crash. */
    int rc = OpenDtaArchiveFile("/tmp/this-archive-should-not-exist.dta");
    ASSERT_EQ(rc, 0);
}

TEST(open_rejects_bad_magic)
{
    /* Write a file with a non-BASE magic — must fail cleanly. */
    uint8_t blob[16];
    memset(blob, 0, sizeof blob);
    /* "FAKE" instead of "BASE" */
    blob[0] = 'F'; blob[1] = 'A'; blob[2] = 'K'; blob[3] = 'E';
    ASSERT_TRUE(write_file_bytes(kTmpDta, blob, sizeof blob));

    int rc = OpenDtaArchiveFile(kTmpDta);
    ASSERT_EQ(rc, 0);

    remove(kTmpDta);
}

SUITE(archive)
{
    RUN_TEST(open_mounts_directory_and_lookup_works);
    RUN_TEST(lookup_is_case_insensitive);
    RUN_TEST(lookup_miss_returns_zero);
    RUN_TEST(open_nonexistent_path_returns_zero);
    RUN_TEST(open_rejects_bad_magic);
}
