/* f1-96b: a UHCI bulk data stage that stops advancing, and the USB storage
 * recovery ladder, on the production VFS/FAT/cache/queue/BOT/UHCI path over
 * the 1 ms frame model (uhci_model.h, at most 16 bulk transactions per
 * frame). The T23 5a10eef4 trace (td=388007ff never executed, stop=packet
 * after 44 ms, result=-110, then reset_recovery and quarantine) is replayed
 * by a driver that wakes after the HC drained the armed ring; the early-Intel
 * element erratum (Linux b761d9d867bc) by a model that leaves the QH element
 * on a completed TD. SPDX-License-Identifier: GPL-2.0-only */
#define main storage_fixture_main
#include "storage_test.c"
#undef main

uint32_t fs_host_clock_ms(void) { return (uint32_t)m.now; }
#define FRAME_PACKETS 16u

/* Driver wake latency in frames, and frames in which the HC found the bulk
 * element on an inactive TD while the READ(10) data stage was unfinished. */
static unsigned wake=1, dry_frames;
static bool in_wake;
static void frame_observer(void)
{
    volatile uint32_t *qh=uhci_dma(NULL,m.h.dma_phys+USB_UHCI_BULK_QH,8);
    uint32_t e=qh[1];
    if(m.cbw[15]==0x28 && m.stage==1 && !(e&3) && e>=m.h.dma_phys+USB_BULK_TD &&
       e<m.h.dma_phys+USB_BULK_TD+USB_BULK_BATCH*16 && !(uhci_dma(NULL,e&~15u,16)[1]&(1u<<23))) dry_frames++;
    /* A wedged HC resumes once its stale queue context is unlinked. */
    if(m.wedged && (qh[1]&1)) m.wedged=false;
    if(in_wake || wake<=1) return;
    in_wake=true; sleep_ms(NULL,wake-1); in_wake=false;
}
/* Element erratum model: after the data packet with index `stuck_at` (or
 * every `stuck_every`-th) of a READ(10) the HC writes the TD status but not
 * the QH element; `stuck_lose` instead writes a terminating element (the HC
 * loses the queue); `wedge_at` starts a wedge after a normal advance. */
static unsigned stuck_at=UINT32_MAX, stuck_every, stuck_hits, wedge_at=UINT32_MAX, data_index, data_command;
static bool stuck_lose;
static bool element_hook(void *cookie,uint32_t address,volatile uint32_t *qh,uint32_t next)
{
    (void)cookie;
    volatile uint32_t *td=uhci_dma(NULL,address,16);
    bool data=address!=m.h.dma_phys+USB_TD && (td[2]&255)==USB_PID_IN && m.cbw[15]==0x28 && m.stage!=0;
    if(data) {
        if(data_command!=m.commands) {data_command=m.commands;data_index=0;}
        unsigned k=data_index++;
        if(k==wedge_at) {wedge_at=UINT32_MAX;m.wedged=true;}
        else if(k==stuck_at || (stuck_every && k%stuck_every==stuck_every-1)) {
            if(k==stuck_at) stuck_at=UINT32_MAX;
            stuck_hits++;
            if(stuck_lose) qh[1]=1;
            return false;
        }
    }
    qh[1]=next;return true;
}
static void begin(const char *path,unsigned latency)
{
    setup(path,false);use_uhci();
    m.h.irq_attached=true;m.interrupt_mask=0x0d;m.native_gate=0x2000;
    wake=latency;sleep_hook=frame_observer;mount_device();
    stuck_at=wedge_at=UINT32_MAX;stuck_every=stuck_hits=dry_frames=0;stuck_lose=false;data_command=0;
    qh_advance_hook=element_hook;
}
static void end(void) {sleep_hook=NULL;qh_advance_hook=NULL;wake=1;cleanup();}
static uint32_t queue_limit(void)
{ struct blk_queue_stats st;blk_queue_snapshot(&device()->storage.requests,&st);return st.limit_bytes; }
static size_t mark(void) {return records_at;}
static const char *since(size_t at) {return records+at;}

/* T23 5a10eef4: the HC executed every armed TD while the driver slept and
 * waited at a slot not yet re-armed. The data stage must keep streaming. */
