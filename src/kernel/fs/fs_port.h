/* F1 filesystem portability boundary. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_FS_PORT_H
#define CIUKI_FS_PORT_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#ifdef FS_HOST
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
typedef pthread_mutex_t fs_lock;
void fs_lock_before(fs_lock *) __attribute__((weak));
void fs_lock_acquired(fs_lock *) __attribute__((weak));
void fs_lock_released(fs_lock *) __attribute__((weak));
void fs_lock_classified(fs_lock *, unsigned, char) __attribute__((weak));
static inline void fs_lock_classify(fs_lock *l,unsigned rank,char drive) {
    if (fs_lock_classified) fs_lock_classified(l,rank,drive);
}
static inline void fs_lock_init(fs_lock *l) { if (pthread_mutex_init(l,0)) abort(); }
static inline void fs_lock_destroy(fs_lock *l) {
    fs_lock_classify(l,0,0); pthread_mutex_destroy(l);
}
static inline void fs_lock_take(fs_lock *l) {
    if (fs_lock_before) fs_lock_before(l);
    if (pthread_mutex_lock(l)) abort();
    if (fs_lock_acquired) fs_lock_acquired(l);
}
static inline bool fs_lock_try_take(fs_lock *l) {
    bool free=!pthread_mutex_trylock(l);
    if (free && fs_lock_acquired) fs_lock_acquired(l);
    return free;
}
static inline void fs_lock_drop(fs_lock *l) {
    if (fs_lock_released) fs_lock_released(l);
    if (pthread_mutex_unlock(l)) abort();
}
struct fs_event { pthread_mutex_t mutex; pthread_cond_t changed; uint32_t sequence; };
static inline void fs_event_init(struct fs_event *q) {
    memset(q,0,sizeof(*q)); if (pthread_mutex_init(&q->mutex,0) || pthread_cond_init(&q->changed,0)) abort();
}
static inline void fs_event_destroy(struct fs_event *q) { pthread_cond_destroy(&q->changed); pthread_mutex_destroy(&q->mutex); }
static inline void fs_event_wake(struct fs_event *q) {
    pthread_mutex_lock(&q->mutex); __atomic_add_fetch(&q->sequence,1,__ATOMIC_RELEASE);
    pthread_cond_broadcast(&q->changed); pthread_mutex_unlock(&q->mutex);
}
static inline void fs_event_wait(struct fs_event *q, fs_lock *l) {
    uint32_t seq=__atomic_load_n(&q->sequence,__ATOMIC_ACQUIRE);
    fs_lock_drop(l); pthread_mutex_lock(&q->mutex);
    while (seq==__atomic_load_n(&q->sequence,__ATOMIC_ACQUIRE)) pthread_cond_wait(&q->changed,&q->mutex);
    pthread_mutex_unlock(&q->mutex); fs_lock_take(l);
}
static inline void *fs_alloc(size_t n) { return n <= 2040 ? malloc(n) : NULL; }
static inline void fs_free(void *p) { free(p); }
extern unsigned fs_host_page_fail;
static inline void *fs_page_alloc(void) {
    if (fs_host_page_fail && !--fs_host_page_fail) return NULL;
    return calloc(1, 4096);
}
static inline void *fs_normal_page_alloc(void) { return fs_page_alloc(); }
static inline void fs_page_free(void *p) { free(p); }
static inline void fs_panic(const char *s) { (void)s; abort(); }
/* Mount-time parser workspaces (f4-04): zeroed, page-aligned, page-rounded.
 * Host tests make the Nth next allocation fail through fs_host_contig_fail. */
