/* lwIP OS abstraction shared by kernel and pthread harness.
 * Internal waits cannot be cancelled by native process signals.
 * SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include "lwip/sys.h"
#include <stdbool.h>
#include <string.h>
#ifdef LWIP_HOST
#include <pthread.h>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <sched.h>
struct event { pthread_cond_t condition; unsigned sequence; };
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local unsigned nesting;
static uint64_t clock_offset;
#else
#include <ciuki/kernel.h>
#include <ciuki/task.h>
#include <ciuki/cpu.h>
struct event { struct kwait queue; unsigned sequence; };
#endif
#define SEMS 128u
#define MUTEXES 64u
#define MBOXES 112u
#define MBOX_CAPACITY 64u
struct cu_sem { struct event event; bool used, signalled; unsigned waiters; };
struct cu_mutex {
#ifdef LWIP_HOST
    pthread_mutex_t lock;
#else
    struct kmutex lock;
#endif
    bool used;
};
struct cu_mbox { struct event event; void *data[MBOX_CAPACITY]; unsigned head, count, size, waiters; bool used; };
static struct cu_sem sems[SEMS];
static struct cu_mutex mutexes[MUTEXES];
static struct cu_mbox mboxes[MBOXES];
static uintptr_t core;
static unsigned core_batch;
static uint64_t random_state;
#ifdef LWIP_HOST
static unsigned random_calls;
unsigned cu_host_random_calls(void) { return random_calls; }
#endif
struct thread_start { void (*fn)(void *); void *arg; bool used;
#ifdef LWIP_HOST
    pthread_t thread;
#endif
};
static struct thread_start threads[7];

uint64_t cu_now(void)
{
#ifdef LWIP_HOST
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (unsigned)t.tv_nsec / 1000000 + clock_offset;
#else
    uint32_t f = irq_save(); uint64_t now = g_ticks; irq_restore(f); return now;
#endif
}
u32_t sys_now(void) { return (u32_t)cu_now(); }
u32_t sys_jiffies(void) { return sys_now(); }
sys_prot_t sys_arch_protect(void)
{
#ifdef LWIP_HOST
    unsigned old = nesting++;
    if (!old) pthread_mutex_lock(&gate);
    return old;
#else
    return irq_save();
#endif
}
void sys_arch_unprotect(sys_prot_t saved)
{
#ifdef LWIP_HOST
    nesting = (unsigned)saved;
    if (!nesting) pthread_mutex_unlock(&gate);
#else
    irq_restore((uint32_t)saved);
#endif
}
static void event_init(struct event *e)
{
#ifdef LWIP_HOST
    pthread_condattr_t a; pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(&e->condition, &a); pthread_condattr_destroy(&a);
#else
    kwait_init(&e->queue);
#endif
    e->sequence = 0;
}
static void event_free(struct event *e)
{
#ifdef LWIP_HOST
    pthread_cond_destroy(&e->condition);
#else
    (void)e;
#endif
}
static void event_wake(struct event *e)
{
    e->sequence++;
#ifdef LWIP_HOST
    pthread_cond_broadcast(&e->condition);
#else
    kwait_wake_all(&e->queue);
#endif
}
#ifndef LWIP_HOST
struct event_wait { struct event *event; unsigned sequence; };
static bool event_changed(void *arg)
{
    struct event_wait *w = arg; return w->event->sequence != w->sequence;
}
#endif
/* Called with the short publication lock held. Always returns with it held. */
#ifdef LWIP_HOST
static void unlock_cancel(void *arg) { nesting = 0; pthread_mutex_unlock(arg); }
#endif
static void event_wait(struct event *e, uint64_t deadline, bool forever, sys_prot_t *saved)
{
#ifdef LWIP_HOST
    (void)saved;
    pthread_cleanup_push(unlock_cancel, &gate);
    if (forever) pthread_cond_wait(&e->condition, &gate);
    else {
        uint64_t real = deadline - clock_offset;
        struct timespec t = { (time_t)(real / 1000), (long)(real % 1000) * 1000000 };
        pthread_cond_timedwait(&e->condition, &gate, &t);
    }
    pthread_cleanup_pop(0);
#else
    struct event_wait w = { e, e->sequence };
    sys_arch_unprotect(*saved);
    /* Periodic safety deadline also bounds the forever case without treating
     * timeout zero as polling. kwait rechecks under IRQ exclusion. */
    kwait_wait_until(&e->queue, event_changed, &w, forever ? cu_now() + 60000 : deadline);
    *saved = sys_arch_protect();
#endif
}
void cu_sleep(unsigned ms)
{
#ifdef LWIP_HOST
    struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000 };
    nanosleep(&t, NULL);
