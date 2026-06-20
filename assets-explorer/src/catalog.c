/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * assets-explorer/src/catalog.c — browse layer over archive.c (see catalog.h).
 *
 * archive.c keeps the mounted directory in two non-static symbols (s_dir +
 * s_dir_count) precisely so out-of-engine tools can walk it — tools/dta-
 * extract.c does the same. We copy that directory into a typed, NUL-
 * terminated CatalogEntry[] once at mount time so the UI can sort/filter it
 * without re-touching engine internals every frame. */

#include "catalog.h"

#include "wacki/types.h"                 /* DtaIndexEntry, DTA_NAME_LEN, magics */
#include "wacki/api.h"                   /* OpenDtaArchiveFile, LoadFileFromDta */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* archive.c exposes the mounted SPIS directory non-static (same access path
 * tools/dta-extract.c uses). file_offset is the absolute entry offset. */
extern DtaIndexEntry *s_dir;
extern int32_t        s_dir_count;

extern void           xfree(void *p);    /* heap.c — pairs LoadFileFromDta's xmalloc */

/* Each PKv2 stream starts with {u32 magic, u32 compressed, u32 unpacked}. We
 * read those three words directly for the size readout without depacking. */
#define PKV2_HDR_WORDS  3

static CatalogEntry *s_entries = NULL;
static int           s_count   = 0;
static char          s_path[512];

/* ASCII extension → type. Case-insensitive; ext is the 3 chars after '.'. */
static AssetType type_from_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot || !dot[1]) return AT_UNKNOWN;

    char e[4] = {0};
    for (int i = 0; i < 3 && dot[1 + i]; ++i) {
        char c = dot[1 + i];
        e[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    if (!strcmp(e, "WYC")) return AT_ANIM;
    if (!strcmp(e, "MSK")) return AT_MASK;
    if (!strcmp(e, "FLD")) return AT_FILD;
    if (!strcmp(e, "PIC")) return AT_PIC;
    if (!strcmp(e, "PAL")) return AT_PAL;
    if (!strcmp(e, "WAV")) return AT_WAV;
    if (!strcmp(e, "AVI")) return AT_FLIC;
    if (!strcmp(e, "FLC")) return AT_FLIC;
    if (!strcmp(e, "SCR")) return AT_SCRIPT;
    return AT_UNKNOWN;
}

const char *catalog_type_label(AssetType t)
{
    switch (t) {
        case AT_ANIM:   return "ANIM";
        case AT_MASK:   return "MASK";
        case AT_FILD:   return "FILD";
        case AT_PIC:    return "PIC";
        case AT_PAL:    return "PAL";
        case AT_WAV:    return "WAV";
        case AT_FLIC:   return "FLIC";
        case AT_SCRIPT: return "SCR";
        default:        return "?HEX";
    }
}

void catalog_close(void)
{
    free(s_entries);
    s_entries = NULL;
    s_count   = 0;
    s_path[0] = 0;
}

int catalog_open(const char *dta_path)
{
    catalog_close();

    if (!OpenDtaArchiveFile(dta_path)) return 0;
    if (s_dir_count <= 0) return 0;

    s_entries = (CatalogEntry *)calloc((size_t)s_dir_count, sizeof *s_entries);
    if (!s_entries) return 0;

    for (int i = 0; i < s_dir_count; ++i) {
        CatalogEntry *ce = &s_entries[i];
        /* DTA names are 12 bytes, NUL-padded only when shorter than 12 —
         * copy exactly DTA_NAME_LEN then force-terminate. */
        memcpy(ce->name, s_dir[i].name, DTA_NAME_LEN);
        ce->name[DTA_NAME_LEN] = '\0';
        ce->offset = s_dir[i].file_offset;
        ce->type   = type_from_ext(ce->name);
    }
    s_count = (int)s_dir_count;
    snprintf(s_path, sizeof s_path, "%s", dta_path);
    return 1;
}

int catalog_count(void) { return s_count; }
const char *catalog_path(void) { return s_path; }

const CatalogEntry *catalog_entry(int i)
{
    if (i < 0 || i >= s_count) return NULL;
    return &s_entries[i];
}

int catalog_load(int i, void **buf, uint32_t *sz)
{
    const CatalogEntry *ce = catalog_entry(i);
    if (!ce) return 0;
    return LoadFileFromDta(ce->name, buf, sz);
}

void catalog_free(void *buf) { if (buf) xfree(buf); }

int catalog_entry_sizes(int i, uint32_t *compressed, uint32_t *unpacked)
{
    const CatalogEntry *ce = catalog_entry(i);
    if (!ce) return 0;

    /* Open the .dta independently of archive.c's held handle and read just
     * the 12-byte PKv2 header at the entry offset — cheaper than a depack
     * and lets the UI show the compression ratio. */
    FILE *fp = fopen(s_path, "rb");
    if (!fp) return 0;

    uint32_t hdr[PKV2_HDR_WORDS] = {0};
    int ok = (fseek(fp, (long)ce->offset, SEEK_SET) == 0) &&
             (fread(hdr, sizeof(uint32_t), PKV2_HDR_WORDS, fp) == PKV2_HDR_WORDS);
    fclose(fp);
    if (!ok) return 0;

    if (compressed) *compressed = hdr[1];
    if (unpacked)   *unpacked   = hdr[2];
    return 1;
}

const char *catalog_magic_label(const void *buf, uint32_t sz)
{
    if (!buf || sz < 4) return NULL;
    uint32_t m;
    memcpy(&m, buf, 4);
    switch (m) {
        case ASSET_MAGIC_ANIM: return "ANIM";
        case ASSET_MAGIC_MASK: return "MASK";
        case ASSET_MAGIC_FILD: return "FILD";
        default:               return NULL;
    }
}
