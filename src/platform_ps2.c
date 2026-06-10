/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform_ps2.c — PlayStation 2 device glue.
 *
 * PS2 file I/O bring-up. ps2sdk's newlib fopen() reaches no device on its
 * own, so the engine's file access goes through fileXio instead (see the
 * cygio.c shim). For that to work the IOP fileio stack must be brought up
 * first — confirmed on-target (PCSX2 via PINE):
 *
 *   SifInitRpc → reset the IOP (so the real iomanX/fileXio don't fight
 *   PCSX2's HLE default modules — that conflict hangs fileXioInit) → sbv
 *   patches (allow loading modules from EE RAM) → load iomanX + fileXio +
 *   cdfs (bin2c-embedded) → fileXioInit → sceCdInit.
 *
 * After this, fileXioOpen("host:...") (dev, bare ELF + ./data) and
 * fileXioOpen("cdfs:/DATA/...") (the ISO) both open. Audio is left off
 * (audsrv wedges the IOP); SDL_Init for video happens later and brings up
 * its own modules.
 *
 * Built only for TARGET=ps2; the build (tools/build-ps2.sh) generates the
 * iomanX_irx.c / fileXio_irx.c / cdfs_irx.c includes with bin2c.
 */

#include <stdint.h>

#ifdef WACKI_PS2

#include <sifrpc.h>
#include <loadfile.h>
#include <iopcontrol.h>
#include <sbv_patches.h>
#include <libcdvd.h>

#include "iomanX_irx.c"
#include "fileXio_irx.c"
#include "cdfs_irx.c"

/* Hand-declared to dodge ps2sdk's guarded fileXio_rpc.h ("don't mix
 * fio/fileXio with the newlib port"): the engine uses ONLY fileXio for
 * file access (cygio.c), never newlib fopen, so this is safe. */
extern int fileXioInit(void);

/* ---- bring-up trace (read over PINE) ----------------------------- */

volatile uint32_t g_ps2_trace[32];
volatile uint32_t g_ps2_trace_n = 0;

/* Frame profiling (read over PINE). exp = 8bpp->ARGB expansion + texture
 * update; draw = RenderClear/Copy/Present (GS + vsync); frame = full
 * present-to-present. "engine software blit" ≈ frame - exp - draw. */
volatile uint32_t g_ps2_exp_ms   = 0;
volatile uint32_t g_ps2_draw_ms  = 0;
volatile uint32_t g_ps2_frame_ms = 0;
volatile uint32_t g_ps2_present_n = 0;

void ps2_mark(uint32_t code)
{
    uint32_t i = g_ps2_trace_n;
    if (i < 32) g_ps2_trace[i] = code;
    g_ps2_trace_n = i + 1;
}

void ps2_spin_forever(void)
{
    for (;;) { }
}

/* ---- file I/O bring-up ------------------------------------------- */

/* Called first thing in WackiMain, before any file access. */
void platform_ps2_io_init(void)
{
    SifInitRpc(0);

    /* Clean IOP — real iomanX/fileXio mustn't fight PCSX2's HLE defaults. */
    while (!SifIopReset("", 0)) { }
    while (!SifIopSync())       { }
    SifInitRpc(0);
    SifLoadFileInit();

    /* Allow loading IRX modules from EE RAM buffers on the reset IOP. */
    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();

    int ret;
    SifExecModuleBuffer(iomanX_irx,  size_iomanX_irx,  0, NULL, &ret);
    SifExecModuleBuffer(fileXio_irx, size_fileXio_irx, 0, NULL, &ret);
    fileXioInit();

    sceCdInit(SCECdINIT);
    SifExecModuleBuffer(cdfs_irx, size_cdfs_irx, 0, NULL, &ret);
}

#endif /* WACKI_PS2 */
