/* f1-87: real FAT/cache/VFS replay, no images. T23 service time is modeled
 * at the device boundary; mutex duration and logical exclusion are distinct.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "vfs.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#include "../t23_trial.h"
static struct vfs space;
static struct fat_volume vol;
static struct block_cache cache;
static struct vfs_table desktop,writer,audio,dos;
static uint8_t disk[2880*512],data[65536];
static uint32_t clock_ms,max_mutex,max_guard;
static _Thread_local unsigned namespace_depth,volume_depth;
static _Thread_local uint32_t acquired[27];
static bool replay;
/* f1-89 Amendment 1 item 4: during the concurrent copy phase the hold and
 * mutex bookkeeping (single-actor globals) is off, and time is real: each
 * device command occupies its actor for 111 ms of wall time and the clock
 * is the monotonic clock, so CPU work is charged at its real cost and one
 * actor's device time is never charged to another actor's CPU section. */
static atomic_bool concurrent;
static atomic_uint turn_commands;
static uint32_t phase_base;
static uint64_t phase_start_us;
static uint64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000+(uint64_t)t.tv_nsec/1000; }
static unsigned transfers,read_transfers,holds,logical_releases,missing_tags;
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
uint32_t fs_host_clock_ms(void) {
    if (atomic_load(&concurrent)) return phase_base+(uint32_t)((now_us()-phase_start_us)/1000);
    return __atomic_load_n(&clock_ms,__ATOMIC_RELAXED);
}
void __real_fs_lock_acquired(fs_lock *);
void __real_fs_lock_released(fs_lock *);
void __wrap_fs_lock_acquired(fs_lock *l) {
    __real_fs_lock_acquired(l);
    if (atomic_load(&concurrent)) {
        if (l==&space.lock) namespace_depth++;
        for (unsigned d=0;d<26;d++) if (l==&space.volume_lock[d]) volume_depth++;
        return;
    }
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
    if (atomic_load(&concurrent)) {
        if (l==&space.lock) namespace_depth--;
        for (unsigned d=0;d<26;d++) if (l==&space.volume_lock[d]) volume_depth--;
        __real_fs_lock_released(l); return;
    }
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
    if (atomic_load(&concurrent)) return;
    assert(!namespace_depth && !volume_depth && owner->op[0] && owner->site[0]);
    assert(strcmp(owner->op,"other")); holds++; if (ms>max_guard) max_guard=ms;
    if (owner->attribution!=VFS_ATTR_COMPLETE) missing_tags++;
    int i=operation(owner->op);
    if (i>=0 && ms>operations[i].max_hold) operations[i].max_hold=ms;
}
static void service(void) {
    if (atomic_load(&concurrent)) {
        /* No device command under a physical FS mutex; count commands
         * issued while this actor holds the writer turn. */
        assert(!namespace_depth && !volume_depth);
        if (__atomic_load_n(&space.turn_depth[2],__ATOMIC_RELAXED) &&
            __atomic_load_n(&space.turn_owner[2],__ATOMIC_RELAXED)==fs_task_token()) atomic_fetch_add(&turn_commands,1);
        struct timespec ts={0,(long)T23_SERVICE_MS*1000000L}; nanosleep(&ts,0); return;
    }
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
/* The desktop copy actor (the C3 copy shape: read a chunk, write it) and
 * a DOS VM writer actor (INT 21h appends and rewrites of its own file). */
#define COPY_BYTES (256u*1024u)
#define COPY_CHUNK (64u*1024u)
static atomic_bool copy_done;
static atomic_uint dos_writes;
static void *copy_actor(void *arg) {
    (void)arg; static uint8_t buf[COPY_CHUNK];
    int in=vfs_open(&desktop,"C:/SOURCE.BIN",VFS_READ,VFS_DENY_NONE,0);
    int out=vfs_open(&desktop,"C:/COPY.BIN",VFS_WRITE,VFS_DENY_NONE,0);
    assert(in>=0 && out>=0);
    for (unsigned pos=0;pos<COPY_BYTES;pos+=COPY_CHUNK) {
        size_t done; assert(!vfs_read(&desktop,in,buf,COPY_CHUNK,&done) && done==COPY_CHUNK);
        assert(!vfs_write(&desktop,out,buf,COPY_CHUNK,&done) && done==COPY_CHUNK);
    }
    assert(!vfs_commit(&desktop,out) && !vfs_close(&desktop,in) && !vfs_close(&desktop,out));
    atomic_store(&copy_done,true); return 0;
}
static void *dos_actor(void *arg) {
    (void)arg; static uint8_t buf[4096]; memset(buf,0x44,sizeof(buf));
    int h=vfs_open(&dos,"C:/DOSVM.SAV",VFS_WRITE,VFS_DENY_NONE,0); assert(h>=0);
    for (unsigned i=0;!atomic_load(&copy_done);i++) {
        uint64_t pos; size_t done;
        assert(!vfs_seek(&dos,h,(int64_t)(i%16)*4096,VFS_SEEK_SET,&pos)); /* append to 64 KiB, then rewrite */
        assert(!vfs_write(&dos,h,buf,sizeof(buf),&done) && done==sizeof(buf));
        atomic_fetch_add(&dos_writes,1);
    }
    assert(!vfs_close(&dos,h)); return 0;
}
int main(int argc,char **argv) {
    bool missing=argc==2 && !strcmp(argv[1],"--missing-tag");
    assert(argc==1 || missing || (argc==2 && !strcmp(argv[1],"--require-bounded")));
    if (missing) {
        vfs_init(&space); vfs_lock(&space,1u<<2); clock_ms+=50; vfs_unlock(&space);
        vfs_destroy(&space);
        fprintf(stderr,"F1-87 missing-tag negative control: rejected=%u\n",missing_tags);
        return missing_tags ? 1 : 0;
    }
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
    assert(read_transfers>=T23_DESKTOP_HOLDS && logical_releases && max_mutex==0 && !missing_tags);
    printf("F1-87 T23 replay desktop=513 log_write=35 commits=6 mkdir=1 service_ms=111 transfers=%u reads=%u physical_mutex_max_ms=%u logical_guard_max_ms=%u diagnostic_holds=%u\n",transfers,read_transfers,max_mutex,max_guard,holds);
    for (unsigned i=0;i<sizeof(operations)/sizeof(*operations);i++) {
        printf("F1-87 B operation=%s logical_hold_max_ms=%u preflight_or_transfer_commands=%u guarded_stage_commands=",
            operations[i].op,operations[i].max_hold,operations[i].unclassified_io);
        for (unsigned j=0;j<FAT_P_STAGES;j++) printf("%s%u",j ? "/" : "",operations[i].stage_io[j]);
        putchar('\n');
    }
    replay=false;
    /* Concurrent phase (f1-89 Amendment 1 item 4). Files are created first;
     * then the copy and the DOS writer run together on the 111 ms model. */
    assert(!vfs_table_init(&space,&dos,2));
    int source=vfs_open(&desktop,"C:/SOURCE.BIN",VFS_CREATE|VFS_WRITE,VFS_DENY_NONE,0); assert(source>=0);
    memset(data,0x53,sizeof(data));
    for (unsigned i=0;i<COPY_BYTES/sizeof(data);i++) assert(!vfs_write(&desktop,source,data,sizeof(data),&done) && done==sizeof(data));
    assert(!vfs_close(&desktop,source));
    int copy=vfs_open(&desktop,"C:/COPY.BIN",VFS_CREATE|VFS_WRITE,VFS_DENY_NONE,0); assert(copy>=0 && !vfs_close(&desktop,copy));
    int save=vfs_open(&dos,"C:/DOSVM.SAV",VFS_CREATE|VFS_WRITE,VFS_DENY_NONE,0); assert(save>=0 && !vfs_close(&dos,save));
    uint32_t waits_before=space.turn_waits[2]; space.turn_wait_max_ms[2]=0;
    uint32_t phase_start=fs_host_clock_ms();
    phase_base=phase_start; phase_start_us=now_us();
    atomic_store(&concurrent,true);
    pthread_t copier,vm; assert(!pthread_create(&copier,0,copy_actor,0) && !pthread_create(&vm,0,dos_actor,0));
    assert(!pthread_join(copier,0) && !pthread_join(vm,0));
    clock_ms=fs_host_clock_ms(); atomic_store(&concurrent,false);
    uint32_t turn_waits=space.turn_waits[2]-waits_before, turn_max=space.turn_wait_max_ms[2];
    printf("F1-89 T23 writer turn (real-time 111 ms commands): desktop_copy_bytes=%u dos_vm_writes=%u wall_ms=%u turn_waits=%u turn_wait_max_ms=%u commands_under_turn=%u budget_ms=20\n",
        COPY_BYTES,atomic_load(&dos_writes),fs_host_clock_ms()-phase_start,turn_waits,turn_max,atomic_load(&turn_commands));
    struct fat_entry st; assert(!vfs_stat(&desktop,"C:/COPY.BIN",&st) && st.size==COPY_BYTES);
    vfs_table_destroy(&dos);
    assert(!vfs_close(&desktop,resource) && !vfs_close(&writer,log));
    vfs_table_destroy(&desktop);vfs_table_destroy(&writer);vfs_table_destroy(&audio);
    assert(!vfs_detach(&space,2));vfs_destroy(&space);cache_destroy(&cache);
    assert(!blk_request_pool_host_cleanup());
    if (max_guard>20) {
        fprintf(stderr,"F1-87 B NOT ACCEPTED: whole-operation logical mutation guard max=%u ms > 20 ms; reserve/transfer/publish not installed\n",max_guard);
        if (argc==2) return 1;
    }
    if (turn_max>20) {
        fprintf(stderr,"F1-89 B NOT ACCEPTED: writer-turn wait behind the desktop copy max=%u ms > 20 ms\n",turn_max);
        if (argc==2) return 1;
    }
    return 0;
}
