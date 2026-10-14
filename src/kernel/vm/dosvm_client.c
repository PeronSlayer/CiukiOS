/* f3-02 native owner boundary: syscall 72 (dosvm) for one DOS window VM per
 * process, delegated only to a desktop-launched /Programs/dosbox. No public VM
 * lookup, device grant or physical display access. execution-abi.md, F3-02.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/abi.h>
#include <ciuki/desktop.h>
#include <ciuki/supervisor.h>
#include "dosvm_internal.h"

struct binding {
    struct dosvm_key key;
    uint32_t pid;
    struct surface *surface;
    bool busy, focused;
};
static struct binding bindings[DOSVM_LIMIT];
static uint32_t creating[DOSVM_LIMIT]; /* owners inside a sleeping create */
static bool owns_vm(uint32_t pid)
{
    for (unsigned i = 0; i < DOSVM_LIMIT; i++)
        if (bindings[i].pid == pid || creating[i] == pid) return true;
    return false;
}

/* ---- Window input: the f3-01 device path ----
 * Focus and events go to dosvm_input_focus/dosvm_input_event: KEY reaches
 * the virtual 8042 as set-1 bytes and IRQ1, whose synthetic INT 9 fills the
 * BIOS buffer (dosvm_kbd.c); MOTION/BUTTON drive the INT 33h model and its
 * callbacks; TEXT is ignored because KEY already carries the key; focus loss
 * and RESYNC release held keys and buttons. */
static void window_focus(struct binding *b, bool focused)
{
    b->focused = focused;
    dosvm_input_focus(b->key, focused);
}
static int window_input(struct binding *b, const struct ciuki_input_event *e)
{
    /* The record was validated; a refusal is a full guest queue (or a VM that
     * lost the single input focus to another window: the desktop's focus
     * message to this window follows). */
    return dosvm_input_event(b->key, e) ? 0 : -EAGAIN;
}

/* ---- Owner lifecycle ---- */
static int release_binding(struct binding *b)
{
    struct dosvm *v = dosvm_lookup(b->key);
    /* A VM that already stopped (EXIT, fault) is not forced: forced
     * termination is what reports possible private-data loss. */
    bool forced = v && v->info.state < DOSVM_STOPPING;
    int e = dosvm_destroy(b->key, forced);
    v = dosvm_lookup(b->key);
    if (!v || v->info.state == DOSVM_DEAD) {
        if (b->surface) surface_release(b->surface);
        memset(b, 0, sizeof(*b));
    }
    return e;
}
/* Supervisor collection hook: the owner died (crash, signal, exit without
 * CLOSE). Idempotent; a binding busy in its owner's syscall is collected by
 * that syscall's exit check or by a later collection pass. */
void dosvm_client_poll(void)
{
    for (unsigned i = 0; i < DOSVM_LIMIT; i++) {
        struct binding *b = &bindings[i];
        if (!b->pid || b->busy) continue;
        struct process *p = proc_find(b->pid);
        if (!p || p->state >= PROC_STOPPING) {
            b->busy = true;
            release_binding(b);
            b->busy = false;
        }
    }
}

/* Probe view (f3:vm-window): the first window VM and its owner, if any. */
struct dosvm *dosvm_client_first(uint32_t *owner)
{
    for (unsigned i = 0; i < DOSVM_LIMIT; i++) {
        struct dosvm *v = bindings[i].pid ? dosvm_lookup(bindings[i].key) : 0;
        if (v) { if (owner) *owner = bindings[i].pid; return v; }
    }
    return 0;
}

static int put_row(void *arg, unsigned y, const void *pixels, unsigned bytes)
{ return surface_write(arg, y * CIUKI_DOSVM_WIDTH * 4, pixels, bytes); }
static bool event_valid(const struct ciuki_input_event *e)
{
    switch (e->type) {
    case CIUKI_INPUT_KEY: return e->code > 0 && e->code <= 0x200 && e->value >= 0 && e->value <= 2 && !e->value2 && !e->lost_count;
    case CIUKI_INPUT_TEXT: return e->code > 0 && e->code <= 0x10FFFF && !(e->code >= 0xD800 && e->code <= 0xDFFF) && !e->value && !e->value2 && !e->lost_count;
    case CIUKI_INPUT_MOTION: return !e->code && !e->lost_count;
    case CIUKI_INPUT_BUTTON: return e->code >= 1 && e->code <= 3 && e->value >= 0 && e->value <= 1 && !e->value2 && !e->lost_count;
    case CIUKI_INPUT_RESYNC: return !e->code && e->value >= 0 && e->value <= 7 && !e->value2;
    default: return false;
    }
}
static uint32_t public_state(const struct dosvm *v)
{
    if (v->info.state >= DOSVM_STOPPING) return CIUKI_DOSVM_STOPPED;
    return v->info.dos_ready ? CIUKI_DOSVM_RUNNING : CIUKI_DOSVM_BOOTING;
}
/* Closed error set (execution-abi.md): internal storage/boot errors are EIO. */
static int closed_error(int e)
{
    switch (e) {
    case 0: case -ENOMEM: case -ENOSPC: case -EINTR: case -EBUSY: return e;
    default: return -EIO;
    }
}
static void video_mode(struct dosvm *v, struct ciuki_dosvm_command *c)
{
    uint8_t mode = 0;
    if (!dosvm_read(v, 0x449, &mode, 1)) c->video_mode = mode;
}

