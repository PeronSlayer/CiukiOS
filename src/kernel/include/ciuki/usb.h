/* Bounded USB 1.x host/HID model. See docs/design/usb-contract.md.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_USB_H
#define CIUKI_USB_H
#include <ciuki/registry.h>
#include "../../fs/blkdev.h"
#define USB_HOST_MAX 4u
#define USB_DEVICE_MAX 16u
#define USB_PORT_MAX 8u
#define USB_HID_MAX 4u
#define USB_CONFIG_MAX 512u
#define USB_DMA_BYTES 12288u
#define USB_PACKET_MAX 64u
#define USB_TIMEOUT_MS 500u
#define USB_PERIODIC_WATCHDOG_MS 20u
#define USB_HID_TRACE_LIMIT 16u
#define USB_PORT_ACTION_TRACE_LIMIT 32u
#define USB_BULK_TIMEOUT_MS 10000u
#define USB_BIND_TIMEOUT_MS 20000u
#define USB_BIND_BULK_MS 3000u
#define USB_BIND_FINISH_MS 100u /* retirement and namespace publication */
#define USB_BULK_TRACE_LIMIT 8u
#define USB_EIO 5
#define USB_EAGAIN 11
#define USB_EPIPE 32
#define USB_EPROTO 71
#define USB_ETIMEDOUT 110
#define USB_ED 4096u
#define USB_UHCI_CONTROL_QH USB_ED
#define USB_UHCI_BULK_QH (USB_ED + 16u)
#define USB_TD 4128u
#define USB_TAIL 4160u
#define USB_BUFFER 4352u
/* Private interrupt slots fit all device/interface records in the same
 * pinned controller allocation. Boot reports need at most eight bytes. */
#define USB_HID_BASE 4608u
#define USB_HID_STRIDE 48u
#define USB_HID_LATENCY_LIMIT 20u
#define USB_BULK_TD 7680u
#define USB_BULK_BUFFER 8192u
#define USB_BULK_BATCH 32u
#define USB_HID_DESCRIPTOR_MAX 512u
#define USB_MOUSE_REPORTS 8u
struct usb_mouse_field { uint16_t bit; uint8_t size; };
struct usb_mouse_report {
    struct usb_mouse_field x, y, wheel, pan, buttons[3];
    uint16_t bits;
    uint8_t id, button_count;
};
struct usb_mouse_input {
    int32_t x, y, wheel, pan;
    uint8_t buttons; /* public left/right/middle bitmap */
};
#define USB_PID_SETUP 0x2du
#define USB_PID_IN 0x69u
#define USB_PID_OUT 0xe1u
#define USB_PORT_CONNECTED 1u
#define USB_PORT_ENABLED 2u
#define USB_PORT_LOW 4u
#define USB_PORT_OVERCURRENT 8u
#define USB_PORT_CHANGED 16u
struct usb_host;
struct usb_device;
/* All callbacks are pinned. sleep must yield with IF=1, even in a fixture.
 * Config width is 2 or 4; register width is 1, 2 or 4. */
struct usb_io {
    void *cookie;
    uint32_t (*read)(void *, unsigned offset, unsigned width);
    void (*write)(void *, unsigned offset, uint32_t, unsigned width);
    uint32_t (*config_read)(void *, unsigned offset, unsigned width);
    void (*config_write)(void *, unsigned offset, uint32_t, unsigned width);
    uint64_t (*now)(void *);
    void (*sleep)(void *, unsigned ms);
};
/* Snapshot after schedule retirement, before another packet reuses DMA.
 * actual=-1 means no known ACK. Raw TD fields remain useful on errors. */
struct usb_packet_trace {
    uint32_t status, token, hc_status;
    unsigned expected, frames, poll_frames;
    int actual, result, retired;
    uint64_t elapsed_ms;
    uint8_t endpoint, pid, toggle;
    bool valid;
};
struct usb_hid {
    uint8_t interface, protocol, endpoint, packet, interval, toggle;
    uint8_t previous[8], leds;
    uint8_t report_records, key_records, poll_records, latency_records;
    uint8_t pipe_state; /* 0 idle, 1 linked, 2 retiring */
    uint16_t repeat_key, report_length, pipe_frame, retire_frame;
    uint64_t next_poll, repeat_at, pipe_begin, retire_begin, progress_at;
    uint64_t pending_at, observed_at, latency_window, latency_max;
    unsigned latency_reports;
    struct usb_packet_trace trace; /* separate from the general/BOT slot */
    bool active, wheel, recovery_pending, recovery_tried, port_retry;
    int recovery_result;
    struct usb_mouse_report mouse[USB_MOUSE_REPORTS];
    uint8_t mouse_reports;
    bool report_protocol, report_ids;
};
struct usb_storage_trace {
    struct usb_packet_trace packet;
    struct usb_packet_trace packets[USB_BULK_TRACE_LIMIT];
    unsigned packet_count, packet_index, packet_offset;
    uint64_t bulk_started_ms, bulk_deadline, bulk_elapsed_ms, bind_remaining_ms;
    const char *bulk_stop;
    const char *stage;
    uint32_t tag, port_status;
    unsigned expected, actual, data_expected, data_actual;
    int result;
    uint32_t csw_signature, csw_tag, residue;
    uint8_t command, csw_status;
    bool csw_available, data_stalled;
};
/* One transparent SCSI/BOT interface, LUN zero. Records and block views are
 * retained until reboot; an uncertain command never becomes a new medium. */
