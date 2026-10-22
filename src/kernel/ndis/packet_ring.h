/* Untrusted process publications; all cursors and ownership below are private.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_NDIS_PACKET_RING_H
#define CIUKI_NDIS_PACKET_RING_H
#include <stdint.h>
#include <stdbool.h>
#define NDIS_RING_SLOTS 16u
#define NDIS_RING_FRAME 1514u
#define NDIS_RING_STRIDE 1536u
struct ndis_ring_descriptor { uint32_t generation, token, slot, offset, length; };
struct ndis_ring_shared {
    uint32_t published;
    struct ndis_ring_descriptor descriptor[NDIS_RING_SLOTS];
    uint8_t frames[NDIS_RING_SLOTS][NDIS_RING_STRIDE];
};
struct ndis_packet_ring {
    struct ndis_ring_shared *shared; /* pinned fixed mapping; no user pointer */
    uint32_t generation, consumed, next_token;
    struct { uint32_t token; bool busy; } slot[NDIS_RING_SLOTS];
    bool live;
    void *context;
    void (*notify)(void *);
    void (*complete)(void *,uint32_t token,int status);
};
void ndis_ring_init(struct ndis_packet_ring *,struct ndis_ring_shared *,uint32_t,
                     void *,void (*notify)(void *),void (*complete)(void *,uint32_t,int));
/* Kernel gives one fixed slot to the host. No free slots = backpressure.
 * Token is monotonic per generation, never reused or wrapped. */
int ndis_ring_reserve(struct ndis_packet_ring *,unsigned *slot,uint32_t *token);
/* Consume a bounded snapshot, copy data before retiring the accepted token.
 * 1 frame, 0 empty/backpressure, -1 hostile publication. */
int ndis_ring_receive(struct ndis_packet_ring *,uint8_t output[NDIS_RING_FRAME],unsigned *length);
int ndis_ring_complete(struct ndis_packet_ring *,uint32_t generation,unsigned slot,uint32_t token,int status);
void ndis_ring_stop(struct ndis_packet_ring *,int error);
#endif
