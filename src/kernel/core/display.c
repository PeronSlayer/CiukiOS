/* f1-23: copied display inventory, serialized firmware transaction and
 * independent desktop-frame watchdog. See docs/design/f1-acceptance.md.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/display.h>
#include <ciuki/biosvm.h>
#include <ciuki/registry.h>
#include <ciuki/supervisor.h>
#include <ciuki/bootlog.h>
#include <ciuki/blackbox.h>
#include <ciuki/display_driver.h>
#include <ciuki/edid.h>
#include <ciuki/dos_video_modes.h>
static const struct display_mode_provider *fullscreen_provider;
static const struct display_mode_provider *present_provider;
static struct fb_device present_previous;
static void *fullscreen_window;
static struct display_lfb fullscreen_lfb;

static bool switching, awaiting_frame;
static bool awaiting_restored_frame;
static bool fullscreen_display;
static bool fullscreen_native;
static bool awaiting_native_frame;
static bool awaiting_present_frame;
static bool fullscreen_return_timed;
static bool fullscreen_log_failed;
static bool fullscreen_recovering;
static uint64_t fullscreen_return_tick;
/* T23 a79f840c: 156307 us registers, 1180 ms complete return with durable
 * checkpoints/capture. See the f3-36 amendment; recovery must never abort. */
#define NATIVE_RETURN_MS 2000u
static unsigned fullscreen_return_budget;
extern void dosvm_fullscreen_poll(void) __attribute__((weak));
extern bool display_present_active(void) __attribute__((weak));
extern int display_present_revoke_kernel(void) __attribute__((weak));
extern void display_present_poll(void) __attribute__((weak));
static uint32_t changes;
static uint64_t frame_deadline;
static struct fb_device previous;
static struct task *watchdog;
static bool check_display;
static struct fb_device check_previous;
static bool check_sweeping;
static uint64_t check_deadline;
static int check_result;
#define CHECK_MODE_MS 5000u

static bool checkpoint_cancel(void *arg) { (void)arg; return false; }
static void checkpoint_pause(void *arg) { (void)arg; task_sleep_ms(1); }
/* fx-22: the last step record, for step_ms and the stall report. */
static const char *step_name;
static uint64_t step_tick, step_reported;
static const struct task *step_task;
static unsigned recovery_sync_left;
#define RECOVERY_SYNC_STEP_MS 50u
#define RECOVERY_SYNC_TOTAL_MS 200u
#define STALL_REPORT_MS 1000u
#define STALL_REPEAT_MS 5000u
static bool recovery_sync_cancel(void *arg) { return deadline_passed(*(const uint64_t *)arg); }
int display_fullscreen_checkpoint(const char *step, int result)
{
    /* The emulated adapters have serial evidence and tight switch timings.
     * Hardware must commit the watermark before the next transaction step. */
    if (g_boot.flags & CBI_F_SMBIOS_QEMU) return 0;
    uint64_t now=g_ticks;
    klog("[display] fullscreen_step=%s result=%d tick=%llu step_ms=%llu",step,result,now,
         step_tick && now>=step_tick ? now-step_tick : 0);
    step_name=step; step_tick=now; step_task=g_current; step_reported=0;
    struct bootlog_stats stats;
    bootlog_snapshot(&stats);
    int e;
    if (fullscreen_recovering) {
        /* Recovery cannot depend on a slow/quarantined storage controller.
         * klog already queued both trails; the storage worker commits the
         * FAT watermark (and the raw trail). fx-22: wait for that commit at
         * most 50 ms per step and 200 ms per return, so the last durable
         * record names the step that did not finish (T23 f441c5bd ended at
         * a queued vga-capture); then it stays queued. EAGAIN means queued,
         * never a failed return checkpoint. */
        unsigned wait=recovery_sync_left<RECOVERY_SYNC_STEP_MS ? recovery_sync_left : RECOVERY_SYNC_STEP_MS;
        if (stats.active && wait) { /* thread context: every checkpoint caller */
            uint64_t began=g_ticks, until=deadline_after_ms(wait);
            e=bootlog_sync(recovery_sync_cancel,checkpoint_pause,&until);
            uint64_t spent=g_ticks-began;
            recovery_sync_left-=spent<recovery_sync_left ? (unsigned)spent : recovery_sync_left;
            if (e==-EBUSY) e=bootlog_try_sync();
        } else e=stats.active ? bootlog_try_sync() : -EIO;
        if (e==-EAGAIN) e=0;
    } else {
        e=stats.active ? bootlog_sync(checkpoint_cancel,checkpoint_pause,0) : -EIO;
        /* Entry must commit both trails before granting hardware access. */
        int raw=blackbox_checkpoint();
        if (!e) e=raw;
    }
    if (e && (!fullscreen_recovering || !fullscreen_log_failed)) {
        fullscreen_log_failed=true;
        klog("[display] fullscreen_log error=%d step=%s",e,step);
    }
    return e;
}

/* T23 c42f556e (docs/validation/t23-war2-fullscreen-2026-10-25.md): the
 * poll's first-guest-frame checkpoint waited the whole BOOT.LOG sync timeout
 * (2000 ms, -EAGAIN) in the display worker, which in a VBE full screen is the
 * only scanout transport: buffer flush and palette stood still meanwhile.
 * A running full screen's record is evidence, not a hardware step: queue it
 * and request the commit (the storage worker writes it shortly after). */
