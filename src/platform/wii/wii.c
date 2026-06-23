/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform/wii/wii.c — Nintendo Wii homebrew platform hooks.
 *
 * Wii reuses the shared SDL family for file I/O, audio, FLIC streaming,
 * video presentation, and system init. Input is split:
 *
 *   Wiimote IR → SDL_MOUSEMOTION (automatic in SDL2-wii, zero extra code)
 *   Wiimote A  → SDL_MOUSEBUTTONDOWN left  (automatic in SDL2-wii)
 *   Wiimote B  → SDL_MOUSEBUTTONDOWN right (automatic in SDL2-wii)
 *   +/-/1/2    → SDL_JOYBUTTONDOWN → gamepad_wii.c
 *
 * This file is the hooks provider (see docs/platform-hal.md):
 *   - plat_apply_video_prefs(): always fullscreen + dynamic WACKI.EXE load
 *   - plat_system_early_init(): handled in system_sdl.c's __wii__ block
 *   - everything else: no-ops */

#include "wacki.h"
#include "wacki/log.h"
#include "wacki/platform/system.h"
#include "wacki/platform/input.h"
#include "wacki/platform/video.h"

#include <SDL.h>
#include <stddef.h>

extern int  g_fullscreen;
extern char g_data_root[260];

/* ---- dynamic WACKI.EXE loading ---------------------------------------- */

static int load_wacki_exe_dynamic(void)
{
    extern int PeLoaderInit(const char *exe_path);
    char path[300];

    if (g_data_root[0]) {
        snprintf(path, sizeof path, "%s/WACKI.EXE", g_data_root);
        if (PeLoaderInit(path)) return 1;
        snprintf(path, sizeof path, "%s/wacki.exe", g_data_root);
        if (PeLoaderInit(path)) return 1;
    }

    /* Homebrew Channel convention: sd:/apps/wacki/ */
    static const char *const fallbacks[] = {
        "sd:/apps/wacki/data/WACKI.EXE",
        "sd:/apps/wacki/data/wacki.exe",
        "sd:/apps/wacki/WACKI.EXE",
        "sd:/apps/wacki/wacki.exe",
    };
    for (size_t i = 0; i < sizeof fallbacks / sizeof fallbacks[0]; ++i)
        if (PeLoaderInit(fallbacks[i])) return 1;

    return 0;
}

void plat_apply_video_prefs(void)
{
    g_fullscreen = 1;

    if (load_wacki_exe_dynamic()) {
        LOG_INFO("wacki", "WACKI.EXE loaded dynamically (PeLoaderInit)");
        return;
    }

    const char *msg =
        "Nie znalazlem pliku WACKI.EXE.\n\n"
        "Skopiuj WACKI.EXE z oryginalnej plyty do folderu\n"
        "sd:/apps/wacki/data/ na karcie SD.";
    LOG_INFO("wacki", "%s", msg);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                             "Wacki — brak WACKI.EXE", msg, NULL);
    exit(1);
}

void plat_restore_system_volume(void) {}

int plat_handle_platform_key(int sym)
{
    (void)sym;
    return 0;
}

void plat_pad_read_extra(float *ax, float *ay)
{
    (void)ax;
    (void)ay;
}

int plat_input_has_keyboard(void) { return 0; }
