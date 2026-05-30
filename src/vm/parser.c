/* src/vm/parser.c — bytecode scanning helpers for the script VM.
 *
 * Implementation notes: every helper here walks the instruction
 * stream linearly using the `len * 2` stride encoded in byte +1 of
 * each instruction. The two scan helpers (vm_skip_to_endif,
 * vm_find_label) terminate on op 0x56 EOF — op 0x55 (END_HARD) is
 * stepped over because it only terminates EXECUTION, not scanning;
 * IF bodies can legitimately contain END_HARD between the opener and
 * the matching ENDIF.
 */

#include "wacki.h"
#include "parser.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* ---- opcodes referenced for structural scanning ------------------- */
#define OP_IF_FIRST         0x00   /* ops 0x00..0x05 are IF variants */
#define OP_IF_LAST          0x05
#define OP_ENDIF            0x06
#define OP_ELSE             0x07
#define OP_LABEL            0x16
#define OP_END_HARD         0x55   /* terminates execution, NOT scanning */
#define OP_END              0x56   /* terminates BOTH execution and scanning */

/* Special script-var indices observed during script execution. The
 * VM module proper aliases var[4] as g_return_reg and var[14] as
 * g_game_over_code; var[17] is the per-stage-completion bitmap. We
 * just log writes to them here to aid debugging. */
#define SCRIPT_VAR_INDEX_MASK   0x1FF
#define VAR_INDEX_GAME_OVER     14
#define VAR_INDEX_COMPLETED     17

uint32_t vm_var_get(uint16_t i)
{
    return g_script_vars[i & SCRIPT_VAR_INDEX_MASK];
}

void vm_var_set(uint16_t i, uint32_t v)
{
    uint16_t idx = i & SCRIPT_VAR_INDEX_MASK;
    g_script_vars[idx] = v;

    /* Trace writes to the two end-of-stage signals so a regression in
     * the death/end-of-stage scripting is observable without a
     * debugger. */
    if (idx == VAR_INDEX_GAME_OVER && v != 0) {
        fprintf(stderr,
                "[game-over] script wrote g_script_vars[%d] = %u  "
                "(1=death 3=chapter-sel 4=stage-end)\n",
                VAR_INDEX_GAME_OVER, (unsigned)v);
    }
    if (idx == VAR_INDEX_COMPLETED) {
        fprintf(stderr,
                "[completed] g_script_vars[%d] = 0x%X "
                "(bit map of completed stages)\n",
                VAR_INDEX_COMPLETED, (unsigned)v);
    }
}

const uint16_t *vm_skip_to_endif(const uint16_t *p)
{
    /* Step past the calling instruction first. Otherwise the IF itself
     * counts as an opener and the matching ENDIF only brings depth
     * back to 0 — we'd scan past it forever. After this step,
     * depth=0 means "looking for THIS IF's matching ENDIF or ELSE". */
    {
        uint8_t initial_len = ((const uint8_t *)p)[1];
        if (initial_len == 0) return NULL;
        p += initial_len * 2;
    }

    int depth = 0;
    for (;;) {
        uint8_t op  = ((const uint8_t *)p)[0];
        uint8_t len = ((const uint8_t *)p)[1];

        if (op >= OP_IF_FIRST && op <= OP_IF_LAST) {
            ++depth;
        }
        if (op == OP_ENDIF) {
            if (depth == 0) return p;
            --depth;
        }
        if (op == OP_ELSE && depth == 0) return p;
        if (op == OP_END)                return NULL;
        if (len == 0)                    return NULL;   /* malformed */

        p += len * 2;
    }
}

const uint16_t *vm_find_label(const uint16_t *base, uint16_t id)
{
    const uint16_t *p = base;
    while (p) {
        uint8_t op  = ((const uint8_t *)p)[0];
        uint8_t len = ((const uint8_t *)p)[1];

        if (op == OP_LABEL && p[1] == id) return p;
        if (op == OP_END)                 return NULL;
        if (len == 0)                     return NULL;

        p += len * 2;
    }
    return NULL;
}
