/* SPDX-License-Identifier: GPL-2.0-only */
#include "cache.h"
#include <ciuki/reclaim.h>
#ifndef FS_HOST
#include <ciuki/ledger.h>
#include <ciuki/timing.h>
#include <ciuki/cpu.h>
static void cache_claim(void *p, enum mm_owner owner) { if (p) mm_claim(V2P(p),1,owner); }
static void cache_unclaim(void *p, enum mm_owner owner) { if (p) mm_release(V2P(p),1,owner); }
#else
#include <ciuki/ledger.h>
#pragma weak ledger_page_pinned
#pragma weak ledger_snapshot
#pragma weak ledger_pressure
static void cache_claim(void *p, enum mm_owner owner) { (void)p; (void)owner; }
static void cache_unclaim(void *p, enum mm_owner owner) { (void)p; (void)owner; }
#endif
/* Optional in the standalone F1 host model; attributes a successful physical
 * read to the native caller (or the child it is preparing), never a global delta. */
void proc_launch_disk_read(uint32_t bytes);
#pragma weak proc_launch_disk_read
extern bool file_read_cancelled(void) __attribute__((weak));
extern bool ledger_pin_transition(void) __attribute__((weak));

/* f4-04 page layouts (docs/design/f4-acceptance.md, "## f4-04"). Data pages
 * hold only media bytes: a page is two 2048-byte halves and a half is four
 * 512-byte slots or one 2048-byte slot. Descriptors (struct cache_block) are
 * packed into descriptor pages carved from the same byte budget, the first
 * page header living inside struct block_cache. Adaptive growth uses the same
 * layout; attaching a 2048-byte device only converts halves in place. */
#define HALF 2048u
#define SLOTS_512 4u
_Static_assert(sizeof(struct cache_page) <= 4096, "cache page header");
#define PAGES_PER_META ((4096u - (unsigned)offsetof(struct cache_meta, pages)) / (unsigned)sizeof(struct cache_page))
_Static_assert(PAGES_PER_META >= 1, "descriptor page");
#ifdef __i386__
/* Reservation pins and per-image ordering still retain F1's >=13440 slots
 * in the default budget (eight headers per descriptor page on i686). */
_Static_assert(PAGES_PER_META >= 8, "i686 descriptor density");
#endif

static unsigned cls(const struct cache_block *b) { return b->capacity >= 2048 ? 1u : 0u; }
static unsigned bucket(struct blkdev *d, uint64_t lba) { return (unsigned)((uintptr_t)d/16 ^ lba ^ (lba>>32)) & 255; }
static uint32_t total_pages(const struct block_cache *c) {
    return c->data_pages+c->meta_pages+c->workspace_pages+c->writeback_pages;
}
static bool page_pinned(void *p) {
#ifdef FS_HOST
    return ledger_page_pinned && ledger_page_pinned((uint32_t)((uintptr_t)p>>12));
#else
    return ledger_page_pinned(V2P(p)>>12);
#endif
}
static void prefetch_finish(struct block_cache *c, struct cache_block *b, bool use) {
    if (!b->speculative) return;
    struct cache_stream *s=b->prefetch_owner;
    use=use && b->prefetch_generation==s->generation;
    if (use) { s->useful+=b->sector_size; c->prefetch_useful+=b->sector_size; }
    else { s->wasted+=b->sector_size; c->prefetch_wasted+=b->sector_size; }
    c->speculative_bytes-=b->sector_size; s->resident-=b->sector_size;
    b->speculative=false; b->io_error=0;
}
static struct cache_device *device(struct block_cache *c, struct blkdev *d) {
    for (unsigned i=0;i<CACHE_DEVICES;i++) if (c->devices[i].dev==d) return &c->devices[i];
    for (unsigned i=0;i<CACHE_DEVICES;i++) if (!c->devices[i].dev) {
        if (c->device_epoch==UINT32_MAX) return 0;
        c->devices[i].dev=d; c->devices[i].generation=d->media_generation; c->devices[i].epoch=++c->device_epoch; return &c->devices[i];
    }
    return 0;
}
static bool current(struct cache_device *s, struct blkdev *d, uint32_t epoch, uint32_t generation) {
    return s && s->dev==d && s->epoch==epoch && s->generation==generation &&
        d->media_generation==generation && !d->quarantined;
}
static int remember(struct block_cache *c, struct blkdev *d, int e) {
    if (e==-VFS_ETIMEOUT || e==-VFS_ECANCELLED) return e;
    struct cache_device *s=device(c,d);
    if (s && e && !s->error) s->error=e;
    return e;
}
/* All schedule publishers hold cache; the writer's due check is lockless. */
static void schedule_writeback(struct block_cache *c, uint32_t due) {
    if (!c->writeback_pending || (int32_t)(due-c->next_writeback)<0)
        __atomic_store_n(&c->next_writeback,due,__ATOMIC_RELEASE);
    __atomic_store_n(&c->writeback_pending,true,__ATOMIC_RELEASE);
}
static void request_writeback(struct block_cache *c, struct cache_device *s) {
    s->requested=true; schedule_writeback(c,fs_now_ms()); fs_event_wake(&c->changed);
}
static void clean(struct block_cache *c, struct cache_block *b) {
    if (!b->dirty) return;
    /* Invalidation already removed the old generation's accounting. */
    if (!b->invalid) {
        struct cache_device *s=device(c,b->dev);
        if (s) s->dirty_bytes-=b->sector_size;
        c->dirty_bytes-=b->sector_size;
        if (b->pins) {
            if (s) s->reserved_bytes+=b->sector_size;
            c->reserved_bytes+=b->sector_size;
        }
    }
    b->dirty=false;
}
/* Local timed event wait: preserve the port's event sequence handshake, but
 * do not turn a missing writer into an unbounded reader allocation wait. */
static void wait_changed(struct block_cache *c, uint32_t ms) {
    if (fs_request_current()) { (void)fs_event_wait_request(&c->changed,&c->lock,VFS_WAIT_CACHE); return; }
#ifdef FS_HOST
    struct timespec end; clock_gettime(CLOCK_REALTIME,&end);
    end.tv_sec+=ms/1000; end.tv_nsec+=(long)(ms%1000)*1000000;
    if (end.tv_nsec>=1000000000) { end.tv_sec++; end.tv_nsec-=1000000000; }
    uint32_t seq=__atomic_load_n(&c->changed.sequence,__ATOMIC_ACQUIRE);
    fs_lock_drop(&c->lock); pthread_mutex_lock(&c->changed.mutex);
    while (seq==__atomic_load_n(&c->changed.sequence,__ATOMIC_ACQUIRE))
        if (pthread_cond_timedwait(&c->changed.changed,&c->changed.mutex,&end)) break;
    pthread_mutex_unlock(&c->changed.mutex); (void)fs_lock_retake(&c->lock,VFS_WAIT_CACHE);
#else
    struct fs_event_ticket t={&c->changed,c->changed.sequence}; fs_lock_drop(&c->lock);
    kwait_wait_until(&c->changed.wait,fs_event_changed,&t,deadline_after_ms(ms)); (void)fs_lock_retake(&c->lock,VFS_WAIT_CACHE);
#endif
}
static void scan_service(unsigned *work);
static struct cache_fill *filling(struct block_cache *c, struct blkdev *d, uint64_t lba) {
    unsigned work=0;
    for (struct cache_fill *f=c->fills; f; f=f->next) {
        scan_service(&work);
        if (f->dev==d && lba>=f->lba && lba-f->lba<f->count) return f;
    }
    return 0;
}
static int join_fill(struct block_cache *c, struct cache_fill *f, uint64_t lba, uint32_t count, void *buf) {
    f->waiters++;
    int e=0;
    while (!f->complete && !e) e=fs_event_wait_request(&c->changed,&c->lock,VFS_WAIT_CACHE);
    if (!e) e=f->error;
    if (c->refresh) c->refresh(c->refresh_ctx,f->dev);
    struct cache_device *state=device(c,f->dev);
    if (!e && (!state || state->epoch!=f->epoch || f->dev->media_generation!=f->generation || f->dev->quarantined)) e=-FS_EIO;
    if (!e) memcpy(buf,(uint8_t *)f->buffer+(size_t)(lba-f->lba)*f->dev->sector_size,(size_t)count*f->dev->sector_size);
    f->waiters--; fs_event_wake(&c->changed); return e;
}
/* Cache lock held on entry/return, absent for transport and sleep. A range
 * reservation also covers sectors for which the tiny cache has no free slot. */
static int read_transport(struct block_cache *c, struct blkdev *d, uint64_t lba, uint32_t count, void *buf) {
    struct cache_fill *other=filling(c,d,lba);
    if (other && count<=other->count-(lba-other->lba)) return join_fill(c,other,lba,count,buf);
    struct cache_device *state=device(c,d);
    if (!state) return -FS_ENOSPC;
    uint32_t generation=d->media_generation, epoch=state->epoch;
    struct cache_fill fill={.next=c->fills,.dev=d,.lba=lba,.count=count,.buffer=buf,.generation=generation,.epoch=epoch};
    c->fills=&fill;
    fs_lock_drop(&c->lock);
    int e=blkdev_read(d,lba,count,buf);
    (void)fs_lock_retake(&c->lock,VFS_WAIT_CACHE);
    if (c->refresh) c->refresh(c->refresh_ctx,d);
    state=device(c,d);
    if (!e && (!state || state->epoch!=epoch || d->media_generation!=generation || d->quarantined)) e=-FS_EIO;
    if (!e && proc_launch_disk_read) proc_launch_disk_read(count*d->sector_size);
    fill.error=e; fill.complete=true; fs_event_wake(&c->changed);
    while (fill.waiters) fs_event_wait(&c->changed,&c->lock);
    unsigned work=0;
    struct cache_fill **at=&c->fills;
    while (*at!=&fill) { scan_service(&work); at=&(*at)->next; }
    *at=fill.next; return e;
}
/* f1-93: a dirty block under write-back holds valid bytes that nobody may
 * change until the write completes (cache_write waits, publication returns
 * EAGAIN): a reader copies them instead of waiting for the device command,
 * as cache_peek already does. A fill (never dirty) is still waited for. */
static bool readable_busy(const struct cache_block *b) {
    return b->busy && b->dirty && !b->invalid && b->dev && b->generation==b->dev->media_generation;
}
static int wait_block(struct block_cache *c, struct cache_block *b) {
    if (b->waiters==255) fs_panic("cache waiters");
    b->waiters++;
    int interrupted=0;
    while (b->busy && !interrupted) interrupted=fs_event_wait_request(&c->changed,&c->lock,VFS_WAIT_CACHE);
    struct cache_device *owner=b->dirty ? device(c,b->dev) : 0;
    int e=owner ? owner->error : b->speculative ? 0 : b->io_error;
    if (b->invalid || (b->dev && b->generation!=b->dev->media_generation)) e=-FS_EIO;
    b->waiters--; fs_event_wake(&c->changed);
    return interrupted ? interrupted : e;
}
static void unhash(struct block_cache *, struct cache_block *);
/* Caller holds cache lock; reserve the bytes while the transport sleeps.
 * Readers/writers wait on the reservation, invalidation marks it cancelled. */