int display_fullscreen_record(const char *step, int result)
{
    if (g_boot.flags & CBI_F_SMBIOS_QEMU) return 0;
    uint64_t now=g_ticks;
    klog("[display] fullscreen_step=%s result=%d tick=%llu step_ms=%llu",step,result,now,
         step_tick && now>=step_tick ? now-step_tick : 0);
    step_name=step; step_tick=now; step_task=g_current; step_reported=0;
    (void)bootlog_try_sync();
    return 0;
}
/* T23 c42f556e: vm-park reported step_ms=4391, the idle time since the
 * previous return's desktop-restored step. An entry starts its own clock. */
void display_fullscreen_entry_request(void)
{
    step_name="entry-request"; step_tick=g_ticks; step_task=g_current; step_reported=0;
}

uint32_t display_change_counter(void) { return changes; }
static void changed(void) { if (!++changes) ++changes; }
void display_present_returned(void)
{
    changed();
    previous=*fbdev_get();
    awaiting_frame=true; awaiting_restored_frame=true; awaiting_native_frame=false;
    awaiting_present_frame=true;
    frame_deadline=deadline_after_ms(10000);
}

int display_monitor_query(struct ciuki_monitor_info *out)
{
    if(out->version!=CIUKI_MONITOR_VERSION || out->size!=sizeof(*out)) return -EINVAL;
    memset(out,0,sizeof(*out)); out->version=CIUKI_MONITOR_VERSION; out->size=sizeof(*out);
    if(g_boot.flags&CBI_F_EDID_VALID) {
        struct ciuki_monitor_info valid=*out;
        if(ciuki_edid_decode(g_boot.edid,&valid)) *out=valid;
    }
    return 0;
}
int display_refresh_query(struct ciuki_display_refresh *out)
{
    if(out->version!=CIUKI_MONITOR_VERSION || out->size!=sizeof(*out) || out->mode_id>UINT16_MAX) return -EINVAL;
    struct ciuki_display_info d={.version=CIUKI_DISPLAY_VERSION,.size=sizeof(d)};
    int e=display_query(&d); if(e) return e;
    uint32_t mode=out->mode_id;
    memset(out,0,sizeof(*out)); out->version=CIUKI_MONITOR_VERSION; out->size=sizeof(*out); out->mode_id=mode;
    for(unsigned i=0;i<d.mode_count;i++) if(d.modes[i].id==mode) {
        out->width=d.modes[i].width; out->height=d.modes[i].height; break;
    }
    if(!out->width) return -EINVAL;
    struct ciuki_monitor_info m={.version=CIUKI_MONITOR_VERSION,.size=sizeof(m)};
    e=display_monitor_query(&m); if(e) return e;
    for(unsigned i=0;i<m.timing_count;i++) if(m.timings[i].width==out->width && m.timings[i].height==out->height) {
        uint32_t rate=m.timings[i].refresh_millihz; unsigned j=0;
        while(j<out->supported_count && out->rates_millihz[j]!=rate) j++;
        if(j==out->supported_count) out->rates_millihz[out->supported_count++]=rate;
    }
    return 0;
}
int display_query(struct ciuki_display_info *out)
{
    if (out->version != CIUKI_DISPLAY_VERSION || out->size != sizeof(*out)) return -EINVAL;
    memset(out, 0, sizeof(*out));
    out->version = CIUKI_DISPLAY_VERSION; out->size = sizeof(*out);
    const struct fb_device *f = fbdev_get();
    out->width = f->width; out->height = f->height; out->pitch = f->pitch; out->bpp = f->bpp;
    const struct ciuki_boot_display *b = &g_boot.display;
    if (g_boot.version >= 2 && b->tag == CBI_DISPLAY_TAG && b->size == sizeof(*b)) {
        out->vram_bytes = b->vram_bytes; out->vbe_version = b->vbe_version;
        memcpy(out->oem, b->oem, 64); memcpy(out->vendor, b->vendor, 64); memcpy(out->product, b->product, 64);
        out->mode_count = b->mode_count;
        if (out->mode_count > CBI_DISPLAY_MAX) out->mode_count = CBI_DISPLAY_MAX;
        for (unsigned i = 0; i < out->mode_count; i++) {
            const struct ciuki_boot_display_mode *m = &b->modes[i];
            out->modes[i] = (struct ciuki_display_mode){m->id, m->width, m->height, m->bpp,
                m->pitch, m->linear, f->present && m->id == f->mode_id};
        }
    } else if (g_boot.flags & CBI_F_VBE_CTRL_VALID) {
        out->vbe_version = g_boot.vbe_ctrl[4] | (uint32_t)g_boot.vbe_ctrl[5] << 8;
        out->vram_bytes = (g_boot.vbe_ctrl[18] | (uint32_t)g_boot.vbe_ctrl[19] << 8) << 16;
        /* v1 strings contain far pointers: never dereference them here. */
    }
    for (unsigned i = 0; i < pci_count(); i++) {
        const struct pci_func *p = pci_get(i);
        if (p && p->class_code == 3) {
            out->pci_vendor = p->vendor; out->pci_device = p->device;
            out->pci_class = p->class_code; out->pci_subclass = p->subclass; break;
        }
    }
    return 0;
}

static const struct ciuki_boot_display_mode *listed(uint16_t id)
{
    if (g_boot.version < 2 || g_boot.display.tag != CBI_DISPLAY_TAG) return 0;
    for (unsigned i = 0; i < g_boot.display.mode_count && i < CBI_DISPLAY_MAX; i++)
        if (g_boot.display.modes[i].id == id) return &g_boot.display.modes[i];
    return 0;
}

