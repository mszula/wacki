/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * assets-explorer/src/vflic.c — AVI cutscene demux + frame render (see vflic.h).
 *
 * AVI layout we care about:
 *   RIFF <sz> 'AVI '
 *     LIST 'hdrl' { avih (fps@0, width@32, height@36), LIST 'strl' … }
 *     LIST 'movi' { '00dc' <sz> <FLC frame>, '01wb' <sz> <audio>, … }
 * Every '00dc' body is one FLC frame; we index them all, then feed each to the
 * engine's flic_decode_frame, which mutates the shared g_back_shadow +
 * g_palette_rgb. We point g_back_shadow at our own buffer for the duration. */

#include "vflic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Engine globals (graphics.c) + the FLIC frame decoder (flic/decoder.c),
 * declared directly to avoid include-order coupling in wacki/globals.h. */
extern uint8_t *g_back_shadow;
extern uint8_t  g_palette_rgb[256 * 3];
extern void flic_decode_frame(const uint8_t *fdata, uint32_t fsize, int w, int h);

/* avih data-area offsets (body = after the 8-byte chunk header). */
#define AVIH_FPS_US   0
#define AVIH_WIDTH    32
#define AVIH_HEIGHT   36

struct VFlic {
    FILE     *fp;
    int       w, h;
    double    fps;
    uint32_t *foff;          /* file offset of each 00dc body */
    uint32_t *fsz;           /* size of each 00dc body */
    int       fcount;
    int       cur;           /* last decoded frame index, -1 = none */
    uint8_t  *shadow;        /* our w*h 8bpp buffer (aliased into g_back_shadow) */
    /* audio: all '01wb' PCM chunks concatenated, + format from the 'strf'. */
    uint8_t  *audio;
    uint32_t  audio_len, audio_cap;
    int       audio_hz, audio_ch, audio_bits;
};

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int vflic_is_avi(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    uint8_t h[12];
    int ok = fread(h, 1, 12, fp) == 12 &&
             memcmp(h, "RIFF", 4) == 0 && memcmp(h + 8, "AVI ", 4) == 0;
    fclose(fp);
    return ok;
}

/* Walk the hdrl LIST: avih → w/h/fps, and the audio strl → format
 * (WAVEFORMATEX: channels@2, samples/sec@4, bits@14 in the strf data). */
static void parse_hdrl(FILE *fp, long start, long end, int *w, int *h, double *fps,
                       int *a_hz, int *a_ch, int *a_bits)
{
    long p = start;
    while (p + 8 <= end) {
        uint8_t sh[8];
        if (fseek(fp, p, SEEK_SET) != 0 || fread(sh, 1, 8, fp) != 8) break;
        uint32_t sz = rd32(sh + 4);
        if (memcmp(sh, "avih", 4) == 0) {
            uint8_t a[48];
            uint32_t n = sz < sizeof a ? sz : (uint32_t)sizeof a;
            if (fread(a, 1, n, fp) == n && n >= AVIH_HEIGHT + 4) {
                uint32_t us = rd32(a + AVIH_FPS_US);
                *fps = us ? 1000000.0 / (double)us : 15.0;
                *w   = (int)rd32(a + AVIH_WIDTH);
                *h   = (int)rd32(a + AVIH_HEIGHT);
            }
        } else if (memcmp(sh, "LIST", 4) == 0) {
            uint8_t lt[4];
            long lp = p + 8;
            if (fseek(fp, lp, SEEK_SET) == 0 && fread(lt, 1, 4, fp) == 4 &&
                memcmp(lt, "strl", 4) == 0) {
                long sp = lp + 4, send = p + 8 + (long)sz;
                int is_audio = 0;
                while (sp + 8 <= send) {
                    uint8_t s2[8];
                    if (fseek(fp, sp, SEEK_SET) != 0 || fread(s2, 1, 8, fp) != 8) break;
                    uint32_t s2sz = rd32(s2 + 4);
                    if (memcmp(s2, "strh", 4) == 0) {
                        uint8_t typ[4];
                        if (fread(typ, 1, 4, fp) == 4)
                            is_audio = (memcmp(typ, "auds", 4) == 0);
                    } else if (memcmp(s2, "strf", 4) == 0 && is_audio) {
                        uint8_t wfe[16];
                        uint32_t rn = s2sz < 16 ? s2sz : 16;
                        if (fread(wfe, 1, rn, fp) == rn && rn >= 16) {
                            *a_ch   = (int)(wfe[2] | (wfe[3] << 8));
                            *a_hz   = (int)rd32(wfe + 4);
                            *a_bits = (int)(wfe[14] | (wfe[15] << 8));
                        }
                    }
                    sp += 8 + (long)s2sz + (s2sz & 1);
                }
            }
        }
        p += 8 + sz + (sz & 1);            /* word-aligned */
    }
}

