/* f1-89: concurrent FAT mutators and readers through the public VFS.
 * The writer turn serialises mutating operations; readers restart their
 * lookups when a publication overlaps them. No public call may surface the
 * internal EAGAIN/EBUSY, and the volume must remount clean with every byte
 * where the operations put it. The image is written out for fsck.fat.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "vfs.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sched.h>
#include <time.h>
#define SECTORS 2880u
static struct vfs space;
static struct fat_volume volume;
static struct block_cache cache;
static uint8_t disk[SECTORS*512];
static pthread_mutex_t disk_lock=PTHREAD_MUTEX_INITIALIZER;
static atomic_uint commands, slow_reads;
static atomic_bool writers_done, stream_stop;
#define APPEND_CHUNK 700u
#define APPEND_COUNT 120u
#define NAMESPACE_ROUNDS 40u
#define STATIC_BYTES 16384u
#define STREAM_BYTES (384u*1024u)
#define CACHE_BYTES (256u*1024u) /* small: the stream misses on every read */
#define OK(what,expr) do { int e_=(expr); if (e_<0) { fprintf(stderr,"FAIL %s: %s = %d\n",what,#expr,e_); abort(); } } while (0)
static void pause_us(unsigned us) {
    struct timespec t={0,(long)us*1000}; nanosleep(&t,0);
}
/* Every command takes a little real time so unguarded phases overlap. */
static int rd(struct blkdev *d,uint64_t lba,uint32_t n,void *out) {
    assert(!blkdev_range(d,lba,n)); atomic_fetch_add(&commands,1);
    pause_us(atomic_load(&slow_reads) ? 2000 : 60);
    pthread_mutex_lock(&disk_lock); memcpy(out,disk+lba*512,n*512); pthread_mutex_unlock(&disk_lock); return 0;
}
static int wr(struct blkdev *d,uint64_t lba,uint32_t n,const void *in) {
    assert(!blkdev_range(d,lba,n)); atomic_fetch_add(&commands,1); pause_us(80);
    pthread_mutex_lock(&disk_lock); memcpy(disk+lba*512,in,n*512); pthread_mutex_unlock(&disk_lock); return 0;
}
static int flush(struct blkdev *d) { (void)d; atomic_fetch_add(&commands,1); pause_us(40); return 0; }
static struct blkdev dev={.read=rd,.write=wr,.flush=flush,.capacity=SECTORS,
    .sector_size=512,.write_cache_state=BLKDEV_CACHE_ENABLED};
