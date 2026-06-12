/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/flic.c — AAFLC (Autodesk FLIC inside AVI) decoder.
 *
 * The original engine drove this via MCI AVIVideo + VIDEO.DRV
 * (FLCCODEC NE DLL) on Win 9x. We ship a portable C decoder of the
 * FLIC frame format here, walking the AVI container ourselves.
 *
 * Streaming, not slurping. The cutscene AVIs are large (Dane_11 is
 * ~117 MB) and playback is strictly forward — no seek-back, no scrub.
 * So instead of malloc'ing the whole file, we keep the file open and
 * pull one chunk at a time, holding only a tiny bounded window in RAM:
 *
 *   - a 64 KB header prefix (read once, freed) to locate movi + parse
 *     stream formats;
 *   - one reusable scratch buffer for the current audio chunk;
 *   - a small ring of pending video frames (look-ahead, see below).
 *
 * Resident RAM during playback is single-digit MB regardless of file
 * size. A large stdio buffer (setvbuf) turns the per-chunk freads into
 * big sequential block reads — the ideal pattern for SD/eMMC on the
 * handheld targets.
 *
 * Audio cushion. The audio device drains in real time on its own
 * thread; if the video loop stalls (a slow DELTA frame after a palette
 * change can overrun the per-frame budget) the device must not starve.
 * We keep its FIFO topped to a fixed time cushion (AUDIO_CUSHION_MS) by
 * reading audio chunks ahead of the displayed frame, buffering the
 * video frames we race past into a bounded ring. This is the streamed
 * equivalent of the old "pre-queue the entire audio stream" trick, but
 * with a bounded window instead of the whole file.
 *
 * Frame chunk types (the only ones the Wacki AVIs use in practice):
 *   COLOR_256 (4)  palette update, 6-bit DAC scaled to 8-bit
 *   DELTA_FLC (7)  line-based delta with skip / RLE / 2-byte runs
 *   BLACK    (13)  fill the whole frame with colour 0
 *   BRUN     (15)  byte run-length (used for the first/key frame)
 *   COPY     (16)  uncompressed
 *
 * The Wacki AVIs are 640×480, 8-bit, ~10 fps, paletted. */
#include "wacki.h"
#include "wacki/log.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint8_t *g_back_shadow;
extern uint8_t  g_palette_rgb[256*3];
extern int      g_no_pacing;                /* T29 — batch-test pacing bypass */

/* T43b — AVI chunk headers aren't guaranteed 4-byte aligned (chunks
 * are byte-aligned in the container). Use memcpy to avoid UBSan
 * misaligned-load complaints (also required on strict-alignment ARM). */
static inline uint32_t rd_u32(const uint8_t *p) {
    uint32_t v; memcpy(&v, p, 4); return v;
}

static inline uint16_t rd_u16(const uint8_t *p) {
    uint16_t v; memcpy(&v, p, 2); return v;
}

/* ---- tuning knobs ------------------------------------------------------- */

/* The RIFF/AVI header (hdrl + per-stream strl lists) always precedes the
 * movi data and is at most a couple KB. We read this prefix, parse it
 * in RAM, then stream the (huge) movi body straight off disk. */
#define STREAM_BOOTSTRAP_BYTES      (64u * 1024)

/* Target depth of queued-but-unplayed audio. ~16× the mmiyoo device
 * buffer (1024 samples ≈ 46 ms) so a stalled video frame can't starve
 * the audio thread. At 10 fps this buffers ~7-8 video frames ahead. */
#define AUDIO_CUSHION_MS            750

/* Hard cap on look-ahead video frames held in RAM. Steady state is
 * ~AUDIO_CUSHION_MS / frame_ms (≈7-8 at 10 fps); 32 is generous head-
 * room and bounds worst-case ring memory regardless of interleave. */
#define VIDEO_RING_MAX              32

