#include <ciuki/system_settings.h>
/* USB enumeration and serialized service. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/usb.h>
#include <ciuki/power_sleep.h>
#include <ciuki/input.h>
#include <ciuki/init.h>
#include <ciuki/smi.h>
/* Standalone storage-only host models omit HID support. */
#pragma weak usb_identity_serial
#ifndef CIUKI_USB_HOST
#include <ciuki/cpu.h>
#include <ciuki/mm.h>
#include <ciuki/task.h>
#endif
static struct usb_host hosts[USB_HOST_MAX];
static unsigned host_count;
static struct usb_host *irq_hosts[USB_HOST_MAX];
static struct kwait input_changed;
static bool input_pending;
/* BOOT.CFG options are already bounded by the loader. Exact token matching:
 * an unknown/partial value keeps the normal 250 ms scan policy. */
static bool periodic_scan(void) {
    const char *s=g_boot.options; unsigned i=0;
    while (i<sizeof(g_boot.options) && s[i]) {
        while (i<sizeof(g_boot.options) && ((unsigned char)s[i]<=32 && s[i])) i++;
        unsigned start=i;
        while (i<sizeof(g_boot.options) && s[i] && (unsigned char)s[i]>32) i++;
        if (i-start==9 && !memcmp(s+start,"usbscan=0",9)) return false;
    }
    return true;
}
bool usb_input_ready(void)
{
    struct input_devices devices;
    input_devices_snapshot(&devices);
    return devices.usb != 0;
}
unsigned usb_count(void) { return host_count; }
const struct usb_host *usb_get(unsigned i) { return i < host_count ? &hosts[i] : 0; }
uint32_t usb_read(struct usb_host *h, unsigned at, unsigned width)
{
    uint32_t v = h->io.read(h->io.cookie, at, width);
    if (at == (h->pci.prog_if == 0x20 ? h->ehci_caplen + 4u : h->pci.prog_if ? 0x0cu : 2u)) v |= h->irq_status;
    return v;
}
void usb_write(struct usb_host *h, unsigned at, uint32_t v, unsigned width)
{ h->io.write(h->io.cookie, at, v, width); }
static void quarantine_irq(struct usb_host *h)
{
    if (h->irq_attached && h->claims[1] >= 0)
        registry_quarantine(h->claims[1], h->claim_gen[1]);
}
static bool uhci_isolate(struct usb_host *h)
{
    /* One bounded, non-sleeping attempt. HSE/HCPE bypass USBINTR, and a
     * claimed storm can otherwise starve the worker indefinitely. */
    h->io.config_write(h->io.cookie, 0xc0, 0x8f00, 2);
    usb_write(h, 4, 0, 2); usb_write(h, 0, 0, 2);
    usb_write(h, 2, 0x1f, 2);
    h->io.config_write(h->io.cookie, 4, (h->pci.command | 1u) & ~4u, 2);
    unsigned legacy = h->io.config_read(h->io.cookie, 0xc0, 2);
    unsigned intr = h->io.read(h->io.cookie, 4, 2);
    unsigned run = h->io.read(h->io.cookie, 0, 2);
    unsigned status = h->io.read(h->io.cookie, 2, 2); /* never irq_status */
    unsigned command = h->io.config_read(h->io.cookie, 4, 2);
    return !(legacy & 0x20bf) && !(intr & 15) && !(run & 1) &&
           (status & 0x3f) == 0x20 && !(command & 4);
}
static uint32_t usb_irq_save(void)
{
#ifndef CIUKI_USB_HOST
    return irq_save();
#else
    /* Portable controller models dispatch events serially. The native-core
     * replay exercises the real IRQ exclusion with modeled EFLAGS. */
    return 0;
#endif
}
static void usb_irq_restore(uint32_t f)
{
#ifndef CIUKI_USB_HOST
    irq_restore(f);
#else
    (void)f;
#endif
}
static void ohci_capture_done(struct usb_host *h)
{
    uint32_t f = usb_irq_save();
    /* Re-read under exclusion: the IRQ may have consumed an earlier WDH
     * between a polling caller's status read and this critical section. */
    unsigned status = h->io.read(h->io.cookie, 0x0c, 4);
    if (status != UINT32_MAX && (status & 2)) {
        volatile uint32_t *hcca = (volatile uint32_t *)(h->dma + 0x84);
        usb_barrier();
        uint32_t head = *hcca & ~15u;
        if (head && h->ohci_done_count == USB_OHCI_DONE_HEADS) {
            /* Never release the HC's publication interlock without saving
             * its head. Retain HCCA/WDH and contain a corrupt producer. */
            h->irq_status |= 0x10;
            usb_write(h, 0x14, UINT32_MAX, 4);
            quarantine_irq(h);
        } else {
            if (head) {
                unsigned at = (h->ohci_done_read + h->ohci_done_count) % USB_OHCI_DONE_HEADS;
                h->ohci_done_heads[at] = head;
                h->ohci_done_count++;
            }
            *hcca = 0;
            usb_barrier();
            usb_write(h, 0x0c, 2, 4); /* sole WDH acknowledgement */
        }
    }
    usb_irq_restore(f);
}
void usb_ohci_service_done(struct usb_host *h)
{
    if (h->pci.prog_if != 0x10) return;
    usb_irq_completion_check(h);
    /* Give WDH a chance to reach the native IRQ before a task acknowledges
     * it. The ISR calls capture directly and never waits. This also prevents
     * polling from destroying the evidence used to qualify a lost IRQ. */
    if (h->irq_probe_pending && !h->irq_fallback &&
        !usb_expired(h, h->irq_probe_at + USB_IRQ_GRACE_MS)) return;
    ohci_capture_done(h);
    /* The transfer mutex serializes task consumers and descriptor reuse.
     * IRQ only appends heads; each list remains pinned until consumed here.
     * Results still come from each pipe's retired TD/ED, independently of
     * completion order or whether its interrupt was delivered. */
    for (unsigned batch = 0; batch < USB_OHCI_DONE_HEADS; batch++) {
        uint32_t f = usb_irq_save();
        if (!h->ohci_done_count) { usb_irq_restore(f); break; }
        uint32_t head = h->ohci_done_heads[h->ohci_done_read];
        h->ohci_done_read = (h->ohci_done_read + 1) % USB_OHCI_DONE_HEADS;
        h->ohci_done_count--;
        usb_irq_restore(f);
        unsigned count = 0;
        while (head) {
            unsigned at = head - h->dma_phys;
            if (++count > USB_OHCI_DONE_HEADS || (head & 15) ||
                (at != USB_TD && (at < USB_HID_BASE + 16 ||
                 at >= USB_HID_BASE + USB_DEVICE_MAX * USB_HID_MAX * USB_HID_STRIDE ||
                 (at - USB_HID_BASE - 16) % USB_HID_STRIDE))) {
                usb_host_quarantine(h, "ohci_done_pointer"); return;
            }
            usb_barrier();
            head = *(volatile uint32_t *)(h->dma + at + 8);
            if (at == USB_TD) h->ohci_packet_done = true;
            h->ohci_done_tds++;
        }
        h->ohci_done_batches++;
    }
}
bool usb_host_irq(struct usb_host *h)
{
    /* A retained fatal cause is worker state, not proof that the hardware
     * stopped asserting. Even quarantined hosts keep servicing their source
     * until isolation permits callback removal. */
    if (!h || !h->irq_attached || h->suspended) return false;
    if (h->pci.prog_if == 0x20) {
        unsigned off = h->ehci_caplen, status = h->io.read(h->io.cookie, off + 4, 4);
        unsigned causes = status & h->io.read(h->io.cookie, off + 8, 4) & 0x17;
        if (!causes) return false;
        if (status == UINT32_MAX || (causes & 0x10)) {
            h->irq_status |= 0x10;
            usb_write(h, off + 8, 0, 4);
            if (h->io.read(h->io.cookie, off + 8, 4) & 0x3f) quarantine_irq(h);
        }
        if (status != UINT32_MAX) usb_write(h, off + 4, causes, 4);
        if (causes & 3) { h->completion_irq = true; h->completion_interrupts++; h->irq_probe_pending = false; }
        h->interrupts++; h->irq_pending = input_pending = true;
        kwait_wake_irq(&h->changed, !(causes & 0x10));
        kwait_wake_irq(&input_changed, false);
        return true; /* IAA remains polled by the sole unlink owner */
    }
    bool uhci = h->pci.prog_if == 0;
    unsigned off = uhci ? 2 : 0x0c, width = uhci ? 2 : 4;
    unsigned status = h->io.read(h->io.cookie, off, width);
    unsigned causes = status & (uhci ? 0x1fu : 0x52u); /* OHCI WDH/UE/RHSC, never SOF */
    bool gone = status == (uhci ? 0xffffu : UINT32_MAX);
    if (!uhci && !gone) {
        unsigned enabled = h->io.read(h->io.cookie, 0x10, 4);
        causes &= (enabled & 0x80000000u) ? enabled : 0;
    }
    if (!causes) return false;
    bool fatal = gone || (causes & (uhci ? 0x18u : 0x10u));
    if (fatal) {
        h->irq_status |= gone ? 0x10 : causes & (uhci ? 0x18u : 0x10u);
        if (uhci) {
            if (!uhci_isolate(h)) quarantine_irq(h); /* before dispatcher EOI */
        } else usb_write(h, 0x14, UINT32_MAX, 4);
    }
    if (!uhci && !gone) {
        if (causes & 0x40) {
            h->ohci_root_pending = true;
            usb_write(h, 0x14, 0x40, 4);
            if (h->io.read(h->io.cookie, 0x10, 4) & 0x40) quarantine_irq(h);
        }
        if (causes & 2) ohci_capture_done(h);
        if (causes & ~2u) usb_write(h, off, causes & ~2u, width);
    } else if (!gone && !fatal) {
        usb_write(h, off, causes, width);
    }
    h->interrupts++; h->irq_pending = input_pending = true;
    if (!fatal && (causes & (uhci ? 3u : 2u))) {
        h->completion_irq = true; h->completion_interrupts++;
        h->irq_probe_pending = false;
    }
    if (!fatal && !gone && (causes & (uhci ? 1u : 2u))) kwait_wake_irq(&h->changed, true);
    else kwait_wake_irq(&h->changed, false);
    kwait_wake_irq(&input_changed, false);
    return true;
}
static bool usb_irq(unsigned irq, const char *owner)
{
    for (unsigned i = 0; i < USB_HOST_MAX; i++) {
        struct usb_host *h = irq_hosts[i];
        if (h && h->pci.irq_line == irq && !strncmp(h->owner, owner, sizeof(h->owner))) return usb_host_irq(h);
    }
    return false;
}
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
    unsigned raw = usb_read(h, uhci ? 0x10 + 2 * port : h->pci.prog_if == 0x20 ? h->ehci_caplen + 0x44 + 4 * port : 0x54 + 4 * port, uhci ? 2 : 4);
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
    unsigned raw = usb_read(h, uhci ? 0x10 + 2 * port : h->pci.prog_if == 0x20 ? h->ehci_caplen + 0x44 + 4 * port : 0x54 + 4 * port, uhci ? 2 : 4);
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
static bool usb_leases_valid(struct usb_host *h)
{
    /* IRQ is optional; register and DMA authority are not. */
    for (unsigned i = 0; i < 3; i += 2)
        if (h->claims[i] >= 0 && !registry_valid(h->claims[i], h->claim_gen[i])) {
            usb_host_quarantine(h, "lease_invalid"); return false;
        }
    if (!h->irq_attached || h->claims[1] < 0 || registry_valid(h->claims[1], h->claim_gen[1])) return true;
    bool uhci = h->pci.prog_if == 0;
    /* Reuse existing local interrupt masks; keep the frame engine, schedule,
     * PCI command, routing and PIC intact. A mask readback failure is fatal. */
    if (uhci) {
        usb_write(h, 4, 0, 2);
        h->io.config_write(h->io.cookie, 0xc0, 0x8f00, 2);
        if ((h->io.read(h->io.cookie, 4, 2) & 15) ||
            (h->io.config_read(h->io.cookie, 0xc0, 2) & 0x20bf)) {
            usb_host_quarantine(h, "irq_mask_failed"); return false;
        }
    } else if (h->pci.prog_if == 0x20) {
        usb_write(h, h->ehci_caplen + 8, 0, 4);
        if (h->io.read(h->io.cookie, h->ehci_caplen + 8, 4) & 0x3f) {
            usb_host_quarantine(h, "irq_mask_failed"); return false;
        }
    } else {
        usb_write(h, 0x14, UINT32_MAX, 4);
        if (h->io.read(h->io.cookie, 0x10, 4) & 0x8000007f) {
            usb_host_quarantine(h, "irq_mask_failed"); return false;
        }
    }
    /* Registry revocation may already have removed the callback. */
    (void)irq_chain_remove(h->pci.irq_line, usb_irq, h->owner);
    h->irq_attached = h->irq_pending = false;
    for (unsigned i = 0; i < USB_HOST_MAX; i++) if (irq_hosts[i] == h) irq_hosts[i] = 0;
    h->reason = "ready_polling";
    klog("[usb] irq_lost controller=%s mode=poll", h->owner);
    return true;
}
void usb_hid_service(struct usb_host *h)
{
    if (!h->ready || h->quarantined) return;
    if (h->irq_status) { usb_host_quarantine(h, "interrupt_host_error"); return; }
    if (!usb_leases_valid(h)) return;
    usb_irq_completion_check(h);
    usb_ohci_service_done(h);
    if (!h->ready) return;
    for (unsigned i = 0; i < h->device_count && h->ready; i++) {
        struct usb_device *d = &h->devices[i];
        if (!d->present || !d->configured || d->quarantined || d->hid_fault) continue;
        for (unsigned j = 0; j < USB_HID_MAX && !d->hid_fault && h->ready; j++)
            usb_hid_poll(d, &d->hid[j]);
        bool general = d->tt_pending || d->hid_fault;
        for (unsigned j = 0; j < USB_HID_MAX; j++) general |= d->hid[j].recovery_pending;
        if (general) { h->irq_pending = true; kwait_wake_all(&h->changed); }
    }
}
void usb_irq_completion_check(struct usb_host *h)
{
    uint32_t flags = usb_irq_save();
    if (!h->irq_attached || h->irq_fallback) {
        h->irq_probe_pending = false; usb_irq_restore(flags); return;
    }
    /* Called from the topology deadline or a synchronous transfer after the
     * HC had a chance to signal IOC. A pending device-local completion,
     * still unacknowledged by our ISR, is evidence; an idle NAK is not. */
    unsigned off = h->pci.prog_if == 0x20 ? h->ehci_caplen + 4u : h->pci.prog_if ? 0x0cu : 2u;
    unsigned status = h->io.read(h->io.cookie, off, h->pci.prog_if ? 4 : 2);
    if (status == UINT32_MAX || !(status & (h->pci.prog_if == 0x10 ? 2u : 3u))) {
        h->irq_probe_pending = false; usb_irq_restore(flags); return;
    }
    if (!h->irq_probe_pending || h->irq_probe_count != h->completion_interrupts) {
        h->irq_probe_pending = true;
        h->irq_probe_at = h->io.now(h->io.cookie);
        h->irq_probe_count = h->completion_interrupts;
        input_pending = true;
        usb_irq_restore(flags); kwait_wake_all(&input_changed);
        return;
    }
    if (!usb_expired(h, h->irq_probe_at + USB_IRQ_GRACE_MS)) { usb_irq_restore(flags); return; }
    h->irq_fallback = true;
    h->irq_probe_pending = false;
    usb_irq_restore(flags);
    klog("[usb] controller=%s mode=hid_fallback interval_ms=%u reason=completion_without_irq",
         h->owner, USB_HID_FALLBACK_MS);
}
uint64_t usb_hid_deadline(struct usb_host *h)
{
    if (!h->ready || h->quarantined) return UINT64_MAX;
    uint64_t now = h->io.now(h->io.cookie), due = UINT64_MAX;
    if (h->irq_probe_pending) due = h->irq_probe_at + USB_IRQ_GRACE_MS;
    for (unsigned i = 0; i < h->device_count; i++) {
        struct usb_device *d = &h->devices[i];
        if (!d->present || !d->configured || d->quarantined || d->hid_fault || d->tt_pending) continue;
        for (unsigned j = 0; j < USB_HID_MAX; j++) {
            struct usb_hid *p = &d->hid[j];
            if (!p->active || p->recovery_pending) continue;
            uint64_t next = UINT64_MAX;
            if (!p->pipe_state) next = p->next_poll > now ? p->next_poll : now;
            else if (p->pipe_state == 2) next = p->retire_begin + 2 > now ? p->retire_begin + 2 : now + 1;
            else if (!h->irq_attached || h->irq_fallback) next = now + USB_HID_FALLBACK_MS;
            if (p->repeat_key && p->repeat_at < next) next = p->repeat_at;
            if (next < due) due = next;
        }
    }
    return due;
}
void usb_host_take(struct usb_host *h)
{
    for (;;) {
        fs_lock_take(&h->transfer_lock);
        if (!h->busy && !h->suspended) { h->busy = true; return; }
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
            if (!h->busy && !h->suspended) { h->busy = true; return true; }
            usb_hid_service(h);
            fs_lock_drop(&h->transfer_lock);
        }
        h->io.sleep(h->io.cookie, 1);
    }
    return false;
}
void usb_poll_pause(struct usb_host *h, unsigned ms)
{
    /* A reservation protects the general TD/BOT sequence, not the mutex.
     * This bounded retry backoff may finish early on notifications, including
     * with a broken clock. It must not be used for mandatory USB delays. */
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
void usb_sleep(struct usb_host *h, unsigned ms)
{
    /* Reset, recovery and debounce consume elapsed time, never wake counts.
     * Interrupt slots still progress with the short DMA mutex released. */
    uint64_t end = h->io.now(h->io.cookie) + ms;
    while (!usb_expired(h, end)) usb_poll_pause(h, 1);
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
            /* The hub cannot isolate its child. Isolate the hub's root
             * port, retaining devices attached to other root ports. */
            usb_device_quarantine(hub, "hub_disable_failed");
    }
    /* A device fault cannot authorize re-enumerating that port while the
     * device stays connected. f1-96b: an observed disconnect (CCS=0)
     * releases the port; the next connection is a new device generation. */
    klog("[usb] quarantine device addr=%u reason=%s stage=%s", d->address, reason,
         d->storage.trace.stage ? d->storage.trace.stage : "enumeration");
}
void usb_host_quarantine(struct usb_host *h, const char *reason)
{
    if (h->quarantined) return;
    h->quarantined = true; h->ready = false; h->reason = reason;
    bool isolated = !h->irq_attached;
    if (h->owned) {
        if (h->pci.prog_if == 0) {
            isolated = uhci_isolate(h);
        } else if (h->pci.prog_if == 0x20) {
            isolated = usb_ehci_stop(h);
        } else if (h->pci.prog_if == 0x10) {
            usb_write(h, 0x14, UINT32_MAX, 4); usb_write(h, 4, 0, 4);
            ohci_capture_done(h);
            usb_write(h, 0x0c, 0x7d, 4); /* WDH belongs to capture */
        }
        /* Do not restore firmware DMA on an uncertain failure. */
        h->io.config_write(h->io.cookie, 4, (h->pci.command | (h->pci.prog_if ? 2 : 1)) & ~4u, 2);
        if (h->irq_attached) {
            unsigned command = h->io.config_read(h->io.cookie, 4, 2);
            if (h->pci.prog_if == 0x10) {
                unsigned intr = h->io.read(h->io.cookie, 0x10, 4);
                unsigned control = h->io.read(h->io.cookie, 4, 4);
                unsigned status = h->io.read(h->io.cookie, 0x0c, 4);
                isolated = !(command & 4) && !(intr & 0x8000007f) &&
                           !(control & 0xc0) && !(status & 0x7f);
            }
        }
    }
    if (h->irq_attached && isolated && !irq_chain_remove(h->pci.irq_line, usb_irq, h->owner)) {
        h->irq_attached = false;
        for (unsigned i = 0; i < USB_HOST_MAX; i++) if (irq_hosts[i] == h) irq_hosts[i] = 0;
    }
    for (unsigned i = 0; i < h->device_count; i++) usb_detach(&h->devices[i], "controller_quarantined");
    /* Failed isolation retains DMA/resources in quarantine. IRQ revocation
     * detaches this member; a physically stuck line still has its own gate. */
    for (unsigned i = 0; i < 3; i++) if (h->claims[i] >= 0) registry_quarantine(h->claims[i], h->claim_gen[i]);
    klog("[usb] quarantine controller=%s reason=%s irq_isolated=%u", h->owner, reason, isolated);
}
/* fx-21b: a refused quiesce leaves the running hosts able to enumerate. */
static void quiesce_abandon(void)
{
    for (unsigned i = 0; i < host_count; i++) if (!hosts[i].quarantined) hosts[i].quiescing = false;
}
int usb_power_quiesce(void)
{
    /* Retain arenas/claims even after a proved stop. Each general transfer
     * reservation excludes BOT/control and HID while local sources stop.
     * fx-21b: an enumeration message may wait 5 s; ask every such wait to
     * end at once so the 2 s reservation bound below still holds. */
    for (unsigned i = 0; i < host_count; i++) hosts[i].quiescing = true;
    for (unsigned i = 0; i < host_count; i++) {
        struct usb_host *h = &hosts[i];
        if (!h->owned) continue;
        if (h->quarantined && h->reason && !strncmp(h->reason,"shutdown_stopped",17)) continue;
        if (!usb_host_take_until(h, h->io.now(h->io.cookie) + 2000)) { quiesce_abandon(); return -16; }
        h->ready = false;
        bool stopped = false;
        if (!h->quarantined && h->pci.prog_if == 0) stopped = uhci_isolate(h);
        else if (!h->quarantined && h->pci.prog_if == 0x20) stopped = usb_ehci_stop(h);
        else if (!h->quarantined && h->pci.prog_if == 0x10) {
            usb_write(h, 0x14, UINT32_MAX, 4); usb_write(h, 4, 0, 4);
            ohci_capture_done(h); usb_write(h, 0x0c, 0x7d, 4);
            h->io.config_write(h->io.cookie, 4, (h->pci.command | 2) & ~4u, 2);
            stopped = !(h->io.read(h->io.cookie, 4, 4) & 0xc0) &&
                !(h->io.read(h->io.cookie, 0x10, 4) & 0x8000007f) &&
                !(h->io.config_read(h->io.cookie, 4, 2) & 4);
        }
        if (stopped && h->irq_attached) {
            stopped = !irq_chain_remove(h->pci.irq_line, usb_irq, h->owner);
            if (stopped) h->irq_attached = false;
        }
        for (unsigned k = 0; k < 3; k++) if (h->claims[k] >= 0) registry_quarantine(h->claims[k], h->claim_gen[k]);
        h->quarantined = true; h->reason = stopped ? "shutdown_stopped" : "shutdown_uncertain";
        usb_host_drop(h);
        if (!stopped) { quiesce_abandon(); return -USB_EIO; }
    }
    return 0;
}
bool usb_power_resume_ready(void)
{
    for (unsigned i = 0; i < host_count; i++) {
        struct usb_host *h = &hosts[i];
        if (!h->owned || h->quarantined || !h->ready || !h->ops ||
            (h->pci.prog_if != 0 && h->pci.prog_if != 0x10 && h->pci.prog_if != 0x20)) return false;
    }
    return true;
}
int usb_host_resume(struct usb_host *h)
{
    if (!h->suspended) return 0;
    fs_lock_take(&h->transfer_lock);
    h->io.config_write(h->io.cookie, 4, h->sleep_regs[10], 2);
    int e = h->io.config_read(h->io.cookie, 4, 2) != h->sleep_regs[10] ? -USB_EIO :
        h->pci.prog_if == 0 ? usb_uhci_resume(h) :
        h->pci.prog_if == 0x10 ? usb_ohci_resume(h) : usb_ehci_resume(h);
    if (!e) for (unsigned i = 0; i < h->device_count; i++) {
        struct usb_device *d = &h->devices[i];
        if (!d->present) continue;
        unsigned st = h->ops->port_status(h, d->root_port);
        if (!(st & USB_PORT_CONNECTED) || !(st & USB_PORT_ENABLED) || (st & USB_PORT_CHANGED)) { e = -USB_EIO; break; }
        for (unsigned k = 0; k < USB_HID_MAX; k++) {
            struct usb_hid *p = &d->hid[k];
            p->pipe_begin = p->progress_at = p->retire_begin = h->io.now(h->io.cookie);
            p->next_poll = p->repeat_at = h->io.now(h->io.cookie) + 20;
        }
    }
    h->suspended = false;
    if (e) usb_host_quarantine(h, "resume_context_lost");
    else { h->ready = h->sleep_ready; h->next_scan = h->io.now(h->io.cookie); }
    fs_lock_drop(&h->transfer_lock);
    return e;
}
int usb_host_suspend(struct usb_host *h)
{
    if (h->suspended) return 0;
    if (!h->ready || h->quarantined || !h->owned) return -USB_ENOTSUP;
    if (!usb_host_take_until(h, h->io.now(h->io.cookie) + 2000)) return -16; /* CiukiOS EBUSY */
    for (unsigned k=0;k<h->device_count;k++) if (h->devices[k].storage.binding) {
        usb_host_drop(h);return -16; /* CiukiOS EBUSY: finish publishing the mount first */
    }
    h->sleep_ready = h->ready; h->ready = false;
    h->sleep_regs[10] = h->io.config_read(h->io.cookie, 4, 2);
    int e = h->pci.prog_if == 0 ? usb_uhci_suspend(h) :
        h->pci.prog_if == 0x10 ? usb_ohci_suspend(h) : usb_ehci_suspend(h);
    h->suspended = true;
    usb_host_drop(h);
    if (e) { int r = usb_host_resume(h); return r ? r : e; }
    return 0;
}
int usb_power_resume(void)
{
    int first = 0;
    for (unsigned i = 0; i < host_count; i++) {
        int e = usb_host_resume(&hosts[i]);
        if (e && !first) first = e;
    }
    return first;
}
int usb_power_suspend(void)
{
    if (!usb_power_resume_ready()) return -USB_ENOTSUP;
    for (unsigned i = 0; i < host_count; i++) {
        int e = usb_host_suspend(&hosts[i]);
        if (e) { int r = usb_power_resume(); return r ? r : e; }
    }
    return 0;
}
static void ohci_root_rearm(struct usb_host *h)
{
    if (h->pci.prog_if != 0x10 || !h->ohci_root_pending || !h->ready) return;
    /* Include scan branches which quarantined a child or disabled its port.
     * Only observed W1C changes are echoed, never hub power/wakeup controls. */
    bool clear = true;
    for (unsigned p = 0; p < h->ports; p++) {
        h->ops->port_ack_changes(h, p);
        if (usb_read(h, 0x54 + 4 * p, 4) & 0x1f0000) clear = false;
    }
    unsigned hub = usb_read(h, 0x50, 4);
    if (hub & 0x20000) usb_write(h, 0x50, 0x20000, 4);
    if (usb_read(h, 0x50, 4) & 0x30000) clear = false;
    if (!clear) return; /* keep polling at the existing 250 ms deadline */
    uint32_t f = usb_irq_save();
    usb_write(h, 0x0c, 0x40, 4);
    h->ohci_root_pending = false;
    if (h->irq_attached) usb_write(h, 0x10, 0x40, 4);
    usb_irq_restore(f);
}
static int claim(struct usb_host *h, unsigned slot, enum res_type type, uint32_t start, uint32_t end, bool share)
{
    int n = registry_claim(type, start, end, h->owner, share);
    if (n < 0) return n;
    h->claims[slot] = n; h->claim_gen[slot] = registry_get((unsigned)n)->generation;
    return registry_activate(n, h->claim_gen[slot]);
}
static bool blocked(void)
{ return drivers_input_firmware(); }
static int ehci_route(struct usb_host *h)
{
    unsigned caplen = usb_read(h, 0, 1), params = usb_read(h, 4, 4);
    if (caplen < 0x10 || caplen > 0x80 || (caplen & 3) || !(params & 15) || (params & 15) > USB_PORT_MAX) return -USB_EPROTO;
    h->ehci_caplen = (uint8_t)caplen; h->ehci_params = params;
    h->inventory.ports = h->ports = params & 15;
    h->inventory.companions = (params >> 12) & 15;
    if (h->pci.vendor == 0x1022 && h->pci.device == 0x7463) h->inventory.quirks |= USB_QUIRK_AMD8111;
    if (h->pci.vendor == 0x10de && (h->pci.device == 0x003c || h->pci.device == 0x005b ||
        h->pci.device == 0x00d8 || h->pci.device == 0x00e8)) h->inventory.quirks |= USB_QUIRK_NVIDIA_31BIT;
    if (h->pci.vendor == 0x1106 && h->pci.device == 0x3104 && (h->pci.revision & 0xf0) == 0x60)
        h->inventory.quirks |= USB_QUIRK_VIA_SLEEP;
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
            if (!(cap & (1u << 24))) return -USB_EIO;
            /* Disable legacy SMI enables, clear defined W1C statuses. */
            h->io.config_write(h->io.cookie, eecp + 4, 0xe0000000u, 4);
            if (h->io.config_read(h->io.cookie, eecp + 4, 4) & 0xe03f) return -USB_EIO;
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
    if (h->io.config_read(h->io.cookie, 4, 2) & 4) return -USB_EIO;
    usb_write(h, caplen + 0x40, 0, 4);
    if (usb_read(h, caplen + 0x40, 4) & 1) return -USB_EIO;
    h->reason = "route_only_companions"; return 0;
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
static int enable_irq(struct usb_host *h)
{
    for (unsigned i = 0; i < USB_HOST_MAX; i++) if (!irq_hosts[i]) {
        irq_hosts[i] = h;
        int err = irq_chain_add(h->pci.irq_line, usb_irq, h->owner);
        if (!err) {
            h->irq_attached = true;
            if (h->pci.prog_if == 0) {
                /* ICH USB_LEGKEY bit 13 gates native PCI interrupts too.
                 * Keep every SMI/trap enable clear (ICH3 §11.1.16). */
                h->io.config_write(h->io.cookie, 0xc0, 0x2000, 2);
                if ((h->io.config_read(h->io.cookie, 0xc0, 2) & 0x20bf) != 0x2000) {
                    usb_host_quarantine(h, "irq_gate_failed"); return -USB_EIO;
                }
            }
            /* Completion/error/root changes only; no per-frame heartbeat. */
            if (h->pci.prog_if == 0x20) {
                usb_write(h, h->ehci_caplen + 8, 0x17, 4);
                if ((h->io.read(h->io.cookie, h->ehci_caplen + 8, 4) & 0x3f) != 0x17) {
                    usb_host_quarantine(h, "ehci_irq_mask_failed"); return -USB_EIO;
                }
            }
            else usb_write(h, h->pci.prog_if ? 0x10 : 4, h->pci.prog_if ? 0x80000052u : 0x0d, h->pci.prog_if ? 4 : 2);
        } else irq_hosts[i] = 0;
        break;
    }
    return 0;
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
    h->inventory.version = USB_CONTROLLER_INVENTORY_VERSION;
    h->inventory.size = sizeof(h->inventory);
    h->inventory.pci_locator = usb_pci_locator(p);
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
    if ((h->ops || dma || phys) && (!dma || !phys || (phys & 4095) || ((uintptr_t)dma & 4095) || phys > 768u * 1024 * 1024 - USB_DMA_BYTES)) {
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
    bool intx = h->ops && pci_irq_routed(p);
    if (!err && intx && claim(h, 1, RES_IRQ, p->irq_line, p->irq_line + 1u, true)) intx = false;
    if (!err && dma) err = claim(h, 2, RES_MMIO, phys, phys + USB_DMA_BYTES, false);
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
    h->ready = h->ops != 0;
    if (intx && enable_irq(h)) return -USB_EIO;
    if (h->ready) h->reason = h->irq_attached ? "ready_irq" : "ready_polling";
    klog("[usb] controller=%s ports=%u pci=%02x:%02x.%u mode=%s reason=%s",
         uhci ? "uhci" : p->prog_if == 0x10 ? "ohci" : "ehci", h->ports, p->bus, p->dev, p->fn,
         h->irq_attached ? "irq" : "poll", h->reason);
    return 0;
}
static bool companion_ready(const struct usb_host *peer)
{
    if (!peer->ready || !peer->owned || peer->quarantined) return false;
    for (unsigned slot = 0; slot < 3; slot += 2)
        if (peer->claims[slot] >= 0 && !registry_valid(peer->claims[slot], peer->claim_gen[slot])) return false;
    return true;
}
bool usb_ehci_companion_ready(const struct usb_host *h, unsigned port)
{
    if (port >= h->ports || !h->inventory.port_companion[port]) return false;
    for (unsigned i = 0; i < h->ehci_group_count; i++) {
        const struct usb_host *peer = &h->ehci_group[i];
        if (usb_pci_locator(&peer->pci) == h->inventory.port_companion[port]) return companion_ready(peer);
    }
    return false;
}
int usb_controller_snapshot(const struct usb_host *h, struct usb_controller_inventory *out)
{
    if (!h || !out || out->version != USB_CONTROLLER_INVENTORY_VERSION || out->size != sizeof(*out)) return -EINVAL;
    *out = h->inventory;
    out->version = USB_CONTROLLER_INVENTORY_VERSION; out->size = sizeof(*out);
    out->pci_locator = usb_pci_locator(&h->pci); out->ports = h->ports;
    if (h->ehci_group) {
        out->ready_functions = 0;
        for (unsigned i = 0; i < h->ehci_group_count; i++) {
            const struct usb_host *peer = &h->ehci_group[i];
            if (peer->pci.fn < 8 && peer->pci.bus == h->pci.bus && peer->pci.dev == h->pci.dev &&
                (out->companion_functions & (1u << peer->pci.fn)) && companion_ready(peer))
                out->ready_functions |= 1u << peer->pci.fn;
        }
    }
    out->flags = (h->owned ? USB_CONTROLLER_OWNED : 0) |
        (h->pci.prog_if == 0x20 ?
         (h->ready ? USB_CONTROLLER_HIGH_SPEED : h->owned ? USB_CONTROLLER_ROUTE_ONLY : 0) : 0) |
        (h->quarantined ? USB_CONTROLLER_QUARANTINED : 0);
    return 0;
}
int usb_ehci_activate(struct usb_host *h, struct usb_host *group, unsigned count)
{
    if (!h || !group || count > USB_HOST_MAX || blocked() || h->pci.prog_if != 0x20 ||
        !h->owned || h->quarantined || h->ready || h->ehci_started) return -USB_EIO;
    h->ehci_group = group; h->ehci_group_count = count;
    unsigned ehcis = 0, companions = 0;
    struct usb_host *by_fn[8] = {0};
    h->inventory.companion_functions = h->inventory.ready_functions = 0;
    memset(h->inventory.port_companion, 0, sizeof(h->inventory.port_companion));
    for (unsigned i = 0; i < count; i++) {
        struct usb_host *peer = &group[i];
        if (peer->pci.bus != h->pci.bus || peer->pci.dev != h->pci.dev || peer->pci.vendor != h->pci.vendor ||
            peer->pci.class_code != 0x0c || peer->pci.subclass != 3) continue;
        if (peer->pci.prog_if == 0x20) { ehcis++; continue; }
        if (peer->pci.fn > 7 || (peer->pci.prog_if != 0 && peer->pci.prog_if != 0x10)) continue;
        if (by_fn[peer->pci.fn]) { h->reason = "route_only_ambiguous_companions"; return -ENOSYS; }
        by_fn[peer->pci.fn] = peer; companions++;
        h->inventory.companion_functions |= 1u << peer->pci.fn;
        if (peer->ready && !peer->quarantined && peer->owned && !peer->device_count && usb_leases_valid(peer))
            h->inventory.ready_functions |= 1u << peer->pci.fn;
    }
    h->reason = "route_only_companions_unqualified";
    if (ehcis != 1 || companions != h->inventory.companions) return -ENOSYS;
    struct usb_host *ordered[8]; unsigned n = 0;
    for (unsigned fn = 0; fn < 8; fn++) if (by_fn[fn]) ordered[n++] = by_fn[fn];
    unsigned routes = usb_read(h, 0x0c, 4), per = (h->ehci_params >> 8) & 15;
    for (unsigned p = 0; p < h->ports && companions; p++) {
        unsigned index = h->ehci_params & 0x80 ? (routes >> (p * 4)) & 15 : per ? p / per : 15;
        if (index >= companions) { h->reason = "route_only_invalid_port_map"; return -ENOSYS; }
        h->inventory.port_companion[p] = usb_pci_locator(&ordered[index]->pci);
    }
    for (unsigned i = 0; i < companions; i++) ordered[i]->inventory.parent_locator = usb_pci_locator(&h->pci);
    if (h->inventory.companion_functions != h->inventory.ready_functions) return -ENOSYS;
    h->reason = "route_only_no_dma";
    if (!h->dma || h->claims[2] < 0) return -ENOSYS;
    h->reason = "route_only_ehci_revision";
    if (usb_read(h, 2, 2) != 0x100) return -ENOSYS;
    h->reason = "route_only_chipset_errata";
    if (h->inventory.quirks & USB_QUIRK_AMD8111) return -ENOSYS;
    if (!usb_leases_valid(h)) return -USB_EIO;
    h->ops = &usb_ehci_ops;
    bool intx = pci_irq_routed(&h->pci);
    if (intx && claim(h, 1, RES_IRQ, h->pci.irq_line, h->pci.irq_line + 1u, true)) intx = false;
    int err = h->ops->start(h);
    if (err) { usb_host_quarantine(h, "ehci_activation"); return err; }
    h->ready = true;
    if (intx && enable_irq(h)) return -USB_EIO;
    h->reason = h->irq_attached ? "ready_irq_hs_tt" : "ready_polling_hs_tt";
    klog("[usb] controller=ehci ports=%u pci=%02x:%02x.%u reason=%s companions=%02x ready=%02x",
         h->ports, h->pci.bus, h->pci.dev, h->pci.fn, h->reason,
         h->inventory.companion_functions, h->inventory.ready_functions);
    return 0;
}
static int packet(struct usb_device *d, unsigned pid, unsigned toggle, void *data, unsigned len)
{
    if (!d->host->ready || d->host->quarantined || d->quarantined) return -USB_EIO;
    return d->host->ops->packet(d, 0, pid, toggle, data, len, false);
}
static int control_message(struct usb_device *d, uint8_t type, uint8_t req, uint16_t value, uint16_t index, void *data, unsigned len)
{
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
int usb_control(struct usb_device *d, uint8_t type, uint8_t req, uint16_t value, uint16_t index, void *data, unsigned len)
{
    if (!d || !d->host || len > USB_CONFIG_MAX || (len && !data) || !d->ep0 || d->ep0 > 64) return -EINVAL;
    /* fx-21b: Linux usb_control_msg() bounds a whole message (SETUP, data,
     * status) by USB_CTRL_GET/SET_TIMEOUT. An enumeration request (device
     * not yet configured) gets that 5 s; a slow device that answers within
     * it enumerates, as on Linux and Windows. Configured devices keep the
     * 500 ms per stage (USB 2.0 section 9.2.6.4). */
    bool bounded = !d->configured;
    if (bounded) d->control_deadline = d->host->io.now(d->host->io.cookie) + USB_CTRL_TIMEOUT_MS;
    int err = control_message(d, type, req, value, index, data, len);
    if (bounded) d->control_deadline = 0;
    return err;
}
static unsigned le16(const uint8_t *p) { return p[0] | (unsigned)p[1] << 8; }
/* fx-21b: Windows reports a device that fails enumeration with the
 * notification "USB Device Not Recognized" (Microsoft KB 2654149). Only a
 * device still connected to a working, not quiescing controller counts: an
 * unplug or a controller fault during enumeration is not a malfunction. */
static uint32_t unrecognized_devices;
uint32_t usb_unrecognized_devices(void) { return __atomic_load_n(&unrecognized_devices, __ATOMIC_RELAXED); }
static void unrecognized(struct usb_host *h, unsigned root, unsigned parent, unsigned port, const char *reason)
{
    if (!h->ready || h->quarantined || h->quiescing ||
        !(h->ops->port_status(h, root) & USB_PORT_CONNECTED)) return;
    uint32_t count = __atomic_add_fetch(&unrecognized_devices, 1, __ATOMIC_RELAXED);
    klog("[usb] unrecognized controller=%s port=%u parent=%u reason=%s count=%u notify=desktop tick=%llu",
         h->owner, port + 1, parent, reason, count, (unsigned long long)h->io.now(h->io.cookie));
}
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
            if (!p[3] && p[5] == 9) d->tt_multi = p[7] == 2;
            if (!p[3]) {
                unsigned bit = 1u << (p[2] & 7);
                if (seen_interface[p[2] / 8] & bit) return -USB_EPROTO;
                seen_interface[p[2] / 8] |= (uint8_t)bit; interfaces++;
            }
            if (!p[3] && p[5] == 8 && p[6] == 6 && p[7] == 0x50) {
                if (d->low_speed || d->storage.bound || p[4] != 2) return -USB_EPROTO;
                storage = &d->storage; storage->bound = true; storage->interface = p[2];
            }
            if (!p[3] && p[5] == 3 && (p[6] == 0 || (p[6] == 1 && (p[7] == 1 || p[7] == 2)))) {
                if (count == USB_HID_MAX) return -ENOSPC;
                hid = &d->hid[count++];
                *hid = (struct usb_hid){.interface=p[2], .protocol=p[6]==1 ? p[7] : 0, .boot_capable=p[6]==1};
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
                    (d->high_speed ? mps != 512 : mps != 8 && mps != 16 && mps != 32 && mps != 64) || (storage_endpoints & bit) || ((p[2] & 0x80) && (endpoints & (1u<<ep)))) return -USB_EPROTO;
                storage_endpoints |= bit;
                if (p[2] & 0x80) {
                    if (storage->in) return -USB_EPROTO;
                    storage->in = (uint8_t)ep; storage->in_packet = (uint16_t)mps;
                } else {
                    if (storage->out) return -USB_EPROTO;
                    storage->out = (uint8_t)ep; storage->out_packet = (uint16_t)mps;
                }
            }
            if (hid && (p[2] & 0x80) && (p[3] & 3) == 3) {
                unsigned mps = le16(p + 4);
                if (!(p[2] & 15) || (p[2] & 0x70) || !p[6] || (d->high_speed && p[6] > 14) || mps > (d->low_speed ? 8u : 64u) || mps < (hid->protocol == 1 ? 8u : hid->protocol == 2 ? 3u : 1u) || hid->endpoint) return -USB_EPROTO;
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
    if (root >= h->ports || parent > h->device_count ||
        (parent && port >= h->devices[parent - 1].hub_ports)) return -EINVAL;
    if (h->device_count == USB_DEVICE_MAX || h->next_address > 127) {
        klog("[usb] quarantine port=%u reason=device_capacity", port); return -ENOSPC;
    }
    unsigned slot = h->device_count++;
    struct usb_device *d = &h->devices[slot]; memset(d, 0, sizeof(*d));
    d->host = h; d->ep0 = 8; d->low_speed = low; d->root_port = (uint8_t)root;
    d->parent = (uint8_t)parent; d->port = (uint8_t)port; d->present = true;
    d->high_speed = h->pci.prog_if == 0x20 && !low &&
        (!parent || (h->devices[parent - 1].hub_status[port] & 0x400));
    if (d->high_speed) d->ep0 = 64;
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
            if (!h->ready || h->quarantined || h->quiescing ||
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
                (b[7] == 8 || b[7] == 16 || b[7] == 32 || b[7] == 64) && (!low || b[7] == 8) && (!d->high_speed || b[7] == 64)) goto descriptor_ready;
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
    unsigned serial_index=b[16];
    klog("[usb] device addr=%u vid:pid=%04x:%04x class=%02x speed=%s", d->address, d->vendor, d->product, d->class_code, low ? "low" : d->high_speed ? "high" : "full");
    err = usb_control(d, 0x80, 6, 0x200, 0, b, 9);
    if (err != 9 || b[0] != 9 || b[1] != 2) goto failed;
    unsigned len = le16(b + 2); if (len < 9 || len > sizeof(b)) goto failed;
    err = usb_control(d, 0x80, 6, 0x200, 0, b, len); if (err != (int)len) goto failed;
    err = usb_parse_config(d, b, len); if (err) goto failed;
    unsigned host_index = 0;
    for (unsigned i = 0; i < host_count; i++) if (&hosts[i] == h) { host_index = i; break; }
    if (!system_admit(0x60000u + host_index * USB_DEVICE_MAX + slot, d->storage.bound ? "usb-storage" : "usb-host")) {
        d->reason = "disabled_by_hardware_profile"; return 0;
    }
    err = usb_control(d, 0, 9, d->config, 0, 0, 0); if (err) goto failed;
    d->configured = true; d->reason = "unsupported_class";
    if (d->class_code == 9) {
        if (parent || low) goto failed;
        err = usb_hub_attach(d); if (err) goto failed;
        d->reason = "hub_ready";
    }
    /* Optional descriptor reads stay in the serialized enumeration worker. */
    bool has_hid=false;
    for (unsigned i=0;i<USB_HID_MAX;i++) if (d->hid[i].endpoint) has_hid=true;
    if (has_hid && usb_identity_serial) usb_identity_serial(d,serial_index);
    for (unsigned i = 0; i < USB_HID_MAX; i++) if (d->hid[i].endpoint) {
        unsigned report_len = d->hid[i].report_length;
        err = usb_hid_attach(d, &d->hid[i], report_len);
        if (err==-USB_ENOTSUP) {
            klog("[usb] hid unsupported addr=%u interface=%u reason=report_layout",d->address,d->hid[i].interface);
            if (!strncmp(d->reason,"unsupported_class",sizeof("unsupported_class"))) d->reason="hid_unsupported";
            continue; /* An unhandled vendor interface must not detach its peers. */
        }
        if (err) goto failed;
        input_pending = true; kwait_wake_all(&input_changed);
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
    unrecognized(h, root, parent, port, err < 0 ? "enumeration_transfer" : "descriptor_or_topology");
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
    if (h->busy || h->suspended) { fs_lock_drop(&h->transfer_lock); return; }
    if (!h->ready || h->quarantined) { fs_lock_drop(&h->transfer_lock); usb_storage_service(h); return; }
    h->busy = true;
    if (!usb_leases_valid(h)) { usb_host_drop(h); return; }
    if ((!h->scanned || periodic_scan()) && usb_expired(h, h->next_scan)) {
        uint64_t tick = h->io.now(h->io.cookie);
        bool first_scan = !h->scanned;
        h->next_scan = tick + 250;
        if (!h->scanned) {
            h->scanned = true; h->scan_window = tick;
            klog("[usb] worker controller=%s first_scan_tick=%llu interval_ms=250", h->owner, tick);
        } else if (tick - h->scan_window >= 1000) {
            uint64_t elapsed = tick - h->scan_window;
            unsigned rate = (unsigned)((uint64_t)h->scan_count * 1000000 / elapsed);
            klog_diag("[usb] worker controller=%s tick=%llu scans=%u elapsed_ms=%llu scans_per_second=%u.%03u",
                 h->owner, tick, h->scan_count, elapsed, rate / 1000, rate % 1000);
            h->scan_window = tick; h->scan_count = 0;
        }
        h->scan_count++;
        if (smi_mark) smi_mark(SMI_USB_SCAN); /* f1-94 blackout coincidence */
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
                 * new connection. Acknowledge observed changes without reset.
                 * fx-21b: once the quarantined connection was reported (its
                 * changes acknowledged by an earlier scan), a new connect
                 * change with CCS set is a re-plug faster than one scan. */
                bool replugged = (s & USB_PORT_CONNECTED) && (s & USB_PORT_CHANGED) &&
                                 log->quarantine_reported;
                h->ops->port_ack_changes(h, p);
                if ((s & USB_PORT_CONNECTED) && !replugged) {
                    if (!log->quarantine_reported) usb_port_record(h, p, "connect", 0);
                } else {
                    if ((previous & USB_PORT_CONNECTED) && !replugged) usb_port_record(h, p, "disconnect", 0);
                    log->quarantine_reported = false;
                    if (child) { usb_detach(child, "disconnected"); h->root_child[p] = 0; }
                    /* f1-96b: the quarantined device is physically gone. A
                     * re-plug is debounced, reset and enumerated as a new
                     * device; a persisting overcurrent blocks it again. */
                    h->root_blocked[p] = false;
                    klog("[usb] release port=%u controller=%s reason=%s tick=%llu",
                         p + 1, h->owner, replugged ? "reconnect" : "disconnect", h->io.now(h->io.cookie));
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
                if (err == -USB_EAGAIN && (h->ops->port_status(h, p) & USB_PORT_COMPANION)) {
                    h->ops->port_ack_changes(h, p); continue;
                }
                if (err) {
                    if (!h->ready || h->quarantined) break;
                    h->root_blocked[p] = true;
                    h->ops->port_disable(h, p);
                    klog("[usb] quarantine controller=%s port=%u stage=root_port_reset result=%d scope=port",
                         h->owner,p+1,err);
                    unrecognized(h, p, 0, p, "root_port_reset");
                    continue;
                }
                s = h->ops->port_status(h, p);
                err = usb_enumerate(h, p, 0, p, !!(s & USB_PORT_LOW));
                if (err > 0) h->root_child[p] = (uint8_t)err;
                else {
                    if (!h->ready || h->quarantined) break;
                    h->root_blocked[p] = true;
                    h->ops->port_disable(h, p);
                    klog("[usb] quarantine controller=%s port=%u stage=enumerate result=%d scope=port",
                         h->owner,p+1,err);
                    continue;
                }
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
        ohci_root_rearm(h);
        usb_uhci_dump_schedule(h);
    }
    for (unsigned i = 0; i < h->device_count && h->ready; i++) {
        struct usb_device *d = &h->devices[i];
        if (!d->configured || !d->present || d->quarantined) continue;
        if (d->tt_pending) usb_ehci_tt_service(d);
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
    uint32_t v = width == 1 ? inb(0xcfc + (at & 3)) : width == 2 ? cpu_inw(0xcfc + (at & 2)) : inl(0xcfc); irq_restore(f); return v;
}
static void cfg_write(void *arg, unsigned at, uint32_t v, unsigned width)
{
    struct usb_host *h = arg; const struct pci_func *p = &h->pci;
    uint32_t f = irq_save(); outl(0xcf8, 0x80000000u | (uint32_t)p->bus << 16 | (uint32_t)p->dev << 11 | (uint32_t)p->fn << 8 | (at & ~3u));
    if (width == 1) outb(0xcfc + (at & 3), (uint8_t)v); else if (width == 2) cpu_outw(0xcfc + (at & 2), (uint16_t)v); else outl(0xcfc, v); irq_restore(f);
}
static uint64_t native_now(void *arg) { (void)arg; uint32_t f = irq_save(); uint64_t v = g_ticks; irq_restore(f); return v; }
static bool host_pending(void *arg) { return ((struct usb_host *)arg)->irq_pending; }
static bool hid_pending(void *arg) { (void)arg; return input_pending; }
static void native_sleep(void *arg, unsigned ms)
{
    struct usb_host *h = arg;
    if (!h->irq_attached) { task_sleep_ms(ms); return; }
    uint64_t end = deadline_after_ms(ms);
    uint32_t f = irq_save(); h->irq_pending = false; irq_restore(f);
    /* Completion callers must recheck TD/QH state on notifications. Mandatory
     * USB delays use usb_sleep, which preserves its absolute deadline. */
    kwait_wait_until(&h->changed, host_pending, h, end);
}
static void service(void *arg)
{
    struct usb_host *h = arg;
    klog("[usb] worker service=started tick=%llu controllers=%u controller=%s", native_now(0), host_count, h->owner);
    for (;;) {
        if (!h->ready || h->quarantined) { task_sleep_ms(1000); continue; }
        uint32_t f = irq_save(); h->irq_pending = false; irq_restore(f);
        if (g_current) g_current->device_worker_site=(uint32_t)(uintptr_t)usb_step;
        usb_step(h);
        if (g_current) g_current->device_worker_site=0;
        uint64_t now = native_now(0);
        uint64_t due = periodic_scan() ? h->next_scan : now+250;
        /* Topology deadlines are independent of completion-driven HID. */
        unsigned ms = due > now && due - now <= 250 ? (uint32_t)(due - now) : 1;
        if (h->irq_attached) kwait_wait_until(&h->changed, host_pending, h, deadline_after_ms(ms));
        else task_sleep_ms(ms);
    }
}
static void input_service(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t f = irq_save(); input_pending = false; irq_restore(f);
        uint64_t due = UINT64_MAX;
        if (smi_mark) smi_mark(SMI_HID_BEGIN); /* f1-94: one usb-hid turn */
        for (unsigned i = 0; i < host_count; i++) {
            fs_lock_take(&hosts[i].transfer_lock);
            if (g_current) g_current->device_worker_site=(uint32_t)(uintptr_t)usb_hid_service;
            usb_hid_service(&hosts[i]);
            uint64_t next = usb_hid_deadline(&hosts[i]);
            if (next < due) due = next;
            if (g_current) g_current->device_worker_site=0;
            fs_lock_drop(&hosts[i].transfer_lock);
        }
        if (smi_mark) smi_mark(SMI_HID_END);
        /* kwait deadlines use modular comparison, so UINT64_MAX is already
         * expired near boot. A half-range-safe horizon represents IRQ-only
         * sleep without a periodic timer in any supported machine lifetime. */
        if (due == UINT64_MAX) due = native_now(0) + (1ull << 62);
        kwait_wait_until(&input_changed, hid_pending, 0, due);
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
        if (!system_admit(system_pci_id(p->bus, p->dev, p->fn), "usb-host")) continue;
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
        uint32_t phys = pmm_alloc_contig(USB_DMA_BYTES / PAGE_SIZE);
        /* The window remains pinned with the controller, including failure. */
        void *mapping = p->prog_if ? vmm_map_mmio(p->bar[0] & ~15u, 4096, true) : 0;
        if ((p->prog_if && !mapping) || (!phys && p->prog_if != 0x20)) {
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
    /* No companion has enumerated yet: changing CF cannot steal a live child. */
    for (unsigned i = 0; i < host_count; i++) if (hosts[i].pci.prog_if == 0x20)
        (void)usb_ehci_activate(&hosts[i], hosts, host_count);
    start_services();
}
#endif
