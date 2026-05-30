/*
 * pe_loader.c — Load WACKI.EXE as a passive memory image so the port
 * can resolve any original bytecode/data address (verb tables, object
 * tables, embedded scripts, asset name strings, click payloads, etc.)
 * without having to manually transcribe each blob into binary_data.c.
 *
 * Strategy: parse the PE header just far enough to locate each section
 * (name, VirtualAddress, SizeOfRawData, PointerToRawData), then build
 * a flat in-memory image of size `max(VA + VirtualSize)`. Each section
 * is memcpy'd from its file location to (image + VA). `PeLoaderRead(va)`
 * returns a host pointer for any original virtual address.
 *
 * No CODE is executed — this is pure data lookup. The .text section is
 * loaded too (so even pointer references INTO .text resolve), but my
 * port never interprets it as x86 — only as opaque bytes for the few
 * cases scripts reference code pointers (almost never).
 *
 * The flat image is ~512 KB for WACKI.EXE — trivially cheap.
 */
#include "wacki.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t  *g_pe_image       = NULL;
static size_t    g_pe_image_size  = 0;
static uint32_t  g_pe_image_base  = 0;

int PeLoaderInit(const char *exe_path)
{
    if (!exe_path) return 0;
    FILE *fp = fopen(exe_path, "rb");
    if (!fp) {
        fprintf(stderr, "[pe] cannot open %s\n", exe_path);
        return 0;
    }
    fseek(fp, 0, SEEK_END);
    long fsz_l = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (fsz_l < 0x200) { fclose(fp); return 0; }
    size_t fsz = (size_t)fsz_l;
    uint8_t *file = (uint8_t *)malloc(fsz);
    if (!file) { fclose(fp); return 0; }
    if (fread(file, 1, fsz, fp) != fsz) {
        fprintf(stderr, "[pe] short read on %s\n", exe_path);
        free(file); fclose(fp); return 0;
    }
    fclose(fp);

    /* DOS stub: 'MZ' magic, e_lfanew at +0x3C points to PE header. */
    if (file[0] != 'M' || file[1] != 'Z') {
        fprintf(stderr, "[pe] %s: not an MZ executable\n", exe_path);
        free(file); return 0;
    }
    uint32_t pe_off = *(uint32_t *)(file + 0x3C);
    if (pe_off + 0x18 + 0xE0 > fsz) {
        fprintf(stderr, "[pe] %s: PE header offset out of range\n", exe_path);
        free(file); return 0;
    }
    if (file[pe_off] != 'P' || file[pe_off+1] != 'E' ||
        file[pe_off+2] != 0  || file[pe_off+3] != 0) {
        fprintf(stderr, "[pe] %s: missing PE signature\n", exe_path);
        free(file); return 0;
    }

    /* COFF header @ pe_off+4: Machine(2) NumberOfSections(2) TimeDateStamp(4)
 * PtrToSymTable(4) NumberOfSymbols(4) SizeOfOptionalHeader(2) Characteristics(2) */
    uint16_t num_sections    = *(uint16_t *)(file + pe_off + 0x06);
    uint16_t opt_header_size = *(uint16_t *)(file + pe_off + 0x14);
    if (opt_header_size < 0x60) {
        fprintf(stderr, "[pe] %s: optional header too small (%u)\n", exe_path, opt_header_size);
        free(file); return 0;
    }
    /* Optional header (PE32) — ImageBase at +0x1C of optional header.
 * Optional header starts at pe_off + 0x18 (after Signature+CoffHeader). */
    uint32_t opt_off = pe_off + 0x18;
    uint32_t image_base = *(uint32_t *)(file + opt_off + 0x1C);

    /* Section headers immediately follow the optional header. Each is 40 bytes:
 * Name[8] VirtualSize(4) VirtualAddress(4) SizeOfRawData(4) PointerToRawData(4)
 * PointerToRelocations(4) PointerToLineNumbers(4) NumberOfRelocations(2)
 * NumberOfLineNumbers(2) Characteristics(4)
 */
    uint32_t sec_off = opt_off + opt_header_size;
    if (sec_off + (uint32_t)num_sections * 40 > fsz) {
        fprintf(stderr, "[pe] %s: section table out of range\n", exe_path);
        free(file); return 0;
    }

    /* Walk sections, find max virtual extent. */
    uint32_t max_va_end = 0;
    for (int i = 0; i < num_sections; ++i) {
        uint32_t shdr = sec_off + (uint32_t)i * 40;
        uint32_t vsize = *(uint32_t *)(file + shdr + 0x08);
        uint32_t va    = *(uint32_t *)(file + shdr + 0x0C);
        uint32_t rsize = *(uint32_t *)(file + shdr + 0x10);
        uint32_t use_size = vsize > rsize ? vsize : rsize;
        uint32_t end = va + use_size;
        if (end > max_va_end) max_va_end = end;
    }
    if (max_va_end < 0x1000 || max_va_end > 0x10000000) {
        fprintf(stderr, "[pe] %s: implausible virtual extent %u\n", exe_path, max_va_end);
        free(file); return 0;
    }

    /* Allocate flat image and copy each section's raw bytes to its VA. */
    uint8_t *image = (uint8_t *)calloc(max_va_end, 1);
    if (!image) {
        fprintf(stderr, "[pe] cannot allocate %u bytes for image\n", max_va_end);
        free(file); return 0;
    }
    int sections_copied = 0;
    for (int i = 0; i < num_sections; ++i) {
        uint32_t shdr = sec_off + (uint32_t)i * 40;
        uint32_t va    = *(uint32_t *)(file + shdr + 0x0C);
        uint32_t rsize = *(uint32_t *)(file + shdr + 0x10);
        uint32_t rptr  = *(uint32_t *)(file + shdr + 0x14);
        if (!rsize) continue;
        if ((size_t)rptr + (size_t)rsize > fsz) {
            fprintf(stderr, "[pe] section %d points past EOF — skipping\n", i);
            continue;
        }
        if (va + rsize > max_va_end) continue;
        memcpy(image + va, file + rptr, rsize);
        ++sections_copied;
    }

    g_pe_image       = image;
    g_pe_image_size  = max_va_end;
    g_pe_image_base  = image_base;
    fprintf(stderr, "[pe] mapped %s: base=0x%08X size=%u sections=%d\n",
            exe_path, image_base, (unsigned)max_va_end, sections_copied);
    free(file);
    return 1;
}

