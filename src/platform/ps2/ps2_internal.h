/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform/ps2/ps2_internal.h — cross-file declarations *within* the PS2
 * platform backend (the src/platform/ps2/ TUs). NOT a public engine header — the
 * engine talks to PS2 only through the platform HAL interfaces
 * (storage/audio/video/system.h). This just lets the four PS2 translation
 * units reference the two functions that cross between them.
 */
#ifndef WACKI_PLATFORM_PS2_INTERNAL_H
#define WACKI_PLATFORM_PS2_INTERNAL_H

/* system_ps2.c — brings up the USB-mass FAT stack lazily; called by
 * storage_ps2.c's data-root probe after host:/cdfs: miss. */
int  platform_ps2_mount_usb(void);

/* audio_ps2.c — starts the audsrv feeder thread; called by video_ps2.c's
 * plat_video_init (the intro cutscene's audio must be up before any mixer
 * attach). */
void platform_ps2_audio_init(void);

#endif /* WACKI_PLATFORM_PS2_INTERNAL_H */
