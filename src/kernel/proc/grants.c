/* Bootstrap-only authority. Presentation reuses F1 conversion/lease checks;
 * input consumes its single queue, never controller ports or firmware calls.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/security_device.h>
extern uint32_t file_description_count(void) __attribute__((weak));
#define FILE_OBJECTS ((file_description_count ? file_description_count() : 0) + (net_socket_count ? net_socket_count() : 0))
extern uint32_t net_socket_count(void) __attribute__((weak));
#pragma weak klog /* sec-user-06 timing records; host harnesses may link no log sink */
#include <ciuki/cpu.h>
#include <ciuki/desktop.h>
#include <ciuki/registry.h>
#include <ciuki/signal.h>
#include <ciuki/pager.h>

static uint32_t grant_owner, grant_descriptions, presenting;
static bool display_suspended;
/* --- sec-k07 (begin): trusted and session presentation domains ---
 * One physical console, two exclusive domains (Microsoft "Desktops": one
 * input desktop at a time, Winlogon's desktop for secure attention). The
 * interim bootstrap desktop is the only USER owner and the domain never
 * changes without LOGON, so its behaviour is the pre-k07 single grant.
 * With LOGON the trusted UI (LOGON_UI) owns separate descriptions; session
 * grants start suspended and only SECURE_LEAVE/initial activation (via
 * security.c) enables them. A suspended display answers EBUSY like a DOS
 * handoff; suspended input reads no events (EAGAIN/blocks). Concealment
 * fences in-flight user presents, DOS full screen, the present lease and a
 * neutral black kernel frame before any trusted pixel; the neutral frame is
 * painted by PID 1 (grants_console_neutral), never with IRQs masked. */
extern void display_console_changed(void) __attribute__((weak));
extern bool display_fullscreen_active(void) __attribute__((weak));
extern bool display_present_active(void) __attribute__((weak));
extern void display_worker_wake(void) __attribute__((weak));
extern bool input_secure_trusted(void) __attribute__((weak));
extern void input_secure_domain(bool, uint32_t) __attribute__((weak));
extern bool input_read_domain(struct input_event *, bool) __attribute__((weak));
static uint32_t trusted_owner, trusted_descriptions, trusted_presenting;
static uint32_t user_session, active_session;
static bool console_trusted, neutral_pending;
static uint64_t neutral_frames;
/* sec-user-06: trusted-gate latency (desktop.h grants_console_timing). */
static struct grants_console_timing timing;
static uint64_t conceal_tick, activate_tick;
static bool timing_neutral, timing_trusted, timing_restore;
static uint32_t timing_ms(uint64_t since)
{ uint64_t d = g_ticks - since; return d > UINT32_MAX ? UINT32_MAX : (uint32_t)d; }
void grants_console_timing(struct grants_console_timing *out)
{ uint32_t f = irq_save(); *out = timing; irq_restore(f); }
/* After a present of the grant's domain: the first trusted frame closes a
 * concealment, the first session frame a restoration. One record each. */
