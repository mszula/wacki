/* src/script_bridge/palette.c — palette fade script bridges.
 *
 * Two opcodes drive palette fades:
 *
 * - op 0x48 ScriptCallPalLoad(step, selector, with_fade=1): snapshot
 * the current palette, load the target palette (selector = 0
 * identity, 1 = white, 2 = black, 3 = gray, else PE-resolved
 * filename), and start a fade in progress.
 * - op 0x4A ScriptCallPalLoad(0, selector, with_fade=0): like 0x48
 * but applies the target palette immediately (no fade).
 * - op 0x49 ScriptCallPalFadeStep: advance the fade by one step.
 * Returns 1 when the fade completes; scripts poll-loop on this.
 */

#include "wacki.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern const void *xlat_binary_ptr(uint32_t addr);

extern uint8_t g_palette_rgb[256*3];

static uint8_t  s_pal_fade_source [256*3];
static uint8_t  s_pal_fade_target [256*3];
static int      s_pal_fade_progress = 100;   /* 100 = no fade in flight */
static int      s_pal_fade_step     = 4;
static int      s_pal_fade_active   = 0;

void ScriptCallPalLoad(uint16_t fade_step, uint32_t selector, int with_fade)
{
    fprintf(stderr, "[script] pal load sel=0x%x fade=%d step=%u\n",
            selector, with_fade, fade_step);

    /* Snapshot CURRENT palette as fade source. */
    memcpy(s_pal_fade_source, g_palette_rgb, sizeof s_pal_fade_source);

    /* Load fade target. */
    if (selector == 0) {
        /* Target = current → fade to itself (no visible change). */
        memcpy(s_pal_fade_target, g_palette_rgb, sizeof s_pal_fade_target);
    } else if (selector == 1) {
        memset(s_pal_fade_target, 0xFF, sizeof s_pal_fade_target);  /* white */
    } else if (selector == 2) {
        memset(s_pal_fade_target, 0x00, sizeof s_pal_fade_target);  /* black */
    } else if (selector == 3) {
        memset(s_pal_fade_target, 0x80, sizeof s_pal_fade_target);  /* gray */
    } else {
        const char *name = (const char *)xlat_binary_ptr(selector);
        if (name && *name) {
            void *pal = NULL; uint32_t psz = 0;
            if (LoadFileFromDta(name, &pal, &psz) && pal) {
                size_t cpy = psz < sizeof s_pal_fade_target
                             ? psz : sizeof s_pal_fade_target;
                memcpy(s_pal_fade_target, pal, cpy);
                xfree(pal);
            } else {
                fprintf(stderr, "[script] pal-load '%s' missing\n", name);
                /* Failed load → target = current (no fade). */
                memcpy(s_pal_fade_target, g_palette_rgb, sizeof s_pal_fade_target);
            }
        }
    }
    s_pal_fade_progress = 0;
    s_pal_fade_step     = fade_step ? (int)fade_step : 4;
    s_pal_fade_active   = 1;
    /* with_fade == 0 (case 0x4A) → script wants snap-to-target. Apply
 * immediately so scripts that don't poll PalFadeStep still see the
 * target take effect. */
    if (!with_fade) {
        memcpy(g_palette_rgb, s_pal_fade_target, sizeof g_palette_rgb);
        InstallPalette(g_palette_rgb, 0);
        s_pal_fade_progress = 100;
        s_pal_fade_active   = 0;
    }
}

/* ScriptCallPalFadeStep —:
 * (source, target, work, progress);
 * (work, 0);
 *
 * Returns 1 when fade complete (poll-loop exit condition). */
int ScriptCallPalFadeStep(void)
{
    if (!s_pal_fade_active || s_pal_fade_progress >= 100) {
        return 1;
    }
    s_pal_fade_progress += s_pal_fade_step;
    if (s_pal_fade_progress > 100) s_pal_fade_progress = 100;
    /* Linear interpolation: out = source + (target - source) * progress / 100. */
    for (int i = 0; i < (int)sizeof g_palette_rgb; ++i) {
        int s = s_pal_fade_source[i];
        int t = s_pal_fade_target[i];
        g_palette_rgb[i] = (uint8_t)(s + ((t - s) * s_pal_fade_progress) / 100);
    }
    InstallPalette(g_palette_rgb, 0);
    if (s_pal_fade_progress >= 100) {
        s_pal_fade_active = 0;
        return 1;
    }
    return 0;
}
