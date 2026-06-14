/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform/ps2/storage_ps2.c — PS2 storage HAL: memory-card save (libmc)
 * + the BIOS-browser save icon, data-root discovery, the fileXio "cygio" file
 * shim (newlib fopen reaches no device), and the async cutscene read-ahead
 * (plat_flic_*). Built only for TARGET=ps2. */

#include <stdint.h>

#ifdef WACKI_PS2

#include <stdio.h>          /* SEEK_SET/CUR/END for the cygio fseek shim */
#include <stdlib.h>         /* malloc/free for the CygFile handle */
#include <string.h>
#include <kernel.h>         /* EE threads + SyncDCache for the read-ahead reader */
#include <loadfile.h>       /* SifExecModuleBuffer — load mcman/mcserv lazily */
#include <libmc.h>

#include "wacki/platform/storage.h"   /* save / data-root / file-I/O / flic */
#include "ps2_internal.h"             /* platform_ps2_mount_usb (data-root) */

/* The memory-card IOP modules, loaded lazily by ps2_mc_ensure() on the first
 * save access (bin2c-embedded; -I/tmp/embed). */
#include "mcman_irx.c"
#include "mcserv_irx.c"

/* fileXio hand-declared (see ps2sdk's guarded fileXio_rpc.h). */
extern int fileXioOpen(const char *name, int flags);
extern int fileXioRead(int fd, void *buf, int size);
extern int fileXioClose(int fd);
extern int fileXioLseek(int fd, int offset, int whence);
#define FIO_O_RDONLY  0x0001
extern void SDL_Delay(unsigned int ms);       /* read-ahead thread pacing */

/* ---- save to the PS2 memory card (libmc) ------------------------- *
 *
 * save.c's stdio path reaches no device on PS2, so its save read/write are
 * routed here. The save lives in MC_SAVE_PATH on mc0: (port 0, slot 0).
 * mcman/mcserv are loaded lazily on first access: the save is read/written
 * from InitializeGameSubsystems / in-game, by which point the SDL pad init
 * (PlatformInit) has already loaded their sio2man dependency — loading
 * sio2man ourselves here would double it and break pad input. */

#define MC_O_RDONLY  0x0001
#define MC_O_WRONLY  0x0002
#define MC_O_CREAT   0x0200
#define MC_O_TRUNC   0x0400
/* Save folder is named after the disc serial (WACK-00101), as real PS2
 * games do (mc0:/B<region><serial> or the bare serial). NOTE: changing
 * this orphans saves under the old /WACKI folder — they don't migrate. */
#define MC_SAVE_DIR  "/WACK-00101"
#define MC_SAVE_PATH "/WACK-00101/Wacki.sav"
#define MC_XFER_MAX  16384                /* bound a single mcRead/mcWrite RPC */

static int s_mc_ready = 0;

/* Block until the pending async mc* op finishes; return its result. Polls
 * (MC_NOWAIT) rather than trusting a blocking MC_WAIT — robust either way.
 * The game thread spins, but the prio-32 audio thread keeps feeding audsrv
 * and mcserv runs on the IOP, so audio is unaffected during a save. */
static int mc_block(void)
{
    int cmd = 0, result = -1;
    while (mcSync(MC_NOWAIT, &cmd, &result) == 0) { /* in progress */ }
    return result;
}

static int ps2_mc_ensure(void)
{
    if (s_mc_ready) return 1;
    int ret;
    SifExecModuleBuffer(mcman_irx,  size_mcman_irx,  0, NULL, &ret);
    SifExecModuleBuffer(mcserv_irx, size_mcserv_irx, 0, NULL, &ret);
    if (mcInit(MC_TYPE_MC) < 0) return 0;   /* mcInit is the one sync call */
    /* Required first step: mcGetInfo establishes the card connection in
     * mcman — without it every file op fails with -13 (sceMcResFailDetect2).
     * The first call returns -1 ("formatted card newly seen"), which is
     * expected; we only need the side effect of detecting the card. */
    int type = 0, freeclu = 0, format = 0;
    mcGetInfo(0, 0, &type, &freeclu, &format);
    mc_block();
    s_mc_ready = 1;
    return 1;
}

