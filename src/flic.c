/* src/flic.c — AAFLC (Autodesk FLIC inside AVI) decoder.
 *
 * The original engine drove this via MCI AVIVideo + VIDEO.DRV
 * (FLCCODEC NE DLL) on Win 9x. We ship a portable C decoder of the
 * FLIC frame format here, walking the AVI container ourselves.
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

    /* mmiyoo holds a single audio device slot — close the SFX/music
     * mixer if it's up so SDL_OpenAudioDevice below doesn't bounce
     * off "Audio device already open". The mixer re-opens lazily on
     * the first SFX/music play after audio_release. */
    extern void mixer_release(void);
    mixer_release();

    SDL_AudioSpec want = {0};
    want.freq     = (int)sample_rate;
    want.format   = (bits == 8) ? AUDIO_U8 : AUDIO_S16LSB;
    want.channels = (Uint8)channels;
    /* 4096-frame buffer (~185 ms at 22 kHz) keeps the device fed
     * between AVI audio chunks, which arrive every video frame —
     * intro Dane_10.dta runs at 10 fps = 100 ms per chunk. With the
     * old 1024 sample (~46 ms) buffer the device underruns midway
     * through every gap → audible chopping. Larger is safer on the
     * Miyoo/mmiyoo backend which can't switch buffers on the fly. */
    want.samples  = 4096;
    /* SDL_AUDIO_ALLOW_CHANNELS_CHANGE + SAMPLES_CHANGE — needed for
     * mmiyoo which can only do mono and 1024-sample buffers. Without
     * the channel flag stereo AVIs fail to open entirely; without the
     * sample flag mmiyoo silently clamps and the audio thread fires
     * twice as often as the calling code expects. */
    s_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &s_audio_spec_cur,
                                      SDL_AUDIO_ALLOW_FREQUENCY_CHANGE |
                                      SDL_AUDIO_ALLOW_CHANNELS_CHANGE |
                                      SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
    if (!s_audio_dev) {
        LOG_INFO("audio", "SDL_OpenAudioDevice (avi): %s", SDL_GetError());
        return;
    }
    s_audio_open = 1;
    SDL_PauseAudioDevice(s_audio_dev, 0);
    LOG_INFO("audio", "AVI audio: %d Hz, %d ch, %d samples",
             s_audio_spec_cur.freq, s_audio_spec_cur.channels,
             s_audio_spec_cur.samples);
#ifdef WACKI_HANDHELD
    /* mmiyoo resets kernel mixer on each SDL_OpenAudioDevice — re-apply
     * the OnionOS-saved volume so the AVI doesn't blast at max. */
    extern void platform_restore_system_volume(void);
    platform_restore_system_volume();
#endif
}

/* Release the AVI audio device. Called at end of playback so the
 * mixer (audio.c) can take the single mmiyoo hardware slot. On a
 * desktop with proper multi-device SDL this is just hygiene. */
