/*
 * archive.c — Cygert "BASE_IO_CPP" archive (.dta) handling.
 *
 * Original addresses:
 * OpenDtaArchiveFile 0x00411210
 * LoadFileFromDta 0x00411080
 *
 * File layout:
 * +0 DWORD magic = "BASE"
 * +4..N payload (each file = DtaFileHeader + PKv2-compressed bytes)
 * N..end-8 SPIS index (PKv2-compressed array of DtaIndexEntry)
 * end-8 DWORD magic = "SPIS"
 * end-4 DWORD spis_size (offset from EOF to the start of "SPIS")
 */
#include "wacki.h"
#include <string.h>
#include <stdio.h>

#ifndef WACKI_WITH_WIN32
#  define lstrcpynA(dst, src, n) (snprintf((dst), (n), "%s", (src)), (dst))
#endif

void DepackPkv2Buffer(void *src, void *dst, void (*progress)(int));

/* heap helpers (real engine uses Base_IO_CPP::malloc / free) */
extern void *xmalloc(uint32_t sz);
extern void  xfree  (void *p);

/* Cygert's tiny file-I/O class — preserved by symbol. The forward
 * declaration lives in wacki.h; this is the full body. */
struct CygFile {
    FILE *fp;
};
extern CygFile *fopen_cyg (const char *name, const char *mode);     /* */
extern void     fclose_cyg(CygFile *f);                             /* */
extern uint32_t fread_cyg (void *dst, uint32_t sz, uint32_t n, CygFile *f); /* */
extern void     fseek_cyg (CygFile *f, int32_t off, int whence);    /* */

/* ---- module state (mirrors DAT_004795B0..B8) ----------------------------
 * The two index globals (s_dir, s_dir_count) are exposed publicly so that
 * the standalone extractor in tools/dta-extract.c can walk the directory
 * after OpenDtaArchiveFile has populated it. */
static char        s_dta_path[260];        /* DAT_00479488 */
static const char *s_dta_path_p = s_dta_path; /* DAT_00479588 */
DtaIndexEntry *s_dir = NULL;               /* DAT_004795B0 */
int32_t        s_dir_count = 0;            /* DAT_004795B4 */

/* ------------------------------------------------------------------------- *
 * OpenDtaArchiveFile — 0x00411210
 *
 * Mounts a new .dta archive. The previous index is replaced.
 * ------------------------------------------------------------------------- */
