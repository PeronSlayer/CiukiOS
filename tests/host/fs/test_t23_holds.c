/* f1-87: real FAT/cache/VFS replay, no images. T23 service time is modeled
 * at the device boundary; mutex duration and logical exclusion are distinct.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "vfs.h"
#include <assert.h>
#include <stdio.h>
static struct vfs space;
static struct fat_volume vol;
static struct block_cache cache;
static struct vfs_table desktop,writer,audio;
static uint8_t disk[2880*512],data[65536];
static uint32_t clock_ms,max_mutex,max_guard;
static _Thread_local unsigned namespace_depth,volume_depth;
static _Thread_local uint32_t acquired[27];
static bool replay;
static unsigned transfers,read_transfers,holds;
uint32_t fs_host_clock_ms(void) { return clock_ms; }
void __real_fs_lock_acquired(fs_lock *);
void __real_fs_lock_released(fs_lock *);
void __wrap_fs_lock_acquired(fs_lock *l) {
    __real_fs_lock_acquired(l);
    if (l==&space.lock) { namespace_depth++; acquired[26]=clock_ms; }
    for (unsigned d=0;d<26;d++) if (l==&space.volume_lock[d]) {
        assert(space.holds[d].op[0] && space.holds[d].site[0]);
        assert(strcmp(space.holds[d].op,"other"));
        volume_depth++; acquired[d]=clock_ms;
    }
}
void __wrap_fs_lock_released(fs_lock *l) {
    if (l==&space.lock) { namespace_depth--; uint32_t ms=clock_ms-acquired[26]; if (ms>max_mutex) max_mutex=ms; }
    for (unsigned d=0;d<26;d++) if (l==&space.volume_lock[d]) {
        volume_depth--; uint32_t ms=clock_ms-acquired[d]; if (ms>max_mutex) max_mutex=ms;
    }
    __real_fs_lock_released(l);
}
void vfs_hold_observer(char drive,uint32_t ms,const struct vfs_hold_info *owner,uint32_t waiters) {
    (void)drive; (void)waiters;
    assert(!namespace_depth && !volume_depth && owner->op[0] && owner->site[0]);
    assert(strcmp(owner->op,"other")); holds++; if (ms>max_guard) max_guard=ms;
}
static void service(void) {
    if (replay) { assert(!namespace_depth && !volume_depth); clock_ms+=111; transfers++; }
}
static int rd(struct blkdev *d,uint64_t lba,uint32_t n,void *buf) {
    assert(!blkdev_range(d,lba,n)); service(); if (replay) read_transfers++;
    memcpy(buf,disk+lba*512,n*512); return 0;
}
static int wr(struct blkdev *d,uint64_t lba,uint32_t n,const void *buf) {
    assert(!blkdev_range(d,lba,n)); service(); memcpy(disk+lba*512,buf,n*512); return 0;
}
static int flush(struct blkdev *d) { (void)d; service(); return 0; }
int main(void) {
    disk[0]=0xeb;disk[1]=0x3c;disk[2]=0x90;fs_wr16(disk+11,512);disk[13]=1;
    fs_wr16(disk+14,1);disk[16]=2;fs_wr16(disk+17,224);fs_wr16(disk+19,2880);
    disk[21]=0xf0;fs_wr16(disk+22,9);fs_wr16(disk+510,0xaa55);
    for (unsigned i=0;i<2;i++) { uint8_t *f=disk+(1+i*9)*512;f[0]=0xf0;f[1]=f[2]=0xff; }
    struct blkdev dev={.read=rd,.write=wr,.flush=flush,.capacity=2880,.sector_size=512,
        .write_cache_state=BLKDEV_CACHE_DISABLED};
    assert(!cache_init(&cache,0) && !fat_mount(&vol,&cache,&dev,0,2880,true));
    vfs_init(&space);assert(!vfs_attach(&space,2,&vol));
    assert(!vfs_table_init(&space,&desktop,2) && !vfs_table_init(&space,&writer,2) && !vfs_table_init(&space,&audio,2));
    int resource=vfs_open(&desktop,"C:/RESOURCE.BIN",VFS_CREATE|VFS_READ|VFS_WRITE,0,0);
    int log=vfs_open(&writer,"C:/BOOT.LOG",VFS_CREATE|VFS_READ|VFS_WRITE,VFS_DENY_ALL,0);
    assert(resource>=0 && log>=0);memset(data,0x87,sizeof(data));size_t done;
    assert(!vfs_write(&desktop,resource,data,sizeof(data),&done) && done==sizeof(data));
    assert(!vfs_log_reserve(&writer,log,32768));
    replay=true;
    /* Exact 513 desktop requests and the 35 log writes from the trial;
     * its long-tail distribution is retained separately in the audio model.
     * Include commit and DOS mkdir through the same production stack. */
    for (unsigned i=0;i<513;i++) {
        /* Exercise device requests for every resource read, rather than
         * replaying 512 cache hits after a single cold read. All previous
         * writes have completed their production durability barrier. */
        assert(!cache.dirty_bytes); cache_invalidate(&cache,&dev,false);
        uint64_t pos; assert(!vfs_seek(&desktop,resource,0,VFS_SEEK_SET,&pos));
        assert(!vfs_read(&desktop,resource,data,sizeof(data),&done) && done==sizeof(data));
        for (unsigned j=0;j<sizeof(data);j++) assert(data[j]==0x87);
        if (i<35) assert(!vfs_log_write(&writer,log,data,512,(i+1)*512,&done) && done==512);
        if (!(i%100)) assert(!vfs_commit(&writer,log));
    }
    assert(!vfs_mkdir(&audio,"C:/DOSDIR",0));
    assert(read_transfers>=513 && holds && max_mutex<=20);
    printf("F1-87 T23 replay desktop=513 log_write=35 commits=6 mkdir=1 service_ms=111 transfers=%u reads=%u physical_mutex_max_ms=%u logical_guard_max_ms=%u\n",transfers,read_transfers,max_mutex,max_guard);
    replay=false;assert(!vfs_close(&desktop,resource) && !vfs_close(&writer,log));
    vfs_table_destroy(&desktop);vfs_table_destroy(&writer);vfs_table_destroy(&audio);
    assert(!vfs_detach(&space,2));vfs_destroy(&space);cache_destroy(&cache);
    assert(!blk_request_pool_host_cleanup());return 0;
}
