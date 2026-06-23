/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform/wii/data_root_wii.c — Wii SD-card data-root probe.
 *
 * Homebrew Channel convention: apps live in sd:/apps/<appname>/, so the
 * player drops Dane_*.dta and WACKI.EXE in sd:/apps/wacki/data/ alongside
 * the boot.dol. */

#include "wacki/platform/storage.h"
#include <stddef.h>

int plat_data_roots(int (*probe)(const char *root))
{
    static const char *const candidates[] = {
        "sd:/apps/wacki/data",
        "sd:/apps/wacki",
        "sd:/wacki/data",
        "sd:/wacki",
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
