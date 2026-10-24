/* SPDX-License-Identifier: MIT
 * Host replay of the desktop app cases of tests/suites/f2-desktop.json
 * (files-qemu-t23, tasks-qemu-t23, ciuknote-qemu-t23).
 *
 * The production desktop (gate mode, crash-isolation probe interaction stage),
 * the survivor demo and every program the desktop launches (Files, CiukNote,
 * Task Manager, demo) run unchanged as threads of this process. Exactly one
 * runs at a time: each is a process of a small virtual kernel whose scheduler
 * hands a baton over at every nanosleep, so a run is deterministic and its
 * clock is virtual. The kernel side is faithful where the cases depend on it:
 * channels (64-message queues, kernel-stamped sender PIDs, descriptor
 * passing, EOF/EPIPE on close), surfaces shared by mapping, spawn with
 * explicit descriptor mappings and process groups, waitpid/kill, call 70/71,
 * the probe's GATE_INTERACT/ARM sequence and report checks.
 *
 * Input follows the same path as in QEMU: each runner batch goes through a
 * model of QEMU's PS/2 devices (16-byte queues per batch: at most five 3-byte
 * mouse packets, deltas clamped to +-127 per packet, button state sampled at
 * the batch's sync; set-2 key bytes, E0 prefixes, F0 breaks) and of the
 * kernel's i8042 decoder (per-axis REL events, inverted Y, button edges after
 * the packet's motion, unshifted TEXT after key downs), then reaches the
 * desktop through ciuki_input_read. QEMU sources: hw/input/ps2.c
 * (ps2_queue_noirq, ps2_mouse_send_packet, ps2_mouse_sync) and ui/input.c
 * (qmp_input_send_event syncs once per command), checked 2026-10-11.
 *
 * --i8042-byte-ms N models a guest that reads one controller byte every N ms
 * instead of at once: the firmware input path of the qemu-e500 profile, whose
 * BIOS VM worker reflects one IRQ1/IRQ12 byte per VM entry
 * (src/kernel/vm/biosvm.c worker_main, src/kernel/vm/fwinput.c). The bytes
 * then wait in QEMU's PS/2 queues, and each time the guest reads the output
 * buffer the controller refills it from a pending keyboard byte before a
 * pending aux byte (hw/input/pckbd.c kbd_update_irq: KBD_PENDING_KBD is tested
 * before KBD_PENDING_AUX), so a key sent after a click can reach the desktop
 * before the click's packet. Keys are translated set-1 bytes there (make 1,
 * break 1, E0 prefix 1); a packet's events follow its third byte; a sync
 * sends packets only while the 16-byte aux queue has room for one.
 * --i8042-stall-batch B --i8042-stall-ms S: the reader reads nothing for S ms
 * once batch B is delivered (a firmware worker that does not run meanwhile);
 * queued bytes keep their order, except that keyboard bytes go first.
 *
 * --usb-poll-ms N selects USB-sourced grant events, with independent keyboard
 * and mouse report queues polled every N ms. The mouse model follows QEMU's
 * hw/input/hid.c hid_pointer_sync/poll: adjacent unconsumed moves with equal
 * button state combine, and each poll drains at most +/-127 on each axis.
 * This matters for corner clamps: combining the move onto Start with the
 * clamp loses the intermediate pointer position (f2-58). Keyboard set-1
 * positions are the output of usb_hid.c's translation, not PS/2 bytes.
 * Sources checked 2026-10-14: https://github.com/qemu/qemu/blob/v11.1.1/hw/input/hid.c
 * and https://www.usb.org/sites/default/files/hid1_11.pdf (Appendix B).
 * This model does not exercise the USB controller, HID parser or typematic.
 *
 * Not covered: the kernel's real scheduler and timings (turn costs and start
 * delays are parameters), pixel digests (the lead pins them from QEMU), the
 * FAT driver (the Files and CiukNote file systems are the fake image tree of
 * fake_vfs.c), and the write gate (always open here).
 *
 *   app_replay --script F --case files|tasks|ciuknote --bin a,b,c [options]
 */
#include "replay_base.h"
#include <assert.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include "desktop.h"
#include <ciuki/input.h>
#include <ciuki/shell_probe.h>
#include "gate.h"
#include "fake_vfs.h"
#include "../../../apps/ciuknote/doc.h"
/* f4-18: desktop.c saves the master volume preference through libreg (sound_storage.c);
 * the replay exercises the application contracts, so the harness supplies the hook. */
int master_preference_save(const char *directory, unsigned left, unsigned right, unsigned mute)
{ (void)directory; (void)left; (void)right; (void)mute; return 0; }


int desktop_main(int, char **);
int files_main(int, char **);
int ciuknote_main(int, char **);
int tasks_main(int, char **);
int demo_main(int, char **);
int survivor_demo_main(int, char **);
extern struct desktop *replay_desktop;
void replay_gate_counts(unsigned out[3]);

#define MS 1000000ull
#define MAX_PROCS 48
#define MAX_FDS CIUKI_OPEN_MAX
#define WIDTH 1024u
#define HEIGHT 768u

/* ---- options ----------------------------------------------------------- */
static const char *case_name, *portrait_path;
static char bin_list[512];
static uint64_t overhead_ns, first_ns = 150 * MS, start_delay_ns, desktop_cost_ns = 20 * MS, app_cost_ns = 5 * MS;
static unsigned pause_scale_pct = 100;
static uint64_t usb_poll_ns, usb_due;
static uint64_t byte_ns;   /* --i8042-byte-ms: 0 = every byte read at once (the native IRQ handler) */
static uint64_t stall_ns;  /* --i8042-stall-ms: the reader reads nothing for this long after ... */
static int stall_batch = -1;   /* ... --i8042-stall-batch: the batch with this index is delivered */
static uint64_t limit_ns = 240000 * MS;
static int trace;
static const char *shots;   /* --shots DIR: the desktop frame before every batch with a button press (diagnosis) */

/* ---- virtual kernel objects ---------------------------------------------- */
enum kind { K_CHANNEL = 1, K_SURFACE, K_DISPLAY, K_INPUT, K_AUTHORITY, K_CONSOLE };
struct chan;
struct surface { struct ciuki_surface_info info; uint32_t *pixels; };
struct obj { int kind, refs; struct chan *chan; int side; struct surface *surface; };
struct qmsg { struct ciuki_message m; struct obj *attached[CIUKI_MESSAGE_FD_MAX]; };
struct chan { struct qmsg q[2][CIUKI_CHANNEL_QUEUE_MAX]; unsigned head[2], count[2]; int open[2]; };
struct fdent { struct obj *o; int cloexec, flags; };

struct proc {
    int used, pid, ppid, pgid, state;   /* state: CIUKI_PROC_LIVE, CIUKI_PROC_ZOMBIE, 0 reaped */
    int status, killed, go;
    char name[32];
    pthread_t thread; pthread_cond_t cv;
    uint64_t wake, cost, start;
    int (*entry)(int, char **); int argc; char **argv; char **env;
    struct fdent fd[MAX_FDS];
};
static struct proc procs[MAX_PROCS];
static struct proc *cur;
static __thread int baton_thread;   /* set on threads that run under the replay's baton */
static pthread_mutex_t big = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sched_cv = PTHREAD_COND_INITIALIZER;
static uint64_t vnow = 1000 * MS;
static int next_pid = 3;
static int desktop_pid, survivor_pid;
static unsigned presents;
/* f2-65: presented pixels once the case has settled (1 s after both the
 * probe's success and the last batch). The QEMU failure of desktop-shell-qemu-t23 on image eb774bb9 had a
 * desktop that presented a complete frame on every loop at interactive
 * priority and so starved its own clients; an idle desktop presents nothing. */
static uint64_t present_px, idle_px, idle_from = UINT64_MAX;

/* Files now performs reads on a persistent worker. This replay's VFS has
 * zero virtual I/O cost, so run a worker turn to its next mailbox wait before
 * advancing the virtual clock. Slow I/O/preemption has a separate real-time
 * test (files_async_test.c); do not depend on host pthread scheduling here. */
struct files_worker_replay {
    int used, go, runnable, done;
    struct proc *owner;
    pthread_t thread;
    pthread_cond_t cv, *mailbox;
    void *(*entry)(void *);
    void *arg;
};
static struct files_worker_replay files_workers[MAX_PROCS], *running_worker;

static void fail(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    printf("FAIL "); vprintf(fmt, ap); printf("\n");
    va_end(ap); fflush(stdout); _exit(1);
}
static void note(const char *fmt, ...)
{
    if (!trace) return;
    va_list ap; va_start(ap, fmt);
    printf("[%8.3f] ", (double)vnow / 1e9); vprintf(fmt, ap); printf("\n");
    va_end(ap);
}

static struct obj *obj_new(int kind)
{
    struct obj *o = calloc(1, sizeof(*o)); assert(o);
    o->kind = kind; o->refs = 1; return o;
}
static void obj_put(struct obj *o)
{
    if (!o || --o->refs) return;
    if (o->kind == K_CHANNEL) {
        struct chan *c = o->chan;
        c->open[o->side] = 0;
        /* A closed reader drops its queue (and the descriptors in flight). */
        while (c->count[o->side]) {
            struct qmsg *q = &c->q[o->side][c->head[o->side]];
            for (unsigned k = 0; k < q->m.fd_count; k++) obj_put(q->attached[k]);
            c->head[o->side] = (c->head[o->side] + 1) % CIUKI_CHANNEL_QUEUE_MAX; c->count[o->side]--;
        }
        if (!c->open[0] && !c->open[1]) free(c);
    }
    /* Surfaces stay allocated: mappings outlive descriptors, as in the kernel. */
    free(o);
}
static struct obj *fd_obj(struct proc *p, int fd)
{
    if (fd < 0 || fd >= MAX_FDS || !p->fd[fd].o) return NULL;
    return p->fd[fd].o;
}
static int fd_install(struct proc *p, struct obj *o, int cloexec)
{
    for (int fd = 0; fd < MAX_FDS; fd++) if (!p->fd[fd].o) { p->fd[fd] = (struct fdent){o, cloexec, 0}; return fd; }
    errno = EMFILE; return -1;
}

