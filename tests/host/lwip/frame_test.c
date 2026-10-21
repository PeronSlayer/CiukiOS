/* The production bridge with driver/stack endpoints only replaced.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/net.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(e) do{if(!(e)){printf("FAIL frame line %u: %s\n",__LINE__,#e);abort();}}while(0)
volatile uint64_t g_ticks;
bool g_cpu_tsc;
static struct netif devices[2];
static net_input_fn input;
static unsigned transmits, stale, attached, inputs;
static bool busy;
static unsigned char received[NET_FRAME_MAX];
unsigned net_count(void){return 2;}
struct netif *net_get(unsigned i){return i<2 ? &devices[i] : NULL;}
void kmutex_init(struct kmutex *m){memset(m,0,sizeof(*m));}
void kmutex_lock(struct kmutex *m){(void)m;}
void kmutex_unlock(struct kmutex *m){(void)m;}
void net_stack_entropy(uint64_t value){(void)value;}
int net_stack_attach(unsigned i,const uint8_t mac[6],bool link){CHECK(i<2 && mac==devices[i].address && link);attached++;return 0;}
void net_set_input(net_input_fn fn,void *cookie){CHECK(!cookie);input=fn;}
void net_stack_link(unsigned i,bool up){CHECK(i<2);(void)up;}
void net_stack_input(unsigned i,const void *data,unsigned bytes){CHECK(i<2 && bytes<=NET_FRAME_MAX);memcpy(received,data,bytes);inputs++;}
int netif_transmit(struct netif *d,gen_t generation,const void *data,unsigned bytes)
{
    if(generation!=d->generation){stale++;return -ENODEV;}
    if(busy)return -16;
    CHECK(bytes==60);memcpy(received,data,bytes);transmits++;return 0;
}
#include "../../../src/kernel/net/bridge.c"
int main(void)
{
    setvbuf(stdout,NULL,_IOLBF,0);
    for(unsigned i=0;i<2;i++){devices[i].running=true;devices[i].generation=i+1;devices[i].link=NET_LINK_UP;}
    net_stack_driver_start();CHECK(attached==2 && input);
    unsigned char frame[60];memset(frame,0x5a,sizeof(frame));
    for(unsigned i=0;i<NET_QUEUE_MAX;i++)CHECK(!net_stack_transmit(0,frame,60));
    CHECK(net_stack_transmit(0,frame,60)==-EAGAIN);memset(frame,0,sizeof(frame));
    busy=true;net_stack_driver_service();CHECK(!transmits);busy=false;
    net_stack_driver_service();CHECK(transmits==8 && received[0]==0x5a);
    net_stack_driver_service();CHECK(transmits==NET_QUEUE_MAX && !outgoing[0].count);
    CHECK(!net_stack_transmit(0,frame,60));devices[0].generation++;net_stack_driver_service();CHECK(stale==1 && !outgoing[0].count);
    CHECK(!net_stack_transmit(0,frame,60));devices[0].quarantined=true;net_stack_driver_service();CHECK(!outgoing[0].count);
    CHECK(net_stack_transmit(0,frame,60)==-ENETDOWN);devices[0].quarantined=false;
    input(&devices[1],frame,60,NULL);CHECK(inputs==1 && !received[0]);
    CHECK(net_stack_transmit(2,frame,60)==-EINVAL && net_stack_transmit(0,frame,1515)==-EINVAL);
    puts("PASS production copied bridge FIFO/full/backpressure/batch/stale generation/quarantine/ingress/bounds");return 0;
}
