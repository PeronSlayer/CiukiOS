/* Bounded duplex queues. Preparation, copyout and publication are separate:
 * bad buffers/full fd tables cannot consume a head message.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
extern uint32_t file_description_count(void) __attribute__((weak));
#define FILE_OBJECTS (file_description_count ? file_description_count() : 0)
#include <ciuki/desktop.h>
#include <ciuki/signal.h>

struct channel_message {
    struct ciuki_message wire;
    struct desktop_description *attachments[CIUKI_MESSAGE_FD_MAX];
    struct channel_message *next;
};
struct channel_queue { struct channel_message *head, *tail; unsigned count; };
struct channel {
    struct channel_queue inbox[2];
    struct kwait changed[2];
    bool open[2];
    struct { uint32_t id, generation; } receiver[2];
};

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

void channel_release(struct desktop_description *d)
{
    struct channel *c = d->u.endpoint.channel;
    if (!c) return;
    unsigned side = d->u.endpoint.side;
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
    if (!c->open[side ^ 1]) {
        desktop_objects.channels--;
        kfree(c);
    }
}

int channel_pair(struct process *p, int32_t fds[2])
{
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
    for (unsigned i = 0; i < 2; i++) {
        ends[i]->u.endpoint.channel = c;
        ends[i]->u.endpoint.side = i;
        c->open[i] = true;
        kwait_init(&c->changed[i]);
    }
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
    return !c->open[peer] || c->inbox[peer].count < CIUKI_CHANNEL_QUEUE_MAX ||
        channel_interrupted(op->process);
}

int channel_send(struct process *p, int32_t fd, uint32_t message, uint32_t flags)
{
    struct desktop_description *d = desktop_fd(p, fd);
    if (!d || d->kind != DESKTOP_CHANNEL) return -EBADF;
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
        if (p->state == PROC_STOPPING) return channel_end(op, -EINTR);
        if (!c->open[peer]) {
            struct proc_thread *t = proc_thread_for(g_current);
            if (t) proc_signal_pipe(t);
            return channel_end(op, -EPIPE);
        }
        struct channel_queue *q = &c->inbox[peer];
        if (q->count < CIUKI_CHANNEL_QUEUE_MAX) {
            if (q->tail) q->tail->next = op->message;
            else q->head = op->message;
            q->tail = op->message;
            q->count++;
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
            op->message = 0;
            return channel_end(op, 0);
        }
        if ((flags & DONTWAIT) || (d->flags & O_NONBLOCK)) return channel_end(op, -EAGAIN);
        if (channel_interrupted(p)) return channel_end(op, -EINTR);
        kwait_wait_until(&c->changed[peer], channel_send_ready, op, g_ticks + 1);
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
    return c->inbox[side].head || !c->open[side ^ 1] || channel_interrupted(op->process);
}

int channel_recv(struct process *p, int32_t fd, uint32_t message, uint32_t flags)
{
    struct desktop_description *d = desktop_fd(p, fd);
    if (!d || d->kind != DESKTOP_CHANNEL) return -EBADF;
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
        if (p->state == PROC_STOPPING) { err = -EINTR; break; }
        struct channel_queue *q = &c->inbox[side];
        struct channel_message *m = q->head;
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
                    kwait_wake_all(&c->changed[side]);
                    channel_message_free(m);
                    err = 1;
                }
            }
            break;
        }
        if (!c->open[side ^ 1]) { err = 0; break; }
        if ((flags & DONTWAIT) || (d->flags & O_NONBLOCK)) { err = -EAGAIN; break; }
        if (channel_interrupted(p)) { err = -EINTR; break; }
        kwait_wait_until(&c->changed[side], channel_receive_ready, &op, g_ticks + 1);
    }
    if (t) { t->operation = 0; t->cleanup = 0; }
    channel_receive_cleanup(&op);
    return err;
}

/* Internal service connection: reuse duplex queues and description limits;
 * move the peer into the server without exposing a cross-process F2 syscall. */
int channel_connect(struct process *client, struct process *server, int32_t fds[2])
{
    int slot=desktop_fd_slot(server,0);
    if(slot<0)return -EMFILE;
    int err=channel_pair(client,fds);if(err)return err;
    /* channel_pair allocates and can reach a voluntary point. Recheck the
     * destination and both lifetimes before publishing the transferred end. */
    slot=desktop_fd_slot(server,0);
    if(slot<0 || client->state==PROC_STOPPING || server->state!=PROC_LIVE){
        desktop_close(client,fds[0]);desktop_close(client,fds[1]);return slot<0?-EMFILE:-EINTR;
    }
    server->fds[slot]=client->fds[fds[1]];
    client->fds[fds[1]]=(struct proc_fd){0};
    fds[1]=slot;return 0;
}