/* ---- scheduler ------------------------------------------------------------ */
static _Noreturn void proc_exit(struct proc *p, int status)
{
    for (int fd = 0; fd < MAX_FDS; fd++) if (p->fd[fd].o) { obj_put(p->fd[fd].o); p->fd[fd].o = NULL; }
    p->state = CIUKI_PROC_ZOMBIE; p->status = status;
    note("exit pid=%d %s status=%#x", p->pid, p->name, status);
    cur = NULL;
    pthread_cond_signal(&sched_cv);
    pthread_mutex_unlock(&big);
    pthread_exit(NULL);
}
static void yield_until(uint64_t wake)
{
    struct proc *p = cur;
    p->wake = wake;
    cur = NULL;
    pthread_cond_signal(&sched_cv);
    while (!p->go) pthread_cond_wait(&p->cv, &big);
    p->go = 0;
    if (p->killed) proc_exit(p, p->killed);
}
static void *proc_thread(void *arg)
{
    struct proc *p = arg;
    baton_thread = 1;
    pthread_mutex_lock(&big);
    while (!p->go) pthread_cond_wait(&p->cv, &big);
    p->go = 0;
    if (p->killed) proc_exit(p, p->killed);
    int r = p->entry(p->argc, p->argv);
    proc_exit(p, (r & 255) << 8);
}
static char **copy_strings(char *const *v, int *count)
{
    int n = 0; while (v && v[n]) n++;
    char **out = calloc((size_t)n + 1, sizeof(*out)); assert(out);
    for (int i = 0; i < n; i++) { out[i] = strdup(v[i]); assert(out[i]); }
    if (count) *count = n;
    return out;
}
static int stub_main(int argc, char **argv) { (void)argc; (void)argv; return 0; }
static struct proc *proc_create(const char *name, int (*entry)(int, char **), char *const argv[], char *const env[],
                                int ppid, int pgid, uint64_t cost, uint64_t delay)
{
    struct proc *p = NULL;
    for (int i = 0; i < MAX_PROCS && !p; i++) if (!procs[i].used) p = &procs[i];
    if (!p) { errno = EAGAIN; return NULL; }
    memset(p, 0, sizeof(*p));
    p->used = 1; p->pid = next_pid++; p->ppid = ppid; p->pgid = pgid ? pgid : p->pid; p->state = CIUKI_PROC_LIVE;
    snprintf(p->name, sizeof(p->name), "%s", name);
    p->entry = entry; p->argv = copy_strings(argv, &p->argc); p->env = copy_strings(env, NULL);
    p->cost = cost; p->wake = vnow + delay; p->start = vnow;
    for (int fd = 0; fd < 3; fd++) p->fd[fd].o = obj_new(K_CONSOLE);
    pthread_cond_init(&p->cv, NULL);
    if (pthread_create(&p->thread, NULL, proc_thread, p)) fail("pthread_create");
    pthread_detach(p->thread);
    note("spawn pid=%d %s", p->pid, name);
    return p;
}
static struct proc *proc_find(int pid)
{
    for (int i = 0; i < MAX_PROCS; i++) if (procs[i].used && procs[i].pid == pid) return &procs[i];
    return NULL;
}
static void run(struct proc *p)
{
    cur = p; p->go = 1;
    pthread_cond_signal(&p->cv);
    while (cur) pthread_cond_wait(&sched_cv, &big);
}

static void *files_worker_thread(void *arg)
{
    struct files_worker_replay *w=arg;
    baton_thread = 1;
    pthread_mutex_lock(&big);
    while (!w->go) pthread_cond_wait(&w->cv,&big);
    w->go=0; running_worker=w;
    void *result=w->entry(w->arg);
    w->done=1; running_worker=NULL; cur=NULL;
    pthread_cond_signal(&sched_cv);pthread_mutex_unlock(&big);
    return result;
}
int rp_files_worker_create(pthread_t *thread,const pthread_attr_t *attr,void *(*entry)(void *),void *arg)
{
    assert(cur && !running_worker);
    for (unsigned i=0;i<MAX_PROCS;i++) if (!files_workers[i].used) {
        struct files_worker_replay *w=&files_workers[i];
        memset(w,0,sizeof(*w));w->used=w->runnable=1;w->owner=cur;w->entry=entry;w->arg=arg;
        int e=pthread_cond_init(&w->cv,NULL);if(e){w->used=0;return e;}
        e=pthread_create(&w->thread,attr,files_worker_thread,w);
        if(e){pthread_cond_destroy(&w->cv);w->used=0;return e;}
        *thread=w->thread;return 0;
    }
    return EAGAIN;
}
int rp_files_worker_signal(pthread_cond_t *condition)
{
    for (unsigned i=0;i<MAX_PROCS;i++) {
        struct files_worker_replay *w=&files_workers[i];
        if(w->used && !w->done && w->mailbox==condition)w->runnable=1;
    }
    return 0;
}
int rp_files_worker_wait(pthread_cond_t *condition,pthread_mutex_t *mutex)
{
    struct files_worker_replay *w=running_worker;assert(w && cur==w->owner);
    w->mailbox=condition;w->runnable=0;
    assert(!pthread_mutex_unlock(mutex));
    cur=NULL;running_worker=NULL;pthread_cond_signal(&sched_cv);
    while(!w->go)pthread_cond_wait(&w->cv,&big);
    w->go=0;running_worker=w;assert(cur==w->owner);
    return pthread_mutex_lock(mutex);
}
int rp_files_worker_join(pthread_t thread,void **result)
{
    for(unsigned i=0;i<MAX_PROCS;i++) {
        struct files_worker_replay *w=&files_workers[i];
        if(!w->used || !pthread_equal(w->thread,thread))continue;
        while(!w->done)yield_until(vnow+MS);
        int e=pthread_join(thread,result);
        if(!e){pthread_cond_destroy(&w->cv);w->used=0;}
        return e;
    }
    return ESRCH;
}
static int run_files_worker(void)
{
    for(unsigned i=0;i<MAX_PROCS;i++) {
        struct files_worker_replay *w=&files_workers[i];
        if(!w->used || !w->runnable || w->done)continue;
        assert(!cur && !running_worker);cur=w->owner;w->go=1;
        pthread_cond_signal(&w->cv);
        while(cur)pthread_cond_wait(&sched_cv,&big);
        return 1;
    }
    return 0;
}

/* ---- ciuki_* system calls -------------------------------------------------- */
int ciuki_channel_pair(int pair[2])
{
    struct chan *c = calloc(1, sizeof(*c)); assert(c);
    c->open[0] = c->open[1] = 1;
    struct obj *a = obj_new(K_CHANNEL), *b = obj_new(K_CHANNEL);
    a->chan = b->chan = c; a->side = 0; b->side = 1;
    pair[0] = fd_install(cur, a, 1); pair[1] = fd_install(cur, b, 1);
    if (pair[0] < 0 || pair[1] < 0) fail("channel_pair: descriptor table full");
    return 0;
}
static int channel_send_locked(int fd, const struct ciuki_message *m, uint32_t flags);
/* A real host thread outside the baton (the desktop's media worker publishes
 * its completion token through desk_wake, holding its own mailbox lock) must
 * never take the baton lock: the desktop takes that mailbox lock while it
 * holds the baton. The send is recorded and delivered at the next virtual
 * poll of any process (rp_poll). The thread's process is unknown, but a
 * completion pair is the one channel whose two ends are both in one process's
 * table, so the token goes to every process holding a loopback channel at
 * that descriptor number; a spurious wake is harmless (tokens only wake and
 * coalesce like a full queue). */
