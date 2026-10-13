/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_SDK_NET_H
#define CIUKI_SDK_NET_H
#include <ciuki/abi.h>
/* Public read-only /dev/net: open(O_RDONLY), NET_GET_STATE, close.
 * Cable carrier is independent of IP configuration or Internet access. */
int ciuki_devctl(int fd, uint32_t op, void *record, uint32_t size);
#endif
