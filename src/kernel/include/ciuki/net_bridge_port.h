/* Selected physical-port adapter. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_NET_BRIDGE_PORT_H
#define CIUKI_NET_BRIDGE_PORT_H
#include <ciuki/net_bridge.h>
#include <ciuki/net.h>
struct net_bridge_port {
    struct net_bridge bridge;
    struct netif *device;
    int (*receive_mode)(struct netif *, bool);
    uint32_t generation, busy_drops;
    bool busy, registered, closing;
};
/* All control/VM bridge accesses take this nonblocking owner gate. Contention
 * returns EBUSY rather than sleeping inside a guest publication or holding a
 * mutex across a NIC call (which has its own address-ordered mutex). */
bool net_bridge_port_acquire(struct net_bridge_port *);
void net_bridge_port_release(struct net_bridge_port *);
int net_bridge_port_open(struct net_bridge_port *, struct bridge_pool *, struct netif *,
                         uint32_t interface, const uint8_t host_id[16],
                         const uint8_t physical[][6], unsigned physical_count,
                         int (*receive_mode)(struct netif *, bool));
/* Return EBUSY while a VM/operation exists; caller retains the static owner. */
int net_bridge_port_close(struct net_bridge_port *);
void net_bridge_port_carrier(struct net_bridge_port *); /* gate held */

struct bridge_identity_ops {
    void *arg;
    /* read: 0 existing valid record, -ENOENT absent, other negative error.
     * write_atomic: durable publication without replacement before success.
     * random: 16 fresh bytes. */
    int (*read)(void *, uint8_t[16]);
    int (*write_atomic)(void *, const uint8_t[16]);
    int (*random)(void *, uint8_t[16]);
};
int net_bridge_identity_load(const struct bridge_identity_ops *, uint8_t[16]);
/* Production VFS callbacks; arg is an exclusively owned struct vfs_table.
 * NETID.BIN is never replaced. A valid pending NETID.TMP is recovered before
 * generating another identity; invalid records and failed barriers refuse LAN. */
int net_bridge_identity_read_file(void *, uint8_t[16]);
int net_bridge_identity_write_file(void *, const uint8_t[16]);
#endif
