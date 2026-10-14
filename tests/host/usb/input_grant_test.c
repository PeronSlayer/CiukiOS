/* Exact boot reports -> production HID -> production queue -> real user grant.
 * Reuse the process/grant fixture's fake RAM and scheduler, not its fake input.
 * SPDX-License-Identifier: GPL-2.0-only */
#define CIUKI_DESKTOP_REAL_INPUT
#include "../proc/desktop_test.c"
#undef main
#include <ciuki/usb.h>

static uint64_t input_tick = 100;
static const uint8_t *pending_report;
static unsigned pending_length, key_logs, drops;
static char last_drop[256];
static uint64_t input_now(void *cookie) { (void)cookie; return input_tick; }
static int input_packet(struct usb_device *d, unsigned ep, unsigned pid, unsigned toggle,
                        void *buffer, unsigned length, bool periodic)
{
    (void)d; (void)toggle;
    CHECK(ep == 1 && pid == USB_PID_IN && periodic && length >= pending_length);
    if (!pending_report) return -USB_EAGAIN;
    memcpy(buffer, pending_report, pending_length);
    pending_report = 0;
    return (int)pending_length;
}
static const struct usb_hc_ops input_ops = { .packet = input_packet };
int usb_control(struct usb_device *d, uint8_t type, uint8_t request, uint16_t value,
                uint16_t index, void *data, unsigned length)
{
    (void)d; (void)data;
    CHECK(type == 0x21 && !index);
    CHECK((request == 11 && !value && !length) ||
          (request == 10 && !value && !length) ||
          (request == 9 && value == 0x200 && length == 1));
    return (int)length;
}
void usb_device_quarantine(struct usb_device *d, const char *reason)
{
    (void)reason;
    d->quarantined = true;
    usb_hid_release(d, &d->hid[0]);
}
bool usb_expired(struct usb_host *host, uint64_t deadline)
{ return host->io.now(host->io.cookie) >= deadline; }
void klog(const char *fmt, ...)
{
    char record[256]; va_list ap; va_start(ap, fmt);
    kvsnprintf(record, sizeof(record), fmt, ap); va_end(ap);
    if (strstr(record, "[input] usb key")) key_logs++;
    if (strstr(record, "[input] usb drop reason=")) {
        drops++; memcpy(last_drop, record, strlen(record) + 1);
    }
}
static void poll_report(struct usb_device *d, const uint8_t *report, unsigned length)
{
    pending_report = report; pending_length = length; input_tick += 20;
    usb_hid_poll(d, &d->hid[0]);
    CHECK(!pending_report && !d->quarantined);
}
static unsigned read_events(struct process *p, int fd, struct ciuki_input_event *events,
                            unsigned capacity)
{
    int n = grant_input_read(p, fd, BUFFER, capacity);
    CHECK(n > 0 && n <= (int)capacity);
    CHECK(!ua_read(p->memory, events, BUFFER, (unsigned)n * sizeof(*events)));
    return (unsigned)n;
}
static void key_event(const struct ciuki_input_event *e, uint16_t code, int value)
{
    CHECK(e->type == CIUKI_INPUT_KEY && e->code == code && e->value == value);
    CHECK(e->source == INPUT_USB && e->generation == 71 && e->monotonic_ns);
}
static void drop_reason(const char *reason, unsigned before)
{
    char expected[80]; snprintf(expected, sizeof(expected), "reason=%s ", reason);
    CHECK(drops == before + 1 && strstr(last_drop, expected));
}
int main(int argc, char **argv)
{
    bool exact_only = argc == 2 && !strcmp(argv[1], "--exact");
    ram = calloc(HOST_PAGES, PAGE_SIZE); CHECK(ram);
    controller.state = T_RUNNING; g_current = &controller; proc_init();
    struct process *p; struct proc_thread *t = make_process(proc_supervisor(), &p);
    desktop_select(t);
    int32_t grants[2]; CHECK(!grants_install(p, grants));
    CHECK(!desktop_fcntl(p, grants[1], F_SETFL, O_NONBLOCK));
    struct usb_host host = { .io = { .now = input_now }, .ops = &input_ops };
    struct usb_device keyboard = { .host = &host, .address = 1, .generation = 71,
        .hid = {{ .protocol = 1, .endpoint = 1, .packet = 8, .interval = 1 }} };
    g_boot.flags = CBI_F_TEST_REQUEST;
    CHECK(!usb_hid_attach(&keyboard, &keyboard.hid[0], 0));
    struct input_devices devices; input_devices_snapshot(&devices);
    CHECK(!devices.ps2 && !devices.firmware && devices.usb == 1);
    CHECK(grant_input_read(p, grants[1], BUFFER, 8) == -EAGAIN);
    static const uint8_t reports[][8] = {
        {0,0,0x45,0,0,0,0,0}, {0}, {0,0,0x04,0,0,0,0,0}, {0}
    };
    struct ciuki_input_event events[8];
    poll_report(&keyboard, reports[0], 8);
    poll_report(&keyboard, reports[1], 8);
    poll_report(&keyboard, reports[2], 8);
    CHECK(read_events(p, grants[1], events, 8) == 4);
    key_event(&events[0], 0x58, 1); key_event(&events[1], 0x58, 0);
    key_event(&events[2], 0x1e, 1);
    CHECK(events[3].type == CIUKI_INPUT_TEXT && events[3].code == 'a' &&
          events[3].source == INPUT_USB && events[3].generation == 71);
    for (unsigned i = 0; i < 4; i++) CHECK(events[i].sequence == i + 1);
    CHECK(events[0].monotonic_ns == UINT64_C(120000000) &&
          events[1].monotonic_ns == UINT64_C(140000000) &&
          events[2].monotonic_ns == UINT64_C(160000000));
    CHECK(key_logs == 3 && !drops);
    poll_report(&keyboard, reports[3], 8);
    CHECK(read_events(p, grants[1], events, 8) == 1); key_event(&events[0], 0x1e, 0);
    puts("PASS exact QEMU F12 down/up and A down/up through production input grant, PS/2 absent");
    if (!exact_only) {
        /* HID F.4: byte 1 is reserved and must not reject a boot report. */
        uint8_t reserved[8] = {2,0x7f,0x45};
        poll_report(&keyboard, reserved, 8);
        CHECK(read_events(p, grants[1], events, 8) == 2);
        key_event(&events[0], 0x2a, 1); key_event(&events[1], 0x58, 1);
        unsigned before = drops;
        uint8_t rollover[8] = {0,0,1,1,1,1,1,1};
        poll_report(&keyboard, rollover, 8); drop_reason("keyboard_rollover", before);
        CHECK(grant_input_read(p, grants[1], BUFFER, 8) == -EAGAIN);
        CHECK(keyboard.hid[0].previous[0] == 2 && keyboard.hid[0].previous[2] == 0x45);
        reserved[0] = reserved[2] = 0;
        poll_report(&keyboard, reserved, 8);
        CHECK(read_events(p, grants[1], events, 8) == 2);
        key_event(&events[0], 0x2a, 0); key_event(&events[1], 0x58, 0);
        puts("PASS reserved byte ignored, modifier and F12 edges reach the grant");
        /* Every discarded keyboard report has a reason, even beyond the
         * bounded prefix for report/translation records. */
        for (unsigned i = 0; i < USB_HID_TRACE_LIMIT + 2; i++) {
            before = drops;
            poll_report(&keyboard, reports[3], 8); drop_reason("unchanged", before);
        }
        uint8_t unknown[8] = {0,0,0x74}; before = drops;
        poll_report(&keyboard, unknown, 8); drop_reason("unsupported_usage", before);
        before = drops;
        CHECK(usb_hid_report(&keyboard, &keyboard.hid[0], reports[0], 7) == -USB_EPROTO);
        drop_reason("keyboard_length", before);
        CHECK(keyboard.hid[0].previous[2] == 0x74);
        before = drops;
        CHECK(usb_hid_report(&keyboard, &keyboard.hid[0], 0, 8) == -EINVAL);
        drop_reason("missing_report", before);
        g_boot.flags = 0; before = drops;
        CHECK(usb_hid_report(&keyboard, &keyboard.hid[0], reports[0], 7) == -USB_EPROTO);
        CHECK(drops == before);
        g_boot.flags = CBI_F_VERBOSE;
        input_usb_event(INPUT_KEY, 0x58, 1, 0, input_tick); drop_reason("generation", before);
        before = drops; g_boot.flags |= CBI_F_INPUT_FORCED;
        input_usb_event(INPUT_KEY, 0x58, 1, 71, input_tick); drop_reason("forced_policy", before);
        before = drops; g_boot.flags = CBI_F_VERBOSE; g_boot.input_policy = CBI_INPUT_FIRMWARE;
        input_usb_event(INPUT_KEY, 0x58, 1, 71, input_tick); drop_reason("firmware_policy", before);
        g_boot.input_policy = CBI_INPUT_NATIVE;
        before = drops; input_usb_event(INPUT_KEY, 0x58, 0, 71, input_tick);
        drop_reason("unbalanced_release", before);
        before = drops; input_usb_event(INPUT_KEY, 0x58, 2, 71, input_tick);
        drop_reason("repeat_without_press", before);
        before = drops; input_usb_event(INPUT_KEY, 0x58, 3, 71, input_tick);
        drop_reason("key_value", before);
        before = drops; input_usb_event(INPUT_KEY, 0x201, 1, 71, input_tick);
        drop_reason("key_code", before);
        CHECK(grant_input_read(p, grants[1], BUFFER, 8) == -EAGAIN);
        puts("PASS discard reasons, rollover state retention, source policy and quiet boot");
        /* The directive's four-byte mouse report stays two signed motions. */
        struct usb_device mouse = { .host = &host, .address = 3, .generation = 72,
            .hid = {{ .protocol = 2, .endpoint = 1, .packet = 4, .interval = 1,
                      .active = true, .wheel = true }} };
        input_devices_changed(INPUT_USB, true);
        uint8_t mouse_report[4] = {0,0x81,0x81,0};
        poll_report(&mouse, mouse_report, 4);
        CHECK(read_events(p, grants[1], events, 8) == 2);
        CHECK(events[0].type == CIUKI_INPUT_MOTION && events[0].value == -127 && !events[0].value2);
        CHECK(events[1].type == CIUKI_INPUT_MOTION && !events[1].value && events[1].value2 == -127);
        for (unsigned i = 0; i < 2; i++) CHECK(events[i].source == INPUT_USB && events[i].generation == 72);
        before = drops;
        CHECK(usb_hid_report(&mouse, &mouse.hid[0], mouse_report, 2) == -USB_EPROTO);
        drop_reason("mouse_length", before);
        usb_hid_release(&mouse, &mouse.hid[0]);
        puts("PASS exact mouse report through the production grant without keyboard changes");
        /* Active-key detach releases remain readable after the final producer disappears. */
        poll_report(&keyboard, reports[0], 8);
        CHECK(read_events(p, grants[1], events, 8) == 1); key_event(&events[0], 0x58, 1);
        usb_hid_release(&keyboard, &keyboard.hid[0]);
        CHECK(read_events(p, grants[1], events, 8) == 1); key_event(&events[0], 0x58, 0);
        before = drops;
        CHECK(usb_hid_report(&keyboard, &keyboard.hid[0], reports[0], 8) == -EINVAL);
        drop_reason("inactive", before);
    }
    usb_hid_release(&keyboard, &keyboard.hid[0]);
    input_devices_snapshot(&devices); CHECK(!devices.usb);
    CHECK(grant_input_read(p, grants[1], BUFFER, 8) == -EAGAIN);
    desktop_clean(p); free(ram);
    return 0;
}
