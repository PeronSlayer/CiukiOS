/* Bootstrap-only authority. Presentation reuses F1 conversion/lease checks;
 * input consumes its single queue, never controller ports or firmware calls.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
extern uint32_t file_description_count(void) __attribute__((weak));
#define FILE_OBJECTS ((file_description_count ? file_description_count() : 0) + (net_socket_count ? net_socket_count() : 0))
extern uint32_t net_socket_count(void) __attribute__((weak));
#include <ciuki/cpu.h>
#include <ciuki/desktop.h>
#include <ciuki/registry.h>
#include <ciuki/signal.h>
#include <ciuki/pager.h>

static uint32_t grant_owner, grant_descriptions, presenting;
static bool display_suspended;
uint32_t grants_display_pid(void) { return grant_owner; }
int grants_display_suspend(bool suspended)
{
    if (suspended && (display_suspended || presenting || !grant_owner)) return -EBUSY;
    display_suspended = suspended;
    return 0;
}
extern void display_frame_complete(uint32_t generation, bool full_frame) __attribute__((weak));
extern void entropy_desktop_presented(void) __attribute__((weak));
bool grants_display_owner(const struct process *p)
{
    if (!p || p->pid != grant_owner) return false;
    for (unsigned i = 0; i < CIUKI_OPEN_MAX; i++) {
        struct desktop_description *d = desktop_fd(p, (int32_t)i);
        if (d && d->kind == DESKTOP_DISPLAY && d->owner == p->pid && (d->maximum & PROT_WRITE)) return true;
    }
    return false;
}
bool grants_display_busy(void) { return presenting != 0; }
static struct desktop_activity activity;
void desktop_activity_snapshot(struct desktop_activity *out) { *out = activity; }

void grant_release(struct desktop_description *d)
{
    if (!d->owner) return;
    desktop_objects.grants--;
    /* The last grant description closes: the screen console draws again. */
    if (!--grant_descriptions) {
        fbdev_cursor_hide();
        grant_owner = 0; console_display_owned(false);
    }
}

int grants_install(struct process *p, int32_t fds[2])
{
    if (!p || p == proc_supervisor() || !p->fds || grant_owner) return -EPERM;
    int a = desktop_fd_slot(p, 0), b = a < 0 ? a : desktop_fd_slot(p, (unsigned)a + 1);
    if (b < 0) return -EMFILE;
    if (desktop_objects.descriptions + FILE_OBJECTS > CIUKI_OPEN_DESCRIPTION_MAX - 2) return -ENFILE;
    struct desktop_description *display = desktop_description_new(DESKTOP_DISPLAY, O_WRONLY);
    struct desktop_description *input = desktop_description_new(DESKTOP_INPUT, O_RDONLY);
    if (!display || !input) {
        if (display) display->object.release(&display->object);
        if (input) input->object.release(&input->object);
        return -ENOMEM;
    }
    grant_owner = p->pid;
    grant_descriptions = 2;
    desktop_objects.grants += 2;
    display->owner = input->owner = p->pid;
    display->maximum = PROT_WRITE;
    input->maximum = PROT_READ;
    /* Noninheritability is unconditional, even if users clear CLOEXEC. */
    p->fds[a] = (struct proc_fd){ &display->object, FD_CLOEXEC };
    p->fds[b] = (struct proc_fd){ &input->object, FD_CLOEXEC };
    fds[0] = a; fds[1] = b;
    /* f3-02: the grant owner composites every LFB pixel; kernel log lines
     * and records (serial, history) must not draw over it. */
    console_display_owned(true);
    return 0;
}

static int grant_check(struct process *p, int32_t fd, enum desktop_kind kind, uint32_t right)
{
    struct desktop_description *d = desktop_fd(p, fd);
    if (!d || d->kind != kind) return -EBADF;
    if (kind == DESKTOP_DISPLAY && display_suspended) return -EBUSY;
    return d->owner == p->pid && grant_owner == p->pid && (d->maximum & right) ? 0 : -EACCES;
}

struct accel_operation { struct desktop_description *grant; bool presenting; uint32_t page; };
int grant_accel_info(struct process *p, int32_t fd, const struct ciuki_accel_request *q,
                     struct ciuki_accel_info *out)
{
    int err = grant_check(p,fd,DESKTOP_DISPLAY,PROT_WRITE);
    if (err) return err;
    if (q->size != sizeof(*q) || q->operation != CIUKI_ACCEL_QUERY) return -EINVAL;
    return fbdev_accel_info(q->generation,out);
}
/* f2-92: fbdev releases the presenter lock between 1 ms engine-wait slices
 * of a display request; the requesting thread may be preempted there. */
