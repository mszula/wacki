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
#include "wacki/platform/audio.h"   /* plat_avi_audio_* cutscene audio device */
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

    /* audio stream info — populated from the second strl LIST if present,
     * passed to plat_avi_audio_begin (the device handles its own downmix /
     * cushion sizing). */
    uint16_t  audio_channels;
    uint16_t  audio_bits;
    uint32_t  audio_samples_per_sec;

    /* reusable scratch for the current audio chunk */
    uint8_t  *ascratch;
    uint32_t  ascratch_cap;

    /* bounded look-ahead ring of pending (undecoded) video frames */
    VidFrame  ring[VIDEO_RING_MAX];
    int       r_head, r_tail, r_count;
} AviStream;

/* Cutscene audio is a separate PUSH device behind the audio HAL
 * (wacki/platform/audio.h): plat_avi_audio_begin / _push / _end plus the
 * _below_cushion / _flush / _needs_pump queries. The SDL queue-mode device
 * (incl. the mmiyoo single-slot juggling + stereo→mono downmix) lives in
 * src/platform/sdl/audio_sdl.c; the audsrv feeder in src/platform_ps2.c. The
 * decoder below just opens it, pushes chunks, and closes it. */

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

    /* Open the cutscene audio device now — but DON'T prequeue. Audio is
     * streamed in the playback loop and kept ahead of video by the cushion.
     * The device handles its own downmix + cushion sizing internally. */
    if (s->audio_samples_per_sec)
        plat_avi_audio_begin((int)s->audio_samples_per_sec,
                             (int)s->audio_channels, (int)s->audio_bits);

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

    if (tag == FOURCC_01WB && sz && plat_avi_audio_is_open()) {
        if (!ascratch_ensure(s, sz)) {                 /* OOM — drop audio */
            flic_seek(s->fp, (int32_t)(sz + pad), SEEK_CUR);
            s->pos = next;
            return 1;
        }
        if (flic_read(s->ascratch, sz, s->fp) != sz) { s->eof = 1; return 0; }
        if (pad) flic_seek(s->fp, 1, SEEK_CUR);
        /* Push the chunk to the cutscene audio device; the backend converts
         * for its own output (SDL folds stereo→mono if it negotiated mono;
         * PS2 routes to its 22050/16/stereo audsrv ring). */
        plat_avi_audio_push(s->ascratch, (int)sz);
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

/* Is the cutscene audio device's FIFO below the cushion target? The device
 * backend answers (0 when there's no audio stream, or where it self-paces on
 * a tiny ring — PS2 audsrv — so the read-ahead loop becomes a no-op). */
static int cushion_low(const AviStream *s)
{
    (void)s;
    return plat_avi_audio_below_cushion(AUDIO_CUSHION_MS);
}

extern void flic_decode_frame(const uint8_t *fdata, uint32_t fsize, int w, int h);

/* ---- public entry — drop-in replacement for the audio.c stub ------------ */
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
            /* Skip: stop audio NOW and stop decoding. Matches the original
             * MCI StopAviPlayback semantic where the abort tears down both
             * video and audio together. */
            g_lmb_clicked = 0;
            g_rmb_clicked = 0;
            g_key_state &= 0xFF00;
            plat_avi_audio_flush();
            skipped = 1;
            break;
        }

        if (!g_no_pacing) {
            uint32_t target_ms = frame_us / 1000;
            /* Where the audio device can't buffer a whole frame interval
             * (PS2 audsrv's ring is ~53 ms), spend the inter-frame wait
             * PUMPING — each chunk is fed to the device (which self-paces by
             * blocking until its ring has room) and a few video frames buffer
             * ahead (cap keeps audio from outrunning the picture). Otherwise
             * the device FIFO already holds the cushion, so just sleep. */
            if (plat_avi_audio_needs_pump()) {
                while (SDL_GetTicks() - t0 < target_ms) {
                    if (!s.eof && s.r_count < 3) stream_pump_one(&s);
                    else SDL_Delay(1);
                }
            } else {
                uint32_t elapsed_ms = SDL_GetTicks() - t0;
                if (elapsed_ms < target_ms)
                    SDL_Delay(target_ms - elapsed_ms);
            }
        }
    }

    LOG_TRACE("avi", "%s end — %d frames decoded%s", path, frame_count,
              skipped ? " (skipped)" : "");
    avi_close(&s);
    plat_avi_audio_end();
    return 1;
}
