/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tests/test_binary_data.c — xlat_binary_ptr / xlat_asset_name.
 *
 * Both wrappers are trivial: they forward to PeLoaderRead(va). The
 * non-trivial behaviour is the NULL fast-path (xlat of VA=0 returns
 * NULL without touching the PE loader). This test exercises both
 * paths against a mini PE built on /tmp.
 *
 * Reference: src/binary_data.c.
 */

#include "test.h"
#include "wacki.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const char *xlat_asset_name(uint32_t va);
extern const void *xlat_binary_ptr(uint32_t addr);
extern int   PeLoaderInit(const char *exe_path);
extern void  PeLoaderFree(void);

/* Reuse the minimal PE builder from test_pe_loader.c, but the test
 * binary will get the symbols at link time. We duplicate the header
 * builder locally to keep tests independent. */

static size_t build_minimal_pe(uint8_t *out, uint32_t image_base,
                                const char *section_str)
{
    memset(out, 0, 0x200);
    out[0x00] = 'M'; out[0x01] = 'Z';
    uint32_t e_lfanew = 0x80;
    memcpy(out + 0x3C, &e_lfanew, 4);
    out[0x80] = 'P'; out[0x81] = 'E';

    uint16_t machine = 0x014C;
    uint16_t nsec = 1;
    uint16_t opt_hdr_size = 0x60;
    uint16_t chars = 0x0103;
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

    /* Section contains a NUL-terminated string starting at VA 0x1000. */
    size_t sl = strlen(section_str);
    if (sl > 60) sl = 60;
    memcpy(out + 0x140, section_str, sl);
    out[0x140 + sl] = 0;

    return 0x200;
}

static const char kTmpPe[] = "/tmp/wacki-test-binary-data.exe";

/* ---- tests -------------------------------------------------------------- */

TEST(xlat_binary_ptr_null_va_returns_null)
{
    /* No PE loader init needed — VA=0 short-circuits. */
    ASSERT_NULL(xlat_binary_ptr(0));
}

TEST(xlat_asset_name_null_va_returns_null)
{
    ASSERT_NULL(xlat_asset_name(0));
}

TEST(xlat_binary_ptr_resolves_pe_va)
{
    uint8_t pe[512];
    build_minimal_pe(pe, 0x00400000u, "TEST.WYC");
    FILE *fp = fopen(kTmpPe, "wb");
    ASSERT_NOT_NULL(fp);
    fwrite(pe, 1, 0x200, fp);
    fclose(fp);

    PeLoaderFree();
    ASSERT_EQ(PeLoaderInit(kTmpPe), 1);

    /* VA 0x00401000 = image_base + section.VA → first byte of .data section. */
    const void *p = xlat_binary_ptr(0x00401000u);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(((const uint8_t *)p)[0], 'T');

    /* xlat_asset_name returns the same bytes but typed as char *. */
    const char *name = xlat_asset_name(0x00401000u);
    ASSERT_NOT_NULL(name);
    ASSERT_STREQ(name, "TEST.WYC");

    PeLoaderFree();
    remove(kTmpPe);
}

TEST(xlat_binary_ptr_outside_image_returns_null)
{
    uint8_t pe[512];
    build_minimal_pe(pe, 0x00400000u, "anything");
    FILE *fp = fopen(kTmpPe, "wb");
    fwrite(pe, 1, 0x200, fp);
    fclose(fp);

    PeLoaderFree();
    ASSERT_EQ(PeLoaderInit(kTmpPe), 1);

    /* VA outside the mapped image. */
    ASSERT_NULL(xlat_binary_ptr(0x80000000u));
    ASSERT_NULL(xlat_asset_name(0x12345678u));

    PeLoaderFree();
    remove(kTmpPe);
}

SUITE(binary_data)
{
    RUN_TEST(xlat_binary_ptr_null_va_returns_null);
    RUN_TEST(xlat_asset_name_null_va_returns_null);
    RUN_TEST(xlat_binary_ptr_resolves_pe_va);
    RUN_TEST(xlat_binary_ptr_outside_image_returns_null);
}
