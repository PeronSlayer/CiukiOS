/* C4 production cache/FAT/VFS controller evidence. Models explicitly exclude
 * physical media latency and use M1's NORMAL zone/hysteresis, not host RAM.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "vfs.h"
#include <ciuki/system_settings.h>
static struct system_perf g30_performance = {0,3,1,0,16,1024};
const struct system_perf *system_performance(void) { return &g30_performance; }
#include "fake.h"
#include <ciuki/ledger.h>
#include <ciuki/reclaim.h>
#include <stdio.h>
#include <assert.h>
#include <stdatomic.h>
#define MIB (1024u*1024u)
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL C4 %u: %s\n",__LINE__,#x); abort(); } } while (0)
static uint32_t pinned;
static bool pin_transition;
bool ledger_pin_transition(void) { return pin_transition; }
bool ledger_page_pinned(uint32_t pfn) { return pinned && pfn==pinned; }
struct disk {
    struct blkdev dev; struct blk_queue queue;
    uint64_t commands, sectors, writes, speculative;
    uint64_t last_lba, data_commands; uint32_t last_count;
    uint8_t *fat;
    _Atomic(bool) hold, entered;
    bool fail_write, replace_media;
};
static uint8_t pattern(uint64_t sector) { return (uint8_t)(sector*37+19); }
static int disk_read(struct blkdev *d,uint64_t lba,uint32_t n,void *buf) {
    struct disk *m=d->ctx; CHECK(!blkdev_range(d,lba,n));
    m->commands++; m->sectors+=n; m->last_lba=lba; m->last_count=n;
    CHECK(d->queue->active);
    m->speculative+=d->queue->active->priority==BLK_SPECULATIVE;
    atomic_store(&m->entered,true);
    while (atomic_load(&m->hold)) { struct timespec t={0,100000}; nanosleep(&t,0); }
    if (m->fat && lba>=33) m->data_commands++;
    for (unsigned i=0;i<n;i++) {
        if (m->fat && lba+i>=1 && lba+i<33) memcpy((uint8_t *)buf+i*512,m->fat+(lba+i-1)*512,512);
        else memset((uint8_t *)buf+i*512,pattern(lba+i),512);
    }
    if (m->replace_media) { m->replace_media=false; d->media_generation++; }
    return 0;
}
static int disk_write(struct blkdev *d,uint64_t lba,uint32_t n,const void *buf) {
    (void)lba; (void)buf; struct disk *m=d->ctx; m->writes+=n; return m->fail_write ? -FS_EIO : 0;
}
static void init_disk(struct disk *m) {
    memset(m,0,sizeof(*m)); blk_queue_init(&m->queue,"c4-host",65536);
    m->dev=(struct blkdev){.read=disk_read,.write=disk_write,.sector_size=512,.capacity=1024*1024,
        .write_cache_state=BLKDEV_CACHE_DISABLED,.ctx=m,.queue=&m->queue};
}
static void end_disk(struct disk *m) { CHECK(!blk_queue_destroy(&m->queue)); CHECK(!blk_request_pool_host_cleanup()); }
static struct cache_block *block(struct block_cache *c,struct blkdev *d,uint64_t lba,void **page) {
    for (unsigned i=0;i<CACHE_PAGE_SLOTS;i++) if (c->inline_page.b[i].dev==d && c->inline_page.b[i].lba==lba) {
        *page=c->inline_page.data; return &c->inline_page.b[i];
    }
    for (struct cache_meta *m=c->meta;m;m=m->next) for (unsigned p=0;p<m->count;p++)
        for (unsigned i=0;m->pages[p].data && i<CACHE_PAGE_SLOTS;i++)
            if (m->pages[p].b[i].dev==d && m->pages[p].b[i].lba==lba) {
                *page=m->pages[p].data; return &m->pages[p].b[i];
            }
    return 0;
}
static void useful_window(struct block_cache *c,struct disk *m,unsigned index) {
    uint8_t buf[512]; uint64_t lba=100000+index;
    CHECK(!cache_read(c,&m->dev,lba,buf) && !cache_read(c,&m->dev,lba,buf));
}
static uint64_t cpu_us(void) {
    struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t);
    return (uint64_t)t.tv_sec*1000000+t.tv_nsec/1000;
}
static void profiles(void) {
    for (unsigned profile=128;profile<=512;profile*=2) {
        struct mm_ledger ledger={0};
        uint32_t usable=(profile-16)*MIB/4096; /* synthetic 16 MiB LOW/DMA exclusion */
        ledger.zone[2].usable=usable; ledger.zone[2].free=usable-16*MIB/4096;
        ledger.low=usable/32; if (ledger.low<1024) ledger.low=1024; ledger.high=2*ledger.low;
        uint32_t initial=cache_initial_bytes(usable,ledger.zone[2].free,ledger.high);
        CHECK(initial==8*MIB);
        struct block_cache c; struct disk m,healthy; struct cache_stats s,prev;
        init_disk(&m); init_disk(&healthy); CHECK(!cache_init(&c,initial) && !cache_adaptive_enable(&c,usable));
        cache_stats(&c,&s); ledger.zone[2].free-=s.pages;
        printf("TRACE C4 sizing profile=%u NORMAL=%u low=%u high=%u initial=%u ceiling=%u data=%u meta=%u work=%u descriptor_bytes=%zu\n",
            profile,usable,ledger.low,ledger.high,s.pages,s.ceiling_pages,s.data_pages,s.meta_pages,
            s.workspace_pages+s.writeback_pages,sizeof(struct cache_page));
        uint32_t now=c.window_time;
        for (unsigned i=0;i<4;i++) {
            prev=s; useful_window(&c,&m,i); now+=100;
            cache_adaptive_tick(&c,now,ledger.zone[2].free,ledger.high,false); cache_stats(&c,&s);
            ledger.zone[2].free-=s.pages-prev.pages;
            CHECK(s.pages-prev.pages<=64 && (i || s.pages==prev.pages));
        }
        CHECK(s.growths==3 && s.pages>initial/4096);
        printf("TRACE C4 growth profile=%u windows=4 pages=%u growths=%u free=%u\n",profile,s.pages,s.growths,ledger.zone[2].free);
        /* Hysteresis, poor prefetch utility and dirty debt all suppress growth. */
        uint32_t before=s.pages;
        useful_window(&c,&m,10); cache_adaptive_tick(&c,now+=100,ledger.high,ledger.high,false);
        useful_window(&c,&m,11); cache_adaptive_tick(&c,now+=100,ledger.high,ledger.high,false);
        cache_stats(&c,&s); CHECK(s.pages==before);
        c.prefetch_wasted=16384;
        useful_window(&c,&m,12); cache_adaptive_tick(&c,now+=100,ledger.zone[2].free,ledger.high,false);
        cache_stats(&c,&s); CHECK(s.pages==before); c.prefetch_wasted=0;
        /* A PMM pin, an in-flight fill and a sticky failed dirty page remain
         * allocated while the cursor reclaims the rest down to the floor. */
        void *pin_page=0,*dirty_page=0,*busy_page=0; uint8_t bytes[512]={0x92};
        CHECK(!cache_read(&c,&m.dev,20000,bytes));
        struct cache_block *pin_block=block(&c,&m.dev,20000,&pin_page); CHECK(pin_block);
        pinned=(uint32_t)((uintptr_t)pin_page>>12);
        CHECK(!cache_write(&c,&m.dev,20100,bytes));
        struct cache_block *dirty=block(&c,&m.dev,20100,&dirty_page); CHECK(dirty);
        m.fail_write=true; CHECK(cache_barrier(&c,&m.dev)==-FS_EIO);
        CHECK(!cache_read(&c,&m.dev,20200,bytes));
        struct cache_block *busy=block(&c,&m.dev,20200,&busy_page); CHECK(busy); busy->busy=true;
        CHECK(!cache_write(&c,&healthy.dev,30000,bytes));
        uint64_t writes=m.writes; uint32_t dirty_bytes=c.dirty_bytes;
        unsigned slices=0,blocked_pin=0,blocked_dirty=0,blocked_error=0;
        while (s.pages>c.floor_pages && slices++<1000) {
            prev=s;
            /* Anonymous demand keeps NORMAL below low; reclaim contributes
             * actual freed cache categories to the same free-page ledger. */
            ledger.zone[2].free=ledger.low/2;
            ledger.pressure=ledger.pressure ? ledger.zone[2].free<=ledger.high : ledger.zone[2].free<ledger.low;
            cache_adaptive_tick(&c,now+=10,ledger.zone[2].free,ledger.high,ledger.pressure);
            cache_stats(&c,&s); ledger.zone[2].free+=prev.pages-s.pages;
            blocked_pin+=s.blocked_pinned; blocked_dirty+=s.blocked_dirty; blocked_error+=s.blocked_error;
            CHECK(prev.pages-s.pages<=64 && s.slice_scanned<=64 && s.speculative_bytes<=s.pages*4096/8);
            CHECK(dirty->dirty && dirty->data && c.dirty_bytes==dirty_bytes && pin_block->data && busy->busy);
        }
        CHECK(s.pages==512 && m.writes==writes && s.max_slice_us<=1000);
        CHECK(blocked_pin && blocked_dirty && blocked_error);
        CHECK(c.writeback_pending && c.devices[1].requested && !healthy.writes);
        printf("TRACE C4 pressure profile=%u floor=%u slices=%u reclaimed=%u pinned_visits=%u dirty_visits=%u error_visits=%u dirty_reclaimed=0 pinned_reclaimed=0 max_cpu_us=%u\n",
            profile,s.pages,slices,s.reclaimed_pages,blocked_pin,blocked_dirty,blocked_error,s.max_slice_us);
        pinned=0; busy->busy=false; c.pressure=false;
        uint8_t hot[16384]; CHECK(!cache_read_run(&c,&m.dev,21000,32,hot));
        uint64_t start=cpu_us(), commands=m.commands;
        for (unsigned i=0;i<1000;i++) {
            CHECK(!cache_read_run(&c,&m.dev,21000,32,hot));
            cache_adaptive_tick(&c,now+=10,usable-4096-s.pages,ledger.high,false);
        }
        printf("TRACE C4 steady profile=%u operations=1000 controller_ticks=1000 bytes=16384000 cpu_us=%llu resident_pages=%u data=%u meta=%u work=%u commands=%llu\n",
            profile,(unsigned long long)(cpu_us()-start),s.pages,s.data_pages,s.meta_pages,s.workspace_pages+s.writeback_pages,
            (unsigned long long)(m.commands-commands));
        cache_destroy(&c); CHECK(!blk_queue_destroy(&healthy.queue)); end_disk(&m);
    }
    CHECK(!cache_initial_bytes(4095,4095,0));
    CHECK(!cache_initial_bytes(100000,700,300));
    CHECK(cache_initial_bytes(32768,2000,1024)==976*4096);
}
static void stream_read(struct block_cache *c,struct disk *m,struct cache_stream *s,uint64_t offset,unsigned bytes) {
    uint8_t buf[16384]; CHECK(bytes<=sizeof(buf));
    cache_stream_begin(c,s,&m->dev,offset);
    CHECK(!cache_read_run(c,&m->dev,offset/512,bytes/512,buf));
    for (unsigned i=0;i<bytes;i++) CHECK(buf[i]==pattern(offset/512+i/512));
    unsigned window=cache_stream_end(s,offset,bytes,true);
    if (window && offset+bytes<MIB) {
        unsigned n=(MIB-offset-bytes)/512; if (n>window/512) n=window/512;
        cache_stream_queue(s,(offset+bytes)/512,n);
    }
}
static void sequential(unsigned bytes) {
    struct block_cache c; struct disk m; struct cache_stream s={0}; init_disk(&m);
    CHECK(!cache_init(&c,8*MIB) && !cache_adaptive_enable(&c,28672));
    for (unsigned offset=0;offset<MIB;offset+=bytes) {
        stream_read(&c,&m,&s,offset,bytes);
        while (cache_prefetch_tick(&c)) { }
        CHECK(c.speculative_bytes<=c.data_pages*4096u); CHECK(c.speculative_bytes<=(c.data_pages+c.meta_pages+c.workspace_pages+c.writeback_pages)*512u);
    }
    CHECK(s.window_bytes==65536 && s.useful>0 && m.commands<MIB/bytes && m.speculative>0);
    printf("TRACE C4 sequential read_bytes=%u baseline_commands=%u commands=%llu factor=%.3f useful=%llu wasted=%llu window=%u peak=%u\n",
        bytes,MIB/bytes,(unsigned long long)m.commands,(double)(MIB/bytes)/m.commands,
        (unsigned long long)s.useful,(unsigned long long)s.wasted,s.window_bytes,c.speculative_peak);
    cache_stream_close(&s); CHECK(!c.streams && !c.speculative_bytes);
    cache_destroy(&c); end_disk(&m);
}
static void fat_sequential(unsigned bytes) {
    uint64_t commands[2], data_commands[2];
    for (unsigned ahead=0;ahead<2;ahead++) {
        struct block_cache c; struct disk m; struct cache_stream stream={0}; init_disk(&m);
        m.fat=calloc(32,512); CHECK(m.fat);
        for (unsigned cluster=2;cluster<258;cluster++) fs_wr32(m.fat+cluster*4,cluster<257 ? cluster+1 : 0x0fffffff);
        CHECK(!cache_init(&c,8*MIB) && !cache_adaptive_enable(&c,28672));
        struct fat_volume vol={.cache=&c,.dev=&m.dev,.sectors=m.dev.capacity,
            .reserved=1,.fat_sectors=32,.data_sector=33,.clusters=2048,
            .type=32,.spc=8,.fats=1,.mounted=true};
        struct fat_entry file={.first=2,.size=MIB}; uint8_t buf[16384]; size_t done;
        for (unsigned offset=0;offset<MIB;offset+=bytes) {
            int e=ahead ? fat_read_stream(&vol,&file,offset,buf,bytes,&done,&stream,0) : fat_read(&vol,&file,offset,buf,bytes,&done);
            CHECK(!e && done==bytes);
            for (unsigned i=0;i<bytes;i++) CHECK(buf[i]==pattern(33+offset/512+i/512));
            while (ahead && cache_prefetch_tick(&c)) { }
        }
        commands[ahead]=m.commands; data_commands[ahead]=m.data_commands;
        cache_stream_close(&stream); cache_destroy(&c); free(m.fat); end_disk(&m);
    }
    CHECK(commands[1]<commands[0] && data_commands[1]<data_commands[0]);
    printf("TRACE C4 FAT sequential read_bytes=%u baseline_commands=%llu commands=%llu factor=%.3f data_commands=%llu/%llu EOF_bytes=%u\n",
        bytes,(unsigned long long)commands[0],(unsigned long long)commands[1],(double)commands[0]/commands[1],
        (unsigned long long)data_commands[0],(unsigned long long)data_commands[1],MIB);
}
static void *worker(void *arg) { CHECK(cache_prefetch_tick(arg)); return 0; }
struct join_reader { struct block_cache *cache; struct disk *disk; struct cache_stream *stream; _Atomic(bool) begun; };
static void *joining_reader(void *arg) {
    struct join_reader *reader=arg; uint8_t bytes[4096];
    cache_stream_begin(reader->cache,reader->stream,&reader->disk->dev,8192);
    atomic_store(&reader->begun,true);
    CHECK(!cache_read_run(reader->cache,&reader->disk->dev,16,8,bytes));
    for (unsigned i=0;i<sizeof(bytes);i++) CHECK(bytes[i]==pattern(16+i/512));
    (void)cache_stream_end(reader->stream,8192,sizeof(bytes),true); return 0;
}
static void same_stream_join(void) {
    struct block_cache c; struct disk m; struct cache_stream s={0}; init_disk(&m);
    CHECK(!cache_init(&c,8*MIB) && !cache_adaptive_enable(&c,28672));
    stream_read(&c,&m,&s,0,4096); stream_read(&c,&m,&s,4096,4096);
    atomic_store(&m.entered,false); atomic_store(&m.hold,true);
    uint64_t commands=m.commands; pthread_t prefetch,demand;
    CHECK(!pthread_create(&prefetch,0,worker,&c));
    while (!atomic_load(&m.entered)) { struct timespec t={0,100000}; nanosleep(&t,0); }
    struct join_reader reader={.cache=&c,.disk=&m,.stream=&s};
    CHECK(!pthread_create(&demand,0,joining_reader,&reader));
    while (!atomic_load(&reader.begun)) { struct timespec t={0,100000}; nanosleep(&t,0); }
    atomic_store(&m.hold,false);
    CHECK(!pthread_join(prefetch,0) && !pthread_join(demand,0));
    CHECK(m.commands==commands+1 && s.useful==4096 && !s.wasted && !s.outstanding);
    cache_stream_close(&s); cache_destroy(&c); end_disk(&m);
    puts("PASS C4 same-stream sequential demand joins issued prefetch without cancellation or duplicate device command");
}
static void cancellation(void) {
    struct block_cache c; struct disk m; struct cache_stream a={0},b={0}; init_disk(&m);
    CHECK(!cache_init(&c,8*MIB) && !cache_adaptive_enable(&c,28672));
    stream_read(&c,&m,&a,0,4096); stream_read(&c,&m,&a,4096,4096); CHECK(a.pending==32);
    uint64_t commands=m.commands; uint32_t gen=a.generation;
    cache_stream_cancel(&a); CHECK(a.generation!=gen && !cache_prefetch_tick(&c) && commands==m.commands);
    stream_read(&c,&m,&a,0,4096); stream_read(&c,&m,&a,4096,4096);
    stream_read(&c,&m,&b,100000*512,16384); CHECK(!a.pending && !cache_prefetch_tick(&c));
    /* Cancellation after issue retains the bounce buffer and cache slots
     * until the transport retires; stale bytes are never published. */
    stream_read(&c,&m,&a,0,4096); stream_read(&c,&m,&a,4096,4096);
    atomic_store(&m.entered,false); atomic_store(&m.hold,true);
    pthread_t thread; CHECK(!pthread_create(&thread,0,worker,&c));
    while (!atomic_load(&m.entered)) { struct timespec t={0,100000}; nanosleep(&t,0); }
    CHECK(a.outstanding==1); cache_stream_cancel(&a); atomic_store(&m.hold,false);
    CHECK(!pthread_join(thread,0)); CHECK(!a.outstanding && !a.resident && a.wasted>=16384);
    commands=m.commands; uint8_t buf[512]; CHECK(!cache_read(&c,&m.dev,16,buf) && m.commands==commands+1);
    /* Media changes during a completed request also cancel publication. */
    cache_invalidate(&c,&m.dev,true);
    stream_read(&c,&m,&a,0,4096); stream_read(&c,&m,&a,4096,4096);
    m.replace_media=true; CHECK(cache_prefetch_tick(&c) && !a.resident && !a.outstanding);
    cache_invalidate(&c,&m.dev,true);
    stream_read(&c,&m,&a,0,4096); stream_read(&c,&m,&a,4096,4096);
    cache_adaptive_tick(&c,c.window_time+1,0,2048,true);
    CHECK(!a.pending && !cache_prefetch_tick(&c));
    cache_stream_close(&a); cache_stream_close(&b); CHECK(!c.speculative_bytes);
    cache_destroy(&c); end_disk(&m);
    puts("PASS C4 cancellation: seek, competing demand, in-flight retirement, media generation, pressure, close");
}
static void random_reads(void) {
    struct block_cache c; struct disk m; struct cache_stream s={0}; init_disk(&m);
    CHECK(!cache_init(&c,8*MIB) && !cache_adaptive_enable(&c,28672));
    stream_read(&c,&m,&s,0,4096); stream_read(&c,&m,&s,4096,4096); CHECK(cache_prefetch_tick(&c));
    uint64_t prefetch=m.speculative, start=cpu_us(); uint32_t seed=12345;
    for (unsigned i=0;i<256;i++) {
        seed=1664525*seed+1013904223;
        uint64_t offset=(uint64_t)(1+(seed%1023))*32768;
        stream_read(&c,&m,&s,offset,16384); CHECK(!s.pending && !cache_prefetch_tick(&c));
        CHECK(c.speculative_bytes<=MIB);
    }
    cache_stream_close(&s);
    CHECK(m.speculative==prefetch && c.prefetch_wasted==16384 && !c.speculative_bytes);
    uint8_t retired[512]; uint64_t commands=m.commands;
    CHECK(!cache_read(&c,&m.dev,16,retired) && m.commands==commands+1); /* close removes unused speculation */
    printf("TRACE C4 random read_bytes=16384 reads=256 additional_prefetch=0 software_prefetch_waits=0 cpu_us=%llu wasted=%llu cap=%u\n",
        (unsigned long long)(cpu_us()-start),(unsigned long long)c.prefetch_wasted,MIB);
    cache_destroy(&c); end_disk(&m);
}
static void dirty_prefetch_hit(void) {
    struct block_cache c; struct disk m; struct cache_stream s={0}; init_disk(&m);
    CHECK(!cache_init(&c,8*MIB) && !cache_adaptive_enable(&c,28672));
    stream_read(&c,&m,&s,0,4096); stream_read(&c,&m,&s,4096,4096);
    uint8_t dirty[512], actual[16384]; memset(dirty,0x93,512);
    CHECK(!cache_write(&c,&m.dev,24,dirty));
    CHECK(cache_prefetch_tick(&c) && cache_prefetch_tick(&c));
    CHECK(!cache_read_run(&c,&m.dev,16,32,actual));
    for (unsigned i=0;i<sizeof(actual);i++) CHECK(actual[i]==(i/512==8 ? 0x93 : pattern(16+i/512)));
    CHECK(!m.writes && c.dirty_bytes==512 && s.useful==31*512);
    cache_stream_close(&s); cache_destroy(&c); end_disk(&m);
    puts("PASS C4 prefetch shares dirty hits without overwriting or writing them");
}
static void policy_edges(void) {
    struct block_cache c; struct disk m; struct cache_stats before,after; init_disk(&m);
    CHECK(!cache_init(&c,8192));
    struct blkdev pinned_optical={.sector_size=2048,.capacity=64};
    pinned=(uint32_t)((uintptr_t)c.inline_page.data>>12);
    CHECK(cache_attach(&c,&pinned_optical)==-FS_EBUSY && c.slots[0]==8 && !c.slots[1]);
    pinned=0; CHECK(!cache_attach(&c,&pinned_optical) && c.slots[1]==2); cache_destroy(&c);
    CHECK(!cache_init(&c,2*MIB) && !cache_adaptive_enable(&c,16384));
    c.window_time=UINT32_MAX-99; uint32_t now=c.window_time;
    useful_window(&c,&m,0); cache_adaptive_tick(&c,now+=100,10000,2048,false);
    cache_stats(&c,&before); fs_host_page_fail=1;
    useful_window(&c,&m,1); cache_adaptive_tick(&c,now+=100,10000,2048,false);
    cache_stats(&c,&after); CHECK(before.pages==after.pages && !fs_host_page_fail);
    useful_window(&c,&m,2); cache_adaptive_tick(&c,now+=100,10000,2048,false);
    CHECK(c.growths==1); /* modulo-32-bit clock rollover and allocation retry */
    uint8_t sector[512]={0x42};
    unsigned dirty=c.dirty_budget/512+1;
    for (unsigned n=0;n<dirty;n++) CHECK(!cache_write(&c,&m.dev,n,sector));
    cache_stats(&c,&before); CHECK(c.dirty_bytes>c.dirty_budget);
    for (unsigned i=0;i<2;i++) { useful_window(&c,&m,10+i); cache_adaptive_tick(&c,now+=100,10000,2048,false); }
    cache_stats(&c,&after); CHECK(before.pages==after.pages);
    CHECK(!cache_barrier(&c,&m.dev));
    for (unsigned i=0;i<120;i++) {
        useful_window(&c,&m,100+i); cache_stats(&c,&before);
        cache_adaptive_tick(&c,now+=100,10000,2048,false); cache_stats(&c,&after);
        CHECK(after.pages-before.pages<=64 && after.pages<=c.ceiling_pages);
    }
    /* A descriptor step may leave one page unspendable at the ceiling. */
    CHECK(c.ceiling_pages-after.pages<=1);
    unsigned pages=after.pages; cache_adaptive_tick(&c,now,10000,2048,false);
    cache_stats(&c,&after); CHECK(after.pages==pages);
    do { cache_adaptive_tick(&c,now+=10,0,2048,true); cache_stats(&c,&after); } while (after.pages>512);
    CHECK(c.pressure);
    struct blkdev optical={.sector_size=2048,.capacity=64};
    CHECK(!cache_attach(&c,&optical) && c.slots[1]);
    useful_window(&c,&m,900); cache_adaptive_tick(&c,now+=100,10000,2048,false);
    useful_window(&c,&m,901); cache_adaptive_tick(&c,now+=100,10000,2048,false);
    CHECK(c.data_pages>after.data_pages && c.blocks==c.slots[0]+c.slots[1]);
    cache_destroy(&c); end_disk(&m);
    puts("PASS C4 controller edges: rollover, OOM retry, actual C1 dirty-debt suppression, ceiling, one growth per window");
}
static void speculative_cap(void) {
    struct block_cache c; struct disk m; struct cache_stream streams[65]={0}; init_disk(&m);
    CHECK(!cache_init(&c,8*MIB) && !cache_adaptive_enable(&c,28672));
    uint8_t buf[8192];
    /* Warm demand locations before speculation, so filling them does not
     * evict the speculation whose global resident cap is being exercised. */
    for (unsigned i=0;i<65;i++) CHECK(!cache_read_run(&c,&m.dev,i*256,16,buf));
    for (unsigned i=0;i<65;i++) {
        for (unsigned r=0;r<2;r++) {
            uint64_t offset=(uint64_t)i*131072+r*4096;
            cache_stream_begin(&c,&streams[i],&m.dev,offset);
            CHECK(!cache_read_run(&c,&m.dev,offset/512,8,buf));
            unsigned window=cache_stream_end(&streams[i],offset,4096,true);
            if (r) { CHECK(window==16384); cache_stream_queue(&streams[i],i*256+16,32); }
        }
        CHECK(cache_prefetch_tick(&c)==(i<64));
        CHECK(c.speculative_bytes<=(c.data_pages+c.meta_pages+c.workspace_pages+c.writeback_pages)*512u);
    }
    CHECK(c.speculative_bytes==MIB && c.speculative_peak==MIB);
    struct cache_stats stats; unsigned slices=0;
    do {
        cache_adaptive_tick(&c,c.window_time+1,0,2048,true); cache_stats(&c,&stats);
        CHECK(stats.speculative_bytes<=stats.pages*512u); CHECK(++slices<1000);
    } while (stats.pages>512);
    for (unsigned i=0;i<65;i++) cache_stream_close(&streams[i]);
    CHECK(!c.speculative_bytes && !c.streams && c.prefetch_wasted==MIB && stats.max_slice_us<=1000);
    /* f4-04 conversion skips descriptor holes made by reclaim, and later
     * growth reuses those holes without changing any surviving address. */
    struct blkdev optical={.sector_size=2048,.capacity=64};
    CHECK(!cache_attach(&c,&optical) && c.slots[1]);
    uint32_t now=c.window_time;
    useful_window(&c,&m,900); cache_adaptive_tick(&c,now+=100,20000,2048,false);
    useful_window(&c,&m,901); cache_adaptive_tick(&c,now+=100,20000,2048,false);
    CHECK(c.data_pages==stats.data_pages && c.blocks==c.slots[0]+c.slots[1]); /* poor utility blocks growth */
    printf("TRACE C4 speculation streams=65 peak=%u cap=%u pressure_floor=%u slices=%u wasted=%llu\n",
        c.speculative_peak,MIB,stats.pages,slices,(unsigned long long)c.prefetch_wasted);
    cache_destroy(&c); end_disk(&m);
}
static void vfs_eof(const char *path) {
    static struct fake disk; static struct block_cache c; static struct fat_volume vol;
    static struct vfs v; static struct vfs_table table;
    CHECK(!fake_open(&disk,path)); CHECK(!cache_init(&c,8*MIB));
    CHECK(!cache_adaptive_enable(&c,28672));
    CHECK(!fat_mount(&vol,&c,&disk.dev,0,disk.dev.capacity,true));
    vfs_init(&v); CHECK(!vfs_attach(&v,2,&vol)); CHECK(!vfs_table_init(&v,&table,2));
    int h=vfs_open(&table,"C:/SHORT.BIN",VFS_READ,VFS_DENY_NONE,0); CHECK(h>=0);
    int dup=vfs_dup(&table,h,&table,-1); CHECK(dup>=0 && table.handles[h]==table.handles[dup]);
    uint8_t buf[512]; size_t done;
    CHECK(!vfs_read(&table,h,buf,512,&done) && done==512);
    CHECK(!table.handles[h]->stream.pending);
    CHECK(!vfs_read(&table,dup,buf,512,&done) && done==512);
    struct cache_stream *s=&table.handles[h]->stream;
    CHECK(s->sequential==2 && s->window_bytes==16384 && s->pending<=22 && s->pending>0);
    uint64_t lba=s->lba; uint32_t count=s->pending;
    CHECK(count*512<=12345-1024); CHECK(cache_prefetch_tick(&c));
    CHECK(lba+count<=disk.dev.capacity);
    uint64_t position; uint32_t generation=s->generation;
    CHECK(!vfs_seek(&table,h,0,VFS_SEEK_SET,&position) && s->generation!=generation && !s->pending);
    CHECK(!vfs_close(&table,h) && s->cache==&c); CHECK(!vfs_close(&table,dup) && !c.streams && !c.speculative_bytes);
    /* A real truncate through a different open cancels the reader's plan
     * before changing EOF. The independent fixture is restored afterwards. */
    disk.undo_enabled=true;
    vfs_lock(&v,1u<<2); CHECK(!VFS_IO(&v,fat_enable_write(&vol))); vfs_unlock(&v);
    h=vfs_open(&table,"C:/SHORT.BIN",VFS_READ,VFS_DENY_NONE,0); CHECK(h>=0);
    int writer=vfs_open(&table,"C:/SHORT.BIN",VFS_WRITE,VFS_DENY_NONE,0); CHECK(writer>=0);
    CHECK(!vfs_read(&table,h,buf,512,&done) && !vfs_read(&table,h,buf,512,&done));
    s=&table.handles[h]->stream; CHECK(s->pending); generation=s->generation;
    CHECK(!vfs_truncate(&table,writer,1024));
    CHECK(s->generation!=generation && !s->pending && !cache_prefetch_tick(&c));
    CHECK(!vfs_close(&table,writer) && !vfs_close(&table,h));
    vfs_table_destroy(&table); CHECK(!vfs_detach(&v,2)); vfs_destroy(&v); cache_destroy(&c);
    fake_reset(&disk); fake_close(&disk);
    CHECK(!blk_request_pool_host_cleanup());
    puts("PASS C4 VFS/FAT: dup shares sequential state; EOF excludes partial tail; truncate cancels other opens; final close retires tags");
}
static _Atomic(bool) held_read, held_entered;
static int (*original_read)(struct blkdev *,uint64_t,uint32_t,void *);
static int hold_read(struct blkdev *d,uint64_t lba,uint32_t count,void *buf) {
    atomic_store(&held_entered,true);
    while (atomic_load(&held_read)) { struct timespec t={0,100000}; nanosleep(&t,0); }
    return original_read(d,lba,count,buf);
}
struct close_reader { struct vfs_table *table; int handle; };
static void *close_reader(void *arg) {
    struct close_reader *reader=arg; CHECK(!vfs_close(reader->table,reader->handle)); return 0;
}
static void vfs_close_retirement(const char *path) {
    static struct fake disk; static struct block_cache c; static struct fat_volume vol;
    static struct vfs v; static struct vfs_table table;
    CHECK(!fake_open(&disk,path)); CHECK(!cache_init(&c,8*MIB) && !cache_adaptive_enable(&c,28672));
    CHECK(!fat_mount(&vol,&c,&disk.dev,0,disk.dev.capacity,true));
    vfs_init(&v); CHECK(!vfs_attach(&v,2,&vol) && !vfs_table_init(&v,&table,2));
    int h=vfs_open(&table,"C:/SHORT.BIN",VFS_READ,VFS_DENY_NONE,0); CHECK(h>=0);
    uint8_t bytes[512]; size_t done;
    CHECK(!vfs_read(&table,h,bytes,512,&done) && !vfs_read(&table,h,bytes,512,&done));
    original_read=disk.dev.read; disk.dev.read=hold_read;
    atomic_store(&held_entered,false); atomic_store(&held_read,true);
    pthread_t prefetch,closing; CHECK(!pthread_create(&prefetch,0,worker,&c));
    while (!atomic_load(&held_entered)) { struct timespec t={0,100000}; nanosleep(&t,0); }
    struct close_reader reader={&table,h}; CHECK(!pthread_create(&closing,0,close_reader,&reader));
    bool unlocked=false;
    for (unsigned i=0;i<10000 && !unlocked;i++) {
        if (fs_lock_try_take(&v.lock)) { unlocked=table.handles[h]==0; fs_lock_drop(&v.lock); }
        if (!unlocked) { struct timespec t={0,100000}; nanosleep(&t,0); }
    }
    CHECK(unlocked); /* final close is still waiting for our held transport */
    atomic_store(&held_read,false); CHECK(!pthread_join(prefetch,0) && !pthread_join(closing,0));
    CHECK(!c.streams && !c.speculative_bytes); disk.dev.read=original_read;
    vfs_table_destroy(&table); CHECK(!vfs_detach(&v,2)); vfs_destroy(&v); cache_destroy(&c); fake_close(&disk);
    CHECK(!blk_request_pool_host_cleanup());
    puts("PASS C4 VFS final close waits for issued retirement with namespace available to unrelated operations");
}
static void global_reclaim_hook(void) {
    struct block_cache c; struct disk m; struct cache_stream stream={0};
    init_disk(&m); CHECK(!cache_init(&c,8*MIB) && !cache_adaptive_enable(&c,28672));
    uint8_t bytes[4096];
    for (unsigned i=0;i<c.data_pages-6;i++) CHECK(!cache_read_run(&c,&m.dev,i*256,8,bytes));
    stream_read(&c,&m,&stream,64*1024,4096); stream_read(&c,&m,&stream,64*1024+4096,4096);
    CHECK(cache_prefetch_tick(&c) && c.speculative_bytes==16384);
    void *dirty_page=0,*pin_page=0,*busy_page=0,*clean_page=0;
    CHECK(!cache_write(&c,&m.dev,(c.data_pages-10)*256,bytes));
    struct cache_block *dirty=block(&c,&m.dev,(c.data_pages-10)*256,&dirty_page);
    struct cache_block *pin=block(&c,&m.dev,(c.data_pages-20)*256,&pin_page);
    struct cache_block *busy=block(&c,&m.dev,(c.data_pages-30)*256,&busy_page);
    struct cache_block *clean_block=block(&c,&m.dev,(c.data_pages-40)*256,&clean_page);
    CHECK(dirty && pin && busy && clean_block && clean_page!=c.inline_page.data);
    pinned=(uint32_t)((uintptr_t)pin_page>>12); busy->busy=true;
    /* Reverse valid bucket chains to model the adverse insertion order;
     * the scanner must continue long removals without a long callback. */
    for (unsigned h=0;h<256;h++) {
        struct cache_block *at=c.hash[h],*prev=0;
        while (at) { struct cache_block *next=at->hash_next; at->hash_next=prev; prev=at; at=next; }
        c.hash[h]=prev;
    }
    uint32_t data=c.data_pages,meta=c.meta_pages,reclaimed=0,blocked_dirty=0,blocked_pin=0,inflight=0;
    uint64_t writes=m.writes,commands=m.commands,max_us=0;
    CHECK(!cache_read(&c,&m.dev,(c.data_pages-40)*256,bytes) && !cache_read(&c,&m.dev,(c.data_pages-40)*256,bytes) && m.commands==commands);
    struct reclaim_result r; bool wrote_during_trim=false;
    pin_transition=true; cache_reclaim_step(&c,true,&r);
    CHECK(r.pinned && !r.reclaimed && c.data_pages==data);
    pin_transition=false;
    for (unsigned stage=0;stage<2;stage++) {
        unsigned calls=0;
        do {
            uint64_t start=cpu_us(); cache_reclaim_step(&c,stage==0,&r);
            uint64_t elapsed=cpu_us()-start; if (elapsed>max_us) max_us=elapsed;
            CHECK(r.scanned<=1 && r.reclaimed<=1 && ++calls<2000000);
            if (stage && !wrote_during_trim && c.trim_page && c.trim_prev) {
                for (unsigned i=0;i<CACHE_PAGE_SLOTS;i++) {
                    struct cache_block *target=&c.trim_page->b[i];
                    if (target->dev && !target->dirty && !target->busy) {
                        CHECK(!cache_write(&c,target->dev,target->lba,bytes));
                        wrote_during_trim=true; break;
                    }
                }
            }
            reclaimed+=r.reclaimed; blocked_dirty+=r.dirty; blocked_pin+=r.pinned; inflight+=r.inflight;
        } while (!r.done);
        if (!stage) {
            CHECK(!c.speculative_bytes && stream.resident==0);
            CHECK(clean_block->data==clean_page && clean_block->dev==&m.dev);
        }
    }
    CHECK(reclaimed==data-c.data_pages+meta-c.meta_pages && meta>c.meta_pages && blocked_dirty && blocked_pin && inflight);
    CHECK(dirty->dirty && pin->data==pin_page && busy->busy && m.writes==writes);
    CHECK(c.writeback_pending && max_us<=1000 && wrote_during_trim);
    printf("TRACE M3 C4 hook reclaimed_frames=%u shared_hits=2 physical_count=1 dirty=%u pinned=%u inflight=%u max_step_cpu_us=%llu io_writes=0 concurrent_redirty=retained\n",
        reclaimed,blocked_dirty,blocked_pin,inflight,(unsigned long long)max_us);
    pinned=0; busy->busy=false; cache_stream_close(&stream); cache_destroy(&c); end_disk(&m);
}
static void performance_settings(void)
{
    for (unsigned role=0;role<3;role++) {
        g30_performance.role=role;
        struct block_cache c; struct disk m; init_disk(&m); CHECK(!cache_init(&c,2*MIB));
        uint8_t data[512]={0}; CHECK(!cache_write(&c,&m.dev,4,data));
        void *page=0; struct cache_block *b=block(&c,&m.dev,4,&page); CHECK(b && b->dirty);
        unsigned age=role==1 ? 500 : role==2 ? 2000 : 1000;
        CHECK(!cache_writeback_due(&c,b->since+age-1) && cache_writeback_due(&c,b->since+age));
        cache_destroy(&c); end_disk(&m);
    }
    g30_performance.role=0;
    for (unsigned ahead=0;ahead<4;ahead++) {
        g30_performance.read_ahead=ahead;
        struct block_cache c; struct disk m; struct cache_stream stream={0}; init_disk(&m);
        CHECK(!cache_init(&c,2*MIB));
        uint8_t data[512]; CHECK(!cache_read_ahead(&c,&m.dev,4000,128,data));
        CHECK(m.last_count==(ahead ? (16u<<ahead) : 1));
        stream_read(&c,&m,&stream,0,4096); stream_read(&c,&m,&stream,4096,4096);
        CHECK(ahead ? stream.window_bytes && stream.window_bytes<=(8192u<<ahead) : !stream.window_bytes && !stream.pending);
        cache_stream_close(&stream); cache_destroy(&c); end_disk(&m);
    }
    g30_performance=system_perf_default();
    puts("PASS G30B file-system roles: dirty-age deadlines and None/16/32/64 KiB read-ahead on production cache");
}
int main(int argc,char **argv) {
    CHECK(argc==2); setvbuf(stdout,0,_IONBF,0);
    performance_settings(); global_reclaim_hook(); profiles(); dirty_prefetch_hit(); policy_edges(); speculative_cap(); sequential(4096); sequential(16384); fat_sequential(4096); fat_sequential(16384); same_stream_join(); cancellation(); random_reads(); vfs_eof(argv[1]); vfs_close_retirement(argv[1]);
    puts("PASS C4 adaptive cache and stream read-ahead");
    return 0;
}
