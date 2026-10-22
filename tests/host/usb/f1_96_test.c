/* f1-96: USB mass-storage transfer size and throughput on the production
 * VFS/FAT/cache/queue/BOT/UHCI path over the 1 ms frame model (at most 16
 * bulk transactions per frame, uhci_model.h). Negotiated 128-sector
 * READ(10)/WRITE(10) CBWs, the TD ring across frames, CSW residue, the
 * fallback after a STALL/CSW error (f1-96b: 128 -> 8 -> 1), the pinned usb-storage queue
 * limit and a 5 MB copy. SPDX-License-Identifier: GPL-2.0-only */
#define main storage_fixture_main
#include "storage_test.c"
#undef main

/* The queue's adaptive split measures the same model clock as the HC. */
uint32_t fs_host_clock_ms(void) { return (uint32_t)m.now; }
#define COPY_BYTES 5458058u /* the T23 b453bc92 Files copy */
#define FRAME_PACKETS 16u

/* Service latency: every driver sleep lasts `wake` frames. wake=3 matches
 * the T23 cadence of about three frames per single-packet BOT phase. */
static unsigned wake=1;
static bool in_wake;
static void wake_latency(void)
{ if(in_wake || wake<=1) return; in_wake=true; sleep_ms(NULL,wake-1); in_wake=false; }

struct cbw_seen { uint8_t op, flags; uint32_t lba, sectors, bytes; };
static struct cbw_seen cbws[256];
static unsigned ncbw;
static void record_cbw(void)
{
    if(ncbw<ARRAY_SIZE(cbws)) cbws[ncbw]=(struct cbw_seen){m.cbw[15],m.cbw[12],be32(m.cbw+17),
        (unsigned)m.cbw[22]*256+m.cbw[23],fs_rd32(m.cbw+8)};
    ncbw++;
}
static const struct cbw_seen *find_cbw(uint8_t op)
{
    for(unsigned i=0;i<ncbw && i<ARRAY_SIZE(cbws);i++) if(cbws[i].op==op) return &cbws[i];
    return NULL;
}
static void begin(const char *path,unsigned latency)
{
    setup(path,false);use_uhci();
    m.h.irq_attached=true;m.interrupt_mask=0x0d;m.native_gate=0x2000;
    wake=latency;sleep_hook=wake_latency;command_hook=record_cbw;ncbw=0;
    mount_device();
}
static void end(void) {sleep_hook=NULL;command_hook=NULL;wake=1;cleanup();}
static uint32_t queue_limit(void)
{ struct blk_queue_stats st;blk_queue_snapshot(&device()->storage.requests,&st);return st.limit_bytes; }
static void fill(uint8_t *b,unsigned n,unsigned seed)
{ for(unsigned i=0;i<n;i++) b[i]=(uint8_t)(i*131u+(i>>9)+seed); }

static void negotiation(const char *path)
{
    begin(path,1);
    struct usb_storage *s=&device()->storage;
    assert(s->max_sectors==USB_STORAGE_MAX_SECTORS && s->max_sectors==128 && s->read_qualified==1);
    assert(strstr(records,"[usb-storage] bind addr=1 ") && strstr(records," read_max=1 max_sectors=128\n"));
    assert(strstr(records,"[usb-storage] cadence addr=1 ") && strstr(records," batch=128\n"));
    assert(queue_limit()==65536);
    uint8_t *data=malloc(BLKDEV_READ_BYTES),*back=malloc(BLKDEV_READ_BYTES);assert(data && back);
    uint32_t lba=(uint32_t)block()->capacity-256;
    /* Nothing longer than one sector has completed as READ(10) yet. */
    fill(data,4096,1);unsigned writes=m.writes;ncbw=0;
    assert(!block()->write(block(),lba,8,data) && m.writes==writes+8 && m.max_write_sectors==1);
    assert(!memcmp(m.image+(size_t)lba*512,data,4096));
    /* One CBW announces the whole 64 KiB READ(10) data stage. */
    ncbw=0;unsigned reads=m.read_commands;
    assert(!block()->read(block(),lba-120,128,back) && m.read_commands==reads+1);
    assert(!memcmp(back,m.image+(size_t)(lba-120)*512,BLKDEV_READ_BYTES));
    const struct cbw_seen *c=find_cbw(0x28);
    assert(c && c->bytes==65536 && c->sectors==128 && c->flags==0x80 && c->lba==lba-120);
    assert(s->read_qualified==128 && m.max_read_sectors==128);
    /* The qualified length now carries WRITE(10) too: one CBW, one command. */
    fill(data,BLKDEV_READ_BYTES,2);ncbw=0;writes=m.writes;
    assert(!block()->write(block(),lba,128,data) && m.writes==writes+1 && m.max_write_sectors==128);
    c=find_cbw(0x2a);assert(c && c->bytes==65536 && c->sectors==128 && !c->flags && c->lba==lba);
    assert(!memcmp(m.image+(size_t)lba*512,data,BLKDEV_READ_BYTES));
    assert(!s->transfer_fallbacks && !strstr(records,"max_sectors addr=") && !m.resets);
    free(data);free(back);end();
    /* A packet-at-a-time host (no chained bulk engine) keeps one page. */
    setup(path,false);command_hook=record_cbw;ncbw=0;mount_device();
    assert(device()->storage.max_sectors==8 && queue_limit()==4096 && strstr(records," read_max=1 max_sectors=8\n"));
    uint8_t page[4096*2];reads=m.read_commands;
    assert(!block()->read(block(),0,16,page) && m.read_commands==reads+2 && m.max_read_sectors==8);
    command_hook=NULL;cleanup();
    puts("PASS F1-96 negotiation: bind max_sectors=128 record, queue limit 64 KiB, one 64 KiB READ(10)/WRITE(10) CBW, writes bounded by the longest completed READ(10); packet-at-a-time host keeps 8 sectors");
}