static void audio_release(void)
{
    if (!s_audio_open) return;
    SDL_CloseAudioDevice(s_audio_dev);
    s_audio_open = 0;
    s_audio_dev = 0;
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

/* Parse the avih chunk inside an hdrl LIST. q points at the chunk
 * header ("avih" + size + data). */
static void parse_avih(AviCtx *c, uint32_t q)
{
    if (memcmp(c->buf + q, "avih", 4) != 0) return;
    c->fps_us = rd_u32(c->buf + q + AVIH_DATA_FPS_US_OFFSET);
    c->width  = (uint16_t)rd_u32(c->buf + q + AVIH_DATA_WIDTH_OFFSET);
    c->height = (uint16_t)rd_u32(c->buf + q + AVIH_DATA_HEIGHT_OFFSET);
}

/* Pre-queue every "01wb" audio chunk in the movi range so SDL's FIFO
 * holds the entire stream before the first video frame draws. On the
 * mmiyoo backend the device buffer is forced to 1024 samples (~46 ms
 * at 22 kHz mono) while video chunks arrive every ~100 ms. Drip-feeding
 * audio one chunk per video frame in that environment guarantees
 * underrun the moment one decode + present pass overruns the 100 ms
 * budget — and FLIC delta frames after a palette change can easily do
 * that. Pre-queueing pushes all bytes upfront; the audio thread drains
 * them at the device rate independent of how slowly the video loop
 * runs.
 *
 * The AVI is short (intro Dane_10.dta is ~30 s) so the queue stays
 * under a few MB. SDL_QueueAudio is essentially memcpy + a linked-list
 * head update, so the cost is one-time at AVI open.
 *
 * If the source is stereo S16 but the device negotiated mono (mmiyoo
 * passes SDL_AUDIO_ALLOW_CHANNELS_CHANGE because it can only output
 * mono) we downmix L+R → (L+R)/2 in place before queuing. Without
 * this the device interprets stereo bytes as a mono stream and plays
 * the audio at double speed with alternating channels — sounds like
 * garbled fast audio with chop artefacts at every chunk boundary.
 * The in-place edit touches only bytes inside the chunk's data area;
 * the RIFF chunk header (and therefore avi_next_video's cursor walk)
 * is unaffected. */
static void prequeue_all_audio(AviCtx *c)
{
    if (!s_audio_open) return;
    int need_downmix = (c->audio_channels == 2 &&
                        s_audio_spec_cur.channels == 1 &&
                        c->audio_bits == 16);
    uint32_t cur = c->movi_start;
    uint32_t total_pushed = 0, chunks = 0;
    while (cur + RIFF_CHUNK_HEADER_BYTES <= c->movi_end) {
        uint32_t tag = rd_u32(c->buf + cur);
        uint32_t sz  = rd_u32(c->buf + cur + 4);
        uint32_t next = cur + RIFF_CHUNK_HEADER_BYTES + sz + (sz & 1);
        if (tag == 0x62773130 /* "01wb" */ && sz) {
            uint8_t *src = c->buf + cur + RIFF_CHUNK_HEADER_BYTES;
            if (need_downmix && (sz & 3) == 0) {
                int16_t *s = (int16_t *)src;
                uint32_t frames = sz / 4;
                for (uint32_t i = 0; i < frames; ++i) {
                    int v = (int)s[i * 2] + (int)s[i * 2 + 1];
                    s[i] = (int16_t)(v / 2);
                }
                SDL_QueueAudio(s_audio_dev, src, frames * 2);
                total_pushed += frames * 2;
            } else {
                SDL_QueueAudio(s_audio_dev, src, sz);
                total_pushed += sz;
            }
            ++chunks;
        }
        if (next <= cur || next > c->movi_end) break;
        cur = next;
    }
    LOG_INFO("avi", "pre-queued audio: %u chunks, %u bytes (downmix=%d)",
             chunks, total_pushed, need_downmix);
}

/* Parse one strl LIST: walk strh (decide audio vs video) + strf
 * (WAVEFORMATEX for audio streams). q points just past the LIST
 * header (= first sub-chunk). */
static void parse_strl(AviCtx *c, uint32_t q, uint32_t end)
{
    int is_audio = 0;
    while (q + RIFF_CHUNK_HEADER_BYTES <= end) {
        uint32_t t2 = rd_u32(c->buf + q);
        uint32_t s2 = rd_u32(c->buf + q + 4);
        if (t2 == FOURCC_STRH) {
            uint32_t fcc = rd_u32(c->buf + q + RIFF_CHUNK_HEADER_BYTES);
            is_audio = (fcc == FOURCC_AUDS);
        } else if (t2 == FOURCC_STRF && is_audio) {
            const uint8_t *wfe = c->buf + q + RIFF_CHUNK_HEADER_BYTES;
            c->audio_channels        = *(const uint16_t *)(wfe + WFE_OFF_CHANNELS);
            c->audio_samples_per_sec = rd_u32(wfe + WFE_OFF_SAMPLES_PER_SEC);
            c->audio_bits            = *(const uint16_t *)(wfe + WFE_OFF_BITS_PER_SAMPLE);
        }
        q += riff_chunk_advance(s2);
    }
}

/* Slurp the whole AVI into c->buf. Returns 1 on success, 0 on
 * fopen/malloc/read failure (caller treats 0 as "couldn't load"). */
static int avi_slurp_file(AviCtx *c, const char *path)
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
    return 1;
}

