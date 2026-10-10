/* Bootstrap-only authority. Presentation reuses F1 conversion/lease checks;
 * input consumes its single queue, never controller ports or firmware calls.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
extern uint32_t file_description_count(void) __attribute__((weak));
#define FILE_OBJECTS (file_description_count ? file_description_count() : 0)
#include <ciuki/cpu.h>
#include <ciuki/desktop.h>
#include <ciuki/i8042.h>
#include <ciuki/registry.h>
#include <ciuki/signal.h>

static uint32_t grant_owner, grant_descriptions;
static struct desktop_activity activity;
void desktop_activity_snapshot(struct desktop_activity *out) { *out = activity; }

void grant_release(struct desktop_description *d)
{
    if (!d->owner) return;
    desktop_objects.grants--;
    if (!--grant_descriptions) grant_owner = 0;
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
    return 0;
}

static int grant_check(struct process *p, int32_t fd, enum desktop_kind kind, uint32_t right)
{
    struct desktop_description *d = desktop_fd(p, fd);
    if (!d || d->kind != kind) return -EBADF;
    return d->owner == p->pid && grant_owner == p->pid && (d->maximum & right) ? 0 : -EACCES;
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

int grant_display_info(struct process *p, int32_t fd, struct ciuki_display_info *out)
{
    int err = grant_check(p, fd, DESKTOP_DISPLAY, PROT_WRITE);
    if (err) return err;
    if ((err = display_live())) return err;
    const struct fb_device *d = fbdev_get();
    *out = (struct ciuki_display_info){ sizeof(*out), d->width, d->height, d->pitch,
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

struct present_operation { struct surface *surface; struct desktop_description *grant; uint32_t row_page; };
static void present_cleanup(void *arg)
{
    struct present_operation *op = arg;
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
        }
    }
    if (t) { t->cleanup = 0; t->operation = 0; }
    present_cleanup(&op);
    if (!err) activity.presents++;
    return err;
}

void desktop_input_event(const struct input_event *in, uint32_t lost, uint32_t buttons,
                          struct ciuki_input_event *out)
{
    *out = (struct ciuki_input_event){ .sequence = (uint32_t)in->sequence, .source = in->source,
        .generation = in->generation, .monotonic_ns = in->tick * UINT64_C(1000000) };
    switch (in->type) {
    case INPUT_KEY: out->type = CIUKI_INPUT_KEY; out->code = in->code; out->value = in->value; break;
    case INPUT_TEXT: out->type = CIUKI_INPUT_TEXT; out->code = in->code; break;
    case INPUT_REL:
        out->type = CIUKI_INPUT_MOTION;
        if (in->code == INPUT_X) out->value = in->value;
        else out->value2 = in->value;
        break;
    case INPUT_BTN: out->type = CIUKI_INPUT_BUTTON; out->code = in->code + 1; out->value = in->value; break;
    case INPUT_RESYNC:
        out->type = CIUKI_INPUT_RESYNC; out->lost_count = lost; out->value = (int32_t)(buttons & 7); break;
    }
}

static int input_live(struct i8042_stats *state)
{
    i8042_snapshot(state);
    if (state->quarantined) return -EIO;
    return state->active ? 0 : -ENODEV;
}

struct input_operation {
    struct process *process;
    struct desktop_description *grant;
    struct ua_pin *pin;
};
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
    struct i8042_stats state;
    if ((err = input_live(&state))) return err;
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
        if ((err = input_live(&state))) break;
        struct input_event in;
        bool have = input_read(&in);
        if (have) {
            struct ciuki_input_event out;
            desktop_input_event(&in, (uint32_t)in.lost_count, (uint32_t)in.value, &out);
            err = copy_to_user(events + count * sizeof(out), &out, sizeof(out));
            if (err) break;
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
