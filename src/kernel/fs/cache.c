/* SPDX-License-Identifier: GPL-2.0-only */
#include "cache.h"
/* Optional in the standalone F1 host model; attributes a successful physical
 * read to the native caller (or the child it is preparing), never a global delta. */
void proc_launch_disk_read(uint32_t bytes);
#pragma weak proc_launch_disk_read
extern bool file_read_cancelled(void) __attribute__((weak));

/* f4-04 page layouts (docs/design/f4-acceptance.md, "## f4-04"). Data pages
 * hold only media bytes: a page is two 2048-byte halves and a half is four
 * 512-byte slots or one 2048-byte slot. Descriptors (struct cache_block) are
 * packed into descriptor pages carved from the same byte budget, the first
 * page header living inside struct block_cache. All pages are allocated by
 * cache_init; attaching a 2048-byte device only converts halves in place. */
#define HALF 2048u
#define SLOTS_512 4u
_Static_assert(sizeof(struct cache_page) <= 4096, "cache page header");
#define PAGES_PER_META ((4096u - (unsigned)offsetof(struct cache_meta, pages)) / (unsigned)sizeof(struct cache_page))
_Static_assert(PAGES_PER_META >= 1, "descriptor page");
#ifdef __i386__
/* On the real target a descriptor page holds 11 page headers: a cache of 20 or
 * more pages packs at least F1's 7 FAT slots per data/metadata budget page.
 * The default, including its 8-page writeback buffer, retains 14016 slots
 * against F1's 13440. The 64-bit host packs fewer headers. */
_Static_assert(PAGES_PER_META >= 11, "i686 descriptor density");
#endif

static unsigned cls(const struct cache_block *b) { return b->capacity >= 2048 ? 1u : 0u; }
static unsigned bucket(struct blkdev *d, uint64_t lba) { return (unsigned)((uintptr_t)d/16 ^ lba ^ (lba>>32)) & 255; }
static struct cache_device *device(struct block_cache *c, struct blkdev *d) {
    for (unsigned i=0;i<CACHE_DEVICES;i++) if (c->devices[i].dev==d) return &c->devices[i];
    for (unsigned i=0;i<CACHE_DEVICES;i++) if (!c->devices[i].dev) {
        if (c->device_epoch==UINT32_MAX) return 0;
        c->devices[i].dev=d; c->devices[i].epoch=++c->device_epoch; return &c->devices[i];
    }
    return 0;
}
static int remember(struct block_cache *c, struct blkdev *d, int e) {
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
    }
    b->dirty=false;
}
/* Local timed event wait: preserve the port's event sequence handshake, but
 * do not turn a missing writer into an unbounded reader allocation wait. */