static void td_ring(const char *path)
{
    for(unsigned latency=1;latency<=3;latency+=2) {
        begin(path,latency);
        struct usb_device *d=device();struct usb_storage *s=&d->storage;
        uint8_t *buf=malloc(BLKDEV_READ_BYTES);assert(buf);
        usb_host_take(&m.h);
        uint8_t cbw[31]={0},csw[13]={0};unsigned n=0;
        fs_wr32(cbw,0x43425355);fs_wr32(cbw+4,0x96);fs_wr32(cbw+8,BLKDEV_READ_BYTES);
        cbw[12]=0x80;cbw[14]=10;cbw[15]=0x28;put32(cbw+17,0);cbw[22]=0;cbw[23]=128;
        assert(!usb_bulk(d,false,cbw,sizeof(cbw),&n) && n==sizeof(cbw));
        unsigned toggle=s->in_toggle;uint64_t start=m.now;
        assert(!usb_bulk(d,true,buf,BLKDEV_READ_BYTES,&n) && n==BLKDEV_READ_BYTES);
        uint64_t frames=m.now-start;
        assert(!memcmp(buf,m.image,BLKDEV_READ_BYTES));
        /* 1024 packets through 128 TDs: every slot was re-armed seven times
         * while the HC kept running; the frame budget is the only limit. */
        struct usb_storage_trace *t=&s->trace;
        assert(t->packet_count==1024 && t->packet_count>USB_BULK_BATCH && !strcmp(t->bulk_stop,"complete"));
        for(unsigned i=0;i<USB_BULK_TRACE_LIMIT;i++)
            assert(t->packets[i].valid && t->packets[i].actual==64 && t->packets[i].toggle==(toggle^(i&1)));
        assert(s->in_toggle==toggle && frames>=1024/FRAME_PACKETS && frames<=1024/FRAME_PACKETS+2*latency);
        unsigned packets=t->packet_count;
        assert(!usb_bulk(d,true,csw,sizeof(csw),&n) && n==sizeof(csw));
        assert(fs_rd32(csw)==0x53425355 && fs_rd32(csw+4)==0x96 && !fs_rd32(csw+8) && !csw[12]);
        usb_host_drop(&m.h);
        printf("F1-96 TD ring wake=%u: 64 KiB data stage packets=%u ring=%u frames=%llu (budget %u)\n",
               latency,packets,USB_BULK_BATCH,(unsigned long long)frames,1024/FRAME_PACKETS);
        free(buf);end();
    }
    puts("PASS F1-96 TD chain across frames: 1024-packet stage streams through the 128-TD ring at the full-speed frame budget, toggles and CSW intact");
}

