/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform/switch/data_root_switch.c — Switch SD-card data-root probe. */

#include "wacki/platform/storage.h"
#include <stddef.h>

int plat_data_roots(int (*probe)(const char *root))
{
    static const char *const candidates[] = {
        "sdmc:/switch/wacki/data",
        "sdmc:/switch/wacki",
        "sdmc:/wacki/data",
        "sdmc:/wacki",
    };
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; ++i) {
        int r = probe(candidates[i]);
        if (r) return r;
    }
    return 0;
}

int plat_prompt_data_folder(int (*probe)(const char *root))
{
    (void)probe;
    return 0;
}
