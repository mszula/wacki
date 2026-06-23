/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform/wii/embedded_wacki_pe_stub.c — empty embedded-PE table.
 * Identical in purpose to src/platform/switch/embedded_wacki_pe_stub.c —
 * see that file's header comment. */

#include "wacki/embedded_exe.h"

const PeSlice      g_wacki_pe_slices[]    = { {0, 0, 0, 0} };
const int          g_wacki_pe_slice_count = 0;
unsigned char      g_wacki_pe_blob[]      = { 0 };
const unsigned int g_wacki_pe_blob_len    = 0;
const uint32_t     g_wacki_pe_image_base  = 0;
