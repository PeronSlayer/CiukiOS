/* Bounded duplex queues. Preparation, copyout and publication are separate:
 * bad buffers/full fd tables cannot consume a head message.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
extern uint32_t file_description_count(void) __attribute__((weak));
#define FILE_OBJECTS ((file_description_count ? file_description_count() : 0) + (net_socket_count ? net_socket_count() : 0))
extern uint32_t net_socket_count(void) __attribute__((weak));
struct process;
extern void net_fd_changed(struct process *, unsigned) __attribute__((weak));
#include <ciuki/desktop.h>
#include <ciuki/signal.h>
#include <ciuki/security.h>
#include "../fs/owner_channel.h"
#pragma weak proc_notify /* sec-own: channel harnesses without the process table */

struct channel_message {
    struct ciuki_message wire;
    struct desktop_description *attachments[CIUKI_MESSAGE_FD_MAX];
    struct channel_message *next;
};
struct channel_queue { struct channel_message *head, *tail; unsigned count; };
static struct channel *security_channels;
static uint32_t endpoint_high_water, service_exchanges;
struct channel {
    struct channel_queue inbox[2];
    struct kwait changed[2];
    bool open[2];
    bool revoked[2], handed[2];
    uint32_t generation[2];
    struct security_subject *subjects[2];
    struct { uint32_t id, generation; } receiver[2];
    struct channel *security_next;
    uint32_t service, service_generation;
    bool accepted, exchange, admitted, complete[2], replying;
    bool readiness, ready_sent;
    uint64_t request_id, last_request_id, deadline;
    uint32_t frame_header[16], reply_header[16], received[2];
    uint32_t body_pages[2][2], delivered[2];
    bool drained[2];
};

static void channel_security_discard(struct channel *c);
static void channel_security_reclaim(struct channel *c);
static int channel_security_frame(struct channel *, unsigned, const struct ciuki_message *);
static void channel_security_timeout(struct channel *c);
static bool channel_security_readable(struct channel *c, unsigned side)
{ return c->exchange && c->complete[side^1] && !c->drained[side]; }
static void channel_security_message(struct channel *, unsigned, struct ciuki_message *);
static struct task *channel_receiver(struct channel *c, unsigned side)
{
    for (struct task *t = sched_tasks(); t; t = t->all_next)
        if (t->id == c->receiver[side].id && t->generation == c->receiver[side].generation &&
            t->state != T_ZOMBIE && t->state != T_DEAD) return t;
    return 0;
}
static bool channel_ui_authority(struct process *p)
{
    for (unsigned i = 0; i < CIUKI_OPEN_MAX; i++) {
        struct desktop_description *d = desktop_fd(p, (int)i);
        if (d && d->kind == DESKTOP_INPUT && d->owner == p->pid) return true;
    }
    return false;
}
struct channel_operation {
    struct desktop_description *endpoint;
    struct channel_message *message;
    struct process *process;
    struct ua_pin pin;
    bool pinned;
};

static void channel_message_free(struct channel_message *m)
{
    if (!m) return;
    for (unsigned i = 0; i < CIUKI_MESSAGE_FD_MAX; i++)
        if (m->attachments[i]) m->attachments[i]->object.release(&m->attachments[i]->object);
    kfree(m);
}

/* f7-14: both processes hold opposite open ends of one channel. The
 * desktop's native clients are exactly its channel peers; the present lease
 * admits them beside the Win32 service links. */
bool channel_connected(const struct process *a, const struct process *b)
{
    if (!a || !b || a == b) return false;
    for (unsigned i = 0; i < CIUKI_OPEN_MAX; i++) {
        struct desktop_description *x = desktop_fd(a, (int32_t)i);
        if (!x || x->kind != DESKTOP_CHANNEL) continue;
        struct channel *c = x->u.endpoint.channel;
        if (!c->open[0] || !c->open[1]) continue;
        for (unsigned j = 0; j < CIUKI_OPEN_MAX; j++) {
            struct desktop_description *y = desktop_fd(b, (int32_t)j);
            if (y && y->kind == DESKTOP_CHANNEL && y->u.endpoint.channel == c &&
                y->u.endpoint.side != x->u.endpoint.side) return true;
        }
    }
    return false;
}
void channel_release(struct desktop_description *d)
{
    struct channel *c = d->u.endpoint.channel;
    if (!c) return;
    unsigned side = d->u.endpoint.side;
    /* Keep this side open while reclaim releases the unaccepted peer: its
     * final release must not free the channel underneath this release. */
    if (!side) channel_security_reclaim(c);
    uint32_t f = irq_save();
    struct task *receiver = channel_receiver(c, side);
    if (receiver && receiver->focused) task_focus(0);
    irq_restore(f);
    c->open[side] = false;
    struct channel_message *m = c->inbox[side].head;
    while (m) {
        struct channel_message *next = m->next;
        channel_message_free(m);
        desktop_objects.messages--;
        m = next;
    }
    c->inbox[side] = (struct channel_queue){ 0 };
    kwait_wake_all(&c->changed[0]);
    kwait_wake_all(&c->changed[1]);
    if (net_fd_changed) net_fd_changed(0, 0);
    if (c->service) channel_security_discard(c);
    if (!c->open[side ^ 1]) {
        struct channel **at=&security_channels;
        while (*at && *at!=c) at=&(*at)->security_next;
        if (*at) *at=c->security_next;
        desktop_objects.channels--;
        security_subject_put(c->subjects[0]);
        security_subject_put(c->subjects[1]);
        kfree(c);
    }
}

