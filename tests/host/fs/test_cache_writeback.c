/* Deterministic production-cache writeback reservations and policy.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "cache.h"
#include <stdio.h>
#include <stdatomic.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %u: %s\n",__LINE__,#x); abort(); } } while (0)
void fs_assert_io_unlocked(void);
struct model {
    struct blkdev dev; struct block_cache cache;
    pthread_mutex_t mutex; pthread_cond_t changed;
    bool block,entered,release,fail_write,fail_flush;
    uint64_t first,last; unsigned calls,sectors,flushes,max_count,block_call;
};
static int read_blocks(struct blkdev *d,uint64_t l,uint32_t n,void *b) {
    fs_assert_io_unlocked(); CHECK(!blkdev_range(d,l,n)); memset(b,0x73,n*d->sector_size); return 0;
}
static int write_blocks(struct blkdev *d,uint64_t l,uint32_t n,const void *b) {
    fs_assert_io_unlocked(); struct model *m=d->ctx; CHECK(!blkdev_range(d,l,n) && n<=64);
    pthread_mutex_lock(&m->mutex);
    if (!m->calls) m->first=l;
    m->last=l+n-1; m->calls++; m->sectors+=n; if (n>m->max_count) m->max_count=n;
    if (m->block && (!m->block_call || m->calls==m->block_call)) {
        m->entered=true; pthread_cond_broadcast(&m->changed);
        while (!m->release) pthread_cond_wait(&m->changed,&m->mutex);
    }
    const uint8_t *bytes=b;
    for (unsigned i=0;i<n*d->sector_size;i++) CHECK(bytes[i]==0x51);
    bool fail=m->fail_write; pthread_mutex_unlock(&m->mutex); return fail ? -FS_EIO : 0;
}
static int flush_blocks(struct blkdev *d) {
    fs_assert_io_unlocked(); struct model *m=d->ctx; m->flushes++; return m->fail_flush ? -FS_EIO : 0;
}
static void init(struct model *m,size_t size,unsigned sector) {
    memset(m,0,sizeof(*m)); CHECK(!pthread_mutex_init(&m->mutex,0) && !pthread_cond_init(&m->changed,0));
    m->dev=(struct blkdev){.read=read_blocks,.write=write_blocks,.flush=flush_blocks,.sector_size=sector,
        .capacity=65536,.write_cache_state=BLKDEV_CACHE_ENABLED,.ctx=m}; CHECK(!cache_init(&m->cache,size));
}
static void finish(struct model *m) {
    cache_destroy(&m->cache); CHECK(!pthread_mutex_destroy(&m->mutex) && !pthread_cond_destroy(&m->changed));
}
static void dirty(struct model *m,unsigned count) {
    uint8_t b[2048]; memset(b,0x51,sizeof(b));
    for (unsigned i=count;i;i--) CHECK(!cache_write(&m->cache,&m->dev,i,b));
}
static void policy(void) {
    struct model m; struct cache_stats st;
    init(&m,1024*1024,512); dirty(&m,1);
    fs_lock_take(&m.cache.lock); uint32_t due=m.cache.next_writeback; fs_lock_drop(&m.cache.lock);
    CHECK(!cache_writeback_tick(&m.cache,due-1) && !m.calls);
    CHECK(!cache_writeback_tick(&m.cache,due) && m.calls==1 && m.flushes==1);
    CHECK(!cache_writeback_report(&m.cache,fs_now_ms()+5000,&st));
    dirty(&m,300); cache_stats(&m.cache,&st);
    CHECK(st.dirty_budget==131072 && st.throttle_bytes==262144 && st.dirty_bytes==300*512);
    CHECK(cache_writeback_due(&m.cache,fs_now_ms()));
    m.calls=m.sectors=m.max_count=0;
    CHECK(!cache_writeback_tick(&m.cache,fs_now_ms())); cache_stats(&m.cache,&st);
    CHECK(m.first==1 && m.last==64 && m.max_count==64 && st.tick_flushed==32768 && st.dirty_bytes==236*512);
    CHECK(!cache_writeback_due(&m.cache,fs_now_ms()));
    CHECK(cache_writeback_report(&m.cache,fs_now_ms()+10001,&st));
    CHECK(st.dirty_bytes==236*512 && !cache_writeback_report(&m.cache,fs_now_ms()+10002,&st));
    CHECK(!cache_barrier(&m.cache,&m.dev)); cache_stats(&m.cache,&st); CHECK(!st.dirty_bytes);
    finish(&m);
    /* Mixed-size device batches obey the byte bound too. */
    init(&m,1024*1024,2048); dirty(&m,32);
    CHECK(!cache_writeback_tick(&m.cache,fs_now_ms()+1000));
    CHECK(m.max_count==16 && m.sectors==16);
    CHECK(!cache_barrier(&m.cache,&m.dev)); finish(&m);
    puts("PASS F1-82 budget/age: 1s, 12.5%, 2x throttle, sorted <=64 blocks/32KiB, 5s report cadence");
}
struct call { struct model *m; struct blkdev *barrier; bool write; int result; atomic_bool started,done; };
static void *call(void *arg) {
    struct call *c=arg; uint8_t b[512]; memset(b,0x51,sizeof(b));
    atomic_store(&c->started,true);
    c->result=c->barrier ? cache_barrier(&c->m->cache,c->barrier) :
        c->write ? cache_write(&c->m->cache,&c->m->dev,60000,b) :
        cache_writeback_tick(&c->m->cache,fs_now_ms()+1000);
    atomic_store(&c->done,true); return 0;
}
static void entered(struct model *m) {
    pthread_mutex_lock(&m->mutex);
    while (!m->entered) pthread_cond_wait(&m->changed,&m->mutex);
    pthread_mutex_unlock(&m->mutex);
}
static void release(struct model *m) {
    pthread_mutex_lock(&m->mutex); m->release=true; pthread_cond_broadcast(&m->changed); pthread_mutex_unlock(&m->mutex);
}
static void reservations(bool invalidate,bool fail) {
    struct model m; init(&m,1024*1024,512); uint8_t b[512];
    CHECK(!cache_read(&m.cache,&m.dev,50000,b)); dirty(&m,300); m.block=true; m.fail_write=fail;
    struct call c={.m=&m}; pthread_t w; CHECK(!pthread_create(&w,0,call,&c)); entered(&m);
    /* These return before the transport rendezvous is released. */
    CHECK(!cache_read(&m.cache,&m.dev,50000,b) && b[0]==0x73);
    CHECK(!cache_read(&m.cache,&m.dev,50001,b) && b[0]==0x73);
    if (invalidate) {
        cache_invalidate(&m.cache,&m.dev,true); m.dev.media_generation++;
        memset(b,0x51,sizeof(b)); CHECK(!cache_write(&m.cache,&m.dev,60001,b));
    }
    CHECK(!atomic_load(&c.done)); release(&m); CHECK(!pthread_join(w,0));
    struct cache_stats st; cache_stats(&m.cache,&st);
    if (invalidate) {
        CHECK(c.result==-FS_EIO && !cache_error(&m.cache,&m.dev) && st.dirty_bytes==512);
        m.fail_write=false; CHECK(!cache_barrier(&m.cache,&m.dev));
    } else if (fail) {
        CHECK(c.result==-FS_EIO && cache_error(&m.cache,&m.dev)==-FS_EIO && st.dirty_bytes==300*512);
        CHECK(cache_barrier(&m.cache,&m.dev)==-FS_EIO);
    } else CHECK(!c.result && st.dirty_bytes==236*512);
    finish(&m);
}
static void throttle(bool fail) {
    struct model m; init(&m,1024*1024,512); dirty(&m,514); m.cache.writer_online=true;
    m.fail_write=fail;
    struct call c={.m=&m,.write=true}; pthread_t w; CHECK(!pthread_create(&w,0,call,&c));
    struct cache_stats st; uint32_t start=fs_now_ms();
    do {
        cache_stats(&m.cache,&st); CHECK((uint32_t)(fs_now_ms()-start)<1000);
    } while (!st.writer_waits);
    while (!atomic_load(&c.done)) {
        int e=cache_writeback_tick(&m.cache,fs_now_ms()); CHECK(!e || (fail && e==-FS_EIO));
        CHECK((uint32_t)(fs_now_ms()-start)<2000);
    }
    CHECK(!pthread_join(w,0)); cache_stats(&m.cache,&st);
    CHECK(st.writer_waits==1 && !st.cache_sync_evictions);
    if (fail) CHECK(c.result==-FS_EIO && st.dirty_bytes==514*512);
    else CHECK(!c.result && st.dirty_bytes<st.dirty_budget+512);
    finish(&m);
}
static void completed_group(bool fail) {
    struct model m; init(&m,1024*1024,512); uint8_t b[512]; memset(b,0x51,sizeof(b));
    for (unsigned i=600;i;i-=2) CHECK(!cache_write(&m.cache,&m.dev,i-1,b));
    m.block=true; m.block_call=2;
    struct call c={.m=&m}; pthread_t w; CHECK(!pthread_create(&w,0,call,&c)); entered(&m);
    /* First sparse command (LBA 1) is already complete; LBA 3 is blocked.
     * A hit on 1 must not inherit 3's transport wait. Then redirty 1: the
     * old batch must not touch/release that descriptor a second time. */
    CHECK(!cache_read(&m.cache,&m.dev,1,b) && b[0]==0x51);
    CHECK(!cache_write(&m.cache,&m.dev,1,b));
    m.fail_write=fail; release(&m); CHECK(!pthread_join(w,0));
    struct cache_stats st; cache_stats(&m.cache,&st);
    CHECK(c.result==(fail ? -FS_EIO : 0));
    CHECK(st.dirty_bytes==(fail ? 300u : 237u)*512);
    finish(&m);
}
static void buffer_epoch(void) {
    struct model a,b; init(&a,1024*1024,512); init(&b,1024*1024,512); dirty(&a,300);
    uint8_t bytes[512]; memset(bytes,0x51,sizeof(bytes)); CHECK(!cache_write(&a.cache,&b.dev,3,bytes));
    a.block=true; struct call active={.m=&a},waiting={.m=&a,.barrier=&b.dev}; pthread_t x,y;
    CHECK(!pthread_create(&x,0,call,&active)); entered(&a);
    CHECK(!pthread_create(&y,0,call,&waiting)); uint32_t start=fs_now_ms();
    while (!atomic_load(&waiting.started)) {
        CHECK(fs_now_ms()-start<1000); struct timespec t={0,1000000}; nanosleep(&t,0);
    }
    /* No other runnable work: let the started barrier enter the buffer wait. */
    struct timespec t={0,20000000}; nanosleep(&t,0); CHECK(!atomic_load(&waiting.done));
    cache_invalidate(&a.cache,&b.dev,true); b.dev.media_generation++;
    struct blkdev replacement=b.dev; CHECK(!cache_write(&a.cache,&replacement,20,bytes));
    release(&a); CHECK(!pthread_join(x,0));
    while (!atomic_load(&waiting.done)) {
        CHECK(fs_now_ms()-start<2000); t=(struct timespec){0,1000000}; nanosleep(&t,0);
    }
    CHECK(!pthread_join(y,0) && !active.result && waiting.result==-FS_EIO);
    CHECK(!b.calls && !b.flushes && !cache_error(&a.cache,&replacement));
    struct cache_stats st; cache_stats(&a.cache,&st); CHECK(st.dirty_bytes==237*512);
    finish(&a); finish(&b);
}
static void stalled_writer(unsigned fault) {
    struct model m; init(&m,1024*1024,512); dirty(&m,514); m.cache.writer_online=true;
    m.fail_write=fault==1; m.fail_flush=fault==2;
    struct call c={.m=&m,.write=true}; pthread_t w; uint32_t start=fs_now_ms();
    CHECK(!pthread_create(&w,0,call,&c));
    /* The declared live service cannot run (for example, it is waiting for
     * this caller's volume to drain a boot log). No writeback tick is issued. */
    while (!atomic_load(&c.done)) {
        CHECK(fs_now_ms()-start<2000); struct timespec t={0,1000000}; nanosleep(&t,0);
    }
    CHECK(!pthread_join(w,0) && c.result==(fault ? -FS_EIO : 0));
    struct cache_stats st; cache_stats(&m.cache,&st);
    CHECK(st.writer_waits==1 && st.writer_wait_ms>=CACHE_CLEAN_WAIT_MS && !st.cache_sync_evictions);
    if (fault) CHECK(cache_error(&m.cache,&m.dev)==-FS_EIO);
    else CHECK(st.dirty_bytes<st.dirty_budget && m.flushes);
    finish(&m);
}
static void fallback(void) {
    struct model m; init(&m,8192,512); dirty(&m,8); m.cache.writer_online=true;
    uint8_t b[512]; uint32_t start=fs_now_ms(); CHECK(!cache_read(&m.cache,&m.dev,50,b));
    struct cache_stats st; cache_stats(&m.cache,&st);
    CHECK(fs_now_ms()-start>=CACHE_CLEAN_WAIT_MS && fs_now_ms()-start<200);
    CHECK(st.cache_sync_evictions==1 && b[0]==0x73);
    finish(&m);
    init(&m,1024*1024,512); dirty(&m,300); m.fail_flush=true;
    CHECK(cache_writeback_tick(&m.cache,fs_now_ms())==-FS_EIO && cache_error(&m.cache,&m.dev)==-FS_EIO);
    CHECK(cache_barrier(&m.cache,&m.dev)==-FS_EIO); finish(&m);
    /* Failure of either the initial bounce reservation or later pages must
     * unwind the complete cache without retaining contiguous memory. */
    struct block_cache c;
    fs_host_contig_fail=1; CHECK(cache_init(&c,1024*1024)==-FS_ENOMEM && !fs_host_contig_live);
    fs_host_page_fail=1; CHECK(cache_init(&c,1024*1024)==-FS_ENOMEM && !fs_host_contig_live);
}
int main(void) {
    policy(); reservations(false,false); reservations(false,true); reservations(true,true);
    completed_group(false); completed_group(true); buffer_epoch(); throttle(false); throttle(true);
    stalled_writer(0); stalled_writer(1); stalled_writer(2); fallback(); CHECK(!fs_host_contig_live);
    puts("PASS F1-82 reservations: unrelated hits/misses during blocked writes, generation fencing, sticky write/flush errors, throttle/wakeup/error, bounded reader fallback");
}