static int write_block(struct block_cache *c, struct cache_block *b) {
    if (b->busy) { int e=wait_block(c,b); if (e) return e; }
    if (!b->dirty) return 0;
    if (c->refresh) c->refresh(c->refresh_ctx,b->dev);
    struct blkdev *d=b->dev;
    struct cache_device *s=device(c,d);
    if (!s) return -FS_ENOSPC;
    if (s->error) return s->error;
    int e=b->generation!=d->media_generation ? -FS_EIO : blkdev_range(d,b->lba,1);
    if (e) return remember(c,d,e);
    uint32_t generation=b->generation;
    uint32_t since=b->since;
    b->busy=true;
    fs_lock_drop(&c->lock);
    e=blkdev_request(d,BLK_WRITE,b->lba,1,b->data,BLK_BACKGROUND,b->order);
    (void)fs_lock_retake(&c->lock,VFS_WAIT_CACHE);
    if (c->refresh) c->refresh(c->refresh_ctx,d);
    if (!e && (b->invalid || generation!=d->media_generation || d->quarantined)) e=-FS_EIO;
    b->busy=false;
    if (e) { b->since=since; if (!b->invalid && generation==d->media_generation) remember(c,d,e); } else { clean(c,b); b->io_error=0; c->writes++; }
    if (b->invalid) { clean(c,b); unhash(c,b); b->io_error=e; }
    fs_event_wake(&c->changed); return e;
}
static void unhash(struct block_cache *c, struct cache_block *b) {
    prefetch_finish(c,b,false);
    if (!b->dev) return;
    struct cache_block **p=&c->hash[bucket(b->dev,b->lba)];
    unsigned work=0;
    while (*p && *p!=b) { scan_service(&work); p=&(*p)->hash_next; }
    if (*p) *p=b->hash_next;
    b->hash_next=0; b->dev=0;
}
/* Descriptor pages never move during the cache lifetime. Unlike LRU links,
 * this cursor survives cooperative service, touches and layout conversions.
 * Inspect layout under the cache lock at each visit; never return inactive
 * 512-byte descriptors from a half that now holds one 2048-byte block. */
struct cache_scan { struct cache_meta *meta; unsigned page, slot; bool in_meta; };
static struct cache_block *scan_next(struct block_cache *c, struct cache_scan *scan)
{
    for (;;) {
        struct cache_page *pg;
        if (!scan->in_meta) pg=&c->inline_page;
        else {
            if (!scan->meta) return 0;
            pg=&scan->meta->pages[scan->page];
        }
        if (scan->slot==CACHE_PAGE_SLOTS || !pg->data) {
            scan->slot=0;
            if (!scan->in_meta) { scan->in_meta=true; scan->meta=c->meta; }
            else if (++scan->page==scan->meta->count) { scan->page=0; scan->meta=scan->meta->next; }
            continue;
        }
        unsigned slot=scan->slot++;
        if (pg->layout[slot/SLOTS_512] && slot%SLOTS_512) continue;
        return &pg->b[slot];
    }
}
/* 16 descriptor visits, each <=64 insertion comparisons/moves at 0.25 us,
 * plus device checks <=2 us: <300 us. The 1 ms fs_service clock gate
 * therefore leaves margin below a 2 ms software span. fs_service retains only sleeping
 * filesystem reservations; there is no IRQ-off section or spinlock here. */
static void scan_service(unsigned *work)
{ if (!(++*work & 15u)) fs_service(); }
/* Invalidation retains the sleeping cache reservation while preparing all
 * old blocks, then publishes the device epoch in one constant-time step. */
static struct cache_block *first_block(struct block_cache *c) {
    return c->lru[0].oldest ? c->lru[0].oldest : c->lru[1].oldest;
}
static struct cache_block *next_block(struct block_cache *c, struct cache_block *b) {
    return b->next ? b->next : cls(b)==0 ? c->lru[1].oldest : 0;
}
static void lru_unlink(struct block_cache *c, struct cache_block *b) {
    struct cache_lru *l=&c->lru[cls(b)];
    if (b->prev) b->prev->next=b->next; else l->oldest=b->next;
    if (b->next) b->next->prev=b->prev; else l->newest=b->prev;
    b->prev=b->next=0;
}
static void lru_newest(struct block_cache *c, struct cache_block *b) {
    struct cache_lru *l=&c->lru[cls(b)];
    b->prev=l->newest; b->next=0;
    if (l->newest) l->newest->next=b; else l->oldest=b;
    l->newest=b;
}
static void lru_oldest(struct block_cache *c, struct cache_block *b) {
    struct cache_lru *l=&c->lru[cls(b)];
    b->next=l->oldest; b->prev=0;
    if (l->oldest) l->oldest->prev=b; else l->newest=b;
    l->oldest=b;
}
static void touch(struct block_cache *c, struct cache_block *b) {
    if (c->lru[cls(b)].newest==b) return;
    lru_unlink(c,b); lru_newest(c,b);
}
static void half_activate(struct block_cache *c, struct cache_page *pg, unsigned h, unsigned layout, bool oldest_end) {
    unsigned count=layout ? 1 : SLOTS_512, size=layout ? HALF : 512;
    pg->layout[h]=(uint8_t)layout;
    for (unsigned k=0;k<count;k++) {
        struct cache_block *b=&pg->b[h*SLOTS_512+k];
        memset(b,0,sizeof(*b));
        b->data=pg->data+h*HALF+k*size; b->capacity=(uint16_t)size;
        if (oldest_end) lru_oldest(c,b); else lru_newest(c,b);
    }
    c->blocks+=count; c->slots[layout]+=count;
}
/* Drop every slot of one half. Dirty data must already be written back. */
static void half_deactivate(struct block_cache *c, struct cache_page *pg, unsigned h) {
    unsigned layout=pg->layout[h], count=layout ? 1 : SLOTS_512;
    for (unsigned k=0;k<count;k++) {
        struct cache_block *b=&pg->b[h*SLOTS_512+k];
        unhash(c,b); b->dirty=false; lru_unlink(c,b);
    }
    c->blocks-=count; c->slots[layout]-=count;
}
static int convert_half(struct block_cache *c, struct cache_page *pg, unsigned h) {
    if (page_pinned(pg->data)) return -FS_EBUSY;
    /* Never drop the mutex halfway through changing a layout. A dirty
     * write may sleep, so restart the complete half preflight afterwards. */
    for (unsigned k=0;k<SLOTS_512;k++) {
        struct cache_block *b=&pg->b[h*SLOTS_512+k];
        if (b->busy || b->waiters || b->pins) return -FS_EBUSY;
        if (b->dirty) {
            int e=write_block(c,b); if (e) return e;
            return -FS_EAGAIN; /* recheck every slot after the unlocked write */
        }
    }
    half_deactivate(c,pg,h); half_activate(c,pg,h,1,true); return 0;
}
static void convert_page(struct block_cache *c, struct cache_page *pg, unsigned halves, unsigned *done, int *blocked) {
    if (!pg->data) return;
    fs_service(); /* <=two complete half preflights before the next page */
    for (unsigned h=0;h<2 && *done<halves;h++) if (!pg->layout[h]) {
        fs_service();
        int e=convert_half(c,pg,h);
        if (e==-FS_EAGAIN) { h--; continue; }
        if (e) *blocked=e; else ++*done;
    }
}
/* Later pages first, the inline header page last: a minimum cache still has
 * exactly one page to convert. Returns success when at least one half moved. */
static int convert(struct block_cache *c, unsigned halves) {
    unsigned done=0; int blocked=-FS_ENOMEM;
    for (struct cache_meta *m=c->meta; m && done<halves; m=m->next)
        for (unsigned i=m->count; i-- && done<halves;) convert_page(c,&m->pages[i],halves,&done,&blocked);
    if (done<halves && c->inline_page.data) convert_page(c,&c->inline_page,halves,&done,&blocked);
    return done ? 0 : blocked;
}
/* A 2048-byte device converts one eighth of the data pages (at least one),
 * as two halves each. Called with the cache lock held. */
static int layout_for(struct block_cache *c, struct blkdev *d) {
    if (d->sector_size!=2048 || c->slots[1]) return 0;
    unsigned pages=c->data_pages/8; if (!pages) pages=1;
    return convert(c,pages*2);
}
static struct cache_block *find(struct block_cache *c, struct blkdev *d, uint64_t lba) {
    unsigned work=0;
    for (struct cache_block *b=c->hash[bucket(d,lba)]; b; b=b->hash_next) {
        scan_service(&work);
        if (b->dev==d && b->lba==lba && b->generation==d->media_generation && b->sector_size==d->sector_size) return b;
    }
    return 0;
}
/* Reserve a miss before dropping the lock. Failed fills retain their result
 * until existing waiters have consumed it, then the slot is reusable. */
