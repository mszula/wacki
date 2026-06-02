/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tests/test_vm_dialog.c — dialog op smoke tests (post stubs.c link).
 *
 * Most dispatch-counter checks were removed because production
 * ScriptCallDialog* / Inventory* in stubs.c are now exercised end-to-end
 * by test_inventory.c and test_dialog_stack.c instead. What remains
 * here are SMOKE tests that just verify the VM dispatches the op
 * without crashing — useful for catching opcode-table corruption.
 *
 * Reference: src/script.c case 0x09 / 0x19 / 0x1A-0x1F / 0x52 / 0x53.
 */

#include "test.h"
#include "wacki.h"
#include "test_engine_stubs.h"

#include <stdint.h>
#include <string.h>

extern int RunScriptInterpreter(uint16_t this_id, uint16_t that_id, uint8_t *bytecode);
extern uint32_t g_script_vars[0x129];

static size_t emit(uint16_t *buf, size_t pos, uint8_t op, uint8_t len,
                    uint16_t a0, uint16_t a1, uint16_t a2)
{
    buf[pos + 0] = (uint16_t)op | (uint16_t)((uint16_t)len << 8);
    buf[pos + 1] = a0;
    if (len >= 2) {
        buf[pos + 2] = a1;
        buf[pos + 3] = a2;
    }
    return pos + (size_t)len * 2;
}

static void reset_vm(void)
{
    memset(g_script_vars, 0, sizeof g_script_vars);
    vm_stubs_reset();
    test_set_click_list(NULL, 0);
}

/* ---- op 0x19 QUEUE_DIALOG: smoke (state-local in VM) ---------------- */

TEST(op_19_queue_dialog_completes_without_crash)
{
    reset_vm();
    uint16_t prog[16] = { 0 };
    size_t pos = 0;
    prog[pos + 0] = (uint16_t)0x19 | (3 << 8);
    prog[pos + 1] = 42;
    prog[pos + 2] = 0xABCD;
    prog[pos + 3] = 0x1234;
    prog[pos + 4] = 0xEEEE;
    prog[pos + 5] = 0x5678;
    pos += 3 * 2;
    emit(prog, pos, 0x55, 1, 0, 0, 0);

    int rc = RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(rc, 1);
}

/* ---- op 0x1F DROP: smoke (production InventoryDropItem) ------------- */

TEST(op_1F_drop_does_not_crash)
{
    reset_vm();
    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x1F, 1, 0x7B, 0, 0);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    int rc = RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(rc, 1);
}

SUITE(vm_dialog)
{
    RUN_TEST(op_19_queue_dialog_completes_without_crash);
    RUN_TEST(op_1F_drop_does_not_crash);
}