static void log_mode(uint16_t mode, int result)
{
    const struct fb_device *f = fbdev_get();
    klog("[display] mode=%u %ux%ux%u pitch=%u result=%d", mode, f->width, f->height, f->bpp, f->pitch, result);
}

static int refuse(uint16_t mode, int e) { log_mode(mode, e); return e; }

static int mode_failure(uint16_t id, const char *stage, int error)
{
    klog("[display] mode_failure mode=%u stage=%s error=%d",id,stage,error);
    return -EIO;
}
static int apply(uint16_t id, struct fb_device *out)
{
    uint8_t info[256];
    int e = biosvm_video_switch(id, info);
    if (e) return mode_failure(id,"bios-switch",e);
    e=fbdev_mode_decode(id, info, out);
    if (e) return mode_failure(id,"mode-decode",e);
    const struct ciuki_boot_display_mode *m = listed(id);
    if (!m || m->width != out->width || m->height != out->height || m->bpp != out->bpp)
        return mode_failure(id,"mode-inventory",-EINVAL);
    e=fbdev_change_commit(out);
    return e ? mode_failure(id,"framebuffer-commit",e) : 0;
}

static int restore(void)
{
    struct fb_device restored;
    int e = apply(previous.mode_id, &restored);
    if (!e) changed();
    log_mode(previous.mode_id, e);
    return e;
}
static int check_restore(void)
{
    struct fb_device restored;
    int e = apply(check_previous.mode_id, &restored);
    if (!e && (restored.width != check_previous.width || restored.height != check_previous.height ||
        restored.pitch != check_previous.pitch || restored.bpp != check_previous.bpp ||
        restored.phys != check_previous.phys)) e = -EIO;
    if (!e) changed();
    if (e) check_result = e;
    klog("[display] check_step=restored mode=%u error=%d",check_previous.mode_id,e);
    return e;
}
int display_check_begin(uint16_t id)
{
    if (!(g_boot.flags & CBI_F_HW_CHECK) || grants_display_pid()) return -EPERM;
    if (check_display || switching || fullscreen_display || present_provider || awaiting_frame) return -EBUSY;
    if (!listed(id) || !listed(fbdev_get()->mode_id)) return -EOPNOTSUPP;
    if (!fbdev_get()->present) return -ENODEV;
    if (display_worker_start()) return -ENOMEM;
    check_previous = *fbdev_get();
    check_result = 0;
    int e = biosvm_video_prepare();
    if (e) return e;
    e = fbdev_change_begin();
    if (e) return e;
    switching = true;
    check_display = true; /* includes restoration of a failed mode set */
    check_deadline = deadline_after_ms(CHECK_MODE_MS);
    klog("[display] check_step=mode-set mode=%u",id);
    struct fb_device next;
    e = apply(id, &next);
    if (e) {
        /* The old mapping is already revoked: ending this transaction and
         * starting another would fail fbdev_change_begin's live() gate. */
        check_result=e;
        klog("[display] check_step=rollback mode=%u error=%d",check_previous.mode_id,e);
        (void)check_restore();
        check_display=false;
    }
    fbdev_change_end();
    switching = false;
    klog("[display] check_step=mode-set-done mode=%u error=%d",id,e);
    if (e) return e;
    if (deadline_passed(check_deadline)) {
        check_result = -ETIMEDOUT;
        display_check_end();
        return check_result;
    }
    changed(); return 0;
}
int display_check_sweep_begin(void)
{
    display_watchdog_poll();
    uint32_t flags=irq_save();
    int e=0;
    if (!check_display) e=check_result ? check_result : -EINVAL;
    else if (switching || check_sweeping) e=-EBUSY;
    else if (deadline_passed(check_deadline)) e=-ETIMEDOUT;
    else check_sweeping = true;
    irq_restore(flags);
    return e;
}
void display_check_sweep_end(bool restored)
{
    uint32_t flags=irq_save();
    check_sweeping = false;
    if (!restored) {
        /* The sweep retains quarantined VGA/engine leases. Firmware must
         * never run underneath an unproven writer, even on watchdog expiry. */
        check_result = -EIO;
        check_display = false;
    }
    irq_restore(flags);
    if (!restored) klog("[display] check_step=restore-refused reason=unsafe-writer");
    else display_watchdog_poll();
}
int display_check_end(void)
{
    uint32_t flags=irq_save();
    if (!check_display || switching || check_sweeping) {
        int e=check_display ? -EBUSY : check_result;
        irq_restore(flags); return e;
    }
    switching = true;
    irq_restore(flags);
    klog("[display] check_step=restore mode=%u",check_previous.mode_id);
    int e = fbdev_change_begin();
    if (!e) {
        e = check_restore();
        fbdev_change_end();
    }
    /* One bounded restoration attempt. A failed firmware backend may be
     * quarantined; the watchdog must not retry it every 100 ms. */
    check_display = false; switching = false;
    if (e) check_result = e;
    return check_result;
}

