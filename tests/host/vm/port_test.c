/* Selected physical boundary and persistent installation identity.
 * SPDX-License-Identifier: GPL-2.0-only */
#define main unused_nic_suite_main
#include "../net/net_test.c"
#undef main
#include "../../../src/kernel/net/bridge_port.c"
static unsigned reads,writes,randoms,modes;
static int read_error,write_error,random_error,mode_error;
static bool zero_id;
static uint8_t stored[16];
static int identity_read(void *arg,uint8_t id[16])
{ (void)arg; reads++; memcpy(id,stored,16); return read_error; }
static int identity_write(void *arg,const uint8_t id[16])
{ (void)arg; writes++; if(!write_error) memcpy(stored,id,16); return write_error; }
static int identity_random(void *arg,uint8_t id[16])
{ (void)arg; randoms++; memset(id,zero_id ? 0 : 0x42,16); return random_error; }
static int mode(struct netif *d,bool enabled)
{ CHECK(d==&devices[0]); modes+=enabled?1:10; return mode_error; }
#ifndef PORT_TEST_MAIN
#define PORT_TEST_MAIN main
#endif
int PORT_TEST_MAIN(void)
{
    struct bridge_identity_ops identity={0,identity_read,identity_write,identity_random};
    uint8_t id[16],zero[16]={0};
    read_error=-2; CHECK(!net_bridge_identity_load(&identity,id));
    CHECK(reads==1 && writes==1 && randoms==1 && !memcmp(stored,id,16));
    read_error=0; CHECK(!net_bridge_identity_load(&identity,id)); CHECK(writes==1 && randoms==1);
    memset(stored,0,16); CHECK(net_bridge_identity_load(&identity,id)==-EINVAL && !memcmp(id,zero,16));
    read_error=-5; CHECK(net_bridge_identity_load(&identity,id)==-5 && !memcmp(id,zero,16));
    read_error=-2; random_error=-11; CHECK(net_bridge_identity_load(&identity,id)==-11 && !memcmp(id,zero,16));
    random_error=0; write_error=-5; CHECK(net_bridge_identity_load(&identity,id)==-5 && !memcmp(id,zero,16));
    CHECK(writes==2); write_error=0; zero_id=true;
    CHECK(net_bridge_identity_load(&identity,id)==-EINVAL && writes==2); zero_id=false;
    CHECK(!net_bridge_identity_load(&identity,id));
    struct net_bridge_port port={0}; static struct bridge_pool pool;
    struct netif *d=&devices[0]; d->generation=77; d->ready=d->running=true; d->link=NET_LINK_UP;
    kmutex_init(&registration_mutex);
    CHECK(!net_bridge_port_open(&port,&pool,d,1,id,0,0,mode));
    CHECK(port.registered && port.bridge.link);
    CHECK(net_bridge_port_open(&port,&pool,d,1,id,0,0,mode)==-EINVAL);
    struct bridge_key key={1,12}; uint8_t mac[6];
    CHECK(net_bridge_port_acquire(&port)); CHECK(!net_bridge_port_acquire(&port));
    CHECK(!net_bridge_enable(&port.bridge,key,0,mac) && modes==1);
    net_bridge_port_release(&port);
    CHECK(net_bridge_port_close(&port)==-16 && port.registered);
    uint8_t frame[64]={0}; memset(frame,255,6); frame[6]=2; frame[7]=99;
    frame[12]=0x81; frame[13]=0x37; frame[14]=frame[15]=255; frame[17]=50;
    memcpy(frame+24,frame,6); memcpy(frame+36,frame+6,6);
    frame_tap(d,frame,64,&port); CHECK(pool.used==1);
    CHECK(net_bridge_port_acquire(&port)); frame_tap(d,frame,64,&port);
    CHECK(port.busy_drops==1); net_bridge_port_release(&port);
    d->link=NET_LINK_DOWN; net_bridge_port_carrier(&port); CHECK(!pool.used && !port.bridge.link);
    CHECK(net_bridge_port_acquire(&port)); net_bridge_disable(&port.bridge,key); net_bridge_port_release(&port);
    CHECK(modes==11 && !port.bridge.promiscuous);
    CHECK(!net_bridge_port_close(&port) && !port.registered && !bridge_tap.fn && port.closing);
    d->link=NET_LINK_UP;
    CHECK(!net_bridge_port_open(&port,&pool,d,1,id,0,0,mode)); CHECK(!port.closing);
    d->generation++; net_bridge_port_carrier(&port); CHECK(!port.bridge.link);
    CHECK(!net_bridge_port_close(&port));
    CHECK(!net_bridge_port_open(&port,&pool,d,1,id,0,0,mode));
    CHECK(!net_bridge_enable(&port.bridge,key,0,mac));
    mode_error=-5; net_bridge_disable(&port.bridge,key);
    CHECK(!port.bridge.count && port.bridge.promiscuous);
    CHECK(net_bridge_port_close(&port)==-5 && !bridge_tap.fn && port.closing && port.bridge.promiscuous);
    CHECK(net_bridge_port_open(&port,&pool,d,1,id,0,0,mode)==-EINVAL);
    mode_error=0; CHECK(!net_bridge_port_close(&port) && !port.bridge.promiscuous);
    printf("port: %u checks passed\n",checks); return 0;
}
