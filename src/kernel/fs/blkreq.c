/* Bounded task-context request arbitration. The existing IRQ transport waiter
 * is the executor's deferred continuation; no filesystem/pin work in IRQs.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "blkdev.h"
#include <ciuki/ledger.h>
#ifndef FS_HOST
#include <ciuki/kernel.h>
#endif

#ifdef FS_HOST
static pthread_once_t pool_once=PTHREAD_ONCE_INIT;
static fs_lock pool_lock=PTHREAD_MUTEX_INITIALIZER;
#else
static fs_lock pool_lock;
static bool pool_initialized;
#endif
static struct blk_request *pool;
void blk_queue_record(const char *,const struct blk_queue_stats *) __attribute__((weak));
static void pool_init(void) {
#ifndef FS_HOST
    fs_lock_init(&pool_lock);
    pool=fs_contig_alloc(sizeof(*pool)*BLKREQ_CAPACITY);
    if (pool) mm_claim(V2P(pool),(sizeof(*pool)*BLKREQ_CAPACITY+4095)/4096,MM_IO_REQUEST);
#else
    pool=calloc(BLKREQ_CAPACITY,sizeof(*pool));
#endif
    if (pool) for (unsigned i=0;i<BLKREQ_CAPACITY;i++) fs_event_init(&pool[i].event);
}
static void initialize(void) {
#ifdef FS_HOST
    pthread_once(&pool_once,pool_init);
#else
    /* Boot and all allocation-capable callers run on this single CPU. Mark
     * before allocation; an interrupt never enters request preparation. */
    if (!pool_initialized) { pool_initialized=true; pool_init(); }
