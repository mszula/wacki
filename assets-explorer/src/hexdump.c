/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * assets-explorer/src/hexdump.c — hex formatting + raw export (see hexdump.h). */

#include "hexdump.h"

#include <stdio.h>
#include <string.h>

int hexdump_line_count(uint32_t size)
{
    return (int)((size + (HEXDUMP_BYTES_PER_LINE - 1)) / HEXDUMP_BYTES_PER_LINE);
}

int hexdump_format_line(const uint8_t *data, uint32_t size, int line,
                        char *out, size_t outcap)
{
    static const char HEX[] = "0123456789ABCDEF";
    uint32_t base = (uint32_t)line * HEXDUMP_BYTES_PER_LINE;
    size_t   p    = 0;

    if (outcap == 0) return 0;
    if (!data || base >= size) { out[0] = '\0'; return 0; }

    /* 8-hex-digit offset, then two spaces. */
    char off[12];
    snprintf(off, sizeof off, "%08X  ", base);
    for (size_t i = 0; off[i] && p + 1 < outcap; ++i) out[p++] = off[i];

    /* 16 hex byte columns; pad missing bytes with spaces so the ASCII
     * gutter stays aligned on the final short line. An extra space after
     * the 8th column splits the row into two readable octets. */
    for (int i = 0; i < HEXDUMP_BYTES_PER_LINE; ++i) {
        uint32_t idx = base + (uint32_t)i;
        if (idx < size) {
            uint8_t b = data[idx];
            if (p + 1 < outcap) out[p++] = HEX[b >> 4];
            if (p + 1 < outcap) out[p++] = HEX[b & 0xF];
        } else {
            if (p + 1 < outcap) out[p++] = ' ';
            if (p + 1 < outcap) out[p++] = ' ';
        }
        if (p + 1 < outcap) out[p++] = ' ';
        if (i == 7 && p + 1 < outcap) out[p++] = ' ';
    }

    if (p + 1 < outcap) out[p++] = '|';
    for (int i = 0; i < HEXDUMP_BYTES_PER_LINE; ++i) {
        uint32_t idx = base + (uint32_t)i;
        if (idx >= size) break;
        uint8_t b = data[idx];
        char    c = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
        if (p + 1 < outcap) out[p++] = c;
    }
    if (p + 1 < outcap) out[p++] = '|';

    out[p] = '\0';
    return (int)p;
}

int hexdump_export_raw(const char *path, const void *buf, uint32_t sz)
{
    if (!path || !buf) return 0;
    FILE *fp = fopen(path, "wb");
    if (!fp) return 0;
    size_t wrote = fwrite(buf, 1, sz, fp);
    int    ok    = (fclose(fp) == 0) && (wrote == sz);
    return ok ? 1 : 0;
}
