/*
 * flic.c — AAFLC (Autodesk FLIC inside AVI) decoder.
 *
 * The original engine drove this via MCI AVIVideo + VIDEO.DRV (FLCCODEC
 * NE DLL) on Win 9x. We ship a portable C decoder of the FLIC frame
 * format here, walking the AVI container ourselves.
 *
 * Frame chunk types (the only ones the Wacki AVIs use in practice):
 * COLOR_256 (4) palette update, 6-bit DAC scaled to 8-bit
 * DELTA_FLC (7) line-based delta with skip / RLE / runs of 2-byte words
 * BLACK (13) fill the whole frame with colour 0
 * BRUN (15) byte run-length (used for the first/key frame)
 * COPY (16) uncompressed
 *
 * The Wacki AVIs are 640×480, 8-bit, ~10 fps, paletted.
 */
#include "wacki.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint8_t *g_back_shadow;
extern uint8_t  g_palette_rgb[256*3];

/* T43b — AVI chunk headers aren't guaranteed 4-byte aligned (chunks
 * are byte-aligned in the container). Use memcpy to avoid UBSan
 * misaligned-load complaints (also required on strict-alignment ARM). */
static inline uint32_t rd_u32(const uint8_t *p) {
    uint32_t v; memcpy(&v, p, 4); return v;
}

/* ---- AVI walker --------------------------------------------------------- */
typedef struct {
    uint8_t  *buf;
    uint32_t  size;
    uint32_t  movi_start;
    uint32_t  movi_end;
    uint32_t  cursor;
    uint16_t  width;
    uint16_t  height;
    uint32_t  fps_us;            /* microseconds per frame */
    /* audio stream info — populated from the second strl LIST if present */
    uint16_t  audio_channels;
    uint16_t  audio_bits;
    uint32_t  audio_samples_per_sec;
} AviCtx;

/* SDL audio device, opened on demand and reused across cutscenes. */
static SDL_AudioDeviceID s_audio_dev = 0;
static SDL_AudioSpec     s_audio_spec_cur;
static int               s_audio_open = 0;

static void audio_ensure(uint32_t sample_rate, uint16_t channels, uint16_t bits)
{
    if (s_audio_open &&
        s_audio_spec_cur.freq == (int)sample_rate &&
        s_audio_spec_cur.channels == channels &&
        s_audio_spec_cur.format ==
            (SDL_AudioFormat)(bits == 8 ? AUDIO_U8 : AUDIO_S16LSB))
        return;

    if (s_audio_open) { SDL_CloseAudioDevice(s_audio_dev); s_audio_open = 0; }

    SDL_AudioSpec want = {0};
    want.freq     = (int)sample_rate;
    want.format   = (bits == 8) ? AUDIO_U8 : AUDIO_S16LSB;
    want.channels = (Uint8)channels;
    want.samples  = 1024;
    s_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &s_audio_spec_cur,
                                      SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!s_audio_dev) {
        fprintf(stderr, "[audio] SDL_OpenAudioDevice: %s\n", SDL_GetError());
        return;
    }
    s_audio_open = 1;
    SDL_PauseAudioDevice(s_audio_dev, 0);
    fprintf(stderr, "[audio] %u Hz, %d ch, %d-bit\n",
            sample_rate, channels, bits);
}