/* ---- file reader (storage HAL) ----------------------------------------- *
 *
 * The streaming loop issues one read per movi sub-chunk (an 8-byte header
 * then the body) — KB-sized. plat_flic_* (wacki/platform/storage.h) is a
 * read-ahead-optimized single global reader so disc latency never pauses the
 * decoder: a setvbuf'd stdio FILE on desktop/handheld, a background ring-fill
 * thread on PS2 (where a flood of tiny fileXio RPCs would also starve audsrv).
 * One cutscene plays at a time, so the reader needs no handle — FlicFp is just
 * an "open" flag, and the per-AviStream fp field tracks that. */
typedef void *FlicFp;                     /* non-NULL = open (state is global) */
static FlicFp   flic_open(const char *path) { return plat_flic_open(path) ? (void *)1 : NULL; }
static uint32_t flic_read(void *dst, uint32_t n, FlicFp f) { (void)f; return plat_flic_read(dst, n); }
static void     flic_seek(FlicFp f, int32_t off, int whence) { (void)f; plat_flic_seek(off, whence); }
static int32_t  flic_tell(FlicFp f) { (void)f; return plat_flic_tell(); }
static void     flic_close(FlicFp f) { (void)f; plat_flic_close(); }

/* ---- AVI streaming context ---------------------------------------------- */

typedef struct { uint8_t *data; uint32_t size; } VidFrame;

typedef struct {
    FlicFp    fp;
    uint32_t  pos;               /* current read offset (mirrors ftell) */
    uint32_t  movi_end;          /* file offset where movi data ends */
    int       eof;

    uint16_t  width;
    uint16_t  height;
    uint32_t  fps_us;            /* microseconds per frame */

    /* audio stream info — populated from the second strl LIST if present */
    uint16_t  audio_channels;
    uint16_t  audio_bits;
    uint32_t  audio_samples_per_sec;
    uint32_t  cushion_bytes;     /* AUDIO_CUSHION_MS in device-rate bytes */

    /* reusable scratch for the current audio chunk */
    uint8_t  *ascratch;
    uint32_t  ascratch_cap;

    /* bounded look-ahead ring of pending (undecoded) video frames */
    VidFrame  ring[VIDEO_RING_MAX];
    int       r_head, r_tail, r_count;
} AviStream;

/* SDL audio device, opened on demand and reused across cutscenes.
 * s_audio_spec_cur is the *obtained* spec — i.e. the device's real
 * hardware rate/format, which on Windows/WASAPI is usually NOT the
 * AVI's rate. */
static SDL_AudioDeviceID s_audio_dev = 0;
static SDL_AudioSpec     s_audio_spec_cur;
static int               s_audio_open = 0;

/* The AVI's source PCM spec the device was last opened for. The device
 * may negotiate something else (hardware mix rate / mono), so the reuse
 * check keys off the source — not the obtained spec. */
static int             s_audio_src_freq  = 0;
static uint16_t        s_audio_src_chans = 0;
static SDL_AudioFormat s_audio_src_fmt   = 0;

/* Explicit resampler: source PCM → obtained device spec. Built only when
 * the two differ. We queue its output, which is already at the device's
 * rate, so SDL never has to convert at queue time. See audio_ensure for
 * why we don't lean on SDL's implicit queue-time conversion. NULL when
 * the device opened at exactly the source spec (no conversion needed). */
static SDL_AudioStream *s_audio_cvt     = NULL;
static uint8_t         *s_cvt_out       = NULL;
static int              s_cvt_out_cap   = 0;

static void audio_cvt_free(void)
{
    if (s_audio_cvt) { SDL_FreeAudioStream(s_audio_cvt); s_audio_cvt = NULL; }
    free(s_cvt_out);
    s_cvt_out = NULL;
    s_cvt_out_cap = 0;
}

#ifdef WACKI_PS2
/* PS2 has no SDL audio device — cutscene PCM is fed to audsrv directly
 * (platform_ps2.c), with the mixer feed thread parked for the duration. */
extern void platform_ps2_avi_audio_begin(int rate, int channels, int bits);
extern void platform_ps2_avi_audio_push(const void *buf, int len);
extern void platform_ps2_avi_audio_end(void);
static int  s_ps2_avi_audio = 0;
#endif

