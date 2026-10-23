/* fx-18: the bounded chipset I/O window the real-mode APM BIOS may use
 * while the BIOS VM runs an APM call (docs/design/acpi-events-power.md,
 * "APM when the OS does not own ACPI"). Kernel-only; no ABI record.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_APM_PORTS_H
#define CIUKI_APM_PORTS_H
#include <stdint.h>
#include <stdbool.h>

/* FADT fixed-hardware SystemIO blocks; 0 = absent or not SystemIO. */
struct apm_pm_blocks {
    uint16_t pm1a_evt, pm1b_evt, pm1a_cnt, pm1b_cnt, pm2_cnt, pm_tmr, gpe0, gpe1, smi_cmd;
    uint8_t pm1_evt_len, pm1_cnt_len, pm2_cnt_len, pm_tmr_len, gpe0_len, gpe1_len;
};
enum apm_port_kind {
    APM_PORT_NONE, APM_PORT_APMC, APM_PORT_PM1_EVT, APM_PORT_PM1_CNT, APM_PORT_PM2_CNT,
    APM_PORT_PM_TMR, APM_PORT_GPE, APM_PORT_CHIPSET, APM_PORT_DELAY
};
struct apm_port_range { uint16_t first, last; uint8_t kind; };
#define APM_PORT_RANGES 16u
#define APM_PORT_SOURCE_FADT 1u
#define APM_PORT_SOURCE_CHIPSET 2u
struct apm_port_window {
    uint8_t count, source;
    struct apm_port_range ranges[APM_PORT_RANGES];
};
enum apm_port_verdict { APM_PORT_DENY, APM_PORT_ALLOW, APM_PORT_ABSORB };

/* B2h/B3h, the FADT blocks when given, the chipset PMBASE block (read
 * only) when chipset_len is nonzero, and port 80h. Specific blocks precede
 * the chipset window so the first containing range decides. */
void apm_port_window_build(struct apm_port_window *, const struct apm_pm_blocks *fadt,
                           uint16_t chipset_base, uint16_t chipset_len);
/* Pure decision for one CPU access [port, port+width-1] during APM
 * function `function` (AX). *reason names a denial; *kind the range. */
enum apm_port_verdict apm_port_check(const struct apm_port_window *, uint16_t port, unsigned width,
                                     bool write, uint32_t value, uint16_t function,
                                     uint8_t *kind, const char **reason);
const char *apm_port_kind_name(uint8_t kind);
/* ACPI layer: the FADT blocks captured by the inventory (0, or -ENOENT
 * without tables), and the legacy-EC hold around one APM BIOS call
 * (acpi/ec.c: 0 held or nothing to exclude, -EAGAIN busy at the deadline). */
int acpi_pm_blocks(struct apm_pm_blocks *);
int acpi_ec_firmware_hold(unsigned ms);
void acpi_ec_firmware_release(void);
#endif
