/* Exact mouse/boot reports -> production HID -> production queue -> real user grant.
 * Reuse the process/grant fixture's fake RAM and scheduler, not its fake input.
 * SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#define CIUKI_DESKTOP_REAL_INPUT
#include "../proc/desktop_test.c"
#undef main
#include <ciuki/usb.h>
#include "mouse_descriptor.h"
#include <time.h>

static uint64_t input_tick = 100;
static const uint8_t *pending_report;
static unsigned pending_length, key_logs, drops, log_calls;
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
void klog_periodic(const char *fmt, ...) __attribute__((alias("klog")));
void klog(const char *fmt, ...)
{
    log_calls++;
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
static void mouse_events(struct process *p, int fd, struct usb_device *mouse,
                         const uint8_t *report, unsigned len, unsigned mask,
                         int x, int y, int wheel)
{
    unsigned old=mouse->hid[0].previous[0];
    poll_report(mouse,report,len);
    struct ciuki_input_event e[8]; unsigned n=0, at=0;
    for (unsigned b=0;b<3;b++) if ((mask^old)&(1u<<b)) n++;
    n+=!!x+!!y+!!wheel;
    if (!n) { CHECK(grant_input_read(p,fd,BUFFER,8)==-EAGAIN); return; }
    CHECK(read_events(p,fd,e,8)==n);
    for (unsigned i=0;i<n;i++) CHECK(e[i].source==INPUT_USB && e[i].generation==mouse->generation && e[i].monotonic_ns==input_tick*UINT64_C(1000000));
    for (unsigned b=0;b<3;b++) if ((mask^old)&(1u<<b)) {
        CHECK(e[at].type==CIUKI_INPUT_BUTTON && e[at].code==(int)b+1 &&
              e[at].value==!!(mask&(1u<<b)) && !e[at].value2); at++;
    }
    if (x) { CHECK(e[at].type==CIUKI_INPUT_MOTION && e[at].value==x && !e[at].value2); at++; }
    if (y) { CHECK(e[at].type==CIUKI_INPUT_MOTION && !e[at].value && e[at].value2==y); at++; }
    if (wheel) CHECK(e[at].type==CIUKI_INPUT_WHEEL && !e[at].code && e[at].value==wheel && !e[at].value2);
}
static uint64_t cpu_ns(void)
{
    struct timespec t; CHECK(!clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&t));
    return (uint64_t)t.tv_sec*UINT64_C(1000000000)+(uint64_t)t.tv_nsec;
}
static void mouse_regression(struct process *p, int fd, struct usb_device *mouse)
{
    struct usb_hid *h=&mouse->hid[0];
    h->packet=8; h->report_protocol=true;
    CHECK(usb_mouse_parse(h,receiver_mouse_descriptor,sizeof(receiver_mouse_descriptor)));
    CHECK(h->mouse_reports==4 && h->report_ids && h->mouse[0].button_count==16);
    CHECK(h->mouse[0].buttons[0].bit==0 && h->mouse[0].buttons[1].bit==1 && h->mouse[0].buttons[2].bit==2);
    CHECK(h->mouse[0].x.bit==16 && h->mouse[0].y.bit==28 && h->mouse[0].wheel.bit==40 && h->mouse[0].pan.bit==48);
    static const uint8_t receiver[][8]={
        {2,1,0,0xff,0x3f,0,0xff,0x7f}, /* left, X=-1 Y=3 wheel=-1 pan=127 */
        {2,2,0x80,0,0xf8,0x7f,1,0x81}, /* right, X=-2048 Y=2047 */
        {2,4,0x40,0xff,0x17,0x80,0x81,1}, /* middle, X=2047 Y=-2047 */
        {2,7,0x80,1,0xf0,0xff,0x7f,0xff}, /* all three, X=1 Y=-1 */
        {2,0,0x80,0,0,0,0,0x81} /* high button/pan alone: no motion */
    };
    static const int pan[]={127,-127,1,-1,-127};
    for (unsigned i=0;i<5;i++) {
        struct usb_mouse_input decoded;
        CHECK(usb_mouse_decode(h,receiver[i],8,&decoded)==1 && decoded.pan==pan[i]);
    }
    mouse_events(p,fd,mouse,receiver[0],8,1,-1,3,-1);
    /* All non-mouse IDs (and unknown IDs) retain held left state. */
    static const uint8_t other[][8]={{3,1,0,2,0},{4,3},{8,0xff},{17,0xff}};
    for (unsigned i=0;i<4;i++) {
        CHECK(!usb_hid_report(mouse,h,other[i],8)); CHECK(h->previous[0]==1);
        CHECK(grant_input_read(p,fd,BUFFER,8)==-EAGAIN);
    }
    for (unsigned n=0;n<8;n++) CHECK(usb_hid_report(mouse,h,receiver[1],n)==-USB_EPROTO && h->previous[0]==1);
    mouse_events(p,fd,mouse,receiver[1],8,2,-2048,2047,1);
    mouse_events(p,fd,mouse,receiver[2],8,4,2047,-2047,-127);
    mouse_events(p,fd,mouse,receiver[3],8,7,1,-1,127);
    mouse_events(p,fd,mouse,receiver[4],8,0,0,0,0);
    CHECK(grant_input_read(p,fd,BUFFER,8)==-EAGAIN);
    puts("PASS F1-68 exact 148-byte 046d:c52b descriptor: left/right/middle, 16 button bits, signed 12-bit extremes, wheel +/-1/127, isolated AC Pan, IDs 3/4/8, short reports through production grant");
    /* Per-report CPU time includes decoding and the production merged queue,
     * draining after each report. Both quiet and diagnostic paths are timed;
     * neither can sleep/control-transfer or wait for a log drain. */
    for (unsigned verbose=0;verbose<2;verbose++) {
        g_boot.flags=verbose ? CBI_F_VERBOSE : 0;
        h->key_records=h->report_records=h->latency_records=0;
        unsigned logs=log_calls; uint64_t begin=cpu_ns();
        for (unsigned i=0;i<10000;i++) {
            pending_report=receiver[i%2]; pending_length=8;
            input_tick+=h->interval; usb_hid_poll(mouse,h); CHECK(!pending_report);
            struct input_event e; unsigned n=0; while(input_read(&e)) n++;
            CHECK(n>=3 && n<=5 && h->next_poll==input_tick+h->interval);
        }
        uint64_t cost=(cpu_ns()-begin)/10000;
        CHECK(cost<UINT64_C(1000000)); /* host CPU cost below 1 ms */
        printf("PASS F1-68 per-report cost mode=%s reports=10000 cpu_ns_per_report=%llu interval_ms=%u logs=%u no_poll_deferral=1\n",
               verbose ? "verbose":"quiet",(unsigned long long)cost,h->interval,log_calls-logs);
    }
    uint8_t release[8]={2};
    CHECK(!usb_hid_report(mouse,h,release,8)); struct input_event stale; while(input_read(&stale)) {}
    CHECK(usb_mouse_parse(h,qemu_mouse_descriptor,sizeof(qemu_mouse_descriptor)) && !h->report_ids);
    static const uint8_t qemu[][4]={{1,0x81,0x7f,0xff},{2,0x7f,0x81,1},{4,0,0,0x81},{0}};
    mouse_events(p,fd,mouse,qemu[0],4,1,-127,127,-1);
    mouse_events(p,fd,mouse,qemu[1],4,2,127,-127,1);
    mouse_events(p,fd,mouse,qemu[2],4,4,0,0,-127);
    mouse_events(p,fd,mouse,qemu[3],4,0,0,0,0);
    puts("PASS F1-68 exact 52-byte QEMU descriptor: left/right/middle, signed motion and wheel through production grant");
    g_boot.flags=CBI_F_VERBOSE;
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
        CHECK(usb_mouse_parse(&mouse.hid[0],receiver_mouse_descriptor,sizeof(receiver_mouse_descriptor)));
        mouse.hid[0].report_protocol=true;mouse.hid[0].packet=8;
        uint8_t numbered[8]={2,0,0,0,0,0,0xff,0};
        poll_report(&mouse,numbered,8);
        CHECK(read_events(p,grants[1],events,8)==1);
        CHECK(events[0].type==CIUKI_INPUT_WHEEL && events[0].value==-1 && !events[0].value2 && !events[0].code);
        CHECK(events[0].source==INPUT_USB && events[0].generation==72);
        numbered[6]=1;poll_report(&mouse,numbered,8);
        CHECK(read_events(p,grants[1],events,8)==1 && events[0].type==CIUKI_INPUT_WHEEL && events[0].value==1);
        puts("PASS F1-66 numbered packed mouse report delivers signed CIUKI_INPUT_WHEEL through the production desktop grant");
        mouse_regression(p,grants[1],&mouse);
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
