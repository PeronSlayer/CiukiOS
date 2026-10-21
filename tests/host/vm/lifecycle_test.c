/* Real CREATE validator, VM creation/boot/dispatch/destruction and bridge
 * ownership. CPU/storage/display transports use the established VM harness.
 * SPDX-License-Identifier: GPL-2.0-only */
#define main unused_dosvm_suite_main
#include "../dosvm_test.c"
#undef main
#include "network_dpmi.inc"
int main(void)
{
    ram = calloc(HOST_PAGES, PAGE_SIZE); CHECK(ram);
    test_client_boundary();
    window_fixtures();
    struct ciuki_dosvm_command c = command_for(0, 0);
    c.surface_fd = 0; memcpy(c.name, "LAN", 4);
    dosvm_launch_network_set(&c, CIUKI_DOSVM_NETWORK_IPX);
    CHECK(dosvm_call(CIUKI_DOSVM_CREATE, &c, sizeof(c)) == -EIO);
    CHECK(fake_surface[0].references == 1 && !used_pages && !heap_blocks);
    uint8_t id[16] = {42}; static struct bridge_pool pool;
    struct netif device = {.generation = 91, .ready = true, .running = true, .link = NET_LINK_UP};
    struct net_bridge_port port = {0};
    CHECK(!net_bridge_port_open(&port, &pool, &device, 0, id, 0, 0, host_network_mode));
    /* Exercise the production first-LAN-launch selection path. */
    host_primary_port = &port; host_primary_error = 0;
    host_network_mode_error = -EBUSY;
    CHECK(dosvm_call(CIUKI_DOSVM_CREATE, &c, sizeof(c)) == -EBUSY);
    CHECK(!port.bridge.count && !host_network_promiscuous && !used_pages && !heap_blocks);
    host_network_mode_error = 0;
    CHECK(!dosvm_call(CIUKI_DOSVM_CREATE, &c, sizeof(c)));
    struct dosvm_key key = {c.id, c.generation}; struct dosvm *v = dosvm_lookup(key);
    CHECK(v && network(v) && port.bridge.count == 1 && host_network_promiscuous);
    struct v86_frame f = {0}; f.tf.eax = 0x7a00;
    CHECK(!interrupt_dispatch(v, &f, 0x2f) && (f.tf.eax & 255) == 255 && f.es == 0xf000);
    CHECK(dosvm_port_register(key, 0x300, 0x31f, virtual_port, 0) == -EBUSY);
    c = command_for(key.id, key.generation);
    host_network_mode_error = -EBUSY;
    CHECK(dosvm_call(CIUKI_DOSVM_CLOSE, &c, sizeof(c)) == -EBUSY);
    CHECK(dosvm_lookup(key) && network(v) && host_network_promiscuous && used_pages);
    host_network_mode_error = 0;
    CHECK(!dosvm_call(CIUKI_DOSVM_CLOSE, &c, sizeof(c)));
    CHECK(!network(v) && !port.bridge.count && !host_network_promiscuous && !pool.used);
    CHECK(!used_pages && !heap_blocks && fake_surface[0].references == 1);
    CHECK(!dosvm_network_bind_port(0) && !net_bridge_port_close(&port));
    test_network_dpmi();
    free(ram); printf("network launch/lifecycle: %u checks passed\n", checks); return 0;
}