/* Index every 00dc chunk in [start,end). Returns frame count. */
static int index_movi(VFlic *vf, long start, long end)
{
    int cap = 0;
    long p = start;
    while (p + 8 <= end) {
        uint8_t sh[8];
        if (fseek(vf->fp, p, SEEK_SET) != 0 || fread(sh, 1, 8, vf->fp) != 8) break;
        uint32_t sz = rd32(sh + 4);
        if (memcmp(sh, "00dc", 4) == 0) {
            if (vf->fcount >= cap) {
                cap = cap ? cap * 2 : 256;
                vf->foff = (uint32_t *)realloc(vf->foff, (size_t)cap * 4);
                vf->fsz  = (uint32_t *)realloc(vf->fsz,  (size_t)cap * 4);
                if (!vf->foff || !vf->fsz) return vf->fcount;
            }
            vf->foff[vf->fcount] = (uint32_t)(p + 8);
            vf->fsz[vf->fcount]  = sz;
            vf->fcount++;
        } else if (memcmp(sh, "01wb", 4) == 0 && sz > 0) {
            if (vf->audio_len + sz > vf->audio_cap) {
                uint32_t nc = vf->audio_cap ? vf->audio_cap * 2 : (1u << 16);
                while (nc < vf->audio_len + sz) nc *= 2;
                uint8_t *na = (uint8_t *)realloc(vf->audio, nc);
                if (na) { vf->audio = na; vf->audio_cap = nc; }
            }
            if (vf->audio && vf->audio_len + sz <= vf->audio_cap &&
                fseek(vf->fp, p + 8, SEEK_SET) == 0 &&
                fread(vf->audio + vf->audio_len, 1, sz, vf->fp) == sz)
                vf->audio_len += sz;
        }
        p += 8 + sz + (sz & 1);
    }
    return vf->fcount;
}

VFlic *vflic_open(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    uint8_t hdr[12];
    if (fread(hdr, 1, 12, fp) != 12 ||
        memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "AVI ", 4) != 0) {
        fclose(fp);
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    long filesz = ftell(fp);

    int    w = 0, h = 0, a_hz = 0, a_ch = 0, a_bits = 0;
    double fps = 15.0;
    long   movi_start = 0, movi_end = 0;

    /* Top-level chunks live after the 12-byte RIFF/AVI header. */
    long pos = 12;
    while (pos + 8 <= filesz) {
        uint8_t ch[12];
        if (fseek(fp, pos, SEEK_SET) != 0 || fread(ch, 1, 8, fp) != 8) break;
        uint32_t sz = rd32(ch + 4);
        if (memcmp(ch, "LIST", 4) == 0) {
            uint8_t lt[4];
            if (fread(lt, 1, 4, fp) == 4) {
                if (memcmp(lt, "movi", 4) == 0) {
                    movi_start = pos + 12;
                    movi_end   = pos + 8 + (long)sz;
                    if (movi_end > filesz) movi_end = filesz;
                } else if (memcmp(lt, "hdrl", 4) == 0) {
                    parse_hdrl(fp, pos + 12, pos + 8 + (long)sz, &w, &h, &fps,
                               &a_hz, &a_ch, &a_bits);
                }
            }
        }
        pos += 8 + (long)sz + (sz & 1);
    }

    if (!movi_start || w <= 0 || h <= 0 || w > 4096 || h > 4096) {
        fclose(fp);
        return NULL;
    }

    VFlic *vf = (VFlic *)calloc(1, sizeof *vf);
    if (!vf) { fclose(fp); return NULL; }
    vf->fp = fp;
    vf->w = w; vf->h = h; vf->fps = fps;
    vf->audio_hz = a_hz; vf->audio_ch = a_ch; vf->audio_bits = a_bits;
    vf->cur = -1;
    vf->shadow = (uint8_t *)calloc((size_t)w * h, 1);
    if (!vf->shadow) { fclose(fp); free(vf); return NULL; }

    index_movi(vf, movi_start, movi_end);
    if (vf->fcount == 0) { vflic_close(vf); return NULL; }
    return vf;
}

