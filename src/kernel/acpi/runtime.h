/* Generic OSPM owner. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_ACPI_RUNTIME_H
#define CIUKI_ACPI_RUNTIME_H
#include "internal.h"
#include <ciuki/abi.h>
extern struct ciuki_power_status fw_power;
extern bool fw_runtime, fw_writes, fw_owned;
extern bool fw_interrupt;
ACPI_STATUS fw_eval(ACPI_HANDLE, char *, ACPI_OBJECT_LIST *, ACPI_BUFFER *);
ACPI_STATUS fw_integer(ACPI_HANDLE, char *, uint32_t *);
bool fw_has(ACPI_HANDLE, char *);
ACPI_STATUS fw_power_takeover(void);
void fw_power_poll(void);
void fw_power_stop(void);
void fw_power_request(void);
ACPI_STATUS fw_sources_add(ACPI_HANDLE, uint32_t, uint32_t);
void fw_sources_refresh(void);
ACPI_STATUS fw_thermal_add(ACPI_HANDLE, uint32_t);
ACPI_STATUS fw_thermal_refresh(bool);
void fw_classes_reset(void);
void fw_thermal_reset(void);
ACPI_STATUS fw_ec_add(ACPI_HANDLE);
ACPI_STATUS fw_ec_enable(void);
void fw_ec_poll(void);
void fw_ec_reset(void);
bool fw_ec_ready(void);
bool fw_ec_idle(void);
bool fw_ec_faulted(void);
int acpi_ec_quirk_transfer(bool, uint8_t, uint8_t *);
int acpi_ec_quirk_transfer_counted(bool, uint8_t, uint8_t *, unsigned, unsigned *);
bool acpi_power_can_off(void);
int acpi_power_off_owned(void);
int acpi_power_snapshot(struct ciuki_power_status *);
void fw_power_publish(void);
void fw_queue_drain(unsigned);
ACPI_STATUS fw_inventory_power_run(const struct fw_ops *, uint32_t);
/* Called by inventory while its handles are still live. */
ACPI_STATUS fw_power_collect(ACPI_HANDLE, uint32_t, uint32_t, bool);
#endif