static void timing_frame(bool trusted)
{
    uint32_t f = irq_save();
    bool conceal = trusted && timing_trusted && !timing_neutral, restore = !trusted && timing_restore;
    uint32_t neutral = timing.last_neutral_ms, ms = 0, count = 0;
    if (conceal) {
        ms = timing_ms(conceal_tick); timing_trusted = false;
        timing.last_trusted_ms = ms; if (ms > timing.max_trusted_ms) timing.max_trusted_ms = ms;
        count = ++timing.concealments;
    } else if (restore) {
        ms = timing_ms(activate_tick); timing_restore = false;
        timing.last_restore_ms = ms; if (ms > timing.max_restore_ms) timing.max_restore_ms = ms;
        count = ++timing.restorations;
    }
    irq_restore(f);
    if (!klog) return;
    if (conceal) klog("[security] console concealed neutral_ms=%u trusted_ms=%u count=%u", neutral, ms, count);
    else if (restore) klog("[security] console restored ms=%u count=%u", ms, count);
}
static bool input_trusted_now(void) { return input_secure_trusted && input_secure_trusted(); }
static bool user_active(void)
{ return grant_owner && !console_trusted && user_session == active_session; }
static void console_changed(void) { if (display_console_changed) display_console_changed(); }
/* User-domain pixels that may still reach scanout after concealment. */
static bool user_draining(void)
{
    return presenting != trusted_presenting || display_suspended ||
        (display_fullscreen_active && display_fullscreen_active()) ||
        (display_present_active && display_present_active());
}
static int grant_domain(const struct desktop_description *d, enum desktop_kind kind)
{
    if (d->grant_trusted) {
        if (!trusted_owner || trusted_owner != d->owner) return -EACCES;
        if (kind == DESKTOP_INPUT) return input_trusted_now() ? 0 : -EBUSY;
        return console_trusted && !neutral_pending ? 0 : -EBUSY;
    }
    if (!grant_owner || grant_owner != d->owner || d->grant_session != user_session) return -EACCES;
    if (kind == DESKTOP_INPUT && input_trusted_now()) return -EBUSY;
    return user_active() ? 0 : -EBUSY;
}
bool grants_console_trusted(void) { return console_trusted; }
void grants_console_snapshot(struct grants_console_state *out)
{
    uint32_t f = irq_save();
    *out = (struct grants_console_state){ .trusted_owner = trusted_owner, .user_owner = grant_owner,
        .user_session = user_session, .active_session = active_session,
        .trusted = console_trusted, .neutral_pending = neutral_pending,
        .draining = user_draining(), .neutral_frames = neutral_frames };
    irq_restore(f);
}
/* Nonblocking, IRQ-masked by security.c. EBUSY while any user pixel source
 * drains or the neutral frame is still owed; never re-exposes user output. */