void grants_display_yield(void) { kpreempt_point(); }
static void accel_cleanup(void *arg)
{
    struct accel_operation *op=arg;
    if (op->presenting) presenting--;
    if (op->page) { pmm_free(op->page); op->page=0; }
    op->grant->object.release(&op->grant->object);
}
/* f2-92 CIUKI_ACCEL_WRITE: clip the request against its source and the
 * optional destination clip, copy each band of user rows into one staging
 * page outside every display lock (user memory may fault), then hand the
 * band to the presenter (engine, else CPU for visible rows). 0: every band
 * went through the engine; 1: at least one band was written by the CPU. */
static int accel_write(struct process *p, const struct ciuki_accel_request *q, struct accel_operation *op)
{
    const struct ciuki_accel_write *w = &q->write;
    if (w->pixels_hi || (w->stride & 3u) || !w->source_width || !w->source_height ||
        w->source_width > INT32_MAX || w->source_height > INT32_MAX ||
        (uint64_t)w->source_width * 4u > w->stride || q->rect.width > INT32_MAX || q->rect.height > INT32_MAX ||
        w->clip_width > INT32_MAX || w->clip_height > INT32_MAX) return -EINVAL;
    int64_t sx = q->rect.src_x, sy = q->rect.src_y, dx = q->rect.dst_x, dy = q->rect.dst_y;
    int64_t width = q->rect.width, height = q->rect.height;
    /* Source bounds, then the destination clip and origin, moving both ends. */
    if (sx < 0) { dx -= sx; width += sx; sx = 0; }
    if (sy < 0) { dy -= sy; height += sy; sy = 0; }
    if (sx + width > w->source_width) width = (int64_t)w->source_width - sx;
    if (sy + height > w->source_height) height = (int64_t)w->source_height - sy;
    int64_t cx = 0, cy = 0, cr = INT32_MAX, cb = INT32_MAX;
    if (w->clip_width && w->clip_height) {
        cx = w->clip_x > 0 ? w->clip_x : 0; cy = w->clip_y > 0 ? w->clip_y : 0;
        cr = (int64_t)w->clip_x + w->clip_width; cb = (int64_t)w->clip_y + w->clip_height;
    }
    if (dx < cx) { sx += cx - dx; width -= cx - dx; dx = cx; }
    if (dy < cy) { sy += cy - dy; height -= cy - dy; dy = cy; }
    if (dx + width > cr) width = cr - dx;
    if (dy + height > cb) height = cb - dy;
    if (width <= 0 || height <= 0) return 0;
    uint64_t base = (uint64_t)w->pixels_lo + (uint64_t)sy * w->stride + (uint64_t)sx * 4u;
    if (base + (uint64_t)(height - 1) * w->stride + (uint64_t)width * 4u > UINT32_MAX) return -EFAULT;
    if (!(op->page = pmm_alloc())) return -EIO;
    uint8_t *buffer = P2V(op->page);
    struct proc_thread *t = proc_thread_for(g_current);
    uint32_t tile = width > PAGE_SIZE / 4 ? PAGE_SIZE / 4 : (uint32_t)width;
    int err = 0, cpu = 0;
    for (uint32_t x = 0; x < (uint32_t)width && !err; x += tile) {
        uint32_t tw = (uint32_t)width - x < tile ? (uint32_t)width - x : tile;
        uint32_t band = (PAGE_SIZE / 4) / tw;
        for (uint32_t y = 0; y < (uint32_t)height && !err; y += band) {
            uint32_t bh = (uint32_t)height - y < band ? (uint32_t)height - y : band;
            for (uint32_t r = 0; r < bh && !err; r++)
                if (copy_from_user(buffer + r * tw * 4u, (uint32_t)(base + (uint64_t)(y + r) * w->stride + x * 4u), tw * 4u))
                    err = -EFAULT;
            if (err) break;
            struct fb_rect rect = {(int32_t)(dx + x), (int32_t)(dy + y), (int32_t)tw, (int32_t)bh};
            int e = fbdev_accel_write(q->generation, &rect, &rect, buffer, tw * 4u);
            if (e < 0) err = e;
            else if (e) cpu = 1;
            kpreempt_point();
            if (!err && (p->state != PROC_LIVE || (t && proc_signal_caught(t)))) err = -EINTR;
        }
    }
    pmm_free(op->page); op->page = 0;
    return err ? err : cpu;
}
int grant_accel(struct process *p, int32_t fd, const struct ciuki_accel_request *q)
{
    int err = grant_check(p,fd,DESKTOP_DISPLAY,PROT_WRITE);
    if (err) return err;
    struct proc_thread *t=proc_thread_for(g_current);
    if (t && proc_signal_caught(t)) return -EINTR;
    struct accel_operation op={.grant=desktop_fd(p,fd),.presenting=true};
    op.grant->object.retain(&op.grant->object);
    if (t) { t->cleanup=accel_cleanup; t->operation=&op; }
    presenting++;
    err = q->size == sizeof(*q) && q->operation == CIUKI_ACCEL_WRITE ? accel_write(p,q,&op) : fbdev_accel_request(q);
    if (t) { t->cleanup=0; t->operation=0; }
    accel_cleanup(&op);
    return err;
}