int32_t dosvm_client_syscall(struct trap_frame *tf)
{
    struct process *p = proc_current();
    if (!p || !p->dosvm_capability) return -EPERM;
    unsigned op = tf->ebx;
    if (op < CIUKI_DOSVM_CREATE || op > CIUKI_DOSVM_PRESENT_BANDS) return -EINVAL;
    if (tf->edx != sizeof(struct ciuki_dosvm_command)) return -EINVAL;
    struct ciuki_dosvm_command c, allowed;
    int e = copy_from_user(&c, tf->ecx, sizeof(c));
    if (e) return e;
    if (c.version != CIUKI_DOSVM_VERSION) return -EOPNOTSUPP;
    if (c.size != sizeof(c)) return -EINVAL;
    /* Every field the operation does not take, outputs included, must be zero. */
    memset(&allowed, 0, sizeof(allowed));
    allowed.version = c.version; allowed.size = c.size;
    if (op == CIUKI_DOSVM_CREATE) {
        allowed.surface_fd = c.surface_fd;
        unsigned n = 0;
        while (n < sizeof(c.name) && c.name[n]) {
            if (c.name[n] < 32 || c.name[n] > 126) return -EINVAL;
            allowed.name[n] = c.name[n]; n++;
        }
        if (!n || n > CIUKI_DOSVM_NAME_MAX) return -EINVAL;
    } else { allowed.id = c.id; allowed.generation = c.generation; }
    if (op == CIUKI_DOSVM_FOCUS) {
        if (c.focused > 1) return -EINVAL;
        allowed.focused = c.focused;
    }
    if (op == CIUKI_DOSVM_INPUT) {
        if (!event_valid(&c.event)) return -EINVAL;
        allowed.event = c.event;
    }
    if (memcmp(&allowed, &c, sizeof(c))) return -EINVAL;
    /* The record stays writable across a sleeping create/close. */
    struct ua_pin pin;
    e = ua_pin(p->memory, &pin, tf->ecx, sizeof(c), true);
    if (e) return e;
    struct binding *b = 0;
    struct dosvm *v = 0;
    if (op == CIUKI_DOSVM_CREATE) {
        struct desktop_description *d = desktop_fd(p, c.surface_fd);
        if (!d || d->kind != DESKTOP_SURFACE) { e = -EBADF; goto out; }
        if (!(d->maximum & PROT_WRITE)) { e = -EACCES; goto out; }
        const struct ciuki_surface_info *g = surface_geometry(d->u.surface);
        if (g->width != CIUKI_DOSVM_WIDTH || g->height != CIUKI_DOSVM_HEIGHT ||
            g->format != CIUKI_SURFACE_XRGB8888 || g->stride != CIUKI_DOSVM_WIDTH * 4) { e = -EINVAL; goto out; }
        if (owns_vm(p->pid)) { e = -EBUSY; goto out; }
        unsigned reservation = 0;
        while (reservation < DOSVM_LIMIT && creating[reservation]) reservation++;
        if (reservation == DOSVM_LIMIT) { e = -ENOSPC; goto out; }
        creating[reservation] = p->pid;
        /* f3-06: an owner-attached local workload disk (\CIUKIGAM.IMG, the
         * DOS system plus \DOOM or \WOLF3D) becomes the window VM's base,
         * with the staged DPMI host (DOSVM_DPMI_PUBLIC is still 0): the
         * owner-visible DOOM window. Without it, the bundled shareware games
         * disk (C:\GAMES\CIUKIGAM.IMG, owner decision 2026-10-12) does;
         * with neither, DOSBOOT.IMG and no DPMI host, as before. */
        char workload[24];
        int found = dosvm_window_base(workload);
        if (found && found != -ENOENT) { creating[reservation] = 0; e = closed_error(found); goto out; }
        /* Creation may sleep opening storage; another thread may close the
         * descriptor meanwhile, so the VM holds its own surface reference. */
        struct surface *surface = d->u.surface;
        surface_retain(surface);
        const struct dosvm_config config = {.probe = "vm-window", .base = found ? 0 : workload, .dpmi = !found};
        e = dosvm_create(&config, &v);
        creating[reservation] = 0;
        if (e) { surface_release(surface); e = closed_error(e); goto out; }
        struct dosvm_key key = dosvm_key(v);
        b = &bindings[key.id - 1];
        *b = (struct binding){key, p->pid, surface, true, false};
        v->owner_pid = p->pid;
        memcpy(v->name, c.name, sizeof(v->name));
        if (p->state != PROC_LIVE) e = -EINTR;
        else e = dosvm_boot(v);
        if (e) {
            release_binding(b);
            if (!b->pid) b = 0; /* released; otherwise collected below/later */
            e = closed_error(e); goto out;
        }
        c.id = key.id; c.generation = key.generation;
        c.state = public_state(v); video_mode(v, &c);
    } else {
        struct dosvm_key key = {c.id, c.generation};
        if (!key.id || key.id > DOSVM_LIMIT) { e = -ESRCH; goto out; }
        b = &bindings[key.id - 1];
        if (!b->pid || b->pid != p->pid || b->key.generation != key.generation) { e = -ESRCH; b = 0; goto out; }
        if (b->busy) { e = -EBUSY; b = 0; goto out; }
        b->busy = true;
        v = dosvm_lookup(key);
        if (!v) { e = -ESRCH; goto out; }
        if (op == CIUKI_DOSVM_CLOSE) {
            e = closed_error(release_binding(b));
            if (!b->pid) b = 0; /* binding released */
        } else if (op == CIUKI_DOSVM_STATUS) {
            c.state = public_state(v);
            c.reason = c.state == CIUKI_DOSVM_STOPPED ? v->info.reason : 0;
            c.focused = b->focused;
            if (v->launch_exec && v->video_direct) for (unsigned i = 0; i < 16; i++) {
                uint32_t pte = 0;
                as_lookup(&v->as, 0xA0000 + i * PAGE_SIZE, &pte);
                if (pte & 0x40) { dosvm_launch_video(v); break; }
            }
            c.changed = (!v->launch_armed || v->launch_ready ? CIUKI_DOSVM_VIDEO_READY : 0) |
                        (v->launch_failed ? CIUKI_DOSVM_LAUNCH_FAILED : 0);
            if (v->launch_failed && c.state != CIUKI_DOSVM_STOPPED) c.reason = v->launch_error;
            if (v->launch_failed || c.state == CIUKI_DOSVM_STOPPED)
                memcpy(c.name, v->console_line, sizeof(c.name) - 1);
            video_mode(v, &c);
            dosvm_perf_report(v);
        } else if (!dosvm_valid(key)) e = -ESRCH;
        else if (op == CIUKI_DOSVM_LAUNCH) {
            if (v->launch_armed) e = -EBUSY;
            else if (!v->info.dos_ready) e = -EAGAIN;
            else {
                v->launch_armed = true;
                v->video_presented = false;
            }
        } else if (op == CIUKI_DOSVM_PRESENT || op == CIUKI_DOSVM_PRESENT_BANDS) {
            if (v->idle.sleeping) dosvm_idle_wake(v);
            e = dosvm_vga_present(v, put_row, b->surface, g_ticks, &c.damage);
            if (e >= 0) {
                c.changed = (uint32_t)e; e = 0; video_mode(v, &c);
                if (op == CIUKI_DOSVM_PRESENT_BANDS) {
                    c.presentation.bands[0] = (uint32_t)v->video_bands;
                    c.presentation.bands[1] = (uint32_t)(v->video_bands >> 32);
                }
            }
            else e = -EIO;
        } else if (op == CIUKI_DOSVM_FOCUS) {
            window_focus(b, c.focused);
        } else if (op == CIUKI_DOSVM_INPUT) {
            if (!b->focused) e = -EACCES;
            else e = window_input(b, &c.event);
        }
    }
    if (!e) e = copy_to_user(tf->ecx, &c, sizeof(c));
out:
    if (b) {
        b->busy = false;
        /* The owner began stopping during this call: collect now, since the
         * supervisor pass skipped the busy binding. */
        if (p->state != PROC_LIVE && b->pid) release_binding(b);
    }
    ua_unpin(p->memory, &pin);
    return e;
}
