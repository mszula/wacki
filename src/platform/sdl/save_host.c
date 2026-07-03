/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform/sdl/save_host.c — file-based save storage (storage HAL).
 *
 * Desktop + handheld implementation of plat_save_read/write: the save is a
 * file (WACKI_SAVE_FILE), written atomically via tmp + rename so a crash
 * mid-write can never leave a zero-byte save (a truncating fopen("wb")
 * would). The PS2 memory-card implementation lives in src/platform/ps2/storage_ps2.c. */

#include "wacki.h"
#include "wacki/log.h"
#include "wacki/platform/storage.h"

#include <stdio.h>
#include <string.h>

/* Windows rename() refuses to overwrite an existing destination, breaking
 * the tmp+rename pattern; MoveFileExA with MOVEFILE_REPLACE_EXISTING is the
 * correct POSIX-rename equivalent. */
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
static int atomic_replace(const char *from, const char *to)
{
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
}
#else
static int atomic_replace(const char *from, const char *to)
{
    return rename(from, to);
}
#endif

/* Flush a file's data to physical storage before we rename it into place.
 * tmp+rename gives atomicity against a crash; fsync adds durability
 * against POWER LOSS — on ext4/f2fs (handhelds) the rename can otherwise
 * commit while the file's bytes are still only in the page cache, so a
 * hard power cut leaves a zero/garbage save that the loader then resets. */
#ifdef _WIN32
#  include <io.h>
static void sync_file(FILE *fp) { _commit(_fileno(fp)); }
#else
#  include <unistd.h>
static void sync_file(FILE *fp) { int fd = fileno(fp); if (fd >= 0) fsync(fd); }
#endif

int plat_save_read(void *buf, int size)
{
    FILE *fp = fopen(WACKI_SAVE_FILE, "rb");
    if (!fp) return 0;
    size_t n = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    return (int)n;
}

int plat_save_write(const void *buf, int size)
{
    const char *tmp_path = WACKI_SAVE_FILE ".tmp";
    FILE *fp = fopen(tmp_path, "wb");
    if (!fp) return 0;

    size_t written = fwrite(buf, 1, (size_t)size, fp);
    if (written != (size_t)size) {
        fclose(fp);
        remove(tmp_path);
        LOG_INFO("save", "short write (%lu/%d) — keeping previous save",
                 (unsigned long)written, size);
        return 0;
    }
    fflush(fp);
    sync_file(fp);          /* durability before the rename (see sync_file) */
    fclose(fp);

    if (atomic_replace(tmp_path, WACKI_SAVE_FILE) != 0) {
        LOG_INFO("save", "rename(%s → %s) failed — keeping tmp",
                 tmp_path, WACKI_SAVE_FILE);
        return 0;
    }
    return 1;
}
