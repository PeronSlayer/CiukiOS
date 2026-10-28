/* Copied Ethernet II IPX bridge. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_NET_BRIDGE_H
#define CIUKI_NET_BRIDGE_H
#include <ciuki/kernel.h>
#include <ciuki/net_packet.h>
#include <ciuki/ipx_wire.h>

#define BRIDGE_VMS 2u
#define BRIDGE_QUEUE 8u
#define BRIDGE_PACKETS 128u
#define BRIDGE_FRAME_MAX 1514u
#define BRIDGE_IPX_TYPE 0x8137u
/* All entry points are serialized by the network owner in thread context.
 * No callback may reenter the owner. No guest pointer crosses this boundary.
 * Slots below contain only ownership metadata. Payloads come from f6-10's
 * global PBUF_POOL, shared with native ingress; no private frame storage. */
struct bridge_packet {
    struct net_frame *frame;
    uint8_t *data;
    uint32_t generation, interface;
    uint16_t length, refs;
};
struct bridge_pool { struct bridge_packet packet[BRIDGE_PACKETS]; unsigned used, peak; };
struct bridge_queue { uint8_t slot[BRIDGE_QUEUE], head, count; };
struct bridge_rate { uint64_t at; uint32_t credit; };
struct bridge_key { uint32_t id, generation; };
struct bridge_vm {
    struct bridge_key key;
    uint8_t mac[6];
    bool enabled;
    struct bridge_queue rx, tx;
    struct bridge_rate send_rate, receive_rate;
    uint32_t rx_dropped, tx_dropped, rx_overflows;
};
struct bridge_ops {
    void *arg;
    /* Copy-before-return, nonblocking. 0 accepted, negative failed. */
    int (*transmit)(void *, const uint8_t *, unsigned);
    /* Required: fail closed if the selected driver cannot safely enable it. */
    int (*promiscuous)(void *, bool);
};
struct net_bridge {
    struct bridge_pool *pool;
    struct bridge_ops ops;
    struct bridge_vm vm[BRIDGE_VMS];
    uint8_t host_id[16], physical[2][6];
    uint32_t interface, generation, malformed, filtered, wire_errors;
    uint64_t now_ms;
    unsigned physical_count, cursor, count;
    bool identity_ready, link, promiscuous;
    uint8_t host_node[6];
    void *host_arg;
    void (*host_input)(void *, const uint8_t *, unsigned, uint64_t);
    void (*host_tick)(void *, bool, uint64_t);
    unsigned wire_framing;
    unsigned frame_type;
    struct ipx_frame_cache frames;
    uint32_t ipx_network; /* atomic carrier of four network-order bytes */
};
int bridge_pool_copy(struct bridge_pool *, const uint8_t *, unsigned, uint32_t, uint32_t);
bool bridge_pool_hold(struct bridge_pool *, unsigned);
void bridge_pool_release(struct bridge_pool *, unsigned);
int net_bridge_init(struct net_bridge *, struct bridge_pool *, const struct bridge_ops *,
                    const uint8_t persisted_host_id[16], const uint8_t physical[][6],
                    unsigned physical_count, uint32_t interface, uint32_t generation);
int net_bridge_enable(struct net_bridge *, struct bridge_key, uint64_t now_ms, uint8_t mac[6]);
void net_bridge_disable(struct net_bridge *, struct bridge_key);
void net_bridge_link(struct net_bridge *, bool up);
void net_bridge_shutdown(struct net_bridge *);
int net_bridge_send(struct net_bridge *, struct bridge_key, const uint8_t *, unsigned, uint64_t);
/* Emulated DOS API uses the host's detected format; direct NE2000 frames keep
 * their selected encapsulation through net_bridge_send. Gate held. */
int net_bridge_send_ipx(struct net_bridge *, struct bridge_key, const uint8_t *, unsigned, uint64_t);
void net_bridge_ingress(struct net_bridge *, uint32_t interface, uint32_t generation,
                        const uint8_t *, unsigned, uint64_t);
/* At most one TX per VM per pass, cursor rotated even on an empty pass. */
void net_bridge_step(struct net_bridge *);
int net_bridge_host_send(struct net_bridge *, const uint8_t *, unsigned);
int net_bridge_receive(struct net_bridge *, struct bridge_key, uint8_t *, unsigned);
const struct bridge_vm *net_bridge_vm(const struct net_bridge *, struct bridge_key);

struct netif;
typedef void (*net_bridge_tap_fn)(struct netif *, const uint8_t *, unsigned, void *);
/* Exclusive type-filtered worker tap, consumed before the native stack. Clear
 * synchronizes with an active callback. The callback must not unregister itself. */
int net_bridge_tap_register(uint16_t type, net_bridge_tap_fn, void *);
/* Optional bounded maintenance on the same net worker, even without frames.
 * Shares tap revocation/active synchronization. No device mutex is held. */
int net_bridge_tap_worker_register(void (*)(struct netif *, void *), void *);
void net_bridge_tap_unregister(net_bridge_tap_fn, void *);
#endif