/* ---- BIOS browser presentation: icon.sys + a minimal 3D icon ----- *
 *
 * Without these the memory-card browser flags the save "Corrupted Data"
 * and draws a default cube. icon.sys (the mcIcon struct) carries the
 * title + names the model; icon.ico is a two-sided flat quad with a solid
 * texture — minimal, but a valid model the browser renders cleanly. */

#define ICON_HALF  0x1400                 /* quad half-size (~1.25 in /4096) */
#define ICON_NRM   0x1000                 /* normal magnitude = 1.0 (/4096) */
#define ICON_UVMAX 0x1000                 /* texcoord = 1.0 (/4096) */
#define ICON_VTX   12                     /* 2 faces × 2 tris × 3 verts */

/* 128x128 BGR555 cover-art texture, baked from assets/icons/wacki.ico by
 * tools/gen-ps2-icon.py (defines s_wacki_icon_tex[128*128]). */
#include "ps2_icon_tex.inc"

/* Pack one .ico vertex (24 bytes): vtx s16[3]+pad, normal s16[3]+pad,
 * texcoord s16[2], rgba u8[4]. Returns p advanced past it. */
static uint8_t *icon_vtx(uint8_t *p, int x, int y, int z, int nz, int u, int v)
{
    int16_t *s = (int16_t *)p;
    s[0]=(int16_t)x; s[1]=(int16_t)y; s[2]=(int16_t)z; s[3]=0;
    s[4]=0; s[5]=0; s[6]=(int16_t)nz; s[7]=0;
    s[8]=(int16_t)u; s[9]=(int16_t)v;
    p[20]=0x80; p[21]=0x80; p[22]=0x80; p[23]=0x80;
    return p + 24;
}

static void ps2_write_one(const char *path, const void *a, int alen,
                          const void *b, int blen)
{
    mcOpen(0, 0, path, MC_O_WRONLY | MC_O_CREAT | MC_O_TRUNC);
    int fd = mc_block();
    if (fd < 0) return;
    if (alen > 0) { mcWrite(fd, a, alen); mc_block(); }
    if (blen > 0) { mcWrite(fd, b, blen); mc_block(); }
    mcClose(fd); mc_block();
}

/* The PS2 BIOS renders the icon.sys title as FULL-WIDTH Shift-JIS, not
 * single-byte ASCII (verified against a real FMCB icon.sys: 'A' is stored
 * as 0x8260, big-endian, not 0x41). Map ASCII letters/digits/space to
 * their full-width SJIS codes; the result is two bytes per character. */
static int sjis_fullwidth(unsigned short *dst, const char *s, int maxchars)
{
    unsigned char *o = (unsigned char *)dst;
    int n = 0;
    for (; s[n] && n < maxchars; ++n) {
        unsigned char c = (unsigned char)s[n];
        unsigned short w;
        if      (c >= 'A' && c <= 'Z') w = 0x8260 + (c - 'A');
        else if (c >= 'a' && c <= 'z') w = 0x8281 + (c - 'a');
        else if (c >= '0' && c <= '9') w = 0x824F + (c - '0');
        else                           w = 0x8140;   /* (full-width) space */
        o[n*2] = (unsigned char)(w >> 8);            /* big-endian */
        o[n*2 + 1] = (unsigned char)(w & 0xFF);
    }
    return n * 2;                                     /* bytes written */
}