static int get(struct block_cache *c, struct blkdev *d, uint64_t lba, bool read, struct cache_block **out, bool *issued) {
    if (issued) *issued=false;
    uint32_t wait_start=fs_now_ms();
retry:
    fs_service();
    if (c->refresh) c->refresh(c->refresh_ctx,d);
    int e=fs_request_check(); if (e) return e;
    e=blkdev_range(d,lba,1); if (e) return e;
    if (!d->read) return -FS_EINVAL;
    if (!device(c,d)) return -FS_ENOSPC;
    if ((e=layout_for(c,d))) return e;
    struct cache_block *b=find(c,d,lba);
    if (b) {
        if (b->busy && !(read && readable_busy(b))) { if (issued && read) *issued=true; e=wait_block(c,b); if (e) return e; goto retry; }
        c->hits++; if (read) c->demand_hits++;
        prefetch_finish(c,b,read); touch(c,b); *out=b; return 0;
    }
    int blocked=-FS_ENOMEM; bool pending=false; struct cache_block *dirty=0;
    unsigned work=0;
    for (unsigned list=d->sector_size==2048 ? 1u : 0u; list<2 && !b; list++) {
        for (struct cache_block *x=c->lru[list].oldest; x; x=x->next) {
            /* The sleeping cache reservation stays held across service, so
             * LRU/hash nodes and this candidate retain their identity. */
            scan_service(&work);
            if (x->capacity<d->sector_size) continue;
            if (x->busy || x->waiters || x->pins || page_pinned(x->data)) { pending=true; continue; }
            struct cache_device *owner=x->dirty ? device(c,x->dev) : 0;
            if (!x->dirty) { b=x; break; }
            if (owner && !owner->error) { if (!dirty) dirty=x; }
            else if (owner) blocked=owner->error;
        }
    }
    if (!b) {
        if (dirty) {
            uint32_t elapsed=fs_now_ms()-wait_start;
            if (c->writer_online && elapsed<CACHE_CLEAN_WAIT_MS) {
                request_writeback(c,device(c,dirty->dev));
                wait_changed(c,CACHE_CLEAN_WAIT_MS-elapsed); goto retry;
            }
            c->cache_sync_evictions++; (void)write_block(c,dirty); goto retry;
        }
        if (pending) { int e=fs_event_wait_request(&c->changed,&c->lock,VFS_WAIT_CACHE); if (e) return e; goto retry; }
        return blocked;
    }
    unhash(c,b); c->misses++; if (read) c->demand_misses++;
    b->dev=d; b->lba=lba; b->generation=d->media_generation; b->sector_size=(uint16_t)d->sector_size;
    b->invalid=false; b->io_error=0;
    unsigned h=bucket(d,lba); b->hash_next=c->hash[h]; c->hash[h]=b;
    touch(c,b);
    if (!read && issued) *issued=true; /* fresh slot, contents undefined (f1-89 fill) */
    if (read) {
        if (issued) *issued=true;
        b->busy=true;
        e=read_transport(c,d,lba,1,b->data);
        if (c->refresh) c->refresh(c->refresh_ctx,d);
        if (!e && (b->invalid || b->generation!=d->media_generation || d->quarantined)) e=-FS_EIO;
        if (!e) e=blkdev_range(d,lba,1);
        b->busy=false; b->io_error=e;
        if (e) unhash(c,b);
        fs_event_wake(&c->changed);
        if (e) return e;
    }
    *out=b; return 0;
}
static unsigned meta_needed(unsigned data) { return data>1 ? (data-1+PAGES_PER_META-1)/PAGES_PER_META : 0; }
int cache_init(struct block_cache *c, size_t bytes) {
    memset(c,0,sizeof(*c)); fs_lock_init(&c->lock); fs_lock_init(&c->workspace_lock); fs_event_init(&c->changed); fs_lock_classify(&c->lock,3,0); fs_lock_classify(&c->workspace_lock,3,0);
    if (!bytes) bytes=CACHE_DEFAULT_BYTES;
    if (bytes<8192 || bytes>32u*1024u*1024u) { cache_destroy(c); return -FS_EINVAL; }
    size_t pages=bytes/4096;
    c->dirty_budget=(uint32_t)(bytes/8);
    if (c->dirty_budget>1024u*1024) c->dirty_budget=1024u*1024;
    c->report_time=fs_now_ms();
    /* Large caches batch up to 32 KiB of transport. Preserve the eight-slot
     * minimum and existing scan workspace for small caches. */
    if (pages>=64) {
        c->writeback_pages=8;
        c->writeback_buffer=fs_contig_alloc(c->writeback_pages*4096u);
        if (!c->writeback_buffer) goto fail;
        for (unsigned i=0;i<c->writeback_pages;i++) cache_claim(c->writeback_buffer+i*4096u,MM_CACHE_WORK);
    }
    c->workspace_pages=(uint32_t)(pages/16); if (!c->workspace_pages) c->workspace_pages=1;
    if (c->workspace_pages>256) c->workspace_pages=256;
    for (unsigned i=0;i<c->workspace_pages;i++) { fs_service(); if (!(c->workspace[i]=fs_page_alloc())) goto fail; cache_claim(c->workspace[i],MM_CACHE_WORK); }
    /* Budget: workspace + writeback + data + descriptors == bytes/4096. */
    unsigned room=(unsigned)(pages-c->workspace_pages-c->writeback_pages), data=room;
    while (data && data+meta_needed(data)>room) data--;
    if (!data) goto fail;
    struct cache_meta *m=0;
    for (unsigned i=0;i<data;i++) {
        /* 16 allocation/zero pages * 18 us <=288 us. */
        if (!(i & 15u)) fs_service();
        struct cache_page *pg;
        if (!i) pg=&c->inline_page;
        else {
            if (!m || m->count==PAGES_PER_META) {
                struct cache_meta *n=fs_page_alloc(); if (!n) goto fail;
                cache_claim(n,MM_CACHE_META);
                n->next=c->meta; if (c->meta) c->meta->prev=n; n->prev=0; c->meta=n; c->meta_pages++; m=n;
            }
            pg=&m->pages[m->count];
        }
        pg->data=fs_page_alloc(); if (!pg->data) goto fail; cache_claim(pg->data,MM_CACHE_DATA);
        if (i) m->count++;
        c->data_pages++;
        half_activate(c,pg,0,0,false); half_activate(c,pg,1,0,false);
    }
    return 0;
fail: cache_destroy(c); return -FS_ENOMEM;
}
/* C4 controller. No transport is called here: dirty debt is only scheduled.
 * Descriptor addresses stay stable; holes are reused, never compacted. */
uint32_t cache_initial_bytes(uint32_t usable, uint32_t free_pages, uint32_t high) {
    uint32_t ceiling=usable/8, initial=CACHE_DEFAULT_BYTES/4096;
    if (ceiling>CACHE_CEILING_BYTES/4096) ceiling=CACHE_CEILING_BYTES/4096;
    if (initial>ceiling) initial=ceiling;
    uint32_t available=free_pages>high ? free_pages-high : 0;
    if (initial>available) initial=available;
    return initial<CACHE_FLOOR_BYTES/4096 ? 0 : initial*4096;
}
int cache_adaptive_enable(struct block_cache *c, uint32_t usable) {
    fs_lock_take(&c->lock);
    uint32_t ceiling=usable/8;
    if (ceiling>CACHE_CEILING_BYTES/4096) ceiling=CACHE_CEILING_BYTES/4096;
    int e=ceiling<CACHE_FLOOR_BYTES/4096 || total_pages(c)>ceiling ||
        total_pages(c)<CACHE_FLOOR_BYTES/4096 ? -FS_ENOMEM : 0;
    if (!e) {
        c->floor_pages=CACHE_FLOOR_BYTES/4096; c->ceiling_pages=ceiling;
        c->adaptive=true; c->window_time=fs_now_ms();
        if (reclaim_cache_register) reclaim_cache_register(c);
        c->window_hits=c->demand_hits; c->window_misses=c->demand_misses;
    }
    fs_lock_drop(&c->lock); return e;
}
static void stream_decay(struct cache_stream *s) {
    __atomic_add_fetch(&s->generation,1,__ATOMIC_RELEASE); s->pending=0; s->sequential=0; s->window_bytes=0;
    s->last_useful=s->useful;
}
static uint64_t slice_clock(void) {
#ifdef FS_HOST
    struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t);
    return (uint64_t)t.tv_sec*1000000+(uint32_t)t.tv_nsec/1000;
#else
    return ktime_cycles();
#endif
}
static uint32_t slice_us(uint64_t start) {
#ifdef FS_HOST
    return (uint32_t)(slice_clock()-start);
#else
    uint64_t delta=ktime_cycles()-start;
    return g_tsc_per_ms ? (uint32_t)(delta/g_tsc_per_ms*1000+(delta%g_tsc_per_ms)*1000/g_tsc_per_ms) : 0;
#endif
}
/* A transitioning scattered pin may still require a vector lookup. Reclaim
 * waits for its descriptor publication, then excludes a new transition
 * through the final single-frame release. No large pin walk in this hook. */
static bool trim_free_begin(uint32_t *flags) {
#ifndef FS_HOST
    *flags=irq_save();
#else
    *flags=0;
#endif
    if (ledger_pin_transition && ledger_pin_transition()) {
#ifndef FS_HOST
        irq_restore(*flags);
#endif
        return false;
    }
    return true;
}
static void trim_free_end(uint32_t flags) {
#ifndef FS_HOST
    irq_restore(flags);
#else
    (void)flags;
#endif
}
static bool reclaim_page(struct block_cache *c, struct cache_page *pg) {
    bool pinned=page_pinned(pg->data), dirty=false, error=false;
    uint32_t speculative=0;
    for (unsigned i=0;i<CACHE_PAGE_SLOTS;i++) {
        if (pg->layout[i/SLOTS_512] && i%SLOTS_512) continue;
        struct cache_block *b=&pg->b[i];
        pinned|=b->busy || b->waiters || b->pins;
        if (b->speculative) speculative+=b->sector_size;
        if (b->dirty) {
            dirty=true; struct cache_device *s=device(c,b->dev);
            if (s && s->error) error=true;
            else if (s) request_writeback(c,s);
        } else if (!b->speculative && b->io_error) error=true;
    }
    c->blocked_pinned+=pinned; c->blocked_dirty+=dirty; c->blocked_error+=error;
    if (pinned || dirty || error || c->speculative_bytes-speculative>(total_pages(c)-1)*4096u/8) return false;
    half_deactivate(c,pg,0); half_deactivate(c,pg,1);
    cache_unclaim(pg->data,MM_CACHE_DATA); fs_page_free(pg->data); pg->data=0;
    c->data_pages--; return true;
}
static void cache_reclaim_slice(struct block_cache *c) {
    uint64_t start=slice_clock();
    c->slice_pages=c->slice_scanned=0;
    c->blocked_pinned=c->blocked_dirty=c->blocked_error=0;
    /* At most 64 page preflights (eight descriptors each). Stop at 500 us,
     * leaving 500 us for the final preflight/unhash/free and accounting.
     * The rotating cursor makes a dirty/pinned prefix unable to starve clean
     * pages later in the cache. The inline page and fixed workspace remain. */
    while (total_pages(c)>c->floor_pages && c->slice_pages<CACHE_STEP_PAGES &&
           c->slice_scanned<CACHE_STEP_PAGES && slice_us(start)<500) {
        if (!c->meta) break;
        if (!c->reclaim_meta) { c->reclaim_meta=c->meta; c->reclaim_index=0; }
        struct cache_meta *m=c->reclaim_meta;
        if (c->reclaim_index>=m->count) {
            c->reclaim_meta=m->next ? m->next : c->meta; c->reclaim_index=0; continue;
        }
        struct cache_page *pg=&m->pages[c->reclaim_index++]; c->slice_scanned++;
        if (pg->data && reclaim_page(c,pg)) c->slice_pages++;
        bool empty=true;
        for (unsigned i=0;i<m->count;i++) if (m->pages[i].data) { empty=false; break; }
        if (empty && !page_pinned(m) && c->slice_pages<CACHE_STEP_PAGES && total_pages(c)>c->floor_pages &&
            c->speculative_bytes<=(total_pages(c)-1)*4096u/8) {
            struct cache_meta **at=&c->meta;
            while (*at!=m) at=&(*at)->next;
            *at=m->next; if (m->next) m->next->prev=m->prev; c->reclaim_meta=m->next; c->reclaim_index=0;
            cache_unclaim(m,MM_CACHE_META); fs_page_free(m); c->meta_pages--; c->slice_pages++;
        }
    }
    c->scanned_pages+=c->slice_scanned; c->reclaimed_pages+=c->slice_pages;
    if (c->slice_pages) c->reclaims++;
    uint32_t us=slice_us(start); if (us>c->max_slice_us) c->max_slice_us=us;
}
/* f0-48 calls the C4 hook one physical-frame preflight at a time. Empty
 * descriptor slots and cursor advances consume the same scan allowance.
 * Empty metadata pages use constant-time unlinking, as separate preflights. */