static _Atomic uint64_t foreign_wake_mask;
int ciuki_channel_send(int fd, const struct ciuki_message *m, uint32_t flags)
{
    if (baton_thread) return channel_send_locked(fd, m, flags);
    if (fd < 0 || fd >= 64) { errno = EBADF; return -1; }
    atomic_fetch_or(&foreign_wake_mask, 1ull << fd);
    return 0;
}
static int loopback_channel(struct proc *p, int fd)
{
    struct obj *o = fd_obj(p, fd);
    if (!o || o->kind != K_CHANNEL) return 0;
    for (int k = 0; k < MAX_FDS; k++) {
        struct obj *q = p->fd[k].o;
        if (q && q->kind == K_CHANNEL && q->chan == o->chan && q->side != o->side) return 1;
    }
    return 0;
}
static void deliver_foreign_wakes(void)
{
    if (!atomic_load(&foreign_wake_mask) || !cur) return;
    uint64_t mask = atomic_exchange(&foreign_wake_mask, 0);
    struct ciuki_message token = {0}; token.length = 1;
    struct proc *saved = cur;
    for (int fd = 0; fd < 64; fd++) if (mask & (1ull << fd))
        for (int i = 0; i < MAX_PROCS; i++) if (procs[i].used && procs[i].state == CIUKI_PROC_LIVE && loopback_channel(&procs[i], fd)) {
            cur = &procs[i]; (void)channel_send_locked(fd, &token, DONTWAIT);
        }
    cur = saved;
}
static int channel_send_locked(int fd, const struct ciuki_message *m, uint32_t flags)
{
    struct obj *o = fd_obj(cur, fd);
    if (!o || o->kind != K_CHANNEL) { errno = EBADF; return -1; }
    if (m->length > CIUKI_MESSAGE_PAYLOAD_MAX || m->fd_count > CIUKI_MESSAGE_FD_MAX) { errno = EINVAL; return -1; }
    struct obj *attached[CIUKI_MESSAGE_FD_MAX] = {0};
    for (unsigned k = 0; k < m->fd_count; k++) {
        attached[k] = fd_obj(cur, m->fds[k]);
        if (!attached[k] || (attached[k]->kind != K_CHANNEL && attached[k]->kind != K_SURFACE)) { errno = EBADF; return -1; }
    }
    for (;;) {
        o = fd_obj(cur, fd);
        if (!o) { errno = EBADF; return -1; }
        struct chan *c = o->chan; int to = 1 - o->side;
        if (!c->open[to]) { errno = EPIPE; return -1; }
        if (c->count[to] < CIUKI_CHANNEL_QUEUE_MAX) {
            struct qmsg *q = &c->q[to][(c->head[to] + c->count[to]++) % CIUKI_CHANNEL_QUEUE_MAX];
            q->m = *m; q->m.sender_pid = (uint32_t)cur->pid; q->m.reserved = 0;
            if (trace > 1 && m->length >= 2 && m->data[1] != DESK_DAMAGE && m->data[1] != DESK_MOTION)
                note("msg pid=%d op=%u len=%u fds=%u", cur->pid, m->data[1], m->length, m->fd_count);
            for (unsigned k = 0; k < m->fd_count; k++) { q->attached[k] = attached[k]; attached[k]->refs++; q->m.fds[k] = -1; }
            return 0;
        }
        if (flags & DONTWAIT) { errno = EAGAIN; return -1; }
        yield_until(vnow + 1 * MS);
    }
}
int ciuki_channel_recv(int fd, struct ciuki_message *m, uint32_t flags)
{
    for (;;) {
        struct obj *o = fd_obj(cur, fd);
        if (!o || o->kind != K_CHANNEL) { errno = EBADF; return -1; }
        struct chan *c = o->chan; int side = o->side;
        if (c->count[side]) {
            struct qmsg *q = &c->q[side][c->head[side]];
            *m = q->m;
            for (unsigned k = 0; k < q->m.fd_count; k++) {
                m->fds[k] = fd_install(cur, q->attached[k], 1);
                if (m->fds[k] < 0) fail("recv: descriptor table full");
            }
            c->head[side] = (c->head[side] + 1) % CIUKI_CHANNEL_QUEUE_MAX; c->count[side]--;
            return 1;
        }
        if (!c->open[1 - side]) return 0;
        if (flags & DONTWAIT) { errno = EAGAIN; return -1; }
        yield_until(vnow + 1 * MS);
    }
}
int ciuki_surface_create(uint32_t w, uint32_t h, uint32_t format)
{
    if (!w || !h || w > 2048 || h > 2048 || format != CIUKI_SURFACE_XRGB8888) { errno = EINVAL; return -1; }
    struct surface *s = calloc(1, sizeof(*s)); assert(s);
    s->info = (struct ciuki_surface_info){sizeof(s->info), w, h, w * 4, format, (w * h * 4 + 4095) & ~4095u};
    s->pixels = calloc(1, s->info.allocation_bytes); assert(s->pixels);
    struct obj *o = obj_new(K_SURFACE); o->surface = s;
    return fd_install(cur, o, 0);
}
void *ciuki_surface_map(int fd, int prot)
{
    (void)prot;
    struct obj *o = fd_obj(cur, fd);
    if (!o || o->kind != K_SURFACE) { errno = EBADF; return MAP_FAILED; }
    return o->surface->pixels;
}
int ciuki_surface_info(int fd, struct ciuki_surface_info *s)
{
    struct obj *o = fd_obj(cur, fd);
    if (!o || o->kind != K_SURFACE) { errno = EBADF; return -1; }
    *s = o->surface->info; return 0;
}
int ciuki_display_info(int fd, struct ciuki_framebuffer_info *s)
{
    struct obj *o = fd_obj(cur, fd);
    if (!o || o->kind != K_DISPLAY) { errno = EBADF; return -1; }
    memset(s, 0, sizeof(*s));
    s->size = sizeof(*s); s->width = WIDTH; s->height = HEIGHT; s->pitch = WIDTH * 4; s->bpp = 32;
    return 0;
}
int ciuki_present(int display, int surface, const struct ciuki_rect *r)
{
    struct obj *d = fd_obj(cur, display), *s = fd_obj(cur, surface);
    if (!d || d->kind != K_DISPLAY || !s || s->kind != K_SURFACE || !r) { errno = EBADF; return -1; }
    presents++;
    present_px += (uint64_t)r->width * r->height;
    if (vnow >= idle_from) idle_px += (uint64_t)r->width * r->height;
    return 0;
}

/* Kernel input queue (i8042 events already converted for the desktop grant). */
static struct ciuki_input_event kq[CIUKI_INPUT_QUEUE_MAX];
static unsigned kq_head, kq_count, kq_sequence, usb_read_keys;
static void kq_push(uint32_t type, int32_t code, int32_t value, int32_t value2)
{
    if (kq_count == CIUKI_INPUT_QUEUE_MAX) fail("kernel input queue overflow");
    struct ciuki_input_event *e = &kq[(kq_head + kq_count++) % CIUKI_INPUT_QUEUE_MAX];
    *e = (struct ciuki_input_event){.sequence = ++kq_sequence, .type = type, .monotonic_ns = vnow / MS * MS,
                                    .source = usb_poll_ns ? INPUT_USB : 0u,
                                    .code = code, .value = value, .value2 = value2};
}
int ciuki_input_read(int fd, struct ciuki_input_event *events, uint32_t capacity)
{
    struct obj *o = fd_obj(cur, fd);
    if (!o || o->kind != K_INPUT) { errno = EBADF; return -1; }
    if (!kq_count) { errno = EAGAIN; return -1; }
    unsigned n = 0;
    while (n < capacity && kq_count) { events[n++] = kq[kq_head]; kq_head = (kq_head + 1) % CIUKI_INPUT_QUEUE_MAX; kq_count--; }
    if (usb_poll_ns) for (unsigned i = 0; i < n; i++) {
        assert(events[i].source == INPUT_USB);
        if (events[i].type == CIUKI_INPUT_KEY) usb_read_keys++;
    }
    if (trace) {
        const struct desktop *d = replay_desktop;
        for (unsigned i = 0; i < n; i++)
            note("input type=%u code=%d value=%d value2=%d (mouse before %d,%d focus=%d)", events[i].type, events[i].code,
                 events[i].value, events[i].value2, d ? d->mouse_x : -1, d ? d->mouse_y : -1, d ? d->focus : -9);
    }
    return (int)n;
}

ciuki_pid_t ciuki_spawn(const char *path, char *const argv[], char *const env[], const struct ciuki_spawn_fd *mapping,
                        uint32_t count, uint32_t flags)
{
    static const struct { const char *path; int (*entry)(int, char **); } programs[] = {
        {"/Programs/files", files_main}, {"/Programs/ciuknote", ciuknote_main}, {"/Programs/tasks", tasks_main}, {"/Programs/demo", demo_main}};
    int (*entry)(int, char **) = NULL;
    for (unsigned i = 0; i < sizeof(programs) / sizeof(programs[0]); i++) if (!strcmp(path, programs[i].path)) entry = programs[i].entry;
    if (!entry) {
        char name[80]; snprintf(name, sizeof(name), ",%s,", path + (strncmp(path, "/Programs/", sizeof("/Programs/") - 1) ? 0 : sizeof("/Programs/") - 1));
        char list[520]; snprintf(list, sizeof(list), ",%s,", bin_list);
        if (strncmp(path, "/Programs/", sizeof("/Programs/") - 1) || !strstr(list, name)) { errno = ENOENT; return -1; }
        entry = stub_main;
    }
    for (uint32_t i = 0; i < count; i++) {
        struct obj *o = fd_obj(cur, mapping[i].source);
        if (!o || cur->fd[mapping[i].source].cloexec || mapping[i].target < 0 || mapping[i].target >= MAX_FDS) { errno = EBADF; return -1; }
    }
    int argn = 0; while (argv && argv[argn]) argn++;
    int survivor = !strcmp(path, "/Programs/demo") && argn == 4 && !strcmp(argv[3], "--test=crash-isolation");
    /* Separate objects preserve the processes' separate static protocol
     * versions (v1 survivor, v2 launched demo) inside this threaded replay. */
    if (survivor) entry = survivor_demo_main;
    struct proc *p = proc_create(path + sizeof("/Programs/") - 1, entry, argv, env, cur->pid, flags & CIUKI_SPAWN_NEW_GROUP ? 0 : cur->pgid,
                                 cur->pid == desktop_pid && !survivor ? app_cost_ns : 2 * MS, survivor ? 0 : start_delay_ns);
    if (!p) return -1;
    for (uint32_t i = 0; i < count; i++) {
        struct obj *o = fd_obj(cur, mapping[i].source);
        if (p->fd[mapping[i].target].o) obj_put(p->fd[mapping[i].target].o);
        o->refs++; p->fd[mapping[i].target] = (struct fdent){o, 0, 0};
    }
    if (survivor) { survivor_pid = p->pid; next_pid = 105; }   /* 100 crash cycles used the PIDs in between */
    return p->pid;
}

