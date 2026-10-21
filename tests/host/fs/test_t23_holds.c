/* f1-87: real FAT/cache/VFS replay, no images. T23 service time is modeled
 * at the device boundary; mutex duration and logical exclusion are distinct.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "vfs.h"
#include <assert.h>
#include <stdio.h>
#include "../t23_trial.h"
static struct vfs space;
static struct fat_volume vol;
static struct block_cache cache;
static struct vfs_table desktop,writer,audio;
static uint8_t disk[2880*512],data[65536];
static uint32_t clock_ms,max_mutex,max_guard;
static _Thread_local unsigned namespace_depth,volume_depth;
static _Thread_local uint32_t acquired[27];
static bool replay;
static unsigned transfers,read_transfers,holds,logical_releases;
static bool logical_held;
static uint32_t logical_started;
static char logical_op[12];
static struct {
    const char *op;
    uint32_t max_hold, unclassified_io, stage_io[FAT_P_STAGES];
} operations[]={{.op="read"},{.op="log_write"},{.op="commit"},{.op="mkdir"},
    {.op="write"},{.op="unlink"},{.op="rename"}};
static int operation(const char *op) {
    for (unsigned i=0;i<sizeof(operations)/sizeof(*operations);i++)
        if (!strcmp(op,operations[i].op)) return (int)i;
    return -1;
}
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
        if (d==2 && !logical_held) {
            logical_held=true; logical_started=clock_ms;
            memcpy(logical_op,space.holds[d].op,sizeof(logical_op));
        }
    }
}
void __wrap_fs_lock_released(fs_lock *l) {
    if (l==&space.lock) {
        namespace_depth--; uint32_t ms=clock_ms-acquired[26]; if (ms>max_mutex) max_mutex=ms;
        /* Observe EVERY logical release, including 21..49 ms sections which
         * the production >=50 ms diagnostic budget intentionally omits. */
        if (logical_held && !((space.reserved|space.mutating)&vfs_drive_mask(2))) {
            uint32_t held=clock_ms-logical_started; logical_held=false;
            if (replay) {
                logical_releases++;
                if (held>max_guard) max_guard=held;
                int i=operation(logical_op);
                if (i>=0 && held>operations[i].max_hold) operations[i].max_hold=held;
            }
        }
    }
    for (unsigned d=0;d<26;d++) if (l==&space.volume_lock[d]) {
        volume_depth--; uint32_t ms=clock_ms-acquired[d]; if (ms>max_mutex) max_mutex=ms;
        if (d==2 && space.holds[d].op[0]) memcpy(logical_op,space.holds[d].op,sizeof(logical_op));
    }
    __real_fs_lock_released(l);
}
void vfs_hold_observer(char drive,uint32_t ms,const struct vfs_hold_info *owner,uint32_t waiters) {
    (void)drive; (void)waiters;
    assert(!namespace_depth && !volume_depth && owner->op[0] && owner->site[0]);
    assert(strcmp(owner->op,"other")); holds++; if (ms>max_guard) max_guard=ms;
    int i=operation(owner->op);
    if (i>=0 && ms>operations[i].max_hold) operations[i].max_hold=ms;
}
static void service(void) {
    if (replay) {
        assert(!namespace_depth && !volume_depth);
        if (space.mutating & vfs_drive_mask(2)) {
            int i=operation(space.holds[2].op);
            if (i>=0) {
                if (vol.publication_pending) operations[i].stage_io[vol.publication_stage]++;
                else operations[i].unclassified_io++;
            }
        }
        clock_ms+=T23_SERVICE_MS; transfers++;
    }
}
static int rd(struct blkdev *d,uint64_t lba,uint32_t n,void *buf) {
    assert(!blkdev_range(d,lba,n)); service(); if (replay) read_transfers++;
    memcpy(buf,disk+lba*512,n*512); return 0;
}
static int wr(struct blkdev *d,uint64_t lba,uint32_t n,const void *buf) {
    assert(!blkdev_range(d,lba,n)); service(); memcpy(disk+lba*512,buf,n*512); return 0;
}
static int flush(struct blkdev *d) { (void)d; service(); return 0; }
int main(int argc,char **argv) {
    assert(argc==1 || (argc==2 && !strcmp(argv[1],"--require-bounded")));
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
    max_guard=max_mutex=0;
    for (unsigned i=0;i<sizeof(operations)/sizeof(*operations);i++) operations[i].max_hold=0;
    replay=true;
    /* Exact 513 desktop requests and the 35 log writes from the trial;
     * its long-tail distribution is retained separately in the audio model.
     * Include commit and DOS mkdir through the same production stack. */
    for (unsigned i=0;i<T23_DESKTOP_HOLDS;i++) {
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
    int changed=vfs_open(&desktop,"C:/CHANGED.BIN",VFS_CREATE|VFS_WRITE,0,0);
    assert(changed>=0);
    assert(!vfs_write(&desktop,changed,data,1536,&done) && done==1536);
    assert(!vfs_close(&desktop,changed));
    assert(!vfs_rename(&desktop,"C:/CHANGED.BIN","C:/RENAMED.BIN"));
    assert(!vfs_delete(&desktop,"C:/RENAMED.BIN"));
    assert(read_transfers>=T23_DESKTOP_HOLDS && logical_releases && max_mutex==0);
    printf("F1-87 T23 replay desktop=513 log_write=35 commits=6 mkdir=1 service_ms=111 transfers=%u reads=%u physical_mutex_max_ms=%u logical_guard_max_ms=%u diagnostic_holds=%u\n",transfers,read_transfers,max_mutex,max_guard,holds);
    for (unsigned i=0;i<sizeof(operations)/sizeof(*operations);i++) {
        printf("F1-87 B operation=%s logical_hold_max_ms=%u preflight_or_transfer_commands=%u guarded_stage_commands=",
            operations[i].op,operations[i].max_hold,operations[i].unclassified_io);
        for (unsigned j=0;j<FAT_P_STAGES;j++) printf("%s%u",j ? "/" : "",operations[i].stage_io[j]);
        putchar('\n');
    }
    replay=false;assert(!vfs_close(&desktop,resource) && !vfs_close(&writer,log));
    vfs_table_destroy(&desktop);vfs_table_destroy(&writer);vfs_table_destroy(&audio);
    assert(!vfs_detach(&space,2));vfs_destroy(&space);cache_destroy(&cache);
    assert(!blk_request_pool_host_cleanup());
    if (max_guard>20) {
        fprintf(stderr,"F1-87 B NOT ACCEPTED: whole-operation logical mutation guard max=%u ms > 20 ms; reserve/transfer/publish not installed\n",max_guard);
        if (argc==2) return 1;
    }
    return 0;
}
