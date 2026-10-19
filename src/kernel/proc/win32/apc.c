/* User APC frames and suspension at a user-return boundary only.
 * No user function is called on a kernel stack or under a kernel lock.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/clock.h>
#include <ciuki/w32_objects.h>
uint64_t w32_system_time(void)
{
    struct ciuki_timespec now;
    if (file_clock_now(CLOCK_REALTIME, 0, &now) || now.tv_sec < -11644473600LL)
        return 0;
    return ((uint64_t)now.tv_sec + 11644473600ULL) * 10000000 + (uint32_t)now.tv_nsec / 100;
}
uint32_t w32_apc_queue(struct proc_thread *t, uint32_t routine, uint32_t arg, uint32_t low, uint32_t high)
{
    if (!t || !t->w32 || t->w32->ending || t->process->state != PROC_LIVE)
        return 6;
    if (routine < CIUKI_W32_LOW || routine >= CIUKI_W32_BUILTIN_LIMIT
        || !ua_range(t->process->memory, routine, 1, PROT_READ | PROT_EXEC))
        return 87;
    if (t->w32->apcs >= 1024)
        return 1816;
    struct w32_apc_node *n = w32_alloc(sizeof(*n));
    if (!n)
        return 8;
    *n = (struct w32_apc_node) { .routine = routine, .argument = arg, .low = low, .high = high };
    if (t->w32->tail)
        t->w32->tail->next = n;
    else
        t->w32->head = n;
    t->w32->tail = n;
    t->w32->apcs++;
    w32_accounts.apcs++;
    w32_changed_wake();
    return 0;
}
/* An alertable wait inside an APC owns a nested saved context. Draining
 * that inner batch must resume the callback, then the outer wait. */