static int avi_open(AviCtx *c, const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    fseek(fp, 0, SEEK_END);
    c->size = (uint32_t)ftell(fp);
    fseek(fp, 0, SEEK_SET);
    c->buf = (uint8_t *)malloc(c->size);
    if (!c->buf) { fclose(fp); return 0; }
    fread(c->buf, 1, c->size, fp);
    fclose(fp);

    if (memcmp(c->buf, "RIFF", 4) != 0 ||
        memcmp(c->buf + 8, "AVI ", 4) != 0)
    { free(c->buf); return 0; }

    /* Walk hdrl LIST to find 'avih', then each 'strl' to learn about the
 * video ('vids') and audio ('auds') streams. */
    uint32_t p = 12;
    int stream_idx = 0;
    (void)stream_idx;       /* counted for diagnostic; not consumed here */
    while (p + 8 <= c->size) {
        uint32_t tag = rd_u32(c->buf + p);
        uint32_t sz  = rd_u32(c->buf + p + 4);
        if (tag == 0x5453494C /* "LIST" */) {
            uint32_t fourcc = rd_u32(c->buf + p + 8);
            if (fourcc == 0x6C726468 /* "hdrl" */) {
                uint32_t q = p + 12;
                if (memcmp(c->buf + q, "avih", 4) == 0) {
                    c->fps_us = rd_u32(c->buf + q + 8);
                    c->width  = (uint16_t)rd_u32(c->buf + q + 8 + 40 - 8);
                    c->height = (uint16_t)rd_u32(c->buf + q + 8 + 44 - 8);
                }
                p += 12;                  /* descend into hdrl */
                continue;
            }
            if (fourcc == 0x6C727473 /* "strl" */) {
                /* parse strh + strf */
                uint32_t q = p + 12;
                uint32_t end = p + 8 + sz;
                int is_audio = 0;
                while (q + 8 <= end) {
                    uint32_t t2 = rd_u32(c->buf + q);
                    uint32_t s2 = rd_u32(c->buf + q + 4);
                    if (t2 == 0x68727473 /* "strh" */) {
                        /* +0 fccType */
                        uint32_t fcc = rd_u32(c->buf + q + 8);
                        is_audio = (fcc == 0x73647561 /* "auds" */);
                    } else if (t2 == 0x66727473 /* "strf" */ && is_audio) {
                        /* WAVEFORMATEX:
 * +0 word wFormatTag
 * +2 word nChannels
 * +4 dword nSamplesPerSec
 * +8 dword nAvgBytesPerSec
 * +12 word nBlockAlign
 * +14 word wBitsPerSample
 */
                        c->audio_channels        = *(uint16_t *)(c->buf + q + 8 + 2);
                        c->audio_samples_per_sec = rd_u32(c->buf + q + 8 + 4);
                        c->audio_bits            = *(uint16_t *)(c->buf + q + 8 + 14);
                    }
                    q += 8 + s2 + (s2 & 1);
                }
                p += 8 + sz + (sz & 1);
                ++stream_idx;
                continue;
            }
            if (fourcc == 0x69766F6D /* "movi" */) {
                c->movi_start = p + 12;
                c->movi_end   = p + 8 + sz;
                c->cursor     = c->movi_start;
                if (c->audio_samples_per_sec)
                    audio_ensure(c->audio_samples_per_sec,
                                 c->audio_channels, c->audio_bits);
                return 1;
            }
            p += 8 + sz + (sz & 1);
            continue;
        }
        p += 8 + sz + (sz & 1);
    }
    free(c->buf);
    return 0;
}

/* Walk forward until we hit "00dc" (video). Along the way, queue any
 * "01wb" (audio) chunks we cross to the SDL audio device. */
static int avi_next_video(AviCtx *c, uint8_t **out_data, uint32_t *out_size)
{
    while (c->cursor + 8 <= c->movi_end) {
        uint32_t tag = rd_u32(c->buf + c->cursor);
        uint32_t sz  = rd_u32(c->buf + c->cursor + 4);
        uint32_t next = c->cursor + 8 + sz + (sz & 1);
        if (tag == 0x63643030 /* "00dc" */) {
            *out_data = c->buf + c->cursor + 8;
            *out_size = sz;
            c->cursor = next;
            return 1;
        }
        if (tag == 0x62773130 /* "01wb" */ && s_audio_open && sz) {
            SDL_QueueAudio(s_audio_dev, c->buf + c->cursor + 8, sz);
            /* T38 audio sync sanity — log when the queue gets very deep
 * (>4 sec at the active spec). Indicates video is decoding
 * too slow or chunks are unevenly distributed. */
            uint32_t qbytes = SDL_GetQueuedAudioSize(s_audio_dev);
            uint32_t bps = (uint32_t)s_audio_spec_cur.freq *
                           s_audio_spec_cur.channels *
                           (SDL_AUDIO_BITSIZE(s_audio_spec_cur.format) / 8);
            if (bps > 0 && qbytes > bps * 4) {
                fprintf(stderr, "[avi-sync] audio queue %.1fs ahead (cursor=0x%X)\n",
                        (double)qbytes / (double)bps, c->cursor);
            }
        }
        /* Safety: a malformed chunk that would jump past movi_end means
 * the AVI is truncated or has a corrupt size field — treat as
 * EOF rather than reading garbage past the buffer. */
        if (next <= c->cursor || next > c->movi_end) return 0;
        c->cursor = next;
    }
    return 0;
}

static void avi_close(AviCtx *c)
{
    if (c->buf) { free(c->buf); c->buf = NULL; }
}