int channel_pair(struct process *p, int32_t fds[2])
{
    if (!security_process_live(p)) return -EACCES;
    if (endpoint_high_water==UINT32_MAX) return -EAGAIN;
    int a = desktop_fd_slot(p, 0);
    int b = a < 0 ? a : desktop_fd_slot(p, (unsigned)a + 1);
    if (b < 0) return -EMFILE;
    if (desktop_objects.descriptions + FILE_OBJECTS > CIUKI_OPEN_DESCRIPTION_MAX - 2) return -ENFILE;
    struct channel *c = kzalloc(sizeof(*c));
    if (!c) return -ENOMEM;
    struct desktop_description *ends[2] = { 0 };
    for (unsigned i = 0; i < 2; i++) {
        ends[i] = desktop_description_new(DESKTOP_CHANNEL, O_RDWR);
        if (!ends[i]) {
            if (ends[0]) ends[0]->object.release(&ends[0]->object);
            kfree(c);
            return -ENOMEM;
        }
    }
    int err = security_process_live(p) ? security_subject_get(p->subject) : -EACCES;
    if (!err) {
        err = security_subject_get(p->subject);
        if (err) security_subject_put(p->subject);
    }
    if (err) {
        ends[0]->object.release(&ends[0]->object);
        ends[1]->object.release(&ends[1]->object);
        kfree(c);
        return err;
    }
    if (endpoint_high_water==UINT32_MAX) {
        security_subject_put(p->subject); security_subject_put(p->subject);
        ends[0]->object.release(&ends[0]->object); ends[1]->object.release(&ends[1]->object);
        kfree(c); return -EAGAIN;
    }
    uint32_t generation=++endpoint_high_water;
    for (unsigned i = 0; i < 2; i++) {
        ends[i]->u.endpoint.channel = c;
        ends[i]->u.endpoint.side = i;
        c->open[i] = true;
        c->subjects[i] = p->subject;
        c->generation[i] = generation;
        kwait_init(&c->changed[i]);
    }
    c->security_next=security_channels; security_channels=c;
    desktop_objects.channels++;
    p->fds[a] = (struct proc_fd){ &ends[0]->object, FD_CLOEXEC };
    p->fds[b] = (struct proc_fd){ &ends[1]->object, FD_CLOEXEC };
    fds[0] = a; fds[1] = b;
    return 0;
}

static void channel_cleanup(void *arg)
{
    struct channel_operation *op = arg;
    if (op->pinned) ua_unpin(op->process->memory, &op->pin);
    channel_message_free(op->message);
    op->endpoint->object.release(&op->endpoint->object);
    kfree(op);
}

static struct channel_operation *channel_begin(struct process *p, struct desktop_description *d)
{
    struct channel_operation *op = kzalloc(sizeof(*op));
    if (!op) return 0;
    op->endpoint = d;
    op->process = p;
    d->object.retain(&d->object);
    struct proc_thread *t = proc_thread_for(g_current);
    if (t) { t->operation = op; t->cleanup = channel_cleanup; }
    return op;
}

static int channel_end(struct channel_operation *op, int rc)
{
    struct proc_thread *t = proc_thread_for(g_current);
    if (t) { t->operation = 0; t->cleanup = 0; }
    channel_cleanup(op);
    return rc;
}

static bool channel_interrupted(struct process *p)
{
    struct proc_thread *t = proc_thread_for(g_current);
    return p->state == PROC_STOPPING || (t && proc_signal_caught(t));
}

static bool channel_send_ready(void *arg)
{
    struct channel_operation *op = arg;
    struct channel *c = op->endpoint->u.endpoint.channel;
    unsigned peer = op->endpoint->u.endpoint.side ^ 1;
    return c->revoked[0] || c->revoked[1] || !c->open[peer] || c->inbox[peer].count < CIUKI_CHANNEL_QUEUE_MAX ||
        channel_interrupted(op->process);
}

int channel_send(struct process *p, int32_t fd, uint32_t message, uint32_t flags)
{
    struct desktop_description *d = desktop_fd(p, fd);
    if (!d || d->kind != DESKTOP_CHANNEL) return -EBADF;
    if (!security_process_live(p) || !security_subject_matches(d->u.endpoint.channel->subjects[d->u.endpoint.side], p)) return -EACCES;
    if (flags & ~(DONTWAIT | CIUKI_CHANNEL_FOCUS | CIUKI_CHANNEL_UNFOCUS | CIUKI_CHANNEL_INPUT)) return -EINVAL;
    unsigned hints = flags & ~DONTWAIT;
    if ((hints & CIUKI_CHANNEL_FOCUS) && (hints & CIUKI_CHANNEL_UNFOCUS)) return -EINVAL;
    if (hints && !channel_ui_authority(p)) return -EPERM;
    struct channel_operation *op = channel_begin(p, d);
    if (!op) return -ENOMEM;
    op->message = kzalloc(sizeof(*op->message));
    if (!op->message) return channel_end(op, -ENOMEM);
    struct ciuki_message *w = &op->message->wire;
    int err = copy_from_user(w, message, sizeof(*w));
    if (err) return channel_end(op, err);
    if (w->length > CIUKI_MESSAGE_PAYLOAD_MAX || w->fd_count > CIUKI_MESSAGE_FD_MAX)
        return channel_end(op, -EMSGSIZE);
    if (w->sender_pid || w->reserved) return channel_end(op, -EINVAL);
    if (d->u.endpoint.channel->service && (w->fd_count || hints)) return channel_end(op,-EPERM);
    if (d->u.endpoint.channel->readiness && (d->u.endpoint.side!=1 || w->fd_count || hints))
        return channel_end(op,-EPERM);
    for (unsigned i = w->length; i < sizeof(w->data); i++)
        if (w->data[i]) return channel_end(op, -EINVAL);
    for (unsigned i = w->fd_count; i < CIUKI_MESSAGE_FD_MAX; i++)
        if (w->fds[i]) return channel_end(op, -EINVAL);
    for (unsigned i = 0; i < w->fd_count; i++) {
        err = surface_attachment(p, w->fds[i], &op->message->attachments[i]);
        if (err) return channel_end(op, err);
        w->fds[i] = 0;
    }
    w->sender_pid = p->pid;
    struct channel *c = d->u.endpoint.channel;
    unsigned peer = d->u.endpoint.side ^ 1;
    for (;;) {
        if (!security_process_live(p)) return channel_end(op, -EACCES);
        channel_security_timeout(c);
        if (c->revoked[d->u.endpoint.side] || (c->service && c->revoked[peer])) return channel_end(op,-ESRCH);
        if (p->state == PROC_STOPPING) return channel_end(op, -EINTR);
        if (!c->open[peer] || c->revoked[peer]) {
            struct proc_thread *t = proc_thread_for(g_current);
            if (t) proc_signal_pipe(t);
            return channel_end(op, -EPIPE);
        }
        struct channel_queue *q = &c->inbox[peer];
        if (c->readiness && c->ready_sent) return channel_end(op,-EINVAL);
        if (c->service) {
            err=channel_security_frame(c,d->u.endpoint.side,w);
            if (!err) {
                kwait_wake_all(&c->changed[peer]);
                if (net_fd_changed) net_fd_changed(0,0);
                /* sec-own: PID 1 serves OWNER in its maintenance turn; wake
                 * it for a complete request instead of the 100-tick poll. */
                if (c->service==CIUKI_SECURITY_SERVICE_OWNER && !d->u.endpoint.side && c->complete[0] && proc_notify) proc_notify();
            }
            return channel_end(op,err);
        }
        if (q->count < CIUKI_CHANNEL_QUEUE_MAX) {
            if (q->tail) q->tail->next = op->message;
            else q->head = op->message;
            q->tail = op->message;
            q->count++;
            if (c->readiness) c->ready_sent=true;
            desktop_objects.messages++;
            /* The successful enqueue is delivery; failed/full queues and
             * polling never grant a boost. Identify the endpoint's UI thread,
             * not an arbitrary first thread in its process. */
            uint32_t f = irq_save();
            c->receiver[d->u.endpoint.side].id = g_current->id;
            c->receiver[d->u.endpoint.side].generation = g_current->generation;
            struct task *receiver = channel_receiver(c, peer);
            if (receiver && (hints & CIUKI_CHANNEL_FOCUS)) task_focus(receiver);
            if (receiver && (hints & CIUKI_CHANNEL_UNFOCUS) && receiver->focused) task_focus(g_current);
            if (hints & CIUKI_CHANNEL_INPUT) task_input_delivered(receiver);
            irq_restore(f);
            kwait_wake_all(&c->changed[peer]);
            if (net_fd_changed) net_fd_changed(0, 0);
            op->message = 0;
            return channel_end(op, 0);
        }
        if ((flags & DONTWAIT) || (d->flags & O_NONBLOCK)) return channel_end(op, -EAGAIN);
        if (channel_interrupted(p)) return channel_end(op, -EINTR);
        kwait_wait_until(&c->changed[peer], channel_send_ready, op, UINT64_MAX);
    }
}

