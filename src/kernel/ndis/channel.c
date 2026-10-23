/* SPDX-License-Identifier: GPL-2.0-only */
#include "channel.h"
#include <ciuki/kernel.h>
#include <stddef.h>
static void wake(void *ctx)
{
    struct ndis_channel *c=ctx;
    /* Epoch exhaustion closes the wait predicate, never wraps into lost wake. */
    if(c->epoch!=UINT32_MAX) c->epoch++;
    c->wake_host(c->context);
}
static void rx_complete(void *ctx,uint32_t token,int status)
{ (void)ctx;(void)token;(void)status; }
static void tx_complete(void *ctx,uint32_t token,int status)
{ struct ndis_channel *c=ctx;c->tx_complete(c->context,token,status); }
static int refill(struct ndis_channel *c)
{
    for(unsigned i=0;i<NDIS_RING_SLOTS;i++) if(!c->rx.slot[i].busy) {
        unsigned slot;uint32_t token;
        if(!ndis_ring_reserve(&c->rx,&slot,&token)) return 0;
        __atomic_store_n(&c->shared->rx_credit[slot],token,__ATOMIC_RELEASE);
    }
    wake(c);return 1;
}
int ndis_channel_init(struct ndis_channel *c,struct ndis_channel_shared *s,uint32_t gen,
                       void *ctx,void (*host)(void *),void (*worker)(void *),void (*complete)(void *,uint32_t,int))
{
    if(!c || !s || !gen || !host || !worker || !complete) return 0;
    *c=(struct ndis_channel){.shared=s,.live=true,.context=ctx,.wake_host=host,.wake_worker=worker,.tx_complete=complete};
    memset(s,0,sizeof(*s));
    ndis_ring_init(&c->rx,&s->rx,gen,c,wake,rx_complete);
    ndis_ring_init(&c->tx,&s->tx,gen,c,wake,tx_complete);
    return refill(c);
}
int ndis_channel_send(struct ndis_channel *c,const uint8_t *frame,unsigned length,uint32_t *token)
{
    if(!c || !c->live || !frame || !token || length<14 || length>NDIS_RING_FRAME ||
       c->tx.next_token==UINT32_MAX) return -1;
    /* The ring descriptor at the next publication index cannot be reused
     * until its previous token completed, even for out-of-order completions. */
    unsigned slot=c->tx_published%NDIS_RING_SLOTS;
    if(c->tx.slot[slot].busy) return 0;
    uint32_t id=++c->tx.next_token;c->tx.slot[slot].token=id;c->tx.slot[slot].busy=true;
    unsigned wire=length<60?60:length;
    memcpy(c->shared->tx.frames[slot],frame,length);
    if(wire>length) memset(c->shared->tx.frames[slot]+length,0,wire-length);
    c->shared->tx.descriptor[slot]=(struct ndis_ring_descriptor){c->tx.generation,id,slot,
        offsetof(struct ndis_ring_shared,frames)+slot*NDIS_RING_STRIDE,wire};
    __atomic_store_n(&c->shared->tx.published,++c->tx_published,__ATOMIC_RELEASE);
    *token=id;wake(c);return 1;
}
int ndis_channel_receive(struct ndis_channel *c,uint8_t *out,unsigned *length)
{
    if(!c || !c->live) return -1;
    int result=ndis_ring_receive(&c->rx,out,length);
    if(result>0 && !refill(c)) return -1;
    return result;
}
int ndis_channel_tx_complete(struct ndis_channel *c,uint32_t gen,unsigned slot,uint32_t token,int status)
{
    if(!c || !c->live || (status!=0 && status!=-1)) return 0;
    return ndis_ring_complete(&c->tx,gen,slot,token,status);
}
int ndis_channel_doorbell(struct ndis_channel *c,uint32_t generation)
{
    if(!c || !c->live || c->rx.generation!=generation) return 0;
    c->worker_pending=true;c->wake_worker(c->context);return 1;
}
bool ndis_channel_wait(struct ndis_channel *c,uint32_t generation,uint32_t observed)
{ return c && c->live && c->rx.generation==generation && c->epoch!=UINT32_MAX && observed==c->epoch; }
void ndis_channel_stop(struct ndis_channel *c,int error)
{
    if(!c || !c->live) return;
    c->live=false;c->worker_pending=false;
    ndis_ring_stop(&c->tx,error);ndis_ring_stop(&c->rx,error);
    wake(c);c->wake_worker(c->context);
}
