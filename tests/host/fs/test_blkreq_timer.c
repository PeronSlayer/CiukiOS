/* Production deferred telemetry callback with a scheduler/timer boundary.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "blkdev.h"
#include <assert.h>
#include <stdio.h>
static uint64_t armed_at;
static uint32_t armed_generation;
static unsigned arms, records;
static bool idle_report;
uint64_t deadline_after_ms(uint32_t ms) { return ms; }
void ktimer_arm(struct ktimer *t,uint64_t deadline,unsigned kind,void *owner,
                const uint32_t *generation,ktimer_fn fn) {
    assert(t && owner && generation && fn && !kind);
    armed_at=deadline; armed_generation=*generation; arms++;
}
#define BLKREQ_HOST_TIMERS
#include "../../../src/kernel/fs/blkreq.c"
void blk_queue_record(const char *name,const struct blk_queue_stats *s) {
    assert(name);
    if (idle_report) assert(!s->depth[BLK_DEMAND] && s->device[0].dma==1 && s->device[0].service[0]==1);
    else assert(s->depth[BLK_DEMAND]==1 && s->oldest_ms>=5000);
    records++;
}
static int complete_read(struct blkdev *d,uint64_t lba,uint32_t n,void *b) {
    (void)lba; (void)n; (void)b; d->stats.dma++; return 0;
}
int main(void) {
    struct blk_queue q; blk_queue_init(&q,"telemetry",65536); q.report_generation=7;
    struct blkdev d={.capacity=512,.sector_size=512,.queue=&q}; uint8_t b[512];
    struct blk_request *r=blk_request_prepare(&d,BLK_READ,0,512,b,BLK_DEMAND,0,0,30000);
    assert(r && !blk_request_submit(r)); r->enqueued-=5001; q.last_record-=5001;
    queue_timer(&q,7,0); assert(records==1 && arms==1 && armed_at==5000 && armed_generation==7);
    queue_timer(&q,8,0); assert(records==1 && arms==1); /* stale callback */
    fs_lock_take(&q.lock); queue_timer(&q,7,0); fs_lock_drop(&q.lock);
    assert(arms==2 && armed_at==1 && records==1); /* never blocks in work context */
    /* A submit-only issued request can be stuck in hardware with no waiter. */
    q.head=0; q.active=r; q.busy=true; r->state=BLK_ISSUED; q.last_record-=5001;
    queue_timer(&q,7,0); assert(records==2 && arms==3 && armed_at==5000);
    q.stopped=true; queue_timer(&q,7,0); assert(arms==3 && records==2);
    q.stopped=false; q.active=0; q.busy=false; q.head=r; r->state=BLK_QUEUED;
    assert(blk_request_cancel(r) && blk_request_wait(r)==-FS_EAGAIN && !blk_request_release(r));
    d.request_read=complete_read;
    r=blk_request_prepare(&d,BLK_READ,0,512,b,BLK_DEMAND,0,0,30000);
    assert(r && !blk_request_submit(r) && blk_queue_run_one(&q));
    assert(!blk_request_wait(r) && !blk_request_release(r) && q.record_dirty);
    idle_report=true; q.last_record-=5001;
    queue_timer(&q,7,0); assert(records==3 && !q.record_dirty);
    q.last_record-=5001; queue_timer(&q,7,0); assert(records==3);
    assert(!blk_queue_destroy(&q));
    assert(!blk_request_pool_host_cleanup());
    puts("PASS deferred queue telemetry: submit-only stalled I/O, 5s cadence, stale generation, lock retry, stop");
    return 0;
}
