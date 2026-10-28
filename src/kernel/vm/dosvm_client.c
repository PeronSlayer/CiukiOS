/* f3-02 native owner boundary: syscall 72 (dosvm) for one DOS window VM per
 * process. No public VM lookup, device grant or physical display access.
 * execution-abi.md, F3-02. sec-k05 (user-security-kernel.md "DOSVM
 * operations (call 72)"): authority comes from the caller's retained
 * credential, never an executable path. VM operations are own-session (the
 * creating process and its unchanged credential); game removal needs an
 * administrator; title data is always the caller's own store.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/abi.h>
#include <ciuki/desktop.h>
#include <ciuki/display.h>
#include <ciuki/supervisor.h>
#include <ciuki/signal.h>
#include <ciuki/probe.h>
#include "dosvm_internal.h"
#pragma weak dosvm_image_query
extern uint32_t joystick_session(void) __attribute__((weak));
extern int joystick_route_validate(const struct joystick_route *) __attribute__((weak));
extern bool joystick_settings_game(unsigned, struct joystick_route *) __attribute__((weak));
#include "dosvm_network.h"
#include <ciuki/dosvm_network_abi.h>
extern int display_worker_start(void) __attribute__((weak));

struct binding {
    struct dosvm_key key;
    uint32_t pid;
    struct surface *surface;
    bool busy, focused;
    bool observed, video_pending;
    unsigned vm_base, window_base;
    uint32_t changes, display, starts, vbe_changes, console, state, flags, reason, phase;
    uint16_t mode;
};
static struct binding bindings[DOSVM_LIMIT];
static void binding_priority(const struct binding *b,struct dosvm *v) {
    if (!b->window_base || !v->task) return;
    unsigned base=v->fullscreen ? b->vm_base : b->window_base;
    if (v->task->base_priority!=base) task_set_base(v->task,P_NORMAL,base);
}
/* Called at the actual display transition, including hotkey/watchdog paths,
 * before the parked worker is released. */
void dosvm_client_priority(struct dosvm *v) {
    struct dosvm_key key=dosvm_key(v);
    if (!key.id || key.id>DOSVM_LIMIT) return;
    const struct binding *b=&bindings[key.id-1];
    if (b->pid && b->key.generation==key.generation) binding_priority(b,v);
}
static uint32_t creating[DOSVM_LIMIT]; /* owners inside a sleeping create */
static const char *window_probe(void)
{
    /* The selected controller owns admission records too, before it can
     * discover the newly created window VM. No native input chooses this.
     * A run carries one probe name only (scripts/test/evidence.py): f3-61's
     * vm-game drives the same window path as vm-fullscreen. */
    static const struct { char prefix[24]; const char *probe; } owners[]={
        {"f3:vm-fullscreen ","vm-fullscreen"},{"f3:vm-game ","vm-game"}};
    if (rec_active() && (g_boot.flags&CBI_F_TEST_REQUEST))
        for (unsigned i=0;i<ARRAY_SIZE(owners);i++) {
            size_t n=strlen(owners[i].prefix);
            if (g_boot.test_request_len>=n && !memcmp(g_boot.test_request,owners[i].prefix,n)) return owners[i].probe;
        }
    return "vm-window";
}
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
    struct dosvm *v=dosvm_lookup(b->key);
    if (v && v->task && !focused) task_input_withdraw(v->task);
}
static int window_input(struct binding *b, const struct ciuki_input_event *e)
{
    /* The record was validated; a refusal is a full guest queue (or a VM that
     * lost the single input focus to another window: the desktop's focus
     * message to this window follows). */
    if (!dosvm_input_event(b->key,e)) return -EAGAIN;
    struct dosvm *v=dosvm_lookup(b->key);
    if (v && v->task && !v->fullscreen) task_input_forward(g_current,v->task);
    return 0;
}

/* ---- Owner lifecycle ---- */
static int release_binding(struct binding *b, bool owner_gone)
{
    struct dosvm *v = dosvm_lookup(b->key);
    if (v && v->task) task_input_withdraw(v->task);
    /* A VM that already stopped (EXIT, fault) is not forced: forced
     * termination is what reports possible private-data loss. f3-61: nor is
     * a VM whose shell is idle at its prompt (dosvm_shell_idle), e.g. after
     * a game returned to DOS; it ends as an ordinary EXIT. */
    bool forced = v && v->info.state < DOSVM_STOPPING && !dosvm_shell_idle(v);
    if (forced && !owner_gone && v->info.dos_ready) {
        /* Ask through ordinary guest keyboard input. DOS applications may
         * decline Ctrl+C; retain the VM and its private disk in that case. */
        bool requested=dosvm_kbd_event(v,0x1d,1) && dosvm_kbd_event(v,0x2e,1);
        (void)dosvm_kbd_event(v,0x2e,0); (void)dosvm_kbd_event(v,0x1d,0);
        dosvm_idle_wake(v);
        klog("[dosvm] close vm=%u generation=%u action=request-quit method=ctrl-c queued=%u outcome=retained",
             b->key.id,b->key.generation,requested);
        return -EBUSY;
    }
    if (forced) klog("[dosvm] close vm=%u generation=%u action=terminate reason=%s dirty=%u",
                    b->key.id,b->key.generation,owner_gone ? "owner-gone" : "startup-cancel",v->info.dirty);
    int e = dosvm_destroy(b->key, forced);
    v = dosvm_lookup(b->key);
    if (!v || v->info.state == DOSVM_DEAD) {
        if (b->surface) surface_release(b->surface);
        memset(b, 0, sizeof(*b));
    }
    return e;
}
/* Supervisor collection hook: the owner died (crash, signal, exit without
 * CLOSE), or sec-k05: its session left ACTIVE (logoff/teardown), so no VM
 * keeps running under a closing session. Idempotent; a binding busy in its
 * owner's syscall is collected by that syscall's exit check or by a later
 * collection pass. */