static void audio_ensure(uint32_t sample_rate, uint16_t channels, uint16_t bits)
{
#ifdef WACKI_PS2
    /* Hand audsrv to the cutscene at its native PCM format; the audio
     * chunks are streamed straight to it in stream_pump_one(). */
    platform_ps2_avi_audio_begin((int)sample_rate, (int)channels, (int)bits);
    s_ps2_avi_audio = 1;
    return;
#endif
    SDL_AudioFormat src_fmt = (bits == 8) ? AUDIO_U8 : AUDIO_S16LSB;

    /* Reuse the open device when the SOURCE spec is unchanged. */
    if (s_audio_open &&
        s_audio_src_freq  == (int)sample_rate &&
        s_audio_src_chans == channels &&
        s_audio_src_fmt   == src_fmt)
        return;

    if (s_audio_open) { SDL_CloseAudioDevice(s_audio_dev); s_audio_open = 0; }
    audio_cvt_free();

    /* mmiyoo holds a single audio device slot — close the SFX/music
     * mixer if it's up so SDL_OpenAudioDevice below doesn't bounce
     * off "Audio device already open". The mixer re-opens lazily on
     * the first SFX/music play after audio_release. */
    extern void mixer_release(void);
    mixer_release();

    SDL_AudioSpec want = {0};
    want.freq     = (int)sample_rate;
    want.format   = src_fmt;
    want.channels = (Uint8)channels;
    /* 4096-frame buffer (~185 ms at 22 kHz) keeps the device fed
     * between AVI audio chunks, which arrive every video frame —
     * intro Dane_10.dta runs at 10 fps = 100 ms per chunk. With the
     * old 1024 sample (~46 ms) buffer the device underruns midway
     * through every gap → audible chopping. Larger is safer on the
     * Miyoo/mmiyoo backend which can't switch buffers on the fly. */
    want.samples  = 4096;
    /* Let the device pick its own rate/channels/buffer; we adapt to
     * whatever it returns. Windows/WASAPI shared mode can only run at
     * the system mix rate (44.1/48 kHz), never the AVI's 22 kHz — and
     * SDL's implicit conversion of *queued* audio doesn't kick in on
     * that backend (the callback-based mixer in audio.c converts fine,
     * but the SDL_QueueAudio path here played our 22 kHz bytes ~2x too
     * fast: "sped up, then silence" on a loop). So we resample
     * explicitly below into the obtained spec instead of trusting SDL
     * to do it for us. */
    s_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &s_audio_spec_cur,
                                      SDL_AUDIO_ALLOW_FREQUENCY_CHANGE |
                                      SDL_AUDIO_ALLOW_CHANNELS_CHANGE |
                                      SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
    if (!s_audio_dev) {
        LOG_INFO("audio", "SDL_OpenAudioDevice (avi): %s", SDL_GetError());
        return;
    }
    s_audio_open      = 1;
    s_audio_src_freq  = (int)sample_rate;
    s_audio_src_chans = channels;
    s_audio_src_fmt   = src_fmt;

    /* Build the resampler whenever the device didn't hand us back the
     * exact source spec. This also subsumes the old stereo→mono downmix
     * (mmiyoo): the stream folds channels for us. */
    if (s_audio_spec_cur.freq != (int)sample_rate ||
        s_audio_spec_cur.channels != channels ||
        s_audio_spec_cur.format != src_fmt) {
        s_audio_cvt = SDL_NewAudioStream(src_fmt, (Uint8)channels, (int)sample_rate,
                                         s_audio_spec_cur.format,
                                         s_audio_spec_cur.channels,
                                         s_audio_spec_cur.freq);
        if (!s_audio_cvt)
            LOG_INFO("audio", "SDL_NewAudioStream failed: %s", SDL_GetError());
    }

    SDL_PauseAudioDevice(s_audio_dev, 0);
    LOG_INFO("audio", "AVI audio: src %u Hz %u ch %u-bit -> device %d Hz %d ch %d samples%s",
             sample_rate, channels, bits,
             s_audio_spec_cur.freq, s_audio_spec_cur.channels,
             s_audio_spec_cur.samples, s_audio_cvt ? " [resampling]" : "");
#ifdef WACKI_MIYOO
    /* mmiyoo resets kernel mixer on each SDL_OpenAudioDevice — re-apply
     * the OnionOS-saved volume so the AVI doesn't blast at max. */
    extern void platform_restore_system_volume(void);
    platform_restore_system_volume();
#endif
}

