/* UHCI 1.1 schedule and root hub. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/usb.h>
_Static_assert(USB_UHCI_BULK_QH + 16 <= USB_TD, "UHCI queues overlap the general TD");
static volatile uint32_t *words(struct usb_host *h, unsigned at)
{ return (volatile uint32_t *)(h->dma + at); }
static int start(struct usb_host *h)
{
    unsigned legacy = h->io.config_read(h->io.cookie, 0xc0, 2);
    /* C0: write W1C statuses, leave all SMI/PIRQ enables cleared. */
    h->io.config_write(h->io.cookie, 0xc0, 0x8f00, 2);
    unsigned handed = h->io.config_read(h->io.cookie, 0xc0, 2);
    klog("[usb] uhci handoff controller=%s legsup_before=%04x legsup_after=%04x result=%d",
         h->owner, legacy, handed, handed & ~0xdf40u ? -USB_EIO : 0);
    if (handed & ~0xdf40u) return -USB_EIO;
    h->owned = true;
    h->io.config_write(h->io.cookie, 4, (h->pci.command | 1u) & ~4u, 2);
    if (h->io.config_read(h->io.cookie, 4, 2) & 4) return -USB_EIO;
    usb_write(h, 4, 0, 2); usb_write(h, 0, 0, 2);
    int err = usb_wait_register(h, 2, 2, 0x20, 0x20, 100);
    if (err) return err;
    usb_write(h, 0, 2, 2);
    err = usb_wait_register(h, 0, 2, 2, 0, 100); if (err) return err;
    memset(h->dma, 0, USB_DMA_BYTES);
    /* UHCI 1.1 sections 1.2.3/3.4: interrupt queues precede control and
     * bulk. Both general QHs are always reachable through horizontal links;
     * only one owns the reserved TD/buffer at a time. No reclamation loop. */
    volatile uint32_t *control = words(h, USB_UHCI_CONTROL_QH);
    volatile uint32_t *bulk = words(h, USB_UHCI_BULK_QH);
    control[0] = (h->dma_phys + USB_UHCI_BULK_QH) | 2; control[1] = 1;
    /* An inactive IOC TD is visited once at every frame's schedule tail;
     * it generates no transaction, including while other queues NAK. */
    bulk[0] = h->dma_phys + USB_TAIL; bulk[1] = 1;
    words(h, USB_TAIL)[0] = 1;
    words(h, USB_TAIL)[1] = 1u << 24;
    for (unsigned i = 0; i < USB_DEVICE_MAX * USB_HID_MAX; i++) {
        volatile uint32_t *pipe = words(h, USB_HID_BASE + i * USB_HID_STRIDE);
        pipe[0] = i + 1 < USB_DEVICE_MAX * USB_HID_MAX ?
            (h->dma_phys + USB_HID_BASE + (i + 1) * USB_HID_STRIDE) | 2 :
            (h->dma_phys + USB_UHCI_CONTROL_QH) | 2;
        pipe[1] = 1;
    }
    for (unsigned i = 0; i < 1024; i++) words(h, 0)[i] = (h->dma_phys + USB_HID_BASE) | 2;
    usb_barrier();
    usb_write(h, 8, h->dma_phys, 4); usb_write(h, 6, 0, 2);
    usb_write(h, 0x0c, 64, 1); usb_write(h, 2, 0x1f, 2);
    /* Only two ports are defined by the baseline UHCI register block. */
    h->ports = 2;
    h->io.config_write(h->io.cookie, 4, h->pci.command | 5u, 2);
    if ((h->io.config_read(h->io.cookie, 4, 2) & 5) != 5) return -USB_EIO;
    usb_write(h, 0, 0xc1, 2); /* configured, max packet 64, RUN */
    err = usb_wait_register(h, 2, 2, 0x20, 0, 100);
    klog("[usb] uhci schedule controller=%s command=%04x status=%04x frame=%u flbase=%08x result=%d",
         h->owner, usb_read(h, 0, 2), usb_read(h, 2, 2), usb_read(h, 6, 2), usb_read(h, 8, 4), err);
    return err;
}
static void dump_preempt(struct usb_host *h)
{
#ifndef CIUKI_USB_HOST
    fs_lock_drop(&h->transfer_lock);
    kpreempt_point();
    fs_lock_take(&h->transfer_lock);
#else
    (void)h;
#endif
}
void usb_uhci_dump_schedule(struct usb_host *h)
{
    if (h->pci.prog_if || !h->ready || h->schedule_dumped ||
        !(g_boot.flags & CBI_F_DIAG_VERBOSE)) return;
    h->schedule_dumped = true;
    usb_barrier();
    for (unsigned frame = 0; frame < 4; frame++) {
        dump_preempt(h);
        klog("[usb] schedule frame=%u entry=%08x controller=%s flbase=%08x current=%u",
             frame, words(h, 0)[frame], h->owner, usb_read(h, 8, 4), usb_read(h, 6, 2));
    }
    /* The four entries share a chain. Dump its actual horizontal links once,
     * including empty QHs; printing only active pipes could hide a broken link.
     * Validate DMA bounds before dereferencing even a diagnostic pointer. */
    uint32_t link = words(h, 0)[0];
    for (unsigned n = 0; n < USB_DEVICE_MAX * USB_HID_MAX + 3; n++) {
        dump_preempt(h);
        if (link & 1) return;
        uint32_t address = link & ~15u;
        if (!(link & 14) && address == h->dma_phys + USB_TAIL) {
            volatile uint32_t *td = words(h, USB_TAIL);
            klog("[usb] schedule frame=0 td=%08x link=%08x status=%08x token=%08x buffer=%08x controller=%s",
                 address, td[0], td[1], td[2], td[3], h->owner);
            link = td[0];
            continue;
        }
        if ((link & 15) != 2 || address < h->dma_phys + USB_ED ||
            address > h->dma_phys + USB_DMA_BYTES - 16) {
            klog("[usb] schedule frame=0 invalid_link=%08x controller=%s", link, h->owner);
            return;
        }
        unsigned at = address - h->dma_phys;
        volatile uint32_t *qh = words(h, at);
        uint32_t horizontal = qh[0], element = qh[1];
        klog("[usb] schedule frame=0 qh=%08x link=%08x element=%08x controller=%s",
             address, horizontal, element, h->owner);
        if (at >= USB_HID_BASE && at < USB_HID_BASE + USB_DEVICE_MAX * USB_HID_MAX * USB_HID_STRIDE &&
            (at - USB_HID_BASE) % USB_HID_STRIDE == 0) {
            unsigned slot = (at - USB_HID_BASE) / USB_HID_STRIDE;
            struct usb_device *d = &h->devices[slot / USB_HID_MAX];
            struct usb_hid *p = &d->hid[slot % USB_HID_MAX];
            if (p->endpoint) {
                volatile uint32_t *td = words(h, at + 16);
                klog("[usb] schedule frame=0 td=%08x link=%08x status=%08x token=%08x buffer=%08x addr=%u interface=%u state=%u controller=%s",
                     address + 16, td[0], td[1], td[2], td[3], d->address, p->interface, p->pipe_state, h->owner);
            }
        }
        link = horizontal;
    }
    if (!(link & 1))
        klog("[usb] schedule frame=0 invalid_link=%08x reason=chain_limit controller=%s", link, h->owner);
}
static unsigned status(struct usb_host *h, unsigned port)
{
    unsigned v = usb_read(h, 0x10 + 2 * port, 2);
    if (v == 0xffff) return USB_PORT_OVERCURRENT;
    return (v & 1 ? USB_PORT_CONNECTED : 0) | (v & 4 ? USB_PORT_ENABLED : 0) |
           (v & 0x100 ? USB_PORT_LOW : 0) | (v & 0x400 ? USB_PORT_OVERCURRENT : 0) |
           (v & 2 ? USB_PORT_CHANGED : 0);
}
static void port_write(struct usb_host *h, unsigned port, unsigned set, unsigned clear)
{
    unsigned off = 0x10 + 2 * port;
    unsigned v = usb_read(h, off, 2);
    /* CSC/PEC/OCC are W1C; reserved 13-15 must be zero. */
    unsigned written = ((v & ~0xe80au) & ~clear) | set;
    usb_write(h, off, written, 2);
    usb_port_action(h, port, set & 0x200 ? "reset_assert" : clear & 0x200 ? "reset_release" :
                    set & 4 ? "enable" : "disable", v, written, 0);
}
static void ack_changes(struct usb_host *h, unsigned port)
{
    unsigned off = 0x10 + 2 * port, v = usb_read(h, off, 2);
    if (v != 0xffff && (v & 0x80a)) usb_write(h, off, v & ~0xe000u, 2);
}
static int enable(struct usb_host *h, unsigned port)
{
    if (port >= h->ports) return -EINVAL;
    unsigned s = status(h, port);
    if (!(s & USB_PORT_CONNECTED) || (s & USB_PORT_OVERCURRENT)) return -USB_EIO;
    port_write(h, port, 4, 0);
    int err = usb_wait_register(h, 0x10 + 2 * port, 2, 5, 5, 100);
    usb_port_record(h, port, "enable", err);
    return err;
}
static int reset(struct usb_host *h, unsigned port)
{
    if (port >= h->ports) return -EINVAL;
    if ((status(h, port) & USB_PORT_CONNECTED) && usb_port_has_live_child(h, port)) return -USB_EAGAIN;
    port_write(h, port, 0x200, 4);
    usb_port_record(h, port, "reset", 0);
    usb_sleep(h, 50);
    port_write(h, port, 0, 0x200);
    usb_port_record(h, port, "reset", 0);
    usb_sleep(h, 1); /* >= Linux's 10 us release-to-PE delay */
    ack_changes(h, port);
    int err = enable(h, port);
    usb_sleep(h, 10); /* TRSTRCY, with the port explicitly enabled */
    ack_changes(h, port);
    return err;
}
static void disable(struct usb_host *h, unsigned port)
{
    if (port >= h->ports || ((status(h, port) & USB_PORT_CONNECTED) && usb_port_has_live_child(h, port))) return;
    port_write(h, port, 0, 4); usb_port_record(h, port, "disable", 0);
}
static int packet(struct usb_device *d, unsigned endpoint, unsigned pid, unsigned toggle,
                  void *data, unsigned length, bool periodic)
{
    struct usb_host *h = d->host;
    d->packet_trace = (struct usb_packet_trace){.actual=-1};
    if (length > 64 || (d->low_speed && length > 8) || endpoint > 15) return -EINVAL;
    uint64_t begin = h->io.now(h->io.cookie);
    unsigned budget = usb_packet_budget(d, endpoint, periodic, begin);
    if (!budget) { d->packet_trace.result=-USB_ETIMEDOUT; return -USB_ETIMEDOUT; }
    unsigned frame = usb_read(h, 6, 2) & 0x7ff, frames = 0;
    struct usb_packet_trace *trace = &d->packet_trace;
    *trace = (struct usb_packet_trace){.expected=length,.actual=-1,
        .endpoint=endpoint,.pid=pid,.toggle=toggle,.valid=true};
    unsigned queue = endpoint && !periodic ? USB_UHCI_BULK_QH : USB_UHCI_CONTROL_QH;
    volatile uint32_t *td = words(h, USB_TD), *qh = words(h, queue);
    /* One-element breadth-first queue: T=1,Vf=0 returns to the QH's
     * horizontal link after an ACK. IRQ mode also requests completion. */
    td[0] = 1;
    td[1] = (1u << 23) | (h->irq_attached ? 1u << 24 : 0) | (3u << 27) | (d->low_speed ? 1u << 26 : 0) | 0x7ff;
    td[2] = pid | (uint32_t)d->address << 8 | endpoint << 15 | toggle << 19 |
            ((length ? length - 1 : 0x7ffu) << 21);
    td[3] = h->dma_phys + USB_BUFFER;
    if (pid != USB_PID_IN && length) memcpy(h->dma + USB_BUFFER, data, length);
    usb_barrier(); qh[1] = h->dma_phys + USB_TD; usb_barrier();
    /* Count opportunities only after the complete TD is published. */
    frame = usb_read(h, 6, 2) & 0x7ff;
    uint64_t end = begin + budget;
    unsigned tries = budget + 1;
    int result = -USB_ETIMEDOUT;
    for (unsigned i = 0; i < tries; i++) {
        if (!h->ready) {
            trace->frames=trace->poll_frames=frames; trace->elapsed_ms=h->io.now(h->io.cookie)-begin;
            trace->result=trace->retired=-USB_EIO; return -USB_EIO;
        }
        unsigned s = usb_read(h, 2, 2);
        unsigned next = usb_read(h, 6, 2) & 0x7ff;
        frames += (next - frame) & 0x7ff; frame = next;
        trace->hc_status = s;
        if (s & 0x38) {
            usb_barrier(); trace->status=td[1]; trace->token=td[2];
            trace->frames=trace->poll_frames=frames; trace->elapsed_ms=h->io.now(h->io.cookie)-begin;
            trace->result=trace->retired=-USB_EIO;
            usb_host_quarantine(h, "uhci_host_error"); return -USB_EIO;
        }
        usb_barrier(); unsigned v = td[1];
        if (!(v & (1u << 23))) {
            break;
        }
        if (periodic && frames >= 2) { result = -USB_EAGAIN; break; }
        if (usb_expired(h, end)) break;
        usb_sleep(h, 1);
    }
    trace->poll_frames=frames;
    qh[1] = 1; usb_barrier();
    int retired = usb_retire_frames(h, 6, 2, 0x7ff);
    frames += (usb_read(h, 6, 2) - frame) & 0x7ff;
    usb_barrier(); unsigned v = td[1];
    trace->status=v; trace->token=td[2]; trace->frames=frames;
    trace->elapsed_ms=h->io.now(h->io.cookie)-begin; trace->retired=retired;
    if (!d->address)
        klog("[usb] addr0 uhci_packet controller=%s pid=%02x command=%04x status=%04x frame=%u td=%08x retire_result=%d",
             h->owner, pid, usb_read(h, 0, 2), usb_read(h, 2, 2), usb_read(h, 6, 2), td[1], retired);
    if (retired) { trace->result=-USB_EIO; return -USB_EIO; }
    /* An ACK racing schedule removal must not be discarded as a NAK. */
    if (!(v & (1u << 23))) {
        /* Babble also sets STALL. It is not an endpoint-halt handshake. */
        if (v & 0x00360000u) result = -USB_EIO;
        else if (v & (1u << 22)) result = -USB_EPIPE;
        else result = (int)(((v & 0x7ff) + 1) & 0x7ff);
    }
    trace->result=result;
    if (result >= 0) trace->actual=result;
    if (result > (int)length) {
        trace->result=-USB_EIO;
        usb_host_quarantine(h, "uhci_dma_length"); return -USB_EIO;
    }
    if (result >= 0 && pid == USB_PID_IN && result) memcpy(data, h->dma + USB_BUFFER, (unsigned)result);
    return result;
}
_Static_assert(USB_HID_BASE + USB_DEVICE_MAX * USB_HID_MAX * USB_HID_STRIDE <= USB_BULK_TD,
               "bulk TDs overlap interrupt slots");