extern void display_driver_desktop_presented(uint32_t) __attribute__((weak));
void display_frame_complete(uint32_t generation, bool full_frame)
{
    if (full_frame && generation==fbdev_get()->generation && display_driver_desktop_presented)
        display_driver_desktop_presented(generation);
    if (!switching && awaiting_frame && (full_frame || awaiting_restored_frame) &&
        generation == fbdev_get()->generation && !deadline_passed(frame_deadline)) {
        if (awaiting_native_frame && fullscreen_return_timed)
            klog("[display] native_driver desktop_frame_ms=%llu budget_ms=%u overrun=%u",
                g_ticks-fullscreen_return_tick,fullscreen_return_budget,
                (unsigned)(g_ticks-fullscreen_return_tick>fullscreen_return_budget));
        awaiting_frame = false;
        awaiting_native_frame = false;
        awaiting_present_frame = false;
        awaiting_restored_frame = false;
    }
}
void display_fullscreen_return_request(void)
{
    /* Input/stop paths can call from IRQ context: only publish the clock. */
    if (fullscreen_display && !fullscreen_return_timed) {
        fullscreen_return_tick=g_ticks; fullscreen_return_timed=true;
        fullscreen_recovering=true; fullscreen_log_failed=false;
        recovery_sync_left=RECOVERY_SYNC_TOTAL_MS;
        step_name="return-request"; step_tick=g_ticks; step_task=0; step_reported=0;
    }
}
/* fx-22: called each second by the device-level cpustat task. A return
 * step that lasts STALL_REPORT_MS is recorded with the task running the
 * switch, again every STALL_REPEAT_MS, and its commit requested. */
void display_fullscreen_stall_poll(void)
{
    if (!fullscreen_recovering || !step_name || g_boot.flags & CBI_F_SMBIOS_QEMU) return;
    uint64_t now=g_ticks, held=now-step_tick;
    if (held<STALL_REPORT_MS || (step_reported && now-step_reported<STALL_REPEAT_MS)) return;
    step_reported=now;
    const struct task *t=step_task;
    klog("[display] fullscreen_stall step=%s step_ms=%llu return_ms=%llu task=%s#%u state=%u level=%u",
         step_name,held,now-fullscreen_return_tick,t ? t->name : "none",t ? t->id : 0,
         t ? (unsigned)t->state : 0,t ? (unsigned)t->effective_priority : 0);
    (void)bootlog_try_sync();
}

void display_watchdog_poll(void)
{
    if (display_present_poll) display_present_poll();
    if (dosvm_fullscreen_poll) dosvm_fullscreen_poll();
    if (display_present_active && display_present_active()) return;
    if (fullscreen_display) return;
    if (check_display) {
        if (deadline_passed(check_deadline)) {
            check_result = -ETIMEDOUT;
            if (!switching && !check_sweeping) (void)display_check_end();
        }
        return;
    }
    if (!awaiting_frame || switching || !deadline_passed(frame_deadline) || grants_display_busy()) return;
    switching = true;
    awaiting_frame = false;
    awaiting_restored_frame = false;
    if (awaiting_native_frame || awaiting_present_frame) {
        /* Native and present-lease returns already verified scanout. A
         * missing desktop repaint cannot authorize another BIOS mode set. */
        if (awaiting_present_frame) klog("[display] present_lease desktop_frame_timeout");
        else klog("[display] native_driver desktop_frame_timeout");
        awaiting_native_frame=false;
        awaiting_present_frame=false;
    } else if (!fbdev_change_begin()) { restore(); fbdev_change_end(); }
    switching = false;
}
extern void w32_render3d_poll(void) __attribute__((weak));
static struct kwait worker_requests;
static bool worker_requested;
static bool worker_has_request(void *arg)
{ (void)arg; return worker_requested; }
void display_worker_wake(void)
{
    uint32_t flags=irq_save();
    worker_requested=true;
    irq_restore(flags);
    kwait_wake_all(&worker_requests);
}
static void watchdog_main(void *arg)
{
    (void)arg;
    for (;;) {
        if (w32_render3d_poll) w32_render3d_poll();
        display_watchdog_poll();
        (void)kwait_wait_until(&worker_requests,worker_has_request,0,
            deadline_after_ms(fullscreen_display || awaiting_native_frame ||
                (display_present_active && display_present_active()) ? 10 : 100));
        uint32_t flags=irq_save();
        worker_requested=false;
        irq_restore(flags);
    }
}
/* f3-21: the worker also services queued DOS full-screen requests
 * (dosvm_fullscreen_poll); the input filter cannot create it from IRQ
 * context, so window VM creation starts it in thread context. Idempotent. */
int display_worker_start(void)
{
    if (watchdog) return 0;
    watchdog = task_create_kernel("display-watchdog", watchdog_main, 0, P_NORMAL);
    if (!watchdog) return -ENOMEM;
    task_start(watchdog);
    return 0;
}