#else
    task_sleep_ms(ms);
#endif
}
void cu_yield(void)
{
#ifdef LWIP_HOST
    sched_yield();
#else
    task_yield();
#endif
}
uintptr_t cu_identity(void)
{
#ifdef LWIP_HOST
    return (uintptr_t)pthread_self();
#else
    return (uintptr_t)g_current;
#endif
}
void sys_init(void) { core = cu_identity(); core_batch = 0; }
err_t sys_sem_new(sys_sem_t *out, u8_t count)
{
    *out = NULL; if (count > 1) return ERR_VAL;
    sys_prot_t f = sys_arch_protect();
    for (unsigned i = 0; i < SEMS; i++) if (!sems[i].used) {
        struct cu_sem *s = &sems[i]; s->used = true; s->signalled = count != 0; s->waiters = 0;
        event_init(&s->event); *out = s; break;
    }
    sys_arch_unprotect(f); return *out ? ERR_OK : ERR_MEM;
}
void sys_sem_signal(sys_sem_t *s)
{
    sys_prot_t f = sys_arch_protect(); (*s)->signalled = true; event_wake(&(*s)->event); sys_arch_unprotect(f);
}
u32_t sys_arch_sem_wait(sys_sem_t *s, u32_t timeout)
{
    uint64_t start = cu_now(), end = start + timeout;
    sys_prot_t f = sys_arch_protect(); (*s)->waiters++;
    while (!(*s)->signalled && (!timeout || cu_now() < end)) event_wait(&(*s)->event, end, !timeout, &f);
    bool ok = (*s)->signalled; if (ok) (*s)->signalled = false;
    (*s)->waiters--; sys_arch_unprotect(f);
    return ok ? (u32_t)(cu_now() - start) : SYS_ARCH_TIMEOUT;
}
void sys_sem_free(sys_sem_t *s)
{
    sys_prot_t f = sys_arch_protect();
    if ((*s)->waiters) { sys_arch_unprotect(f); ciuki_lwip_assert_fail("semaphore still referenced"); return; }
    event_free(&(*s)->event); (*s)->used = false; *s = NULL; sys_arch_unprotect(f);
}
err_t sys_mutex_new(sys_mutex_t *out)
{
    *out = NULL; sys_prot_t f = sys_arch_protect();
    for (unsigned i = 0; i < MUTEXES; i++) if (!mutexes[i].used) {
        struct cu_mutex *m = &mutexes[i]; m->used = true;
#ifdef LWIP_HOST
        pthread_mutex_init(&m->lock, NULL);
#else
        kmutex_init(&m->lock);
#endif
        *out = m; break;
    }
    sys_arch_unprotect(f); return *out ? ERR_OK : ERR_MEM;
}
void sys_mutex_lock(sys_mutex_t *m)
{
#ifdef LWIP_HOST
    pthread_mutex_lock(&(*m)->lock);
#else
    kmutex_lock(&(*m)->lock);
#endif
}
void sys_mutex_unlock(sys_mutex_t *m)
{
#ifdef LWIP_HOST
    pthread_mutex_unlock(&(*m)->lock);
#else
    kmutex_unlock(&(*m)->lock);
#endif
}
void sys_mutex_free(sys_mutex_t *m)
{
    sys_prot_t f = sys_arch_protect();
#ifdef LWIP_HOST
    pthread_mutex_destroy(&(*m)->lock);
#endif
    (*m)->used = false; *m = NULL; sys_arch_unprotect(f);
}
err_t sys_mbox_new(sys_mbox_t *out, int size)
{
    *out = NULL; if (size < 1 || size > (int)MBOX_CAPACITY) return ERR_VAL;
    sys_prot_t f = sys_arch_protect();
    for (unsigned i = 0; i < MBOXES; i++) if (!mboxes[i].used) {
        struct cu_mbox *m = &mboxes[i]; m->used = true; m->size = (unsigned)size;
        m->head = m->count = m->waiters = 0; event_init(&m->event); *out = m; break;
    }
    sys_arch_unprotect(f); return *out ? ERR_OK : ERR_MEM;
}
err_t sys_mbox_trypost(sys_mbox_t *box, void *msg)
{
    sys_prot_t f = sys_arch_protect(); struct cu_mbox *m = *box;
    err_t r = ERR_MEM;
    if (m->count < m->size) {
        m->data[(m->head + m->count++) % m->size] = msg; event_wake(&m->event); r = ERR_OK;
    }
    sys_arch_unprotect(f); return r;
}
err_t sys_mbox_trypost_fromisr(sys_mbox_t *box, void *msg)
{ (void)box; (void)msg; return ERR_MEM; /* driver IRQs publish causes only */ }
void sys_mbox_post(sys_mbox_t *box, void *msg)
{
    sys_prot_t f = sys_arch_protect(); struct cu_mbox *m = *box; m->waiters++;
    while (m->count == m->size) event_wait(&m->event, 0, true, &f);
    m->data[(m->head + m->count++) % m->size] = msg;
    m->waiters--; event_wake(&m->event); sys_arch_unprotect(f);
}
u32_t sys_arch_mbox_tryfetch(sys_mbox_t *box, void **msg)
{
    sys_prot_t f = sys_arch_protect(); struct cu_mbox *m = *box; u32_t r = SYS_MBOX_EMPTY;
    if (m->count) {
        if (msg) *msg = m->data[m->head]; m->data[m->head] = NULL;
        m->head = (m->head + 1) % m->size; m->count--; event_wake(&m->event); r = 0;
    }
    sys_arch_unprotect(f); return r;
}
u32_t sys_arch_mbox_fetch(sys_mbox_t *box, void **msg, u32_t timeout)
{
    uint64_t start = cu_now(), end = start + timeout;
    sys_prot_t f = sys_arch_protect(); struct cu_mbox *m = *box; m->waiters++;
    while (!m->count && (!timeout || cu_now() < end)) event_wait(&m->event, end, !timeout, &f);
    bool ok = m->count != 0;
    if (ok) {
        if (msg) *msg = m->data[m->head]; m->data[m->head] = NULL;
        m->head = (m->head + 1) % m->size; m->count--; event_wake(&m->event);
    }
    m->waiters--; sys_arch_unprotect(f);
    return ok ? (u32_t)(cu_now() - start) : SYS_ARCH_TIMEOUT;
}
void sys_mbox_free(sys_mbox_t *box)
{
    sys_prot_t f = sys_arch_protect(); struct cu_mbox *m = *box;
    if (m->waiters || m->count) { sys_arch_unprotect(f); ciuki_lwip_assert_fail("mailbox still referenced"); return; }
    event_free(&m->event); m->used = false; *box = NULL; sys_arch_unprotect(f);
}
static void thread_entry(void *arg)
{
    struct thread_start *t = arg; t->fn(t->arg);
#ifndef LWIP_HOST
    task_exit(0);
#endif
}
#ifdef LWIP_HOST
static void *host_entry(void *arg) { thread_entry(arg); return NULL; }
void cu_host_join(sys_thread_t id)
{
    struct thread_start *t = (void *)id; pthread_join(t->thread, NULL); t->used = false;
}
void cu_host_clock_offset(uint64_t value) { clock_offset = value; }
void cu_host_shutdown(void)
{
    for (unsigned i = 0; i < 7; i++) if (threads[i].used) pthread_cancel(threads[i].thread);
    for (unsigned i = 0; i < 7; i++) if (threads[i].used) cu_host_join((uintptr_t)&threads[i]);
}
#endif
sys_thread_t sys_thread_new(const char *name, lwip_thread_fn fn, void *arg, int stack, int priority)
{
    (void)stack; (void)priority;
    struct thread_start *t = NULL; sys_prot_t f = sys_arch_protect();
    for (unsigned i = 0; i < 7; i++) if (!threads[i].used) { t = &threads[i]; t->used = true; break; }
    sys_arch_unprotect(f); if (!t) return 0;
    t->fn = fn; t->arg = arg;
#ifdef LWIP_HOST
    (void)name;
    if (pthread_create(&t->thread, NULL, host_entry, t)) { t->used = false; return 0; }
    return (uintptr_t)t;
#else
    struct task *task = task_create_kernel(name, thread_entry, t,
        !strncmp(name, "tcpip_thread", 13) ? P_DEVICE : P_NORMAL);
    if (!task) { t->used = false; return 0; }
    task_start(task); return (uintptr_t)task;
#endif
}
void ciuki_lwip_mark_core(void) { core = cu_identity(); }
void ciuki_lwip_assert_core(void)
{
    if (core != cu_identity()) ciuki_lwip_assert_fail("wrong core owner");
#ifndef LWIP_HOST
    uint32_t flags; __asm__ volatile("pushfl; popl %0" : "=r"(flags));
    if (!(flags & 512)) ciuki_lwip_assert_fail("core called from IRQ/atomic context");
#endif
}
void ciuki_lwip_alive(void) { if (++core_batch == 16) { core_batch = 0; cu_yield(); } }
void ciuki_lwip_assert_fail(const char *message)
{
#ifdef LWIP_HOST
    fprintf(stderr, "lwIP assertion: %s\n", message); abort();
#else
    klog("[net] stack stopped assertion=%s", message); task_exit(-EINVAL);
#endif
}
void ciuki_lwip_seed(uint64_t sample)
{
    sys_prot_t f = sys_arch_protect(); random_state ^= sample + 0x9e3779b97f4a7c15ULL;
    random_state = (random_state ^ (random_state >> 30)) * 0xbf58476d1ce4e5b9ULL;
    sys_arch_unprotect(f);
}
uint32_t ciuki_lwip_rand(void)
{
    sys_prot_t f = sys_arch_protect(); uint64_t z = (random_state += 0x9e3779b97f4a7c15ULL);
#ifdef LWIP_HOST
    random_calls++;
#endif
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    sys_arch_unprotect(f); return (uint32_t)(z ^ (z >> 31));
}
unsigned cu_arch_bytes(void) { return sizeof(sems) + sizeof(mutexes) + sizeof(mboxes) + sizeof(threads); }
unsigned cu_mbox_bytes(void) { return sizeof(struct cu_mbox); }

