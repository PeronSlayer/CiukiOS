/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "src/kernel/ndis/packet_ring.h"
static struct ndis_ring_shared shared;
static struct ndis_packet_ring ring;
static unsigned completed,notified;
static uint32_t last;
static void notify(void *c) { (void)c;notified++; }
static void done(void *c,uint32_t token,int status) { (void)c;(void)status;assert(token);completed++;last=token; }
static void setup(void) { completed=notified=0;last=0;ndis_ring_init(&ring,&shared,77,0,notify,done); }
static void publish(unsigned slot,uint32_t token,unsigned length)
{
    uint32_t p=shared.published;
    shared.descriptor[p%NDIS_RING_SLOTS]=(struct ndis_ring_descriptor){77,token,slot,
        offsetof(struct ndis_ring_shared,frames)+slot*NDIS_RING_STRIDE,length};
    memset(shared.frames[slot],0xab,NDIS_RING_FRAME);
    __atomic_store_n(&shared.published,p+1,__ATOMIC_RELEASE);
}
int main(void)
{
    uint8_t frame[NDIS_RING_FRAME];unsigned slot,length;uint32_t token;
    setup();assert(ndis_ring_receive(&ring,frame,&length)==0);
    for(unsigned i=0;i<16;i++) assert(ndis_ring_reserve(&ring,&slot,&token));
    assert(!ndis_ring_reserve(&ring,&slot,&token));ndis_ring_stop(&ring,-1);
    assert(completed==16);ndis_ring_stop(&ring,-1);assert(completed==16);
    assert(!ndis_ring_complete(&ring,77,slot,token,0));
    setup();assert(ndis_ring_reserve(&ring,&slot,&token));publish(slot,token,1514);
    assert(ndis_ring_receive(&ring,frame,&length)==1 && length==1514 && completed==1 && last==token);
    memset(shared.frames[slot],0xcd,NDIS_RING_FRAME);assert(frame[0]==0xab && frame[1513]==0xab);
    assert(!ndis_ring_complete(&ring,77,slot,token,0));
    assert(ndis_ring_reserve(&ring,&slot,&token));assert(token==2);
    assert(!ndis_ring_complete(&ring,78,slot,token,0));
    assert(!ndis_ring_complete(&ring,77,slot,1,0));
    assert(ndis_ring_complete(&ring,77,slot,token,0));
    for(unsigned bad=0;bad<8;bad++) {
        setup();assert(ndis_ring_reserve(&ring,&slot,&token));publish(slot,token,100);
        struct ndis_ring_descriptor *d=&shared.descriptor[0];
        switch(bad) {
        case 0:d->generation++;break;
        case 1:d->token++;break;
        case 2:d->slot=UINT32_MAX;break;
        case 3:d->offset=UINT32_MAX;break;
        case 4:d->length=1515;break;
        case 5:d->length=13;break;
        case 6:shared.published=17;break;
        case 7:shared.published=UINT32_MAX;break;
        }
        assert(ndis_ring_receive(&ring,frame,&length)==-1 && !completed);
        ndis_ring_stop(&ring,-1);assert(completed==1);
    }
    setup();ring.consumed=shared.published=UINT32_MAX;
    assert(ndis_ring_reserve(&ring,&slot,&token));publish(slot,token,14);
    assert(ndis_ring_receive(&ring,frame,&length)==1 && !ring.consumed);
    ring.next_token=UINT32_MAX;assert(!ndis_ring_reserve(&ring,&slot,&token));
    /* Repeated wraps, delayed duplicate descriptors and full bounded queues. */
    setup();
    for(unsigned batch=0;batch<2000;batch++) {
        for(unsigned i=0;i<16;i++) { assert(ndis_ring_reserve(&ring,&slot,&token));publish(slot,token,14+i); }
        for(unsigned i=0;i<16;i++) assert(ndis_ring_receive(&ring,frame,&length)==1);
        assert(ndis_ring_receive(&ring,frame,&length)==0);
    }
    assert(completed==32000);
    puts("PASS hostile shared ring snapshots, fixed offsets, copied RX, backpressure, stale/duplicate tokens, wrap, teardown");
    fflush(stdout);return 0;
}
