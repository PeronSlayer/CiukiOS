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
 * more pages keeps at least F1's 7 FAT slots per non-workspace page (default
 * 8 MiB: 14080 slots against 13440). The 64-bit host packs fewer headers. */
_Static_assert(PAGES_PER_META >= 11, "i686 descriptor density");
#endif

static unsigned cls(const struct cache_block *b) { return b->capacity >= 2048 ? 1u : 0u; }
static unsigned bucket(struct blkdev *d, uint64_t lba) { return (unsigned)((uintptr_t)d/16 ^ lba ^ (lba>>32)) & 255; }
static struct cache_device *device(struct block_cache *c, struct blkdev *d) {
    for (unsigned i=0;i<CACHE_DEVICES;i++) if (c->devices[i].dev==d) return &c->devices[i];
    for (unsigned i=0;i<CACHE_DEVICES;i++) if (!c->devices[i].dev) { c->devices[i].dev=d; return &c->devices[i]; }
    return 0;
}
static int remember(struct block_cache *c, struct blkdev *d, int e) {
    struct cache_device *s=device(c,d);
    if (s && e && !s->error) s->error=e;
    return e;
}
static int write_block(struct block_cache *c, struct cache_block *b) {
    if (!b->dirty) return 0;
    if (c->refresh) c->refresh(c->refresh_ctx,b->dev);
    struct cache_device *s=device(c,b->dev);
    if (!s) return -FS_ENOSPC;
    if (s->error) return s->error;
    int e=blkdev_range(b->dev,b->lba,1);
    if (!e) e=b->dev->write(b->dev,b->lba,1,b->data);
    if (e) return remember(c,b->dev,e);
    b->dirty=false; c->writes++; return 0;
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
    for (unsigned k=0;k<SLOTS_512;k++) {
        struct cache_block *b=&pg->b[h*SLOTS_512+k];
        int e=b->dev && b->dirty ? write_block(c,b) : 0;
        if (e) return e;     /* sticky failed block pins this half only */
    }
    half_deactivate(c,pg,h); half_activate(c,pg,h,1,true);
    return 0;
}
static void convert_page(struct block_cache *c, struct cache_page *pg, unsigned halves, unsigned *done, int *blocked) {
    for (unsigned h=0;h<2 && *done<halves;h++) if (!pg->layout[h]) {
        fs_service();
        int e=convert_half(c,pg,h);
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
static int get(struct block_cache *c, struct blkdev *d, uint64_t lba, bool read, struct cache_block **out) {
    fs_service();
    if (c->refresh) c->refresh(c->refresh_ctx,d);
    int e=blkdev_range(d,lba,1); if (e) return e;
    if (!d->read) return -FS_EINVAL;
    struct cache_device *s=device(c,d); if (!s) return -FS_ENOSPC;
    if ((e=layout_for(c,d))) return e;
    for (struct cache_block *b=c->hash[bucket(d,lba)];b;b=b->hash_next) {
        if (b->dev==d && b->lba==lba && b->generation==d->media_generation && b->sector_size==d->sector_size) { c->hits++; touch(c,b); *out=b; return 0; }
    }
    c->misses++;
    struct cache_block *b=0;
    int blocked=-FS_ENOMEM;
    /* A 512-byte request may use a 2048-byte slot when every 512-byte slot is
     * pinned (f4-02 rule); a 2048-byte request needs a 2048-byte slot. */
    for (unsigned list=d->sector_size==2048 ? 1u : 0u; list<2 && !b; list++) {
        for (struct cache_block *x=c->lru[list].oldest;x;x=x->next) {
            if (x->capacity<d->sector_size) continue;
            struct cache_device *owner=x->dirty ? device(c,x->dev) : 0;
            if (!x->dirty || !owner || !owner->error) { b=x; break; }
            blocked=owner->error;
        }
    }
    if (!b) return blocked;
    if ((e=write_block(c,b))) return e;
    unhash(c,b);
    if (read && (e=d->read(d,lba,1,b->data))) return e;
    if (read && proc_launch_disk_read) proc_launch_disk_read(d->sector_size);
    b->dev=d; b->lba=lba; b->generation=d->media_generation; b->sector_size=(uint16_t)d->sector_size;
    unsigned h=bucket(d,lba); b->hash_next=c->hash[h]; c->hash[h]=b;
    touch(c,b); *out=b; return 0;
}
static unsigned meta_needed(unsigned data) { return data>1 ? (data-1+PAGES_PER_META-1)/PAGES_PER_META : 0; }
int cache_init(struct block_cache *c, size_t bytes) {
    memset(c,0,sizeof(*c)); fs_lock_init(&c->lock); fs_lock_init(&c->workspace_lock);
    if (!bytes) bytes=CACHE_DEFAULT_BYTES;
    if (bytes<8192 || bytes>32u*1024u*1024u) { cache_destroy(c); return -FS_EINVAL; }
    size_t pages=bytes/4096;
    c->workspace_pages=(uint32_t)(pages/16); if (!c->workspace_pages) c->workspace_pages=1;
    if (c->workspace_pages>256) c->workspace_pages=256;
    for (unsigned i=0;i<c->workspace_pages;i++) if (!(c->workspace[i]=fs_page_alloc())) goto fail;
    /* Budget: workspace + data pages + descriptor pages == bytes/4096. */
    unsigned room=(unsigned)(pages-c->workspace_pages), data=room;
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
    if (c->inline_page.data) fs_page_free(c->inline_page.data);
    while (c->meta) {
        struct cache_meta *m=c->meta; c->meta=m->next;
        for (unsigned i=0;i<m->count;i++) fs_page_free(m->pages[i].data);
        fs_page_free(m);
    }
    for (unsigned i=0;i<c->workspace_pages;i++) fs_page_free(c->workspace[i]);
    fs_lock_destroy(&c->lock); fs_lock_destroy(&c->workspace_lock);
    memset(c,0,sizeof(*c));
}
int cache_attach(struct block_cache *c, struct blkdev *d) {
    if (!c || !d || (d->sector_size!=512 && d->sector_size!=2048)) return -FS_EINVAL;
    fs_lock_take(&c->lock);
    int e=device(c,d) ? layout_for(c,d) : -FS_ENOSPC;
    fs_lock_drop(&c->lock); return e;
}
void cache_stats(struct block_cache *c, struct cache_stats *out) {
    fs_lock_take(&c->lock);
    *out=(struct cache_stats){ .pages=c->workspace_pages+c->data_pages+c->meta_pages,
        .data_pages=c->data_pages, .meta_pages=c->meta_pages, .workspace_pages=c->workspace_pages,
        .slots_512=c->slots[0], .slots_2048=c->slots[1], .hits=c->hits, .misses=c->misses, .writes=c->writes };
    fs_lock_drop(&c->lock);
}
int cache_read(struct block_cache *c, struct blkdev *d, uint64_t lba, void *buf) {
    if (!buf) return -FS_EINVAL;
    fs_lock_take(&c->lock); struct cache_block *b;
    int e=get(c,d,lba,true,&b); if (!e) memcpy(buf,b->data,d->sector_size);
    fs_lock_drop(&c->lock); return e;
}
int cache_read_run(struct block_cache *c, struct blkdev *d, uint64_t lba, uint32_t count, void *buf) {
    if (!c || !d || !buf || d->sector_size!=512 || !count || count>BLKDEV_READ_SECTORS) return -FS_EINVAL;
    fs_lock_take(&c->lock);
    if (c->refresh) c->refresh(c->refresh_ctx,d);
    int e=blkdev_range(d,lba,count);
    if (!e && !d->read) e=-FS_EINVAL;
    if (!e && !device(c,d)) e=-FS_ENOSPC;
    /* Preserve all hits before miss insertion can evict any later hit in a
     * small cache. The mutex serializes cache writes and media invalidation.
     * A bitmap, rather than pinned slots, needs no extra pages or allocation. */
    uint32_t hits[BLKDEV_READ_SECTORS/32]={0};
    uint32_t generation=d->media_generation;
    for (unsigned i=0; !e && i<count; i++) {
        fs_service();
        for (struct cache_block *b=c->hash[bucket(d,lba+i)]; b; b=b->hash_next)
            if (b->dev==d && b->lba==lba+i && b->generation==generation && b->sector_size==512) {
                memcpy((uint8_t *)buf+i*512,b->data,512);
                hits[i/32]|=1u<<(i%32); c->hits++; touch(c,b); break;
            }
    }
    for (unsigned i=0; !e && i<count;) {
        if (hits[i/32] & (1u<<(i%32))) { i++; continue; }
        unsigned end=i+1;
        while (end<count && !(hits[end/32] & (1u<<(end%32)))) end++;
        fs_service();
        if (file_read_cancelled && file_read_cancelled()) { e=-4; break; }
        e=d->read(d,lba+i,end-i,(uint8_t *)buf+i*512);
        if (e) break;                 /* never publish a partial/failed read */
        if (proc_launch_disk_read) proc_launch_disk_read((end-i)*512);
        if (c->refresh) c->refresh(c->refresh_ctx,d);
        if (d->media_generation!=generation || d->quarantined) { e=-FS_EIO; break; }
        for (; i<end; i++) {
            struct cache_block *b;
            e=get(c,d,lba+i,false,&b);
            if (e) break;
            memcpy(b->data,(uint8_t *)buf+i*512,512);
        }
    }
    fs_lock_drop(&c->lock); return e;
}
int cache_write(struct block_cache *c, struct blkdev *d, uint64_t lba, const void *buf) {
    if (c->refresh) c->refresh(c->refresh_ctx,d);
    if (!buf || !blkdev_durable(d)) return d && d->quarantined ? -FS_EQUARANTINED : -FS_EROFS;
    fs_lock_take(&c->lock); struct cache_device *s=device(c,d); struct cache_block *b;
    int e=!s ? -FS_ENOSPC : s->error;
    if (!e) e=get(c,d,lba,false,&b);
    if (!e) {
        memcpy(b->data,buf,d->sector_size);
        if (!b->dirty) {
            b->since=fs_now_ms();
            uint32_t due=b->since+5000;
            if (!c->writeback_pending || (int32_t)(due-c->next_writeback)<0) c->next_writeback=due;
            c->writeback_pending=true;
        }
        b->dirty=true;
    }
    fs_lock_drop(&c->lock); return e;
}
static int barrier(struct block_cache *c, struct blkdev *d) {
    if (c->refresh) c->refresh(c->refresh_ctx,d);
    struct cache_device *s=device(c,d); if (!s) return -FS_ENOSPC;
    if (s->error) return s->error;
    if (!blkdev_durable(d)) return remember(c,d,d->quarantined ? -FS_EQUARANTINED : -FS_EROFS);
    for (struct cache_block *b=first_block(c);b;b=next_block(c,b)) if (b->dev==d) {
        fs_service();
        int e=write_block(c,b); if (e) return e;
    }
    if (d->write_cache_state!=BLKDEV_CACHE_DISABLED) {
        int e=d->flush(d); if (e) return remember(c,d,e);
    }
    return 0;
}
int cache_barrier(struct block_cache *c, struct blkdev *d) {
    fs_lock_take(&c->lock); int e=barrier(c,d); fs_lock_drop(&c->lock); return e;
}
bool cache_writeback_due(const struct block_cache *c, uint32_t now) {
    return c->writeback_pending && (int32_t)(now-c->next_writeback)>=0;
}
int cache_writeback_tick(struct block_cache *c, uint32_t now) {
    if (!cache_writeback_due(c,now)) return 0;
    fs_lock_take(&c->lock); int result=0;
    for (unsigned i=0;i<CACHE_DEVICES;i++) {
        struct blkdev *d=c->devices[i].dev; if (!d) continue;
        bool due=false;
        for (struct cache_block *b=first_block(c);b;b=next_block(c,b))
            { fs_service(); if (b->dev==d && b->dirty && (uint32_t)(now-b->since)>=5000) { due=true; break; } }
        if (due) { int e=barrier(c,d); if (e && !result) result=e; }
    }
    c->writeback_pending=false;
    for (struct cache_block *b=first_block(c);b;b=next_block(c,b)) {
        fs_service();
        struct cache_device *owner=b->dirty ? device(c,b->dev) : 0;
        if (owner && !owner->error) {
            uint32_t due=b->since+5000;
            if (!c->writeback_pending || (int32_t)(due-c->next_writeback)<0) c->next_writeback=due;
            c->writeback_pending=true;
        }
    }
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
    for (struct cache_block *b=first_block(c);b;b=next_block(c,b)) if (b->dev==d) { unhash(c,b); b->dirty=false; }
    if (forget) for (unsigned i=0;i<CACHE_DEVICES;i++) if (c->devices[i].dev==d) memset(&c->devices[i],0,sizeof(c->devices[i]));
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