struct channel_receive_operation {
    struct desktop_description *endpoint;
    struct process *process;
    struct ua_pin *pin;
};
static void channel_receive_cleanup(void *arg)
{
    struct channel_receive_operation *op = arg;
    ua_unpin(op->process->memory, op->pin);
    op->endpoint->object.release(&op->endpoint->object);
}

static bool channel_receive_ready(void *arg)
{
    struct channel_receive_operation *op = arg;
    struct channel *c = op->endpoint->u.endpoint.channel;
    unsigned side = op->endpoint->u.endpoint.side;
    channel_security_timeout(c);
    return (c->service ? channel_security_readable(c,side) : c->inbox[side].head!=0) ||
        c->revoked[0] || c->revoked[1] || !c->open[side ^ 1] || channel_interrupted(op->process);
}

int channel_recv(struct process *p, int32_t fd, uint32_t message, uint32_t flags)
{
    struct desktop_description *d = desktop_fd(p, fd);
    if (!d || d->kind != DESKTOP_CHANNEL) return -EBADF;
    if (!security_process_live(p) || !security_subject_matches(d->u.endpoint.channel->subjects[d->u.endpoint.side], p)) return -EACCES;
    if (flags & ~DONTWAIT) return -EINVAL;
    /* The syscall contract has no ENOMEM result for receive. A small
     * operation lives on the retained kernel stack, without allocation. */
    struct ua_pin pin;
    int err = ua_pin(p->memory, &pin, message, sizeof(struct ciuki_message), true);
    if (err) return err;
    d->object.retain(&d->object);
    struct channel_receive_operation op = { .endpoint = d, .process = p, .pin = &pin };
    /* See stack cleanup below: a killed thread's stack remains until the
     * process collector invokes cleanup, before cancelling user pins. */
    struct proc_thread *t = proc_thread_for(g_current);
    if (t) { t->operation = &op; t->cleanup = channel_receive_cleanup; }
    struct channel *c = d->u.endpoint.channel;
    unsigned side = d->u.endpoint.side;
    for (;;) {
        if (!security_process_live(p)) { err = -EACCES; break; }
        channel_security_timeout(c);
        if (c->revoked[side] || (c->service && c->revoked[side^1])) { err=-ESRCH; break; }
        if (p->state == PROC_STOPPING) { err = -EINTR; break; }
        if (c->service && side==1 && !c->accepted) { err=-EAGAIN; break; }
        if (c->service && channel_security_readable(c,side)) {
            struct ciuki_message out;
            channel_security_message(c,side,&out);
            err=copy_to_user(message,&out,sizeof(out));
            if (!err) {
                /* Pinned copy cannot yield: first server copy and CANCEL
                 * serialize at this transport admission boundary. */
                if (side==1) c->admitted=true;
                c->delivered[side]+=out.length-64;
                c->drained[side]=c->delivered[side]==c->received[side^1];
                if (side==1 && c->drained[side]) for (unsigned page=0;page<2;page++) {
                    if (c->body_pages[0][page]) pmm_free(c->body_pages[0][page]);
                    c->body_pages[0][page]=0;
                }
                if (side==0 && c->drained[side]) channel_security_discard(c);
                kwait_wake_all(&c->changed[side]);
                if (net_fd_changed) net_fd_changed(0,0);
                err=1;
            }
            break;
        }
        struct channel_queue *q = &c->inbox[side];
        struct channel_message *m = (!c->service || c->complete[side^1]) ? q->head : 0;
        if (m) {
            struct ciuki_message out = m->wire;
            unsigned next = 0;
            err = 1;
            for (unsigned i = 0; i < out.fd_count; i++) {
                int slot = desktop_fd_slot(p, next);
                if (slot < 0) { err = -EMFILE; break; }
                out.fds[i] = slot;
                next = (unsigned)slot + 1;
            }
            if (err > 0) {
                err = copy_to_user(message, &out, sizeof(out));
                if (!err) {
                    c->receiver[side].id = g_current->id;
                    c->receiver[side].generation = g_current->generation;
                    for (unsigned i = 0; i < out.fd_count; i++) {
                        p->fds[out.fds[i]] = (struct proc_fd){ &m->attachments[i]->object, FD_CLOEXEC };
                        m->attachments[i] = 0;
                    }
                    q->head = m->next;
                    if (!q->head) q->tail = 0;
                    q->count--; desktop_objects.messages--;
                    if (c->service && side==1) c->admitted=true;
                    if (c->service && side==0 && !q->count) channel_security_discard(c);
                    kwait_wake_all(&c->changed[side]);
                    if (net_fd_changed) net_fd_changed(0, 0);
                    channel_message_free(m);
                    err = 1;
                }
            }
            break;
        }
        /* Ordinary channels retain their byte/message drain-to-EOF contract.
         * Authentication leases are already revoked; service channels above
         * discard unadmitted work/replies and never drain after revocation. */
        if (!c->open[side ^ 1] || c->revoked[side^1]) { err = 0; break; }
        if ((flags & DONTWAIT) || (d->flags & O_NONBLOCK)) { err = -EAGAIN; break; }
        if (channel_interrupted(p)) { err = -EINTR; break; }
        kwait_wait_until(&c->changed[side], channel_receive_ready, &op, UINT64_MAX);
    }
    if (t) { t->operation = 0; t->cleanup = 0; }
    channel_receive_cleanup(&op);
    return err;
}

