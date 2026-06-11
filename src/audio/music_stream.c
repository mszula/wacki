/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/audio/music_stream.c — streaming background music.
 *
 * Large standalone BGM (the ~25 MB menu music) is played through a fixed
 * looping ring on MIX_CHAN_MUSIC instead of being loaded whole, so the RAM
 * footprint stays flat regardless of track length. A per-frame producer
 * (music_stream_refill, run from TickMenuMusic on the game thread) keeps
 * the ring topped up from the open file, looping at end of PCM data.
 *
 * Single-producer / single-consumer, lock-free in steady state: the mixer
 * (audio thread) only READS the ring and advances MIX_CHAN_MUSIC.pos; the
 * producer reads that play position (a naturally-atomic aligned 32-bit
 * field) to bound how much of the already-played region it refills, staying
 * a near-full ring ahead so the consumer never reads a byte the producer
 * hasn't written. The two touch disjoint regions; desktop + Linux/ARM
 * handhelds have cache-coherent SMP so no flush is needed — but on PS2
 * fileXio DMAs the file data into RAM behind the EE data cache, so the
 * producer must flush each just-written span (ms_dcache_flush) or the
 * audio thread reads a stale ring. Setup/teardown synchronise with the
 * callback through mixer_assign / mixer_stop_channel (which lock the device).
 * Streaming is raw (no per-chunk convert), so it only engages when the
 * file's format already matches the mixer output exactly. */

#include "wacki.h"
#include "wacki/log.h"

#include <SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mixer_internal.h"
#include "music_stream.h"

#define MUSIC_RING_BYTES        (256 * 1024)        /* ~3.0 s @ 88200 B/s */
#define MUSIC_READ_MAX          (64 * 1024)         /* cap per-frame file read */
#define MUSIC_STREAM_MIN_BYTES  (4u * 1024 * 1024)  /* below this, load whole */

/* PS2: fileXio DMAs into EE RAM behind the data cache, so the audio thread
 * (a separate EE thread) can read stale bytes from the ring. Flush the span
 * the producer just wrote. Other targets are cache-coherent → no-op. */
#ifdef WACKI_PS2
extern void SyncDCache(void *start, void *end);
static inline void ms_dcache_flush(void *p, uint32_t n) { SyncDCache(p, (uint8_t *)p + n); }
#else
static inline void ms_dcache_flush(void *p, uint32_t n) { (void)p; (void)n; }
#endif

static CygFile  *s_mstream_file   = NULL;
static uint8_t  *s_mstream_ring   = NULL;   /* non-owning: MIX_CHAN_MUSIC owns it */
static uint32_t  s_mstream_rlen   = 0;
static uint32_t  s_mstream_wpos   = 0;      /* producer write cursor (bytes) */
static uint32_t  s_mstream_dstart = 0;      /* PCM data file offset */
static uint32_t  s_mstream_dend   = 0;      /* PCM data end file offset */
static uint32_t  s_mstream_fpos   = 0;      /* current file read offset */
static int       s_mstream_loop   = 0;
static int       s_mstream_on     = 0;

/* Find the `data` chunk and verify the PCM format matches the mixer output
 * (so we can stream raw, no conversion). Canonical RIFF/WAVE only. */
