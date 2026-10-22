/* EHCI 1.0 high-speed QH/qTD engine. SPDX-License-Identifier: GPL-2.0-only
 * Written against Intel EHCI 1.0; research/limits: peripheral-driver-coverage.md.
 * Full/low-speed TT splits and isochronous transfers are not implemented. */
#include <ciuki/kernel.h>
#include <ciuki/usb.h>
#define END 1u
#define QH 2u
#define ACTIVE 0x80u
#define HALTED 0x40u
#define ASE 0x20u
#define PSE 0x10u
#define ASS 0x8000u
#define PSS 0x4000u
#define IAA 0x20u
#define SLOTS (USB_DEVICE_MAX * USB_HID_MAX)
_Static_assert(USB_EHCI_HID_BASE + SLOTS * USB_EHCI_HID_STRIDE <= USB_EHCI_BULK_BUFFER,
               "EHCI periodic slots overlap bulk DMA");
_Static_assert(USB_EHCI_BULK_BUFFER + 1024 <= USB_DMA_BYTES, "EHCI arena overflow");
static volatile uint32_t *words(struct usb_host *h, unsigned at)
{ return (volatile uint32_t *)(h->dma + at); }
static unsigned rd(struct usb_host *h, unsigned at)
{ return usb_read(h, h->ehci_caplen + at, 4); }
static void wr(struct usb_host *h, unsigned at, unsigned v)
{ usb_write(h, h->ehci_caplen + at, v, 4); }
static int wait(struct usb_host *h, unsigned at, unsigned mask, unsigned value)
{ return usb_wait_register(h, h->ehci_caplen + at, 4, mask, value, 100); }
static unsigned port_read(struct usb_host *h, unsigned p)
{ return rd(h, 0x44 + p * 4); }
static void port_write(struct usb_host *h, unsigned p, unsigned set, unsigned clear, unsigned ack)
{
    /* Preserve ordinary RW fields; never echo W1C, test or reserved bits. */
    unsigned v = port_read(h, p) & 0x0070f1c4u;
    wr(h, 0x44 + p * 4, ((v | set) & ~clear) | (ack & 0x2a));
}
bool usb_ehci_stop(struct usb_host *h)
{
    if (!h->owned || !h->ehci_caplen) return false;
    h->ready = false;
    wr(h, 8, 0);
    wr(h, 0, rd(h, 0) & ~(ASE | PSE | 0x41u));
    int err = wait(h, 4, 0xd000, 0x1000);
    h->io.config_write(h->io.cookie, 4, (h->pci.command | 2u) & ~4u, 2);
    wr(h, 4, 0x3f);
    bool stopped = !err && !(h->io.config_read(h->io.cookie, 4, 2) & 4) &&
        !(rd(h, 8) & 0x3f) && !(rd(h, 0) & (ASE | PSE | 1)) &&
        /* Retained IRQ faults describe the failure, not a still-asserted
         * source. Isolation proof must read the physical status directly. */
        (h->io.read(h->io.cookie, h->ehci_caplen + 4, 4) & 0xd03f) == 0x1000;
    if (stopped) { wr(h, 0x40, 0); stopped = !(rd(h, 0x40) & 1); }
    /* No arena or lease is released, even after a successful stop. */
    h->ehci_started = false;
    return stopped;
}
static void empty_qh(struct usb_host *h, unsigned at, unsigned link)
{
    memset(h->dma + at, 0, 96); /* also initializes 64-bit buffer extensions */
    volatile uint32_t *q = words(h, at);
    q[0] = link; q[4] = q[5] = END; q[6] = HALTED;
}
static int start(struct usb_host *h)
{
    if (!h->owned || !h->dma || !h->ehci_caplen) return -USB_EIO;
    wr(h, 0, 2); /* HCRESET only on this halted controller, before clients */
    int err = wait(h, 0, 2, 0); if (err) return err;
    if (h->inventory.quirks & USB_QUIRK_VIA_SLEEP) {
        unsigned v = h->io.config_read(h->io.cookie, 0x4b, 1);
        h->io.config_write(h->io.cookie, 0x4b, v | 0x20, 1);
        if (!(h->io.config_read(h->io.cookie, 0x4b, 1) & 0x20)) return -USB_EIO;
    }
    memset(h->dma, 0, USB_DMA_BYTES);
    /* Frame links always reference an inactive QH, including AMD/ATI parts
     * whose errata forbid NULL+terminate frame entries. */
    empty_qh(h, USB_EHCI_HEAD, h->dma_phys + USB_EHCI_HEAD + QH);
    words(h, USB_EHCI_HEAD)[1] = (2u << 12) | (1u << 15);
    empty_qh(h, USB_EHCI_QH, h->dma_phys + USB_EHCI_HEAD + QH);
    for (unsigned i = 0; i < SLOTS; i++)
        empty_qh(h, USB_EHCI_HID_BASE + i * USB_EHCI_HID_STRIDE, END);
    /* A separate immutable periodic terminator, outside any live QH. */
    empty_qh(h, 4480, END);
    for (unsigned i = 0; i < 1024; i++) words(h, 0)[i] = h->dma_phys + 4480 + QH;
    wr(h, 8, 0); wr(h, 4, 0x3f); wr(h, 0x0c, 0);
    if (usb_read(h, 8, 4) & 1) wr(h, 0x10, 0); /* CTRLDSSEGMENT */
    wr(h, 0x14, h->dma_phys); wr(h, 0x18, h->dma_phys + USB_EHCI_HEAD);
    usb_barrier();
    h->io.config_write(h->io.cookie, 4, h->pci.command | 6u, 2);
    if ((h->io.config_read(h->io.cookie, 4, 2) & 6) != 6) return -USB_EIO;
    wr(h, 0, (8u << 16) | ASE | PSE | 1); /* 1 ms IRQ threshold, 1024 frames */
    if ((err = wait(h, 4, 0xd000, ASS | PSS))) return err;
    wr(h, 0x40, 1);
    if (!(rd(h, 0x40) & 1)) return -USB_EIO;
    if (h->ehci_params & 0x10) {
        for (unsigned p = 0; p < h->ports; p++) port_write(h, p, 0x1000, 0, 0);
        usb_sleep(h, 20);
    }
    h->ehci_started = true;
    return usb_retire_frames(h, h->ehci_caplen + 0x0c, 4, 0x3fff);
}
static unsigned status(struct usb_host *h, unsigned p)
{
    unsigned v = port_read(h, p);
    if (v == UINT32_MAX) { usb_host_quarantine(h, "ehci_removed"); return 0; }
    if (v & 0x2000) return USB_PORT_COMPANION | (v & 0x2a ? USB_PORT_CHANGED : 0);
    return (v & 1 ? USB_PORT_CONNECTED : 0) | (v & 4 ? USB_PORT_ENABLED | USB_PORT_HIGH : 0) |
        (v & 0x10 ? USB_PORT_OVERCURRENT : 0) | (v & 0x2a ? USB_PORT_CHANGED : 0);
}
static int hand_back(struct usb_host *h, unsigned p)
{
    if (!usb_ehci_companion_ready(h, p)) {
        klog("[usb] companion port=%u reason=companion_unavailable target=%08x",
             p + 1, h->inventory.port_companion[p]);
        return -ENOSYS;
    }
    port_write(h, p, 0x2000, 4, 0);
    if (!(port_read(h, p) & 0x2000)) return -USB_EIO;
    klog("[usb] companion port=%u parent=%08x target=%08x reason=low_full_speed",
         p + 1, h->inventory.pci_locator, h->inventory.port_companion[p]);
    return -USB_EAGAIN;
}
static int reset(struct usb_host *h, unsigned p)
{
    if (p >= h->ports) return -EINVAL;
    if (usb_port_has_live_child(h, p)) return -USB_EAGAIN;
    unsigned v = port_read(h, p);
    if (!(v & 1) || (v & 0x2010)) return -USB_EIO;
    if ((v & 0xc00) == 0x400) return hand_back(h, p);
    port_write(h, p, 0x100, 4, 0); usb_sleep(h, 50);
    port_write(h, p, 0, 0x100, 0);
    int err = wait(h, 0x44 + p * 4, 0x100, 0); if (err) return err;
    usb_sleep(h, 10);
    v = port_read(h, p);
    if (!(v & 1) || (v & 0x10)) return -USB_EIO;
    if (!(v & 4)) return hand_back(h, p);
    port_write(h, p, 0, 0, v & 0x2a);
    return 0;
}
static void disable(struct usb_host *h, unsigned p)
{
    if (p < h->ports && !(port_read(h, p) & 0x2000) && !usb_port_has_live_child(h, p))
        port_write(h, p, 0, 4, 0);
}
static int enable(struct usb_host *h, unsigned p)
{
    /* EHCI PE can only be set by hardware following reset. Resetting a live
     * child would silently invalidate its address and peer transactions. */
    return p < h->ports && (port_read(h, p) & 5) == 5 ? 0 : -USB_EIO;
}
static void ack(struct usb_host *h, unsigned p)
{
    unsigned v = port_read(h, p);
    if (v != UINT32_MAX && (v & 0x2a)) port_write(h, p, 0, 0, v & 0x2a);
}
static void prepare(struct usb_device *d, unsigned qhat, unsigned tdat, unsigned buffer,
                    unsigned ep, unsigned pid, unsigned toggle, unsigned length, unsigned mps)
{
    struct usb_host *h = d->host;
    volatile uint32_t *q = words(h, qhat), *td = words(h, tdat);
    unsigned link = q[0], meta = q[20];
    memset(h->dma + qhat, 0, 96); memset(h->dma + tdat, 0, 64);
    q[0] = link; q[20] = meta;
    q[1] = d->address | ep << 8 | (2u << 12) | (1u << 14) | mps << 16;
    q[2] = 1u << 30; q[4] = h->dma_phys + tdat; q[5] = END;
    td[0] = td[1] = END;
    td[2] = (toggle << 31) | length << 16 | (h->irq_attached ? 0x8000 : 0) |
        (3u << 10) | (pid == USB_PID_SETUP ? 2u : pid == USB_PID_IN ? 1u : 0) << 8 | ACTIVE;
    td[3] = h->dma_phys + buffer;
    /* All bounce regions fit in one page; high buffer dwords remain zero. */
}
static bool healthy(struct usb_host *h)
{
    unsigned s = rd(h, 4);
    if (!h->ready || h->quarantined) return false;
    if (s == UINT32_MAX || (s & 0x1010)) {
        usb_host_quarantine(h, "ehci_host_error"); return false;
    }
    return true;
}
static int decode(struct usb_host *h, unsigned token, unsigned length)
{
    unsigned left = (token >> 16) & 0x7fff;
    if (left > length) { usb_host_quarantine(h, "ehci_dma_length"); return -USB_EIO; }
    if (token & 0x38) return -USB_EIO; /* babble, data buffer, transaction error */
    if (token & HALTED) return -USB_EPIPE;
    if (token & ACTIVE) return -USB_ETIMEDOUT;
    return (int)(length - left);
}
static int unlink_async(struct usb_host *h)
{
    words(h, USB_EHCI_HEAD)[0] = h->dma_phys + USB_EHCI_HEAD + QH;
    usb_barrier();
    /* Poll IAA independently of IRQ delivery. A stale IAA cannot prove this
     * unlink. Two cycles also cover controllers with premature IAA errata. */
    for (unsigned cycle = 0; cycle < 2; cycle++) {
        wr(h, 4, IAA); wr(h, 0, rd(h, 0) | 0x40);
        if (wait(h, 4, IAA, IAA)) {
            /* A stopped asynchronous schedule is an alternative DMA proof. */
            wr(h, 0, rd(h, 0) & ~(ASE | 0x40u));
            if (wait(h, 4, ASS, 0)) {
                usb_host_quarantine(h, "ehci_unlink_uncertain"); return -USB_EIO;
            }
            wr(h, 0, rd(h, 0) | ASE);
            if (wait(h, 4, ASS, ASS)) {
                usb_host_quarantine(h, "ehci_async_restart"); return -USB_EIO;
            }
            break;
        }
        wr(h, 4, IAA);
    }
    return healthy(h) ? 0 : -USB_EIO;
}
static int packet(struct usb_device *d, unsigned ep, unsigned pid, unsigned toggle,
                  void *data, unsigned length, bool periodic)
{
    struct usb_host *h = d->host;
    struct usb_packet_trace *t = &d->packet_trace;
    *t = (struct usb_packet_trace){.expected=length,.actual=-1,.endpoint=ep,.pid=pid,.toggle=toggle};
    if (!d->high_speed || d->low_speed || periodic) return -ENOSYS;
    if (ep > 15 || length > (ep ? 1024u : 64u) || (length && !data)) return -EINVAL;
    if (!healthy(h)) return -USB_EIO;
    uint64_t begin = h->io.now(h->io.cookie);
    unsigned budget = usb_packet_budget(d, ep, false, begin);
    if (!budget) return -USB_ETIMEDOUT;
    unsigned buffer = ep ? USB_EHCI_BULK_BUFFER : USB_BUFFER;
    unsigned mps = ep ? (pid == USB_PID_IN ? d->storage.in_packet : d->storage.out_packet) : d->ep0;
    if (!mps || (ep && mps != 512) || (!ep && mps != 64)) return -USB_EPROTO;
    if (pid != USB_PID_IN && length) memcpy(h->dma + buffer, data, length);
    prepare(d, USB_EHCI_QH, USB_EHCI_TD, buffer, ep, pid, toggle, length, mps);
    usb_barrier(); words(h, USB_EHCI_HEAD)[0] = h->dma_phys + USB_EHCI_QH + QH;
    usb_barrier();
    volatile uint32_t *td = words(h, USB_EHCI_TD);
    uint64_t end = begin + budget;
    for (unsigned i = 0; i <= budget; i++) {
        if (!healthy(h)) return -USB_EIO;
        usb_barrier();
        if (!(td[2] & ACTIVE) || usb_expired(h, end)) break;
        usb_poll_pause(h, 1);
    }
    int retired = unlink_async(h);
    usb_barrier();
    *t = (struct usb_packet_trace){.expected=length,.actual=-1,.endpoint=ep,.pid=pid,
        .toggle=toggle,.status=td[2],.token=words(h, USB_EHCI_QH)[1],.hc_status=rd(h,4),
        .elapsed_ms=h->io.now(h->io.cookie)-begin,.retired=retired,.valid=true};
    int result = retired ? -USB_EIO : decode(h, td[2], length);
    t->result = result;
    if (result >= 0) {
        t->actual = result;
        if (pid == USB_PID_IN && result) memcpy(data, h->dma + buffer, (unsigned)result);
    }
    return result;
}
static unsigned slot(struct usb_device *d, struct usb_hid *p)
{ return USB_EHCI_HID_BASE + ((unsigned)(d - d->host->devices) * USB_HID_MAX + (unsigned)(p - d->hid)) * USB_EHCI_HID_STRIDE; }
static unsigned period(struct usb_host *h, unsigned at) { return words(h, at)[20]; }
static bool periodic_pointer(struct usb_host *h, unsigned link)
{
    unsigned at = (link & ~31u) - h->dma_phys;
    if ((link & 31) == QH && (at == 4480 ||
        (at >= USB_EHCI_HID_BASE && at < USB_EHCI_HID_BASE + SLOTS * USB_EHCI_HID_STRIDE &&
         !((at - USB_EHCI_HID_BASE) % USB_EHCI_HID_STRIDE)))) return true;
    usb_host_quarantine(h, "ehci_periodic_pointer"); return false;
}
static int periodic_link(struct usb_host *h, unsigned at, unsigned frames, unsigned mask)
{
    /* Conservative worst-frame admission: all frame phases are zero, so
     * coincident masks are the maximum load. Budget <=80 us/microframe. */
    for (unsigned u = 0; u < 8; u++) if (mask & (1u << u)) {
        unsigned ns = (words(h, at)[1] >> 16 & 0x7ff) * 20 + 2000;
        for (unsigned i = 0; i < SLOTS; i++) {
            unsigned other = USB_EHCI_HID_BASE + i * USB_EHCI_HID_STRIDE;
            if (period(h, other) && (words(h, other)[2] & (1u << u)))
                ns += (words(h, other)[1] >> 16 & 0x7ff) * 20 + 2000;
        }
        if (ns > 80000) return -ENOSPC;
    }
    words(h, at)[20] = frames;
    /* Powers of two, phase zero: larger periods precede smaller periods.
     * Each shared predecessor has the same successor on every listed frame. */
    for (unsigned f = 0; f < 1024; f += frames) {
        volatile uint32_t *link = words(h, f * 4);
        unsigned walked = 0;
        if (!periodic_pointer(h, *link)) return -USB_EIO;
        while ((*link & ~31u) != h->dma_phys + 4480) {
            if (++walked > SLOTS) { usb_host_quarantine(h, "ehci_periodic_cycle"); return -USB_EIO; }
            unsigned other = (*link & ~31u) - h->dma_phys;
            if (other == at) break;
            if (period(h, other) < frames) break;
            link = words(h, other);
            if (!periodic_pointer(h, *link)) return -USB_EIO;
        }
        if ((*link & ~31u) == h->dma_phys + at) continue;
        words(h, at)[0] = *link; usb_barrier(); *link = h->dma_phys + at + QH;
    }
    usb_barrier(); return 0;
}
static void periodic_unlink(struct usb_host *h, unsigned at)
{
    unsigned next = words(h, at)[0], target = h->dma_phys + at;
    if (!periodic_pointer(h, next)) return;
    for (unsigned f = 0; f < 1024; f++) {
        volatile uint32_t *link = words(h, f * 4);
        if (!periodic_pointer(h, *link)) return;
        for (unsigned n = 0; (*link & ~31u) != h->dma_phys + 4480; n++) {
            if (n >= SLOTS) { usb_host_quarantine(h, "ehci_periodic_cycle"); return; }
            if ((*link & ~31u) == target) { *link = next; break; }
            link = words(h, (*link & ~31u) - h->dma_phys);
            if (!periodic_pointer(h, *link)) return;
        }
    }
    usb_barrier(); words(h, at)[20] = 0;
}
static void interrupt_stop(struct usb_device *d, struct usb_hid *p)
{
    if (p->pipe_state == 1 && d->host->ready && !d->host->quarantined) periodic_unlink(d->host, slot(d, p));
    /* Monotonic device slots remain pinned; no synchronous wait in detach. */
    p->pipe_state = 0;
}
static int interrupt(struct usb_device *d, struct usb_hid *p, void *data, unsigned length)
{
    struct usb_host *h = d->host;
    struct usb_packet_trace *t = &p->trace; t->valid = false;
    if (!d->high_speed || d->low_speed) return -ENOSYS;
    if (!healthy(h)) return -USB_EIO;
    unsigned at = slot(d, p), frame = rd(h, 0x0c) & 0x3fff;
    volatile uint32_t *td = words(h, at + 96);
    uint64_t tick = h->io.now(h->io.cookie);
    if (!p->pipe_state) {
        if (!length || length > 64 || length > p->packet || !p->interval || p->interval > 14) return -USB_EPROTO;
        if (!usb_expired(h, p->next_poll)) return -USB_EAGAIN;
        unsigned microframes = 1u << (p->interval - 1), frames = microframes < 8 ? 1 : microframes / 8;
        unsigned phase = ((at - USB_EHCI_HID_BASE) / USB_EHCI_HID_STRIDE) & 7, mask = 0;
        for (unsigned u = phase % (microframes < 8 ? microframes : 8); u < 8; u += microframes < 8 ? microframes : 8) mask |= 1u << u;
        prepare(d, at, at + 96, at + 160, p->endpoint, USB_PID_IN, p->toggle, length, p->packet);
        words(h, at)[2] |= mask;
        int err = periodic_link(h, at, frames, mask); if (err) return err;
        p->pipe_state = 1; p->pipe_frame = (uint16_t)frame;
        p->pipe_begin = p->pending_at = p->progress_at = tick; p->observed_at = 0;
        *t = (struct usb_packet_trace){.expected=length,.actual=-1,.endpoint=p->endpoint,.pid=USB_PID_IN,.toggle=p->toggle};
        return -USB_EAGAIN;
    }
    usb_barrier();
    if (p->pipe_state == 1) {
        if (frame != p->pipe_frame) p->progress_at = tick;
        p->pipe_frame = (uint16_t)frame;
        if (td[2] & ACTIVE) {
            if (tick - p->progress_at >= USB_PERIODIC_WATCHDOG_MS) usb_host_quarantine(h, "ehci_frame_clock_stalled");
            return -USB_EAGAIN;
        }
        p->observed_at = tick;
        periodic_unlink(h, at);
        if (!h->ready) return -USB_EIO;
        p->pipe_state = 2; p->retire_frame = (uint16_t)frame; p->retire_begin = tick;
        return -USB_EAGAIN;
    }
    /* FRINDEX counts microframes; wait >= two whole frames after unlink. */
    unsigned elapsed = (frame - p->retire_frame) & 0x3fff;
    if (elapsed < 16 || elapsed >= 0x2000) {
        if (tick - p->retire_begin >= USB_PERIODIC_WATCHDOG_MS) usb_host_quarantine(h, "ehci_periodic_retire_uncertain");
        return -USB_EAGAIN;
    }
    t->status = td[2]; t->frames = elapsed / 8; t->elapsed_ms = tick - p->pipe_begin;
    int result = decode(h, td[2], length);
    t->result = result; t->retired = 0; t->valid = true;
    if (result >= 0) { t->actual = result; memcpy(data, h->dma + at + 160, (unsigned)result); }
    p->pipe_state = 0;
    return result;
}
const struct usb_hc_ops usb_ehci_ops = {
    .start=start,.port_status=status,.port_reset=reset,.port_enable=enable,
    .port_disable=disable,.port_ack_changes=ack,.packet=packet,
    .interrupt=interrupt,.interrupt_stop=interrupt_stop,
};