/* Internal service connection: reuse duplex queues and description limits;
 * move the peer into the server without exposing a cross-process F2 syscall. */
int channel_connect(struct process *client, struct process *server, int32_t fds[2])
{
    if (!security_process_live(client) || !security_process_live(server)) return -EACCES;
    if (!security_same(client, server) && !security_system(client) && !security_system(server)) return -EACCES;
    int slot=desktop_fd_slot(server,0);
    if(slot<0)return -EMFILE;
    int err=channel_pair(client,fds);if(err)return err;
    /* channel_pair allocates and can reach a voluntary point. Recheck the
     * destination and both lifetimes before publishing the transferred end. */
    slot=desktop_fd_slot(server,0);
    if(slot<0 || client->state==PROC_STOPPING || server->state!=PROC_LIVE ||
       !security_process_live(client) || !security_process_live(server)){
        desktop_close(client,fds[0]);desktop_close(client,fds[1]);return slot<0?-EMFILE:-EINTR;
    }
    struct desktop_description *end = desktop_fd(client, fds[1]);
    struct channel *c = end->u.endpoint.channel;
    if ((err = security_subject_get(server->subject))) {
        desktop_close(client, fds[0]); desktop_close(client, fds[1]); return err;
    }
    security_subject_put(c->subjects[1]);
    c->subjects[1] = server->subject;
    c->handed[1] = true;
    server->fds[slot]=client->fds[fds[1]];
    client->fds[fds[1]]=(struct proc_fd){0};
    fds[1]=slot;return 0;
}

int channel_peer_get(struct process *p, int32_t fd, struct security_peer *out)
{
    struct desktop_description *d = desktop_fd(p, fd);
    if (!d || d->kind != DESKTOP_CHANNEL) return -EBADF;
    struct channel *c = d->u.endpoint.channel;
    unsigned side = d->u.endpoint.side, peer = side ^ 1;
    if (!security_process_live(p) || !security_subject_matches(c->subjects[side], p)) return -EACCES;
    if (!c->open[peer] || c->revoked[side] || c->revoked[peer] || !security_subject_live(c->subjects[peer])) return -ESRCH;
    int err = security_subject_get(c->subjects[peer]);
    if (err) return err;
    d->object.retain(&d->object);
    *out = (struct security_peer){ &d->object, c->subjects[peer], c->generation[peer] };
    return 0;
}
bool channel_peer_live(const struct security_peer *p)
{
    if (!p || !p->endpoint || !p->subject) return false;
    const struct desktop_description *d = (const struct desktop_description *)p->endpoint;
    struct channel *c = d->u.endpoint.channel;
    unsigned side = d->u.endpoint.side, peer = side ^ 1;
    return c->open[peer] && !c->revoked[side] && !c->revoked[peer] &&
        c->generation[peer] == p->generation && c->subjects[peer] == p->subject && security_subject_live(p->subject);
}
void channel_peer_put(struct security_peer *p)
{
    if (!p) return;
    security_subject_put(p->subject);
    if (p->endpoint) p->endpoint->release(p->endpoint);
    *p = (struct security_peer){0};
}
/* sec-wire (QEMU run of image ae977fbe: "Files: desktop connection failed:
 * No such process"): the desktop launches a program as POSIX does, a pair,
 * a spawn that maps the client end to the child's fd 3, then close() of
 * its own copy (apps/desktop/desktop.c launch_program). POSIX close(2)
 * releases only that descriptor; the open description lives until the
 * last descriptor referring to it is closed. An ordinary endpoint reaches
 * another process only through same-credential inheritance (proc_inherit
 * requires security_same, channel_process_prepare checks the bound
 * credential; attachments carry surfaces only), and inheritance keeps the
 * established peer (user-security-kernel.md: "Dup/inheritance do not change
 * its peer"). So the bound subject's close does not revoke an ordinary side
 * while a live process with that credential still holds it; the side then
 * closes at the final release (channel_release) or with the subject's exit
 * (channel_security_revoke). Service links keep revocation at close. */
