/* Bounded NIC ownership, packet queues and thread service.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/mm.h>
#include <ciuki/task.h>
#include <ciuki/net.h>
#include <ciuki/net_control.h>
#include <ciuki/timing.h>
#include <ciuki/net_bridge.h>

#define NET_EIO 5
#define NET_EBUSY 16
#define NET_ENODEV 19
static struct netif devices[NETIF_MAX];
static unsigned device_count;
static bool initialized, shutting_down;
static struct task *worker;
static bool input_active;
static struct kmutex registration_mutex;
int net_stack_init(void) __attribute__((weak));
void net_stack_driver_start(void) __attribute__((weak));
bool net_stack_driver_service(void) __attribute__((weak)); /* true: frames still queued */
void net_stack_address(unsigned, uint32_t *) __attribute__((weak));
static net_input_fn input;
static void *input_cookie;
/* fx-11: the worker sleeps on this queue. The IRQ handler, a transmit, a
 * new device and shutdown wake it; a deadline bounds every sleep. */
static struct kwait worker_wait;
static bool worker_wait_ready, worker_kicked;
static struct net_worker_stats worker_stats;
/* f6-11 BEGIN: exclusive Ethernet type tap, independent of native ingress.
 * Serialized registration, revocation before waiting, no device mutex held
 * across the callback. The bridge callback must not unregister itself. */
static struct {
    net_bridge_tap_fn fn;
    void (*work)(struct netif *, void *);
    void *cookie; uint16_t type; bool active;
} bridge_tap;
/* Notification only under the NIC mutex: no bridge owner/driver lock cycle. */
static void bridge_tap_changed(struct netif *d) { d->bridge_epoch++; }
int net_bridge_tap_worker_register(void (*work)(struct netif *, void *), void *cookie)
{
    kmutex_lock(&registration_mutex);
    int e = !bridge_tap.fn || bridge_tap.cookie != cookie ? -NET_ENODEV : 0;
    if (!e) { uint32_t flags = irq_save(); bridge_tap.work = work; irq_restore(flags); }
    kmutex_unlock(&registration_mutex); return e;
}
static void bridge_tap_work(struct netif *d)
{
    uint32_t flags = irq_save();
    void (*work)(struct netif *, void *) = bridge_tap.work;
    if (!bridge_tap.fn || !work) { irq_restore(flags); return; }
    void *cookie = bridge_tap.cookie; bridge_tap.active = true; irq_restore(flags);
    work(d, cookie);
    flags = irq_save(); bridge_tap.active = false; irq_restore(flags);
}
int net_bridge_tap_register(uint16_t type, net_bridge_tap_fn fn, void *cookie)
{
    if (type != BRIDGE_IPX_TYPE || !fn) return -EINVAL;
    kmutex_lock(&registration_mutex);
    int e = shutting_down ? -NET_ENODEV : bridge_tap.fn ? -NET_EBUSY : 0;
    if (!e) {
        uint32_t flags = irq_save();
        bridge_tap.cookie = cookie; bridge_tap.type = type; bridge_tap.fn = fn;
        irq_restore(flags);
    }
    kmutex_unlock(&registration_mutex); return e;
}
void net_bridge_tap_unregister(net_bridge_tap_fn fn, void *cookie)
{
    kmutex_lock(&registration_mutex);
    if (bridge_tap.fn == fn && bridge_tap.cookie == cookie) {
        uint32_t flags = irq_save(); bridge_tap.fn = 0; bridge_tap.work = 0; irq_restore(flags);
        while (bridge_tap.active) task_sleep_ms(1);
        bridge_tap.cookie = 0; bridge_tap.type = 0;
    }
    kmutex_unlock(&registration_mutex);
}
static bool bridge_tap_frame(struct netif *d, const struct net_packet *p)
{
    uint32_t flags = irq_save();
    net_bridge_tap_fn fn = bridge_tap.fn;
    if (!fn || p->length < 14 || (((uint16_t)p->data[12] << 8) | p->data[13]) != bridge_tap.type) {
        irq_restore(flags); return false;
    }
    void *cookie = bridge_tap.cookie; bridge_tap.active = true; irq_restore(flags);
    fn(d, p->data, p->length, cookie);
    flags = irq_save(); bridge_tap.active = false; irq_restore(flags);
    return true;
}
/* f6-11 END: bridge tap. */
static bool native_destination(const struct netif *d, const struct net_packet *p)
{
    return !d->promiscuous || (p->length >= 14 &&
        ((p->data[0] & 1) || !memcmp(p->data, d->address, 6)));
}
static struct netif *proof_device;
static const struct netif_driver *const net_drivers[] = {
    &net_e100, &net_rtl8139, &net_ne2000, &net_e1000, &net_pcnet,
    &net_tulip, &net_3c90x, &net_rhine
};
static const struct netif_driver *find_driver(const struct pci_func *p)
{
    for (unsigned i = 0; i < ARRAY_SIZE(net_drivers); i++)
        if (net_drivers[i]->match(p)) return net_drivers[i];
    return 0;
}

