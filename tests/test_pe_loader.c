/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tests/test_pe_loader.c — passive PE32 image map.
 *
 * The PE loader maps a PE32 image (WACKI.EXE) so that any original-VA
 * pointer the port encounters in script bytecode (`xlat_binary_ptr(VA)`)
 * resolves to a host pointer. The mapping logic is:
 *   - parse MZ + PE headers
 *   - find max(VA + VirtualSize)
 *   - allocate flat image
 *   - memcpy each section to image + VA
 *   - PeLoaderRead(va) = image + (va - image_base)
 *
 * This test builds a minimal valid PE32 file in /tmp with one section,
 * exercises PeLoaderInit, then verifies PeLoaderRead returns the right
 * bytes and PeLoaderContainsVA reports the right range.
 *
 * Reference: src/pe_loader.c.
 */

#include "test.h"
#include "wacki.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward decls for PE loader (not in public wacki.h). */
extern int   PeLoaderInit(const char *exe_path);
extern void  PeLoaderFree(void);
extern int   PeLoaderLoaded(void);
extern const void *PeLoaderRead(uint32_t va);
extern int   PeLoaderContainsVA(uint32_t va);

static const char kTmpPe[] = "/tmp/wacki-test-mini.exe";

/* ---- minimal PE32 builder ---------------------------------------------- */

/* Layout (offsets):
 *   0x000  'M' 'Z' + zero pad
 *   0x03C  e_lfanew = 0x80
 *   0x080  'P' 'E' 0 0  (signature)
 *   0x084  COFF header (20 bytes): machine, num_sections, ..., opt_size, chars
 *   0x098  Optional header (0x60 bytes; ImageBase at +0x1C = file 0xB4)
 *   0x0F8  Section header (40 bytes): ".data", VA=0x1000, VSize=0x40,
 *                                      RawSize=0x40, RawPtr=0x140
 *   0x120  pad
 *   0x140  section raw data (0x40 bytes)
 *   total file size = 0x200 (pad with zeros to meet loader's 0x200 floor)
 */
static size_t build_minimal_pe(uint8_t *out, size_t cap,
                                uint32_t image_base,
                                const uint8_t section_data[64])
{
    if (cap < 0x200) return 0;
    memset(out, 0, 0x200);

    /* DOS stub */
    out[0x00] = 'M'; out[0x01] = 'Z';
    uint32_t e_lfanew = 0x80;
    memcpy(out + 0x3C, &e_lfanew, 4);

    /* PE signature */
    out[0x80] = 'P'; out[0x81] = 'E'; out[0x82] = 0; out[0x83] = 0;

    /* COFF header @ 0x84 */
    uint16_t machine        = 0x014C;        /* IMAGE_FILE_MACHINE_I386 */
    uint16_t num_sections   = 1;
    uint16_t opt_hdr_size   = 0x60;
    uint16_t characteristics = 0x0103;
    memcpy(out + 0x84, &machine, 2);
    memcpy(out + 0x86, &num_sections, 2);
    /* TimeDateStamp, SymTab, NumSyms = 0 */
    memcpy(out + 0x94, &opt_hdr_size, 2);
    memcpy(out + 0x96, &characteristics, 2);

    /* Optional header @ 0x98 */
    uint16_t opt_magic = 0x010B;              /* PE32 */
    memcpy(out + 0x98, &opt_magic, 2);
    /* ImageBase at opt_off + 0x1C = file 0xB4 */
    memcpy(out + 0xB4, &image_base, 4);

    /* Section header @ 0xF8 */
    const char *secname = ".data";
    memcpy(out + 0xF8, secname, strlen(secname));     /* name (nul-padded) */
    uint32_t vsize   = 0x40;
    uint32_t va      = 0x1000;
    uint32_t rawsize = 0x40;
    uint32_t rawptr  = 0x140;
    memcpy(out + 0xF8 + 0x08, &vsize,   4);
    memcpy(out + 0xF8 + 0x0C, &va,      4);
    memcpy(out + 0xF8 + 0x10, &rawsize, 4);
    memcpy(out + 0xF8 + 0x14, &rawptr,  4);

    /* Section raw data @ 0x140 */
    memcpy(out + 0x140, section_data, 64);

    return 0x200;
}

static int write_file_bytes(const char *path, const void *buf, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    size_t w = fwrite(buf, 1, n, f);
    fclose(f);
    return w == n;
}

/* ---- tests -------------------------------------------------------------- */

