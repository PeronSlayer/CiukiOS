/* F1 filesystem portability boundary. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_FS_PORT_H
#define CIUKI_FS_PORT_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <ciuki/kheap_limits.h>
/* Request context is task scoped and follows f1-83 calldowns, including a
 * controller worker. The caller keeps it alive until transport retirement. */
#define VFS_ETIMEOUT 205
#define VFS_ECANCELLED 206
enum vfs_wait_reason { VFS_WAIT_NAMESPACE=1,VFS_WAIT_VOLUME,VFS_WAIT_PROVIDER,VFS_WAIT_TRANSPORT,VFS_WAIT_CACHE };
struct vfs_wait_info { uint64_t owner,elapsed_us; uint32_t drives; enum vfs_wait_reason reason; int outcome; bool valid; };
struct fs_task_identity { char name[16]; uint32_t id; };
struct vfs_request {
    void *arg; uint64_t deadline;
    uint64_t (*now)(void *);
    bool (*cancelled)(void *);
    void (*pause)(void *);
    uint64_t owner; struct vfs_wait_info *wait;
    int *cleanup_status; /* first failed mutation cleanup, not ordinary DOS errors */
    struct fs_task_identity task; /* diagnostic caller across worker calldowns */
    const char *operation; /* short diagnostic tag, copied on admission */
    const char *site; /* static family: native, dos, writer, mount, vfs */
    bool urgent_reader; /* deadline-sensitive audio; never inferred from a filename */
    /* fx-23: idle-class maintenance I/O (the DOS image check, a background
     * free count). Its reads queue behind every other demand read, never
     * start read-ahead on the storage writer, and the blocks it fills or hits
     * stay at the cold end of the cache LRU (drop-behind), so a scan larger
     * than the cache cannot evict the foreground working set. Windows:
     * THREAD_MODE_BACKGROUND_BEGIN lowers I/O and memory priority as well as
     * CPU; Linux ionice -c3: disk time only when nobody else asked for it. */
    bool background;
};
struct fs_request_scope { struct fs_request_scope *next; uintptr_t task; const struct vfs_request *request; };
void fs_request_enter(struct fs_request_scope *,const struct vfs_request *);
void fs_request_leave(struct fs_request_scope *);
const struct vfs_request *fs_request_current(void);
/* Opaque identity of the calling task (pthread on the host). Used only to
 * recognise the owner of an f1-89 writer turn; never dereferenced. */
uintptr_t fs_task_token(void);
int fs_request_check(void);
/* Read through the platform RTC owner; never infer the NMI mask from a
 * read of port 70h (not a readable index register on the PC/AT). */
uint8_t fs_floppy_cmos_types(void);
uint8_t fs_cmos_read(uint8_t reg); /* fx-26: native only (no FS_HOST definition) */
/* Kernel APM broker only: one byte under the RTC lock, with the firmware's
 * full index/NMI byte applied temporarily and native index restored. */
uint8_t fs_cmos_bios_data(uint8_t index, bool write, uint8_t value);
void fs_request_wait(enum vfs_wait_reason,uint64_t begin,int outcome);
/* f1-98 (R2): a waiter that polls (request pause, event wait) for a drive's
 * reservation, guard or writer turn, or for a lock, names the holder (an
 * fs_task_token, 0 = none/withdraw) and the held object; the kernel lends the
 * holder the waiter's priority (sync.h ksync_depend). The holder calls
 * fs_depend_release(object) when it gives that object up, so the inherited
 * priority never outlives the hold. */