struct usb_storage {
    struct blkdev block;
    uint32_t tag;
    uint64_t next_check;
    uint64_t bulk_deadline; /* only live while usb_bulk calls the packet engine */
    uint64_t bind_deadline; /* MAX LUN through namespace publication, including recovery */
    uint64_t bind_started_ms, bind_stage_started_ms;
    const char *bind_stage;
    struct usb_storage_trace trace;
    uint8_t interface, in, out, in_packet, out_packet, in_toggle, out_toggle;
    uint8_t sense_key, asc, ascq;
    bool bound, online, published, ejected, write_protected, fault_logged, quarantine_logged;
    bool binding, bind_reserved, bind_stage_active;
    uint32_t bind_read_requests, bind_read_sectors, bind_read_max;
    uint32_t bind_packets, bind_poll_frames, bind_retire_frames;
};
/* Coalesce suppressed events; root and external-hub ports use the same budget. */
struct usb_port_log {
    uint64_t next_record;
    const char *pending;
    int result;
    bool quarantine_reported;
    unsigned operations;
    uint64_t next_operation;
};
struct usb_device {
    struct usb_host *host;
    gen_t generation;
    const char *reason;
    const char *hid_fault; /* general owner performs hub control/quarantine */
    uint16_t vendor, product;
    uint8_t address, ep0, class_code, root_port, parent, port, hub_ports, config;
    bool present, configured, quarantined, low_speed;
    struct usb_hid hid[USB_HID_MAX];
    struct usb_storage storage;
    struct usb_packet_trace packet_trace;
    bool hub_blocked[USB_PORT_MAX];
    uint8_t hub_child[USB_PORT_MAX]; /* index+1; zero = not enumerated */
    uint16_t hub_status[USB_PORT_MAX]; /* last observed class port status */
    struct usb_port_log hub_log[USB_PORT_MAX];
    bool hub_scanned;
};
struct usb_hc_ops {
    int (*start)(struct usb_host *);
    unsigned (*port_status)(struct usb_host *, unsigned);
    int (*port_reset)(struct usb_host *, unsigned);
    int (*port_enable)(struct usb_host *, unsigned); /* no device reset */
    void (*port_disable)(struct usb_host *, unsigned);
    int (*packet)(struct usb_device *, unsigned endpoint, unsigned pid,
                  unsigned toggle, void *, unsigned bytes, bool periodic);
    void (*port_ack_changes)(struct usb_host *, unsigned);
    /* Nonblocking: EAGAIN can mean pending, or a retired NAK (trace.valid).
     * The caller holds transfer_lock; these callbacks never sleep. */
    int (*interrupt)(struct usb_device *, struct usb_hid *, void *, unsigned);
    void (*interrupt_stop)(struct usb_device *, struct usb_hid *);
    int (*bulk)(struct usb_device *, bool, void *, unsigned, unsigned *);
};
struct usb_host {
    struct pci_func pci;
    struct usb_io io;
    const struct usb_hc_ops *ops;
    uint8_t *dma;
    uint32_t dma_phys, base;
    void *mmio;
    gen_t generation;
    int claims[3];
    gen_t claim_gen[3];
    char owner[24];
    const char *reason;
    unsigned ports, device_count, next_address;
    bool root_blocked[USB_PORT_MAX];
    uint8_t root_child[USB_PORT_MAX];
    unsigned root_status[USB_PORT_MAX]; /* last observed normalized state */
    struct usb_port_log root_log[USB_PORT_MAX];
    bool attempted, ready, quarantined, touched, owned, busy;
    bool scanned, schedule_dumped;
    unsigned scan_count;
    uint64_t next_scan, scan_window;
    fs_lock transfer_lock; /* short DMA/state mutex; always free across sleeps */
    struct usb_device devices[USB_DEVICE_MAX];
};
extern const struct usb_hc_ops usb_uhci_ops, usb_ohci_ops;
static inline void usb_barrier(void) { __asm__ volatile("" ::: "memory"); }
/* Bulk NAK never renews a deadline. Binding ACK progress may renew the bulk
 * wait, always capped by the aggregate bind limit. Published I/O retains its
 * whole-stage bound. Poll counts still bound a frozen software clock. */
