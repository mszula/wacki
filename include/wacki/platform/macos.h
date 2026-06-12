/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * include/wacki/platform/macos.h — macOS-only platform glue (src/platform/
 * macos/macos.m). These are called from the SDL platform layer under
 * #ifdef __APPLE__ and provided only on Darwin desktop builds; this header is
 * the single declaration point instead of scattered local externs.
 */
#ifndef WACKI_PLATFORM_MACOS_H
#define WACKI_PLATFORM_MACOS_H

/* A Finder-launched .app starts with cwd="/" (read-only). Move to
 * ~/Library/Application Support/Wacki/ so saves/config/screenshots have a
 * writable home. Returns 1 if it relocated, 0 for a bare terminal binary. */
int  PlatformMacUseAppSupportDir(void);

/* Polish-localise SDL's stock menu bar and add the "Gra" menu (wired to the
 * PlatformMenu* bridges in platform_sdl.c). */
void PlatformSetupMacMenu(void);

#endif /* WACKI_PLATFORM_MACOS_H */
