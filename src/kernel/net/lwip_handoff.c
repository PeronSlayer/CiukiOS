/* Copied, bounded handoff between the physical NIC worker and tcpip_thread.
 * The f6-01 ARP helpers remain available only to their existing probes.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/net.h>
#include <ciuki/net_stack.h>
#include <ciuki/cpu.h>
static struct {
    struct net_packet frames[NET_QUEUE_MAX];
    uint32_t generation[NET_QUEUE_MAX];
    uint32_t config_epoch[NET_QUEUE_MAX];
    unsigned head, count;
    struct kmutex lock;
} outgoing[2];
static void ingress(struct netif *device, const uint8_t *frame, unsigned bytes, void *cookie)
{
    (void)cookie;
    for (unsigned i = 0; i < net_count() && i < 2; i++) if (net_get(i) == device) net_stack_input(i, frame, bytes);
}
/* fx-11: the net-input worker sleeps until woken (drivers/net.c); a queued
 * frame wakes it. Weak: host harnesses link this file without the worker. */
void net_worker_kick(void) __attribute__((weak));
int net_stack_transmit(unsigned index, const void *frame, unsigned bytes)
{
    if (index >= 2 || bytes < 14 || bytes > NET_FRAME_MAX) return -EINVAL;
    kmutex_lock(&outgoing[index].lock);
    unsigned count = outgoing[index].count;
    int e = count == NET_QUEUE_MAX ? -EAGAIN : 0;
    if (!e) {
        unsigned at = (outgoing[index].head + count) % NET_QUEUE_MAX;
        struct netif *d = net_get(index);
        if (!d || !d->running || d->administratively_down || d->quarantined) { kmutex_unlock(&outgoing[index].lock); return -ENETDOWN; }
        struct net_packet *p = &outgoing[index].frames[at];
        outgoing[index].generation[at] = d->generation;
        outgoing[index].config_epoch[at] = d->config_epoch;
        memcpy(p->data, frame, bytes); p->length = (uint16_t)bytes; outgoing[index].count++;
    }
    kmutex_unlock(&outgoing[index].lock);
    if (!e && net_worker_kick) net_worker_kick();
    return e;
}
void net_stack_driver_entropy(void)
{
    for (unsigned i = 0; i < net_count() && i < 2; i++) {
        struct netif *d = net_get(i); if (!d->running) continue;
        uint64_t mac = 0; memcpy(&mac, d->address, 6); net_stack_entropy(mac);
        if (g_cpu_tsc) net_stack_entropy(read_tsc_serialized());
        break; /* selected primary, before tcp_init/udp_init */
    }
}
void net_stack_driver_start(void)
{
    for (unsigned i = 0; i < 2; i++) kmutex_init(&outgoing[i].lock);
    for (unsigned i = 0; i < net_count() && i < 2; i++) {
        struct netif *d = net_get(i); if (!d->running) continue;
        uint64_t mac = 0; memcpy(&mac, d->address, 6); net_stack_entropy(mac);
        if (g_cpu_tsc) net_stack_entropy(read_tsc_serialized());
        (void)net_stack_attach(i, d->address, d->link != NET_LINK_DOWN);
    }
    net_set_input(ingress, NULL);
}
/* f6-22: driver counters for the stack's `[net]` records (core thread). */
void net_stack_driver_counters(unsigned index, struct net_stack_counters *out)
{
    struct netif *d = index < 2 ? net_get(index) : 0;
    if (!d) return;
    struct net_stats stats; netif_snapshot(d, &stats);
    out->rx = stats.rx_packets; out->tx = stats.tx_completed;
    out->rx_errors = stats.rx_errors; out->tx_errors = stats.tx_errors;
    out->dropped = stats.rx_dropped;
}
/* True when transmit frames remain queued (ring full or the 1 ms slice). */
bool net_stack_driver_service(void)
{
    uint64_t start = g_ticks; struct net_packet packet; bool pending = false;
    net_stack_watch();
    for (unsigned i = 0; i < net_count() && i < 2; i++) {
        struct netif *d = net_get(i);
        net_stack_link(i, d->running && !d->administratively_down && !d->quarantined && d->link != NET_LINK_DOWN);
        if (!d->running || d->administratively_down || d->quarantined || d->link == NET_LINK_DOWN) {
            kmutex_lock(&outgoing[i].lock); outgoing[i].head = outgoing[i].count = 0;
            kmutex_unlock(&outgoing[i].lock); continue;
        }
        for (unsigned n = 0; n < 8 && g_ticks - start < 1; n++) {
            kmutex_lock(&outgoing[i].lock);
            if (!outgoing[i].count) { kmutex_unlock(&outgoing[i].lock); break; }
            packet = outgoing[i].frames[outgoing[i].head];
            uint32_t generation = outgoing[i].generation[outgoing[i].head];
            uint32_t epoch = outgoing[i].config_epoch[outgoing[i].head];
            kmutex_unlock(&outgoing[i].lock);
            int e = epoch != d->config_epoch ? -ENETDOWN : netif_transmit(d, generation, packet.data, packet.length);
            if (e == -16) break;
            kmutex_lock(&outgoing[i].lock);
            outgoing[i].head = (outgoing[i].head + 1) % NET_QUEUE_MAX; outgoing[i].count--;
            kmutex_unlock(&outgoing[i].lock);
        }
        kmutex_lock(&outgoing[i].lock); if (outgoing[i].count) pending = true; kmutex_unlock(&outgoing[i].lock);
    }
    return pending;
}
