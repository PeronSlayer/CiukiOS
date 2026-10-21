/* Real VFS transactions racing the still-supported legacy/native FAT
 * calldowns. SPDX-License-Identifier: GPL-2.0-only */
#include "vfs.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sched.h>
static struct vfs space;
static struct fat_volume volume;
static struct block_cache cache;
static struct vfs_table writer, peer;
static uint8_t disk[2880*512];
static pthread_mutex_t disk_lock=PTHREAD_MUTEX_INITIALIZER;
static atomic_bool in_transfer, release_transfer, peer_started;
static _Thread_local bool transferring;
static unsigned scenario;
static int handle, write_error, peer_error;
static size_t written;
static struct fat_entry other;
static void await(atomic_bool *flag) {
    uint32_t limit=fs_now_ms()+10000;
    while (!atomic_load(flag)) { assert((int32_t)(fs_now_ms()-limit)<0); sched_yield(); }
}
static atomic_bool read_armed, in_read, release_read, guard_watch;
static atomic_uint guarded_reads, watched_reads;
static uint64_t stall_lba;
static int rd(struct blkdev *d,uint64_t lba,uint32_t n,void *out) {
    assert(!blkdev_range(d,lba,n));
    if (atomic_load(&guard_watch)) {
        atomic_fetch_add(&watched_reads,1);
        if (__atomic_load_n(&space.reserved,__ATOMIC_RELAXED)&(1u<<2) ||
            __atomic_load_n(&space.mutating,__ATOMIC_RELAXED)&(1u<<2)) atomic_fetch_add(&guarded_reads,1);
    }
    if (atomic_load(&read_armed) && lba<=stall_lba && stall_lba<lba+n) {
        atomic_store(&read_armed,false); atomic_store(&in_read,true); await(&release_read);
    }
    pthread_mutex_lock(&disk_lock);
    memcpy(out,disk+lba*512,n*512); pthread_mutex_unlock(&disk_lock); return 0;
}
static int wr(struct blkdev *d,uint64_t lba,uint32_t n,const void *in) {
    assert(!blkdev_range(d,lba,n));
    bool stop=scenario==3 ? lba==1 || lba==10 : lba>=33;
    if (transferring && stop && !atomic_load(&release_transfer)) {
        atomic_store(&in_transfer,true); await(&release_transfer);
    }
    pthread_mutex_lock(&disk_lock); memcpy(disk+lba*512,in,n*512); pthread_mutex_unlock(&disk_lock); return 0;
}
static int flush(struct blkdev *d) { (void)d; return 0; }
static struct blkdev dev={.read=rd,.write=wr,.flush=flush,.capacity=2880,
    .sector_size=512,.write_cache_state=BLKDEV_CACHE_DISABLED};
static void format(void) {
    memset(disk,0,sizeof(disk)); disk[0]=0xeb; disk[1]=0x3c; disk[2]=0x90;
    fs_wr16(disk+11,512); disk[13]=1; fs_wr16(disk+14,1); disk[16]=2;
    fs_wr16(disk+17,224); fs_wr16(disk+19,2880); disk[21]=0xf0;
    fs_wr16(disk+22,9); fs_wr16(disk+510,0xaa55);
    for (unsigned f=0;f<2;f++) { uint8_t *fat=disk+(1+f*9)*512; fat[0]=0xf0; fat[1]=fat[2]=0xff; }
}
static void *write_thread(void *unused) {
    (void)unused; uint8_t data[512]; memset(data,0x89,sizeof(data));
    transferring=true;
    write_error=vfs_write(&writer,handle,data,sizeof(data),&written);
    transferring=false; return 0;
}
static struct fat_entry observed;
static _Atomic uintptr_t peer_token;
static void *peer_thread(void *unused) {
    atomic_store(&peer_token,fs_task_token());
    (void)unused;
    if (scenario==4) { atomic_store(&peer_started,true); vfs_table_destroy(&writer); return 0; }
    /* f1-89 public peers: a mutation queues for the writer turn and then
     * succeeds; a reader never waits for the stalled writer at all. */
    if (scenario==5) { atomic_store(&peer_started,true); peer_error=vfs_mkdir(&peer,"C:/Other",0); return 0; }
    if (scenario==6) { peer_error=vfs_stat(&peer,"C:/Target",&observed); atomic_store(&peer_started,true); return 0; }
    vfs_lock(&space,1u<<2); vfs_tag_locked(&space,"race");
    struct vfs_node *node=writer.handles[handle]->node;
    other=node->entry;
    struct vfs_io_state io=vfs_io_begin_locked(&space);
    atomic_store(&peer_started,true);
    if (!scenario) peer_error=fat_create(&volume,0,"Other",FAT_ATTR_DIR,&other);
    else if (scenario==2) peer_error=fat_rename(&volume,&other,0,"Renamed");
    else peer_error=fat_remove(&volume,&other,false);
    vfs_io_end_locked(&space,io);
    if (!peer_error && scenario) {
        if (scenario==2) node->entry=other;
        else node->entry.index=UINT32_MAX; /* native open-unlinked retention */
    }
    vfs_unlock(&space); return 0;
}
/* f1-89: an unguarded lookup that read the root directory, then waits for
 * a cold subdirectory sector while a writer publishes a new root entry,
 * must restart (value validation) and still answer from one state. */
