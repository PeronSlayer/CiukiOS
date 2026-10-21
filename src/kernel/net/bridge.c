/* Bounded one-port Ethernet II IPX forwarding; see f6-11-dos-network.md.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/net_bridge.h>

static bool key_equal(struct bridge_key a, struct bridge_key b)
{ return a.id == b.id && a.generation == b.generation; }
static bool broadcast(const uint8_t *p)
{ static const uint8_t all[6] = {255,255,255,255,255,255}; return !memcmp(p, all, 6); }
static bool frame_ok(const uint8_t *p, unsigned n)
{
    if (!p || n < 44 || n > BRIDGE_FRAME_MAX || p[12] != 0x81 || p[13] != 0x37) return false;
    unsigned length = (unsigned)p[16] << 8 | p[17];
    /* No checksum variant, router forwarding, VLAN, raw 802.3 or LLC. Padding
     * is retained; the declared IPX datagram must fit the copied frame. */
    return p[14] == 255 && p[15] == 255 && length >= 30 && length <= n - 14 &&
        p[18] == 0 && !(p[6] & 1) && !broadcast(p + 6) &&
        memcmp(p + 6, "\0\0\0\0\0\0", 6) &&
        !memcmp(p, p + 24, 6) && !memcmp(p + 6, p + 36, 6) &&
        !memcmp(p + 20, "\0\0\0\0", 4) && !memcmp(p + 32, "\0\0\0\0", 4);
}
int bridge_pool_copy(struct bridge_pool *p, const uint8_t *data, unsigned n,
                     uint32_t interface, uint32_t generation)
{
    if (!p || !data || n < 14 || n > BRIDGE_FRAME_MAX || !generation) return -EINVAL;
    for (unsigned i = 0; i < BRIDGE_PACKETS; i++) if (!p->packet[i].refs) {
        struct bridge_packet *f = &p->packet[i];
        f->frame = net_packet_copy(data, n);
        if (!f->frame) return -ENOSPC;
        f->data = net_packet_data(f->frame);
        unsigned padded = n < 60 ? 60 : n;
        f->length = padded; f->interface = interface; f->generation = generation; f->refs = 1;
        if (++p->used > p->peak) p->peak = p->used;
        return (int)i;
    }
    return -ENOSPC;
}
bool bridge_pool_hold(struct bridge_pool *p, unsigned i)
{
    if (!p || i >= BRIDGE_PACKETS || !p->packet[i].refs || p->packet[i].refs == UINT16_MAX) return false;
    p->packet[i].refs++; return true;
}
void bridge_pool_release(struct bridge_pool *p, unsigned i)
{
    if (!p || i >= BRIDGE_PACKETS || !p->packet[i].refs) return;
    if (!--p->packet[i].refs) {
        net_packet_free(p->packet[i].frame);
        memset(&p->packet[i], 0, sizeof(p->packet[i])); p->used--;
    }
}
static int queue_pop(struct bridge_queue *q)
{
    if (!q->count) return -1;
    unsigned slot = q->slot[q->head];
    q->head = (q->head + 1) % BRIDGE_QUEUE; q->count--; return (int)slot;
}
static void queue_push(struct bridge_queue *q, unsigned slot)
{ q->slot[(q->head + q->count++) % BRIDGE_QUEUE] = slot; }
static void drain(struct net_bridge *b, struct bridge_vm *v)
{
    int i;
    while ((i = queue_pop(&v->rx)) >= 0) bridge_pool_release(b->pool, (unsigned)i);
    while ((i = queue_pop(&v->tx)) >= 0) { v->tx_dropped++; bridge_pool_release(b->pool, (unsigned)i); }
}
static struct bridge_vm *lookup(struct net_bridge *b, struct bridge_key k)
{
    for (unsigned i = 0; i < BRIDGE_VMS; i++) if (b->vm[i].enabled && key_equal(b->vm[i].key, k)) return &b->vm[i];
    return 0;
}
const struct bridge_vm *net_bridge_vm(const struct net_bridge *b, struct bridge_key k)
{
    if (!b) return 0;
    for (unsigned i = 0; i < BRIDGE_VMS; i++) if (b->vm[i].enabled && key_equal(b->vm[i].key, k)) return &b->vm[i];
    return 0;
}
int net_bridge_init(struct net_bridge *b, struct bridge_pool *pool, const struct bridge_ops *ops,
                    const uint8_t id[16], const uint8_t physical[][6], unsigned count,
                    uint32_t interface, uint32_t generation)
{
    if (!b || !pool || !ops || !ops->transmit || !ops->promiscuous || !id || count > 2 ||
        (count && !physical) || !generation) return -EINVAL;
    uint8_t nonzero = 0; for (unsigned i = 0; i < 16; i++) nonzero |= id[i];
    if (!nonzero) return -EINVAL; /* Caller must load/create AND persist random identity first. */
    memset(b, 0, sizeof(*b)); b->pool = pool; b->ops = *ops;
    memcpy(b->host_id, id, 16); if (count) memcpy(b->physical, physical, count * 6);
    b->physical_count = count; b->interface = interface; b->generation = generation; b->identity_ready = true;
    return 0;
}
static void derive_mac(const struct net_bridge *b, struct bridge_key k, unsigned salt, uint8_t out[6])
{
    /* Explicit byte order, no structure padding or libc hash randomization. */
    uint32_t h = 2166136261u, j = 0x9e3779b9u;
    for (unsigned n = 0; n < 28; n++) {
        uint8_t c = n < 16 ? b->host_id[n] : n < 20 ? (uint8_t)(k.id >> (8 * (n - 16))) :
            n < 24 ? (uint8_t)(k.generation >> (8 * (n - 20))) : (uint8_t)(salt >> (8 * (n - 24)));
        h = (h ^ c) * 16777619u; j = (j ^ c) * 2246822519u;
    }
    out[0] = (uint8_t)((h & 0xfc) | 2);
    for (unsigned i = 1; i < 4; i++) out[i] = (uint8_t)(h >> (8 * i));
    out[4] = (uint8_t)j; out[5] = (uint8_t)(j >> 8);
}
int net_bridge_enable(struct net_bridge *b, struct bridge_key k, uint64_t now, uint8_t mac[6])
{
    if (!b || !b->identity_ready || !k.id || !k.generation || !mac) return -EINVAL;
    if (lookup(b, k)) return -16;
    struct bridge_vm *v = 0;
    for (unsigned i = 0; i < BRIDGE_VMS; i++) {
        if (b->vm[i].enabled && b->vm[i].key.id == k.id) return -16;
        if (!b->vm[i].enabled) v = &b->vm[i];
    }
    if (!v) return -ENOSPC;
    bool found = false;
    for (unsigned salt = 0; salt < 256 && !found; salt++) {
        derive_mac(b, k, salt, mac); found = true;
        for (unsigned i = 0; i < b->physical_count; i++) if (!memcmp(mac, b->physical[i], 6)) found = false;
        for (unsigned i = 0; i < BRIDGE_VMS; i++) if (b->vm[i].enabled && !memcmp(mac, b->vm[i].mac, 6)) found = false;
    }
    if (!found) return -ENOSPC;
    if (!b->count) {
        int e = b->ops.promiscuous(b->ops.arg, true); if (e) return e;
        b->promiscuous = true;
    }
    memset(v, 0, sizeof(*v)); v->key = k; memcpy(v->mac, mac, 6); v->enabled = true;
    v->send_rate = v->receive_rate = (struct bridge_rate){now, 32000}; b->count++;
    return 0;
}
void net_bridge_disable(struct net_bridge *b, struct bridge_key k)
{
    if (!b) return;
    struct bridge_vm *v = lookup(b, k);
    if (v) { drain(b, v); v->enabled = false; b->count--; }
    /* Retrying a last-owner detach must retry receive-mode revocation too. */
    if (!b->count && b->promiscuous && !b->ops.promiscuous(b->ops.arg, false)) b->promiscuous = false;
}
void net_bridge_link(struct net_bridge *b, bool up)
{
    if (!b) return;
    b->link = up;
    if (!up) for (unsigned i = 0; i < BRIDGE_VMS; i++) drain(b, &b->vm[i]);
}
void net_bridge_shutdown(struct net_bridge *b)
{
    if (!b) return;
    net_bridge_link(b, false);
    for (unsigned i = 0; i < BRIDGE_VMS; i++) net_bridge_disable(b, b->vm[i].key);
    if (b->promiscuous && !b->ops.promiscuous(b->ops.arg, false)) b->promiscuous = false;
}
static bool token(struct bridge_rate *r, uint64_t now)
{
    if (now >= r->at) {
        uint64_t elapsed = now - r->at;
        uint32_t refill = elapsed >= 125 ? 32000 : (uint32_t)elapsed * 256;
        r->credit = r->credit + refill > 32000 ? 32000 : r->credit + refill; r->at = now;
    }
    if (r->credit < 1000) return false;
    r->credit -= 1000; return true;
}
int net_bridge_send(struct net_bridge *b, struct bridge_key k, const uint8_t *p, unsigned n, uint64_t now)
{
    if (!b) return -EINVAL;
    struct bridge_vm *v = lookup(b, k); if (!v || !b->link) return -19;
    if (now > b->now_ms) b->now_ms = now;
    if (!frame_ok(p, n)) { b->malformed++; return -EINVAL; }
    if (memcmp(p + 6, v->mac, 6)) { b->filtered++; return -13; }
    if (v->tx.count == BRIDGE_QUEUE || !token(&v->send_rate, now)) { v->tx_dropped++; return -11; }
    int slot = bridge_pool_copy(b->pool, p, n, b->interface, k.generation);
    if (slot < 0) { v->tx_dropped++; return slot; }
    queue_push(&v->tx, (unsigned)slot); return 0;
}
static void deliver(struct net_bridge *b, struct bridge_vm *v, unsigned slot, uint64_t now, bool limit)
{
    if (v->rx.count == BRIDGE_QUEUE || (limit && !token(&v->receive_rate, now))) {
        v->rx_dropped++; v->rx_overflows++; return;
    }
    if (bridge_pool_hold(b->pool, slot)) queue_push(&v->rx, slot);
}
void net_bridge_ingress(struct net_bridge *b, uint32_t interface, uint32_t generation,
                        const uint8_t *p, unsigned n, uint64_t now)
{
    if (!b || !b->link || !b->count || interface != b->interface || generation != b->generation) return;
    if (now > b->now_ms) b->now_ms = now;
    if (!frame_ok(p, n)) { b->malformed++; return; }
    bool any = false;
    for (unsigned i = 0; i < BRIDGE_VMS; i++) if (b->vm[i].enabled) {
        if (!memcmp(p + 6, b->vm[i].mac, 6)) { b->filtered++; return; } /* reflection/spoof */
        if (broadcast(p) || !memcmp(p, b->vm[i].mac, 6)) any = true;
    }
    if (!any) { b->filtered++; return; }
    int slot = bridge_pool_copy(b->pool, p, n, interface, generation);
    if (slot < 0) {
        for (unsigned i = 0; i < BRIDGE_VMS; i++) if (b->vm[i].enabled &&
            (broadcast(p) || !memcmp(p, b->vm[i].mac, 6))) { b->vm[i].rx_dropped++; b->vm[i].rx_overflows++; }
        return;
    }
    for (unsigned i = 0; i < BRIDGE_VMS; i++) if (b->vm[i].enabled &&
        (broadcast(p) || !memcmp(p, b->vm[i].mac, 6))) deliver(b, &b->vm[i], (unsigned)slot, now, true);
    bridge_pool_release(b->pool, (unsigned)slot);
}
void net_bridge_step(struct net_bridge *b)
{
    if (!b || !b->link) return;
    for (unsigned n = 0; n < BRIDGE_VMS; n++) {
        struct bridge_vm *sender = &b->vm[(b->cursor + n) % BRIDGE_VMS];
        int slot = queue_pop(&sender->tx); if (slot < 0) continue;
        struct bridge_packet *p = &b->pool->packet[slot];
        bool local = false;
        for (unsigned i = 0; i < BRIDGE_VMS; i++) if (b->vm[i].enabled &&
            !memcmp(p->data, b->vm[i].mac, 6)) local = true;
        if (sender->enabled && p->generation == sender->key.generation) {
            for (unsigned i = 0; i < BRIDGE_VMS; i++) if ((&b->vm[i] != sender || !broadcast(p->data)) && b->vm[i].enabled &&
                (broadcast(p->data) || !memcmp(p->data, b->vm[i].mac, 6)))
                deliver(b, &b->vm[i], (unsigned)slot, b->now_ms, true);
            if (!local && b->ops.transmit(b->ops.arg, p->data, p->length)) { sender->tx_dropped++; b->wire_errors++; }
        }
        bridge_pool_release(b->pool, (unsigned)slot);
    }
    b->cursor = (b->cursor + 1) % BRIDGE_VMS;
}
int net_bridge_receive(struct net_bridge *b, struct bridge_key k, uint8_t *out, unsigned capacity)
{
    if (!b || !out) return -EINVAL;
    struct bridge_vm *v = lookup(b, k); if (!v || !b->link) return -19;
    if (!v->rx.count) return 0;
    unsigned slot = v->rx.slot[v->rx.head];
    struct bridge_packet *p = &b->pool->packet[slot];
    if (capacity < p->length) return -ENOSPC;
    int length = p->length; memcpy(out, p->data, p->length);
    (void)queue_pop(&v->rx); bridge_pool_release(b->pool, slot); return length;
}
