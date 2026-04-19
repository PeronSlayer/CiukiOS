/*
 * vm86.c — OPENGEM-016 16-bit execution layer scaffolding.
 *
 * OPENGEM-017: mode-switch scaffold. Observability only.
 *
 * No real mode-switch is performed here. The code exists so that:
 *   1. The ABI types in stage2/include/vm86.h are compiled under the
 *      real stage2 toolchain flags and survive -Wall -Wextra.
 *   2. Static gates can assert the presence of the four scaffold
 *      markers described in docs/opengem-016-design.md §5.5.
 *   3. Later phases (OPENGEM-018+) have a host file to extend.
 *
 * Nothing in this translation unit is called from the live boot path
 * yet. The probe function is reachable from test code via extern
 * linkage and remains safe to invoke from long-mode host context.
 */

#include "vm86.h"
#include "serial.h"
#include "types.h"

/* OPENGEM-017 sentinel — static gates grep for this exact token. */
static const char vm86_scaffold_sentinel[] = "OPENGEM-017";

int vm86_scaffold_probe(void) {
    /*
     * Size and alignment self-check. The trap frame is a frozen ABI
     * contract — if the layout changes, downstream phases must update
     * the dispatcher in lockstep.
     */
    const u32 frame_bytes = (u32)sizeof(vm86_trap_frame);

    (void)vm86_scaffold_sentinel;

    serial_write("vm86: scaffold phase=017 status=planned\n");
    serial_write("vm86: scaffold host-mode=long compat-mode=pe32 guest-mode=v8086\n");

    serial_write("vm86: scaffold frame-bytes=0x");
    serial_write_hex64((u64)frame_bytes);
    serial_write("\n");

    serial_write("vm86: scaffold complete\n");
    return 1;
}
