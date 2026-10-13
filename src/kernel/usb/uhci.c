/* UHCI 1.1 schedule and root hub. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/usb.h>
static volatile uint32_t *words(struct usb_host *h, unsigned at)
{ return (volatile uint32_t *)(h->dma + at); }
static int start(struct usb_host *h)
{
    /* C0: write W1C statuses, leave all SMI/PIRQ enables cleared. */
    h->io.config_write(h->io.cookie, 0xc0, 0x8f00, 2);
    if (h->io.config_read(h->io.cookie, 0xc0, 2) & ~0xdf40u) return -USB_EIO;
    h->owned = true;
    h->io.config_write(h->io.cookie, 4, (h->pci.command | 1u) & ~4u, 2);
    if (h->io.config_read(h->io.cookie, 4, 2) & 4) return -USB_EIO;
    usb_write(h, 4, 0, 2); usb_write(h, 0, 0, 2);
    int err = usb_wait_register(h, 2, 2, 0x20, 0x20, 100);
    if (err) return err;
    usb_write(h, 0, 2, 2);
    err = usb_wait_register(h, 0, 2, 2, 0, 100); if (err) return err;
    memset(h->dma, 0, USB_DMA_BYTES);
    volatile uint32_t *qh = words(h, USB_ED);
    qh[0] = qh[1] = 1;
    for (unsigned i = 0; i < 1024; i++) words(h, 0)[i] = (h->dma_phys + USB_ED) | 2;
    usb_barrier();
    usb_write(h, 8, h->dma_phys, 4); usb_write(h, 6, 0, 2);
    usb_write(h, 0x0c, 64, 1); usb_write(h, 2, 0x1f, 2);
    /* Only two ports are defined by the baseline UHCI register block. */
    h->ports = 2;
    h->io.config_write(h->io.cookie, 4, h->pci.command | 5u, 2);
    if ((h->io.config_read(h->io.cookie, 4, 2) & 5) != 5) return -USB_EIO;
    usb_write(h, 0, 0xc1, 2); /* configured, max packet 64, RUN */
    return usb_wait_register(h, 2, 2, 0x20, 0, 100);
}
static unsigned status(struct usb_host *h, unsigned port)
{
    unsigned v = usb_read(h, 0x10 + 2 * port, 2);
    if (v == 0xffff) return USB_PORT_OVERCURRENT;
    return (v & 1 ? USB_PORT_CONNECTED : 0) | (v & 4 ? USB_PORT_ENABLED : 0) |
           (v & 0x100 ? USB_PORT_LOW : 0) | (v & 2 ? USB_PORT_CHANGED : 0);
}
static void port_write(struct usb_host *h, unsigned port, unsigned set, unsigned clear)
{
    unsigned off = 0x10 + 2 * port;
    unsigned v = usb_read(h, off, 2);
    /* CSC/PEC are W1C, never echoed during reset/enable writes. */
    usb_write(h, off, ((v & ~0x0au) & ~clear) | set, 2);
}
static int reset(struct usb_host *h, unsigned port)
{
    if (port >= h->ports) return -EINVAL;
    port_write(h, port, 0x200, 4);
    h->io.sleep(h->io.cookie, 50);
    port_write(h, port, 0, 0x200);
    h->io.sleep(h->io.cookie, 10);
    port_write(h, port, 4, 0);
    int err = usb_wait_register(h, 0x10 + 2 * port, 2, 5, 5, 100);
    usb_write(h, 0x10 + 2 * port, (usb_read(h, 0x10 + 2 * port, 2) & ~0x0au) | 0x0a, 2);
    return err;
}
static void disable(struct usb_host *h, unsigned port) { port_write(h, port, 0, 4); }
static int packet(struct usb_device *d, unsigned endpoint, unsigned pid, unsigned toggle,
                  void *data, unsigned length, bool periodic)
{
    struct usb_host *h = d->host;
    if (length > 64 || (d->low_speed && length > 8) || endpoint > 15) return -EINVAL;
    volatile uint32_t *td = words(h, USB_TD), *qh = words(h, USB_ED);
    td[0] = 1;
    td[1] = (1u << 23) | (3u << 27) | (d->low_speed ? 1u << 26 : 0) | 0x7ff;
    td[2] = pid | (uint32_t)d->address << 8 | endpoint << 15 | toggle << 19 |
            ((length ? length - 1 : 0x7ffu) << 21);
    td[3] = h->dma_phys + USB_BUFFER;
    if (pid != USB_PID_IN && length) memcpy(h->dma + USB_BUFFER, data, length);
    usb_barrier(); qh[1] = h->dma_phys + USB_TD; usb_barrier();
    uint64_t end = h->io.now(h->io.cookie) + (periodic ? 2 : USB_TIMEOUT_MS);
    unsigned tries = periodic ? 3 : USB_TIMEOUT_MS + 1;
    int result = -USB_ETIMEDOUT;
    for (unsigned i = 0; i < tries; i++) {
        unsigned s = usb_read(h, 2, 2);
        if (s & 0x38) { usb_host_quarantine(h, "uhci_host_error"); return -USB_EIO; }
        usb_barrier(); unsigned v = td[1];
        if (!(v & (1u << 23))) {
            break;
        }
        if (usb_expired(h, end)) { if (periodic) result = -USB_EAGAIN; break; }
        h->io.sleep(h->io.cookie, 1);
    }
    qh[1] = 1; usb_barrier();
    if (usb_retire_frames(h, 6, 2, 0x7ff)) return -USB_EIO;
    /* An ACK racing schedule removal must not be discarded as a NAK. */
    usb_barrier(); unsigned v = td[1];
    if (!(v & (1u << 23))) {
        if (v & (1u << 22)) result = -USB_EPIPE;
        else if (v & 0x00360000u) result = -USB_EIO;
        else result = (int)(((v & 0x7ff) + 1) & 0x7ff);
    }
    if (result > (int)length) { usb_host_quarantine(h, "uhci_dma_length"); return -USB_EIO; }
    if (result >= 0 && pid == USB_PID_IN && result) memcpy(data, h->dma + USB_BUFFER, (unsigned)result);
    return result;
}
const struct usb_hc_ops usb_uhci_ops = {start, status, reset, disable, packet};