/* Push one AVI audio chunk to the device, resampling through s_audio_cvt
 * first when the device rate/format differs from the source. */
static void audio_queue_chunk(const uint8_t *data, uint32_t sz)
{
    if (!s_audio_cvt) {                       /* device matches source */
        SDL_QueueAudio(s_audio_dev, data, sz);
        return;
    }
    if (SDL_AudioStreamPut(s_audio_cvt, data, (int)sz) < 0)
        return;
    for (;;) {
        int avail = SDL_AudioStreamAvailable(s_audio_cvt);
        if (avail <= 0) break;
        if (avail > s_cvt_out_cap) {
            uint8_t *n = (uint8_t *)realloc(s_cvt_out, (size_t)avail);
            if (!n) return;                   /* OOM — drop the tail */
            s_cvt_out     = n;
            s_cvt_out_cap = avail;
        }
        int got = SDL_AudioStreamGet(s_audio_cvt, s_cvt_out, avail);
        if (got <= 0) break;
        SDL_QueueAudio(s_audio_dev, s_cvt_out, (Uint32)got);
    }
}

/* Release the AVI audio device. Called at end of playback so the
 * mixer (audio.c) can take the single mmiyoo hardware slot. On a
 * desktop with proper multi-device SDL this is just hygiene. */
static void audio_release(void)
{
#ifdef WACKI_PS2
    if (s_ps2_avi_audio) { platform_ps2_avi_audio_end(); s_ps2_avi_audio = 0; }
    return;
#endif
    audio_cvt_free();
    if (!s_audio_open) return;
    SDL_CloseAudioDevice(s_audio_dev);
    s_audio_open      = 0;
    s_audio_dev       = 0;
    s_audio_src_freq  = 0;
    s_audio_src_chans = 0;
    s_audio_src_fmt   = 0;
}

/* ---- RIFF / AVI FourCCs ------------------------------------------- */

/* Four-character codes packed little-endian for byte-wise comparison
 * with rd_u32 reads. Spelt out in the macro name so callers don't need
 * to decode hex back into ASCII. */
#define FOURCC_LIST                     0x5453494Cu   /* "LIST" */
#define FOURCC_HDRL                     0x6C726468u   /* "hdrl" */
#define FOURCC_STRL                     0x6C727473u   /* "strl" */
#define FOURCC_STRH                     0x68727473u   /* "strh" */
#define FOURCC_STRF                     0x66727473u   /* "strf" */
#define FOURCC_MOVI                     0x69766F6Du   /* "movi" */
#define FOURCC_AUDS                     0x73647561u   /* "auds" */
#define FOURCC_00DC                     0x63643030u   /* "00dc" video chunk */
#define FOURCC_01WB                     0x62773130u   /* "01wb" audio chunk */

/* RIFF chunk header is 8 bytes: 4-byte FourCC + 4-byte size. LIST
 * chunks add a 4-byte type, so list bodies start at +12. */
#define RIFF_CHUNK_HEADER_BYTES         8
#define RIFF_LIST_BODY_OFFSET           12

/* Magic + "AVI " marker at fixed RIFF offsets. */
#define AVI_FILE_HEADER_BYTES           12   /* "RIFF" + size + "AVI " */
#define AVI_FORMAT_TYPE_OFFSET          8

/* avih fields (offsets into the avih chunk's data after the 8-byte
 * chunk header). The original AVIMAINHEADER struct has fps_us at byte
 * 0, total_frames + flags etc., then width@32 / height@36 inside the
 * data area — which is +8+32 and +8+36 from the strh-style "tag+size+
 * data" arrangement. We keep the historical (+8+40-8) and (+8+44-8)
 * expression below as numeric constants for clarity. */
