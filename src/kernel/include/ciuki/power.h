/* Power control: ACPI S5 soft-off discovery and system call 73.
 * Contract: docs/design/execution-abi.md, amendment 2026-10-12.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_POWER_H
#define CIUKI_POWER_H
#include <stdint.h>
#include <stdbool.h>

/* What the FADT and the \_S5 object give for a soft-off write. fx-acpioff:
 * the fixed-hardware event/GPE blocks (ACPI 2.0 5.2.8 offsets 56-93, X_ GAS
 * blocks in SystemIO when the legacy field is zero) are read as well, so the
 * fallback can clear wake status and runtime GPE enables as ACPICA does
 * (hwsleep.c acpi_hw_legacy_sleep) once the platform is in ACPI mode. */
#define ACPI_S5_SOURCE_NONE 0u
#define ACPI_S5_SOURCE_DSDT 1u
#define ACPI_S5_SOURCE_SSDT 2u
#define ACPI_S5_SOURCE_ACPICA 3u   /* evaluated by the inventory's interpreter */
struct acpi_soft_off {
    uint16_t pm1a_cnt, pm1b_cnt;  /* SystemIO ports; pm1b_cnt 0 when absent */
    uint16_t smi_cmd;             /* 0: no SCI_EN handshake is possible */
    uint8_t acpi_enable;
    uint8_t slp_typa, slp_typb;   /* 3-bit SLP_TYP values from \_S5 */
    uint8_t fadt_revision;
    uint8_t acpi_disable;
    uint8_t source;               /* ACPI_S5_SOURCE_* of slp_typa/b */
    uint8_t pm1_evt_len, pm1_cnt_len, gpe0_len, gpe1_len;
    uint16_t pm1a_evt, pm1b_evt;  /* PM1x_EVT_BLK ports, 0 when absent */
    uint16_t gpe0, gpe1;          /* GPEx_BLK ports, 0 when absent */
    uint16_t sci_int;
    uint32_t flags;               /* FADT fixed feature flags (offset 112) */
};

/* Return `len` readable bytes of physical memory at `phys`, or NULL. The
 * parser never reads outside a returned extent. */
typedef const uint8_t *(*acpi_map_fn)(void *ctx, uint32_t phys, uint32_t len);

/* 0, or a negative errno with *why naming the failed step. Checks the RSDP,
 * RSDT (or XSDT), FADT and DSDT signatures, lengths and checksums; a \_S5
 * missing from the DSDT is searched in every SSDT the root table names
 * (iPXE acpipwr.c does the same). On the "s5" failure *out already holds
 * the FADT description (source NONE) so a caller may supply the SLP_TYP
 * values from another evaluated source. */
int acpi_soft_off_parse(acpi_map_fn map, void *ctx, uint32_t rsdp_phys,
                        struct acpi_soft_off *out, const char **why);
/* Finds Name(\_S5, Package(){a, b, ...}) in an AML body. 0 or -ENOENT. */
int acpi_s5_find(const uint8_t *aml, uint32_t len, uint8_t *typa, uint8_t *typb);

/* fx-acpioff: the ACPI S5 register fallback (core/power_acpi_s5.c), used by
 * call 73 only when no active provider is qualified for Off, or after the
 * active provider's Off returned. qualified() parses the tables once
 * (read-only, bounded) and is cache-only afterwards; off() performs the
 * final write after the orderly or emergency latch and returns -EIO when
 * the machine stays on, -ENOTSUP when not qualified. */
bool power_s5_fallback_qualified(void);
const char *power_s5_fallback_state(void);
bool power_s5_fallback_data(struct acpi_soft_off *out); /* fx-selftest: parsed copy only */
int power_s5_fallback_off(bool forced);

#ifndef POWER_HOST
/* The caller is the current process; see CIUKI_POWER_* in abi.h. */
int32_t power_syscall(uint32_t action);
/* Boot diagnostic; ownership and AML preparation run on the ACPI worker. */
void power_discover(void);
#endif
#endif