static uint32_t proc_records(void *buffer, uint32_t bytes)
{
    struct ciuki_proc_system sys = {.version = CIUKI_PROC_VERSION, .size = sizeof(sys), .tick_hz = 1000,
        .total_ticks = vnow / MS, .idle_ticks = vnow / MS / 2, .usable_pages = 65536, .free_pages = 40000,
        .kheap_pages = 64, .kheap_bytes = 100000, .kheap_high_water = 120000};
    struct ciuki_proc_record list[MAX_PROCS + 2]; unsigned n = 0;
    static const char *const system[] = {"init", "probe"};
    for (int pid = 1; pid <= 2; pid++) {
        list[n] = (struct ciuki_proc_record){.version = CIUKI_PROC_VERSION, .size = sizeof(list[0]), .pid = (uint32_t)pid,
                                            .ppid = pid == 1 ? 0 : 1, .pgid = (uint32_t)pid, .state = CIUKI_PROC_LIVE, .threads = 1, .resident_pages = 32};
        snprintf(list[n++].name, CIUKI_PROC_NAME_MAX, "%s", system[pid - 1]);
    }
    for (int pid = 3; pid < next_pid; pid++) {
        struct proc *p = proc_find(pid);
        if (!p || !p->state) continue;
        list[n] = (struct ciuki_proc_record){.version = CIUKI_PROC_VERSION, .size = sizeof(list[0]), .pid = (uint32_t)p->pid,
            .ppid = (uint32_t)p->ppid, .pgid = (uint32_t)p->pgid, .state = (uint32_t)p->state, .threads = 1,
            .resident_pages = 256, .cpu_ticks = (vnow - p->start) / MS / 10, .start_tick = p->start / MS};
        snprintf(list[n++].name, CIUKI_PROC_NAME_MAX, "%.31s", p->name);
    }
    sys.processes = n;
    if (bytes < CIUKI_PROC_LIST_MIN) { errno = EINVAL; return (uint32_t)-1; }
    uint32_t fit = (bytes - (uint32_t)sizeof(sys)) / (uint32_t)sizeof(list[0]);
    sys.records = n < fit ? n : fit;
    memcpy(buffer, &sys, sizeof(sys));
    memcpy((uint8_t *)buffer + sizeof(sys), list, sys.records * sizeof(list[0]));
    return sys.records;
}
/* Desktop shell services: the gate scenarios never use the mixer or the power interface. */
int ciuki_devctl(int fd, uint32_t op, void *record, uint32_t size)
{ (void)fd; (void)op; (void)record; (void)size; errno = ENODEV; return -1; }
int ciuki_power(uint32_t action) { (void)action; errno = ENOSYS; return -1; }
/* G6 file information (call 98): not modelled; Files' Properties falls back. */
int ciuki_file_info_set(const char *path, uint32_t mask, const struct ciuki_file_info *in) { (void)path; (void)mask; (void)in; errno = ENOSYS; return -1; }
int ciuki_power_status(struct ciuki_power_status *out) { (void)out; errno=ENOSYS; return -1; }
/* G11: the EVENTS and POLICY records (no kernel in these scenarios). */
int ciuki_power_events(struct ciuki_power_events *out) { (void)out; errno=ENOSYS; return -1; }
int ciuki_power_policy(struct ciuki_power_policy *out) { (void)out; errno=ENOSYS; return -1; }
int ciuki_launch_mark(uint32_t event, ciuki_pid_t pid, uint64_t click_us)
{ (void)event; (void)pid; (void)click_us; return 0; } /* quiet replay */
int ciuki_proc_list(void *buffer, uint32_t bytes)
{
    uint32_t r = proc_records(buffer, bytes);
    return r == (uint32_t)-1 ? -1 : (int)r;
}
int ciuki_proc_terminate(ciuki_pid_t pid, int signo, int authority_fd)
{
    struct obj *a = authority_fd >= 0 ? fd_obj(cur, authority_fd) : NULL;
    if (authority_fd >= 0 && (!a || a->kind != K_AUTHORITY)) { errno = EBADF; return -1; }
    if (signo != SIGTERM && signo != SIGKILL) { errno = EINVAL; return -1; }
    /* PIDs 1 (init) and 2 (the probe's owner) are live system processes. */
    if (pid == 1 || pid == 2 || (int)pid == desktop_pid || (int)pid == cur->pid) { errno = EPERM; return -1; }
    struct proc *p = proc_find((int)pid);
    if (!p || p->state != CIUKI_PROC_LIVE) { errno = ESRCH; return -1; }
    p->killed = signo; p->wake = vnow;
    return 0;
}

/* ---- probe-report capture (the kernel side of call ciuki_raw_probe_report) -- */
static struct { unsigned generation, keys, motion, buttons, reports; int checkpoint, app, survivor_ready; } probe;
static uint64_t checkpoint_ns[8], success_ns;
static unsigned field(const char *line, const char *name, int *present)
{
    char key[40]; snprintf(key, sizeof(key), " %s=", name);
    const char *at = strstr(line, key);
    if (!at) { if (present) *present = 0; return 0; }
    if (present) *present = 1;
    return (unsigned)strtoul(at + strlen(key), NULL, 10);
}
uint32_t ciuki_raw_probe_report(uintptr_t address, uint32_t n, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    const char *line = (const char *)address;
    if (n != strlen(line) || n > CIUKI_PROBE_REPORT_MAX) fail("report length %u", n);
    if (cur->pid == survivor_pid && !strncmp(line, "case=native-demo ", 17)) {
        if (field(line, "stage", NULL) == 2) probe.survivor_ready = 1;
        return 0;
    }
    if (cur->pid != desktop_pid) fail("report from pid %d: %s", cur->pid, line);
    if (!strncmp(line, "case=native-setup ", 18)) fail("desktop setup failed: %s", line);
    if (strncmp(line, "case=native-desktop ", 20)) fail("unexpected report: %s", line);
    unsigned generation = field(line, "generation", NULL), keys = field(line, "keys", NULL);
    unsigned motion = field(line, "motion", NULL), buttons = field(line, "buttons", NULL);
    if (generation < probe.generation || keys < probe.keys || motion < probe.motion || buttons < probe.buttons)
        fail("probe order check: %s", line);
    probe.generation = generation; probe.keys = keys; probe.motion = motion; probe.buttons = buttons; probe.reports++;
    int files, notes;
    field(line, "files", &files); field(line, "note", &notes);
    if (files || notes) {
        int checkpoint = (int)field(line, "checkpoint", NULL);
        if (probe.app && probe.app != (files ? 1 : 2)) fail("app changed: %s", line);
        probe.app = files ? 1 : 2;
        if (checkpoint != probe.checkpoint && checkpoint != probe.checkpoint + 1) fail("checkpoint order: %s", line);
        if (checkpoint > probe.checkpoint) {
            probe.checkpoint = checkpoint; checkpoint_ns[checkpoint] = vnow;
            printf("CHECKPOINT %d t=%.3f\n", checkpoint, (double)vnow / 1e9);
        }
    }
    int shell;
    field(line,"shell",&shell);
    if (!success_ns && keys >= 2 && motion >= 1 && buttons >= 2 &&
        (!shell || !shell_probe_pending(field(line,"shell_stage",NULL),keys,buttons))) success_ns = vnow;
    note("report %s", line);
    return 0;
}

/* ---- rp_* (replay_os.h) ---------------------------------------------------- */
int rp_nanosleep(const struct timespec *t, struct timespec *rest)
{
    (void)rest;
    uint64_t ns = (uint64_t)t->tv_sec * 1000000000ull + (uint64_t)t->tv_nsec;
    yield_until(vnow + ns + cur->cost);
    return 0;
}
/* f2-98c: native applications block in poll() on their window channel, the
 * kernel input queue and completion channels. Descriptors are virtual, so the
 * wait is modelled in virtual time: readiness is evaluated against the
 * virtual objects and, while nothing is ready, the process yields in 1 ms
 * steps (the step ciuki_channel_recv uses) until the timeout (negative: no
 * deadline). A ready return charges the process cost once, as a nanosleep
 * turn did, so a loop that always finds work still advances virtual time. */
