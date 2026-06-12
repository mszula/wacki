/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * tests/test_heap_cygio.c — heap.c + cygio file-I/O shim thin wrappers.
 *
 * Both are minimal shims (xmalloc → malloc; fopen_cyg → fopen via a CygFile
 * handle). The tests are completeness smokes — they verify the wrappers
 * don't silently break (e.g. someone "optimizes" xcalloc to skip the
 * zero-init flag, or fseek_cyg flips SEEK_END semantics).
 *
 * Reference: src/heap.c, src/platform/sdl/file_host.c (the stdio backend
 * the host tests link; the PS2 fileXio backend is in platform_ps2.c).
 */

#include "test.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern void *xmalloc(uint32_t sz);
extern void  xfree(void *p);
extern void *xcalloc(uint32_t sz, int zero);

/* CygFile is forward-declared in wacki.h. The .c file defines its full
 * body as `struct CygFile { FILE *fp; }`. Tests use it opaquely. */
typedef struct CygFile CygFile;
extern CygFile *fopen_cyg(const char *name, const char *mode);
extern void     fclose_cyg(CygFile *f);
extern uint32_t fread_cyg(void *dst, uint32_t sz, uint32_t n, CygFile *f);
extern void     fseek_cyg(CygFile *f, int32_t off, int whence);
extern int32_t  ftell_cyg(CygFile *f);

/* ---- heap.c ----------------------------------------------------------- */

TEST(xmalloc_returns_writable_buffer)
{
    void *p = xmalloc(128);
    ASSERT_NOT_NULL(p);
    /* Writeable. */
    memset(p, 0x55, 128);
    ASSERT_EQ(((uint8_t *)p)[0], 0x55);
    ASSERT_EQ(((uint8_t *)p)[127], 0x55);
    xfree(p);
}

TEST(xcalloc_with_zero_flag_zeroes_buffer)
{
    void *p = xcalloc(64, 1);
    ASSERT_NOT_NULL(p);
    for (int i = 0; i < 64; ++i)
        ASSERT_EQ(((uint8_t *)p)[i], 0);
    xfree(p);
}

TEST(xcalloc_without_zero_flag_leaves_uninitialized)
{
    /* With zero=0 the buffer is NOT memset'd. We can't assert the content
     * (it's uninitialized), only that the call returns a non-NULL ptr
     * and xfree accepts it. */
    void *p = xcalloc(32, 0);
    ASSERT_NOT_NULL(p);
    xfree(p);
}

TEST(xfree_null_is_safe)
{
    /* Should not crash. */
    xfree(NULL);
    ASSERT_TRUE(1);
}

/* ---- cygio.c roundtrip ------------------------------------------------ */

static const char kTmpCyg[] = "/tmp/wacki-test-cygio.bin";

TEST(fopen_write_read_roundtrip)
{
    /* Write 16 bytes, read back, verify content. */
    const uint8_t payload[16] = {
        0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80,
        0x90, 0xA0, 0xB0, 0xC0, 0xD0, 0xE0, 0xF0, 0xFF
    };

    CygFile *f = fopen_cyg(kTmpCyg, "wb");
    ASSERT_NOT_NULL(f);
    /* fwrite_cyg isn't exposed in this codebase — write directly via
     * stdio FILE inside CygFile. But CygFile is opaque. Use a normal
     * fopen for the write side. */
    fclose_cyg(f);

    FILE *raw = fopen(kTmpCyg, "wb");
    ASSERT_NOT_NULL(raw);
    fwrite(payload, 1, sizeof payload, raw);
    fclose(raw);

    /* Read via cygio. */
    f = fopen_cyg(kTmpCyg, "rb");
    ASSERT_NOT_NULL(f);
    uint8_t buf[16] = { 0 };
    uint32_t n = fread_cyg(buf, 1, sizeof buf, f);
    ASSERT_EQ(n, 16);
    ASSERT_MEMEQ(buf, payload, 16);
    fclose_cyg(f);

    remove(kTmpCyg);
}

TEST(fseek_and_ftell)
{
    /* Write 64 bytes, seek to offset 16, ftell == 16, read 1 byte == known. */
    uint8_t payload[64];
    for (int i = 0; i < 64; ++i) payload[i] = (uint8_t)i;

    FILE *raw = fopen(kTmpCyg, "wb");
    fwrite(payload, 1, 64, raw);
    fclose(raw);

    CygFile *f = fopen_cyg(kTmpCyg, "rb");
    ASSERT_NOT_NULL(f);

    fseek_cyg(f, 16, SEEK_SET);
    int32_t pos = ftell_cyg(f);
    ASSERT_EQ(pos, 16);

    uint8_t b = 0;
    fread_cyg(&b, 1, 1, f);
    ASSERT_EQ(b, 16);

    /* SEEK_END semantics. */
    fseek_cyg(f, -8, SEEK_END);
    pos = ftell_cyg(f);
    ASSERT_EQ(pos, 56);

    fread_cyg(&b, 1, 1, f);
    ASSERT_EQ(b, 56);

    fclose_cyg(f);
    remove(kTmpCyg);
}

TEST(fopen_nonexistent_returns_null)
{
    CygFile *f = fopen_cyg("/tmp/wacki-cygio-no-such-file.bin", "rb");
    ASSERT_NULL(f);
}

TEST(fclose_null_is_safe)
{
    fclose_cyg(NULL);
    ASSERT_TRUE(1);
}

SUITE(heap_cygio)
{
    RUN_TEST(xmalloc_returns_writable_buffer);
    RUN_TEST(xcalloc_with_zero_flag_zeroes_buffer);
    RUN_TEST(xcalloc_without_zero_flag_leaves_uninitialized);
    RUN_TEST(xfree_null_is_safe);
    RUN_TEST(fopen_write_read_roundtrip);
    RUN_TEST(fseek_and_ftell);
    RUN_TEST(fopen_nonexistent_returns_null);
    RUN_TEST(fclose_null_is_safe);
}
