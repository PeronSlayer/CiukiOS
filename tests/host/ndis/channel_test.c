/* Hostile shared packet/doorbell fixture. SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include "src/kernel/ndis/channel.h"
static struct ndis_channel c;
static struct ndis_channel_shared s;
static unsigned host,worker,completed,errors;
static void h(void *ctx) { assert(ctx==&c);host++; }
static void w(void *ctx) { assert(ctx==&c);worker++; }
static void done(void *ctx,uint32_t token,int status) { assert(ctx==&c && token);completed++;if(status) errors++; }
int main(void)
{
    assert(ndis_channel_init(&c,&s,7,&c,h,w,done));
    assert(host);uint32_t epoch=c.epoch;
    assert(ndis_channel_wait(&c,7,epoch));
    uint8_t frame[1514]={0x42},out[1514];uint32_t token[16];unsigned length;
    for(unsigned i=0;i<16;i++) assert(ndis_channel_send(&c,frame,14,&token[i])==1);
    assert(!ndis_channel_wait(&c,7,epoch)); /* publication before park */
    assert(ndis_channel_send(&c,frame,14,&epoch)==0);
    assert(s.tx.descriptor[0].length==60 && s.tx.frames[0][14]==0);
    assert(!ndis_channel_tx_complete(&c,8,0,token[0],0));
    assert(ndis_channel_tx_complete(&c,7,1,token[1],0));
    assert(ndis_channel_send(&c,frame,14,&epoch)==0); /* FIFO descriptor still busy */
    assert(ndis_channel_tx_complete(&c,7,0,token[0],0));
    assert(!ndis_channel_tx_complete(&c,7,0,token[0],0));
    assert(ndis_channel_send(&c,frame,14,&epoch)==1 && epoch!=token[0]);
    s.rx.descriptor[0]=(struct ndis_ring_descriptor){7,s.rx_credit[3],3,
        offsetof(struct ndis_ring_shared,frames)+3*NDIS_RING_STRIDE,14};
    memcpy(s.rx.frames[3],frame,14);__atomic_store_n(&s.rx.published,1,__ATOMIC_RELEASE);
    assert(ndis_channel_doorbell(&c,7) && worker && c.worker_pending);
    assert(!ndis_channel_doorbell(&c,8));
    uint32_t old=s.rx_credit[3];assert(ndis_channel_receive(&c,out,&length)==1 && length==14 && out[0]==0x42);
    assert(s.rx_credit[3]!=old && !ndis_channel_receive(&c,out,&length));
    s.rx.descriptor[1]=s.rx.descriptor[0];s.rx.published=2; /* retired token replay */
    assert(ndis_channel_receive(&c,out,&length)==-1);
    c.epoch=UINT32_MAX;assert(!ndis_channel_wait(&c,7,UINT32_MAX));
    ndis_channel_stop(&c,-1);assert(completed==17 && errors==15 && !c.live);
    ndis_channel_stop(&c,-1);assert(completed==17 && !ndis_channel_tx_complete(&c,7,0,epoch,0));
    puts("PASS two packet directions, padding, backpressure, credits, epochs, replay and teardown");fflush(stdout);return 0;
}
