/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform/sdl/file_host.c — file I/O shim (storage HAL), stdio backend.
 *
 * The Cygert "Base_IO_CPP" file class collapsed to thin wrappers over newlib
 * stdio. This is the desktop + handheld implementation of the fopen_cyg /
 * fread_cyg / fseek_cyg / ftell_cyg / fclose_cyg contract declared in
 * wacki/platform/storage.h. The PS2 backend (fileXio, since ps2sdk's newlib
 * fopen reaches no device) lives in src/platform/ps2/storage_ps2.c. */

#include "wacki/platform/storage.h"   /* CygFile + fopen_cyg/... contract */
#ifdef __ANDROID__
#include "wacki/platform/android_saf.h"   /* read-in-place from the SAF tree */
#endif

#include <stdio.h>
#include <stdlib.h>

struct CygFile { FILE *fp; };         /* completes storage.h's opaque CygFile */

CygFile *fopen_cyg(const char *name, const char *mode)
{
    FILE *fp = NULL;
#ifdef __ANDROID__
    /* When the data root is the SAF tree (read-in-place, no copy), archive +
     * asset reads resolve to a content:// fd instead of a real path. Read
     * modes only; if the name isn't in the tree this returns NULL and we fall
     * through to fopen() (which fails on the "saf:/" sentinel root — i.e. the
     * file genuinely isn't there). */
    if (android_saf_active() && mode && mode[0] == 'r')
        fp = android_saf_fopen(name);
    if (!fp)
#endif
    fp = fopen(name, mode);
    if (!fp) return NULL;
    CygFile *f = (CygFile *)malloc(sizeof *f);
    if (!f) { fclose(fp); return NULL; }
    f->fp = fp;
    return f;
}

void fclose_cyg(CygFile *f)
{
    if (!f) return;
    fclose(f->fp);
    free(f);
}

uint32_t fread_cyg(void *dst, uint32_t sz, uint32_t n, CygFile *f)
{
    return (uint32_t)fread(dst, sz, n, f->fp);
}

void fseek_cyg(CygFile *f, int32_t off, int whence)
{
    fseek(f->fp, (long)off, whence);
}

int32_t ftell_cyg(CygFile *f)
{
    return (int32_t)ftell(f->fp);
}