static void ring_drained(const char *path)
{
    static const unsigned latencies[]={9,20,40};
    for(unsigned i=0;i<ARRAY_SIZE(latencies);i++) {
        begin(path,latencies[i]);
        struct usb_storage *s=&device()->storage;
        uint8_t *buf=malloc(BLKDEV_READ_BYTES);assert(buf);
        size_t at=mark();uint64_t start=m.now;unsigned reads=m.read_commands;
        for(unsigned lba=0;lba<1024;lba+=128) {
            assert(!block()->read(block(),lba,128,buf));
            assert(!memcmp(buf,m.image+(size_t)lba*512,BLKDEV_READ_BYTES));
        }
        uint64_t ms=m.now-start;
        assert(m.read_commands==reads+8 && !m.resets && !block()->quarantined && dry_frames);
        assert(s->max_sectors==128 && !s->transfer_fallbacks && !s->relinks && !s->advance_fixes);
        assert(!strstr(since(at),"fault") && !strstr(since(at),"max_sectors addr="));
        assert(!strcmp(s->trace.bulk_stop,"complete"));
        printf("F1-96b drained ring wake=%u: 8 x 128-sector READ(10) ms=%llu KB/s=%llu dry_frames=%u\n",
               latencies[i],(unsigned long long)ms,(unsigned long long)(8ull*BLKDEV_READ_BYTES*1000/1024/ms),dry_frames);
        free(buf);end();
    }
    puts("PASS F1-96b drained TD ring: the HC finishing every armed TD before the driver wakes no longer ends the stage (T23 td=388007ff stop=packet result=-110)");
}

/* Early-Intel erratum: the element stays on a completed TD. */
static void element_erratum(const char *path)
{
    static const unsigned packets[]={0,127,272,1022,1023};
    for(unsigned i=0;i<ARRAY_SIZE(packets);i++) {
        begin(path,1);
        struct usb_storage *s=&device()->storage;
        uint8_t *buf=malloc(BLKDEV_READ_BYTES);assert(buf);
        size_t at=mark();stuck_at=packets[i];
        uint64_t start=m.now;
        assert(!block()->read(block(),256,128,buf) && !memcmp(buf,m.image+256*512,BLKDEV_READ_BYTES));
        uint64_t ms=m.now-start;
        assert(stuck_hits==1 && s->advance_fixes==1 && !s->relinks && !m.resets && !s->transfer_fallbacks);
        char needle[96];snprintf(needle,sizeof(needle)," packet=%u element=",packets[i]);
        const char *line=strstr(since(at),"[usb] uhci_advance controller=");
        assert(line && strstr(line,needle) && strstr(line," fixes=1\n"));
        /* 64 frames of data, the CBW/TUR/CSW phases and at most a few
         * frames for the stuck element: far below any bulk deadline. */
        assert(ms<64+USB_UHCI_ADVANCE_FRAMES+24);
        printf("F1-96b element erratum at packet %u: READ(10) 64 KiB ms=%llu fixes=%u\n",packets[i],(unsigned long long)ms,s->advance_fixes);
        free(buf);end();
    }
    /* A controller that misses every 100th advance still streams. */
    begin(path,3);
    struct usb_storage *s=&device()->storage;
    uint8_t *buf=malloc(BLKDEV_READ_BYTES);assert(buf);
    stuck_every=100;size_t at=mark();
    for(unsigned lba=0;lba<512;lba+=128)
        assert(!block()->read(block(),lba,128,buf) && !memcmp(buf,m.image+(size_t)lba*512,BLKDEV_READ_BYTES));
    assert(stuck_hits==40 && s->advance_fixes==40 && !m.resets && !block()->quarantined && s->max_sectors==128);
    unsigned lines=0;for(const char *p=since(at);(p=strstr(p,"[usb] uhci_advance "));p++) lines++;
    assert(lines==4); /* fixes 1-4, then every 64th: bounded diagnostics */
    free(buf);end();
    puts("PASS F1-96b early-Intel QH element erratum: an element left on a completed TD (first, ring boundary, T23 index 272, last two packets, every 100th) is advanced by hand within a few frames, no reset or fallback");
}

/* The same chain is retried once when the HC stops running the queue. */
static void same_chain(const char *path)
{
    for(unsigned kind=0;kind<2;kind++) {
        begin(path,1);
        struct usb_storage *s=&device()->storage;
        uint8_t *buf=malloc(BLKDEV_READ_BYTES);assert(buf);
        size_t at=mark();
        if(kind==0) {stuck_at=500;stuck_lose=true;} else wedge_at=300;
        uint64_t start=m.now;unsigned reads=m.read_commands;
        assert(!block()->read(block(),512,128,buf) && !memcmp(buf,m.image+512*512,BLKDEV_READ_BYTES));
        uint64_t ms=m.now-start;
        assert(s->relinks==1 && !s->advance_fixes && m.read_commands==reads+1 && !m.resets && !m.wedged);
        assert(ms>=USB_BULK_STALL_MS && ms<USB_BULK_STALL_MS+120);
        const char *line=strstr(since(at),"[usb-storage] data_retry addr=1 ");
        char needle[64];snprintf(needle,sizeof(needle)," packet=%u offset=%u ",kind ? 301u : 501u,kind ? 301u*64 : 501u*64);
        assert(line && strstr(line,needle) && strstr(line," result=relinked\n"));
        assert(strstr(line,kind ? " td=388007ff " : " element=00000001 "));
        free(buf);end();
    }
    /* A NAKing device is answering: no relink, no reset, no fallback. */
    begin(path,1);
    struct usb_storage *s=&device()->storage;
    uint8_t *buf=malloc(BLKDEV_READ_BYTES);assert(buf);
    m.data_nak=true;m.data_nak_offset=4096;m.data_nak_ms=1500;
    assert(!block()->read(block(),0,128,buf) && !memcmp(buf,m.image,BLKDEV_READ_BYTES));
    assert(!s->relinks && !m.resets && !s->transfer_fallbacks && m.naks>1000);
    free(buf);end();
    puts("PASS F1-96b same-chain retry: a lost element and a wedged queue are unlinked, retired and relinked once after 200 ms (same TDs/toggles, one CBW); a 1.5 s NAK is waited out");
}