void vflic_close(VFlic *vf)
{
    if (!vf) return;
    if (vf->fp) fclose(vf->fp);
    free(vf->foff);
    free(vf->fsz);
    free(vf->shadow);
    free(vf->audio);
    free(vf);
}

int    vflic_frame_count(const VFlic *vf) { return vf ? vf->fcount : 0; }
int    vflic_width(const VFlic *vf)       { return vf ? vf->w : 0; }
int    vflic_height(const VFlic *vf)      { return vf ? vf->h : 0; }
double vflic_fps(const VFlic *vf)         { return vf ? vf->fps : 0.0; }

const uint8_t *vflic_audio_data(const VFlic *vf) { return vf ? vf->audio : NULL; }
uint32_t       vflic_audio_len(const VFlic *vf)  { return vf ? vf->audio_len : 0; }
int            vflic_audio_hz(const VFlic *vf)    { return vf ? vf->audio_hz : 0; }
int            vflic_audio_ch(const VFlic *vf)    { return vf ? vf->audio_ch : 0; }
int            vflic_audio_bits(const VFlic *vf)  { return vf ? vf->audio_bits : 0; }

int vflic_render_frame(VFlic *vf, int idx, ViewImage *out)
{
    if (!vf || vf->fcount == 0) return 0;
    if (idx < 0) idx = 0;
    if (idx >= vf->fcount) idx = vf->fcount - 1;

    /* Seeking backward means replaying from frame 0 (delta frames). */
    if (idx < vf->cur) {
        vf->cur = -1;
        memset(vf->shadow, 0, (size_t)vf->w * vf->h);
    }

    /* Point the engine's shared backbuffer at our scratch for the decode. */
    uint8_t *saved = g_back_shadow;
    g_back_shadow = vf->shadow;

    uint8_t *body = NULL;
    uint32_t bcap = 0;
    while (vf->cur < idx) {
        vf->cur++;
        uint32_t sz = vf->fsz[vf->cur];
        if (sz > bcap) {
            uint8_t *nb = (uint8_t *)realloc(body, sz);
            if (!nb) break;
            body = nb; bcap = sz;
        }
        if (fseek(vf->fp, (long)vf->foff[vf->cur], SEEK_SET) == 0 &&
            fread(body, 1, sz, vf->fp) == sz) {
            flic_decode_frame(body, sz, vf->w, vf->h);
        }
    }
    free(body);
    g_back_shadow = saved;

    /* Map 8bpp + the FLIC palette (R,G,B — same order as the engine present
     * path) to RGBA. Cutscene frames are opaque. */
    int w = vf->w, h = vf->h;
    out->rgba = (uint8_t *)malloc((size_t)w * h * 4);
    if (!out->rgba) return 0;
    out->w = w; out->h = h;
    for (int i = 0; i < w * h; ++i) {
        uint8_t ci = vf->shadow[i];
        uint8_t *o = out->rgba + (size_t)i * 4;
        o[0] = g_palette_rgb[ci * 3 + 0];
        o[1] = g_palette_rgb[ci * 3 + 1];
        o[2] = g_palette_rgb[ci * 3 + 2];
        o[3] = 255;
    }
    return 1;
}
