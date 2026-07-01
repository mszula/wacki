/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform/sdl/system_sdl.c — process-lifecycle HAL, desktop/handheld.
 *
 * Adds a #ifdef __SWITCH__ block (mirrors the existing __ANDROID__ block):
 * mkdir + chdir to sdmc:/switch/wacki/ so Wacki.sav and wacki.cfg always
 * land in the same place regardless of what hbmenu sets as cwd. */

#include "wacki.h"
#include "wacki/log.h"
#include "wacki/platform/system.h"
#ifdef __APPLE__
#include "wacki/platform/macos.h"
#endif
#ifdef __ANDROID__
#include <SDL.h>
#include <unistd.h>
#endif
#ifdef __SWITCH__
#include <unistd.h>
#include <sys/stat.h>
#endif

#ifdef _WIN32
#include <stdio.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static void redirect_streams_to_logfile_if_no_console(void)
{
    if (GetConsoleWindow()) return;
    FILE *f = freopen("wacki.log", "w", stderr);
    if (f) { setvbuf(stderr, NULL, _IOLBF, 0); freopen("wacki.log", "a", stdout); }
}
#endif

void plat_system_early_init(void)
{
#ifdef _WIN32
    redirect_streams_to_logfile_if_no_console();
#endif
#ifdef __APPLE__
    if (PlatformMacUseAppSupportDir())
        LOG_INFO("platform", "user dir: ~/Library/Application Support/Wacki");
#endif
#ifdef __ANDROID__
    const char *home = SDL_AndroidGetInternalStoragePath();
    if (home && chdir(home) == 0)
        LOG_INFO("platform", "user dir: %s", home);
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
#endif
#ifdef __SWITCH__
    /* hbmenu/nx-hbloader does not guarantee a consistent cwd across launches.
     * Pin to a fixed, writable directory before ConfigLoad and FindDataRoot.
     * Must run before any relative fopen (Wacki.sav, wacki.cfg). */
    mkdir("sdmc:/switch/wacki", 0777);
    if (chdir("sdmc:/switch/wacki") == 0)
        LOG_INFO("platform", "user dir: sdmc:/switch/wacki");
    else
        LOG_INFO("platform", "chdir(sdmc:/switch/wacki) failed");
#endif
}

void plat_system_exit(int rc)   { (void)rc; }
void plat_dcache_flush(void *p, unsigned int n) { (void)p; (void)n; }
void plat_trace_mark(unsigned int code)         { (void)code; }
