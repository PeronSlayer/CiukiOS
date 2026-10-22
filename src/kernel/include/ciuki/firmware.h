/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_FIRMWARE_H
#define CIUKI_FIRMWARE_H
#include <ciuki/firmware_abi.h>
void firmware_inventory_start(void);
int firmware_inventory_page(struct ciuki_firmware_page *page);
#endif