static int restart_error;
static struct fat_entry restart_entry;
static void *restart_reader(void *unused) {
    (void)unused; restart_error=vfs_stat(&peer,"C:/Dir/Inner.bin",&restart_entry); return 0;
}
static void reader_restart_scenario(void) {
    format(); assert(!cache_init(&cache,0) && !fat_mount(&volume,&cache,&dev,0,2880,true));
    vfs_init(&space); assert(!vfs_attach(&space,2,&volume));
    assert(!vfs_table_init(&space,&writer,2) && !vfs_table_init(&space,&peer,2));
    assert(!vfs_mkdir(&writer,"C:/Dir",0));
    int h=vfs_open(&writer,"C:/Dir/Inner.bin",VFS_CREATE|VFS_WRITE,VFS_DENY_NONE,0); assert(h>=0);
    assert(!vfs_close(&writer,h) && !vfs_flush_drive(&writer,2));
    struct fat_entry dir; assert(!fat_lookup(&volume,0,"Dir",&dir) && dir.first>=2);
    /* Root (sector 19) cached by the stat below; Dir's sector cold. */
    cache_invalidate(&cache,&dev,false);
    struct fat_entry root_probe; assert(!vfs_stat(&peer,"C:/Dir",&root_probe));
    stall_lba=volume.data_sector+(uint64_t)(dir.first-2)*volume.spc;
    uint32_t restarts=space.lookup_restarts, guarded=space.lookup_guarded;
    cache_invalidate(&cache,&dev,false);
    assert(!vfs_stat(&peer,"C:/Dir",&root_probe)); /* re-warm root only */
    atomic_store(&in_read,false); atomic_store(&release_read,false); atomic_store(&read_armed,true);
    pthread_t r; assert(!pthread_create(&r,0,restart_reader,0)); await(&in_read);
    /* The reader holds no reservation: a writer completes meanwhile and
     * republishes the root directory sector the reader already copied. */
    assert(!vfs_mkdir(&writer,"C:/Other",0));
    atomic_store(&release_read,true); assert(!pthread_join(r,0));
    assert(!restart_error && !restart_entry.size && restart_entry.parent==dir.first);
    assert(space.lookup_restarts==restarts+1 && space.lookup_guarded==guarded);
    vfs_table_destroy(&writer); vfs_table_destroy(&peer);
    assert(!vfs_detach(&space,2)); vfs_destroy(&space); cache_destroy(&cache);
    puts("PASS FAT transaction reader restart: unguarded lookup revalidated by value, one restart, no guarded fallback, consistent result");
}
/* f1-89: the storage writer's log append publishes only DATA into clusters
 * its file already owns, so it neither takes nor waits for the writer turn:
 * it completes while a turn holder is parked in a cold metadata read. */
