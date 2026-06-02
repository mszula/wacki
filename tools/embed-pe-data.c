/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tools/embed-pe-data.c — extract WACKI.EXE data sections into a C source.
 *
 * Reads a PE32 executable from argv[1], identifies its read-only +
 * initialised data sections (.rdata, .data), and emits a generated
 * C source at argv[2] containing:
 *
 *   - uint8_t  g_wacki_pe_blob[]       (concatenated section bytes)
 *   - PeSlice  g_wacki_pe_slices[]     (VA→blob mapping, N entries)
 *   - int      g_wacki_pe_slice_count
 *   - uint32_t g_wacki_pe_image_base
 *
 * The engine's PeLoaderRead consults the slice table on every read —
 * no PE parsing at runtime. .text (x86 code we never execute) and
 * .idata/.rsrc (imports + resources we don't reference) are
 * intentionally skipped, which is what keeps the embedded blob small
 * (~160 KB instead of ~302 KB for the full EXE).
 *
 * Run-time safety: any VA falling inside a section's raw range
 * resolves; reads past raw_size (= BSS tail of .data, or unmapped
 * .text VAs) return NULL. The engine never reads from BSS in any
 * traced path through all 5 stages, so the missing tail bytes are
 * harmless. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* PE header offsets — same constants the runtime PE loader uses. */
#define DOS_OFF_E_LFANEW            0x3C
#define PE_SIG_BYTES                4
#define COFF_HEADER_BYTES           20
#define COFF_OFF_NUM_SECTIONS       0x02
#define COFF_OFF_OPT_HEADER_SIZE    0x10
#define OPT_HEADER_OFF_IMAGE_BASE   0x1C
#define SECTION_HEADER_BYTES        40
#define SECTION_OFF_NAME            0x00
#define SECTION_OFF_VSIZE           0x08
#define SECTION_OFF_VA              0x0C
#define SECTION_OFF_RSIZE           0x10
#define SECTION_OFF_RPTR            0x14

#define MAX_SECTIONS                32
#define LINE_BREAK_EVERY            16          /* bytes per emitted C line */

/* Sections we want to embed. Order matters only for blob layout. */
static const char *const k_keep_sections[] = { ".rdata", ".data" };
static const int         k_keep_section_count =
    (int)(sizeof k_keep_sections / sizeof k_keep_sections[0]);

static int section_is_kept(const char *name)
{
    for (int i = 0; i < k_keep_section_count; ++i) {
        if (strcmp(name, k_keep_sections[i]) == 0) return 1;
    }
    return 0;
}

/* Read u32 little-endian from raw bytes. */
static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}
static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

