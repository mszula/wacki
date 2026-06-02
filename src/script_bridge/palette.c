/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* src/script_bridge/palette.c — palette fade script bridges.
 *
 * Three opcodes drive palette fades:
 *
 *   - op 0x48 ScriptCallPalLoad(step, selector, with_fade=1): snapshot
 *     the current palette, load the target palette (selector picks
 *     between identity / white / black / gray / PE-resolved filename),
 *     and start an in-progress fade.
 *   - op 0x4A ScriptCallPalLoad(0, selector, with_fade=0): like 0x48
 *     but applies the target palette immediately (no fade).
 *   - op 0x49 ScriptCallPalFadeStep: advance the fade by one step.
 *     Returns 1 when complete; scripts poll-loop on this.
 */

#include "wacki.h"
#include "wacki/log.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern const void *xlat_binary_ptr(uint32_t addr);
extern uint8_t     g_palette_rgb[256 * 3];

/* ---- constants ---------------------------------------------------- */

#define PALETTE_BYTES               (256 * 3)
#define FADE_PROGRESS_COMPLETE      100
#define FADE_DEFAULT_STEP           4

/* Selector special values for ScriptCallPalLoad. Anything outside this
 * range is treated as a PE address of a palette filename. */
#define PAL_SELECTOR_IDENTITY       0
#define PAL_SELECTOR_WHITE          1
#define PAL_SELECTOR_BLACK          2
#define PAL_SELECTOR_GRAY           3

#define PAL_FILL_WHITE              0xFF
#define PAL_FILL_BLACK              0x00
#define PAL_FILL_GRAY               0x80

#define INSTALL_PALETTE_FIRST_SLOT  0

/* ---- module state ------------------------------------------------- */

static uint8_t  s_fade_source[PALETTE_BYTES];
static uint8_t  s_fade_target[PALETTE_BYTES];
static int      s_fade_progress = FADE_PROGRESS_COMPLETE;  /* idle when at COMPLETE */
static int      s_fade_step     = FADE_DEFAULT_STEP;
static int      s_fade_active   = 0;

/* ---- helpers ------------------------------------------------------- */

/* Load the requested target palette into s_fade_target. The selector
 * argument can be one of the special PAL_SELECTOR_* values OR a PE
 * address of a palette filename string. */
static void load_fade_target(uint32_t selector)
{
    switch (selector) {
    case PAL_SELECTOR_IDENTITY:
        memcpy(s_fade_target, g_palette_rgb, sizeof s_fade_target);
        return;
    case PAL_SELECTOR_WHITE:
        memset(s_fade_target, PAL_FILL_WHITE, sizeof s_fade_target);
        return;
    case PAL_SELECTOR_BLACK:
        memset(s_fade_target, PAL_FILL_BLACK, sizeof s_fade_target);
        return;
    case PAL_SELECTOR_GRAY:
        memset(s_fade_target, PAL_FILL_GRAY, sizeof s_fade_target);
        return;
    default:
        break;
    }

    /* PE address of a palette filename — try to load it from the DTA. */
    const char *name = (const char *)xlat_binary_ptr(selector);
    if (!name || !*name) return;

    void    *pal = NULL;
    uint32_t psz = 0;
    if (LoadFileFromDta(name, &pal, &psz) && pal) {
        size_t cpy = psz < sizeof s_fade_target ? psz : sizeof s_fade_target;
        memcpy(s_fade_target, pal, cpy);
        xfree(pal);
    } else {
        LOG_TRACE("script", "pal-load '%s' missing", name);
        /* Failure → target = current so the resulting fade is invisible. */
        memcpy(s_fade_target, g_palette_rgb, sizeof s_fade_target);
    }
}

/* Apply the target palette immediately (no-fade path). */
static void snap_to_target(void)
{
    memcpy(g_palette_rgb, s_fade_target, sizeof g_palette_rgb);
    InstallPalette(g_palette_rgb, INSTALL_PALETTE_FIRST_SLOT);
    s_fade_progress = FADE_PROGRESS_COMPLETE;
    s_fade_active   = 0;
}

/* Linear interpolation step: g_palette_rgb = lerp(source, target, progress). */
static void apply_fade_step(int progress)
{
    for (int i = 0; i < (int)sizeof g_palette_rgb; ++i) {
        int s = s_fade_source[i];
        int t = s_fade_target[i];
        g_palette_rgb[i] = (uint8_t)(s + ((t - s) * progress) / FADE_PROGRESS_COMPLETE);
    }
    InstallPalette(g_palette_rgb, INSTALL_PALETTE_FIRST_SLOT);
}

/* ---- public API ---------------------------------------------------- */

void ScriptCallPalLoad(uint16_t fade_step, uint32_t selector, int with_fade)
{
    LOG_TRACE("script", "pal load sel=0x%x fade=%d step=%u", selector, with_fade, fade_step);

    /* Snapshot the current palette as the fade source. */
    memcpy(s_fade_source, g_palette_rgb, sizeof s_fade_source);

    load_fade_target(selector);

    s_fade_progress = 0;
    s_fade_step     = fade_step ? (int)fade_step : FADE_DEFAULT_STEP;
    s_fade_active   = 1;

    if (!with_fade) snap_to_target();
}

int ScriptCallPalFadeStep(void)
{
    if (!s_fade_active || s_fade_progress >= FADE_PROGRESS_COMPLETE) {
        return 1;
    }

    s_fade_progress += s_fade_step;
    if (s_fade_progress > FADE_PROGRESS_COMPLETE) {
        s_fade_progress = FADE_PROGRESS_COMPLETE;
    }
    apply_fade_step(s_fade_progress);

    if (s_fade_progress >= FADE_PROGRESS_COMPLETE) {
        s_fade_active = 0;
        return 1;
    }
    return 0;
}
