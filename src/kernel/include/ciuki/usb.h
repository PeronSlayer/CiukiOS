/* Bounded USB host/HID model. See docs/design/usb-contract.md.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_USB_H
#define CIUKI_USB_H
#include <ciuki/registry.h>
#include <ciuki/hid.h>
#include "../../fs/blkdev.h"
#define USB_HOST_MAX 4u
#define USB_DEVICE_MAX 16u
#define USB_PORT_MAX 8u
#define USB_HID_MAX 4u
#define USB_CONFIG_MAX 512u
#define USB_DMA_BYTES 24576u /* f1-104b: six pinned pages, 64-byte HID payloads */
#define USB_PACKET_MAX 64u
#define USB_TIMEOUT_MS 500u
/* fx-21b: a control message to a device that is not yet configured (the
 * enumeration requests up to SET_CONFIGURATION) is bounded as a whole by
 * Linux's USB_CTRL_GET_TIMEOUT/USB_CTRL_SET_TIMEOUT, 5000 ms
 * (include/linux/usb.h; hub.c initial_descriptor_timeout), instead of
 * USB_TIMEOUT_MS per stage. Configured devices keep USB_TIMEOUT_MS. */
#define USB_CTRL_TIMEOUT_MS 5000u
#define USB_PERIODIC_WATCHDOG_MS 20u
#define USB_HID_FALLBACK_MS 4u /* only unrouteable/revoked or proven lost IRQ */
#define USB_IRQ_GRACE_MS 2u /* exceeds EHCI's configured 1 ms ITC */
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
#define USB_ENOTSUP 95
#define USB_ETIMEDOUT 110
#define USB_ED 4096u
#define USB_UHCI_CONTROL_QH USB_ED
#define USB_UHCI_BULK_QH (USB_ED + 16u)
#define USB_TD 4128u
#define USB_TAIL 4160u
/* fx-17: UHCI terminating QH (Linux uhci-hcd skel_term_qh). Its element is
 * the inactive USB_TAIL TD (Intel PIIX errata workaround, as in Linux); its
 * horizontal link closes the full-speed bandwidth-reclamation loop back to
 * the control QH while a bulk stage runs, and terminates otherwise. */
#define USB_UHCI_TERM_QH (USB_TAIL + 16u)
#define USB_BUFFER 4352u
/* Private interrupt slots fit all device/interface records in the same
 * pinned controller allocation: 16-byte QH/ED, 16-byte TD, 64-byte payload. */
#define USB_HID_BASE 4608u
#define USB_HID_STRIDE 96u
#define USB_OHCI_DONE_HEADS (USB_DEVICE_MAX * USB_HID_MAX + 1u)
#define USB_HID_LATENCY_LIMIT 20u
/* f1-96: UHCI bulk data stages stream through a ring of USB_BULK_BATCH TDs
 * (about seven full-speed frames of 64-byte packets); completed slots are
 * re-armed while the controller executes the rest. One 64-byte buffer per
 * slot. A phase of at most USB_BULK_BATCH packets is a single chain. */
#define USB_BULK_TD (USB_HID_BASE + USB_DEVICE_MAX * USB_HID_MAX * USB_HID_STRIDE)
#define USB_BULK_BUFFER (USB_BULK_TD + USB_BULK_BATCH * 16u)
#define USB_BULK_BATCH 128u
/* f1-96: BOT READ(10)/WRITE(10) transfer limit. Windows limits USB mass
 * storage to 128 sectors and Linux to 240 by default; our block requests
 * are at most BLKDEV_READ_SECTORS. A failed multi-sector command steps the
 * device down the f1-96b ladder (128, 8, then one sector per command). */
#define USB_STORAGE_MAX_SECTORS 128u
/* f1-96b: recovery ladder of a failed multi-sector READ(10): 128 -> 8 -> 1
 * sectors per command, each step after a successful BOT Reset Recovery. */
#define USB_STORAGE_FALLBACK_SECTORS 8u
/* f1-96b: the Bulk-Only Mass Storage Reset class request waits up to 5 s for
 * its status stage (Linux usb_stor_reset_common, 5*HZ); other control
 * packets keep USB_TIMEOUT_MS. */
