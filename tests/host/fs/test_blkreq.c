/* Production queue ownership, admission, ordering and lifetime under faults.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "blkdev.h"
#include <stdio.h>
#include <assert.h>
#include <stdatomic.h>
static unsigned records;
void blk_queue_record(const char *name,const struct blk_queue_stats *s) {
    assert(name && s->depth[BLK_DEMAND]==1 && s->oldest_ms>=5000); records++;
}
struct disk {
    struct blkdev dev;
    struct blk_queue queue;
    uint8_t data[256*1024];
    uint64_t order[128]; unsigned calls, sizes[128];
    unsigned delay_ms; bool fail, cancel_issued, cleanup_issued;
    uint64_t frontier; enum blk_priority priority;
};
static int transfer(struct blkdev *d,uint64_t lba,uint32_t n,void *buf,bool write) {
    struct disk *m=d->ctx;
    assert(n<=128 && lba*512+n*512<=sizeof(m->data));
    unsigned call=m->calls++; assert(call<128); m->order[call]=lba; m->sizes[call]=n;
    m->frontier=m->queue.active->frontier; m->priority=m->queue.active->priority;
    if (m->cleanup_issued) assert(blk_request_pool_host_cleanup()==-FS_EBUSY);
    if (m->delay_ms) { struct timespec t={0,(long)m->delay_ms*1000000}; nanosleep(&t,0); }
    if (m->cancel_issued) blk_queue_cancel_device(&m->queue,d);
    if (m->fail) return -FS_EIO;
    if (write) memcpy(m->data+lba*512,buf,n*512); else memcpy(buf,m->data+lba*512,n*512);
    return 0;
}
static int read_disk(struct blkdev *d,uint64_t lba,uint32_t n,void *buf) { return transfer(d,lba,n,buf,false); }
static int write_disk(struct blkdev *d,uint64_t lba,uint32_t n,const void *buf) { return transfer(d,lba,n,(void *)buf,true); }
static int flush_disk(struct blkdev *d) {
    struct disk *m=d->ctx; assert(m->calls<128); m->order[m->calls++]=UINT64_MAX; return m->fail ? -FS_EIO : 0;
}
static void init(struct disk *m) {
    memset(m,0,sizeof(*m)); blk_queue_init(&m->queue,"host",65536);
    m->dev=(struct blkdev){.read=blkdev_read,.write=blkdev_write,.flush=blkdev_flush,
        .request_read=read_disk,.request_write=write_disk,.request_flush=flush_disk,
        .queue=&m->queue,.ctx=m,.capacity=512,.sector_size=512,.media_generation=7,
        .write_cache_state=BLKDEV_CACHE_ENABLED};
}
static struct blk_request *prepare(struct disk *m,enum blk_operation op,uint64_t lba,
    uint32_t bytes,void *buf,enum blk_priority priority,uint64_t frontier) {
    struct blk_request *r=blk_request_prepare(&m->dev,op,lba,bytes,buf,priority,frontier,fs_now_ms()+20,30000);
    assert(r && r->state==BLK_PREPARED); return r;
}
static void drop(struct blk_request *r,int result) {
    assert(blk_request_wait(r)==result); assert(r->completed>=r->enqueued);
    assert(!blk_request_cancel(r)); assert(!blk_request_release(r));
    assert(blk_request_release(r)==-FS_EBUSY);
}
static void pool_bounds(void) {
    struct disk m; init(&m); uint8_t b[512]; struct blk_request *all[BLKREQ_CAPACITY];
    for (unsigned i=0;i<BLKREQ_CAPACITY;i++) all[i]=prepare(&m,BLK_READ,0,512,b,BLK_DEMAND,0);
    assert(!blk_request_prepare(&m.dev,BLK_READ,0,512,b,BLK_DEMAND,0,0,30000));
    assert(m.queue.stats.backpressure==1);
    assert(blk_queue_destroy(&m.queue)==-FS_EBUSY);
    for (unsigned i=0;i<BLKREQ_CAPACITY;i++) {
        assert(blk_request_release(all[i])==-FS_EBUSY); assert(blk_request_cancel(all[i])); drop(all[i],-FS_EAGAIN);
    }
    assert(!m.calls && !blk_queue_destroy(&m.queue));
    puts("PASS requests bounded=32 prepared cancellation, backpressure, no early/double release");
}
static void priorities(void) {
    struct disk m; init(&m); uint8_t b[4][512];
    struct blk_request *w=prepare(&m,BLK_WRITE,100,512,b[0],BLK_BACKGROUND,4);
    struct blk_request *s=prepare(&m,BLK_READ,80,512,b[1],BLK_SPECULATIVE,0);
    struct blk_request *r=prepare(&m,BLK_READ,20,512,b[2],BLK_DEMAND,0);
    assert(!blk_request_submit(w) && !blk_request_submit(s) && !blk_request_submit(r));
    assert(blk_request_submit(r)==-FS_EINVAL);
    assert(blk_queue_destroy(&m.queue)==-FS_EBUSY);
    drop(w,0); drop(s,0); drop(r,0);
    assert(m.calls==3 && m.order[0]==20 && m.order[1]==80 && m.order[2]==100);
    w=prepare(&m,BLK_WRITE,100,512,b[0],BLK_BACKGROUND,4);
    r=prepare(&m,BLK_READ,20,512,b[2],BLK_DEMAND,0);
    assert(!blk_request_submit(w) && !blk_request_submit(r)); w->enqueued-=101;
    drop(r,0); drop(w,0); assert(m.order[3]==100);
    /* An overlapping demand read cannot observe stale data by passing an
     * earlier write; a flush cannot pass its finite predecessor frontier. */
    memset(b[0],0x7b,512); memset(b[2],0,512);
    w=prepare(&m,BLK_WRITE,20,512,b[0],BLK_BACKGROUND,5);
    s=prepare(&m,BLK_FLUSH,0,0,0,BLK_BACKGROUND,5);
    r=prepare(&m,BLK_READ,20,512,b[2],BLK_DEMAND,6);
    assert(!blk_request_submit(w) && !blk_request_submit(s) && !blk_request_submit(r));
    drop(r,0); drop(w,0); drop(s,0);
    assert(m.order[5]==20 && m.order[6]==UINT64_MAX && m.order[7]==20 && !memcmp(b[0],b[2],512));
    assert(!blk_queue_destroy(&m.queue));
    puts("PASS queue demand/speculative/background, age<=100ms, overlap and flush frontiers");
}
static void merging(void) {
    for (unsigned mismatch=0;mismatch<6;mismatch++) {
        struct disk m; init(&m); uint8_t b[1024]; memset(b,0xa9,sizeof(b));
        struct mm_pin token={.generation=7,.ticket=1},other={.generation=7,.ticket=2};
        struct blk_request *a=prepare(&m,BLK_WRITE,20,512,b,BLK_BACKGROUND,1);
        struct blk_request *r=prepare(&m,BLK_WRITE,mismatch==1 ? 22 : 21,512,b+512,BLK_BACKGROUND,1);
        a->pin_owner=&token; r->pin_owner=&token;
        if (mismatch==2) r->generation++;
        if (mismatch==3) r->priority=BLK_SPECULATIVE;
        if (mismatch==4) r->frontier++;
        if (mismatch==5) r->pin_owner=&other;
        assert(!blk_request_submit(a) && !blk_request_submit(r));
        drop(a,0); drop(r,mismatch==2 ? -FS_EIO : 0);
        assert(m.queue.stats.merged==(mismatch==0));
        if (!mismatch) assert(m.calls==1 && m.sizes[0]==2);
        assert(!blk_queue_destroy(&m.queue));
    }
    puts("PASS adjacent merge compatibility: generation, priority, frontier, pin identity and contiguous bytes");
}
static void faults(void) {
    struct disk m; init(&m); uint8_t b[131072]; memset(b,0x63,sizeof(b));
    assert(!blkdev_write(&m.dev,0,256,b));
    assert(m.calls==2 && m.sizes[0]==128 && m.sizes[1]==128 && !memcmp(b,m.data,sizeof(b)));
    m.fail=true; unsigned before=m.calls;
    assert(blkdev_write(&m.dev,0,256,b)==-FS_EIO && m.calls==before+1); /* uncertain write never replayed */
    m.fail=false;
    struct blk_request *r=prepare(&m,BLK_READ,0,512,b,BLK_DEMAND,0);
    assert(!blk_request_submit(r)); r->enqueued-=30001; drop(r,-FS_EIO); assert(m.queue.stats.timeouts==1);
    before=m.calls; r=prepare(&m,BLK_READ,0,512,b,BLK_DEMAND,0);
    assert(!blk_request_submit(r)); m.dev.media_generation++; drop(r,-FS_EIO); assert(m.calls==before);
    r=prepare(&m,BLK_WRITE,0,512,b,BLK_BACKGROUND,0); assert(!blk_request_submit(r));
    blk_queue_cancel_device(&m.queue,&m.dev); drop(r,-FS_EAGAIN); assert(m.calls==before);
    /* Cancellation after command issue must retain the hardware's result. */
    m.cancel_issued=true; assert(!blkdev_write(&m.dev,0,128,b)); assert(m.calls==before+1);
    m.cancel_issued=false; m.delay_ms=25;
    assert(!blkdev_read(&m.dev,0,128,b));
    struct blk_queue_stats stats; blk_queue_snapshot(&m.queue,&stats);
    assert(stats.limit_bytes<65536 && stats.limit_bytes>=4096 && stats.limit_bytes%512==0);
    /* T23 regression (image 3d18f2db): a slow phase must not leave single-
     * sector commands for the rest of the session. Very slow commands keep
     * the floor at one page; fast commands grow the limit back to 64 KiB. */
    m.delay_ms=200; assert(!blkdev_read(&m.dev,0,128,b));
    blk_queue_snapshot(&m.queue,&stats); assert(stats.limit_bytes==4096);
    m.delay_ms=0;
    for (unsigned i=0;i<8 && stats.limit_bytes<65536;i++) { m.calls=0; assert(!blkdev_read(&m.dev,0,128,b)); blk_queue_snapshot(&m.queue,&stats); }
    assert(stats.limit_bytes==65536);
    printf("[storage] queue name=host depth=%u/%u/%u age_ms=%u issued=%llu merged=%llu split=%llu cancelled=%llu timeout=%llu backpressure=%llu limit=%u\n",
        stats.depth[0],stats.depth[1],stats.depth[2],stats.oldest_ms,
        (unsigned long long)stats.issued,(unsigned long long)stats.merged,(unsigned long long)stats.split,
        (unsigned long long)stats.cancelled,(unsigned long long)stats.timeouts,(unsigned long long)stats.backpressure,stats.limit_bytes);
    assert(!blk_queue_destroy(&m.queue));
    puts("PASS command cap=64KiB adaptive<=10ms, timeout, generation, queued/issued cancel and uncertain write");
}
static void telemetry(void) {
    struct disk m; init(&m); uint8_t b[512];
    struct blk_request *r=prepare(&m,BLK_READ,0,512,b,BLK_DEMAND,0);
    uint32_t sequence=r->event.sequence;
    assert(!blk_request_submit(r)); r->enqueued-=5001; m.queue.last_record-=5001;
    blk_queue_poll(&m.queue); blk_queue_poll(&m.queue); assert(records==1);
    assert(blk_request_cancel(r)); assert(blk_request_wait(r)==-FS_EAGAIN);
    assert(r->event.sequence==sequence+1);
    assert(!blk_request_cancel(r) && r->event.sequence==sequence+1);
    assert(!blk_request_release(r) && !blk_queue_destroy(&m.queue));
    puts("PASS queue records every 5s while nonempty; exactly one completion event");
}
static int forwarding_read(struct blkdev *d,uint64_t lba,uint32_t n,void *buf) {
    return blkdev_read(d->ctx,lba,n,buf);
}
static void calldown(void) {
    struct disk m; init(&m); uint8_t b[512];
    struct blkdev view={.read=forwarding_read,.capacity=100,.sector_size=512,.ctx=&m.dev,.media_generation=7};
    assert(!blkdev_request(&view,BLK_READ,0,1,b,BLK_SPECULATIVE,42));
    assert(m.frontier==42 && m.priority==BLK_SPECULATIVE);
    view.request_parent=&m.dev; view.request_offset=20;
    assert(!blkdev_request(&view,BLK_READ,1,1,b,BLK_BACKGROUND,43));
    assert(m.order[1]==21 && m.frontier==43 && m.priority==BLK_BACKGROUND);
    view.media_generation++;
    assert(blkdev_read(&view,0,1,b)==-FS_EIO && m.calls==2);
    assert(!blk_queue_destroy(&m.queue));
    puts("PASS mount calldowns preserve priority/frontier; partition offsets and generation fence");
}
static void aged_split(void) {
    struct disk m; init(&m); uint8_t b[131072],read[512]; memset(b,0x39,sizeof(b));
    struct blk_request *w=prepare(&m,BLK_WRITE,0,sizeof(b),b,BLK_BACKGROUND,0);
    struct blk_request *r=prepare(&m,BLK_READ,400,sizeof(read),read,BLK_DEMAND,0);
    assert(!blk_request_submit(w) && !blk_request_submit(r)); w->enqueued-=101;
    assert(blk_queue_run_one(&m.queue) && w->bytes==65536 && m.order[0]==0);
    drop(w,0); drop(r,0);
    assert(m.calls==3 && m.order[1]==400 && m.order[2]==128);
    assert(!blk_queue_destroy(&m.queue));
    puts("PASS an aged bulk request earns one command; splitting still admits the next demand read");
}
static void pool_cleanup(void) {
    assert(!blk_request_pool_host_cleanup());
    for (unsigned cycle=0;cycle<2;cycle++) {
        struct disk m; init(&m); uint8_t b[512]; m.cleanup_issued=true;
        struct blk_request *r=prepare(&m,BLK_READ,0,sizeof(b),b,BLK_DEMAND,0);
        assert(blk_request_pool_host_cleanup()==-FS_EBUSY);
        assert(!blk_request_submit(r));
        assert(blk_request_pool_host_cleanup()==-FS_EBUSY);
        assert(!blk_request_wait(r) && r->state==BLK_COMPLETED);
        assert(blk_request_pool_host_cleanup()==-FS_EBUSY);
        assert(!blk_request_release(r));
        r=prepare(&m,BLK_READ,0,sizeof(b),b,BLK_DEMAND,0);
        assert(blk_request_cancel(r) && r->state==BLK_CANCELLED);
        assert(blk_request_pool_host_cleanup()==-FS_EBUSY);
        drop(r,-FS_EAGAIN);
        assert(!blk_queue_destroy(&m.queue));
        assert(!blk_request_pool_host_cleanup());
        assert(!blk_request_pool_host_cleanup());
    }
    puts("PASS host pool cleanup: rejects prepared/queued/issued/completed/cancelled owners, reinitializes after teardown");
}
int main(void) { pool_bounds(); priorities(); merging(); faults(); telemetry(); calldown(); aged_split(); pool_cleanup(); return 0; }