/* ---- FLIC frame decoder ------------------------------------------------- */
#define FLIC_FRAME_MAGIC  0xF1FA
#define CK_COLOR_64       11           /* T134 — legacy 6-bit palette */
#define CK_COLOR_256      4
#define CK_DELTA_FLC      7
#define CK_BLACK          13
#define CK_BRUN           15
#define CK_COPY           16
#define CK_PSTAMP         18

static void flic_color_256(const uint8_t *p, uint32_t sz)
{
    (void)sz;
    uint16_t packets = (uint16_t)(p[0] | (p[1] << 8));
    p += 2;
    uint16_t idx = 0;
    while (packets--) {
        idx += p[0];                       /* skip count */
        uint16_t count = p[1];              /* 0 means 256 */
        p += 2;
        uint16_t n = count ? count : 256;
        for (uint16_t i = 0; i < n && idx < 256; ++i, ++idx) {
            g_palette_rgb[idx*3 + 0] = p[0];
            g_palette_rgb[idx*3 + 1] = p[1];
            g_palette_rgb[idx*3 + 2] = p[2];
            p += 3;
        }
    }
}

/* T134 — FLIC COLOR_64 chunk (type 11). Same packet structure as
 * COLOR_256 but RGB values are 6-bit (0..63) — legacy VGA palette
 * encoding. Multiply each by 4 to get 8-bit values. */
static void flic_color_64(const uint8_t *p, uint32_t sz)
{
    (void)sz;
    uint16_t packets = (uint16_t)(p[0] | (p[1] << 8));
    p += 2;
    uint16_t idx = 0;
    while (packets--) {
        idx += p[0];
        uint16_t count = p[1];
        p += 2;
        uint16_t n = count ? count : 256;
        for (uint16_t i = 0; i < n && idx < 256; ++i, ++idx) {
            g_palette_rgb[idx*3 + 0] = (uint8_t)(p[0] << 2);   /* 6-bit → 8-bit */
            g_palette_rgb[idx*3 + 1] = (uint8_t)(p[1] << 2);
            g_palette_rgb[idx*3 + 2] = (uint8_t)(p[2] << 2);
            p += 3;
        }
    }
}

static void flic_brun(const uint8_t *p, uint32_t sz, int w, int h)
{
    (void)sz;
    /* one byte per scanline = packet count; then packets. Each packet:
 * signed byte n: n > 0 → copy n literal bytes; n < 0 → |n| run of next byte. */
    for (int y = 0; y < h; ++y) {
        uint8_t *dst = g_back_shadow + (size_t)y * w;
        ++p;                                /* packet count — unused */
        int x = 0;
        while (x < w) {
            int8_t n = (int8_t)*p++;
            if (n >= 0) {
                /* In BRUN the convention is inverted vs DELTA: n positive
 * means "n repetitions of next byte". */
                uint8_t v = *p++;
                for (int i = 0; i < n; ++i) {
                    if (x < w) dst[x++] = v;
                }
            } else {
                int cnt = -n;
                for (int i = 0; i < cnt; ++i) {
                    if (x < w) dst[x++] = *p++;
                }
            }
        }
    }
}

static void flic_delta_flc(const uint8_t *p, uint32_t sz, int w)
{
    (void)sz;
    uint16_t lines = (uint16_t)(p[0] | (p[1] << 8));
    p += 2;
    int y = 0;
    while (lines > 0) {
        uint16_t opcode = (uint16_t)(p[0] | (p[1] << 8));
        p += 2;
        if ((opcode & 0xC000) == 0xC000) {
            /* line skip: -opcode lines down */
            y += -(int16_t)opcode;
            continue;
        }
        if ((opcode & 0xC000) == 0x8000) {
            /* last byte in line */
            uint8_t *dst = g_back_shadow + (size_t)y * w + (w - 1);
            *dst = (uint8_t)(opcode & 0xFF);
            ++y; --lines;
            continue;
        }
        /* opcode = packet count */
        uint16_t packets = opcode;
        uint8_t *dst = g_back_shadow + (size_t)y * w;
        int x = 0;
        for (uint16_t pk = 0; pk < packets; ++pk) {
            x += *p++;                      /* skip */
            int8_t n = (int8_t)*p++;
            if (n >= 0) {
                /* n words (= 2*n bytes) literal */
                for (int i = 0; i < n; ++i) {
                    if (x < w) dst[x++] = *p;
                    if (x < w) dst[x++] = *(p+1);
                    p += 2;
                }
            } else {
                int cnt = -n;
                uint8_t v0 = *p++;
                uint8_t v1 = *p++;
                for (int i = 0; i < cnt; ++i) {
                    if (x < w) dst[x++] = v0;
                    if (x < w) dst[x++] = v1;
                }
            }
        }
        ++y; --lines;
    }
}