int grants_console_conceal(uint32_t session)
{
    (void)session;
    uint32_t f = irq_save();
    bool exposed = user_active();
    console_trusted = true;
    if (input_secure_domain) input_secure_domain(true, 0);
    if (exposed) { neutral_pending = true; console_changed(); }
    if (exposed) { conceal_tick = g_ticks; timing_neutral = timing_trusted = true; timing_restore = false; } /* sec-user-06 */
    int busy = user_draining() || neutral_pending ? -EBUSY : 0;
    irq_restore(f);
    if (busy) {
        if (display_worker_wake) display_worker_wake(); /* DOS/present return */
        proc_notify();                                   /* PID 1 paints neutral */
    }
    return busy;
}
int grants_console_activate(uint32_t session)
{
    uint32_t f = irq_save();
    if (!session || session != user_session || !grant_owner) { irq_restore(f); return -ESRCH; }
    console_trusted = false;
    neutral_pending = false;
    active_session = session;
    if (input_secure_domain) input_secure_domain(false, 0);
    console_changed();
    activate_tick = g_ticks; timing_restore = true; timing_neutral = timing_trusted = false; /* sec-user-06 */
    irq_restore(f);
    return 0;
}
void grants_console_release(uint32_t session)
{
    uint32_t f = irq_save();
    if (session && session == user_session) {
        if (active_session == session) active_session = 0;
        if (!console_trusted) { console_trusted = true; console_changed(); }
        if (input_secure_domain && trusted_owner) input_secure_domain(true, 0);
    }
    irq_restore(f);
}
/* PID 1 thread context: the neutral kernel frame owed by a concealment. */
int grants_console_neutral(void)
{
    uint32_t f = irq_save();
    bool owed = neutral_pending && console_trusted && !user_draining();
    if (owed) presenting++;
    irq_restore(f);
    if (!owed) return neutral_pending ? -EBUSY : 0;
    const struct fb_device *dev = fbdev_get();
    int err = 0;
    uint32_t page = dev->present ? pmm_alloc() : 0;
    if (dev->present && !page) err = -ENOMEM;
    if (page) {
        memset(P2V(page), 0, PAGE_SIZE);
        fbdev_cursor_hide();
        uint32_t generation = dev->generation;
        for (uint32_t y = 0; y < dev->height && !err; y++)
            for (uint32_t x = 0; x < dev->width && !err; x += PAGE_SIZE / 4) {
                uint32_t width = dev->width - x > PAGE_SIZE / 4 ? PAGE_SIZE / 4 : dev->width - x;
                struct fb_surface src = { P2V(page), width, 1, width * 4, width * 4 };
                struct fb_rect dst = { (int32_t)x, (int32_t)y, (int32_t)width, 1 };
                err = fbdev_present(&src, &dst);
                kpreempt_point();
                if (!err && (!console_trusted || fbdev_get()->generation != generation)) err = -EAGAIN;
            }
        pmm_free(page);
    }
    f = irq_save();
    presenting--;
    if (!err && console_trusted) { neutral_pending = false; neutral_frames++; console_changed(); }
    if (!err && console_trusted && timing_neutral) { /* sec-user-06 */
        uint32_t ms = timing_ms(conceal_tick);
        timing.last_neutral_ms = ms; if (ms > timing.max_neutral_ms) timing.max_neutral_ms = ms;
        timing_neutral = false;
    }
    irq_restore(f);
    return err;
}
static void grant_owned(void)
{ if (grant_descriptions + trusted_descriptions == 2) console_display_owned(true); }
static int grant_pair(struct process *p, bool trusted, uint32_t session, int32_t fds[2])
{
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
    display->grant_trusted = input->grant_trusted = trusted;
    display->grant_session = input->grant_session = session;
    display->owner = input->owner = p->pid;
    display->maximum = PROT_WRITE;
    input->maximum = PROT_READ;
    desktop_objects.grants += 2;
    p->fds[a] = (struct proc_fd){ &display->object, FD_CLOEXEC };
    p->fds[b] = (struct proc_fd){ &input->object, FD_CLOEXEC };
    fds[0] = a; fds[1] = b;
    return 0;
}
/* LOGON_UI's private grants, installed by PID 1 before publication. */
int grants_install_trusted(struct process *p, int32_t fds[2])
{
    if (!p || p == proc_supervisor() || !p->fds || p->published) return -EPERM;
    if (trusted_owner) return -EBUSY;
    int err = grant_pair(p, true, 0, fds);
    if (err) return err;
    uint32_t f = irq_save();
    trusted_owner = p->pid;
    trusted_descriptions = 2;
    if (!user_active()) {
        console_trusted = true;
        if (input_secure_domain) input_secure_domain(true, 0);
    }
    irq_restore(f);
    grant_owned();
    return 0;
}
/* SESSION_LAUNCH desktop grants: suspended until activation. One session
 * domain at a time; a lingering earlier owner blocks the next logon. */
int grants_install_session(struct process *p, uint32_t session, int32_t fds[2])
{
    if (!p || p == proc_supervisor() || !p->fds || p->published || !session) return -EPERM;
    if (grant_owner) return -EBUSY;
    int err = grant_pair(p, false, session, fds);
    if (err) return err;
    uint32_t f = irq_save();
    grant_owner = p->pid;
    grant_descriptions = 2;
    user_session = session;
    if (active_session == session) active_session = 0;
    irq_restore(f);
    grant_owned();
    return 0;
}
/* --- sec-k07 (end) --- */
uint32_t grants_display_pid(void) { return user_active() ? grant_owner : 0; } /* sec-k07: active USER owner */
int grants_display_suspend(bool suspended)
{
    if (suspended && (display_suspended || presenting || !user_active())) return -EBUSY;
    display_suspended = suspended;
    return 0;
}
extern void display_frame_complete(uint32_t generation, bool full_frame) __attribute__((weak));
extern void entropy_desktop_presented(void) __attribute__((weak));
extern void dosvm_image_desktop_presented(void) __attribute__((weak)); /* fx-4b */
extern void bootstat_presented(void) __attribute__((weak)); /* G14: the start is complete */
bool grants_display_owner(const struct process *p)
{
    if (!p || p->pid != grant_owner || !user_active()) return false; /* sec-k07 */
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
    grant_batch_close(d);
    if (!d->owner) return;
    desktop_objects.grants--;
    /* sec-k07: each domain forgets its owner with its last description. */
    if (d->grant_trusted) {
        if (!--trusted_descriptions) trusted_owner = 0;
    } else if (!--grant_descriptions) {
        grant_owner = 0;
        user_session = 0;
    }
    /* The last grant description closes: the screen console draws again. */
    if (!grant_descriptions && !trusted_descriptions) {
        fbdev_cursor_hide();
        console_display_owned(false);
    }
}