static int poll_ready(struct pollfd *fds, nfds_t n)
{
    int ready = 0;
    deliver_foreign_wakes();
    for (nfds_t i = 0; i < n; i++) {
        struct obj *o = fd_obj(cur, fds[i].fd);
        fds[i].revents = 0;
        if (!o) { fds[i].revents = POLLNVAL; ready++; continue; }
        if (o->kind == K_CHANNEL) {
            struct chan *c = o->chan; int side = o->side;
            if ((fds[i].events & POLLIN) && c->count[side]) fds[i].revents |= POLLIN;
            if ((fds[i].events & POLLOUT) && c->open[1 - side] && c->count[1 - side] < CIUKI_CHANNEL_QUEUE_MAX)
                fds[i].revents |= POLLOUT;
            if (!c->open[1 - side]) fds[i].revents |= POLLHUP;
        } else if (o->kind == K_INPUT && (fds[i].events & POLLIN) && kq_count) fds[i].revents |= POLLIN;
        if (fds[i].revents) ready++;
    }
    return ready;
}
int rp_poll(struct pollfd *fds, nfds_t n, int timeout_ms)
{
    uint64_t deadline = timeout_ms < 0 ? UINT64_MAX : vnow + (uint64_t)timeout_ms * MS;
    for (;;) {
        int ready = poll_ready(fds, n);
        if (ready) { if (cur->cost) yield_until(vnow + cur->cost); return ready; }
        if (vnow >= deadline) { if (cur->cost) yield_until(vnow + cur->cost); return 0; }
        uint64_t wake = vnow + 1 * MS;
        yield_until(wake < deadline ? wake : deadline);
    }
}
int rp_clock_gettime(clockid_t clock, struct timespec *out)
{
    uint64_t ns = clock == CLOCK_REALTIME ? 1791711660ull * 1000000000ull + vnow : vnow;
    out->tv_sec = (time_t)(ns / 1000000000ull); out->tv_nsec = (long)(ns % 1000000000ull);
    return 0;
}
char *rp_getenv(const char *name)
{
    size_t n = strlen(name);
    for (char **e = cur ? cur->env : NULL; e && *e; e++) if (!strncmp(*e, name, n) && (*e)[n] == '=') return *e + n + 1;
    return NULL;
}
int rp_open(const char *path, int flags, ...)
{
    /* The virtual kernel publishes no device nodes: a production open of
     * /dev/mixer or /dev/net must fail here, never reach the host (whose
     * /dev/net directory opens, and whose fd numbers collide with ours). */
    if (!strncmp(path, "/dev/", 5)) { errno = ENOENT; return -1; }
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap); }
    return open(path, flags, mode);
}
int rp_close(int fd)
{
    struct obj *o = fd_obj(cur, fd);
    if (!o) { errno = EBADF; return -1; }
    cur->fd[fd].o = NULL; obj_put(o);
    return 0;
}
int rp_munmap(void *p, size_t n) { (void)p; (void)n; return 0; }
/* The shell's post-fault geometry includes 100 accepted victim windows.
 * Exercise their production protocol/lifecycle rather than seeding cascade:
 * the earlier PID-only skip concealed f2-67's changed window positions. Fault
 * execution and ledger isolation remain the kernel/QEMU tests' responsibility. */
static void shell_window_history(struct proc *desktop)
{
    assert(!cur && replay_desktop);
    cur = desktop;
    struct desktop *d = replay_desktop;
    for (unsigned cycle = 0; cycle < 100; cycle++) {
        int pair[2]; assert(!ciuki_channel_pair(pair));
        int slot = desk_add_client(d, pair[0]); assert(slot >= 0);
        struct ciuki_message m;
        desk_message(&m, DESK_HELLO, 4); m.sender_pid = 5 + cycle;
        desk_receive(d, slot, &m);
        int surface = ciuki_surface_create(240, 160, CIUKI_SURFACE_XRGB8888);
        assert(surface >= 0);
        const char *title = gate_faults[cycle % 5];
        unsigned length = (unsigned)strlen(title);
        desk_message(&m, DESK_CREATE_WINDOW, 16 + length); m.sender_pid = 5 + cycle;
        desk_put32(m.data + 4, 240); desk_put32(m.data + 8, 160); desk_put32(m.data + 12, length);
        memcpy(m.data + 16, title, length); m.fd_count = 1; m.fds[0] = surface;
        desk_receive(d, slot, &m);
        assert(d->clients[slot].window);
        desk_drop_client(d, slot);
        assert(!rp_close(pair[1]));
        /* In the guest these cycles are separated by survivor turns. Deliver
         * its focus changes now, so the condensed history cannot overflow a
         * queue and accidentally turn this into a client-exit regression. */
        for (int i = 0; i < DESK_CLIENTS; i++) if (d->clients[i].pid == (uint32_t)survivor_pid) {
            desk_flush(d, i);
            struct obj *o = fd_obj(cur, d->clients[i].channel);
            assert(o && o->kind == K_CHANNEL);
            cur = NULL;
            while (o->chan->count[1 - o->side]) run(proc_find(survivor_pid));
            cur = desktop;
        }
    }
    printf("HISTORY windows=100 cascade=%u focus=%d\n", d->cascade, d->focus);
    cur = NULL;
}
static _Alignas(4096) uint8_t control_page[4096];
void *rp_mmap(void *addr, size_t n, int prot, int flags, int fd, off_t off)
{
    (void)prot;
    if (addr || n != 4096 || fd != -1 || off || !(flags & MAP_ANONYMOUS)) { errno = EINVAL; return MAP_FAILED; }
    return control_page;
}
int rp_fcntl(int fd, int cmd, ...)
{
    if (!fd_obj(cur, fd)) { errno = EBADF; return -1; }
    va_list ap; va_start(ap, cmd);
    int arg = cmd == F_SETFD || cmd == F_SETFL ? va_arg(ap, int) : 0;
    va_end(ap);
    switch (cmd) {
    case F_GETFD: return cur->fd[fd].cloexec ? FD_CLOEXEC : 0;
    case F_SETFD: cur->fd[fd].cloexec = (arg & FD_CLOEXEC) != 0; return 0;
    case F_GETFL: return cur->fd[fd].flags;
    case F_SETFL: cur->fd[fd].flags = arg; return 0;
    default: errno = EINVAL; return -1;
    }
}
pid_t rp_waitpid(pid_t pid, int *status, int options)
{
    int any = 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        struct proc *p = &procs[i];
        if (!p->used || p->ppid != cur->pid || !p->state || (pid > 0 && p->pid != pid)) continue;
        any = 1;
        if (p->state == CIUKI_PROC_ZOMBIE) {
            if (status) *status = p->status;
            p->state = 0;
            note("reap pid=%d status=%#x by %d", p->pid, p->status, cur->pid);
            return p->pid;
        }
    }
    if (!any) { errno = ECHILD; return -1; }
    if (!(options & WNOHANG)) fail("blocking waitpid");
    return 0;
}
int rp_kill(pid_t pid, int sig)
{
    struct proc *p = proc_find(pid);
    if (!p || p->state != CIUKI_PROC_LIVE) { errno = ESRCH; return -1; }
    if (sig) { p->killed = sig; p->wake = vnow; note("kill pid=%d sig=%d by %d", pid, sig, cur->pid); }
    return 0;
}
pid_t rp_getpid(void) { return cur->pid; }

/* /Programs as the desktop scans it: the image's programs (scripts/build_image.py). */
struct rp_dir { char names[DESK_MENU_MAX + 1][64]; unsigned count, at; struct dirent entry; };
DIR *rp_opendir(const char *path)
{
    if (strcmp(path, "/Programs")) { errno = ENOENT; return NULL; }
    struct rp_dir *d = calloc(1, sizeof(*d)); assert(d);
    char list[512]; snprintf(list, sizeof(list), "%s", bin_list);
    for (char *s = strtok(list, ","); s && d->count < DESK_MENU_MAX + 1; s = strtok(NULL, ",")) snprintf(d->names[d->count++], 64, "%s", s);
    return (DIR *)d;
}
struct dirent *rp_readdir(DIR *dir)
{
    struct rp_dir *d = (struct rp_dir *)dir;
    if (d->at == d->count) return NULL;
    memset(&d->entry, 0, sizeof(d->entry));
    snprintf(d->entry.d_name, sizeof(d->entry.d_name), "%s", d->names[d->at++]);
    return &d->entry;
}
int rp_closedir(DIR *dir) { free(dir); return 0; }
static int in_bin(const char *path)
{
    if (strncmp(path, "/Programs/", sizeof("/Programs/") - 1)) return 0;
    char name[80], list[520]; snprintf(name, sizeof(name), ",%s,", path + sizeof("/Programs/") - 1); snprintf(list, sizeof(list), ",%s,", bin_list);
    return strstr(list, name) != NULL;
}
int rp_stat(const char *path, struct stat *st)
{
    if (!in_bin(path)) { errno = ENOENT; return -1; }
    memset(st, 0, sizeof(*st)); st->st_mode = S_IFREG | 0755; st->st_size = 100000;
    return 0;
}
static unsigned char elf_header[52] = {0x7f, 'E', 'L', 'F', 1, 1, 1, [16] = 2, [18] = 3, [20] = 1, [40] = 52};
FILE *rp_fopen(const char *path, const char *mode)
{
    if (in_bin(path) && !strcmp(mode, "rb")) return fmemopen(elf_header, sizeof(elf_header), "rb");
    if (!strcmp(path, "/system/assets/ciuki-portrait.xrgb") && portrait_path) return fopen(portrait_path, "rb");
    errno = ENOENT; return NULL;
}
int rp_uname(struct utsname *u)
{
    memset(u, 0, sizeof(*u));
    snprintf(u->sysname, sizeof(u->sysname), "CiukiOS"); snprintf(u->release, sizeof(u->release), "replay");
    return 0;
}
_Noreturn void rp_exit(int code) { proc_exit(cur, (code & 255) << 8); }
int desk_load_portrait(uint32_t *pixels)
{
    memset(pixels, 0, DESK_PORTRAIT_SIZE * DESK_PORTRAIT_SIZE * 4);
    FILE *f = portrait_path ? fopen(portrait_path, "rb") : NULL;
    if (!f) return 0;
    size_t n = fread(pixels, 4, DESK_PORTRAIT_SIZE * DESK_PORTRAIT_SIZE, f);
    fclose(f);
    return n == DESK_PORTRAIT_SIZE * DESK_PORTRAIT_SIZE ? 0 : -1;
}

