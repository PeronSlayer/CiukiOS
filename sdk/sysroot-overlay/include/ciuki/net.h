/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_SDK_NET_H
#define CIUKI_SDK_NET_H
#include <ciuki/abi.h>
/* Public read-only /dev/net: open(O_RDONLY), NET_GET_STATE, close.
 * Cable carrier is independent of IP configuration or Internet access. */
int ciuki_devctl(int fd, uint32_t op, void *record, uint32_t size);
/* Existing /dev/net NET_GET_STATE helper: opens read-only, initializes the
 * version/size, queries and closes. present/link/state/address_configured
 * describe local availability; syscall 90 remains reserved ENOSYS. */
int ciuki_net_status(struct ciuki_net_state *);
#endif
