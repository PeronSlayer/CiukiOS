/* C3 production FAT/VFS/cache/request replay. Reuse the audited FAT32
 * fixture; the negative reader explicitly uses the former volume ticket.
 * SPDX-License-Identifier: GPL-2.0-only */
#define main f182_fixture_main
#include "test_writeback.c"
#undef main
#include <fcntl.h>
#include <unistd.h>
void f183_usb_source_open(void) __attribute__((weak));
int f183_usb_source_read(uint64_t,uint32_t,void *) __attribute__((weak));
void f183_usb_source_close(void) __attribute__((weak));
struct fairness {
    struct trial t;
    struct blk_queue queue;
    struct vfs_table sync_table;
    bool old, fragmented, dirty, sync, mixed, error, cancel;
    uint64_t missed, latency, calls;
};
static int fair_flush(struct blkdev *d) {
    struct disk *m=d->ctx;
    if (m->timed) until(ns(CLOCK_MONOTONIC)+2000000); /* qualified model: 2ms <=5ms */
    return flush_disk(d);
}
static int fair_read(struct blkdev *d,uint64_t lba,uint32_t n,void *buf) {
    if (lba==TOTAL-1) return -FS_EIO; /* one independent failed read, never retried */
    return read_disk(d,lba,n,buf);
}
static void *fair_reader(void *arg) {
    struct fairness *f=arg; struct trial *t=&f->t;
    uint8_t buf[16384]; unsigned pos=0; uint64_t next=t->start;
    while (!atomic_load(&t->stop)) {
        next+=20000000; until(next); size_t done; uint64_t p;
        CHECK(!vfs_seek(&t->reader,t->in,pos,VFS_SEEK_SET,&p));
        if (f->old) {
            vfs_lock(&t->vfs,1u<<2);
            struct vfs_description *d=t->reader.handles[t->in];
            CHECK(!VFS_IO(&t->vfs,fat_read(d->node->volume,&d->node->entry,d->position,buf,sizeof(buf),&done)));
            d->position+=done; vfs_unlock(&t->vfs);
        } else CHECK(!vfs_read(&t->reader,t->in,buf,sizeof(buf),&done));
        uint64_t latency=ns(CLOCK_MONOTONIC)-next;
        if (latency>f->latency) f->latency=latency;
        if (latency>20000000) f->missed++;
        CHECK(done==sizeof(buf)); for (unsigned i=0;i<sizeof(buf);i++) CHECK(buf[i]==0x97);
        pos=(pos+sizeof(buf))%READER_BYTES; f->calls++;
    }
    return 0;
}
static void *fair_sync(void *arg) {
    struct fairness *f=arg;
    while (!atomic_load(&f->t.stop)) {
        until(ns(CLOCK_MONOTONIC)+37000000);
        CHECK(!vfs_commit(&f->sync_table,0));
    }
    return 0;
}
static void fragment(struct trial *t) {
    struct disk *m=&t->disk;
    size_t old=m->stored; m->stored+=1024*1024+COPY_BYTES; m->bytes=realloc(m->bytes,m->stored); CHECK(m->bytes);
    memset(m->bytes+old,0,m->stored-old);
    for (unsigned f=0;f<2;f++) {
        uint8_t *fat=m->bytes+(32+f*FAT_SECTORS)*512;
        for (unsigned c=3;c<FIRST_FREE;c++) fs_wr32(fat+c*4,0);
        for (unsigned c=0;c<READER_BYTES/4096;c++) {
            unsigned cluster=3+2*c;
            fs_wr32(fat+cluster*4,c+1<READER_BYTES/4096 ? cluster+2 : 0x0fffffff);
            memset(m->bytes+(DATA+(cluster-2)*8)*512,0x97,4096);
        }
    }
    t->volume.next_free=3+2*(READER_BYTES/4096);
    /* A valid filler file occupies alternating future clusters, forcing the
     * copy itself to fragment as well as the periodic reader's chain. */
    uint32_t filler=t->volume.next_free+1;
    for (unsigned f=0;f<2;f++) {
        uint8_t *fat=m->bytes+(32+f*FAT_SECTORS)*512;
        for (unsigned i=0;i<COPY_BYTES/4096;i++) {
            uint32_t cluster=filler+2*i;
            fs_wr32(fat+cluster*4,i+1<COPY_BYTES/4096 ? cluster+2 : 0x0fffffff);
        }
    }
    uint8_t *entry=m->bytes+DATA*512+64;
    memcpy(entry,"FILLER  BIN",11); entry[11]=FAT_ATTR_ARCHIVE;
    fs_wr16(entry+26,(uint16_t)filler); fs_wr32(entry+28,COPY_BYTES);
    t->volume.free_clusters-=COPY_BYTES/4096;
}
struct source {
    struct blkdev dev; struct blk_queue queue;
    pthread_mutex_t lock; pthread_cond_t changed;
    unsigned ready, consumed; uint8_t *buffers[2];
};
static int source_read(struct blkdev *d,uint64_t lba,uint32_t n,void *buf) {
    (void)d; (void)lba; CHECK(n<=128);
    uint64_t ready=ns(CLOCK_MONOTONIC)+(uint64_t)n*512*1000000000/1048576;
    int e=0;
    if (f183_usb_source_read) e=f183_usb_source_read(lba*512,n*512,buf);
    else memset(buf,0x58,n*512);
    until(ready); return e;
}
static void *produce(void *arg) {
    struct source *s=arg;
    for (unsigned chunk=0;chunk<10;chunk++) {
        pthread_mutex_lock(&s->lock);
        while (chunk>=s->consumed+2) pthread_cond_wait(&s->changed,&s->lock);
        pthread_mutex_unlock(&s->lock);
        CHECK(!blkdev_read(&s->dev,(uint64_t)chunk*2048,2048,s->buffers[chunk%2]));
        pthread_mutex_lock(&s->lock); s->ready++; pthread_cond_broadcast(&s->changed); pthread_mutex_unlock(&s->lock);
    }
    return 0;
}
static void independent_oracle(struct trial *t) {
    /* Microsoft FAT specification §§3/5: finish the virtual disk's BPB and
     * backup headers for independent tools. Only accessed bytes are written;
     * the temporary 512 MiB host fixture is sparse and removed immediately. */
    struct disk *m=&t->disk; uint8_t *b=m->bytes;
    memset(b,0,512); b[0]=0xeb; b[1]=0x58; b[2]=0x90; memcpy(b+3,"CIUKIOS ",8);
    fs_wr16(b+11,512); b[13]=8; fs_wr16(b+14,32); b[16]=2; b[21]=0xf8;
    fs_wr16(b+24,63); fs_wr16(b+26,255); fs_wr32(b+32,TOTAL); fs_wr32(b+36,FAT_SECTORS);
    fs_wr32(b+44,2); fs_wr16(b+48,1); fs_wr16(b+50,6);
    b[64]=0x80; b[66]=0x29; fs_wr32(b+67,0xc183); memcpy(b+71,"NO NAME    ",11);
    memcpy(b+82,"FAT32   ",8); fs_wr16(b+510,0xaa55);
    memcpy(b+6*512,b,512); memcpy(b+7*512,b+512,512);
    const char *image="build/host/fs/fairness-work.img",*copy="build/host/fs/fairness-copy.bin";
    int fd=open(image,O_CREAT|O_TRUNC|O_RDWR,0600); CHECK(fd>=0);
    CHECK(!ftruncate(fd,(off_t)TOTAL*512));
    CHECK(pwrite(fd,m->bytes,m->stored,0)==(ssize_t)m->stored && !fsync(fd) && !close(fd));
    fflush(stdout); CHECK(!system("fsck.fat -n build/host/fs/fairness-work.img"));
    (void)unlink(copy);
    CHECK(!system("mcopy -i build/host/fs/fairness-work.img ::COPY.BIN build/host/fs/fairness-copy.bin"));
    FILE *file=fopen(copy,"rb"); CHECK(file); uint8_t buffer[65536]; size_t total=0,take;
    while ((take=fread(buffer,1,sizeof(buffer),file))) {
        for (size_t i=0;i<take;i++) CHECK(buffer[i]==0x58);
        total+=take;
    }
    CHECK(!ferror(file) && total==COPY_BYTES && !fclose(file));
    CHECK(!unlink(copy) && !unlink(image));
    puts("C3 independent oracle fsck=clean mtools_copy_bytes=10485760 payload=match temporary_fixture=removed");
}
static void fair_replay(const char *name) {
    struct fairness *f=calloc(1,sizeof(*f)); CHECK(f);
    f->old=!strcmp(name,"old-volume-ticket") || !strncmp(name,"old/",4);
    const char *variant=!strncmp(name,"old/",4) ? name+4 : name;
    f->fragmented=!strcmp(variant,"fragmented");
    f->dirty=!strcmp(variant,"all-dirty"); f->sync=!strcmp(variant,"concurrent-fsync");
    f->mixed=(!strcmp(variant,"mixed-source-128-sector") || !strcmp(variant,"mixed-USB-source")); f->error=!strcmp(variant,"read-error"); f->cancel=!strcmp(variant,"cancellation");
    struct trial *t=&f->t; init(&t->disk,&t->cache,&t->volume);
    if (f->fragmented) fragment(t);
    uint32_t first=t->volume.next_free;
    blk_queue_init(&f->queue,"fair-destination",65536);
    t->disk.dev.queue=&f->queue; t->disk.dev.read=fair_read; t->disk.dev.flush=fair_flush;
    vfs_init(&t->vfs); CHECK(!vfs_attach(&t->vfs,2,&t->volume));
    CHECK(!vfs_table_init(&t->vfs,&t->copy,2) && !vfs_table_init(&t->vfs,&t->reader,2));
    t->out=vfs_open(&t->copy,"C:/COPY.BIN",VFS_WRITE,VFS_DENY_NONE,0);
    t->in=vfs_open(&t->reader,"C:/AUDIO.BIN",VFS_READ,VFS_DENY_NONE,0); CHECK(t->out>=0 && t->in>=0);
    if (f->sync) {
        CHECK(!vfs_table_init(&t->vfs,&f->sync_table,2));
        CHECK(vfs_open(&f->sync_table,"C:/COPY.BIN",VFS_READ,VFS_DENY_NONE,0)==0);
    }
    if (f->dirty) {
        size_t stored=t->disk.stored; t->disk.stored+=CACHE_DEFAULT_BYTES;
        t->disk.bytes=realloc(t->disk.bytes,t->disk.stored); CHECK(t->disk.bytes);
        memset(t->disk.bytes+stored,0,t->disk.stored-stored);
        uint8_t sector[512]; memset(sector,0x41,sizeof(sector));
        for (unsigned i=0;i<t->cache.blocks;i++) CHECK(!cache_write(&t->cache,&t->disk.dev,t->disk.stored/512-t->cache.blocks-1+i,sector));
        CHECK(t->cache.dirty_bytes); /* zero clean slots at admission */
    }
    uint8_t *buf=malloc(1024*1024); CHECK(buf); memset(buf,0x58,1024*1024);
    struct source source={0}; pthread_t producer;
    if (f->mixed) {
        if (f183_usb_source_open) f183_usb_source_open();
        blk_queue_init(&source.queue,"source-128-sector",128*512);
        source.dev=(struct blkdev){.read=source_read,.capacity=COPY_BYTES/512,.sector_size=512,.queue=&source.queue};
        CHECK(!pthread_mutex_init(&source.lock,0) && !pthread_cond_init(&source.changed,0));
        source.buffers[0]=malloc(1024*1024); source.buffers[1]=malloc(1024*1024); CHECK(source.buffers[0] && source.buffers[1]);
    }
    t->cache.writer_online=true; pthread_t r,w,sync;
    CHECK(!pthread_create(&w,0,writer,t));
    t->disk.timed=true; t->start=ns(CLOCK_MONOTONIC);
    CHECK(!pthread_create(&r,0,fair_reader,f));
    if (f->sync) CHECK(!pthread_create(&sync,0,fair_sync,f));
    if (f->mixed) CHECK(!pthread_create(&producer,0,produce,&source));
    for (unsigned chunk=0;chunk<10;chunk++) {
        const uint8_t *data=buf;
        if (f->mixed) {
            pthread_mutex_lock(&source.lock);
            while (source.ready<=chunk) pthread_cond_wait(&source.changed,&source.lock);
            data=source.buffers[chunk%2]; pthread_mutex_unlock(&source.lock);
        } else until(t->start+(uint64_t)(chunk+1)*1000000000);
        if (chunk==5 && (f->error || f->cancel)) {
            uint8_t spare[512];
            if (f->error) CHECK(blkdev_read(&t->disk.dev,TOTAL-1,1,spare)==-FS_EIO);
            else {
                struct blk_request *request=blk_request_prepare(&t->disk.dev,BLK_READ,TOTAL-2,512,spare,BLK_SPECULATIVE,0,0,30000);
                CHECK(request && !blk_request_submit(request) && blk_request_cancel(request));
                CHECK(blk_request_wait(request)==-FS_EAGAIN && !blk_request_release(request));
            }
        }
        size_t done;
        if (f->old) {
            /* Preserve BOTH sides of the old whole-operation negative
             * control after f1-89 moves public writes to transactions. */
            vfs_lock(&t->vfs,1u<<2);
            struct vfs_description *d=t->copy.handles[t->out];
            CHECK(!VFS_IO(&t->vfs,fat_write(d->node->volume,&d->node->entry,d->position,data,1024*1024,&done)));
            d->position+=done; vfs_unlock(&t->vfs);
        } else CHECK(!vfs_write(&t->copy,t->out,data,1024*1024,&done));
        CHECK(done==1024*1024);
        if (f->mixed) {
            pthread_mutex_lock(&source.lock); source.consumed++; pthread_cond_broadcast(&source.changed); pthread_mutex_unlock(&source.lock);
        }
    }
    int commit_error=vfs_commit(&t->copy,t->out);
    if (commit_error) fprintf(stderr,"variant=%s final commit error=%d\n",name,commit_error);
    CHECK(!commit_error); uint64_t elapsed=ns(CLOCK_MONOTONIC)-t->start;
    atomic_store(&t->stop,true); CHECK(!pthread_join(r,0) && !pthread_join(w,0));
    uint64_t expected=elapsed/20000000;
    if (f->calls<expected) f->missed+=expected-f->calls;
    if (f->sync) CHECK(!pthread_join(sync,0));
    t->cache.writer_online=false; t->disk.timed=false;
    if (f->mixed) {
        CHECK(!pthread_join(producer,0) && !blk_queue_destroy(&source.queue));
        CHECK(!pthread_mutex_destroy(&source.lock) && !pthread_cond_destroy(&source.changed));
        free(source.buffers[0]); free(source.buffers[1]);
        if (f183_usb_source_close) f183_usb_source_close();
    }
    CHECK(!cache_barrier(&t->cache,&t->disk.dev));
    CHECK(!memcmp(t->disk.bytes+32*512,t->disk.bytes+(32+FAT_SECTORS)*512,FAT_SECTORS*512));
    uint32_t cluster=t->copy.handles[t->out]->node->entry.first; size_t checked=0;
    CHECK(t->copy.handles[t->out]->node->entry.size==COPY_BYTES);
    while (checked<COPY_BYTES) {
        CHECK(cluster>=first && cluster<t->volume.clusters+2);
        size_t offset=((uint64_t)DATA+(cluster-2)*8)*512; CHECK(offset+4096<=t->disk.stored);
        for (unsigned i=0;i<4096;i++) CHECK(t->disk.bytes[offset+i]==0x58);
        checked+=4096; cluster=fs_rd32(t->disk.bytes+32*512+cluster*4)&0xfffffff;
    }
    CHECK(cluster>=0xffffff8);
    independent_oracle(t);
    uint64_t throughput=(uint64_t)COPY_BYTES*1000000000/elapsed;
    printf("C3 variant=%s copy_bytes=%u source_Bps=1048576 destination_Bps=20000000 seek_us=0 read_setup_us=0 command_us=1000 flush_us=2000 copy_Bps=%llu reader_calls=%llu max_arrival_us=%llu missed=%llu durable=match\n",
        name,COPY_BYTES,(unsigned long long)throughput,(unsigned long long)f->calls,
        (unsigned long long)(f->latency/1000),(unsigned long long)f->missed); fflush(stdout);
    if (f->old) CHECK(f->latency>=100000000);
    else CHECK(f->latency<=20000000 && !f->missed && f->calls>=expected);
    CHECK(throughput>=943718 && throughput<=1153433);
    if (f->sync) vfs_table_destroy(&f->sync_table);
    CHECK(!vfs_close(&t->copy,t->out) && !vfs_close(&t->reader,t->in));
    vfs_table_destroy(&t->copy); vfs_table_destroy(&t->reader); vfs_destroy(&t->vfs);
    CHECK(!blk_queue_destroy(&f->queue)); finish(&t->disk,&t->cache); free(buf); free(f);
}
int main(int argc,char **argv) {
    if (argc==2) fair_replay(argv[1]);
    else {
        const char *names[]={"cold-hot","fragmented","all-dirty","mixed-USB-source","concurrent-fsync","read-error","cancellation"};
        for (unsigned i=0;i<sizeof(names)/sizeof(names[0]);i++) {
            char before[80]; snprintf(before,sizeof(before),"old/%s",names[i]);
            fair_replay(before); fair_replay(names[i]);
        }
    }
    CHECK(!fs_host_contig_live); puts("PASS C3 production VFS/FAT/cache/controller queue replay"); return 0;
}
