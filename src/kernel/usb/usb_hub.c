/* One external USB 1.x hub level. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/usb.h>
static int feature(struct usb_device *d, unsigned port, unsigned value, bool set)
{ return usb_control(d, 0x23, set ? 3 : 1, (uint16_t)value, (uint16_t)(port + 1), 0, 0); }
static int status(struct usb_device *d, unsigned port, unsigned *s, unsigned *change)
{
    uint8_t b[4]; int err = usb_control(d, 0xa3, 0, 0, (uint16_t)(port + 1), b, 4);
    if (err != 4) return err < 0 ? err : -USB_EPROTO;
    *s = b[0] | (unsigned)b[1] << 8; *change = b[2] | (unsigned)b[3] << 8; return 0;
}
int usb_hub_attach(struct usb_device *d)
{
    uint8_t b[16]; int err = usb_control(d, 0xa0, 6, 0x2900, 0, b, sizeof(b));
    if (err < 9 || b[1] != 0x29 || b[0] < 9 || b[0] > err || !b[2] || b[2] > USB_PORT_MAX || (b[3] & 3) == 3) return -USB_EPROTO;
    d->hub_ports = b[2];
    for (unsigned p = 0; p < d->hub_ports; p++) {
        err = feature(d, p, 8, true); if (err) return err;
    }
    d->host->io.sleep(d->host->io.cookie, (unsigned)b[5] * 2 + 100);
    return 0;
}
void usb_hub_scan(struct usb_device *d)
{
    struct usb_host *h = d->host;
    for (unsigned p = 0; p < d->hub_ports && h->ready; p++) {
        if (d->hub_blocked[p]) continue;
        unsigned s, c; int err = status(d, p, &s, &c);
        if (err) { usb_device_quarantine(d, "hub_status"); return; }
        if (s & 8) { usb_device_quarantine(d, "hub_overcurrent"); return; }
        if (c & 1) {
            if (d->hub_child[p]) {
                usb_detach(&h->devices[d->hub_child[p]-1], "hub_port_changed"); d->hub_child[p] = 0;
            }
            err = feature(d, p, 16, false); if (err) goto failed;
        }
        if (!(s & 1)) {
            if (d->hub_child[p]) { usb_detach(&h->devices[d->hub_child[p]-1], "disconnected"); d->hub_child[p] = 0; }
            continue;
        }
        if (d->hub_child[p]) continue;
        h->io.sleep(h->io.cookie, 100);
        err = status(d, p, &s, &c); if (err) goto failed;
        if (!(s & 1)) continue;
        err = feature(d, p, 4, true); if (err) goto failed;
        uint64_t end = h->io.now(h->io.cookie) + 200;
        for (unsigned n = 0; n < 200; n++) {
            err = status(d, p, &s, &c); if (err) goto failed;
            if (!(s & 1)) break;
            if ((c & 0x10) && !(s & 0x10)) break;
            if (usb_expired(h, end)) break;
            h->io.sleep(h->io.cookie, 1);
        }
        if ((s & 3) != 3 || !(c & 0x10) || (s & 0x10)) { err = -USB_ETIMEDOUT; goto failed; }
        err = feature(d, p, 20, false); if (err) goto failed;
        h->io.sleep(h->io.cookie, 10);
        err = usb_enumerate(h, d->root_port, (unsigned)(d - h->devices) + 1, p, !!(s & 0x200));
        if (err < 0) goto failed;
        d->hub_child[p] = (uint8_t)err;
        continue;
failed:
        usb_device_quarantine(d, "hub_port_transfer_or_timeout"); return;
    }
}