int display_set_for_present(struct process *p, uint16_t id, bool delegated)
{
    if (!p || p->pid != supervisor_desktop_pid() || !grants_display_owner(p)) return refuse(id, -EPERM);
    const struct ciuki_boot_display_mode *m = listed(id);
    if (!m) return refuse(id, -EINVAL);
    /* Window DOS VMs currently have private surfaces, never physical grants.
     * A competing/foreign display owner or an in-flight frame prevents entry. */
    if (switching || fullscreen_display || check_display || awaiting_frame || grants_display_busy()) return refuse(id, -EBUSY);
    if (!fbdev_get()->present) return refuse(id, -ENODEV);
    if (!listed(fbdev_get()->mode_id)) return refuse(id, -EOPNOTSUPP); /* cannot promise restore */
    const struct fb_device *f=fbdev_get();
    /* Firmware mode numbers can alias one scanout layout. In particular a
     * lease of the boot layout must work even when firmware is unavailable. */
    if (m->width==f->width && m->height==f->height && m->bpp==f->bpp && m->pitch==f->pitch)
        return refuse(id, 0);
    const struct display_mode_provider *provider=present_provider;
    bool returning=provider!=0;
    if (returning && id!=present_previous.mode_id) return refuse(id,-EBUSY);
    if (!returning && delegated) {
        int binding=display_driver_mode_bind(f,&provider);
        if (binding) return refuse(id,binding);
        if (provider && (!provider->publish || (m->bpp!=16 && m->bpp!=32) || !provider->supported(f,id)))
            provider=0;
        if (provider) {
            int ready=provider->prepare(f,id);
            if (ready==-EOPNOTSUPP) provider=0; /* clean software refusal */
            else if (ready) return refuse(id,ready);
        }
    }
    if (display_worker_start()) return refuse(id, -ENOMEM);
    switching = true;
    previous = *fbdev_get();
    int e = provider ? 0 : biosvm_video_prepare();
    if (e) { switching = false; return refuse(id, mode_failure(id,"bios-prepare",e)); }
    if (grants_display_busy()) { switching = false; return refuse(id, -EBUSY); }
    e = fbdev_change_begin();
    if (e) mode_failure(id,"framebuffer-begin",e);
    if (!e) {
        struct fb_device next;
        if (!provider) {
            e=apply(id,&next);
            if (e) restore();
        } else if (returning) {
            e=provider->end();
            if (!e) e=fbdev_change_commit(&present_previous);
            if (e && !biosvm_video_prepare()) e=apply(id,&next);
            provider->complete(!e);
            if (!e) present_provider=0;
        } else {
            e=provider->begin(id);
            if (e==-EOPNOTSUPP) {
                int released=provider->end();
                if (!released) {
                    provider->complete(true);
                    provider=0;
                    e=biosvm_video_prepare();
                    if (!e) e=apply(id,&next);
                    if (e) restore();
                } else e=released;
            }
            if (!e && provider) e=provider->publish(&next);
            if (!e && provider && (next.width!=m->width || next.height!=m->height ||
                next.bpp!=m->bpp || next.pitch!=m->pitch)) e=-EIO;
            if (!e && provider) e=fbdev_change_commit(&next);
            if (!e && provider) {
                present_previous=previous;
                present_provider=provider;
            } else if (e && provider) {
                int recovered=provider->end();
                if (!recovered) recovered=fbdev_change_commit(&previous);
                if (recovered && !biosvm_video_prepare()) recovered=restore();
                provider->complete(!recovered);
                if (recovered) {
                    present_previous=previous;
                    present_provider=provider; /* retain recovery ownership */
                    e=recovered;
                }
            }
        }
        if (!e) {
            changed();
            awaiting_frame = !delegated;
            awaiting_restored_frame = false;
            awaiting_native_frame = false;
            awaiting_present_frame = false;
            frame_deadline = deadline_after_ms(10000);
        }
        fbdev_change_end();
    }
    switching = false;
    if (e) e = -EIO;
    log_mode(id, e);
    return e;
}

int display_set(struct process *p, uint16_t id)
{
    if (!p || p->pid != supervisor_desktop_pid() || !grants_display_owner(p)) return refuse(id,-EPERM);
    bool reclaimed=false;
    if (display_present_revoke_kernel) {
        int e=display_present_revoke_kernel();
        if (e<0) return refuse(id,e);
        reclaimed=e>0;
        if (reclaimed && id==fbdev_get()->mode_id) return refuse(id,0);
        if (reclaimed && listed(id)) awaiting_frame=false; /* new set supersedes the return repaint */
    }
    int e=display_set_for_present(p,id,false);
    if (e && reclaimed && fbdev_get()->present) display_present_returned();
    return e;
}

/* G10: policy state is shared by native and Win32 clients. No firmware under
 * the IRQ guard; CONFIG/STATUS and real DPMS require the desktop authority. */
static struct ciuki_screen_control screen_policy = {
    .version=CIUKI_SCREEN_VERSION,.size=sizeof(struct ciuki_screen_control),
    .timeout=600,.saver=1
};
static int screen_control(struct process *p, struct ciuki_screen_control *q)
{
    if (q->version!=CIUKI_SCREEN_VERSION || q->size!=sizeof(*q) ||
        (q->flags&~CIUKI_SCREEN_PERSIST)) return -EINVAL;
    bool desktop=p && p->pid==supervisor_desktop_pid();
    if ((q->action==CIUKI_SCREEN_CONFIG || q->action==CIUKI_SCREEN_STATUS) && !desktop) return -EPERM;
    uint32_t action=q->action;
    bool inhibited=fullscreen_display || (display_present_active && display_present_active());
    if (action==0xf140 && inhibited) return -EBUSY;
    int32_t old=0;
    uint32_t irq=irq_save();
    switch (action) {
    case 0: break;
    case CIUKI_SCREEN_CONFIG:
        if (q->enabled>1 || q->saver>2 || q->timeout>INT32_MAX || q->monitor_seconds>INT32_MAX) goto invalid;
        screen_policy.enabled=q->enabled; screen_policy.timeout=q->timeout;
        screen_policy.saver=q->saver; screen_policy.monitor_seconds=q->monitor_seconds;
        break;
    case CIUKI_SCREEN_STATUS:
        if (q->value<0 || q->value>1) goto invalid;
        screen_policy.running=(uint32_t)q->value;
        /* Acknowledge persistence only for the revision the desktop read. */
        if (q->sequence==screen_policy.sequence) { screen_policy.flags=0; screen_policy.command=0; }
        break;
    case 15:
        if (q->value<0) goto invalid;
        screen_policy.timeout=(uint32_t)q->value; break;
    case 17: screen_policy.enabled=q->value!=0; break;
    case 97:
        old=(int32_t)screen_policy.running; screen_policy.running=q->value!=0; break;
    case 0xf140:
        if (q->value < -1 || q->value>2) goto invalid;
        screen_policy.command=action; screen_policy.argument=q->value; break;
    case 0xf170:
        if (q->value!=-1 && q->value!=1 && q->value!=2) goto invalid;
        screen_policy.command=action; screen_policy.argument=q->value; break;
    default: goto invalid;
    }
    if (action && action!=CIUKI_SCREEN_STATUS) {
        if (!++screen_policy.sequence) ++screen_policy.sequence;
        screen_policy.flags|=q->flags;
    }
    *q=screen_policy; q->value=old; q->inhibited=inhibited;
    irq_restore(irq); return 0;
invalid:
    irq_restore(irq); return -EINVAL;
}