#define USB_STORAGE_RESET_MS 5000u
/* f1-96b: UHCI bulk data stage without progress. Early Intel controllers
 * sometimes leave the QH element on a TD that completed successfully (Linux
 * b761d9d867bc, uhci_advance_check); the HC writes the element right after
 * the TD status (UHCI 1.1 section 3.4.1.3 steps 6/9, ICH3-M 5.16.2.2 steps
 * 10/11), so an element still on that TD after USB_UHCI_ADVANCE_FRAMES frame
 * boundaries is stuck and is advanced by hand. A stage without progress for
 * USB_BULK_STALL_MS (Linux QH_WAIT_TIMEOUT) whose next TD was never executed
 * or is off the QH element is unlinked, retired and relinked once. */
#define USB_UHCI_ADVANCE_FRAMES 4u
#define USB_BULK_STALL_MS 200u
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
#define USB_PORT_HIGH 32u
#define USB_PORT_COMPANION 64u
#define USB_EHCI_HEAD 4096u
#define USB_EHCI_QH 4192u
#define USB_EHCI_TD 4288u
#define USB_EHCI_HID_BASE 4608u
#define USB_EHCI_HID_STRIDE 224u
#define USB_EHCI_BULK_BUFFER 19456u
#define USB_CONTROLLER_INVENTORY_VERSION 1u
#define USB_CONTROLLER_OWNED 1u
#define USB_CONTROLLER_ROUTE_ONLY 2u
#define USB_CONTROLLER_HIGH_SPEED 4u
#define USB_CONTROLLER_TT_UNSUPPORTED 8u
#define USB_CONTROLLER_QUARANTINED 16u
#define USB_QUIRK_NVIDIA_31BIT 1u
#define USB_QUIRK_AMD8111 2u
#define USB_QUIRK_VIA_SLEEP 4u
struct usb_controller_inventory {
    uint32_t version, size, pci_locator, parent_locator, flags, quirks;
    uint32_t ports, companions, companion_functions, ready_functions;
    uint32_t port_companion[USB_PORT_MAX];
};
_Static_assert(sizeof(struct usb_controller_inventory) == 72, "USB inventory v1");
static inline uint32_t usb_pci_locator(const struct pci_func *p)
{ return 0x80000000u | (uint32_t)p->bus << 16 | (uint32_t)p->dev << 8 | p->fn; }
struct usb_host;
struct usb_device;
/* All callbacks are pinned. sleep must yield with IF=1, even in a fixture.
 * Config and register widths are 1, 2 or 4. */
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
    bool boot_capable, input_registered, unsupported;
    int recovery_result;
    struct usb_mouse_report mouse[USB_MOUSE_REPORTS];
    uint8_t mouse_reports;
    bool report_protocol, report_ids;
    struct usb_hid_reports *reports; /* bounded, retained with monotonic device record */
};
struct usb_storage_trace {
    struct usb_packet_trace packet;
    struct usb_packet_trace packets[USB_BULK_TRACE_LIMIT];
    unsigned packet_count, packet_index, packet_offset;
    uint64_t bulk_started_ms, bulk_deadline, bulk_elapsed_ms, bind_remaining_ms;
    const char *bulk_stop;
    const char *stage;
    const char *fault_reason; /* f1-96: last transport/CSW fault of this command */
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
    struct blk_queue requests;
    uint32_t tag;
    uint64_t next_check;
    uint64_t bulk_deadline; /* only live while usb_bulk calls the packet engine */
    uint64_t bind_deadline; /* MAX LUN through namespace publication, including recovery */
    uint64_t bind_started_ms, bind_stage_started_ms;
    const char *bind_stage;
    struct usb_storage_trace trace;
    uint8_t interface, in, out, in_toggle, out_toggle;
    uint16_t in_packet, out_packet;
    uint8_t sense_key, asc, ascq;
    bool bound, online, published, ejected, write_protected, fault_logged, quarantine_logged;
    unsigned command_errors; /* fx-17: CHECK CONDITION READ/WRITE failures kept online */
    bool write_through; /* fx-17b: no SYNCHRONIZE CACHE; every WRITE(10) completes on the medium */
    bool binding, bind_reserved, bind_stage_active;
    uint32_t bind_read_requests, bind_read_sectors, bind_read_max;
    uint32_t bind_packets, bind_poll_frames, bind_retire_frames;
    /* f1-96: negotiated sectors per READ(10)/WRITE(10); the largest READ(10)
     * that completed with a passed CSW on this medium (writes never use a
     * longer transfer); ladder fallbacks (128, 8, 1), recorded. */
    uint32_t max_sectors, read_qualified, transfer_fallbacks;
    /* f1-96b: QH elements advanced by hand (early-Intel erratum) and data
     * stages relinked once after a stall, for this device. */
    uint32_t advance_fixes, relinks;
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
    uint8_t serial_identity[32]; /* SHA-256 of full UTF-16LE serial descriptor */
    bool serial_advertised, serial_valid;
    uint8_t address, ep0, class_code, root_port, parent, port, hub_ports, config;
    bool present, configured, quarantined, low_speed, high_speed;
    /* Selected hub interface (alternate zero); multi-TT is never inferred
     * from the device's capability protocol alone. One external hub level. */
    bool tt_multi, tt_pending, tt_recovering;
    uint8_t tt_think; /* 8, 16, 24 or 32 full-speed bit times */
    uint16_t control_timeout_ms; /* f1-96b: nonzero only around a BOT class reset */
    uint64_t control_deadline; /* fx-21b: end of an enumeration control message, 0 otherwise */
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
    struct mm_pin storage_pin; /* shared HCD bounce/descriptor arena, host lifetime */
    gen_t claim_gen[3];
    char owner[24];
    const char *reason;
    struct usb_controller_inventory inventory;
    const struct usb_host *ehci_group; /* pinned controller records, never stack storage */
    unsigned ehci_group_count;
    uint32_t ehci_params;
    uint8_t ehci_caplen;
    bool ehci_started;
    unsigned ports, device_count, next_address;
    bool root_blocked[USB_PORT_MAX];
    uint8_t root_child[USB_PORT_MAX];
    unsigned root_status[USB_PORT_MAX]; /* last observed normalized state */
    struct usb_port_log root_log[USB_PORT_MAX];
    bool attempted, ready, quarantined, touched, owned, busy;
    bool suspended, sleep_ready;
    uint32_t sleep_regs[12];
    bool irq_attached, irq_pending;
    bool completion_irq, irq_fallback;
    uint64_t completion_interrupts;
    bool irq_probe_pending;
    uint64_t irq_probe_at, irq_probe_count;
    uint32_t irq_status; /* fatal causes retained after device-local W1C */
    bool ohci_root_pending; /* RHSC stays disabled until the next root scan */
    uint32_t ohci_done_heads[USB_OHCI_DONE_HEADS];
    unsigned ohci_done_read, ohci_done_count;
    bool ohci_packet_done; /* USB_TD consumed from its published done chain */
    uint64_t ohci_done_batches, ohci_done_tds;
    uint64_t interrupts;
    struct kwait changed;
    bool scanned, schedule_dumped;
    /* fx-21b: usb_power_quiesce() asks enumeration control waits to end. */
    volatile bool quiescing;
    unsigned scan_count;
    uint64_t next_scan, scan_window;
    fs_lock transfer_lock; /* short DMA/state mutex; always free across sleeps */
    struct usb_device devices[USB_DEVICE_MAX];
};
extern const struct usb_hc_ops usb_uhci_ops, usb_ohci_ops, usb_ehci_ops;
int usb_ehci_activate(struct usb_host *, struct usb_host *, unsigned);
int usb_uhci_suspend(struct usb_host *);
int usb_uhci_resume(struct usb_host *);
int usb_ohci_suspend(struct usb_host *);
int usb_ohci_resume(struct usb_host *);
int usb_ehci_suspend(struct usb_host *);
int usb_ehci_resume(struct usb_host *);
int usb_host_suspend(struct usb_host *);
int usb_host_resume(struct usb_host *);
bool usb_ehci_stop(struct usb_host *); /* pinned claims/arena are never released */
int usb_controller_snapshot(const struct usb_host *, struct usb_controller_inventory *);
bool usb_ehci_companion_ready(const struct usb_host *, unsigned port);
static inline void usb_barrier(void) { __asm__ volatile("" ::: "memory"); }
/* Bulk NAK never renews a deadline. Binding ACK progress may renew the bulk
 * wait, always capped by the aggregate bind limit. Published I/O retains its
 * whole-stage bound. Poll counts still bound a frozen software clock. */