#define AVIH_DATA_FPS_US_OFFSET         8
#define AVIH_DATA_WIDTH_OFFSET          40   /* = 8 (header) + 32 (width) */
#define AVIH_DATA_HEIGHT_OFFSET         44   /* = 8 (header) + 36 (height) */

/* WAVEFORMATEX field offsets inside the strf chunk's data area (the
 * data area starts after the 8-byte chunk header). */
#define WFE_OFF_CHANNELS                2
#define WFE_OFF_SAMPLES_PER_SEC         4
#define WFE_OFF_BITS_PER_SAMPLE         14

/* RIFF chunks are word-aligned: each chunk advances by 8 + size + pad,
 * where pad = (size & 1). */
static inline uint32_t riff_chunk_advance(uint32_t sz)
{
    return RIFF_CHUNK_HEADER_BYTES + sz + (sz & 1);
}

/* ---- video look-ahead ring ---------------------------------------------- */

static int ring_push(AviStream *s, uint8_t *data, uint32_t size)
{
    if (s->r_count >= VIDEO_RING_MAX) return 0;
    s->ring[s->r_tail].data = data;
    s->ring[s->r_tail].size = size;
    s->r_tail = (s->r_tail + 1) % VIDEO_RING_MAX;
    ++s->r_count;
    return 1;
}

static int ring_pop(AviStream *s, VidFrame *out)
{
    if (s->r_count == 0) return 0;
    *out = s->ring[s->r_head];
    s->r_head = (s->r_head + 1) % VIDEO_RING_MAX;
    --s->r_count;
    return 1;
}

static void ring_clear(AviStream *s)
{
    VidFrame f;
    while (ring_pop(s, &f)) free(f.data);
}

static int ascratch_ensure(AviStream *s, uint32_t need)
{
    if (s->ascratch_cap >= need) return 1;
    uint8_t *n = (uint8_t *)realloc(s->ascratch, need);
    if (!n) return 0;
    s->ascratch     = n;
    s->ascratch_cap = need;
    return 1;
}

/* ---- header parse (runs on the in-RAM bootstrap prefix) ----------------- */

/* Parse the avih chunk inside an hdrl LIST. q is the chunk-header offset
 * ("avih" + size + data) into buf. */
static void parse_avih(AviStream *s, const uint8_t *buf, uint32_t bufsz, uint32_t q)
{
    if ((uint64_t)q + AVIH_DATA_HEIGHT_OFFSET + 4 > bufsz) return;
    if (memcmp(buf + q, "avih", 4) != 0) return;
    s->fps_us = rd_u32(buf + q + AVIH_DATA_FPS_US_OFFSET);
    s->width  = (uint16_t)rd_u32(buf + q + AVIH_DATA_WIDTH_OFFSET);
    s->height = (uint16_t)rd_u32(buf + q + AVIH_DATA_HEIGHT_OFFSET);
}

/* Parse one strl LIST: walk strh (decide audio vs video) + strf
 * (WAVEFORMATEX for audio streams). q points at the first sub-chunk. */
static void parse_strl(AviStream *s, const uint8_t *buf, uint32_t bufsz,
                       uint32_t q, uint32_t end)
{
    if (end > bufsz) end = bufsz;
    int is_audio = 0;
    while (q + RIFF_CHUNK_HEADER_BYTES <= end) {
        uint32_t t2 = rd_u32(buf + q);
        uint32_t s2 = rd_u32(buf + q + 4);
        if (t2 == FOURCC_STRH) {
            if (q + RIFF_CHUNK_HEADER_BYTES + 4 <= end)
                is_audio = (rd_u32(buf + q + RIFF_CHUNK_HEADER_BYTES) == FOURCC_AUDS);
        } else if (t2 == FOURCC_STRF && is_audio) {
            const uint8_t *wfe = buf + q + RIFF_CHUNK_HEADER_BYTES;
            if (q + RIFF_CHUNK_HEADER_BYTES + WFE_OFF_BITS_PER_SAMPLE + 2 <= end) {
                s->audio_channels        = rd_u16(wfe + WFE_OFF_CHANNELS);
                s->audio_samples_per_sec = rd_u32(wfe + WFE_OFF_SAMPLES_PER_SEC);
                s->audio_bits            = rd_u16(wfe + WFE_OFF_BITS_PER_SAMPLE);
            }
        }
        q += riff_chunk_advance(s2);
    }
}

