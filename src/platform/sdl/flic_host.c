/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform/sdl/flic_host.c — FLIC/AVI cutscene streaming reader (storage
 * HAL), stdio backend.
 *
 * Desktop + handheld implementation of plat_flic_* (wacki/platform/storage.h):
 * a single global FILE with a large fully-buffered read-ahead so the
 * decoder's many small per-chunk reads coalesce into a few big sequential
 * block reads — best case for SD/eMMC. The PS2 backend (a background ring-fill
 * thread, since fileXio RPCs are costly and would starve audsrv) lives in
 * src/platform/ps2/storage_ps2.c. */

#include "wacki/platform/storage.h"
#ifdef __ANDROID__
#include "wacki/platform/android_saf.h"   /* read-in-place from the SAF tree */
#endif

#include <stdio.h>   /* + plat_resolve_path_ci (storage.h) for the case fallback */

/* The playback loop issues one fread per AVI sub-chunk (KB-sized); a 1 MiB
 * fully-buffered FILE coalesces them into ~1 MiB sequential block reads. */
#define FLIC_IO_BUFFER_BYTES   (1u << 20)

static FILE *s_fp = NULL;

int plat_flic_open(const char *path)
{
    if (s_fp) { fclose(s_fp); s_fp = NULL; }
#ifdef __ANDROID__
    /* Cutscene paths are built from the data root; when that's the SAF tree,
     * open the archive in place via a content:// fd (seekable → setvbuf +
     * fseek work as on a real file). */
    if (android_saf_active())
        s_fp = android_saf_fopen(path);
    if (!s_fp)
#endif
    s_fp = fopen(path, "rb");
    /* Case-sensitive FS fallback: the cutscene files (Dane_NN.dta) are
     * lower-cased on the original Linux CD, so the literal open misses.
     * Resolve a case-insensitive directory match and retry. */
    if (!s_fp) {
        char real[1024];
        if (plat_resolve_path_ci(path, real, sizeof real))
            s_fp = fopen(real, "rb");
    }
    if (!s_fp) return 0;
    setvbuf(s_fp, NULL, _IOFBF, FLIC_IO_BUFFER_BYTES);
    return 1;
}

uint32_t plat_flic_read(void *dst, uint32_t n)
{
    if (!s_fp) return 0;
    return (uint32_t)fread(dst, 1, n, s_fp);
}

void plat_flic_seek(int32_t off, int whence)
{
    if (s_fp) fseek(s_fp, (long)off, whence);
}

int32_t plat_flic_tell(void)
{
    return s_fp ? (int32_t)ftell(s_fp) : 0;
}

void plat_flic_close(void)
{
    if (s_fp) { fclose(s_fp); s_fp = NULL; }
}
