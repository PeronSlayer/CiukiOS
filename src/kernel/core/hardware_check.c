/* H boot page: device tests in ordinary task context, no desktop process.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/task.h>
#include <ciuki/hardware_check.h>
#include <ciuki/storage.h>
#include <ciuki/usb.h>
#include <ciuki/input.h>
#include <ciuki/i8042.h>
#include <ciuki/timing.h>
#include <ciuki/display.h>
#include <ciuki/display_driver.h>
#include <ciuki/audio.h>
#include <ciuki/thinkpad_ec.h>
#include <ciuki/bootlog.h>
#include <ciuki/blackbox.h>
#include <ciuki/abi.h>
#include <ciuki/mm.h>
/* Host/older configurations may omit the recorder; the kernel links f0-25's
 * RAM producer. Only its validated descriptor admits raw disk writes. */
#pragma weak blackbox_capture
void hardware_check_cache(struct storage *, struct hw_check_result *);
int hardware_check_ata(struct storage *, struct hw_check_result *);
int hardware_check_repair(struct storage *, struct hw_check_result *);
static struct task *check_task;
static uint64_t io_until;
static char summary_page[2400];
static unsigned summary_bytes;
static bool collecting_summary;
static uint64_t check_now(void *ctx)
{
    (void)ctx;
    return g_cpu_tsc && g_tsc_per_ms ? read_tsc_serialized()/g_tsc_per_ms : deadline_after_ms(0);
}
bool hardware_check_io_cancelled(void)
{ return check_task && g_current==check_task && io_until && check_now(0)>=io_until; }
uint64_t hardware_check_io_deadline(void)
{
    if (!check_task || g_current!=check_task || !io_until) return 0;
    uint64_t now=check_now(0), left=io_until>now ? io_until-now : 0;
    return deadline_after_ms(left>UINT32_MAX ? UINT32_MAX : (uint32_t)left);
}
static void page_line(void *ctx,const char *line)
{
    (void)ctx;
    klog("[hwcheck] %s",line);
    if (!strncmp(line,"Hardware check complete",23)) collecting_summary=true;
    if (collecting_summary && summary_bytes<sizeof(summary_page)-1) {
        int n=ksnprintf(summary_page+summary_bytes,sizeof(summary_page)-summary_bytes,"%s\n",line);
        if (n>0 && (unsigned)n<sizeof(summary_page)-summary_bytes) summary_bytes+=(unsigned)n;
    }
    /* klog captures this exact prefixed line once through output.c. */
}
static bool usb_mouse_ready(void)
{
    for (unsigned i=0;i<usb_count();i++) {
        const struct usb_host *h=usb_get(i);
        if (!h || !h->ready || h->quarantined) continue;
        for (unsigned j=0;j<h->device_count;j++) {
            const struct usb_device *d=&h->devices[j];
            if (!d->present || d->quarantined) continue;
            for (unsigned k=0;k<USB_HID_MAX;k++)
                if (d->hid[k].protocol==2 && d->hid[k].endpoint && d->hid[k].active) return true;
        }
    }
    return false;
}
static void input_window(bool mouse,uint64_t until,struct hw_check_result *out)
{
    uint64_t start=check_now(0);
    if (mouse && usb_count()) {
        /* The ordinary USB owner may still be enumerating the receiver.
         * Leave five seconds for live events and two for reporting/cleanup. */
        uint64_t wait=start+8000;
        uint64_t budget=until>7000 ? until-7000 : 0;
        if (wait>budget) wait=budget;
        page_line(0,"Waiting for USB mouse enumeration (up to 8 s)");
        if (!hardware_check_usb_wait_first_scan(wait,out,check_now)) return;
        while (!usb_mouse_ready()) {
            uint64_t tick=check_now(0);
            if (tick>=wait) break;
            uint64_t left=wait-tick;
            task_sleep_ms(left<10 ? (uint32_t)left : 10);
        }
    }
    uint64_t enumerated=check_now(0)-start;
    unsigned interfaces=0, bad=0;
    if (mouse) for (unsigned i=0;i<usb_count();i++) {
        const struct usb_host *h=usb_get(i);
        if (!h || !h->ready || h->quarantined) continue;
        for (unsigned j=0;j<h->device_count;j++) {
            const struct usb_device *d=&h->devices[j];
            if (!d->present) continue;
            for (unsigned k=0;k<USB_HID_MAX;k++) {
                const struct usb_hid *hid=&d->hid[k];
                if (hid->protocol!=2 || !hid->endpoint) continue;
                interfaces++; bad+=!hid->active || d->quarantined;
                unsigned reports=hid->mouse_reports ? hid->mouse_reports : 1;
                for (unsigned r=0;r<reports;r++) {
                    const struct usb_mouse_report *m=&hid->mouse[r];
                    char msg[192];
                    ksnprintf(msg,sizeof(msg),"USB mouse %04x:%04x iface=%u protocol=%s id=%u buttons=%u X=%u:%u Y=%u:%u wheel=%u:%u active=%u",
                        d->vendor,d->product,hid->interface,hid->report_protocol ? "report" : "boot",m->id,
                        hid->mouse_reports ? m->button_count : 3,
                        hid->mouse_reports ? m->x.bit : 8,hid->mouse_reports ? m->x.size : 8,
                        hid->mouse_reports ? m->y.bit : 16,hid->mouse_reports ? m->y.size : 8,
                        m->wheel.bit,m->wheel.size,hid->active);
                    page_line(0,msg);
                    if (hid->report_protocol && (!m->x.size || !m->y.size)) bad++;
                }
            }
        }
    }
    struct i8042_stats ps2; i8042_snapshot(&ps2);
    if ((mouse && !interfaces) || (!mouse && !ps2.active && !audio.master_in_hardware)) {
        out->status=HW_SKIP; ksnprintf(out->detail,sizeof(out->detail),"device absent enumeration_ms=%llu",enumerated); return;
    }
    struct input_event stale;
    for (unsigned i=0;i<INPUT_CAPACITY && input_read(&stale);i++) {} /* live window starts with an empty queue */
    page_line(0,mouse ? "Move/click/scroll the USB mouse now (5 s)" : "Press PS/2 keyboard and EC volume keys now (5 s)");
    struct input_stats before,after; input_snapshot(&before);
    struct ciuki_mixer_master master=audio.master, last=master;
    uint64_t end=check_now(0)+5000;
    if (end>until-500) end=until-500;
    unsigned events=0, volume=0, displayed=0;
    uint64_t ec_next=0;
    while (check_now(0)<end) {
        /* f1-100: no periodic EC traffic; this user-run key window asks
         * for one rate-bounded EC read per second instead. */
        if (!mouse && audio.master_in_hardware && check_now(0)>=ec_next) {
            thinkpad_ec_refresh_request(); ec_next=check_now(0)+1000;
        }
        struct input_event e;
        for (unsigned count=0;count<INPUT_CAPACITY && input_read(&e);count++) {
            bool relevant=mouse ? e.source==INPUT_USB && (e.type==INPUT_REL || e.type==INPUT_BTN) :
                e.source==INPUT_NATIVE && e.type==INPUT_KEY;
            if (!relevant) continue;
            events++;
            if (displayed<96) {
                char msg[112];
                if (mouse && e.type==INPUT_REL)
                    ksnprintf(msg,sizeof(msg),"mouse axis=%s delta=%d tick=%llu",
                        e.code==INPUT_X ? "X" : e.code==INPUT_Y ? "Y" : "Wheel",e.value,e.tick);
                else if (mouse)
                    ksnprintf(msg,sizeof(msg),"mouse button=%s state=%s tick=%llu",
                        e.code==INPUT_LEFT ? "Left" : e.code==INPUT_RIGHT ? "Right" : "Middle",
                        e.value ? "down" : "up",e.tick);
                else ksnprintf(msg,sizeof(msg),"keyboard key=%04x state=%s tick=%llu",e.code,
                    e.value==2 ? "repeat" : e.value ? "down" : "up",e.tick);
                page_line(0,msg); displayed++;
            }
        }
        if (!mouse && (audio.master.left!=last.left || audio.master.right!=last.right || audio.master.mute!=last.mute)) {
            char msg[96]; volume++; last=audio.master;
            ksnprintf(msg,sizeof(msg),"EC volume left=%u right=%u mute=%u",last.left,last.right,last.mute); page_line(0,msg);
        }
        task_sleep_ms(1);
    }
    input_snapshot(&after);
    int restore=!mouse && volume ? audio_master_set(&master) : 0;
    out->status=bad || restore || end<start+enumerated+5000 || after.overflow!=before.overflow ? HW_FAIL : HW_PASS;
    ksnprintf(out->detail,sizeof(out->detail),"ready=%u enumeration_ms=%llu events=%u shown=%u EC=%u changes=%u lost=%llu restore=%d",
        mouse ? interfaces : ps2.active,enumerated,events,displayed,audio.master_in_hardware,volume,after.overflow-before.overflow,restore);
}
static void serial_check(uint64_t until,struct hw_check_result *out)
{
    struct serial_stats a,b; serial_snapshot(&a);
    if (!a.present) {
        out->status=HW_SKIP;
        ksnprintf(out->detail,sizeof(out->detail),"UART absent (loader step %u, kernel probe %u)",
            (unsigned)g_boot.uart_state,(unsigned)a.probe_result);
        return;
    }
    /* Wait for older records first; no forced UART mode or busy drain. */
    while (a.queued && check_now(0)<until-12000) { task_sleep_ms(1); serial_snapshot(&a); }
    uint64_t tickets[100]; unsigned accepted=0; uint64_t target=0;
    for (unsigned i=0;i<100;i++) {
        char record[64]; int n=ksnprintf(record,sizeof(record),"[hwcheck] serial record=%03u/100\n",i+1);
        /* Same formatted line in every sink; the returned ticket belongs
         * to this record even if IRQ/device producers interleave. */
        bootlog_capture(record,(size_t)n); console_write(record,(size_t)n);
        if (blackbox_capture) blackbox_capture(record,(size_t)n);
        uint32_t flags=serial_evidence_begin((size_t)n+1); /* final CRLF */
        tickets[i]=serial_write_ticket(record,(size_t)n);
        irq_restore(flags);
        if (tickets[i]) { accepted++; target=tickets[i]; }
    }
    serial_snapshot(&b);
    while (b.sent<target && check_now(0)<until-500) { task_sleep_ms(1); serial_snapshot(&b); }
    unsigned sent=0;
    for (unsigned i=0;i<100;i++) if (tickets[i] && tickets[i]<=b.sent) sent++;
    out->status=sent==100 && b.evidence_dropped==a.evidence_dropped && accepted==100 && b.resent==a.resent ? HW_PASS : HW_FAIL;
    ksnprintf(out->detail,sizeof(out->detail),"requested=100 accepted=%u whole_sent=%u resent=%u dropped=%u periodic=%u evidence=%u queued=%u",
        accepted,sent,b.resent-a.resent,b.dropped-a.dropped,
        b.periodic_dropped-a.periodic_dropped,b.evidence_dropped-a.evidence_dropped,b.queued);
}
/* One DM9 dispatch for both ordinary native drivers and completed sweeps.
 * Diagnostic submission never uses software replay; CPU safety and native
 * FILL/COPY evidence are independent conditions for a passing result. */