static bool binding_revoked(const struct binding *b)
{
    struct dosvm *v = dosvm_lookup(b->key);
    return v && v->info.state != DOSVM_DEAD && !dos_identity_live(&v->identity);
}
void dosvm_client_poll(void)
{
    dosvm_fullscreen_poll();
    for (unsigned i = 0; i < DOSVM_LIMIT; i++) {
        struct binding *b = &bindings[i];
        if (!b->pid || b->busy) continue;
        struct process *p = proc_find(b->pid);
        if (!p || p->state >= PROC_STOPPING || binding_revoked(b)) {
            b->busy = true;
            release_binding(b, true);
            b->busy = false;
        }
    }
}

unsigned dosvm_session_revoke(uint32_t session)
{
    unsigned retained = 0;
    if (!session) return 0;
    for (unsigned i = 0; i < DOSVM_LIMIT; i++) {
        struct binding *b = &bindings[i];
        struct dosvm *v = b->pid ? dosvm_lookup(b->key) : 0;
        if (!v || dos_identity_session(&v->identity) != session) continue;
        if (b->busy) { retained++; continue; }
        b->busy = true;
        release_binding(b, true);
        b->busy = false;
        if (b->pid) retained++;
    }
    return retained;
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

struct row_target { struct surface *surface; bool force; const struct dosvm *vm; };
static int put_row(void *arg, unsigned y, const void *pixels, unsigned bytes)
{
    struct row_target *target=arg;
    unsigned offset=y*CIUKI_DOSVM_WIDTH*4;
    /* Compare the existing shared surface, including text/VBE paths which
     * have page/band dirt rather than an exact indexed-plane row diff.
     * A small stack chunk avoids another retained VM frame or large stack.
     * fx-27: a row whose displayed indexes differ is written directly. */
    if (!target->force && !(target->vm && target->vm->video_row_proven)) {
        uint8_t previous[256];unsigned at=0;
        for (;at<bytes;at+=sizeof(previous)) {
            unsigned n=bytes-at;if(n>sizeof(previous))n=sizeof(previous);
            int error=surface_read(target->surface,offset+at,previous,n);
            if(error)return error;
            if(memcmp(previous,(const uint8_t *)pixels+at,n))break;
        }
        if(at>=bytes)return 1; /* unchanged; presenter must not mark damage */
    }
    return surface_write(target->surface,offset,pixels,bytes);
}
static bool event_valid(const struct ciuki_input_event *e)
{
    switch (e->type) {
    case CIUKI_INPUT_KEY: return e->code > 0 && e->code <= 0x200 && e->value >= 0 && e->value <= 2 && !e->value2 && !e->lost_count;
    case CIUKI_INPUT_TEXT: return e->code > 0 && e->code <= 0x10FFFF && !(e->code >= 0xD800 && e->code <= 0xDFFF) && !e->value && !e->value2 && !e->lost_count;
    case CIUKI_INPUT_MOTION: return !e->code && !e->lost_count;
    case CIUKI_INPUT_WHEEL: return !e->code && e->value>=-127 && e->value<=127 && !e->value2 && !e->lost_count;
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
static uint32_t status_flags(const struct dosvm *v)
{
    return (!v->launch_armed || v->launch_ready ? CIUKI_DOSVM_VIDEO_READY : 0) |
           (v->launch_failed ? CIUKI_DOSVM_LAUNCH_FAILED : 0) |
           (v->fullscreen ? CIUKI_DOSVM_FULL_SCREEN : 0) |
           (v->fullscreen_suspended ? CIUKI_DOSVM_SUSPENDED : 0) |
           (v->fullscreen_error ? CIUKI_DOSVM_FULLSCREEN_REFUSED : 0) |
           (v->persistent && v->save.full ? CIUKI_DOSVM_SAVE_FULL : 0) |
           (v->launch_returned && !v->exec_pending && !v->exec_overflow ? CIUKI_DOSVM_PROGRAM_RETURNED : 0) |
           (v->fullscreen_hotkey ? CIUKI_DOSVM_FULLSCREEN_HOTKEY : 0) |
           (dosvm_shell_idle(v) && v->idle.sleeping && !v->key_count && !v->kbd.count ? CIUKI_DOSVM_SHELL_READY : 0);
}
static uint32_t wait_phase(void)
{ return (uint32_t)((g_ticks / DOSVM_BLINK_TICKS) & 1) | (uint32_t)((g_ticks / DOSVM_CURSOR_TICKS) & 1) << 1; }
struct present_request {
    struct binding *binding;
    struct dosvm *vm;
    struct process *process;
    struct proc_thread *thread;
};
static bool present_interrupted(const struct present_request *r)
{
    /* Notifications run in the producer's context, not the waiter's. */
    return r->process->state != PROC_LIVE || (r->thread && proc_signal_caught(r->thread));
}
static uint32_t present_events(struct present_request *r)
{
    struct binding *b = r->binding;
    struct dosvm *v = r->vm;
    uint32_t events = 0;
    uint32_t reason = v->fullscreen_error ? (uint32_t)v->fullscreen_error : (uint32_t)v->info.reason;
    if (!b->observed || b->state != public_state(v) || b->flags != status_flags(v) || b->reason != reason)
        events |= CIUKI_DOSVM_EVENT_STATE;
    if (v->clipboard.pending && !v->clipboard.done) events |= CIUKI_DOSVM_EVENT_CLIPBOARD;
    if (b->console != v->console_sequence) events |= CIUKI_DOSVM_EVENT_CONSOLE;
    /* fx-doswin3: every VGA register write (Mode Y selects the plane of
     * each column, about 1,300 OUTs per DOOM frame) and every harvest of
     * back-buffer stores advance vga.changes without changing the page on
     * screen. While the guest is page flipping, the displayed page changes
     * only with the CRTC start address (display_changes, video_starts) and
     * the other display registers and palette (display_changes), so only
     * those wake the presenter: each sample then shows a completed frame
     * (T23 trial 5bb73dba, DOOM window bank n=4: 76 samples and 87 flips
     * for 34 changed frames). A guest that stops flipping returns to the
     * store-driven wake after its flip hold (twice its last flip gap, one
     * refresh to DOSVM_FLIP_HOLD_MS; dosvm_vga.c flip_hold_ms). */
    if (!v->fullscreen && !v->fullscreen_hold && deadline_passed(v->present_due_us / 1000u) &&
        (!v->video_presented || (!v->vbe_mode && v->video_geometry.text) || deadline_passed(v->video_due_us / 1000u)) &&
        (b->video_pending || !b->observed || (dosvm_present_store_wake(v, g_ticks) && b->changes != v->vga.changes) ||
         b->display != v->vga.display_changes ||
         b->starts != v->video_starts || b->mode != v->vbe_mode || b->vbe_changes != v->present_vbe_changes ||
         (v->video_geometry.text && b->phase != wait_phase()))) events |= CIUKI_DOSVM_EVENT_VIDEO;
    return events;
}
static bool present_ready(void *arg)
{
    struct present_request *r = arg;
    if (present_interrupted(r)) return true;
    uint32_t events = present_events(r);
    /* t23-dosperf: stamp the first evaluation that finds the frame ready;
     * wait_present measures how long the presenter took to run after it. */
    if ((events & CIUKI_DOSVM_EVENT_VIDEO) && !r->vm->present_ready_tsc)
        r->vm->present_ready_tsc = dosvm_perf_clock();
    /* fx-doswin3: from this wake until the presenter runs, the scheduler
     * may run the woken presenter (base 9 above the VM's 7) ahead of the
     * focused VM's preference for at most PRESENT_SLICE ticks
     * (dosvm_task_present, task.c rq_pop) instead of behind the VM's eight
     * focused ticks (trial 5bb73dba: WAIT dispatch 7.7-12.8 ms average,
     * 100 ms maximum, for Wolfenstein 3-D and DOOM). */
    if ((events & CIUKI_DOSVM_EVENT_VIDEO) && !r->vm->present_dispatch) {
        r->vm->present_dispatch = true;
        r->vm->present_dispatch_tick = g_ticks;
    }
    return events != 0;
}
static int wait_present(struct binding *b, struct dosvm *v, struct process *p, struct ciuki_dosvm_command *c)
{
    struct present_request r = {b, v, p, proc_thread_for(g_current)};
    /* This is a producer dependency, not a physical device wait. Existing
     * sync machinery propagates boosts and withdraws them on every wake. */
    kwait_set_owner(&v->present_wait, v->task);
    unsigned timeout=c->wait.timeout_ms>33 ? 33 : c->wait.timeout_ms;
    v->present_ready_tsc = 0;
    v->present_waiter = g_current;
    kwait_wait_until(&v->present_wait, present_ready, &r, deadline_after_ms(timeout));
    kwait_set_owner(&v->present_wait, 0);
    v->present_dispatch = false; /* the presenter runs: the VM's preference returns */
    v->present_waiter = 0;
    if (present_interrupted(&r)) return -EINTR;
    c->wait.events = present_events(&r);
    uint32_t flags = irq_save();
    uint64_t ready = v->present_ready_tsc, now = dosvm_perf_clock();
    v->present_ready_tsc = 0;
    if ((c->wait.events & CIUKI_DOSVM_EVENT_VIDEO) && ready && now >= ready) {
        v->perf.present_waits++; v->perf.present_wait_cycles += now - ready;
        if (now - ready > v->perf.present_wait_max_cycles) v->perf.present_wait_max_cycles = now - ready;
    }
    irq_restore(flags);
    if (!c->wait.events) c->wait.events = CIUKI_DOSVM_EVENT_TIMEOUT;
    c->wait.frame_ms = dosvm_vga_period_ms(v);
    if (c->wait.events & CIUKI_DOSVM_EVENT_VIDEO) {
        b->video_pending = true;
        b->changes = v->vga.changes; b->display = v->vga.display_changes;
        b->starts = v->video_starts; b->mode = v->vbe_mode;
        b->vbe_changes = v->present_vbe_changes; b->phase = wait_phase();
        v->present_due_us = dosvm_vga_next_due(v->present_due_us, dosvm_vga_period_us(v, g_ticks), g_ticks);
    }
    b->state = public_state(v); b->flags = status_flags(v);
    b->reason = v->fullscreen_error ? (uint32_t)v->fullscreen_error : (uint32_t)v->info.reason;
    b->console = v->console_sequence; b->observed = true;
    return 0;
}
/* fx-4: the creating owner was ended (for example from Tasks) while a cold
 * base identity hash ran inside CREATE; stop hashing for it. */
static bool owner_stopping(void *arg)
{
    const struct process *p = arg;
    return p->state != PROC_LIVE;
}
/* Closed error set (execution-abi.md): internal storage/boot errors are EIO.
 * dosvm_create records the internal error first (fx-4 refusal record). */
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
/* fx-dosfull: Windows 98 runs a game "that uses graphics modes" full screen
 * (Resource Kit ch. 25, Running Games); the DOS Box enters full screen at the
 * first one. Graphics: an active VBE mode, a BIOS graphics mode in the BDA
 * (0049h other than the text modes 0-3 and 7), or a windowed VGA model a
 * program set to a graphics layout through the registers alone. Read in the
 * STATUS call only: the BDA read may fault in a page, which the WAIT_PRESENT
 * predicate (status_flags) must never do. */
static bool status_graphics(struct dosvm *v, uint32_t bios_mode)
{
    if (v->vbe_mode) return true;
    bios_mode &= 0x7Fu;
    if (bios_mode > 3 && bios_mode != 7) return true;
    cvga_geometry g;
    return !v->fullscreen && cvga_get_geometry(&v->vga, &g) && !g.text;
}

int32_t dosvm_client_syscall(struct trap_frame *tf)
{
    struct process *p = proc_current();
    if (!p) return -EPERM;
    unsigned op = tf->ebx;
    /* fx-4b: read-only state of the DOS system image verification, so a
     * launcher shows progress instead of blocking inside CREATE. */
    if (op==CIUKI_DOSVM_IMAGE_QUERY) {
        if (tf->edx!=sizeof(struct ciuki_dosvm_command)) return -EINVAL;
        struct ciuki_dosvm_command c,valid={0};
        int e=copy_from_user(&c,tf->ecx,sizeof(c)); if (e) return e;
        if (c.version!=CIUKI_DOSVM_VERSION) return -EOPNOTSUPP;
        valid.version=CIUKI_DOSVM_VERSION; valid.size=sizeof(valid);
        if (memcmp(&c,&valid,sizeof(c))) return -EINVAL;
        if (dosvm_image_query) dosvm_image_query(&c.state,&c.changed,&c.reason);
        return copy_to_user(tf->ecx,&c,sizeof(c));
    }
    if (op>=CIUKI_DOSVM_SAVE_QUERY && op<=CIUKI_DOSVM_SAVE_EXPORT) {
        /* Own-session: the caller's own store only, for every user class
         * (Guest included: its data lives in its own profile). */
        if (!dos_process_interactive(p)) return -EPERM;
        if (tf->edx!=sizeof(struct ciuki_dosvm_command)) return -EINVAL;
        struct ciuki_dosvm_command c,valid={0};
        int e=copy_from_user(&c,tf->ecx,sizeof(c)); if (e) return e;
        if (c.version!=CIUKI_DOSVM_VERSION) return -EOPNOTSUPP;
        valid.version=CIUKI_DOSVM_VERSION; valid.size=sizeof(valid); valid.launch.game=c.launch.game;
        if (!c.launch.game || c.launch.game>CIUKI_DOSVM_GAME_COUNT || memcmp(&c,&valid,sizeof(c))) return -EINVAL;
        struct ua_pin pin; e=ua_pin(p->memory,&pin,tf->ecx,sizeof(c),true); if (e) return e;
        struct dos_identity owner;
        e=dos_identity_capture(&owner,p);
        if (!e) { e=dosvm_game_data(c.launch.game,op,&c,&owner); dos_identity_release(&owner); }
        if (!e) e=copy_to_user(tf->ecx,&c,sizeof(c));
        ua_unpin(p->memory,&pin); return e;
    }
    /* Query is public catalogue state. Removal is a machine-wide change:
     * administrator only (no path-derived applet grant). Other users'
     * private title data is left to explicit file management. */
    if(op==CIUKI_DOSVM_GAME_QUERY || op==CIUKI_DOSVM_GAME_REMOVE) {
        if(op==CIUKI_DOSVM_GAME_REMOVE && !dos_process_admin(p))return -EPERM;
        if(tf->edx!=sizeof(struct ciuki_dosvm_command))return -EINVAL;
        struct ciuki_dosvm_command c,valid={0};
        int error=copy_from_user(&c,tf->ecx,sizeof(c));if(error)return error;
        if(c.version!=CIUKI_DOSVM_VERSION)return -EOPNOTSUPP;
        valid.version=CIUKI_DOSVM_VERSION;valid.size=sizeof(valid);valid.launch.game=c.launch.game;
        if(!c.launch.game || c.launch.game>CIUKI_DOSVM_GAME_COUNT || memcmp(&c,&valid,sizeof(c)))return -EINVAL;
        struct ua_pin pin;error=ua_pin(p->memory,&pin,tf->ecx,sizeof(c),true);if(error)return error;
        uint32_t size=0;
        error=dosvm_game_program(c.launch.game,op==CIUKI_DOSVM_GAME_REMOVE,&size);
        if(error==-FS_ENOENT && op==CIUKI_DOSVM_GAME_QUERY)error=0;
        else if(error>=0){c.state=error ? 2 : 1;error=0;}
        c.changed=size;
        if(!error)error=copy_to_user(tf->ecx,&c,sizeof(c));
        ua_unpin(p->memory,&pin);return error;
    }
    /* Own-session class: an authenticated interactive session. */
    if (!dos_process_interactive(p)) return -EPERM;
    if (op < CIUKI_DOSVM_CREATE || (op > CIUKI_DOSVM_WAIT_PRESENT &&
        op != CIUKI_DOSVM_TEXT && op != CIUKI_DOSVM_MARK &&
        op != CIUKI_DOSVM_TEXT_CHUNK && op != CIUKI_DOSVM_POLICY &&
        op != CIUKI_DOSVM_CLIP_REQUEST && op != CIUKI_DOSVM_CLIP_COMPLETE)) return -EINVAL;
    if (tf->edx != sizeof(struct ciuki_dosvm_command)) return -EINVAL;
    struct ciuki_dosvm_command c, allowed;
    bool direct = op == CIUKI_DOSVM_WAIT_PRESENT || op == CIUKI_DOSVM_PRESENT || op == CIUKI_DOSVM_PRESENT_BANDS;
    int e = direct ? ua_user_read(p->memory, &c, tf->ecx, sizeof(c)) : copy_from_user(&c, tf->ecx, sizeof(c));
    if (e) return e;
    if (c.version != CIUKI_DOSVM_VERSION) return -EOPNOTSUPP;
    if (c.size != sizeof(c)) return -EINVAL;
    /* Every field the operation does not take, outputs included, must be zero. */
    memset(&allowed, 0, sizeof(allowed));
    allowed.version = c.version; allowed.size = c.size;
    if (op == CIUKI_DOSVM_CREATE) {
        allowed.surface_fd = c.surface_fd;
        if (c.launch.kernel>CIUKI_DOSVM_UNIFIED || c.launch.game>CIUKI_DOSVM_GAME_COUNT ||
            c.launch.memory>CIUKI_DOSVM_MEMORY_LOCKED ||
            dosvm_launch_network(&c)>CIUKI_DOSVM_NETWORK_IPX || c.launch.reserved[5]>1) return -EINVAL;
        allowed.launch.kernel=c.launch.kernel; allowed.launch.game=c.launch.game;
        allowed.launch.memory=c.launch.memory;
        dosvm_launch_network_set(&allowed,dosvm_launch_network(&c));
        for (unsigned j=1;j<=6;j++) allowed.launch.reserved[j]=c.launch.reserved[j];
        struct joystick_route route={c.launch.reserved[1],c.launch.reserved[2],c.launch.reserved[3],c.launch.reserved[4],c.launch.reserved[5]};
        if (route.device==CIUKI_JOY_DISABLED) {
            if (route.generation || route.second_device || route.second_generation || route.mode || c.launch.reserved[6]) return -EINVAL;
        } else if (route.device) {
            if (route.device>CIUKI_JOYSTICK_LAST || !route.generation || !c.launch.reserved[6] ||
                route.second_device>CIUKI_JOYSTICK_LAST ||
                (route.second_device ? (!route.mode || !route.second_generation) : route.second_generation)) return -EINVAL;
            if (!joystick_session || !c.launch.reserved[6] || c.launch.reserved[6]!=joystick_session() || !joystick_route_validate) return -ENODEV;
            uint32_t jf=irq_save(); int je=joystick_route_validate(&route); irq_restore(jf);
            if (je) return je;
        } else if (route.generation || route.second_device || route.second_generation || route.mode || c.launch.reserved[6]) return -EINVAL;
        unsigned n = 0;
        while (n < sizeof(c.name) && c.name[n]) {
            if (c.name[n] < 32 || c.name[n] > 126) return -EINVAL;
            allowed.name[n] = c.name[n]; n++;
        }
        if (!n || n > CIUKI_DOSVM_NAME_MAX) return -EINVAL;
    } else { allowed.id = c.id; allowed.generation = c.generation; }
    if (op == CIUKI_DOSVM_FOCUS || op == CIUKI_DOSVM_FULLSCREEN || op == CIUKI_DOSVM_BACKGROUND || op == CIUKI_DOSVM_MARK) {
        if (c.focused > 1) return -EINVAL;
        allowed.focused = c.focused;
    }
    if (op==CIUKI_DOSVM_TEXT) {
        if (!c.text.buffer || !c.text.capacity || c.text.capacity>CIUKI_DOSVM_TEXT_MAX || c.text.buffer>UINT32_MAX-c.text.capacity) return -EINVAL;
        allowed.text.buffer=c.text.buffer; allowed.text.capacity=c.text.capacity;
    }
    if (op==CIUKI_DOSVM_CLIP_REQUEST || op==CIUKI_DOSVM_CLIP_COMPLETE) {
        if (c.clip.capacity>CIUKI_DOSVM_CLIP_MAX ||
            (c.clip.capacity && (!c.clip.buffer || c.clip.buffer>UINT32_MAX-c.clip.capacity))) return -EINVAL;
        allowed.clip.buffer=c.clip.buffer; allowed.clip.capacity=c.clip.capacity;
        if (op==CIUKI_DOSVM_CLIP_COMPLETE) {
            if (c.clip.bytes>CIUKI_DOSVM_CLIP_MAX || c.clip.result>0) return -EINVAL;
            allowed.clip.serial=c.clip.serial; allowed.clip.bytes=c.clip.bytes; allowed.clip.result=c.clip.result;
        }
    }
    if (op == CIUKI_DOSVM_INPUT) {
        if (!event_valid(&c.event)) return -EINVAL;
        allowed.event = c.event;
    }
    if (op == CIUKI_DOSVM_TEXT_CHUNK) {
        if (c.text_chunk.row>=100 || c.text_chunk.column>=160) return -EINVAL;
        allowed.text_chunk.row=c.text_chunk.row;
        allowed.text_chunk.column=c.text_chunk.column;
    }
    if (op == CIUKI_DOSVM_POLICY) {
        if (c.policy.marking>1 || c.policy.idle_sensitivity>100) return -EINVAL;
        allowed.policy.marking=c.policy.marking;
        allowed.policy.idle_sensitivity=c.policy.idle_sensitivity;
    }
    if (op == CIUKI_DOSVM_WAIT_PRESENT) {
        if (!c.wait.timeout_ms || c.wait.timeout_ms > CIUKI_DOSVM_WAIT_MAX_MS) return -EINVAL;
        allowed.wait.timeout_ms = c.wait.timeout_ms;
    }
    if (op == CIUKI_DOSVM_PRESENT_BANDS) {
        if (c.presentation.redraw > 1 || c.presentation.flags>CIUKI_DOSVM_PRESENT_SCANLINES) return -EINVAL;
        allowed.presentation.redraw = c.presentation.redraw;
        allowed.presentation.flags = c.presentation.flags;
    }
    if (memcmp(&allowed, &c, sizeof(c))) return -EINVAL;
    /* The record stays writable across a sleeping create/close. */
    struct ua_pin pin;
    e = direct ? 0 : ua_pin(p->memory, &pin, tf->ecx, sizeof(c), true);
    if (e) return e;
    struct binding *b = 0;
    struct dosvm *v = 0;
    if (op == CIUKI_DOSVM_CREATE) {
        /* Game launch diagnostics must survive quiet boot and failed create;
         * the ELF/first-frame latency records are verbose-mode only. */
        klog("[launch-stage] name=dosbox pid=%u stage=vm-create window=%s", p->pid, c.name);
        struct desktop_description *d = desktop_fd(p, c.surface_fd);
        if (!d || d->kind != DESKTOP_SURFACE) { e = -EBADF; goto out; }
        if (!(d->maximum & PROT_WRITE)) { e = -EACCES; goto out; }
        const struct ciuki_surface_info *g = surface_geometry(d->u.surface);
        if (g->width != CIUKI_DOSVM_WIDTH || g->height != CIUKI_DOSVM_HEIGHT ||
            g->format != CIUKI_SURFACE_XRGB8888 || g->stride != CIUKI_DOSVM_WIDTH * 4) { e = -EINVAL; goto out; }
        if (owns_vm(p->pid)) { e = -EBUSY; goto out; }
        /* f3-21: Alt+Enter only queues a request; the display worker polls
         * it. Without a prior mode switch nothing else would (QEMU case
         * vm-fullscreen-qemu-t23 was refused only at cleanup, -EPERM). */
        if (display_worker_start && (e = display_worker_start())) goto out;
        /* The credential is fixed before guest boot; no later input changes it. */
        struct dos_identity identity;
        if ((e = dos_identity_capture(&identity, p))) goto out;
        unsigned reservation = 0;
        while (reservation < DOSVM_LIMIT && creating[reservation]) reservation++;
        if (reservation == DOSVM_LIMIT) { dos_identity_release(&identity); e = -ENOSPC; goto out; }
        creating[reservation] = p->pid;
        /* ABI 0/1 are compatibility aliases. Every desktop window, including
         * the interactive shell, uses the qualified unified base. */
        const char *workload="C:\\SYSTEM\\DOS\\UNIFIED.IMG";
        c.launch.kernel=CIUKI_DOSVM_UNIFIED;
        /* Creation may sleep opening storage; another thread may close the
         * descriptor meanwhile, so the VM holds its own surface reference. */
        struct surface *surface = d->u.surface;
        surface_retain(surface);
        const struct dosvm_config config = {.probe = window_probe(), .base = workload, .dpmi = true,
            .game=(uint8_t)c.launch.game,.kernel=(uint8_t)c.launch.kernel,
            .memory_locked=c.launch.memory==CIUKI_DOSVM_MEMORY_LOCKED,
            .disable_file_bridge=true,.cancelled=owner_stopping,.cancel_arg=p,.identity=&identity};
        e = dosvm_create(&config, &v);
        dos_identity_release(&identity); /* the VM holds its own reference */
        creating[reservation] = 0;
        if (e) { surface_release(surface); e = e==-ENOENT && c.launch.game ? e : closed_error(e); goto out; }
        c.changed|=v->ports_changed;
        if (c.launch.reserved[1]) {
            v->joystick.route=(struct joystick_route){0};
            if (c.launch.reserved[1]!=CIUKI_JOY_DISABLED)
                v->joystick.route=(struct joystick_route){c.launch.reserved[1],c.launch.reserved[2],c.launch.reserved[3],c.launch.reserved[4],c.launch.reserved[5]};
        } else if (joystick_settings_game) {
            uint32_t jf=irq_save();
            bool fallback=joystick_settings_game(c.launch.game,&v->joystick.route);
            irq_restore(jf);
            if (fallback) {
                c.changed|=CIUKI_DOSVM_JOYSTICK_FALLBACK;
                klog("[joystick-settings] selected controller absent game=%u; first available fallback",c.launch.game);
            }
        }
        struct dosvm_key key = dosvm_key(v);
        b = &bindings[key.id - 1];
        *b = (struct binding){.key=key, .pid=p->pid, .surface=surface, .busy=true};
        v->owner_pid = p->pid;
        memcpy(v->name, c.name, sizeof(v->name));
        /* fx-10: MSCDEX drive and virtual disc (never fatal: no disc is a record). */
        if (p->state == PROC_LIVE && dosvm_cdaudio_open) (void)dosvm_cdaudio_open(v);
        if (p->state != PROC_LIVE) e = -EINTR;
        else if (!(e = dosvm_network_launch(v,dosvm_launch_network(&c)))) e = dosvm_boot(v);
        /* Focus belongs to the native endpoint. Its +2 promotion is never
         * copied into the VM's permanent base; WAIT carries transient boosts. */
        if (!e && v->task && g_current) {
            b->vm_base=v->task->base_priority;
            b->window_base=b->vm_base;
            binding_priority(b,v);
        }
        if (e) {
            release_binding(b, true);
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
        /* Same pid alone is not ownership: the creating credential and
         * session must be unchanged and live (no reused-pid control). A VM
         * already DEAD has released its identity and runs nothing; its
         * owner may still collect the binding as before. */
        if (v->info.state != DOSVM_DEAD && !dos_identity_owner(&v->identity, p)) { e = -ESRCH; goto out; }
        binding_priority(b,v);
        if (op == CIUKI_DOSVM_CLOSE) {
            e = closed_error(release_binding(b, false));
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
            c.changed = status_flags(v);
            if (v->fullscreen_error && c.state!=CIUKI_DOSVM_STOPPED) c.reason=(uint32_t)-v->fullscreen_error;
            if (v->launch_failed && c.state != CIUKI_DOSVM_STOPPED) c.reason = v->launch_error;
            if (v->launch_failed || c.state == CIUKI_DOSVM_STOPPED)
                memcpy(c.name, v->console_line, sizeof(c.name) - 1);
            video_mode(v, &c);
            if (c.state != CIUKI_DOSVM_STOPPED && status_graphics(v, c.video_mode)) c.changed |= CIUKI_DOSVM_GRAPHICS;
            dosvm_startup_report(v, false);
            dosvm_perf_report(v);
        } else if (op==CIUKI_DOSVM_CLIP_REQUEST || op==CIUKI_DOSVM_CLIP_COMPLETE)
            e=dosvm_clipboard_client(v,op,&c);
        else if (op == CIUKI_DOSVM_WAIT_PRESENT) e = wait_present(b, v, p, &c);
        else if (!dosvm_valid(key)) e = -ESRCH;
        else if (op == CIUKI_DOSVM_FULLSCREEN) {
            if (c.focused && !b->focused) e=-EACCES;
            else {
                if (c.focused) v->fullscreen_suspended=false;
                e=c.focused ? dosvm_fullscreen_enter(key) : dosvm_fullscreen_leave(key,false);
                if (e==DISPLAY_FULLSCREEN_WINDOWED) e=0;
            }
        } else if (op == CIUKI_DOSVM_BACKGROUND) {
            v->fullscreen_pause=c.focused!=0;
            if (!v->fullscreen) {
                v->fullscreen_suspended=v->fullscreen_pause && !b->focused;
                dosvm_idle_wake(v);
            }
        } else if (op == CIUKI_DOSVM_POLICY) {
            v->window_marking=c.policy.marking!=0;
            v->window_idle=(uint8_t)(c.policy.idle_sensitivity+1);
            if (v->window_marking) dosvm_kbd_release(v);
            dosvm_idle_wake(v);
        } else if (op == CIUKI_DOSVM_TEXT_CHUNK) {
            dosvm_vga_sync(v,true);
            unsigned rows=0;
            c.text_chunk.columns=v->vbe_mode ? 0 : cvga_text_chunk(&v->vga,c.text_chunk.row,
                c.text_chunk.column,c.text_chunk.characters,CIUKI_DOSVM_TEXT_BYTES,&rows);
            c.text_chunk.rows=rows;
            if (!c.text_chunk.columns) e=-EOPNOTSUPP;
        }
        else if (op==CIUKI_DOSVM_MARK) {
            if (c.focused && (!b->focused || v->fullscreen)) e=-EACCES;
            else { v->clipboard_mark=c.focused!=0; dosvm_idle_wake(v); }
        } else if (op==CIUKI_DOSVM_TEXT) {
            /* BIOS data-area layout: V7 VGA Technical Reference, table 4-4.
             * Validate guest-controlled geometry before accessing the text aperture. */
            uint8_t mode=0,rows=0; uint16_t columns=0,page=0,height=0;
            e=dosvm_read(v,0x449,&mode,1);
            if (!e) e=dosvm_read(v,0x44a,&columns,2);
            if (!e) e=dosvm_read(v,0x44e,&page,2);
            if (!e) e=dosvm_read(v,0x484,&rows,1);
            if (!e) e=dosvm_read(v,0x485,&height,2);
            unsigned count=(unsigned)columns*(rows+1u);
            if (!e && (v->vbe_mode || (mode>3 && mode!=7))) e=-EOPNOTSUPP;
            if (!e && (!columns || columns>160 || rows>=100 || !height || height>32 || (page&1) || page>32768 || count>16384-page/2)) e=-EINVAL;
            if (!e && count>c.text.capacity) e=-ENOSPC;
            if (!e) {
                /* copy_to_user validates each bounded row. The command is
                 * pinned separately; no nested pin is needed for this snapshot. */
                uint8_t raw[320],line[160];
                for (unsigned y=0;y<=rows && !e;y++) {
                    e=dosvm_read(v,(mode==7 ? 0xb0000 : 0xb8000)+page+y*columns*2,raw,columns*2);
                    if (!e) { for (unsigned x=0;x<columns;x++) line[x]=raw[2*x]; e=copy_to_user(c.text.buffer+y*columns,line,columns); }
                }
                if (!e) { c.text.columns=columns; c.text.rows=rows+1; c.text.cell_height=height; c.changed=count; }
            }
        }
        else if (op == CIUKI_DOSVM_LAUNCH) {
            if (v->launch_armed) e = -EBUSY;
            else if (!v->info.dos_ready) e = -EAGAIN;
            else {
                v->launch_armed = true;
                v->video_presented = false;
                dosvm_startup_report(v, true);
            }
        } else if (op == CIUKI_DOSVM_PRESENT || op == CIUKI_DOSVM_PRESENT_BANDS) {
            bool scanlines=op==CIUKI_DOSVM_PRESENT_BANDS && c.presentation.flags==CIUKI_DOSVM_PRESENT_SCANLINES;
            uint32_t samples = v->present_samples;
            if (op == CIUKI_DOSVM_PRESENT_BANDS && c.presentation.redraw) v->video_presented = false;
            struct row_target target={b->surface,!v->video_presented,v};
            e = dosvm_vga_present(v, put_row, &target, g_ticks, &c.damage);
            if (e >= 0) {
                /* A cadence-deferred call must not acknowledge the pending
                 * frame. The last store may otherwise never reach the window. */
                if (v->present_samples != samples) {
                    b->video_pending = false;
                    /* WAIT and PRESENT share the sampled frame's phase,
                     * including when rendering was delayed by other work. */
                    v->present_due_us = v->video_due_us;
                    v->present_sample_due_us = v->video_due_us;
                }
                c.changed = (uint32_t)e; e = 0; video_mode(v, &c);
                if (op == CIUKI_DOSVM_PRESENT_BANDS) {
                    if (scanlines) {
                        memset(&c.scanlines,0,sizeof(c.scanlines));
                        memcpy(c.scanlines.bits,v->video_rows,sizeof(v->video_rows));
                    } else {
                        c.presentation.bands[0] = (uint32_t)v->video_bands;
                        c.presentation.bands[1] = (uint32_t)(v->video_bands >> 32);
                    }
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
    if (!e) e = direct ? ua_user_write(p->memory, tf->ecx, &c, sizeof(c)) : copy_to_user(tf->ecx, &c, sizeof(c));
out:
    if (op == CIUKI_DOSVM_CREATE && e)
        klog("[launch] name=dosbox pid=%u error=%d stage=vm-create window=%s", p->pid, e, c.name);
    if (b) {
        b->busy = false;
        /* The owner began stopping during this call: collect now, since the
         * supervisor pass skipped the busy binding. */
        if (p->state != PROC_LIVE && b->pid) release_binding(b, true);
    }
    if (!direct) ua_unpin(p->memory, &pin);
    return e;
}