static void ps2_write_icons(void)
{
    /* --- icon.sys --- rewritten every save so title tweaks take effect */
    mcIcon sys;
    memset(&sys, 0, sizeof sys);
    memcpy(sys.head, "PS2D", 4);
    sys.type     = MCICON_TYPE_SAVED_DATA;
    sys.nlOffset = 12;                         /* "Wacki " (6 full-width chars × 2 B) */
    sys.trans    = 0;                          /* matches real saves */
    static const int bg[4][4] = {
        {0x12,0x12,0x48,0x00}, {0x12,0x12,0x48,0x00},
        {0x06,0x06,0x20,0x00}, {0x06,0x06,0x20,0x00},
    };
    for (int i=0;i<4;i++) for (int j=0;j<4;j++) sys.bgCol[i][j]=bg[i][j];
    sys.lightDir[0][0]= 0.5f; sys.lightDir[0][1]= 0.5f; sys.lightDir[0][2]= 0.5f;
    sys.lightDir[1][0]=-0.5f; sys.lightDir[1][1]= 0.5f; sys.lightDir[1][2]= 0.5f;
    sys.lightDir[2][2]= 1.0f;
    for (int i=0;i<3;i++) sys.lightCol[i][0]=sys.lightCol[i][1]=sys.lightCol[i][2]=0.4f;
    sys.lightAmbient[0]=sys.lightAmbient[1]=sys.lightAmbient[2]=0.5f;
    sjis_fullwidth(sys.title, "Wacki Kosmiczna Rozgrywka", 34);  /* full-width SJIS */
    memcpy(sys.view, "icon.ico", 8);
    memcpy(sys.copy, "icon.ico", 8);
    memcpy(sys.del,  "icon.ico", 8);
    ps2_write_one(MC_SAVE_DIR "/icon.sys", &sys, (int)sizeof sys, NULL, 0);

    /* --- icon.ico --- rewritten every save (cheap enough; picks up art
     * changes without deleting the save). */
    uint8_t ico[20 + ICON_VTX*24 + 44];
    memset(ico, 0, sizeof ico);
    uint32_t *h = (uint32_t *)ico;
    h[0]=0x00010000; h[1]=1; h[2]=0x04; h[3]=0x3F800000; h[4]=ICON_VTX;
    uint8_t *p = ico + 20;
    const int S=ICON_HALF, U=ICON_UVMAX, N=ICON_NRM;
    /* front face (+z) */
    p=icon_vtx(p,-S, S,0, N,0,0); p=icon_vtx(p, S, S,0, N,U,0); p=icon_vtx(p,-S,-S,0, N,0,U);
    p=icon_vtx(p, S, S,0, N,U,0); p=icon_vtx(p, S,-S,0, N,U,U); p=icon_vtx(p,-S,-S,0, N,0,U);
    /* back face (−z, reversed winding) */
    p=icon_vtx(p, S, S,0,-N,U,0); p=icon_vtx(p,-S, S,0,-N,0,0); p=icon_vtx(p, S,-S,0,-N,U,U);
    p=icon_vtx(p,-S, S,0,-N,0,0); p=icon_vtx(p,-S,-S,0,-N,0,U); p=icon_vtx(p, S,-S,0,-N,U,U);
    /* animation: one static frame on shape 0 */
    uint32_t *a = (uint32_t *)p;
    a[0]=1;          /* magic */
    a[1]=1;          /* frame_length */
    a[2]=0x3F800000; /* anim_speed 1.0 */
    a[3]=0;          /* play_offset */
    a[4]=1;          /* frame_count */
    a[5]=0;          /* frame: shape_id */
    a[6]=1;          /* frame: key_count */
    a[7]=0; a[8]=0;  /* frame: unknown */
    a[9]=0; a[10]=0; /* key: time 0.0, value 0.0 */
    ps2_write_one(MC_SAVE_DIR "/icon.ico", ico, (int)sizeof ico,
                  s_wacki_icon_tex, (int)sizeof s_wacki_icon_tex);
}

/* Storage HAL (include/wacki/platform/storage.h): the save lives on the
 * memory card via libmc. Write `size` bytes; returns 1 on full success. */
int plat_save_write(const void *buf, int size)
{
    if (!ps2_mc_ensure()) return 0;
    mcMkDir(0, 0, MC_SAVE_DIR); mc_block();           /* ok if it exists */
    ps2_write_icons();   /* refresh icon.sys (title); writes icon.ico if absent */
    mcOpen(0, 0, MC_SAVE_PATH, MC_O_WRONLY | MC_O_CREAT | MC_O_TRUNC);
    int fd = mc_block();
    if (fd < 0) return 0;
    const char *p = (const char *)buf;
    int total = 0;
    while (total < size) {
        int chunk = size - total;
        if (chunk > MC_XFER_MAX) chunk = MC_XFER_MAX;
        mcWrite(fd, p + total, chunk);
        int w = mc_block();
        if (w <= 0) break;
        total += w;
    }
    mcClose(fd); mc_block();
    return total == size;
}

/* Storage HAL: read up to `size` bytes from the card save. Returns bytes
 * read (0 if the save is absent / unreadable). */