extern unsigned fs_host_contig_fail, fs_host_contig_live;
static inline void *fs_contig_alloc(size_t n) {
    if (fs_host_contig_fail && !--fs_host_contig_fail) return NULL;
    size_t bytes = (n + 4095) & ~(size_t)4095;
    void *p = bytes ? aligned_alloc(4096, bytes) : NULL;
    if (p) { memset(p, 0, bytes); __atomic_add_fetch(&fs_host_contig_live,1,__ATOMIC_RELAXED); }
    return p;
}
static inline void fs_contig_free(void *p, size_t n) { (void)n; if (p) { __atomic_sub_fetch(&fs_host_contig_live,1,__ATOMIC_RELAXED); free(p); } }
static inline uint32_t fs_now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((uint64_t)t.tv_sec * 1000 + (uint32_t)t.tv_nsec / 1000000);
}
#else
#include <ciuki/kernel.h>
#include <ciuki/mm.h>
#include <ciuki/cpu.h>
#include <ciuki/task.h>
#include <ciuki/sync.h>
/* Task-context FIFO mutex. Ticket allocation/test and release use IF=0;
 * blocked waiters retain their ticket across the bounded safety wait.
 * A preemption hold is acquired only when served, released on unlock. */
typedef struct { uint32_t next, serving; struct kwait wait; unsigned rank; char drive; bool held; } fs_lock;
static inline void fs_lock_classify(fs_lock *l,unsigned rank,char drive) { l->rank=rank; l->drive=drive; }
struct fs_ticket { fs_lock *lock; uint32_t ticket; };
static inline bool fs_ticket_ready(void *arg) {
    struct fs_ticket *t=arg; return t->lock->serving==t->ticket;
}
static inline void fs_lock_init(fs_lock *l) { memset(l,0,sizeof(*l)); kwait_init(&l->wait); }
static inline void fs_lock_destroy(fs_lock *l) { (void)l; }
static inline bool fs_lock_try_take(fs_lock *l) {
    uint32_t f=irq_save(); bool free=l->next==l->serving;
    if (free) { l->next++; l->held=true; kpreempt_hold(); }
    irq_restore(f); return free;
}
static inline void fs_lock_take(fs_lock *l) {
    uint32_t f=irq_save(); struct fs_ticket t={l,l->next++}; irq_restore(f);
    for (;;) {
        f=irq_save();
        if (fs_ticket_ready(&t)) { l->held=true; kpreempt_hold(); irq_restore(f); return; }
        irq_restore(f);
        kwait_wait_until(&l->wait,fs_ticket_ready,&t,deadline_after_ms(1000));
    }
}
static inline void fs_lock_drop(fs_lock *l) {
    uint32_t f=irq_save(); l->held=false; l->serving++; kpreempt_release(); irq_restore(f); kwait_wake_all(&l->wait);
}
struct fs_event { struct kwait wait; uint32_t sequence; };
struct fs_event_ticket { struct fs_event *event; uint32_t sequence; };
static inline bool fs_event_changed(void *arg) {
    struct fs_event_ticket *t=arg; return t->event->sequence!=t->sequence;
}
static inline void fs_event_init(struct fs_event *q) { q->sequence=0; kwait_init(&q->wait); }
static inline void fs_event_destroy(struct fs_event *q) { (void)q; }
static inline void fs_event_wake(struct fs_event *q) {
    uint32_t f=irq_save(); q->sequence++; irq_restore(f); kwait_wake_all(&q->wait);
}
static inline void fs_event_wait(struct fs_event *q, fs_lock *l) {
    struct fs_event_ticket t={q,q->sequence}; fs_lock_drop(l);
    kwait_wait_until(&q->wait,fs_event_changed,&t,deadline_after_ms(1000)); fs_lock_take(l);
}
static inline void *fs_alloc(size_t n) { return n <= 2040 ? kmalloc(n) : 0; }
static inline void fs_free(void *p) { kfree(p); }
static inline void *fs_page_alloc(void) {
    uint32_t p = pmm_alloc();
    if (!p) return 0;
    void *v = P2V(p); memset(v, 0, PAGE_SIZE); return v;
}
static inline void fs_page_free(void *p) { if (p) pmm_free(V2P(p)); }
static inline void *fs_normal_page_alloc(void) {
    uint32_t p=pmm_alloc_zone(ZONE_NORMAL);
    if (!p) return 0;
    void *v=P2V(p); memset(v,0,PAGE_SIZE); return v;
}
static inline void fs_panic(const char *s) { panic("fs: %s", s); }
/* Mount-time parser workspaces (f4-04): zeroed contiguous pages from the
 * physical allocator, reached through the direct map. */
