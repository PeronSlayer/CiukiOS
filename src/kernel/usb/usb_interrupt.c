/* Nonblocking private HID slots, independent of control/BOT ownership.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/usb.h>
_Static_assert(USB_HID_BASE + USB_DEVICE_MAX * USB_HID_MAX * USB_HID_STRIDE <= USB_DMA_BYTES,
               "interrupt slots exceed pinned DMA");
static unsigned slot(struct usb_device *d, struct usb_hid *p)
{
    return USB_HID_BASE + ((unsigned)(d - d->host->devices) * USB_HID_MAX +
                          (unsigned)(p - d->hid)) * USB_HID_STRIDE;
}
void usb_interrupt_stop(struct usb_device *d, struct usb_hid *p)
{
    if (!p->pipe_state) return;
    volatile uint32_t *ed = (volatile uint32_t *)(d->host->dma + slot(d, p));
    if (d->host->pci.prog_if == 0) ed[1] = 1;
    else ed[0] |= 1u << 14;
    usb_barrier();
    /* Device records/slots are monotonic until reboot. A detached TD is
     * never reassigned, including a late ACK or uncertain retirement. */
    p->pipe_state = 0;
}
int usb_interrupt(struct usb_device *d, struct usb_hid *p, void *data, unsigned length)
{
    struct usb_host *h = d->host;
    bool uhci = h->pci.prog_if == 0;
    unsigned mask = uhci ? 0x7ff : 0xffff;
    unsigned frame = usb_read(h, uhci ? 6 : 0x3c, uhci ? 2 : 4) & mask;
    unsigned status = usb_read(h, uhci ? 2 : 0x0c, uhci ? 2 : 4);
    uint64_t tick = h->io.now(h->io.cookie);
    unsigned at = slot(d, p), buffer = at + 32;
    volatile uint32_t *ed = (volatile uint32_t *)(h->dma + at);
    volatile uint32_t *td = ed + 4;
    struct usb_packet_trace *t = &p->trace;
    t->valid = false;
    if (status & (uhci ? 0x38u : 0x10u)) {
        usb_host_quarantine(h, uhci ? "uhci_host_error" : "ohci_host_error");
        return -USB_EIO;
    }
    if (!p->pipe_state) {
        if (!usb_expired(h, p->next_poll)) return -USB_EAGAIN;
        if (!length || length > 8 || length > p->packet) return -USB_EPROTO;
        *t = (struct usb_packet_trace){.expected=length,.actual=-1,
            .endpoint=p->endpoint,.pid=USB_PID_IN,.toggle=p->toggle,
            .result=-USB_ETIMEDOUT};
        if (uhci) {
            /* T=1,Vf=0 terminates this private queue, not the frame. The
             * controller resumes at the QH horizontal link (UHCI 3.4).
             * IOC requests device completion only on an attached IRQ line. */
            td[0] = 1;
            td[1] = (1u << 23) | (h->irq_attached ? 1u << 24 : 0) | (3u << 27) | (d->low_speed ? 1u << 26 : 0) | 0x7ff;
            td[2] = USB_PID_IN | (uint32_t)d->address << 8 | (unsigned)p->endpoint << 15 |
                    (unsigned)p->toggle << 19 | (length - 1) << 21;
            td[3] = h->dma_phys + buffer;
            usb_barrier(); ed[1] = h->dma_phys + at + 16;
        } else {
            /* DI=0 flushes the done chain each frame, before two-frame
             * retirement permits this TD to be reused. The dummy is shared. */
            td[0] = (0xfu << 28) | ((2u | p->toggle) << 24) | (2u << 19) | (1u << 18);
            td[1] = h->dma_phys + buffer;
            td[2] = h->dma_phys + USB_TAIL;
            td[3] = h->dma_phys + buffer + length - 1;
            ed[0] = d->address | (unsigned)p->endpoint << 7 | (d->low_speed ? 1u << 13 : 0) |
                    (unsigned)p->packet << 16 | (1u << 14);
            ed[1] = h->dma_phys + USB_TAIL; ed[2] = h->dma_phys + at + 16;
            usb_barrier(); ed[0] &= ~(1u << 14);
            usb_write(h, 4, usb_read(h, 4, 4) | 4u, 4);
        }
        usb_barrier();
        p->pipe_state = 1; p->pipe_frame = (uint16_t)frame;
        p->pipe_begin = p->pending_at = tick; p->observed_at = 0;
        p->progress_at = tick;
        p->next_poll = tick + p->interval;
        return -USB_EAGAIN;
    }
    usb_barrier();
    bool done = uhci ? !(td[1] & (1u << 23)) :
        (td[0] >> 28) < 14 && ((ed[2] & ~15u) == ed[1] || (ed[2] & 1));
    t->hc_status = status;
    if (p->pipe_state == 1) {
        unsigned elapsed = (frame - p->pipe_frame) & mask;
        t->poll_frames += elapsed;
        if (elapsed) p->progress_at = tick; /* watchdog follows HC progress, not idle duration */
        p->pipe_frame = (uint16_t)frame;
        if (done) p->observed_at = tick;
        else p->pending_at = tick;
        if (!done) {
            if (tick - p->progress_at >= USB_PERIODIC_WATCHDOG_MS) {
                usb_host_quarantine(h, "frame_clock_stalled"); return -USB_EIO;
            }
            /* A NAK is flow control. Do not unlink or rewrite this TD: the
             * controller keeps polling it until a report or a retired error.
             * Publish bounded diagnostic snapshots without claiming retirement. */
            if (t->poll_frames >= 2) {
                t->status = uhci ? td[1] : td[0]; t->token = uhci ? td[2] : ed[0];
                t->frames = t->poll_frames; t->elapsed_ms = tick - p->pipe_begin;
                t->result = t->retired = -USB_EAGAIN; t->valid = true;
            }
            return -USB_EAGAIN;
        }
        if (uhci) ed[1] = 1;
        else ed[0] |= 1u << 14;
        usb_barrier();
        p->pipe_state = 2; p->retire_frame = (uint16_t)frame; p->retire_begin = tick;
        return -USB_EAGAIN;
    }
    unsigned retired_frames = (frame - p->retire_frame) & mask;
    if (retired_frames < 2 || retired_frames >= mask / 2) {
        if (tick - p->retire_begin >= USB_PERIODIC_WATCHDOG_MS) {
            usb_host_quarantine(h, "frame_clock_stalled"); return -USB_EIO;
        }
        return -USB_EAGAIN;
    }
    t->frames = t->poll_frames + retired_frames;
    t->elapsed_ms = tick - p->pipe_begin; t->retired = 0;
    t->status = uhci ? td[1] : td[0]; t->token = uhci ? td[2] : ed[0];
    int result = t->result;
    if (done) {
        if (!p->observed_at) p->observed_at = tick; /* ACK racing unlink */
        if (uhci) {
            if (td[1] & 0x00360000u) result = -USB_EIO;
            else if (td[1] & (1u << 22)) result = -USB_EPIPE;
            else result = (int)(((td[1] & 0x7ff) + 1) & 0x7ff);
        } else {
            unsigned cc = td[0] >> 28, cur = td[1];
            if (cc == 4) result = -USB_EPIPE;
            else if (cc && cc != 9) result = -USB_EIO;
            else if (cur && (cur < h->dma_phys + buffer || cur > h->dma_phys + buffer + length)) {
                usb_host_quarantine(h, "ohci_dma_pointer"); return -USB_EIO;
            } else result = cur ? (int)(cur - h->dma_phys - buffer) : (int)length;
        }
    }
    if (result > (int)length) {
        usb_host_quarantine(h, "uhci_dma_length"); return -USB_EIO;
    }
    t->result = result; t->valid = true;
    if (result >= 0) { t->actual = result; memcpy(data, h->dma + buffer, (unsigned)result); }
    p->pipe_state = 0;
    return result;
}
