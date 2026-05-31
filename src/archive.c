/* src/archive.c — Cygert "BASE_IO_CPP" archive (.dta) handling.
 *
 * .DTA archive layout:
 *
 *   +0           u32 magic = 'BASE'
 *   +4..N        payload — for each file: DtaFileHeader + PKv2-
 *                compressed bytes
 *   N..end-8     SPIS index — a PKv2-compressed array of
 *                DtaIndexEntry records
 *   end-8        u32 magic = 'SPIS'
 *   end-4        u32 spis_size = offset from EOF back to the SPIS
 *                magic
 *
 * OpenDtaArchiveFile loads + depacks the SPIS index into s_dir[].
 * LoadFileFromDta does an UPPER-cased linear name lookup and depacks
 * the matching file in-place; the caller owns the returned buffer
 * (free via xfree). */

#include "wacki.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef WACKI_WITH_WIN32
#  define lstrcpynA(dst, src, n) (snprintf((dst), (n), "%s", (src)), (dst))
#endif

/* ---- constants ---------------------------------------------------- */

/* DTA path buffer size + a sanity ceiling on the SPIS index size so a
 * corrupt header doesn't request a multi-GB allocation. */
#define DTA_PATH_BYTES                      260
#define SPIS_MAX_SIZE_BYTES                 (32u * 1024u * 1024u)

/* Each DtaIndexEntry is 16 bytes (= 4-byte file-offset + 12-byte
 * NUL-padded name) — the depacked SPIS length divided by this gives
 * the entry count. */
#define DTA_INDEX_ENTRY_BYTES               16
#define DTA_INDEX_ENTRY_LOG2                4

/* SPIS trailer layout. The trailing u32 stores the distance from
 * EOF-4 back to the SPIS magic; ADD this to the magic offset to find
 * the start of the embedded PKv2 stream. */
#define SPIS_FOOTER_SIZE_BYTES              4
#define SPIS_HEADER_BYTES                   16     /* 4 × u32 */

/* Lower-to-upper-case ASCII fold mask (a..z → A..Z). */
#define ASCII_UPPERCASE_MASK                0xDF

/* ---- module state ----------------------------------------------- */

/* s_dir + s_dir_count are non-static so the standalone extractor in
 * tools/dta-extract.c can walk the directory after OpenDtaArchive
 * File populates it. The path buffer is module-local — only one
 * archive is mounted at a time. */
static char        s_dta_path[DTA_PATH_BYTES];
static const char *s_dta_path_p = s_dta_path;
DtaIndexEntry     *s_dir        = NULL;
int32_t            s_dir_count  = 0;

/* ---- helpers ---------------------------------------------------- */

/* macOS / case-sensitive FS fallback: retry with the basename
 * uppercased so files written as `DANE_02.DTA` by the original
 * installer are found even when the caller passes `dane_02.dta`. */
static void uppercase_basename(char *path)
{
    size_t plen = strlen(path);
    size_t cut  = plen;
    while (cut > 0 && path[cut - 1] != '/' && path[cut - 1] != '\\') --cut;
    for (size_t i = cut; i < plen; ++i) {
        char c = path[i];
        if (c >= 'a' && c <= 'z') path[i] = (char)(c - 32);
    }
}

/* Open the .dta file, retrying once with the basename uppercased for
 * case-sensitive filesystems. */
static CygFile *open_dta_with_case_fallback(void)
{
    CygFile *f = fopen_cyg(s_dta_path_p, "rb");
    if (f) return f;
    uppercase_basename(s_dta_path);
    return fopen_cyg(s_dta_path_p, "rb");
}

/* Verify the BASE magic at file offset 0. Returns 1 on match, 0
 * otherwise (logs the failure). */
static int verify_base_magic(CygFile *f, const char *path)
{
    uint32_t magic = 0;
    fread_cyg(&magic, 1, 4, f);
    if (magic == DTA_MAGIC_BASE) return 1;
    fprintf(stderr, "%s: bad BASE magic 0x%08X\n", path, magic);
    return 0;
}

/* Read the SPIS header (4×u32 starting at EOF-spis_size-8). Returns 1
 * on success; writes *out_comp + *out_unp from the embedded PKv2 size
 * fields. */