void cache_reclaim_step(struct block_cache *c, bool speculative, struct reclaim_result *out)
{
    *out=(struct reclaim_result){0};
    fs_lock_take(&c->lock);
    if (!c->adaptive || total_pages(c)<=c->floor_pages || !c->meta) {
        c->trim_page=0; c->trim_prev=0;
        out->done=true; fs_lock_drop(&c->lock); return;
    }
    out->scanned=1;
    if (ledger_pin_transition && ledger_pin_transition()) { out->pinned=1; goto done; }
    if (!c->trim_page) {
        if (!c->reclaim_meta) { c->reclaim_meta=c->meta; c->reclaim_index=0; }
        struct cache_meta *m=c->reclaim_meta;
        if (c->reclaim_index>=m->count) {
            c->reclaim_meta=m->next; c->reclaim_index=0;
            bool empty=true;
            for (unsigned i=0;i<m->count;i++) if (m->pages[i].data) { empty=false; break; }
            if (empty && !page_pinned(m) && c->speculative_bytes<=(total_pages(c)-1)*4096u/8) {
                uint32_t flags;
                if (!trim_free_begin(&flags)) { out->pinned=1; goto done; }
                if (m->prev) m->prev->next=m->next; else c->meta=m->next;
                if (m->next) m->next->prev=m->prev;
                cache_unclaim(m,MM_CACHE_META); fs_page_free(m); c->meta_pages--;
                out->reclaimed=1; c->reclaimed_pages++; c->reclaims++;
                trim_free_end(flags);
            }
            out->done=!c->reclaim_meta; goto done;
        }
        c->trim_page=&m->pages[c->reclaim_index++]; c->trim_slot=0; c->trim_prev=0;
    }
    struct cache_page *pg=c->trim_page;
    if (!pg->data) goto skip;
    bool unused=true, error=false; uint32_t spec=0;
    out->pinned=page_pinned(pg->data);
    for (unsigned i=0;i<CACHE_PAGE_SLOTS;i++) {
        if (pg->layout[i/SLOTS_512] && i%SLOTS_512) continue;
        struct cache_block *b=&pg->b[i];
        out->pinned|=b->waiters!=0 || b->pins!=0; out->inflight|=b->busy;
        if (b->dev && !b->speculative) unused=false;
        if (b->speculative) spec+=b->sector_size;
        if (b->dirty) {
            out->dirty=1; struct cache_device *d=device(c,b->dev);
            if (d && !d->error) request_writeback(c,d);
        } else if (!b->speculative && b->io_error) error=true;
    }
    if (out->pinned || out->inflight || out->dirty || error || (speculative && !unused) ||
        c->speculative_bytes-spec>(total_pages(c)-1)*4096u/8) goto skip;
    /* Do not run C4's unbounded hash search in a scanner preflight. Detach
     * at most 32 links per continuation. Descriptors stay resident; a saved
     * predecessor is usable only while it still belongs to this bucket.
     * Readers/writers may reuse detached slots between calls: recheck the
     * whole frame on every continuation and before final release. */
    unsigned probes=0;
    while (c->trim_slot<CACHE_PAGE_SLOTS) {
        unsigned i=c->trim_slot;
        if ((pg->layout[i/SLOTS_512] && i%SLOTS_512) || !pg->b[i].dev) {
            c->trim_slot++; c->trim_prev=0; continue;
        }
        struct cache_block *b=&pg->b[i], *prev=c->trim_prev;
        unsigned h=bucket(b->dev,b->lba);
        if (prev==b || (prev && (!prev->dev || bucket(prev->dev,prev->lba)!=h))) prev=0;
        struct cache_block **at=prev?&prev->hash_next:&c->hash[h];
        while (*at && *at!=b && probes<32) {
            prev=*at; at=&prev->hash_next; probes++;
        }
        if (probes==32) { c->trim_prev=prev; goto done; }
        if (!*at) goto skip; /* concurrent identity change: retry a later pass */
        *at=b->hash_next; b->hash_next=0; prefetch_finish(c,b,false); b->dev=0;
        c->trim_slot++; c->trim_prev=0; probes++;
        if (probes==32) goto done;
    }
    for (unsigned i=0;i<CACHE_PAGE_SLOTS;i++) if (pg->b[i].dev) {
        c->trim_slot=0; c->trim_prev=0; goto done;
    }
    uint32_t flags;
    if (!trim_free_begin(&flags)) { out->pinned=1; goto skip; }
    half_deactivate(c,pg,0); half_deactivate(c,pg,1);
    cache_unclaim(pg->data,MM_CACHE_DATA); fs_page_free(pg->data); pg->data=0;
    c->data_pages--; out->reclaimed=1; c->reclaimed_pages++; c->reclaims++;
    trim_free_end(flags);
skip:
    c->trim_page=0; c->trim_prev=0;
done:
    c->scanned_pages+=out->scanned;
    fs_lock_drop(&c->lock);
}
static void grow_window(struct block_cache *c, uint32_t allowance) {
    unsigned before=total_pages(c), limit=before+allowance;
    if (limit>c->ceiling_pages) limit=c->ceiling_pages;
    while (total_pages(c)<limit) {
#ifdef FS_HOST
        if (ledger_snapshot && ledger_pressure)
#endif
        {
            struct mm_ledger memory; ledger_snapshot(&memory);
            if (ledger_pressure() || memory.zone[2].free<=memory.high+2) break;
        }
        struct cache_page *pg=0; struct cache_meta *m=0;
        for (m=c->meta;m && !pg;m=m->next) {
            for (unsigned i=0;i<m->count;i++) if (!m->pages[i].data) { pg=&m->pages[i]; break; }
            if (!pg && m->count<PAGES_PER_META) pg=&m->pages[m->count++];
        }
        if (!pg) {
            if (total_pages(c)+2>limit) break;
            m=fs_normal_page_alloc(); if (!m) break;
            cache_claim(m,MM_CACHE_META); m->next=c->meta; if (c->meta) c->meta->prev=m; m->prev=0; c->meta=m; c->meta_pages++;
            pg=&m->pages[m->count++];
        }
        pg->data=fs_normal_page_alloc(); if (!pg->data) break;
        cache_claim(pg->data,MM_CACHE_DATA); c->data_pages++;
        half_activate(c,pg,0,0,true); half_activate(c,pg,1,0,true);
        if (!(c->data_pages&15u)) fs_service();
    }
    if (total_pages(c)>before) c->growths++;
}
void cache_adaptive_tick(struct block_cache *c, uint32_t now, uint32_t free_pages, uint32_t high, bool pressure) {
    if (!c->adaptive) return;
    fs_lock_take(&c->lock);
    if (pressure && !c->pressure) {
        for (struct cache_stream *s=c->streams;s;s=s->next) stream_decay(s);
    }
    c->pressure=pressure;
    if (pressure) { c->useful_windows=0; if (!reclaim_cache_register) cache_reclaim_slice(c); }
    uint32_t elapsed=now-c->window_time;
    if (elapsed>=CACHE_WINDOW_MS) {
        uint64_t hits=c->demand_hits-c->window_hits, misses=c->demand_misses-c->window_misses;
        bool poor=c->prefetch_wasted>=CACHE_PREFETCH_MIN && c->prefetch_wasted>c->prefetch_useful;
        bool useful=!pressure && free_pages>high+CACHE_STEP_PAGES && !poor &&
            c->dirty_bytes<=c->dirty_budget && hits && misses;
        if (elapsed>=2*CACHE_WINDOW_MS) c->useful_windows=0;
        c->useful_windows=useful ? c->useful_windows+1 : 0;
        if (c->useful_windows>=2) {
            grow_window(c,CACHE_STEP_PAGES); c->useful_windows=2;
        }
        c->window_time=now; c->window_hits=c->demand_hits; c->window_misses=c->demand_misses;
    }
    /* Preserve C1's formula as C changes, including its 1 MiB ceiling. */
    c->dirty_budget=total_pages(c)*4096u/8;
    if (c->dirty_budget>1024u*1024u) c->dirty_budget=1024u*1024u;
    fs_lock_drop(&c->lock);
}
void cache_stream_begin(struct block_cache *c, struct cache_stream *s, struct blkdev *d, uint64_t offset) {
    fs_lock_take(&c->lock);
    if (!s->cache) { s->cache=c; s->next=c->streams; c->streams=s; s->generation=1; }
    c->demand_active++;
    for (struct cache_stream *other=c->streams;other;other=other->next)
        if (other!=s && (other->pending || other->outstanding || other->resident)) stream_decay(other);
    if (s->dev!=d || s->media_generation!=d->media_generation || offset!=s->last_offset || c->pressure) stream_decay(s);
    s->dev=d; s->media_generation=d->media_generation;
    fs_lock_drop(&c->lock);
}
uint32_t cache_stream_end(struct cache_stream *s, uint64_t offset, size_t done, bool success) {
    struct block_cache *c=s->cache; if (!c) return 0;
    fs_lock_take(&c->lock); c->demand_active--;
    if (!success || !done || c->pressure || s->dev->media_generation!=s->media_generation) stream_decay(s);
    else {
        s->last_offset=offset+done;
        if (s->sequential<2) s->sequential++;
        if (s->sequential>=2) {
            if (!s->window_bytes) s->window_bytes=CACHE_PREFETCH_MIN;
            else if (s->useful>s->last_useful && s->window_bytes<CACHE_PREFETCH_MAX) s->window_bytes*=2;
        }
        s->last_useful=s->useful;
    }
    uint32_t window=!s->pending && !s->outstanding && s->resident<s->window_bytes/2 ? s->window_bytes : 0;
    fs_lock_drop(&c->lock); return window;
}
void cache_stream_queue(struct cache_stream *s, uint64_t lba, uint32_t count) {
    struct block_cache *c=s->cache; if (!c || !count) return;
    fs_lock_take(&c->lock);
    if (!c->pressure && !s->pending && !s->outstanding && s->window_bytes &&
        s->media_generation==s->dev->media_generation && !blkdev_range(s->dev,lba,count)) {
        s->lba=lba; s->pending=count>s->window_bytes/512 ? s->window_bytes/512 : count;
        fs_event_wake(&c->changed);
    }
    fs_lock_drop(&c->lock);
}
void cache_stream_cancel(struct cache_stream *s) {
    struct block_cache *c=s->cache; if (!c) return;
    fs_lock_take(&c->lock); stream_decay(s); fs_lock_drop(&c->lock);
}
void cache_stream_close(struct cache_stream *s) {
    struct block_cache *c=s->cache; if (!c) return;
    fs_lock_take(&c->lock); stream_decay(s);
    while (s->outstanding) fs_event_wait(&c->changed,&c->lock);
    unsigned work=0;
    for (struct cache_block *b=first_block(c);b;b=next_block(c,b)) {
        scan_service(&work);
        if (b->speculative && b->prefetch_owner==s) unhash(c,b);
    }
    struct cache_stream **at=&c->streams; while (*at && *at!=s) at=&(*at)->next;
    if (*at) *at=s->next;
    s->cache=0; s->next=0; fs_lock_drop(&c->lock);
}
struct prefetch_context { struct cache_stream *stream;
    struct blkdev *dev; uint32_t generation, media_generation; };
