/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform/android/data_root_android.c — data-root discovery (storage
 * HAL), Android implementation.
 *
 * The portable search (env var, cwd, binary-adjacent) lives in data_root.c;
 * this supplies the two SDL-family hooks for Android, the counterpart to
 * data_root_desktop.c (external media + GUI picker) and data_root_handheld.c
 * (fixed SD-card paths).
 *
 * Asset delivery is handled outside the engine: the launcher (SetupActivity,
 * Java) lets the user pick the folder holding their original DANE_*.DTA via
 * the Storage Access Framework and copies them into the app's external files
 * dir — a real, fopen()-able filesystem path. So all this hook does is point
 * the search at that directory. SDL_AndroidGetExternalStoragePath() returns
 * exactly getExternalFilesDir(null) (e.g. /storage/emulated/0/Android/data/
 * <pkg>/files); the copy lands in its data/ subfolder, which try_root_and_data
 * already probes. The internal path is offered as a secondary candidate for
 * users who sideload the archives straight into app storage. */

#include "wacki.h"
#include "wacki/log.h"
#include "wacki/platform/storage.h"

#include <SDL.h>   /* SDL_AndroidGet{External,Internal}StoragePath */

int plat_data_roots(int (*probe)(const char *root))
{
    /* probe() == try_root_and_data: tests <root> and <root>/data. */
    const char *ext = SDL_AndroidGetExternalStoragePath();
    if (ext && probe(ext)) {
        LOG_INFO("data-root", "matched on external storage: %s", ext);
        return 1;
    }
    const char *intern = SDL_AndroidGetInternalStoragePath();
    if (intern && probe(intern)) {
        LOG_INFO("data-root", "matched on internal storage: %s", intern);
        return 1;
    }
    return 0;
}

/* No native in-engine folder picker on Android: the SAF picker + copy runs in
 * the Java launcher before SDL_main, so by the time the engine searches the
 * data is already present (or the launcher refused to start the game). */
int plat_prompt_data_folder(int (*probe)(const char *root))
{
    (void)probe;
    return 0;
}
