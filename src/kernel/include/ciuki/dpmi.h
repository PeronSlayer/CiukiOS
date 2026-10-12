/* Kernel-owned DPMI 0.9 host for DOS VMs (directive f3-04). Kernel-internal:
 * no native ABI. Clients run at ring 3, IOPL 0, in the VM's address space
 * with the VM's LDT; INT 31h arrives as #GP and never shares the INT 80h path.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_DPMI_H
#define CIUKI_DPMI_H
#include <ciuki/v86.h>

/* INT 2Fh/1687h is answered only for VMs created with dosvm_config.dpmi
 * until the complete F3 DPMI row passes on QEMU; then the lead sets this
 * to 1 (directive f3-04, stage 1; f3-acceptance.md, f3-04). */
#define DOSVM_DPMI_PUBLIC 0

/* INT 31h rows observed by the host (bit set = the row returned to the
 * client at least once). Index = dpmi_row(AX); refusals count as rows. */
enum { DPMI_ROW_WORDS = 4 };
struct dpmi_stats {
    uint32_t rows_ok[DPMI_ROW_WORDS], rows_fail[DPMI_ROW_WORDS];
    uint32_t entries, entry_failures, terminations, faults;
    uint32_t irqs_pm, irqs_reflected, soft_reflected, exceptions, callbacks;
    uint32_t raw_switches, realcalls, max_depth, hostcalls, stale;
    uint32_t if_armed, if_restored;     /* f3-06: flags images armed at CLI, POPF/IRET restores seen */
};
struct dpmi_info {
    bool active, bits32;
    uint32_t clients, selectors, blocks, dos_blocks, callbacks, watchpoints, depth;
    uint32_t pool_used, hstack;
    struct dpmi_stats stats;
};
struct dosvm_key;
bool dosvm_dpmi_info(struct dosvm_key, struct dpmi_info *);
int dpmi_row(unsigned ax);              /* -1: not an INT 31h 0.9/1.0 row */

#endif