uint32_t net_read(struct netif *d, unsigned reg, unsigned width)
{
    if (d->mmio) {
        if (width != 4 || (reg & 3) || reg > d->driver->aperture - 4) { d->fault_pending = true; return UINT32_MAX; }
        return d->io.read_mmio(d->io.cookie, d->mmio, reg);
    }
    return d->io.read(d->io.cookie, d->base + reg, width);
}
void net_write(struct netif *d, unsigned reg, uint32_t value, unsigned width)
{
    if (d->mmio) {
        if (width != 4 || (reg & 3) || reg > d->driver->aperture - 4) { d->fault_pending = true; return; }
        d->io.write_mmio(d->io.cookie, d->mmio, reg, value);
    }
    else d->io.write(d->io.cookie, d->base + reg, value, width);
}
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
         d->irq_attached && !d->irq_fallback ? "irq" : "poll", d->quarantined ? "quarantined" :
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
    return pci_irq_routed(&d->pci);
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
    struct irq_stamp start = irq_budget_stamp();
    bool handled = d->driver->interrupt(d);
    if (handled) { d->irq_pending = true; d->stats.interrupts++; }
    /* fx-11: the driver masked the NIC; only the worker's pass unmasks it. */
    if (handled && worker_wait_ready) kwait_wake_irq(&worker_wait, true);
    struct irq_elapsed elapsed = irq_budget_elapsed(start);
    uint64_t us = elapsed.us;
    if (us > d->stats.max_irq_us) d->stats.max_irq_us = us;
    if (elapsed.smi_attributed) d->stats.smi_attributed++;
    /* Keep isolated samples visible; fault cleanup follows three actual
     * work overruns in 100 ms, using the same policy as the shared chain. */
    if (elapsed.work_us > 50) d->stats.budget_violations++;
    if (irq_budget_fault(&d->irq_budget, elapsed, 50)) d->fault_pending = true;
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
    bridge_tap_changed(d); /* bounded worker drains after the NIC mutex */
    if(d->external_owner) {
        if(d->external_fault) d->external_fault(d,reason);
        d->queue_count=0;record(d);return;
    }
    /* Mask sources and revoke PCI bus mastering, but never reset or release
     * buffers after fault. Neither write by itself is an idle proof. */
    if (d->touched) {
        d->driver->irq_enable(d, false);
        (void)pci_command(d, (d->saved_command | (d->mmio_phys ? 2u : 1u)) & ~4u);
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
    bridge_tap_changed(d);
    if(d->external_owner) {
        int result=d->driver->stop(d);
        if(result==-EAGAIN || result==-NET_EBUSY) return result;
        if(result) { quarantine_locked(d,"stop_unproved");return result; }
        d->running=false;d->link=NET_LINK_DOWN;d->queue_count=0;record(d);return 0;
    }
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
    if (d->mmio) {
        vmm_revoke_mmio((void *)d->mmio, d->driver->aperture); d->mmio = 0;
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
        mm_release(d->dma_phys, d->driver->dma_pages, MM_NET);
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
    d->driver = find_driver(p);
    if (!d->driver) return -NET_ENODEV;
    if (g_boot.flags & CBI_F_SAFE_MODE) { d->reason = "safe_mode"; return -NET_ENODEV; }
    unsigned bytes = d->driver->aperture;
    /* Intel table 3 specifies 32 bytes. QEMU's eepro100 PCI_IO_SIZE is
     * 64 bytes: retain its complete decoding aperture on validated twins. */
    if (d->driver == &net_e100 && (g_boot.flags & CBI_F_SMBIOS_QEMU)) bytes = 64;
    bool mmio = d->driver == &net_e1000;
    unsigned decode = mmio ? 2 : 1;
    uint32_t bar = p->bar[d->driver->bar], base = bar & (mmio ? ~15u : ~3u);
    bool valid_bar = mmio ? !(bar & 1) && ((bar & 6) == 0 || ((bar & 6) == 4 && !p->bar[1])) &&
        base >= PMM_MAX_PAGES * PAGE_SIZE && base <= UINT32_MAX - bytes && io->read_mmio && io->write_mmio :
        (bar & 1) && base <= 0x10000u - bytes;
    if ((p->header_type & 0x7f) || !valid_bar || (base & (bytes - 1)) || !base) {
        d->reason = "invalid_bar"; return -NET_ENODEV;
    }
    if (mmio) d->mmio_phys = base; else d->base = base;
    d->saved_command = p->command;
    if ((pci_read_config(d, p, 4) & 0xffff) != p->command || !power_d0(d) ||
        (!(p->command & decode) && (p->command & 4))) { d->reason = "firmware_state"; return -NET_EBUSY; }
    d->generation = gen_alloc(); if (!d->generation) return -ENOMEM;
    ksnprintf(d->owner, sizeof(d->owner), "net-%u", d->generation);
    bool intx = routed(d);
    uint32_t starts[3] = { base, p->irq_line, 0 }, ends[3] = { base + bytes, p->irq_line + 1u, 0 };
    const enum res_type types[3] = { mmio ? RES_MMIO : RES_PORT, RES_IRQ, RES_MMIO };
    int err = 0;
    for (unsigned i = 0; i < 2; i++) {
        if (i == 1 && !intx) continue;
        d->claims[i] = registry_claim(types[i], starts[i], ends[i], d->owner, i == 1);
        if (d->claims[i] < 0) {
            if (i == 1) { d->claims[i] = -1; intx = false; d->reason = "irq_unavailable"; continue; }
            err = d->claims[i]; d->claims[i] = -1; break;
        }
        d->claim_generation[i] = registry_get(d->claims[i])->generation;
    }
    if (!err && mmio) {
        d->mmio = vmm_map_mmio(base, bytes, true);
        if (!d->mmio) err = -ENOMEM;
    }
    if (!err && d->driver->dma_pages) {
        d->dma_phys = pmm_alloc_contig(d->driver->dma_pages);
        if (!d->dma_phys) err = -ENOMEM;
        else {
            mm_claim(d->dma_phys, d->driver->dma_pages, MM_NET);
            d->dma = P2V(d->dma_phys); memset(d->dma, 0, d->driver->dma_pages * PAGE_SIZE);
            d->claims[2] = registry_claim(RES_MMIO, d->dma_phys,
                d->dma_phys + d->driver->dma_pages * PAGE_SIZE, d->owner, false);
            if (d->claims[2] < 0) { err = d->claims[2]; d->claims[2] = -1; }
            else d->claim_generation[2] = registry_get(d->claims[2])->generation;
        }
    }
    /* Indexed/windowed idle probes write selectors, so claims must succeed
     * first. They never reset, start DMA or acknowledge firmware IRQs. */
    if (!err && (p->command & decode) && !d->driver->idle(d)) {
        d->reason = "firmware_active"; err = -NET_EBUSY;
    }
    for (unsigned i = 0; !err && i < 3; i++) if (d->claims[i] >= 0)
        err = registry_activate(d->claims[i], d->claim_generation[i]);
    if (!err) {
        d->touched = true;
        err = pci_command(d, (p->command | decode) & ~4u);
        if (!err) err = d->driver->init(d);
        if (!err) {
            uint8_t address[6]; d->driver->mac(d, address); memcpy(d->address, address, sizeof(address));
            if (!valid_mac(d->address)) err = -NET_EIO;
        }
        if (!err && d->driver->dma_pages) err = pci_command(d, p->command | decode | 4u);
        if (!err) err = d->driver->start(d);
        if (!err) {
            d->ready = d->running = true;
            if (intx) {
                int irq_err = irq_chain_add(p->irq_line, irq_handler, d->owner);
                d->irq_attached = !irq_err;
                if (irq_err) d->reason = "irq_unavailable";
            }
            d->driver->irq_enable(d, d->irq_attached);
        }
    }
    if (err) {
        if (!strncmp(d->reason, "admission", 10)) d->reason = "activation_failed";
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
    int err = !d->ready || !d->running || d->stopping || d->quarantined || d->administratively_down || generation != d->generation ? -NET_ENODEV : 0;
    if (!err) err = d->driver->transmit(d, frame, length);
    if (err == -NET_EBUSY || err == -EAGAIN) d->stats.tx_busy++;
    else if (err && err != -NET_ENODEV) quarantine_locked(d, "transmit_fault");
    else if (!err) d->stats.tx_submitted++;
    kmutex_unlock(&d->mutex); return err;
}
int netif_promiscuous(struct netif *d, gen_t generation, bool on)
{
    if (!d || !d->driver) return -EINVAL;
    kmutex_lock(&d->mutex);
    int err = generation != d->generation || !d->ready || !d->running ||
        d->stopping || d->quarantined ? -NET_ENODEV : 0;
    if (!err && d->promiscuous != on) {
        err = d->driver->promiscuous ? d->driver->promiscuous(d, on) : -ENOSYS;
        if (!err && d->fault_pending) err = -NET_EIO;
        if (!err) d->promiscuous = on;
        else if (err != -NET_EBUSY && err != -ENOSYS) quarantine_locked(d, "receive_mode_fault");
    }
    kmutex_unlock(&d->mutex); return err;
}
bool netif_poll(struct netif *d)
{
    if (!d || !d->driver) return false;
    kmutex_lock(&d->mutex);
    if (!d->running || d->stopping || d->quarantined) { kmutex_unlock(&d->mutex); return false; }
    if (d->smi_reported != d->stats.smi_attributed) {
        d->smi_reported = d->stats.smi_attributed;
        klog("[net] smi_attributed owner=%s count=%llu", d->owner, d->smi_reported);
    }
    for (unsigned i = 0; i < 3; i++) if (d->claims[i] >= 0 && !registry_valid(d->claims[i], d->claim_generation[i])) d->fault_pending = true;
    if (d->fault_pending) { quarantine_locked(d, "irq_or_registry_fault"); kmutex_unlock(&d->mutex); return false; }
    d->servicing = true;
    int len = d->driver->receive_poll(d, d->scratch.data);
    bool received = len > 0 && len <= (int)NET_FRAME_MAX;
    d->servicing = false;
    if (len < 0 || len > (int)NET_FRAME_MAX) quarantine_locked(d, "receive_fault");
    else if (len) {
        d->stats.rx_packets++; d->stats.rx_bytes += len; d->rx_activity = d->io.now(d->io.cookie);
        if (d->administratively_down || d->queue_count == NET_QUEUE_MAX) d->stats.rx_dropped++;
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
            else if (link != d->link) {
                d->link = link;
                if (link == NET_LINK_DOWN) bridge_tap_changed(d);
                record(d);
            }
            d->link_deadline = d->io.now(d->io.cookie) + 1000;
        }
    }
    kmutex_unlock(&d->mutex);
    return received;
}
bool netif_pop(struct netif *d, struct net_packet *out)
{
    if (!d || !out) return false;
    kmutex_lock(&d->mutex);
    bool available = d->queue_count && !d->quarantined && !d->administratively_down;
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
void net_state_snapshot(struct ciuki_net_state *out)
{
    *out = (struct ciuki_net_state){ .version = CIUKI_NET_VERSION, .size = sizeof(*out),
                                   .state = CIUKI_NET_STOPPED };
    struct netif *d = net_get(0);
    for (unsigned i = 0; i < net_count(); i++) {
        struct netif *candidate = net_get(i);
        if (candidate->running && !candidate->quarantined && !candidate->administratively_down) { d = candidate; break; }
    }
    if (!d) {
        ksnprintf(out->reason, sizeof(out->reason), "%s", g_boot.flags & CBI_F_SAFE_MODE ? "safe_mode" : "no_adapter");
        return;
    }
    kmutex_lock(&d->mutex);
    out->present = 1;
    ksnprintf(out->driver, sizeof(out->driver), "%s", d->driver->name);
    memcpy(out->mac, d->address, sizeof(out->mac));
    out->link = d->link;
    out->state = d->quarantined ? CIUKI_NET_QUARANTINED : d->running && !d->administratively_down ? CIUKI_NET_RUNNING : CIUKI_NET_STOPPED;
    ksnprintf(out->reason, sizeof(out->reason), "%s", d->administratively_down ? "disabled" : d->reason ? d->reason : "unknown");
    out->rx_frames = d->stats.rx_packets;
    out->tx_frames = d->stats.tx_completed;
    if (net_stack_address) {
        uint32_t address = 0;
        for (unsigned i = 0; i < net_count(); i++) if (net_get(i) == d) net_stack_address(i, &address);
        memcpy(out->ipv4, &address, 4); out->address_configured = address != 0;
    }
    kmutex_unlock(&d->mutex);
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
/* fx-11, T23 trial 423b803a (fx-9 analysis): this worker polled every 1 ms
 * with three yields per pass even with a working IRQ, about 9.5 % of the
 * T23's CPU at an idle desktop, and took one frame per pass and millisecond.
 * A pass still makes one receive_poll per device (netif_poll unmasks the NIC
 * afterwards; the register sequence is unchanged) and returns how long to
 * sleep: 0 after a received frame (the next pass follows at once, see
 * worker_sleep), 1 ms while transmit frames wait for ring space,
 * NET_WORKER_IDLE_MS (the stack watch cadence; link checks are every 1 s)
 * when every device has a working IRQ, and the bounded fallback poll
 * (NET_FALLBACK_ACTIVE_MS for NET_FALLBACK_WINDOW_MS after a frame, else
 * NET_FALLBACK_IDLE_MS) for a device without one: not attached, external, or
 * found missing interrupts (frames waiting without a signal, three passes in
 * a row). */
static void missed_irq(struct netif *d, unsigned frames, bool signalled)
{
    if (!frames || !d->irq_attached || d->irq_fallback) return;
    if (signalled) { d->irq_missed = 0; return; }
    if (++d->irq_missed < 3) return;
    d->irq_fallback = true; worker_stats.fallbacks++;
    klog("[net] irq_fallback owner=%s irq=%u reason=missed frames=%u interrupts=%llu",
         d->owner, d->pci.irq_line, frames, (unsigned long long)d->stats.interrupts);
    record(d);
}
unsigned net_worker_step(void)
{
    struct net_packet packet;
    uint32_t flags = irq_save(); worker_kicked = false; worker_stats.passes++; irq_restore(flags);
    unsigned wait = NET_WORKER_IDLE_MS; bool more = false;
    if (net_stack_driver_service) {
        if (net_stack_driver_service()) wait = 1; /* transmit frames wait for ring space */
        task_yield();
    }
    for (unsigned i = 0; i < device_count; i++) {
        if (shutting_down) break;
        struct netif *d = &devices[i];
        /* The rest of a burst whose first frame was signalled was signalled too. */
        flags = irq_save(); bool signalled = d->irq_pending || d->rx_more; uint64_t interrupts = d->stats.interrupts; irq_restore(flags);
        bool received = netif_poll(d); unsigned frames = received;
        bridge_tap_work(d); /* no VM scheduling or incoming frame needed */
        task_yield();
        if ((input || bridge_tap.fn) && netif_pop(d, &packet) &&
            !bridge_tap_frame(d, &packet)) { /* f6-11 type tap before native ingress */
            net_input_fn fn = input; void *cookie = input_cookie;
            if (fn && native_destination(d, &packet)) {
                input_active = true; fn(d, packet.data, packet.length, cookie); input_active = false;
            }
        }
        flags = irq_save(); worker_stats.frames += frames;
        signalled = signalled || d->stats.interrupts != interrupts; irq_restore(flags);
        missed_irq(d, frames, signalled);
        d->rx_more = received;
        if (received) more = true;
        if (d->running && !d->quarantined && (!d->irq_attached || d->irq_fallback)) {
            uint64_t now = d->io.now ? d->io.now(d->io.cookie) : g_ticks;
            bool recent = d->rx_activity && now - d->rx_activity < NET_FALLBACK_WINDOW_MS;
            unsigned poll = recent ? NET_FALLBACK_ACTIVE_MS : NET_FALLBACK_IDLE_MS;
            if (poll < wait) wait = poll;
        }
    }
    return more ? 0 : wait;
}
static bool worker_wanted(void *arg)
{
    (void)arg;
    if (worker_kicked || shutting_down) return true;
    for (unsigned i = 0; i < device_count; i++) if (devices[i].irq_pending) return true;
    return false;
}
static void worker_wait_init(void)
{
    if (!worker_wait_ready) { kwait_init(&worker_wait); worker_wait_ready = true; }
}
void net_worker_kick(void)
{
    uint32_t flags = irq_save(); worker_kicked = true; worker_stats.kicks++; irq_restore(flags);
    if (worker_wait_ready) kwait_wake_all(&worker_wait);
}
void net_worker_snapshot(struct net_worker_stats *out)
{
    uint32_t flags = irq_save(); *out = worker_stats; irq_restore(flags);
}
/* The sleep after a pass: back-to-back passes while frames arrive, but after
 * NET_WORKER_BATCH of them 1 ms, so a flood cannot hold the CPU at device
 * priority (16 frames per ms, above 100 Mbit/s of full frames). */
static unsigned worker_sleep(unsigned wait, unsigned *burst)
{
    if (wait) { *burst = 0; return wait; }
    if (++*burst < NET_WORKER_BATCH) return 0;
    *burst = 0; return 1;
}
static void worker_main(void *arg)
{
    (void)arg; unsigned burst = 0;
    while (!shutting_down) {
        unsigned wait = worker_sleep(net_worker_step(), &burst);
        if (shutting_down) break;
        if (!wait) { task_yield(); continue; }
        if (kwait_wait_until(&worker_wait, worker_wanted, 0, deadline_after_ms(wait))) {
            uint32_t flags = irq_save(); bool irq = false;
            for (unsigned i = 0; i < device_count; i++) if (devices[i].irq_pending) irq = true;
            if (irq) worker_stats.irq_wakes++;
            irq_restore(flags);
        }
    }
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
static uint32_t mmio_read(void *cookie, volatile void *mapping, unsigned reg)
{ (void)cookie; return *(volatile uint32_t *)((volatile uint8_t *)mapping + reg); }
static void mmio_write(void *cookie, volatile void *mapping, unsigned reg, uint32_t value)
{ (void)cookie; *(volatile uint32_t *)((volatile uint8_t *)mapping + reg) = value; }
int net_init(void)
{
    if (initialized) return shutting_down ? -NET_ENODEV : 0;
    initialized = true; kmutex_init(&registration_mutex);
    if (g_boot.flags & CBI_F_SAFE_MODE) {
        if (net_stack_init && net_stack_init()) { klog("[net] disabled reason=stack_no_memory"); return -ENOMEM; }
        klog("[net] disabled reason=safe_mode"); return 0;
    }
    for (unsigned i = 0; i < pci_count() && device_count < NETIF_MAX; i++) {
        const struct pci_func *p = pci_get(i);
        if (!find_driver(p)) continue;
        struct netif *d = &devices[device_count++];
        struct net_io io = { 0, port_read, port_write, port_now, port_sleep, port_delay, mmio_read, mmio_write };
        int err = netif_attach(d, p, &io);
        if (err && !d->quarantined) record(d);
    }
    bool active = false;
    for (unsigned i = 0; i < device_count; i++) if (devices[i].running) active = true;
    if (net_stack_init && net_stack_init()) {
        net_shutdown(); klog("[net] disabled reason=stack_no_memory"); return -ENOMEM;
    }
    if (net_stack_driver_start) net_stack_driver_start();
    if (!active) return 0;
    worker_wait_init();
    worker = task_create_kernel("net-input", worker_main, 0, P_DEVICE);
    if (!worker) { net_shutdown(); return -ENOMEM; }
    task_start(worker); return 0;
}
void net_shutdown(void)
{
    shutting_down = true; net_worker_kick(); net_set_input(0, 0);
    net_bridge_tap_unregister(bridge_tap.fn, bridge_tap.cookie); /* f6-11 tap revocation */
    for (unsigned i = 0; i < device_count; i++) (void)netif_stop(&devices[i]);
    if (worker && worker != g_current) {
        while (task_alive(worker)) task_sleep_ms(1);
        task_reap(worker); worker = 0;
    }
}
int net_power_quiesce(void)
{
    /* The general shutdown API is also used at initialization failure. S5
     * needs a bounded barrier and must observe every driver's stop result. */
    uint64_t end = deadline_after_ms(2000);
    shutting_down = true; net_worker_kick();
    if (!kmutex_lock_until(&registration_mutex,end)) return -NET_EBUSY;
    uint32_t flags = irq_save();
    input = 0; bridge_tap.fn = 0; bridge_tap.work = 0;
    irq_restore(flags);
    unsigned waits = 0;
    while (input_active || bridge_tap.active) {
        if (waits++ == 2000 || deadline_passed(end)) { kmutex_unlock(&registration_mutex); return -NET_EBUSY; }
        task_sleep_ms(1);
    }
    input_cookie = 0; bridge_tap.cookie = 0; bridge_tap.type = 0;
    kmutex_unlock(&registration_mutex);
    for (unsigned i = 0; i < device_count; i++) {
        struct netif *d = &devices[i];
        if (deadline_passed(end)) return -NET_EBUSY;
        if (!kmutex_lock_until(&d->mutex,end)) return -NET_EBUSY;
        if (d->stopping && !d->ready && !d->running && !d->quarantined && !d->dma_phys && !d->mmio &&
            d->claims[0] < 0 && d->claims[1] < 0 && d->claims[2] < 0) { kmutex_unlock(&d->mutex); continue; }
        int e = stop_locked(d); kmutex_unlock(&d->mutex);
        if (e) return e;
    }
    waits = 0;
    while (worker && task_alive(worker)) {
        if (worker == g_current || waits++ == 2000 || deadline_passed(end)) return -NET_EBUSY;
        task_sleep_ms(1);
    }
    if (worker) { task_reap(worker); worker = 0; }
    return 0;
}
unsigned net_count(void) { return device_count; }
struct netif *net_get(unsigned i) { return i < device_count ? &devices[i] : 0; }

/* Device mutex protects registers and the counters. No lwIP callback or disk
 * operation runs while this mutex is held. Unsupported drivers expose auto. */
int net_control_driver(unsigned index, struct ciuki_net_control *out, bool apply)
{
    struct netif *d = net_get(index); out->count = net_count();
    if (!d) return -ENODEV;
    kmutex_lock(&d->mutex);
    bool media = d->driver == &net_e100 || (d->driver == &net_e1000 && !d->media_control);
    int e = 0;
    if (apply) {
        if (!d->running || d->quarantined) e = -ENETDOWN;
        else if (!media && out->config.speed) e = -EOPNOTSUPP;
        else if (d->configured_speed != out->config.speed || d->configured_duplex != out->config.duplex) {
            e = d->driver == &net_e100 ? net_e100_media(d, out->config.speed, out->config.duplex) :
                d->driver == &net_e1000 ? net_e1000_media(d, out->config.speed, out->config.duplex) : 0;
            if (!e) { d->configured_speed = out->config.speed; d->configured_duplex = out->config.duplex; }
        }
        if (!e && d->administratively_down != !out->config.enabled) {
            d->administratively_down = !out->config.enabled;
            d->queue_head = d->queue_count = 0; bridge_tap_changed(d);
        }
        if (!e) d->config_epoch++;
    } else {
        ksnprintf(out->name, sizeof(out->name), "en%u", index);
        ksnprintf(out->driver, sizeof(out->driver), "%s", d->driver->name);
        memcpy(out->mac, d->address, 6);
        out->generation = d->generation;
        out->pci_vendor = d->pci.vendor; out->pci_device = d->pci.device; out->irq = d->pci.irq_line;
        out->io_base = d->base; out->mmio_base = d->mmio_phys;
        out->link = d->link; out->state = d->quarantined ? CIUKI_NET_QUARANTINED : d->running && !d->administratively_down ? CIUKI_NET_RUNNING : CIUKI_NET_STOPPED;
        out->media_caps = CIUKI_NET_MEDIA_AUTO | (media ? CIUKI_NET_MEDIA_10 | CIUKI_NET_MEDIA_100 : 0);
        out->speed = d->speed; out->duplex = d->duplex;
        out->rx_packets = d->stats.rx_packets; out->tx_packets = d->stats.tx_completed;
        out->rx_errors = d->stats.rx_errors; out->tx_errors = d->stats.tx_errors;
    }
    kmutex_unlock(&d->mutex); return e;
}

struct netif *netif_publish_external(const struct pci_func *p,const struct netif_driver *driver,
                                      void *context,const uint8_t mac[6],gen_t generation,bool carrier,
                                      void (*fault)(struct netif *,const char *))
{
    if(!initialized || shutting_down || !p || !driver || !context || !fault || !generation ||
       !valid_mac(mac) || (g_boot.flags&CBI_F_SAFE_MODE)) return 0;
    kmutex_lock(&registration_mutex);
    unsigned slot=device_count;
    for(unsigned i=0;i<device_count;i++) if(devices[i].pci.bus==p->bus && devices[i].pci.dev==p->dev && devices[i].pci.fn==p->fn) { slot=i;break; }
    struct netif *d=slot<NETIF_MAX?&devices[slot]:0;
    if(d && slot<device_count) {
        bool free=!d->running && !d->quarantined && !d->irq_attached && !d->dma_phys && !d->servicing;
        for(unsigned i=0;i<3;i++) if(d->claims[i]>=0) free=false;
        if(!free) d=0;
    }
    bool start_worker=false;
    if(d && !worker) {
        worker_wait_init();
        worker=task_create_kernel("net-input",worker_main,0,P_DEVICE);
        if(worker) start_worker=true;else d=0;
    }
    if(d) {
        memset(d,0,sizeof(*d));kmutex_init(&d->mutex);
        d->pci=*p;d->driver=driver;d->driver_context=context;d->external_owner=true;d->external_fault=fault;
        d->generation=generation;d->ready=d->running=true;d->link=carrier?NET_LINK_UP:NET_LINK_DOWN;
        d->reason="ndis_running";memcpy(d->address,mac,6);ksnprintf(d->owner,sizeof(d->owner),"ndis-%u:%u.%u",p->bus,p->dev,p->fn);
        for(unsigned i=0;i<3;i++) d->claims[i]=-1;
        d->io=(struct net_io){0,port_read,port_write,port_now,port_sleep,port_delay,mmio_read,mmio_write};
        if(slot==device_count) device_count++;
        if(start_worker) task_start(worker);
        if(d) record(d);
    }
    kmutex_unlock(&registration_mutex);
    if(d) net_worker_kick(); /* fx-11: an external device without an IRQ needs the fallback poll */
    return d;
}
