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

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct CygFile { FILE *fp; };         /* completes storage.h's opaque CygFile */

/* ASCII case-insensitive string equality. Locale-independent on purpose —
 * the only callers compare DTA/asset filenames, which are pure ASCII, and a
 * locale-aware fold could mis-handle a non-C locale's case rules. */
static int ascii_ieq(const char *a, const char *b)
{
    for (; *a && *b; ++a, ++b) {
        unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + 32);
        if (ca != cb) return 0;
    }
    return *a == *b;
}

int plat_resolve_path_ci(const char *in, char *out, size_t outcap)
{
    if (!in || !out || outcap == 0) return 0;

    /* Split into directory + basename; only the basename's case varies in
     * practice (the user creates ./data themselves), so we scan one dir. */
    const char *slash = strrchr(in, '/');
    const char *base  = slash ? slash + 1 : in;
    if (!*base) return 0;                       /* trailing slash — no file */

    char dir[1024];
    if (!slash)              { dir[0] = '.'; dir[1] = 0; }   /* cwd */
    else if (slash == in)    { dir[0] = '/'; dir[1] = 0; }   /* filesystem root */
    else {
        size_t dlen = (size_t)(slash - in);
        if (dlen >= sizeof dir) return 0;
        memcpy(dir, in, dlen);
        dir[dlen] = 0;
    }

    DIR *d = opendir(dir);
    if (!d) return 0;

    int found = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!ascii_ieq(e->d_name, base)) continue;
        /* Preserve the caller's exact directory prefix (and its slash) and
         * swap in the real on-disk basename — avoids reformatting "/" or
         * "./data" and the double-slash that "%s/%s" would produce at root. */
        size_t prefix = (size_t)(base - in);
        size_t nlen   = strlen(e->d_name);
        if (prefix + nlen < outcap) {
            memcpy(out, in, prefix);
            memcpy(out + prefix, e->d_name, nlen + 1);
            found = 1;
        }
        break;
    }
    closedir(d);
    return found;
}

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
    /* Case-sensitive FS (Linux) + a CD whose files are lower-cased: the
     * literal open missed, so retry against a case-insensitive directory
     * match. Read modes only — a write must create the exact name asked
     * for, not silently target a differently-cased sibling. */
    if (!fp && mode && mode[0] == 'r') {
        char real[1024];
        if (plat_resolve_path_ci(name, real, sizeof real))
            fp = fopen(real, mode);
    }
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