#endif
}
static bool terminal(const struct blk_request *r) {
    return r->state==BLK_COMPLETED || r->state==BLK_CANCELLED;
}
#ifndef FS_HOST
static void queue_timer(void *,uint32_t,unsigned);
static void queue_worker(void *arg) {
    struct blk_queue *q=arg;
    for (;;) {
        if (blk_queue_run_one(q)) { task_yield(); continue; }
        fs_lock_take(&q->lock);
        if (q->stopped && !q->head && !q->busy) {
            if (!ktimer_cancel(&q->report_timer)) {
                fs_lock_drop(&q->lock); task_yield(); continue;
            }
            q->worker=0; fs_event_wake(&q->changed); fs_lock_drop(&q->lock); task_exit(0);
        }
        fs_event_wait(&q->changed,&q->lock); fs_lock_drop(&q->lock);
    }
}
#endif
void blk_queue_init(struct blk_queue *q,const char *name,uint32_t max_bytes) {
    memset(q,0,sizeof(*q)); fs_lock_init(&q->lock); fs_event_init(&q->changed);
    q->name=name; q->max_bytes=max_bytes && max_bytes<BLKREQ_COMMAND_BYTES ? max_bytes : BLKREQ_COMMAND_BYTES;
    q->limit_bytes=q->max_bytes; q->last_record=fs_now_ms();
}
int blk_queue_start(struct blk_queue *q) {
    initialize(); if (!pool) return -FS_ENOMEM;
#ifndef FS_HOST
    if (q->worker) return 0;
    q->worker=task_create_kernel("storage-io",queue_worker,q,P_NORMAL);
    if (!q->worker) return -FS_ENOMEM;
    q->report_generation=q->worker->generation;
    task_start(q->worker);
    ktimer_arm(&q->report_timer,deadline_after_ms(5000),0,q,&q->report_generation,queue_timer);
#else
    (void)q;
#endif
    return 0;
}
int blk_queue_destroy(struct blk_queue *q) {
    fs_lock_take(&q->lock);
    if (q->busy || q->head || q->requests) { fs_lock_drop(&q->lock); return -FS_EBUSY; }
    q->stopped=true; fs_event_wake(&q->changed);
    while (q->worker) fs_event_wait(&q->changed,&q->lock);
    fs_lock_drop(&q->lock);
    fs_event_destroy(&q->changed); fs_lock_destroy(&q->lock); return 0;
}
struct blk_request *blk_request_prepare(struct blkdev *d,enum blk_operation op,
    uint64_t lba,uint32_t bytes,void *buf,enum blk_priority priority,uint64_t frontier,
    uint32_t deadline,uint32_t timeout) {
    if (!d || (unsigned)op>BLK_FLUSH || (unsigned)priority>=BLK_PRIORITIES || !timeout ||
        timeout>INT32_MAX || (op==BLK_FLUSH ? bytes!=0 :
        (!buf || !bytes || !d->sector_size || bytes%d->sector_size ||
         blkdev_range(d,lba,bytes/d->sector_size)))) return 0;
    initialize(); if (!pool) return 0;
    struct blk_queue *q=d->queue;
    if (q) {
        fs_lock_take(&q->lock);
        if (q->stopped) { fs_lock_drop(&q->lock); return 0; }
    }
    fs_lock_take(&pool_lock); struct blk_request *r=0;
    for (unsigned i=0;i<BLKREQ_CAPACITY;i++) if (pool[i].state==BLK_FREE) {
        r=&pool[i]; memset(r,0,offsetof(struct blk_request,event));
        r->device=d; r->queue=d->queue; r->operation=op; r->lba=lba;
        r->length=bytes; r->buffer=buf; r->generation=d->media_generation;
        r->priority=priority; r->frontier=frontier; r->deadline=deadline;
        r->timeout=timeout; r->state=BLK_PREPARED; break;
    }
    fs_lock_drop(&pool_lock);
    if (q) {
        if (r) q->requests++; else q->stats.backpressure++;
        fs_lock_drop(&q->lock);
    }
    return r;
}
static void finish(struct blk_request *r,int e) {
    /* Called by the thread executor only, after the lower layer proved DMA
     * stopped and retired its pins. An issued write's real result wins over
     * a cancellation request; cancellation cannot claim it never happened. */
    r->result=e; r->completed=fs_now_ms();
    r->state=(r->cancel && !r->bytes) ? BLK_CANCELLED : BLK_COMPLETED;
#ifdef FS_HOST
    fs_event_wake(&r->event);
#else
    uint32_t flags=irq_save(); r->event.sequence++; irq_restore(flags);
    kwait_wake_io(&r->event.wait);
#endif
}
static int execute_raw(struct blk_request *r,uint32_t bytes) {
    struct blkdev *d=r->device;
    if (d->media_generation!=r->generation || d->quarantined) return -FS_EIO;
    if (r->operation==BLK_FLUSH) {
        int (*fn)(struct blkdev *)=d->request_flush ? d->request_flush : d->flush;
        return fn ? fn(d) : -FS_EROFS;
    }
    uint32_t n=bytes/d->sector_size;
    if (r->operation==BLK_READ) {
        int (*fn)(struct blkdev *,uint64_t,uint32_t,void *)=d->request_read ? d->request_read : d->read;
        return fn ? fn(d,r->lba+r->bytes/d->sector_size,n,(uint8_t *)r->buffer+r->bytes) : -FS_EINVAL;
    }
    int (*fn)(struct blkdev *,uint64_t,uint32_t,const void *)=d->request_write ? d->request_write : d->write;
    return fn ? fn(d,r->lba+r->bytes/d->sector_size,n,(uint8_t *)r->buffer+r->bytes) : -FS_EROFS;
}
static uintptr_t executor_id(void) {
#ifdef FS_HOST
    return (uintptr_t)pthread_self();
#else
    return (uintptr_t)g_current;
#endif
}
static int execute(struct blk_request *r,uint32_t bytes) {
    fs_lock_take(&pool_lock); r->executor=executor_id(); fs_lock_drop(&pool_lock);
    int result=execute_raw(r,bytes);
    if (!result && (r->device->media_generation!=r->generation || r->device->quarantined)) result=-FS_EIO;
    fs_lock_take(&pool_lock); r->executor=0; fs_lock_drop(&pool_lock);
    return result;
}
int blk_request_submit(struct blk_request *r) {
    if (!r || r->state!=BLK_PREPARED) return -FS_EINVAL;
    struct blk_queue *q=r->queue; r->enqueued=fs_now_ms(); r->serviced=r->enqueued;
    if (!q) { r->state=BLK_QUEUED; return 0; }
    fs_lock_take(&q->lock);
    if (q->stopped) { fs_lock_drop(&q->lock); return -FS_EIO; }
    r->order=++q->sequence; r->state=BLK_QUEUED;
    struct blk_request **at=&q->head; while (*at) at=&(*at)->next;
    *at=r; fs_event_wake(&q->changed); fs_lock_drop(&q->lock); return 0;
}
/* A flush/dependency frontier fences only its own device/generation. Demand
 * reads of unrelated files can pass writes, never a dependency predecessor. */
