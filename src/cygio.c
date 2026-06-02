/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* src/cygio.c — Cygert "Base_IO_CPP" file-class shim.
 *
 * In the original WACKI.EXE these are methods of a small C++ file
 * class (the literal "Base_IO_CPP by Henryk Cygert" sits in the
 * object's vtable). The port collapses them to thin stdio wrappers —
 * we keep the original signatures so the archive + asset loaders
 * stay unchanged. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct CygFile { FILE *fp; } CygFile;

CygFile *fopen_cyg(const char *name, const char *mode)
{
    FILE *fp = fopen(name, mode);
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
