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
 * Two delivery paths, tried in this order:
 *
 *   1. Read-in-place via SAF. The launcher (SetupActivity) lets the user pick
 *      the folder holding their DANE_*.DTA and persists the grant; the engine
 *      then reads the archives straight from there through a content:// fd
 *      (android_saf.h / saf.c) — no copy. We turn that mode on and commit a
 *      "saf:" sentinel root; fopen_cyg("saf:/NAME") routes to the bridge by
 *      basename.
 *   2. A real path: the app's external (or internal) files dir, for users who
 *      placed the archives there directly (adb push / file manager).
 *      SDL_AndroidGetExternalStoragePath() == getExternalFilesDir(null).
 */

#include "wacki.h"
#include "wacki/log.h"
#include "wacki/platform/storage.h"
#include "wacki/platform/android_saf.h"

#include <SDL.h>   /* SDL_AndroidGet{External,Internal}StoragePath */

/* The sentinel "root" for SAF mode. fopen_cyg/plat_flic_open ignore the path
 * and key on the basename once android_saf_active(); try_root_and_data only
 * needs probe("saf:") to succeed (it opens "saf:/Dane_02.dta" through the
 * bridge). */
#define SAF_ROOT_SENTINEL  "saf:"

int plat_data_roots(int (*probe)(const char *root))
{
    /* 1. Read-in-place from the SAF folder the user picked. */
    if (android_saf_has_data()) {
        android_saf_set_active(1);
        if (probe(SAF_ROOT_SENTINEL)) {
            LOG_INFO("data-root", "reading in place via SAF (no copy)");
            return 1;
        }
        /* Configured but the probe couldn't open — grant lost mid-flight;
         * don't leave SAF routing on for the real-path fallback below. */
        android_saf_set_active(0);
    }

    /* 2. Archives placed directly in the app's storage (probe() == try_root_
     * and_data tests <root> and <root>/data). */
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

/* No native in-engine folder picker on Android: the SAF picker runs in the
 * Java launcher before SDL_main, so by the time the engine searches the data
 * is already reachable (or the launcher refused to start the game). */
int plat_prompt_data_folder(int (*probe)(const char *root))
{
    (void)probe;
    return 0;
}
