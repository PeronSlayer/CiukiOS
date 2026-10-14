/* OHCI 1.0a HCCA, ED/TD and root hub. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/usb.h>
static volatile uint32_t *words(struct usb_host *h, unsigned at)
{ return (volatile uint32_t *)(h->dma + at); }
static int start(struct usb_host *h)
{
    unsigned rev = usb_read(h, 0, 4);
    if ((rev & 0xff) != 0x10) return -USB_EPROTO;
    if (usb_read(h, 4, 4) & 0x100) {
        usb_write(h, 8, 8, 4); /* OwnershipChangeRequest, never force IR off. */
        int err = usb_wait_register(h, 4, 4, 0x100, 0, 500); if (err) return err;
    }
    h->owned = true;
    h->io.config_write(h->io.cookie, 4, (h->pci.command | 2u) & ~4u, 2);
    if (h->io.config_read(h->io.cookie, 4, 2) & 4) return -USB_EIO;
    if (rev & 0x100) usb_write(h, 0x100, 0, 4); /* optional HceControl */
    usb_write(h, 0x14, UINT32_MAX, 4);
    usb_write(h, 4, 0, 4); h->io.sleep(h->io.cookie, 50);
    usb_write(h, 8, 1, 4);
    int err = usb_wait_register(h, 8, 4, 1, 0, 100); if (err) return err;
    memset(h->dma, 0, USB_DMA_BYTES);
    words(h, USB_ED)[0] = 1u << 14; /* skipped until a complete TD exists */
    for (unsigned i = 0; i < 32; i++) words(h, 0)[i] = h->dma_phys + USB_ED;
    usb_barrier(); usb_write(h, 0x18, h->dma_phys, 4);
    usb_write(h, 0x20, 0, 4); usb_write(h, 0x28, 0, 4);
    unsigned fi = 11999;
    usb_write(h, 0x34, (1u << 31) | (((fi - 210) * 6 / 7) << 16) | fi, 4);
    usb_write(h, 0x40, fi * 9 / 10, 4); usb_write(h, 0x44, 0x628, 4);
    unsigned rh = usb_read(h, 0x48, 4);
    h->ports = rh & 0xff;
    if (!h->ports || h->ports > USB_PORT_MAX) return -USB_EPROTO;
    /* Respect per-port vs ganged/no-switching power modes. */
    if (!(rh & (1u << 9))) {
        if (rh & (1u << 8)) for (unsigned i = 0; i < h->ports; i++) usb_write(h, 0x54 + 4 * i, 1u << 8, 4);
        else usb_write(h, 0x50, 1u << 16, 4);
    }
    h->io.config_write(h->io.cookie, 4, h->pci.command | 6u, 2);
    if ((h->io.config_read(h->io.cookie, 4, 2) & 6) != 6) return -USB_EIO;
    usb_write(h, 4, 0x80, 4); /* UsbOperational, schedules still disabled */
    h->io.sleep(h->io.cookie, ((rh >> 24) & 255) * 2 + 10);
    return usb_retire_frames(h, 0x3c, 4, 0xffff);
}
static unsigned status(struct usb_host *h, unsigned port)
{
    unsigned v = usb_read(h, 0x54 + 4 * port, 4);
    return (v & 1 ? USB_PORT_CONNECTED : 0) | (v & 2 ? USB_PORT_ENABLED : 0) |
           (v & 0x200 ? USB_PORT_LOW : 0) | (v & 8 ? USB_PORT_OVERCURRENT : 0) |
           (v & 0x10000 ? USB_PORT_CHANGED : 0);
}
static int reset(struct usb_host *h, unsigned port)
{
    if (port >= h->ports) return -EINVAL;
    unsigned off = 0x54 + 4 * port;
    /* OHCI emits 10 ms pulses. USB 7.1.7.5 requires root hubs to
     * provide 50 ms total reset; repeat five pulses, with IF=1. */
    for (unsigned pulse = 0; pulse < 5; pulse++) {
        usb_write(h, off, 0x10000, 4);
        usb_write(h, off, 0x10, 4);
        usb_port_record(h, port, "reset", 0);
        h->io.sleep(h->io.cookie, 10);
        int err = usb_wait_register(h, off, 4, 0x10010, 0x10000, 100);
        usb_port_record(h, port, "reset", err);
        if (err) return err;
        if (!(status(h, port) & USB_PORT_CONNECTED)) return -USB_EIO;
    }
    usb_write(h, off, 0x1f0000, 4); h->io.sleep(h->io.cookie, 10);
    int err = (status(h, port) & 3) == 3 ? 0 : -USB_EIO;
    usb_port_record(h, port, "enable", err);
    return err;
}
static void disable(struct usb_host *h, unsigned port)
{ usb_write(h, 0x54 + 4 * port, 1, 4); usb_port_record(h, port, "enable", 0); }
static int packet(struct usb_device *d, unsigned ep, unsigned pid, unsigned toggle,
                  void *data, unsigned length, bool periodic)
{
    struct usb_host *h = d->host;
    d->packet_trace = (struct usb_packet_trace){.actual=-1};
    if (length > 64 || (d->low_speed && length > 8) || ep > 15) return -EINVAL;
    uint64_t begin = h->io.now(h->io.cookie);
    unsigned budget = usb_packet_budget(d, ep, periodic, begin);
    if (!budget) { d->packet_trace.result=-USB_ETIMEDOUT; return -USB_ETIMEDOUT; }
    unsigned frame = usb_read(h, 0x3c, 4) & 0xffff, frames = 0;
    struct usb_packet_trace *trace = &d->packet_trace;
    *trace = (struct usb_packet_trace){.expected=length,.actual=-1,
        .endpoint=ep,.pid=pid,.toggle=toggle,.valid=true};
    volatile uint32_t *ed = words(h, USB_ED), *td = words(h, USB_TD);
    unsigned direction = pid == USB_PID_SETUP ? 0 : pid == USB_PID_OUT ? 1 : 2;
    bool bulk = ep && !periodic;
    unsigned mps = bulk ? (pid == USB_PID_IN ? d->storage.in_packet : d->storage.out_packet) : ep ? length : d->ep0;
    if (!mps) mps = 8;
    unsigned head = bulk ? 0x28u : 0x20u, current = bulk ? 0x2cu : 0x24u;
    memset(h->dma + USB_TAIL, 0, 16);
    td[0] = (0xfu << 28) | (7u << 21) | ((2u | toggle) << 24) | direction << 19 | (1u << 18);
    td[1] = length ? h->dma_phys + USB_BUFFER : 0;
    td[2] = h->dma_phys + USB_TAIL;
    td[3] = length ? h->dma_phys + USB_BUFFER + length - 1 : 0;
    if (pid != USB_PID_IN && length) memcpy(h->dma + USB_BUFFER, data, length);
    ed[0] = d->address | ep << 7 | (d->low_speed ? 1u << 13 : 0) | mps << 16 | (1u << 14);
    ed[1] = h->dma_phys + USB_TAIL; ed[2] = h->dma_phys + USB_TD; ed[3] = 0;
    words(h, 0x84)[0] = 0; /* old HCCA DoneHead */
    usb_write(h, 0x0c, 0x7f, 4);
    if (!periodic) { usb_write(h, current, 0, 4); usb_write(h, head, h->dma_phys + USB_ED, 4); }
    usb_barrier(); ed[0] &= ~(1u << 14); usb_barrier();
    usb_write(h, 4, 0x80 | (periodic ? 4 : bulk ? 0x20 : 0x10), 4);
    if (!periodic) usb_write(h, 8, bulk ? 4 : 2, 4); /* Bulk/ControlListFilled */
    frame = usb_read(h, 0x3c, 4) & 0xffff;
    uint64_t end = begin + budget;
    unsigned tries = budget + 1;
    int result = -USB_ETIMEDOUT;
    for (unsigned i = 0; i < tries; i++) {
        unsigned s = usb_read(h, 0x0c, 4);
        unsigned next = usb_read(h, 0x3c, 4) & 0xffff;
        frames += (next - frame) & 0xffff; frame = next;
        trace->hc_status=s;
        if (s & 0x10) {
            usb_barrier(); trace->status=td[0]; trace->token=ed[0];
            trace->frames=trace->poll_frames=frames; trace->elapsed_ms=h->io.now(h->io.cookie)-begin;
            trace->result=trace->retired=-USB_EIO;
            usb_host_quarantine(h, "ohci_host_error"); return -USB_EIO;
        }
        usb_barrier(); unsigned cc = td[0] >> 28;
        /* ED HeadP retirement, not only TD CC: hardware may still write
         * NextTD/DoneHead until the packet has been removed from the ED. */
        if (cc < 14 && ((ed[2] & ~15u) == ed[1] || (ed[2] & 1))) {
            break;
        }
        if (periodic && frames >= 2) { result = -USB_EAGAIN; break; }
        if (usb_expired(h, end)) break;
        h->io.sleep(h->io.cookie, 1);
    }
    trace->poll_frames=frames;
    ed[0] |= 1u << 14; usb_barrier();
    usb_write(h, 4, 0x80, 4);
    int retired = usb_retire_frames(h, 0x3c, 4, 0xffff);
    frames += (usb_read(h, 0x3c, 4) - frame) & 0xffff;
    usb_barrier(); trace->status=td[0]; trace->token=ed[0]; trace->frames=frames;
    trace->elapsed_ms=h->io.now(h->io.cookie)-begin; trace->retired=retired;
    if (retired) { trace->result=-USB_EIO; return -USB_EIO; }
    usb_write(h, head, 0, 4); usb_write(h, current, 0, 4);
    /* A packet may finish between the last poll and ED skip. Decode the
     * retired TD so a late ACK advances the software toggle exactly once. */
    usb_barrier(); unsigned cc = td[0] >> 28;
    if (cc < 14 && ((ed[2] & ~15u) == ed[1] || (ed[2] & 1))) {
        if (cc == 4) result = -USB_EPIPE;
        else if (cc && cc != 9) result = -USB_EIO;
        else {
            unsigned cur = td[1];
            if (cur && (cur < h->dma_phys + USB_BUFFER || cur > h->dma_phys + USB_BUFFER + length)) {
                trace->result=-USB_EIO;
                usb_host_quarantine(h, "ohci_dma_pointer"); return -USB_EIO;
            }
            result = cur ? (int)(cur - h->dma_phys - USB_BUFFER) : (int)length;
        }
    }
    trace->result=result;
    if (result >= 0) trace->actual=result;
    if (result >= 0 && pid == USB_PID_IN && result) memcpy(data, h->dma + USB_BUFFER, (unsigned)result);
    return result;
}
const struct usb_hc_ops usb_ohci_ops = {start, status, reset, disable, packet};