static void residue(const char *path)
{
    for(unsigned kind=0;kind<3;kind++) {
        begin(path,1);
        struct usb_storage *s=&device()->storage;
        if(kind==0) {m.transfer_limit=8;m.limit_reaction=3;}        /* 4 KiB, ZLP, passed CSW */
        else if(kind==1) m.read_bad_residue=1;                        /* residue > dCBWDataTransferLength */
        else {m.transfer_limit=100;m.limit_reaction=3;}               /* ZLP in a re-armed ring slot */
        uint8_t *buf=malloc(BLKDEV_READ_BYTES);assert(buf);
        unsigned reads=m.read_commands;
        assert(!block()->read(block(),64,128,buf) && !memcmp(buf,m.image+64*512,BLKDEV_READ_BYTES));
        /* f1-96b: the ladder's next size, 8 sectors, is within every limit. */
        assert(!block()->quarantined && s->max_sectors==8 && s->transfer_fallbacks==1 && queue_limit()==4096);
        assert(m.read_commands==reads+1+16 && m.resets==1 && m.clear_in==1 && m.clear_out==1);
        if(kind!=1) {
            assert(strstr(records,"reason=short_command tag=") &&
                   strstr(records,kind==0 ? "csw=1 status=0 residue=61440" : "csw=1 status=0 residue=14336"));
            assert(strstr(records,"max_sectors addr=1 cmd=28 lba=64 sectors=128 from=128 to=8 reason=short_command resend=1 fallbacks=1"));
        } else {
            assert(strstr(records,"csw_tag=") && strstr(records,"residue=65537"));
            assert(strstr(records,"max_sectors addr=1 cmd=28 lba=64 sectors=128 from=128 to=8 reason=invalid_csw resend=1 fallbacks=1"));
        }
        free(buf);end();
    }
    puts("PASS F1-96 CSW residue: short 64 KiB READ(10) (in the first window and after the ring wrapped) with consistent residue, and a residue beyond dCBWDataTransferLength, reset, fall back to 8 sectors (f1-96b ladder) and re-read the range");
}

static void stall_fallback(const char *path)
{
    static const char *reasons[]={0,"data_stall","phase_error",0,"cbw"};
    for(unsigned reaction=1;reaction<=4;reaction++) {
        if(reaction==3) continue; /* short data: residue() */
        begin(path,1);
        struct usb_storage *s=&device()->storage;
        m.transfer_limit=64;m.limit_reaction=reaction;
        uint8_t *buf=malloc(BLKDEV_READ_BYTES);assert(buf);
        unsigned reads=m.read_commands;
        assert(!block()->read(block(),200,128,buf) && !memcmp(buf,m.image+200*512,BLKDEV_READ_BYTES));
        assert(!block()->quarantined && !device()->quarantined && s->max_sectors==8 && queue_limit()==4096);
        char needle[160];
        snprintf(needle,sizeof(needle),"max_sectors addr=1 cmd=28 lba=200 sectors=128 from=128 to=8 reason=%s resend=1 fallbacks=1",reasons[reaction]);
        assert(strstr(records,needle));
        assert(m.read_commands==reads+(reaction==4 ? 16u : 17u)); /* a STALLed CBW starts no READ(10) */
        assert(m.resets==(reaction==1 ? 0u : 1u) && m.max_read_sectors==(reaction==4 ? 8u : 128u));
        if(reaction==1) assert(m.clear_in==1 && strstr(records,"scsi addr=1 cmd=28 sense=05/24/00"));
        /* The queue now admits 8 sectors per command; no second fallback.
         * Completed 8-sector READ(10)s qualify 8-sector WRITE(10)s. */
        reads=m.read_commands;
        assert(!block()->read(block(),400,128,buf) && !memcmp(buf,m.image+400*512,BLKDEV_READ_BYTES));
        assert(m.read_commands==reads+16 && s->transfer_fallbacks==1);
        unsigned writes=m.writes;fill(buf,4096,3);
        assert(!block()->write(block(),(uint32_t)block()->capacity-16,8,buf) && m.writes==writes+1 && m.max_write_sectors==8);
        assert(!memcmp(m.image+((size_t)block()->capacity-16)*512,buf,4096));
        free(buf);end();
    }
    /* A multi-sector WRITE(10) failure is recorded but never resent. */
    begin(path,1);
    struct usb_storage *s=&device()->storage;
    uint8_t *buf=malloc(BLKDEV_READ_BYTES);assert(buf);
    m.transfer_limit=32;m.limit_writes_only=true;m.limit_reaction=1;
    assert(!block()->read(block(),0,128,buf) && s->read_qualified==128);
    unsigned writes=m.writes;fill(buf,BLKDEV_READ_BYTES,4);
    uint32_t lba=(uint32_t)block()->capacity-128;
    assert(block()->write(block(),lba,64,buf)<0 && block()->quarantined && m.writes==writes+1);
    char needle[160];
    snprintf(needle,sizeof(needle),"max_sectors addr=1 cmd=2a lba=%u sectors=64 from=128 to=8 reason=data_stall resend=0 fallbacks=1",lba);
    assert(strstr(records,needle) && strstr(records,"quarantine addr=1 reason=write_failed"));
    unsigned commands=m.commands;
    assert(block()->write(block(),lba,1,buf)<0 && m.commands==commands && m.writes==writes+1);
    free(buf);end();
    puts("PASS F1-96 STALL/CSW fallback: data STALL + ILLEGAL REQUEST, phase error and CBW STALL step to 8 sectors (f1-96b ladder) with a record and re-read the range; queue limit 4096; a failed multi-sector write is recorded and never resent");
}