_Static_assert(USB_BULK_TD + USB_BULK_BATCH * 16 <= USB_BULK_BUFFER &&
               USB_BULK_BUFFER + USB_BULK_BATCH * 64 <= USB_DMA_BYTES, "bulk DMA bounds");
static int bulk_status(uint32_t v, unsigned expected)
{
    if (v & (1u<<23)) return -USB_ETIMEDOUT;
    if (v & 0x00360000u) return -USB_EIO;
    if (v & (1u<<22)) return -USB_EPIPE;
    unsigned n=((v & 0x7ff)+1)&0x7ff;
    return n>expected ? -USB_EIO : (int)n;
}
/* Depth-first batches permit several ACKs per frame. SPD leaves a short IN
 * at the QH element so no following TD can consume the BOT status wrapper.
 * The reservation owns this window; HID uses independent DMA throughout. */
static int bulk(struct usb_device *d, bool in, void *data, unsigned bytes, unsigned *actual)
{
    struct usb_host *h=d->host; struct usb_storage *s=&d->storage;
    unsigned mps=in ? s->in_packet : s->out_packet, ep=in ? s->in : s->out;
    uint8_t *toggle=in ? &s->in_toggle : &s->out_toggle;
    volatile uint32_t *qh=words(h,USB_UHCI_BULK_QH);
    while (*actual<bytes) {
        unsigned count=(bytes-*actual+mps-1)/mps;
        if (count>USB_BULK_BATCH) count=USB_BULK_BATCH;
        uint64_t begin=h->io.now(h->io.cookie);
        unsigned budget=usb_packet_budget(d,ep,false,begin);
        unsigned port=h->ops->port_status(h,d->root_port);
        bool connected=h->ready && !h->quarantined && d->present && !d->quarantined &&
            (port&(USB_PORT_CONNECTED|USB_PORT_ENABLED|USB_PORT_CHANGED|USB_PORT_OVERCURRENT))==
            (USB_PORT_CONNECTED|USB_PORT_ENABLED);
        if (!budget || !connected) {
            int result=connected ? -USB_ETIMEDOUT : -USB_EIO;
            unsigned n=bytes-*actual; if (n>mps) n=mps;
            d->packet_trace=(struct usb_packet_trace){.expected=n,.actual=-1,
                .result=result,.endpoint=ep,.pid=in ? USB_PID_IN : USB_PID_OUT,.toggle=*toggle};
            struct usb_storage_trace *t=&s->trace;
            t->packet_index=t->packet_count; t->packet_offset=*actual;
            if (t->packet_count<USB_BULK_TRACE_LIMIT) t->packets[t->packet_count]=d->packet_trace;
            t->packet_count++;
            return result;
        }
        unsigned offset=*actual;
        for (unsigned i=0;i<count;i++) {
            unsigned n=bytes-offset; if (n>mps) n=mps;
            volatile uint32_t *td=words(h,USB_BULK_TD+i*16);
            td[0]=i+1<count ? (h->dma_phys+USB_BULK_TD+(i+1)*16)|4 : 1;
            td[1]=(1u<<23)|(h->irq_attached ? 1u<<24 : 0)|(3u<<27)|(in ? 1u<<29 : 0)|0x7ff;
            td[2]=(in ? USB_PID_IN : USB_PID_OUT)|(unsigned)d->address<<8|ep<<15|
                  (*toggle^(i&1))<<19|(n-1)<<21;
            td[3]=h->dma_phys+USB_BULK_BUFFER+i*64;
            if (!in) memcpy(h->dma+USB_BULK_BUFFER+i*64,(uint8_t *)data+offset,n);
            offset+=n;
        }
        usb_barrier(); qh[1]=h->dma_phys+USB_BULK_TD; usb_barrier();
        unsigned frame=usb_read(h,6,2)&0x7ff, frames=0, progress=0, hc=0;
        int failure=0;
        /* ACK progress can renew the binding wait, but never the aggregate
         * deadline. The finite poll count also bounds a frozen clock. */
        for (unsigned polls=0;polls<=USB_BULK_TIMEOUT_MS;polls++) {
            if (!h->ready) return -USB_EIO; /* quarantined DMA remains pinned */
            hc=usb_read(h,2,2);
            if (hc&0x38) { usb_host_quarantine(h,"uhci_host_error"); return -USB_EIO; }
            unsigned next=usb_read(h,6,2)&0x7ff;
            frames+=(next-frame)&0x7ff; frame=next;
            usb_barrier();
            unsigned completed=progress;
            while (completed<count) {
                volatile uint32_t *td=words(h,USB_BULK_TD+completed*16);
                unsigned expected=(((td[2]>>21)&0x7ff)+1)&0x7ff;
                if (bulk_status(td[1],expected)!=(int)expected) break;
                completed++;
            }
            if (completed>progress && s->binding) {
                uint64_t end=h->io.now(h->io.cookie)+USB_BIND_BULK_MS;
                if ((int64_t)(end-(s->bind_deadline-USB_BIND_FINISH_MS))>0)
                    end=s->bind_deadline-USB_BIND_FINISH_MS;
                s->bulk_deadline=end; s->trace.bulk_deadline=end;
            }
            progress=completed;
            unsigned port=h->ops->port_status(h,d->root_port);
            if (!d->present || d->quarantined ||
                (port&(USB_PORT_CONNECTED|USB_PORT_ENABLED|USB_PORT_CHANGED|USB_PORT_OVERCURRENT))!=
                (USB_PORT_CONNECTED|USB_PORT_ENABLED)) { failure=-USB_EIO; break; }
            if (progress==count || !(words(h,USB_BULK_TD+progress*16)[1]&(1u<<23))) break;
            if (usb_expired(h,s->bulk_deadline)) break;
            usb_sleep(h,1);
        }
        qh[1]=1; usb_barrier();
        int retired=usb_retire_frames(h,6,2,0x7ff);
        unsigned all_frames=frames+((usb_read(h,6,2)-frame)&0x7ff);
        if (s->binding) { s->bind_poll_frames+=frames; s->bind_retire_frames+=all_frames-frames; }
        if (retired) return -USB_EIO;
        for (unsigned i=0;i<count;i++) {
            volatile uint32_t *td=words(h,USB_BULK_TD+i*16);
            usb_barrier(); unsigned v=td[1], n=(((td[2]>>21)&0x7ff)+1)&0x7ff;
            int result=bulk_status(v,n);
            d->packet_trace=(struct usb_packet_trace){.status=v,.token=td[2],.hc_status=hc,
                .expected=n,.actual=result>=0 ? result : -1,.result=result,.valid=true,
                .endpoint=ep,.pid=in ? USB_PID_IN : USB_PID_OUT,.toggle=*toggle,
                .frames=all_frames,.poll_frames=frames,.elapsed_ms=h->io.now(h->io.cookie)-begin};
            struct usb_storage_trace *t=&s->trace;
            t->packet_index=t->packet_count; t->packet_offset=*actual;
            if (t->packet_count<USB_BULK_TRACE_LIMIT) t->packets[t->packet_count]=d->packet_trace;
            t->packet_count++; if (s->binding) s->bind_packets++;
            if (!(v&(1u<<23)) && (((v&0x7ff)+1)&0x7ff)>n) {
                usb_host_quarantine(h,"uhci_dma_length"); return -USB_EIO;
            }
            if (result<0) return failure ? failure : result;
            if (!in && result!=(int)n) return -USB_EPROTO;
            if (in && result) memcpy((uint8_t *)data+*actual,h->dma+USB_BULK_BUFFER+i*64,(unsigned)result);
            *actual+=(unsigned)result; *toggle^=1;
            if ((unsigned)result<n) return failure ? failure : 0;
        }
        if (failure) return failure;
    }
    return 0;
}
const struct usb_hc_ops usb_uhci_ops = {
    .start = start, .port_status = status, .port_reset = reset,
    .port_enable = enable, .port_disable = disable, .packet = packet, .port_ack_changes = ack_changes,
    .interrupt = usb_interrupt, .interrupt_stop = usb_interrupt_stop,
    .bulk = bulk,
};
