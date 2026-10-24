/* T23 56e58374: cache publication versus a served, sleeping FIFO waiter.
 * The production locks, kwait and scheduler come from the latency replay;
 * pthread mutexes do not model a ticket reserved by a sleeping task.
 * SPDX-License-Identifier: GPL-2.0-only */
#define _XOPEN_SOURCE 700
#define HOST_SYNC_TIMERS
#define main latency_fixture_main
#include "../kernel_latency_test.c"
#undef main
#define P2V(p) ((void *)(uintptr_t)(p))
#define V2P(p) ((uint32_t)(uintptr_t)(p))
#define ktime_cycles sync_unused_cycles
#include "../../../src/kernel/core/sync.c"
#undef ktime_cycles
static void timer_fixture_reset(void)
{
    timer_root=timer_first=timer_due_head=timer_due_tail=0;
    memset(&timer_stats,0,sizeof(timer_stats)); deferred_fn=0;
}
#include "../../../src/kernel/fs/vfs.c"
#include "../../../src/kernel/fs/blkreq.c"
bool g_cpu_tsc;
uint32_t ticks_lo(void) { return (uint32_t)g_ticks; }
void klog(const char *fmt,...) { (void)fmt; }
void klog_diag(const char *fmt,...) { (void)fmt; }
static struct vfs space;
static struct task *waiter;
static bool pumping;
#define get cache_get
#ifndef T23_CACHE_SOURCE
#define T23_CACHE_SOURCE "../../../src/kernel/fs/cache.c"
#endif
#include T23_CACHE_SOURCE
#undef get
#include <ucontext.h>

