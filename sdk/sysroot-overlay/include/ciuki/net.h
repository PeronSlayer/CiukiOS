/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_SDK_NET_H
#define CIUKI_SDK_NET_H
#include <ciuki/abi.h>
#include <stddef.h>
/* Public read-only /dev/net: open(O_RDONLY), NET_GET_STATE, close.
 * Cable carrier is independent of IP configuration or Internet access. */
int ciuki_devctl(int fd, uint32_t op, void *record, uint32_t size);
/* Existing /dev/net NET_GET_STATE helper: opens read-only, initializes the
 * version/size, queries and closes. present/link/state/address_configured
 * describe local availability. */
int ciuki_net_status(struct ciuki_net_state *);
/* DNS v2: start fills a process-owned token; poll returns status 4 while pending.
 * Cancel invalidates that token immediately; late upstream results are discarded. */
/* LMHOSTS then local-subnet NBNS; suffix is the NetBIOS service byte. */
int ciuki_netbios_resolve(const char *, unsigned, uint32_t *);
/* HOSTS then PTR DNS, no numeric fallback. */
int ciuki_hosts_reverse4(uint32_t, char *, size_t);
int ciuki_reverse4(uint32_t, char *, size_t);
int ciuki_resolve4_query(struct ciuki_resolve_query *);
/* Numeric/HOSTS/localhost first; otherwise starts a cancellable DNS query. */
int ciuki_resolve4_begin(const char *, struct ciuki_resolve_query *);
/* Privileged configuration; returns 0 or -1 with errno. */
int ciuki_net_control(unsigned operation, struct ciuki_net_control *);
/* Read-only CONFIG, SOCKETS, STATS or ROUTES snapshot; no capability.
 * operation is CIUKI_NET_INFO_*; bytes must match the corresponding record. */
int ciuki_net_info(unsigned operation, void *record, uint32_t bytes);
#endif
