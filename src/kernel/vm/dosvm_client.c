/* Native owner/capability boundary. No public VM lookup or hardware grants.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/desktop.h>
#include <ciuki/supervisor.h>
#include "dosvm_internal.h"

struct binding { struct dosvm_key key; uint32_t pid; struct surface *surface; bool busy; };
static struct binding bindings[DOSVM_LIMIT];
static dosvm_input_handler input_handler;
void dosvm_input_register(dosvm_input_handler fn) { input_handler = fn; }
bool dosvm_input_event(struct dosvm_key key, const struct ciuki_input_event *event, bool focused)
{
    struct dosvm *v = dosvm_lookup(key);
    if (!v || !dosvm_valid(key)) return false;
    v->focused = focused;
    if (input_handler) return input_handler(key, event, focused);
    /* Core-only integration keeps printable BIOS keys usable; full set-1,
     * releases and mouse callbacks are delivered by the f3-01 consumer. */
    if (!focused || !event) return true;
    if (event->type == CIUKI_INPUT_TEXT && event->code > 0 && event->code < 128)
        return dosvm_keyboard(key, event->code);
    if (event->type == CIUKI_INPUT_KEY && event->value &&
        (event->code == 0x1C || event->code == 0x0E || event->code == 0x01))
        return dosvm_keyboard(key, (event->code << 8) | (event->code == 0x1C ? 13 : event->code == 0x0E ? 8 : 27));
    return true;
}
static int cleanup(struct binding *b, bool forced)
{
    dosvm_input_event(b->key, 0, false);
    int e = dosvm_destroy(b->key, forced);
    struct dosvm *v = dosvm_lookup(b->key);
    if (v && v->info.state == DOSVM_DEAD) {
        if (b->surface) surface_release(b->surface);
        memset(b, 0, sizeof(*b));
    }
    return e;
}
void dosvm_client_poll(void)
{
    for (unsigned i = 0; i < DOSVM_LIMIT; i++) {
        struct binding *b = &bindings[i];
        if (!b->pid || b->busy) continue;
        struct process *p = proc_find(b->pid);
        if (!p || p->state >= PROC_STOPPING) {
            b->busy = true;
            cleanup(b, true);
            b->busy = false;
        }
    }
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
int32_t dosvm_client_syscall(struct trap_frame *tf)
{
    struct process *p = proc_current();
    if (!p || !p->dosvm_capability) return -EPERM;
    unsigned op = tf->ebx;
    if (op < CIUKI_DOSVM_CREATE || op > CIUKI_DOSVM_STATUS) return -EINVAL;
    struct ciuki_dosvm_command c, allowed = {0};
    int e = copy_from_user(&c, tf->ecx, sizeof(c));
    if (e) return e;
    if (c.version != CIUKI_DOSVM_VERSION) return -EOPNOTSUPP;
    if (c.size != sizeof(c)) return -EINVAL;
    allowed.version = c.version; allowed.size = c.size;
    if (op == CIUKI_DOSVM_CREATE) {
        allowed.surface_fd = c.surface_fd;
        unsigned n = 0;
        while (n < sizeof(c.name) && c.name[n]) {
            if (c.name[n] < 32 || c.name[n] > 126) return -EINVAL;
            allowed.name[n] = c.name[n]; n++;
        }
        if (!n || n == sizeof(c.name)) return -EINVAL;
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
        if (g->width != CIUKI_DOSVM_WIDTH || g->height != CIUKI_DOSVM_HEIGHT) { e = -EINVAL; goto out; }
        for (unsigned i = 0; i < DOSVM_LIMIT; i++) if (bindings[i].pid == p->pid) { e = -EBUSY; goto out; }
        /* Creation may sleep while opening storage. Retain before that point:
         * another native thread may close the descriptor in the meantime. */
        struct surface *surface = d->u.surface;
        uint32_t allocation_bytes = g->allocation_bytes;
        surface_retain(surface);
        e = dosvm_create(0, &v);
        if (e) { surface_release(surface); goto out; }
        struct dosvm_key key = dosvm_key(v);
        b = &bindings[key.id - 1];
        *b = (struct binding){key, p->pid, surface, true};
        v->owner_pid = p->pid;
        v->info.resident += allocation_bytes;
        memcpy(v->name, c.name, sizeof(v->name));
        if (p->state != PROC_LIVE) e = -EINTR;
        else e = dosvm_boot(v);
        if (e) { cleanup(b, true); goto out; }
        c.id = key.id; c.generation = key.generation;
    } else {
        struct dosvm_key key = {c.id, c.generation};
        if (!key.id || key.id > DOSVM_LIMIT) { e = -ESRCH; goto out; }
        b = &bindings[key.id - 1];
        if (b->pid != p->pid || b->key.generation != key.generation) { e = -ESRCH; b = 0; goto out; }
        if (b->busy) { e = -EBUSY; b = 0; goto out; }
        b->busy = true;
        v = dosvm_lookup(key);
        if (!v) { e = -ESRCH; goto out; }
        if (op == CIUKI_DOSVM_CLOSE) e = cleanup(b, true);
        else if (op == CIUKI_DOSVM_STATUS) c.state = v->info.state;
        else if (!dosvm_valid(key)) e = -ESRCH;
        else if (op == CIUKI_DOSVM_PRESENT) {
            e = dosvm_vga_present(v, put_row, b->surface, g_ticks, &c.damage);
            if (e >= 0) { c.changed = e; e = 0; }
        } else if (op == CIUKI_DOSVM_FOCUS) {
            dosvm_input_event(key, 0, c.focused);
        } else if (op == CIUKI_DOSVM_INPUT) {
            if (!v->focused) e = -EACCES;
            else if (!dosvm_input_event(key, &c.event, true)) e = -EAGAIN;
        }
    }
    if (!e) e = copy_to_user(tf->ecx, &c, sizeof(c));
out:
    if (b) b->busy = false;
    ua_unpin(p->memory, &pin);
    return e;
}
