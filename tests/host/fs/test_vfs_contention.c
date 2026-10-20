/* Real FAT/cache/VFS, a stalled device write, and two diagnostic task owners.
 * No guest images or kernel scheduling model. SPDX-License-Identifier: GPL-2.0-only */
#include "vfs.h"
#include <assert.h>
#include <stdio.h>
#include <stdatomic.h>

static struct vfs v;
static struct fat_volume volume;
static struct block_cache cache;
static uint8_t *disk;
static atomic_bool stall, in_device, release_device;
static unsigned hold_records, wait_records;
static uint32_t held_ms, waited_ms;
static unsigned threshold_holds;
static bool fake_clock;
static uint32_t clock_ms;
uint32_t fs_host_clock_ms(void) {
    if (fake_clock) return clock_ms;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint32_t)((uint64_t)t.tv_sec*1000+(uint32_t)t.tv_nsec/1000000);
}
static void pause_ms(unsigned ms) { struct timespec t={ms/1000,(long)(ms%1000)*1000000}; nanosleep(&t,0); }
static int read_disk(struct blkdev *d,uint64_t lba,uint32_t n,void *b) {
    assert(!blkdev_range(d,lba,n)); memcpy(b,disk+lba*512,n*512); return 0;
}
static int write_disk(struct blkdev *d,uint64_t lba,uint32_t n,const void *b) {
    assert(!blkdev_range(d,lba,n));
    if (atomic_exchange(&stall,false)) {
        /* The mutation guard, not namespace, spans the slow device call. */
        atomic_store(&in_device,true);
        uint32_t deadline=fs_now_ms()+10000;
        while (!atomic_load(&release_device)) { assert((int32_t)(fs_now_ms()-deadline)<0); pause_ms(1); }
    }
    memcpy(disk+lba*512,b,n*512); return 0;
}
static int flush_disk(struct blkdev *d) { (void)d; return 0; }
static uint64_t now(void *arg) { (void)arg; return (uint64_t)fs_now_ms()*1000; }
static void pause_request(void *arg) { (void)arg; pause_ms(1); }
static struct vfs_request writer_request={.now=now,.pause=pause_request,
    .task={.name="log writer",.id=17}};
static struct vfs_request waiter_request={.now=now,.pause=pause_request,
    .task={.name="files",.id=29}};
