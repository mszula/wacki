/* include/wacki/embedded_exe.h — WACKI.EXE data sections baked in.
 *
 * The engine resolves original-VA references (verb tables, scripts,
 * asset filename strings, komnata tables, …) through PeLoaderRead.
 * Historically PeLoaderRead read from a flat memory image built by
 * parsing WACKI.EXE off disk at boot. To remove that runtime
 * dependency, we now embed ONLY the data sections (.rdata + .data)
 * directly into the engine binary as a precomputed slice table +
 * concatenated byte blob.
 *
 * The slice table is generated at build time by tools/embed-pe-data
 * from data/WACKI.EXE. .text (132 KB of x86 code we never execute),
 * .idata (3.5 KB of imports), and .rsrc (3.5 KB of resources) are
 * skipped — they're never read by any code path through all five
 * stages.
 *
 * Lookups are constant-time per slice (linear scan of N=2 entries —
 * one for .rdata, one for .data) — no PE parsing, no malloc, no
 * runtime init. The slice table + blob live in `.rodata` so the
 * engine starts with the data already mapped.
 *
 * Any VA outside both slice ranges returns NULL from PeLoaderRead;
 * VAs that fall in the BSS tail (.data's virtual extent past its
 * raw_size) also return NULL. Both are safe in practice — the engine
 * never references those addresses in any traced path. */

#ifndef WACKI_EMBEDDED_EXE_H
#define WACKI_EMBEDDED_EXE_H

#include <stdint.h>

/* Per-section slice. `va_start`/`va_end` cover the full virtual
 * extent (including BSS tail). `blob_off` is the offset of the
 * section's raw bytes inside `g_wacki_pe_blob`. `raw_size` is the
 * count of those raw bytes; reads at offsets ≥ raw_size fall into
 * the BSS tail and return NULL. */
typedef struct PeSlice {
    uint32_t va_start;
    uint32_t va_end;
    uint32_t blob_off;
    uint32_t raw_size;
} PeSlice;

extern const PeSlice            g_wacki_pe_slices[];
extern const int                g_wacki_pe_slice_count;
extern const unsigned char      g_wacki_pe_blob[];
extern const unsigned int       g_wacki_pe_blob_len;
extern const uint32_t           g_wacki_pe_image_base;

#endif /* WACKI_EMBEDDED_EXE_H */