int OpenDtaArchiveFile(const char *path)
{
    s_dta_path_p = s_dta_path;
    lstrcpynA(s_dta_path, path, sizeof s_dta_path);
    s_dir_count = 0;

    CygFile *f = fopen_cyg(s_dta_path_p, "rb");
    if (!f) {
        /* macOS case-fix: retry with all-uppercase filename component */
        size_t plen = strlen(s_dta_path);
        size_t cut  = plen;
        while (cut > 0 && s_dta_path[cut-1] != '/' && s_dta_path[cut-1] != '\\') --cut;
        for (size_t i = cut; i < plen; ++i) {
            char c = s_dta_path[i];
            if (c >= 'a' && c <= 'z') s_dta_path[i] = (char)(c - 32);
        }
        f = fopen_cyg(s_dta_path_p, "rb");
        if (!f) return 0;                           /* silent — caller retries */
    }

    /* +0 DWORD magic "BASE" */
    uint32_t magic = 0;
    fread_cyg(&magic, 1, 4, f);
    if (magic != DTA_MAGIC_BASE) {
        fprintf(stderr, "%s: bad BASE magic 0x%08X\n", path, magic);
        fclose_cyg(f); return 0;
    }

    /* end-4: DWORD spis_size — distance from EOF-4 back to the SPIS magic */
    int32_t spis_size = 0;
    fseek_cyg(f, -4, SEEK_END);
    fread_cyg(&spis_size, 1, 4, f);

    /* At (EOF-8-spis_size): 16-byte SPIS header overlapping the start of
 * the embedded PKv2 stream:
 * +0 DWORD magic = "SPIS"
 * +4 DWORD pkv2_magic ("PKv2")
 * +8 DWORD pkv2_compressed_size
 * +12 DWORD pkv2_unpacked_size
 * The 4 SPIS-magic bytes are *separate*; immediately after them, at
 * (EOF-4-spis_size), the PKv2 stream begins (its own header repeats
 * the magic and the sizes we just read).
 */
    uint32_t spis_hdr[4] = {0};
    fseek_cyg(f, -(spis_size + 8), SEEK_END);
    fread_cyg(spis_hdr, 1, sizeof spis_hdr, f);
    if (spis_hdr[0] != DTA_MAGIC_SPIS) {
        fprintf(stderr, "%s: bad SPIS magic 0x%08X at -%d\n",
                path, spis_hdr[0], spis_size + 8);
        fclose_cyg(f); return 0;
    }
    uint32_t comp = spis_hdr[2];
    uint32_t unp  = spis_hdr[3];
    uint32_t cap  = comp > unp ? comp : unp;
    if (!cap || cap > 32u*1024u*1024u) {
        fprintf(stderr, "%s: SPIS size %u/%u out of range\n", path, comp, unp);
        fclose_cyg(f); return 0;
    }

    /* Read the PKv2 stream (starts right after the SPIS magic). */
    s_dir = (DtaIndexEntry *)xmalloc(cap);
    if (!s_dir) { fclose_cyg(f); return 0; }
    fseek_cyg(f, -(spis_size + 4), SEEK_END);
    fread_cyg(s_dir, 1, comp, f);
    fclose_cyg(f);

    DepackPkv2Buffer(s_dir, s_dir, NULL);
    s_dir_count = (int32_t)(unp >> 4);     /* 16-byte directory entries */
    fprintf(stderr, "[archive] %s mounted (%d entries)\n", path, s_dir_count);
    return 1;
}

/* ------------------------------------------------------------------------- *
 * LoadFileFromDta — 0x00411080
 *
 * Look up a named entry, allocate buffer, read compressed bytes, depack
 * in-place. The caller receives a malloc'd buffer + size; ownership is
 * theirs (free with xfree).
 * ------------------------------------------------------------------------- */
int LoadFileFromDta(const char *name, void **out_buf, uint32_t *out_size)
{
    char key[DTA_NAME_LEN];
    memset(key, 0, sizeof key);
    strncpy(key, name, sizeof key);
    /* upper-case ASCII a..z */
    for (int i = 0; i < (int)sizeof key && key[i]; ++i)
        if (key[i] >= 'a' && key[i] <= 'z') key[i] &= 0xDF;

    /* linear search */
    int32_t idx = -1;
    for (int32_t i = 0; i < s_dir_count; ++i) {
        if (memcmp(s_dir[i].name, key, DTA_NAME_LEN) == 0) {
            idx = (int32_t)s_dir[i].file_offset;
            break;
        }
    }
    if (idx <= 0) { fprintf(stderr, "*** Brak takiego pliku w bazie %s\n", name); return 0; }

    CygFile *f = fopen_cyg(s_dta_path_p, "rb");
    if (!f) return 0;
    fseek_cyg(f, idx, SEEK_SET);

    DtaFileHeader hdr;
    fread_cyg(&hdr, 1, sizeof hdr, f);
    uint32_t cap = hdr.compressed_size > hdr.unpacked_size
                 ? hdr.compressed_size : hdr.unpacked_size;
    void *buf = xmalloc(cap);
    if (!buf) { fclose_cyg(f); return 0; }

    /* re-read including header (DepackPkv2Buffer expects it) */
    fseek_cyg(f, idx, SEEK_SET);
    fread_cyg(buf, 1, hdr.compressed_size, f);
    fclose_cyg(f);

    DepackPkv2Buffer(buf, buf, NULL);

    *out_buf  = buf;
    *out_size = hdr.unpacked_size;
    return 1;
}
