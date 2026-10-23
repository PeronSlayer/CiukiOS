/* Two copied packet directions and scalar doorbells. Kernel-private state.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_NDIS_CHANNEL_H
#define CIUKI_NDIS_CHANNEL_H
#include "packet_ring.h"
struct ndis_channel_shared {
    struct ndis_ring_shared rx,tx;
    uint32_t rx_credit[NDIS_RING_SLOTS];
    uint32_t callback[NDIS_RING_SLOTS][7];
    uint32_t irq_result[2];
};
struct ndis_channel {
    struct ndis_channel_shared *shared;
    struct ndis_packet_ring rx,tx;
    uint32_t epoch,tx_published;
    bool live,worker_pending;
    void *context;
    void (*wake_host)(void *);
    void (*wake_worker)(void *);
    void (*tx_complete)(void *,uint32_t,int);
};
int ndis_channel_init(struct ndis_channel *,struct ndis_channel_shared *,uint32_t,void *,
                       void (*host)(void *),void (*worker)(void *),void (*complete)(void *,uint32_t,int));
int ndis_channel_send(struct ndis_channel *,const uint8_t *,unsigned,uint32_t *token);
int ndis_channel_receive(struct ndis_channel *,uint8_t *,unsigned *);
int ndis_channel_tx_complete(struct ndis_channel *,uint32_t,unsigned,uint32_t,int);
int ndis_channel_doorbell(struct ndis_channel *,uint32_t generation);
/* Caller holds the same UP exclusion as notification and task parking. */
bool ndis_channel_wait(struct ndis_channel *,uint32_t generation,uint32_t observed_epoch);
void ndis_channel_stop(struct ndis_channel *,int);
#endif
