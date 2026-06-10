/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/cygio.c — Cygert "Base_IO_CPP" file-class shim.
 *
 * In the original WACKI.EXE these are methods of a small C++ file
 * class (the literal "Base_IO_CPP by Henryk Cygert" sits in the
 * object's vtable). The port collapses them to thin stdio wrappers —
 * we keep the original signatures so the archive + asset loaders
 * stay unchanged. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct CygFile { FILE *fp; } CygFile;

#ifdef WACKI_PS2
/* PS2 cdrom0: file I/O wants DOS-style paths: backslash separators,
 * upper-case names, and an ISO9660 ';1' version suffix on the file. The
 * engine builds unix-ish paths (forward-slash joiner, mixed case), so
 * rewrite cdrom0: paths here — the single fopen chokepoint for every DTA
 * archive read. host:/mass: paths pass through untouched (HostFS + USB
 * use forward slashes and are case-sensitive). Shared with data_root.c's
 * probe via an extern declaration there. */
void ps2_normalize_path(const char *in, char *out, size_t outsz)
{
    if (outsz == 0) return;
    if (strncmp(in, "cdrom0:", 7) != 0) {
        snprintf(out, outsz, "%s", in);
        return;
    }
    size_t o = 0;
    /* Keep the device token verbatim — the device match is exact and
     * lower-case ("cdrom0:"); only the path that follows is DOS-ified. */
    for (size_t k = 0; k < 7 && o + 1 < outsz; ++k) out[o++] = in[k];
    for (size_t i = 7; in[i] && o + 3 < outsz; ++i) {
        char c = in[i];
        if (c == '/')                   c = '\\';
        else if (c >= 'a' && c <= 'z')  c = (char)(c - 32);
        out[o++] = c;
    }
    out[o] = 0;
    if (!strstr(out + 7, ";1") && o + 2 < outsz) {
        out[o++] = ';';
        out[o++] = '1';
        out[o]   = 0;
    }
}
#endif

CygFile *fopen_cyg(const char *name, const char *mode)
{
#ifdef WACKI_PS2
    char fixed[320];
    ps2_normalize_path(name, fixed, sizeof fixed);
    name = fixed;
#endif
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
