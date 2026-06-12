/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform/sdl/hooks_desktop.c — desktop no-op platform hooks.
 *
 * The handhelds + PS2 each override a few small platform-variant behaviors
 * (firmware volume restore, hardware-button keysyms, fullscreen default, the
 * PS2 analog/USB-mouse pad extras). The plain desktop SDL build needs none of
 * them, so it links these no-ops. Exactly one hooks provider is linked per
 * target (see the Makefile), which is what keeps the sdl/ core files free of
 * any WACKI_MIYOO / WACKI_PORTMASTER / WACKI_PS2 #ifdef. */

#include "wacki/platform/system.h"   /* plat_restore_system_volume */
#include "wacki/platform/input.h"    /* plat_handle_platform_key, plat_pad_read_extra */
#include "wacki/platform/video.h"    /* plat_apply_video_prefs */

void plat_restore_system_volume(void)            { }
int  plat_handle_platform_key(int sym)           { (void)sym; return 0; }
void plat_apply_video_prefs(void)                { }
void plat_pad_read_extra(float *ax, float *ay)   { (void)ax; (void)ay; }
