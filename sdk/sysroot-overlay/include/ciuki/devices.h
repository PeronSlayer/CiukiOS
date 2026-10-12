/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_SDK_DEVICES_H
#define CIUKI_SDK_DEVICES_H
#include <ciuki/abi.h>
/* Initialize version=CIUKI_DEVICES_VERSION and size=sizeof(*info).
 * Call 76: read-only, bounded inventory. Returns 0, or -1 with errno.
 * Resource ends are exclusive; UNKNOWN_SIZE records carry a base only.
 * See docs/design/device-manager.md. */
int ciuki_devices(struct ciuki_devices_info *info);
#endif
