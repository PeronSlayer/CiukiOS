/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_ACPI_INTERNAL_H
#define CIUKI_ACPI_INTERNAL_H
#include "acpi.h"
#include <ciuki/firmware.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define FW_MAX 64u
#define FW_TABLE_MAX (256u * 1024u)
#define FW_ALLOC_MAX (1024u * 1024u)
/* This backend is private to the serialized firmware worker. All callbacks
 * are bounded; table_map validates firmware ownership and returns RO pages.
 * access revalidates the live registry lease immediately before device I/O. */
struct fw_ops {
    void *(*alloc)(size_t);
    void (*free)(void *);
    size_t (*charge)(size_t);
    const void *(*table_map)(uint32_t, uint32_t);
    void (*table_unmap)(const void *, uint32_t);
    uint64_t (*clock)(void); /* monotonic 100 ns */
    void (*pause)(uint32_t); /* ms; zero yields */
    int (*stall)(uint32_t); /* bounded microseconds, never sleeps */
    uint64_t (*thread)(void);
    uint32_t (*lock)(void);
    void (*unlock)(uint32_t);
    ACPI_STATUS (*access)(unsigned, uint64_t, unsigned, UINT64 *, const ACPI_PCI_ID *);
    ACPI_STATUS (*write)(unsigned, uint64_t, unsigned, UINT64, const ACPI_PCI_ID *);
    ACPI_STATUS (*claim)(uint32_t, uint32_t);
    ACPI_STATUS (*sci)(uint32_t, ACPI_OSD_HANDLER, void *);
    ACPI_STATUS (*unsci)(uint32_t, ACPI_OSD_HANDLER);
    void (*finish)(bool); /* false: retain uncertain claims in quarantine */
};
extern const struct fw_ops fw_kernel_ops;
extern const struct fw_ops *fw_os;
struct fw_table { uint32_t address, length; };
extern struct fw_table fw_tables[FW_MAX];
extern unsigned fw_table_count;
extern uint32_t fw_rsdp;
void fw_bind(const struct fw_ops *ops);
ACPI_STATUS fw_tables_discover(uint32_t rsdp);
void fw_budget_begin(uint32_t steps, uint32_t ms);
void fw_budget_cancel(void);
void fw_budget_end(void);
ACPI_STATUS fw_error(void);
ACPI_STATUS fw_run_error(void);
void fw_fail(ACPI_STATUS status);
void fw_alloc_stats(uint32_t *live, uint32_t *peak);
void fw_os_cleanup(void);
ACPI_STATUS fw_inventory_run(const struct fw_ops *ops, uint32_t rsdp);
void fw_inventory_unavailable(ACPI_STATUS status);
void fw_pnp_discover(void);
/* Explicit lease token. Read-only inventory issues no boot grant; the power
 * owner admits only checked PM/EC SystemIO ranges. PCI grants name one BDF. */
int fw_region_grant(unsigned space, int handle, uint32_t generation, const ACPI_PCI_ID *pci);
bool fw_region_check(unsigned space, uint64_t address, unsigned width, const ACPI_PCI_ID *pci);
void fw_regions_reset(void);
#endif