/* ---- the image's file system for Files and CiukNote (fake_vfs.c tree) -------- */
const struct fm_vfs *fm_native_vfs(void) { return &fake_vfs; }
/* G6 native metadata has no kernel in this replay; the virtual VFS keeps
 * supplying ordinary size/time/stat metadata. No PE fixture is installed. */
int fm_native_version(const char *path,char *out,size_t cap)
{ (void)path; if (cap) out[0]=0; return -ENOENT; }
int ciuki_file_info_get(const char *path,struct ciuki_file_info *out)
{ (void)path; (void)out; errno=ENOSYS; return -1; }
int ciuki_set_attributes(const char *path,uint32_t attributes)
{ (void)path; (void)attributes; errno=ENOSYS; return -1; }

static int cn_stat(void *ctx, const char *path, struct cn_stat *st)
{
    struct fm_meta m; int r = fake_vfs.stat_path(ctx, path, &m);
    if (!r) *st = (struct cn_stat){m.type == FM_DIR, m.size, m.modified};
    return r;
}
struct cn_list { cn_emit emit; void *arg; };
static int cn_list_one(const char *name, const struct fm_meta *m, void *arg)
{
    struct cn_list *l = arg; struct cn_stat st = {m->type == FM_DIR, m->size, m->modified};
    return l->emit(name, &st, l->arg);
}
static int cn_list(void *ctx, const char *dir, cn_emit emit, void *arg)
{ struct cn_list l = {emit, arg}; return fake_vfs.list(ctx, dir, FM_LIST_SNIFF, cn_list_one, &l); }
static int cn_load(void *ctx, const char *path, char *buf, size_t cap, size_t *len)
{
    (void)ctx; int n = fake_lookup(path);
    if (n < 0) return -ENOENT;
    if (fake_nodes[n].meta.type == FM_DIR) return -EISDIR;
    if (fake_nodes[n].meta.size > cap) return -EFBIG;
    memcpy(buf, fake_nodes[n].data, fake_nodes[n].meta.size); *len = fake_nodes[n].meta.size;
    return 0;
}
static int cn_save(void *ctx, const char *path, const char *data, size_t len)
{
    (void)ctx;
    if (len > FAKE_DATA) return -ENOSPC;
    char parent[FM_PATH]; fm_parent(parent, path);
    int p = fake_lookup(parent);
    if (p < 0 || fake_nodes[p].meta.type != FM_DIR) return -ENOENT;
    int n = fake_lookup(path);
    if (n >= 0 && fake_nodes[n].meta.type == FM_DIR) return -EISDIR;
    if (n < 0 && (n = fake_create(path, FM_FILE)) < 0) return n;
    memcpy(fake_nodes[n].data, data, len); fake_nodes[n].meta.size = len;
    return 0;
}
static const struct cn_fs replay_cn_fs = {NULL, cn_stat, cn_list, cn_load, cn_save};
const struct cn_fs *cn_native_fs(void) { return &replay_cn_fs; }
static void image_tree(void)
{
    fake_image_tree();   /* /, /Programs, /system as in tests/host/desktop/files_ui_test.c */
    fake_create("/Documents", FM_DIR); fake_create("/tmp", FM_DIR);
    char list[512]; snprintf(list, sizeof(list), "%s", bin_list);
    for (char *s = strtok(list, ","); s; s = strtok(NULL, ",")) {
        char path[80]; snprintf(path, sizeof(path), "/Programs/%s", s);
        if (fake_lookup(path) < 0) { int n = fake_file(path, 2000, 1); fake_nodes[n].meta.exec = 1; fake_nodes[n].meta.mode = 0100777; }
    }
}

/* ---- the runner's batches through QEMU's PS/2 devices and the i8042 driver --- */
enum { EV_KEY, EV_REL, EV_BTN };
struct event { int type, code, value; };
struct batch { unsigned pause_ms, count; struct event events[32]; };
static struct batch batches[512];
static unsigned batch_count, batch_next;
static uint64_t batch_due, last_batch_ns;
static int qemu_dx, qemu_dy, qemu_buttons, kernel_buttons, kernel_down[0x200];
static unsigned dropped_bytes, leftover_batches, max_kbd_bytes, max_packets;
static const char us[0x59] = {
    [0x02]='1', [0x03]='2', [0x04]='3', [0x05]='4', [0x06]='5', [0x07]='6', [0x08]='7', [0x09]='8', [0x0A]='9', [0x0B]='0',
    [0x0C]='-', [0x0D]='=', [0x0E]='\b', [0x0F]='\t', [0x10]='q', [0x11]='w', [0x12]='e', [0x13]='r', [0x14]='t', [0x15]='y',
    [0x16]='u', [0x17]='i', [0x18]='o', [0x19]='p', [0x1A]='[', [0x1B]=']', [0x1C]='\r', [0x1E]='a', [0x1F]='s', [0x20]='d',
    [0x21]='f', [0x22]='g', [0x23]='h', [0x24]='j', [0x25]='k', [0x26]='l', [0x27]=';', [0x28]=39, [0x29]='`', [0x2B]=92,
    [0x2C]='z', [0x2D]='x', [0x2E]='c', [0x2F]='v', [0x30]='b', [0x31]='n', [0x32]='m', [0x33]=',', [0x34]='.', [0x35]='/',
    [0x39]=' '};
static void kernel_key(int code, int down)
{
    int value = down;
    if (kernel_down[code] == down) { if (!down) return; value = 2; }
    else kernel_down[code] = down;
    kq_push(CIUKI_INPUT_KEY, code, value, 0);
    if (down && code < (int)sizeof(us) && us[code]) kq_push(CIUKI_INPUT_TEXT, (unsigned char)us[code], 0, 0);
}
/* --i8042-byte-ms: QEMU's keyboard and aux queues (units of one key edge or one packet) and the output
 * buffer the guest reads one byte at a time. */
struct unit { int code, value, dx, dy, buttons; unsigned left, seq; };
static struct unit kbd_q[16], aux_q[16];
static unsigned kbd_n, aux_n, kbd_bytes, aux_bytes, unit_seq, reordered;
static int latched, sent_buttons;   /* latched: 0 empty output buffer, 1 keyboard byte, 2 aux byte */
static uint64_t byte_due;
static void latch(void)
{
    latched = kbd_n ? 1 : aux_n ? 2 : 0;           /* pckbd.c: the keyboard first */
    if (latched == 1) kbd_bytes--; else if (latched == 2) aux_bytes--;
    byte_due = latched ? vnow + byte_ns : 0;
}
static void kernel_packet(int dx, int dy, int buttons)
{
    if (dx) kq_push(CIUKI_INPUT_MOTION, 0, dx, 0);
    if (dy) kq_push(CIUKI_INPUT_MOTION, 0, 0, -dy);
    int changed = buttons ^ kernel_buttons; kernel_buttons = buttons;
    for (int i = 0; i < 3; i++) if (changed & (1 << i)) kq_push(CIUKI_INPUT_BUTTON, i + 1, (buttons >> i) & 1, 0);
}
static void read_byte(void)
{
    struct unit *q = latched == 1 ? kbd_q : aux_q;
    unsigned *n = latched == 1 ? &kbd_n : &aux_n;
    if (!--q[0].left) {
        struct unit u = q[0];
        memmove(q, q + 1, (--*n) * sizeof(*q));
        if (latched == 1) {
            if (aux_n && aux_q[0].seq < u.seq) reordered++;   /* an older packet still waits */
            kernel_key(u.code, u.value);
        } else kernel_packet(u.dx, u.dy, u.buttons);
    }
    latch();
}
static void deliver_queued(struct batch *b)
{
    int mouse = 0;
    for (unsigned i = 0; i < b->count; i++) {
        struct event *e = &b->events[i];
        if (e->type == EV_KEY) {
            unsigned need = (e->code >= 0x100) + 1u;
            if (kbd_bytes + need > 16 || kbd_n == 16) { dropped_bytes += need; continue; }
            kbd_q[kbd_n++] = (struct unit){.code = e->code, .value = e->value, .left = need, .seq = ++unit_seq};
            kbd_bytes += need;
            if (kbd_bytes > max_kbd_bytes) max_kbd_bytes = kbd_bytes;
        } else if (e->type == EV_REL) {
            mouse = 1;
            if (e->code == 0) qemu_dx += e->value; else qemu_dy -= e->value;
        } else {
            mouse = 1;
            unsigned bit = e->code == 1 ? 1u : e->code == 2 ? 2u : 4u;
            if (e->value) qemu_buttons |= (int)bit; else qemu_buttons &= ~(int)bit;
        }
    }
    if (mouse) {
        /* ps2_mouse_sync: packets while the queue has room for 3 bytes; none at all when it is full. */
        unsigned packets = 0;
        while (aux_bytes + 3 <= 16 && aux_n < 16) {
            int dx = qemu_dx > 127 ? 127 : qemu_dx < -127 ? -127 : qemu_dx;
            int dy = qemu_dy > 127 ? 127 : qemu_dy < -127 ? -127 : qemu_dy;
            qemu_dx -= dx; qemu_dy -= dy; packets++;
            aux_q[aux_n++] = (struct unit){.dx = dx, .dy = dy, .buttons = qemu_buttons, .left = 3, .seq = ++unit_seq};
            aux_bytes += 3; sent_buttons = qemu_buttons;
            if (!qemu_dx && !qemu_dy) break;
        }
        if (qemu_dx || qemu_dy || sent_buttons != qemu_buttons) leftover_batches++;
        if (packets > max_packets) max_packets = packets;
    }
    if (!latched) latch();
}
/* Independent QEMU HID queues. Store translated keyboard edges; pointer
 * entries are reports before hid_pointer_poll splits their relative deltas.
 * QEMU reserves one of its 16 pointer slots for the incoming sync. */