#define FS_DEPEND_DRIVE 1u
#define FS_DEPEND_CACHE 2u
#define FS_DEPEND_IO 3u /* a block request's executor task (blkreq.c) */
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
/* Host: observers only (fs_port.c); a pthread mutex names no owner. */
void fs_depend(uintptr_t holder,const void *object,unsigned reason);
void fs_depend_release(const void *object);
void fs_host_depend(uintptr_t holder,const void *object,unsigned reason) __attribute__((weak));
void fs_host_depend_release(const void *object) __attribute__((weak));
static inline uintptr_t fs_lock_holder(fs_lock *l) { (void)l; return 0; }
static inline unsigned fs_lock_reason(fs_lock *l) { (void)l; return FS_DEPEND_DRIVE; }
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
/* As fs_event_wait, but returns after `ms` without a wake. */
static inline void fs_event_wait_ms(struct fs_event *q, fs_lock *l, uint32_t ms) {
    uint32_t seq=__atomic_load_n(&q->sequence,__ATOMIC_ACQUIRE);
    struct timespec until; clock_gettime(CLOCK_REALTIME,&until);
    until.tv_sec+=ms/1000; until.tv_nsec+=(long)(ms%1000)*1000000L;
    if (until.tv_nsec>=1000000000L) { until.tv_sec++; until.tv_nsec-=1000000000L; }
    fs_lock_drop(l); pthread_mutex_lock(&q->mutex);
    while (seq==__atomic_load_n(&q->sequence,__ATOMIC_ACQUIRE) &&
           pthread_cond_timedwait(&q->changed,&q->mutex,&until)==0) {}
    pthread_mutex_unlock(&q->mutex); fs_lock_take(l);
}
static inline void *fs_alloc(size_t n) { return n <= KHEAP_MAX_REQUEST ? malloc(n) : NULL; }
static inline void fs_free(void *p) { free(p); }
extern unsigned fs_host_page_fail;
void *fs_page_alloc(void);
void *fs_normal_page_alloc(void);
void fs_page_free(void *);
/* Configure a synthetic E820 map only with no outstanding allocations.
 * The default map is the architectural upper bound, not hardware proof. */
void fs_host_memory_configure(unsigned normal_pages, unsigned dma_pages);
unsigned fs_host_memory_live(void);
unsigned fs_host_memory_pfn(const void *);
static inline void fs_panic(const char *s) { (void)s; abort(); }
/* Mount-time parser workspaces (f4-04): zeroed, page-aligned, page-rounded.
 * Host tests make the Nth next allocation fail through fs_host_contig_fail. */