/* Walk the top-level RIFF structure in the bootstrap prefix to extract
 * dimensions/fps (hdrl/avih), audio format (strl), and the movi data
 * range. movi_start/movi_end are FILE offsets (the prefix starts at
 * file offset 0). Returns 1 once movi is located, 0 on failure. */
static int avi_parse_header(AviStream *s, const uint8_t *buf, uint32_t bufsz,
                            uint32_t filesz)
{
    if (bufsz < AVI_FILE_HEADER_BYTES) return 0;
    if (memcmp(buf, "RIFF", 4) != 0 ||
        memcmp(buf + AVI_FORMAT_TYPE_OFFSET, "AVI ", 4) != 0)
        return 0;

    uint32_t p = AVI_FILE_HEADER_BYTES;
    while (p + RIFF_CHUNK_HEADER_BYTES <= bufsz) {
        uint32_t tag = rd_u32(buf + p);
        uint32_t sz  = rd_u32(buf + p + 4);

        if (tag != FOURCC_LIST) {
            p += riff_chunk_advance(sz);
            continue;
        }
        if (p + RIFF_LIST_BODY_OFFSET > bufsz) break;

        uint32_t fourcc = rd_u32(buf + p + RIFF_CHUNK_HEADER_BYTES);
        if (fourcc == FOURCC_HDRL) {
            parse_avih(s, buf, bufsz, p + RIFF_LIST_BODY_OFFSET);
            p += RIFF_LIST_BODY_OFFSET;   /* descend into hdrl body */
            continue;
        }
        if (fourcc == FOURCC_STRL) {
            parse_strl(s, buf, bufsz, p + RIFF_LIST_BODY_OFFSET,
                       p + RIFF_CHUNK_HEADER_BYTES + sz);
            p += riff_chunk_advance(sz);
            continue;
        }
        if (fourcc == FOURCC_MOVI) {
            uint32_t movi_start = p + RIFF_LIST_BODY_OFFSET;
            uint32_t movi_end   = p + RIFF_CHUNK_HEADER_BYTES + sz;
            if (movi_end > filesz || movi_end < movi_start) movi_end = filesz;
            s->movi_end = movi_end;
            s->pos      = movi_start;
            return 1;
        }
        p += riff_chunk_advance(sz);
    }
    return 0;
}

/* ---- open / stream / close ---------------------------------------------- */

static int avi_open_stream(AviStream *s, const char *path)
{
    s->fp = flic_open(path);
    if (!s->fp) return 0;

    flic_seek(s->fp, 0, SEEK_END);
    int32_t fsz_l = flic_tell(s->fp);
    flic_seek(s->fp, 0, SEEK_SET);
    if (fsz_l <= 0) { flic_close(s->fp); s->fp = NULL; return 0; }
    uint32_t filesz = (uint32_t)fsz_l;

    /* Read the header prefix and parse it in RAM, then free it. */
    uint32_t boot = filesz < STREAM_BOOTSTRAP_BYTES ? filesz : STREAM_BOOTSTRAP_BYTES;
    uint8_t *hdr = (uint8_t *)malloc(boot);
    if (!hdr) { flic_close(s->fp); s->fp = NULL; return 0; }
    int ok = (flic_read(hdr, boot, s->fp) == boot) &&
             avi_parse_header(s, hdr, boot, filesz);
    free(hdr);
    if (!ok) { flic_close(s->fp); s->fp = NULL; return 0; }

    /* Open the audio device now — but DON'T prequeue. Audio is streamed
     * in the playback loop and kept ahead of video by the cushion. */
    if (s->audio_samples_per_sec) {
        audio_ensure(s->audio_samples_per_sec, s->audio_channels, s->audio_bits);
        if (s_audio_open) {
            /* Cushion is measured against the device FIFO, so size it in
             * the obtained (device) rate/format — that's also what we
             * queue, since audio_queue_chunk resamples to it. */
            int bytes_per_sample = SDL_AUDIO_BITSIZE(s_audio_spec_cur.format) / 8;
            uint32_t bps = (uint32_t)s_audio_spec_cur.freq *
                           s_audio_spec_cur.channels * (uint32_t)bytes_per_sample;
            s->cushion_bytes = bps * AUDIO_CUSHION_MS / 1000;
        }
    }

    flic_seek(s->fp, (int32_t)s->pos, SEEK_SET);   /* to first movi chunk */
    return 1;
}

