/* Frozen f1-01/ATA interface, v1. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_BLKDEV_H
#define CIUKI_BLKDEV_H
#include "fs_port.h"
#include <ciuki/ledger.h>
#include <ciuki/sync.h>
#define BLKDEV_SECTOR_SIZE 512u
#define BLKDEV_READ_BYTES 65536u
#define BLKDEV_READ_SECTORS (BLKDEV_READ_BYTES / BLKDEV_SECTOR_SIZE)
enum blkdev_cache_state { BLKDEV_CACHE_UNKNOWN, BLKDEV_CACHE_DISABLED, BLKDEV_CACHE_ENABLED };
struct blk_queue;
struct blkdev {
    /* One sector, bounded polling, no locks/allocation/scheduler. Optional
     * emergency crash recorder only; failure is never retried. */
    int (*emergency_write)(struct blkdev *, uint64_t, const void *);
    /* Synchronous, complete request or negative FS_EIO / FS_EINVAL /
     * FS_EQUARANTINED. No retry after an issued error. count is in sectors.
     * flush != NULL certifies a qualified durability implementation; returning
     * success means all earlier writes reached stable media. */
    int (*read)(struct blkdev *dev, uint64_t lba, uint32_t count, void *buf);
    int (*write)(struct blkdev *dev, uint64_t lba, uint32_t count, const void *buf);
    int (*flush)(struct blkdev *dev);
    int (*media_check)(struct blkdev *dev); /* optional, sleepable readiness */
    uint64_t capacity;                 /* number of sectors, not last LBA */
    uint32_t sector_size;              /* FAT/partition: 512; ATAPI: 2048 */
    uint32_t media_generation;         /* zero for fixed disks; never recycled */
    enum blkdev_cache_state write_cache_state;
    bool quarantined;
    /* A logical image provider already owns caching/coherence. FAT must not
     * retain duplicate data blocks in the outer physical-device cache. */
    bool logical;
    void *ctx;                        /* owned by the device implementation */
    /* Queue adapter. Existing callbacks remain synchronous; these lower
     * callbacks are called only by the controller's task-context executor. */
    struct blk_queue *queue;
    int (*request_read)(struct blkdev *, uint64_t, uint32_t, void *);
    int (*request_write)(struct blkdev *, uint64_t, uint32_t, const void *);
    int (*request_flush)(struct blkdev *);
    /* Offset-only views resolve before queue admission, preserving the
     * caller's priority/frontier and the parent controller's generation. */
    struct blkdev *request_parent;
    uint64_t request_offset;
};
static inline int blkdev_range(struct blkdev *d, uint64_t lba, uint32_t n) {
    if (!d || (d->sector_size != 512 && d->sector_size != 2048) || !n || lba >= d->capacity || n > d->capacity-lba) return -FS_EINVAL;
    return d->quarantined ? -FS_EQUARANTINED : 0;
}
static inline bool blkdev_durable(struct blkdev *d) {
    return d && d->write && !d->quarantined &&
        (d->write_cache_state == BLKDEV_CACHE_DISABLED ||
         (d->write_cache_state == BLKDEV_CACHE_ENABLED && d->flush));
}
#define BLKREQ_CAPACITY 32u
#define BLKREQ_COMMAND_BYTES 65536u
#define BLKREQ_PROMOTE_MS 100u
#define BLKREQ_READER_MS 20u
#define BLKREQ_PAGES 17u
enum blk_operation { BLK_READ, BLK_WRITE, BLK_FLUSH };
enum blk_priority { BLK_DEMAND, BLK_SPECULATIVE, BLK_BACKGROUND, BLK_PRIORITIES };
enum blk_state { BLK_FREE, BLK_PREPARED, BLK_QUEUED, BLK_ISSUED, BLK_COMPLETED, BLK_CANCELLED };
/* The caller owns buffer storage until completion, including cancellation
 * after issue. Pins are owned by the executing DMA adapter until stop proof. */
struct blk_page { uint32_t physical, bytes; const struct mm_pin *pin; };
struct blk_request {
    struct blk_request *next;
    struct blkdev *device;
    struct blk_queue *queue;
    void *buffer;
    uint64_t lba, frontier, order;
    uint32_t generation, length, bytes;
    uint32_t enqueued, started, completed, deadline, timeout, serviced;
    enum blk_operation operation;
    enum blk_priority priority;
    uintptr_t executor; /* pool lock: lower-layer synchronous adapter context */
    _Atomic(enum blk_state) state;
    int result;
    _Atomic(bool) cancel;
    /* Pin owner identity is explicit; zero never authorizes merging. A DMA
     * adapter supplies its generation-safe token(s), never an unlocked PFN. */
    const struct mm_pin *pin_owner;
    struct blk_page pages[BLKREQ_PAGES];
    unsigned page_count;
    struct fs_event event;
};
struct blk_queue_stats {
    uint32_t depth[BLK_PRIORITIES], oldest_ms, limit_bytes;
    uint64_t issued, merged, split, cancelled, timeouts, backpressure;
};
struct blk_queue {
    fs_lock lock;
    struct fs_event changed;
    struct blk_request *head, *active;
    struct blk_queue_stats stats;
    uint64_t sequence;
    unsigned requests; /* prepared through waiter release; queue lifetime pin */
    uint32_t last_record, max_bytes, limit_bytes;
    bool busy, stopped;
    struct task *worker;
    struct ktimer report_timer;
    uint32_t report_generation;
    const char *name;
};
void blk_queue_init(struct blk_queue *, const char *name, uint32_t max_bytes);
int blk_queue_start(struct blk_queue *); /* resident controller queues only */
int blk_queue_destroy(struct blk_queue *); /* only after all completions */
struct blk_request *blk_request_prepare(struct blkdev *, enum blk_operation,
    uint64_t lba, uint32_t bytes, void *, enum blk_priority, uint64_t frontier,
    uint32_t deadline, uint32_t timeout);
int blk_request_submit(struct blk_request *);
int blk_request_wait(struct blk_request *);
bool blk_request_cancel(struct blk_request *); /* issued writes may complete */
int blk_request_release(struct blk_request *);
#ifdef FS_HOST
/* Quiescent fixture boundary, no callers/workers; refuses unretired records.
 * Frees the host's resident pool and permits a later fixture to initialize it. */
int blk_request_pool_host_cleanup(void);
#endif
bool blk_queue_run_one(struct blk_queue *); /* task context, one command */
void blk_queue_cancel_device(struct blk_queue *, struct blkdev *);
void blk_queue_snapshot(struct blk_queue *, struct blk_queue_stats *);
void blk_queue_poll(struct blk_queue *);
int blkdev_request(struct blkdev *, enum blk_operation, uint64_t, uint32_t,
                   void *, enum blk_priority, uint64_t frontier);
int blkdev_read(struct blkdev *, uint64_t, uint32_t, void *);
int blkdev_write(struct blkdev *, uint64_t, uint32_t, const void *);
int blkdev_flush(struct blkdev *);
#endif