static void wait_changed(struct block_cache *c, uint32_t ms) {
#ifdef FS_HOST
    struct timespec end; clock_gettime(CLOCK_REALTIME,&end);
    end.tv_sec+=ms/1000; end.tv_nsec+=(long)(ms%1000)*1000000;
    if (end.tv_nsec>=1000000000) { end.tv_sec++; end.tv_nsec-=1000000000; }
    uint32_t seq=__atomic_load_n(&c->changed.sequence,__ATOMIC_ACQUIRE);
    fs_lock_drop(&c->lock); pthread_mutex_lock(&c->changed.mutex);
    while (seq==__atomic_load_n(&c->changed.sequence,__ATOMIC_ACQUIRE))
        if (pthread_cond_timedwait(&c->changed.changed,&c->changed.mutex,&end)) break;
    pthread_mutex_unlock(&c->changed.mutex); fs_lock_take(&c->lock);
#else
    struct fs_event_ticket t={&c->changed,c->changed.sequence}; fs_lock_drop(&c->lock);
    kwait_wait_until(&c->changed.wait,fs_event_changed,&t,deadline_after_ms(ms)); fs_lock_take(&c->lock);
#endif
}
static struct cache_fill *filling(struct block_cache *c, struct blkdev *d, uint64_t lba) {
    for (struct cache_fill *f=c->fills; f; f=f->next)
        if (f->dev==d && lba>=f->lba && lba-f->lba<f->count) return f;
    return 0;
}
static int join_fill(struct block_cache *c, struct cache_fill *f, uint64_t lba, uint32_t count, void *buf) {
    f->waiters++;
    while (!f->complete) fs_event_wait(&c->changed,&c->lock);
    int e=f->error;
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
    int e=d->read(d,lba,count,buf);
    fs_lock_take(&c->lock);
    if (c->refresh) c->refresh(c->refresh_ctx,d);
    state=device(c,d);
    if (!e && (!state || state->epoch!=epoch || d->media_generation!=generation || d->quarantined)) e=-FS_EIO;
    if (!e && proc_launch_disk_read) proc_launch_disk_read(count*d->sector_size);
    fill.error=e; fill.complete=true; fs_event_wake(&c->changed);
    while (fill.waiters) fs_event_wait(&c->changed,&c->lock);
    struct cache_fill **at=&c->fills; while (*at!=&fill) at=&(*at)->next;
    *at=fill.next; return e;
}
static int wait_block(struct block_cache *c, struct cache_block *b) {
    if (b->waiters==255) fs_panic("cache waiters");
    b->waiters++;
    while (b->busy) fs_event_wait(&c->changed,&c->lock);
    struct cache_device *owner=b->dirty ? device(c,b->dev) : 0;
    int e=owner ? owner->error : b->io_error;
    if (b->invalid || (b->dev && b->generation!=b->dev->media_generation)) e=-FS_EIO;
    b->waiters--; fs_event_wake(&c->changed);
    return e;
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
    e=d->write(d,b->lba,1,b->data);
    fs_lock_take(&c->lock);
    if (c->refresh) c->refresh(c->refresh_ctx,d);
    if (!e && (b->invalid || generation!=d->media_generation || d->quarantined)) e=-FS_EIO;
    b->busy=false;
    if (e) { b->since=since; if (!b->invalid && generation==d->media_generation) remember(c,d,e); } else { clean(c,b); b->io_error=0; c->writes++; }
    if (b->invalid) { clean(c,b); unhash(c,b); b->io_error=e; }
    fs_event_wake(&c->changed); return e;
}
static void unhash(struct block_cache *c, struct cache_block *b) {
    if (!b->dev) return;
    struct cache_block **p=&c->hash[bucket(b->dev,b->lba)];
    while (*p && *p!=b) p=&(*p)->hash_next;
    if (*p) *p=b->hash_next;
    b->hash_next=0; b->dev=0;
}
/* Two LRU lists, one per slot capacity, so a 2048-byte miss never walks the
 * many 512-byte slots. Maintenance loops visit both through first/next. */
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
    /* Never drop the mutex halfway through changing a layout. A dirty
     * write may sleep, so restart the complete half preflight afterwards. */
    for (unsigned k=0;k<SLOTS_512;k++) {
        struct cache_block *b=&pg->b[h*SLOTS_512+k];
        if (b->busy || b->waiters) return -FS_EBUSY;
        if (b->dirty) {
            int e=write_block(c,b); if (e) return e;
            return -FS_EAGAIN; /* recheck every slot after the unlocked write */
        }
    }
    half_deactivate(c,pg,h); half_activate(c,pg,h,1,true); return 0;
}
static void convert_page(struct block_cache *c, struct cache_page *pg, unsigned halves, unsigned *done, int *blocked) {
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
    for (struct cache_block *b=c->hash[bucket(d,lba)]; b; b=b->hash_next)
        if (b->dev==d && b->lba==lba && b->generation==d->media_generation && b->sector_size==d->sector_size) return b;
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
    int e=blkdev_range(d,lba,1); if (e) return e;
    if (!d->read) return -FS_EINVAL;
    if (!device(c,d)) return -FS_ENOSPC;
    if ((e=layout_for(c,d))) return e;
    struct cache_block *b=find(c,d,lba);
    if (b) {
        if (b->busy) { if (issued && read) *issued=true; e=wait_block(c,b); if (e) return e; goto retry; }
        c->hits++; touch(c,b); *out=b; return 0;
    }
    int blocked=-FS_ENOMEM; bool pending=false; struct cache_block *dirty=0;
    for (unsigned list=d->sector_size==2048 ? 1u : 0u; list<2 && !b; list++) {
        for (struct cache_block *x=c->lru[list].oldest; x; x=x->next) {
            if (x->capacity<d->sector_size) continue;
            if (x->busy || x->waiters) { pending=true; continue; }
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
        if (pending) { fs_event_wait(&c->changed,&c->lock); goto retry; }
        return blocked;
    }
    unhash(c,b); c->misses++;
    b->dev=d; b->lba=lba; b->generation=d->media_generation; b->sector_size=(uint16_t)d->sector_size;
    b->invalid=false; b->io_error=0;
    unsigned h=bucket(d,lba); b->hash_next=c->hash[h]; c->hash[h]=b;
    touch(c,b);
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
    }
    c->workspace_pages=(uint32_t)(pages/16); if (!c->workspace_pages) c->workspace_pages=1;
    if (c->workspace_pages>256) c->workspace_pages=256;
    for (unsigned i=0;i<c->workspace_pages;i++) if (!(c->workspace[i]=fs_page_alloc())) goto fail;
    /* Budget: workspace + writeback + data + descriptors == bytes/4096. */
    unsigned room=(unsigned)(pages-c->workspace_pages-c->writeback_pages), data=room;
    while (data && data+meta_needed(data)>room) data--;
    if (!data) goto fail;
    struct cache_meta *m=0;
    for (unsigned i=0;i<data;i++) {
        struct cache_page *pg;
        if (!i) pg=&c->inline_page;
        else {
            if (!m || m->count==PAGES_PER_META) {
                struct cache_meta *n=fs_page_alloc(); if (!n) goto fail;
                n->next=c->meta; c->meta=n; c->meta_pages++; m=n;
            }
            pg=&m->pages[m->count];
        }
        pg->data=fs_page_alloc(); if (!pg->data) goto fail;
        if (i) m->count++;
        c->data_pages++;
        half_activate(c,pg,0,0,false); half_activate(c,pg,1,0,false);
    }
    return 0;
fail: cache_destroy(c); return -FS_ENOMEM;
}
void cache_destroy(struct block_cache *c) {
    fs_contig_free(c->writeback_buffer,c->writeback_pages*4096u);
    if (c->inline_page.data) fs_page_free(c->inline_page.data);
    while (c->meta) {
        struct cache_meta *m=c->meta; c->meta=m->next;
        for (unsigned i=0;i<m->count;i++) fs_page_free(m->pages[i].data);
        fs_page_free(m);
    }
    for (unsigned i=0;i<c->workspace_pages;i++) fs_page_free(c->workspace[i]);
    fs_lock_destroy(&c->lock); fs_lock_destroy(&c->workspace_lock); fs_event_destroy(&c->changed);
    memset(c,0,sizeof(*c));
}
int cache_attach(struct block_cache *c, struct blkdev *d) {
    if (!c || !d || (d->sector_size!=512 && d->sector_size!=2048)) return -FS_EINVAL;
    fs_lock_take(&c->lock);
    int e=device(c,d) ? layout_for(c,d) : -FS_ENOSPC;
    fs_lock_drop(&c->lock); return e;
}
static void stats_locked(struct block_cache *c, struct cache_stats *out) {
    *out=(struct cache_stats){ .pages=c->workspace_pages+c->data_pages+c->meta_pages+c->writeback_pages,
        .data_pages=c->data_pages, .meta_pages=c->meta_pages, .workspace_pages=c->workspace_pages,
        .writeback_pages=c->writeback_pages,
        .slots_512=c->slots[0], .slots_2048=c->slots[1], .hits=c->hits, .misses=c->misses, .writes=c->writes,
        .dirty_bytes=c->dirty_bytes,.dirty_budget=c->dirty_budget,.throttle_bytes=2*c->dirty_budget,
        .tick_flushed=c->tick_flushed,.max_flush_ms=c->max_flush_ms,.max_writer_wait_ms=c->max_writer_wait_ms,
        .cache_sync_evictions=c->cache_sync_evictions,.writer_waits=c->writer_waits,.writer_wait_ms=c->writer_wait_ms };
}
void cache_stats(struct block_cache *c, struct cache_stats *out) {
    fs_lock_take(&c->lock); stats_locked(c,out);
    fs_lock_drop(&c->lock);
}
int cache_read(struct block_cache *c, struct blkdev *d, uint64_t lba, void *buf) {
    if (!buf) return -FS_EINVAL;
    fs_lock_take(&c->lock); struct cache_block *b;
    struct cache_fill *fill=d ? filling(c,d,lba) : 0;
    if (fill && !find(c,d,lba)) {
        int e=join_fill(c,fill,lba,1,buf); fs_lock_drop(&c->lock); return e;
    }
    bool issued=false;
    uint32_t generation=d ? d->media_generation : 0;
    int e=get(c,d,lba,true,&b,&issued);
    if (!e) memcpy(buf,b->data,d->sector_size);
    else if (!issued && !blkdev_range(d,lba,1) && d->read) {
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
    fs_lock_take(&c->lock);
    if (c->refresh) c->refresh(c->refresh_ctx,d);
    int e=blkdev_range(d,lba,count);
    if (!e && !d->read) e=-FS_EINVAL;
    if (!e && !device(c,d)) e=-FS_ENOSPC;
    struct cache_device *state=!e ? device(c,d) : 0;
    uint32_t epoch=state ? state->epoch : 0;
    uint32_t hits[BLKDEV_READ_SECTORS/32]={0}, generation=d->media_generation;
    for (unsigned i=0; !e && i<count; i++) {
        struct cache_block *b=find(c,d,lba+i);
        if (b && b->busy) { e=wait_block(c,b); if (e) break; b=find(c,d,lba+i); }
        if (b) {
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
        if (hits[i/32] & (1u<<(i%32))) { i++; continue; }
        /* Recheck after any preceding unlocked transport. */
        struct cache_block *existing=find(c,d,lba+i);
        if (existing) {
            if (existing->busy) { e=wait_block(c,existing); continue; }
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
            struct cache_block *b=0;
            for (unsigned list=0; list<2 && !b; list++)
                for (struct cache_block *x=c->lru[list].oldest; x; x=x->next)
                    if (!x->busy && !x->waiters && !x->dirty) { b=x; break; }
            if (!b) continue;
            unhash(c,b); b->dev=d; b->lba=lba+j; b->generation=generation; b->sector_size=512;
            b->busy=true; b->invalid=false; b->io_error=0;
            unsigned h=bucket(d,b->lba); b->hash_next=c->hash[h]; c->hash[h]=b;
            touch(c,b); reserved[j]=b; c->misses++;
        }
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
    fs_lock_take(&c->lock);
    if (c->refresh) c->refresh(c->refresh_ctx,d);
    int e=blkdev_range(d,lba,count);
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
int cache_write(struct block_cache *c, struct blkdev *d, uint64_t lba, const void *buf) {
    if (c->refresh) c->refresh(c->refresh_ctx,d);
    if (!buf || !blkdev_durable(d)) return d && d->quarantined ? -FS_EQUARANTINED : -FS_EROFS;
    fs_lock_take(&c->lock); struct cache_device *s=device(c,d); struct cache_block *b;
    int e=!s ? -FS_ENOSPC : s->error;
    uint32_t generation=d->media_generation, epoch=s ? s->epoch : 0;
    if (!e && c->writer_online && s->dirty_bytes>2*c->dirty_budget) {
        uint32_t start=fs_now_ms(); c->writer_waits++;
        uint32_t progress=start, previous=s->dirty_bytes; bool assist=false;
        while (!s->error && s->epoch==epoch && generation==d->media_generation && !d->quarantined &&
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
    if (!e) e=get(c,d,lba,false,&b,0);
    s=device(c,d);
    if (!e && (!s || epoch!=s->epoch || generation!=d->media_generation || d->quarantined)) {
        if (!b->dirty) unhash(c,b);
        e=-FS_EIO;
    }
    if (!e) {
        memcpy(b->data,buf,d->sector_size);
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
/* A bounded selection in LBA order. Pins every selected block before any
 * unlock; bytes are immutable until that reservation is completed. The
 * preallocated bounce buffer lets discontiguous cache pages share commands.
 * Cache lock held on entry/return, never during a transport or timed wait. */
static int write_batch(struct block_cache *c, struct blkdev *d, uint32_t *flushed) {
    struct cache_device *s=device(c,d); if (!s) return -FS_ENOSPC;
    if (s->error) return s->error;
    uint32_t generation=d->media_generation, epoch=s->epoch;
    while (c->batch_busy) fs_event_wait(&c->changed,&c->lock);
    if (c->refresh) c->refresh(c->refresh_ctx,d);
    /* The buffer wait may outlive this media or even reuse its device slot.
     * An old barrier must not borrow a replacement device's dirty count. */
    if (s->dev!=d || s->epoch!=epoch || generation!=d->media_generation) return -FS_EIO;
    if (s->error) return s->error;
    if (!blkdev_durable(d)) return remember(c,d,d->quarantined ? -FS_EQUARANTINED : -FS_EROFS);
    struct cache_block *blocks[CACHE_WRITEBACK_BLOCKS]; unsigned n=0,seen=0;
    unsigned limit=c->writeback_buffer ? c->writeback_pages*4096u/d->sector_size : 1;
    if (limit>CACHE_WRITEBACK_BLOCKS) limit=CACHE_WRITEBACK_BLOCKS;
    /* Dirty blocks are normally newest. Stop once every dirty byte of this
     * device was seen, avoiding an 8 MiB clean-cache scan at each FAT commit. */
    for (unsigned list=0;list<2;list++)
        for (struct cache_block *b=c->lru[list].newest;b;b=b->prev) {
            if (b->dev!=d || !b->dirty || b->invalid) continue;
            seen+=b->sector_size;
            if (!b->busy && !b->waiters) {
                unsigned at=0; while (at<n && blocks[at]->lba<b->lba) at++;
                if (at<limit) {
                    if (n<limit) n++;
                    for (unsigned j=n-1;j>at;j--) blocks[j]=blocks[j-1];
                    blocks[at]=b;
                }
            }
            if (seen>=s->dirty_bytes) goto selected;
        }
selected:
    if (!n) return 0;
    for (unsigned i=0;i<n;i++) {
        if (blocks[i]->generation!=generation) return remember(c,d,-FS_EIO);
        int e=blkdev_range(d,blocks[i]->lba,1); if (e) return remember(c,d,e);
    }
    c->batch_busy=true;
    for (unsigned i=0;i<n;i++) {
        blocks[i]->busy=true;
        if (c->writeback_buffer) memcpy(c->writeback_buffer+i*d->sector_size,blocks[i]->data,d->sector_size);
    }
    int result=0; unsigned completed=0;
    for (unsigned i=0;i<n;) {
        unsigned end=i+1;
        if (c->writeback_buffer) while (end<n && blocks[end]->lba==blocks[end-1]->lba+1) end++;
        if (c->refresh) c->refresh(c->refresh_ctx,d);
        int e=s->error;
        if (!e && (s->epoch!=epoch || generation!=d->media_generation || d->quarantined)) e=-FS_EIO;
        for (unsigned j=i;j<end;j++) if (blocks[j]->invalid) e=-FS_EIO;
        if (!e) {
            const void *data=c->writeback_buffer ? c->writeback_buffer+i*d->sector_size : blocks[i]->data;
            uint64_t lba=blocks[i]->lba;
            fs_lock_drop(&c->lock); e=d->write(d,lba,end-i,data); fs_lock_take(&c->lock);
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
}
static int flush_device(struct block_cache *c, struct blkdev *d) {
    struct cache_device *s=device(c,d); if (!s) return -FS_ENOSPC;
    if (s->error) return s->error;
    if (c->refresh) c->refresh(c->refresh_ctx,d);
    if (!blkdev_durable(d)) return remember(c,d,d->quarantined ? -FS_EQUARANTINED : -FS_EROFS);
    if (d->write_cache_state!=BLKDEV_CACHE_DISABLED) {
        uint32_t generation=d->media_generation,epoch=s->epoch;
        fs_lock_drop(&c->lock); int e=d->flush(d); fs_lock_take(&c->lock);
        if (c->refresh) c->refresh(c->refresh_ctx,d);
        if (s->epoch!=epoch || generation!=d->media_generation) return -FS_EIO;
        if (!e && d->quarantined) e=-FS_EIO;
        if (e) return remember(c,d,e);
    }
    return 0;
}
static int barrier(struct block_cache *c, struct blkdev *d) {
    if (c->refresh) c->refresh(c->refresh_ctx,d);
    struct cache_device *s=device(c,d); if (!s) return -FS_ENOSPC;
    if (s->error) return s->error;
    if (!blkdev_durable(d)) return remember(c,d,d->quarantined ? -FS_EQUARANTINED : -FS_EROFS);
    while (s->dirty_bytes) {
        uint32_t flushed=0; int e=write_batch(c,d,&flushed); if (e) return e;
        if (!flushed) fs_event_wait(&c->changed,&c->lock);
    }
    return flush_device(c,d);
}
int cache_barrier(struct block_cache *c, struct blkdev *d) {
    fs_lock_take(&c->lock); int e=barrier(c,d); fs_lock_drop(&c->lock); return e;
}
bool cache_writeback_due(const struct block_cache *c, uint32_t now) {
    return __atomic_load_n(&c->writeback_pending,__ATOMIC_ACQUIRE) &&
        (int32_t)(now-__atomic_load_n(&c->next_writeback,__ATOMIC_ACQUIRE))>=0;
}
void cache_writer_wait(struct block_cache *c, uint32_t ms) {
    fs_lock_take(&c->lock);
    if (!cache_writeback_due(c,fs_now_ms())) wait_changed(c,ms);
    fs_lock_drop(&c->lock);
}
bool cache_writeback_report(struct block_cache *c, uint32_t now, struct cache_stats *out) {
    fs_lock_take(&c->lock); bool report=false;
    if (!c->dirty_bytes) c->report_time=now;
    else if ((uint32_t)(now-c->report_time)>=5000) {
        c->report_time=now; stats_locked(c,out); report=true;
    }
    fs_lock_drop(&c->lock); return report;
}
int cache_writeback_tick(struct block_cache *c, uint32_t now) {
    fs_lock_take(&c->lock); int result=0; c->tick_flushed=0;
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
    for (struct cache_block *b=first_block(c);b;b=next_block(c,b)) {
        fs_service();
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
    fs_lock_take(&c->lock); struct cache_device *s=device(c,d);
    int e=s ? s->error : -FS_ENOSPC; fs_lock_drop(&c->lock); return e;
}
void cache_invalidate(struct block_cache *c, struct blkdev *d, bool forget) {
    fs_lock_take(&c->lock);
    for (struct cache_block *b=first_block(c);b;b=next_block(c,b)) if (b->dev==d) {
        if (b->busy) {
            if (b->dirty && !b->invalid) { c->dirty_bytes-=b->sector_size; device(c,d)->dirty_bytes-=b->sector_size; }
            b->invalid=true;
        } else { clean(c,b); unhash(c,b); }
    }
    fs_event_wake(&c->changed);
    for (unsigned i=0;i<CACHE_DEVICES;i++) if (c->devices[i].dev==d) {
        if (c->device_epoch==UINT32_MAX) fs_panic("cache epoch exhausted");
        c->devices[i].epoch=++c->device_epoch;
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
    for (unsigned i=0;i<(bytes+4095)/4096;i++) memset(c->workspace[i],0,4096);
    return 0;
}
uint8_t *cache_workspace_byte(struct block_cache *c, uint32_t off) {
    if (off>=c->workspace_pages*4096u) fs_panic("workspace bounds");
    return (uint8_t *)c->workspace[off/4096]+off%4096;
}
void cache_workspace_end(struct block_cache *c) {
    fs_lock_take(&c->workspace_lock); c->workspace_in_use=false; fs_lock_drop(&c->workspace_lock);
}