static bool eligible(struct blk_queue *q,struct blk_request *r) {
    for (struct blk_request *p=q->head;p && p!=r;p=p->next) {
        if (p->device!=r->device || p->generation!=r->generation) continue;
        if (p->operation==BLK_FLUSH || r->operation==BLK_FLUSH ||
            (r->operation==BLK_WRITE && p->operation==BLK_WRITE && p->frontier!=r->frontier)) return false;
        if (r->operation!=BLK_FLUSH && p->operation!=BLK_FLUSH) {
            uint64_t a=p->lba+p->bytes/p->device->sector_size,b=r->lba+r->bytes/r->device->sector_size;
            uint32_t pn=(p->length-p->bytes)/p->device->sector_size,rn=(r->length-r->bytes)/r->device->sector_size;
            if (a<b+rn && b<a+pn && (p->operation==BLK_WRITE || r->operation==BLK_WRITE)) return false;
        }
    }
    return true;
}
static unsigned rank(struct blk_queue *q,struct blk_request *r,uint32_t now) {
    if (now-(r->bytes ? r->serviced : r->enqueued)>=BLKREQ_PROMOTE_MS) return 0;
    if (r->operation!=BLK_READ) {
        bool after=false,barrier=r->operation==BLK_FLUSH;
        for (struct blk_request *p=q->head;p;p=p->next) {
            if (p==r) { after=true; continue; }
            if (!after || p->device!=r->device || p->generation!=r->generation) continue;
            if (p->operation==BLK_FLUSH) barrier=true;
            if (p->priority==BLK_DEMAND) {
                uint64_t a=r->lba+r->bytes/r->device->sector_size,b=p->lba+p->bytes/p->device->sector_size;
                uint32_t an=(r->length-r->bytes)/r->device->sector_size,bn=(p->length-p->bytes)/p->device->sector_size;
                if (barrier || (a<b+bn && b<a+an)) return BLK_DEMAND+1;
            }
        }
    }
    return (unsigned)r->priority+1;
}
static bool better(struct blk_queue *q,struct blk_request *a,struct blk_request *b,uint32_t now) {
    if (!b) return true;
    unsigned ar=rank(q,a,now),br=rank(q,b,now);
    if (ar!=br) return ar<br;
    if (ar==BLK_DEMAND+1 && a->deadline && b->deadline && a->deadline!=b->deadline)
        return (int32_t)(a->deadline-b->deadline)<0;
    return a->order<b->order;
}
static void unlink_request(struct blk_queue *q,struct blk_request *r) {
    struct blk_request **at=&q->head; while (*at && *at!=r) at=&(*at)->next;
    if (*at) *at=r->next;
    r->next=0;
}
bool blk_queue_run_one(struct blk_queue *q) {
    fs_lock_take(&q->lock);
    if (q->busy || !q->head) { fs_lock_drop(&q->lock); return false; }
    uint32_t now=fs_now_ms(); struct blk_request *r=0;
    for (struct blk_request *p=q->head;p;p=p->next)
        if (eligible(q,p) && better(q,p,r,now)) r=p;
    if (!r) { fs_lock_drop(&q->lock); return false; }
    unlink_request(q,r);
    if (r->cancel || r->generation!=r->device->media_generation || r->device->quarantined || now-r->enqueued>=r->timeout) {
        int e=r->cancel ? -FS_EAGAIN : -FS_EIO;
        if (now-r->enqueued>=r->timeout) q->stats.timeouts++;
        q->stats.cancelled++; finish(r,e); fs_event_wake(&q->changed); fs_lock_drop(&q->lock); return true;
    }
    q->busy=true; q->active=r; r->state=BLK_ISSUED; if (!r->bytes) r->started=now;
    uint32_t bytes=r->length-r->bytes;
    if (bytes>q->limit_bytes) { bytes=q->limit_bytes; q->stats.split++; }
    bytes-=bytes%r->device->sector_size;
    if (!bytes && r->operation!=BLK_FLUSH) bytes=r->device->sector_size;
    struct blk_request *merged=0;
    /* Only an explicitly shared live pin can authorize a combined command.
     * Ordinary independently owned request buffers are never concatenated. */
    if (r->operation!=BLK_FLUSH && !r->bytes && bytes==r->length &&
        r->pin_owner && r->pin_owner->ticket) {
        for (struct blk_request *p=q->head;p;p=p->next) {
            if (p->operation==r->operation && p->device==r->device &&
                p->generation==r->generation && p->priority==r->priority &&
                p->frontier==r->frontier && p->pin_owner==r->pin_owner &&
                !p->bytes && !p->cancel && eligible(q,p) &&
                p->lba==r->lba+r->length/r->device->sector_size &&
                (uint8_t *)r->buffer+r->length==p->buffer &&
                p->length<=q->limit_bytes-bytes) {
                merged=p; unlink_request(q,p); p->state=BLK_ISSUED;
                p->started=now; bytes+=p->length; q->stats.merged++; break;
            }
        }
    }
    q->stats.issued++;
    fs_lock_drop(&q->lock);
    int e=execute(r,bytes); /* sleeps with IF=1; lower IRQ only wakes this continuation */
    fs_lock_take(&q->lock);
    uint32_t elapsed=fs_now_ms()-now;
    r->serviced=fs_now_ms();
    if (r->generation!=r->device->media_generation && !e) e=-FS_EIO;
    if (elapsed>BLKREQ_READER_MS/2 && bytes>r->device->sector_size) {
        uint32_t limit=(uint32_t)((uint64_t)bytes*(BLKREQ_READER_MS/2)/elapsed);
        limit-=limit%r->device->sector_size;
        if (limit<r->device->sector_size) limit=r->device->sector_size;
        if (limit<q->limit_bytes) q->limit_bytes=limit;
    }
    if (!e) r->bytes+=merged ? r->length : bytes;
    if (merged) {
        if (!e) merged->bytes=merged->length;
        finish(merged,e);
    }
    /* No retry of an issued error, including a partially completed write. */
    if (e || r->bytes==r->length || r->cancel) {
        if (r->cancel) q->stats.cancelled++;
        finish(r,e ? e : r->cancel && r->bytes<r->length ? -FS_EAGAIN : 0);
    } else {
        r->state=BLK_QUEUED; r->next=q->head; q->head=r;
    }
    q->active=0; q->busy=false; fs_event_wake(&q->changed); fs_lock_drop(&q->lock);
    return true;
}
int blk_request_wait(struct blk_request *r) {
    if (!r || r->state==BLK_PREPARED || r->state==BLK_FREE) return -FS_EINVAL;
    if (!r->queue) {
        if (r->state==BLK_QUEUED) {
            r->state=BLK_ISSUED; r->started=fs_now_ms(); int e=0;
            while (!e && r->bytes<r->length && !r->cancel) {
                uint32_t take=r->length-r->bytes; if (take>BLKREQ_COMMAND_BYTES) take=BLKREQ_COMMAND_BYTES;
                e=execute(r,take); if (!e) r->bytes+=take;
            }
            if (r->operation==BLK_FLUSH && !r->cancel) e=execute(r,0);
            finish(r,e ? e : r->cancel ? -FS_EAGAIN : 0);
        }
        return r->result;
    }
    struct blk_queue *q=r->queue;
    for (;;) {
        fs_lock_take(&q->lock);
        if (terminal(r)) { int e=r->result; fs_lock_drop(&q->lock); return e; }
        if (q->worker) {
            fs_event_wait(&r->event,&q->lock); fs_lock_drop(&q->lock); continue;
        }
        if (q->busy) {
#ifdef FS_HOST
            struct timespec end; clock_gettime(CLOCK_REALTIME,&end); end.tv_sec++;
            uint32_t sequence=__atomic_load_n(&q->changed.sequence,__ATOMIC_ACQUIRE);
            fs_lock_drop(&q->lock); pthread_mutex_lock(&q->changed.mutex);
            if (sequence==__atomic_load_n(&q->changed.sequence,__ATOMIC_ACQUIRE))
                (void)pthread_cond_timedwait(&q->changed.changed,&q->changed.mutex,&end);
            pthread_mutex_unlock(&q->changed.mutex); fs_lock_take(&q->lock);
#else
            fs_event_wait(&q->changed,&q->lock);
#endif
        }
        fs_lock_drop(&q->lock);
        blk_queue_run_one(q);
    }
}
bool blk_request_cancel(struct blk_request *r) {
    if (!r) return false;
    struct blk_queue *q=r->queue; if (q) fs_lock_take(&q->lock);
    bool accepted=!terminal(r) && r->state!=BLK_FREE;
    if (accepted) {
        r->cancel=true;
        if (r->state==BLK_PREPARED) finish(r,-FS_EAGAIN);
    }
    if (q) { fs_event_wake(&q->changed); fs_lock_drop(&q->lock); }
    return accepted;
}
int blk_request_release(struct blk_request *r) {
    if (!r || !terminal(r)) return -FS_EBUSY;
    struct blk_queue *q=r->queue;
    fs_lock_take(&pool_lock); r->state=BLK_FREE; fs_lock_drop(&pool_lock);
    if (q) { fs_lock_take(&q->lock); q->requests--; fs_event_wake(&q->changed); fs_lock_drop(&q->lock); }
    return 0;
}
void blk_queue_cancel_device(struct blk_queue *q,struct blkdev *d) {
    if (!q) return;
    fs_lock_take(&q->lock);
    for (struct blk_request *r=q->head;r;r=r->next) if (r->device==d) r->cancel=true;
    if (q->active && q->active->device==d) q->active->cancel=true;
    fs_event_wake(&q->changed); fs_lock_drop(&q->lock);
}
static void snapshot_locked(struct blk_queue *q,struct blk_queue_stats *s) {
    *s=q->stats; memset(s->depth,0,sizeof(s->depth));
    s->oldest_ms=0; s->limit_bytes=q->limit_bytes;
    uint32_t now=fs_now_ms();
    for (struct blk_request *r=q->head;r;r=r->next) {
        s->depth[r->priority]++; uint32_t age=now-r->enqueued;
        if (age>s->oldest_ms) s->oldest_ms=age;
    }
    if (q->active) {
        s->depth[q->active->priority]++;
        uint32_t age=now-q->active->enqueued; if (age>s->oldest_ms) s->oldest_ms=age;
    }
}
void blk_queue_snapshot(struct blk_queue *q,struct blk_queue_stats *s) {
    fs_lock_take(&q->lock); snapshot_locked(q,s); fs_lock_drop(&q->lock);
}
static void record_queue(struct blk_queue *q,const struct blk_queue_stats *snapshot) {
    const struct blk_queue_stats s=*snapshot;
    if (blk_queue_record) blk_queue_record(q->name,&s);
#ifndef FS_HOST
    klog("[storage] queue name=%s depth=%u/%u/%u age_ms=%u issued=%llu merged=%llu split=%llu cancelled=%llu timeout=%llu backpressure=%llu limit=%u",
        q->name,s.depth[0],s.depth[1],s.depth[2],s.oldest_ms,s.issued,s.merged,s.split,s.cancelled,s.timeouts,s.backpressure,s.limit_bytes);
#endif
}
void blk_queue_poll(struct blk_queue *q) {
    struct blk_queue_stats s; uint32_t now=fs_now_ms();
    fs_lock_take(&q->lock);
    bool report=(q->head || q->busy) && now-q->last_record>=5000;
    if (report) { q->last_record=now; snapshot_locked(q,&s); }
    fs_lock_drop(&q->lock);
    if (report) record_queue(q,&s);
}
#if !defined(FS_HOST) || defined(BLKREQ_HOST_TIMERS)
/* Deferred timer context: never block on a controller or allocate. This
 * reports even a submit-only request whose executor is stuck in hardware.
 * The f0-46 work consumer uses nonblocking record publication. */
