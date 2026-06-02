/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* src/vm/parser.h — bytecode scanning helpers shared inside the VM.
 *
 * These helpers walk the main script's instruction stream looking for
 * structural targets. They're used by RunScriptInterpreter (`main.c`)
 * for IF/ELSE skip and label jumps.
 *
 * Bytecode format (instruction is a u16 array; pc is `const uint16_t *`):
 * - byte +0: opcode
 * - byte +1: len in DWORDS (instruction size = len * 4 bytes = len * 2 u16s)
 * - +2 .. +6: operand u16s
 *
 * Structural opcodes referenced here:
 * ops < 6 — IF family (any condition); opens a conditional block
 * op 6 — ENDIF; closes a conditional block at the current depth
 * op 7 — ELSE; matches a still-open IF at depth 0
 * op 0x16 — LABEL; jump target for op 0x17 JUMP_LABEL etc.
 * op 0x56 — END (the only opcode that terminates a scan; op 0x55
 * END_HARD only terminates execution, NOT scanning)
 */

#ifndef WACKI_VM_PARSER_H
#define WACKI_VM_PARSER_H

#include <stdint.h>

/* Scan forward from a conditional (IF/ELSE at `p`) to its matching
 * ENDIF (op 6) or ELSE (op 7). Returns the new pc, pointing AT the
 * matching marker — the main loop steps past it with the usual
 * `pc += len * 2`. Returns NULL on 0x56 EOF or malformed bytecode. */
const uint16_t *vm_skip_to_endif(const uint16_t *p);

/* Find a LABEL (op 0x16) whose argument matches `id`. Scans from
 * `base` to the first 0x56 EOF. Returns NULL if not found. */
const uint16_t *vm_find_label(const uint16_t *base, uint16_t id);

/* Register-file accessors. The script vars array (`g_script_vars`)
 * is a 297-entry u32 register file used by every opcode that reads
 * or writes a script variable. The index is masked to keep accesses
 * in range even for malformed scripts. */
uint32_t vm_var_get(uint16_t i);
void     vm_var_set(uint16_t i, uint32_t v);

#endif /* WACKI_VM_PARSER_H */
