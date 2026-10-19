/* f3-49b/f1-84 integration: cancellation cannot steal a publication barrier.
 * Production cache/request code; synthetic time and transport only. */
#include "cache.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned checks, writes, flushes;
static uint64_t now;
static bool stopped;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"barrier request line %u: %s\n",__LINE__,#x); exit(1); } } while (0)
static uint64_t clock_now(void *arg) { (void)arg; return now; }
static bool request_stopped(void *arg) { (void)arg; return stopped; }
static void pause_request(void *arg) { (void)arg; now+=1000; }
static int read_blocks(struct blkdev *d,uint64_t lba,uint32_t count,void *buffer) {
    (void)d; (void)lba; memset(buffer,0,count*512u); return 0;
}
static int write_blocks(struct blkdev *d,uint64_t lba,uint32_t count,const void *buffer) {
    (void)d; (void)lba; (void)count; (void)buffer; writes++; return 0;
}
static int flush_blocks(struct blkdev *d) { (void)d; flushes++; return 0; }

enum wait_case { WRITE_ADMISSION, BARRIER_ADMISSION, GROUP_ADMISSION,
                 BARRIER_LOCK, GROUP_LOCK, COMPLETION_LOCK, OWN_BATCH };
static void cancelled_wait(enum wait_case which,bool cancel) {
    struct block_cache cache;
    struct blkdev dev={.read=read_blocks,.write=write_blocks,.flush=flush_blocks,
        .capacity=8192,.sector_size=512,.media_generation=7,.write_cache_state=BLKDEV_CACHE_ENABLED};
    CHECK(!cache_init(&cache,65536));
    uint64_t group=0; CHECK(!cache_group_begin(&cache,&dev,&group) && group==1);
    uint8_t bytes[512]={0x49}; CHECK(!cache_write(&cache,&dev,100,bytes));
    struct cache_device *state=&cache.devices[0];
    bool locked=which>=BARRIER_LOCK && which<=COMPLETION_LOCK;
    state->barrier_active=!locked && which!=OWN_BATCH;
    cache.batch_busy=which==OWN_BATCH;
    if (locked) fs_lock_take(&cache.lock);
    struct vfs_wait_info wait={.drives=4};
    struct vfs_request request={.deadline=now+2000000,.now=clock_now,.cancelled=request_stopped,
        .pause=pause_request,.owner=49,.wait=&wait};
    /* Expiration exercises the wait; cancellation exercises pre-admission. */
    stopped=cancel; uint64_t begin=now; int expected=cancel ? -VFS_ECANCELLED : -VFS_ETIMEOUT;
    struct fs_request_scope scope; fs_request_enter(&scope,&request);
    struct cache_completion completion={0}; uint64_t successor=99;
    unsigned old_writes=writes,old_flushes=flushes; int error;
    if (which==WRITE_ADMISSION) error=cache_write(&cache,&dev,101,bytes);
    else if (which==GROUP_ADMISSION || which==GROUP_LOCK) error=cache_group_begin(&cache,&dev,&successor);
    else if (which==COMPLETION_LOCK) error=cache_completion(&cache,&dev,&completion);
    else error=cache_barrier_record(&cache,&dev,&completion);
    CHECK(error==expected && now==(cancel ? begin : request.deadline));
    CHECK(writes==old_writes && flushes==old_flushes && !state->error);
    CHECK(state->accepted==1 && !state->durable && state->frontier==1 && cache.dirty_bytes==512);
    CHECK(successor==99 && cache.batch_busy==(which==OWN_BATCH));
    CHECK(state->barrier_active==(!locked && which!=OWN_BATCH));
    if (!cancel) CHECK(wait.valid && wait.reason==VFS_WAIT_CACHE && wait.outcome==-VFS_ETIMEOUT &&
        wait.elapsed_us && wait.elapsed_us<=2000000);
    if (which==BARRIER_ADMISSION || which==BARRIER_LOCK || which==OWN_BATCH)
        CHECK(completion.error==expected && completion.incomplete);
    fs_request_leave(&scope); CHECK(!fs_request_current());
    if (locked) fs_lock_drop(&cache.lock);
    state->barrier_active=false; cache.batch_busy=false;
    CHECK(!cache_barrier_record(&cache,&dev,&completion));
    CHECK(!cache.dirty_bytes && !state->barrier_active && state->frontier==1);
    CHECK(completion.accepted==1 && completion.durable==1 && completion.generation==7);
    CHECK(writes==old_writes+1 && flushes==old_flushes+1);
    CHECK(!cache_group_begin(&cache,&dev,&successor) && successor==2);
    cache_destroy(&cache);
}
int main(void) {
    for (unsigned which=WRITE_ADMISSION;which<=OWN_BATCH;which++) {
        cancelled_wait((enum wait_case)which,false);
        cancelled_wait((enum wait_case)which,true);
    }
    CHECK(!blk_request_pool_host_cleanup());
    printf("PASS cache publication/request integration checks=%u waits=14 dirty/version/frontier/owner/successor preserved\n",checks);
    return 0;
}