void PeLoaderFree(void)
{
    if (g_pe_image) free(g_pe_image);
    g_pe_image = NULL;
    g_pe_image_size = 0;
    g_pe_image_base = 0;
}

const void *PeLoaderRead(uint32_t va)
{
    if (!g_pe_image) return NULL;
    if (va < g_pe_image_base) return NULL;
    uint32_t off = va - g_pe_image_base;
    if (off >= g_pe_image_size) return NULL;
    return g_pe_image + off;
}

int PeLoaderLoaded(void) { return g_pe_image != NULL; }

/* PeLoaderContainsVA — true iff `va` falls inside the loaded PE image's
 * mapped address range (= original .text/.rdata/.data sections).
 *
 * Used by bytecode handlers to decide whether an operand is a PE VA that
 * needs xlat_binary_ptr translation, or an already-resolved native
 * pointer that should be cast through (uintptr_t).
 *
 * Earlier port used a hardcoded `[0x00400000, 0x00500000)` check based on
 * WACKI.EXE's image base + a generous size margin. This helper makes the
 * check exact (uses actual image size from PE headers) and portable to
 * other PE images that don't share WACKI's specific layout. */
int PeLoaderContainsVA(uint32_t va)
{
    if (!g_pe_image) return 0;
    if (va < g_pe_image_base) return 0;
    return (va - g_pe_image_base) < g_pe_image_size;
}
