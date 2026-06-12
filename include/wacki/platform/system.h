/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * include/wacki/platform/system.h — process-lifecycle HAL.
 *
 * The few platform-specific things that have to happen at the very edges of
 * the process — before any file/config access, and after the game loop
 * returns — live behind these hooks so main.c stays platform-agnostic:
 *
 *   desktop / handheld  src/platform/sdl/system_sdl.c  (Win32 stderr→logfile,
 *                                                       macOS app-support cwd)
 *   PS2                 src/platform_ps2.c             (IOP fileio bring-up;
 *                                                       park the EE on exit)
 */
#ifndef WACKI_PLATFORM_SYSTEM_H
#define WACKI_PLATFORM_SYSTEM_H

/* Earliest process setup, before ConfigLoad / FindDataRoot / any file access.
 *   PS2     — bring up the IOP fileio stack (reset IOP, load iomanX/fileXio/
 *             cdfs, fileXioInit, sceCdInit); newlib fopen reaches no device
 *             until this runs.
 *   Windows — on a GUI-subsystem build with no console, pipe stderr/stdout to
 *             wacki.log so bug reports have output.
 *   macOS   — a Finder-launched .app starts with cwd="/" (read-only); move to
 *             ~/Library/Application Support/Wacki/ so saves/config/screenshots
 *             have a writable home.
 * A no-op where nothing is needed (bare Linux, handheld). */
void plat_system_early_init(void);

/* Final teardown after the game loop. On most platforms a no-op — the caller
 * returns `rc` to the OS. On PS2 it stamps the exit code into the trace and
 * parks the EE forever so PINE can still read the bring-up breadcrumbs (a
 * returned main() reboots PCSX2 to the BIOS browser); it never returns there. */
void plat_system_exit(int rc);

#endif /* WACKI_PLATFORM_SYSTEM_H */
