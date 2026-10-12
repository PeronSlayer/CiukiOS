/* f1-23: copied display inventory, serialized firmware transaction and
 * independent desktop-frame watchdog. See docs/design/f1-acceptance.md.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/display.h>
#include <ciuki/biosvm.h>
#include <ciuki/registry.h>
#include <ciuki/supervisor.h>

static bool switching, awaiting_frame;
static uint32_t changes;
static uint64_t frame_deadline;
static struct fb_device previous;
static struct task *watchdog;

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

int display_set(struct process *p, uint16_t id)
{
    if (!p || p->pid != supervisor_desktop_pid() || !grants_display_owner(p)) return refuse(id, -EPERM);
    const struct ciuki_boot_display_mode *m = listed(id);
    if (!m) return refuse(id, -EINVAL);
    /* Window DOS VMs currently have private surfaces, never physical grants.
     * A competing/foreign display owner or an in-flight frame prevents entry. */
    if (switching || awaiting_frame || grants_display_busy()) return refuse(id, -EBUSY);
    if (!fbdev_get()->present) return refuse(id, -ENODEV);
    if (!listed(fbdev_get()->mode_id)) return refuse(id, -EOPNOTSUPP); /* cannot promise restore */
    if (id == fbdev_get()->mode_id) return refuse(id, 0);
    if (!watchdog) {
        watchdog = task_create_kernel("display-watchdog", watchdog_main, 0, P_NORMAL);
        if (!watchdog) return refuse(id, -ENOMEM);
        task_start(watchdog);
    }
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
