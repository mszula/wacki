/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * tools/ps2-fileio-probe.c — interactive PS2 file-I/O test rig driven
 * over PINE. Boot it once in PCSX2; it initialises the EE<->IOP RPC and
 * then spins on a command block the host pokes via PINE memory writes:
 *
 *   1. host writes a NUL-terminated path into g_probe_path[]
 *   2. host writes g_probe_req = g_probe_resp + 1
 *   3. engine fopen()s the path, reads up to 16 bytes, and sets
 *      g_probe_result = 0 on failure, or 0x10000 | bytesRead on success
 *   4. engine sets g_probe_resp = g_probe_req
 *   5. host polls g_probe_resp, then reads g_probe_result
 *
 * This lets us probe host:/cdrom0:/cdfs:/mass: paths and init sequences
 * live, with zero rebuilds. Symbol addresses come from `nm` on the ELF.
 * Built standalone (no SDL) by tools/build-ps2-probe.sh.
 */

#include <tamtypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sifrpc.h>

volatile uint32_t g_probe_status = 0;     /* progress: 0xA1 entry, 0xA2 after SifInitRpc, 0xAF loop */
volatile uint32_t g_probe_req    = 0;     /* host increments to request an open */
volatile uint32_t g_probe_resp   = 0;     /* engine echoes req when done */
volatile uint32_t g_probe_result = 0;     /* 0 = fail; 0x10000 | bytesRead = ok */
volatile char     g_probe_path[256];      /* host writes the path here */

static uint32_t do_open(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    char b[16];
    size_t r = fread(b, 1, sizeof b, f);
    fclose(f);
    return 0x10000u | (uint32_t)(r & 0xffff);
}

int main(void)
{
    g_probe_status = 0xA1;
    SifInitRpc(0);              /* the init nobody (SDL/engine) was doing */
    g_probe_status = 0xA2;

    for (;;) {
        if (g_probe_req != g_probe_resp) {
            char local[256];
            memcpy(local, (const void *)g_probe_path, sizeof local);
            local[sizeof local - 1] = 0;
            g_probe_result = do_open(local);
            g_probe_resp   = g_probe_req;
        }
        g_probe_status = 0xAF;
    }
}