static void flic_decode_frame(const uint8_t *fdata, uint32_t fsize, int w, int h)
{
    if (fsize < 16) return;
    /* +0 dword size, +4 word magic, +6 word chunks, +8.. */
    uint16_t magic  = (uint16_t)(fdata[4] | (fdata[5] << 8));
    uint16_t chunks = (uint16_t)(fdata[6] | (fdata[7] << 8));
    if (magic != FLIC_FRAME_MAGIC) return;

    const uint8_t *p = fdata + 16;
    const uint8_t *end = fdata + fsize;
    for (uint16_t i = 0; i < chunks && p + 6 <= end; ++i) {
        uint32_t sz = rd_u32(p);
        uint16_t type = (uint16_t)(p[4] | (p[5] << 8));
        const uint8_t *body = p + 6;
        uint32_t bsz = sz - 6;
        switch (type) {
        case CK_COLOR_64:  flic_color_64 (body, bsz);    break;   /* T134 */
        case CK_COLOR_256: flic_color_256(body, bsz);    break;
        case CK_BRUN:      flic_brun(body, bsz, w, h);   break;
        case CK_DELTA_FLC: flic_delta_flc(body, bsz, w); break;
        case CK_BLACK:     memset(g_back_shadow, 0, (size_t)w * h); break;
        case CK_COPY:      memcpy(g_back_shadow, body, (size_t)w * h); break;
        case CK_PSTAMP:    break;
        default:           break;
        }
        p += sz;
    }
}

/* ---- public entry — drop-in replacement for the audio.c stub ------------ */
extern void PlatformPumpEvents(void);
extern int  PlatformShouldQuit(void);
extern uint8_t g_lmb_clicked, g_rmb_clicked;
extern uint16_t g_key_state;

int PlayFlicAviFile(const char *path)
{
    AviCtx c = {0};
    if (!avi_open(&c, path)) return 0;
    if (!g_back_shadow) {
        g_back_shadow = (uint8_t *)xmalloc(640 * 480);
        if (g_back_shadow) memset(g_back_shadow, 0, 640 * 480);
    }
    fprintf(stderr, "[avi] play %s (%dx%d, %u us/frame)\n",
            path, c.width, c.height, c.fps_us);

    uint8_t  *frame_data;
    uint32_t  frame_size;
    uint32_t  frame_us = c.fps_us ? c.fps_us : 100000;
    int       skip = 0;
    int       frame_count = 0;          /* T29 — for batch-test coverage report */
    int       skipped     = 0;          /* whether user aborted via click/key */
    while (avi_next_video(&c, &frame_data, &frame_size)) {
        uint32_t t0 = SDL_GetTicks();
        flic_decode_frame(frame_data, frame_size, c.width, c.height);
        ++frame_count;

        if (!skip) {
            FlushFrameToPrimary();
            PlatformPumpEvents();
            if (PlatformShouldQuit()) break;
            if (g_lmb_clicked || g_rmb_clicked ||
                (g_key_state & 0xFF) != 0)
            {
                /* Skip: stop audio NOW (pause device + clear queue) and
 * stop decoding further frames. This is
 * original MCI behaviour where StopAviPlayback aborts
 * both video and audio immediately. */
                g_lmb_clicked = 0;
                g_rmb_clicked = 0;
                g_key_state &= 0xFF00;
                if (s_audio_open) {
                    SDL_PauseAudioDevice(s_audio_dev, 1);
                    SDL_ClearQueuedAudio(s_audio_dev);
                    SDL_PauseAudioDevice(s_audio_dev, 0);
                }
                skipped = 1;
                break;
            }
            uint32_t elapsed_ms = SDL_GetTicks() - t0;
            uint32_t target_ms  = frame_us / 1000;
            extern int g_no_pacing;                /* T29 — batch-test bypass */
            if (!g_no_pacing && elapsed_ms < target_ms)
                SDL_Delay(target_ms - elapsed_ms);
        }
    }
    fprintf(stderr, "[avi] %s end — %d frames decoded%s\n",
            path, frame_count, skipped ? " (skipped)" : "");
    avi_close(&c);
    return 1;
}