/* Read exactly one movi chunk: queue audio into the device FIFO, push
 * video into the look-ahead ring, skip anything else. Returns 1 if a
 * chunk was consumed (call again to advance), 0 at end-of-movi / on a
 * short or corrupt read. */
static int stream_pump_one(AviStream *s)
{
    if (s->eof) return 0;
    if (s->pos + RIFF_CHUNK_HEADER_BYTES > s->movi_end) { s->eof = 1; return 0; }

    uint8_t hdr[RIFF_CHUNK_HEADER_BYTES];
    if (flic_read(hdr, RIFF_CHUNK_HEADER_BYTES, s->fp) != RIFF_CHUNK_HEADER_BYTES) {
        s->eof = 1; return 0;
    }
    uint32_t tag = rd_u32(hdr);
    uint32_t sz  = rd_u32(hdr + 4);
    uint32_t pad = (sz & 1);
    uint32_t next = s->pos + RIFF_CHUNK_HEADER_BYTES + sz + pad;
    /* Truncated / corrupt size field — stop rather than read past movi. */
    if (next <= s->pos || next > s->movi_end) { s->eof = 1; return 0; }

    if (tag == FOURCC_00DC) {
        uint8_t *data = (uint8_t *)malloc(sz ? sz : 1);
        if (!data) {                                   /* OOM — drop frame */
            flic_seek(s->fp, (int32_t)(sz + pad), SEEK_CUR);
            s->pos = next;
            return 1;
        }
        if (sz && flic_read(data, sz, s->fp) != sz) { free(data); s->eof = 1; return 0; }
        if (pad) flic_seek(s->fp, 1, SEEK_CUR);
        if (!ring_push(s, data, sz)) free(data);       /* ring full guard */
        s->pos = next;
        return 1;
    }

    int audio_wanted = s_audio_open;
#ifdef WACKI_PS2
    audio_wanted = s_ps2_avi_audio;
#endif
    if (tag == FOURCC_01WB && sz && audio_wanted) {
        if (!ascratch_ensure(s, sz)) {                 /* OOM — drop audio */
            flic_seek(s->fp, (int32_t)(sz + pad), SEEK_CUR);
            s->pos = next;
            return 1;
        }
        if (flic_read(s->ascratch, sz, s->fp) != sz) { s->eof = 1; return 0; }
        if (pad) flic_seek(s->fp, 1, SEEK_CUR);
#ifdef WACKI_PS2
        /* Push the cutscene's PCM to the audio thread's ring (converted to
         * 22050/16/stereo there); the thread plays it independent of decode. */
        platform_ps2_avi_audio_push(s->ascratch, (int)sz);
        s->pos = next;
        return 1;
#endif
        audio_queue_chunk(s->ascratch, sz);    /* resamples + downmixes if needed */
        s->pos = next;
        return 1;
    }

    /* Audio with no device, or any other chunk type — skip the body. */
    flic_seek(s->fp, (int32_t)(sz + pad), SEEK_CUR);
    s->pos = next;
    return 1;
}

static void avi_close(AviStream *s)
{
    ring_clear(s);
    free(s->ascratch);
    s->ascratch = NULL;
    if (s->fp) { flic_close(s->fp); s->fp = NULL; }
}

/* Is the audio device's FIFO below the cushion target? Always false when
 * there's no audio stream (so the read-ahead loop becomes a no-op). */