static int music_wav_probe(CygFile *f, uint32_t *data_off, uint32_t *data_sz)
{
    uint8_t h[256];
    fseek_cyg(f, 0, SEEK_SET);
    if (fread_cyg(h, 1, sizeof h, f) < 12) return 0;
    if (memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return 0;
    int fmt_ok = 0, found = 0;
    uint32_t p = 12;
    while (p + 8 <= sizeof h) {
        const uint8_t *c = h + p;
        uint32_t csz = c[4] | (c[5] << 8) | (c[6] << 16) | ((uint32_t)c[7] << 24);
        if (!memcmp(c, "fmt ", 4)) {
            uint16_t afmt = c[8]  | (c[9]  << 8);
            uint16_t ch   = c[10] | (c[11] << 8);
            uint32_t rate = c[12] | (c[13] << 8) | (c[14] << 16) | ((uint32_t)c[15] << 24);
            uint16_t bits = c[22] | (c[23] << 8);
            fmt_ok = (afmt == 1 && ch == MIX_OUT_CHANS &&
                      rate == (uint32_t)MIX_OUT_FREQ && bits == 16);
        } else if (!memcmp(c, "data", 4)) {
            *data_off = p + 8;
            *data_sz  = csz;
            found = 1;
            break;
        }
        p += 8 + csz + (csz & 1);
    }
    return found && fmt_ok;
}

/* Upper-case the basename in place (case-sensitive FS fallback). */
static void music_upper_basename(char *p)
{
    size_t l = strlen(p), i = l;
    while (i > 0 && p[i - 1] != '/' && p[i - 1] != '\\') --i;
    for (size_t j = i; j < l; ++j)
        if (p[j] >= 'a' && p[j] <= 'z') p[j] &= 0xDF;
}

/* Locate `name` as a standalone file across the data roots (mirrors
 * try_load_wav_at's search), with an upper-cased-basename retry. */
static CygFile *music_open_data_file(const char *name)
{
    const char *roots[3];
    int nr = 0;
    roots[nr++] = "";                                        /* cwd / absolute */
    if (g_data_root[0]) roots[nr++] = g_data_root;
    roots[nr++] = "./data";
    for (int r = 0; r < nr; ++r) {
        char p[1024];
        if (roots[r][0]) snprintf(p, sizeof p, "%s/%s", roots[r], name);
        else             snprintf(p, sizeof p, "%s", name);
        CygFile *f = fopen_cyg(p, "rb");
        if (f) return f;
        music_upper_basename(p);
        f = fopen_cyg(p, "rb");
        if (f) return f;
    }
    return NULL;
}

int music_stream_try_open(const char *name, int loop)
{
    CygFile *f = music_open_data_file(name);
    if (!f) return 0;

    uint32_t doff = 0, dsz = 0;
    if (!music_wav_probe(f, &doff, &dsz)) { fclose_cyg(f); return 0; }

    fseek_cyg(f, 0, SEEK_END);
    uint32_t fsize = (uint32_t)ftell_cyg(f);
    uint32_t dend  = doff + dsz;
    if (dend > fsize) dend = fsize;
    if (dend <= doff) { fclose_cyg(f); return 0; }

    /* Small clips load whole (simpler, lower latency, no held handle). */
    if (dend - doff < MUSIC_STREAM_MIN_BYTES) { fclose_cyg(f); return 0; }

    uint8_t *ring = (uint8_t *)SDL_malloc(MUSIC_RING_BYTES);
    if (!ring) { fclose_cyg(f); return 0; }

    /* Prime the ring nearly full (leave a 4-byte gap so wpos == rpos stays
     * an unambiguous "full"), starting at the PCM data. */
    uint32_t prime  = MUSIC_RING_BYTES - 4;
    uint32_t favail = dend - doff;
    if (prime > favail) prime = favail & ~3u;
    fseek_cyg(f, (int32_t)doff, SEEK_SET);
    uint32_t got = fread_cyg(ring, 1, prime, f);
    ms_dcache_flush(ring, got);                 /* PS2: DMA bypassed the cache */

    s_mstream_file   = f;
    s_mstream_ring   = ring;
    s_mstream_rlen   = MUSIC_RING_BYTES;
    s_mstream_wpos   = got % MUSIC_RING_BYTES;
    s_mstream_dstart = doff;
    s_mstream_dend   = dend;
    s_mstream_fpos   = doff + got;
    s_mstream_loop   = loop ? 1 : 0;
    s_mstream_on     = 1;

    /* Hand the ring to the mixer as a looping channel; it now owns the
     * buffer (SDL_free on stop). pos starts at 0. */
    mixer_assign(MIX_CHAN_MUSIC, ring, MUSIC_RING_BYTES, 1, name);
    LOG_INFO("music", "%s streaming: %u-byte ring, %u bytes PCM",
             name, (unsigned)MUSIC_RING_BYTES, (unsigned)(dend - doff));
    return 1;
}

void music_stream_refill(void)
{
    if (!s_mstream_on) return;
    uint32_t rlen = s_mstream_rlen;
    uint32_t rpos = s_mix[MIX_CHAN_MUSIC].pos;     /* atomic aligned 32-bit read */
    if (rpos >= rlen) rpos %= rlen;
    uint32_t empty = (rpos + rlen - s_mstream_wpos) % rlen;
    if (empty <= 4) return;
    uint32_t budget = empty - 4;
    if (budget > MUSIC_READ_MAX) budget = MUSIC_READ_MAX;
    while (budget >= 4) {
        uint32_t favail = s_mstream_dend - s_mstream_fpos;
        if (favail == 0) {
            if (!s_mstream_loop) break;
            fseek_cyg(s_mstream_file, (int32_t)s_mstream_dstart, SEEK_SET);
            s_mstream_fpos = s_mstream_dstart;
            favail = s_mstream_dend - s_mstream_fpos;
            if (favail == 0) break;
        }
        uint32_t to_end = rlen - s_mstream_wpos;
        uint32_t n = budget;
        if (n > to_end) n = to_end;
        if (n > favail) n = favail;
        n &= ~3u;
        if (n == 0) break;
        uint32_t got = fread_cyg(s_mstream_ring + s_mstream_wpos, 1, n, s_mstream_file);
        if (got == 0) break;
        ms_dcache_flush(s_mstream_ring + s_mstream_wpos, got);  /* PS2: DMA vs cache */
        s_mstream_wpos = (s_mstream_wpos + got) % rlen;
        s_mstream_fpos += got;
        budget -= got;
    }
}

void music_stream_stop(void)
{
    s_mstream_on = 0;
    if (s_mstream_file) { fclose_cyg(s_mstream_file); s_mstream_file = NULL; }
    s_mstream_ring = NULL;
    s_mstream_rlen = s_mstream_wpos = 0;
    s_mstream_dstart = s_mstream_dend = s_mstream_fpos = 0;
}
