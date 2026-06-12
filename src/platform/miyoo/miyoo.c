/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * _GNU_SOURCE must be defined before any system header so glibc's
 * features.h locks in the GNU extension subset — needed for
 * RTLD_DEFAULT in dlfcn.h. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/* src/platform/miyoo/miyoo.c — Miyoo Mini Plus / OnionOS specific glue.
 *
 * Linked into the binary only when building with TARGET=miyoo (see
 * Makefile). Keeps platform_sdl.c portable; anything that touches
 * libmi_ao, OnionOS config files, or hardware-button keysym mapping
 * specific to the mmiyoo SDL2 backend lives here.
 *
 * This is the Miyoo target's single platform-hooks provider. Real hooks:
 *   plat_restore_system_volume — re-apply the OnionOS-saved system volume via
 *       MStar's MI_AO_SetVolume, after every SDL_OpenAudioDevice (the mmiyoo
 *       SDL2 backend resets the MStar audio-out volume on each open).
 *   plat_handle_platform_key — translate a Miyoo hardware button (delivered
 *       as an SDL keysym by the mmiyoo backend) into the engine input latches.
 * The remaining hooks (plat_apply_video_prefs, plat_pad_read_extra) are
 * no-ops here — the Miyoo has a fixed display and no controller / USB mouse.
 * All four are declared in the HAL headers (system.h / input.h / video.h).
 *
 * Background on the volume side: OnionOS' launch_standalone.sh kills
 * the audioserver daemon before launching ports (KillAudioserver=1 in
 * our .notfound shortcut) because the engine drives /dev/dsp directly
 * via SDL_AudioDevice and needs exclusive access. audioserver is also
 * the daemon that boot-applies the user's saved volume to the kernel
 * mixer — without it the mixer sits at its driver default (typically
 * max) until the user hits Vol+/-, at which point OnionOS' firmware-
 * level input handler calls into MI_AO_SetVolume to restore the saved
 * value. We do the same restoration in-process so it works regardless
 * of how the engine was launched and persists across SDL device
 * re-opens. */

#include "wacki.h"
#include "wacki/log.h"
#include "wacki/platform/system.h"   /* plat_restore_system_volume */
#include "wacki/platform/input.h"    /* plat_handle_platform_key, plat_pad_read_extra */
#include "wacki/platform/video.h"    /* plat_apply_video_prefs */

#include <SDL.h>

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- OnionOS volume config reader -------------------------------- */

/* Find the saved 0..20 volume in OnionOS system.json. Returns -1 if
 * no config file matched or the field is missing. */
static int read_onion_volume(void)
{
    /* OnionOS 4.x keeps user prefs in system.json with "vol": N (0-20). */
    const char *paths[] = {
        "/mnt/SDCARD/.tmp_update/config/system.json",
        "/appconfigs/system.json",
        NULL
    };
    for (int i = 0; paths[i]; ++i) {
        FILE *fp = fopen(paths[i], "rb");
        if (!fp) continue;
        char buf[2048];
        size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
        fclose(fp);
        buf[n] = 0;
        /* Match the JSON shape "vol": N — tolerate spaces, ignore
         * surrounding fields. */
        const char *p = strstr(buf, "\"vol\"");
        if (!p) continue;
        p = strchr(p, ':');
        if (!p) continue;
        ++p;
        while (*p == ' ' || *p == '\t') ++p;
        int v = -1;
        if (sscanf(p, "%d", &v) == 1 && v >= 0 && v <= 100) {
            LOG_INFO("volume", "read vol=%d from %s", v, paths[i]);
            return v;
        }
    }
    return -1;
}

/* ---- MI_AO_SetVolume dynamic-bind -------------------------------- */

/* OnionOS dB lookup table — index = saved volume 0..20, value = dB
 * passed to MI_AO_SetVolume. Same curve audioserver uses. -60 dB is
 * effectively mute; 0 dB is unity; positive values are gain. */
static const int s_vol_db_table[21] = {
    -60, -50, -45, -40, -35, -30, -25, -20, -15, -10,
     -8,  -6,  -4,  -2,   0,   2,   4,   6,   8,  10, 12
};

/* MStar MI_AO_SetVolume(AoDevId, s32VolumeDb) — exposed by libmi_ao.so
 * which the mmiyoo SDL2 backend already dlopen'd for its own audio
 * path. We grab the symbol via RTLD_DEFAULT and call it directly
 * because tinymix on MMP returns "Invalid mixer control" for every
 * alsa name (kernel exposes MStar's MI audio interface, not alsa
 * controls). */
typedef int (*mi_ao_set_volume_fn)(int /*AoDevId*/, int /*s32VolumeDb*/);
static mi_ao_set_volume_fn s_mi_ao_set_volume = NULL;

/* Try to resolve MI_AO_SetVolume. Retries on every call so the FIRST
 * plat_restore_system_volume — fired from PlatformInit before any
 * SDL_OpenAudioDevice — can fail (libmi_ao not yet in process) and a later
 * call (after the mixer opens its device) can succeed once SDL has pulled in
 * the library. */
