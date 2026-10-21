/* The bridge uses the same 128 entries as native Ethernet ingress. These
 * static memp pages are charged once by net_stack_init to MM_NET.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/net_packet.h>
#include "lwip/pbuf.h"

struct net_frame *net_packet_copy(const void *data, unsigned bytes)
{
    if (!data || bytes < 14 || bytes > 1514) return NULL;
    unsigned padded = bytes < 60 ? 60 : bytes;
    struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)padded, PBUF_POOL);
    if (!p) return NULL;
    /* The frozen pool has 1536-byte payloads; refuse a changed/chained pool
     * instead of exposing a partial frame or falling back to heap storage. */
    if (p->next || p->len != padded) { pbuf_free(p); return NULL; }
    memcpy(p->payload, data, bytes);
    memset((uint8_t *)p->payload + bytes, 0, padded - bytes);
    return (struct net_frame *)p;
}
uint8_t *net_packet_data(struct net_frame *frame)
{ return frame ? ((struct pbuf *)frame)->payload : NULL; }
void net_packet_free(struct net_frame *frame)
{ if (frame) pbuf_free((struct pbuf *)frame); }