/* 5 MB copy to and from the stick in Files' 64 KiB chunks. The read side
 * uses a private cold cache so every data byte crosses the bus. */
static void copy(const char *path,unsigned latency,bool old_policy,uint64_t *read_out)
{
    begin(path,latency);
    struct storage *s=storage_get();struct usb_storage *u=&device()->storage;
    if(old_policy) blk_queue_set_limits(&u->requests,65536,0); /* f1-83 adaptive split, one-page floor */
    assert(!storage_enable_write(s,4));
    /* The write-enable FAT scan reads whole 32 KiB FATs: writes may use 64 sectors. */
    assert(old_policy || u->read_qualified>=64);
    uint8_t *pattern=malloc(COPY_BYTES),*back=malloc(COPY_BYTES);assert(pattern && back);
    fill(pattern,COPY_BYTES,5);
    struct vfs_table *t=calloc(1,sizeof(*t));assert(t && !vfs_table_init(&s->vfs,t,4));
    int h=vfs_open(t,"E:/COPY.BIN",VFS_READ|VFS_WRITE|VFS_CREATE|VFS_EXCLUSIVE,VFS_DENY_NONE,0);assert(h>=0);
    uint64_t start=m.now;unsigned writes=m.writes;size_t done;
    for(unsigned at=0;at<COPY_BYTES;at+=65536) {
        unsigned n=COPY_BYTES-at<65536 ? COPY_BYTES-at : 65536;
        assert(!vfs_write(t,h,pattern+at,n,&done) && done==n);
    }
    assert(!vfs_commit(t,h) && !vfs_close(t,h));
    uint64_t write_ms=m.now-start;writes=m.writes-writes;
    vfs_table_destroy(t);free(t);
    struct block_cache cache;struct fat_volume vol;static struct vfs v;static struct vfs_table table;
    assert(!cache_init(&cache,2u<<20));
    assert(!fat_prepare_removable(&vol,&cache,block(),0,block()->capacity));
    vol.preparing=false;vfs_init(&v);assert(!vfs_attach(&v,4,&vol));assert(!vfs_table_init(&v,&table,4));
    h=vfs_open(&table,"E:/COPY.BIN",VFS_READ,VFS_DENY_NONE,0);assert(h>=0);
    m.max_read_sectors=0;start=m.now;unsigned reads=m.read_commands;
    for(unsigned at=0;at<COPY_BYTES;at+=65536) {
        unsigned n=COPY_BYTES-at<65536 ? COPY_BYTES-at : 65536;
        assert(!vfs_read(&table,h,back+at,n,&done) && done==n);
    }
    uint64_t read_ms=m.now-start;reads=m.read_commands-reads;
    assert(!memcmp(back,pattern,COPY_BYTES) && !vfs_close(&table,h));
    uint32_t limit=queue_limit();
    printf("F1-96 copy wake=%u policy=%s: to stick bytes=%u ms=%llu KB/s=%llu WRITE(10)=%u max=%u; "
           "from stick ms=%llu KB/s=%llu READ(10)=%u max=%u; queue limit=%u\n",
           latency,old_policy ? "f1-83-adaptive" : "f1-96-pinned",COPY_BYTES,(unsigned long long)write_ms,
           (unsigned long long)(COPY_BYTES*1000ull/1024/write_ms),writes,m.max_write_sectors,
           (unsigned long long)read_ms,(unsigned long long)(COPY_BYTES*1000ull/1024/read_ms),reads,
           m.max_read_sectors,limit);
    if(!old_policy) {
        assert(limit==65536 && m.max_read_sectors==128 && !u->transfer_fallbacks && !m.resets);
        assert(read_ms<8000);
    }
    *read_out=read_ms;
    vfs_table_destroy(&table);vfs_destroy(&v);cache_destroy(&cache);
    free(pattern);free(back);end();
}
static void throughput(const char *path)
{
    uint64_t ms[2][2];
    for(unsigned latency=1;latency<=3;latency+=2)
        for(unsigned old=0;old<2;old++) copy(path,latency,old,&ms[latency/2][old]);
    /* Negative control: the one-page adaptive floor reproduces the T23
     * limit=4096 split and misses the budget with T23 service latency. */
    assert(ms[1][1]>=8000 && ms[1][1]>ms[1][0]+ms[1][0]/2 && ms[0][1]>ms[0][0]);
    puts("PASS F1-96 5 MB copy from the stick under 8 s at the full-speed frame budget (wake 1 and 3); the old adaptive queue floor does not meet it");
}

int main(int argc,char **argv)
{
    assert(argc==2);
    negotiation(argv[1]);td_ring(argv[1]);residue(argv[1]);stall_fallback(argv[1]);throughput(argv[1]);
    puts("PASS F1-96 USB storage throughput suite (ASan/UBSan)");
    return 0;
}
