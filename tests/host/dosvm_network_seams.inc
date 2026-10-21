/* The VM/network adapters are production; only the physical NIC transport is
 * replaced here. Frame-pool integration has its separate F6 fixture. */
static net_bridge_tap_fn host_network_tap;
static void *host_network_cookie;
static int host_network_mode_error;
static bool host_network_promiscuous;
int net_bridge_tap_register(uint16_t type, net_bridge_tap_fn fn, void *arg)
{
    CHECK(type == BRIDGE_IPX_TYPE && fn);
    if (host_network_tap) return -EBUSY;
    host_network_tap = fn; host_network_cookie = arg; return 0;
}
void net_bridge_tap_unregister(net_bridge_tap_fn fn, void *arg)
{
    CHECK(host_network_tap == fn && host_network_cookie == arg);
    host_network_tap = 0; host_network_cookie = 0;
}
int netif_transmit(struct netif *d, gen_t generation, const void *frame, unsigned length)
{
    CHECK(d && d->generation == generation && frame && length >= 60 && length <= 1514);
    return 0;
}
static inline int host_network_mode(struct netif *d, bool on)
{
    CHECK(d && d->running);
    if (host_network_mode_error) return host_network_mode_error;
    host_network_promiscuous = on; return 0;
}
#include "../../src/kernel/net/bridge_port.c"
#include "../../src/kernel/vm/dosvm_network.c"
