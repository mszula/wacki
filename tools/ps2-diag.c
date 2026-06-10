/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * tools/ps2-diag.c — standalone PS2 smoke test that reports via SCREEN
 * COLOUR, because ps2sdk's fprintf(stderr) doesn't reach PCSX2's console.
 * One run reveals the three things that can kill the engine's startup
 * (FindDataRoot → PlatformInit), in the same order the engine does them.
 *
 * What you'll see:
 *   - Stays BLACK forever            -> SDL video init failed. Rendering
 *                                       is the blocker (not data, not audio).
 *   - BLUE for ~2 s                  -> SDL video WORKS.
 *   - then a steady screen:
 *       bottom colour  GREEN         -> a Dane_02 archive opened via fopen
 *                                       (N white bars = which path form, see
 *                                       the candidate list below)
 *                      RED           -> no candidate opened (file I/O blocked)
 *       top stripe     CYAN          -> SDL audio init + open device OK
 *                      ORANGE        -> audio subsystem init OK, but
 *                                       SDL_OpenAudioDevice failed
 *                      MAGENTA       -> SDL_INIT_AUDIO failed (this is what
 *                                       would abort the engine's combined
 *                                       SDL_Init(VIDEO|EVENTS|AUDIO))
 *
 * Files are probed BEFORE SDL_Init (the engine reads data in FindDataRoot
 * before SDL too, and SDL's IOP reset can tear down host:). The screen then
 * holds forever so nothing falls back to the BIOS menu.
 */

#include <SDL.h>
#include <stdio.h>

/* Candidate data paths, tried in order; 1-based index of the first hit is
 * drawn as that many white bars. */
static const char *const k_candidates[] = {
    "host:data/DANE_02.DTA",          /* 1 */
    "host:data/Dane_02.dta",          /* 2 */
    "host0:data/DANE_02.DTA",         /* 3 */
    "host:DANE_02.DTA",               /* 4  archives beside the ELF */
    "host:./data/DANE_02.DTA",        /* 5 */
    "cdrom0:\\DATA\\DANE_02.DTA;1",   /* 6  ISO/disc */
    "mass:/data/DANE_02.DTA",         /* 7  USB */
};
#define N_CANDIDATES ((int)(sizeof k_candidates / sizeof k_candidates[0]))

static int can_open(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

int main(void)
{
    /* 1. FILE PROBE FIRST — before SDL_Init, matching the engine's
     *    FindDataRoot (and before SDL's IOP reset can kill host:). */
    int found = -1;
    for (int i = 0; i < N_CANDIDATES; ++i) {
        printf("[diag] try %s\n", k_candidates[i]);
        if (can_open(k_candidates[i])) { found = i; break; }
    }

    /* 2. VIDEO. Hang black on failure so it's unmistakable. */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) { for (;;) {} }
    SDL_Window   *w = SDL_CreateWindow("wacki-diag", 0, 0, 640, 448,
                                       SDL_WINDOW_SHOWN);
    SDL_Renderer *r = w ? SDL_CreateRenderer(w, -1, 0) : NULL;
    if (!r) { for (;;) {} }

    for (int i = 0; i < 120; ++i) {              /* blue ~2 s = video OK */
        SDL_SetRenderDrawColor(r, 0, 0, 255, 255);
        SDL_RenderClear(r);
        SDL_RenderPresent(r);
        SDL_Delay(16);
    }

    /* 3. AUDIO — the suspect. Init subsystem, then actually open a device
     *    the way the mixer does (22050 S16 stereo). */
    int audio = 0;                               /* 0=init fail,1=ok,2=open fail */
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
        audio = 1;
        SDL_AudioSpec want, have;
        SDL_zero(want);
        want.freq = 22050; want.format = AUDIO_S16SYS;
        want.channels = 2;  want.samples = 1024;
        SDL_AudioDeviceID dev =
            SDL_OpenAudioDevice(NULL, 0, &want, &have,
                                SDL_AUDIO_ALLOW_ANY_CHANGE);
        if (dev == 0) audio = 2;
        else SDL_CloseAudioDevice(dev);
    }

    /* 4. Hold the verdict forever. */
    for (;;) {
        if (found < 0) SDL_SetRenderDrawColor(r, 180,   0,   0, 255); /* RED  */
        else           SDL_SetRenderDrawColor(r,   0, 150,   0, 255); /* GREEN*/
        SDL_RenderClear(r);

        SDL_Rect top = { 0, 0, 640, 80 };
        if      (audio == 1) SDL_SetRenderDrawColor(r,   0, 200, 255, 255); /* CYAN    */
        else if (audio == 2) SDL_SetRenderDrawColor(r, 255, 170,   0, 255); /* ORANGE  */
        else                 SDL_SetRenderDrawColor(r, 255,   0, 255, 255); /* MAGENTA */
        SDL_RenderFillRect(r, &top);

        if (found >= 0) {
            SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
            for (int b = 0; b <= found; ++b) {
                SDL_Rect bar = { 40 + b * 70, 180, 40, 128 };
                SDL_RenderFillRect(r, &bar);
            }
        }
        SDL_RenderPresent(r);
        SDL_Delay(33);
    }
}
