/* UHCI 1.1 schedule and root hub. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/usb.h>
#ifndef CIUKI_USB_HOST
#include <ciuki/cpu.h>
#include <ciuki/sync.h>
#endif
_Static_assert(USB_UHCI_BULK_QH + 16 <= USB_TD, "UHCI queues overlap the general TD");
_Static_assert(USB_UHCI_TERM_QH % 16 == 0 && USB_UHCI_TERM_QH >= USB_TAIL + 16 && USB_UHCI_TERM_QH + 16 <= USB_BUFFER, "UHCI terminating QH placement");
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
     * only one owns the reserved TD/buffer at a time. The terminating QH
     * closes the fx-17 reclamation loop only while a bulk stage runs. */
    volatile uint32_t *control = words(h, USB_UHCI_CONTROL_QH);
    volatile uint32_t *bulk = words(h, USB_UHCI_BULK_QH);
    control[0] = (h->dma_phys + USB_UHCI_BULK_QH) | 2; control[1] = 1;
    /* Keep the inactive tail silent: IOC here would interrupt every frame,
     * even with no transaction. Actual transfer TDs request completion. */
    volatile uint32_t *term = words(h, USB_UHCI_TERM_QH);
    bulk[0] = (h->dma_phys + USB_UHCI_TERM_QH) | 2; bulk[1] = 1;
    term[0] = 1; term[1] = h->dma_phys + USB_TAIL;
    words(h, USB_TAIL)[0] = 1;
    words(h, USB_TAIL)[1] = 0;
    /* fx-21: the pseudo TD never executes (inactive, UHCI 1.1 section
     * 3.4.1.3 step 2). Its token is still Linux uhci_start()'s term_td, a
     * null-length IN to address 127, never address 0, which is where every
     * device being enumerated answers. */
    words(h, USB_TAIL)[2] = (0x7ffu << 21) | (0x7fu << 8) | USB_PID_IN;
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
        klog_diag("[usb] schedule frame=%u entry=%08x controller=%s flbase=%08x current=%u",
             frame, words(h, 0)[frame], h->owner, usb_read(h, 8, 4), usb_read(h, 6, 2));
    }
    /* The four entries share a chain. Dump its actual horizontal links once,
     * including empty QHs; printing only active pipes could hide a broken link.
     * Validate DMA bounds before dereferencing even a diagnostic pointer. */
    uint32_t link = words(h, 0)[0];
    for (unsigned n = 0; n < USB_DEVICE_MAX * USB_HID_MAX + 4; n++) {
        dump_preempt(h);
        if (link & 1) return;
        uint32_t address = link & ~15u;
        if (!(link & 14) && address == h->dma_phys + USB_TAIL) {
            volatile uint32_t *td = words(h, USB_TAIL);
            klog_diag("[usb] schedule frame=0 td=%08x link=%08x status=%08x token=%08x buffer=%08x controller=%s",
                 address, td[0], td[1], td[2], td[3], h->owner);
            link = td[0];
            continue;
        }
        if ((link & 15) != 2 || address < h->dma_phys + USB_ED ||
            address > h->dma_phys + USB_DMA_BYTES - 16) {
            klog_diag("[usb] schedule frame=0 invalid_link=%08x controller=%s", link, h->owner);
            return;
        }
        unsigned at = address - h->dma_phys;
        volatile uint32_t *qh = words(h, at);
        uint32_t horizontal = qh[0], element = qh[1];
        klog_diag("[usb] schedule frame=0 qh=%08x link=%08x element=%08x controller=%s",
             address, horizontal, element, h->owner);
        if (at >= USB_HID_BASE && at < USB_HID_BASE + USB_DEVICE_MAX * USB_HID_MAX * USB_HID_STRIDE &&
            (at - USB_HID_BASE) % USB_HID_STRIDE == 0) {
            unsigned slot = (at - USB_HID_BASE) / USB_HID_STRIDE;
            struct usb_device *d = &h->devices[slot / USB_HID_MAX];
            struct usb_hid *p = &d->hid[slot % USB_HID_MAX];
            if (p->endpoint) {
                volatile uint32_t *td = words(h, at + 16);
                klog_diag("[usb] schedule frame=0 td=%08x link=%08x status=%08x token=%08x buffer=%08x addr=%u interface=%u state=%u controller=%s",
                     address + 16, td[0], td[1], td[2], td[3], d->address, p->interface, p->pipe_state, h->owner);
            }
        }
        if (at == USB_UHCI_TERM_QH && element == h->dma_phys + USB_TAIL) {
            volatile uint32_t *td = words(h, USB_TAIL);
            klog_diag("[usb] schedule frame=0 td=%08x link=%08x status=%08x token=%08x buffer=%08x controller=%s",
                 element, td[0], td[1], td[2], td[3], h->owner);
        }
        link = horizontal;
    }
    if (!(link & 1))
        klog_diag("[usb] schedule frame=0 invalid_link=%08x reason=chain_limit controller=%s", link, h->owner);
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
/* fx-21: a UHCI root port is enabled by software only (UHCI 1.1 section
 * 2.1.7 bit 2; ICH3-M 11.2.7). Between the end of the reset and PE the port
 * forwards no SOF, and a device that sees more than 3 ms of idle bus enters
 * Suspend (USB 2.0 section 7.1.7.6), here in the Default state just after
 * its reset. Hub ports are Enabled when the reset ends (USB 2.0 section
 * 7.1.7.5, chapter 11.5) and EHCI root ports within 2 ms (EHCI 1.0 section
 * 2.3.9, PORTSC Port Reset), so devices never see that gap elsewhere.
 * Linux uhci_check_ports() clears PR, waits 10 us, clears CSC/PEC and sets
 * PE under the HCD lock; FreeBSD uhci_portreset(): "This delay needs to be
 * exactly 100us, else some USB devices fail to attach!". The T23 trial of
 * f441c5bd spent 9 ms in a 1 ms task sleep there (ACPI scan and APM BIOS
 * start-up running), and its stick then ACKed every SETUP and NAKed every
 * data stage at address 0. Release and enable therefore share one IRQ-off
 * section with a microsecond delay and never cross a task sleep. The frame
 * counter, which an SMI cannot stop, measures the window for the record.
 * Evidence and open points: docs/design/usb-contract.md, fx-21. */