extern unsigned fs_host_contig_fail, fs_host_contig_live;
void *fs_contig_alloc(size_t n);
void fs_contig_free(void *, size_t n);
uint32_t fs_host_clock_ms(void) __attribute__((weak)); /* deterministic fixture boundary */
static inline uint32_t fs_now_ms(void) {
    if (fs_host_clock_ms) return fs_host_clock_ms();
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
static inline void fs_lock_owner_died(struct kwait *q) {
    fs_lock *l=(fs_lock *)((char *)q - __builtin_offsetof(fs_lock,wait));
    l->held=false; l->serving++;
}
static inline void fs_lock_ticket_cancelled(struct kwait *q) {
    fs_lock *l=(fs_lock *)((char *)q - __builtin_offsetof(fs_lock,wait));
    l->next--;
}
static inline void fs_lock_init(fs_lock *l) {
    memset(l,0,sizeof(*l)); kwait_init(&l->wait); l->wait.owner_died=fs_lock_owner_died;
    l->wait.ticket_cancelled=fs_lock_ticket_cancelled;
}
static inline void fs_lock_destroy(fs_lock *l) { (void)l; }
static inline bool fs_lock_try_take(fs_lock *l) {
    uint32_t f=irq_save(); bool free=l->next==l->serving;
    if (free) { l->next++; l->held=true; kwait_set_owner(&l->wait,g_current); kpreempt_hold(); }
    irq_restore(f); return free;
}
/* T23 trial 2ca3b0b6: a task still waiting for a filesystem lock after
 * FS_LOCK_LONG_WAIT_MS is named once per wait, with the lock's rank and
 * drive, its owner (none while a served ticket waits to run) and tickets
 * (fs_port.c; weak: harnesses without it skip). Diagnostic only. */
#define FS_LOCK_LONG_WAIT_MS 5000u
void fs_lock_long_wait(fs_lock *,uint32_t ticket,uint32_t waited_ms,bool polling) __attribute__((weak));
static inline void fs_lock_take(fs_lock *l) {
    uint32_t f=irq_save(); struct fs_ticket t={l,l->next++}; irq_restore(f);
    uint32_t start=0; bool started=false, reported=false;
    for (;;) {
        f=irq_save();
        if (fs_ticket_ready(&t)) { l->held=true; kwait_set_owner(&l->wait,g_current); kpreempt_hold(); irq_restore(f); return; }
        irq_restore(f);
        /* The wait's own clock (deadline_after_ms(0): the tick now), taken
         * once; the flag, not the value, says it was taken. */
        if (!started) { start=(uint32_t)deadline_after_ms(0); started=true; }
        kwait_wait_ticket_until(&l->wait,fs_ticket_ready,&t,deadline_after_ms(1000),&t.ticket);
        uint32_t waited=(uint32_t)deadline_after_ms(0)-start;
        if (!reported && fs_lock_long_wait && waited>=FS_LOCK_LONG_WAIT_MS) {
            reported=true; fs_lock_long_wait(l,t.ticket,waited,false);
        }
    }
}
static inline void fs_depend(uintptr_t holder,const void *object,unsigned reason) {
    if (ksync_depend) ksync_depend((struct task *)holder,object,reason);
}
static inline void fs_depend_release(const void *object) {
    if (ksync_depend_release) ksync_depend_release(object);
}
/* Edges to an explicitly named holder (blkreq.c: a request's executor). */
static inline void fs_depend_revoke(uintptr_t holder,const void *object) {
    if (ksync_depend_revoke) ksync_depend_revoke((struct task *)holder,object);
}
static inline uintptr_t fs_lock_holder(fs_lock *l) { return (uintptr_t)l->wait.owner; }
/* Rank 3 is the block cache (cache.c); namespace and volume locks guard a
 * drive's reservations. */
static inline unsigned fs_lock_reason(fs_lock *l) { return l->rank==3 ? FS_DEPEND_CACHE : FS_DEPEND_DRIVE; }
static inline void fs_lock_drop(fs_lock *l) {
    uint32_t f=irq_save(); l->held=false; l->serving++; kwait_set_owner(&l->wait,0); kpreempt_release(); irq_restore(f);
    fs_depend_release(l); /* f1-98: polling requests stop lending their priority */
    kwait_wake_all(&l->wait);
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
static inline void fs_event_wait_ms(struct fs_event *q, fs_lock *l, uint32_t ms) {
    struct fs_event_ticket t={q,q->sequence}; fs_lock_drop(l);
    kwait_wait_until(&q->wait,fs_event_changed,&t,deadline_after_ms(ms)); fs_lock_take(l);
}
static inline void *fs_alloc(size_t n) { return n <= KHEAP_MAX_REQUEST ? kmalloc(n) : 0; }
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
    if (!n || n>(size_t)(PMM_MAX_PAGES-PMM_NORMAL_START_PFN)*PAGE_SIZE) return 0;
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
static inline struct fs_task_identity fs_request_identity(const struct vfs_request *r) {
    struct fs_task_identity out={0}; const char *name="host";
    if (r && r->task.name[0]) { name=r->task.name; out.id=r->task.id; }
#ifndef FS_HOST
    else if (g_current) { name=g_current->name; out.id=g_current->id; }
    else name="boot";
#endif
    for (unsigned i=0;i<sizeof(out.name)-1 && name[i];i++) {
        unsigned char c=(unsigned char)name[i];
        out.name[i]=(c>='a' && c<='z') || (c>='A' && c<='Z') ||
            (c>='0' && c<='9') || c=='-' || c=='_' || c=='.' ? (char)c : '_';
    }
    return out;
}
static inline int fs_lock_take_request(fs_lock *lock,enum vfs_wait_reason reason) {
    const struct vfs_request *r=fs_request_current();
    if (!r) { fs_lock_take(lock); return 0; }
    uint64_t begin=r->now(r->arg); int error=fs_request_check(); bool blocked=false;
#ifndef FS_HOST
    uint32_t start=(uint32_t)deadline_after_ms(0); bool reported=false;
#endif
    while (!error && !fs_lock_try_take(lock)) {
        blocked=true; fs_depend(fs_lock_holder(lock),lock,fs_lock_reason(lock));
        r->pause(r->arg); error=fs_request_check();
#ifndef FS_HOST
        uint32_t waited=(uint32_t)deadline_after_ms(0)-start;
        if (!reported && fs_lock_long_wait && waited>=FS_LOCK_LONG_WAIT_MS) {
            reported=true; fs_lock_long_wait(lock,0,waited,true);
        }
#endif
    }
    if (blocked) { fs_depend(0,0,0); fs_request_wait(reason,begin,error); }
    return error;
}
/* A published reservation/reference must retire under its bookkeeping lock.
 * Cancellation is observed during reacquisition; no further storage is issued.
 * Namespace/cache critical sections never sleep in production. */
static inline int fs_lock_retake(fs_lock *lock,enum vfs_wait_reason reason) {
    int error=fs_lock_take_request(lock,reason);
    if (error) {
        const struct vfs_request *r=fs_request_current();
        bool blocked=false;
        while (!fs_lock_try_take(lock)) {
            blocked=true; fs_depend(fs_lock_holder(lock),lock,fs_lock_reason(lock)); r->pause(r->arg);
        }
        if (blocked) fs_depend(0,0,0);
    }
    return error;
}
static inline int fs_event_wait_request(struct fs_event *event,fs_lock *lock,enum vfs_wait_reason reason) {
    const struct vfs_request *r=fs_request_current();
    if (!r) { fs_event_wait(event,lock); return 0; }
    uint64_t begin=r->now(r->arg);
    fs_lock_drop(lock); r->pause(r->arg);
    int error=fs_lock_retake(lock,reason);
    if (!error) error=fs_request_check();
    fs_request_wait(reason,begin,error); return error;
}
/* Install only at boot/quiescence. F0 exports monotonic PIT time, no RTC.
 * A calendar provider supplies packed DOS date/time; absence uses 1980-01-01.
 * The same hook is used by the host, keeping FAT independent of the target. */
typedef bool (*fs_calendar_clock)(uint16_t *date, uint16_t *time, uint8_t *tenths);
void fs_set_calendar_clock(fs_calendar_clock);
void fs_set_timestamp_clock(fs_calendar_clock);
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
struct fs_rtc_writer { struct fs_rtc_ops sample; void (*write)(void *, uint8_t, uint8_t); };
bool fs_rtc_write(const struct fs_rtc_writer *, void *, uint16_t, uint16_t, uint8_t);
bool fs_rtc_store(uint16_t, uint16_t, uint8_t);
void fs_calendar_init(void);
/* Long cache/scan loops cooperate with the scheduler. Only the storage
 * callback brackets worker context; ordinary VFS callers use task_yield. */
void fs_worker_enter(void);
void fs_worker_leave(void);
void fs_service(void);
#ifdef FS_HOST
/* f1-93b: optional host observer of every fs_service point (the kernel's
 * only preemption point inside filesystem work). */
void fs_host_service(void) __attribute__((weak));
#endif
/* No dependency on the host's errno ABI. All public failures are NEGATIVE. */
enum fs_error {
    FS_EAGAIN=11, FS_EIO=5, FS_ENOENT=2, FS_EBADF=9, FS_ENOMEM=12, FS_EACCES=13,
    FS_EBUSY=16, FS_EEXIST=17, FS_EXDEV=18, FS_ENOTDIR=20, FS_EISDIR=21,
    FS_EINVAL=22, FS_EMFILE=24, FS_EFBIG=27, FS_ENOSPC=28, FS_EROFS=30,
    FS_ENAMETOOLONG=36, FS_ENOTEMPTY=39, FS_ELOOP=40, FS_EILSEQ=84,
    FS_EOPNOTSUPP=95, FS_EUCLEAN=117, FS_EQUARANTINED=200,
    /* fx-emptydrive: the letter belongs to a removable-media drive (CD-ROM,
     * floppy) with no medium mounted: Win32 ERROR_NOT_READY, DOS 15h. */
    FS_ENXIO=6
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
