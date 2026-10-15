/* Arrival -> real HC TD -> production HID -> production merged queue.
 * Bulk NAKs and scan/reset waits run concurrently with interrupt slots.
 * SPDX-License-Identifier: GPL-2.0-only */
#define USB_LATENCY_QUEUE
#define main usb_model_main
#include "usb_test.c"
#undef main
uint64_t deadline_after_ms(uint32_t ms) { return m.now+ms; }

#define REPORTS 24u
struct sample { uint64_t available, ack, delivered; };
static struct sample samples[2][REPORTS];
static unsigned produced, consumed[2], peripheral_index[2], device_index[2];
static uint64_t next_report;
static unsigned report_period;
static unsigned latency_prefix[128],latency_windows[128],recorded_bound;
static void record(const char *line)
{
    unsigned addr,ms,reports;
    if (sscanf(line,"[usb] hid latency addr=%u ms=%u",&addr,&ms)==2) {
        assert(addr<128 && strstr(line,"observed_ms=") && strstr(line,"bound=upper"));
        latency_prefix[addr]++;assert(latency_prefix[addr]<=USB_HID_LATENCY_LIMIT);
        if (ms>recorded_bound) recorded_bound=ms;
    }
    if (sscanf(line,"[usb] hid latency max addr=%u ms=%u",&addr,&ms)==2) {
        assert(addr<128 && sscanf(strstr(line,"reports="),"reports=%u",&reports)==1 && reports);
        assert(strstr(line,"window_ms=") && strstr(line,"bound=upper"));latency_windows[addr]++;
        if (ms>recorded_bound) recorded_bound=ms;
    }
}
static void inject(struct model *model)
{
    if (produced==REPORTS || model->now<next_report) return;
    for (unsigned i=0;i<2;i++) {
        struct peripheral *p=&model->dev[peripheral_index[i]];
        assert(!p->report_len);
        memset(p->report,0,sizeof(p->report));
        if (!i) { p->report[2]=(produced&1) ? 0:0x45;p->report_len=8; }
        else { p->report[0]=(produced&1) ? 0:1;p->report[1]=1;p->report[2]=2;p->report_len=4; }
        samples[i][produced].available=model->now;
    }
    produced++;next_report+=report_period;
}
static void ack(struct model *model,struct peripheral *p)
{
    unsigned index=(unsigned)(p-model->dev);
    unsigned i=index==peripheral_index[0] ? 0:1;
    assert(index==peripheral_index[i]);
    assert(i<2 && produced && !samples[i][produced-1].ack);
    samples[i][produced-1].ack=model->now;
}
static void drain(void)
{
    struct input_event event;
    while (input_read(&event)) {
        assert(event.source==INPUT_USB && event.type!=INPUT_RESYNC);
        unsigned i;
        if (event.type==INPUT_KEY && event.code==0x58) i=0;
        else if (event.type==INPUT_REL && event.code==INPUT_X) i=1;
        else continue;
        assert(consumed[i]<REPORTS);
        struct sample *s=&samples[i][consumed[i]++];
        assert(s->available && s->ack && event.tick>=s->ack);
        s->delivered=event.tick;
    }
}
static void fixture(bool ohci,unsigned frame_ms,unsigned gap,unsigned phase,bool scan,unsigned period)
{
    report_period=period;
    setup(ohci,scan);assert(!attach());usb_step(&m.host);
    peripheral_index[0]=scan ? 2:0;peripheral_index[1]=scan ? 3:1;
    device_index[0]=scan ? 1:0;device_index[1]=scan ? 2:1;
    for (unsigned i=0;i<20;i++) {
        sleep_ms(&m,1);
        fs_lock_take(&m.host.transfer_lock);usb_hid_service(&m.host);fs_lock_drop(&m.host.transfer_lock);
    }
    memset(latency_prefix,0,sizeof(latency_prefix));memset(latency_windows,0,sizeof(latency_windows));recorded_bound=0;
    record_hook=record;
    memset(samples,0,sizeof(samples));memset(consumed,0,sizeof(consumed));produced=0;
    struct input_event stale;while(input_read(&stale)) {}
    m.frame_ms=frame_ms;m.sleep_gap=gap;m.check_sleep_lock=true;
    g_boot.flags=CBI_F_TEST_REQUEST;
    next_report=m.now+1+phase;frame_hook=inject;report_ack_hook=ack;
    uint64_t begin=m.now;
    struct usb_device *d=&m.host.devices[0];
    unsigned toggle=0;
    if (scan) {
        /* Existing hub keyboard/mouse receive input while the real scan
         * debounces, resets and enumerates a new root-port keyboard. */
        m.dev[1]=(struct peripheral){.kind=KBD,.present=true};
        if (ohci) m.regs[0x58/4]=0x10001;
        else m.regs[0x10/4]=(m.regs[0x10/4]&0xffffu)|(0x83u<<16);
        m.host.next_scan=0;usb_step(&m.host);
        assert(m.host.device_count==4 && m.host.devices[3].configured);
    } else {
        /* Composite HID + BOT device: independent endpoints, same host and
         * generation. The production bulk engine owns the general TD for
         * 1.2 seconds; every interrupt pipe must still deliver throughout. */
        d->storage.bound=true;d->storage.in=2;d->storage.out=3;
        d->storage.in_packet=d->storage.out_packet=64;
        m.bulk_test=true;m.bulk_ready_at=m.now+1200;
        uint8_t data[64];unsigned actual=0;
        usb_host_take(&m.host);
        assert(!usb_bulk(d,true,data,sizeof(data),&actual) && actual==sizeof(data));
        toggle=d->storage.in_toggle;
        usb_host_drop(&m.host);
        assert(toggle==1 && m.bulk_packets==1 && !d->storage.bulk_deadline);
        for (unsigned i=0;i<sizeof(data);i++) assert(data[i]==0x5a);
    }
    while (m.now-begin<1400) {
        sleep_ms(&m,1);
        fs_lock_take(&m.host.transfer_lock);usb_hid_service(&m.host);fs_lock_drop(&m.host.transfer_lock);
    }
    frame_hook=NULL;report_ack_hook=NULL;record_hook=NULL;drain();
    assert(m.host.ready && !m.host.busy && produced==REPORTS);
    unsigned arrival_max=0,ack_max=0;
    for (unsigned i=0;i<2;i++) {
        assert(consumed[i]==REPORTS);
        struct usb_hid *h=&m.host.devices[device_index[i]].hid[0];
        assert(h->latency_records==USB_HID_LATENCY_LIMIT);
        unsigned addr=m.host.devices[device_index[i]].address;
        assert(latency_prefix[addr]==USB_HID_LATENCY_LIMIT && latency_windows[addr]==1);
        for (unsigned j=0;j<REPORTS;j++) {
            struct sample *s=&samples[i][j];
            unsigned from_ack=(unsigned)(s->delivered-s->ack);
            unsigned from_arrival=(unsigned)(s->delivered-s->available);
            /* The acceptance bound begins at the controller report frame.
             * Nominal 1 ms service also bounds device availability -> queue. */
            assert(from_ack<=2u*h->interval);
            if (frame_ms==1 && !gap) assert(from_arrival<=2u*h->interval);
            if (from_ack>ack_max) ack_max=from_ack;
            if (from_arrival>arrival_max) arrival_max=from_arrival;
        }
    }
    struct input_stats stats;input_snapshot(&stats);assert(!stats.overflow);
    printf("PASS USB latency %s %s phase=%u frame_ms=%u dispatch_ms=%u period_ms=%u reports=%u arrival_max_ms=%u ack_max_ms=%u logged_bound_max_ms=%u\n",
           ohci ? "OHCI":"UHCI",scan ? "scan/reset":"bulk NAK",phase,frame_ms,gap+1,
           period,2*REPORTS,arrival_max,ack_max,recorded_bound);
    assert(recorded_bound>=ack_max);
    /* Stop pinned slots and release merged state before the next model. */
    for (unsigned i=0;i<m.host.device_count;i++) usb_detach(&m.host.devices[i],"fixture_end");
    while(input_read(&stale)) {}
}
int main(void)
{
    for (unsigned hc=0;hc<2;hc++) {
        for (unsigned phase=0;phase<10;phase++) fixture(hc,1,0,phase,false,40);
        fixture(hc,4,0,3,false,40);fixture(hc,1,9,3,false,40);fixture(hc,4,9,3,false,40);
        fixture(hc,1,0,3,true,40);
        /* F1-68: sustained reports at the actual 10 ms endpoint interval,
         * under BOT NAKs, rather than sparse isolated pointer samples. */
        fixture(hc,1,0,0,false,10);
    }
    puts("PASS USB latency through production merged queue, concurrent bulk, scan/reset, independent frame clock and delayed dispatch");
    return 0;
}