static struct vfs_table writer, waiter;
static int handle;
void vfs_hold_observer(char drive,uint32_t ms,const struct vfs_hold_info *h,uint32_t waiters) {
    if (drive=='D') {
        assert(fake_clock && !waiters && h->task.id==43 && !strcmp(h->task.name,"abcdefghijklmno"));
        assert(!strcmp(h->op,"log_reserve") && ms==(threshold_holds ? 75u : 50u));
        threshold_holds++; return;
    }
    if (strcmp(h->op,"write")) return;
    assert(drive=='C' && ms>=150 && !strcmp(h->task.name,"log_writer") && h->task.id==17);
    assert(waiters==1); held_ms=ms; hold_records++;
}
void vfs_wait_observer(char drive,uint32_t ms,char holder,const char *reason,
                       const struct fs_task_identity *task,const struct vfs_hold_info *h) {
    assert(drive=='C' && holder=='C' && ms>=100 && !strcmp(reason,"volume"));
    assert(!strcmp(task->name,"files") && task->id==29);
    assert(!strcmp(h->task.name,"log_writer") && h->task.id==17 && !strcmp(h->op,"write"));
    waited_ms=ms; wait_records++;
}
static void *write_thread(void *arg) {
    (void)arg; uint8_t data[512]; memset(data,0x86,sizeof(data)); size_t done=0;
    assert(!vfs_write(&writer,handle,data,sizeof(data),&done) && done==sizeof(data)); return 0;
}
static void *wait_thread(void *arg) {
    (void)arg; struct fat_entry e; assert(!vfs_stat(&waiter,"C:/TEST.BIN",&e) && e.size==512); return 0;
}
int main(void) {
    disk=calloc(2880,512); assert(disk);
    disk[0]=0xeb; disk[1]=0x3c; disk[2]=0x90;
    fs_wr16(disk+11,512); disk[13]=1; fs_wr16(disk+14,1); disk[16]=2;
    fs_wr16(disk+17,224); fs_wr16(disk+19,2880); disk[21]=0xf0;
    fs_wr16(disk+22,9); fs_wr16(disk+510,0xaa55);
    for (unsigned i=0;i<2;i++) { uint8_t *f=disk+(1+i*9)*512; f[0]=0xf0; f[1]=f[2]=0xff; }
    struct blkdev dev={.read=read_disk,.write=write_disk,.flush=flush_disk,
        .sector_size=512,.capacity=2880,.write_cache_state=BLKDEV_CACHE_DISABLED};
    assert(!cache_init(&cache,0) && !fat_mount(&volume,&cache,&dev,0,2880,true));
    vfs_init(&v); assert(!vfs_attach(&v,2,&volume));
    assert(!vfs_table_init(&v,&writer,2) && !vfs_table_init(&v,&waiter,2));
    handle=vfs_open(&writer,"C:/TEST.BIN",VFS_READ|VFS_WRITE|VFS_CREATE,0,0); assert(handle>=0);
    writer_request.deadline=waiter_request.deadline=now(0)+10000000;
    writer.request=&writer_request; waiter.request=&waiter_request;
    atomic_store(&stall,true); pthread_t a,b; assert(!pthread_create(&a,0,write_thread,0));
    uint32_t deadline=fs_now_ms()+10000;
    while (!atomic_load(&in_device)) { assert((int32_t)(fs_now_ms()-deadline)<0); pause_ms(1); }
    assert(fs_lock_try_take(&v.lock)); assert(v.mutating==(1u<<2) && !v.active);
    fs_lock_drop(&v.lock);
    assert(!pthread_create(&b,0,wait_thread,0));
    for (;;) {
        fs_lock_take(&v.lock); bool queued=v.diagnostic_waiters[2]==1; fs_lock_drop(&v.lock);
        if (queued) break;
        assert((int32_t)(fs_now_ms()-deadline)<0); pause_ms(1);
    }
    pause_ms(150); atomic_store(&release_device,true);
    assert(!pthread_join(a,0) && !pthread_join(b,0));
    assert(hold_records==1 && wait_records==1 && held_ms>=waited_ms);
    /* Exact release threshold and bounded name copy, including a context
     * temporarily suppressed for uncancellable legacy reservation cleanup. */
    struct vfs_request diagnostic=writer_request;
    memset(diagnostic.task.name,'x',sizeof(diagnostic.task.name));
    memcpy(diagnostic.task.name,"abcdefghijklmno",15); diagnostic.task.id=43;
    diagnostic.operation="log_reserve";
    fake_clock=true; clock_ms=1000;
    struct fs_request_scope scope; fs_request_enter(&scope,&diagnostic);
    vfs_lock(&v,1u<<3); clock_ms+=49; vfs_unlock(&v); assert(!threshold_holds);
    vfs_lock(&v,1u<<3); clock_ms+=50; vfs_unlock(&v); assert(threshold_holds==1);
    vfs_lock(&v,1u<<3); struct vfs_io_state io=vfs_io_begin_locked(&v);
    clock_ms+=75; vfs_io_end_locked(&v,io); vfs_unlock(&v); assert(threshold_holds==2);
    fs_request_leave(&scope); fake_clock=false;
    writer.request=waiter.request=0;
    assert(!vfs_close(&writer,handle)); vfs_table_destroy(&writer); vfs_table_destroy(&waiter);
    assert(!vfs_detach(&v,2)); vfs_destroy(&v); assert(!volume.mounted); cache_destroy(&cache); free(disk);
    assert(!blk_request_pool_host_cleanup());
    puts("PASS f1-86 VFS real FAT/cache + slow device: C, write, log_writer#17, files#29, waiters=1; exact 49/50ms threshold, bounded names and calldown context");
    return 0;
}
