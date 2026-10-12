/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_KERNEL_DEVICES_H
#define CIUKI_KERNEL_DEVICES_H
#include <ciuki/abi.h>
/* No allocation, waiting or device I/O. Caller owns the large output buffer. */
void devices_snapshot(struct ciuki_devices_info *out);
int devices_query(uint32_t address, uint32_t bytes);
#endif
