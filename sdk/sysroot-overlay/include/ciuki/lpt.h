/* Exclusive /dev/lptN: read/write and versioned LPT_* devctl records.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_SDK_LPT_H
#define CIUKI_SDK_LPT_H
#include <ciuki/abi.h>
int ciuki_devctl(int fd, uint32_t op, void *record, uint32_t size);
#endif
