/* Bounded NIC ownership, packet queues and thread service.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/mm.h>
#include <ciuki/task.h>
#include <ciuki/net.h>
#include <ciuki/timing.h>

#define NET_EIO 5
#define NET_EBUSY 16
#define NET_ENODEV 19
static struct netif devices[NETIF_MAX];
static unsigned device_count;
static bool initialized, shutting_down;
static struct task *worker;
static bool input_active;
static struct kmutex registration_mutex;
static net_input_fn input;
static void *input_cookie;
static struct netif *proof_device;

uint32_t net_read(struct netif *d, unsigned reg, unsigned width)
{ return d->io.read(d->io.cookie, d->base + reg, width); }
void net_write(struct netif *d, unsigned reg, uint32_t value, unsigned width)
{ d->io.write(d->io.cookie, d->base + reg, value, width); }
void net_dma_barrier(void) { __asm__ volatile("" ::: "memory"); }
unsigned net_take_pending(struct netif *d)
{
    uint32_t f = irq_save(); unsigned pending = d->pending; d->pending = 0;
    irq_restore(f); return pending;
}
bool net_expired(struct netif *d, uint64_t deadline)
{ return (int64_t)(d->io.now(d->io.cookie) - deadline) >= 0; }
void net_tx_complete(struct netif *d, unsigned slot, bool ok)
{
    if (!d->tx_used[slot]) return;
    d->tx_used[slot] = false;
    if (ok) { d->stats.tx_completed++; d->stats.tx_bytes += d->tx_length[slot]; }
    else d->stats.tx_errors++;
}
void net_mac_text(const uint8_t address[6], char text[18])
{
    ksnprintf(text, 18, "%02x:%02x:%02x:%02x:%02x:%02x", address[0], address[1],
              address[2], address[3], address[4], address[5]);
}
const char *net_link_text(enum net_link state)
{ return state == NET_LINK_UP ? "up" : state == NET_LINK_DOWN ? "down" : "unknown"; }
static void record(struct netif *d)
{
    char mac[18]; net_mac_text(d->address, mac);
    klog("[net] driver=%s mac=%s link=%s irq=%u mode=%s state=%s reason=%s",
         d->driver->name, mac, net_link_text(d->link), d->pci.irq_line,
         d->irq_attached ? "intx" : "poll", d->quarantined ? "quarantined" :
         d->running ? "running" : "stopped", d->reason);
}
static uint32_t pci_read_config(struct netif *d, const struct pci_func *p, unsigned reg)
{
    uint32_t f = irq_save();
    d->io.write(d->io.cookie, 0xcf8, 0x80000000u | (uint32_t)p->bus << 16 |
                (uint32_t)p->dev << 11 | (uint32_t)p->fn << 8 | (reg & ~3u), 4);
    uint32_t v = d->io.read(d->io.cookie, 0xcfc, 4);
    irq_restore(f); return v;
}
static int pci_command(struct netif *d, uint16_t command)
{
    uint32_t f = irq_save();
    const struct pci_func *p = &d->pci;
    d->io.write(d->io.cookie, 0xcf8, 0x80000000u | (uint32_t)p->bus << 16 |
                (uint32_t)p->dev << 11 | (uint32_t)p->fn << 8 | 4, 4);
    /* Never echo write-one-to-clear PCI Status. */
    d->io.write(d->io.cookie, 0xcfc, command, 2);
    uint16_t got = d->io.read(d->io.cookie, 0xcfc, 2);
    irq_restore(f); return got == command ? 0 : -NET_EIO;
}
static bool power_d0(struct netif *d)
{
    if (!(pci_read_config(d, &d->pci, 4) & (1u << 20))) return true;
    unsigned pos = pci_read_config(d, &d->pci, 0x34) & 0xff;
    uint64_t seen = 0;
    for (unsigned n = 0; pos && n < 48; n++) {
        if (pos < 0x40 || pos > 0xfc || (pos & 3)) return false;
        uint64_t bit = (uint64_t)1 << ((pos - 0x40) / 4);
        if (seen & bit) return false;
        seen |= bit;
        uint32_t cap = pci_read_config(d, &d->pci, pos);
        if ((cap & 0xff) == 1) return pos <= 0xf8 && !(pci_read_config(d, &d->pci, pos + 4) & 3);
        pos = (cap >> 8) & 0xff;
    }
    return !pos;
}
static bool routed(struct netif *d)
{
    const struct pci_func *p = &d->pci;
    /* Only the already-qualified QEMU PIIX root slots. Notebook routing is
     * not inferred from Interrupt Line: use masked, documented polling. */
    if (!(g_boot.flags & CBI_F_SMBIOS_QEMU) || (p->command & 0x400) || p->bus || !p->irq_pin || p->irq_pin > 4 ||
        (p->irq_line != 5 && p->irq_line != 9 && p->irq_line != 10 && p->irq_line != 11)) return false;
    for (unsigned i = 0; i < pci_count(); i++) {
        const struct pci_func *b = pci_get(i);
        if (b->vendor != 0x8086 || b->bus || b->class_code != 6 || b->subclass != 1 ||
            (b->device != 0x7000 && b->device != 0x7110)) continue;
        unsigned pirq = (p->dev + p->irq_pin - 2) & 3;
        unsigned route = (pci_read_config(d, b, 0x60) >> (pirq * 8)) & 0xff;
        unsigned elcr = d->io.read(d->io.cookie, 0x4d0 + p->irq_line / 8, 1);
        return !(route & 0x80) && (route & 15) == p->irq_line && (elcr & (1u << (p->irq_line % 8)));
    }
    return false;
}
static bool irq_handler(unsigned irq, const char *owner)
{
    for (unsigned i = 0; i < device_count; i++)
        if (devices[i].pci.irq_line == irq && !strncmp(devices[i].owner, owner, sizeof(devices[i].owner)))
            return netif_irq(&devices[i]);
    return false;
}
bool netif_irq(struct netif *d)
{
    if (!d || !d->running || d->stopping || d->quarantined) return false;
    uint64_t start = ktime_cycles();
    bool handled = d->driver->interrupt(d);
    if (handled) { d->irq_pending = true; d->stats.interrupts++; }
    uint64_t us = g_tsc_per_ms ? (ktime_cycles() - start) * 1000 / g_tsc_per_ms : 0;
    if (us > d->stats.max_irq_us) d->stats.max_irq_us = us;
    /* A callback uses at most 50 us of the chain's 250 us aggregate limit.
     * Source already masked by interrupt(); fault cleanup runs in thread. */
    if (us > 50) { d->stats.budget_violations++; d->fault_pending = true; }
    return handled;
}
static bool idle_proof(int handle, gen_t generation)
{
    (void)handle; (void)generation;
    return proof_device && !proof_device->running && !proof_device->irq_attached &&
           !proof_device->servicing && !proof_device->quarantined;
}
static void quarantine_locked(struct netif *d, const char *reason)
{
    if (d->quarantined) return;
    d->quarantined = d->stopping = true; d->ready = d->running = false;
    d->link = NET_LINK_DOWN; d->reason = reason; d->stats.quarantines++;
    /* Mask sources and revoke PCI bus mastering, but never reset or release
     * buffers after fault. Neither write by itself is an idle proof. */
    if (d->touched) {
        d->driver->irq_enable(d, false);
        (void)pci_command(d, (d->saved_command | 1u) & ~4u);
    }
    if (d->irq_attached && !irq_chain_remove(d->pci.irq_line, irq_handler, d->owner)) d->irq_attached = false;
    for (unsigned i = 0; i < 3; i++) if (d->claims[i] >= 0)
        registry_quarantine(d->claims[i], d->claim_generation[i]);
    d->queue_count = 0;
    record(d);
}
void netif_quarantine(struct netif *d, const char *reason)
{
    if (!d || !d->driver) return;
    kmutex_lock(&d->mutex); quarantine_locked(d, reason); kmutex_unlock(&d->mutex);
}
static int stop_locked(struct netif *d)
{
    if (d->quarantined) return -NET_EIO;
    d->stopping = true; d->ready = false;
    if (d->touched) {
        d->driver->irq_enable(d, false);
        if (d->driver->stop(d)) { quarantine_locked(d, "stop_unproved"); return -NET_EIO; }
    }
    d->running = false;
    for (unsigned i = 0; i < 4; i++) net_tx_complete(d, i, false);
    if (d->irq_attached) {
        if (irq_chain_remove(d->pci.irq_line, irq_handler, d->owner)) {
            quarantine_locked(d, "callback_unproved"); return -NET_EIO;
        }
        d->irq_attached = false;
    }
    if (d->touched && pci_command(d, d->saved_command)) {
        quarantine_locked(d, "command_restore"); return -NET_EIO;
    }
    proof_device = d;
    for (unsigned i = 0; i < 3; i++) if (d->claims[i] >= 0) {
        const struct resource *r = registry_get(d->claims[i]);
        int err = !r || r->generation != d->claim_generation[i] ? -EINVAL : 0;
        if (!err && r->state == RS_CLAIMED) err = registry_activate(d->claims[i], d->claim_generation[i]);
        if (!err) err = registry_quiesce(d->claims[i], d->claim_generation[i], idle_proof);
        if (!err) err = registry_release(d->claims[i], d->claim_generation[i]);
        if (err) { proof_device = 0; quarantine_locked(d, "release_unproved"); return err; }
        d->claims[i] = -1;
    }
    proof_device = 0;
    if (d->dma_phys) {
        for (unsigned i = 0; i < d->driver->dma_pages; i++) pmm_free(d->dma_phys + i * PAGE_SIZE);
        d->dma_phys = 0; d->dma = 0;
    }
    d->queue_count = 0; d->link = NET_LINK_DOWN;
    return 0;
}
int netif_stop(struct netif *d)
{
    if (!d || !d->driver) return -EINVAL;
    kmutex_lock(&d->mutex); int rc = stop_locked(d); kmutex_unlock(&d->mutex); return rc;
}
static bool valid_mac(const uint8_t mac[6])
{
    unsigned bits = 0; for (unsigned i = 0; i < 6; i++) bits |= mac[i];
    return bits && !(mac[0] & 1);
}
int netif_attach(struct netif *d, const struct pci_func *p, const struct net_io *io)
{
    if (!d || !p || !io || !io->read || !io->write || !io->now || !io->sleep || !io->delay) return -EINVAL;
    if (d->driver) return -NET_EBUSY; /* includes previous failed/quarantined attempts */
    memset(d, 0, sizeof(*d));
    for (unsigned i = 0; i < 3; i++) d->claims[i] = -1;
    kmutex_init(&d->mutex); d->pci = *p; d->io = *io; d->reason = "admission";
    const struct netif_driver *all[] = { &net_e100, &net_rtl8139, &net_ne2000 };
    for (unsigned i = 0; i < ARRAY_SIZE(all); i++) if (all[i]->match(p)) { d->driver = all[i]; break; }
    if (!d->driver) return -NET_ENODEV;
    if (g_boot.flags & CBI_F_SAFE_MODE) { d->reason = "safe_mode"; return -NET_ENODEV; }
    unsigned bytes = d->driver->aperture;
    /* Intel table 3 specifies 32 bytes. QEMU's eepro100 PCI_IO_SIZE is
     * 64 bytes: retain its complete decoding aperture on validated twins. */
    if (d->driver == &net_e100 && (g_boot.flags & CBI_F_SMBIOS_QEMU)) bytes = 64;
    uint32_t bar = p->bar[d->driver->bar], base = bar & ~3u;
    if ((p->header_type & 0x7f) || !(bar & 1) || (base & (bytes - 1)) || !base || base > 0x10000u - bytes) {
        d->reason = "invalid_bar"; return -NET_ENODEV;
    }
    d->base = base; d->saved_command = p->command;
    if ((pci_read_config(d, p, 4) & 0xffff) != p->command || !power_d0(d) ||
        (!(p->command & 1) && (p->command & 4))) { d->reason = "firmware_state"; return -NET_EBUSY; }
    if ((p->command & 1) && !d->driver->idle(d)) { d->reason = "firmware_active"; return -NET_EBUSY; }
    d->generation = gen_alloc(); if (!d->generation) return -ENOMEM;
    ksnprintf(d->owner, sizeof(d->owner), "net-%u", d->generation);
    bool intx = routed(d);
    uint32_t starts[3] = { base, p->irq_line, 0 }, ends[3] = { base + bytes, p->irq_line + 1u, 0 };
    const enum res_type types[3] = { RES_PORT, RES_IRQ, RES_MMIO };
    int err = 0;
    for (unsigned i = 0; i < 2; i++) {
        if (i == 1 && !intx) continue;
        d->claims[i] = registry_claim(types[i], starts[i], ends[i], d->owner, i == 1);
        if (d->claims[i] < 0) { err = d->claims[i]; d->claims[i] = -1; break; }
        d->claim_generation[i] = registry_get(d->claims[i])->generation;
    }
    if (!err && d->driver->dma_pages) {
        d->dma_phys = pmm_alloc_contig(d->driver->dma_pages);
        if (!d->dma_phys) err = -ENOMEM;
        else {
            d->dma = P2V(d->dma_phys); memset(d->dma, 0, d->driver->dma_pages * PAGE_SIZE);
            d->claims[2] = registry_claim(RES_MMIO, d->dma_phys,
                d->dma_phys + d->driver->dma_pages * PAGE_SIZE, d->owner, false);
            if (d->claims[2] < 0) { err = d->claims[2]; d->claims[2] = -1; }
            else d->claim_generation[2] = registry_get(d->claims[2])->generation;
        }
    }
    for (unsigned i = 0; !err && i < 3; i++) if (d->claims[i] >= 0)
        err = registry_activate(d->claims[i], d->claim_generation[i]);
    if (!err) {
        d->touched = true;
        err = pci_command(d, (p->command | 1u) & ~4u);
        if (!err) err = d->driver->init(d);
        if (!err) {
            uint8_t address[6]; d->driver->mac(d, address); memcpy(d->address, address, sizeof(address));
            if (!valid_mac(d->address)) err = -NET_EIO;
        }
        if (!err && d->driver->dma_pages) err = pci_command(d, p->command | 5u);
        if (!err) err = d->driver->start(d);
        if (!err) {
            d->ready = d->running = true;
            if (intx) { err = irq_chain_add(p->irq_line, irq_handler, d->owner); d->irq_attached = !err; }
            if (!err) d->driver->irq_enable(d, d->irq_attached);
        }
    }
    if (err) {
        d->reason = "activation_failed";
        /* An init/start failure may leave unknown device work: retain pins.
         * Admission/allocation failures before touch have a safe rollback. */
        if (d->touched) netif_quarantine(d, "activation_failed");
        else (void)netif_stop(d);
        return err;
    }
    d->link = d->driver->link_state(d); d->link_deadline = io->now(io->cookie) + 1000;
    if (d->fault_pending) { netif_quarantine(d, "link_fault"); return -NET_EIO; }
    record(d); return 0;
}
int netif_transmit(struct netif *d, gen_t generation, const void *frame, unsigned length)
{
    if (!d || !frame || length < 14 || length > NET_FRAME_MAX) return -EINVAL;
    kmutex_lock(&d->mutex);
    int err = !d->ready || !d->running || d->stopping || d->quarantined || generation != d->generation ? -NET_ENODEV : 0;
    if (!err) err = d->driver->transmit(d, frame, length);
    if (err == -NET_EBUSY) d->stats.tx_busy++;
    else if (err && err != -NET_ENODEV) quarantine_locked(d, "transmit_fault");
    else if (!err) d->stats.tx_submitted++;
    kmutex_unlock(&d->mutex); return err;
}
void netif_poll(struct netif *d)
{
    if (!d || !d->driver) return;
    kmutex_lock(&d->mutex);
    if (!d->running || d->stopping || d->quarantined) { kmutex_unlock(&d->mutex); return; }
    for (unsigned i = 0; i < 3; i++) if (d->claims[i] >= 0 && !registry_valid(d->claims[i], d->claim_generation[i])) d->fault_pending = true;
    if (d->fault_pending) { quarantine_locked(d, "irq_or_registry_fault"); kmutex_unlock(&d->mutex); return; }
    d->servicing = true;
    int len = d->driver->receive_poll(d, d->scratch.data);
    d->servicing = false;
    if (len < 0 || len > (int)NET_FRAME_MAX) quarantine_locked(d, "receive_fault");
    else if (len) {
        d->stats.rx_packets++; d->stats.rx_bytes += len;
        if (d->queue_count == NET_QUEUE_MAX) d->stats.rx_dropped++;
        else {
            unsigned at = (d->queue_head + d->queue_count) % NET_QUEUE_MAX;
            d->queue[at].length = len; memcpy(d->queue[at].data, d->scratch.data, len);
            d->queue_count++;
            if (d->queue_count > d->stats.queue_peak) d->stats.queue_peak = d->queue_count;
        }
    }
    if (d->running && d->fault_pending) quarantine_locked(d, "irq_or_registry_fault");
    if (d->running) {
        d->irq_pending = false;
        d->driver->irq_enable(d, d->irq_attached);
        if (net_expired(d, d->link_deadline)) {
            enum net_link link = d->driver->link_state(d);
            if (d->fault_pending) quarantine_locked(d, "link_fault");
            else if (link != d->link) { d->link = link; record(d); }
            d->link_deadline = d->io.now(d->io.cookie) + 1000;
        }
    }
    kmutex_unlock(&d->mutex);
}
bool netif_pop(struct netif *d, struct net_packet *out)
{
    if (!d || !out) return false;
    kmutex_lock(&d->mutex);
    bool available = d->queue_count && !d->quarantined;
    if (available) {
        *out = d->queue[d->queue_head]; d->queue_head = (d->queue_head + 1) % NET_QUEUE_MAX; d->queue_count--;
    }
    kmutex_unlock(&d->mutex); return available;
}
void netif_snapshot(struct netif *d, struct net_stats *out)
{
    if (!d || !out) return;
    uint32_t f = irq_save(); *out = d->stats; out->queued = d->queue_count; irq_restore(f);
}
void net_set_input(net_input_fn fn, void *cookie)
{
    /* UP, non-preemptible kernel: publish callback revocation before a wait.
     * No mutex spans the stack callback, which may submit a response. */
    kmutex_lock(&registration_mutex);
    input = 0;
    while (input_active) task_sleep_ms(1);
    input_cookie = cookie; input = fn;
    kmutex_unlock(&registration_mutex);
}
void net_worker_step(void)
{
    struct net_packet packet;
    for (unsigned i = 0; i < device_count; i++) {
        if (shutting_down) break;
        netif_poll(&devices[i]); task_yield();
        if (input && netif_pop(&devices[i], &packet)) {
            net_input_fn fn = input; void *cookie = input_cookie;
            if (fn) {
                input_active = true; fn(&devices[i], packet.data, packet.length, cookie); input_active = false;
            }
        }
        task_yield();
    }
}
static void worker_main(void *arg)
{
    (void)arg;
    while (!shutting_down) { net_worker_step(); task_sleep_ms(1); }
    task_exit(0);
}
static uint32_t port_read(void *cookie, uint16_t port, unsigned width)
{
    (void)cookie;
    if (width == 1) return inb(port);
    if (width == 4) return inl(port);
    uint16_t v; __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(port)); return v;
}
static void port_write(void *cookie, uint16_t port, uint32_t value, unsigned width)
{
    (void)cookie;
    if (width == 1) outb(port, value);
    else if (width == 4) outl(port, value);
    else __asm__ volatile("outw %0,%1" :: "a"((uint16_t)value), "Nd"(port));
}
static uint64_t port_now(void *cookie) { (void)cookie; return g_ticks; }
static void port_sleep(void *cookie, uint32_t ms) { (void)cookie; task_sleep_ms(ms); }
static int port_delay(void *cookie, uint32_t us) { (void)cookie; return udelay(us); }
int net_init(void)
{
    if (initialized) return shutting_down ? -NET_ENODEV : 0;
    initialized = true; kmutex_init(&registration_mutex);
    if (g_boot.flags & CBI_F_SAFE_MODE) { klog("[net] disabled reason=safe_mode"); return 0; }
    for (unsigned i = 0; i < pci_count() && device_count < NETIF_MAX; i++) {
        const struct pci_func *p = pci_get(i);
        if (!net_e100.match(p) && !net_rtl8139.match(p) && !net_ne2000.match(p)) continue;
        struct netif *d = &devices[device_count++];
        struct net_io io = { 0, port_read, port_write, port_now, port_sleep, port_delay };
        int err = netif_attach(d, p, &io);
        if (err && !d->quarantined) record(d);
    }
    bool active = false;
    for (unsigned i = 0; i < device_count; i++) if (devices[i].running) active = true;
    if (!active) return 0;
    worker = task_create_kernel("net-input", worker_main, 0, P_DEVICE);
    if (!worker) { net_shutdown(); return -ENOMEM; }
    task_start(worker); return 0;
}
void net_shutdown(void)
{
    shutting_down = true; net_set_input(0, 0);
    for (unsigned i = 0; i < device_count; i++) (void)netif_stop(&devices[i]);
    if (worker && worker != g_current) {
        while (task_alive(worker)) task_sleep_ms(1);
        task_reap(worker); worker = 0;
    }
}
unsigned net_count(void) { return device_count; }
struct netif *net_get(unsigned i) { return i < device_count ? &devices[i] : 0; }