static struct event changes;
static bool changes_initialized;
static void change_init(void)
{
    if (!changes_initialized) { event_init(&changes); changes_initialized = true; }
}
unsigned cu_change_snapshot(void)
{
    sys_prot_t f = sys_arch_protect(); change_init(); unsigned n = changes.sequence;
    sys_arch_unprotect(f); return n;
}
void cu_changed(void)
{
    sys_prot_t f = sys_arch_protect(); change_init(); event_wake(&changes); sys_arch_unprotect(f);
}
void cu_wait_change(unsigned sequence, uint64_t deadline)
{
    sys_prot_t f = sys_arch_protect(); change_init();
    if (changes.sequence == sequence && cu_now() < deadline) event_wait(&changes, deadline, false, &f);
    sys_arch_unprotect(f);
}

unsigned cu_mbox_count(sys_mbox_t m)
{
    sys_prot_t f = sys_arch_protect(); unsigned n = m ? m->count : 0; sys_arch_unprotect(f); return n;
}

int ciuki_lwip_atoi(const char *s)
{
    unsigned n = 0;
    for (; *s >= '0' && *s <= '9'; s++) { if (n > 255) return 0; n = n * 10 + (unsigned)(*s - '0'); }
    return *s || n > 255 ? 0 : (int)n;
}
int ciuki_lwip_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
