/* Generational handles and refcounted, ledger-visible Win32 objects.
 * Names/handles are independent of native descriptors and PID slot reuse.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/w32_objects.h>
#include <ciuki/files.h>
extern void w32_snapshot_destroy(struct w32_object *);
extern void w32_find_destroy(struct w32_object *) __attribute__((weak));
extern void w32_file_handle_close(struct process *,struct w32_object *) __attribute__((weak));
void w32_apc_contexts_cleanup(struct proc_thread *t);
struct w32_account w32_accounts;
struct w32_object *w32_objects;
struct kwait w32_changed;
static struct w32_process *w32_processes;
struct process *w32_process_for_memory(struct uaddr *u)
{
    for (struct w32_process *s = w32_processes; s; s = s->next)
        if (s->owner->memory == u)
            return s->owner;
    return 0;
}
/* The production heap accepts at most 2040 payload bytes. Larger vectors
 * and 64 KiB relay payloads use PMM runs through the existing direct map. */
void *w32_alloc(uint32_t n)
{
    void *p = 0;
    if (n <= KHEAP_MAX_REQUEST)
        p = kzalloc(n);
    else if (n <= UINT32_MAX - (PAGE_SIZE - 1)) {
        uint32_t pages = PAGE_ALIGN_UP(n) / PAGE_SIZE, phys = pmm_alloc_contig(pages);
        if (phys) {
            p = P2V(phys);
            memset(p, 0, pages * PAGE_SIZE);
            if (ledger_promise)
                ledger_promise(MM_KERNEL, 0, (int32_t)pages);
        }
    }
    if (p)
        w32_accounts.metadata_bytes += n;
    return p;
}
void w32_free(void *p, uint32_t n)
{
    if (!p)
        return;
    w32_accounts.metadata_bytes -= n;
    if (n <= KHEAP_MAX_REQUEST)
        kfree(p);
    else {
        uint32_t pages = PAGE_ALIGN_UP(n) / PAGE_SIZE, phys = V2P(p);
        for (unsigned i = 0; i < pages; i++)
            mm_free(phys + i * PAGE_SIZE, MM_KERNEL);
        if (ledger_promise)
            ledger_promise(MM_KERNEL, 0, -(int32_t)pages);
    }
}
void w32_snapshot(struct w32_account *out) { *out = w32_accounts; }
uint64_t w32_now(void)
{
    uint32_t f = irq_save();
    uint64_t n = g_ticks;
    irq_restore(f);
    return n;
}
struct w32_object *w32_object_new(uint32_t type)
{
    if (type < W32_OBJ_PROCESS || type > W32_OBJ_SNAPSHOT || w32_accounts.objects >= W32_OBJECT_MAX)
        return 0;
    struct w32_object *o = w32_alloc(sizeof(*o));
    if (!o)
        return 0;
    if (w32_accounts.objects >= W32_OBJECT_MAX) {
        w32_free(o, sizeof(*o));
        return 0;
    }
    o->type = type;
    o->refs = 1;
    o->exit_code = 259;
    o->next = w32_objects;
    w32_objects = o;
    w32_accounts.objects++;
    return o;
}
void w32_retain(struct w32_object *o)
{
    if (o)
        o->refs++;
}
void w32_release(struct w32_object *o)
{
    if (!o || --o->refs)
        return;
    struct w32_object **at = &w32_objects;
    while (*at && *at != o)
        at = &(*at)->next;
    if (*at)
        *at = o->next;
    if (o->type == W32_OBJ_SNAPSHOT) w32_snapshot_destroy(o);
    if (o->type == W32_OBJ_FIND && w32_find_destroy) w32_find_destroy(o);
    if (o->type == W32_OBJ_SECTION)
        w32_section_destroy(o);
    if (o->type == W32_OBJ_QUEUE)
        w32_queue_destroy(o);
    if (o->type == W32_OBJ_FILE && o->u.file.description)
        o->u.file.description->object.release(&o->u.file.description->object);
    if (o->name) {
        uint32_t words = 1;
        while (o->name[words - 1])
            words++;
        w32_free(o->name, words * 2);
    }
    w32_accounts.objects--;
    w32_free(o, sizeof(*o));
}
int w32_process_init(struct process *p)
{
    if (p->w32)
        return 0;
    struct w32_process *s = w32_alloc(sizeof(*s));
    if (!s)
        return -ENOMEM;
    s->identity = w32_object_new(W32_OBJ_PROCESS);
    if (!s->identity) {
        w32_free(s, sizeof(*s));
        return -ENOMEM;
    }
    s->identity->id = p->pid;
    s->owner = p;
    s->priority_class=0x20;
    s->next = w32_processes;
    w32_processes = s;
    p->w32 = s;
    return 0;
}
int w32_thread_objects_init(struct proc_thread *t)
{
    if (t->w32)
        return 0;
    if (w32_process_init(t->process))
        return -ENOMEM;
    struct w32_thread *s = w32_alloc(sizeof(*s));
    if (!s)
        return -ENOMEM;
    s->identity = w32_object_new(W32_OBJ_THREAD);
    if (!s->identity) {
        w32_free(s, sizeof(*s));
        return -ENOMEM;
    }
    s->identity->id = t->tid;
    t->w32 = s;
    return 0;
}
/* q3exec: a handle value is (slot + 1) * 4, from 4 to 0x4000, as on Windows
 * 95/98 and NT, where a kernel handle is a small multiple of four indexing
 * the process handle table and a closed handle's value is reused by the next
 * handle created in that slot. Win16-derived code keeps HFILEs in 16-bit
 * fields: the MindVision VISE 3.x stub of the Quake III Arena demo installer
 * stores its _lopen/_lcreat handles in WORDs and sign-extends them (movswl)
 * before _hread/_hwrite, so any value of 0x8000 or more names another
 * handle. The former generation bits (generation << 14) gave every reused
 * slot such a value. */
