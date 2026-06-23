/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform/wii/storage_wii.c — save-image storage HAL, Wii.
 *
 * Same tmp+copy atomic-write contract as storage_switch.c, with the
 * libfat equivalent of fsdevCommitDevice: fatSync("sd"). Without it,
 * libfat buffers writes in memory and may not flush them to the SD card
 * before power-off, silently losing the save — same root cause as the
 * Switch issue fixed by fsdevCommitDevice. */

#include "wacki.h"
#include "wacki/log.h"
#include "wacki/platform/storage.h"

#include <fat.h>
#include <stdio.h>

static int atomic_replace(const char *from, const char *to)
{
    FILE *src = fopen(from, "rb");
    if (!src) return -1;
    FILE *dst = fopen(to, "wb");
    if (!dst) { fclose(src); return -1; }

    char buf[4096];
    size_t n;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof buf, src)) > 0) {
        if (fwrite(buf, 1, n, dst) != n) { ok = 0; break; }
    }
    fclose(src);
    fflush(dst);
    fclose(dst);
    if (!ok) return -1;
    remove(from);

    /* libfat: flush pending writes to the physical SD card. Without this,
     * data may be lost on power-off — same issue as Switch's
     * fsdevCommitDevice. "sd" is the default libfat mount name. */
    if (!fatSync("sd"))
        LOG_INFO("save", "fatSync(sd) failed — save may not persist after power-off");
    return 0;
}

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
    fclose(fp);

    if (atomic_replace(tmp_path, WACKI_SAVE_FILE) != 0) {
        LOG_INFO("save", "replace(%s -> %s) failed", tmp_path, WACKI_SAVE_FILE);
        return 0;
    }
    return 1;
}