static bool monitor_asleep;
static struct fb_device monitor_previous;
static int monitor_power(struct process *p, int state)
{
    if (!p || p->pid!=supervisor_desktop_pid() || !grants_display_owner(p)) return -EPERM;
    if (state!=-1 && state!=1 && state!=2) return -EINVAL;
    if (fullscreen_display || check_display || awaiting_frame ||
        (display_present_active && display_present_active()) || (switching && !monitor_asleep)) return -EBUSY;
    if (state==-1 && !monitor_asleep) return 0;
    if (state!=-1 && monitor_asleep) return 0;
    int e=biosvm_video_prepare();
    if (e) return e;
    if (!monitor_asleep) {
        monitor_previous=*fbdev_get();
        e=grants_display_suspend(true);
        if (e) return e;
        e=fbdev_change_begin();
        if (e) { (void)grants_display_suspend(false); return e; }
        switching=true;
    }
    e=biosvm_video_dpms(state==-1 ? 0 : state==1 ? 1 : 4);
    if (!e && state!=-1) { monitor_asleep=true; return 0; }
    /* Never discard the suspended transaction on a failed wake: input can
     * retry it. Failed entry restores the on state before resuming drawing. */
    if (e && monitor_asleep) return e;
    if (e) (void)biosvm_video_dpms(0);
    int restored=fbdev_change_commit(&monitor_previous);
    if (restored) { monitor_asleep=true; return restored; }
    monitor_asleep=false; switching=false;
    fbdev_change_end(); (void)grants_display_suspend(false); changed();
    return e;
}

int32_t display_syscall(struct trap_frame *tf)
{
    if (tf->edx || tf->esi || tf->edi || tf->ebp) return -EINVAL;
    if(tf->ebx==CIUKI_DISPLAY_MONITOR) {
        struct ciuki_monitor_info q;
        int e=copy_from_user(&q,tf->ecx,8);
        if(!e) e=display_monitor_query(&q);
        return e ? e : copy_to_user(tf->ecx,&q,sizeof(q));
    }
    if(tf->ebx==CIUKI_DISPLAY_REFRESH) {
        struct ciuki_display_refresh q;
        int e=copy_from_user(&q,tf->ecx,12);
        if(!e) e=display_refresh_query(&q);
        return e ? e : copy_to_user(tf->ecx,&q,sizeof(q));
    }
    if (tf->ebx == CIUKI_DISPLAY_SCREEN) {
        struct ciuki_screen_control q;
        int e=copy_from_user(&q,tf->ecx,sizeof(q));
        if (!e) e=screen_control(proc_current(),&q);
        return e ? e : copy_to_user(tf->ecx,&q,sizeof(q));
    }
    if (tf->ebx == CIUKI_DISPLAY_DPMS) return monitor_power(proc_current(),(int32_t)tf->ecx);
    if (tf->ebx == CIUKI_DISPLAY_SET) {
        if (tf->ecx > UINT16_MAX) return -EINVAL;
        return display_set(proc_current(), (uint16_t)tf->ecx);
    }
    if (tf->ebx != CIUKI_DISPLAY_QUERY) return -EINVAL;
    struct ciuki_display_info out;
    /* Only the header is input. Bad size/version never reads/writes modes. */
    int e = copy_from_user(&out, tf->ecx, 8);
    if (e) return e;
    e = display_query(&out);
    return e ? e : copy_to_user(tf->ecx, &out, sizeof(out));
}