static int display_live(void)
{
    const struct fb_device *d = fbdev_get();
    if (!d->present) return -ENODEV;
    const struct resource *r = d->handle < 0 ? 0 : registry_get((unsigned)d->handle);
    if (!r || !registry_valid(d->handle, d->generation) || r->state != RS_ACTIVE)
        return -EIO;
    return 0;
}

int grant_display_info(struct process *p, int32_t fd, struct ciuki_framebuffer_info *out)
{
    int err = grant_check(p, fd, DESKTOP_DISPLAY, PROT_WRITE);
    if (err) return err;
    if ((err = display_live())) return err;
    const struct fb_device *d = fbdev_get();
    *out = (struct ciuki_framebuffer_info){ sizeof(*out), d->width, d->height, d->pitch,
        d->bpp, d->red_size, d->red_pos, d->green_size, d->green_pos,
        d->blue_size, d->blue_pos, d->generation };
    return 0;
}

/* Clip a parametric translation: s+t and d+t share the same t interval.
 * int64_t endpoint arithmetic also handles INT_MIN without negation UB. */
int desktop_clip(const struct ciuki_surface_info *s, const struct fb_device *d,
                  const struct ciuki_rect *r, struct ciuki_rect *out)
{
    const int32_t src[2] = { r->src_x, r->src_y }, dst[2] = { r->dst_x, r->dst_y };
    const uint32_t len[2] = { r->width, r->height }, slimit[2] = { s->width, s->height };
    const uint32_t dlimit[2] = { d->width, d->height };
    int64_t lo[2], hi[2];
    for (unsigned i = 0; i < 2; i++) {
        if ((int64_t)src[i] + len[i] > INT32_MAX || (int64_t)dst[i] + len[i] > INT32_MAX)
            return -EINVAL;
        lo[i] = 0; hi[i] = len[i];
        if (-(int64_t)src[i] > lo[i]) lo[i] = -(int64_t)src[i];
        if (-(int64_t)dst[i] > lo[i]) lo[i] = -(int64_t)dst[i];
        if ((int64_t)slimit[i] - src[i] < hi[i]) hi[i] = (int64_t)slimit[i] - src[i];
        if ((int64_t)dlimit[i] - dst[i] < hi[i]) hi[i] = (int64_t)dlimit[i] - dst[i];
    }
    *out = (struct ciuki_rect){ 0 };
    if (hi[0] <= lo[0] || hi[1] <= lo[1]) return 0;
    *out = (struct ciuki_rect){ (int32_t)(src[0] + lo[0]), (int32_t)(src[1] + lo[1]),
        (int32_t)(dst[0] + lo[0]), (int32_t)(dst[1] + lo[1]),
        (uint32_t)(hi[0] - lo[0]), (uint32_t)(hi[1] - lo[1]) };
    return 0;
}

struct present_operation { struct surface *surface; struct desktop_description *grant; uint32_t row_page; bool presenting; };
static void present_cleanup(void *arg)
{
    struct present_operation *op = arg;
    if (op->presenting) presenting--;
    surface_release(op->surface);
    op->grant->object.release(&op->grant->object);
    pmm_free(op->row_page);
}

