/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform/sdl/data_root_handheld.c — data-root discovery (storage HAL),
 * handheld implementation (Miyoo / PortMaster & friends).
 *
 * The portable search (env var, cwd, binary-adjacent) lives in data_root.c;
 * this supplies the two SDL-family hooks for handhelds. A handheld has fixed
 * SD-card data locations and no native folder picker, so this is the small
 * counterpart to data_root_desktop.c (external-media scanners + GUI picker) —
 * the Makefile links exactly one of the two per target, which is what keeps
 * either file free of a WACKI_HANDHELD #ifdef. */

#include "wacki.h"
#include "wacki/log.h"
#include "wacki/platform/storage.h"

/* The normal case is already covered by data_root.c's cwd + SDL_GetBasePath
 * probes (OnionOS' launch_standalone and PortMaster both cd into the game dir
 * before exec), so this is a belt-and-suspenders fallback: the Miyoo SD root
 * and the common PortMaster ports folders (/roms/ports, /roms2/ports) used
 * across Anbernic firmwares (muOS, ROCKNIX, ArkOS, Knulli, Batocera...). */
int plat_data_roots(int (*probe)(const char *root))
{
    static const char *const paths[] = {
        /* Miyoo / OnionOS */
        "/mnt/SDCARD",
        "/mnt/SDCARD/data",
        "/mnt/SDCARD/wacki",
        "/mnt/SDCARD/wacki/data",
        "/mnt/SDCARD/Roms/PORTS/Games/Wacki",
        "/mnt/SDCARD/Roms/PORTS/Games/Wacki/data",
        /* PortMaster (Anbernic & friends) */
        "/roms/ports/Wacki",
        "/roms/ports/Wacki/data",
        "/roms2/ports/Wacki",
        "/roms2/ports/Wacki/data",
        NULL
    };
    for (int i = 0; paths[i]; ++i) {
        if (probe(paths[i])) {
            LOG_INFO("data-root", "matched on %s", paths[i]);
            return 1;
        }
    }
    return 0;
}

/* No keyboard / native folder picker on a handheld, and the launcher would
 * have refused to start without the data present. */
int plat_prompt_data_folder(int (*probe)(const char *root))
{
    (void)probe;
    return 0;
}
