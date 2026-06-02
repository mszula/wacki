/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* src/audio/cutscene.c — AVI/FLIC cutscene playback shim.
 *
 * The original engine drove MCI AVIVideo to play Dane_10.dta (intro),
 * Dane_14.dta (death), the per-stage outro AVIs, etc. Those files are
 * Autodesk FLIC (AFLC fourCC) inside an AVI container; the actual
 * decode lives in src/flic.c (PlayFlicAviFile). This module is the
 * thin entry-point that resolves the asset name against the data-root
 * search list and invokes the decoder.
 *
 * Resolution order matches the engine's:
 *   1. CWD (named as-is)
 *   2. g_data_root (CLI / env CD-data location)
 *   3. ./data (default install layout)
 *
 * Each root is retried with the file basename uppercased so case-
 * sensitive filesystems (Linux + macOS case-sensitive APFS volumes)
 * find files written as "DANE_10.DTA" by the original installer.
 *
 * InitializeDirectSound is here as a no-op for the SDL build (the
 * original engine's DSound init). Real DSound + MCI music live in the
 * legacy Win32 build path (audio_win32.c).  */

#include "wacki.h"

#include <SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern int  PlayFlicAviFile(const char *path);   /* flic.c */
extern char g_data_root[260];

/* SDL build always succeeds; nothing actually plays. */
int InitializeDirectSound(void) { return 0; }

/* Try to open `name` relative to `root`; on macOS auto-uppercase the
 * basename on the second pass so case-sensitive filesystems find files
 * the original installer wrote in upper-case. */
static int try_play_at(const char *root, const char *name)
{
    char p[1024];
    if (root && *root) snprintf(p, sizeof p, "%s/%s", root, name);
    else               snprintf(p, sizeof p, "%s", name);
    if (PlayFlicAviFile(p)) return 1;

    /* Uppercase the last path component. */
    size_t l = strlen(p);
    size_t i = l;
    while (i > 0 && p[i - 1] != '/' && p[i - 1] != '\\') --i;
    for (size_t j = i; j < l; ++j) {
        if (p[j] >= 'a' && p[j] <= 'z') p[j] &= 0xDF;
    }
    return PlayFlicAviFile(p);
}

void PlaySceneCutsceneAvi(const char *avi_name)
{
    if (!avi_name) return;
    if (try_play_at(NULL,      avi_name)) return;
    if (try_play_at(g_data_root, avi_name)) return;
    if (try_play_at("./data",  avi_name)) return;
}