static bool prefetch_cancelled(void *arg) {
    struct prefetch_context *p=arg; struct cache_stream *s=p->stream;
    /* Worker context callbacks cannot acquire cache (the transport may be
     * called while reacquiring it). Acquire loads publish cancellation. */
    return __atomic_load_n(&s->generation,__ATOMIC_ACQUIRE)!=p->generation ||
        p->dev->media_generation!=p->media_generation || p->dev->quarantined;
}
static uint64_t prefetch_now(void *arg) { (void)arg; return fs_now_ms(); }
static void prefetch_pause(void *arg) {
    (void)arg;
#ifdef FS_HOST
    struct timespec t={0,100000}; nanosleep(&t,0);
#else
    task_sleep_ms(1);
#endif
}
bool cache_prefetch_tick(struct block_cache *c) {
    fs_lock_take(&c->lock);
    struct cache_stream *s=c->streams;
    while (s && (!s->pending || s->outstanding)) s=s->next;
    if (!s || c->pressure || c->demand_active || c->batch_busy || !c->writeback_buffer || c->dirty_bytes>c->dirty_budget) {
        fs_lock_drop(&c->lock); return false;
    }
    struct blkdev *d=s->dev;
    if (c->refresh) c->refresh(c->refresh_ctx,d);
    if (d->logical || d->sector_size!=512 || s->media_generation!=d->media_generation || d->quarantined) {
        stream_decay(s); fs_lock_drop(&c->lock); return false;
    }
    /* Skip shared hits, including dirty data, without touching or crediting
     * them. Only genuinely missing, cleanly reservable blocks are speculative. */
    while (s->pending && find(c,d,s->lba)) { s->lba++; s->pending--; }
    uint32_t count=s->pending, cap=total_pages(c)*4096u/8;
    uint32_t room=cap>c->speculative_bytes ? (cap-c->speculative_bytes)/512 : 0;
    if (count>room) count=room;
    if (count>c->writeback_pages*8) count=c->writeback_pages*8;
    for (unsigned i=0;i<count;i++) if (find(c,d,s->lba+i) || filling(c,d,s->lba+i)) { count=i; break; }
    struct cache_block *blocks[CACHE_WRITEBACK_BLOCKS]; unsigned n=0;
    for (struct cache_block *b=c->lru[0].oldest;b && n<count;b=b->next) {
        if (b->busy || b->waiters || b->pins || b->dirty || b->speculative || page_pinned(b->data)) continue;
        unhash(c,b); b->dev=d; b->lba=s->lba+n; b->generation=d->media_generation;
        b->sector_size=512; b->invalid=false; b->io_error=0; b->busy=true;
        unsigned h=bucket(d,b->lba); b->hash_next=c->hash[h]; c->hash[h]=b; blocks[n++]=b;
    }
    if (!n) { s->pending=0; fs_lock_drop(&c->lock); return false; }
    uint64_t lba=s->lba; s->lba+=n; s->pending-=n; s->outstanding++; c->batch_busy=true;
    /* In-flight bytes also consume the speculation cap, not just completions. */
    c->speculative_bytes+=n*512; s->resident+=n*512;
    if (c->speculative_bytes>c->speculative_peak) c->speculative_peak=c->speculative_bytes;
    struct cache_device *state=device(c,d); uint32_t epoch=state ? state->epoch : 0;
    struct prefetch_context context={s,d,s->generation,s->media_generation};
    struct vfs_request request={.arg=&context,.deadline=UINT64_MAX,.now=prefetch_now,
        .cancelled=prefetch_cancelled,.pause=prefetch_pause};
    fs_lock_drop(&c->lock);
    struct fs_request_scope scope; fs_request_enter(&scope,&request);
    int e=blkdev_request(d,BLK_READ,lba,n,c->writeback_buffer,BLK_SPECULATIVE,0);
    fs_request_leave(&scope);
    fs_lock_take(&c->lock);
    if (c->refresh) c->refresh(c->refresh_ctx,d);
    state=device(c,d);
    if (!state || state->epoch!=epoch || prefetch_cancelled(&context)) e=-VFS_ECANCELLED;
    c->prefetch_commands++;
    c->speculative_bytes-=n*512; s->resident-=n*512;
    for (unsigned i=0;i<n;i++) {
        struct cache_block *b=blocks[i]; b->busy=false; b->io_error=0;
        if (e || b->invalid) { unhash(c,b); s->wasted+=512; c->prefetch_wasted+=512; }
        else {
            memcpy(b->data,c->writeback_buffer+i*512,512); b->speculative=true;
            b->prefetch_owner=s; b->prefetch_generation=context.generation;
            c->speculative_bytes+=512; s->resident+=512;
        }
    }
    if (e) stream_decay(s);
    s->outstanding--; c->batch_busy=false; fs_event_wake(&c->changed);
    fs_lock_drop(&c->lock); return true;
}

void cache_destroy(struct block_cache *c) {
    if (reclaim_cache_unregister) reclaim_cache_unregister(c);
    while (c->streams) cache_stream_close(c->streams);
    if (c->writeback_buffer) for (unsigned i=0;i<c->writeback_pages;i++) cache_unclaim(c->writeback_buffer+i*4096u,MM_CACHE_WORK);
    fs_contig_free(c->writeback_buffer,c->writeback_pages*4096u);
    if (c->inline_page.data) { cache_unclaim(c->inline_page.data,MM_CACHE_DATA); fs_page_free(c->inline_page.data); }
    while (c->meta) {
        struct cache_meta *m=c->meta; c->meta=m->next;
        for (unsigned i=0;i<m->count;i++) { fs_service(); cache_unclaim(m->pages[i].data,MM_CACHE_DATA); fs_page_free(m->pages[i].data); }
        cache_unclaim(m,MM_CACHE_META); fs_page_free(m);
    }
    for (unsigned i=0;i<c->workspace_pages;i++) { fs_service(); cache_unclaim(c->workspace[i],MM_CACHE_WORK); fs_page_free(c->workspace[i]); }
    fs_lock_destroy(&c->lock); fs_lock_destroy(&c->workspace_lock); fs_event_destroy(&c->changed);
    memset(c,0,sizeof(*c));
}
int cache_attach(struct block_cache *c, struct blkdev *d) {
    if (!c || !d || (d->sector_size!=512 && d->sector_size!=2048)) return -FS_EINVAL;
    (void)fs_lock_retake(&c->lock,VFS_WAIT_CACHE);
    int e=device(c,d) ? layout_for(c,d) : -FS_ENOSPC;
    fs_lock_drop(&c->lock); return e;
}
static void stats_page(struct block_cache *c, struct cache_page *pg, struct cache_stats *out) {
    if (!pg->data) return;
    bool pinned=page_pinned(pg->data), dirty=false, error=false;
    for (unsigned i=0;i<CACHE_PAGE_SLOTS;i++) {
        if (pg->layout[i/SLOTS_512] && i%SLOTS_512) continue;
        struct cache_block *b=&pg->b[i];
        pinned|=b->busy || b->waiters || b->pins; dirty|=b->dirty;
        if (b->dirty) { struct cache_device *owner=device(c,b->dev); error|=owner && owner->error; }
        else if (!b->speculative && b->io_error) error=true;
    }
    out->pinned_data+=pinned; out->dirty_data+=dirty; out->error_data+=error;
    out->unreclaimable_data+=pinned || dirty || error;
}
static void stats_locked(struct block_cache *c, struct cache_stats *out) {
    *out=(struct cache_stats){ .pages=c->workspace_pages+c->data_pages+c->meta_pages+c->writeback_pages,
        .data_pages=c->data_pages, .meta_pages=c->meta_pages, .workspace_pages=c->workspace_pages,
        .writeback_pages=c->writeback_pages,
        .slots_512=c->slots[0], .slots_2048=c->slots[1], .hits=c->hits, .misses=c->misses, .writes=c->writes,
        .dirty_bytes=c->dirty_bytes,.dirty_budget=c->dirty_budget,.throttle_bytes=2*c->dirty_budget,
        .tick_flushed=c->tick_flushed,.max_flush_ms=c->max_flush_ms,.max_writer_wait_ms=c->max_writer_wait_ms,
        .cache_sync_evictions=c->cache_sync_evictions,.writer_waits=c->writer_waits,.writer_wait_ms=c->writer_wait_ms };
    out->floor_pages=c->floor_pages; out->ceiling_pages=c->ceiling_pages;
    out->growths=c->growths; out->reclaims=c->reclaims; out->reclaimed_pages=c->reclaimed_pages;
    out->speculative_bytes=c->speculative_bytes; out->speculative_peak=c->speculative_peak;
    out->slice_pages=c->slice_pages; out->slice_scanned=c->slice_scanned; out->max_slice_us=c->max_slice_us;
    out->blocked_pinned=c->blocked_pinned; out->blocked_dirty=c->blocked_dirty; out->blocked_error=c->blocked_error;
    out->prefetch_useful=c->prefetch_useful; out->prefetch_wasted=c->prefetch_wasted;
    out->prefetch_commands=c->prefetch_commands;
    stats_page(c,&c->inline_page,out);
    unsigned work=0;
    for (struct cache_meta *m=c->meta;m;m=m->next) for (unsigned i=0;i<m->count;i++) {
        scan_service(&work); stats_page(c,&m->pages[i],out);
    }
}
void cache_stats(struct block_cache *c, struct cache_stats *out) {
    (void)fs_lock_retake(&c->lock,VFS_WAIT_CACHE); stats_locked(c,out);
    fs_lock_drop(&c->lock);
}
int cache_read(struct block_cache *c, struct blkdev *d, uint64_t lba, void *buf) {
    if (!buf) return -FS_EINVAL;
    int admission=fs_lock_take_request(&c->lock,VFS_WAIT_CACHE); if (admission) return admission;
    struct cache_block *b;
    struct cache_fill *fill=d ? filling(c,d,lba) : 0;
    if (fill && !find(c,d,lba)) {
        int e=join_fill(c,fill,lba,1,buf); fs_lock_drop(&c->lock); return e;
    }
    bool issued=false;
    uint32_t generation=d ? d->media_generation : 0;
    int e=get(c,d,lba,true,&b,&issued);
    if (!e) memcpy(buf,b->data,d->sector_size);
    else if (!issued && !fs_request_check() && !blkdev_range(d,lba,1) && d->read) {
        /* All slots may contain dirty data pinned by failed writeback.
         * A miss still has a read transport; never retry an issued read. */
        struct cache_device *state=device(c,d);
        uint32_t epoch=state ? state->epoch : 0;
        e=read_transport(c,d,lba,1,buf);
        if (c->refresh) c->refresh(c->refresh_ctx,d);
        state=device(c,d);
        if (!e && (!state || epoch!=state->epoch || generation!=d->media_generation || d->quarantined)) e=-FS_EIO;
    }
    fs_lock_drop(&c->lock); return e;
}
int cache_read_run(struct block_cache *c, struct blkdev *d, uint64_t lba, uint32_t count, void *buf) {
    if (!c || !d || !buf || d->sector_size!=512 || !count || count>BLKDEV_READ_SECTORS) return -FS_EINVAL;
    int admission=fs_lock_take_request(&c->lock,VFS_WAIT_CACHE); if (admission) return admission;
    if (c->refresh) c->refresh(c->refresh_ctx,d);
    int e=fs_request_check(); if (!e) e=blkdev_range(d,lba,count);
    if (!e && !d->read) e=-FS_EINVAL;
    if (!e && !device(c,d)) e=-FS_ENOSPC;
    struct cache_device *state=!e ? device(c,d) : 0;
    uint32_t epoch=state ? state->epoch : 0;
    uint32_t hits[BLKDEV_READ_SECTORS/32]={0}, generation=d->media_generation;
    for (unsigned i=0; !e && i<count; i++) {
        if (!(i & 15u)) fs_service(); /* 16 short hash walks plus <=8 KiB copy */
        struct cache_block *b=find(c,d,lba+i);
        if (b && b->busy && !readable_busy(b)) { e=wait_block(c,b); if (e) break; b=find(c,d,lba+i); }
        if (b) {
            prefetch_finish(c,b,true); c->demand_hits++;
            memcpy((uint8_t *)buf+i*512,b->data,512);
            hits[i/32]|=1u<<(i%32); c->hits++; touch(c,b);
        } else {
            struct cache_fill *fill=filling(c,d,lba+i);
            if (fill) {
                e=join_fill(c,fill,lba+i,1,(uint8_t *)buf+i*512);
                if (!e) hits[i/32]|=1u<<(i%32);
            }
        }
    }
    for (unsigned i=0; !e && i<count;) {
        if (!(i & 15u)) fs_service();
        if (hits[i/32] & (1u<<(i%32))) { i++; continue; }
        /* Recheck after any preceding unlocked transport. */
        struct cache_block *existing=find(c,d,lba+i);
        if (existing) {
            if (existing->busy && !readable_busy(existing)) { e=wait_block(c,existing); continue; }
            prefetch_finish(c,existing,true); c->demand_hits++;
            memcpy((uint8_t *)buf+i*512,existing->data,512); i++; continue;
        }
        struct cache_fill *fill=filling(c,d,lba+i);
        if (fill) { e=join_fill(c,fill,lba+i,1,(uint8_t *)buf+i*512); i++; continue; }
        unsigned end=i+1;
        while (end<count && !(hits[end/32] & (1u<<(end%32))) && !find(c,d,lba+end) && !filling(c,d,lba+end)) end++;
        struct cache_block *reserved[BLKDEV_READ_SECTORS]={0};
        /* Reserve only available clean slots. A run larger than the cache
         * still uses one command; uncached bytes remain in the caller buffer. */
        for (unsigned j=i; j<end; j++) {
            if (!(j & 15u)) fs_service();
            struct cache_block *b=0;
            unsigned work=0;
            for (unsigned list=0; list<2 && !b; list++)
                for (struct cache_block *x=c->lru[list].oldest; x; x=x->next) {
                    scan_service(&work);
                    if (!x->busy && !x->waiters && !x->pins && !x->dirty && !page_pinned(x->data)) { b=x; break; }
                }
            if (!b) continue;
            unhash(c,b); b->dev=d; b->lba=lba+j; b->generation=generation; b->sector_size=512;
            b->busy=true; b->invalid=false; b->io_error=0;
            unsigned h=bucket(d,b->lba); b->hash_next=c->hash[h]; c->hash[h]=b;
            touch(c,b); reserved[j]=b; c->misses++;
        }
        c->demand_misses+=end-i;
        if (file_read_cancelled && file_read_cancelled()) e=-4;
        if (!e) e=blkdev_range(d,lba+i,end-i);
        if (!e) {
            e=read_transport(c,d,lba+i,end-i,(uint8_t *)buf+i*512);
        }
        if (c->refresh) c->refresh(c->refresh_ctx,d);
        state=device(c,d);
        if (!e && (!state || epoch!=state->epoch || d->media_generation!=generation || d->quarantined)) e=-FS_EIO;
        for (unsigned j=i; j<end; j++) if (reserved[j] && reserved[j]->invalid && !e) e=-FS_EIO;
        for (unsigned j=i; j<end; j++) if (reserved[j]) {
            struct cache_block *b=reserved[j]; b->busy=false; b->io_error=e;
            if (e) unhash(c,b); else memcpy(b->data,(uint8_t *)buf+j*512,512);
        }
        fs_event_wake(&c->changed); i=end;
    }
    fs_lock_drop(&c->lock); return e;
}
int cache_read_ahead(struct block_cache *c, struct blkdev *d, uint64_t lba, uint32_t count, void *sector) {
    if (!c || !d || !sector || d->sector_size!=512 || !count || count>BLKDEV_READ_SECTORS) return -FS_EINVAL;
    int admission=fs_lock_take_request(&c->lock,VFS_WAIT_CACHE); if (admission) return admission;
    if (c->refresh) c->refresh(c->refresh_ctx,d);
    int e=fs_request_check(); if (!e) e=blkdev_range(d,lba,count);
    bool hit=!e && find(c,d,lba);
    fs_lock_drop(&c->lock);
    if (e) return e;
    if (hit || count==1) return cache_read(c,d,lba,sector);
    /* Bounded to the already qualified transport workspace size. Failure
     * preserves the demand-read path and never changes the write gate. */
    void *run=fs_contig_alloc((size_t)count*512);
    if (!run) return cache_read(c,d,lba,sector);
    e=cache_read_run(c,d,lba,count,run);
    if (!e) memcpy(sector,run,512);
    fs_contig_free(run,(size_t)count*512); return e;
}
static int write_batch(struct block_cache *, struct blkdev *, uint32_t *);
static int flush_device(struct block_cache *, struct blkdev *);
/* Reservation credits cover each pinned clean slot exactly once. Cleaning a
 * published slot restores its credit; publishing consumes it. Dirty slots
 * continue to count against the ordinary debt, even after release. */