/* fx-21b: a power quiesce ends an enumeration message's wait at once. */
static inline bool usb_control_cancelled(const struct usb_device *d)
{ return d->control_deadline && d->host->quiescing; }
static inline unsigned usb_packet_budget(struct usb_device *d, unsigned ep, bool periodic,
                                         uint64_t now)
{
    /* Interrupt opportunities follow the HC clock, not the scheduler's
     * software clock. Packet engines stop after two linked HC frames;
     * this is only the fail-closed watchdog, including a frozen clock. */
    if (periodic) return USB_PERIODIC_WATCHDOG_MS;
    unsigned budget = !ep && d->control_timeout_ms ? d->control_timeout_ms : USB_TIMEOUT_MS;
    if (!ep && d->control_deadline) {
        /* fx-21b: the stages of one enumeration message share its 5 s. */
        int64_t left = (int64_t)(d->control_deadline - now);
        if (left <= 0 || d->host->quiescing) return 0;
        budget = (uint64_t)left < USB_CTRL_TIMEOUT_MS ? (unsigned)left : USB_CTRL_TIMEOUT_MS;
    }
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
bool usb_host_irq(struct usb_host *);
void usb_ohci_service_done(struct usb_host *); /* transfer mutex held; before TD reuse */
int usb_control(struct usb_device *, uint8_t type, uint8_t request, uint16_t value,
                uint16_t index, void *, unsigned length);
int usb_enumerate(struct usb_host *, unsigned root, unsigned parent, unsigned port, bool low);
/* fx-21b: devices quarantined at enumeration since boot ("USB Device Not
 * Recognized"); /dev/media reports it to the desktop notification area. */
uint32_t usb_unrecognized_devices(void);
int usb_parse_config(struct usb_device *, const uint8_t *, unsigned);
void usb_step(struct usb_host *);
void usb_host_take(struct usb_host *); /* reserves serialized general/BOT slot */
bool usb_host_take_until(struct usb_host *, uint64_t deadline);
void usb_host_drop(struct usb_host *);
void usb_sleep(struct usb_host *, unsigned); /* caller's reservation survives */
void usb_poll_pause(struct usb_host *, unsigned); /* bounded retry backoff; may finish early */
void usb_hid_service(struct usb_host *); /* mutex held, no general transfers */
uint64_t usb_hid_deadline(struct usb_host *); /* absolute; UINT64_MAX = IRQ only */
void usb_irq_completion_check(struct usb_host *); /* mutex held, lost IOC evidence */
void usb_ehci_tt_service(struct usb_device *); /* general reservation held */
void usb_uhci_dump_schedule(struct usb_host *); /* once, after enumeration; mutex held */
int usb_hub_attach(struct usb_device *);
void usb_hub_scan(struct usb_device *);
int usb_hid_attach(struct usb_device *, struct usb_hid *, unsigned report_length);
int usb_hid_query(uint32_t command, void *record);
/* Cache only, no physical USB transaction; common joystick calibration. */
int usb_hid_raw(uint32_t device, uint32_t generation, int32_t raw[4]);
int usb_hid_describe(struct usb_device *, struct usb_hid *);
int usb_joystick_identity(uint32_t, uint32_t, uint8_t[32]);
void usb_identity_serial(struct usb_device *, unsigned index);
void usb_hid_publish(struct usb_device *, struct usb_hid *, const struct hid_values *, unsigned report);
void usb_hid_lifecycle(struct usb_device *, struct usb_hid *, bool attached);
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