struct usb_pointer { int dx, dy, buttons; };
static struct usb_pointer usb_mouse[16];
static struct event usb_keys[16];
static unsigned usb_mouse_n, usb_keys_n, usb_compressed, usb_reports;
static int usb_buttons;
static int usb_first_click_x = -1, usb_first_click_y = -1;
static void deliver_usb(struct batch *b)
{
    struct usb_pointer p = {.buttons = usb_buttons};
    int mouse = 0;
    for (unsigned i = 0; i < b->count; i++) {
        const struct event *e = &b->events[i];
        if (e->type == EV_KEY) {
            if (usb_keys_n == 16) fail("USB keyboard model queue full");
            usb_keys[usb_keys_n++] = *e;
        } else if (e->type == EV_REL) {
            mouse = 1;
            if (e->code == 0) p.dx += e->value; else p.dy += e->value;
        } else {
            mouse = 1;
            int bit = 1 << (e->code - 1);
            if (e->value) p.buttons |= bit; else p.buttons &= ~bit;
        }
    }
    usb_buttons = p.buttons;
    if (!mouse) return;
    if (usb_mouse_n && usb_mouse[usb_mouse_n - 1].buttons == p.buttons) {
        usb_mouse[usb_mouse_n - 1].dx += p.dx;
        usb_mouse[usb_mouse_n - 1].dy += p.dy;
        usb_compressed++;
    } else {
        if (usb_mouse_n == 15) fail("USB pointer model queue full");
        usb_mouse[usb_mouse_n++] = p;
    }
}
static void poll_usb(void)
{
    if (usb_keys_n) {
        struct event e = usb_keys[0];
        memmove(usb_keys, usb_keys + 1, --usb_keys_n * sizeof(*usb_keys));
        kernel_key(e.code, e.value);
    }
    if (usb_mouse_n) {
        struct usb_pointer *p = &usb_mouse[0];
        int dx = p->dx > 127 ? 127 : p->dx < -127 ? -127 : p->dx;
        int dy = p->dy > 127 ? 127 : p->dy < -127 ? -127 : p->dy;
        p->dx -= dx; p->dy -= dy;
        usb_reports++;
        note("USB mouse report dx=%d dy=%d buttons=%d", dx, dy, p->buttons);
        /* usb_hid_report emits button transitions before its axis events. */
        int changed = p->buttons ^ kernel_buttons; kernel_buttons = p->buttons;
        for (int i = 0; i < 3; i++) if (changed & (1 << i)) {
            if (i == 0 && (p->buttons & 1) && usb_first_click_x < 0 && replay_desktop) {
                usb_first_click_x = replay_desktop->mouse_x;
                usb_first_click_y = replay_desktop->mouse_y;
            }
            kq_push(CIUKI_INPUT_BUTTON, i + 1, (p->buttons >> i) & 1, 0);
        }
        if (dx) kq_push(CIUKI_INPUT_MOTION, 0, dx, 0);
        if (dy) kq_push(CIUKI_INPUT_MOTION, 0, 0, dy);
        if (!p->dx && !p->dy) memmove(usb_mouse, usb_mouse + 1, --usb_mouse_n * sizeof(*usb_mouse));
    }
    usb_due = vnow + usb_poll_ns;
}
static void deliver(struct batch *b)
{
    if (usb_poll_ns) { deliver_usb(b); return; }
    if (byte_ns) { deliver_queued(b); return; }
    unsigned bytes = 0; int mouse = 0;
    for (unsigned i = 0; i < b->count; i++) {
        struct event *e = &b->events[i];
        if (e->type == EV_KEY) {
            /* Set 2 without translation (the kernel decodes set 2): make 1 byte, break F0+1, E0 prefix. */
            unsigned need = (e->code >= 0x100) + (e->value ? 1u : 2u);
            if (bytes + need > 16) { dropped_bytes += need; continue; }   /* ps2_queue_noirq drops at 16 */
            bytes += need; kernel_key(e->code, e->value);
        } else if (e->type == EV_REL) {
            mouse = 1;
            if (e->code == 0) qemu_dx += e->value; else qemu_dy -= e->value;   /* PS/2 Y grows up */
        } else {
            mouse = 1;
            unsigned bit = e->code == 1 ? 1u : e->code == 2 ? 2u : 4u;     /* PS/2 header: left, right, middle */
            if (e->value) qemu_buttons |= (int)bit; else qemu_buttons &= ~(int)bit;
        }
    }
    if (bytes > max_kbd_bytes) max_kbd_bytes = bytes;
    if (!mouse) return;
    /* ps2_mouse_sync: packets while the 16-byte queue has room for 3 bytes, the first one always. */
    unsigned packets = 0;
    for (;;) {
        if (packets == 5) { leftover_batches++; break; }
        int dx = qemu_dx > 127 ? 127 : qemu_dx < -127 ? -127 : qemu_dx;
        int dy = qemu_dy > 127 ? 127 : qemu_dy < -127 ? -127 : qemu_dy;
        qemu_dx -= dx; qemu_dy -= dy; packets++;
        if (dx) kq_push(CIUKI_INPUT_MOTION, 0, dx, 0);
        if (dy) kq_push(CIUKI_INPUT_MOTION, 0, 0, -dy);
        int changed = qemu_buttons ^ kernel_buttons; kernel_buttons = qemu_buttons;
        for (int i = 0; i < 3; i++) if (changed & (1 << i)) kq_push(CIUKI_INPUT_BUTTON, i + 1, (qemu_buttons >> i) & 1, 0);
        if (!qemu_dx && !qemu_dy) break;
    }
    if (packets > max_packets) max_packets = packets;
}
static void load_script(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) fail("script %s", path);
    char kind[4];
    while (fscanf(f, "%3s", kind) == 1) {
        if (kind[0] == 'B') {
            if (batch_count == sizeof(batches) / sizeof(batches[0])) fail("too many batches");
            if (fscanf(f, "%u", &batches[batch_count].pause_ms) != 1) fail("script B");
            batch_count++;
            continue;
        }
        if (!batch_count) fail("script: event before batch");
        struct batch *b = &batches[batch_count - 1];
        if (b->count == 32) fail("script: batch too long");
        struct event *e = &b->events[b->count++];
        int ok = fscanf(f, "%d %d", &e->code, &e->value) == 2;
        e->type = kind[0] == 'K' ? EV_KEY : kind[0] == 'R' ? EV_REL : kind[0] == 'M' ? EV_BTN : -1;
        if (!ok || e->type < 0) fail("script line %s", kind);
    }
    fclose(f);
}

