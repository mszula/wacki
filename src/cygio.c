/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/cygio.c — Cygert "Base_IO_CPP" file-class shim.
 *
 * In the original WACKI.EXE these are methods of a small C++ file
 * class (the literal "Base_IO_CPP by Henryk Cygert" sits in the
 * object's vtable). The port collapses them to thin wrappers — we keep
 * the original signatures so the archive + asset loaders stay unchanged.
 *
 * Two backends behind the same shim:
 *   - desktop / handheld: newlib stdio (FILE *).
 *   - PS2 (WACKI_PS2): fileXio. ps2sdk's newlib fopen reaches no device,
 *     so every DTA archive read routes through fileXio instead — the
 *     IOP fileio stack is brought up in platform_ps2.c first. This single
 *     chokepoint is exactly why the engine itself needs no other changes.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef WACKI_PS2

/* fileXio hand-declared to dodge ps2sdk's guarded fileXio_rpc.h (it
 * #errors on mixing fio/fileXio with the newlib port — we use ONLY
 * fileXio for file access, never newlib fopen, so this is fine). */
extern int fileXioOpen(const char *name, int flags);
extern int fileXioRead(int fd, void *buf, int size);
extern int fileXioClose(int fd);
extern int fileXioLseek(int fd, int offset, int whence);
#define FIO_O_RDONLY  0x0001

/* cdfs: (the ISO9660 disc device) is upper-case and wants NO ';1'
 * version suffix; the engine asks for mixed-case names, so upper-case
 * the path after the device prefix. host:/mass: are left untouched —
 * HostFS mirrors the host filesystem (case-insensitive on macOS dev).
 * Shared with data_root.c's probe via an extern declaration there. */
void ps2_normalize_path(const char *in, char *out, size_t outsz)
{
    if (outsz == 0) return;
    if (strncmp(in, "cdfs:", 5) != 0) {
        snprintf(out, outsz, "%s", in);
        return;
    }
    size_t o = 0;
    for (size_t k = 0; k < 5 && o + 1 < outsz; ++k) out[o++] = in[k];
    for (size_t i = 5; in[i] && o + 1 < outsz; ++i) {
        char c = in[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        out[o++] = c;
    }
    out[o] = 0;
}

typedef struct CygFile { int fd; } CygFile;

CygFile *fopen_cyg(const char *name, const char *mode)
{
    (void)mode;                       /* the engine opens archives read-only */
    char fixed[320];
    ps2_normalize_path(name, fixed, sizeof fixed);
    int fd = fileXioOpen(fixed, FIO_O_RDONLY);
    if (fd < 0) return NULL;
    CygFile *f = (CygFile *)malloc(sizeof *f);
    if (!f) { fileXioClose(fd); return NULL; }
    f->fd = fd;
    return f;
}

void fclose_cyg(CygFile *f)
{
    if (!f) return;
    fileXioClose(f->fd);
    free(f);
}

uint32_t fread_cyg(void *dst, uint32_t sz, uint32_t n, CygFile *f)
{
    if (sz == 0) return 0;
    uint32_t total = sz * n, done = 0;
    char *p = (char *)dst;
    while (done < total) {                /* fileXioRead may short-read */
        int got = fileXioRead(f->fd, p + done, (int)(total - done));
        if (got <= 0) break;
        done += (uint32_t)got;
    }
    return done / sz;
}

void fseek_cyg(CygFile *f, int32_t off, int whence)
{
    fileXioLseek(f->fd, (int)off, whence);   /* SEEK_SET/CUR/END == 0/1/2 */
}

int32_t ftell_cyg(CygFile *f)
{
    return (int32_t)fileXioLseek(f->fd, 0, 1 /* SEEK_CUR */);
}

#else  /* ---- desktop / handheld: newlib stdio ---- */

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

#endif /* WACKI_PS2 */