static struct block_cache publication_cache;
static struct cache_reservation publication;
static struct cache_block block;
static uint8_t bytes[512], image_bytes[512];
static unsigned earlier_completed;
static int unused_flush(struct blkdev *d) { (void)d; CHECK(false); return 0; }
static int unused_write(struct blkdev *d,uint64_t lba,uint32_t count,const void *data)
{ (void)d;(void)lba;(void)count;(void)data;CHECK(false);return 0; }
static int unused_read(struct blkdev *d,uint64_t lba,uint32_t count,void *data)
{ (void)d;(void)lba;(void)count;(void)data;CHECK(false);return 0; }
static ucontext_t marker_context,writer_context;
static struct task *on_cpu;
static struct cache_reservation writer_publication;
static struct cache_block writer_block;
static uint8_t writer_bytes[512],writer_stack[256*1024];
static struct cache_image cycle_image;
static unsigned cycle_attempts[2],cycle_done,cycle_switches;
static void cycle_switch(void)
{
    struct task *previous=on_cpu,*next=g_current;
    if (previous==next) return;
    cycle_switches++;
    on_cpu=next;
    CHECK(previous==&ui || previous==&normal);
    CHECK(next==&ui || next==&normal);
    CHECK(!swapcontext(previous==&ui ? &marker_context : &writer_context,
                       next==&ui ? &marker_context : &writer_context));
}
static void cycle_publish(unsigned actor,struct cache_reservation *r)
{
    for (;;) {
        if (++cycle_attempts[actor]>1000) {
            fprintf(stderr,"T23 FIFO livelock: marker_attempts=%u writer_attempts=%u accepted=%llu switches=%u\n",
                cycle_attempts[0],cycle_attempts[1],(unsigned long long)publication_cache.devices[0].accepted,
                cycle_switches);
            CHECK(false);
        }
        vfs_lock(&space,vfs_drive_mask(2));
        int e=cache_publish(r,&cycle_image,1,0);
        vfs_unlock(&space);
        if (!e) { cycle_done++; return; }
        CHECK(e==-FS_EAGAIN);
        CHECK(!cache_reservation_wait(r));
    }
}
static void cycle_writer(void)
{
    flags=0x200;
    task_start(&ui); /* marker resumes once writer queues behind its cache lock */
    CHECK(!cache_reservation_wait(&writer_publication));
    cycle_publish(1,&writer_publication);
    normal.state=T_BLOCKED; normal.wake_tick=0; schedule();
    CHECK(false);
}
static void cycle_test(void)
{
    reset();vfs_init(&space);memset(&publication_cache,0,sizeof(publication_cache));
    fs_lock_init(&publication_cache.lock);fs_event_init(&publication_cache.changed);
    fs_event_init(&publication_cache.writer_kick);
    struct blkdev dev={.sector_size=512,.capacity=128,.media_generation=7,
        .write_cache_state=BLKDEV_CACHE_ENABLED,.flush=unused_flush,.write=unused_write,.read=unused_read};
    CHECK(blkdev_durable(&dev));
    publication_cache.devices[0]=(struct cache_device){.dev=&dev,.epoch=1,.generation=7,.dirty_bytes=1024};
    publication_cache.dirty_bytes=1024;publication_cache.dirty_budget=65536;
    block=(struct cache_block){.dev=&dev,.data=bytes,.lba=10,.capacity=512,.sector_size=512,.generation=7,.pins=1,.dirty=true};
    writer_block=block;writer_block.data=writer_bytes;writer_block.lba=11;
    publication=(struct cache_reservation){.cache=&publication_cache,.dev=&dev,.epoch=1,.generation=7,.count=1,.blocks={&block}};
    writer_publication=publication;writer_publication.blocks[0]=&writer_block;
    cycle_image=(struct cache_image){0,image_bytes};
    hog.state=T_BLOCKED;normal.user=ui.user=false;
    task_set_class(&normal,P_DEVICE);task_set_class(&ui,P_NORMAL);
    strcpy(ui.name,"display-marker");ui.id=33;strcpy(normal.name,"storage-writer");normal.id=15;
    idle.state=T_READY;ui.state=T_RUNNING;g_current=on_cpu=&ui;cpu_switch(&idle,&ui);
    CHECK(!getcontext(&writer_context));
    writer_context.uc_stack.ss_sp=writer_stack;writer_context.uc_stack.ss_size=sizeof(writer_stack);
    writer_context.uc_link=0;makecontext(&writer_context,cycle_writer,0);
    latency_switch_hook=cycle_switch;
    fs_lock_take(&publication_cache.lock);
    /* Suspend marker at a real scheduling boundary with its cache lock;
     * writer enters cache_reservation_wait and reserves the next ticket. */
    ui.state=T_BLOCKED;rq_push(&normal);schedule();
    CHECK(g_current==&ui && publication_cache.lock.next==2);
    fs_lock_drop(&publication_cache.lock);
    cycle_publish(0,&publication);
    if (cycle_done<2) schedule();
    CHECK(cycle_done==2 && cycle_attempts[0]==1 && cycle_attempts[1]==1);
    CHECK(publication_cache.devices[0].accepted==2 && !waiters && !ui.preempt_holds && !normal.preempt_holds);
    latency_switch_hook=0;
    printf("PASS T23 two-publisher FIFO cycle: real coroutine waits; marker+writer publish once each, switches=%u, no retry convoy or device I/O\n",cycle_switches);
}
static void publication_dispatch(void)
{
    if (pumping) return;
    pumping=true;
    while (g_current!=waiter) {
        CHECK(g_current==&normal);
        /* Boundary after B's ticket was served but before its stack ran.
         * Complete B's already-issued acquisition, then use the real unlock.
         * This is precisely the state cache_reservation_wait's unlock hands
         * to the other publisher on every retry. */
        struct fs_ticket earlier={&publication_cache.lock,0};
        CHECK(fs_ticket_ready(&earlier) && !publication_cache.lock.held);
        publication_cache.lock.held=true;
        kwait_set_owner(&publication_cache.lock.wait,g_current);
        kpreempt_hold();
        earlier_completed++;
        fs_lock_drop(&publication_cache.lock);
        normal.state=T_BLOCKED; normal.wake_tick=0; schedule();
    }
    pumping=false;
}
int main(void)
{
    cycle_test();
    reset(); vfs_init(&space); memset(&publication_cache,0,sizeof(publication_cache));
    fs_lock_init(&publication_cache.lock); fs_lock_classify(&publication_cache.lock,3,0);
    fs_event_init(&publication_cache.changed); fs_event_init(&publication_cache.writer_kick);
    struct blkdev dev={.sector_size=512,.capacity=128,.media_generation=7,
        .write_cache_state=BLKDEV_CACHE_ENABLED,.flush=unused_flush,.write=unused_write,.read=unused_read};
    CHECK(blkdev_durable(&dev));
    publication_cache.devices[0]=(struct cache_device){.dev=&dev,.epoch=1,.generation=7,.dirty_bytes=512};
    publication_cache.dirty_bytes=512; publication_cache.dirty_budget=65536;
    block=(struct cache_block){.dev=&dev,.data=bytes,.lba=10,.capacity=512,.sector_size=512,
        .generation=7,.pins=1,.dirty=true};
    publication=(struct cache_reservation){.cache=&publication_cache,.dev=&dev,.epoch=1,
        .generation=7,.count=1,.blocks={&block}};
    memset(image_bytes,0x5a,sizeof(image_bytes));
    struct cache_image image={0,image_bytes};
    hog.state=T_BLOCKED; normal.user=ui.user=false;
    task_set_class(&normal,P_DEVICE); task_set_class(&ui,P_NORMAL);
    strcpy(ui.name,"display-marker"); ui.id=33;
    strcpy(normal.name,"storage-writer"); normal.id=15;
    idle.state=T_READY; ui.state=T_RUNNING; g_current=&ui;
    cpu_switch(&idle,&ui); waiter=&ui; rq_push(&normal);
    /* Ticket 0 belongs to storage-writer; no task holds the cache mutex.
     * An ordinary optimistic try-lock can never take this served ticket. */
    publication_cache.lock.next=1;
    CHECK(!fs_lock_try_take(&publication_cache.lock));
    vfs_lock(&space,vfs_drive_mask(2));
    latency_switch_hook=publication_dispatch;
    int result=cache_publish(&publication,&image,1,0);
    latency_switch_hook=0;
    CHECK(result==0); /* pre-fix: -FS_EAGAIN, no scheduler/device progress */
    CHECK(earlier_completed==1 && publication.published==1 && publication.group==1);
    CHECK(!memcmp(bytes,image_bytes,512) && !publication_cache.lock.held);
    CHECK(space.active==vfs_drive_mask(2) && !normal.preempt_holds);
    vfs_unlock(&space); CHECK(!ui.preempt_holds);
    puts("PASS T23 publication FIFO: display-marker#33 queues behind storage-writer#15, one publication, zero device commands, guard order preserved");
    return 0;
}