/* Weak: channel host harnesses without the process table keep the plain rule. */
#pragma weak proc_reserved_at_slot
static bool channel_endpoint_inherited(const struct process *closing, const struct desktop_description *d,
                                       const struct security_subject *bound)
{
    if (d->references <= 1 || !proc_reserved_at_slot) return false; /* only the closing descriptor remains */
    for (unsigned slot = 0; slot < CIUKI_PROCESS_MAX; slot++) {
        const struct process *q = proc_reserved_at_slot(slot);
        if (!q || q == closing || !q->fds || !security_process_live(q) || !security_subject_matches(bound, q)) continue;
        for (unsigned fd = 0; fd < CIUKI_OPEN_MAX; fd++)
            if (q->fds[fd].object == &d->object) return true;
    }
    return false;
}
void channel_fd_closed(struct process *p, struct proc_object *object)
{
    /* Called after detaching the slot. A duplicate still owns the endpoint. */
    for (unsigned i = 0; i < CIUKI_OPEN_MAX; i++)
        if (p->fds[i].object == object) return;
    struct desktop_description *d = 0;
    /* Object may be a file/socket; never inspect a foreign object layout. */
    if (!desktop_object_is_channel(object)) return;
    d = (struct desktop_description *)object;
    if (d->kind == DESKTOP_CHANNEL) {
        struct channel *c = d->u.endpoint.channel;
        unsigned side = d->u.endpoint.side;
        if (c->subjects[side] == p->subject &&
            (c->service || !channel_endpoint_inherited(p, d, c->subjects[side]))) {
            c->revoked[side] = true;
            if (c->service) channel_security_discard(c);
            kwait_wake_all(&c->changed[0]); kwait_wake_all(&c->changed[1]);
            channel_security_reclaim(c);
        }
    }
}
/* Inheritance preserves the established peer and generation. Only an explicit
 * controller handoff may replace an initially unbound endpoint identity. */
int channel_process_prepare(struct process *child)
{
    for (unsigned fd = 0; fd < CIUKI_OPEN_MAX; fd++) {
        struct desktop_description *d = desktop_fd(child, (int32_t)fd);
        if (!d || d->kind != DESKTOP_CHANNEL) continue;
        struct channel *c = d->u.endpoint.channel;
        unsigned side = d->u.endpoint.side;
        if (!security_subject_matches(c->subjects[side], child) || c->revoked[side]) return -EACCES;
    }
    return 0;
}
void channel_process_publish(struct process *child) { (void)child; }
void channel_process_abort(struct process *child) { (void)child; }

/* Readiness is observed before poll atomically registers/rechecks its global
 * wake generation; channel waits retain their existing queue predicates. */
int channel_poll_events(struct desktop_description *d)
{
    struct channel *c = d->u.endpoint.channel; unsigned side = d->u.endpoint.side;
    channel_security_timeout(c);
    bool revoked=c->revoked[0] || c->revoked[1];
    int events = (c->service ? channel_security_readable(c,side) : c->inbox[side].count!=0) || !c->open[side ^ 1] ? POLLIN : 0;
    if (!revoked && c->open[side ^ 1] && c->inbox[side ^ 1].count < CIUKI_CHANNEL_QUEUE_MAX) events |= POLLOUT;
    if (!c->open[side ^ 1] || revoked) events |= POLLHUP;
    return events;
}

/* Initial controller-installed handoff; ordinary inheritance never calls it.
 * No delivery/old request can survive the initial binding change. */
int channel_endpoint_handoff(struct process *parent, int32_t fd, struct process *child,
                             int32_t *child_fd, bool restricted)
{
    struct desktop_description *d=desktop_fd(parent,fd);
    if (!d || d->kind!=DESKTOP_CHANNEL) return -EBADF;
    struct channel *c=d->u.endpoint.channel; unsigned side=d->u.endpoint.side;
    if (!security_system(parent) || child->published || child->state!=PROC_PREPARING ||
        c->subjects[side]!=parent->subject || c->handed[side] || c->service ||
        c->inbox[0].head || c->inbox[1].head || c->revoked[0] || c->revoked[1]) return -EPERM;
    int slot=desktop_fd_slot(child,0);
    if (slot<0) return slot;
    int err=security_subject_get(child->subject);
    if (err) return err;
    security_subject_put(c->subjects[side]); c->subjects[side]=child->subject;
    c->handed[side]=true;
    if (restricted) d->object.inheritable=false;
    child->fds[slot]=parent->fds[fd]; parent->fds[fd]=(struct proc_fd){0};
    *child_fd=slot;
    return 0;
}
/* The readiness coordinator is kernel-owned. A LOGON instance owns its fd,
 * while its death cannot discard a child's authenticated ready handshake.
 * Only this initial, unpublished binding and fixed-role recovery can install
 * the endpoint; it never travels through inheritance or attachments. */
