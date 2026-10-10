/* SPDX-License-Identifier: GPL-2.0-only */
#include "cache.h"
struct cache_block {
    struct cache_block *hash_next, *prev, *next;
    struct blkdev *dev;
    uint64_t lba;
    uint32_t since;
    bool dirty;
    uint8_t data[512];
};
#define BLOCKS_PER_PAGE ((4096-sizeof(void *))/sizeof(struct cache_block))
struct cache_page { struct cache_page *next; struct cache_block b[BLOCKS_PER_PAGE]; };
_Static_assert(sizeof(struct cache_page)<=4096, "cache page");
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
static void newest(struct block_cache *c, struct cache_block *b) {
    if (c->newest==b) return;
    if (b->prev) b->prev->next=b->next; else c->oldest=b->next;
    if (b->next) b->next->prev=b->prev;
    b->prev=c->newest; b->next=0;
    if (c->newest) c->newest->next=b;
    c->newest=b; if (!c->oldest) c->oldest=b;
}
static int get(struct block_cache *c, struct blkdev *d, uint64_t lba, bool read, struct cache_block **out) {
    fs_service();
    if (c->refresh) c->refresh(c->refresh_ctx,d);
    int e=blkdev_range(d,lba,1); if (e) return e;
    if (!d->read) return -FS_EINVAL;
    struct cache_device *s=device(c,d); if (!s) return -FS_ENOSPC;
    for (struct cache_block *b=c->hash[bucket(d,lba)];b;b=b->hash_next) {
        if (b->dev==d && b->lba==lba) { c->hits++; newest(c,b); *out=b; return 0; }
    }
    c->misses++;
    struct cache_block *b=c->oldest;
    int blocked=-FS_ENOMEM;
    while (b && b->dirty) {
        struct cache_device *owner=device(c,b->dev);
        if (!owner || !owner->error) break;
        blocked=owner->error; b=b->next;
    }
    if (!b) return blocked;
    if ((e=write_block(c,b))) return e;
    unhash(c,b);
    if (read && (e=d->read(d,lba,1,b->data))) return e;
    b->dev=d; b->lba=lba;
    unsigned h=bucket(d,lba); b->hash_next=c->hash[h]; c->hash[h]=b;
    newest(c,b); *out=b; return 0;
}
int cache_init(struct block_cache *c, size_t bytes) {
    memset(c,0,sizeof(*c)); fs_lock_init(&c->lock); fs_lock_init(&c->workspace_lock);
    if (!bytes) bytes=CACHE_DEFAULT_BYTES;
    if (bytes<8192 || bytes>32u*1024u*1024u) return -FS_EINVAL;
    size_t pages=bytes/4096;
    c->workspace_pages=(uint32_t)(pages/16); if (!c->workspace_pages) c->workspace_pages=1;
    if (c->workspace_pages>256) c->workspace_pages=256;
    for (unsigned i=0;i<c->workspace_pages;i++) if (!(c->workspace[i]=fs_page_alloc())) goto fail;
    for (size_t i=c->workspace_pages;i<pages;i++) {
        struct cache_page *p=fs_page_alloc(); if (!p) goto fail;
        p->next=c->pages; c->pages=p;
        for (unsigned j=0;j<BLOCKS_PER_PAGE;j++) {
            struct cache_block *b=&p->b[j]; b->prev=c->newest;
            if (c->newest) c->newest->next=b; else c->oldest=b;
            c->newest=b; c->blocks++;
        }
    }
    return 0;
fail: cache_destroy(c); return -FS_ENOMEM;
}
void cache_destroy(struct block_cache *c) {
    while (c->pages) { struct cache_page *p=c->pages; c->pages=p->next; fs_page_free(p); }
    for (unsigned i=0;i<c->workspace_pages;i++) fs_page_free(c->workspace[i]);
    fs_lock_destroy(&c->lock); fs_lock_destroy(&c->workspace_lock);
    memset(c,0,sizeof(*c));
}
int cache_read(struct block_cache *c, struct blkdev *d, uint64_t lba, void *buf) {
    if (!buf) return -FS_EINVAL;
    fs_lock_take(&c->lock); struct cache_block *b;
    int e=get(c,d,lba,true,&b); if (!e) memcpy(buf,b->data,512);
    fs_lock_drop(&c->lock); return e;
}
int cache_write(struct block_cache *c, struct blkdev *d, uint64_t lba, const void *buf) {
    if (c->refresh) c->refresh(c->refresh_ctx,d);
    if (!buf || !blkdev_durable(d)) return d && d->quarantined ? -FS_EQUARANTINED : -FS_EROFS;
    fs_lock_take(&c->lock); struct cache_device *s=device(c,d); struct cache_block *b;
    int e=!s ? -FS_ENOSPC : s->error;
    if (!e) e=get(c,d,lba,false,&b);
    if (!e) {
        memcpy(b->data,buf,512);
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
    for (struct cache_block *b=c->oldest;b;b=b->next) if (b->dev==d) {
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
        for (struct cache_block *b=c->oldest;b;b=b->next)
            { fs_service(); if (b->dev==d && b->dirty && (uint32_t)(now-b->since)>=5000) { due=true; break; } }
        if (due) { int e=barrier(c,d); if (e && !result) result=e; }
    }
    c->writeback_pending=false;
    for (struct cache_block *b=c->oldest;b;b=b->next) {
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
    for (struct cache_block *b=c->oldest;b;b=b->next) if (b->dev==d) { unhash(c,b); b->dirty=false; }
    if (forget) for (unsigned i=0;i<CACHE_DEVICES;i++) if (c->devices[i].dev==d) memset(&c->devices[i],0,sizeof(c->devices[i]));
    fs_lock_drop(&c->lock);
}
int cache_workspace_begin(struct block_cache *c, uint32_t bytes) {
    if (bytes>c->workspace_pages*4096u) return -FS_ENOMEM;
    fs_lock_take(&c->workspace_lock);
    for (unsigned i=0;i<(bytes+4095)/4096;i++) memset(c->workspace[i],0,4096);
    return 0;
}
uint8_t *cache_workspace_byte(struct block_cache *c, uint32_t off) {
    if (off>=c->workspace_pages*4096u) fs_panic("workspace bounds");
    return (uint8_t *)c->workspace[off/4096]+off%4096;
}
void cache_workspace_end(struct block_cache *c) { fs_lock_drop(&c->workspace_lock); }
