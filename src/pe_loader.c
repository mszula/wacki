/* src/pe_loader.c — PE-VA resolver, backed by an embedded slice table.
 *
 * The engine resolves original WACKI.EXE addresses via PeLoaderRead.
 * The default storage is a static `PeSlice[]` table generated at
 * build time by tools/embed-pe-data (see include/wacki/embedded_exe.h
 * + src/embedded_wacki_pe.c). No PE parsing happens at runtime — the
 * data lives in `.rodata` and is mapped from process start.
 *
 * The legacy file-based init paths (PeLoaderInit, PeLoaderInitFromMemory)
 * are kept for two reasons:
 *
 *   1. The unit tests construct synthetic PE blobs in tmpfs and
 *      verify the parser's edge-case handling (malformed headers,
 *      truncated section tables, etc.).
 *   2. Standalone tools that want to inspect a different PE without
 *      rebuilding the engine.
 *
 * When either init call succeeds, the runtime image takes precedence
 * over the embedded slice table. PeLoaderFree releases the runtime
 * image and falls back to the embedded path. */

#include "wacki.h"
#include "wacki/embedded_exe.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- DOS / PE header offsets (for the legacy parser path) -------- */

#define DOS_OFF_E_LFANEW            0x3C
#define DOS_MIN_FILE_SIZE           0x200

#define PE_SIG_BYTES                4

#define COFF_HEADER_BYTES           20
#define COFF_OFF_NUM_SECTIONS       0x02
#define COFF_OFF_OPT_HEADER_SIZE    0x10
#define COFF_OPT_HEADER_MIN_BYTES   0x60

#define PE_HEADER_TAIL_GUARD        0xE0

#define OPT_HEADER_OFF_IMAGE_BASE   0x1C

#define SECTION_HEADER_BYTES        40
#define SECTION_OFF_VSIZE           0x08
#define SECTION_OFF_VA              0x0C
#define SECTION_OFF_RSIZE           0x10
#define SECTION_OFF_RPTR            0x14

#define MIN_VIRTUAL_EXTENT          0x1000
#define MAX_VIRTUAL_EXTENT          0x10000000

/* ---- module state ------------------------------------------------- */

/* Dynamic-mode image — populated by PeLoaderInit / InitFromMemory.
 * NULL means lookups fall through to the embedded slice table. */
static uint8_t  *g_pe_image      = NULL;
static size_t    g_pe_image_size = 0;
static uint32_t  g_pe_image_base = 0;

/* ---- embedded-slice lookup --------------------------------------- */

static const void *embedded_read(uint32_t va)
{
    for (int i = 0; i < g_wacki_pe_slice_count; ++i) {
        const PeSlice *s = &g_wacki_pe_slices[i];
        if (va < s->va_start || va >= s->va_end) continue;
        uint32_t off_in_slice = va - s->va_start;
        if (off_in_slice >= s->raw_size) {
            /* BSS tail — unmapped in the embedded blob. The engine
             * never references this in practice. */
            return NULL;
        }
        return &g_wacki_pe_blob[s->blob_off + off_in_slice];
    }
    return NULL;
}

static int embedded_contains(uint32_t va)
{
    for (int i = 0; i < g_wacki_pe_slice_count; ++i) {
        const PeSlice *s = &g_wacki_pe_slices[i];
        if (va >= s->va_start && va < s->va_end) return 1;
    }
    return 0;
}

/* ---- helpers for the legacy dynamic parser ----------------------- */