static int reservation_status(struct cache_reservation *r, struct cache_device **out) {
    struct cache_device *s=device(r->cache,r->dev);
    if (out) *out=s;
    if (!current(s,r->dev,r->epoch,r->generation)) return -FS_EIO;
    return s->error;
}
static void release_locked(struct cache_reservation *r) {
    struct block_cache *c=r->cache;
    struct cache_device *s=device(c,r->dev);
    bool same=s && s->epoch==r->epoch;
    if (same) { c->reserved_bytes-=r->unassigned; s->reserved_bytes-=r->unassigned; }
    for (unsigned i=0;i<r->count;i++) {
        struct cache_block *b=r->blocks[i]; if (!b) continue;
        if (!b->invalid && !b->dirty) {
            c->reserved_bytes-=b->sector_size;
            if (same) s->reserved_bytes-=b->sector_size;
        }
        b->pins=0;
        if (b->invalid && !b->busy) { clean(c,b); unhash(c,b); }
        /* Filled from an image but never published: the bytes were never
         * accepted, so the slot must not survive as a clean cached copy. */
        else if ((r->filled&(1u<<i)) && !b->busy) { clean(c,b); unhash(c,b); }
    }
    memset(r,0,sizeof(*r)); fs_event_wake(&c->changed);
}
int cache_reserve(struct block_cache *c, struct blkdev *d,
                  const struct cache_reserve_sector *set, unsigned count, struct cache_reservation *r) {
    if (!c || !d || !set || !r || !count || count>CACHE_RESERVE_MAX) return -FS_EINVAL;
    memset(r,0,sizeof(*r));
    if (!blkdev_durable(d)) return -FS_EROFS;
    uint32_t bytes=count*d->sector_size;
    if (bytes>2*c->dirty_budget) return -FS_ENOSPC;
    unsigned sorted[CACHE_RESERVE_MAX];
    for (unsigned i=0;i<count;i++) {
        int e=blkdev_range(d,set[i].lba,1); if (e) return e;
        unsigned j=i;
        while (j && set[sorted[j-1]].lba>set[i].lba) { sorted[j]=sorted[j-1]; j--; }
        if (j && set[sorted[j-1]].lba==set[i].lba) return -FS_EINVAL;
        sorted[j]=i;
    }
    int e=fs_lock_take_request(&c->lock,VFS_WAIT_CACHE); if (e) return e;
    struct cache_device *s=device(c,d);
    if (!s) { fs_lock_drop(&c->lock); return -FS_ENOSPC; }
    *r=(struct cache_reservation){.cache=c,.dev=d,.generation=d->media_generation,.epoch=s->epoch,.count=count};
    /* Admission may assist a stalled writer, but never owns a VFS guard.
     * No credit-only deadlock: a competing reservation may be retried. */
    while (!(e=reservation_status(r,0)) &&
           (c->dirty_bytes+c->reserved_bytes+bytes>2*c->dirty_budget ||
            s->dirty_bytes+s->reserved_bytes+bytes>2*c->dirty_budget)) {
        if ((e=fs_request_check())) break;
        struct cache_device *debt=0;
        for (unsigned i=0;i<CACHE_DEVICES;i++) if (c->devices[i].dirty_bytes && !c->devices[i].error) {
            debt=&c->devices[i]; break;
        }
        if (!debt) { e=-FS_EAGAIN; break; }
        uint32_t flushed=0;
        e=write_batch(c,debt->dev,&flushed);
        if (e) break;
        if (!flushed && (e=fs_event_wait_request(&c->changed,&c->lock,VFS_WAIT_CACHE))) break;
    }
    if (!e) {
        c->reserved_bytes+=bytes; s->reserved_bytes+=bytes; r->unassigned=bytes;
        for (unsigned k=0;k<count;k++) {
            unsigned i=sorted[k]; struct cache_block *b=0;
            for (;;) {
                bool fresh=false;
                if ((e=reservation_status(r,0)) || (e=get(c,d,set[i].lba,!set[i].fill,&b,&fresh))) break;
                /* A miss with a fill image is hashed without a read and is
                 * filled before the lock can drop: never visible unfilled.
                 * A resident slot keeps its bytes until publication. */
                if (set[i].fill && fresh) { memcpy(b->data,set[i].fill,d->sector_size); r->filled|=1u<<i; }
                if ((e=reservation_status(r,0))) break;
                if (!b->pins) break;
                e=fs_event_wait_request(&c->changed,&c->lock,VFS_WAIT_CACHE); if (e) break;
            }
            if (e) break;
            b->pins=1; r->blocks[i]=b; r->unassigned-=d->sector_size;
            if (b->dirty) { c->reserved_bytes-=d->sector_size; s->reserved_bytes-=d->sector_size; }
            if (set[i].snapshot) memcpy(set[i].snapshot,b->data,d->sector_size);
        }
    }
    if (e) release_locked(r);
    fs_lock_drop(&c->lock); return e;
}
int cache_publish(struct cache_reservation *r, const struct cache_image *images, unsigned count, uint64_t dependency) {
    if (!r || !r->cache || !images || !count || count>r->count) return -FS_EINVAL;
    uint32_t seen=0;
    for (unsigned i=0;i<count;i++) {
        if (!images[i].data || images[i].slot>=r->count || (seen&(1u<<images[i].slot))) return -FS_EINVAL;
        seen|=1u<<images[i].slot;
    }
    struct block_cache *c=r->cache;
    if (!fs_lock_try_take(&c->lock)) return -FS_EAGAIN;
    struct cache_device *s; int e=reservation_status(r,&s);
    if (!e && (dependency>s->accepted || r->unassigned)) e=-FS_EINVAL;
    if (!e && (s->barrier_active || s->durable<dependency || s->durable<r->published)) e=-FS_EAGAIN;
    for (unsigned i=0;!e && i<count;i++) {
        struct cache_block *b=r->blocks[images[i].slot];
        if (!b || !b->pins || b->invalid || b->dev!=r->dev || b->generation!=r->generation) e=-FS_EIO;
        else if (b->busy) e=-FS_EAGAIN;
    }
    if (!e && (s->frontier==UINT64_MAX || s->accepted>UINT64_MAX-count)) e=-FS_EIO;
    if (!e) {
        uint64_t group=r->group_hint && r->group_hint==s->frontier ? r->group_hint : ++s->frontier;
        for (unsigned i=0;i<count;i++) {
            struct cache_block *b=r->blocks[images[i].slot];
            memcpy(b->data,images[i].data,r->dev->sector_size); b->order=group;
            r->filled&=~(1u<<images[i].slot);
            if (!b->dirty) {
                b->since=fs_now_ms();
                if (!s->dirty_bytes) s->oldest=b->since;
                c->reserved_bytes-=b->sector_size; s->reserved_bytes-=b->sector_size;
                c->dirty_bytes+=b->sector_size; s->dirty_bytes+=b->sector_size;
                schedule_writeback(c,b->since+CACHE_DIRTY_AGE_MS);
            }
            b->dirty=true;
        }
        s->accepted+=count; s->buffered_completions+=count;
        r->published=s->accepted; r->group=group; r->dependency=dependency;
        if (s->dirty_bytes>=c->dirty_budget) request_writeback(c,s);
    }
    fs_lock_drop(&c->lock); return e;
}
/* f1-89 read-set validation: copy a resident, settled sector. Never waits,
 * fills, touches the LRU or issues I/O; absent or being filled is EAGAIN.
 * A dirty block under write-back is busy but its bytes are stable (every
 * writer waits for busy), so it is readable here. */
