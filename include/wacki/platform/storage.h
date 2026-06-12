/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * include/wacki/platform/storage.h — platform storage HAL.
 *
 * Where bytes live and how they're read/written is platform-specific; the
 * engine core calls these and never #ifdefs on a platform. Each platform
 * provides exactly one implementation (linked by the build per TARGET — see
 * docs/platform-hal.md). This header grows as the storage subsystems migrate
 * behind it; for now it covers the save image.
 *
 * Implementations:
 *   desktop / handheld  src/platform/sdl/save_host.c  (a file + atomic rename)
 *   PS2                 src/platform_ps2.c            (the memory card, libmc)
 */
#ifndef WACKI_PLATFORM_STORAGE_H
#define WACKI_PLATFORM_STORAGE_H

/* ---- save image -------------------------------------------------- *
 *
 * The whole save (`g_save`) is read once at boot and written on demand. */

/* Read up to `size` bytes of the save image into `buf`. Returns the number
 * of bytes read — 0 if no save exists yet or the read failed. */
int plat_save_read(void *buf, int size);

/* Persist `size` bytes from `buf` as the save image, as atomically as the
 * platform allows (the engine relies on never observing a half-written
 * save). Returns 1 on success, 0 on failure. */
int plat_save_write(const void *buf, int size);

#endif /* WACKI_PLATFORM_STORAGE_H */