struct apc_saved {
    struct apc_saved *next;
    struct proc_thread *thread;
    struct ciuki_w32_context *previous;
    struct ciuki_w32_context context;
};
static struct apc_saved *apc_saved_frames;
static void apc_pop(struct proc_thread *t)
{
    struct apc_saved **at = &apc_saved_frames;
    while (*at && &(*at)->context != t->w32->apc_context)
        at = &(*at)->next;
    if (!*at)
        return;
    struct apc_saved *saved = *at;
    *at = saved->next;
    t->w32->apc_context = saved->previous;
    w32_free(saved, sizeof(*saved));
}
void w32_apc_contexts_cleanup(struct proc_thread *t)
{
    while (t->w32->apc_context)
        apc_pop(t);
}
static bool apc_next(struct proc_thread *t, struct trap_frame *tf)
{
    struct w32_thread *s = t->w32;
    struct w32_apc_node *n = s->head;
    uint32_t frame[5] = { 0, n->routine, n->argument, n->low, n->high };
    uint32_t sp = tf->user_esp >= sizeof(frame) ? (tf->user_esp - sizeof(frame)) & ~15u : 0;
    if (!ua_range(t->process->memory, sp, sizeof(frame), PROT_READ | PROT_WRITE)
        || ua_write(t->process->memory, sp, frame, sizeof(frame))) {
        proc_stop(t->process, (int32_t)W32_EXCEPTION_STACK, 0);
        return false;
    }
    s->head = n->next;
    if (!s->head)
        s->tail = 0;
    s->apcs--;
    w32_accounts.apcs--;
    w32_free(n, sizeof(*n));
    tf->eip = t->process->w32->apc_dispatcher;
    tf->user_esp = sp;
    tf->eflags = 0x202;
    tf->fs = CIUKI_W32_FS_SELECTOR;
    tf->gs = tf->es = tf->ds = SEL_UDATA;
    return true;
}
bool w32_apc_deliver(struct proc_thread *t, struct trap_frame *tf)
{
    if (!t->w32 || !t->w32->head || !t->process->w32->apc_dispatcher)
        return false;
    struct apc_saved *saved = w32_alloc(sizeof(*saved));
    if (!saved)
        return false;
    saved->thread = t;
    saved->previous = t->w32->apc_context;
    saved->next = apc_saved_frames;
    w32_context_capture(&saved->context, tf);
    fpu_w32_export(t->task, &saved->context);
    apc_saved_frames = saved;
    t->w32->apc_context = &saved->context;
    if (apc_next(t, tf))
        return true;
    apc_pop(t);
    return false;
}
uint32_t w32_apc_call(
    struct proc_thread *t, struct ciuki_w32_apc_args *a, struct trap_frame *tf, bool *replaced)
{
    if (a->action == W32_APC_REGISTER) {
        if (a->routine < CIUKI_W32_APP_LIMIT || a->routine >= CIUKI_W32_BUILTIN_LIMIT
            || !ua_range(t->process->memory, a->routine, 1, PROT_READ | PROT_EXEC))
            a->error = 87;
        else
            t->process->w32->apc_dispatcher = a->routine;
    } else if (a->action == W32_APC_QUEUE) {
        struct w32_object *o = w32_lookup(t, a->thread, W32_OBJ_THREAD);
        struct proc_thread *target = 0;
        for (unsigned i = 0; o && i < CIUKI_THREAD_MAX; i++) {
            struct proc_thread *v = proc_thread_slot(i);
            if (v && v->tid == o->id && v->w32 && v->w32->identity == o)
                target = v;
        }
        a->error = w32_apc_queue(target, a->routine, a->argument, 0, 0);
        a->result = !a->error;
    } else if (a->action == W32_APC_RETURN) {
        if (!t->w32->apc_context)
            a->error = 87;
        else {
            w32_context_restore(tf, t->w32->apc_context);
            fpu_w32_import(t->task, t->w32->apc_context);
            *replaced = true;
            if (!t->w32->head) {
                apc_pop(t);
            } else if (!apc_next(t, tf))
                a->error = 8;
        }
    } else
        a->error = 87;
    return 0;
}
static bool resume_ready(void *arg)
{
    struct proc_thread *t = arg;
    return !t->w32->suspend || t->process->state != PROC_LIVE || t->stopped;
}
void w32_user_return(struct proc_thread *t, struct trap_frame *tf)
{
    if (!t->w32)
        return;
    struct w32_thread *s = t->w32;
    if (!s->suspend)
        return;
    if (!s->context)
        s->context = w32_alloc(sizeof(*s->context));
    if (!s->context) {
        proc_stop(t->process, (int32_t)W32_STATUS_NO_MEMORY, 0);
        return;
    }
    w32_context_capture(s->context, tf);
    fpu_w32_export(t->task, s->context);
    s->safe = true;
    w32_changed_wake();
    while (!resume_ready(t))
        kwait_wait_until(&w32_changed, resume_ready, t, w32_now() + 1);
    if (t->process->state == PROC_LIVE) {
        w32_context_restore(tf, s->context);
        fpu_w32_import(t->task, s->context);
    }
    s->safe = false;
}
static struct proc_thread *context_target(struct proc_thread *caller, uint32_t handle)
{
    struct w32_object *o = w32_lookup(caller, handle, W32_OBJ_THREAD);
    for (unsigned i = 0; o && i < CIUKI_THREAD_MAX; i++) {
        struct proc_thread *t = proc_thread_slot(i);
        if (t && t->process == caller->process && t->w32 && t->w32->identity == o && !t->w32->ending)
            return t;
    }
    return 0;
}
static void merge_context(struct ciuki_w32_context *dst, const struct ciuki_w32_context *src, uint32_t f)
{
    if (f & 1) {
        dst->ebp = src->ebp;
        dst->eip = src->eip;
        dst->cs = src->cs;
        dst->eflags = src->eflags;
        dst->esp = src->esp;
        dst->ss = src->ss;
    }
    if (f & 2) {
        dst->edi = src->edi;
        dst->esi = src->esi;
        dst->ebx = src->ebx;
        dst->edx = src->edx;
        dst->ecx = src->ecx;
        dst->eax = src->eax;
    }
    if (f & 4) {
        dst->gs = src->gs;
        dst->fs = src->fs;
        dst->es = src->es;
        dst->ds = src->ds;
    }
    if (f & 8)
        dst->floating = src->floating;
    if (f & 0x20)
        memcpy(dst->extended, src->extended, 512);
    memset(dst->debug, 0, sizeof(dst->debug));
}
uint32_t w32_context_call(struct proc_thread *caller, struct ciuki_w32_context_args *a)
{
    struct proc_thread *t = context_target(caller, a->thread);
    if (!t) {
        a->error = 6;
        return 0;
    }
    struct w32_thread *s = t->w32;
    if (a->action == W32_CTX_SUSPEND) {
        if (s->suspend == 127)
            a->error = 87;
        else
            a->result = s->suspend++;
    } else if (a->action == W32_CTX_RESUME) {
        a->result = s->suspend;
        if (s->suspend)
            s->suspend--;
        w32_changed_wake();
    } else if (a->action == W32_CTX_GET || a->action == W32_CTX_SET) {
        if (!s->suspend || !s->safe || !s->context) {
            a->error = 170;
            return 0;
        }
        uint32_t f = a->flags;
        if ((f & 0xffff0000) != 0x10000 || (f & ~0x1003f) || !(f & 0x3f) || ((f & 0x20) && !g_cpu_fxsr)) {
            a->error = 87;
            return 0;
        }
        struct ciuki_w32_context c = { 0 };
        if (a->action == W32_CTX_GET) {
            c.flags = f;
            merge_context(&c, s->context, f);
            if (copy_to_user(a->context, &c, sizeof(c)))
                a->error = 998;
        } else {
            if (copy_from_user(&c, a->context, sizeof(c))) {
                a->error = 998;
                return 0;
            }
            struct ciuki_w32_context merged = *s->context;
            merge_context(&merged, &c, f);
            merged.flags = W32_CONTEXT_FULL | W32_CONTEXT_FLOAT | (g_cpu_fxsr ? W32_CONTEXT_EXTENDED : 0);
            if (!w32_context_validate(&merged, t->process->memory, g_cpu_fxsr, fpu_mxcsr_mask()))
                a->error = 87;
            else
                *s->context = merged;
        }
    } else
        a->error = 87;
    return 0;
}