static inline unsigned usb_packet_budget(struct usb_device *d, unsigned ep, bool periodic,
                                         uint64_t now)
{
    /* Interrupt opportunities follow the HC clock, not the scheduler's
     * software clock. Packet engines stop after two linked HC frames;
     * this is only the fail-closed watchdog, including a frozen clock. */
    if (periodic) return USB_PERIODIC_WATCHDOG_MS;
    unsigned budget = USB_TIMEOUT_MS;
    if (ep && d->storage.bulk_deadline) {
        int64_t left = (int64_t)(d->storage.bulk_deadline - now);
        if (left <= 0) return 0;
        budget = (uint64_t)left < USB_BULK_TIMEOUT_MS ? (unsigned)left : USB_BULK_TIMEOUT_MS;
    }
    if (d->storage.binding) {
        int64_t left = (int64_t)(d->storage.bind_deadline - now);
        /* Leave time for the unlinked TD's two-frame retirement. */
        if (left <= USB_BIND_FINISH_MS) return 0;
        left -= USB_BIND_FINISH_MS;
        if ((uint64_t)left < budget) budget = (unsigned)left;
    }
    return budget;
}
uint32_t usb_read(struct usb_host *, unsigned, unsigned);
void usb_write(struct usb_host *, unsigned, uint32_t, unsigned);
bool usb_port_record(struct usb_host *, unsigned port, const char *event, int result);
void usb_port_action(struct usb_host *, unsigned, const char *, unsigned before, unsigned written, int);
bool usb_port_has_live_child(struct usb_host *, unsigned);
bool usb_expired(struct usb_host *, uint64_t deadline);
int usb_wait_register(struct usb_host *, unsigned, unsigned, uint32_t mask, uint32_t value, unsigned ms);
int usb_retire_frames(struct usb_host *, unsigned offset, unsigned width, unsigned mask);
void usb_host_quarantine(struct usb_host *, const char *);
void usb_device_quarantine(struct usb_device *, const char *);
void usb_detach(struct usb_device *, const char *);
/* One owner thread calls attach/control/step. Caller supplies pinned, aligned
 * contiguous DMA pages; attachment takes their registry lease before DMA. */
int usb_attach(struct usb_host *, const struct pci_func *, const struct usb_io *, uint8_t *, uint32_t);
int usb_control(struct usb_device *, uint8_t type, uint8_t request, uint16_t value,
                uint16_t index, void *, unsigned length);
int usb_enumerate(struct usb_host *, unsigned root, unsigned parent, unsigned port, bool low);
int usb_parse_config(struct usb_device *, const uint8_t *, unsigned);
void usb_step(struct usb_host *);
void usb_host_take(struct usb_host *); /* reserves serialized general/BOT slot */
bool usb_host_take_until(struct usb_host *, uint64_t deadline);
void usb_host_drop(struct usb_host *);
void usb_sleep(struct usb_host *, unsigned); /* caller's reservation survives */
void usb_hid_service(struct usb_host *); /* mutex held, no general transfers */
void usb_uhci_dump_schedule(struct usb_host *); /* once, after enumeration; mutex held */
int usb_hub_attach(struct usb_device *);
void usb_hub_scan(struct usb_device *);
int usb_hid_attach(struct usb_device *, struct usb_hid *, unsigned report_length);
int usb_hid_report(struct usb_device *, struct usb_hid *, const uint8_t *, unsigned);
void usb_hid_poll(struct usb_device *, struct usb_hid *);
void usb_hid_release(struct usb_device *, struct usb_hid *);
void usb_hid_outputs(struct usb_device *); /* general owner only */
void usb_hid_recover(struct usb_device *); /* general owner only */
int usb_interrupt(struct usb_device *, struct usb_hid *, void *, unsigned);
int usb_hub_reset(struct usb_device *, unsigned port);
void usb_interrupt_stop(struct usb_device *, struct usb_hid *);
int usb_bulk(struct usb_device *, bool in, void *, unsigned, unsigned *actual);
int usb_storage_attach(struct usb_device *); /* no host lock/reservation held */
void usb_storage_detach(struct usb_device *); /* host lock held, no namespace I/O */
struct storage;
int usb_storage_eject(struct storage *, unsigned drive, uint32_t generation);
void usb_storage_service(struct usb_host *); /* host lock NOT held */
bool usb_mouse_wheel_layout(const uint8_t *, unsigned);
bool usb_mouse_parse(struct usb_hid *, const uint8_t *, unsigned);
/* 1 mouse report, 0 unrelated ID, negative malformed report. No state/log I/O. */
int usb_mouse_decode(const struct usb_hid *, const uint8_t *, unsigned, struct usb_mouse_input *);
void usb_init(void);
bool usb_input_ready(void);
unsigned usb_count(void);
const struct usb_host *usb_get(unsigned);
#endif
