/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/config.c — wacki.cfg: persisted player display preferences.
 *
 * A tiny key=value file in the working directory (next to Wacki.sav)
 * remembering the display mode the player chose so the engine doesn't
 * re-ask every launch. Deliberately NOT stored in Wacki.sav — that
 * file's WackiSettings struct is byte-compatible with the original
 * 1998 game and we don't want to break that. wacki.cfg is purely a
 * port-side convenience.
 *
 * Keys:
 *   fullscreen=0|1   — start full-screen
 *   scale=N          — window zoom factor when not full-screen (1..8)
 *
 * Precedence is handled by call order in main.c: ConfigLoad runs
 * FIRST (baseline from saved prefs), then the CLI parser and env
 * overrides layer on top, so an explicit --scale / --fullscreen /
 * WACKI_* always wins over the stored preference.
 *
 * g_config_first_run is set when no wacki.cfg exists — PlatformInit
 * uses it to decide whether to show the one-time display-mode
 * picker. */

#include "wacki.h"
#include "wacki/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WACKI_CFG_PATH      "wacki.cfg"
#define CFG_SCALE_MAX       8


int g_config_first_run = 0;

void ConfigLoad(void)
{
    FILE *fp = fopen(WACKI_CFG_PATH, "r");
    if (!fp) {
        /* No config yet → first launch. PlatformInit shows the
         * display-mode picker (unless a CLI/env flag pre-empts it). */
        g_config_first_run = 1;
        return;
    }
    char line[128];
    while (fgets(line, sizeof line, fp)) {
        int v;
        if (sscanf(line, "fullscreen=%d", &v) == 1) {
            g_fullscreen = v ? 1 : 0;
        } else if (sscanf(line, "scale=%d", &v) == 1) {
            if (v >= 1 && v <= CFG_SCALE_MAX) g_scale_factor = v;
        }
    }
    fclose(fp);
    LOG_INFO("config", "loaded wacki.cfg: fullscreen=%d scale=%d",
             g_fullscreen, g_scale_factor);
}

void ConfigSave(void)
{
    FILE *fp = fopen(WACKI_CFG_PATH, "w");
    if (!fp) {
        LOG_INFO("config", "cannot write %s (display prefs not saved)",
                 WACKI_CFG_PATH);
        return;
    }
    fprintf(fp, "# Wacki display preferences (port-side, not the save file).\n");
    fprintf(fp, "# Skasuj ten plik, by ponownie wybrać tryb przy starcie.\n");
    fprintf(fp, "fullscreen=%d\n", g_fullscreen ? 1 : 0);
    fprintf(fp, "scale=%d\n", g_scale_factor > 0 ? g_scale_factor : 1);
    fclose(fp);
    LOG_INFO("config", "saved wacki.cfg: fullscreen=%d scale=%d",
             g_fullscreen, g_scale_factor ? g_scale_factor : 1);
}
