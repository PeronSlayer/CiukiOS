/* Production FAT/cache run reads over prequalified synthetic geometry.
 * The existing fixture suite independently mounts FAT12/16/32 images and
 * retains its write/crash proofs. SPDX-License-Identifier: GPL-2.0-only */
#include "fat.h"
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while (0)
#define FILE_BYTES (1024u*1024u)
#ifdef ATA_DMA_THROUGHPUT
int ata_host_dma_io(uint8_t *, uint32_t, uint64_t, uint32_t, void *, bool);
#endif
static uint64_t disk_requests, disk_bytes;
static uint32_t disk_max;
void proc_launch_disk_read(uint32_t bytes) {
    disk_requests++; disk_bytes+=bytes; if (bytes>disk_max) disk_max=bytes;
}
static bool cancel;
bool file_read_cancelled(void) { return cancel; }
struct media {
    struct blkdev dev;
    uint8_t *bytes;
    uint64_t calls, writes, flushes, data_calls;
    uint32_t data_start, max_count;
    bool fail, change, cancel_after_read;
    struct { uint64_t lba; uint32_t n; } trace[4096];
};
static int read_blocks(struct blkdev *dev, uint64_t lba, uint32_t n, void *buf) {
    struct media *m=dev->ctx;
    int e=blkdev_range(dev,lba,n); if (e) return e;
    CHECK(m->calls<4096 && n<=BLKDEV_READ_SECTORS);
    m->trace[m->calls].lba=lba; m->trace[m->calls++].n=n;
    if (lba>=m->data_start) m->data_calls++;
    if (n>m->max_count) m->max_count=n;
#ifdef ATA_DMA_THROUGHPUT
    CHECK(!ata_host_dma_io(m->bytes, (uint32_t)dev->capacity, lba, n, buf, false));
#else
    memcpy(buf,m->bytes+lba*512,n*512);
#endif
    if (m->change) dev->media_generation++;
    if (m->cancel_after_read) cancel=true;
    return m->fail ? -FS_EIO : 0;
}
static int write_blocks(struct blkdev *dev, uint64_t lba, uint32_t n, const void *buf) {
    struct media *m=dev->ctx; int e=blkdev_range(dev,lba,n); if (e) return e;
    CHECK(n==1); m->writes++;
#ifdef ATA_DMA_THROUGHPUT
    CHECK(!ata_host_dma_io(m->bytes, (uint32_t)dev->capacity, lba, n, (void *)buf, true));
#else
    memcpy(m->bytes+lba*512,buf,512);
#endif
    return 0;
}
static int flush_blocks(struct blkdev *dev) { ((struct media *)dev->ctx)->flushes++; return 0; }
static void init_media(struct media *m, uint32_t sectors) {
    memset(m,0,sizeof(*m)); m->bytes=malloc((size_t)sectors*512); CHECK(m->bytes);
    for (uint32_t l=0;l<sectors;l++) for (unsigned i=0;i<512;i++) m->bytes[l*512+i]=(uint8_t)(l*13+i*7);
    m->dev=(struct blkdev){.read=read_blocks,.write=write_blocks,.flush=flush_blocks,
        .capacity=sectors,.sector_size=512,.write_cache_state=BLKDEV_CACHE_ENABLED,.ctx=m};
}
static void reset_counts(struct media *m) {
    m->calls=m->data_calls=0; m->max_count=0;
    disk_requests=disk_bytes=disk_max=0;
}
static void cache_runs(void) {
    struct media m; struct block_cache c; init_media(&m,512);
    uint8_t *buf=malloc(BLKDEV_READ_BYTES), hit[512], dirty[512]; CHECK(buf);
    CHECK(!cache_init(&c,8192)); /* only eight slots: hits must survive eviction */
    CHECK(!cache_read(&c,&m.dev,64,hit)); memset(dirty,0x93,512);
    CHECK(!cache_write(&c,&m.dev,65,dirty)); reset_counts(&m);
    CHECK(!cache_read_run(&c,&m.dev,0,128,buf));
    CHECK(m.calls==2 && m.trace[0].lba==0 && m.trace[0].n==64 && m.trace[1].lba==66 && m.trace[1].n==62);
    CHECK(!memcmp(buf,m.bytes,64*512) && !memcmp(buf+64*512,hit,512) && !memcmp(buf+65*512,dirty,512));
    CHECK(!memcmp(buf+66*512,m.bytes+66*512,62*512));
    CHECK(disk_requests==2 && disk_bytes==126*512 && disk_max==32768);
    CHECK(!cache_barrier(&c,&m.dev) && m.writes==1 && m.flushes==1);
    CHECK(!memcmp(m.bytes+65*512,dirty,512));
    cache_destroy(&c); CHECK(!cache_init(&c,131072));
    reset_counts(&m); CHECK(!cache_read_run(&c,&m.dev,128,128,buf) && m.calls==1 && disk_max==65536);
    CHECK(!cache_read_run(&c,&m.dev,128,128,buf) && m.calls==1); /* all hits */
    unsigned before=m.calls;
    CHECK(cache_read_run(&c,&m.dev,0,129,buf)==-FS_EINVAL && m.calls==before);
    CHECK(cache_read_run(&c,&m.dev,511,2,buf)==-FS_EINVAL && m.calls==before);
    cache_invalidate(&c,&m.dev,false); m.fail=true;
    CHECK(cache_read_run(&c,&m.dev,0,128,buf)==-FS_EIO);
    m.fail=false; before=m.calls;
    CHECK(!cache_read_run(&c,&m.dev,0,128,buf) && m.calls==before+1); /* failure published no hits */
    cache_invalidate(&c,&m.dev,false); m.change=true;
    CHECK(cache_read_run(&c,&m.dev,0,128,buf)==-FS_EIO);
    m.change=false; before=m.calls;
    CHECK(!cache_read_run(&c,&m.dev,0,128,buf) && m.calls==before+1);
    /* Cancellation after the first missing interval forbids a second issue. */
    cache_invalidate(&c,&m.dev,false); CHECK(!cache_read(&c,&m.dev,64,hit));
    reset_counts(&m); m.cancel_after_read=true;
    CHECK(cache_read_run(&c,&m.dev,0,128,buf)==-4 && m.calls==1);
    cancel=false; m.cancel_after_read=false;
    cache_destroy(&c); free(buf); free(m.bytes);
    puts("PASS cache run: clean/dirty middle hits, tiny-cache eviction, full hits, failures/generations, cancellation, range checks");
}
static void fat_runs(unsigned spc, unsigned type, bool fragmented) {
    struct media m; struct block_cache c;
    uint32_t cluster_bytes=spc*512, count=(FILE_BYTES+cluster_bytes-1)/cluster_bytes;
    uint32_t clusters=count+count/37*6+256, start=7, data_start=start+64;
    init_media(&m,data_start+clusters*spc); m.data_start=data_start;
    CHECK(!cache_init(&c,2*1024*1024));
    struct fat_volume v={.cache=&c,.dev=&m.dev,.start=start,.sectors=m.dev.capacity-start,
        .reserved=1,.fat_sectors=32,.data_sector=64,.clusters=clusters,.type=type,.spc=spc,.fats=1,.mounted=true};
    memset(m.bytes+(start+1)*512,0,32*512);
    uint32_t *chain=malloc(count*sizeof(*chain));
    uint8_t *expected=malloc(FILE_BYTES), *actual=malloc(FILE_BYTES+1024); CHECK(chain && expected && actual);
    uint32_t cluster=3;
    for (unsigned i=0;i<count;i++) {
        chain[i]=cluster;
        unsigned take=FILE_BYTES-i*cluster_bytes; if (take>cluster_bytes) take=cluster_bytes;
        memcpy(expected+i*cluster_bytes,m.bytes+(data_start+(cluster-2)*spc)*512,take);
        cluster+=fragmented && (i%37)==36 ? 7 : 1;
    }
    for (unsigned i=0;i<count;i++) {
        uint32_t next=i+1<count ? chain[i+1] : type==16 ? 0xffff : 0x0fffffff;
        if (type==16) fs_wr16(m.bytes+(start+1)*512+chain[i]*2,(uint16_t)next);
        else fs_wr32(m.bytes+(start+1)*512+chain[i]*4,next);
    }
    struct fat_entry file={.first=chain[0],.size=FILE_BYTES}; size_t done;
    reset_counts(&m);
    CHECK(!fat_read(&v,&file,0,actual,FILE_BYTES,&done) && done==FILE_BYTES && !memcmp(actual,expected,FILE_BYTES));
    if (!fragmented) CHECK(m.max_count==128 && disk_max==65536 && m.data_calls==16);
    else CHECK(m.max_count==(37*spc<128 ? 37*spc : 128));
    for (unsigned r=0;r<m.calls;r++) if (m.trace[r].lba>=data_start) {
        uint64_t first=m.trace[r].lba, last=first+m.trace[r].n;
        bool inside=false;
        for (unsigned i=0;i<count;i++) {
            uint64_t a=data_start+(chain[i]-2)*spc;
            if (first<a || first>=a+spc) continue;
            unsigned j=i;
            while (j+1<count && chain[j+1]==chain[j]+1) j++;
            CHECK(last<=data_start+(chain[j]-2)*spc+spc); inside=true; break;
        }
        CHECK(inside);
    }
    uint64_t calls=m.calls;
    CHECK(!fat_read(&v,&file,0,actual,FILE_BYTES,&done) && !memcmp(actual,expected,FILE_BYTES) && m.calls==calls);
    static const unsigned offsets[]={1,511,512,65535,65536,FILE_BYTES-513,FILE_BYTES-1,FILE_BYTES};
    file.size=FILE_BYTES-13;
    for (unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);i++) {
        unsigned off=offsets[i], size=off<file.size ? file.size-off : 0; if (size>65537) size=65537;
        cache_invalidate(&c,&m.dev,false); memset(actual,0xa5,FILE_BYTES+1024);
        CHECK(!fat_read(&v,&file,off,actual,65537,&done) && done==size && !memcmp(actual,expected+off,size));
        CHECK(actual[size]==0xa5);
    }
    /* Cache hits in a file run retain newer shared dirty bytes. */
    cache_invalidate(&c,&m.dev,false); uint8_t dirty[512]; memset(dirty,0xbc,512);
    uint64_t mid=data_start+(chain[64/spc]-2)*spc+64%spc;
    CHECK(!cache_write(&c,&m.dev,mid,dirty)); memcpy(expected+64*512,dirty,512);
    CHECK(!fat_read(&v,&file,0,actual,BLKDEV_READ_BYTES,&done) && done==BLKDEV_READ_BYTES && !memcmp(actual,expected,done));
    CHECK(!cache_barrier(&c,&m.dev));
    /* Reject a broken successor and keep the bounded-walk rule. */
    cache_invalidate(&c,&m.dev,false);
    if (type==16) fs_wr16(m.bytes+(start+1)*512+chain[0]*2,0);
    else fs_wr32(m.bytes+(start+1)*512+chain[0]*4,0);
    CHECK(fat_read(&v,&file,0,actual,cluster_bytes+512,&done)==-FS_EUCLEAN && v.readonly);
    printf("PASS FAT%u run spc=%u fragmented=%u: 1 MiB identical, EOF/partial/boundary/hit/corruption checks\n",type,spc,fragmented);
    cache_destroy(&c); free(chain); free(expected); free(actual); free(m.bytes);
}
int main(void) {
    cache_runs();
    for (unsigned type=16;type<=32;type*=2)
        for (unsigned spc=1;spc<=128;spc*=spc==1 ? 4 : 32)
            for (unsigned fragmented=0;fragmented<2;fragmented++) fat_runs(spc,type,fragmented!=0);
    CHECK(!fs_host_contig_live); return 0;
}