int grants_install(struct process *p, int32_t fds[2])
{
    /* sec-k07: never a legacy grant beside the trusted logon domain. */
    if (!p || p == proc_supervisor() || !p->fds || grant_owner || trusted_owner) return -EPERM;
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
    user_session = active_session = 0; /* sec-k07: interim USER domain, always active */
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
    if (d->owner != p->pid || !(d->maximum & right)) return -EACCES;
    return grant_domain(d, kind); /* sec-k07 */
}
/* Geometry and capability queries of a concealed session's own display:
 * concealment masks presentation and input (security.h), not the queries
 * the session desktop needs to compose its first frame before LOGON
 * activates it (logon_session.c). The trusted pair keeps its own rule. */
static int grant_query_check(struct process *p, int32_t fd)
{
    int err = grant_check(p, fd, DESKTOP_DISPLAY, PROT_WRITE);
    if (err != -EBUSY || display_suspended) return err;
    const struct desktop_description *d = desktop_fd(p, fd);
    return d && !d->grant_trusted ? 0 : err;
}
int grant_batch_authorized(struct process *p, struct desktop_description *d)
{
    if (!d || d->kind!=DESKTOP_DISPLAY) return -EBADF;
    if (d->owner!=p->pid || !(d->maximum&PROT_WRITE)) return -EACCES;
    /* sec-k07: the trusted UI has no batch engine; batches re-check the
     * domain through this callback between slices and stop when concealed. */
    if (d->grant_trusted) return -EOPNOTSUPP;
    int domain=grant_domain(d,DESKTOP_DISPLAY);
    if (domain==-EACCES) return domain;
    if (display_suspended || domain) return -EBUSY;
    return p->state==PROC_LIVE ? 0 : -ECANCELED;
}
void grant_batch_busy(bool busy) { if (busy) presenting++; else presenting--; }