static uint8_t *slurp_file(FILE *fp, const char *path, size_t *out_size)
{
    fseek(fp, 0, SEEK_END);
    long fsz_l = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (fsz_l < DOS_MIN_FILE_SIZE) {
        fclose(fp);
        return NULL;
    }
    size_t   fsz  = (size_t)fsz_l;
    uint8_t *file = (uint8_t *)malloc(fsz);
    if (!file) { fclose(fp); return NULL; }
    if (fread(file, 1, fsz, fp) != fsz) {
        fprintf(stderr, "[pe] short read on %s\n", path);
        free(file);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    *out_size = fsz;
    return file;
}

static uint32_t parse_pe_signature(const uint8_t *file, size_t fsz,
                                   const char *path)
{
    if (file[0] != 'M' || file[1] != 'Z') {
        fprintf(stderr, "[pe] %s: not an MZ executable\n", path);
        return 0;
    }
    uint32_t pe_off = *(const uint32_t *)(file + DOS_OFF_E_LFANEW);
    if (pe_off + PE_SIG_BYTES + COFF_HEADER_BYTES +
            PE_HEADER_TAIL_GUARD > fsz) {
        fprintf(stderr, "[pe] %s: PE header offset out of range\n", path);
        return 0;
    }
    if (file[pe_off]     != 'P' || file[pe_off + 1] != 'E' ||
        file[pe_off + 2] != 0   || file[pe_off + 3] != 0)
    {
        fprintf(stderr, "[pe] %s: missing PE signature\n", path);
        return 0;
    }
    return pe_off;
}

static uint32_t compute_image_extent(const uint8_t *file, uint32_t sec_off,
                                     uint16_t num_sections)
{
    uint32_t max_va_end = 0;
    for (int i = 0; i < num_sections; ++i) {
        uint32_t shdr  = sec_off + (uint32_t)i * SECTION_HEADER_BYTES;
        uint32_t vsize = *(const uint32_t *)(file + shdr + SECTION_OFF_VSIZE);
        uint32_t va    = *(const uint32_t *)(file + shdr + SECTION_OFF_VA);
        uint32_t rsize = *(const uint32_t *)(file + shdr + SECTION_OFF_RSIZE);
        uint32_t use_size = vsize > rsize ? vsize : rsize;
        uint32_t end      = va + use_size;
        if (end > max_va_end) max_va_end = end;
    }
    return max_va_end;
}

static int copy_sections(const uint8_t *file, size_t fsz,
                         uint32_t sec_off, uint16_t num_sections,
                         uint8_t *image, uint32_t image_size)
{
    int copied = 0;
    for (int i = 0; i < num_sections; ++i) {
        uint32_t shdr  = sec_off + (uint32_t)i * SECTION_HEADER_BYTES;
        uint32_t va    = *(const uint32_t *)(file + shdr + SECTION_OFF_VA);
        uint32_t rsize = *(const uint32_t *)(file + shdr + SECTION_OFF_RSIZE);
        uint32_t rptr  = *(const uint32_t *)(file + shdr + SECTION_OFF_RPTR);
        if (!rsize) continue;
        if ((size_t)rptr + (size_t)rsize > fsz) {
            fprintf(stderr, "[pe] section %d points past EOF — skipping\n", i);
            continue;
        }
        if (va + rsize > image_size) continue;
        memcpy(image + va, file + rptr, rsize);
        ++copied;
    }
    return copied;
}

/* ---- public API ------------------------------------------------- */

int PeLoaderInitFromMemory(const uint8_t *file, size_t fsz,
                           const char *label)
{
    if (!file || fsz < DOS_MIN_FILE_SIZE) return 0;
    if (!label) label = "<memory>";

    uint32_t pe_off = parse_pe_signature(file, fsz, label);
    if (!pe_off) return 0;

    uint32_t coff_off = pe_off + PE_SIG_BYTES;
    uint16_t num_sections = *(const uint16_t *)
        (file + coff_off + COFF_OFF_NUM_SECTIONS);
    uint16_t opt_header_size = *(const uint16_t *)
        (file + coff_off + COFF_OFF_OPT_HEADER_SIZE);
    if (opt_header_size < COFF_OPT_HEADER_MIN_BYTES) {
        fprintf(stderr, "[pe] %s: optional header too small (%u)\n",
                label, opt_header_size);
        return 0;
    }

    uint32_t opt_off    = coff_off + COFF_HEADER_BYTES;
    uint32_t image_base = *(const uint32_t *)
        (file + opt_off + OPT_HEADER_OFF_IMAGE_BASE);

    uint32_t sec_off = opt_off + opt_header_size;
    if (sec_off + (uint32_t)num_sections * SECTION_HEADER_BYTES > fsz) {
        fprintf(stderr, "[pe] %s: section table out of range\n", label);
        return 0;
    }

    uint32_t max_va_end = compute_image_extent(file, sec_off, num_sections);
    if (max_va_end < MIN_VIRTUAL_EXTENT ||
        max_va_end > MAX_VIRTUAL_EXTENT) {
        fprintf(stderr, "[pe] %s: implausible virtual extent %u\n",
                label, max_va_end);
        return 0;
    }

    uint8_t *image = (uint8_t *)calloc(max_va_end, 1);
    if (!image) {
        fprintf(stderr, "[pe] cannot allocate %u bytes for image\n",
                max_va_end);
        return 0;
    }
    int sections_copied = copy_sections(file, fsz, sec_off, num_sections,
                                        image, max_va_end);

    g_pe_image      = image;
    g_pe_image_size = max_va_end;
    g_pe_image_base = image_base;
    fprintf(stderr, "[pe] mapped %s: base=0x%08X size=%u sections=%d\n",
            label, image_base, (unsigned)max_va_end, sections_copied);
    return 1;
}

int PeLoaderInit(const char *exe_path)
{
    if (!exe_path) return 0;

    FILE *fp = fopen(exe_path, "rb");
    if (!fp) {
        fprintf(stderr, "[pe] cannot open %s\n", exe_path);
        return 0;
    }
    size_t   fsz  = 0;
    uint8_t *file = slurp_file(fp, exe_path, &fsz);
    if (!file) return 0;

    int ok = PeLoaderInitFromMemory(file, fsz, exe_path);
    free(file);
    return ok;
}

void PeLoaderFree(void)
{
    if (g_pe_image) free(g_pe_image);
    g_pe_image      = NULL;
    g_pe_image_size = 0;
    g_pe_image_base = 0;
}

const void *PeLoaderRead(uint32_t va)
{
    /* Dynamic image takes precedence (tests, ad-hoc tools). */
    if (g_pe_image) {
        if (va < g_pe_image_base) return NULL;
        uint32_t off = va - g_pe_image_base;
        if (off >= g_pe_image_size) return NULL;
        return g_pe_image + off;
    }
    return embedded_read(va);
}

/* True when *some* PE image is available — either a runtime-mapped
 * one or the embedded slice table (the latter is present as long as
 * embedded_wacki_pe.c is linked in). */
int PeLoaderLoaded(void)
{
    return g_pe_image != NULL || g_wacki_pe_slice_count > 0;
}

int PeLoaderContainsVA(uint32_t va)
{
    if (g_pe_image) {
        if (va < g_pe_image_base) return 0;
        return (va - g_pe_image_base) < g_pe_image_size;
    }
    return embedded_contains(va);
}
