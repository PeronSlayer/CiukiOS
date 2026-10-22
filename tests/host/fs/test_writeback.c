/* F1-82: production FAT/VFS copy versus a periodic reader, and allocation
 * accounting on a virtual 512 MiB FAT32 disk. No images or QEMU.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "vfs.h"
#include <stdio.h>
#include <stdatomic.h>
#include <errno.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %u: %s\n",__LINE__,#x); abort(); } } while (0)
#define COPY_BYTES (10u*1024*1024)
#define CHUNK 65536u
#define TOTAL 1048576u
#define FAT_SECTORS 1024u
#define DATA (32u+2*FAT_SECTORS)
#define READER_BYTES (1024u*1024)
#define FIRST_FREE (3u+READER_BYTES/4096)
void fs_assert_io_unlocked(void);
static uint64_t ns(clockid_t clock) {
    struct timespec t; CHECK(!clock_gettime(clock,&t)); return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;
}
static void until(uint64_t time) {
    struct timespec t={(time_t)(time/1000000000),(long)(time%1000000000)};
    int e; do { e=clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&t,0); } while (e==EINTR); CHECK(!e);
}
struct disk {
    struct blkdev dev; uint8_t *bytes; size_t stored;
    pthread_mutex_t transport;
    bool timed; uint64_t commands, sectors, fat_writes, directory_writes, hint_writes, flushes;
};
static int read_disk(struct blkdev *d,uint64_t lba,uint32_t n,void *buf) {
    fs_assert_io_unlocked(); struct disk *m=d->ctx; CHECK(!blkdev_range(d,lba,n));
    pthread_mutex_lock(&m->transport);
    if (m->timed) until(ns(CLOCK_MONOTONIC)+(uint64_t)n*512*1000000000/20000000);
    memset(buf,0,n*512);
    if (lba*512<m->stored) {
        size_t take=m->stored-lba*512; if (take>n*512) take=n*512;
        memcpy(buf,m->bytes+lba*512,take);
    }
    pthread_mutex_unlock(&m->transport); return 0;
}
static int write_disk(struct blkdev *d,uint64_t lba,uint32_t n,const void *buf) {
    fs_assert_io_unlocked(); struct disk *m=d->ctx; CHECK(!blkdev_range(d,lba,n) && (lba+n)*512<=m->stored);
    pthread_mutex_lock(&m->transport);
    /* A 20 MB/s transfer plus 1 ms command/setup latency. Bulk bandwidth
     * alone does not model the measured single-sector write amplification. */
    if (m->timed) until(ns(CLOCK_MONOTONIC)+1000000+(uint64_t)n*512*1000000000/20000000);
    m->commands++; m->sectors+=n;
    for (unsigned i=0;i<n;i++) {
        uint64_t l=lba+i;
        if (l>=32 && l<DATA) m->fat_writes++;
        else if (l==DATA) m->directory_writes++;
        else if (l==1) m->hint_writes++;
    }
    memcpy(m->bytes+lba*512,buf,n*512);
    pthread_mutex_unlock(&m->transport); return 0;
}
static int flush_disk(struct blkdev *d) {
    fs_assert_io_unlocked(); struct disk *m=d->ctx;
    pthread_mutex_lock(&m->transport); m->flushes++; pthread_mutex_unlock(&m->transport); return 0;
}
static void init(struct disk *m,struct block_cache *c,struct fat_volume *v) {
    memset(m,0,sizeof(*m)); CHECK(!pthread_mutex_init(&m->transport,0));
    m->stored=(size_t)DATA*512+READER_BYTES+COPY_BYTES+1024*1024;
    m->bytes=calloc(1,m->stored); CHECK(m->bytes);
    m->dev=(struct blkdev){.read=read_disk,.write=write_disk,.flush=flush_disk,.capacity=TOTAL,
        .sector_size=512,.write_cache_state=BLKDEV_CACHE_ENABLED,.ctx=m};
    CHECK(!cache_init(c,0));
    *v=(struct fat_volume){.cache=c,.dev=&m->dev,.sectors=TOTAL,.reserved=32,.fat_sectors=FAT_SECTORS,
        .data_sector=DATA,.clusters=(TOTAL-DATA)/8,.root=2,.type=32,.spc=8,.fats=2,.fsinfo=1,
        .next_free=FIRST_FREE,.mounted=true,.writable_session=true};
    v->free_clusters=v->clusters-(FIRST_FREE-2);
    for (unsigned f=0;f<2;f++) {
        uint8_t *fat=m->bytes+(32+f*FAT_SECTORS)*512;
        fs_wr32(fat,0x0ffffff8); fs_wr32(fat+4,0x0fffffff); fs_wr32(fat+8,0x0fffffff);
        for (unsigned i=3;i<FIRST_FREE;i++) fs_wr32(fat+i*4,i+1<FIRST_FREE ? i+1 : 0x0fffffff);
    }
    uint8_t *root=m->bytes+DATA*512;
    memcpy(root,"AUDIO   BIN",11); root[11]=FAT_ATTR_ARCHIVE; fs_wr16(root+26,3); fs_wr32(root+28,READER_BYTES);
    memcpy(root+32,"COPY    BIN",11); root[43]=FAT_ATTR_ARCHIVE;
    memset(m->bytes+(DATA+8)*512,0x97,READER_BYTES);
    fs_wr32(m->bytes+512,0x41615252); fs_wr32(m->bytes+512+484,0x61417272); fs_wr32(m->bytes+512+508,0xaa550000);
}
static void finish(struct disk *m,struct block_cache *c) {
    cache_destroy(c); free(m->bytes); CHECK(!pthread_mutex_destroy(&m->transport));
}
/* Count actual metadata cache calls without changing/instrumenting FAT. */
static _Atomic uint64_t fat_probes, fat_updates;
int __real_cache_read_ahead(struct block_cache *,struct blkdev *,uint64_t,uint32_t,void *);
int __wrap_cache_read_ahead(struct block_cache *c,struct blkdev *d,uint64_t l,uint32_t n,void *b) {
    if (l>=32 && l<DATA) fat_probes++;
    return __real_cache_read_ahead(c,d,l,n,b);
}
int __real_cache_write(struct block_cache *,struct blkdev *,uint64_t,const void *);
int __wrap_cache_write(struct block_cache *c,struct blkdev *d,uint64_t l,const void *b) {
    if (l>=32 && l<DATA) fat_updates++;
    return __real_cache_write(c,d,l,b);
}
static void audit(void) {
    struct disk m; struct block_cache c; struct fat_volume v; init(&m,&c,&v);
    struct fat_entry file; CHECK(!fat_lookup(&v,2,"COPY.BIN",&file));
    uint8_t buf[CHUNK]; memset(buf,0x58,sizeof(buf));
    uint64_t max=0,total=0;
    for (unsigned i=0;i<160;i++) {
        uint64_t probes=fat_probes,updates=fat_updates,fw=m.fat_writes,dw=m.directory_writes,hw=m.hint_writes,fl=m.flushes;
        uint32_t hint=v.next_free; size_t done;
        uint64_t start=ns(CLOCK_THREAD_CPUTIME_ID);
        CHECK(!fat_write(&v,&file,file.size,buf,sizeof(buf),&done) && done==sizeof(buf));
        uint64_t elapsed=ns(CLOCK_THREAD_CPUTIME_ID)-start; total+=elapsed; if (elapsed>max) max=elapsed;
        uint64_t reads=fat_probes-probes, changes=fat_updates-updates;
        /* Each FAT sector update has two preserving reads (set_cluster +
         * fat_bytes); later appends also inspect the existing tail's EOC.
         * All remaining FAT reads are the actual allocation probes. */
        CHECK(reads>=2*changes+(i!=0));
        uint64_t scan=reads-2*changes-(i!=0); CHECK(scan==48);
        CHECK(v.next_free==hint+16 && m.directory_writes-dw==1 && m.hint_writes-hw==1);
        if (i==0 || i==159) printf("AUDIT append=%u cluster_scan=%llu fat_cache_reads=%llu fat_cache_updates=%llu fat_physical_sectors=%llu directory_sectors=%llu fsinfo_sectors=%llu barriers=%llu cpu_us=%llu\n",
            i,(unsigned long long)scan,(unsigned long long)reads,(unsigned long long)changes,
            (unsigned long long)(m.fat_writes-fw),(unsigned long long)(m.directory_writes-dw),
            (unsigned long long)(m.hint_writes-hw),(unsigned long long)(m.flushes-fl),(unsigned long long)(elapsed/1000));
    }
    printf("AUDIT 512MiB FAT32 spc=8 next_free=validated appends=160 mean_cpu_us=%llu max_cpu_us=%llu\n",
        (unsigned long long)(total/160/1000),(unsigned long long)(max/1000));
#ifdef F182_AFTER
    CHECK(max<=1000000);
#endif
    finish(&m,&c);
}
struct trial {
    struct disk disk; struct block_cache cache; struct fat_volume volume; struct vfs vfs;
    struct vfs_table copy,reader;
    int out,in; atomic_bool stop; uint64_t start,reader_max; unsigned reads;
};
static void *reader(void *arg) {
    struct trial *t=arg; uint8_t buf[16384]; unsigned pos=0; uint64_t next=t->start;
    while (!atomic_load(&t->stop)) {
        next+=20000000; until(next);
        size_t done; uint64_t p,start=ns(CLOCK_MONOTONIC);
        CHECK(!vfs_seek(&t->reader,t->in,pos,VFS_SEEK_SET,&p));
        CHECK(!vfs_read(&t->reader,t->in,buf,sizeof(buf),&done) && done==sizeof(buf));
        uint64_t elapsed=ns(CLOCK_MONOTONIC)-start; if (elapsed>t->reader_max) t->reader_max=elapsed;
        for (unsigned i=0;i<sizeof(buf);i++) CHECK(buf[i]==0x97);
        pos=(pos+sizeof(buf))%READER_BYTES; t->reads++;
    }
    return 0;
}
#ifdef F182_AFTER
static void *writer(void *arg) {
    struct trial *t=arg;
    while (!atomic_load(&t->stop)) {
        cache_writer_wait(&t->cache,10);
        CHECK(!cache_writeback_tick(&t->cache,fs_now_ms()));
    }
    return 0;
}
#endif
static void replay(void) {
    struct trial *t=calloc(1,sizeof(*t)); CHECK(t); init(&t->disk,&t->cache,&t->volume);
    vfs_init(&t->vfs); CHECK(!vfs_attach(&t->vfs,2,&t->volume));
    CHECK(!vfs_table_init(&t->vfs,&t->copy,2) && !vfs_table_init(&t->vfs,&t->reader,2));
    t->out=vfs_open(&t->copy,"C:/COPY.BIN",VFS_WRITE,VFS_DENY_NONE,0);
    t->in=vfs_open(&t->reader,"C:/AUDIO.BIN",VFS_READ,VFS_DENY_NONE,0); CHECK(t->out>=0 && t->in>=0);
    uint8_t buf[CHUNK]; memset(buf,0x58,sizeof(buf)); pthread_t r;
#ifdef F182_AFTER
    t->cache.writer_online=true; pthread_t w; CHECK(!pthread_create(&w,0,writer,t));
#endif
    t->disk.timed=true; t->start=ns(CLOCK_MONOTONIC); CHECK(!pthread_create(&r,0,reader,t));
    for (unsigned pos=0;pos<COPY_BYTES;pos+=CHUNK) {
        /* Source has produced exactly 1 MiB/s since start; a slow consumer
         * may catch up from its buffered source, never exceed that rate. */
        until(t->start+(uint64_t)(pos+CHUNK)*1000000000/(1024*1024));
        size_t done; int e=vfs_write(&t->copy,t->out,buf,sizeof(buf),&done);
        if (e) fprintf(stderr,"copy write error=%d done=%zu readonly=%u reason=%s\n",e,done,t->volume.readonly,fat_readonly_reason(&t->volume));
        CHECK(!e && done==sizeof(buf));
    }
    uint64_t elapsed=ns(CLOCK_MONOTONIC)-t->start; atomic_store(&t->stop,true); CHECK(!pthread_join(r,0));
#ifdef F182_AFTER
    CHECK(!pthread_join(w,0)); t->cache.writer_online=false;
#endif
    t->disk.timed=false; CHECK(!cache_barrier(&t->cache,&t->disk.dev));
    CHECK(!memcmp(t->disk.bytes+32*512,t->disk.bytes+(32+FAT_SECTORS)*512,FAT_SECTORS*512));
    CHECK(t->volume.next_free==FIRST_FREE+COPY_BYTES/4096);
    size_t payload=((uint64_t)DATA+(FIRST_FREE-2)*8)*512;
    CHECK(payload+COPY_BYTES<=t->disk.stored);
    for (unsigned i=0;i<COPY_BYTES;i++) CHECK(t->disk.bytes[payload+i]==0x58);
    printf("REPLAY copy_bytes=%u source_Bps=1048576 destination_Bps=20000000 command_us=1000 elapsed_ms=%llu copy_Bps=%llu reader_calls=%u reader_max_us=%llu write_commands=%llu\n",
        COPY_BYTES,(unsigned long long)(elapsed/1000000),(unsigned long long)((uint64_t)COPY_BYTES*1000000000/elapsed),
        t->reads,(unsigned long long)(t->reader_max/1000),(unsigned long long)t->disk.commands);
#ifdef F182_AFTER
    CHECK(t->reader_max<=20000000 && elapsed>=9000000000ull && elapsed<=11000000000ull);
    /* f1-89 Amendment 1 item 1: data chunks of one C2 group are published
     * without a barrier and merge to 32 KiB again (1278 commands before
     * f1-89; the per-8-KiB-barrier engine issued 2238). */
    CHECK(t->disk.commands<=1300);
#else
    CHECK(t->reader_max>=100000000);
#endif
    CHECK(!vfs_close(&t->copy,t->out) && !vfs_close(&t->reader,t->in));
    vfs_table_destroy(&t->copy); vfs_table_destroy(&t->reader); vfs_destroy(&t->vfs);
    finish(&t->disk,&t->cache); free(t);
}
int main(void) { audit(); replay(); CHECK(!fs_host_contig_live); puts("PASS F1-82 allocation audit and copy/reader replay"); return 0; }