static void resolve_mi_ao(void)
{
    if (s_mi_ao_set_volume) return;

    /* RTLD_DEFAULT looks across every shared library already loaded by
     * the process — SDL2's mmiyoo backend has libmi_ao in there from
     * the first SDL_OpenAudioDevice (see "_MI_AO_OpenVqeLib: success"
     * in the audio init log). */
    s_mi_ao_set_volume = (mi_ao_set_volume_fn)
        dlsym(RTLD_DEFAULT, "MI_AO_SetVolume");
    if (s_mi_ao_set_volume) {
        LOG_INFO("volume", "MI_AO_SetVolume resolved via RTLD_DEFAULT");
        return;
    }

    /* Fallback: dlopen the lib explicitly. RTLD_NOW so we get any
     * symbol resolution errors up front instead of segfaulting on
     * first call. */
    void *h = dlopen("libmi_ao.so", RTLD_NOW);
    if (!h) {
        /* Don't spam — log at INFO once per unsuccessful attempt is
         * acceptable since attempts only happen on device open. */
        LOG_INFO("volume", "dlopen libmi_ao.so failed: %s",
                 dlerror() ? dlerror() : "(no error)");
        return;
    }
    s_mi_ao_set_volume = (mi_ao_set_volume_fn)dlsym(h, "MI_AO_SetVolume");
    if (s_mi_ao_set_volume) {
        LOG_INFO("volume", "MI_AO_SetVolume resolved via libmi_ao.so");
    } else {
        LOG_INFO("volume", "dlsym MI_AO_SetVolume failed: %s",
                 dlerror() ? dlerror() : "(no error)");
    }
}

/* ---- public entry ------------------------------------------------- */

/* Re-apply the OnionOS-saved volume. Safe to call multiple times; the
 * intended call sites are:
 *   - PlatformInit (first chance — may no-op if libmi_ao isn't loaded
 *                   yet, that's fine; subsequent calls retry resolve)
 *   - audio_sdl.c plat_audio_open   (after the SFX/music device opens)
 *   - audio_sdl.c plat_avi_audio_begin (after each AVI device opens)
 *
 * Failure modes are logged at INFO but never fatal — the only
 * consequence is the user hears playback at the firmware default
 * volume until they hit Vol+/-, which is the existing (pre-fix)
 * behaviour. */
void plat_restore_system_volume(void)
{
    resolve_mi_ao();

    int vol = read_onion_volume();
    if (vol < 0) {
        LOG_INFO("volume", "no saved volume found — leaving driver default");
        return;
    }
    if (vol > 20) vol = 20;
    if (vol < 0)  vol = 0;
    int db = s_vol_db_table[vol];

    LOG_INFO("volume", "applying vol=%d -> %d dB", vol, db);

    if (!s_mi_ao_set_volume) {
        LOG_INFO("volume", "MI_AO_SetVolume not available — no-op");
        return;
    }
    /* AoDevId 0 is the headphone/speaker output on MMP. */
    int rc = s_mi_ao_set_volume(0, db);
    LOG_INFO("volume", "MI_AO_SetVolume(0, %d dB) -> rc=0x%X", db, rc);
}

/* ---- hardware-button → engine latch mapping ----------------------- */

/* Engine input latches we drive from the Miyoo hardware buttons.
 * All defined in src/main.c (or platform_sdl.c) and declared extern
 * in wacki/globals.h — re-declared here as a quick sanity check that
 * the symbols actually exist at link time. */

/* Translate one Miyoo Mini Plus hardware button (delivered as an
 * SDL_Keycode by the mmiyoo SDL2 backend) into engine input latches.
 *
 * Returns 1 if the keysym resulted in a face-button MOUSE-CLICK latch
 * (A → LMB, B → RMB) — used by the input-debug log in platform_sdl.c
 * to annotate the dump. Non-click bindings (START / L* / R* → request
 * flags) also return 0 — debug-log doesn't need to distinguish them.
 *
 * Button → latch table (the mmiyoo backend feeds these keysyms; we
 * accept several variants per button because mmiyoo forks differ on
 * which scancode they pick for L2/R2):
 *
 *   A          (SDLK_SPACE)             → LMB latch + swallow keystate
 *   B          (SDLK_LCTRL)             → RMB latch + swallow keystate
 *   START      (SDLK_RETURN)            → pause menu  (= F12 opszyns)
 *   L1/L2      (TAB / PAGEUP / e)       → quickload   (= F9)
 *   R1/R2      (BACKSPACE / PAGEDOWN/t) → quicksave   (= F5)
 *
 * Mnemonic: L for "Load", R for "Record state". Volume keys aren't
 * in the table — the firmware handler applies them via MI_AO_SetVolume
 * itself, no engine action needed.
 *
 * Why swallow g_key_state on face buttons: play_loop.c had a VK_SPACE
 * → toggle-active-actor binding. Without the swallow, every A press
 * fired BOTH the click latch AND the actor toggle ("A robi LMB+RMB
 * jednocześnie" from the user's bug report). play_loop.c now skips
 * the toggle under WACKI_HANDHELD too, so the swallow is belt-and-
 * suspenders. */
int plat_handle_platform_key(int sym)
{
    switch (sym) {
    case SDLK_SPACE:                       /* A → LMB */
        g_lmb_clicked = 1;
        g_key_state &= 0xFF00;
        return 1;
    case SDLK_LCTRL:                       /* B → RMB */
        g_rmb_clicked = 1;
        g_key_state &= 0xFF00;
        return 1;
    case SDLK_RETURN:                      /* START → pause menu */
        g_pause_menu_request = 1;
        return 0;
    case SDLK_TAB:
    case SDLK_PAGEUP:
    case SDLK_e:                           /* L1/L2 → quickload */
        g_quickload_request = 1;
        return 0;
    case SDLK_BACKSPACE:
    case SDLK_PAGEDOWN:
    case SDLK_t:                           /* R1/R2 → quicksave */
        g_quicksave_request = 1;
        return 0;
    default:
        return 0;
    }
}

/* The Miyoo has a fixed fullscreen display and no SDL_GameController / USB
 * mouse, so these platform hooks are no-ops here. Its hardware buttons arrive
 * as unpredictable firmware keysyms, so it reports no usable keyboard. */
void plat_apply_video_prefs(void)              { }
void plat_pad_read_extra(float *ax, float *ay) { (void)ax; (void)ay; }
int  plat_input_has_keyboard(void)             { return 0; }
