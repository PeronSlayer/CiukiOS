/* f1-23: copied display inventory, serialized firmware transaction and
 * independent desktop-frame watchdog. See docs/design/f1-acceptance.md.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/display.h>
#include <ciuki/biosvm.h>
#include <ciuki/registry.h>
#include <ciuki/supervisor.h>
#include <ciuki/bootlog.h>

static bool switching, awaiting_frame;
static bool fullscreen_display;
extern void dosvm_fullscreen_poll(void) __attribute__((weak));
static uint32_t changes;
static uint64_t frame_deadline;
static struct fb_device previous;
static struct task *watchdog;

static bool checkpoint_cancel(void *arg) { (void)arg; return false; }
static void checkpoint_pause(void *arg) { (void)arg; task_sleep_ms(1); }
int display_fullscreen_checkpoint(const char *step, int result)
{
    /* The emulated adapters have serial evidence and tight switch timings.
     * Hardware must commit the watermark before the next transaction step. */
    if (g_boot.flags & CBI_F_SMBIOS_QEMU) return 0;
    klog("[display] fullscreen_step=%s result=%d tick=%llu",step,result,g_ticks);
    struct bootlog_stats stats;
    bootlog_snapshot(&stats);
    int e=stats.active ? bootlog_sync(checkpoint_cancel,checkpoint_pause,0) : -EIO;
    if (e) klog("[display] fullscreen_log error=%d step=%s",e,step);
    return e;
}

uint32_t display_change_counter(void) { return changes; }
static void changed(void) { if (!++changes) ++changes; }

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

static int apply(uint16_t id, struct fb_device *out)
{
    uint8_t info[256];
    int e = biosvm_video_switch(id, info);
    if (e) return -EIO;
    if (fbdev_mode_decode(id, info, out)) return -EIO;
    const struct ciuki_boot_display_mode *m = listed(id);
    if (!m || m->width != out->width || m->height != out->height || m->bpp != out->bpp) return -EIO;
    return fbdev_change_commit(out) ? -EIO : 0;
}

static int restore(void)
{
    struct fb_device restored;
    int e = apply(previous.mode_id, &restored);
    if (!e) changed();
    log_mode(previous.mode_id, e);
    return e;
}

void display_frame_complete(uint32_t generation)
{
    if (!switching && awaiting_frame && generation == fbdev_get()->generation && !deadline_passed(frame_deadline))
        awaiting_frame = false;
}

