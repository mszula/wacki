/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform/sdl/system_sdl.c — process-lifecycle HAL, desktop/handheld.
 *
 * Implements plat_system_early_init / plat_system_exit (wacki/platform/
 * system.h) for the SDL family. The only edge-of-process work here is on
 * Windows (redirect the GUI build's lost stderr to a log file), macOS
 * (escape the read-only Finder cwd), Android (escape the read-only "/"
 * cwd + trap the Back button), and Nintendo Switch (pin a fixed, known-
 * writable cwd — hbmenu/nx-hbloader give no guarantee here, unlike a
 * desktop shell or a handheld launcher script); Linux + the other
 * handhelds need nothing. The PS2 lifecycle (IOP bring-up + the EE park)
 * lives in src/platform/ps2/system_ps2.c. */

#include "wacki.h"
#include "wacki/log.h"
#include "wacki/platform/system.h"
#ifdef __APPLE__
#include "wacki/platform/macos.h"   /* PlatformMacUseAppSupportDir */
#endif
#ifdef __ANDROID__
#include <SDL.h>        /* SDL_AndroidGetInternalStoragePath, SDL_SetHint */
#include <unistd.h>     /* chdir */
#endif
#ifdef __SWITCH__
#include <unistd.h>     /* chdir */
#include <sys/stat.h>   /* mkdir */
#endif
#ifdef __wii__
#include <fat.h>        /* fatInitDefault */
#include <unistd.h>     /* chdir */
#include <sys/stat.h>   /* mkdir */
#endif

#ifdef _WIN32
#include <stdio.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
/* On a GUI-subsystem build (Makefile -mwindows) there's no console when
 * Explorer double-clicks wacki.exe — printf goes nowhere. Pipe stderr (and
 * stdout) into wacki.log so users can attach it to bug reports. Only when
 * GetConsoleWindow() == NULL: a console-attached dev/CI launch keeps its
 * live output. */
static void redirect_streams_to_logfile_if_no_console(void)
{
    if (GetConsoleWindow())
        return;
    FILE *f = freopen("wacki.log", "w", stderr);
    if (f) {
        setvbuf(stderr, NULL, _IOLBF, 0);   /* line-buffered → flush per LOG */
        freopen("wacki.log", "a", stdout);
    }
}
#endif

void plat_system_early_init(void)
{
#ifdef _WIN32
    redirect_streams_to_logfile_if_no_console();
#endif
#ifdef __APPLE__
    /* A Finder-launched .app runs with cwd="/" (read-only). Move to
     * ~/Library/Application Support/Wacki/ so Wacki.sav, wacki.cfg and
     * screenshots have a writable home. Must precede ConfigLoad and the
     * cwd-relative FindDataRoot probes. No-op for a bare binary. */
    if (PlatformMacUseAppSupportDir())
        LOG_INFO("platform", "user dir: ~/Library/Application Support/Wacki");
#endif
#ifdef __ANDROID__
    /* Android starts the process with cwd="/" (read-only). Move to the app's
     * private internal storage so Wacki.sav, wacki.cfg and screenshots have a
     * writable home (the read-only DTA archives live in external storage —
     * see data_root_android.c). Must precede ConfigLoad and the cwd-relative
     * FindDataRoot probes. The JNI refs SDL_AndroidGetInternalStoragePath
     * needs are wired up by SDLActivity before SDL_main runs, so this is safe
     * here (before SDL_Init). Trap the Back button so SDL delivers it as a key
     * (SDLK_AC_BACK → pause menu in platform_sdl.c) instead of finishing the
     * activity. */
    const char *home = SDL_AndroidGetInternalStoragePath();
    if (home && chdir(home) == 0)
        LOG_INFO("platform", "user dir: %s", home);
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    /* Lock to landscape. The manifest already forces it per-activity; this
     * pins SDL's own runtime orientation pick (it sets the activity's
     * requested orientation when the 640×480 window is created) to landscape
     * only, so a device held in portrait never flashes/rotates the game. */
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
#endif
#ifdef __SWITCH__
    /* hbmenu/nx-hbloader do NOT guarantee a consistent cwd across launches
     * the way a desktop shell or a handheld launcher script does (Miyoo/
     * PortMaster cd into the game's ROM folder before exec'ing the binary).
     * Pin a fixed, known-writable, known-readable absolute directory before
     * ANY relative fopen — must precede ConfigLoad and the cwd-relative
     * FindDataRoot probes (data_root_switch.c uses absolute sdmc:/ paths
     * regardless, but Wacki.sav / wacki.cfg are still opened with bare
     * relative names by save_host-style code, so the cwd has to be right).
     *
     * mkdir first in case this is genuinely the first launch and the folder
     * doesn't exist yet; mkdir failing because the directory already exists
     * is expected and harmless, so its return value isn't checked. chdir's
     * return IS checked: if it somehow fails (read-only SD card, directory
     * genuinely inaccessible), every subsequent relative fopen will simply
     * fail too and the game continues with defaults rather than crashing —
     * the same fallback a fresh install already has on every other target. */
    mkdir("sdmc:/switch/wacki", 0777);
    if (chdir("sdmc:/switch/wacki") == 0)
        LOG_INFO("platform", "user dir: sdmc:/switch/wacki");
    else
        LOG_INFO("platform", "chdir(sdmc:/switch/wacki) failed — Wacki.sav/wacki.cfg location not pinned");
#endif
#ifdef __wii__
    /* Homebrew Channel: init libfat so SD card fopen() works, then pin
     * the cwd to sd:/apps/wacki/ — same reasoning as __SWITCH__ above.
     * fatInitDefault() mounts the SD card as "sd:" (and optionally USB).
     * Must run before any fopen, including ConfigLoad. */
    if (!fatInitDefault())
        LOG_INFO("platform", "fatInitDefault() failed — SD card may be unavailable");
    mkdir("sd:/apps/wacki", 0777);
    if (chdir("sd:/apps/wacki") == 0)
        LOG_INFO("platform", "user dir: sd:/apps/wacki");
    else
        LOG_INFO("platform", "chdir(sd:/apps/wacki) failed — Wacki.sav/wacki.cfg location not pinned");
#endif
}

void plat_system_exit(int rc)
{
    /* Desktop/handheld: nothing to do — main() returns rc to the OS. */
    (void)rc;
}

/* Desktop/handheld caches are coherent and there's no PINE trace buffer. */
void plat_dcache_flush(void *p, unsigned int n) { (void)p; (void)n; }
void plat_trace_mark(unsigned int code)         { (void)code; }
