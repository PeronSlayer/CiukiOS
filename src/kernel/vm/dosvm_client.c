/* f3-02 native owner boundary: syscall 70 (dosvm) for one DOS window VM per
 * process, delegated only to a desktop-launched /bin/dosbox. No public VM
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
    uint8_t mods;          /* window keyboard adapter state, see below */
};
static struct binding bindings[DOSVM_LIMIT];
static uint32_t creating[DOSVM_LIMIT]; /* owners inside a sleeping create */
static bool owns_vm(uint32_t pid)
{
    for (unsigned i = 0; i < DOSVM_LIMIT; i++)
        if (bindings[i].pid == pid || creating[i] == pid) return true;
    return false;
}

/* ---- Window keyboard adapter (until the f3-01 virtual keyboard) ----
 * Focused desktop KEY events (public set-1 positions, E0 keys = 100h|code)
 * become BIOS INT 16h words (scan << 8 | ASCII), US layout, through the
 * existing dosvm_keyboard() queue. TEXT is ignored: KEY already carries the
 * key with its modifiers. MOTION/BUTTON have no consumer before the f3-01
 * INT 33h mouse. When f3-01 is merged, window_focus/window_input become
 * dosvm_input_focus(key, focused) and dosvm_input_event(key, event). */
enum { MOD_SHIFT_L = 1, MOD_SHIFT_R = 2, MOD_CTRL = 4, MOD_ALT = 8, MOD_CAPS = 16, MOD_NUM = 32 };
static const char key_plain[0x3A] = {
    [0x01]=27, [0x02]='1', [0x03]='2', [0x04]='3', [0x05]='4', [0x06]='5', [0x07]='6',
    [0x08]='7', [0x09]='8', [0x0A]='9', [0x0B]='0', [0x0C]='-', [0x0D]='=', [0x0E]=8,
    [0x0F]=9, [0x10]='q', [0x11]='w', [0x12]='e', [0x13]='r', [0x14]='t', [0x15]='y',
    [0x16]='u', [0x17]='i', [0x18]='o', [0x19]='p', [0x1A]='[', [0x1B]=']', [0x1C]=13,
    [0x1E]='a', [0x1F]='s', [0x20]='d', [0x21]='f', [0x22]='g', [0x23]='h', [0x24]='j',
    [0x25]='k', [0x26]='l', [0x27]=';', [0x28]='\'', [0x29]='`', [0x2B]='\\', [0x2C]='z',
    [0x2D]='x', [0x2E]='c', [0x2F]='v', [0x30]='b', [0x31]='n', [0x32]='m', [0x33]=',',
    [0x34]='.', [0x35]='/', [0x37]='*', [0x39]=' ',
};
static const char key_shift[0x3A] = {
    [0x01]=27, [0x02]='!', [0x03]='@', [0x04]='#', [0x05]='$', [0x06]='%', [0x07]='^',
    [0x08]='&', [0x09]='*', [0x0A]='(', [0x0B]=')', [0x0C]='_', [0x0D]='+', [0x0E]=8,
    [0x10]='Q', [0x11]='W', [0x12]='E', [0x13]='R', [0x14]='T', [0x15]='Y', [0x16]='U',
    [0x17]='I', [0x18]='O', [0x19]='P', [0x1A]='{', [0x1B]='}', [0x1C]=13, [0x1E]='A',
    [0x1F]='S', [0x20]='D', [0x21]='F', [0x22]='G', [0x23]='H', [0x24]='J', [0x25]='K',
    [0x26]='L', [0x27]=':', [0x28]='"', [0x29]='~', [0x2B]='|', [0x2C]='Z', [0x2D]='X',
    [0x2E]='C', [0x2F]='V', [0x30]='B', [0x31]='N', [0x32]='M', [0x33]='<', [0x34]='>',
    [0x35]='?', [0x37]='*', [0x39]=' ',
};
/* IBM PC AT BIOS keyboard words, NumLock off; grey keys use the 00h form
 * that INT 16h AH=00h returns. */