static uint32_t handle_value(unsigned slot) { return (slot + 1) << 2; }
static struct w32_handle_entry *entry(struct process *p, uint32_t h)
{
    if (!p->w32 || !h || (h & 3) || h > handle_value(64 * 64 - 1))
        return 0;
    uint32_t slot = (h >> 2) - 1;
    struct w32_handle_entry *chunk = p->w32->chunks[slot / 64];
    struct w32_handle_entry *e = chunk ? &chunk[slot % 64] : 0;
    return e && e->object ? e : 0;
}
struct w32_object *w32_lookup(struct proc_thread *t, uint32_t h, uint32_t type)
{
    struct w32_object *o = 0;
    if (h == W32_CURRENT_PROCESS && t->process->w32)
        o = t->process->w32->identity;
    else if (h == W32_CURRENT_THREAD && t->w32)
        o = t->w32->identity;
    else {
        struct w32_handle_entry *e = entry(t->process, h);
        if (e)
            o = e->object;
    }
    return o && (!type || o->type == type) ? o : 0;
}
uint32_t w32_handle_install(
    struct process *p, struct w32_object *o, uint32_t access, bool inherit, uint32_t *h)
{
    if (!p->w32 && w32_process_init(p))
        return 8;
    if (p->w32->closing || p->state == PROC_STOPPING || p->state == PROC_ZOMBIE)
        return 5;
    for (unsigned c = 0; c < 64; c++) {
        if (!p->w32->chunks[c]) {
            struct w32_handle_entry *chunk = w32_alloc(64 * sizeof(*chunk));
            if (!chunk)
                return 8;
            if (!p->w32 || p->w32->closing || p->state==PROC_STOPPING || p->state==PROC_ZOMBIE) {
                w32_free(chunk,64*sizeof(*chunk));return 5;
            }
            if (p->w32->chunks[c])
                w32_free(chunk, 64 * sizeof(*chunk));
            else
                p->w32->chunks[c] = chunk;
        }
        for (unsigned i = 0; i < 64; i++) {
            struct w32_handle_entry *e = &p->w32->chunks[c][i];
            if (e->object)
                continue;
            *h = handle_value(c * 64 + i);
            e->object = o;
            e->access = access;
            e->inherit = inherit;
            w32_retain(o);
            p->w32->handles++;
            if(p->w32->handles>p->w32->peak_handles)p->w32->peak_handles=p->w32->handles;
            w32_accounts.handles++;
            return 0;
        }
    }
    return 4; /* ERROR_TOO_MANY_OPEN_FILES */
}
uint32_t w32_handle_close(struct process *p, uint32_t h)
{
    struct w32_handle_entry *e = entry(p, h);
    if (!e)
        return W32_ERROR_INVALID_HANDLE;
    struct w32_object *o = e->object;
    e->object = 0;
    p->w32->handles--;
    w32_accounts.handles--;
    if (o->type==W32_OBJ_FILE && w32_file_handle_close) w32_file_handle_close(p,o);
    w32_release(o);
    return 0;
}
uint32_t w32_handles_inherit(struct process *child, const struct process *parent)
{
    if (child->personality != PROC_WIN32 || !parent->w32)
        return 0;
    if (w32_process_init(child))
        return 8;
    /* Transactional: provision all chunks before installing any reference. */
    for (unsigned c = 0; c < 64; c++)
        if (parent->w32->chunks[c]) {
            bool needed = false;
            for (unsigned i = 0; i < 64; i++)
                needed |= parent->w32->chunks[c][i].object && parent->w32->chunks[c][i].inherit;
            if (!needed)
                continue;
            if (child->w32->chunks[c])
                return 87;
            struct w32_handle_entry *chunk = w32_alloc(64 * sizeof(*chunk));
            if (!chunk)
                return 8;
            if (!child->w32 || child->w32->closing || !parent->w32 || parent->w32->closing) {
                w32_free(chunk,64*sizeof(*chunk));return 5;
            }
            child->w32->chunks[c] = chunk;
        }
    for (unsigned c = 0; c < 64; c++)
        if (child->w32->chunks[c] && parent->w32->chunks[c])
            for (unsigned i = 0; i < 64; i++) {
                const struct w32_handle_entry *s = &parent->w32->chunks[c][i];
                if (!s->object || !s->inherit)
                    continue;
                child->w32->chunks[c][i] = *s;
                w32_retain(s->object);
                child->w32->handles++;
                if(child->w32->handles>child->w32->peak_handles)child->w32->peak_handles=child->w32->handles;
                w32_accounts.handles++;
            }
    return 0;
}
static const uint16_t *flat_name(const uint16_t *n)
{
    if (!n)
        return 0;
    static const uint16_t global[] = { 'G', 'l', 'o', 'b', 'a', 'l', '\\', 0 },
                          local[] = { 'L', 'o', 'c', 'a', 'l', '\\', 0 };
    unsigned i = 0;
    while (global[i] && n[i] == global[i])
        i++;
    if (!global[i])
        return n + i;
    i = 0;
    while (local[i] && n[i] == local[i])
        i++;
    return !local[i] ? n + i : n;
}
static bool name_equal(const uint16_t *a, const uint16_t *b)
{
    if (!a || !b)
        return false;
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}
uint32_t w32_named_create(struct process *p, uint32_t type, const uint16_t *name, uint32_t access,
    bool inherit, struct w32_object **out, uint32_t *h, bool *existing)
{
    name = flat_name(name);
    if (name && !*name)
        name = 0;
    unsigned len = 0;
    if (name) {
        while (len <= 260 && name[len])
            len++;
        if (len > 260)
            return 206;
    }
    *existing = false;
    struct w32_object *o = 0;
    for (struct w32_object *it = w32_objects; name && it; it = it->next)
        if (name_equal(it->name, name)) {
            o = it;
            break;
        }
    if (o) {
        if (o->type != type)
            return 6;
        if (type == W32_OBJ_SECTION && !o->section_limit)
            return 170;
        *existing = true;
        w32_retain(o);
    } else {
        o = w32_object_new(type);
        if (!o)
            return 8;
        if (name) {
            o->name = w32_alloc((len + 1) * 2);
            if (!o->name) {
                w32_release(o);
                return 8;
            }
            memcpy(o->name, name, (len + 1) * 2);
        }
    }
    uint32_t error = w32_handle_install(p, o, access, inherit, h);
    *out = error ? 0 : o;
    w32_release(o);
    return error;
}
static uint32_t copy_name(uint32_t va, uint16_t name[261])
{
    if (!va) {
        name[0] = 0;
        return 0;
    }
    for (unsigned i = 0; i <= 260; i++) {
        if (va > UINT32_MAX - i * 2 || copy_from_user(&name[i], va + i * 2, 2))
            return 998;
        if (!name[i])
            return 0;
    }
    return 206;
}
static struct process *process_handle(struct proc_thread *t, uint32_t h)
{
    struct w32_object *o = w32_lookup(t, h, W32_OBJ_PROCESS);
    if (!o || o->signalled)
        return 0;
    struct process *p = proc_find(o->id);
    return p && p->w32 && p->w32->identity == o ? p : 0;
}
static uint32_t duplicate(struct proc_thread *t, struct ciuki_w32_object_args *a)
{
    struct process *src = process_handle(t, a->source), *dst = process_handle(t, a->target);
    if (!src)
        return 6;
    struct w32_handle_entry *e = entry(src, a->handle);
    struct w32_object *o = e ? e->object : 0;
    if (src == t->process && (a->handle == W32_CURRENT_PROCESS || a->handle == W32_CURRENT_THREAD))
        o = w32_lookup(t, a->handle, 0);
    uint32_t access = e ? e->access : UINT32_MAX, error = 0;
    if (!o)
        error = 6;
    else {
        w32_retain(o);
        if (!dst)
            error = a->target ? 6 : (a->flags & W32_DUP_CLOSE_SOURCE ? 0 : 6);
        else
            error = w32_handle_install(
                dst, o, (a->flags & W32_DUP_SAME_ACCESS) ? access : a->access, !!a->value, &a->result);
        w32_release(o);
    }
    if (a->flags & W32_DUP_CLOSE_SOURCE)
        w32_handle_close(src, a->handle);
    return error;
}
uint32_t w32_object_call(struct proc_thread *t, struct ciuki_w32_object_args *a)
{
    uint32_t error = 0;
    struct w32_object *o = 0;
    if (a->action == W32_OBJ_CLOSE)
        error = w32_handle_close(t->process, a->handle);
    else if (a->action == W32_OBJ_DUPLICATE) {
        if (a->flags & ~3u)
            error = 87;
        else
            error = duplicate(t, a);
    } else if (a->action == W32_OBJ_CREATE || a->action == W32_OBJ_OPEN) {
        uint16_t name[261];
        error = copy_name(a->name, name);
        if (error)
            goto done;
        if (a->type == W32_OBJ_PROCESS || a->type == W32_OBJ_THREAD) {
            struct process *p = proc_find(a->value);
            if (a->type == W32_OBJ_PROCESS)
                o = p && p->w32 ? p->w32->identity : 0;
            else
                for (unsigned i = 0; i < CIUKI_THREAD_MAX; i++) {
                    struct proc_thread *v = proc_thread_slot(i);
                    if (v && v->tid == a->value && v->w32)
                        o = v->w32->identity;
                }
            error = o ? w32_handle_install(t->process, o, a->access, !!a->flags, &a->result) : 6;
        } else if (a->type == W32_OBJ_FILE) {
            struct file_description *d = file_fd(t->process, (int32_t)a->source);
            if (!d)
                error = 6;
            else {
                o = w32_object_new(W32_OBJ_FILE);
                if (!o)
                    error = 8;
                else {
                    o->u.file.description = d;
                    d->object.retain(&d->object);
                    o->u.file.access = a->access;
                    o->u.file.share = a->maximum;
                    o->signalled = true;
                    error = w32_handle_install(t->process, o, a->access, !!a->flags, &a->result);
                    w32_release(o);
                }
            }
        } else if (a->type == W32_OBJ_EVENT || a->type == W32_OBJ_MUTEX || a->type == W32_OBJ_SEMAPHORE
            || a->type == W32_OBJ_TIMER || a->type == W32_OBJ_WINSTATION) {
            if (a->type == W32_OBJ_SEMAPHORE
                && (!a->maximum || a->maximum > INT32_MAX || a->value > a->maximum)) {
                error = 87;
                goto done;
            }
            bool exists = false;
            if (a->action == W32_OBJ_OPEN) {
                for (struct w32_object *it = w32_objects; it; it = it->next)
                    if (name_equal(it->name, flat_name(name))) {
                        o = it;
                        break;
                    }
                error = !o ? 2
                    : o->type != a->type
                    ? 6
                    : w32_handle_install(t->process, o, a->access, !!a->flags, &a->result);
            } else {
                error = w32_named_create(t->process, a->type, name[0] ? name : 0, a->access, !!(a->flags & 1),
                    &o, &a->result, &exists);
                if (!error && !exists) {
                    o->manual = !!a->maximum;
                    o->signalled = !!a->value;
                    if (o->type == W32_OBJ_MUTEX) {
                        o->signalled = false;
                        if (a->value) {
                            o->u.mutex.owner = t->tid;
                            o->u.mutex.recursion = 1;
                            w32_retain(o);
                        }
                    }
                    if (o->type == W32_OBJ_SEMAPHORE)
                        o->u.semaphore.count = a->value, o->u.semaphore.maximum = a->maximum;
                }
                if (!error && exists)
                    error = 183;
            }
        } else
            error = 87;
    } else {
        o = w32_lookup(t, a->handle, 0);
        if (!o) {
            error = 6;
            goto done;
        }
        switch (a->action) {
        case W32_OBJ_SET:
        case W32_OBJ_RESET:
        case W32_OBJ_PULSE:
            if (o->type != W32_OBJ_EVENT) {
                error = 6;
                break;
            }
            o->signalled = a->action != W32_OBJ_RESET;
            w32_wait_pump();
            if (a->action == W32_OBJ_PULSE)
                o->signalled = false;
            break;
        case W32_OBJ_RELEASE:
            if (o->type == W32_OBJ_MUTEX) {
                if (o->u.mutex.owner != t->tid) {
                    error = 288;
                    break;
                }
                if (!--o->u.mutex.recursion) {
                    o->u.mutex.owner = 0;
                    w32_release(o);
                }
                w32_wait_pump();
            } else if (o->type == W32_OBJ_SEMAPHORE) {
                if (!a->value || a->value > o->u.semaphore.maximum - o->u.semaphore.count) {
                    error = 298;
                    break;
                }
                a->result = o->u.semaphore.count;
                o->u.semaphore.count += a->value;
                w32_wait_pump();
            } else
                error = 6;
            break;
        case W32_OBJ_QUERY: {
            struct w32_handle_entry *e = entry(t->process, a->handle);
            a->access = e ? e->access : UINT32_MAX;
            a->flags = e ? e->inherit : 0;
            a->type = o->type;
            a->result = o->exit_code;
            break;
        }
        case W32_OBJ_SET_INHERIT: {
            struct w32_handle_entry *e = entry(t->process, a->handle);
            if (!e)
                error = 6;
            else
                e->inherit = !!a->value;
            break;
        }
        case W32_OBJ_TIMER_SET:
            if (o->type != W32_OBJ_TIMER) {
                error = 6;
                break;
            }
            {
                int64_t due = (int64_t)(((uint64_t)a->maximum << 32) | a->value);
                uint64_t now = w32_now(), delta;
                if (due < 0)
                    delta = ((uint64_t)(-(due + 1)) + 1 + 9999) / 10000;
                else {
                    uint64_t sys = w32_system_time();
                    delta = (uint64_t)due > sys ? ((uint64_t)due - sys + 9999) / 10000 : 0;
                }
                if (a->extra > INT32_MAX) {
                    error = 87;
                    break;
                }
                o->u.timer.due = now + delta;
                o->u.timer.period = a->extra;
                o->u.timer.routine = a->source;
                o->u.timer.argument = a->target;
                o->u.timer.tid = t->tid;
                o->u.timer.armed = true;
                o->signalled = false;
            }
            break;
        case W32_OBJ_TIMER_CANCEL:
            if (o->type != W32_OBJ_TIMER)
                error = 6;
            else
                o->u.timer.armed = false;
            break;
        default:
            error = 87;
            break;
        }
    }
done:
    a->error = error;
    w32_changed_wake();
    return 0;
}
void w32_thread_died(struct proc_thread *t)
{
    if (!t->w32 || t->w32->ending)
        return;
    w32_messages_thread_died(t);
    t->w32->ending = true;
    t->w32->identity->signalled = true;
    t->w32->identity->exit_code = t->value;
    for (struct w32_object *o = w32_objects, *next; o; o = next) {
        next = o->next;
        if (o->type == W32_OBJ_MUTEX && o->u.mutex.owner == t->tid) {
            o->u.mutex.owner = 0;
            o->u.mutex.recursion = 0;
            o->abandoned = true;
            w32_release(o);
        } else if (o->type == W32_OBJ_TIMER && o->u.timer.tid == t->tid)
            o->u.timer.routine = 0;
    }
    w32_changed_wake();
}
void w32_process_died(struct process *p)
{
    if (!p->w32)
        return;
    w32_diagnostic_exit(p);
    p->w32->identity->signalled = true;
    p->w32->identity->exit_code = (uint32_t)p->raw_exit;
    w32_changed_wake();
}
void w32_thread_objects_cleanup(struct proc_thread *t)
{
    struct w32_thread *s = t->w32;
    if (!s)
        return;
    w32_thread_died(t);
    while (s->head) {
        struct w32_apc_node *n = s->head;
        s->head = n->next;
        w32_free(n, sizeof(*n));
        w32_accounts.apcs--;
    }
    w32_free(s->context, sizeof(*s->context));
    w32_apc_contexts_cleanup(t);
    w32_release(s->queue);
    w32_release(s->identity);
    w32_free(s, sizeof(*s));
    t->w32 = 0;
}
void w32_process_cleanup(struct process *p)
{
    if (!p->w32)
        return;
    p->w32->closing = true;
    for (unsigned i = 0; i < CIUKI_THREAD_MAX; i++) {
        struct proc_thread *t = proc_thread_slot(i);
        if (t && t->process == p) {
            if (t->task)
                ua_cancel_pins(p->memory, t->task);
            w32_thread_objects_cleanup(t);
        }
    }
    w32_process_died(p);
    w32_diagnostic_cleanup(p);
    for(unsigned d=0;d<26;d++)if(p->w32->drive_cwd[d]&&p->cwd_release)p->cwd_release(p->w32->drive_cwd[d]);
    w32_services_cleanup(p);
    w32_views_cleanup(p);
    for (unsigned c = 0; c < 64; c++)
        if (p->w32->chunks[c]) {
            for (unsigned i = 0; i < 64; i++) {
                struct w32_handle_entry *e = &p->w32->chunks[c][i];
                if (e->object)
                    w32_handle_close(p, handle_value(c * 64 + i));
            }
            w32_free(p->w32->chunks[c], 64 * sizeof(struct w32_handle_entry));
        }
    struct w32_process **at = &w32_processes;
    while (*at && *at != p->w32)
        at = &(*at)->next;
    if (*at)
        *at = p->w32->next;
    w32_release(p->w32->identity);
    w32_free(p->w32, sizeof(*p->w32));
    p->w32 = 0;
}
