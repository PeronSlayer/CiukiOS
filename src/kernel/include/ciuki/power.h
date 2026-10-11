/* Power control: ACPI S5 soft-off discovery and system call 73.
 * Contract: docs/design/execution-abi.md, amendment 2026-10-12.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_POWER_H
#define CIUKI_POWER_H
#include <stdint.h>
#include <stdbool.h>

/* What the FADT and the DSDT \_S5 object give for a soft-off write. */
struct acpi_soft_off {
    uint16_t pm1a_cnt, pm1b_cnt;  /* SystemIO ports; pm1b_cnt 0 when absent */
    uint16_t smi_cmd;             /* 0: no SCI_EN handshake is possible */
    uint8_t acpi_enable;
    uint8_t slp_typa, slp_typb;   /* 3-bit SLP_TYP values from \_S5 */
    uint8_t fadt_revision;
};

/* Return `len` readable bytes of physical memory at `phys`, or NULL. The
 * parser never reads outside a returned extent. */
typedef const uint8_t *(*acpi_map_fn)(void *ctx, uint32_t phys, uint32_t len);

/* 0, or a negative errno with *why naming the failed step. Checks the RSDP,
 * RSDT (or XSDT), FADT and DSDT signatures, lengths and checksums. */
int acpi_soft_off_parse(acpi_map_fn map, void *ctx, uint32_t rsdp_phys,
                        struct acpi_soft_off *out, const char **why);
/* Finds Name(\_S5, Package(){a, b, ...}) in an AML body. 0 or -ENOENT. */
int acpi_s5_find(const uint8_t *aml, uint32_t len, uint8_t *typa, uint8_t *typb);

#ifndef POWER_HOST
/* The caller is the current process; see CIUKI_POWER_* in abi.h. */
int32_t power_syscall(uint32_t action);
/* Normal boot: parse the tables once and log the soft-off facts. */
void power_discover(void);
#endif
#endif