static void format(void) {
    memset(disk,0,sizeof(disk)); disk[0]=0xeb; disk[1]=0x3c; disk[2]=0x90;
    memcpy(disk+3,"CIUKIOS ",8);
    fs_wr16(disk+11,512); disk[13]=1; fs_wr16(disk+14,1); disk[16]=2;
    fs_wr16(disk+17,224); fs_wr16(disk+19,SECTORS); disk[21]=0xf0;
    fs_wr16(disk+22,9); fs_wr16(disk+24,18); fs_wr16(disk+26,2); disk[38]=0x29;
    memcpy(disk+43,"NO NAME    ",11); memcpy(disk+54,"FAT12   ",8); fs_wr16(disk+510,0xaa55);
    for (unsigned f=0;f<2;f++) { uint8_t *fat=disk+(1+f*9)*512; fat[0]=0xf0; fat[1]=fat[2]=0xff; }
}
static uint8_t pattern(unsigned stream,unsigned offset) { return (uint8_t)(stream*37u+offset*7u+(offset>>9)); }
static void *appender(void *arg) {
    (void)arg; struct vfs_table t; OK("table",vfs_table_init(&space,&t,2));
    int h=vfs_open(&t,"C:/LOGS/APPEND.BIN",VFS_CREATE|VFS_WRITE|VFS_READ,VFS_DENY_WRITE,0); OK("append open",h);
    uint8_t chunk[APPEND_CHUNK];
    for (unsigned i=0;i<APPEND_COUNT;i++) {
        for (unsigned j=0;j<APPEND_CHUNK;j++) chunk[j]=pattern(1,i*APPEND_CHUNK+j);
        size_t done; OK("append write",vfs_write(&t,h,chunk,sizeof(chunk),&done)); assert(done==sizeof(chunk));
        if (!(i%16)) OK("append commit",vfs_commit(&t,h));
    }
    OK("append close",vfs_close(&t,h)); vfs_table_destroy(&t); return 0;
}
static void *namer(void *arg) {
    (void)arg; struct vfs_table t; OK("table",vfs_table_init(&space,&t,2));
    uint8_t data[1500]; char a[48],b[48],dir[32];
    for (unsigned i=0;i<NAMESPACE_ROUNDS;i++) {
        snprintf(dir,sizeof(dir),"C:/WORK/D%u",i%5);
        int e=vfs_mkdir(&t,dir,0); if (e!=-FS_EEXIST) OK("mkdir",e);
        snprintf(a,sizeof(a),"%s/Long name %u.txt",dir,i); snprintf(b,sizeof(b),"%s/R%u.TXT",dir,i);
        int h=vfs_open(&t,a,VFS_CREATE|VFS_WRITE,VFS_DENY_ALL,0); OK("create",h);
        for (unsigned j=0;j<sizeof(data);j++) data[j]=pattern(2+i,j);
        size_t done; OK("write",vfs_write(&t,h,data,sizeof(data),&done)); assert(done==sizeof(data));
        OK("times",vfs_handle_times(&t,h,0x5a21,0x6000));
        OK("close",vfs_close(&t,h));
        OK("rename",vfs_rename(&t,a,b));
        OK("attrib",vfs_attrib(&t,b,FAT_ATTR_ARCHIVE));
        struct fat_entry st; OK("stat",vfs_stat(&t,b,&st)); assert(st.size==sizeof(data));
        if (i%3) OK("delete",vfs_delete(&t,b)); /* every third file survives */
    }
    vfs_table_destroy(&t); return 0;
}
static void *reader(void *arg) {
    unsigned id=(unsigned)(uintptr_t)arg; struct vfs_table t; OK("table",vfs_table_init(&space,&t,2));
    static uint8_t buffers[4][STATIC_BYTES]; uint8_t *buf=buffers[id&3]; unsigned rounds=0;
    while (!atomic_load(&writers_done) || rounds<4) {
        struct fat_entry st;
        OK("reader stat deep",vfs_stat(&t,"C:/DATA/A/B/C/DEEP.TXT",&st)); assert(st.size==STATIC_BYTES);
        int h=vfs_open(&t,"C:/DATA/A/B/C/DEEP.TXT",VFS_READ,VFS_DENY_NONE,0); OK("reader open",h);
        size_t done; OK("reader read",vfs_read(&t,h,buf,STATIC_BYTES,&done)); assert(done==STATIC_BYTES);
        for (unsigned j=0;j<STATIC_BYTES;j++) assert(buf[j]==pattern(9,j));
        OK("reader close",vfs_close(&t,h));
        int e=vfs_stat(&t,"C:/WORK/D1",&st); if (e!=-FS_ENOENT) OK("reader stat work",e);
        rounds++;
    }
    vfs_table_destroy(&t); return 0;
}
/* Same file: one handle commits, one reads, the path's attributes change.
 * A commit pins nothing, so neither side may see EBUSY. */
static void *same_file(void *arg) {
    unsigned role=(unsigned)(uintptr_t)arg; struct vfs_table t; OK("table",vfs_table_init(&space,&t,2));
    int h=role==2 ? -1 : vfs_open(&t,"C:/DATA/SHARED.BIN",role ? VFS_READ : VFS_READ|VFS_WRITE,VFS_DENY_NONE,0);
    if (role!=2) OK("shared open",h);
    uint8_t buf[512]; unsigned rounds=0;
    while (!atomic_load(&writers_done) || rounds<4) {
        if (role==0) OK("shared commit",vfs_commit(&t,h));
        else if (role==1) { uint64_t pos; size_t done; OK("shared seek",vfs_seek(&t,h,0,VFS_SEEK_SET,&pos));
            OK("shared read",vfs_read(&t,h,buf,sizeof(buf),&done)); assert(done==sizeof(buf)); }
        else OK("shared attrib",vfs_attrib(&t,"C:/DATA/SHARED.BIN",(rounds&1) ? FAT_ATTR_ARCHIVE : 0));
        rounds++;
    }
    if (role!=2) OK("shared close",vfs_close(&t,h));
    vfs_table_destroy(&t); return 0;
}
/* Overlapping readers keep readers[] busy continuously; a publication
 * step must still get its short guard (reader gate). */
