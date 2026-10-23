/* SPDX-License-Identifier: GPL-2.0-only */
#include "packet_ring.h"
#include <stddef.h>
#include <ciuki/kernel.h>
_Static_assert(NDIS_RING_SLOTS==16,"bounded copied F6 queue");
void ndis_ring_init(struct ndis_packet_ring *r,struct ndis_ring_shared *s,uint32_t gen,
                     void *ctx,void (*notify)(void *),void (*complete)(void *,uint32_t,int))
{
    *r=(struct ndis_packet_ring){.shared=s,.generation=gen,.context=ctx,.notify=notify,
        .complete=complete,.live=s && gen && notify && complete};
    if(r->live) memset(s,0,sizeof(*s));
}
int ndis_ring_reserve(struct ndis_packet_ring *r,unsigned *slot,uint32_t *token)
{
    if(!r->live || !slot || !token || r->next_token==UINT32_MAX) return 0;
    for(unsigned i=0;i<NDIS_RING_SLOTS;i++) if(!r->slot[i].busy) {
        r->slot[i].busy=true; r->slot[i].token=++r->next_token;
        *slot=i; *token=r->next_token;
        r->notify(r->context); return 1;
    }
    return 0;
}
int ndis_ring_complete(struct ndis_packet_ring *r,uint32_t gen,unsigned slot,uint32_t token,int status)
{
    if(!r->live || gen!=r->generation || slot>=NDIS_RING_SLOTS || !r->slot[slot].busy ||
       !token || token!=r->slot[slot].token) return 0;
    r->slot[slot].busy=false; /* detach before callback (reentrancy) */
    r->complete(r->context,token,status); r->notify(r->context); return 1;
}
int ndis_ring_receive(struct ndis_packet_ring *r,uint8_t out[NDIS_RING_FRAME],unsigned *length)
{
    if(!r->live || !out || !length) return -1;
    *length=0;
    uint32_t published=__atomic_load_n(&r->shared->published,__ATOMIC_ACQUIRE);
    uint32_t available=published-r->consumed;
    if(available>NDIS_RING_SLOTS) return -1;
    if(!available) return 0;
    const struct ndis_ring_descriptor *p=&r->shared->descriptor[r->consumed%NDIS_RING_SLOTS];
    /* User can mutate every word at any time. Load once, validate this copy,
     * and never read a validated length/index from shared memory again. */
    struct ndis_ring_descriptor d={
        __atomic_load_n(&p->generation,__ATOMIC_RELAXED),
        __atomic_load_n(&p->token,__ATOMIC_RELAXED),
        __atomic_load_n(&p->slot,__ATOMIC_RELAXED),
        __atomic_load_n(&p->offset,__ATOMIC_RELAXED),
        __atomic_load_n(&p->length,__ATOMIC_RELAXED)};
    if(d.generation!=r->generation || d.slot>=NDIS_RING_SLOTS || !d.token ||
       !r->slot[d.slot].busy || r->slot[d.slot].token!=d.token ||
       d.offset!=offsetof(struct ndis_ring_shared,frames)+d.slot*NDIS_RING_STRIDE ||
       d.length<14 || d.length>NDIS_RING_FRAME) return -1;
    memcpy(out,(const uint8_t *)r->shared+d.offset,d.length);
    *length=d.length; r->consumed++;
    /* out belongs to the kernel. The network stack never sees process RAM. */
    return ndis_ring_complete(r,d.generation,d.slot,d.token,0)?1:-1;
}
void ndis_ring_stop(struct ndis_packet_ring *r,int error)
{
    if(!r->live) return;
    r->live=false; r->generation=0;
    for(unsigned i=0;i<NDIS_RING_SLOTS;i++) if(r->slot[i].busy) {
        uint32_t token=r->slot[i].token;
        r->slot[i].busy=false; r->complete(r->context,token,error);
    }
    r->notify(r->context);
}
