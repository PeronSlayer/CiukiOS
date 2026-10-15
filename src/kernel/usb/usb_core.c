/* USB enumeration and serialized service. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/usb.h>
#include <ciuki/input.h>
#ifndef CIUKI_USB_HOST
#include <ciuki/cpu.h>
#include <ciuki/mm.h>
#include <ciuki/task.h>
#endif
static struct usb_host hosts[USB_HOST_MAX];
static unsigned host_count;
bool usb_input_ready(void)
{
    struct input_devices devices;
    input_devices_snapshot(&devices);
    return devices.usb != 0;
}
unsigned usb_count(void) { return host_count; }
const struct usb_host *usb_get(unsigned i) { return i < host_count ? &hosts[i] : 0; }
uint32_t usb_read(struct usb_host *h, unsigned at, unsigned width)
{ return h->io.read(h->io.cookie, at, width); }
void usb_write(struct usb_host *h, unsigned at, uint32_t v, unsigned width)
{ h->io.write(h->io.cookie, at, v, width); }
bool usb_port_has_live_child(struct usb_host *h, unsigned port)
{
    if (!h->root_child[port]) return false;
    struct usb_device *d = &h->devices[h->root_child[port] - 1];
    return d->present && !d->quarantined && !d->storage.block.quarantined;
}
void usb_port_action(struct usb_host *h, unsigned port, const char *event,
                     unsigned before, unsigned written, int result)
{
    struct usb_port_log *log = &h->root_log[port];
    unsigned count = ++log->operations;
    if (count > USB_PORT_ACTION_TRACE_LIMIT && !usb_expired(h, log->next_operation)) return;
    bool uhci = h->pci.prog_if == 0;
    unsigned raw = usb_read(h, uhci ? 0x10 + 2 * port : 0x54 + 4 * port, uhci ? 2 : 4);
    klog("[usb] port_action port=%u event=%s before=%08x write=%08x status=%08x count=%u controller=%s tick=%llu result=%d",
         port + 1, event, before, written, raw, count, h->owner, h->io.now(h->io.cookie), result);
    log->next_operation = h->io.now(h->io.cookie) + 1000;
}
bool usb_port_record(struct usb_host *h, unsigned port, const char *event, int result)
{
    struct usb_port_log *log = &h->root_log[port];
    if (h->root_blocked[port] && strncmp(event, "disconnect", 11) &&
        (strncmp(event, "connect", 8) || log->quarantine_reported)) return false;
    log->pending = event; log->result = result;
    if (!usb_expired(h, log->next_record)) return false;
    bool uhci = h->pci.prog_if == 0;
    unsigned raw = usb_read(h, uhci ? 0x10 + 2 * port : 0x54 + 4 * port, uhci ? 2 : 4);
    bool quarantined = h->root_blocked[port] && !strncmp(event, "connect", 8);
    klog("[usb] port=%u status=%08x event=%s reason=%s controller=%s tick=%llu result=%d",
         port + 1, raw, event, quarantined ? "quarantined" : "state",
         h->owner, h->io.now(h->io.cookie), result);
    log->next_record = h->io.now(h->io.cookie) + 1000; log->pending = 0;
    if (quarantined) log->quarantine_reported = true;
    return true;
}
bool usb_expired(struct usb_host *h, uint64_t deadline)
{ return (int64_t)(h->io.now(h->io.cookie) - deadline) >= 0; }
void usb_hid_service(struct usb_host *h)
{
    if (!h->ready || h->quarantined) return;
    if (h->pci.prog_if == 0x10 && (usb_read(h, 0x0c, 4) & 2)) {
        /* All TDs stay pinned. DI=0 writes back each frame's done chain;
         * acknowledge it without walking/reusing another pipe's TD. */
        *(volatile uint32_t *)(h->dma + 0x84) = 0;
        usb_barrier(); usb_write(h, 0x0c, 2, 4);
    }
    for (unsigned i = 0; i < h->device_count && h->ready; i++) {
        struct usb_device *d = &h->devices[i];
        if (!d->present || !d->configured || d->quarantined || d->hid_fault) continue;
        for (unsigned j = 0; j < USB_HID_MAX && !d->hid_fault && h->ready; j++)
            usb_hid_poll(d, &d->hid[j]);
    }
}
void usb_host_take(struct usb_host *h)
{
    for (;;) {
        fs_lock_take(&h->transfer_lock);
        if (!h->busy) { h->busy = true; return; }
        usb_hid_service(h);
        fs_lock_drop(&h->transfer_lock);
        h->io.sleep(h->io.cookie, 1);
    }
}
void usb_host_drop(struct usb_host *h)
{ h->busy = false; fs_lock_drop(&h->transfer_lock); }
bool usb_host_take_until(struct usb_host *h, uint64_t deadline)
{
    for (unsigned waits = 0; waits < USB_BIND_TIMEOUT_MS; waits++) {
        if (usb_expired(h, deadline)) return false;
        if (fs_lock_try_take(&h->transfer_lock)) {
            if (!h->busy) { h->busy = true; return true; }
            usb_hid_service(h);
            fs_lock_drop(&h->transfer_lock);
        }
        h->io.sleep(h->io.cookie, 1);
    }
    return false;
}
void usb_sleep(struct usb_host *h, unsigned ms)
{
    /* A reservation protects the general TD/BOT sequence, not the mutex.
     * Interrupt slots can progress during reset, retirement and bulk NAKs. */
    while (ms--) {
        bool reserved = h->busy;
        if (reserved) fs_lock_drop(&h->transfer_lock);
        h->io.sleep(h->io.cookie, 1);
        if (reserved) {
            fs_lock_take(&h->transfer_lock);
            usb_hid_service(h);
        }
    }
}
int usb_wait_register(struct usb_host *h, unsigned at, unsigned width, uint32_t mask, uint32_t want, unsigned ms)
{
    uint64_t end = h->io.now(h->io.cookie) + ms;
    for (unsigned i = 0; i <= ms; i++) {
        uint32_t v = usb_read(h, at, width);
        if (v == (width == 2 ? 0xffffu : UINT32_MAX)) return -USB_EIO;
        if ((v & mask) == want) return 0;
        if (usb_expired(h, end)) break;
        usb_sleep(h, 1);
    }
    return -USB_ETIMEDOUT;
}
int usb_retire_frames(struct usb_host *h, unsigned off, unsigned width, unsigned mask)
{
    unsigned start = usb_read(h, off, width) & mask;
    for (unsigned i = 0; i < 20; i++) {
        usb_sleep(h, 1);
        unsigned elapsed = (usb_read(h, off, width) - start) & mask;
        if (elapsed >= 2 && elapsed < mask / 2) return 0;
    }
    usb_host_quarantine(h, "frame_clock_stalled"); return -USB_ETIMEDOUT;
}
void usb_detach(struct usb_device *d, const char *reason)
{
    usb_storage_detach(d);
    for (unsigned i = 0; i < USB_HID_MAX; i++) usb_hid_release(d, &d->hid[i]);
    for (unsigned i = 0; i < d->hub_ports; i++) if (d->hub_child[i]) {
        usb_detach(&d->host->devices[d->hub_child[i] - 1], reason); d->hub_child[i] = 0;
    }
    d->present = d->configured = false; d->reason = reason;
}
void usb_device_quarantine(struct usb_device *d, const char *reason)
{
    usb_detach(d, reason); d->quarantined = true;
    struct usb_host *h = d->host;
    if (!d->parent) {
        h->root_blocked[d->root_port] = true;
        if (h->ready) h->ops->port_disable(h, d->root_port);
    } else {
        struct usb_device *hub = &h->devices[d->parent - 1];
        hub->hub_blocked[d->port] = true;
        if (h->ready && hub->present && !hub->quarantined &&
            usb_control(hub, 0x23, 1, 1, d->port + 1u, 0, 0))
            usb_host_quarantine(h, "hub_disable_failed");
    }
    /* A device fault cannot authorize re-enumerating that port until reboot. */
    klog("[usb] quarantine device addr=%u reason=%s stage=%s", d->address, reason,
         d->storage.trace.stage ? d->storage.trace.stage : "enumeration");
}
void usb_host_quarantine(struct usb_host *h, const char *reason)
{
    if (h->quarantined) return;
    h->quarantined = true; h->ready = false; h->reason = reason;
    if (h->owned) {
        if (h->pci.prog_if == 0) { usb_write(h, 4, 0, 2); usb_write(h, 0, 0, 2); usb_write(h, 2, 0x1f, 2); }
        else if (h->pci.prog_if == 0x10) { usb_write(h, 0x14, UINT32_MAX, 4); usb_write(h, 4, 0, 4); usb_write(h, 0x0c, 0x7f, 4); }
        /* Do not restore firmware DMA on an uncertain failure. */
        h->io.config_write(h->io.cookie, 4, (h->pci.command | (h->pci.prog_if ? 2 : 1)) & ~4u, 2);
    }
    for (unsigned i = 0; i < h->device_count; i++) usb_detach(&h->devices[i], "controller_quarantined");
    for (unsigned i = 0; i < 3; i++) if (h->claims[i] >= 0) registry_quarantine(h->claims[i], h->claim_gen[i]);
    klog("[usb] quarantine controller=%s reason=%s", h->owner, reason);
}
static int claim(struct usb_host *h, unsigned slot, enum res_type type, uint32_t start, uint32_t end, bool share)
{
    int n = registry_claim(type, start, end, h->owner, share);
    if (n < 0) return n;
    h->claims[slot] = n; h->claim_gen[slot] = registry_get((unsigned)n)->generation;
    return registry_activate(n, h->claim_gen[slot]);
}
static bool blocked(void)
{ return (g_boot.flags & (CBI_F_SAFE_MODE | CBI_F_INPUT_FORCED)) || g_boot.input_policy == CBI_INPUT_FIRMWARE; }
static int ehci_route(struct usb_host *h)
{
    unsigned caplen = usb_read(h, 0, 1), params = usb_read(h, 4, 4);
    if (caplen < 0x10 || caplen > 0x80 || !(params & 15) || (params & 15) > USB_PORT_MAX || !(params & 0xf000)) return -USB_EPROTO;
    unsigned eecp = (usb_read(h, 8, 4) >> 8) & 255;
    bool seen[64] = {0};
    for (unsigned n = 0; eecp && n < 48; n++) {
        if (eecp < 0x40 || eecp > 0xf8 || (eecp & 3) || seen[eecp / 4]) return -USB_EPROTO;
        seen[eecp / 4] = true;
        unsigned cap = h->io.config_read(h->io.cookie, eecp, 4);
        if ((cap & 255) == 1) {
            h->io.config_write(h->io.cookie, eecp, cap | (1u << 24), 4);
            for (unsigned i = 0; i < 500; i++) {
                cap = h->io.config_read(h->io.cookie, eecp, 4);
                if (!(cap & (1u << 16))) break;
                h->io.sleep(h->io.cookie, 1);
            }
            if (cap & (1u << 16)) return -USB_ETIMEDOUT;
            /* Disable legacy SMI enables, clear defined W1C statuses. */
            h->io.config_write(h->io.cookie, eecp + 4, 0xe0000000u, 4);
            break;
        }
        eecp = (cap >> 8) & 255;
        if (n == 47 && eecp) return -USB_EPROTO;
    }
    h->owned = true;
    usb_write(h, caplen + 8, 0, 4); usb_write(h, caplen, usb_read(h, caplen, 4) & ~1u, 4);
    int err = usb_wait_register(h, caplen + 4, 4, 1u << 12, 1u << 12, 100);
    if (err) return err;
    h->io.config_write(h->io.cookie, 4, (h->pci.command | 2u) & ~4u, 2);
    usb_write(h, caplen + 0x40, 0, 4);
    if (usb_read(h, caplen + 0x40, 4) & 1) return -USB_EIO;
    h->ports = params & 15; h->reason = "companion_routing"; return 0;
}
static bool power_d0(struct usb_host *h)
{
    unsigned status = h->io.config_read(h->io.cookie, 4, 4);
    if (!(status & (1u << 20))) return true;
    unsigned at = h->io.config_read(h->io.cookie, 0x34, 4) & 255;
    bool seen[64] = {0};
    for (unsigned i = 0; at && i < 48; i++) {
        if (at < 0x40 || at > 0xf8 || (at & 3) || seen[at / 4]) return false;
        seen[at / 4] = true;
        unsigned cap = h->io.config_read(h->io.cookie, at, 4);
        if ((cap & 255) == 1) return !(h->io.config_read(h->io.cookie, at + 4, 4) & 3);
        at = (cap >> 8) & 255;
    }
    return !at;
}
int usb_attach(struct usb_host *h, const struct pci_func *p, const struct usb_io *io, uint8_t *dma, uint32_t phys)
{
    if (!h || !p || !io || !io->read || !io->write || !io->config_read || !io->config_write || !io->now || !io->sleep) return -EINVAL;
    if (h->attempted) return -USB_EIO;
    void *mapping = h->mmio;
    memset(h, 0, sizeof(*h)); h->mmio = mapping; h->attempted = true; h->pci = *p; h->io = *io;
    fs_lock_init(&h->transfer_lock);
    for (unsigned i = 0; i < 3; i++) h->claims[i] = -1;
    h->reason = "unsupported_controller";
    if (p->class_code != 0x0c || p->subclass != 3 || (p->prog_if != 0 && p->prog_if != 0x10 && p->prog_if != 0x20)) return -ENOSYS;
    if (blocked()) { h->reason = "firmware_first_or_safe_mode"; return -ENOSYS; }
    if (p->header_type & 0x7f) return -EINVAL;
    bool uhci = p->prog_if == 0;
    unsigned bar = p->bar[uhci ? 4 : 0], bytes = uhci ? 32 : 4096;
    h->base = bar & (uhci ? ~3u : ~15u);
    if (!h->base || bar == UINT32_MAX || (uhci ? !(bar & 1) || h->base > 65536 - bytes : (bar & 7) || h->base > UINT32_MAX - bytes) || (h->base & (bytes - 1))) {
        h->reason = "invalid_bar"; return -EINVAL;
    }
    if (!uhci && p->prog_if != 0x20) h->ops = &usb_ohci_ops;
    if (uhci) h->ops = &usb_uhci_ops;
    if (h->ops && (!dma || !phys || (phys & 4095) || ((uintptr_t)dma & 4095) || phys > 768u * 1024 * 1024 - USB_DMA_BYTES)) {
        h->reason = "dma_bounds"; return -EINVAL;
    }
    if ((io->config_read(io->cookie, 4, 2) & 0xffff) != p->command || !power_d0(h)) {
        h->reason = "firmware_config_or_power_state"; return -USB_EIO;
    }
    h->dma = dma; h->dma_phys = phys;
    h->generation = gen_alloc(); h->next_address = 1;
    if (!h->generation) return -ENOSPC;
    ksnprintf(h->owner, sizeof(h->owner), "usb-%u", h->generation);
    int err = claim(h, 0, uhci ? RES_PORT : RES_MMIO, h->base, h->base + bytes, false);
    if (!err && p->irq_pin && p->irq_line >= 3 && p->irq_line < 16 && p->irq_line != 8 && p->irq_line != 13)
        err = claim(h, 1, RES_IRQ, p->irq_line, p->irq_line + 1u, true);
    if (!err && h->ops) err = claim(h, 2, RES_MMIO, phys, phys + USB_DMA_BYTES, false);
    if (err) {
        /* No controller write occurred. Retain partial leases in quarantine;
         * no idle proof or runtime recovery is published by this driver. */
        for (unsigned i = 0; i < 3; i++) if (h->claims[i] >= 0) {
            registry_quarantine(h->claims[i], h->claim_gen[i]);
        }
        h->quarantined = true; h->reason = "resource_conflict"; return err;
    }
    h->touched = true;
    io->config_write(io->cookie, 4, (p->command | (uhci ? 1 : 2)), 2);
    unsigned cmd = io->config_read(io->cookie, 4, 2);
    if ((cmd & 7) != ((p->command | (uhci ? 1 : 2)) & 7u)) err = -USB_EIO;
    if (!err) err = h->ops ? h->ops->start(h) : ehci_route(h);
    if (err) { usb_host_quarantine(h, "activation_or_handoff"); return err; }
    h->ready = h->ops != 0; if (h->ready) h->reason = "ready_polling";
    klog("[usb] controller=%s ports=%u pci=%02x:%02x.%u mode=poll reason=%s",
         uhci ? "uhci" : p->prog_if == 0x10 ? "ohci" : "ehci-companion", h->ports, p->bus, p->dev, p->fn, h->reason);
    return 0;
}
static int packet(struct usb_device *d, unsigned pid, unsigned toggle, void *data, unsigned len)
{
    if (!d->host->ready || d->host->quarantined || d->quarantined) return -USB_EIO;
    return d->host->ops->packet(d, 0, pid, toggle, data, len, false);
}
int usb_control(struct usb_device *d, uint8_t type, uint8_t req, uint16_t value, uint16_t index, void *data, unsigned len)
{
    if (!d || !d->host || len > USB_CONFIG_MAX || (len && !data) || !d->ep0 || d->ep0 > 64) return -EINVAL;
    uint8_t setup[8] = {type, req, value, value >> 8, index, index >> 8, len, len >> 8};
    int err = packet(d, USB_PID_SETUP, 0, setup, 8); if (err != 8) return err < 0 ? err : -USB_EPROTO;
    unsigned got = 0, toggle = 1;
    while (got < len) {
        unsigned n = len - got; if (n > d->ep0) n = d->ep0;
        err = packet(d, type & 0x80 ? USB_PID_IN : USB_PID_OUT, toggle, (uint8_t *)data + got, n);
        if (err < 0) return err;
        if ((unsigned)err > n || (!(type & 0x80) && (unsigned)err != n)) return -USB_EPROTO;
        got += (unsigned)err; toggle ^= 1;
        if ((unsigned)err < n) break;
    }
    err = packet(d, type & 0x80 ? USB_PID_OUT : USB_PID_IN, 1, 0, 0);
    return err ? err < 0 ? err : -USB_EPROTO : (int)got;
}
static unsigned le16(const uint8_t *p) { return p[0] | (unsigned)p[1] << 8; }
int usb_parse_config(struct usb_device *d, const uint8_t *b, unsigned len)
{
    if (len < 9 || len > USB_CONFIG_MAX || b[0] != 9 || b[1] != 2 || le16(b + 2) != len || !b[5]) return -USB_EPROTO;
    d->config = b[5];
    struct usb_hid *hid = 0;
    struct usb_storage *storage = 0;
    unsigned storage_endpoints = 0;
    memset(&d->storage, 0, sizeof(d->storage));
    unsigned count = 0, report_len = 0, endpoints = 0, interfaces = 0;
    uint8_t seen_interface[32] = {0};
    for (unsigned at = 9; at < len; ) {
        if (len - at < 2 || b[at] < 2 || b[at] > len - at) return -USB_EPROTO;
        unsigned size = b[at]; const uint8_t *p = b + at;
        if (p[1] == 4) {
            if (size < 9) return -USB_EPROTO;
            hid = 0; storage = 0; report_len = 0;
            if (!p[3]) {
                unsigned bit = 1u << (p[2] & 7);
                if (seen_interface[p[2] / 8] & bit) return -USB_EPROTO;
                seen_interface[p[2] / 8] |= (uint8_t)bit; interfaces++;
            }
            if (!p[3] && p[5] == 8 && p[6] == 6 && p[7] == 0x50) {
                if (d->low_speed || d->storage.bound || p[4] != 2) return -USB_EPROTO;
                storage = &d->storage; storage->bound = true; storage->interface = p[2];
            }
            if (!p[3] && p[5] == 3 && p[6] == 1 && (p[7] == 1 || p[7] == 2)) {
                if (count == USB_HID_MAX) return -ENOSPC;
                hid = &d->hid[count++];
                *hid = (struct usb_hid){.interface=p[2], .protocol=p[7]};
            }
        } else if (p[1] == 0x21 && hid) {
            if (size < 9 || p[5] < 1 || size < 6u + p[5] * 3u) return -USB_EPROTO;
            for (unsigned i = 0; i < p[5]; i++) if (p[6 + i * 3] == 0x22) report_len = le16(p + 7 + i * 3);
            hid->report_length = (uint16_t)report_len;
        } else if (p[1] == 5) {
            if (size < 7) return -USB_EPROTO;
            if (storage) {
                unsigned mps = le16(p + 4), ep = p[2] & 15;
                unsigned bit = 1u << (ep + (p[2] & 0x80 ? 16 : 0));
                if (!ep || (p[2] & 0x70) || p[3] != 2 ||
                    (mps != 8 && mps != 16 && mps != 32 && mps != 64) || (storage_endpoints & bit) || ((p[2] & 0x80) && (endpoints & (1u<<ep)))) return -USB_EPROTO;
                storage_endpoints |= bit;
                if (p[2] & 0x80) {
                    if (storage->in) return -USB_EPROTO;
                    storage->in = (uint8_t)ep; storage->in_packet = (uint8_t)mps;
                } else {
                    if (storage->out) return -USB_EPROTO;
                    storage->out = (uint8_t)ep; storage->out_packet = (uint8_t)mps;
                }
            }
            if (hid && (p[2] & 0x80) && (p[3] & 3) == 3) {
                unsigned mps = le16(p + 4);
                if (!(p[2] & 15) || (p[2] & 0x70) || !p[6] || mps > (d->low_speed ? 8u : 64u) || mps < (hid->protocol == 1 ? 8u : 3u) || hid->endpoint) return -USB_EPROTO;
                unsigned bit = 1u << (p[2] & 15);
                if ((endpoints & bit) || (storage_endpoints & (bit<<16))) return -USB_EPROTO;
                endpoints |= bit;
                hid->endpoint = p[2] & 15; hid->packet = (uint8_t)mps;
                hid->interval = p[6]; if (d->low_speed && hid->interval < 10) hid->interval = 10;
            }
        }
        at += size;
    }
    if (d->storage.bound && (!d->storage.in || !d->storage.out)) return -USB_EPROTO;
    if (interfaces != b[4]) return -USB_EPROTO;
    for (unsigned i = 0; i < count; i++) if (!d->hid[i].endpoint) return -USB_EPROTO;
    return 0;
}
int usb_enumerate(struct usb_host *h, unsigned root, unsigned parent, unsigned port, bool low)
{
    if (!h->ready || h->quarantined) return -USB_EIO;
    if (h->device_count == USB_DEVICE_MAX || h->next_address > 127) {
        klog("[usb] quarantine port=%u reason=device_capacity", port); return -ENOSPC;
    }
    unsigned slot = h->device_count++;
    struct usb_device *d = &h->devices[slot]; memset(d, 0, sizeof(*d));
    d->host = h; d->ep0 = 8; d->low_speed = low; d->root_port = (uint8_t)root;
    d->parent = (uint8_t)parent; d->port = (uint8_t)port; d->present = true;
    d->generation = gen_alloc(); d->reason = "enumerating";
    uint8_t b[USB_CONFIG_MAX];
    int err = -USB_EIO;
    for (unsigned series = 0; series < 2; series++) {
        if (series) {
            if (!h->ready || h->quarantined) goto failed;
            err = parent ? usb_hub_reset(&h->devices[parent - 1], port) : h->ops->port_reset(h, port);
            if (err) goto failed;
        }
        for (unsigned operation = 0; operation < 3; operation++) {
            if (operation) usb_sleep(h, 100);
            if (!h->ready || h->quarantined ||
                !(h->ops->port_status(h, root) & USB_PORT_CONNECTED)) { err = -USB_EIO; goto failed; }
            memset(b, 0, 8);
            uint64_t descriptor_tick = h->io.now(h->io.cookie);
            unsigned attempt = series * 3 + operation + 1;
            klog("[usb] addr0 get_descriptor controller=%s port=%u parent=%u attempt=%u series=%u operation=%u tick=%llu phase=start length=8",
                 h->owner, port + 1, parent, attempt, series + 1, operation + 1, descriptor_tick);
            err = usb_control(d, 0x80, 6, 0x100, 0, b, 8);
            klog("[usb] addr0 get_descriptor controller=%s port=%u parent=%u attempt=%u tick=%llu result=%d elapsed_ms=%llu",
                 h->owner, port + 1, parent, attempt, h->io.now(h->io.cookie), err,
                 h->io.now(h->io.cookie) - descriptor_tick);
            if (err == 8 && b[0] == 18 && b[1] == 1 &&
                (b[7] == 8 || b[7] == 16 || b[7] == 32 || b[7] == 64) && (!low || b[7] == 8)) goto descriptor_ready;
        }
    }
    goto failed;
descriptor_ready:
    d->ep0 = b[7];
    unsigned addr = h->next_address++;
    err = usb_control(d, 0, 5, (uint16_t)addr, 0, 0, 0); if (err) goto failed;
    d->address = (uint8_t)addr; usb_sleep(h, 2);
    err = usb_control(d, 0x80, 6, 0x100, 0, b, 18);
    if (err != 18 || b[0] != 18 || b[1] != 1 || b[7] != d->ep0 || !b[17]) goto failed;
    d->class_code = b[4]; d->vendor = (uint16_t)le16(b + 8); d->product = (uint16_t)le16(b + 10);
    klog("[usb] device addr=%u vid:pid=%04x:%04x class=%02x speed=%s", d->address, d->vendor, d->product, d->class_code, low ? "low" : "full");
    err = usb_control(d, 0x80, 6, 0x200, 0, b, 9);
    if (err != 9 || b[0] != 9 || b[1] != 2) goto failed;
    unsigned len = le16(b + 2); if (len < 9 || len > sizeof(b)) goto failed;
    err = usb_control(d, 0x80, 6, 0x200, 0, b, len); if (err != (int)len) goto failed;
    err = usb_parse_config(d, b, len); if (err) goto failed;
    err = usb_control(d, 0, 9, d->config, 0, 0, 0); if (err) goto failed;
    d->configured = true; d->reason = "unsupported_class";
    if (d->class_code == 9) {
        if (parent || low) goto failed;
        err = usb_hub_attach(d); if (err) goto failed;
        d->reason = "hub_ready";
    }
    for (unsigned i = 0; i < USB_HID_MAX; i++) if (d->hid[i].endpoint) {
        unsigned report_len = d->hid[i].report_length;
        err = usb_hid_attach(d, &d->hid[i], report_len); if (err) goto failed;
        d->reason = "hid_ready";
    }
    if (d->storage.bound) {
        usb_host_drop(h);
        err = usb_storage_attach(d);
        usb_storage_service(h); /* publication uses the same bind deadline now */
        usb_host_take(h); /* restore the enumeration caller's reservation */
        klog("[usb-storage] attach addr=%u result=%d", d->address, err);
    }
    return (int)slot + 1;
failed:
    usb_device_quarantine(d, err < 0 ? "enumeration_transfer" : "descriptor_or_topology");
    return (int)slot + 1; /* retain after the bounded enumeration budget */
}
static bool root_debounce(struct usb_host *h, unsigned port)
{
    unsigned stable = 0;
    h->ops->port_ack_changes(h, port);
    for (unsigned elapsed = 0; elapsed < 2000 && h->ready; elapsed += 25) {
        usb_sleep(h, 25);
        unsigned s = h->ops->port_status(h, port);
        if (!(s & USB_PORT_CONNECTED) || (s & USB_PORT_OVERCURRENT)) return false;
        if (s & USB_PORT_CHANGED) { stable = 0; h->ops->port_ack_changes(h, port); }
        else stable += 25;
        if (stable >= 100) return true;
    }
    return false;
}
void usb_step(struct usb_host *h)
{
    if (!h) return;
    fs_lock_take(&h->transfer_lock);
    usb_hid_service(h);
    if (h->busy) { fs_lock_drop(&h->transfer_lock); return; }
    if (!h->ready || h->quarantined) { fs_lock_drop(&h->transfer_lock); usb_storage_service(h); return; }
    h->busy = true;
    for (unsigned i = 0; i < 3; i++) if (h->claims[i] >= 0 && !registry_valid(h->claims[i], h->claim_gen[i])) {
        usb_host_quarantine(h, "lease_invalid"); usb_host_drop(h); return;
    }
    if (usb_expired(h, h->next_scan)) {
        uint64_t tick = h->io.now(h->io.cookie);
        bool first_scan = !h->scanned;
        h->next_scan = tick + 250;
        if (!h->scanned) {
            h->scanned = true; h->scan_window = tick;
            klog("[usb] worker controller=%s first_scan_tick=%llu interval_ms=250", h->owner, tick);
        } else if (tick - h->scan_window >= 1000) {
            uint64_t elapsed = tick - h->scan_window;
            unsigned rate = (unsigned)((uint64_t)h->scan_count * 1000000 / elapsed);
            klog_periodic("[usb] worker controller=%s tick=%llu scans=%u elapsed_ms=%llu scans_per_second=%u.%03u",
                 h->owner, tick, h->scan_count, elapsed, rate / 1000, rate % 1000);
            h->scan_window = tick; h->scan_count = 0;
        }
        h->scan_count++;
        for (unsigned p = 0; p < h->ports && h->ready; p++) {
            unsigned s = h->ops->port_status(h, p);
            unsigned previous = h->root_status[p];
            struct usb_port_log *log = &h->root_log[p];
            struct usb_device *child = h->root_child[p] ? &h->devices[h->root_child[p]-1] : 0;
            if (!h->root_blocked[p] && child && child->storage.bound &&
                child->storage.block.quarantined && !child->storage.ejected) {
                h->root_blocked[p] = true;
                h->ops->port_disable(h, p);
            }
            if (h->root_blocked[p]) {
                /* Do not interpret a quarantined device's sticky CSC as a
                 * new connection. Acknowledge observed changes without reset. */
                h->ops->port_ack_changes(h, p);
                if (s & USB_PORT_CONNECTED) {
                    if (!log->quarantine_reported) usb_port_record(h, p, "connect", 0);
                } else {
                    if (previous & USB_PORT_CONNECTED) usb_port_record(h, p, "disconnect", 0);
                    log->quarantine_reported = false;
                    if (child) { usb_detach(child, "disconnected"); h->root_child[p] = 0; }
                }
                if (log->pending) usb_port_record(h, p, log->pending, log->result);
                h->root_status[p] = s;
                continue;
            }
            if (first_scan || ((s ^ previous) & USB_PORT_CONNECTED))
                usb_port_record(h, p, s & USB_PORT_CONNECTED ? "connect" : "disconnect", 0);
            else if ((s & USB_PORT_CHANGED) && h->root_child[p])
                usb_port_record(h, p, s & USB_PORT_CONNECTED ? "connect" : "disconnect", 0);
            if ((s ^ previous) & USB_PORT_ENABLED)
                usb_port_record(h, p, s & USB_PORT_ENABLED ? "enable" : "disable", 0);
            h->root_status[p] = s;
            if (s & USB_PORT_OVERCURRENT) {
                if (h->root_child[p]) usb_device_quarantine(&h->devices[h->root_child[p]-1], "overcurrent");
                h->root_blocked[p] = true;
                h->ops->port_disable(h, p);
                usb_port_record(h, p, "overcurrent", -USB_EIO); continue;
            }
            if (!(s & USB_PORT_CONNECTED)) {
                if (h->root_child[p]) {
                    usb_detach(&h->devices[h->root_child[p]-1], "disconnected");
                    h->root_child[p] = 0;
                }
            } else if (!h->root_child[p]) {
                bool stable = root_debounce(h, p);
                s = h->ops->port_status(h, p);
                if (!stable) {
                    usb_port_record(h, p, s & USB_PORT_CONNECTED ? "debounce" : "disconnect", -USB_EAGAIN);
                    h->ops->port_ack_changes(h, p);
                    h->root_status[p] = s; continue;
                }
                int err = h->ops->port_reset(h, p);
                if (err) { usb_host_quarantine(h, "root_port_reset"); break; }
                s = h->ops->port_status(h, p);
                err = usb_enumerate(h, p, 0, p, !!(s & USB_PORT_LOW));
                if (err > 0) h->root_child[p] = (uint8_t)err;
                else { h->ops->port_disable(h, p); usb_host_quarantine(h, "device_capacity"); break; }
            } else if (!(s & USB_PORT_ENABLED) && usb_port_has_live_child(h, p)) {
                /* PEDC/CSC do not revoke a live address while CCS stays set.
                 * Recover PE without resetting the device or its toggles. */
                int err = h->ops->port_enable(h, p);
                if (err) { usb_device_quarantine(child, "root_port_enable"); continue; }
            }
            if (!h->ready) break; /* A transfer may have stopped/quarantined the HC. */
            h->ops->port_ack_changes(h, p);
            if (log->pending) usb_port_record(h, p, log->pending, log->result);
            h->root_status[p] = h->ops->port_status(h, p);
        }
        for (unsigned i = 0; i < h->device_count && h->ready; i++) if (h->devices[i].present && h->devices[i].hub_ports) usb_hub_scan(&h->devices[i]);
        usb_uhci_dump_schedule(h);
    }
    for (unsigned i = 0; i < h->device_count && h->ready; i++) {
        struct usb_device *d = &h->devices[i];
        if (!d->configured || !d->present || d->quarantined) continue;
        /* A scan first recognizes disconnect/change. A TD failure caused
         * by unplug must not permanently block that port's next generation. */
        if (d->hid_fault) { usb_device_quarantine(d,d->hid_fault); continue; }
        usb_hid_recover(d);
        if (d->hid_fault) { usb_device_quarantine(d,d->hid_fault); continue; }
        if (!d->present || d->quarantined) continue;
        usb_hid_outputs(d);
    }
    usb_hid_service(h);
    usb_host_drop(h);
    usb_storage_service(h);
}
#ifndef CIUKI_USB_HOST
static uint32_t native_read(void *arg, unsigned at, unsigned width)
{
    struct usb_host *h = arg;
    if (h->pci.prog_if == 0) {
        uint16_t port = (uint16_t)(h->base + at);
        return width == 1 ? inb(port) : width == 2 ? cpu_inw(port) : inl(port);
    }
    volatile uint8_t *m = h->mmio;
    return width == 1 ? m[at] : width == 2 ? *(volatile uint16_t *)(m + at) : *(volatile uint32_t *)(m + at);
}
static void native_write(void *arg, unsigned at, uint32_t value, unsigned width)
{
    struct usb_host *h = arg;
    if (h->pci.prog_if == 0) {
        uint16_t port = (uint16_t)(h->base + at);
        if (width == 1) outb(port, (uint8_t)value); else if (width == 2) cpu_outw(port, (uint16_t)value); else outl(port, value);
    } else {
        volatile uint8_t *m = h->mmio;
        if (width == 1) m[at] = (uint8_t)value; else if (width == 2) *(volatile uint16_t *)(m + at) = (uint16_t)value; else *(volatile uint32_t *)(m + at) = value;
    }
}
static uint32_t cfg_read(void *arg, unsigned at, unsigned width)
{
    struct usb_host *h = arg; const struct pci_func *p = &h->pci;
    uint32_t f = irq_save(); outl(0xcf8, 0x80000000u | (uint32_t)p->bus << 16 | (uint32_t)p->dev << 11 | (uint32_t)p->fn << 8 | (at & ~3u));
    uint32_t v = width == 2 ? cpu_inw(0xcfc + (at & 2)) : inl(0xcfc); irq_restore(f); return v;
}
static void cfg_write(void *arg, unsigned at, uint32_t v, unsigned width)
{
    struct usb_host *h = arg; const struct pci_func *p = &h->pci;
    uint32_t f = irq_save(); outl(0xcf8, 0x80000000u | (uint32_t)p->bus << 16 | (uint32_t)p->dev << 11 | (uint32_t)p->fn << 8 | (at & ~3u));
    if (width == 2) cpu_outw(0xcfc + (at & 2), (uint16_t)v); else outl(0xcfc, v); irq_restore(f);
}
static uint64_t native_now(void *arg) { (void)arg; uint32_t f = irq_save(); uint64_t v = g_ticks; irq_restore(f); return v; }
static void native_sleep(void *arg, unsigned ms) { (void)arg; task_sleep_ms(ms); }
static void service(void *arg)
{
    struct usb_host *h = arg;
    klog("[usb] worker service=started tick=%llu controllers=%u controller=%s", native_now(0), host_count, h->owner);
    for (;;) {
        if (!h->ready || h->quarantined) { task_sleep_ms(1000); continue; }
        usb_step(h);
        uint64_t now = native_now(0);
        uint64_t due = h->next_scan;
        /* Control/BOT sleeps retain their per-host reservation. The separate
         * HID task services completed reports at 1 ms, independently. */
        task_sleep_ms(due > now && due - now <= 250 ? (uint32_t)(due - now) : 1);
    }
}
static void input_service(void *arg)
{
    (void)arg;
    for (;;) {
        for (unsigned i = 0; i < host_count; i++) {
            fs_lock_take(&hosts[i].transfer_lock);
            usb_hid_service(&hosts[i]);
            fs_lock_drop(&hosts[i].transfer_lock);
        }
        task_sleep_ms(1);
    }
}
static void start_services(void)
{
    if (host_count) {
        struct task *workers[USB_HOST_MAX] = {0};
        bool failed = false;
        for (unsigned i = 0; i < host_count; i++) {
            workers[i] = task_create_kernel("usb-host", service, &hosts[i], P_DEVICE);
            if (!workers[i]) failed = true;
        }
        struct task *input = task_create_kernel("usb-hid", input_service, 0, P_DEVICE);
        if (!input) failed = true;
        if (failed) {
            for (unsigned i = 0; i < host_count; i++) if (hosts[i].ready)
                usb_host_quarantine(&hosts[i], "service_thread_failed");
        } else {
            for (unsigned i = 0; i < host_count; i++) task_start(workers[i]);
            task_start(input);
            klog("[usb] worker service=queued tick=%llu controllers=%u", native_now(0), host_count);
        }
    }
}
void usb_init(void)
{
    static bool initialized; if (initialized) return; initialized = true;
    if (blocked()) { klog("[usb] native activation skipped reason=firmware_first_or_safe_mode"); return; }
    /* First pass routes EHCI ports before companions enumerate. */
    for (unsigned pass = 0; pass < 2; pass++) for (unsigned i = 0; i < pci_count() && host_count < USB_HOST_MAX; i++) {
        const struct pci_func *p = pci_get(i);
        if (p->class_code != 0x0c || p->subclass != 3 || (p->prog_if != 0 && p->prog_if != 0x10 && p->prog_if != 0x20) || (p->prog_if == 0x20) != (pass == 0)) continue;
        struct usb_host *h = &hosts[host_count++];
        struct usb_io io = {h, native_read, native_write, cfg_read, cfg_write, native_now, native_sleep};
        uint32_t bar = p->bar[p->prog_if ? 0 : 4];
        unsigned bytes = p->prog_if ? 4096 : 32;
        uint32_t base = bar & (p->prog_if ? ~15u : ~3u);
        if (!base || bar == UINT32_MAX || (base & (bytes - 1)) ||
            (p->prog_if ? (bar & 7) || base > UINT32_MAX - bytes : !(bar & 1) || base > 65536 - bytes)) {
            h->pci = *p; h->reason = "invalid_bar"; continue;
        }
        uint32_t phys = p->prog_if == 0x20 ? 0 : pmm_alloc_contig(USB_DMA_BYTES / PAGE_SIZE);
        /* The window remains pinned with the controller, including failure. */
        void *mapping = p->prog_if ? vmm_map_mmio(p->bar[0] & ~15u, 4096, true) : 0;
        if ((p->prog_if && !mapping) || (p->prog_if != 0x20 && !phys)) {
            if (phys) for (unsigned at=0;at<USB_DMA_BYTES;at+=PAGE_SIZE) pmm_free(phys+at);
            h->pci = *p; h->reason = "allocation_failed"; continue;
        }
        /* usb_attach preserves this window while initializing the host. */
        h->mmio = mapping;
        int err = usb_attach(h, p, &io, phys ? P2V(phys) : 0, phys);
        if (err && !h->touched && !h->quarantined && phys) {
            for (unsigned at=0;at<USB_DMA_BYTES;at+=PAGE_SIZE) pmm_free(phys+at);
            h->dma = 0;
        }
    }
    start_services();
}
#endif
