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
static inline void fs_lock_init(fs_lock *l) { if (pthread_mutex_init(l, 0)) abort(); }
static inline void fs_lock_destroy(fs_lock *l) { pthread_mutex_destroy(l); }
static inline void fs_lock_take(fs_lock *l) { if (pthread_mutex_lock(l)) abort(); }
static inline bool fs_lock_try_take(fs_lock *l) { return !pthread_mutex_trylock(l); }
static inline void fs_lock_drop(fs_lock *l) { if (pthread_mutex_unlock(l)) abort(); }
static inline void *fs_alloc(size_t n) { return n <= 2040 ? malloc(n) : NULL; }
static inline void fs_free(void *p) { free(p); }
static inline void *fs_page_alloc(void) { return calloc(1, 4096); }
static inline void fs_page_free(void *p) { free(p); }
static inline void fs_panic(const char *s) { (void)s; abort(); }
/* Mount-time parser workspaces (f4-04): zeroed, page-aligned, page-rounded.
 * Host tests make the Nth next allocation fail through fs_host_contig_fail. */
extern unsigned fs_host_contig_fail, fs_host_contig_live;
static inline void *fs_contig_alloc(size_t n) {
    if (fs_host_contig_fail && !--fs_host_contig_fail) return NULL;
    size_t bytes = (n + 4095) & ~(size_t)4095;
    void *p = bytes ? aligned_alloc(4096, bytes) : NULL;
    if (p) { memset(p, 0, bytes); fs_host_contig_live++; }
    return p;
}
static inline void fs_contig_free(void *p, size_t n) { (void)n; if (p) { fs_host_contig_live--; free(p); } }
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
#include <ciuki/sync.h>
/* Task-context mutex: test/set with IF clear, waiters BLOCK on a kwait queue
 * (f1-18 rule). A yield-spin livelocked the boot: the P_DEVICE storage worker
 * draining the boot log yielded only to its own priority while the P_NORMAL
 * boot task held the VFS lock reading /Programs/desktop (lead fix, 2026-10-11).
 * No filesystem entry point may run in an IRQ. The bounded wait guards a
 * missed wake-up, like the DOS VM disk gate. */
typedef struct { bool held; struct kwait wait; } fs_lock;
static inline bool fs_lock_free(void *arg) { return !((fs_lock *)arg)->held; }
static inline void fs_lock_init(fs_lock *l) { l->held = false; kwait_init(&l->wait); }
static inline void fs_lock_destroy(fs_lock *l) { (void)l; }
static inline bool fs_lock_try_take(fs_lock *l) {
    uint32_t f = irq_save(); bool free = !l->held;
    if (free) { l->held = true; kpreempt_hold(); }
    irq_restore(f); return free;
}
static inline void fs_lock_take(fs_lock *l) {
    for (;;) {
        uint32_t f = irq_save();
        if (!l->held) { l->held = true; kpreempt_hold(); irq_restore(f); return; }
        irq_restore(f);
        kwait_wait_until(&l->wait, fs_lock_free, l, deadline_after_ms(1000));
    }
}
static inline void fs_lock_drop(fs_lock *l) {
    uint32_t f = irq_save(); l->held = false; kpreempt_release(); irq_restore(f);
    kwait_wake_all(&l->wait);
}
static inline void *fs_alloc(size_t n) { return n <= 2040 ? kmalloc(n) : 0; }
static inline void fs_free(void *p) { kfree(p); }
static inline void *fs_page_alloc(void) {
    uint32_t p = pmm_alloc();
    if (!p) return 0;
    void *v = P2V(p); memset(v, 0, PAGE_SIZE); return v;
}
static inline void fs_page_free(void *p) { if (p) pmm_free(V2P(p)); }
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