static int cushion_low(const AviStream *s)
{
#ifdef WACKI_PS2
    /* No SDL FIFO to pre-fill — audsrv's ring is tiny (~53 ms) and the
     * direct feed in stream_pump_one() self-paces (blocks until room), so
     * the read-ahead loop is a no-op. The main loop pumps audio as it
     * walks to each video frame. */
    (void)s;
    return 0;
#else
    return s_audio_open && SDL_GetQueuedAudioSize(s_audio_dev) < s->cushion_bytes;
#endif
}

extern void flic_decode_frame(const uint8_t *fdata, uint32_t fsize, int w, int h);

/* ---- public entry — drop-in replacement for the audio.c stub ------------ */
extern void PlatformPumpEvents(void);
extern int  PlatformShouldQuit(void);
extern uint8_t g_lmb_clicked, g_rmb_clicked;
extern uint16_t g_key_state;

int PlayFlicAviFile(const char *path)
{
    AviStream s = {0};
    if (!avi_open_stream(&s, path)) return 0;
    if (!g_back_shadow) {
        g_back_shadow = (uint8_t *)xmalloc(640 * 480);
        if (g_back_shadow) memset(g_back_shadow, 0, 640 * 480);
    }
    LOG_TRACE("avi", "play %s (%dx%d, %u us/frame)", path, s.width, s.height, s.fps_us);

    uint32_t frame_us    = s.fps_us ? s.fps_us : 100000;
    int      frame_count = 0;          /* T29 — batch-test coverage report */
    int      skipped     = 0;          /* user aborted via click/key */

    for (;;) {
        /* 1. Top the audio FIFO to the cushion, buffering any video
         *    frames raced past into the ring. No-op without audio. */
        while (!s.eof && cushion_low(&s) && s.r_count < VIDEO_RING_MAX)
            stream_pump_one(&s);

        /* 2. Make sure a video frame is ready; queue audio met en route. */
        while (!s.eof && s.r_count == 0)
            stream_pump_one(&s);

        VidFrame vf;
        if (!ring_pop(&s, &vf)) break;     /* EOF — nothing left to show */

        uint32_t t0 = SDL_GetTicks();
        flic_decode_frame(vf.data, vf.size, s.width, s.height);
        free(vf.data);
        ++frame_count;

        FlushFrameToPrimary();
        PlatformPumpEvents();
        if (PlatformShouldQuit()) break;
        if (g_lmb_clicked || g_rmb_clicked || (g_key_state & 0xFF) != 0) {
            /* Skip: stop audio NOW (pause + clear queue) and stop
             * decoding. Matches the original MCI StopAviPlayback semantic
             * where the abort tears down both video and audio together. */
            g_lmb_clicked = 0;
            g_rmb_clicked = 0;
            g_key_state &= 0xFF00;
            if (s_audio_open) {
                SDL_PauseAudioDevice(s_audio_dev, 1);
                SDL_ClearQueuedAudio(s_audio_dev);
                if (s_audio_cvt) SDL_AudioStreamClear(s_audio_cvt);
                SDL_PauseAudioDevice(s_audio_dev, 0);
            }
            skipped = 1;
            break;
        }

        if (!g_no_pacing) {
            uint32_t target_ms = frame_us / 1000;
#ifdef WACKI_PS2
            /* audsrv's ring is only ~53 ms — smaller than one frame
             * interval — so if we just SDL_Delay() here it drains between
             * frames and SPU2 repeats its last buffer (looping sample,
             * which then stutters the video as the next feed blocks). Spend
             * the wait PUMPING instead: each audio chunk is fed to audsrv
             * (the feed self-paces by blocking until the ring has room), and
             * a few video frames buffer in the ring. The look-ahead cap
             * keeps audio from running far ahead of the picture. */
            while (SDL_GetTicks() - t0 < target_ms) {
                if (!s.eof && s.r_count < 3) stream_pump_one(&s);
                else SDL_Delay(1);
            }
#else
            uint32_t elapsed_ms = SDL_GetTicks() - t0;
            if (elapsed_ms < target_ms)
                SDL_Delay(target_ms - elapsed_ms);
#endif
        }
    }

    LOG_TRACE("avi", "%s end — %d frames decoded%s", path, frame_count,
              skipped ? " (skipped)" : "");
    avi_close(&s);
    audio_release();
    return 1;
}
