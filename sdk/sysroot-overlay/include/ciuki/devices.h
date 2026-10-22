/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_SDK_DEVICES_H
#define CIUKI_SDK_DEVICES_H
#include <ciuki/abi.h>
#include <ciuki/firmware_abi.h>
/* Initialize version=CIUKI_DEVICES_VERSION and size=sizeof(*info).
 * Call 76: read-only, bounded inventory. Returns 0, or -1 with errno.
 * Resource ends are exclusive; UNKNOWN_SIZE records carry a base only.
 * See docs/design/device-manager.md. */
int ciuki_devices(struct ciuki_devices_info *info);
/* Version 2, exact sizeof(*page), cursor/generation paging; same call 76.
 * No firmware calls occur during this query. */
int ciuki_firmware_inventory(struct ciuki_firmware_page *page);
/* Version 3, exact sizeof(*info); read-only SMBIOS/map/CPUID identity and
 * a bounded boot-log snapshot. No firmware evaluation or device activation. */
int ciuki_hardware_inventory(struct ciuki_hardware_info *info);
#endif