int grant_present(struct process *p, int32_t display, int32_t fd, const struct ciuki_rect *rect)
{
    int err = grant_check(p, display, DESKTOP_DISPLAY, PROT_WRITE);
    if (err) return err;
    struct desktop_description *d = desktop_fd(p, fd);
    if (!d || d->kind != DESKTOP_SURFACE) return -EBADF;
    if (!(d->maximum & PROT_READ)) return -EACCES;
    if ((err = display_live())) return err;
    struct ciuki_rect clipped;
    const struct ciuki_surface_info *s = surface_geometry(d->u.surface);
    err = desktop_clip(s, fbdev_get(), rect, &clipped);
    if (err || !clipped.width || !clipped.height) return err;
    struct proc_thread *t = proc_thread_for(g_current);
    if (t && proc_signal_caught(t)) return -EINTR;
    /* One physical staging page avoids the kernel heap's 2040-byte limit.
     * Every tile runs through the same F1 lease/serialization/conversion path. */
    struct present_operation op = { .surface = d->u.surface, .row_page = pmm_alloc() };
    if (!op.row_page) return -EIO;
    surface_retain(op.surface);
    op.grant = desktop_fd(p, display);
    op.grant->object.retain(&op.grant->object);
    if (t) { t->cleanup = present_cleanup; t->operation = &op; }
    uint32_t generation = fbdev_get()->generation;
    /* Allocation may have yielded after the initial authority check. A
     * delegated/DOS handoff must not admit that parked desktop submission. */
    err = grant_check(p, display, DESKTOP_DISPLAY, PROT_WRITE);
    if (err) {
        if (t) { t->cleanup = 0; t->operation = 0; }
        present_cleanup(&op);
        return err;
    }
    presenting++; op.presenting = true;
    for (unsigned y = 0; y < clipped.height && !err; y++) {
        for (unsigned x = 0; x < clipped.width; x += PAGE_SIZE / 4) {
            uint32_t width = clipped.width - x;
            if (width > PAGE_SIZE / 4) width = PAGE_SIZE / 4;
            struct fb_surface src = { P2V(op.row_page), width, 1, width * 4, width * 4 };
            struct fb_rect dst = { clipped.dst_x + (int32_t)x, clipped.dst_y + (int32_t)y, (int32_t)width, 1 };
            surface_read(op.surface, ((uint32_t)clipped.src_y + y) * s->stride + ((uint32_t)clipped.src_x + x) * 4,
                         P2V(op.row_page), width * 4);
            err = fbdev_present(&src, &dst);
            if (err) { if (err == -ENODEV) err = -EIO; break; }
            /* Conversion/copy is at most one 4 KiB tile. The F1 mutex and
             * console drawing claim have both been released here. The
             * operation retains its surface and grant across the yield. */
            kpreempt_point();
            if (p->state != PROC_LIVE || (t && proc_signal_caught(t))) err = -EINTR;
            else if (fbdev_get()->generation != generation || display_live()) err = -EIO;
            if (err) break;
        }
    }
    bool full_frame = s->width == fbdev_get()->width && s->height == fbdev_get()->height &&
                      !clipped.dst_x && !clipped.dst_y && clipped.width == fbdev_get()->width &&
                      clipped.height == fbdev_get()->height;
    if (t) { t->cleanup = 0; t->operation = 0; }
    present_cleanup(&op);
    if (!err) {
        activity.presents++;
        if (display_frame_complete) display_frame_complete(generation, full_frame);
        if (full_frame && pager_bootstrap_complete) pager_bootstrap_complete();
        if (full_frame && entropy_desktop_presented) entropy_desktop_presented(); /* f6-21 */
    }
    return err;
}

void desktop_input_event(const struct input_event *in, uint32_t lost, uint32_t buttons,
                          struct ciuki_input_event *out)
{
    *out = (struct ciuki_input_event){ .sequence = (uint32_t)in->sequence, .source = in->source,
        .generation = in->generation, .monotonic_ns = in->tick * UINT64_C(1000000) };
    switch (in->type) {
    case INPUT_USAGE: case INPUT_CONSUMER: case INPUT_JOY_AXIS:
    case INPUT_JOY_HAT: case INPUT_JOY_BUTTON: case INPUT_DEVICE:
        out->type=in->type; out->code=in->code; out->value=in->value;
        out->value2=(int32_t)in->device; out->lost_count=CIUKI_HID_VERSION; break;
    case INPUT_KEY: out->type = CIUKI_INPUT_KEY; out->code = in->code; out->value = in->value; break;
    case INPUT_TEXT: out->type = CIUKI_INPUT_TEXT; out->code = in->code; break;
    case INPUT_REL:
        out->type = in->code == INPUT_WHEEL ? CIUKI_INPUT_WHEEL : CIUKI_INPUT_MOTION;
        if (in->code == INPUT_WHEEL) out->value = in->value;
        else if (in->code == INPUT_X) out->value = in->value;
        else out->value2 = in->value;
        break;
    case INPUT_BTN: out->type = CIUKI_INPUT_BUTTON; out->code = in->code + 1; out->value = in->value; break;
    case INPUT_RESYNC:
        out->type = CIUKI_INPUT_RESYNC; out->lost_count = lost; out->value = (int32_t)(buttons & 31); break;
    }
}