static int bypass_error; static size_t bypass_done;
static void *turn_holder(void *unused) {
    (void)unused; uint8_t data[512]; memset(data,0x42,sizeof(data));
    bypass_error=vfs_write(&writer,handle,data,sizeof(data),&bypass_done); return 0;
}
static void log_bypass_scenario(void) {
    format(); assert(!cache_init(&cache,0) && !fat_mount(&volume,&cache,&dev,0,2880,true));
    vfs_init(&space); assert(!vfs_attach(&space,2,&volume));
    assert(!vfs_table_init(&space,&writer,2) && !vfs_table_init(&space,&peer,2));
    int log=vfs_open(&peer,"C:/BOOT.LOG",VFS_CREATE|VFS_READ|VFS_WRITE,VFS_DENY_ALL,0); assert(log>=0);
    uint8_t line[512]; size_t done; memset(line,0x4c,sizeof(line));
    assert(!vfs_log_reserve(&peer,log,4096) && !vfs_log_write(&peer,log,line,512,512,&done) && done==512);
    handle=vfs_open(&writer,"C:/Target",VFS_CREATE|VFS_WRITE|VFS_READ,VFS_DENY_NONE,0); assert(handle>=0);
    assert(!vfs_commit(&peer,log) && !vfs_flush_drive(&writer,2) && !cache.dirty_bytes);
    cache_invalidate(&cache,&dev,false);
    uint32_t waits=space.turn_waits[2];
    /* Park the turn holder in its DIRECTORY step (cold root sector), after
     * its DATA and detached CHAIN steps; the log append never reads it. */
    stall_lba=volume.root_sector;
    atomic_store(&in_read,false); atomic_store(&release_read,false); atomic_store(&read_armed,true);
    pthread_t w; assert(!pthread_create(&w,0,turn_holder,0)); await(&in_read);
    vfs_lock(&space,0); assert(!space.turn_waiters[2]); vfs_unlock(&space);
    memset(line,0x4d,sizeof(line));
    assert(!vfs_log_write(&peer,log,line,512,1024,&done) && done==512);
    assert(atomic_load(&in_read) && !atomic_load(&release_read) && space.turn_waits[2]==waits);
    atomic_store(&release_read,true); assert(!pthread_join(w,0));
    assert(!bypass_error && bypass_done==512 && space.turn_waits[2]==waits);
    assert(!vfs_log_publish(&peer,log,true) && !vfs_close(&peer,log) && !vfs_close(&writer,handle));
    vfs_table_destroy(&writer); vfs_table_destroy(&peer);
    assert(!vfs_detach(&space,2)); vfs_destroy(&space); cache_destroy(&cache);
    assert(!cache_init(&cache,0) && !fat_mount(&volume,&cache,&dev,0,2880,true));
    assert(!volume.readonly && !volume.repaired_lost);
    vfs_init(&space); assert(!vfs_attach(&space,2,&volume)); assert(!vfs_table_init(&space,&peer,2));
    struct fat_entry e; assert(!vfs_stat(&peer,"C:/Target",&e) && e.size==512);
    int h=vfs_open(&peer,"C:/BOOT.LOG",VFS_READ,VFS_DENY_NONE,0); assert(h>=0);
    uint8_t back[1024]; assert(!vfs_read(&peer,h,back,sizeof(back),&done) && done==1024);
    for (unsigned i=0;i<1024;i++) assert(back[i]==(i<512 ? 0x4c : 0x4d));
    assert(!vfs_close(&peer,h)); vfs_table_destroy(&peer);
    assert(!vfs_detach(&space,2)); vfs_destroy(&space); cache_destroy(&cache);
    puts("PASS FAT transaction log append bypass: completed while the turn holder was parked, no turn wait, both durable after remount");
}
/* f1-89 Amendment 1 item 4: an in-flight open-file reader excludes a
 * publication step. A writer parked in its unguarded data transfer (no
 * reservation held) lets a reader start; the reader then parks in the
 * device. Released, the writer's next publication step must not run
 * beside the reader, and must not wait for it while keeping its cache
 * reservation (pins and credits): it drops the reservation, gates new
 * readers and completes once the reader drains. The delivered step waited
 * inside its metadata reservation with the reservation still held. */
