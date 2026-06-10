/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform_ps2.c — PlayStation 2 device glue + bring-up tracing.
 *
 * ps2sdk's stderr doesn't reach PCSX2's console, so during bring-up we
 * leave breadcrumbs in a known global array (g_ps2_trace) and read them
 * over PINE from the host. ps2_spin_forever() keeps the program "running"
 * (so PINE has a live game to talk to) instead of returning to the BIOS
 * browser, which is what made every earlier failure invisible.
 *
 * All of this is compiled only for TARGET=ps2 (WACKI_PS2). */

#include <stdint.h>

#ifdef WACKI_PS2

/* Breadcrumb ring read over PINE. g_ps2_trace_n is the running count;
 * the first 32 codes are retained. Addresses come from `nm` on the ELF. */
volatile uint32_t g_ps2_trace[32];
volatile uint32_t g_ps2_trace_n = 0;

void ps2_mark(uint32_t code)
{
    uint32_t i = g_ps2_trace_n;
    if (i < 32) g_ps2_trace[i] = code;
    g_ps2_trace_n = i + 1;
}

/* Never returns — parks the EE so the title stays alive for PINE memory
 * inspection during bring-up (a returned main() reboots PCSX2 to OSDSYS,
 * tearing down the game PINE needs). */
void ps2_spin_forever(void)
{
    for (;;) { }
}

#endif /* WACKI_PS2 */