void display_watchdog_poll(void)
{
    if (dosvm_fullscreen_poll) dosvm_fullscreen_poll();
    if (fullscreen_display) return;
    if (!awaiting_frame || switching || !deadline_passed(frame_deadline) || grants_display_busy()) return;
    switching = true;
    awaiting_frame = false;
    if (!fbdev_change_begin()) { restore(); fbdev_change_end(); }
    switching = false;
}
static void watchdog_main(void *arg)
{
    (void)arg;
    for (;;) { task_sleep_ms(100); display_watchdog_poll(); }
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

int display_set(struct process *p, uint16_t id)
{
    if (!p || p->pid != supervisor_desktop_pid() || !grants_display_owner(p)) return refuse(id, -EPERM);
    const struct ciuki_boot_display_mode *m = listed(id);
    if (!m) return refuse(id, -EINVAL);
    /* Window DOS VMs currently have private surfaces, never physical grants.
     * A competing/foreign display owner or an in-flight frame prevents entry. */
    if (switching || fullscreen_display || awaiting_frame || grants_display_busy()) return refuse(id, -EBUSY);
    if (!fbdev_get()->present) return refuse(id, -ENODEV);
    if (!listed(fbdev_get()->mode_id)) return refuse(id, -EOPNOTSUPP); /* cannot promise restore */
    if (id == fbdev_get()->mode_id) return refuse(id, 0);
    if (display_worker_start()) return refuse(id, -ENOMEM);
    switching = true;
    previous = *fbdev_get();
    int e = biosvm_video_prepare();
    if (e) { switching = false; return refuse(id, -EIO); }
    if (grants_display_busy()) { switching = false; return refuse(id, -EBUSY); }
    e = fbdev_change_begin();
    if (!e) {
        struct fb_device next;
        e = apply(id, &next);
        if (e) restore();
        else {
            changed();
            awaiting_frame = true;
            frame_deadline = deadline_after_ms(10000);
        }
        fbdev_change_end();
    }
    switching = false;
    if (e) e = -EIO;
    log_mode(id, e);
    return e;
}

int32_t display_syscall(struct trap_frame *tf)
{
    if (tf->edx || tf->esi || tf->edi || tf->ebp) return -EINVAL;
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

bool display_fullscreen_active(void) { return fullscreen_display; }
int display_fullscreen_begin(uint8_t mode)
{
    int logged=display_fullscreen_checkpoint("request",0);
    if (logged) return logged;
    struct process *desktop=proc_find(grants_display_pid());
    const char *why=0;
    if (!desktop) why="no-display-owner";
    else if (desktop->state>=PROC_STOPPING) why="desktop-stopping";
    else if (!grants_display_owner(desktop)) why="owner-without-writable-display";
    if (why) {
        klog("[display] fullscreen_refused reason=%s owner_pid=%u mode=%u",why,grants_display_pid(),mode);
        return -EPERM;
    }
    if (switching || fullscreen_display || awaiting_frame || grants_display_busy()) {
        klog("[display] fullscreen_refused reason=busy switching=%u active=%u awaiting_frame=%u presenting=%u",
            (unsigned)switching,(unsigned)fullscreen_display,(unsigned)awaiting_frame,(unsigned)grants_display_busy());
        return -EBUSY;
    }
    if (!fbdev_get()->present || !listed(fbdev_get()->mode_id)) {
        klog("[display] fullscreen_refused reason=unsupported present=%u mode_id=%u",
            (unsigned)fbdev_get()->present,(unsigned)fbdev_get()->mode_id);
        return -EOPNOTSUPP;
    }
    /* The standard VGA transport cannot save the mobile panel/PLL/streams.
     * A denied BIOS dependency disables the backend, including rollback.
     * Refuse before grants, acceleration or the firmware can alter scanout.
     * See docs/design/savage-2d.md, f3-26. Never guess CR67/CR69 settings. */
    for (unsigned i=0;i<pci_count();i++) {
        const struct pci_func *p=pci_get(i);
        if (p && p->class_code==3 && p->vendor==0x5333 && p->device==0x8C2E) {
            klog("[display] fullscreen_refused reason=supersavage-recovery-unavailable pci=5333:8c2e mode=%u",mode);
            display_fullscreen_checkpoint("refused",-EOPNOTSUPP);
            return -EOPNOTSUPP;
        }
    }
    if (display_worker_start()) return -ENOMEM;
    switching=true;
    previous=*fbdev_get();
    int e=biosvm_video_prepare();
    if (e) { switching=false; return -EIO; }
    e=display_fullscreen_checkpoint("grant-suspend",0);
    if (e) { switching=false; return e; }
    e=grants_display_suspend(true);
    if (e) { switching=false; return e; }
    e=display_fullscreen_checkpoint("desktop-revoke",0);
    if (e) { grants_display_suspend(false); switching=false; return e; }
    e=fbdev_change_begin();
    if (e) { grants_display_suspend(false); switching=false; return e; }
    e=biosvm_video_fullscreen_begin(mode);
    fullscreen_display=true; /* also brackets rollback of a partially set mode */
    switching=false;
    if (e) { display_fullscreen_end(); return -EIO; }
    klog("[display] fullscreen=1 mode=%u desktop_mode=%u",mode,previous.mode_id);
    return 0;
}
int display_fullscreen_end(void)
{
    if (!fullscreen_display) return 0;
    if (switching) return -EBUSY;
    switching=true;
    /* Logging failure must never prevent an already needed recovery. */
    display_fullscreen_checkpoint("desktop-restore",0);
    uint8_t info[256]; struct fb_device next;
    int e=biosvm_video_fullscreen_end(previous.mode_id,info);
    if (!e) e=fbdev_mode_decode(previous.mode_id,info,&next);
    if (!e && (next.width!=previous.width || next.height!=previous.height ||
               next.pitch!=previous.pitch || next.bpp!=previous.bpp || next.phys!=previous.phys)) e=-EIO;
    if (!e) e=fbdev_change_commit(&next);
    /* If exact firmware restoration fails, try the existing confirmed-mode
     * recovery. A failed recovery leaves mappings unavailable, never guessed. */
    if (e) {
        int recovery=restore();
        klog("[display] fullscreen_restore error=%d recovery=%d",e,recovery);
        e=recovery;
    }
    fbdev_change_end();
    fullscreen_display=false;
    grants_display_suspend(false);
    if (!e) {
        changed(); awaiting_frame=true; frame_deadline=deadline_after_ms(10000);
    }
    switching=false;
    log_mode(previous.mode_id,e);
    display_fullscreen_checkpoint("desktop-restored",e);
    return e;
}