static int drain_read_error, drain_write_error; static size_t drain_done, drain_written;
static atomic_bool drain_write_done; static uint8_t drain_back[512];
static int drain_handle;
static void *drain_reader(void *unused) {
    (void)unused; drain_read_error=vfs_read(&peer,drain_handle,drain_back,sizeof(drain_back),&drain_done); return 0;
}
static void *drain_writer(void *unused) {
    (void)unused; uint8_t data[512]; memset(data,0x89,sizeof(data));
    transferring=true; drain_write_error=vfs_write(&writer,handle,data,sizeof(data),&drain_written); transferring=false;
    atomic_store(&drain_write_done,true); return 0;
}
static void reader_drain_scenario(void) {
    format(); assert(!cache_init(&cache,0) && !fat_mount(&volume,&cache,&dev,0,2880,true));
    vfs_init(&space); assert(!vfs_attach(&space,2,&volume));
    assert(!vfs_table_init(&space,&writer,2) && !vfs_table_init(&space,&peer,2));
    int h=vfs_open(&writer,"C:/Source",VFS_CREATE|VFS_WRITE,VFS_DENY_NONE,0); assert(h>=0);
    uint8_t data[512]; size_t done; memset(data,0x5a,sizeof(data));
    assert(!vfs_write(&writer,h,data,sizeof(data),&done) && done==512 && !vfs_close(&writer,h));
    assert(!vfs_flush_drive(&writer,2) && !cache.dirty_bytes);
    struct fat_entry source; assert(!fat_lookup(&volume,0,"Source",&source) && source.first>=2);
    drain_handle=vfs_open(&peer,"C:/Source",VFS_READ,VFS_DENY_NONE,0); assert(drain_handle>=0);
    handle=vfs_open(&writer,"C:/Target",VFS_CREATE|VFS_WRITE|VFS_READ,VFS_DENY_NONE,0); assert(handle>=0);
    assert(!vfs_flush_drive(&writer,2) && !cache.dirty_bytes);
    cache_invalidate(&cache,&dev,false);
    scenario=7; /* writer transfer stalls at its first data-area write */
    atomic_store(&in_transfer,false); atomic_store(&release_transfer,false); atomic_store(&drain_write_done,false);
    pthread_t r,w; assert(!pthread_create(&w,0,drain_writer,0)); await(&in_transfer);
    vfs_lock(&space,0); assert(!space.mutating && !space.reserved && !space.publish_waiting[2]); vfs_unlock(&space);
    stall_lba=volume.data_sector+(uint64_t)(source.first-2)*volume.spc;
    atomic_store(&in_read,false); atomic_store(&release_read,false); atomic_store(&read_armed,true);
    assert(!pthread_create(&r,0,drain_reader,0)); await(&in_read);
    vfs_lock(&space,0); assert(space.readers[2]==1); vfs_unlock(&space);
    atomic_store(&release_transfer,true);
    uint32_t until=fs_now_ms()+300;
    while ((int32_t)(fs_now_ms()-until)<0) { assert(!atomic_load(&drain_write_done)); sched_yield(); }
    vfs_lock(&space,0);
    assert(space.publish_waiting[2]==1 && space.readers[2]==1 && !space.reserved && !space.mutating);
    vfs_unlock(&space);
    assert(!__atomic_load_n(&cache.reserved_bytes,__ATOMIC_RELAXED)); /* no pins/credits while waiting */
    atomic_store(&release_read,true);
    assert(!pthread_join(r,0) && !pthread_join(w,0));
    assert(!drain_read_error && drain_done==512 && !drain_write_error && drain_written==512);
    for (unsigned i=0;i<sizeof(drain_back);i++) assert(drain_back[i]==0x5a);
    vfs_lock(&space,0); assert(!space.readers[2] && !space.publish_waiting[2] && !space.turn_depth[2]); vfs_unlock(&space);
    assert(!volume.transactions && !cache.reserved_bytes);
    assert(!vfs_close(&peer,drain_handle) && !vfs_close(&writer,handle));
    vfs_table_destroy(&writer); vfs_table_destroy(&peer);
    assert(!vfs_detach(&space,2)); vfs_destroy(&space); cache_destroy(&cache);
    assert(!cache_init(&cache,0) && !fat_mount(&volume,&cache,&dev,0,2880,true));
    struct fat_entry found;
    assert(!volume.readonly && !volume.repaired_lost && !fat_lookup(&volume,0,"Target",&found) && found.size==512);
    assert(!fat_unmount(&volume)); cache_destroy(&cache);
    puts("PASS FAT transaction reader drain: the writer's publication step waited for an in-flight reader with no cache reservation held and new readers gated, then both succeeded; remount clean");
}
/* f1-89 Amendment 1 item 3: no transaction (lookup per path component,
 * write, commit) needs physically contiguous frames. One armed contiguous
 * failure must survive lookups, a write and a commit unconsumed. */
