/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * assets-explorer/src/catalog.h — thin browse layer over archive.c for the asset
 * explorer. Mounts a .dta, exposes its SPIS directory as a flat array of
 * typed entries, and depacks an entry on demand. The viewer UI talks only
 * to this header; it never touches archive.c internals directly. */

#ifndef WACKI_VIEWER_CATALOG_H
#define WACKI_VIEWER_CATALOG_H

#include <stdint.h>

/* Asset class, guessed from the 8.3 name's extension for the list (cheap,
 * no depack), then confirmed against the real 4-byte magic once an entry is
 * selected and decompressed. Order = display grouping order. */
typedef enum {
    AT_ANIM,        /* .wyc — sprite atlas (ANIM) */
    AT_MASK,        /* .msk — click-region mask (MASK) */
    AT_FILD,        /* .fld — walkability / perspective (FILD) */
    AT_PIC,         /* .pic — full-screen background */
    AT_PAL,         /* .pal — 256-colour palette */
    AT_WAV,         /* .wav — audio sample */
    AT_FLIC,        /* .avi / .flc — cutscene */
    AT_SCRIPT,      /* .scr — Wacky.scr script text */
    AT_UNKNOWN,     /* anything else → hex */
    AT__COUNT
} AssetType;

typedef struct {
    char      name[16];     /* NUL-terminated copy of the 12-byte DTA name */
    uint32_t  offset;       /* absolute byte offset of the entry in the .dta */
    AssetType type;         /* extension-based guess */
} CatalogEntry;

/* Mount `dta_path` (replaces any previously mounted archive) and build the
 * entry table. Returns 1 on success, 0 if the archive can't be opened. */
int                 catalog_open(const char *dta_path);
void                catalog_close(void);

int                 catalog_count(void);
const CatalogEntry *catalog_entry(int i);          /* NULL if i out of range */
const char         *catalog_path(void);            /* mounted .dta path */

/* Short upper-case tag for a type ("ANIM", "PIC", "?HEX", …). */
const char         *catalog_type_label(AssetType t);

/* Depack entry `i` in full. On success *buf owns the bytes (free via
 * catalog_free) and *sz is the unpacked length. Returns 1/0. */
int                 catalog_load(int i, void **buf, uint32_t *sz);
void                catalog_free(void *buf);

/* Read the per-entry PKv2 header (no full depack): compressed + unpacked
 * sizes straight from the stream at the entry offset. Returns 1/0. */
int                 catalog_entry_sizes(int i, uint32_t *compressed,
                                        uint32_t *unpacked);

/* Map the leading 4-byte magic of a depacked buffer to a label, or NULL if
 * it isn't one of the known asset magics. */
const char         *catalog_magic_label(const void *buf, uint32_t sz);

#endif /* WACKI_VIEWER_CATALOG_H */