/* 128 -> 8 -> 1 with records; quarantine only after one sector fails. */
static void ladder(const char *path)
{
    /* One or two failed steps (NAK timeout in the data stage). */
    for(unsigned failures=1;failures<=2;failures++) {
        begin(path,1);
        struct usb_storage *s=&device()->storage;
        uint8_t *buf=malloc(BLKDEV_READ_BYTES);assert(buf);
        unsigned reads=m.read_commands;m.read_commands=0;
        m.read_fault_kind=1;m.read_fault_commands=failures;size_t at=mark();
        assert(!block()->read(block(),640,128,buf) && !memcmp(buf,m.image+640*512,BLKDEV_READ_BYTES));
        assert(!block()->quarantined && !device()->quarantined && s->transfer_fallbacks==failures);
        assert(m.resets==failures && m.clear_in==failures && m.clear_out==failures);
        assert(strstr(since(at),"max_sectors addr=1 cmd=28 lba=640 sectors=128 from=128 to=8 reason=data_transfer resend=1 fallbacks=1"));
        if(failures==1) {
            assert(m.read_commands==1+16 && s->max_sectors==8 && queue_limit()==4096);
            /* The lowered size stays for this device; writes follow it. */
            reads=m.read_commands;assert(!block()->read(block(),0,128,buf) && m.read_commands==reads+16);
            unsigned writes=m.writes;assert(!storage_enable_write(storage_get(),4));
            writes=m.writes;memset(buf,0x5a,8192);
            assert(!block()->write(block(),(uint32_t)block()->capacity-16,16,buf) && m.writes==writes+2 && m.max_write_sectors<=8);
        } else {
            assert(strstr(since(at),"max_sectors addr=1 cmd=28 lba=640 sectors=8 from=8 to=1 reason=data_transfer resend=1 fallbacks=2"));
            assert(m.read_commands==1+1+128 && s->max_sectors==1 && queue_limit()==512);
        }
        assert(!strstr(since(at),"quarantine") && !strstr(since(at),"retry addr=1 cmd=28 attempt=2"));
        (void)reads;free(buf);end();
    }
    /* STALL, CSW and timeout faults on every step: quarantine at the end. */
    for(unsigned kind=0;kind<2;kind++) {
        begin(path,1);
        struct usb_storage *s=&device()->storage;
        uint8_t *buf=malloc(BLKDEV_READ_BYTES);assert(buf);
        m.read_commands=0;size_t at=mark();
        if(kind==0) m.nak_mount=true;                                  /* data NAK forever */
        else {m.read_fault_kind=3;m.read_fault_commands=UINT32_MAX;}   /* invalid CSW always */
        assert(block()->read(block(),768,128,buf)==-FS_EIO && block()->quarantined);
        assert(m.read_commands==4 && m.resets==4 && s->transfer_fallbacks==2 && s->max_sectors==1);
        const char *r=since(at);
        const char *step1=strstr(r,"max_sectors addr=1 cmd=28 lba=768 sectors=128 from=128 to=8 ");
        const char *step2=strstr(r,"max_sectors addr=1 cmd=28 lba=768 sectors=8 from=8 to=1 ");
        const char *retry=strstr(r,"retry addr=1 cmd=28 attempt=2");
        const char *quarantine=strstr(r,"[usb-storage] quarantine addr=1 reason=");
        assert(step1 && step2 && retry && quarantine && step1<step2 && step2<retry && retry<quarantine);
        assert(!strstr(quarantine+1,"[usb-storage] quarantine addr=1 "));
        unsigned commands=m.commands;assert(block()->read(block(),0,1,buf)<0 && m.commands==commands);
        free(buf);end();
    }
    /* The device no longer answers the reset: quarantine immediately. */
    begin(path,1);
    uint8_t *buf=malloc(BLKDEV_READ_BYTES);assert(buf);
    m.read_commands=0;m.nak_mount=true;m.reset_nak_ms=UINT32_MAX;size_t at=mark();
    assert(block()->read(block(),0,128,buf)==-FS_EIO && block()->quarantined && m.read_commands==1);
    assert(strstr(since(at),"[usb-storage] reset addr=1 result=-110") && strstr(since(at),"quarantine addr=1 reason=reset_recovery stage=data"));
    assert(!strstr(since(at),"max_sectors addr="));
    free(buf);end();
    /* A reset whose status stage is NAKed for 2 s still answers (Linux
     * allows 5 s); the former 500 ms packet budget quarantined it. */
    begin(path,1);
    struct usb_storage *s=&device()->storage;
    buf=malloc(BLKDEV_READ_BYTES);assert(buf);
    m.read_commands=0;m.read_fault_kind=1;m.read_fault_commands=1;m.reset_nak_ms=2000;at=mark();
    assert(!block()->read(block(),0,128,buf) && !memcmp(buf,m.image,BLKDEV_READ_BYTES));
    assert(!block()->quarantined && m.resets==1 && s->max_sectors==8 && m.read_commands==17);
    assert(strstr(since(at),"[usb-storage] reset addr=1 result=0"));
    free(buf);end();
    puts("PASS F1-96b recovery ladder: 128 -> 8 -> 1 sectors with records after each Reset Recovery, writes follow the lowered size, quarantine only after the one-sector retry fails or the reset is not answered within 5 s");
}