struct Section {
    char     name[16];
    uint32_t va;
    uint32_t vsize;
    uint32_t rsize;
    uint32_t rptr;
};

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr,
                "usage: %s <input.exe> <output.c>\n"
                "  Reads a PE32 file and emits a generated C source\n"
                "  containing the embedded .rdata + .data sections\n"
                "  + a slice table mapping VAs to blob offsets.\n",
                argv[0]);
        return 2;
    }
    const char *in_path  = argv[1];
    const char *out_path = argv[2];

    /* ---- slurp input ---------------------------------------------- */
    FILE *in = fopen(in_path, "rb");
    if (!in) {
        fprintf(stderr, "[embed-pe-data] cannot open %s\n", in_path);
        return 1;
    }
    fseek(in, 0, SEEK_END);
    long fsz_l = ftell(in);
    fseek(in, 0, SEEK_SET);
    if (fsz_l < 0x200) {
        fprintf(stderr, "[embed-pe-data] %s: too small (%ld bytes)\n",
                in_path, fsz_l);
        fclose(in);
        return 1;
    }
    size_t   fsz  = (size_t)fsz_l;
    uint8_t *file = malloc(fsz);
    if (!file || fread(file, 1, fsz, in) != fsz) {
        fprintf(stderr, "[embed-pe-data] %s: short read\n", in_path);
        free(file);
        fclose(in);
        return 1;
    }
    fclose(in);

    /* ---- parse PE header ------------------------------------------ */
    if (file[0] != 'M' || file[1] != 'Z') {
        fprintf(stderr, "[embed-pe-data] %s: not an MZ executable\n", in_path);
        free(file);
        return 1;
    }
    uint32_t pe_off = rd_u32(file + DOS_OFF_E_LFANEW);
    if (pe_off + PE_SIG_BYTES + COFF_HEADER_BYTES + 0xE0 > fsz ||
        file[pe_off] != 'P' || file[pe_off + 1] != 'E' ||
        file[pe_off + 2] != 0   || file[pe_off + 3] != 0)
    {
        fprintf(stderr, "[embed-pe-data] %s: PE signature invalid\n", in_path);
        free(file);
        return 1;
    }
    uint32_t coff_off = pe_off + PE_SIG_BYTES;
    uint16_t num_sec  = rd_u16(file + coff_off + COFF_OFF_NUM_SECTIONS);
    uint16_t opt_sz   = rd_u16(file + coff_off + COFF_OFF_OPT_HEADER_SIZE);
    uint32_t opt_off  = coff_off + COFF_HEADER_BYTES;
    uint32_t img_base = rd_u32(file + opt_off + OPT_HEADER_OFF_IMAGE_BASE);
    uint32_t sec_off  = opt_off + opt_sz;

    if (num_sec > MAX_SECTIONS) {
        fprintf(stderr, "[embed-pe-data] %s: too many sections (%u)\n",
                in_path, num_sec);
        free(file);
        return 1;
    }

    /* ---- collect kept sections ------------------------------------ */
    struct Section keep[MAX_SECTIONS];
    int            keep_n = 0;
    for (int i = 0; i < (int)num_sec; ++i) {
        const uint8_t *h = file + sec_off + i * SECTION_HEADER_BYTES;
        char name[16] = {0};
        memcpy(name, h + SECTION_OFF_NAME, 8);
        if (!section_is_kept(name)) continue;
        struct Section *s = &keep[keep_n++];
        snprintf(s->name, sizeof s->name, "%s", name);
        s->vsize = rd_u32(h + SECTION_OFF_VSIZE);
        s->va    = rd_u32(h + SECTION_OFF_VA);
        s->rsize = rd_u32(h + SECTION_OFF_RSIZE);
        s->rptr  = rd_u32(h + SECTION_OFF_RPTR);
        if ((size_t)s->rptr + s->rsize > fsz) {
            fprintf(stderr,
                    "[embed-pe-data] %s: section %s raw bytes past EOF\n",
                    in_path, s->name);
            free(file);
            return 1;
        }
    }
    if (!keep_n) {
        fprintf(stderr, "[embed-pe-data] %s: no kept sections found\n", in_path);
        free(file);
        return 1;
    }

    /* ---- emit output ---------------------------------------------- */
    FILE *out = fopen(out_path, "w");
    if (!out) {
        fprintf(stderr, "[embed-pe-data] cannot write %s\n", out_path);
        free(file);
        return 1;
    }

    fprintf(out,
        "/* SPDX-License-Identifier: GPL-3.0-or-later\n"
        " * Copyright (C) 2026 Mateusz Szu\xc5\x82\x61\n"
        " */\n"
        "\n"
        "/* src/embedded_wacki_pe.c — GENERATED by tools/embed-pe-data.\n"
        " * DO NOT EDIT — regenerated from data/WACKI.EXE on each build.\n"
        " *\n"
        " * Contains only the read-only + initialised-data sections of\n"
        " * the original WACKI.EXE (%d sections kept: ",
        keep_n);
    for (int i = 0; i < keep_n; ++i) {
        fprintf(out, "%s%s", keep[i].name, (i + 1 < keep_n) ? ", " : "");
    }
    fprintf(out,
        ").\n"
        " * The engine reads from this via PeLoaderRead. .text and the\n"
        " * import/resource sections are intentionally NOT embedded — we\n"
        " * never execute x86 code or touch imports/resources at runtime,\n"
        " * so those ~140 KB are pure overhead. */\n"
        "\n"
        "#include \"wacki/embedded_exe.h\"\n"
        "\n"
        "const uint32_t g_wacki_pe_image_base = 0x%08xu;\n\n",
        (unsigned)img_base);

    /* Slice table FIRST (small, easy to read at the top of the file).
     *
     * PE section headers store VirtualAddress as an RVA — we publish
     * full VAs (image_base + RVA) so PeLoaderRead can compare
     * directly against the operand VA from a bytecode opcode. */
    fprintf(out,
        "const PeSlice g_wacki_pe_slices[] = {\n");
    uint32_t blob_off = 0;
    for (int i = 0; i < keep_n; ++i) {
        uint32_t va_start = img_base + keep[i].va;
        uint32_t va_end   = va_start + keep[i].vsize;
        fprintf(out,
            "    { /* %-7s */ 0x%08xu, 0x%08xu, 0x%08xu, 0x%08xu },\n",
            keep[i].name,
            (unsigned)va_start,
            (unsigned)va_end,
            (unsigned)blob_off,
            (unsigned)keep[i].rsize);
        blob_off += keep[i].rsize;
    }
    fprintf(out,
        "};\n"
        "const int g_wacki_pe_slice_count =\n"
        "    (int)(sizeof g_wacki_pe_slices / sizeof g_wacki_pe_slices[0]);\n"
        "\n");

    /* The blob itself — concatenated raw bytes, line-broken every 16
     * for readable diffs. Deliberately NOT const: op 0x27
     * SET_TAGGED_FIELD patches the actor's bytecode in place via the
     * PeLoaderRead result, and the original engine's runtime image
     * was always writable. Linking the blob into .rodata would
     * SIGBUS on that store. */
    fprintf(out,
        "unsigned char g_wacki_pe_blob[] = {\n");
    uint32_t total = 0;
    for (int i = 0; i < keep_n; ++i) {
        fprintf(out,
            "    /* ---- %s VA=0x%08x rsize=%u ---- */\n",
            keep[i].name, (unsigned)(img_base + keep[i].va),
            (unsigned)keep[i].rsize);
        const uint8_t *p = file + keep[i].rptr;
        for (uint32_t j = 0; j < keep[i].rsize; ++j) {
            if ((j % LINE_BREAK_EVERY) == 0) fputs("    ", out);
            fprintf(out, "0x%02x,", p[j]);
            if ((j % LINE_BREAK_EVERY) == LINE_BREAK_EVERY - 1) fputc('\n', out);
            else                                                fputc(' ',  out);
            ++total;
        }
        if (keep[i].rsize % LINE_BREAK_EVERY) fputc('\n', out);
    }
    fprintf(out,
        "};\n"
        "const unsigned int g_wacki_pe_blob_len = %uu;\n",
        (unsigned)total);

    fclose(out);
    free(file);
    fprintf(stderr,
        "[embed-pe-data] %s -> %s: %d slices, %u blob bytes (image_base=0x%08x)\n",
        in_path, out_path, keep_n, (unsigned)total, (unsigned)img_base);
    return 0;
}