#define UHCI_RELEASE_DELAY_US 10u
struct release_window { unsigned before, released, ready, enabled, frames; bool connected; };
static void release_enable(struct usb_host *h, unsigned port, struct release_window *w)
{
    unsigned off = 0x10 + 2 * port;
#ifndef CIUKI_USB_HOST
    uint32_t flags = irq_save();
#endif
    unsigned start = usb_read(h, 6, 2) & 0x7ff;
    unsigned v = usb_read(h, off, 2);
    w->before = v;
    /* CSC/PEC/OCC are W1C and not echoed here; reserved 13-15 stay zero. */
    w->released = v & ~0xea0au;
    usb_write(h, off, w->released, 2);
#ifndef CIUKI_USB_HOST
    /* Only before TSC calibration: an ICH I/O read takes about 1 us. */
    if (udelay(UHCI_RELEASE_DELAY_US))
        for (unsigned i = 0; i < 4 * UHCI_RELEASE_DELAY_US; i++) (void)usb_read(h, off, 2);
#endif
    v = usb_read(h, off, 2);
    if (v & 0x80a) usb_write(h, off, v & ~0xe200u, 2); /* the reset's CSC/PEC */
    v = w->ready = usb_read(h, off, 2);
    /* PE only for a present device without overcurrent, as enable(). */
    w->connected = v != 0xffff && (v & 1) && !(v & 0x400);
    w->enabled = (v & ~0xea0au) | (w->connected ? 4u : 0u);
    if (w->connected) usb_write(h, off, w->enabled, 2);
    w->frames = ((usb_read(h, 6, 2) & 0x7ff) - start) & 0x7ff;
#ifndef CIUKI_USB_HOST
    irq_restore(flags);
#endif
}
static int reset(struct usb_host *h, unsigned port)
{
    if (port >= h->ports) return -EINVAL;
    if ((status(h, port) & USB_PORT_CONNECTED) && usb_port_has_live_child(h, port)) return -USB_EAGAIN;
    uint64_t asserted = h->io.now(h->io.cookie);
    port_write(h, port, 0x200, 4);
    usb_port_record(h, port, "reset", 0);
    usb_sleep(h, 50);
    struct release_window w;
    release_enable(h, port, &w);
    uint64_t released = h->io.now(h->io.cookie);
    usb_port_action(h, port, "reset_release", w.before, w.released, 0);
    usb_port_record(h, port, "reset", 0);
    int err = -USB_EIO;
    if (w.connected) {
        usb_port_action(h, port, "enable", w.ready, w.enabled, 0);
        /* PE may follow a transaction in progress (UHCI 1.1 section 2.1.7). */
        err = usb_wait_register(h, 0x10 + 2 * port, 2, 5, 5, 100);
        usb_port_record(h, port, "enable", err);
    }
    klog("[usb] port_reset port=%u controller=%s assert_ms=%llu release_to_enable_frames=%u connected=%u result=%d",
         port + 1, h->owner, (unsigned long long)(released - asserted), w.frames, w.connected, err);
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
        if (!(v & (1u << 23)) &&
            (!endpoint || periodic || (v & 0x00760000u) || (qh[1] & 1))) {
            break;
        }
        if (periodic && frames >= 2) { result = -USB_EAGAIN; break; }
        if (usb_expired(h, end)) break;
        usb_poll_pause(h, 1);
    }
    trace->poll_frames=frames;
    /* Status precedes QH writeback. Only an HC-written terminating element
     * proves a successful bulk TD cannot still write the shared queue. */
    usb_barrier();
    bool completed = endpoint && !periodic && !(td[1] & 0x00f60000u) && (qh[1] & 1);
    qh[1] = 1; usb_barrier();
    int retired = completed ? 0 : usb_retire_frames(h, 6, 2, 0x7ff);
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
/* Depth-first chains permit several ACKs per frame. SPD leaves a short IN
 * at the QH element so no following TD can consume the BOT status wrapper.
 * The reservation owns this window; HID uses independent DMA throughout.
 *
 * f1-96: a data stage longer than the window streams through it as a ring
 * (UHCI 1.1 §3.4.1.3, depth-first queue advance). Slot k%USB_BULK_BATCH
 * holds packet k; every TD links depth-first to the next slot except the
 * final packet, which terminates the chain and requests IOC. The HC stops
 * at a slot whose TD is not yet re-armed (inactive: the queue does not
 * advance, as with the Linux uhci-q.c dummy TD) and resumes in a later
 * frame once the driver re-arms it. Completed slots are harvested in order
 * and re-armed only after the following packet completed, so the HC has
 * finished that TD's QH writeback before its link can change. */
