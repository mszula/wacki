/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform/switch/embedded_wacki_pe_stub.c — empty PE slice table.
 * Linked when data/WACKI.EXE is absent at build time. PeLoaderRead checks
 * a dynamically-loaded image first, so plat_apply_video_prefs()'s SD-card
 * PeLoaderInit() call (switch.c) makes the engine work unmodified. */

#include "wacki/embedded_exe.h"

const PeSlice      g_wacki_pe_slices[]    = { {0, 0, 0, 0} };
const int          g_wacki_pe_slice_count = 0;
unsigned char      g_wacki_pe_blob[]      = { 0 };
const unsigned int g_wacki_pe_blob_len    = 0;
const uint32_t     g_wacki_pe_image_base  = 0;
