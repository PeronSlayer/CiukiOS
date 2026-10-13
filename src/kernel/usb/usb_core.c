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
    for (unsigned i = 0; i < host_count; i++) if (hosts[i].ready && !hosts[i].quarantined) return true;
    return false;
}
unsigned usb_count(void) { return host_count; }
const struct usb_host *usb_get(unsigned i) { return i < host_count ? &hosts[i] : 0; }
uint32_t usb_read(struct usb_host *h, unsigned at, unsigned width)
{ return h->io.read(h->io.cookie, at, width); }
void usb_write(struct usb_host *h, unsigned at, uint32_t v, unsigned width)
{ h->io.write(h->io.cookie, at, v, width); }
bool usb_expired(struct usb_host *h, uint64_t deadline)
{ return (int64_t)(h->io.now(h->io.cookie) - deadline) >= 0; }
int usb_wait_register(struct usb_host *h, unsigned at, unsigned width, uint32_t mask, uint32_t want, unsigned ms)
{
    uint64_t end = h->io.now(h->io.cookie) + ms;
    for (unsigned i = 0; i <= ms; i++) {
        uint32_t v = usb_read(h, at, width);
        if (v == (width == 2 ? 0xffffu : UINT32_MAX)) return -USB_EIO;
        if ((v & mask) == want) return 0;
        if (usb_expired(h, end)) break;
        h->io.sleep(h->io.cookie, 1);
    }
    return -USB_ETIMEDOUT;
}
int usb_retire_frames(struct usb_host *h, unsigned off, unsigned width, unsigned mask)
{
    unsigned start = usb_read(h, off, width) & mask;
    for (unsigned i = 0; i < 20; i++) {
        h->io.sleep(h->io.cookie, 1);
        unsigned elapsed = (usb_read(h, off, width) - start) & mask;
        if (elapsed >= 2 && elapsed < mask / 2) return 0;
    }
    usb_host_quarantine(h, "frame_clock_stalled"); return -USB_ETIMEDOUT;
}
void usb_detach(struct usb_device *d, const char *reason)
{
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
    klog("[usb] quarantine device addr=%u reason=%s", d->address, reason);
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
    unsigned count = 0, report_len = 0, endpoints = 0, interfaces = 0;
    uint8_t seen_interface[32] = {0};
    for (unsigned at = 9; at < len; ) {
        if (len - at < 2 || b[at] < 2 || b[at] > len - at) return -USB_EPROTO;
        unsigned size = b[at]; const uint8_t *p = b + at;
        if (p[1] == 4) {
            if (size < 9) return -USB_EPROTO;
            hid = 0; report_len = 0;
            if (!p[3]) {
                unsigned bit = 1u << (p[2] & 7);
                if (seen_interface[p[2] / 8] & bit) return -USB_EPROTO;
                seen_interface[p[2] / 8] |= (uint8_t)bit; interfaces++;
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
            if (hid && (p[2] & 0x80) && (p[3] & 3) == 3) {
                unsigned mps = le16(p + 4);
                if (!(p[2] & 15) || (p[2] & 0x70) || !p[6] || mps > (d->low_speed ? 8u : 64u) || mps < (hid->protocol == 1 ? 8u : 3u) || hid->endpoint) return -USB_EPROTO;
                unsigned bit = 1u << (p[2] & 15);
                if (endpoints & bit) return -USB_EPROTO;
                endpoints |= bit;
                hid->endpoint = p[2] & 15; hid->packet = (uint8_t)mps;
                hid->interval = p[6]; if (d->low_speed && hid->interval < 10) hid->interval = 10;
            }
        }
        at += size;
    }
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
    int err = usb_control(d, 0x80, 6, 0x100, 0, b, 8);
    if (err != 8 || b[0] != 18 || b[1] != 1 || (b[7] != 8 && b[7] != 16 && b[7] != 32 && b[7] != 64) || (low && b[7] != 8)) goto failed;
    d->ep0 = b[7];
    unsigned addr = h->next_address++;
    err = usb_control(d, 0, 5, (uint16_t)addr, 0, 0, 0); if (err) goto failed;
    d->address = (uint8_t)addr; h->io.sleep(h->io.cookie, 2);
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
    return (int)slot + 1;
failed:
    usb_device_quarantine(d, err < 0 ? "enumeration_transfer" : "descriptor_or_topology");
    return (int)slot + 1; /* retain failed record at the port; never retry */
}
void usb_step(struct usb_host *h)
{
    if (!h || !h->ready || h->quarantined || h->busy) return;
    h->busy = true;
    for (unsigned i = 0; i < 3; i++) if (h->claims[i] >= 0 && !registry_valid(h->claims[i], h->claim_gen[i])) {
        usb_host_quarantine(h, "lease_invalid"); h->busy = false; return;
    }
    if (usb_expired(h, h->next_scan)) {
        h->next_scan = h->io.now(h->io.cookie) + 250;
        for (unsigned p = 0; p < h->ports && h->ready; p++) {
            if (h->root_blocked[p]) continue;
            unsigned s = h->ops->port_status(h, p);
            if (s & USB_PORT_OVERCURRENT) {
                if (h->root_child[p]) usb_device_quarantine(&h->devices[h->root_child[p]-1], "overcurrent");
                h->root_blocked[p] = true;
                h->ops->port_disable(h, p);
                klog("[usb] quarantine port=%u reason=overcurrent", p); continue;
            }
            if ((s & USB_PORT_CHANGED) && h->root_child[p]) {
                usb_detach(&h->devices[h->root_child[p]-1], "root_port_changed");
                h->root_child[p] = 0;
            }
            if (!(s & USB_PORT_CONNECTED)) {
                if (h->root_child[p]) {
                    usb_detach(&h->devices[h->root_child[p]-1], "disconnected");
                    h->root_child[p] = 0;
                }
            } else if (!h->root_child[p]) {
                h->io.sleep(h->io.cookie, 100);
                if (!(h->ops->port_status(h, p) & USB_PORT_CONNECTED)) continue;
                int err = h->ops->port_reset(h, p);
                if (err) { usb_host_quarantine(h, "root_port_reset"); break; }
                s = h->ops->port_status(h, p);
                err = usb_enumerate(h, p, 0, p, !!(s & USB_PORT_LOW));
                if (err > 0) h->root_child[p] = (uint8_t)err;
                else { h->ops->port_disable(h, p); usb_host_quarantine(h, "device_capacity"); break; }
            }
        }
        for (unsigned i = 0; i < h->device_count && h->ready; i++) if (h->devices[i].present && h->devices[i].hub_ports) usb_hub_scan(&h->devices[i]);
    }
    for (unsigned i = 0; i < h->device_count && h->ready; i++) {
        struct usb_device *d = &h->devices[i];
        if (!d->configured || !d->present || d->quarantined) continue;
        for (unsigned j = 0; j < USB_HID_MAX && !d->quarantined && h->ready; j++) usb_hid_poll(d, &d->hid[j]);
        h->io.sleep(h->io.cookie, 1);
    }
    h->busy = false;
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
    (void)arg;
    for (;;) { for (unsigned i = 0; i < host_count; i++) usb_step(&hosts[i]); task_sleep_ms(1); }
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
        uint32_t phys = p->prog_if == 0x20 ? 0 : pmm_alloc_contig(2);
        /* The window remains pinned with the controller, including failure. */
        void *mapping = p->prog_if ? vmm_map_mmio(p->bar[0] & ~15u, 4096, true) : 0;
        if ((p->prog_if && !mapping) || (p->prog_if != 0x20 && !phys)) {
            if (phys) { pmm_free(phys); pmm_free(phys + PAGE_SIZE); }
            h->pci = *p; h->reason = "allocation_failed"; continue;
        }
        /* usb_attach preserves this window while initializing the host. */
        h->mmio = mapping;
        int err = usb_attach(h, p, &io, phys ? P2V(phys) : 0, phys);
        if (err && !h->touched && !h->quarantined && phys) { pmm_free(phys); pmm_free(phys + PAGE_SIZE); h->dma = 0; }
    }
    if (host_count && !task_create_kernel("usb-host", service, 0, P_DEVICE))
        for (unsigned i = 0; i < host_count; i++) if (hosts[i].ready) usb_host_quarantine(&hosts[i], "service_thread_failed");
}
#endif
