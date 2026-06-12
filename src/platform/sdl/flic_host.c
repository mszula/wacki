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
 * platform_ps2.c. */

#include "wacki/platform/storage.h"

#include <stdio.h>

/* The playback loop issues one fread per AVI sub-chunk (KB-sized); a 1 MiB
 * fully-buffered FILE coalesces them into ~1 MiB sequential block reads. */
#define FLIC_IO_BUFFER_BYTES   (1u << 20)

static FILE *s_fp = NULL;

int plat_flic_open(const char *path)
{
    if (s_fp) { fclose(s_fp); s_fp = NULL; }
    s_fp = fopen(path, "rb");
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