static void queue_timer(void *arg,uint32_t generation,unsigned kind) {
    (void)kind;
    struct blk_queue *q=arg; struct blk_queue_stats s;
    if (generation!=q->report_generation || q->stopped) return;
    if (!fs_lock_try_take(&q->lock)) {
        ktimer_arm(&q->report_timer,deadline_after_ms(1),0,q,&q->report_generation,queue_timer); return;
    }
    uint32_t now=fs_now_ms();
    bool report=(q->head || q->busy) && now-q->last_record>=5000;
    if (report) { q->last_record=now; snapshot_locked(q,&s); }
    fs_lock_drop(&q->lock);
    if (report) record_queue(q,&s);
    if (!q->stopped) ktimer_arm(&q->report_timer,deadline_after_ms(5000),0,q,&q->report_generation,queue_timer);
}
#endif
int blkdev_request(struct blkdev *d,enum blk_operation op,uint64_t lba,uint32_t n,
                   void *buf,enum blk_priority priority,uint64_t frontier) {
    if (!d || (unsigned)op>BLK_FLUSH || (unsigned)priority>=BLK_PRIORITIES) return -FS_EINVAL;
    if (d->quarantined) return -FS_EQUARANTINED;
    if (op!=BLK_FLUSH) {
        if (!buf) return -FS_EINVAL;
        int range=blkdev_range(d,lba,n); if (range) return range;
        if (n>UINT32_MAX/d->sector_size) return -FS_EINVAL;
    }
    for (unsigned depth=0;d->request_parent;depth++) {
        struct blkdev *parent=d->request_parent;
        if (depth>=4 || d->media_generation!=parent->media_generation ||
            parent->quarantined || lba>UINT64_MAX-d->request_offset) return -FS_EIO;
        if (op!=BLK_FLUSH) lba+=d->request_offset;
        d=parent;
        if (op!=BLK_FLUSH) { int range=blkdev_range(d,lba,n); if (range) return range; }
    }
    uint32_t now=fs_now_ms();
    uint32_t deadline=priority==BLK_DEMAND ? now+BLKREQ_READER_MS : 0;
    /* Mount views keep their read gates, trace/counters and error refresh
     * callbacks. Carry the submitting context through those synchronous
     * calldowns instead of silently replacing its frontier/priority with the
     * physical device wrapper's defaults. Context is per executing task,
     * including across a sleep; it is never a process-global current I/O. */
    initialize(); if (!pool) return -FS_ENOMEM;
    fs_lock_take(&pool_lock);
    uintptr_t executor=executor_id();
    for (unsigned i=0;i<BLKREQ_CAPACITY;i++) if (pool[i].executor==executor && executor) {
        priority=pool[i].priority; frontier=pool[i].frontier; deadline=pool[i].deadline; break;
    }
    fs_lock_drop(&pool_lock);
    struct blk_request *r=blk_request_prepare(d,op,lba,op==BLK_FLUSH ? 0 : n*d->sector_size,buf,
        priority,frontier,deadline,op==BLK_FLUSH ? 60000 : 30000);
    if (!r) return -FS_EAGAIN;
    int e=blk_request_submit(r);
    if (!e) e=blk_request_wait(r);
    else blk_request_cancel(r);
    (void)blk_request_release(r); return e;
}
int blkdev_read(struct blkdev *d,uint64_t lba,uint32_t n,void *buf) {
    return blkdev_request(d,BLK_READ,lba,n,buf,BLK_DEMAND,0);
}
int blkdev_write(struct blkdev *d,uint64_t lba,uint32_t n,const void *buf) {
    return blkdev_request(d,BLK_WRITE,lba,n,(void *)buf,BLK_BACKGROUND,0);
}
int blkdev_flush(struct blkdev *d) {
    return blkdev_request(d,BLK_FLUSH,0,0,0,BLK_BACKGROUND,0);
}