int cache_peek(struct block_cache *c, struct blkdev *d, uint64_t lba, void *out) {
    if (!c || !d || !out) return -FS_EINVAL;
    fs_lock_take(&c->lock);
    struct cache_block *b=find(c,d,lba);
    int e=b && (!b->busy || b->dirty) && !b->invalid ? 0 : -FS_EAGAIN;
    if (!e) memcpy(out,b->data,d->sector_size);
    fs_lock_drop(&c->lock); return e;
}
int cache_reservation_wait(struct cache_reservation *r) {
    if (!r || !r->cache) return -FS_EINVAL;
    struct block_cache *c=r->cache;
    int e=fs_lock_take_request(&c->lock,VFS_WAIT_CACHE); if (e) return e;
    struct cache_device *s;
    while (!(e=reservation_status(r,&s))) {
        bool busy=s->barrier_active;
        for (unsigned i=0;i<r->count;i++) if (r->blocks[i]) busy|=r->blocks[i]->busy;
        if (!busy) break;
        if ((e=fs_event_wait_request(&c->changed,&c->lock,VFS_WAIT_CACHE))) break;
    }
    fs_lock_drop(&c->lock); return e;
}
void cache_release(struct cache_reservation *r) {
    if (!r || !r->cache) return;
    struct block_cache *c=r->cache;
    (void)fs_lock_retake(&c->lock,VFS_WAIT_CACHE); release_locked(r); fs_lock_drop(&c->lock);
}
int cache_write(struct block_cache *c, struct blkdev *d, uint64_t lba, const void *buf) {
    if (c->refresh) c->refresh(c->refresh_ctx,d);
    if (!buf || !blkdev_durable(d)) return d && d->quarantined ? -FS_EQUARANTINED : -FS_EROFS;
    int admission=fs_lock_take_request(&c->lock,VFS_WAIT_CACHE); if (admission) return admission;
    struct cache_device *s=device(c,d); struct cache_block *b;
    int e=!s ? -FS_ENOSPC : s->error;
    uint32_t generation=d->media_generation, epoch=s ? s->epoch : 0;
    if (!e && !current(s,d,epoch,generation)) e=-FS_EIO;
    if (!e && c->writer_online && s->dirty_bytes>2*c->dirty_budget) {
        uint32_t start=fs_now_ms(); c->writer_waits++;
        uint32_t progress=start, previous=s->dirty_bytes; bool assist=false;
        while (!(e=fs_request_check()) && !s->error && s->epoch==epoch && generation==d->media_generation && !d->quarantined &&
               s->dirty_bytes>=c->dirty_budget && c->writer_online) {
            request_writeback(c,s);
            if (assist) {
                /* The shared service may be waiting for this caller's volume
                 * to drain a boot log. Make progress on our own debt instead
                 * of retaining that reservation while waiting indefinitely. */
                uint32_t flushed=0; e=write_batch(c,d,&flushed);
                if (!e && flushed) e=flush_device(c,d);
                if (e) break;
                if (!flushed) wait_changed(c,CACHE_CLEAN_WAIT_MS);
            } else {
                wait_changed(c,CACHE_CLEAN_WAIT_MS);
                if (s->dirty_bytes<previous) { previous=s->dirty_bytes; progress=fs_now_ms(); }
                else if ((uint32_t)(fs_now_ms()-progress)>=CACHE_CLEAN_WAIT_MS) assist=true;
            }
            if (c->refresh) c->refresh(c->refresh_ctx,d);
        }
        uint32_t ms=fs_now_ms()-start; c->writer_wait_ms+=ms;
        if (ms>c->max_writer_wait_ms) c->max_writer_wait_ms=ms;
        if (!e) e=s->error;
        if (!e && (s->epoch!=epoch || generation!=d->media_generation || d->quarantined)) e=-FS_EIO;
    }
retry_slot:
    if (!e) e=get(c,d,lba,false,&b,0);
    s=device(c,d);
    if (!e && (!s || epoch!=s->epoch || generation!=d->media_generation || d->quarantined)) {
        if (!b->dirty) unhash(c,b);
        e=-FS_EIO;
    }
    /* get/throttle may sleep. Do not publish bytes into a captured barrier.
     * Reacquire the slot after waiting: it may have been evicted meanwhile. */
    while (!e && (s->barrier_active || b->pins)) {
        e=fs_event_wait_request(&c->changed,&c->lock,VFS_WAIT_CACHE);
        if (e) break;
        if (!current(s,d,epoch,generation)) e=-FS_EIO;
        else if (s->error) e=s->error;
        else if (!s->barrier_active) e=get(c,d,lba,false,&b,0);
    }
    if (!e && !current(s,d,epoch,generation)) e=-FS_EIO;
    /* Ordinary writers cannot spend a transaction's promised clean credits.
     * Re-fetch after every sleep; their earlier slot was not pinned. */
    if (!e && !b->dirty && (c->reserved_bytes || s->reserved_bytes) &&
           (c->dirty_bytes+c->reserved_bytes+d->sector_size>2*c->dirty_budget ||
            s->dirty_bytes+s->reserved_bytes+d->sector_size>2*c->dirty_budget)) {
        e=fs_event_wait_request(&c->changed,&c->lock,VFS_WAIT_CACHE);
        if (!e && !current(s,d,epoch,generation)) e=-FS_EIO;
        if (!e) goto retry_slot;
    }
    if (!e && s->accepted==UINT64_MAX) e=remember(c,d,-FS_EIO);
    if (!e) {
        s->accepted++; s->buffered_completions++;
        memcpy(b->data,buf,d->sector_size);
        b->order=s->frontier;
        if (!b->dirty) {
            b->since=fs_now_ms();
            if (!s->dirty_bytes) s->oldest=b->since;
            s->dirty_bytes+=d->sector_size; c->dirty_bytes+=d->sector_size;
            schedule_writeback(c,b->since+CACHE_DIRTY_AGE_MS);
        }
        b->dirty=true;
        if (s->dirty_bytes>=c->dirty_budget) request_writeback(c,s);
    }
    fs_lock_drop(&c->lock); return e;
}
static void select_dirty(struct block_cache *c, struct blkdev *d, struct cache_block *b,
                         struct cache_block **blocks, unsigned *count, unsigned limit)
{
    if (b->dev!=d || !b->dirty || b->invalid || b->busy || b->waiters) return;
    unsigned n=*count, at=0;
    while (at<n && blocks[at]->lba<b->lba) at++;
    if (at>=limit) return;
    if (n<limit) n++;
    else {
        struct cache_block *old=blocks[n-1]; old->busy=false;
        if (old->invalid) { clean(c,old); unhash(c,old); old->io_error=-FS_EIO; }
    }
    for (unsigned j=n-1;j>at;j--) blocks[j]=blocks[j-1];
    blocks[at]=b; b->busy=true; *count=n;
}
/* A bounded selection in LBA order. Pins every selected block before any
 * unlock; bytes are immutable until that reservation is completed. The
 * preallocated bounce buffer lets discontiguous cache pages share commands.
 * Cache lock held on entry/return, never during a transport or timed wait. */