struct input_operation {
    struct process *process;
    struct desktop_description *grant;
    struct ua_pin *pin;
};
extern int usb_hid_query(uint32_t, void *) __attribute__((weak));
int grant_input_devctl(struct process *p, int32_t fd, uint32_t command, uint32_t address, uint32_t bytes)
{
    int err=grant_check(p,fd,DESKTOP_INPUT,PROT_READ);
    if (err) return err;
    unsigned expected=command==INPUT_HID_GET_CAPS ? sizeof(struct ciuki_hid_caps) :
        command==INPUT_HID_GET_STATE ? sizeof(struct ciuki_hid_state) :
        command==INPUT_HID_CALIBRATE ? sizeof(struct ciuki_hid_calibration) : 0;
    if (!expected) return -ENOTTY;
    if (bytes!=expected) return -EINVAL;
    union { struct ciuki_hid_caps caps; struct ciuki_hid_state state;
        struct ciuki_hid_calibration calibration; uint32_t header[4]; } record;
    struct ua_pin pin;
    err=ua_pin(p->memory,&pin,address,bytes,true);
    if (err) return err;
    err=copy_from_user(&record,address,bytes);
    if (!err && (record.header[0]!=CIUKI_HID_VERSION || record.header[1]!=bytes ||
        (command==INPUT_HID_GET_CAPS && record.caps.reserved) ||
        (command==INPUT_HID_GET_STATE && record.state.reserved))) err=-EINVAL;
    if (!err) {
        uint32_t flags=irq_save();
        err=usb_hid_query ? usb_hid_query(command,&record) : -ENODEV;
        irq_restore(flags);
    }
    if (!err) err=copy_to_user(address,&record,bytes);
    ua_unpin(p->memory,&pin);
    return err;
}
static void input_cleanup(void *arg)
{
    struct input_operation *op = arg;
    ua_unpin(op->process->memory, op->pin);
    op->grant->object.release(&op->grant->object);
}

int grant_input_read(struct process *p, int32_t fd, uint32_t events, uint32_t capacity)
{
    int err = grant_check(p, fd, DESKTOP_INPUT, PROT_READ);
    if (err) return err;
    if (!capacity || capacity > CIUKI_INPUT_READ_MAX) return -EINVAL;
    /* This grant owns the merged queue, not a particular controller. Device
     * absence (including i8042 quarantine) is an empty queue, not lost access.
     * Drain detach releases too, after the last producer has disappeared. */
    struct ua_pin pin;
    err = ua_pin(p->memory, &pin, events, capacity * sizeof(struct ciuki_input_event), true);
    if (err) return err;
    struct desktop_description *d = desktop_fd(p, fd);
    d->object.retain(&d->object);
    struct input_operation op = { p, d, &pin };
    struct proc_thread *t = proc_thread_for(g_current);
    if (t) { t->operation = &op; t->cleanup = input_cleanup; }
    unsigned count = 0;
    for (;;) {
        if (p->state == PROC_STOPPING) { err = -EINTR; break; }
        extern bool dosvm_fullscreen_desktop_event(struct ciuki_input_event *) __attribute__((weak));
        struct ciuki_input_event pending;
        if (dosvm_fullscreen_desktop_event && dosvm_fullscreen_desktop_event(&pending)) {
            err=copy_to_user(events+count*sizeof(pending),&pending,sizeof(pending));
            if (err || ++count==capacity) break;
            continue;
        }
        struct input_event in;
        bool have = input_read(&in);
        if (have) {
            struct ciuki_input_event out;
            desktop_input_event(&in, (uint32_t)in.lost_count, (uint32_t)in.value, &out);
            err = copy_to_user(events + count * sizeof(out), &out, sizeof(out));
            if (err) break;
            if (out.type != CIUKI_INPUT_RESYNC) task_input_delivered(g_current);
            if (++count == capacity) break;
            continue;
        }
        if (count) break;
        if (d->flags & O_NONBLOCK) { err = -EAGAIN; break; }
        if (p->state == PROC_STOPPING || (t && proc_signal_caught(t))) { err = -EINTR; break; }
        task_sleep_ms(1);
    }
    if (t) { t->operation = 0; t->cleanup = 0; }
    input_cleanup(&op);
    activity.input_events += count;
    return count ? (int)count : err;
}