static uint16_t navigation(unsigned scan, bool ctrl)
{
    static const uint8_t ctrl_scan[13] = {0x77, 0x8D, 0x84, 0x8E, 0x73, 0x8F, 0x74, 0x90,
                                          0x75, 0x91, 0x76, 0x92, 0x93};
    if (scan < 0x47 || scan > 0x53) return 0;
    if (scan == 0x4A) return ctrl ? 0x8E00 : 0x4A2D;
    if (scan == 0x4E) return ctrl ? 0x9000 : 0x4E2B;
    return (uint16_t)((ctrl ? ctrl_scan[scan - 0x47] : scan) << 8);
}
static uint16_t numpad(unsigned scan)
{
    static const char digits[] = "789-456+1230.";
    return scan >= 0x47 && scan <= 0x53 ? (uint16_t)(scan << 8 | (uint8_t)digits[scan - 0x47]) : 0;
}
/* Returns the BIOS word for a key press, or 0 for no keyboard character. */
static uint16_t bios_word(unsigned code, uint8_t mods)
{
    bool shift = mods & (MOD_SHIFT_L | MOD_SHIFT_R), ctrl = mods & MOD_CTRL, alt = mods & MOD_ALT;
    bool extended = code & 0x100;
    unsigned scan = code & 0xFF;
    if (extended) {
        if (scan == 0x1C) return ctrl ? 0x1C0A : 0x1C0D;        /* keypad Enter */
        if (scan == 0x35) return 0x352F;                        /* keypad / */
        return navigation(scan, ctrl);
    }
    if (scan >= 0x3B && scan <= 0x44) {                         /* F1-F10 */
        unsigned base = alt ? 0x68 : ctrl ? 0x5E : shift ? 0x54 : 0x3B;
        return (uint16_t)((base + scan - 0x3B) << 8);
    }
    if (scan == 0x57 || scan == 0x58) {                         /* F11, F12 */
        unsigned base = alt ? 0x8B : ctrl ? 0x89 : shift ? 0x87 : 0x85;
        return (uint16_t)((base + scan - 0x57) << 8);
    }
    if (scan >= 0x47 && scan <= 0x53 && scan != 0x4A && scan != 0x4E)
        return ((mods & MOD_NUM) != 0) != shift ? numpad(scan) : navigation(scan, ctrl);
    if (scan == 0x4A || scan == 0x4E) return navigation(scan, ctrl);
    if (scan >= sizeof(key_plain) || !key_plain[scan]) return 0;
    char c = key_plain[scan];
    if (alt) {
        if (scan >= 0x02 && scan <= 0x0D) return (uint16_t)((scan + 0x76) << 8); /* Alt+1..= */
        return (uint16_t)(scan << 8);
    }
    if (ctrl) {
        if (c >= 'a' && c <= 'z') return (uint16_t)(scan << 8 | (c - 'a' + 1));
        if (scan == 0x1C) return 0x1C0A;
        if (scan == 0x0E) return 0x0E7F;
        if (scan == 0x03) return 0x0300;
        if (scan == 0x07) return 0x071E;
        if (scan == 0x0C) return 0x0C1F;
        if (scan == 0x1A) return 0x1A1B;
        if (scan == 0x2B) return 0x2B1C;
        if (scan == 0x1B) return 0x1B1D;
        return 0;
    }
    if (scan == 0x0F) return shift ? 0x0F00 : 0x0F09;
    bool letter = c >= 'a' && c <= 'z';
    bool upper = letter ? shift != ((mods & MOD_CAPS) != 0) : shift;
    return (uint16_t)(scan << 8 | (uint8_t)(upper ? key_shift[scan] : c));
}
static void window_focus(struct binding *b, bool focused)
{
    b->focused = focused;
    /* Focus loss releases held modifiers; lock states persist. */
    b->mods &= MOD_CAPS | MOD_NUM;
}
static int window_input(struct binding *b, const struct ciuki_input_event *e)
{
    uint8_t bit = 0;
    switch (e->type) {
    case CIUKI_INPUT_RESYNC: b->mods &= MOD_CAPS | MOD_NUM; return 0;
    case CIUKI_INPUT_TEXT: case CIUKI_INPUT_MOTION: case CIUKI_INPUT_BUTTON: return 0;
    case CIUKI_INPUT_KEY: break;
    default: return -EINVAL;
    }
    switch (e->code) {
    case 0x2A: bit = MOD_SHIFT_L; break;
    case 0x36: bit = MOD_SHIFT_R; break;
    case 0x1D: case 0x11D: bit = MOD_CTRL; break;
    case 0x38: case 0x138: bit = MOD_ALT; break;
    default: break;
    }
    if (bit) {
        if (e->value) b->mods |= bit; else b->mods &= (uint8_t)~bit;
        return 0;
    }
    if (!e->value) return 0;
    if (e->code == 0x3A) { if (e->value == 1) b->mods ^= MOD_CAPS; return 0; }
    if (e->code == 0x45) { if (e->value == 1) b->mods ^= MOD_NUM; return 0; }
    uint16_t word = bios_word((unsigned)e->code, b->mods);
    if (!word) return 0;
    return dosvm_keyboard(b->key, word) ? 0 : -EAGAIN;
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
    if (op < CIUKI_DOSVM_CREATE || op > CIUKI_DOSVM_STATUS) return -EINVAL;
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
        /* Creation may sleep opening storage; another thread may close the
         * descriptor meanwhile, so the VM holds its own surface reference. */
        struct surface *surface = d->u.surface;
        surface_retain(surface);
        const struct dosvm_config config = {.probe = "vm-window"};
        e = dosvm_create(&config, &v);
        creating[reservation] = 0;
        if (e) { surface_release(surface); e = closed_error(e); goto out; }
        struct dosvm_key key = dosvm_key(v);
        b = &bindings[key.id - 1];
        *b = (struct binding){key, p->pid, surface, true, false, 0};
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
            video_mode(v, &c);
        } else if (!dosvm_valid(key)) e = -ESRCH;
        else if (op == CIUKI_DOSVM_PRESENT) {
            e = dosvm_vga_present(v, put_row, b->surface, g_ticks, &c.damage);
            if (e >= 0) { c.changed = (uint32_t)e; e = 0; video_mode(v, &c); }
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