static void page_object_scenario(void) {
    format(); assert(!cache_init(&cache,0) && !fat_mount(&volume,&cache,&dev,0,2880,true));
    vfs_init(&space); assert(!vfs_attach(&space,2,&volume)); assert(!vfs_table_init(&space,&writer,2));
    assert(!vfs_mkdir(&writer,"C:/A",0) && !vfs_mkdir(&writer,"C:/A/B",0));
    int h=vfs_open(&writer,"C:/A/B/File.bin",VFS_CREATE|VFS_WRITE|VFS_READ,VFS_DENY_NONE,0); assert(h>=0);
    uint8_t data[4096]; memset(data,0x33,sizeof(data)); size_t done; struct fat_entry e;
    assert(!vfs_write(&writer,h,data,sizeof(data),&done) && !vfs_stat(&writer,"C:/A/B/File.bin",&e)); /* warm */
    fs_host_contig_fail=1;
    assert(!vfs_stat(&writer,"C:/A/B/File.bin",&e) && e.size==sizeof(data));
    uint64_t pos; assert(!vfs_seek(&writer,h,0,VFS_SEEK_END,&pos));
    assert(!vfs_write(&writer,h,data,sizeof(data),&done) && done==sizeof(data));
    assert(!vfs_commit(&writer,h) && fs_host_contig_fail==1);
    fs_host_contig_fail=0;
    assert(!vfs_close(&writer,h)); vfs_table_destroy(&writer);
    assert(!vfs_detach(&space,2)); vfs_destroy(&space); cache_destroy(&cache);
    puts("PASS FAT transaction objects from ordinary pages: lookups, write and commit take no contiguous frames");
}
/* f1-89 Amendment 1 item 5: the guarded lookup attempt never reads the
 * disk while the guard is held. Directory larger than the 32-sector read
 * set, cold cache: every device read happens with no reservation held,
 * and the walk restarts until the sectors are resident. */
