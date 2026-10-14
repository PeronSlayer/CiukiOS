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
static void record(struct usb_device *d, unsigned port, unsigned s, unsigned c,
                   const char *event, int result)
{
    struct usb_host *h = d->host;
    struct usb_port_log *log = &d->hub_log[port];
    log->pending = event; log->result = result;
    if (!usb_expired(h, log->next_record)) return;
    bool quarantined = d->hub_blocked[port] && !strncmp(event, "connect", 8);
    klog("[usb] hub=%u port=%u status=%04x change=%04x event=%s reason=%s controller=%s root=%u tick=%llu result=%d",
         d->address, port + 1, s, c, event, quarantined ? "quarantined" : "state", h->owner, d->root_port + 1,
         h->io.now(h->io.cookie), result);
    log->next_record = h->io.now(h->io.cookie) + 1000; log->pending = 0;
    if (quarantined) log->quarantine_reported = true;
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
    klog("[usb] hub=%u ports=%u power=%u controller=%s event=ready",
         d->address, d->hub_ports, b[3] & 3, d->host->owner);
    return 0;
}
void usb_hub_scan(struct usb_device *d)
{
    struct usb_host *h = d->host;
    bool first = !d->hub_scanned;
    d->hub_scanned = true;
    for (unsigned p = 0; p < d->hub_ports && h->ready; p++) {
        unsigned s = 0, c = 0; int err = status(d, p, &s, &c);
        if (err) { record(d, p, s, c, "status", err); usb_device_quarantine(d, "hub_status"); return; }
        unsigned previous = d->hub_status[p];
        struct usb_port_log *log = &d->hub_log[p];
        struct usb_device *child = d->hub_child[p] ? &h->devices[d->hub_child[p]-1] : 0;
        if (!d->hub_blocked[p] && child && child->storage.bound &&
            child->storage.block.quarantined && !child->storage.ejected) {
            d->hub_blocked[p] = true;
            err = feature(d, p, 1, false); if (err) goto failed;
        }
        if (d->hub_blocked[p]) {
            for (unsigned bit = 0; bit < 5; bit++) if (c & (1u << bit)) {
                err = feature(d, p, 16 + bit, false); if (err) goto failed;
            }
            if (s & 1) {
                if (!log->quarantine_reported) record(d, p, s, c, "connect", 0);
            } else {
                if (previous & 1) record(d, p, s, c, "disconnect", 0);
                log->quarantine_reported = false;
                if (child) { usb_detach(child, "disconnected"); d->hub_child[p] = 0; }
            }
            if (log->pending) record(d, p, s, c, log->pending, log->result);
            d->hub_status[p] = (uint16_t)s;
            continue;
        }
        if (log->pending) record(d, p, s, c, log->pending, log->result);
        if (first || ((s ^ previous) & 1) || (c & 1))
            record(d, p, s, c, s & 1 ? "connect" : "disconnect", 0);
        if ((s ^ previous) & 2) record(d, p, s, c, "enable", 0);
        d->hub_status[p] = (uint16_t)s;
        if (s & 8) { record(d, p, s, c, "overcurrent", -USB_EIO); usb_device_quarantine(d, "hub_overcurrent"); return; }
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
        if (!(s & 1)) { record(d, p, s, c, "disconnect", 0); d->hub_status[p] = (uint16_t)s; continue; }
        /* Discard a stale completion before issuing this reset. */
        if (c & 0x10) { err = feature(d, p, 20, false); if (err) goto failed; }
        err = feature(d, p, 4, true); if (err) goto failed;
        record(d, p, s, c, "reset_start", 0);
        uint64_t end = h->io.now(h->io.cookie) + 200;
        for (unsigned n = 0; n < 200; n++) {
            err = status(d, p, &s, &c); if (err) goto failed;
            if (!(s & 1)) break;
            if ((c & 0x10) && !(s & 0x10)) break;
            if (usb_expired(h, end)) break;
            h->io.sleep(h->io.cookie, 1);
        }
        if (!(s & 1)) { record(d, p, s, c, "disconnect", 0); d->hub_status[p] = (uint16_t)s; continue; }
        if ((s & 3) != 3 || !(c & 0x10) || (s & 0x10)) { err = -USB_ETIMEDOUT; goto failed; }
        record(d, p, s, c, "reset_complete", 0);
        record(d, p, s, c, "enable", 0);
        err = feature(d, p, 20, false); if (err) goto failed;
        if (c & 2) { err = feature(d, p, 17, false); if (err) goto failed; }
        h->io.sleep(h->io.cookie, 10);
        err = usb_enumerate(h, d->root_port, (unsigned)(d - h->devices) + 1, p, !!(s & 0x200));
        if (err < 0) goto failed;
        d->hub_child[p] = (uint8_t)err;
        d->hub_status[p] = (uint16_t)s;
        child = &h->devices[err - 1];
        record(d, p, s, c, "enumerate", child->quarantined ? -USB_EIO : 0);
        continue;
failed:
        record(d, p, s, c, "failed", err);
        usb_device_quarantine(d, "hub_port_transfer_or_timeout"); return;
    }
}
