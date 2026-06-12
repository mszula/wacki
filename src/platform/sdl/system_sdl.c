/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform/sdl/system_sdl.c — process-lifecycle HAL, desktop/handheld.
 *
 * Implements plat_system_early_init / plat_system_exit (wacki/platform/
 * system.h) for the SDL family. The only edge-of-process work here is on
 * Windows (redirect the GUI build's lost stderr to a log file) and macOS
 * (escape the read-only Finder cwd); Linux + handheld need nothing. The PS2
 * lifecycle (IOP bring-up + the EE park) lives in platform_ps2.c. */

#include "wacki.h"
#include "wacki/log.h"
#include "wacki/platform/system.h"
#ifdef __APPLE__
#include "wacki/platform/macos.h"   /* PlatformMacUseAppSupportDir */
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
}

void plat_system_exit(int rc)
{
    /* Desktop/handheld: nothing to do — main() returns rc to the OS. */
    (void)rc;
}

/* Desktop/handheld caches are coherent and there's no PINE trace buffer. */
void plat_dcache_flush(void *p, unsigned int n) { (void)p; (void)n; }
void plat_trace_mark(unsigned int code)         { (void)code; }
