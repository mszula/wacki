/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/data_root.c — FindDataRoot: locate the directory holding
 * Dane_*.dta.
 *
 * This file is the platform-agnostic half of the search; everything that
 * varies by platform (external media, the handheld SD-card layout, the PS2
 * storage devices, the native folder picker) lives behind the storage HAL —
 * plat_data_roots() / plat_prompt_data_folder() (see
 * include/wacki/platform/storage.h). File-existence probing goes through the
 * cygio shim (fopen_cyg), which is itself per-platform (stdio vs fileXio).
 *
 * Search order:
 *
 *   1. WACKI_PATH env var
 *   2. ./, ./data, data
 *   3. Adjacent to the binary (SDL_GetBasePath() and that/data)
 *   4. Platform candidate roots — plat_data_roots():
 *        desktop:   /Volumes (macOS) · A:..Z: (Windows) · /media,/mnt
 *                   mounts (Linux) · the macOS .app-neighbor folder
 *        handheld:  /mnt/SDCARD · PortMaster /roms/ports
 *        PS2:       host:/cdfs:/mass: + a lazy USB-mass mount
 *   5. GUI fallback — plat_prompt_data_folder(): native folder picker
 *        (desktop only). User selects a folder; we re-probe it.
 *
 * First hit wins. Probe file is hard-coded to Dane_02.dta — the only
 * archive every original Wacki CD shipped that the engine references at
 * startup. Case-insensitive: lowercase tried first, then the same basename
 * uppercased (for CDs written with DOS-conventional DANE_02.DTA).
 *
 * On success, g_data_root is committed to the absolute or relative root
 * that matched. */

#include "wacki.h"
#include "wacki/log.h"
#include "wacki/platform/storage.h"

#include <SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* DATA_ROOT_FOUND must match the constant in src/main.c. */
#define DATA_ROOT_FOUND                     2

#define ARCHIVE_PROBE_PATH_BYTES            1024
#define UPPERCASE_NAME_BYTES                64
#define DATA_PROBE_FILENAME                 "Dane_02.dta"

extern char g_data_root[260];

/* ---- probe helpers ----------------------------------------------- */

/* Whether `path` names an openable file. Routes through the cygio shim so
 * the probe uses the same backend (stdio / fileXio) as the real archive
 * reads — on PS2 they must agree, or the probe and the loader would disagree
 * about which device a path lives on. */
static int try_open_path(const char *path)
{
    CygFile *f = fopen_cyg(path, "rb");
    if (!f) return 0;
    fclose_cyg(f);
    return 1;
}

static int directory_has_archive(const char *root, const char *needle)
{
    char buf[ARCHIVE_PROBE_PATH_BYTES];

    snprintf(buf, sizeof buf, "%s/%s", root, needle);
    if (try_open_path(buf)) return 1;

    char   upper[UPPERCASE_NAME_BYTES];
    size_t i;
    for (i = 0; needle[i] && i < sizeof upper - 1; ++i) {
        char c = needle[i];
        upper[i] = (char)((c >= 'a' && c <= 'z') ? c - 32 : c);
    }
    upper[i] = 0;
    snprintf(buf, sizeof buf, "%s/%s", root, upper);
    return try_open_path(buf);
}

/* Accept `root` as the data directory if it contains the probe file.
 * Commits g_data_root and returns 1; otherwise leaves g_data_root
 * untouched and returns 0. */
static int try_root(const char *root)
{
    if (!root || !*root) return 0;
    if (!directory_has_archive(root, DATA_PROBE_FILENAME)) return 0;
    snprintf(g_data_root, sizeof g_data_root, "%s", root);
    return 1;
}

/* Try `<root>` and then `<root>/data`. Common pattern that lets a caller
 * probe both "bare files" and the "files in data/ subfolder" conventions in
 * one call. This is the predicate the platform HAL drives: given a candidate
 * directory, decide whether it (or its data/ child) is the data root. */
static int try_root_and_data(const char *root)
{
    if (!root || !*root) return 0;
    if (try_root(root)) return 1;
    char buf[ARCHIVE_PROBE_PATH_BYTES];
    snprintf(buf, sizeof buf, "%s/data", root);
    return try_root(buf);
}

/* ---- public entry ------------------------------------------------ */

int FindDataRoot(void)
{
    /* 1. Explicit env override. */
    if (try_root(getenv("WACKI_PATH"))) return DATA_ROOT_FOUND;

    /* 2. cwd + ./data. */
    if (try_root("."))      return DATA_ROOT_FOUND;
    if (try_root("./data")) return DATA_ROOT_FOUND;
    if (try_root("data"))   return DATA_ROOT_FOUND;

    /* 3. Adjacent to the binary. SDL_GetBasePath gives the directory
     * containing the executable (or, in an .app bundle, the Resources/
     * folder). Portable across every SDL target. */
    char *base = SDL_GetBasePath();
    if (base) {
        size_t blen = strlen(base);
        while (blen > 1 &&
               (base[blen - 1] == '/' || base[blen - 1] == '\\'))
        {
            base[--blen] = 0;
        }
        int hit = try_root_and_data(base);
        SDL_free(base);
        if (hit) return DATA_ROOT_FOUND;
    }

    /* 4. Platform-specific candidate roots (external media / SD card / PS2
     * devices). 5. Native folder picker (desktop). Both drive try_root_and
     * _data as the commit predicate. */
    if (plat_data_roots(try_root_and_data))        return DATA_ROOT_FOUND;
    if (plat_prompt_data_folder(try_root_and_data)) return DATA_ROOT_FOUND;

    return 0;
}
