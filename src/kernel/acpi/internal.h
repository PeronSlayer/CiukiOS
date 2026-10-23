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
struct ciuki_power_status;
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
    /* Kernel-only exclusive ACPI/APM provider (ciuki/power_source.h). NULL in
     * read-only and host fixtures: no reservation and no publication. reserve
     * precedes any ownership write; retire(true) keeps APM excluded. */
    int (*reserve)(void);
    int (*activate)(void);
    void (*retire)(bool uncertain);
    void (*publish)(const struct ciuki_power_status *);
    /* fx-3: the calling task's own execution time (100 ns, monotonic, does
     * not advance while the task waits to run). NULL: wall clock. */
    uint64_t (*run_clock)(void);
};
extern const struct fw_ops fw_kernel_ops;
extern const struct fw_ops *fw_os;
struct fw_table { uint32_t address, length; };
extern struct fw_table fw_tables[FW_MAX];
extern unsigned fw_table_count;
extern uint32_t fw_rsdp;
void fw_bind(const struct fw_ops *ops);
ACPI_STATUS fw_tables_discover(uint32_t rsdp);
/* fx-3: one budgeted firmware operation. `ms` is execution time of the
 * calling task (fw_ops.run_clock), not wall time; a hard wall cap of
 * max(ms, FW_WALL_CAP_MS) and the step count still bound it. Inside a scan
 * (fw_scan_begin..fw_scan_end) the whole scan also has FW_SCAN_WALL_MS. */
#define FW_WALL_CAP_MS 2000u   /* = ACPICA ACPI_MAX_SLEEP, the longest single AML wait upstream admits */
#define FW_SCAN_WALL_MS 10000u /* whole namespace scan, five operation caps */
void fw_budget_begin(uint32_t steps, uint32_t ms);
void fw_budget_stage(const char *stage, ACPI_HANDLE node);
void fw_scan_begin(void);
void fw_scan_end(void);
struct fw_budget_report {
    const char *reason;        /* "none", "run", "wall", "scan_wall", "steps", "cancel" */
    const char *stage, *first_stage;
    char node[5], first_node[5];
    uint32_t run_us, wall_us, steps, run_limit_ms, wall_cap_ms;
    uint32_t scan_run_us, scan_wall_us, first_error, semaphore_timeouts;
    bool expired, run_clock;
};
void fw_budget_snapshot(struct fw_budget_report *);
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