TEST(init_then_read_returns_section_bytes)
{
    /* Known pattern in the section's 64 bytes. */
    uint8_t data[64];
    for (int i = 0; i < 64; ++i) data[i] = (uint8_t)(0xA0 + i);

    uint8_t blob[512];
    size_t n = build_minimal_pe(blob, sizeof blob, 0x00400000u, data);
    ASSERT_EQ(n, 0x200);
    ASSERT_TRUE(write_file_bytes(kTmpPe, blob, n));

    PeLoaderFree();    /* clear any previous test's state */
    ASSERT_EQ(PeLoaderInit(kTmpPe), 1);
    ASSERT_EQ(PeLoaderLoaded(), 1);

    /* VA = ImageBase + section.VA = 0x00401000. */
    const uint8_t *p = (const uint8_t *)PeLoaderRead(0x00401000u);
    ASSERT_NOT_NULL(p);
    ASSERT_MEMEQ(p, data, 64);

    /* Read in the middle of the section. */
    const uint8_t *p_mid = (const uint8_t *)PeLoaderRead(0x00401010u);
    ASSERT_NOT_NULL(p_mid);
    ASSERT_EQ(*p_mid, 0xA0 + 0x10);

    PeLoaderFree();
    remove(kTmpPe);
}

TEST(contains_va_respects_image_bounds)
{
    uint8_t data[64];
    memset(data, 0x77, sizeof data);

    uint8_t blob[512];
    build_minimal_pe(blob, sizeof blob, 0x00400000u, data);
    write_file_bytes(kTmpPe, blob, 0x200);

    PeLoaderFree();
    ASSERT_EQ(PeLoaderInit(kTmpPe), 1);

    /* Image VA range: [0x00400000, 0x00401040). */
    ASSERT_EQ(PeLoaderContainsVA(0x00400000u), 1);
    ASSERT_EQ(PeLoaderContainsVA(0x00401000u), 1);
    ASSERT_EQ(PeLoaderContainsVA(0x0040103Fu), 1);

    ASSERT_EQ(PeLoaderContainsVA(0x003FFFFFu), 0);   /* below base */
    ASSERT_EQ(PeLoaderContainsVA(0x00401040u), 0);   /* one past end */
    ASSERT_EQ(PeLoaderContainsVA(0x00500000u), 0);

    PeLoaderFree();
    remove(kTmpPe);
}

TEST(read_below_image_base_returns_null)
{
    uint8_t data[64] = { 0 };
    uint8_t blob[512];
    build_minimal_pe(blob, sizeof blob, 0x00400000u, data);
    write_file_bytes(kTmpPe, blob, 0x200);

    PeLoaderFree();
    ASSERT_EQ(PeLoaderInit(kTmpPe), 1);

    ASSERT_NULL(PeLoaderRead(0x00200000u));     /* below image_base */
    ASSERT_NULL(PeLoaderRead(0x10000000u));     /* far above mapped extent */

    PeLoaderFree();
    remove(kTmpPe);
}

TEST(init_rejects_nonexistent_file)
{
    PeLoaderFree();
    int rc = PeLoaderInit("/tmp/this-pe-should-not-exist.exe");
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(PeLoaderLoaded(), 0);
}

TEST(init_rejects_non_mz)
{
    uint8_t blob[512];
    memset(blob, 0, sizeof blob);
    /* No MZ magic. */
    blob[0] = 'N'; blob[1] = 'O';
    write_file_bytes(kTmpPe, blob, sizeof blob);

    PeLoaderFree();
    ASSERT_EQ(PeLoaderInit(kTmpPe), 0);

    remove(kTmpPe);
}

TEST(read_without_init_returns_null)
{
    PeLoaderFree();
    ASSERT_EQ(PeLoaderLoaded(), 0);
    ASSERT_NULL(PeLoaderRead(0x00401000u));
    ASSERT_EQ(PeLoaderContainsVA(0x00401000u), 0);
}

TEST(custom_image_base)
{
    /* Verify the loader honors image_base from the optional header. */
    uint8_t data[64];
    memset(data, 0xEE, sizeof data);

    uint8_t blob[512];
    build_minimal_pe(blob, sizeof blob, 0x10000000u, data);
    write_file_bytes(kTmpPe, blob, 0x200);

    PeLoaderFree();
    ASSERT_EQ(PeLoaderInit(kTmpPe), 1);

    /* Section at VA 0x10001000 (base 0x10000000 + section VA 0x1000). */
    const uint8_t *p = (const uint8_t *)PeLoaderRead(0x10001000u);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(p[0], 0xEE);
    ASSERT_EQ(PeLoaderContainsVA(0x10001020u), 1);
    ASSERT_EQ(PeLoaderContainsVA(0x00401000u), 0);   /* old base no longer mapped */

    PeLoaderFree();
    remove(kTmpPe);
}

SUITE(pe_loader)
{
    RUN_TEST(init_then_read_returns_section_bytes);
    RUN_TEST(contains_va_respects_image_bounds);
    RUN_TEST(read_below_image_base_returns_null);
    RUN_TEST(init_rejects_nonexistent_file);
    RUN_TEST(init_rejects_non_mz);
    RUN_TEST(read_without_init_returns_null);
    RUN_TEST(custom_image_base);
}
