/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * tools/ps2-fileio-probe.c — interactive PS2 file-I/O test rig (PINE).
 *
 * Full IOP bring-up for disc reads: reset the IOP to a clean state (so
 * the real iomanX/fileXio don't fight PCSX2's HLE default modules — that
 * conflict hung fileXioInit), sbv-patch to allow EE-buffer module loads,
 * load iomanX + fileXio + cdfs, init fileXio + CDVD. Then serve open
 * requests over PINE, testing fopen() AND fileXioOpen() per path.
 *
 * g_probe_status walks A1->A6->AF so a hang pinpoints the failing step.
 * fileXio decls are hand-extern'd to dodge the guarded fileXio_rpc.h.
 */

#include <tamtypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <iopcontrol.h>
#include <sbv_patches.h>
#include <libcdvd.h>

#include "iomanX_irx.c"
#include "fileXio_irx.c"
#include "cdfs_irx.c"

extern int fileXioInit(void);
extern int fileXioOpen(const char *name, int flags);
extern int fileXioRead(int fd, void *buf, int size);
extern int fileXioClose(int fd);
#define FIO_O_RDONLY 0x0001

volatile uint32_t g_probe_status  = 0;   /* A1 rpc, A2 reset, A3 sbv, A4 mods, A5 fxinit, A6 cd, AF loop */
volatile uint32_t g_probe_mods    = 0;
volatile uint32_t g_probe_cd      = 0;
volatile uint32_t g_probe_fxinit  = 0;
volatile uint32_t g_probe_req     = 0;
volatile uint32_t g_probe_resp    = 0;
volatile uint32_t g_probe_result  = 0;   /* fopen()      */
volatile uint32_t g_probe_result2 = 0;   /* fileXioOpen() */
volatile char     g_probe_path[256];

static uint32_t open_stdio(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    char b[16]; size_t r = fread(b, 1, sizeof b, f); fclose(f);
    return 0x10000u | (uint32_t)(r & 0xffff);
}
static uint32_t open_fxio(const char *p)
{
    int fd = fileXioOpen(p, FIO_O_RDONLY);
    if (fd < 0) return 0;
    char b[16]; int r = fileXioRead(fd, b, sizeof b); fileXioClose(fd);
    if (r < 0) r = 0;
    return 0x10000u | (uint32_t)(r & 0xffff);
}

int main(void)
{
    SifInitRpc(0);
    g_probe_status = 0xA1;

    /* Clean IOP so loaded modules don't fight PCSX2's HLE defaults. */
    while (!SifIopReset("", 0)) {}
    while (!SifIopSync()) {}
    SifInitRpc(0);
    SifLoadFileInit();
    g_probe_status = 0xA2;

    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();
    g_probe_status = 0xA3;

    int r1 = -99, r2 = -99, r3 = -99;
    int e1 = SifExecModuleBuffer(iomanX_irx,  size_iomanX_irx,  0, NULL, &r1);
    int e2 = SifExecModuleBuffer(fileXio_irx, size_fileXio_irx, 0, NULL, &r2);
    g_probe_mods = ((uint32_t)(e1 & 0xff) << 24) | ((uint32_t)(r1 & 0xff) << 16) |
                   ((uint32_t)(e2 & 0xff) << 8)  |  (uint32_t)(r2 & 0xff);
    g_probe_status = 0xA4;

    int fx = fileXioInit();
    g_probe_fxinit = 0x80000000u | (uint32_t)(fx & 0xffff);
    g_probe_status = 0xA5;

    int ci = sceCdInit(SCECdINIT);
    int e3 = SifExecModuleBuffer(cdfs_irx, size_cdfs_irx, 0, NULL, &r3);
    g_probe_cd = ((uint32_t)(e3 & 0xff) << 24) | ((uint32_t)(r3 & 0xff) << 16) |
                  (uint32_t)(ci & 0xffff);
    g_probe_status = 0xA6;

    for (;;) {
        if (g_probe_req != g_probe_resp) {
            char local[256];
            memcpy(local, (const void *)g_probe_path, sizeof local);
            local[sizeof local - 1] = 0;
            g_probe_result  = open_stdio(local);
            g_probe_result2 = open_fxio(local);
            g_probe_resp    = g_probe_req;
        }
        g_probe_status = 0xAF;
    }
}
