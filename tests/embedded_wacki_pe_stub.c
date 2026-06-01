/* tests/embedded_wacki_pe_stub.c — empty embedded slice table for tests.
 *
 * The engine binary links a real src/embedded_wacki_pe.c generated
 * by tools/embed-pe-data from data/WACKI.EXE. The test binary doesn't
 * want a 150 KB blob baked in for tests that don't need PE data, so
 * we provide an empty slice table here.
 *
 * Tests that DO need PE-resolvable VAs (test_pe_loader, test_binary_
 * data, etc.) call PeLoaderInit(tmpfile) with a synthetic PE blob —
 * the dynamic runtime image then takes precedence over this empty
 * embedded table. */

#include "wacki/embedded_exe.h"

const PeSlice       g_wacki_pe_slices[]    = {{0, 0, 0, 0}};
const int           g_wacki_pe_slice_count = 0;
const unsigned char g_wacki_pe_blob[]      = {0};
const unsigned int  g_wacki_pe_blob_len    = 0;
const uint32_t      g_wacki_pe_image_base  = 0;
