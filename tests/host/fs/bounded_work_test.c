/* SPDX-License-Identifier: GPL-2.0-only */
/* Production FAT/cache loops with deterministic CPU accounting only. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cache.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"bounded-fs:%u %s\n",__LINE__,#x); abort(); } } while (0)
static uint64_t model_total,model_span,model_max;
static bool model_points,model_enabled;
static void (*model_interleave)(void);
void model_charge(uint64_t ns) { if (model_enabled) { model_total+=ns; model_span+=ns; } }
static void model_boundary(void) { if (model_span>model_max) model_max=model_span; model_span=0; }
void model_service(void) {
    if (model_enabled && model_points) { model_charge(200); model_boundary(); }
    if (model_interleave) { void (*fn)(void)=model_interleave; model_interleave=0; fn(); }
}
#define fs_service model_service
#ifdef BOUNDED_FAT
#include "bounded_fat.inc"
static uint8_t fat_data[(131072+128)*4];
static unsigned reads;
static int read_fat(struct blkdev *d,uint64_t l,uint32_t n,void *data) {
    (void)d; CHECK(l>=1 && l-1+n<=2050);
    for (unsigned i=0;i<n;i++) memcpy((uint8_t *)data+i*512,fat_data+((l-1+i)%1025)*512,512);
    reads++; return 0;
}
int main(void) {
    struct block_cache cache; CHECK(!cache_init(&cache,8u*1024*1024));
    struct blkdev d={.sector_size=512,.capacity=1048576,.read=read_fat};
    struct fat_volume v={.cache=&cache,.dev=&d,.type=32,.clusters=131072,.spc=8,
        .reserved=1,.fat_sectors=1025,.sectors=1048576,.fats=2};
    uint32_t random=0x44;
    uint32_t *order=malloc(131072*sizeof(*order)); CHECK(order);
    for (unsigned i=0;i<131072;i++) order[i]=i+2;
    for (unsigned i=131071;i;i--) {
        random=random*1664525u+1013904223u; unsigned at=random%(i+1);
        uint32_t swap=order[at]; order[at]=order[i]; order[i]=swap;
    }
    for (unsigned i=0;i<131072;i++) fs_wr32(fat_data+order[i]*4,i+1<131072 ? order[i+1] : 0x0fffffff);
    uint8_t sector[512];
    for (unsigned i=1;i<=2050;i++) CHECK(!cache_read(&cache,&d,i,sector));
    unsigned warm=reads;
    uint64_t before=0,total_before=0;
    for (unsigned site=0;site<3;site++) for (unsigned enabled=0;enabled<2;enabled++) {
        model_total=model_span=model_max=0; model_enabled=true; model_points=enabled;
        if (site==0) {
            uint32_t last=0; CHECK(!chain_at(&v,order[0],131071,&last) && last==order[131071]);
        } else if (site==1) {
            bool agree=true; CHECK(!scan_census(&v,false,&agree) && agree);
        } else {
            CHECK(!cache_workspace_begin(&cache,20480));
            struct fat_scan_state state={.workspace=true,.begin=0,.end=v.clusters+2};
            CHECK(!scan_chain(&v,&state,order[0],131072u*4096u,false));
            cache_workspace_end(&cache);
        }
        model_boundary(); model_enabled=false;
        CHECK(reads==warm); /* all random links came from the resident cache */
        if (!enabled) { before=model_max; total_before=model_total; }
        else {
            CHECK(model_max<=2000000 && model_total<=total_before*110/100);
            printf("F0-44 cached random FAT site=%u: clusters=131072 reads_after_warm=0 span=%llu->%lluns work=%llu->%lluns PASS\n",
                site,(unsigned long long)before,(unsigned long long)model_max,(unsigned long long)total_before,(unsigned long long)model_total);
        }
    }
    free(order); cache_destroy(&cache); return 0;
}
#else
static void *model_alloc(void) { model_charge(18000); return fs_page_alloc(); }
static void model_free(void *p) { model_charge(6000); fs_page_free(p); }
static void *model_clear(void *p,int v,size_t n) { model_charge(n*8000/4096); return memset(p,v,n); }
static void *model_copy(void *p,const void *from,size_t n) { model_charge(n*8000/4096); return memcpy(p,from,n); }
#define fs_page_alloc model_alloc
#define fs_page_free model_free
#define memset model_clear
#define memcpy model_copy
#include "bounded_cache.inc"
static unsigned write_sectors;
static int write_data(struct blkdev *d,uint64_t l,uint32_t n,const void *data) {
    (void)d;(void)l; CHECK(data && n<=64); write_sectors+=n;
    model_boundary(); return 0; /* transport drops cache lock */
}
static int read_data(struct blkdev *d,uint64_t l,uint32_t n,void *data) {
    (void)d;(void)l; memset(data,0x44,n*512); return 0;
}
static struct block_cache *interleaved_cache;
static struct blkdev *interleaved_dev;
static void invalidate_during_scan(void) {
    fs_lock_drop(&interleaved_cache->lock);
    cache_invalidate(interleaved_cache,interleaved_dev,true);
    interleaved_dev->media_generation++;
    fs_lock_take(&interleaved_cache->lock);
}
static void touch_during_scan(void) {
    fs_lock_drop(&interleaved_cache->lock);
    uint8_t data[512];
    /* Find an unreserved slot to touch without waiting for this writer. */
    for (struct cache_meta *m=interleaved_cache->meta;m;m=m->next)
        for (unsigned i=0;i<m->count;i++) for (unsigned j=0;j<8;j++) {
            struct cache_block *b=&m->pages[i].b[j];
            if (b->dev==interleaved_dev && !b->busy) {
                CHECK(!cache_read(interleaved_cache,interleaved_dev,b->lba,data));
                fs_lock_take(&interleaved_cache->lock); return;
            }
        }
    abort();
}
static void fill(struct block_cache *c,struct blkdev *d) {
    uint8_t data[512]; memset(data,0x44,sizeof(data));
    for (unsigned i=0;i<c->blocks;i++) CHECK(!cache_write(c,d,((uint64_t)i*4051u+13u)%65536u,data));
}
int main(void) {
    for (unsigned site=0;site<3;site++) {
        uint64_t before=0,total_before=0;
        for (unsigned enabled=0;enabled<2;enabled++) {
            struct block_cache c;
            model_points=enabled;
            if (site) CHECK(!cache_init(&c,8u*1024*1024));
            model_total=model_span=model_max=0; model_enabled=true;
            if (!site) CHECK(!cache_init(&c,8u*1024*1024));
            if (site==1) cache_destroy(&c);
            if (site==2) { CHECK(!cache_workspace_begin(&c,128*4096)); cache_workspace_end(&c); }
            model_boundary(); model_enabled=false;
            if (!enabled) { before=model_max; total_before=model_total; }
            else {
                CHECK(model_max<=2000000 && model_total<=total_before*110/100);
                printf("F0-44 cache lifecycle site=%u span=%llu->%lluns work=%llu->%lluns PASS\n",site,
                    (unsigned long long)before,(unsigned long long)model_max,(unsigned long long)total_before,(unsigned long long)model_total);
            }
            if (site!=1) cache_destroy(&c);
        }
    }
    uint64_t before=0,total_before=0;
    for (unsigned enabled=0;enabled<2;enabled++) {
        struct block_cache c; CHECK(!cache_init(&c,8u*1024*1024));
        struct blkdev d={.sector_size=512,.capacity=65536,.write=write_data,.write_cache_state=BLKDEV_CACHE_DISABLED};
        /* get() requires a read callback even for a full-sector write. */
        d.read=read_data;
        fill(&c,&d); CHECK(c.dirty_bytes==c.blocks*512);
        unsigned blocks=c.blocks; write_sectors=0;
        model_total=model_span=model_max=0; model_enabled=true; model_points=enabled;
        CHECK(!cache_writeback_tick(&c,fs_now_ms()+1001)); model_boundary(); model_enabled=false;
        CHECK(write_sectors==64 && c.dirty_bytes==(blocks-64)*512);
        if (!enabled) { before=model_max; total_before=model_total; }
        else {
            CHECK(model_max<=2000000 && model_total<=total_before*110/100);
            printf("F0-44 all-dirty 8 MiB cache: slots=%u span=%llu->%lluns work=%llu->%lluns PASS\n",blocks,
                (unsigned long long)before,(unsigned long long)model_max,(unsigned long long)total_before,(unsigned long long)model_total);
            interleaved_cache=&c; interleaved_dev=&d;
            model_interleave=touch_during_scan;
            CHECK(!cache_writeback_tick(&c,fs_now_ms()+1001) && !model_interleave);
            model_interleave=invalidate_during_scan;
            CHECK(cache_writeback_tick(&c,fs_now_ms()+1001)==-FS_EIO && !model_interleave);
            CHECK(!c.dirty_bytes && !c.batch_busy);
            struct cache_scan scan={0};
            for (struct cache_block *b;(b=scan_next(&c,&scan));) CHECK(!b->busy && !b->dirty);
            puts("F0-44 cache interleavings: LRU touch, forget/invalidate during selection, all pins released PASS");
        }
        cache_destroy(&c);
    }
    for (unsigned site=0;site<3;site++) {
        uint64_t before=0,total_before=0;
        for (unsigned enabled=0;enabled<2;enabled++) {
            struct block_cache c; CHECK(!cache_init(&c,8u*1024*1024));
            struct blkdev devices[4]; memset(devices,0,sizeof(devices));
            for (unsigned i=0;i<4;i++) devices[i]=(struct blkdev){.sector_size=512,.capacity=1048576,
                .read=read_data,.write=write_data,.write_cache_state=BLKDEV_CACHE_DISABLED};
            struct cache_scan scan={0}; struct cache_block *target=0; unsigned count=0;
            for (struct cache_block *b;(b=scan_next(&c,&scan));) {
                unsigned dev=count/4096;
                b->dev=&devices[dev]; b->sector_size=512;
                b->lba=(count%4096)*256u+bucket(b->dev,0);
                b->hash_next=c.hash[0]; c.hash[0]=b;
                if (!target) target=b;
                count++;
            }
            if (site==2) {
                for (struct cache_block *b=c.lru[0].oldest;b;b=b->next) b->busy=true;
                c.lru[0].newest->busy=false;
                /* Discard the synthetic hash fixture; force a clean victim
                 * behind the maximum number of busy descriptors. */
                memset(c.hash,0,sizeof(c.hash));
                for (struct cache_block *b=c.lru[0].oldest;b;b=b->next) b->dev=0;
            }
            model_points=enabled; model_total=model_span=model_max=0; model_enabled=true;
            if (!site) CHECK(find(&c,target->dev,target->lba)==target);
            if (site==1) unhash(&c,target);
            if (site==2) { struct cache_block *b=0; CHECK(!get(&c,&devices[0],1,false,&b,0) && b==c.lru[0].newest); }
            model_boundary(); model_enabled=false;
            if (!enabled) { before=model_max; total_before=model_total; }
            else {
                CHECK(model_max<=2000000 && model_total<=total_before*110/100);
                printf("F0-44 cache hash/victim site=%u slots=%u span=%llu->%lluns work=%llu->%lluns PASS\n",site,count,
                    (unsigned long long)before,(unsigned long long)model_max,(unsigned long long)total_before,(unsigned long long)model_total);
            }
            cache_destroy(&c);
        }
    }
    return 0;
}
#endif
