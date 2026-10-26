/* Internal hardware sweep/reset seams; no process or boot-info ABI changes.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_REBOOT_H
#define CIUKI_REBOOT_H
#include <stdint.h>
#include <stdbool.h>

struct reboot_ops {
    void (*disable)(void *);
    uint8_t (*status)(void *);
    void (*pulse)(void *);
    void (*delay)(void *);
    void (*triple)(void *);
    /* Optional (T23 c42f556e, review 3 finding 3): a qualified hard PCI
     * reset (ICH/PIIX reset control register 0CF9h: SYS_RST then RST_CPU,
     * PCIRST# asserted; not FULL_RST) tried after the 8042 pulse and before
     * the CPU-only triple fault, on Intel ISA/LPC bridges only. Returns
     * whether it was written (unqualified: false, nothing written). */
    bool (*hard)(void *);
};
/* Bounded even when the controller is missing or its input buffer is stuck. */
void reboot_sequence(const struct reboot_ops *, void *);
/* The qualified hard PCI reset alone (review 5, finding 1): for a forced
 * restart whose device fence left DMA unresolved, PCIRST# is the only safe
 * reset; the CPU-only steps are never taken. Returns false when unqualified
 * (nothing written) and true when written but the machine is still running. */
bool reboot_pci_reset(const struct reboot_ops *, void *);
__attribute__((noreturn)) void kernel_reboot(void);
/* -ENOTSUP without a qualified bridge, -EIO when the written reset did not
 * take; both leave every resource as it is (the user resets the machine). */
int kernel_reboot_pci_only(void);

struct storage;
/* Only the existing boot-volume SYSTEM/BOOT.CFG is writable through this seam.
 * NULL removes its probe line. Other options are preserved, total <=127 bytes. */
int storage_boot_cfg(struct storage *, const char *request);

struct probe_selection;
struct probe_tables;
struct probe_hooks;
struct sweep_cursor { unsigned phase, step; uint32_t state; char run[9]; };
struct sweep_step { unsigned phase, index, boot; const char *name; bool panic; };
#define SWEEP_PENDING (1u << 18)
#define SWEEP_ALL_PHASE 4u /* phase 3 is DOS; cursor metadata is internal */
bool sweep_parse(const char *, unsigned, struct sweep_cursor *);
unsigned sweep_count(unsigned, const struct probe_tables *);
bool sweep_at(unsigned, unsigned, const struct probe_tables *, struct sweep_step *);
int sweep_dispatch(const struct sweep_step *, const struct sweep_cursor *,
                   const struct probe_tables *, const struct probe_hooks *);
#endif
