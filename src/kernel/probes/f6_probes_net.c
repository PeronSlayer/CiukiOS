/* One NIC and QEMU's IPv4 user backend. No socket or IPX claims.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/task.h>
#include <ciuki/net.h>
#include <ciuki/probe.h>

struct arp_probe { struct netif *device; bool replied; uint8_t peer[6]; };
static void arp_input(struct netif *d, const uint8_t *frame, unsigned len, void *cookie)
{
    struct arp_probe *state = cookie;
    if (d == state->device && net_arp_reply(frame, len, d->address, state->peer)) state->replied = true;
}
static int probe_net_arp(void)
{
    const char *probe = "net-arp";
    rec_emit(probe, "BEGIN", 0);
    struct netif *d = 0;
    for (unsigned i = 0; i < net_count(); i++) if (net_get(i)->running) { d = net_get(i); break; }
    if (!d) { rec_emit(probe, "END", "status=FAIL reason=nic_unavailable"); return -19; }
    char mac[18]; net_mac_text(d->address, mac);
    rec_emit(probe, "READY", "driver=%s mac=%s link=%s irq=%u mode=%s", d->driver->name,
             mac, net_link_text(d->link), d->pci.irq_line, d->irq_attached ? "intx" : "poll");
    struct arp_probe state = { .device = d };
    net_set_input(arp_input, &state);
    uint8_t frame[60]; net_arp_request(frame, d->address);
    int err = netif_transmit(d, d->generation, frame, sizeof(frame));
    uint64_t deadline = deadline_after_ms(5000);
    struct net_stats stats = { 0 };
    while (!err && !deadline_passed(deadline)) {
        netif_snapshot(d, &stats);
        if (d->quarantined) { err = -5; break; }
        if (state.replied && stats.tx_completed) break;
        task_sleep_ms(1);
    }
    net_set_input(0, 0); /* synchronize before stack state leaves scope */
    netif_snapshot(d, &stats);
    bool ok = !err && state.replied && stats.tx_completed && !d->quarantined && !stats.budget_violations &&
              (!d->irq_attached || stats.interrupts);
    char peer[18]; net_mac_text(state.peer, peer);
    rec_emit(probe, "DATA", "case=arp arp=%s mac=%s tx=%llu rx=%llu dropped=%llu errors=%llu",
             ok ? "ok" : "fail", peer, stats.tx_completed, stats.rx_packets, stats.rx_dropped,
             stats.rx_errors + stats.tx_errors);
    rec_emit(probe, "DATA", "case=budget max_irq_us=%llu violations=%llu interrupts=%llu quarantine=%u", stats.max_irq_us, stats.budget_violations, stats.interrupts, d->quarantined);
    int stop = netif_stop(d);
    rec_emit(probe, "DATA", "case=cleanup stopped=%u dma_pages=%u irq_attached=%u", !stop,
             d->dma_phys ? d->driver->dma_pages : 0, d->irq_attached);
    ok = ok && !stop;
    rec_emit(probe, "END", "status=%s", ok ? "PASS" : "FAIL");
    return ok ? 0 : -5;
}
CIUKI_F6_PROBE("net-arp", probe_net_arp);