static int read_spis_header(CygFile *f, int32_t spis_size,
                            const char *path,
                            uint32_t *out_comp, uint32_t *out_unp)
{
    uint32_t spis_hdr[SPIS_HEADER_BYTES / 4] = {0};
    fseek_cyg(f, -(spis_size + (int32_t)SPIS_FOOTER_SIZE_BYTES
                              + (int32_t)SPIS_FOOTER_SIZE_BYTES), SEEK_END);
    fread_cyg(spis_hdr, 1, sizeof spis_hdr, f);

    if (spis_hdr[0] != DTA_MAGIC_SPIS) {
        fprintf(stderr, "%s: bad SPIS magic 0x%08X at -%d\n",
                path, spis_hdr[0],
                spis_size + (int32_t)SPIS_FOOTER_SIZE_BYTES + 4);
        return 0;
    }
    *out_comp = spis_hdr[2];
    *out_unp  = spis_hdr[3];
    return 1;
}

/* Upper-case the 12-byte DTA lookup key in place (a..z → A..Z). */
static void dta_key_upper(char *key)
{
    for (int i = 0; i < DTA_NAME_LEN && key[i]; ++i) {
        if (key[i] >= 'a' && key[i] <= 'z') key[i] &= ASCII_UPPERCASE_MASK;
    }
}

/* Linear search for `key` in the loaded directory. Returns the
 * matching entry's file_offset, or -1 if not found. */
static int32_t dta_find_offset(const char *key)
{
    for (int32_t i = 0; i < s_dir_count; ++i) {
        if (memcmp(s_dir[i].name, key, DTA_NAME_LEN) == 0) {
            return (int32_t)s_dir[i].file_offset;
        }
    }
    return -1;
}

/* ---- public API ------------------------------------------------- */

int OpenDtaArchiveFile(const char *path)
{
    s_dta_path_p = s_dta_path;
    lstrcpynA(s_dta_path, path, sizeof s_dta_path);
    s_dir_count = 0;

    CygFile *f = open_dta_with_case_fallback();
    if (!f) return 0;                       /* silent — caller retries */

    if (!verify_base_magic(f, path)) { fclose_cyg(f); return 0; }

    /* Distance from EOF-4 back to the SPIS magic. */
    int32_t spis_size = 0;
    fseek_cyg(f, -(int32_t)SPIS_FOOTER_SIZE_BYTES, SEEK_END);
    fread_cyg(&spis_size, 1, SPIS_FOOTER_SIZE_BYTES, f);

    uint32_t comp = 0, unp = 0;
    if (!read_spis_header(f, spis_size, path, &comp, &unp)) {
        fclose_cyg(f);
        return 0;
    }
    uint32_t cap = comp > unp ? comp : unp;
    if (!cap || cap > SPIS_MAX_SIZE_BYTES) {
        fprintf(stderr, "%s: SPIS size %u/%u out of range\n", path, comp, unp);
        fclose_cyg(f);
        return 0;
    }

    /* Read the PKv2 stream (starts right after the SPIS magic word). */
    s_dir = (DtaIndexEntry *)xmalloc(cap);
    if (!s_dir) { fclose_cyg(f); return 0; }
    fseek_cyg(f, -(spis_size + (int32_t)SPIS_FOOTER_SIZE_BYTES), SEEK_END);
    fread_cyg(s_dir, 1, comp, f);
    fclose_cyg(f);

    DepackPkv2Buffer(s_dir, s_dir, NULL);
    s_dir_count = (int32_t)(unp >> DTA_INDEX_ENTRY_LOG2);
    fprintf(stderr, "[archive] %s mounted (%d entries)\n", path, s_dir_count);
    return 1;
}

int LoadFileFromDta(const char *name, void **out_buf, uint32_t *out_size)
{
    char key[DTA_NAME_LEN];
    memset(key, 0, sizeof key);
    strncpy(key, name, sizeof key);
    dta_key_upper(key);

    int32_t off = dta_find_offset(key);
    if (off <= 0) {
        fprintf(stderr, "*** Brak takiego pliku w bazie %s\n", name);
        return 0;
    }

    CygFile *f = fopen_cyg(s_dta_path_p, "rb");
    if (!f) return 0;

    fseek_cyg(f, off, SEEK_SET);
    DtaFileHeader hdr;
    fread_cyg(&hdr, 1, sizeof hdr, f);

    uint32_t cap = hdr.compressed_size > hdr.unpacked_size
                 ? hdr.compressed_size : hdr.unpacked_size;
    void *buf = xmalloc(cap);
    if (!buf) { fclose_cyg(f); return 0; }

    /* Re-read INCLUDING the header — DepackPkv2Buffer expects the full
     * PKv2 stream (magic + sizes + payload). */
    fseek_cyg(f, off, SEEK_SET);
    fread_cyg(buf, 1, hdr.compressed_size, f);
    fclose_cyg(f);

    DepackPkv2Buffer(buf, buf, NULL);

    *out_buf  = buf;
    *out_size = hdr.unpacked_size;
    return 1;
}
