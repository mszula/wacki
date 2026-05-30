/*
 * binary_data.c — minimal `xlat_binary_ptr` helper, PE loader fallback.
 *
 * History: this file used to embed hand-transcribed copies of every
 * .data / .rdata blob the bytecode referenced (~800 lines of byte
 * arrays + a 100-entry XlatEntry table). After the PE loader landed
 * (pe_loader.c) ALL of those bytes are resolvable straight from the
 * mapped `WACKI.EXE` image — the manual copies were dead weight (and
 * a source of stale-when-edited bugs).
 *
 * What's left:
 * - `xlat_binary_ptr(va)` — single fallback to `PeLoaderRead(va)`.
 * - `xlat_asset_name(va)` — thin wrapper returning the same PE bytes
 * cast to `const char *` (asset filenames live in .data as plain
 * NUL-terminated strings).
 *
 * Removed (now resolved via PE loader):
 * - s_asset_names[] table (lines 32-47 in pre-cleanup version)
 * - s_drut_script / s_barstoi_script / s_barstoi_alt_script
 * - s_button_42[70FC/7100/710C/7110/7114/7118/7120]
 * - s_data_4251B0 / s_data_4251C8
 * - s_data_4263EC / s_sub_426418 (kiosk21 payload + sub)
 * - s_data_42527C (klatka2 click block)
 * - s_data_425A04 (plac click block)
 * - s_script_4238A8 / s_script_423A90 / s_script_423B18 … (~11 blobs)
 * - s_xlat_table[] aliases (~100 entries)
 * - g_maluch_enter_script / g_klatka2_enter_script /
 * g_kiosk21_enter_script / g_plac_enter_script
 *
 * Every removal verified byte-for-byte against PE memory before drop.
 * See review document REVIEW-2026-05.md section C1.
 */
#include "wacki.h"
#include <stddef.h>
#include <string.h>

extern const void *PeLoaderRead(uint32_t va);

/* xlat_asset_name — returns a NUL-terminated C string for `va` if it
 * points inside the original .data string region used for asset
 * filenames (0x004283C0..). PE memory holds the strings verbatim. */
const char *xlat_asset_name(uint32_t va)
{
    if (!va) return NULL;
    return (const char *)PeLoaderRead(va);
}

/* xlat_binary_ptr — resolve an original-VA pointer that some script /
 * data structure references. Caller treats the return as a typed
 * pointer based on what the original code did with it (string,
 * bytecode, verb table, etc.). NULL if VA is outside the PE image. */
const void *xlat_binary_ptr(uint32_t addr)
{
    if (!addr) return NULL;
    return PeLoaderRead(addr);
}