static void bulk_arm(struct usb_device *d, bool in, unsigned ep, unsigned toggle,
                     const uint8_t *data, unsigned k, unsigned n, bool last)
{
    struct usb_host *h=d->host;
    unsigned slot=k%USB_BULK_BATCH;
    volatile uint32_t *td=words(h,USB_BULK_TD+slot*16);
    td[0]=last ? 1 : (h->dma_phys+USB_BULK_TD+((slot+1)%USB_BULK_BATCH)*16)|4;
    td[2]=(in ? USB_PID_IN : USB_PID_OUT)|(unsigned)d->address<<8|ep<<15|toggle<<19|(n-1)<<21;
    td[3]=h->dma_phys+USB_BULK_BUFFER+slot*64;
    if (!in) memcpy(h->dma+USB_BULK_BUFFER+slot*64,data,n);
    /* Active last: the HC sees either an inactive slot or the whole TD. */
    usb_barrier();
    td[1]=(1u<<23)|(h->irq_attached && last ? 1u<<24 : 0)|(3u<<27)|(in ? 1u<<29 : 0)|0x7ff;
}
/* Record and consume packet k: 1 full ACK, 0 short (transfer ends),
 * negative error. The TD is inactive or already unlinked and retired. */
static int bulk_take(struct usb_device *d, bool in, void *data, unsigned *actual, unsigned k,
                     unsigned hc, unsigned frames, unsigned poll_frames, uint64_t begin, int failure)
{
    struct usb_host *h=d->host; struct usb_storage *s=&d->storage;
    unsigned slot=k%USB_BULK_BATCH, ep=in ? s->in : s->out;
    uint8_t *toggle=in ? &s->in_toggle : &s->out_toggle;
    volatile uint32_t *td=words(h,USB_BULK_TD+slot*16);
    usb_barrier(); unsigned v=td[1], n=(((td[2]>>21)&0x7ff)+1)&0x7ff;
    int result=bulk_status(v,n);
    d->packet_trace=(struct usb_packet_trace){.status=v,.token=td[2],.hc_status=hc,
        .expected=n,.actual=result>=0 ? result : -1,.result=result,.valid=true,
        .endpoint=ep,.pid=in ? USB_PID_IN : USB_PID_OUT,.toggle=*toggle,
        .frames=frames,.poll_frames=poll_frames,.elapsed_ms=h->io.now(h->io.cookie)-begin};
    struct usb_storage_trace *t=&s->trace;
    t->packet_index=t->packet_count; t->packet_offset=*actual;
    if (t->packet_count<USB_BULK_TRACE_LIMIT) t->packets[t->packet_count]=d->packet_trace;
    t->packet_count++; if (s->binding) s->bind_packets++;
    if (!(v&(1u<<23)) && (((v&0x7ff)+1)&0x7ff)>n) {
        usb_host_quarantine(h,"uhci_dma_length"); return -USB_EIO;
    }
    if (result<0) return failure ? failure : result;
    if (!in && result!=(int)n) return -USB_EPROTO;
    if (in && result) memcpy((uint8_t *)data+*actual,h->dma+USB_BULK_BUFFER+slot*64,(unsigned)result);
    *actual+=(unsigned)result; *toggle^=1;
    if ((unsigned)result<n) return failure ? failure : 0;
    return 1;
}
static uint32_t bulk_td_address(struct usb_host *h, unsigned k)
{ return h->dma_phys+USB_BULK_TD+(k%USB_BULK_BATCH)*16; }
static bool bulk_full(struct usb_host *h, unsigned k)
{
    volatile uint32_t *td=words(h,USB_BULK_TD+(k%USB_BULK_BATCH)*16);
    unsigned expected=(((td[2]>>21)&0x7ff)+1)&0x7ff;
    return bulk_status(td[1],expected)==(int)expected;
}
/* f1-96b: early Intel erratum (Linux b761d9d867bc "UHCI: Work around old
 * Intel bug", uhci_advance_check): the QH element sometimes stays on a TD
 * that completed successfully and the queue never moves again. Packet k is
 * the newest full ACK; its slot is not re-armed before packet k+1 completes,
 * so its link is exactly what the HC should have written. The HC writes the
 * element only after executing an active element TD (UHCI 1.1 section
 * 3.4.1.3 steps 2/9, ICH3-M 5.16.2.2 steps 2/11), and this element is
 * inactive, so the write cannot race a writeback. */
