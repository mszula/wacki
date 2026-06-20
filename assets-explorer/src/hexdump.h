/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * assets-explorer/src/hexdump.h — hex formatting + raw export for the asset explorer.
 * Pure helpers, no UI/SDL dependency. */

#ifndef WACKI_VIEWER_HEXDUMP_H
#define WACKI_VIEWER_HEXDUMP_H

#include <stddef.h>
#include <stdint.h>

#define HEXDUMP_BYTES_PER_LINE 16

/* Number of hex lines needed to show `size` bytes. */
int  hexdump_line_count(uint32_t size);

/* Format one classic hex line — "OFFSET  HH HH … HH  |ascii|" — for the
 * 16-byte row starting at line*16. Writes a NUL-terminated string into out
 * (truncated to outcap). Returns the string length written. */
int  hexdump_format_line(const uint8_t *data, uint32_t size, int line,
                         char *out, size_t outcap);

/* Write `sz` raw bytes to `path`. Returns 1 on success, 0 on any I/O error. */
int  hexdump_export_raw(const char *path, const void *buf, uint32_t sz);

#endif /* WACKI_VIEWER_HEXDUMP_H */
