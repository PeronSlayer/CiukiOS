/* Bounded USB 1.x host/HID model. See docs/design/usb-contract.md.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_USB_H
#define CIUKI_USB_H
#include <ciuki/registry.h>
#define USB_HOST_MAX 4u
#define USB_DEVICE_MAX 16u
#define USB_PORT_MAX 8u
#define USB_HID_MAX 4u
#define USB_CONFIG_MAX 512u
#define USB_DMA_BYTES 8192u
#define USB_PACKET_MAX 64u
#define USB_TIMEOUT_MS 500u
#define USB_EIO 5
#define USB_EAGAIN 11
#define USB_EPIPE 32
#define USB_EPROTO 71
#define USB_ETIMEDOUT 110
#define USB_ED 4096u
#define USB_TD 4128u
#define USB_TAIL 4160u
#define USB_BUFFER 4352u
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
struct usb_hid {
    uint8_t interface, protocol, endpoint, packet, interval, toggle;
    uint8_t previous[8], leds;
    uint16_t repeat_key, report_length;
    uint64_t next_poll, repeat_at;
    bool active, wheel;
};
struct usb_device {
    struct usb_host *host;
    gen_t generation;
    const char *reason;
    uint16_t vendor, product;
    uint8_t address, ep0, class_code, root_port, parent, port, hub_ports, config;
    bool present, configured, quarantined, low_speed;
    struct usb_hid hid[USB_HID_MAX];
    bool hub_blocked[USB_PORT_MAX];
    uint8_t hub_child[USB_PORT_MAX]; /* index+1; zero = not enumerated */
};
struct usb_hc_ops {
    int (*start)(struct usb_host *);
    unsigned (*port_status)(struct usb_host *, unsigned);
    int (*port_reset)(struct usb_host *, unsigned);
    void (*port_disable)(struct usb_host *, unsigned);
    int (*packet)(struct usb_device *, unsigned endpoint, unsigned pid,
                  unsigned toggle, void *, unsigned bytes, bool periodic);
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
    bool attempted, ready, quarantined, touched, owned, busy;
    uint64_t next_scan;
    struct usb_device devices[USB_DEVICE_MAX];
};
extern const struct usb_hc_ops usb_uhci_ops, usb_ohci_ops;
static inline void usb_barrier(void) { __asm__ volatile("" ::: "memory"); }
uint32_t usb_read(struct usb_host *, unsigned, unsigned);
void usb_write(struct usb_host *, unsigned, uint32_t, unsigned);
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
int usb_hub_attach(struct usb_device *);
void usb_hub_scan(struct usb_device *);
int usb_hid_attach(struct usb_device *, struct usb_hid *, unsigned report_length);
int usb_hid_report(struct usb_device *, struct usb_hid *, const uint8_t *, unsigned);
void usb_hid_poll(struct usb_device *, struct usb_hid *);
void usb_hid_release(struct usb_device *, struct usb_hid *);
bool usb_mouse_wheel_layout(const uint8_t *, unsigned);
void usb_init(void);
bool usb_input_ready(void);
unsigned usb_count(void);
const struct usb_host *usb_get(unsigned);
#endif
