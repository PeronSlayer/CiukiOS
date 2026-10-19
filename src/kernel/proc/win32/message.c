/* Sparse per-thread queues, fixed MSG records and explicit sent-message relay.
 * Pointer payloads are copied only for the three contracted messages.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/w32_objects.h>
struct w32_send {
    struct w32_send *next;
    struct process *payer;
    uint32_t token, sender, receiver, value, error, bytes, output;
    void *buffer;
    bool replied, delivered;
};
struct w32_message_node {
    struct w32_message_node *next;
    struct ciuki_w32_message message;
    struct w32_send *send;
    uint32_t bits;
};
static struct w32_send *sends;
static uint32_t next_send = 1;
static struct proc_thread *thread_id(uint32_t tid)
{
    for (unsigned i = 0; i < CIUKI_THREAD_MAX; i++) {
        struct proc_thread *t = proc_thread_slot(i);
        if (t && t->tid == tid && t->w32 && !t->w32->ending)
            return t;
    }
    return 0;
}
static uint32_t queue_charge(uint32_t bytes)
{
    if (bytes > 2040)
        return PAGE_ALIGN_UP(bytes);
    uint32_t charge = 32;
    while (charge < bytes + 8)
        charge *= 2;
    return charge;
}
static void *queue_alloc(struct process *p, uint32_t bytes)
{
    uint32_t charge = queue_charge(bytes);
    if (charge > 4 * 1024 * 1024 - p->w32->private_bytes)
        return 0;
    void *b = w32_alloc(bytes);
    if (b) {
        w32_accounts.metadata_bytes -= bytes;
        p->w32->private_bytes += charge;
        w32_accounts.private_bytes += charge;
    }
    return b;
}
static void queue_free(struct process *p, void *buffer, uint32_t bytes)
{
    if (buffer) {
        uint32_t charge = queue_charge(bytes);
        p->w32->private_bytes -= charge;
        w32_accounts.private_bytes -= charge;
        w32_accounts.metadata_bytes += bytes;
        w32_free(buffer, bytes);
    }
}
static struct w32_object *queue_for(struct proc_thread *t, bool create)
{
    if (!t || !t->w32)
        return 0;
    if (!t->w32->queue && create) {
        t->w32->queue = w32_object_new(W32_OBJ_QUEUE);
        if (t->w32->queue)
            t->w32->queue->u.queue.tid = t->tid;
    }
    return t->w32->queue;
}
uint32_t w32_queue_bits(struct proc_thread *t)
{
    struct w32_object *q = queue_for(t, false);
    return q ? q->u.queue.bits : 0;
}
static void queue_classes(struct w32_object *q, uint32_t bits, bool add)
{
    for (unsigned i = 0; i < 8; i++)
        if (bits & (1u << i)) {
            if (add)
                q->u.queue.classes[i]++;
            else
                q->u.queue.classes[i]--;
            if (q->u.queue.classes[i])
                q->u.queue.bits |= 1u << i;
            else
                q->u.queue.bits &= ~(1u << i);
        }
}
static void node_free(struct w32_object *q, struct w32_message_node *n)
{
    queue_classes(q, n->bits, false);
    struct proc_thread *owner = thread_id(q->u.queue.tid);
    /* During cleanup the thread is ending but still occupies its process slot. */
    if (!owner)
        for (unsigned i = 0; i < CIUKI_THREAD_MAX; i++) {
            struct proc_thread *t = proc_thread_slot(i);
            if (t && t->tid == q->u.queue.tid) {
                owner = t;
                break;
            }
        }
    if (owner && owner->process->w32)
        queue_free(owner->process, n, sizeof(*n));
    else
        kfree(n);
    w32_accounts.messages--;
}
void w32_queue_destroy(struct w32_object *q)
{
    struct w32_message_node *n = q->u.queue.head;
    while (n) {
        struct w32_message_node *next = n->next;
        if (n->send) {
            n->send->replied = true;
            n->send->error = 1062;
        }
        node_free(q, n);
        n = next;
    }
}
static void remove_send_node(struct w32_send *s)
{
    for (struct w32_object *q = w32_objects; q; q = q->next)
        if (q->type == W32_OBJ_QUEUE) {
            struct w32_message_node **at = (struct w32_message_node **)&q->u.queue.head, *last = 0;
            while (*at) {
                struct w32_message_node *n = *at;
                if (n->send == s) {
                    *at = n->next;
                    node_free(q, n);
                } else {
                    last = n;
                    at = &n->next;
                }
            }
            q->u.queue.tail = last;
        }
}
static void send_free(struct w32_send *s)
{
    struct w32_send **at = &sends;
    while (*at && *at != s)
        at = &(*at)->next;
    if (*at)
        *at = s->next;
    remove_send_node(s);
    queue_free(s->payer, s->buffer, s->bytes);
    queue_free(s->payer, s, sizeof(*s));
}
void w32_messages_thread_died(struct proc_thread *t)
{
    for (struct w32_send *s = sends, *next; s; s = next) {
        next = s->next;
        if (s->sender == t->tid)
            send_free(s);
        else if (s->receiver == t->tid) {
            s->replied = true;
            s->error = 1062;
        }
    }
}
static uint32_t enqueue(
    struct proc_thread *target, const struct ciuki_w32_message *message, uint32_t bits, struct w32_send *send)
{
    struct w32_object *q = queue_for(target, true);
    if (!q)
        return 8;
    if (!send && q->u.queue.count >= W32_MESSAGE_MAX)
        return 1816;
    struct w32_message_node *n = queue_alloc(target->process, sizeof(*n));
    if (!n)
        return 1816;
    n->message = *message;
    n->message.time = (uint32_t)w32_now();
    n->bits = bits;
    n->send = send;
    if (q->u.queue.tail)
        ((struct w32_message_node *)q->u.queue.tail)->next = n;
    else
        q->u.queue.head = n;
    q->u.queue.tail = n;
    queue_classes(q, bits, true);
    if (!send)
        q->u.queue.count++;
    w32_accounts.messages++;
    w32_changed_wake();
    return 0;
}
static bool pointer_message(uint32_t message) { return message == 0xc || message == 0xd || message == 0x4a; }
static struct w32_send *find_send(uint32_t token)
{
    for (struct w32_send *s = sends; s; s = s->next)
        if (s->token == token)
            return s;
    return 0;
}
static bool send_ready(void *arg)
{
    struct w32_send *s = arg;
    struct proc_thread *t = thread_id(s->sender);
    return s->replied || !t || t->process->state != PROC_LIVE || (w32_queue_bits(t) & W32_QS_SENDMESSAGE);
}
uint32_t w32_message_call(struct proc_thread *t, struct ciuki_w32_message_args *a)
{
    if (a->bits & ~W32_QS_ALL) {
        a->error = 87;
        return 0;
    }
    if (a->action == W32_MSG_STATUS) {
        struct w32_object *q = queue_for(t, true);
        if (!q)
            a->error = 8;
        else {
            a->result = q->u.queue.bits;
            a->token = q->u.queue.count;
        }
        return 0;
    }
    if (a->action == W32_MSG_POST) {
        struct proc_thread *target = thread_id(a->tid);
        if (!target)
            a->error = 1444;
        else if (pointer_message(a->message.message))
            a->error = 87;
        else
            a->error = enqueue(target, &a->message, a->bits ? a->bits : W32_QS_POSTMESSAGE, 0);
        a->result = !a->error;
        return 0;
    }
    if (a->action == W32_MSG_GET) {
        struct w32_object *q = queue_for(t, true);
        if (!q) {
            a->error = 8;
            return 0;
        }
        struct w32_message_node **at = (struct w32_message_node **)&q->u.queue.head;
        /* Sent records have priority over posted records. */
        while (*at && !(*at)->send)
            at = &(*at)->next;
        if (!*at) {
            at = (struct w32_message_node **)&q->u.queue.head;
            while (*at && a->bits && !((*at)->bits & a->bits))
                at = &(*at)->next;
        }
        struct w32_message_node *n = *at;
        if (!n) {
            a->result = 0;
            return 0;
        }
        if (n->send) {
            if (a->bytes < n->send->bytes) {
                a->bytes = n->send->bytes;
                a->error = 122;
                return 0;
            }
            if (n->send->bytes && copy_to_user(a->buffer, n->send->buffer, n->send->bytes)) {
                a->error = 998;
                return 0;
            }
            n->send->delivered = true;
            a->token = n->send->token;
            a->bytes = n->send->bytes;
        } else {
            a->token = 0;
            q->u.queue.count--;
        }
        a->message = n->message;
        if (n->send && n->send->bytes)
            a->message.lparam = a->buffer;
        a->result = 1;
        *at = n->next;
        if (q->u.queue.tail == n) {
            struct w32_message_node *tail = q->u.queue.head;
            while (tail && tail->next)
                tail = tail->next;
            q->u.queue.tail = tail;
        }
        node_free(q, n);
        return 0;
    }
    if (a->action == W32_MSG_REPLY) {
        struct w32_send *s = find_send(a->token);
        if (!s || s->receiver != t->tid || !s->delivered || s->replied) {
            a->error = 6;
            return 0;
        }
        if (s->output) {
            if (a->bytes > s->bytes) {
                a->error = 87;
                return 0;
            }
            if (a->bytes && copy_from_user(s->buffer, a->buffer, a->bytes)) {
                a->error = 998;
                return 0;
            }
        }
        s->value = a->message.lparam;
        s->replied = true;
        w32_changed_wake();
        a->result = 1;
        return 0;
    }
    if (a->action != W32_MSG_SEND && a->action != W32_MSG_POLL_REPLY) {
        a->error = 87;
        return 0;
    }
    struct w32_send *s = a->token ? find_send(a->token) : 0;
    if (a->token && (!s || s->sender != t->tid)) {
        a->error = 6;
        return 0;
    }
    if (!s) {
        if (a->action == W32_MSG_POLL_REPLY) {
            a->error = 6;
            return 0;
        }
        struct proc_thread *target = thread_id(a->tid);
        if (!target) {
            a->error = 1444;
            return 0;
        }
        if (a->bytes > 65536 || (!pointer_message(a->message.message) && a->bytes) || !next_send) {
            a->error = 87;
            return 0;
        }
        if (pointer_message(a->message.message) && !a->bytes) {
            a->error = 87;
            return 0;
        }
        s = queue_alloc(t->process, sizeof(*s));
        if (!s) {
            a->error = 8;
            return 0;
        }
        s->payer = t->process;
        s->token = next_send++;
        s->sender = t->tid;
        s->receiver = target->tid;
        s->bytes = a->bytes;
        s->output = a->message.message == 0xd;
        if (a->bytes) {
            s->buffer = queue_alloc(t->process, a->bytes);
            if (!s->buffer) {
                queue_free(t->process, s, sizeof(*s));
                a->error = 1816;
                return 0;
            }
            if (!s->output && copy_from_user(s->buffer, a->buffer, a->bytes)) {
                queue_free(t->process, s->buffer, s->bytes);
                queue_free(t->process, s, sizeof(*s));
                a->error = 998;
                return 0;
            }
        }
        s->next = sends;
        sends = s;
        a->token = s->token;
        a->error = enqueue(target, &a->message, W32_QS_SENDMESSAGE, s);
        if (a->error) {
            send_free(s);
            return 0;
        }
    }
    /* Returning SENT is an explicit user-dispatch continuation: kernel32 must
     * drain sent records, reply and re-enter SEND with this token. No callback
     * is ever invoked from this blocked kernel invocation. */
    while (!s->replied && t->process->state == PROC_LIVE) {
        if (w32_queue_bits(t) & W32_QS_SENDMESSAGE) {
            a->result = W32_WAIT_SENT;
            return 0;
        }
        if (a->action == W32_MSG_POLL_REPLY) {
            a->result = W32_WAIT_PENDING;
            return 0;
        }
        kwait_wait_until(&w32_changed, send_ready, s, w32_now() + 1);
    }
    a->error = s->error;
    a->result = s->value;
    if (s->output && !a->error && s->bytes && copy_to_user(a->buffer, s->buffer, s->bytes)) {
        a->error = 998;
        return 0;
    }
    send_free(s);
    return 0;
}