/* Validate the RIFF / AVI file header. */
static int avi_validate_header(const AviCtx *c)
{
    return memcmp(c->buf,                            "RIFF", 4) == 0
        && memcmp(c->buf + AVI_FORMAT_TYPE_OFFSET,   "AVI ", 4) == 0;
}

static int avi_open(AviCtx *c, const char *path)
{
    if (!avi_slurp_file(c, path)) return 0;
    if (!avi_validate_header(c)) { free(c->buf); return 0; }

    /* Walk top-level LIST chunks: hdrl supplies fps + dimensions, each
     * strl describes one stream (video / audio), movi marks the start
     * of the encoded frame data. */
    uint32_t p = AVI_FILE_HEADER_BYTES;
    while (p + RIFF_CHUNK_HEADER_BYTES <= c->size) {
        uint32_t tag = rd_u32(c->buf + p);
        uint32_t sz  = rd_u32(c->buf + p + 4);

        if (tag != FOURCC_LIST) {
            p += riff_chunk_advance(sz);
            continue;
        }

        uint32_t fourcc = rd_u32(c->buf + p + RIFF_CHUNK_HEADER_BYTES);
        if (fourcc == FOURCC_HDRL) {
            parse_avih(c, p + RIFF_LIST_BODY_OFFSET);
            p += RIFF_LIST_BODY_OFFSET;   /* descend into hdrl body */
            continue;
        }
        if (fourcc == FOURCC_STRL) {
            parse_strl(c, p + RIFF_LIST_BODY_OFFSET,
                          p + RIFF_CHUNK_HEADER_BYTES + sz);
            p += riff_chunk_advance(sz);
            continue;
        }
        if (fourcc == FOURCC_MOVI) {
            c->movi_start = p + RIFF_LIST_BODY_OFFSET;
            c->movi_end   = p + RIFF_CHUNK_HEADER_BYTES + sz;
            c->cursor     = c->movi_start;
            if (c->audio_samples_per_sec) {
                audio_ensure(c->audio_samples_per_sec,
                             c->audio_channels, c->audio_bits);
                prequeue_all_audio(c);
            }
            return 1;
        }
        p += riff_chunk_advance(sz);
    }

    free(c->buf);
    return 0;
}

/* Walk forward until we hit "00dc" (video). "01wb" (audio) chunks are
 * skipped here — prequeue_all_audio already pushed them into SDL's
 * FIFO at avi_open time, so this loop only consumes video. */
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
        /* Safety: a malformed chunk that would jump past movi_end
         * means the AVI is truncated or has a corrupt size field —
         * treat as EOF rather than reading garbage past the buffer. */
        if (next <= c->cursor || next > c->movi_end) return 0;
        c->cursor = next;
    }
    return 0;
}

static void avi_close(AviCtx *c)
{
    if (c->buf) { free(c->buf); c->buf = NULL; }
}


extern void flic_decode_frame(const uint8_t *fdata, uint32_t fsize, int w, int h);

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
    LOG_TRACE("avi", "play %s (%dx%d, %u us/frame)", path, c.width, c.height, c.fps_us);

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
                /* Skip: stop audio NOW (pause device + clear queue)
                 * and stop decoding further frames. Matches the
                 * original MCI StopAviPlayback semantic where the
                 * abort tears down both video and audio together. */
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
    LOG_TRACE("avi", "%s end — %d frames decoded%s", path, frame_count, skipped ? " (skipped)" : "");
    audio_release();
    avi_close(&c);
    return 1;
}