int channel_control_bind(struct process *parent, int32_t fd)
{
    struct desktop_description *d=desktop_fd(parent,fd);
    if (!security_logon(parent) || !d || d->kind!=DESKTOP_CHANNEL) return -EPERM;
    struct channel *c=d->u.endpoint.channel;
    if (d->u.endpoint.side || c->service || c->subjects[0]!=parent->subject ||
        c->inbox[0].head || c->inbox[1].head || !c->handed[1] || c->handed[0]) return -EINVAL;
    int err=security_subject_get(proc_supervisor()->subject);
    if (err) return err;
    security_subject_put(c->subjects[0]); c->subjects[0]=proc_supervisor()->subject;
    c->handed[0]=true; c->readiness=true; d->object.inheritable=false;
    return 0;
}
int channel_control_install(struct process *p, struct proc_object *object)
{
    if (!security_logon(p) || !desktop_object_is_channel(object)) return -EPERM;
    struct desktop_description *d=(struct desktop_description *)object;
    struct channel *c=d->u.endpoint.channel;
    if (d->u.endpoint.side || c->service || c->subjects[0]!=proc_supervisor()->subject ||
        !c->handed[0] || c->revoked[0] || c->revoked[1]) return -ESRCH;
    int fd=desktop_fd_slot(p,0);
    if (fd<0) return fd;
    object->retain(object); p->fds[fd]=(struct proc_fd){object,FD_CLOEXEC};
    return fd;
}
static void channel_security_discard(struct channel *c)
{
    for (unsigned side=0;side<2;side++) {
        for (unsigned page=0;page<2;page++) {
            if (c->body_pages[side][page]) pmm_free(c->body_pages[side][page]);
            c->body_pages[side][page]=0;
        }
        struct channel_message *m=c->inbox[side].head;
        c->inbox[side]=(struct channel_queue){0};
        while (m) {
            struct channel_message *next=m->next;
            channel_message_free(m); desktop_objects.messages--; m=next;
        }
    }
    if (c->exchange) { c->exchange=false; service_exchanges--; }
    c->complete[0]=c->complete[1]=false;
    c->received[0]=c->received[1]=0;
    c->delivered[0]=c->delivered[1]=0;
    c->drained[0]=c->drained[1]=false;
    c->replying=false;
}
static void channel_security_timeout(struct channel *c)
{
    if (!c->service || !c->exchange) return;
    if (!security_subject_live(c->subjects[0]) || !security_subject_live(c->subjects[1]) ||
        ((!c->admitted || (c->replying && !c->complete[1])) && g_ticks>=c->deadline)) {
        channel_security_discard(c); c->revoked[0]=c->revoked[1]=true;
        kwait_wake_all(&c->changed[0]); kwait_wake_all(&c->changed[1]);
    }
}
static int channel_security_server_fd(const struct channel *c, const struct process *p)
{
    for (unsigned fd=0;fd<CIUKI_OPEN_MAX;fd++) {
        struct desktop_description *d=desktop_fd(p,(int32_t)fd);
        if (d && d->kind==DESKTOP_CHANNEL && d->u.endpoint.channel==c && d->u.endpoint.side==1)
            return (int)fd;
    }
    return -EBADF;
}
static void channel_security_reclaim(struct channel *c)
{
    if (!c->service || c->accepted) return;
    /* OWNER is the kernel supervisor, outside the published process table. */
    struct process *server=proc_supervisor();
    if (server->subject!=c->subjects[1]) server=proc_find(security_subject_pid(c->subjects[1]));
    if (!server || server->subject!=c->subjects[1]) return;
    int fd=channel_security_server_fd(c,server);
    if (fd<0) return;
    struct proc_object *endpoint=server->fds[fd].object;
    /* Hold the exact object across duplicate detachment and recursive close
     * hooks. A reused original fd belongs to somebody else. No allocation,
     * user copy or voluntary scheduling point occurs in this cleanup. */
    endpoint->retain(endpoint);
    for (unsigned i=0;i<CIUKI_OPEN_MAX;i++)
        if (server->fds[i].object==endpoint) desktop_close(server,(int32_t)i);
    endpoint->release(endpoint); /* May free c; callers must own an end or stop using c. */
}
void channel_security_revoke(struct process *p)
{
    for (struct channel *c=security_channels,*next;c;c=next) {
        next=c->security_next;
        for (unsigned side=0;side<2;side++) if (c->subjects[side]==p->subject) {
            c->revoked[side]=true;
            if (c->service) channel_security_discard(c);
            kwait_wake_all(&c->changed[0]); kwait_wake_all(&c->changed[1]);
        }
        if (c->revoked[0] || c->revoked[1]) channel_security_reclaim(c);
    }
}
static int channel_security_frame(struct channel *c, unsigned side, const struct ciuki_message *m)
{
    uint32_t h[16];
    if (m->fd_count || m->length<64) return -EINVAL;
    memcpy(h,m->data,64);
    uint64_t id=(uint64_t)h[6]|(uint64_t)h[7]<<32;
    if (h[0]!=0x31435343 || h[1]!=1 || h[2]!=64 || h[3]!=c->service ||
        h[5]!=(side ? 2u : 1u) || !id || h[8]!=c->generation[side] ||
        h[9]>192 || m->length!=64+h[9] || h[13]>8192 ||
        h[14]>h[13] || h[9]>h[13]-h[14] || h[15] ||
        (!h[9] && h[13]) || (int32_t)h[12]>0 || (!side && h[12]) || (h[12] && h[13])) return -EINVAL;
    /* sec-own: OWNER (service 3) adds the query operations 8..14. */
    static const uint32_t max_op[]={0,7,9,14,0xffffffffu,7,0xffffffffu};
    if (!h[4] || h[4]>max_op[c->service]) return -EOPNOTSUPP;
    bool first=!c->exchange;
    if (first) {
        if (side || h[14] || id<=c->last_request_id) return -EINVAL;
        if (service_exchanges>=CIUKI_SECURITY_SERVICE_PENDING_MAX) return -EAGAIN;
    } else {
        if (id!=c->request_id) return -EBUSY;
        if (c->complete[side]) return -EINVAL;
        if (side && (!c->admitted || !c->complete[0] || !c->drained[1])) return -EAGAIN;
        uint32_t *base=side ? c->reply_header : c->frame_header;
        if (!side || c->replying) {
            for (unsigned i=0;i<16;i++)
                if (i!=9 && i!=14 && h[i]!=base[i]) return -EINVAL;
        } else if (h[4]!=c->frame_header[4] || h[14]) return -EINVAL;
    }
    if (h[14]!=c->received[side]) return -EINVAL;
    /* Reassemble at most two pages per exchange. The request must drain and
     * release its pages before reply preparation. An allocation per
     * fragment would turn an 8192-byte body into 8192 queued descriptions for
     * one-byte fragments and defeat the frozen memory bound. */
    uint32_t pages[2]={0};
    if (first || (side && !c->replying)) {
        for (unsigned i=0;i<(h[13]+PAGE_SIZE-1)/PAGE_SIZE;i++) {
            pages[i]=pmm_alloc();
            if (!pages[i]) {
                for (unsigned j=0;j<i;j++) pmm_free(pages[j]);
                return -ENOMEM;
            }
        }
        if (c->revoked[0] || c->revoked[1] || !security_subject_live(c->subjects[0]) ||
            !security_subject_live(c->subjects[1])) {
            for (unsigned i=0;i<2;i++) if (pages[i]) pmm_free(pages[i]);
            return -ESRCH;
        }
        memcpy(c->body_pages[side],pages,sizeof(pages));
    }
    if (first) {
        c->exchange=true; service_exchanges++; c->admitted=false;
        c->request_id=c->last_request_id=id;
        c->deadline=g_ticks+CIUKI_SECURITY_SERVICE_TIMEOUT_MS;
        memcpy(c->frame_header,h,64);
    }
    if (side && !c->replying) {
        memcpy(c->reply_header,h,64); c->replying=true;
        c->deadline=g_ticks+CIUKI_SECURITY_SERVICE_TIMEOUT_MS;
    }
    for (unsigned i=0;i<h[9];i++) {
        uint32_t at=h[14]+i;
        ((uint8_t *)P2V(c->body_pages[side][at/PAGE_SIZE]))[at%PAGE_SIZE]=m->data[64+i];
    }
    c->received[side]+=h[9];
    if (c->received[side]==h[13]) c->complete[side]=true;
    return 0;
}
static void channel_security_message(struct channel *c, unsigned side, struct ciuki_message *out)
{
    unsigned sender=side^1;
    uint32_t header[16]; memcpy(header,sender ? c->reply_header : c->frame_header,sizeof(header));
    uint32_t offset=c->delivered[side],bytes=c->received[sender]-offset;
    if (bytes>CIUKI_SECURITY_SERVICE_FRAGMENT_MAX) bytes=CIUKI_SECURITY_SERVICE_FRAGMENT_MAX;
    header[9]=bytes; header[14]=offset;
    *out=(struct ciuki_message){.length=64+bytes,.sender_pid=security_subject_pid(c->subjects[sender])};
    memcpy(out->data,header,sizeof(header));
    for (unsigned i=0;i<bytes;i++) {
        uint32_t at=offset+i;
        out->data[64+i]=((const uint8_t *)P2V(c->body_pages[sender][at/PAGE_SIZE]))[at%PAGE_SIZE];
    }
}
static void channel_link_output(struct channel *c, unsigned side, int32_t fd, struct ciuki_security_link *out)
{
    struct security_subject *peer=c->subjects[side^1];
    *out=(struct ciuki_security_link){.version=1,.size=64,.service=c->service,
        .endpoint_fd=fd,.endpoint_generation=c->generation[side],
        .server_role=security_subject_role(c->subjects[1]),.peer_pid=security_subject_pid(peer),
        .peer_process_generation=security_subject_process_generation(peer),.peer_role=security_subject_role(peer),
        .session_id=security_subject_credential(c->subjects[0])->session_id,.service_generation=c->service_generation};
}
int channel_security_link(struct process *p, uint32_t operation, const struct ciuki_security_link *in,
                          struct ciuki_security_link *out, uint32_t *result)
{
    if (operation==CIUKI_SECURITY_SERVICE_CANCEL) {
        struct desktop_description *d=desktop_fd(p,in->endpoint_fd);
        if (!d || d->kind!=DESKTOP_CHANNEL) return -EBADF;
        struct channel *c=d->u.endpoint.channel; unsigned side=d->u.endpoint.side;
        if (!c->service || c->service!=in->service || side ||
            c->subjects[0]!=p->subject) return -EACCES;
        if (c->generation[0]!=in->endpoint_generation) return -EAGAIN;
        uint64_t id=(uint64_t)in->request_id_lo|((uint64_t)in->request_id_hi<<32);
        channel_security_timeout(c);
        *result=c->exchange && c->request_id==id && !c->admitted && !c->revoked[0] && !c->revoked[1];
        /* Also drop late replies for admitted/unknown cancellation. An admitted
         * worker's retained subject revokes and its durable protocol recovers. */
        channel_security_discard(c); c->revoked[0]=c->revoked[1]=true;
        kwait_wake_all(&c->changed[0]); kwait_wake_all(&c->changed[1]);
        channel_security_reclaim(c);
        return 0;
    }
    if (operation==CIUKI_SECURITY_SERVICE_ACCEPT) {
        const struct ciuki_credential *cred=security_credential(p);
        struct process *server=security_service_server(in->service,cred ? cred->session_id : 0);
        if (server!=p) return -EPERM;
        for (struct channel *c=security_channels;c;c=c->security_next) {
            if (c->service!=in->service || c->accepted || c->subjects[1]!=p->subject ||
                c->revoked[0] || c->revoked[1] || !security_subject_live(c->subjects[0])) continue;
            int fd=channel_security_server_fd(c,p);
            if (fd<0) continue;
            c->accepted=true; channel_link_output(c,1,fd,out); return 0;
        }
        return -EAGAIN;
    }
    int err=security_service_allowed(p,in->service);
    if (err) return err;
    const struct ciuki_credential *cred=security_credential(p);
    struct process *server=security_service_server(in->service,cred->session_id);
    if (!server) return -ESRCH;
    uint32_t generation=security_role_generation(server);
    int32_t fds[2]; err=channel_connect(p,server,fds);
    if (err) return err;
    if (security_service_server(in->service,cred->session_id)!=server ||
        security_role_generation(server)!=generation || security_service_allowed(p,in->service)) {
        desktop_close(p,fds[0]); desktop_close(server,fds[1]); return -EAGAIN;
    }
    struct desktop_description *d=desktop_fd(p,fds[0]);
    struct channel *c=d->u.endpoint.channel;
    c->service=in->service; c->service_generation=generation;
    d->object.inheritable=false; desktop_fd(server,fds[1])->object.inheritable=false;
    channel_link_output(c,0,fds[0],out);
    return 0;
}
/* Private readiness dequeue. It consumes only the exact retained child end,
 * with no user pointers, copied PID lookup, attachments or scheduling point. */
