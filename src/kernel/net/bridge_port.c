/* Thread boundary to the selected physical NIC; no driver register shortcuts.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/net_bridge_port.h>
#include <ciuki/cpu.h>

bool net_bridge_port_acquire(struct net_bridge_port *p)
{
    if (!p) return false;
    uint32_t flags = irq_save(); bool available = !p->busy;
    if (available) p->busy = true;
    irq_restore(flags); return available;
}
void net_bridge_port_release(struct net_bridge_port *p)
{ uint32_t flags = irq_save(); p->busy = false; irq_restore(flags); }
static int wire_transmit(void *arg, const uint8_t *frame, unsigned length)
{
    struct net_bridge_port *p = arg;
    if (p->device->link == NET_LINK_DOWN) return -19;
    return netif_transmit(p->device, p->generation, frame, length);
}
static int receive_mode(void *arg, bool on)
{
    struct net_bridge_port *p = arg;
    if (p->device->generation != p->generation || !p->receive_mode) return -19;
    return p->receive_mode(p->device, on);
}
void net_bridge_port_carrier(struct net_bridge_port *p)
{
    struct netif *d = p->device;
    bool up = d && d->generation == p->generation && d->ready && d->running &&
        !d->stopping && !d->quarantined && d->link != NET_LINK_DOWN;
    if (p->bridge.link != up) net_bridge_link(&p->bridge, up);
}
static void frame_tap(struct netif *d, const uint8_t *frame, unsigned length, void *arg)
{
    struct net_bridge_port *p = arg;
    if (d != p->device || d->generation != p->generation) return;
    if (!net_bridge_port_acquire(p)) { p->busy_drops++; return; }
    net_bridge_port_carrier(p);
    net_bridge_ingress(&p->bridge, p->bridge.interface, p->generation, frame, length, g_ticks);
    net_bridge_port_release(p);
}
int net_bridge_port_open(struct net_bridge_port *p, struct bridge_pool *pool, struct netif *d,
                         uint32_t interface, const uint8_t id[16],
                         const uint8_t physical[][6], unsigned count,
                         int (*mode)(struct netif *, bool))
{
    if (!p || !d || !mode || p->registered || !d->generation || !d->ready || !d->running ||
        d->stopping || d->quarantined) return -EINVAL;
    memset(p, 0, sizeof(*p)); p->device = d; p->generation = d->generation; p->receive_mode = mode;
    struct bridge_ops ops = {p,wire_transmit,receive_mode};
    int error = net_bridge_init(&p->bridge, pool, &ops, id, physical, count, interface, p->generation);
    if (error) return error;
    net_bridge_port_carrier(p);
    error = net_bridge_tap_register(BRIDGE_IPX_TYPE, frame_tap, p);
    if (!error) p->registered = true;
    return error;
}
int net_bridge_port_close(struct net_bridge_port *p)
{
    if (!p) return -EINVAL;
    if (!net_bridge_port_acquire(p)) return -16;
    if (p->bridge.count) { net_bridge_port_release(p); return -16; }
    /* Revocation can wait for a tap callback; never wait with its gate held. */
    bool registered = p->registered; p->registered = false;
    net_bridge_port_release(p);
    if (registered) net_bridge_tap_unregister(frame_tap, p);
    if (!net_bridge_port_acquire(p)) return -16;
    net_bridge_shutdown(&p->bridge);
    int error = p->bridge.promiscuous ? -5 : 0;
    net_bridge_port_release(p); return error;
}
int net_bridge_identity_load(const struct bridge_identity_ops *ops, uint8_t id[16])
{
    if (!ops || !id || !ops->read || !ops->write_atomic || !ops->random) return -EINVAL;
    memset(id, 0, 16);
    int error = ops->read(ops->arg, id);
    if (error && error != -2) { memset(id, 0, 16); return error; }
    if (error == -2) {
        error = ops->random(ops->arg, id);
        if (error) { memset(id, 0, 16); return error; }
    }
    uint8_t nonzero = 0; for (unsigned i = 0; i < 16; i++) nonzero |= id[i];
    if (!nonzero) return -EINVAL;
    /* Existing all-zero/corrupt records are refused, never silently replaced. */
    if (error == -2) return -EINVAL;
    return 0;
}
