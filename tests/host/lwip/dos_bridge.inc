/* Two DOS owners contend with native tcpip_input for the actual 128-entry
 * PBUF_POOL. The core is parked at an explicit barrier during exhaustion.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/net_bridge.h>
static sys_sem_t bridge_core_entered, bridge_core_release;
static unsigned bridge_wire_frames, bridge_modes;
static void bridge_park_core(void *unused)
{
    (void)unused; sys_sem_signal(&bridge_core_entered);
    sys_arch_sem_wait(&bridge_core_release, 0);
}
static int bridge_test_wire(void *unused, const uint8_t *data, unsigned bytes)
{ (void)unused; CHECK(bytes >= 60 && data[12] == 0x81 && data[13] == 0x37); bridge_wire_frames++; return 0; }
static int bridge_test_mode(void *unused, bool on)
{ (void)unused; bridge_modes += on ? 1 : 10; return 0; }
static void bridge_ipx_frame(uint8_t *frame, const uint8_t from[6], const uint8_t to[6])
{
    memset(frame, 0, 64); memcpy(frame, to, 6); memcpy(frame + 6, from, 6);
    frame[12] = 0x81; frame[13] = 0x37; frame[14] = frame[15] = 255; frame[17] = 50;
    memcpy(frame + 24, to, 6); memcpy(frame + 36, from, 6);
}
static void dos_bridge_pool_test(void)
{
    CHECK(net_stack_primary() == 0);
    uint8_t id[16]; CHECK(!net_stack_random_id(id));
    static struct bridge_pool pool; struct net_bridge bridge;
    struct bridge_ops ops = {NULL, bridge_test_wire, bridge_test_mode};
    CHECK(!net_bridge_init(&bridge, &pool, &ops, id, NULL, 0, 0, 55));
    net_bridge_link(&bridge, true);
    struct bridge_key a = {1, 100}, b = {2, 200}; uint8_t ma[6], mb[6], frame[64], copy[64];
    const uint8_t peer[6] = {2,3,4,5,6,7}, all[6] = {255,255,255,255,255,255};
    CHECK(!net_bridge_enable(&bridge, a, 0, ma) && !net_bridge_enable(&bridge, b, 0, mb));
    CHECK(bridge_modes == 1 && memcmp(ma, mb, 6));
    CHECK(sys_sem_new(&bridge_core_entered, 0) == ERR_OK && sys_sem_new(&bridge_core_release, 0) == ERR_OK);
    CHECK(tcpip_callback(bridge_park_core, NULL) == ERR_OK);
    CHECK(sys_arch_sem_wait(&bridge_core_entered, 2000) != SYS_ARCH_TIMEOUT);
    unsigned baseline = lwip_stats.memp[MEMP_PBUF_POOL]->used;
    CHECK(baseline == 0 && PBUF_POOL_SIZE == 128);
    /* A real native ingress pbuf remains owned by the tcpip mailbox. */
    uint8_t native[60] = {0}; memcpy(native, client_mac, 6); native[12] = 8; native[13] = 6;
    net_stack_input(0, native, sizeof(native));
    CHECK(lwip_stats.memp[MEMP_PBUF_POOL]->used == baseline + 1);
    struct pbuf *held[113];
    for (unsigned i = 0; i < 113; i++) { held[i] = pbuf_alloc(PBUF_RAW, 64, PBUF_POOL); CHECK(held[i]); }
    for (unsigned i = 0; i < 7; i++) {
        bridge_ipx_frame(frame, ma, peer); CHECK(!net_bridge_send(&bridge, a, frame, 64, 0));
        bridge_ipx_frame(frame, mb, peer); CHECK(!net_bridge_send(&bridge, b, frame, 64, 0));
    }
    CHECK(pool.used == 14 && lwip_stats.memp[MEMP_PBUF_POOL]->used == 128);
    bridge_ipx_frame(frame, ma, peer); CHECK(net_bridge_send(&bridge, a, frame, 64, 0) == -ENOSPC);
    bridge_ipx_frame(frame, mb, peer); CHECK(net_bridge_send(&bridge, b, frame, 64, 0) == -ENOSPC);
    bridge_ipx_frame(frame, peer, all); net_bridge_ingress(&bridge, 0, 55, frame, 64, 0);
    CHECK(net_bridge_vm(&bridge, a)->rx_overflows == 1 && net_bridge_vm(&bridge, b)->rx_overflows == 1);
    unsigned dropped = lwip_stats.link.drop;
    net_stack_input(0, native, sizeof(native)); CHECK(lwip_stats.link.drop == dropped + 1);
    CHECK(!pbuf_alloc(PBUF_RAW, 60, PBUF_POOL)); /* native cannot use a second pool */
    net_bridge_link(&bridge, false);
    CHECK(!pool.used && lwip_stats.memp[MEMP_PBUF_POOL]->used == 114 && !bridge_wire_frames);
    net_bridge_link(&bridge, true);
    bridge_ipx_frame(frame, peer, all); net_bridge_ingress(&bridge, 0, 55, frame, 64, 1000);
    CHECK(pool.used == 1 && lwip_stats.memp[MEMP_PBUF_POOL]->used == 115);
    CHECK(net_bridge_receive(&bridge, a, copy, sizeof(copy)) == 64 && !memcmp(frame, copy, 64));
    CHECK(pool.used == 1 && lwip_stats.memp[MEMP_PBUF_POOL]->used == 115);
    net_bridge_disable(&bridge, b); /* final fan-out reference */
    CHECK(!pool.used && lwip_stats.memp[MEMP_PBUF_POOL]->used == 114);
    bridge_ipx_frame(frame, ma, peer); CHECK(!net_bridge_send(&bridge, a, frame, 64, 1000));
    net_bridge_shutdown(&bridge);
    CHECK(!pool.used && bridge_modes == 11 && lwip_stats.memp[MEMP_PBUF_POOL]->used == 114);
    for (unsigned i = 0; i < 113; i++) CHECK(pbuf_free(held[i]) == 1);
    CHECK(lwip_stats.memp[MEMP_PBUF_POOL]->used == 1); /* native ownership survives VM drain */
    sys_sem_signal(&bridge_core_release);
    CHECK(tcpip_callback_wait(core_barrier, NULL) == ERR_OK);
    CHECK(lwip_stats.memp[MEMP_PBUF_POOL]->used == baseline);
    sys_sem_free(&bridge_core_entered); sys_sem_free(&bridge_core_release);
    puts("PASS f6-11 two-VM/native shared PBUF_POOL exhaustion, fan-out references, link/disable/destroy drain and baseline recovery");
}