/* The T23 sequence end to end: Files copies a file from E: in 64 KiB
 * chunks while the driver wakes late (drained ring) and the HC misses an
 * element advance every 700 packets. The read side uses a cold cache. */
#define COPY_BYTES (2u<<20)
static void t23_copy(const char *path)
{
    begin(path,9);
    struct storage *st=storage_get();struct usb_storage *u=&device()->storage;
    assert(!storage_enable_write(st,4));
    uint8_t *pattern=malloc(COPY_BYTES),*back=malloc(COPY_BYTES);assert(pattern && back);
    for(unsigned i=0;i<COPY_BYTES;i++) pattern[i]=(uint8_t)(i*167u+(i>>9));
    struct vfs_table *t=calloc(1,sizeof(*t));assert(t && !vfs_table_init(&st->vfs,t,4));
    int h=vfs_open(t,"E:/T23COPY.BIN",VFS_READ|VFS_WRITE|VFS_CREATE|VFS_EXCLUSIVE,VFS_DENY_NONE,0);assert(h>=0);
    size_t done;
    for(unsigned at=0;at<COPY_BYTES;at+=65536) assert(!vfs_write(t,h,pattern+at,65536,&done) && done==65536);
    assert(!vfs_commit(t,h) && !vfs_close(t,h));vfs_table_destroy(t);free(t);
    struct block_cache cache;struct fat_volume vol;static struct vfs v;static struct vfs_table table;
    assert(!cache_init(&cache,2u<<20));
    assert(!fat_prepare_removable(&vol,&cache,block(),0,block()->capacity));
    vol.preparing=false;vfs_init(&v);assert(!vfs_attach(&v,4,&vol));assert(!vfs_table_init(&v,&table,4));
    h=vfs_open(&table,"E:/T23COPY.BIN",VFS_READ,VFS_DENY_NONE,0);assert(h>=0);
    stuck_every=700;stuck_hits=0;dry_frames=0;
    uint64_t start=m.now;unsigned reads=m.read_commands;
    for(unsigned at=0;at<COPY_BYTES;at+=65536) assert(!vfs_read(&table,h,back+at,65536,&done) && done==65536);
    uint64_t ms=m.now-start;
    assert(!memcmp(back,pattern,COPY_BYTES) && !vfs_close(&table,h));
    assert(!block()->quarantined && !m.resets && u->max_sectors==128 && !u->transfer_fallbacks);
    assert(dry_frames && stuck_hits && u->advance_fixes==stuck_hits);
    printf("F1-96b T23 replay wake=9: copy from E: bytes=%u ms=%llu KB/s=%llu READ(10)=%u advance_fixes=%u dry_frames=%u\n",
           COPY_BYTES,(unsigned long long)ms,(unsigned long long)(COPY_BYTES*1000ull/1024/ms),
           m.read_commands-reads,u->advance_fixes,dry_frames);
    vfs_table_destroy(&table);vfs_destroy(&v);cache_destroy(&cache);
    free(pattern);free(back);end();
    puts("PASS F1-96b T23 replay: a 2 MiB copy from E: with a late driver and missed element advances completes without reset, fallback or quarantine");
}

int main(int argc,char **argv)
{
    assert(argc==2);setvbuf(stdout,NULL,_IONBF,0);
    ring_drained(argv[1]);element_erratum(argv[1]);same_chain(argv[1]);ladder(argv[1]);t23_copy(argv[1]);
    puts("PASS F1-96b UHCI stall and recovery ladder suite (ASan/UBSan)");
    return 0;
}
