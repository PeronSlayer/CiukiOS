/* Reuse the NIC host seams, execute only the focused tap checks.
 * SPDX-License-Identifier: GPL-2.0-only */
#define main unused_nic_suite_main
#include "../net/net_test.c"
#undef main
static unsigned tapped,native;
static void tap(struct netif *d,const uint8_t *p,unsigned n,void *cookie)
{ CHECK(d==&devices[0] && p[12]==0x81 && n==60 && cookie==&tapped); tapped++; }
static void native_input(struct netif *d,const uint8_t *p,unsigned n,void *cookie)
{ CHECK(d==&devices[0] && p[12]==8 && n==60 && cookie==&native); native++; }
static void release_tap(void) { CHECK(!bridge_tap.fn); bridge_tap.active=false; }
static void enqueue(uint16_t type)
{
    struct netif *d=&devices[0]; unsigned i=(d->queue_head+d->queue_count)%NET_QUEUE_MAX;
    d->queue[i]=(struct net_packet){.length=60}; d->queue[i].data[12]=type>>8; d->queue[i].data[13]=type; d->queue_count++;
}
int main(void)
{
    kmutex_init(&registration_mutex); device_count=1; devices[0].quarantined=false;
    CHECK(net_bridge_tap_register(0x800,tap,&tapped)==-EINVAL);
    CHECK(!net_bridge_tap_register(0x8137,tap,&tapped));
    CHECK(net_bridge_tap_register(0x8137,tap,&tapped)==-NET_EBUSY);
    enqueue(0x8137); net_worker_step(); CHECK(tapped==1 && !native && !devices[0].queue_count);
    net_set_input(native_input,&native); enqueue(0x8137); net_worker_step();
    CHECK(tapped==2 && !native);
    enqueue(0x0806); net_worker_step(); CHECK(native==1 && tapped==2);
    net_bridge_tap_unregister(tap,&native); CHECK(bridge_tap.fn==tap);
    bridge_tap.active=true; sleep_hook=release_tap;
    net_bridge_tap_unregister(tap,&tapped); CHECK(!bridge_tap.fn && sleeps==1); sleep_hook=0;
    net_set_input(0,0); CHECK(!net_bridge_tap_register(0x8137,tap,&tapped));
    net_shutdown(); CHECK(!bridge_tap.fn && !bridge_tap.active);
    CHECK(net_bridge_tap_register(0x8137,tap,&tapped)==-NET_ENODEV);
    printf("tap: %u checks passed\n",checks); return 0;
}