static void accelerator_compare(struct vfs *vfs,uint64_t until,struct hw_check_result *out)
{
    uint32_t tested=0;
    int e=fbdev_accel_diagnostic(vfs,until,0,0,&tested);
    struct ciuki_display_engine_info info;
    display_driver_info(&info);
    out->status=!e && (tested&3u)==3u && info.cpu_ready && !info.quarantined ? HW_PASS :
        e==-EOPNOTSUPP && info.cpu_ready && !info.quarantined ? HW_SKIP : HW_FAIL;
    ksnprintf(out->detail,sizeof(out->detail),"Native-only software-reference comparison: draw_mask=%x cursor_evidence=%u error=%d",
        tested&15u,!!(tested&DISPLAY_CHECK_CURSOR),e);
    page_line(0,out->detail);
}
static void accelerator_check(uint64_t until,struct hw_check_result *out)
{
    const struct fb_device original=*fbdev_get();
    const char *reason;
    if (display_accel_disabled(&reason)) { out->status=HW_SKIP; ksnprintf(out->detail,sizeof(out->detail),"2D disabled: %s",reason); return; }
    const struct display_driver *driver=display_driver_bind(&original);
    if (!driver) { out->status=HW_SKIP; ksnprintf(out->detail,sizeof(out->detail),"No native display driver"); return; }
    page_line(0,driver->name);
    struct storage *storage=storage_get();
    if (!driver->selftest) {
        accelerator_compare(&storage->vfs,until,out);
        return;
    }
    if (!driver->check_begin || !driver->attempt_end) {
        out->status=HW_FAIL;
        ksnprintf(out->detail,sizeof(out->detail),"Missing native display sweep authorization");
        return;
    }
    bool retained_marker=false;
    if (driver->check_begin(&storage->vfs,&retained_marker)) {
        out->status=HW_FAIL; ksnprintf(out->detail,sizeof(out->detail),"durable attempt unavailable/session quarantined"); return;
    }
    struct display_selftest_result a={.first=-1,.restored=true},b={.first=-1};
    int e=driver->selftest ? driver->selftest(deadline_after_ms(5000),page_line,0,&a) : -EOPNOTSUPP;
    /* A driver-specific configuration sweep is optional. Clean refusal
     * before any trial still permits the generic native-only comparison.
     * A failed/touched sweep or retained marker never authorizes activation. */
    if (e==-EOPNOTSUPP && !a.tried && a.restored) {
        if (retained_marker) {
            out->status=HW_SKIP;
            ksnprintf(out->detail,sizeof(out->detail),"Native comparison skipped: retained diagnostic marker preserved");
            return;
        }
        e=driver->attempt_end ? driver->attempt_end(&storage->vfs) : -EIO;
        if (!e) accelerator_compare(&storage->vfs,until,out);
        else {
            out->status=HW_FAIL;
            ksnprintf(out->detail,sizeof(out->detail),"Native comparison marker clear failed: error=%d",e);
            page_line(0,out->detail);
        }
        return;
    }
    char stage[176];
    ksnprintf(stage,sizeof(stage),"Native 2D original-depth result: bpp=%u first=%d passed=%u/%u done=%u skip=%u timeout=%u restored=%u error=%d",
        original.bpp,a.first,a.passed,a.tried,a.completed,a.skipped,a.timeouts,a.restored,e);
    page_line(0,stage); /* publish before making any firmware request */
    bool restored=a.restored, changed=false, sweep_started=false; int mode_error=0;
    if (!e && a.restored && a.first>=0 && check_now(0)<until-8000) {
        uint16_t mode=0;
        for (unsigned i=0;i<g_boot.display.mode_count && i<CBI_DISPLAY_MAX;i++) {
            const struct ciuki_boot_display_mode *m=&g_boot.display.modes[i];
            if (m->width==1024 && m->height==768 && m->bpp==16 && m->linear) { mode=m->id; break; }
        }
        if (mode && mode!=original.mode_id) {
            page_line(0,"Native 2D stage=16bpp-mode-set (display watchdog armed)");
            mode_error=display_check_begin(mode); changed=!mode_error;
            if (changed) {
                e=display_check_sweep_begin();
                if (!e) {
                    sweep_started=true;
                    page_line(0,"Native 2D stage=16bpp-sweep");
                    e=driver->selftest(deadline_after_ms(5000),page_line,0,&b);
                    display_check_sweep_end(b.restored);
                }
            }
            int restore=0;
            page_line(0,"Native 2D stage=desktop-restore");
            if (changed) restore=(!sweep_started || b.restored) ? display_check_end() : -EIO;
            else {
                /* begin already attempted rollback on a failed mode set.
                 * Do not retry a firmware backend that may be quarantined. */
                const struct fb_device *now=fbdev_get();
                if (!now->present || now->mode_id!=original.mode_id || now->pitch!=original.pitch ||
                    now->bpp!=original.bpp || now->phys!=original.phys) restore=-EIO;
            }
            if (restore) mode_error=restore;
            restored=restored && (!sweep_started || b.restored) && !restore;
        } else if (original.bpp==16) b=a;
        else mode_error=-EOPNOTSUPP;
    } else mode_error=e ? e : a.first<0 ? -EIO : -ETIMEDOUT;
    page_line(0,restored ? "Native 2D stage=desktop-restored" : "Native 2D stage=desktop-restore-failed");
    if (restored && !retained_marker) { int clear=driver->attempt_end(&storage->vfs); if (clear && !e) e=clear; }
    out->status=!e && !mode_error && a.passed && b.passed && restored ? HW_PASS : HW_FAIL;
    char first_a[12],first_b[12];
    if (a.first<0) ksnprintf(first_a,sizeof(first_a),"none");
    else ksnprintf(first_a,sizeof(first_a),"%d",a.first);
    if (b.first<0) ksnprintf(first_b,sizeof(first_b),"none");
    else ksnprintf(first_b,sizeof(first_b),"%d",b.first);
    ksnprintf(out->detail,sizeof(out->detail),"%ubpp first=%s %u/%u done=%u; 16bpp first=%s %u/%u done=%u skip=%u timeout=%u restore=%u e=%d mode=%d",
        original.bpp,first_a,a.passed,a.tried,a.completed,first_b,b.passed,b.tried,b.completed,
        a.skipped+(changed ? b.skipped : 0),a.timeouts+(changed ? b.timeouts : 0),restored,e,mode_error);
    if (a.first<0) page_line(0,"Native 2D first exact configuration: none (original depth)");
    if (b.first<0) page_line(0,"Native 2D first exact configuration: none (16 bpp)");
    if (out->status==HW_PASS && !retained_marker) {
        struct hw_check_result comparison;
        accelerator_compare(&storage->vfs,until,&comparison);
        if (comparison.status!=HW_PASS) out->status=HW_FAIL;
    } else if (retained_marker) {
        page_line(0,"Native comparison skipped: retained diagnostic marker preserved");
        if (out->status==HW_PASS) out->status=HW_SKIP;
    }

}
static void audio_check(struct hw_check_result *out)
{
    struct ciuki_audio_diagnostics d={.version=1,.size=sizeof(d)}; audio_diagnostics(&d);
    uint32_t frames=0;
    uint64_t underruns=audio.underruns;
    int e=audio.ready ? audio_check_duration(10,&frames) : audio.init_error;
    struct ciuki_audio_diagnostics after; audio_diagnostics(&after);
    out->status=audio.ready && !e && frames==10*AUDIO_RATE ? HW_PASS : audio.backend_touched ? HW_FAIL : HW_SKIP;
    ksnprintf(out->detail,sizeof(out->detail),"frames=%u underruns=%u irqs=%u lead=%u error=%d (listen)",
        frames,(unsigned)(audio.underruns-underruns),(unsigned)(after.irqs-d.irqs),audio.lead_periods,e);
}
static void check_step(void *ctx,unsigned item,uint64_t until,struct hw_check_result *out)
{
    (void)ctx;
    const struct hw_check_ops ops={0,check_now,check_step,page_line};
    io_until=until-1500; /* leave cleanup and result publication time */
    switch (item) {
    case 0: hardware_check_ata(storage_get(),out); break;
    case 1: hardware_check_repair(storage_get(),out); break;
    case 2: hardware_check_usb_storage(until,out,check_now); break;
    case 3: input_window(true,until,out); break;
    case 4: input_window(false,until,out); break;
    case 5: hardware_check_timer(until,false,out,&ops); break;
    case 6: hardware_check_timer(until,true,out,&ops); break;
    case 7: serial_check(until,out); break;
    case 8: accelerator_check(until,out); break;
    case 9: audio_check(out); break;
    case 11: hardware_check_cache(storage_get(),out); break;
    case 10: {
        bool ok=ledger_reconcile(); struct mm_ledger v; ledger_snapshot(&v);
        ledger_record("hardware-check"); out->status=ok ? HW_PASS : HW_FAIL;
        ksnprintf(out->detail,sizeof(out->detail),"NORMAL free=%u low=%u high=%u DMA free=%u errors=%u",
            v.zone[ZONE_NORMAL].free,v.low,v.high,v.zone[ZONE_DMA].free,v.mismatches);
        break;
    }
    }
    io_until=0;
}
extern void bootstat_presented(void) __attribute__((weak));
void hardware_check_run(void)
{
    static struct hw_check_result results[HW_CHECK_ITEMS];
    /* G14: the start reached its chosen page; it is not a failed start. */
    if (bootstat_presented) bootstat_presented();
    summary_bytes=0; collecting_summary=false;
    check_task=g_current; console_wake();
    if (!blackbox_capture) page_line(0,"Black box unavailable: f0-25 capture hook not linked");
    const struct hw_check_ops ops={0,check_now,check_step,page_line};
    hardware_check_model(&ops,results);
    hardware_check_smi_summary(&ops);
    struct bootlog_stats log; bootlog_snapshot(&log);
    char sinks[128];
    ksnprintf(sinks,sizeof(sinks),"Evidence: disk=%s dropped=%u error=%d; black box=%s",
        log.active ? "active" : "unavailable",log.dropped,log.error,blackbox_capture ? "linked" : "unavailable (f0-25)");
    page_line(0,sinks);
    /* All preceding results must reach the disk log before the page rests. */
    int synced=bootlog_try_sync(); uint64_t end=check_now(0)+2000;
    for (unsigned i=0;i<2000 && synced==-EAGAIN && check_now(0)<end;i++) {
        task_sleep_ms(1); synced=bootlog_try_sync();
    }
    if (synced || log.dropped) {
        ksnprintf(sinks,sizeof(sinks),"FAIL disk evidence sync=%d dropped=%u",synced,log.dropped);
        page_line(0,sinks);
    }
    console_pin_page(summary_page);
    check_task=0;
}
