/* tests/test_archive_lifecycle.c — DTA re-mount + lifecycle.
 *
 * test_archive.c + test_archive_extended.c cover happy-path mount and
 * multi-entry lookup. This file pins LIFECYCLE behavior:
 *   - OpenDtaArchiveFile called twice → second replaces first
 *   - LoadFileFromDta on stale (closed) state
 *   - Quick re-open of the same path
 *   - Path case-fold retry (uppercase variant of the basename)
 *
 * Reference: src/archive.c lines 53-122.
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

/* Build a 1-real-entry DTA with `name` and 20-byte payload (last byte 0). */
static size_t build_one_entry_dta(uint8_t *out, const char *name,
                                    const uint8_t *payload)
{
    uint32_t base = DTA_MAGIC_BASE;
    memcpy(out + 0, &base, 4);
    write_pkv2_literal(out + 4, payload, 20);
    uint32_t after_file = 4 + 12 + 20;

    uint32_t spis = DTA_MAGIC_SPIS;
    memcpy(out + after_file, &spis, 4);

    uint8_t dir[32];
    memset(dir, 0, sizeof dir);
    size_t namelen = strlen(name);
    if (namelen > DTA_NAME_LEN) namelen = DTA_NAME_LEN;
    memcpy(dir, name, namelen);
    uint32_t off = 4;
    memcpy(dir + 12, &off, 4);

    write_pkv2_literal(out + after_file + 4, dir, 32);
    uint32_t after_spis = after_file + 4 + 12 + 32;
    uint32_t spis_size = 12 + 32;
    memcpy(out + after_spis, &spis_size, 4);
    return after_spis + 4;
}

static const char kTmpDtaA[] = "/tmp/wacki-test-archive-lifecycle-A.dta";
static const char kTmpDtaB[] = "/tmp/wacki-test-archive-lifecycle-B.dta";

/* ---- re-mount: second OpenDtaArchiveFile replaces first ------------- */

TEST(open_twice_second_replaces_first)
{
    uint8_t pA[20];
    memcpy(pA, "Archive A contents\0\0", 20);
    pA[19] = 0;
    uint8_t pB[20];
    memcpy(pB, "Archive B contents\0\0", 20);
    pB[19] = 0;

    uint8_t blobA[128], blobB[128];
    memset(blobA, 0, sizeof blobA);
    memset(blobB, 0, sizeof blobB);

    /* Archive A has file "ONLYA.BIN". */
    build_one_entry_dta(blobA, "ONLYA.BIN", pA);
    ASSERT_TRUE(write_file_bytes(kTmpDtaA, blobA, 88));

    /* Archive B has file "ONLYB.BIN". */
    build_one_entry_dta(blobB, "ONLYB.BIN", pB);
    ASSERT_TRUE(write_file_bytes(kTmpDtaB, blobB, 88));

    /* Mount A — ONLYA.BIN findable, ONLYB.BIN not. */
    ASSERT_EQ(OpenDtaArchiveFile(kTmpDtaA), 1);

    void *buf = NULL; uint32_t sz = 0;
    ASSERT_EQ(LoadFileFromDta("ONLYA.BIN", &buf, &sz), 1);
    ASSERT_EQ(sz, 20);
    ASSERT_EQ(LoadFileFromDta("ONLYB.BIN", &buf, &sz), 0);

    /* Mount B — replaces A. ONLYB.BIN findable, ONLYA.BIN not. */
    ASSERT_EQ(OpenDtaArchiveFile(kTmpDtaB), 1);

    buf = NULL; sz = 0;
    ASSERT_EQ(LoadFileFromDta("ONLYB.BIN", &buf, &sz), 1);
    ASSERT_EQ(sz, 20);
    ASSERT_EQ(LoadFileFromDta("ONLYA.BIN", &buf, &sz), 0);

    remove(kTmpDtaA);
    remove(kTmpDtaB);
}

TEST(open_same_path_twice_idempotent)
{
    uint8_t p[20];
    memcpy(p, "same archive content", 20);
    p[19] = 0;

    uint8_t blob[128];
    memset(blob, 0, sizeof blob);
    build_one_entry_dta(blob, "SAME.BIN", p);
    write_file_bytes(kTmpDtaA, blob, 88);

    /* Mount once + verify. */
    ASSERT_EQ(OpenDtaArchiveFile(kTmpDtaA), 1);
    void *buf = NULL; uint32_t sz = 0;
    ASSERT_EQ(LoadFileFromDta("SAME.BIN", &buf, &sz), 1);

    /* Mount again — should succeed identically. */
    ASSERT_EQ(OpenDtaArchiveFile(kTmpDtaA), 1);
    buf = NULL; sz = 0;
    ASSERT_EQ(LoadFileFromDta("SAME.BIN", &buf, &sz), 1);
    ASSERT_EQ(sz, 20);

    remove(kTmpDtaA);
}

/* ---- case-fold path retry ------------------------------------------- *
 *
 * If fopen fails on the original path, the loader uppercases the
 * BASENAME (the part after the last `/` or `\`) and retries. This
 * handles ISO9660 mounts on macOS that present mixed-case paths. */

TEST(case_fold_basename_retry_uppercase_fallback)
{
    /* Write file with UPPERCASE basename. */
    uint8_t p[20];
    memcpy(p, "case-fold test\0\0\0\0\0", 20);
    p[19] = 0;

    uint8_t blob[128];
    memset(blob, 0, sizeof blob);
    build_one_entry_dta(blob, "TEST.BIN", p);

    /* The loader uppercases the FULL basename on retry (every a-z
     * → A-Z), so the on-disk filename has to be fully uppercase for
     * the retry to find it. Previously this path used partial
     * uppercase ("...CASE-FOLD.DTA") which happened to work on
     * macOS APFS (case-insensitive by default) but failed under a
     * case-sensitive Linux filesystem. */
    const char *upper_path = "/tmp/WACKI-TEST-CASE-FOLD.DTA";
    write_file_bytes(upper_path, blob, 88);

    /* Try opening with lowercase basename — should fail fopen first
     * try, then retry with uppercase and succeed. */
    int rc = OpenDtaArchiveFile("/tmp/wacki-test-case-fold.dta");
    ASSERT_EQ(rc, 1);

    void *buf = NULL; uint32_t sz = 0;
    ASSERT_EQ(LoadFileFromDta("TEST.BIN", &buf, &sz), 1);

    remove(upper_path);
}

TEST(case_fold_failure_when_no_uppercase_either)
{
    /* Both lowercase AND uppercase variants don't exist → returns 0. */
    int rc = OpenDtaArchiveFile("/tmp/wacki-test-truly-nonexistent.dta");
    ASSERT_EQ(rc, 0);
}

SUITE(archive_lifecycle)
{
    RUN_TEST(open_twice_second_replaces_first);
    RUN_TEST(open_same_path_twice_idempotent);
    RUN_TEST(case_fold_basename_retry_uppercase_fallback);
    RUN_TEST(case_fold_failure_when_no_uppercase_either);
}
