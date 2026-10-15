/* Virtual 4/32 GiB layouts; generate sparse sectors, never allocate/copy a
 * whole image. Production FAT code performs every scan/read/write. */
#include "fat.h"
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#define EOC 0x0fffffffu
struct media {
    struct blkdev dev;
    uint32_t total, fat, data, clusters, far;
    unsigned spc, reads, max_read, writes, flushes, saved;
    bool dirty, diff, cross, cycle, orphan, bad, fail_read, cross_high, live_diff;
    struct { uint64_t lba;uint8_t bytes[512]; } changes[64];
};
static char memory_record[256];
void klog(const char *fmt,...) {
    if (!strstr(fmt,"memory")) return;
    va_list ap;va_start(ap,fmt);vsnprintf(memory_record,sizeof(memory_record),fmt,ap);va_end(ap);
}
static void put_fat(struct media *m,uint8_t *buf,uint64_t lba,uint32_t n,unsigned copy,uint32_t c,uint32_t val) {
    uint64_t sec=32+(uint64_t)copy*m->fat+c/128;
    if (sec>=lba && sec-lba<n) fs_wr32(buf+(sec-lba)*512+(c%128)*4,val);
}
static int read_blocks(struct blkdev *d,uint64_t lba,uint32_t n,void *out) {
    struct media *m=d->ctx;assert(!blkdev_range(d,lba,n) && n<=128);
    if (m->fail_read) return -FS_EIO;
    m->reads++;if (n>m->max_read) m->max_read=n;
    uint8_t *buf=out;memset(buf,0,n*512);
    for (unsigned i=0;i<n;i++) {
        uint8_t *b=buf+i*512;uint64_t sec=lba+i;
        if (sec==0 || sec==6) {
            b[0]=0xeb;b[1]=0x58;b[2]=0x90;fs_wr16(b+11,512);b[13]=(uint8_t)m->spc;
            fs_wr16(b+14,32);b[16]=2;b[21]=0xf8;fs_wr32(b+32,m->total);
            fs_wr32(b+36,m->fat);fs_wr32(b+44,2);fs_wr16(b+48,1);fs_wr16(b+50,6);fs_wr16(b+510,0xaa55);
        } else if (sec==1) {
            fs_wr32(b,0x41615252);fs_wr32(b+484,0x61417272);fs_wr32(b+508,0xaa550000);
            /* Deliberately false hints: no used-extent/free-count authority. */
            fs_wr32(b+488,0);fs_wr32(b+492,2);
        } else if (sec==m->data) {
            uint32_t first=m->cross_high ? m->far : 3;
            memcpy(b,"FIRST   BIN",11);b[11]=FAT_ATTR_ARCHIVE;fs_wr16(b+26,(uint16_t)first);fs_wr16(b+20,(uint16_t)(first>>16));fs_wr32(b+28,512);
            if (m->cross || m->cross_high) {
                memcpy(b+32,"SECOND  BIN",11);b[43]=FAT_ATTR_ARCHIVE;fs_wr16(b+58,(uint16_t)first);
                fs_wr16(b+52,(uint16_t)(first>>16));fs_wr32(b+60,512);
            }
        } else if (sec==m->data+m->spc) memset(b,0x5a,512);
    }
    for (unsigned f=0;f<2;f++) {
        put_fat(m,buf,lba,n,f,0,0x0ffffff8);put_fat(m,buf,lba,n,f,1,m->dirty ? EOC&~0x08000000u : EOC);
        put_fat(m,buf,lba,n,f,2,EOC);put_fat(m,buf,lba,n,f,3,m->cycle ? 4 : EOC);
        if (m->cycle) put_fat(m,buf,lba,n,f,4,3);
        if (m->orphan || (m->diff && f==1)) put_fat(m,buf,lba,n,f,m->far,EOC);
        if (m->bad) put_fat(m,buf,lba,n,f,m->far,0x0ffffff7);
        if (m->cross_high) put_fat(m,buf,lba,n,f,m->far,EOC);
        if (m->live_diff && f==1) put_fat(m,buf,lba,n,f,3,4);
    }
    for (unsigned j=0;j<m->saved;j++) if (m->changes[j].lba>=lba && m->changes[j].lba-lba<n)
        memcpy(buf+(m->changes[j].lba-lba)*512,m->changes[j].bytes,512);
    return 0;
}
static int write_blocks(struct blkdev *d,uint64_t lba,uint32_t n,const void *buf) {
    struct media *m=d->ctx;assert(!blkdev_range(d,lba,n) && n==1);m->writes++;
    unsigned i=0;for (;i<m->saved;i++) if (m->changes[i].lba==lba) break;
    assert(i<64);if (i==m->saved) m->saved++;
    m->changes[i].lba=lba;memcpy(m->changes[i].bytes,buf,512);return 0;
}
static int flush_blocks(struct blkdev *d) { ((struct media *)d->ctx)->flushes++;return 0; }
static void init(struct media *m,unsigned gib,unsigned spc) {
    memset(m,0,sizeof(*m));m->total=gib*2097152u;m->spc=spc;
    m->fat=((m->total/spc+2)*4u+511)/512;m->data=32+2*m->fat;
    m->clusters=(m->total-m->data)/spc;m->far=m->clusters-3;
    m->dev=(struct blkdev){.read=read_blocks,.write=write_blocks,.flush=flush_blocks,
        .capacity=m->total,.sector_size=512,.write_cache_state=BLKDEV_CACHE_ENABLED,.ctx=m};
    memory_record[0]=0;
}
static void readable(struct fat_volume *v) {
    struct fat_entry e;uint8_t bytes[512];size_t done;
    assert(!fat_lookup(v,2,"FIRST.BIN",&e));
    assert(!fat_read(v,&e,0,bytes,sizeof(bytes),&done) && done==512);
    for (unsigned i=0;i<512;i++) assert(bytes[i]==0x5a);
}
static void layout(unsigned gib,unsigned spc,bool dirty) {
    struct media m;struct block_cache cache;struct fat_volume v;
    init(&m,gib,spc);m.dirty=dirty;m.orphan=dirty;
    assert(!cache_init(&cache,128*1024));uint32_t start=fs_now_ms();
    assert(!fat_prepare(&v,&cache,&m.dev,0,m.dev.capacity));
    assert(v.mounted && v.scan_complete && !m.writes && !m.flushes && !v.prepare_cache);
    if (dirty) assert(v.scan_needed_bytes==(v.scan_extent+7)/8 && v.scan_available_bytes<=1024*1024);
    else assert(v.scan_fast && !v.scan_passes && v.free_clusters==v.clusters-2);
    assert(!fat_enable_write(&v) && !v.readonly && !v.prepare_cache);
    if (dirty) { uint32_t n;assert(v.dirty_recovered && v.repaired_lost==1);assert(!fat_get_cluster(&v,m.far,&n) && !n); }
    assert(m.max_read==128);readable(&v);
    printf("PASS F1-70 %u GiB FAT32 spc=%u dirty=%u: clusters=%u bitmap=%u available=%u passes=%u reads=%u max=128 writes=%u host_ms=%u mount_ms=%u repair_ms=%u\n",
        gib,spc,dirty,v.clusters,v.scan_needed_bytes,v.scan_available_bytes,v.scan_passes,m.reads,m.writes,fs_now_ms()-start,v.mount_ms,v.repair_ms);
    assert(!fat_unmount(&v));cache_destroy(&cache);assert(!fs_host_contig_live);
}
static void fallback(void) {
    struct media m;struct block_cache cache;struct fat_volume v;
    init(&m,4,8);m.dirty=m.orphan=true;
    assert(!cache_init(&cache,16384));fs_host_page_fail=1;
    assert(!fat_prepare(&v,&cache,&m.dev,0,m.dev.capacity));
    assert(v.scan_passes>1 && v.scan_available_bytes==4096 && !m.writes);
    assert(strstr(memory_record,"needed_bytes=") && strstr(memory_record,"available_bytes=4096"));
    fs_host_page_fail=1;assert(!fat_enable_write(&v) && !v.readonly && v.repaired_lost==1 && v.scan_passes>1);
    assert(!cache.workspace_in_use);readable(&v);assert(!fat_unmount(&v));cache_destroy(&cache);
    puts("PASS F1-70 bitmap allocation failure: 4 KiB reserved fallback, multiple complete passes, writable repair, needed/available byte record, reservation released");
    init(&m,4,8);m.dirty=true;assert(!cache_init(&cache,16384));
    assert(!cache_workspace_begin(&cache,4096));fs_host_page_fail=1;
    assert(fat_prepare(&v,&cache,&m.dev,0,m.dev.capacity)==-FS_EBUSY && !m.writes && !v.mounted);
    assert(strstr(memory_record,"available_bytes=0") && strstr(memory_record,"error=-16"));
    cache_workspace_end(&cache);cache_destroy(&cache);assert(!fs_host_contig_live);
    puts("PASS F1-70 exhausted fallback: exact memory reason, no writes/publication and private windows released");
    init(&m,4,8);m.dirty=m.orphan=m.cross_high=true;assert(!cache_init(&cache,16384));
    fs_host_page_fail=1;assert(!fat_prepare(&v,&cache,&m.dev,0,m.dev.capacity));
    assert(v.scan_passes>1 && v.ro_reasons&FAT_RO_CORRUPT && !v.scan_complete);
    assert(fat_enable_write(&v)==-FS_EROFS && !m.writes && !m.flushes);
    cache_destroy(&cache);assert(!fs_host_contig_live);
    puts("PASS F1-70 late-pass cross-link: all earlier passes complete, no orphan reclamation or dirty-flag writes before final ownership proof");
}
static void integrity(void) {
    for (unsigned mode=0;mode<6;mode++) {
        struct media m;struct block_cache cache;struct fat_volume v;
        init(&m,4,64);m.dirty=true;
        if (mode==0) m.cross=true;
        if (mode==1) m.cycle=true;
        if (mode==2) m.diff=true;
        if (mode==3) m.bad=true;
        if (mode==4) m.fail_read=true;
        if (mode==5) m.live_diff=true;
        assert(!cache_init(&cache,16384));
        int e=fat_prepare(&v,&cache,&m.dev,0,m.dev.capacity);
        assert(!m.writes && !m.flushes);
        if (mode<2) assert(!e && v.readonly && v.ro_reasons&FAT_RO_CORRUPT && fat_enable_write(&v)==-FS_EROFS && !m.writes);
        if (mode==2) assert(!e && v.lost_clusters==1 && !fat_enable_write(&v) && !v.readonly && v.repaired_copies==1);
        if (mode==3) { uint32_t n;assert(!e && !fat_enable_write(&v) && !fat_get_cluster(&v,m.far,&n) && n==0x0ffffff7); }
        if (mode==4) assert(e==-FS_EIO && !v.mounted);
        if (mode==5) { assert(!e && v.ro_reasons&FAT_RO_COPIES && fat_enable_write(&v)==-FS_EROFS && !m.writes);readable(&v); }
        cache_destroy(&cache);assert(!cache.workspace_in_use && !fs_host_contig_live);
    }
    puts("PASS F1-70 integrity: cross-link/cycle/live-copy disagreement refuse writes, RO file remains readable, high unowned copy difference repaired, bad cluster preserved, read failure not retried");
}
int main(void) {
    layout(4,64,false);layout(4,8,true);layout(32,8,true);layout(32,1,true);
    fallback();integrity();return 0;
}
