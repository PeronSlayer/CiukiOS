/* Reserved cache publication: actual cache/barrier concurrency, no timing
 * threshold substitutions. SPDX-License-Identifier: GPL-2.0-only */
#include "cache.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sched.h>
static struct block_cache cache;
static uint8_t disk[64][512];
static atomic_bool stalled, entered, resume_io;
static unsigned commands;
static int read_disk(struct blkdev *d,uint64_t lba,uint32_t n,void *out) {
    assert(!blkdev_range(d,lba,n)); commands++; memcpy(out,disk[lba],n*512); return 0;
}
static int write_disk(struct blkdev *d,uint64_t lba,uint32_t n,const void *in) {
    assert(!blkdev_range(d,lba,n)); commands++;
    if (atomic_load(&stalled)) {
        atomic_store(&entered,true);
        while (!atomic_load(&resume_io)) sched_yield();
    }
    memcpy(disk[lba],in,n*512); return 0;
}
static int flush_disk(struct blkdev *d) { (void)d; commands++; return 0; }
static struct blkdev dev={.read=read_disk,.write=write_disk,.flush=flush_disk,
    .capacity=64,.sector_size=512,.write_cache_state=BLKDEV_CACHE_ENABLED};
static void *barrier_thread(void *arg) { (void)arg; assert(!cache_barrier(&cache,&dev)); return 0; }
int main(void) {
    assert(!cache_init(&cache,8192));
    uint8_t before[512],first[512],second[512],readback[512];
    memset(first,0x89,512); memset(second,0x98,512);
    struct cache_reserve_sector set[]={{3,before},{5,0}};
    struct cache_reservation r;
    assert(!cache_reserve(&cache,&dev,set,2,&r));
    assert(cache.reserved_bytes==1024 && !cache.dirty_bytes);
    assert(!memcmp(before,disk[3],512));
    /* Pinned descriptors survive an eviction walk and stay readable. */
    for (unsigned i=10;i<64;i++) assert(!cache_read(&cache,&dev,i,readback));
    assert(r.blocks[0]->lba==3 && r.blocks[1]->lba==5);
    struct cache_image images[]={{0,first},{1,second}};
    unsigned old=commands;
    fs_lock_take(&cache.lock);
    assert(cache_publish(&r,images,2,0)==-FS_EAGAIN);
    fs_lock_drop(&cache.lock);
    assert(commands==old && !cache.dirty_bytes && !memcmp(r.blocks[0]->data,before,512));
    assert(!cache_publish(&r,images,2,0) && commands==old);
    assert(cache.dirty_bytes==1024 && !cache.reserved_bytes);
    assert(r.blocks[0]->order==r.group && r.blocks[1]->order==r.group);
    uint64_t detached=r.published;
    /* A repeated FAT-sector version cannot erase its undurable predecessor. */
    images[0].data=second;
    assert(cache_publish(&r,images,1,detached)==-FS_EAGAIN);
    assert(!memcmp(r.blocks[0]->data,first,512));
    atomic_store(&stalled,true); pthread_t worker;
    assert(!pthread_create(&worker,0,barrier_thread,0));
    while (!atomic_load(&entered)) sched_yield();
    assert(cache_publish(&r,images,1,detached)==-FS_EAGAIN);
    atomic_store(&resume_io,true); assert(!pthread_join(worker,0));
    assert(!memcmp(disk[3],first,512));
    assert(cache.reserved_bytes==1024 && !cache.dirty_bytes);
    old=commands;
    assert(!cache_publish(&r,images,1,detached) && commands==old);
    assert(r.dependency==detached && r.published>detached);
    assert(!cache_barrier(&cache,&dev) && !memcmp(disk[3],second,512));
    cache_release(&r); cache_release(&r);
    assert(!cache.reserved_bytes && !cache.dirty_bytes);
    /* Invalid image sets are atomic failures. Unpublished reservations never
     * write media; invalidation/remount fences both bytes and credit epochs. */
    assert(!cache_reserve(&cache,&dev,set,2,&r));
    struct cache_image duplicate[]={{0,first},{0,second}};
    assert(cache_publish(&r,duplicate,2,0)==-FS_EINVAL);
    old=commands; cache_invalidate(&cache,&dev,true);
    assert(cache_publish(&r,images,1,0)==-FS_EIO);
    cache_release(&r); assert(!cache.reserved_bytes && commands==old);
    assert(!cache_reserve(&cache,&dev,set,2,&r));
    assert(!cache_publish(&r,images,1,0));
    cache_release(&r); /* published debt outlives the pin */
    assert(!cache.reserved_bytes && cache.dirty_bytes==512);
    assert(!cache_barrier(&cache,&dev));
    struct cache_reserve_sector duplicate_set[]={{3,0},{3,0}};
    assert(cache_reserve(&cache,&dev,duplicate_set,2,&r)==-FS_EINVAL);
    cache_destroy(&cache); assert(!blk_request_pool_host_cleanup());
    puts("PASS reserved cache: cold pins, eviction, atomic failure, no-wait publication, active barrier, repeated-sector dependency, credits, abort and generation fencing");
    return 0;
}
