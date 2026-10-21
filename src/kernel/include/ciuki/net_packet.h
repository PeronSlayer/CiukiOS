/* Shared f6-10 PBUF_POOL ownership, without leaking lwIP types to VMs.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_NET_PACKET_H
#define CIUKI_NET_PACKET_H
#include <ciuki/kernel.h>
struct net_frame;
/* One contiguous, private copy. NULL on exhaustion; no heap fallback.
 * Caller owns the returned frame until free. Payload is borrowed from it. */
struct net_frame *net_packet_copy(const void *, unsigned);
uint8_t *net_packet_data(struct net_frame *);
void net_packet_free(struct net_frame *);
#endif