/* ---- controller: the crash-isolation probe's interaction stage -------------- */
static void describe_desktop(void)
{
    const struct desktop *d = replay_desktop;
    if (!d) return;
    printf("DESKTOP mouse=%d,%d focus=%d shell=%d menu_open=%d app_test=%d app_pid=%u app_checkpoint=%d app_reported=%d app_done=%d"
           " tasks_test=%d tasks_stage=%d shell_test=%d shell_stage=%d notice=\"%s\"\n", d->mouse_x, d->mouse_y, d->focus, d->shell, d->menu_open, d->app_test,
           d->app_pid, d->app_checkpoint, d->app_reported, d->app_done, d->tasks_test, d->tasks_stage, d->shell_test, d->shell_stage, d->notice);
    for (int i = 0; i < DESK_CLIENTS; i++) if (d->clients[i].window)
        printf("WINDOW slot=%d pid=%u title=\"%s\" frame=%d,%d,%d,%d order=%llu%s\n", i, d->clients[i].pid, d->clients[i].title,
               d->clients[i].frame.x, d->clients[i].frame.y, d->clients[i].frame.w, d->clients[i].frame.h,
               (unsigned long long)d->clients[i].order, d->clients[i].minimized ? " minimized" : "");
}
int main(int argc, char **argv)
{
    const char *script = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
#define OPT(name) (!strcmp(a, name) && v && ++i)
        if (OPT("--script")) script = v;
        else if (OPT("--case")) case_name = v;
        else if (OPT("--bin")) snprintf(bin_list, sizeof(bin_list), "%s", v);
        else if (OPT("--portrait")) portrait_path = v;
        else if (OPT("--overhead-ms")) overhead_ns = strtoull(v, NULL, 10) * MS;
        else if (OPT("--pause-scale-pct")) pause_scale_pct = (unsigned)strtoul(v, NULL, 10);
        else if (OPT("--first-ms")) first_ns = strtoull(v, NULL, 10) * MS;
        else if (OPT("--start-delay-ms")) start_delay_ns = strtoull(v, NULL, 10) * MS;
        else if (OPT("--desktop-cost-ms")) desktop_cost_ns = strtoull(v, NULL, 10) * MS;
        else if (OPT("--app-cost-ms")) app_cost_ns = strtoull(v, NULL, 10) * MS;
        else if (OPT("--usb-poll-ms")) usb_poll_ns = strtoull(v, NULL, 10) * MS;
        else if (OPT("--i8042-byte-ms")) byte_ns = strtoull(v, NULL, 10) * MS;
        else if (OPT("--i8042-stall-ms")) stall_ns = strtoull(v, NULL, 10) * MS;
        else if (OPT("--i8042-stall-batch")) stall_batch = atoi(v);
        else if (OPT("--limit-s")) limit_ns = strtoull(v, NULL, 10) * 1000 * MS;
        else if (!strcmp(a, "--trace")) trace++;
        else if (OPT("--shots")) shots = v;
        else fail("usage: argument %s", a);
#undef OPT
    }
    if (!script || !case_name || !bin_list[0]) fail("usage: --script, --case and --bin are required");
    if (usb_poll_ns && byte_ns) fail("USB and i8042 models are mutually exclusive");
    load_script(script);
    image_tree();
    pthread_mutex_lock(&big);
    /* The supervisor's desktop: display 3, input 4, termination authority 5 (CIUKI_TASKS_FD). */
    char *dargv[] = {"desktop", "--test=crash-isolation", "--gate", NULL};
    char *denv[] = {"LC_ALL=C", "TZ=UTC0", "HOME=/Documents", "TMPDIR=/tmp", "PATH=/Programs", "CIUKI_TASKS_FD=5", NULL};
    struct proc *desktop = proc_create("desktop", desktop_main, dargv, denv, 2, 0, desktop_cost_ns, 0);
    desktop_pid = desktop->pid;
    desktop->fd[3].o = obj_new(K_DISPLAY); desktop->fd[4].o = obj_new(K_INPUT); desktop->fd[5].o = obj_new(K_AUTHORITY);
    struct gate_control *control = (struct gate_control *)control_page;
    int stage = 0; uint64_t arm_ns = 0, end_ns = 0;
    unsigned last_cp = 0;
    while (vnow < limit_ns) {
        if (run_files_worker()) continue;
        /* Controller: survivor ready -> GATE_INTERACT -> its report -> 20 ms -> ARM. */
        if (stage == 0 && probe.reports && probe.survivor_ready) {
            if (!strcmp(case_name, "shell")) shell_window_history(desktop);
            control->generation = 501; control->command = GATE_INTERACT; stage = 1;
        } else if (stage == 1 && probe.generation == 501 && !control->command) {
            if (usb_poll_ns) usb_due = vnow + usb_poll_ns;
            arm_ns = vnow + 20 * MS; batch_due = arm_ns + first_ns; stage = 2;
            note("ARM at %.3f", (double)arm_ns / 1e9);
        }
        if (stage == 2 && batch_next < batch_count && vnow >= batch_due) {
            struct batch *b = &batches[batch_next++];
            int press = 0;
            for (unsigned i = 0; i < b->count; i++) press |= b->events[i].type == EV_BTN && b->events[i].value;
            if (shots && (press || batch_next == batch_count) && replay_desktop && replay_desktop->pixels) {
                char path[1024]; snprintf(path, sizeof(path), "%s/batch-%03u.xrgb", shots, batch_next - 1);
                FILE *f = fopen(path, "wb");
                if (f) { fwrite(replay_desktop->pixels, 4, WIDTH * HEIGHT, f); fclose(f); }
            }
            deliver(b);
            if (byte_ns && (int)batch_next - 1 == stall_batch) {
                /* The byte in the output buffer is read only after the stall; nothing else in between. */
                if (!latched) latch();
                if (latched) byte_due = vnow + stall_ns;
            }
            last_batch_ns = vnow;
            batch_due = vnow + overhead_ns + (uint64_t)b->pause_ms * MS * pause_scale_pct / 100;
            if (trace) note("batch %u delivered (%u events, queue %u)", batch_next - 1, b->count, kq_count);
        }
        if (usb_poll_ns && stage == 2 && vnow >= usb_due) poll_usb();
        if (latched && vnow >= byte_due) read_byte();
        if (trace && probe.checkpoint != (int)last_cp) { last_cp = (unsigned)probe.checkpoint; describe_desktop(); }
        if (success_ns && !end_ns) end_ns = success_ns + 3000 * MS;   /* the probe sleeps 3 s, then ENDs */
        if (success_ns && batch_next == batch_count && idle_from == UINT64_MAX)
            idle_from = (success_ns > last_batch_ns ? success_ns : last_batch_ns) + 1000 * MS;
        if (end_ns && vnow >= end_ns) break;
        struct proc *next = NULL;
        for (int i = 0; i < MAX_PROCS; i++) {
            struct proc *p = &procs[i];
            if (p->used && p->state == CIUKI_PROC_LIVE && p->wake <= vnow && (!next || p->wake < next->wake || (p->wake == next->wake && p->pid < next->pid)))
                next = p;
        }
        if (next) { run(next); continue; }
        uint64_t wake = UINT64_MAX;
        for (int i = 0; i < MAX_PROCS; i++) if (procs[i].used && procs[i].state == CIUKI_PROC_LIVE && procs[i].wake < wake) wake = procs[i].wake;
        if (stage == 2 && batch_next < batch_count && batch_due < wake) wake = batch_due;
        if (usb_poll_ns && stage == 2 && usb_due < wake) wake = usb_due;
        if (latched && byte_due < wake) wake = byte_due;
        if (end_ns && end_ns < wake) wake = end_ns;
        if (wake == UINT64_MAX) fail("every process exited");
        vnow = wake;
    }
    unsigned counts[3]; replay_gate_counts(counts);
    describe_desktop();
    printf("PROBE generation=%u keys=%u motion=%u buttons=%u checkpoint=%d app=%d reports=%u presents=%u\n", probe.generation,
           probe.keys, probe.motion, probe.buttons, probe.checkpoint, probe.app, probe.reports, presents);
    printf("GATE keys=%u motion=%u buttons=%u\n", counts[0], counts[1], counts[2]);
    printf("PRESENTS count=%u px=%llu idle_px=%llu idle_ms=%llu frame_px=%u\n", presents, (unsigned long long)present_px,
           (unsigned long long)idle_px, (unsigned long long)(vnow > idle_from ? (vnow - idle_from) / MS : 0), WIDTH * HEIGHT);
    printf("PS2 dropped_bytes=%u leftover_batches=%u max_key_bytes=%u max_packets=%u reordered=%u\n", dropped_bytes,
           leftover_batches, max_kbd_bytes, max_packets, reordered);
    if (usb_poll_ns) printf("USB poll_ms=%llu compressed=%u reports=%u read_keys=%u pending_mouse=%u pending_keys=%u first_click_x=%d first_click_y=%d\n",
        (unsigned long long)(usb_poll_ns / MS), usb_compressed, usb_reports, usb_read_keys, usb_mouse_n, usb_keys_n,
        usb_first_click_x, usb_first_click_y);
    printf("TIMING arm=%.3f batches=%u/%u last_batch=%.3f success=%.3f\n", (double)arm_ns / 1e9, batch_next, batch_count,
           arm_ns && last_batch_ns ? (double)(last_batch_ns - arm_ns) / 1e9 : -1.0,
           success_ns ? (double)(success_ns - arm_ns) / 1e9 : -1.0);
    for (int i = 1; i <= probe.checkpoint; i++) printf("CHECKPOINT_AT %d %.3f\n", i, (double)(checkpoint_ns[i] - arm_ns) / 1e9);
    for (int i = 0; i < FAKE_NODES; i++) if (fake_nodes[i].live) {
        char parent[FM_PATH];
        fm_parent(parent, fake_nodes[i].path);
        if (fm_same_path(parent, "/system")) printf("SYSTEM %s\n", fake_nodes[i].path + sizeof("/system/") - 1);
    }
    int note_txt = fake_lookup("/system/tests/note.txt");
    if (note_txt >= 0) {
        printf("FILE /system/tests/note.txt ");
        for (unsigned i = 0; i < fake_nodes[note_txt].meta.size; i++) printf("%02x", fake_nodes[note_txt].data[i]);
        printf("\n");
    }
    for (int i = 0; i < MAX_PROCS; i++) if (procs[i].used)
        printf("PROC pid=%d ppid=%d name=%s state=%d status=%#x\n", procs[i].pid, procs[i].ppid, procs[i].name, procs[i].state, procs[i].status);
    printf("CASE %s\n", case_name);
    printf("RESULT success=%d\n", success_ns != 0);
    fflush(stdout);
    _exit(0);
}

int ciuki_display_query(struct ciuki_display_info *i) { (void)i; return -ENOSYS; }
int ciuki_display_set(uint16_t id) { (void)id; return -EPERM; }
int ciuki_display_accel(int fd,const struct ciuki_accel_request *q) { (void)fd;(void)q;return -ENOSYS; }
int ciuki_display_accel_info(int fd,uint32_t generation,struct ciuki_accel_info *out) { (void)fd;(void)generation;(void)out;return -ENOSYS; }
int ciuki_display_engine_info(int fd,uint32_t generation,struct ciuki_display_engine_info *out) { (void)fd;(void)generation;(void)out;return -ENOSYS; }
int ciuki_display_commands(int fd,uint32_t generation,struct ciuki_command_info *out) { (void)fd;(void)generation;(void)out;return -ENOSYS; }
int ciuki_display_batch(int fd,const void *blob,uint32_t bytes,struct ciuki_batch_reply *out) { (void)fd;(void)blob;(void)bytes;(void)out;return -ENOSYS; }
int ciuki_display_fence(int fd,uint64_t fence,uint16_t action,uint32_t timeout,struct ciuki_batch_reply *out) { (void)fd;(void)fence;(void)action;(void)timeout;(void)out;return -ENOSYS; }