bool display_fullscreen_active(void) { return fullscreen_display || present_provider; }
bool display_fullscreen_supported(uint16_t mode)
{
    const struct display_mode_provider *p = display_driver_modes(fbdev_get());
    return (p && p->supported(fbdev_get(),mode)) ||
        (dos_video_mode(mode) && fbdev_get()->present && fbdev_get()->size>=DOS_VBE_BYTES);
}
int display_fullscreen_write(uint32_t offset,const void *data,uint32_t bytes)
{
    if (!fullscreen_display || switching) return -EIO;
    if (fullscreen_native) return fullscreen_provider->write(offset,data,bytes);
    if (!fullscreen_lfb.mapped) return -EIO;
    if (!data || offset>fullscreen_lfb.bytes || bytes>fullscreen_lfb.bytes-offset) return -EINVAL;
    const uint8_t *src=data;
    for (uint32_t i=0;i<bytes;i++) fullscreen_lfb.mapped[offset+i]=src[i];
    return 0;
}
int display_fullscreen_lfb(struct display_lfb *out)
{
    if (!out) return -EINVAL;
    memset(out,0,sizeof(*out));
    if (!fullscreen_display || switching) return -EOPNOTSUPP;
    if (fullscreen_native && fullscreen_provider && fullscreen_provider->lfb) return fullscreen_provider->lfb(out);
    if (!fullscreen_lfb.mapped) return -EOPNOTSUPP;
    *out=fullscreen_lfb;
    return 0;
}
unsigned display_fullscreen_dac_bits(void)
{
    if (!fullscreen_display || switching) return 0;
    /* VBE mode setting restores six-bit precision (VBE 2.0, 4.2/4.11).
     * Native providers describe the precision their packed setup verifies. */
    return fullscreen_native ? fullscreen_provider->indexed_dac_bits : 6;
}
static int fullscreen_map(uint16_t mode,const uint8_t info[256])
{
    const struct dos_video_mode *m=dos_video_mode(mode);
    unsigned attributes=(unsigned)info[0]|(unsigned)info[1]<<8;
    unsigned width=(unsigned)info[18]|(unsigned)info[19]<<8;
    unsigned height=(unsigned)info[20]|(unsigned)info[21]<<8;
    unsigned pitch_at=g_boot.display.vbe_version>=0x300 ? 50 : 16;
    unsigned pitch=(unsigned)info[pitch_at]|(unsigned)info[pitch_at+1]<<8;
    uint32_t physical=(uint32_t)info[40]|(uint32_t)info[41]<<8|(uint32_t)info[42]<<16|(uint32_t)info[43]<<24;
    /* Borrow only the stopped desktop's exact aperture. Firmware pointers
     * never authorize RAM or another device, and the guest layout is fixed. */
    const struct resource *r=registry_get(previous.handle);
    if (!m || (attributes&0x91)!=0x91 || info[24]!=1 || info[25]!=m->bpp ||
        info[27]!=(m->bpp==8 ? 4 : 6) || width!=m->width || height!=m->height || pitch!=m->pitch ||
        physical!=previous.phys || (physical&(PAGE_SIZE-1)) || previous.size<DOS_VBE_BYTES ||
        !r || r->generation!=previous.generation || r->state!=RS_QUIESCING || r->type!=RES_MMIO ||
        r->start!=physical || r->end-r->start<DOS_VBE_BYTES) return -EOPNOTSUPP;
    if (m->bpp==16) {
        unsigned masks=g_boot.display.vbe_version>=0x300 ? 54 : 31;
        if (info[masks]!=5 || info[masks+1]!=11 || info[masks+2]!=6 ||
            info[masks+3]!=5 || info[masks+4]!=5 || info[masks+5]!=0 || info[masks+6]!=0)
            return -EOPNOTSUPP;
    }
    volatile uint8_t *mapped=vmm_replace_framebuffer(fullscreen_window,DOS_VBE_BYTES,physical,DOS_VBE_BYTES);
    if (!mapped) return -ENOMEM;
    uint32_t cache=0;
    if (!vmm_cache_alias_ok(physical,DOS_VBE_BYTES,cache)) {
        cache=PTE_PCD|PTE_PWT;
        if (!vmm_cache_alias_ok(physical,DOS_VBE_BYTES,cache)) {
            vmm_revoke_mmio((void *)mapped,DOS_VBE_BYTES);
            return -EIO;
        }
    }
    fullscreen_lfb=(struct display_lfb){physical,DOS_VBE_BYTES,pitch,m->width,m->height,m->bpp,cache,mapped};
    return 0;
}
static int fullscreen_windowed(int error)
{
    klog("[display] fullscreen=0 continuation=windowed error=%d",error);
    return DISPLAY_FULLSCREEN_WINDOWED;
}
int display_fullscreen_begin(uint16_t mode)
{
    bool reclaimed=false;
    if (display_present_revoke_kernel) {
        int e=display_present_revoke_kernel();
        if (e<0) return fullscreen_windowed(e);
        reclaimed=e>0;
    }
    int logged=display_fullscreen_checkpoint("request",0);
    if (logged) return fullscreen_windowed(logged);
    struct process *desktop=proc_find(grants_display_pid());
    const char *why=0;
    if (!desktop) why="no-display-owner";
    else if (desktop->state>=PROC_STOPPING) why="desktop-stopping";
    else if (!grants_display_owner(desktop)) why="owner-without-writable-display";
    if (why) {
        klog("[display] fullscreen_refused reason=%s owner_pid=%u mode=%u",why,grants_display_pid(),mode);
        return fullscreen_windowed(-EPERM);
    }
    if (switching || fullscreen_display || present_provider || (awaiting_frame && !reclaimed) || grants_display_busy()) {
        klog("[display] fullscreen_refused reason=busy switching=%u active=%u awaiting_frame=%u presenting=%u",
            (unsigned)switching,(unsigned)fullscreen_display,(unsigned)awaiting_frame,(unsigned)grants_display_busy());
        return fullscreen_windowed(-EBUSY);
    }
    if (!fbdev_get()->present || !listed(fbdev_get()->mode_id)) {
        klog("[display] fullscreen_refused reason=unsupported present=%u mode_id=%u",
            (unsigned)fbdev_get()->present,(unsigned)fbdev_get()->mode_id);
        return fullscreen_windowed(-EOPNOTSUPP);
    }
    const struct display_mode_provider *provider=0;
    int binding=display_driver_mode_bind(fbdev_get(),&provider);
    if (binding) return fullscreen_windowed(binding);
    bool native = provider != 0;
    if (native) {
        int e = provider->prepare(fbdev_get(),mode);
        if (e==-EOPNOTSUPP) { provider=0; native=false; }
        else if (e) {
            klog("[display] fullscreen_refused reason=native-recovery-unavailable mode=%u error=%d",mode,e);
            display_fullscreen_checkpoint("refused",e);
            return fullscreen_windowed(e);
        }
    }
    if (dos_video_mode(mode) && !fullscreen_window) {
        fullscreen_window=vmm_reserve_mmio(DOS_VBE_BYTES);
        if (!fullscreen_window) return fullscreen_windowed(-ENOMEM);
    }
    if (display_worker_start()) return fullscreen_windowed(-ENOMEM);
    if (!native && mode>0x7F && !dos_video_mode(mode)) return fullscreen_windowed(-EOPNOTSUPP);
    switching=true;
    previous=*fbdev_get();
    int e=native ? 0 : biosvm_video_prepare();
    if (e) { switching=false; return fullscreen_windowed(-EIO); }
    e=display_fullscreen_checkpoint("grant-suspend",0);
    if (e) { switching=false; return fullscreen_windowed(e); }
    e=grants_display_suspend(true);
    if (e) { switching=false; return fullscreen_windowed(e); }
    e=display_fullscreen_checkpoint("desktop-revoke",0);
    if (e) { grants_display_suspend(false); switching=false; return fullscreen_windowed(e); }
    e=fbdev_change_begin();
    if (e) { grants_display_suspend(false); switching=false; return fullscreen_windowed(e); }
    awaiting_frame=false; /* the DOS transfer now supersedes the returned lease's repaint */
    fullscreen_native=native;
    fullscreen_provider=provider;
    fullscreen_return_budget=NATIVE_RETURN_MS;
    fullscreen_return_timed=false;
    if (native) {
        e=provider->begin(mode);
        if (e==-EOPNOTSUPP && !provider->end()) {
            provider->complete(true);
            fullscreen_provider=provider=0;
            fullscreen_native=native=false;
            e=0;
        }
    }
    if (!native) {
        uint8_t info[256];
        e=biosvm_video_fullscreen_begin(mode,info);
        if (!e && dos_video_mode(mode)) e=fullscreen_map(mode,info);
    }
    fullscreen_display=true; /* also brackets rollback of a partially set mode */
    switching=false;
    if (e) {
        if (native && e==-EOPNOTSUPP)
            klog("[display] fullscreen_refused reason=native-recovery-unavailable mode=%u",mode);
        int restored=display_fullscreen_end();
        return restored ? restored : fullscreen_windowed(e);
    }
    klog("[display] fullscreen=1 mode=%u desktop_mode=%u provider=%s",mode,previous.mode_id,native ? "native-driver" : "firmware-video");
    return 0;
}
int display_fullscreen_end(void)
{
    if (!fullscreen_display) return 0;
    if (switching) return -EBUSY;
    switching=true;
    display_fullscreen_return_request();
    /* Logging failure must never prevent an already needed recovery. */
    display_fullscreen_checkpoint("desktop-restore",0);
    if (fullscreen_lfb.mapped) {
        vmm_revoke_mmio((void *)fullscreen_lfb.mapped,fullscreen_lfb.bytes);
        memset(&fullscreen_lfb,0,sizeof(fullscreen_lfb));
    }
    uint8_t info[256]; struct fb_device next;
    int e, native_result=0, fallback_result=0;
    bool fallback=false;
    if (fullscreen_native) {
        e=native_result=fullscreen_provider->end();
        next=previous; /* only remap the saved layout after native proof */
    } else {
        e=biosvm_video_fullscreen_end(previous.mode_id,info);
        if (!e) e=fbdev_mode_decode(previous.mode_id,info,&next);
    }
    if (!e && (next.width!=previous.width || next.height!=previous.height ||
               next.pitch!=previous.pitch || next.bpp!=previous.bpp || next.phys!=previous.phys)) e=-EIO;
    if (!e) e=fbdev_change_commit(&next);
    /* Native end revokes its aliases/releases CPU access leases even after
     * readback failure. Never bypass a quarantined firmware/registry owner:
     * the mediated switch must acquire its own leases and validate the mode. */
    if (e) {
        fallback=true;
        fallback_result=restore();
        klog("[display] fullscreen_restore provider=%s primary_result=%d fallback_result=%d",
            fullscreen_native ? "native-driver" : "firmware-vga",e,fallback_result);
        e=fallback_result;
    }
    if (fullscreen_native) fullscreen_provider->complete(!e);
    fbdev_change_end();
    bool native=fullscreen_native;
    fullscreen_display=false;
    fullscreen_native=false;
    fullscreen_provider=0;
    grants_display_suspend(false);
    if (native) {
        klog("[display] native_driver native_result=%d fallback_attempted=%u fallback_result=%d result=%d",
            native_result,(unsigned)fallback,fallback_result,e);
        klog("[display] native_driver lease_return_ms=%llu budget_ms=%u overrun=%u result=%d",
            g_ticks-fullscreen_return_tick,fullscreen_return_budget,
            (unsigned)(g_ticks-fullscreen_return_tick>fullscreen_return_budget),e);
    }
    if (!e) {
        /* This path is reached only after fbdev_change_begin revoked the
         * desktop. Preflight refusals never arm a repaint wait. Scanout is
         * verified; the first successful new-generation upload proves that
         * the desktop has resumed, even when its compositor uses strips. */
        changed(); awaiting_frame=true; awaiting_native_frame=native;
        awaiting_restored_frame=true;
        frame_deadline=deadline_after_ms(10000);
    }
    switching=false;
    log_mode(previous.mode_id,e);
    display_fullscreen_checkpoint("desktop-restored",e);
    fullscreen_recovering=false;
    return e;
}
