/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* src/heap.c — thin C wrappers over malloc / free / calloc-with-flag.
 *
 * In the original engine these are C++ method exports of Cygert's
 * "Base_IO_CPP" allocator class (alloc / free / new-with-vtable-hook /
 * calloc-with-init-flag). For the port they collapse to the libc
 * allocator — the engine has no GC, no pool tracking, nothing else the
 * original allocator did that we need to mirror.
 *
 * Kept as named wrappers (not #defines) so callers retain a single
 * signature and so a future build can swap in a tracking allocator
 * without touching every call site. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void *xmalloc(uint32_t sz) { return malloc(sz); }

void  xfree(void *p)       { free(p); }

void *xcalloc(uint32_t sz, int zero)
{
    void *p = malloc(sz);
    if (p && zero) memset(p, 0, sz);
    return p;
}