int channel_kernel_receive(struct process *p, int32_t fd, struct ciuki_message *out)
{
    struct desktop_description *d=desktop_fd(p,fd);
    if (!d || d->kind!=DESKTOP_CHANNEL) return -EBADF;
    struct channel *c=d->u.endpoint.channel; unsigned side=d->u.endpoint.side;
    if (!security_logon(p) || !security_subject_matches(c->subjects[side],p) || c->revoked[0] || c->revoked[1] ||
        !security_subject_live(c->subjects[side^1])) return -ESRCH;
    struct channel_queue *q=&c->inbox[side]; struct channel_message *m=q->head;
    if (!m) return -EAGAIN;
    if (m->wire.fd_count) return -EINVAL;
    *out=m->wire; q->head=m->next; if (!q->head) q->tail=0;
    q->count--; desktop_objects.messages--; channel_message_free(m);
    return 1;
}

void channel_security_poll(void)
{
    for (struct channel *c=security_channels,*next;c;c=next) {
        next=c->security_next;
        channel_security_timeout(c);
        if (c->revoked[0] || c->revoked[1]) channel_security_reclaim(c);
    }
}
uint32_t channel_peer_service(const struct security_peer *peer)
{
    if (!channel_peer_live(peer)) return 0;
    const struct desktop_description *d=(const struct desktop_description *)peer->endpoint;
    struct channel *c=d->u.endpoint.channel;
    return c->accepted && d->u.endpoint.side==1 ? c->service : 0;
}