static bool bulk_advance(struct usb_device *d, unsigned ep, unsigned k, unsigned frames)
{
    struct usb_host *h=d->host; struct usb_storage *s=&d->storage;
    volatile uint32_t *qh=words(h,USB_UHCI_BULK_QH);
    volatile uint32_t *td=words(h,USB_BULK_TD+(k%USB_BULK_BATCH)*16);
    usb_barrier();
    uint32_t element=qh[1];
    if ((element&3) || (element&~15u)!=bulk_td_address(h,k) || (td[1]&(1u<<23))) return false;
    uint32_t link=td[0];
    qh[1]=link; usb_barrier();
    s->advance_fixes++;
    if (s->advance_fixes<=4 || !(s->advance_fixes&63))
        klog("[usb] uhci_advance controller=%s addr=%u ep=%u tag=%u packet=%u element=%08x link=%08x frames=%u fixes=%u",
             h->owner,d->address,ep,s->trace.tag,k,element,link,frames,s->advance_fixes);
    return true;
}
/* fx-17: full-speed bandwidth reclamation (FSBR). Without it the HC visits
 * the bulk QH once per frame: the first NAK (a flash stick fetching its next
 * page) ends bulk work until the next 1 ms frame, which held the T23's
 * 64 KiB READ(10)s to about 250 KB/s. While a bulk stage runs, the
 * terminating QH links back to the control QH, so the HC retries the NAKed
 * TD within the same frame until the frame's bandwidth is spent (UHCI 1.1
 * section 3.4.3 / 2.3.1 SOFMOD reclamation; Linux uhci-q.c uhci_fsbr_on and
 * uhci_fsbr_off, used for every full-speed bulk queue). It is switched off
 * when the stage ends, so an idle schedule never loops. */