int plat_save_read(void *buf, int size)
{
    if (!ps2_mc_ensure()) return 0;
    mcOpen(0, 0, MC_SAVE_PATH, MC_O_RDONLY);
    int fd = mc_block();
    if (fd < 0) return 0;
    char *p = (char *)buf;
    int total = 0;
    while (total < size) {
        int chunk = size - total;
        if (chunk > MC_XFER_MAX) chunk = MC_XFER_MAX;
        mcRead(fd, p + total, chunk);
        int r = mc_block();
        if (r <= 0) break;
        total += r;
    }
    mcClose(fd); mc_block();
    return total;
}

/* ---- data-root discovery (storage HAL) --------------------------- *
 *
 * The probe callback (data_root.c::try_root_and_data) opens through the
 * cygio shim, which on PS2 is fileXio — so here we only supply the PS2
 * device list and bring up the USB-mass FAT stack lazily. */
int plat_data_roots(int (*probe)(const char *root))
{
    /* host: — PCSX2 HostFS, rooted at the booted ELF's folder (a bare ELF
     *         next to ./data). cdfs: — the ISO9660 disc; DATA/ holds the
     *         archives (upper-case, forward-slash, no ';1' suffix — the
     *         cygio normalizer handles the case fold). Both confirmed on
     *         PCSX2. */
    static const char *const dev[] = {
        "host:data", "host:", "cdfs:/DATA", "cdfs:", NULL
    };
    for (int i = 0; dev[i]; ++i)
        if (probe(dev[i])) return 1;

    /* Real-hardware last resort: host:/cdfs: don't exist when booted from a
     * USB stick via uLaunchELF — the data sits on the same stick at
     * mass:/wacki/data/. Bring up the USB FAT stack (lazily, so PCSX2/disc
     * boots skip it) and probe mass:. */
    if (platform_ps2_mount_usb()) {
        static const char *const usb[] = {
            "mass:/wacki/data", "mass:/wacki", "mass:/DATA", "mass:", NULL
        };
        for (int i = 0; usb[i]; ++i)
            if (probe(usb[i])) return 1;
    }
    return 0;
}

/* No native folder picker on the PS2 — the data location is fixed by the
 * boot device (disc / HostFS / USB stick). */
int plat_prompt_data_folder(int (*probe)(const char *root))
{
    (void)probe;
    return 0;
}

/* ---- file I/O shim (storage HAL) --------------------------------- *
 *
 * ps2sdk's newlib fopen() reaches no device, so every DTA archive + asset
 * read routes through fileXio instead. This is the PS2 backend of the
 * fopen_cyg/... contract (storage.h); the stdio backend is file_host.c. The
 * IOP fileio stack is brought up in platform_ps2_io_init() before any read. */

/* cdfs: (the ISO9660 disc device) is upper-case and wants NO ';1' version
 * suffix; the engine asks for mixed-case names, so upper-case the path after
 * the device prefix. host:/mass: are left untouched — HostFS mirrors the host
 * filesystem (case-insensitive on macOS dev). */