static void guarded_lookup_scenario(void) {
    format(); assert(!cache_init(&cache,0) && !fat_mount(&volume,&cache,&dev,0,2880,true));
    struct fat_entry big, file;
    assert(!fat_create(&volume,0,"BIG",FAT_ATTR_DIR,&big));
    for (unsigned i=0;i<280;i++) { char name[16]; snprintf(name,sizeof(name),"F%03u.BIN",i); assert(!fat_create(&volume,big.first,name,0,&file)); }
    assert(!fat_create(&volume,big.first,"ZZZ.BIN",0,&file) && file.index/16>=32);
    vfs_init(&space); assert(!vfs_attach(&space,2,&volume)); assert(!vfs_table_init(&space,&peer,2));
    assert(!cache.dirty_bytes); cache_invalidate(&cache,&dev,false);
    atomic_store(&guarded_reads,0); atomic_store(&watched_reads,0); atomic_store(&guard_watch,true);
    vfs_lock(&space,1u<<2);
    struct fat_entry found; int e; unsigned attempts=0;
    do e=vfs_fat_transaction_locked(&space,&volume,(struct fat_transaction_args){.op=FAT_TX_LOOKUP,
        .directory=big.first,.name="ZZZ.BIN",.entry=&found,.guarded=true}); while (e==-FS_EAGAIN && ++attempts<512);
    vfs_unlock(&space);
    assert(!e && found.index==file.index && attempts>1);
    assert(atomic_load(&watched_reads)>0 && !atomic_load(&guarded_reads));
    /* The public walk over more than 32 sectors also completes. */
    cache_invalidate(&cache,&dev,false);
    assert(!vfs_stat(&peer,"C:/BIG/ZZZ.BIN",&found) && !atomic_load(&guarded_reads));
    atomic_store(&guard_watch,false);
    vfs_table_destroy(&peer); assert(!vfs_detach(&space,2)); vfs_destroy(&space); cache_destroy(&cache);
    printf("PASS FAT guarded lookup: %u cache-warming restarts over a %u-sector directory, %u device reads, none under the guard\n",
        attempts,file.index/16+1,atomic_load(&watched_reads));
}
int main(void) {
    for (scenario=0;scenario<7;scenario++) {
        format(); assert(!cache_init(&cache,0));
        assert(!fat_mount(&volume,&cache,&dev,0,2880,true));
        vfs_init(&space); assert(!vfs_attach(&space,2,&volume));
        assert(!vfs_table_init(&space,&writer,2) && !vfs_table_init(&space,&peer,2));
        handle=vfs_open(&writer,"C:/Target",VFS_CREATE|VFS_WRITE|VFS_READ,0,0); assert(handle>=0);
        uint32_t free_before=volume.free_clusters;
        uint8_t data[512]={0}; size_t done;
        fs_host_page_fail=1; /* the transaction head page (f1-89 Amendment 1 item 3) */
        assert(vfs_write(&writer,handle,data,sizeof(data),&done)==-FS_ENOMEM && !done);
        assert(!volume.transactions && !cache.reserved_bytes && !writer.handles[handle]->node->pins);
        assert(writer.handles[handle]->refs==1 && !writer.handles[handle]->io_active);
        atomic_store(&in_transfer,false); atomic_store(&release_transfer,false); atomic_store(&peer_started,false);
        write_error=peer_error=0; written=0;
        pthread_t a,b; assert(!pthread_create(&a,0,write_thread,0)); await(&in_transfer);
        vfs_lock(&space,0);
        assert(!space.mutating && !space.reserved && space.transaction_users[2]==1);
        assert(writer.handles[handle]->node->pins==1 && volume.transactions);
        vfs_unlock(&space);
        assert(!pthread_create(&b,0,peer_thread,0)); await(&peer_started);
        if (scenario==5) {
            /* f1-89 Amendment 1 item 4: the writer gave its turn up for the
             * durability wait, so the mkdir owns the turn while the writer
             * is still parked; nobody queues for the turn. */
            uint32_t limit=fs_now_ms()+10000;
            for (bool owned=false;!owned;sched_yield()) {
                vfs_lock(&space,0); owned=space.turn_depth[2] && space.turn_owner[2]==atomic_load(&peer_token);
                assert(!space.turn_waiters[2]); vfs_unlock(&space);
                assert((int32_t)(fs_now_ms()-limit)<0);
            }
            assert(atomic_load(&in_transfer) && !atomic_load(&release_transfer));
        }
        if (scenario==6) {
            /* Completed while the writer is still parked inside its transfer. */
            assert(!pthread_join(b,0) && !peer_error && atomic_load(&in_transfer) && !atomic_load(&release_transfer));
            assert(observed.size==0 && !observed.first); /* consistent prefix */
        }
        atomic_store(&release_transfer,true);
        assert(!pthread_join(a,0) && (scenario==6 || !pthread_join(b,0)));
        if (scenario>=5) {
            assert(!write_error && written==512 && !peer_error && !volume.readonly);
            assert(!volume.transactions && !cache.reserved_bytes && !space.transaction_users[2]);
            assert(!space.turn_depth[2] && !space.turn_waiters[2]);
            struct fat_entry found;
            assert(!fat_lookup(&volume,0,"Target",&found) && found.size==512 && found.first);
            if (scenario==5) assert(!fat_lookup(&volume,0,"Other",&found) && (found.attr&FAT_ATTR_DIR));
            else { assert(fat_lookup(&volume,0,"Other",&found)==-FS_ENOENT); assert(!vfs_stat(&peer,"C:/Target",&observed) && observed.size==512); }
            assert(!vfs_close(&writer,handle)); vfs_table_destroy(&writer); vfs_table_destroy(&peer);
            assert(!vfs_detach(&space,2)); vfs_destroy(&space); cache_destroy(&cache);
            assert(!cache_init(&cache,0) && !fat_mount(&volume,&cache,&dev,0,2880,true));
            assert(!volume.readonly && !volume.repaired_lost);
            assert(!fat_lookup(&volume,0,"Target",&found) && found.size==512);
            assert(!fat_unmount(&volume)); cache_destroy(&cache);
            printf("PASS FAT transaction public race=%u %s; no EAGAIN, remount clean\n",scenario,
                scenario==5 ? "mkdir took the writer turn while the writer waited for durability, then both succeeded" :
                "stat completed during the writer's unguarded transfer");
            continue;
        }
        if (scenario==4) {
            assert(!write_error && written==512 && !writer.vfs);
            assert(!volume.transactions && !cache.reserved_bytes && !space.transaction_users[2]);
            vfs_table_destroy(&peer);
            assert(!vfs_detach(&space,2)); vfs_destroy(&space); cache_destroy(&cache);
            puts("PASS FAT transaction table teardown waits without a volume guard; borrowed description retires once");
            continue;
        }
        /* f1-89 Amendment 1 item 4: the writer waited for durability without
         * the turn and rebased, so an unrelated legacy create (0) and an
         * in-place legacy rename of its file (2, same slot) no longer abort
         * it; a legacy remove of its own target still aborts cleanly. */
        bool completes=scenario==0 || scenario==2;
        if (completes) assert(!peer_error && !write_error && written==512 && !volume.readonly);
        else assert(!peer_error && write_error==-FS_EAGAIN && !written && !volume.readonly);
        assert(!volume.transactions && !cache.reserved_bytes && !cache.dirty_bytes);
        assert(!writer.handles[handle]->node->pins && writer.handles[handle]->refs==1);
        struct fat_entry found; uint32_t cluster;
        assert(!fat_get_cluster(&volume,2,&cluster));
        if (!scenario) {
            assert(other.first==3 && cluster>=0xff8 && volume.free_clusters==free_before-2);
            assert(!fat_lookup(&volume,0,"Target",&found) && found.first==2 && found.size==512);
        } else {
            assert(fat_lookup(&volume,0,"Target",&found)==-FS_ENOENT);
            if (scenario==2) assert(!fat_lookup(&volume,0,"Renamed",&found) && found.first==2 && found.size==512);
            if (scenario>=2) assert(cluster>=0xff8); else assert(!cluster);
        }
        assert(!vfs_close(&writer,handle)); vfs_table_destroy(&writer); vfs_table_destroy(&peer);
        assert(!vfs_detach(&space,2)); vfs_destroy(&space); cache_destroy(&cache);
        assert(!cache_init(&cache,0) && !fat_mount(&volume,&cache,&dev,0,2880,true));
        assert(!volume.readonly && !volume.transactions);
        if (scenario==3) assert(volume.repaired_lost==1 && volume.free_clusters==free_before);
        assert(!fat_get_cluster(&volume,2,&cluster) && (completes ? cluster>=0xff8 : !cluster) && !volume.repaired_lost==(scenario!=3));
        assert(!fat_unmount(&volume)); cache_destroy(&cache);
        printf("PASS FAT transaction race=%u allocation exclusion, %s, pins/credits retired, remount reclaimed=%u\n",scenario,
            completes ? "legacy create/in-place rename meanwhile: rebase and complete" : "target removed by a legacy caller: clean abort",scenario==3);
    }
    reader_restart_scenario();
    log_bypass_scenario();
    reader_drain_scenario();
    page_object_scenario();
    guarded_lookup_scenario();
    assert(!fs_host_contig_live && !blk_request_pool_host_cleanup());
    return 0;
}