/* PID 1 is the registered OWNER endpoint. This private dequeue uses the
 * production frame reassembly, charges and cancellation admission boundary;
 * no SYSTEM user process can use it as an unframed forwarding route. */
bool channel_owner_pending(void)
{
    for(struct channel *c=security_channels;c;c=c->security_next)
        if(c->service==3 && !c->revoked[0] && !c->revoked[1] && channel_security_readable(c,1)) return true;
    return false;
}
void channel_owner_reap(void)
{
    struct process *p=proc_supervisor();
    if(!p || security_role(p)!=CIUKI_SECURITY_ROLE_OWNER) return;
    for(unsigned fd=0;fd<CIUKI_OPEN_MAX;fd++) {
        struct desktop_description *d=desktop_fd(p,(int32_t)fd);
        if(!d || d->kind!=DESKTOP_CHANNEL || d->u.endpoint.side!=1) continue;
        struct channel *c=d->u.endpoint.channel;
        if(c->service!=3 || !c->accepted || (c->open[0] && !c->revoked[0] && !c->revoked[1] && security_subject_live(c->subjects[0]))) continue;
        /* A worker may still retain this exact endpoint. Detach only PID 1's
         * fd; its retained exchange remains valid for durable completion and
         * rejects any late reply through the revoked peer. */
        channel_security_discard(c); c->revoked[0]=c->revoked[1]=true;
        desktop_close(p,(int32_t)fd);
    }
}
int channel_owner_next(struct owner_exchange *exchange,void *body,size_t cap,size_t *bytes)
{
    if(!exchange || !body || !bytes || cap<CIUKI_SECURITY_SERVICE_BODY_MAX) return -EINVAL;
    memset(exchange,0,sizeof(*exchange)); *bytes=0;
    struct process *p=proc_supervisor();
    if(!p || security_role(p)!=CIUKI_SECURITY_ROLE_OWNER || !security_system(p)) return -EPERM;
    for(struct channel *c=security_channels;c;c=c->security_next) {
        if(c->service!=CIUKI_SECURITY_SERVICE_OWNER || c->subjects[1]!=p->subject) continue;
        channel_security_timeout(c);
        if(c->revoked[0] || c->revoked[1] || !channel_security_readable(c,1)) continue;
        int fd=channel_security_server_fd(c,p); if(fd<0) continue;
        c->accepted=true;
        int e=channel_peer_get(p,fd,&exchange->peer); if(e) return e;
        e=security_subject_acquire_endpoint(p,fd,c->generation[1],&exchange->subject);
        if(e || !channel_peer_live(&exchange->peer) || c->revoked[0] || c->revoked[1] || !channel_security_readable(c,1)) {
            channel_owner_done(exchange); return e?e:-ESRCH;
        }
        /* Allocation above may have yielded. The retained endpoint and exact
         * exchange must still be the one admitted; fd numbers are insufficient. */
        struct desktop_description *d=desktop_fd(p,fd);
        if(!d || &d->object!=exchange->peer.endpoint || d->u.endpoint.channel!=c) { channel_owner_done(exchange); return -ESRCH; }
        memcpy(exchange->header,c->frame_header,64); *bytes=c->received[0];
        c->admitted=true;
        for(unsigned i=0;i<c->received[0];i++) ((uint8_t *)body)[i]=((uint8_t *)P2V(c->body_pages[0][i/PAGE_SIZE]))[i%PAGE_SIZE];
        c->delivered[1]=c->received[0]; c->drained[1]=true;
        for(unsigned i=0;i<2;i++) { if(c->body_pages[0][i]) pmm_free(c->body_pages[0][i]); c->body_pages[0][i]=0; }
        return 1;
    }
    return -EAGAIN;
}
int channel_owner_reply(struct owner_exchange *exchange,int status,const void *body,size_t bytes)
{
    if(!exchange || !exchange->subject || status>0 || (status && bytes) || bytes>CIUKI_SECURITY_SERVICE_BODY_MAX || (!body && bytes)) return -EINVAL;
    if(!channel_peer_live(&exchange->peer) || !security_subject_live(exchange->subject)) return -ESRCH;
    struct desktop_description *d=(struct desktop_description *)exchange->peer.endpoint;
    struct channel *c=d->u.endpoint.channel;
    if(d->u.endpoint.side!=1 || c->service!=3 || !c->exchange || !c->admitted || !c->drained[1] ||
       memcmp(c->frame_header,exchange->header,64)) return -ESRCH;
    size_t offset=0;
    do {
        unsigned n=bytes-offset>192?192:(unsigned)(bytes-offset);
        struct ciuki_message message={.length=64+n}; uint32_t h[16]; memcpy(h,exchange->header,64);
        h[5]=2; h[8]=c->generation[1]; h[9]=n; h[12]=(uint32_t)status; h[13]=(uint32_t)bytes; h[14]=(uint32_t)offset;
        memcpy(message.data,h,64); if(n) memcpy(message.data+64,(const uint8_t *)body+offset,n);
        int e=channel_security_frame(c,1,&message);
        if(e) { channel_security_discard(c); c->revoked[0]=c->revoked[1]=true; return e; }
        offset+=n;
    } while(offset<bytes);
    kwait_wake_all(&c->changed[0]); if(net_fd_changed) net_fd_changed(0,0); return 0;
}
void channel_owner_done(struct owner_exchange *exchange)
{
    if(!exchange) return;
    security_subject_put(exchange->subject); channel_peer_put(&exchange->peer); memset(exchange,0,sizeof(*exchange));
}