struct accel_operation { struct desktop_description *grant; bool presenting; uint32_t page; };
static bool grant_still_active(const struct desktop_description *d) /* sec-k07 */
{ return !grant_domain(d, DESKTOP_DISPLAY); }
int grant_accel_info(struct process *p, int32_t fd, const struct ciuki_accel_request *q,
                     struct ciuki_accel_info *out)
{
    int err = grant_query_check(p,fd);
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
    if (op->presenting) { presenting--; if (op->grant->grant_trusted) trusted_presenting--; }
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
            int e = fbdev_accel_write_epoch(q->generation, &rect, &rect, buffer, tw * 4u, q->background);
            if (e < 0) err = e;
            else if (e) cpu = 1;
            kpreempt_point();
            if (!err && (p->state != PROC_LIVE || (t && proc_signal_caught(t)))) err = -EINTR;
            if (!err && !grant_still_active(op->grant)) err = -EBUSY; /* sec-k07 */
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
    if (op.grant->grant_trusted) trusted_presenting++; /* sec-k07 */
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
    int err = grant_query_check(p, fd);
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
    if (op->presenting) { presenting--; if (op->grant->grant_trusted) trusted_presenting--; } /* sec-k07 */
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
    if (s->format!=CIUKI_SURFACE_XRGB8888 && s->format!=CIUKI_SURFACE_BATCH_XRGB8888) return -EOPNOTSUPP;
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
    bool trusted_grant = op.grant->grant_trusted; /* sec-user-06: timing_frame after cleanup */
    if (op.grant->grant_trusted) trusted_presenting++; /* sec-k07 */
    for (unsigned y = 0; y < clipped.height && !err; y++) {
        for (unsigned x = 0; x < clipped.width; x += PAGE_SIZE / 4) {
            uint32_t width = clipped.width - x;
            if (width > PAGE_SIZE / 4) width = PAGE_SIZE / 4;
            struct fb_surface src = { P2V(op.row_page), width, 1, width * 4, width * 4 };
            struct fb_rect dst = { clipped.dst_x + (int32_t)x, clipped.dst_y + (int32_t)y, (int32_t)width, 1 };
            err=surface_read(op.surface, ((uint32_t)clipped.src_y + y) * s->stride + ((uint32_t)clipped.src_x + x) * 4,
                         P2V(op.row_page), width * 4);
            if (err) break;
            err = fbdev_present(&src, &dst);
            if (err) { if (err == -ENODEV) err = -EIO; break; }
            /* Conversion/copy is at most one 4 KiB tile. The F1 mutex and
             * console drawing claim have both been released here. The
             * operation retains its surface and grant across the yield. */
            kpreempt_point();
            if (p->state != PROC_LIVE || (t && proc_signal_caught(t))) err = -EINTR;
            else if (!grant_still_active(op.grant)) err = -EBUSY; /* sec-k07: concealed */
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
        if (full_frame && dosvm_image_desktop_presented) dosvm_image_desktop_presented(); /* fx-4b */
        if (full_frame && bootstat_presented) bootstat_presented();
        timing_frame(trusted_grant); /* sec-user-06 */
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
    case INPUT_ATTENTION: out->type = CIUKI_INPUT_ATTENTION; out->code = in->code; break; /* sec-k07 */
    }
}

struct input_operation {
    struct process *process;
    struct desktop_description *grant;
    struct ua_pin *pin;
};
extern int input_keyboard_settings(const struct ciuki_keyboard_settings *) __attribute__((weak));
extern int usb_hid_query(uint32_t, void *) __attribute__((weak));
extern int joystick_hid_query(uint32_t, void *) __attribute__((weak));
extern int joystick_control(struct ciuki_joystick_control *) __attribute__((weak));
extern int joystick_settings_calibrate(const struct ciuki_joystick_control *) __attribute__((weak));
extern int joystick_settings_select(struct process *) __attribute__((weak)); /* sec-user-05 */
int grant_input_devctl(struct process *p, int32_t fd, uint32_t command, uint32_t address, uint32_t bytes)
{
    int err=grant_check(p,fd,DESKTOP_INPUT,PROT_READ);
    if (err) return err;
    if (!security_process_live(p)) return -EACCES;
    /* sec-user-05: joystick preferences are per session. Before any
     * calibration or joystick operation the caller's scope is selected:
     * SYSTEM the machine store, an interactive session its own profile store
     * (joystick_storage.c); machine defaults are never changed by a session. */
    if ((command==INPUT_HID_CALIBRATE || command==INPUT_JOY_CONTROL) && joystick_settings_select &&
        (err=joystick_settings_select(p))) return err;
    bool personal_store=joystick_settings_select!=0;
    if (command==INPUT_HID_CALIBRATE && !personal_store && !security_device_system(p)) return -EOPNOTSUPP;
    if (command == INPUT_KEYBOARD_SET) {
        struct ciuki_keyboard_settings settings;
        if (bytes != sizeof(settings)) return -EINVAL;
        err = copy_from_user(&settings, address, bytes);
        if (!err) err=grant_check(p,fd,DESKTOP_INPUT,PROT_READ);
        if (!err && !security_process_live(p)) err=-EACCES;
        return err ? err : input_keyboard_settings ? input_keyboard_settings(&settings) : -ENODEV;
    }
    unsigned expected=command==INPUT_HID_GET_CAPS ? sizeof(struct ciuki_hid_caps) :
        command==INPUT_HID_GET_STATE ? sizeof(struct ciuki_hid_state) :
        command==INPUT_HID_CALIBRATE ? sizeof(struct ciuki_hid_calibration) :
        command==INPUT_JOY_CONTROL ? sizeof(struct ciuki_joystick_control) : 0;
    if (!expected) return -ENOTTY;
    if (bytes!=expected) return -EINVAL;
    union { struct ciuki_hid_caps caps; struct ciuki_hid_state state;
        struct ciuki_hid_calibration calibration; struct ciuki_joystick_control joystick; uint32_t header[4]; } record;
    struct ua_pin pin;
    err=ua_pin(p->memory,&pin,address,bytes,true);
    if (err) return err;
    err=copy_from_user(&record,address,bytes);
    if (!err && (record.header[0]!=(command==INPUT_JOY_CONTROL ? CIUKI_JOYSTICK_VERSION : CIUKI_HID_VERSION) || record.header[1]!=bytes ||
        (command==INPUT_HID_GET_CAPS && record.caps.reserved) ||
        (command==INPUT_HID_GET_STATE && record.state.reserved))) err=-EINVAL;
    /* Without the per-session store, personal changes must not reach the
     * machine store (sec-k04's refusal). */
    if (!err && command==INPUT_JOY_CONTROL && !personal_store && !security_device_system(p)) {
        switch (record.joystick.operation) {
        case CIUKI_JOY_CALIBRATE: case CIUKI_JOY_ROUTE: case CIUKI_JOY_CONFIGURE: case CIUKI_JOY_SAVE_GAME:
            err=-EOPNOTSUPP; break;
        default: break;
        }
    }
    if (!err) err=grant_check(p,fd,DESKTOP_INPUT,PROT_READ);
    if (!err && !security_process_live(p)) err=-EACCES;
    if (!err) {
        uint32_t flags=irq_save();
        if (command==INPUT_HID_CALIBRATE && joystick_settings_calibrate) {
            const struct ciuki_hid_calibration *cal=&record.calibration;
            struct ciuki_joystick_control c={.version=1,.size=sizeof(c),.operation=CIUKI_JOY_CALIBRATE,
                .device=cal->device,.generation=cal->generation,.axis=cal->axis,
                .minimum=cal->minimum,.centre=cal->centre,.maximum=cal->maximum};
            err=joystick_settings_calibrate(&c);
        } else err=command==INPUT_JOY_CONTROL ? (joystick_control ? joystick_control(&record.joystick) : -ENODEV) :
            joystick_hid_query ? joystick_hid_query(command,&record) :
            usb_hid_query ? usb_hid_query(command,&record) : -ENODEV;
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
    if (err && err != -EBUSY) return err; /* sec-k07: a suspended domain reads no events */
    err = 0;
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
        /* sec-k07: each turn re-proves the domain; dequeue is atomic with it. */
        int live = grant_domain(d, DESKTOP_INPUT);
        if (live == -EACCES) { err = -EACCES; break; }
        extern bool dosvm_fullscreen_desktop_event(struct ciuki_input_event *) __attribute__((weak));
        struct ciuki_input_event pending;
        if (!live && !d->grant_trusted && dosvm_fullscreen_desktop_event && dosvm_fullscreen_desktop_event(&pending)) {
            err=copy_to_user(events+count*sizeof(pending),&pending,sizeof(pending));
            if (err || ++count==capacity) break;
            continue;
        }
        struct input_event in;
        bool have = !live && (input_read_domain ? input_read_domain(&in, d->grant_trusted) :
                              !d->grant_trusted && input_read(&in));
        if (have) {
            if (in.type == INPUT_ABS) continue; /* internal pad sample; REL/BTN is the public projection */
            if (in.type == INPUT_ATTENTION && !d->grant_trusted) continue; /* sec-k07: trusted UI only */
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