static void *stream_reader(void *arg) {
    (void)arg; struct vfs_table t; OK("table",vfs_table_init(&space,&t,2));
    int h=vfs_open(&t,"C:/DATA/STREAM.BIN",VFS_READ,VFS_DENY_NONE,0); OK("stream open",h);
    static uint8_t buffers[3][4096]; uint8_t *buf=buffers[(uintptr_t)arg%3]; unsigned n=(unsigned)(uintptr_t)arg*29u;
    while (!atomic_load(&stream_stop)) {
        uint64_t pos; OK("stream seek",vfs_seek(&t,h,(int64_t)((n++%(STREAM_BYTES/4096))*4096),VFS_SEEK_SET,&pos));
        size_t done; OK("stream read",vfs_read(&t,h,buf,4096,&done)); assert(done==4096);
    }
    OK("stream close",vfs_close(&t,h)); vfs_table_destroy(&t); return 0;
}
static uint64_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000; }
int main(int argc,char **argv) {
    assert(argc==2);
    format(); assert(!cache_init(&cache,CACHE_BYTES) && !fat_mount(&volume,&cache,&dev,0,SECTORS,true));
    vfs_init(&space); assert(!vfs_attach(&space,2,&volume));
    struct vfs_table setup; assert(!vfs_table_init(&space,&setup,2));
    const char *dirs[]={"C:/LOGS","C:/WORK","C:/DATA","C:/DATA/A","C:/DATA/A/B","C:/DATA/A/B/C"};
    for (unsigned i=0;i<sizeof(dirs)/sizeof(*dirs);i++) OK("setup mkdir",vfs_mkdir(&setup,dirs[i],0));
    int h=vfs_open(&setup,"C:/DATA/A/B/C/DEEP.TXT",VFS_CREATE|VFS_WRITE,VFS_DENY_NONE,0); OK("setup create",h);
    static uint8_t data[STATIC_BYTES]; for (unsigned j=0;j<STATIC_BYTES;j++) data[j]=pattern(9,j);
    size_t done; OK("setup write",vfs_write(&setup,h,data,sizeof(data),&done)); assert(done==sizeof(data));
    OK("setup close",vfs_close(&setup,h));
    h=vfs_open(&setup,"C:/DATA/STREAM.BIN",VFS_CREATE|VFS_WRITE,VFS_DENY_NONE,0); OK("setup stream",h);
    for (unsigned i=0;i<STREAM_BYTES/STATIC_BYTES;i++) { OK("setup stream write",vfs_write(&setup,h,data,sizeof(data),&done)); assert(done==sizeof(data)); }
    OK("setup stream close",vfs_close(&setup,h));
    h=vfs_open(&setup,"C:/DATA/SHARED.BIN",VFS_CREATE|VFS_WRITE,VFS_DENY_NONE,0); OK("setup shared",h);
    OK("setup shared write",vfs_write(&setup,h,data,1024,&done)); OK("setup shared close",vfs_close(&setup,h));
    OK("setup flush",vfs_flush_drive(&setup,2));
    /* Phase 1: two mutators and three readers, all public entry points. */
    pthread_t w[2],r[3],same[3];
    assert(!pthread_create(&w[0],0,appender,0) && !pthread_create(&w[1],0,namer,0));
    for (unsigned i=0;i<3;i++) assert(!pthread_create(&r[i],0,reader,(void *)(uintptr_t)i));
    for (unsigned i=0;i<3;i++) assert(!pthread_create(&same[i],0,same_file,(void *)(uintptr_t)i));
    for (unsigned i=0;i<2;i++) assert(!pthread_join(w[i],0));
    atomic_store(&writers_done,true);
    for (unsigned i=0;i<3;i++) assert(!pthread_join(r[i],0) && !pthread_join(same[i],0));
    uint32_t restarts=space.lookup_restarts, guarded=space.lookup_guarded, turn_waits=space.turn_waits[2], turn_max=space.turn_wait_max_ms[2];
    /* Phase 2: a continuous overlapping reader stream versus a writer. */
    atomic_store(&slow_reads,true);
    pthread_t s[3]; for (unsigned i=0;i<3;i++) assert(!pthread_create(&s[i],0,stream_reader,(void *)(uintptr_t)i));
    pause_us(20000);
    uint64_t start=now_ms();
    int g=vfs_open(&setup,"C:/WORK/GATE.BIN",VFS_CREATE|VFS_WRITE,VFS_DENY_ALL,0); OK("gate create",g);
    for (unsigned i=0;i<8;i++) { OK("gate write",vfs_write(&setup,g,data,2048,&done)); assert(done==2048); }
    OK("gate close",vfs_close(&setup,g));
    uint64_t gate_ms=now_ms()-start;
    atomic_store(&stream_stop,true); for (unsigned i=0;i<3;i++) assert(!pthread_join(s[i],0));
    atomic_store(&slow_reads,false);
    assert(gate_ms<20000); /* starvation would not terminate at all */
    /* Verification through the API, then through a fresh mount. */
    struct fat_entry st; OK("verify append",vfs_stat(&setup,"C:/LOGS/APPEND.BIN",&st)); assert(st.size==APPEND_COUNT*APPEND_CHUNK);
    OK("final flush",vfs_flush_drive(&setup,2));
    vfs_table_destroy(&setup);
    assert(!space.turn_depth[2] && !space.turn_waiters[2] && !space.transaction_users[2]);
    assert(!volume.transactions && !cache.reserved_bytes && !volume.readonly);
    assert(!vfs_detach(&space,2)); vfs_destroy(&space); cache_destroy(&cache);
    assert(!cache_init(&cache,CACHE_BYTES) && !fat_mount(&volume,&cache,&dev,0,SECTORS,true));
    assert(!volume.readonly && !volume.repaired_lost && !volume.dirty_recovered);
    vfs_init(&space); assert(!vfs_attach(&space,2,&volume)); assert(!vfs_table_init(&space,&setup,2));
    h=vfs_open(&setup,"C:/LOGS/APPEND.BIN",VFS_READ,VFS_DENY_NONE,0); OK("reopen",h);
    static uint8_t all[APPEND_COUNT*APPEND_CHUNK];
    OK("reread",vfs_read(&setup,h,all,sizeof(all),&done)); assert(done==sizeof(all));
    for (unsigned j=0;j<sizeof(all);j++) assert(all[j]==pattern(1,j));
    OK("reclose",vfs_close(&setup,h));
    unsigned kept=0;
    for (unsigned i=0;i<NAMESPACE_ROUNDS;i++) {
        char b[48]; snprintf(b,sizeof(b),"C:/WORK/D%u/R%u.TXT",i%5,i);
        int e=vfs_stat(&setup,b,&st);
        if (i%3) assert(e==-FS_ENOENT);
        else {
            assert(!e && st.size==1500 && st.times.write_date==0x5a21 && (st.attr&FAT_ATTR_ARCHIVE)); kept++;
            uint8_t back[1500]; int fh=vfs_open(&setup,b,VFS_READ,VFS_DENY_NONE,0); OK("kept open",fh);
            OK("kept read",vfs_read(&setup,fh,back,sizeof(back),&done)); assert(done==sizeof(back));
            for (unsigned j=0;j<sizeof(back);j++) assert(back[j]==pattern(2+i,j));
            OK("kept close",vfs_close(&setup,fh));
        }
    }
    vfs_table_destroy(&setup); assert(!vfs_detach(&space,2)); vfs_destroy(&space); /* detach unmounts */
    assert(!volume.mounted); cache_destroy(&cache);
    FILE *out=fopen(argv[1],"wb"); assert(out && fwrite(disk,1,sizeof(disk),out)==sizeof(disk) && !fclose(out));
    assert(!fs_host_contig_live && !blk_request_pool_host_cleanup());
    printf("PASS F1-89 concurrency: appends=%u namespace_rounds=%u kept=%u commands=%u reader_restarts=%u guarded_retries=%u writer_turn_waits=%u writer_turn_wait_max_ms=%u gate_write_ms=%llu remount=clean\n",
        APPEND_COUNT,NAMESPACE_ROUNDS,kept,atomic_load(&commands),restarts,guarded,turn_waits,turn_max,(unsigned long long)gate_ms);
    return 0;
}