static inline void *fs_contig_alloc(size_t n) {
    uint32_t pages = (uint32_t)((n + PAGE_SIZE - 1) >> 12);
    uint32_t p = pages ? pmm_alloc_contig(pages) : 0;
    if (!p) return 0;
    void *v = P2V(p); memset(v, 0, (size_t)pages << 12); return v;
}
static inline void fs_contig_free(void *v, size_t n) {
    uint32_t pages = (uint32_t)((n + PAGE_SIZE - 1) >> 12);
    if (!v) return;
    uint32_t p = V2P(v);
    for (uint32_t i = 0; i < pages; i++) pmm_free(p + (i << 12));
}
static inline uint32_t fs_now_ms(void) { return ticks_lo(); }
#endif
/* Install only at boot/quiescence. F0 exports monotonic PIT time, no RTC.
 * A calendar provider supplies packed DOS date/time; absence uses 1980-01-01.
 * The same hook is used by the host, keeping FAT independent of the target. */
typedef bool (*fs_calendar_clock)(uint16_t *date, uint16_t *time, uint8_t *tenths);
void fs_set_calendar_clock(fs_calendar_clock);
void fs_timestamp(uint16_t *date, uint16_t *time, uint8_t *tenths);
/* Read-only MC146818 boundary. begin/end serialize a single sample, never
 * the timeout; read must not acknowledge register C or write CMOS data. */
struct fs_rtc_ops {
    void (*begin)(void *);
    void (*end)(void *);
    uint8_t (*read)(void *, uint8_t);
    uint32_t (*now_ms)(void *);
    bool (*pause)(void *);
};
struct fs_rtc_sample { uint8_t second, minute, hour, day, month, year, control, valid; };
bool fs_rtc_decode(const struct fs_rtc_sample *, uint16_t *, uint16_t *, uint8_t *);
bool fs_rtc_read(const struct fs_rtc_ops *, void *, uint16_t *, uint16_t *, uint8_t *);
void fs_calendar_init(void);
/* Long cache/scan loops cooperate with the scheduler. Only the storage
 * callback brackets worker context; ordinary VFS callers use task_yield. */
void fs_worker_enter(void);
void fs_worker_leave(void);
void fs_service(void);
/* No dependency on the host's errno ABI. All public failures are NEGATIVE. */
enum fs_error {
    FS_EAGAIN=11, FS_EIO=5, FS_ENOENT=2, FS_EBADF=9, FS_ENOMEM=12, FS_EACCES=13,
    FS_EBUSY=16, FS_EEXIST=17, FS_EXDEV=18, FS_ENOTDIR=20, FS_EISDIR=21,
    FS_EINVAL=22, FS_EMFILE=24, FS_EFBIG=27, FS_ENOSPC=28, FS_EROFS=30,
    FS_ENAMETOOLONG=36, FS_ENOTEMPTY=39, FS_ELOOP=40, FS_EILSEQ=84,
    FS_EOPNOTSUPP=95, FS_EUCLEAN=117, FS_EQUARANTINED=200
};
static inline uint16_t fs_rd16(const void *v) {
    const uint8_t *p=v; return (uint16_t)(p[0] | (uint16_t)p[1]<<8);
}
static inline uint32_t fs_rd32(const void *v) {
    const uint8_t *p=v; return p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static inline void fs_wr16(void *v, uint16_t x) { uint8_t *p=v; p[0]=(uint8_t)x; p[1]=(uint8_t)(x>>8); }
static inline void fs_wr32(void *v, uint32_t x) { uint8_t *p=v; fs_wr16(p,(uint16_t)x); fs_wr16(p+2,(uint16_t)(x>>16)); }
#endif