static int write_batch(struct block_cache *c, struct blkdev *d, uint32_t *flushed) {
    struct cache_device *s=device(c,d); if (!s) return -FS_ENOSPC;
    if (s->error) return s->error;
    uint32_t generation=d->media_generation, epoch=s->epoch;
    while (c->batch_busy) { int e=fs_event_wait_request(&c->changed,&c->lock,VFS_WAIT_CACHE); if (e) return e; }
    if (c->refresh) c->refresh(c->refresh_ctx,d);
    /* The buffer wait may outlive this media or even reuse its device slot.
     * An old barrier must not borrow a replacement device's dirty count. */
    if (s->dev!=d || s->epoch!=epoch || generation!=d->media_generation) return -FS_EIO;
    if (s->error) return s->error;
    if (!blkdev_durable(d)) return remember(c,d,d->quarantined ? -FS_EQUARANTINED : -FS_EROFS);
    struct cache_block *blocks[CACHE_WRITEBACK_BLOCKS]; unsigned n=0;
    unsigned work=0;
    struct cache_scan scan={0};
    c->batch_busy=true;
    unsigned limit=c->writeback_buffer ? c->writeback_pages*4096u/d->sector_size : 1;
    if (limit>CACHE_WRITEBACK_BLOCKS) limit=CACHE_WRITEBACK_BLOCKS;
    /* Keep the short newest-first fast path used by FAT commits. If the
     * scan is longer, switch to a stable descriptor cursor; no LRU cursor
     * survives service. Selected slots are busy until transport/cancel. */
    unsigned seen=0, probes=0;
    for (unsigned list=0;list<2;list++)
        for (struct cache_block *b=c->lru[list].newest;b;b=b->prev) {
            if (++probes>32) goto long_scan;
            if (b->dev==d && b->dirty && !b->invalid) seen+=b->sector_size;
            select_dirty(c,d,b,blocks,&n,limit);
            if (seen>=s->dirty_bytes) goto selected;
        }
    goto selected;
long_scan:
    fs_service();
    for (;;) {
        scan_service(&work);
        struct cache_block *b=scan_next(c,&scan); if (!b) break;
        if (s->epoch!=epoch || s->dev!=d || generation!=d->media_generation || s->error) goto cancelled;
        select_dirty(c,d,b,blocks,&n,limit);
        /* New dirty entries behind this cursor are requested by the age
         * pass; they cannot extend this finite batch's selection forever. */
    }
selected:
    if (!n) { c->batch_busy=false; fs_event_wake(&c->changed); return 0; }
    for (unsigned i=0;i<n;i++) {
        if (blocks[i]->invalid || blocks[i]->generation!=generation) goto cancelled;
        int e=blkdev_range(d,blocks[i]->lba,1); if (e) { remember(c,d,e); goto cancelled; }
    }
    for (unsigned i=0;i<n;i++) {
        blocks[i]->busy=true;
        if (c->writeback_buffer) memcpy(c->writeback_buffer+i*d->sector_size,blocks[i]->data,d->sector_size);
    }
    int result=0; unsigned completed=0;
    for (unsigned i=0;i<n;) {
        unsigned end=i+1;
        if (c->writeback_buffer) while (end<n && blocks[end]->lba==blocks[end-1]->lba+1 &&
            blocks[end]->order==blocks[i]->order) end++;
        if (c->refresh) c->refresh(c->refresh_ctx,d);
        int e=s->error;
        if (!e && (s->epoch!=epoch || generation!=d->media_generation || d->quarantined)) e=-FS_EIO;
        for (unsigned j=i;j<end;j++) if (blocks[j]->invalid) e=-FS_EIO;
        if (!e) {
            const void *data=c->writeback_buffer ? c->writeback_buffer+i*d->sector_size : blocks[i]->data;
            uint64_t lba=blocks[i]->lba;
            uint64_t order=blocks[i]->order;
            fs_lock_drop(&c->lock); e=blkdev_request(d,BLK_WRITE,lba,end-i,(void *)data,BLK_BACKGROUND,order); (void)fs_lock_retake(&c->lock,VFS_WAIT_CACHE);
            if (c->refresh) c->refresh(c->refresh_ctx,d);
            if (!e && (s->epoch!=epoch || generation!=d->media_generation || d->quarantined)) e=-FS_EIO;
            for (unsigned j=i;j<end;j++) if (blocks[j]->invalid) e=-FS_EIO;
        }
        if (e && s->epoch==epoch && generation==d->media_generation) remember(c,d,e);
        if (e) { result=e; break; }
        for (unsigned j=i;j<end;j++) {
            clean(c,blocks[j]); blocks[j]->io_error=0; c->writes++; *flushed+=d->sector_size;
            blocks[j]->busy=false;
        }
        /* Completed descriptors may now be read, redirtied or evicted.
         * Never dereference them again while later groups do transport. */
        completed=end; fs_event_wake(&c->changed);
        i=end;
    }
    for (unsigned i=completed;i<n;i++) {
        struct cache_block *b=blocks[i]; b->busy=false;
        if (b->invalid) { clean(c,b); unhash(c,b); b->io_error=-FS_EIO; }
    }
    c->batch_busy=false; fs_event_wake(&c->changed); return result;
cancelled:
    for (unsigned i=0;i<n;i++) {
        struct cache_block *b=blocks[i]; b->busy=false;
        if (b->invalid) { clean(c,b); unhash(c,b); b->io_error=-FS_EIO; }
    }
    c->batch_busy=false; fs_event_wake(&c->changed);
    return s->epoch==epoch && s->error ? s->error : -FS_EIO;
}
static int flush_device(struct block_cache *c, struct blkdev *d) {
    struct cache_device *s=device(c,d); if (!s) return -FS_ENOSPC;
    if (s->error) return s->error;
    if (c->refresh) c->refresh(c->refresh_ctx,d);
    if (!current(s,d,s->epoch,s->generation)) return -FS_EIO;
    if (!blkdev_durable(d)) return remember(c,d,d->quarantined ? -FS_EQUARANTINED : -FS_EROFS);
    if (d->write_cache_state!=BLKDEV_CACHE_DISABLED) {
        uint32_t generation=d->media_generation,epoch=s->epoch;
        uint64_t frontier=s->frontier;
        fs_lock_drop(&c->lock); int e=blkdev_request(d,BLK_FLUSH,0,0,0,BLK_BACKGROUND,frontier); (void)fs_lock_retake(&c->lock,VFS_WAIT_CACHE);
        if (c->refresh) c->refresh(c->refresh_ctx,d);
        if (s->epoch!=epoch || generation!=d->media_generation) return -FS_EIO;
        if (!e && d->quarantined) e=-FS_EIO;
        if (e) return remember(c,d,e);
    }
    return 0;
}
static void completion_record(struct cache_device *s, struct cache_completion *out) {
    if (out) *out=(struct cache_completion){.accepted=s->accepted,.durable=s->durable,
        .group=s->frontier,.buffered_completions=s->buffered_completions,
        .durable_completions=s->durable_completions,.generation=s->generation,
        .epoch=s->epoch,.error=s->error,.incomplete=s->durability_lost || s->durable<s->accepted};
}
static int barrier(struct block_cache *c, struct blkdev *d, struct cache_completion *out) {
    if (c->refresh) c->refresh(c->refresh_ctx,d);
    struct cache_device *s=device(c,d); if (!s) return -FS_ENOSPC;
    uint32_t epoch=s->epoch,generation=s->generation;
    while (s->barrier_active && current(s,d,epoch,generation)) {
        int e=fs_event_wait_request(&c->changed,&c->lock,VFS_WAIT_CACHE); if (e) return e;
    }
    if (!current(s,d,epoch,generation)) return -FS_EIO;
    if (s->error) { completion_record(s,out); return s->error; }
    if (!blkdev_durable(d)) return remember(c,d,-FS_EROFS);
    s->barrier_active=true;
    uint64_t version=s->accepted; /* finite: write admission is frozen */
    completion_record(s,out);
    int e=0;
    while (s->dirty_bytes) {
        uint32_t flushed=0; e=write_batch(c,d,&flushed); if (e) break;
        if (!current(s,d,epoch,generation)) { e=-FS_EIO; break; }
        if (!flushed) e=fs_event_wait_request(&c->changed,&c->lock,VFS_WAIT_CACHE);
        if (e) break;
        if (!current(s,d,epoch,generation)) { e=-FS_EIO; break; }
    }
    if (!e) e=flush_device(c,d);
    if (current(s,d,epoch,generation)) {
        if (!e) { s->durable=version; s->durable_completions++; }
        else s->durability_lost=true;
        s->barrier_active=false; completion_record(s,out);
    } else { e=-FS_EIO; if (out) { out->error=e; out->incomplete=true; } }
    fs_event_wake(&c->changed); return e;
}
int cache_barrier_record(struct block_cache *c, struct blkdev *d, struct cache_completion *out) {
    if (out) memset(out,0,sizeof(*out));
    int e=fs_lock_take_request(&c->lock,VFS_WAIT_CACHE);
    if (e) { if (out) { out->error=e; out->incomplete=true; } return e; }
    e=barrier(c,d,out);
    if (out && e) { out->error=e; out->incomplete=true; }
    fs_lock_drop(&c->lock); return e;
}
int cache_barrier(struct block_cache *c, struct blkdev *d) { return cache_barrier_record(c,d,0); }
int cache_completion(struct block_cache *c, struct blkdev *d, struct cache_completion *out) {
    if (!out || !d) return -FS_EINVAL;
    int e=fs_lock_take_request(&c->lock,VFS_WAIT_CACHE); if (e) return e;
    struct cache_device *s=device(c,d); e=s ? 0 : -FS_ENOSPC;
    if (s) { completion_record(s,out); if (!current(s,d,s->epoch,s->generation)) { out->error=-FS_EIO; out->incomplete=true; e=-FS_EIO; } }
    fs_lock_drop(&c->lock); return e;
}
int cache_group_begin(struct block_cache *c, struct blkdev *d, uint64_t *group) {
    if (!d || !group) return -FS_EINVAL;
    int e=fs_lock_take_request(&c->lock,VFS_WAIT_CACHE); if (e) return e;
    struct cache_device *s=device(c,d); e=s ? 0 : -FS_ENOSPC;
    if (!e) {
        uint32_t epoch=s->epoch,generation=s->generation;
        while (s->barrier_active && current(s,d,epoch,generation) && !e)
            e=fs_event_wait_request(&c->changed,&c->lock,VFS_WAIT_CACHE);
        if (!e) {
            if (!current(s,d,epoch,generation)) e=-FS_EIO;
            else if (s->error) e=s->error;
            else if (s->frontier==UINT64_MAX) e=remember(c,d,-FS_EIO);
            else *group=++s->frontier;
        }
    }
    fs_lock_drop(&c->lock); return e;
}
void cache_media_lost(struct block_cache *c, struct blkdev *d) {
    (void)fs_lock_retake(&c->lock,VFS_WAIT_CACHE); struct cache_device *s=device(c,d);
    if (s) { s->durability_lost=true; if (!s->error) s->error=-FS_EIO; }
    fs_lock_drop(&c->lock);
    cache_invalidate(c,d,false);
}
bool cache_writeback_due(const struct block_cache *c, uint32_t now) {
    return __atomic_load_n(&c->writeback_pending,__ATOMIC_ACQUIRE) &&
        (int32_t)(now-__atomic_load_n(&c->next_writeback,__ATOMIC_ACQUIRE))>=0;
}
void cache_writer_wait(struct block_cache *c, uint32_t ms) {
    (void)fs_lock_retake(&c->lock,VFS_WAIT_CACHE);
    if (!cache_writeback_due(c,fs_now_ms())) wait_changed(c,ms);
    fs_lock_drop(&c->lock);
}
bool cache_writeback_report(struct block_cache *c, uint32_t now, struct cache_stats *out) {
    (void)fs_lock_retake(&c->lock,VFS_WAIT_CACHE); bool report=false;
    if (!c->dirty_bytes) c->report_time=now;
    else if ((uint32_t)(now-c->report_time)>=5000) {
        c->report_time=now; stats_locked(c,out); report=true;
    }
    fs_lock_drop(&c->lock); return report;
}
int cache_writeback_tick(struct block_cache *c, uint32_t now) {
    (void)fs_lock_retake(&c->lock,VFS_WAIT_CACHE); int result=0; c->tick_flushed=0;
    if (!cache_writeback_due(c,now)) { fs_lock_drop(&c->lock); return 0; }
    uint32_t start=fs_now_ms();
    for (unsigned i=0;i<CACHE_DEVICES;i++) {
        struct blkdev *d=c->devices[i].dev; if (!d) continue;
        struct cache_device *s=&c->devices[i];
        if (s->error || !s->dirty_bytes) continue;
        bool due=s->requested || s->dirty_bytes>=c->dirty_budget || (uint32_t)(now-s->oldest)>=CACHE_DIRTY_AGE_MS;
        if (due) {
            s->requested=false;
            int e=write_batch(c,d,&c->tick_flushed);
            if (!e) e=flush_device(c,d);
            if (e && !result) result=e;
        }
    }
    __atomic_store_n(&c->writeback_pending,false,__ATOMIC_RELEASE);
    for (unsigned i=0;i<CACHE_DEVICES;i++) c->devices[i].oldest=now;
    unsigned work=0; struct cache_scan scan={0};
    for (;;) {
        scan_service(&work);
        struct cache_block *b=scan_next(c,&scan); if (!b) break;
        struct cache_device *owner=b->dirty && !b->invalid ? device(c,b->dev) : 0;
        if (owner && !owner->error) {
            if ((int32_t)(b->since-owner->oldest)<0) owner->oldest=b->since;
            schedule_writeback(c,owner->requested || owner->dirty_bytes>=c->dirty_budget ? fs_now_ms() : b->since+CACHE_DIRTY_AGE_MS);
        }
    }
    uint32_t ms=fs_now_ms()-start; if (ms>c->max_flush_ms) c->max_flush_ms=ms;
    fs_lock_drop(&c->lock); return result;
}
int cache_error(struct block_cache *c, struct blkdev *d) {
    if (c->refresh) c->refresh(c->refresh_ctx,d);
    if (d && d->quarantined) return -FS_EIO;
    (void)fs_lock_retake(&c->lock,VFS_WAIT_CACHE); struct cache_device *s=device(c,d);
    int e=s ? s->error : -FS_ENOSPC; fs_lock_drop(&c->lock); return e;
}
void cache_invalidate(struct block_cache *c, struct blkdev *d, bool forget) {
    (void)fs_lock_retake(&c->lock,VFS_WAIT_CACHE);
    unsigned work=0;
    for (struct cache_block *b=first_block(c);b;b=next_block(c,b)) {
        scan_service(&work);
        if (b->dev!=d) continue;
        if (b->busy || b->pins) {
            if (b->dirty && !b->invalid) { c->dirty_bytes-=b->sector_size; device(c,d)->dirty_bytes-=b->sector_size; }
            if (b->pins && !b->dirty && !b->invalid) {
                c->reserved_bytes-=b->sector_size; device(c,d)->reserved_bytes-=b->sector_size;
            }
            b->invalid=true;
            if (!b->busy) unhash(c,b);
        } else { clean(c,b); unhash(c,b); }
    }
    fs_event_wake(&c->changed);
    for (unsigned i=0;i<CACHE_DEVICES;i++) if (c->devices[i].dev==d) {
        c->reserved_bytes-=c->devices[i].reserved_bytes; c->devices[i].reserved_bytes=0;
        if (c->device_epoch==UINT32_MAX) fs_panic("cache epoch exhausted");
        c->devices[i].epoch=++c->device_epoch; c->devices[i].barrier_active=false;
        if (forget) memset(&c->devices[i],0,sizeof(c->devices[i]));
    }
    fs_lock_drop(&c->lock);
}
int cache_workspace_begin(struct block_cache *c, uint32_t bytes) {
    if (bytes>c->workspace_pages*4096u) return -FS_ENOMEM;
    fs_lock_take(&c->workspace_lock);
    if (c->workspace_in_use) { fs_lock_drop(&c->workspace_lock); return -FS_EBUSY; }
    c->workspace_in_use=true;
    fs_lock_drop(&c->workspace_lock);
    for (unsigned i=0;i<(bytes+4095)/4096;i++) { memset(c->workspace[i],0,4096); fs_service(); }
    return 0;
}
uint8_t *cache_workspace_byte(struct block_cache *c, uint32_t off) {
    if (off>=c->workspace_pages*4096u) fs_panic("workspace bounds");
    return (uint8_t *)c->workspace[off/4096]+off%4096;
}
void cache_workspace_end(struct block_cache *c) {
    fs_lock_take(&c->workspace_lock); c->workspace_in_use=false; fs_lock_drop(&c->workspace_lock);
}