void ps2_normalize_path(const char *in, char *out, size_t outsz)
{
    if (outsz == 0) return;
    if (strncmp(in, "cdfs:", 5) != 0) {
        snprintf(out, outsz, "%s", in);
        return;
    }
    size_t o = 0;
    for (size_t k = 0; k < 5 && o + 1 < outsz; ++k) out[o++] = in[k];
    for (size_t i = 5; in[i] && o + 1 < outsz; ++i) {
        char c = in[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        out[o++] = c;
    }
    out[o] = 0;
}

struct CygFile { int fd; };           /* completes storage.h's opaque CygFile */

CygFile *fopen_cyg(const char *name, const char *mode)
{
    (void)mode;                       /* the engine opens archives read-only */

    /* Every PS2 path needs a device prefix (host: / cdfs: / mass: / …). The
     * portable asset search tries cwd-relative candidates too — a bare
     * "Dane_01.dta" or "./data/fiacik.wav" — which are valid on desktop but can
     * never open here. Reject them quietly: handing them to fileXioOpen only
     * spams the IOP log with "Unknown device" and burns a SIF RPC per try. The
     * caller then falls through to the next, device-prefixed candidate
     * (g_data_root). A device prefix = a ':' that precedes the first '/'. */
    const char *colon = strchr(name, ':');
    const char *slash = strchr(name, '/');
    if (!colon || (slash && slash < colon)) return NULL;

    char fixed[320];
    ps2_normalize_path(name, fixed, sizeof fixed);
    int fd = fileXioOpen(fixed, FIO_O_RDONLY);
    if (fd < 0) return NULL;
    CygFile *f = (CygFile *)malloc(sizeof *f);
    if (!f) { fileXioClose(fd); return NULL; }
    f->fd = fd;
    return f;
}

void fclose_cyg(CygFile *f)
{
    if (!f) return;
    fileXioClose(f->fd);
    free(f);
}

uint32_t fread_cyg(void *dst, uint32_t sz, uint32_t n, CygFile *f)
{
    if (sz == 0) return 0;
    uint32_t total = sz * n, done = 0;
    char *p = (char *)dst;
    while (done < total) {                /* fileXioRead may short-read */
        int got = fileXioRead(f->fd, p + done, (int)(total - done));
        if (got <= 0) break;
        done += (uint32_t)got;
    }
    return done / sz;
}

void fseek_cyg(CygFile *f, int32_t off, int whence)
{
    fileXioLseek(f->fd, (int)off, whence);   /* SEEK_SET/CUR/END == 0/1/2 */
}

int32_t ftell_cyg(CygFile *f)
{
    return (int32_t)fileXioLseek(f->fd, 0, 1 /* SEEK_CUR */);
}

/* ---- FLIC reader (storage HAL): async read-ahead for cutscenes -- *
 *
 * The PS2 plat_flic_* backend. A blocking disc read pauses the FLIC decoder,
 * so off a disc the 52 MB cutscene AVI stutters on every refill. A background
 * thread reads the file sequentially into a big ring; flic.c pulls from RAM
 * and only ever waits if the disc can't keep up (the emulated/real drive is
 * several× faster than the AVI bitrate, so it stays ahead). Single instance —
 * one cutscene plays at a time, which is why the HAL reader needs no handle.
 * SPSC ring: the thread is the sole producer (wpos), the decoder the sole
 * consumer (rpos). Seeks are forward-within-buffer (just advance rpos) except
 * the one-time header seeks at open, which reposition the thread via
 * s_aread_seekreq. The underlying bytes come through the fopen_cyg/... shim
 * (the PS2 file backend above). */
#define AREAD_RING (2 * 1024 * 1024)          /* ~2.4 s at the AVI bitrate */
static uint8_t  s_aread_ring[AREAD_RING] __attribute__((aligned(64)));
static char     s_aread_stack[16 * 1024]  __attribute__((aligned(16)));
static CygFile *s_aread_cf       = NULL;
static int32_t  s_aread_filesize = 0;
static volatile uint32_t s_aread_wpos = 0, s_aread_rpos = 0;
static volatile int32_t  s_aread_rfilepos = 0, s_aread_wfilepos = 0;
static volatile int32_t  s_aread_seekreq = -1;
static volatile int      s_aread_run = 0, s_aread_eof = 0, s_aread_alive = 0;
static int               s_aread_tid = -1;

static void aread_thread(void *arg)
{
    (void)arg;
    s_aread_alive = 1;
    while (s_aread_run) {
        if (s_aread_seekreq >= 0) {                  /* consumer asked to reposition */
            int32_t t = s_aread_seekreq;
            fseek_cyg(s_aread_cf, t, SEEK_SET);
            s_aread_rpos = s_aread_wpos = 0;
            s_aread_rfilepos = s_aread_wfilepos = t;
            s_aread_eof = (t >= s_aread_filesize);
            s_aread_seekreq = -1;
            continue;
        }
        if (s_aread_wfilepos >= s_aread_filesize) { s_aread_eof = 1; SDL_Delay(2); continue; }
        uint32_t used  = (s_aread_wpos - s_aread_rpos + AREAD_RING) % AREAD_RING;
        uint32_t freeb = AREAD_RING - used - 1;
        if (freeb < 4096) { SDL_Delay(1); continue; }     /* ring full → wait */
        uint32_t to_end = AREAD_RING - s_aread_wpos;
        uint32_t n = freeb;
        if (n > to_end) n = to_end;
        if (n > 65536)  n = 65536;
        uint32_t favail = (uint32_t)(s_aread_filesize - s_aread_wfilepos);
        if (n > favail) n = favail;
        uint32_t got = fread_cyg(s_aread_ring + s_aread_wpos, 1, n, s_aread_cf);
        if (got == 0) { s_aread_eof = 1; SDL_Delay(2); continue; }
        SyncDCache(s_aread_ring + s_aread_wpos, s_aread_ring + s_aread_wpos + got);
        s_aread_wpos = (s_aread_wpos + got) % AREAD_RING;
        s_aread_wfilepos += (int32_t)got;
    }
    s_aread_alive = 0;
    ExitThread();
}

int plat_flic_open(const char *path)
{
    s_aread_cf = fopen_cyg(path, "rb");
    if (!s_aread_cf) return 0;
    fseek_cyg(s_aread_cf, 0, SEEK_END);
    s_aread_filesize = ftell_cyg(s_aread_cf);
    fseek_cyg(s_aread_cf, 0, SEEK_SET);
    if (s_aread_filesize <= 0) { fclose_cyg(s_aread_cf); s_aread_cf = NULL; return 0; }
    s_aread_wpos = s_aread_rpos = 0;
    s_aread_rfilepos = s_aread_wfilepos = 0;
    s_aread_seekreq = -1;
    s_aread_eof = 0;
    s_aread_run = 1;
    ee_thread_t th;
    th.func = (void *)aread_thread; th.stack = s_aread_stack;
    th.stack_size = sizeof s_aread_stack; th.gp_reg = GetGP();
    th.initial_priority = 36;            /* below audio (32), above game (40) */
    th.attr = 0; th.option = 0;
    s_aread_tid = CreateThread(&th);
    if (s_aread_tid >= 0) StartThread(s_aread_tid, NULL);
    return 1;
}

uint32_t plat_flic_read(void *dst, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    uint32_t got = 0;
    while (got < n) {
        uint32_t avail = (s_aread_wpos - s_aread_rpos + AREAD_RING) % AREAD_RING;
        if (avail == 0) {
            if (s_aread_eof && s_aread_rfilepos >= s_aread_filesize) break;
            SDL_Delay(1);                /* underrun — wait for the reader */
            continue;
        }
        uint32_t to_end = AREAD_RING - s_aread_rpos;
        uint32_t take = n - got;
        if (take > avail)  take = avail;
        if (take > to_end) take = to_end;
        memcpy(d + got, s_aread_ring + s_aread_rpos, take);
        s_aread_rpos = (s_aread_rpos + take) % AREAD_RING;
        s_aread_rfilepos += (int32_t)take;
        got += take;
    }
    return got;
}

void plat_flic_seek(int32_t off, int whence)
{
    int32_t target;
    if      (whence == SEEK_SET) target = off;
    else if (whence == SEEK_CUR) target = s_aread_rfilepos + off;
    else                         target = s_aread_filesize + off;
    uint32_t avail = (s_aread_wpos - s_aread_rpos + AREAD_RING) % AREAD_RING;
    if (target >= s_aread_rfilepos &&
        target <= s_aread_rfilepos + (int32_t)avail) {
        s_aread_rpos = (s_aread_rpos + (uint32_t)(target - s_aread_rfilepos)) % AREAD_RING;
        s_aread_rfilepos = target;       /* forward skip within the ring */
    } else {
        s_aread_seekreq = target;        /* reposition the reader, wait for ack */
        for (int i = 0; i < 1000 && s_aread_seekreq >= 0; ++i) SDL_Delay(1);
    }
}

int32_t plat_flic_tell(void) { return s_aread_rfilepos; }

void plat_flic_close(void)
{
    s_aread_run = 0;
    for (int i = 0; i < 1000 && s_aread_alive; ++i) SDL_Delay(1);  /* let it exit */
    if (s_aread_tid >= 0) { DeleteThread(s_aread_tid); s_aread_tid = -1; }
    if (s_aread_cf) { fclose_cyg(s_aread_cf); s_aread_cf = NULL; }
}

#endif /* WACKI_PS2 */