static int bulk_stage(struct usb_device *d, bool in, void *data, unsigned bytes, unsigned *actual);
static int bulk(struct usb_device *d, bool in, void *data, unsigned bytes, unsigned *actual)
{
    struct usb_host *h=d->host;
    volatile uint32_t *term=words(h,USB_UHCI_TERM_QH);
    usb_barrier(); term[0]=(h->dma_phys+USB_UHCI_CONTROL_QH)|2; usb_barrier();
    int e=bulk_stage(d,in,data,bytes,actual);
    usb_barrier(); term[0]=1; usb_barrier();
    return e;
}
static int bulk_stage(struct usb_device *d, bool in, void *data, unsigned bytes, unsigned *actual)
{
    struct usb_host *h=d->host; struct usb_storage *s=&d->storage;
    unsigned mps=in ? s->in_packet : s->out_packet, ep=in ? s->in : s->out;
    uint8_t *toggle=in ? &s->in_toggle : &s->out_toggle;
    volatile uint32_t *qh=words(h,USB_UHCI_BULK_QH);
    bool relinked=false; /* f1-96b: one same-chain retry per data stage */
    while (*actual<bytes) {
        unsigned base=*actual, total=(bytes-base+mps-1)/mps;
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
        unsigned first=*toggle, armed=0, done=0;
        for (;armed<total && armed<USB_BULK_BATCH;armed++) {
            unsigned at=base+armed*mps, n=bytes-at<mps ? bytes-at : mps;
            bulk_arm(d,in,ep,first^(armed&1),(const uint8_t *)data+at,armed,n,armed+1==total);
        }
        usb_barrier(); qh[1]=h->dma_phys+USB_BULK_TD; usb_barrier();
        unsigned frame=usb_read(h,6,2)&0x7ff, frames=0, progress=0, hc=0, moved_frames=0;
        uint64_t moved_at=begin;
        int failure=0, refill=1;
        bool completed_batch=false, tail_advanced=false;
        /* ACK progress can renew the binding wait, but never the aggregate
         * deadline. The finite poll count also bounds a frozen clock; each
         * completed packet earns one more poll. */
        for (unsigned polls=0;polls<=USB_BULK_TIMEOUT_MS+progress;polls++) {
            if (!h->ready) return -USB_EIO; /* quarantined DMA remains pinned */
            hc=usb_read(h,2,2);
            if (hc&0x38) { usb_host_quarantine(h,"uhci_host_error"); return -USB_EIO; }
            unsigned next=usb_read(h,6,2)&0x7ff;
            frames+=(next-frame)&0x7ff; frame=next;
            usb_barrier();
            /* f1-96b: `stopped` is the status of the TD that ended the walk.
             * A walk that reached `armed` (the HC drained every armed TD and
             * waits at a slot not yet re-armed) leaves it active: the refill
             * below arms more TDs, and the stale status of the last full ACK
             * must not end the stage (T23 5a10eef4: td=388007ff never
             * executed, stop=packet after 44 ms, result=-110). */
            unsigned completed=progress, stopped=1u<<23;
            while (completed<armed) {
                volatile uint32_t *td=words(h,USB_BULK_TD+(completed%USB_BULK_BATCH)*16);
                unsigned expected=(((td[2]>>21)&0x7ff)+1)&0x7ff, v=td[1];
                if (bulk_status(v,expected)!=(int)expected) { stopped=v; break; }
                completed++;
            }
            uint64_t now=h->io.now(h->io.cookie);
            if (completed>progress) {
                moved_at=now; moved_frames=frames;
                if (s->binding) {
                    uint64_t end=now+USB_BIND_BULK_MS;
                    if ((int64_t)(end-(s->bind_deadline-USB_BIND_FINISH_MS))>0)
                        end=s->bind_deadline-USB_BIND_FINISH_MS;
                    s->bulk_deadline=end; s->trace.bulk_deadline=end;
                }
            }
            progress=completed;
            /* Refill the ring behind the HC. Data leaves a slot before its
             * reuse; the newest completed slot stays until its successor
             * completes (its QH writeback may still be in flight). */
            if (armed<total && done<progress) {
                for (;done<progress && refill>0;done++)
                    refill=bulk_take(d,in,data,actual,done,hc,frames,frames,begin,0);
                if (refill<=0) break; /* a proven full ACK cannot end here */
                for (;armed<total && armed+1<progress+USB_BULK_BATCH;armed++) {
                    unsigned at=base+armed*mps, n=bytes-at<mps ? bytes-at : mps;
                    bulk_arm(d,in,ep,first^(armed&1),(const uint8_t *)data+at,armed,n,armed+1==total);
                }
            }
            unsigned port=h->ops->port_status(h,d->root_port);
            if (!d->present || d->quarantined ||
                (port&(USB_PORT_CONNECTED|USB_PORT_ENABLED|USB_PORT_CHANGED|USB_PORT_OVERCURRENT))!=
                (USB_PORT_CONNECTED|USB_PORT_ENABLED)) { failure=-USB_EIO; break; }
            /* The HC can finish the current TD (and notify us) between the
             * prefix walk and this decision. Re-reading Active here could
             * mistake a full ACK for a short/error and unlink the unfinished
             * suffix. Decide using the exact snapshot that stopped the walk. */
            if (progress==total) {
                usb_barrier();
                /* A terminating element written by hand is no HC proof:
                 * the post-loop unlink still retires the queue. */
                if (qh[1]&1) { completed_batch=!tail_advanced; break; }
            } else if (progress<armed && !(stopped&(1u<<23))) break;
            if (progress && frames-moved_frames>=USB_UHCI_ADVANCE_FRAMES &&
                bulk_advance(d,ep,progress-1,frames)) {
                tail_advanced=progress==total;
                moved_at=now; moved_frames=frames;
            } else if (!relinked && progress<armed && now-moved_at>=USB_BULK_STALL_MS) {
                /* f1-96b: the same chain is retried once. A next TD that
                 * the HC never executed (no NAK, error or length written)
                 * or an element off this chain means the HC is not running
                 * the queue; a NAKing device keeps its bounded wait. The QH
                 * is unlinked and retired first, so the relink cannot race
                 * a writeback: same TDs, toggles and buffers, never a CBW. */
                volatile uint32_t *td=words(h,USB_BULK_TD+(progress%USB_BULK_BATCH)*16);
                usb_barrier();
                uint32_t element=qh[1], v=td[1];
                bool off=(element&3) || (element&~15u)!=bulk_td_address(h,progress);
                if (off || (v&0x18ff07ffu)==((3u<<27)|(1u<<23)|0x7ffu)) {
                    relinked=true; s->relinks++;
                    qh[1]=1; usb_barrier();
                    int retired=usb_retire_frames(h,6,2,0x7ff);
                    next=usb_read(h,6,2)&0x7ff; frames+=(next-frame)&0x7ff; frame=next;
                    if (retired) return -USB_EIO;
                    usb_barrier();
                    const char *result="relinked";
                    unsigned c=progress;
                    if (!(qh[1]&1)) result="advancing"; /* an ACK raced the unlink */
                    else {
                        while (c<armed && bulk_full(h,c)) c++;
                        if (c==total) result="complete";
                        else if (c<armed && !(words(h,USB_BULK_TD+(c%USB_BULK_BATCH)*16)[1]&(1u<<23)))
                            result="stopped"; /* short or error: the walk ends the stage */
                        else { qh[1]=bulk_td_address(h,c); usb_barrier(); }
                    }
                    klog("[usb-storage] data_retry addr=%u tag=%u packet=%u offset=%u element=%08x td=%08x stalled_ms=%llu frames=%u relink=%08x result=%s",
                         d->address,s->trace.tag,base/mps+progress,base+progress*mps,element,v,
                         (unsigned long long)(now-moved_at),frames,bulk_td_address(h,c),result);
                    moved_at=h->io.now(h->io.cookie); moved_frames=frames;
                }
            }
            if (usb_expired(h,s->bulk_deadline)) break;
            usb_poll_pause(h,1);
        }
        qh[1]=1; usb_barrier();
        int retired=completed_batch ? 0 : usb_retire_frames(h,6,2,0x7ff);
        unsigned all_frames=frames+((usb_read(h,6,2)-frame)&0x7ff);
        if (s->binding) { s->bind_poll_frames+=frames; s->bind_retire_frames+=all_frames-frames; }
        if (retired) return -USB_EIO;
        if (refill<=0) return refill ? refill : -USB_EPROTO;
        for (;done<armed;done++) {
            int e=bulk_take(d,in,data,actual,done,hc,all_frames,frames,begin,failure);
            if (e<=0) return e;
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

/* UHCI 1.1: global suspend retains FLBASEADD, FRNUM and endpoint state.
 * Resume signalling lasts at least 20 ms before RUN is restored. */
int usb_uhci_suspend(struct usb_host *h)
{
    h->sleep_regs[0] = usb_read(h, 0, 2);
    h->sleep_regs[1] = usb_read(h, 4, 2);
    h->sleep_regs[2] = usb_read(h, 6, 2);
    h->sleep_regs[3] = usb_read(h, 8, 4);
    h->sleep_regs[4] = usb_read(h, 0x0c, 1);
    usb_write(h, 4, 0, 2); usb_write(h, 0, h->sleep_regs[0] & ~1u, 2);
    int e = usb_wait_register(h, 2, 2, 0x20, 0x20, 100);
    if (!e) usb_write(h, 0, (h->sleep_regs[0] & ~1u) | 8u, 2);
    return e;
}
int usb_uhci_resume(struct usb_host *h)
{
    usb_write(h, 4, 0, 2);
    usb_write(h, 8, h->sleep_regs[3], 4); usb_write(h, 6, h->sleep_regs[2], 2);
    usb_write(h, 0x0c, h->sleep_regs[4], 1);
    usb_write(h, 0, (h->sleep_regs[0] & ~9u) | 0x10u, 2);
    usb_sleep(h, 20); usb_write(h, 0, h->sleep_regs[0] & ~0x19u, 2);
    usb_sleep(h, 10); usb_write(h, 0, h->sleep_regs[0], 2);
    int e = usb_wait_register(h, 2, 2, 0x20, 0, 100);
    if (!e) usb_write(h, 4, h->sleep_regs[1], 2);
    return e;
}
