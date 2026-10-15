/* Production BOT/UHCI/FAT/VFS/disk log, with production scheduler run queues.
 * Only HC registers, fixed disk bytes and stack switching are modeled.
 * SPDX-License-Identifier: GPL-2.0-only */
#define main storage_fixture_main
#include "storage_test.c"
#undef main
#include <ciuki/task.h>
#include <ciuki/bootlog.h>
/* The fixtures contain FAT partitions only. */
const struct fs_ro_ops ntfs_ops={0};
volatile uint64_t g_ticks;
#include "usb_bind_queues.inc"

static struct task desktop_task, writer_task, other_usb_task;
static unsigned desktop_turns, writer_turns, other_turns, durable_flushes;
static bool fixed_flush_error;
static uint64_t hold_until;
static bool desktop_holding;
static bool other_reserving;
static uint64_t reservation_until;
static bool writer_paused;
static struct blkdev fixed;
static uint8_t *fixed_image, fixed_mbr[512];
static unsigned fixed_offset;
static int fixed_io(struct blkdev *b,uint64_t lba,uint32_t n,void *buf,bool write)
{
    int e=blkdev_range(b,lba,n);if(e)return e;
    while(n--) {
        uint8_t *p=lba<fixed_offset ? fixed_mbr : fixed_image+(lba-fixed_offset)*512;
        if(write)memcpy(p,buf,512);else memcpy(buf,p,512);
        lba++;buf=(uint8_t *)buf+512;
    }
    return 0;
}
static int fixed_read(struct blkdev *b,uint64_t lba,uint32_t n,void *p)
{return fixed_io(b,lba,n,p,false);}
static int fixed_write(struct blkdev *b,uint64_t lba,uint32_t n,const void *p)
{return fixed_io(b,lba,n,(void *)p,true);}
static int fixed_flush(struct blkdev *b) {(void)b;durable_flushes++;return fixed_flush_error ? -FS_EIO : 0;}
static void replay(void)
{
    /* usb_sleep must release its short DMA mutex even when the general slot
     * remains reserved. All three peers become ready while USB is BLOCKED. */
    assert(!pthread_mutex_trylock(&m.h.transfer_lock));fs_lock_drop(&m.h.transfer_lock);
    assert(fs_lock_try_take(&storage_get()->cache.lock));fs_lock_drop(&storage_get()->cache.lock);
    assert(fs_lock_try_take(&storage_get()->cache.workspace_lock));fs_lock_drop(&storage_get()->cache.workspace_lock);
    g_ticks=m.now;
    rq_push(&desktop_task);rq_push(&writer_task);rq_push(&other_usb_task);
    struct task *t;unsigned dispatched=0;
    while((t=rq_pop())) {
        dispatched++;
        if(t==&desktop_task) {
            if(desktop_holding && m.now>=hold_until) {
                fs_lock_drop(&storage_get()->vfs.lock);desktop_holding=false;
            }
            if(!desktop_holding) {
                assert(fs_lock_try_take(&storage_get()->vfs.lock));
                desktop_turns++;fs_lock_drop(&storage_get()->vfs.lock);
            }
        } else if(t==&writer_task) {
            writer_turns++;
            if(!desktop_holding && !writer_paused) {int e=bootlog_drain();assert(!e || e==-FS_EBUSY);}
        } else {
            assert(t==&other_usb_task);other_turns++;
            if(other_reserving && m.now>=reservation_until) {m.h.busy=false;other_reserving=false;}
        }
    }
    assert(dispatched==3);
}
static void activate_log(void)
{
    fixed_image=m.image;fixed_offset=m.offset;memcpy(fixed_mbr,m.mbr,sizeof(fixed_mbr));
    fixed=(struct blkdev){.read=fixed_read,.write=fixed_write,.flush=fixed_flush,
        .capacity=m.bytes/512+m.offset,.sector_size=512,.write_cache_state=BLKDEV_CACHE_ENABLED};
    assert(!storage_add_disk(storage_get(),0,&fixed));
    struct fat_volume *fat=&storage_get()->volumes[2].fat;
    assert(!fat_enable_write(fat));
    struct fat_entry system,logs;
    assert(!fat_create(fat,0,"SYSTEM",FAT_ATTR_DIR,&system));
    assert(!fat_create(fat,system.first,"LOGS",FAT_ATTR_DIR,&logs));
    assert(!fat_commit(fat));
    assert(!bootlog_activate(&storage_get()->vfs,true,storage_get()->sequence));
    log_hook=bootlog_capture;
}
static void check_disk_record(const char *needle)
{
    struct vfs_table table;assert(!vfs_table_init(&storage_get()->vfs,&table,2));
    int fd=vfs_open(&table,BOOTLOG_PATH,VFS_READ,VFS_DENY_NONE,0);assert(fd>=0);
    char bytes[4096];size_t n,keep=0;bool found=false;
    assert(strlen(needle)<256);
    do {
        assert(!vfs_read(&table,fd,bytes+keep,sizeof(bytes)-keep-1,&n));
        bytes[keep+n]=0;
        if(strstr(bytes,needle))found=true;
        keep=keep+n<255 ? keep+n : 255;
        memmove(bytes,bytes+strlen(bytes)-keep,keep);
    } while(n);
    assert(found);assert(!vfs_close(&table,fd));vfs_table_destroy(&table);
}
static void scenario(const char *path,unsigned kind)
{
    setup(path,true);activate_log();use_uhci();
    desktop_task=(struct task){.prio=P_NORMAL};writer_task=(struct task){.prio=P_DEVICE};
    other_usb_task=(struct task){.prio=P_DEVICE};
    desktop_turns=writer_turns=other_turns=0;desktop_holding=false;hold_until=0;
    other_reserving=false;
    switch(kind) {
    case 0:m.forever_data=true;break;
    case 1:m.forever_csw=true;break;
    case 2:m.forever_stall=true;break;
    case 3:m.nak_capacity=true;break;
    case 4:m.nak_mount=true;break;
    case 5:m.slow_all=true;break;
    case 6: /* A desktop client owns VFS during the first synchronous record. */
        fs_lock_take(&storage_get()->vfs.lock);desktop_holding=true;hold_until=30; m.forever_data=true;break;
    case 7: /* FAT scan I/O, including its reserved workspace, must not hold a mutex. */
        break;
    case 8:case 9: /* Contended reservation: finite and permanent owners. */
        m.h.busy=true;other_reserving=true;reservation_until=kind==8 ? 30 : USB_BIND_TIMEOUT_MS+10;
        m.forever_data=true;break;
    case 10:m.never_ready=true;break;
    }
    sleep_hook=replay;
    uint64_t begin=m.now;
    int e=usb_storage_attach(device());
    if(!e) {usb_storage_service(&m.h);e=block()->quarantined ? -FS_EIO : 0;}
    sleep_hook=NULL;
    assert(e && m.now-begin<=USB_BIND_TIMEOUT_MS);
    assert(block()->quarantined && device()->quarantined && !device()->storage.online);
    assert(m.h.ready && !m.h.quarantined && !device()->storage.bind_reserved);
    assert(m.h.busy==(kind==9)); /* timeout must not drop the other task's reservation */
    assert(desktop_turns && writer_turns && other_turns && !desktop_holding);
    assert(strstr(records,"[usb-storage] quarantine addr=1"));
    check_disk_record("[usb-storage] stage=max_lun addr=1\n");
    if(kind!=9)check_disk_record("[usb-storage] stage=inquiry addr=1\n");
    check_disk_record("[usb-storage] quarantine addr=1");
    if(kind==4) {
        check_disk_record("[usb-storage] stage=mount addr=1\n");
        assert(!storage_get()->vfs.volumes[4]);
    }
    if(kind==10) {
        assert(m.now-begin>=USB_BIND_TIMEOUT_MS-USB_BIND_FINISH_MS);
        check_disk_record("quarantine addr=1 reason=bind_deadline stage=ready");
        check_disk_record("stage=ready addr=1 elapsed_ms=");
        assert(!device()->storage.published && !storage_get()->vfs.volumes[4]);
        assert(!m.writes && !m.resets);
    }
    unsigned commands=m.commands;uint8_t b[512];
    assert(block()->read(block(),0,1,b)<0 && m.commands==commands);
    if(other_reserving) {m.h.busy=false;other_reserving=false;}
    assert(!bootlog_shutdown());log_hook=NULL;bootlog_reset();cleanup();
    printf("PASS F1-59 bind fault=%u elapsed_ms=%llu desktop=%u writer=%u other_usb=%u durable_flushes=%u\n",
           kind,(unsigned long long)(m.now-begin),desktop_turns,writer_turns,other_turns,durable_flushes);
}
static uint64_t stage_elapsed(const char *name)
{
    char needle[96];snprintf(needle,sizeof(needle),"[usb-storage] stage=%s addr=1 elapsed_ms=",name);
    const char *p=strstr(records,needle);assert(p);p+=strlen(needle);
    char *end;uint64_t elapsed=strtoull(p,&end,10);assert(end!=p);
    assert(!strncmp(end," result=0\n",10));
    check_disk_record(needle);return elapsed;
}
static void small_stick(void)
{
    static uint8_t stick[64*512];memset(stick,0,sizeof(stick));
    stick[0]=0xeb;stick[1]=0x3c;stick[2]=0x90;
    fs_wr16(stick+11,512);stick[13]=1;fs_wr16(stick+14,1);stick[16]=2;
    fs_wr16(stick+17,16);fs_wr16(stick+19,64);stick[21]=0xf8;fs_wr16(stick+22,1);
    fs_wr16(stick+510,0xaa55);
    for(unsigned i=1;i<=2;i++) {stick[i*512]=0xf8;stick[i*512+1]=stick[i*512+2]=0xff;}
    for(unsigned i=1;i<=2;i++) {stick[i*512+3]=0xff;stick[i*512+4]=0x0f;}
    memcpy(stick+3*512,"SHORT   BIN",11);stick[3*512+11]=FAT_ATTR_ARCHIVE;
    fs_wr16(stick+3*512+26,2);fs_wr32(stick+3*512+28,512);memset(stick+4*512,0x5a,512);
    m.image=stick;m.bytes=sizeof(stick);m.mbr[450]=1;fs_wr32(m.mbr+458,64);
}
static void successful_bind(const char *path,bool slow)
{
    setup(path,true);activate_log();use_uhci();
    uint8_t *original=m.image;size_t original_bytes=m.bytes;
    small_stick();
    if(slow) {m.ready_at=m.now+4000;m.flush_delay=2000;m.first_read_delay=2000;}
    desktop_holding=false;desktop_turns=writer_turns=other_turns=0;sleep_hook=replay;
    assert(!usb_storage_attach(device()));usb_storage_service(&m.h);sleep_hook=NULL;
    assert(device()->storage.online && !block()->quarantined && !device()->storage.binding);
    assert(m.now<=USB_BIND_TIMEOUT_MS && storage_get()->vfs.volumes[4]);
    check_disk_record("[usb-storage] stage=capacity addr=1\n");
    check_disk_record("[usb-storage] stage=flush addr=1\n");
    check_disk_record("[usb-storage] stage=mount addr=1\n");
    const char *stages[]={"max_lun","inquiry","ready","capacity","flush","mode","mount"};
    uint64_t total=0,ready_ms=0,flush_ms=0,mount_ms=0;
    for(unsigned i=0;i<ARRAY_SIZE(stages);i++) {
        uint64_t elapsed=stage_elapsed(stages[i]);
        if(i==2)ready_ms=elapsed;if(i==4)flush_ms=elapsed;if(i==6)mount_ms=elapsed;
        else total+=elapsed;
    }
    const char *p=strstr(records,"[usb-storage] ready addr=1 sectors=");assert(p);
    p=strstr(p," elapsed_ms=");assert(p);
    assert(strtoull(p+12,NULL,10)==total);
    if(slow) {
        assert(ready_ms>=3900 && ready_ms<4200 && flush_ms>=2000 && flush_ms<2100);
        assert(mount_ms>=2000 && m.ready_commands>3 && device()->storage.published);
        struct vfs_table table;assert(!vfs_table_init(&storage_get()->vfs,&table,4));
        int fd=vfs_open(&table,"E:/SHORT.BIN",VFS_READ,VFS_DENY_NONE,0);assert(fd>=0);
        uint8_t data[512];size_t got;assert(!vfs_read(&table,fd,data,sizeof(data),&got) && got==sizeof(data));
        for(unsigned i=0;i<sizeof(data);i++)assert(data[i]==0x5a);
        assert(!vfs_close(&table,fd));vfs_table_destroy(&table);
    }
    assert(!usb_storage_eject(storage_get(),4,block()->media_generation));
    assert(!bootlog_shutdown());log_hook=NULL;bootlog_reset();
    m.image=original;m.bytes=original_bytes;cleanup();
    printf("PASS F1-62 real-UHCI FAT12 bind slow=%u ready_ms=%llu flush_ms=%llu mount_ms=%llu elapsed_ms=%llu volume=E: desktop=%u writer=%u other_usb=%u\n",
           slow,(unsigned long long)ready_ms,(unsigned long long)flush_ms,(unsigned long long)mount_ms,
           (unsigned long long)m.now,desktop_turns,writer_turns,other_turns);
}
static unsigned hid_polls,hid_naks,hid_reports;
static bool receiver_transaction(uint32_t address,uint32_t qh,volatile uint32_t *td)
{
    assert(qh==m.h.dma_phys+USB_HID_BASE || qh==m.h.dma_phys+USB_HID_BASE+USB_HID_STRIDE);
    assert(address==qh+16 && ((td[2]>>8)&127)==1 && (td[2]&255)==USB_PID_IN);
    /* Idle receiver polls NAK, with occasional reports to exercise retirement. */
    if(m.now%40) {td[1]|=1u<<19;hid_naks++;return false;}
    unsigned buffer=td[3]-m.h.dma_phys;assert(buffer<=USB_DMA_BYTES-8);
    memset(m.dma+buffer,0,8);
    td[1]&=~(0xffu<<16|0x7ffu);td[1]|=7;
    return true;
}
static void receiver_poll(struct usb_device *d,struct usb_hid *p)
{
    if(d->address!=1 || !p->endpoint)return;
    uint8_t report[8];hid_polls++;
    int e=usb_interrupt(d,p,report,sizeof(report));
    if(e>=0) {assert(e==8);p->toggle^=1;hid_reports++;}
    else assert(e==-USB_EAGAIN);
    if(p->trace.valid && p->poll_records<20) {
        p->poll_records++;
        klog("[usb] hid poll addr=1 interface=%u ep=%u interval=%u toggle=%u maxlen=8 result=%d controller=usb-2807 root=1 parent=0",
             p->interface,p->endpoint,p->interval,p->toggle,e);
    }
}
static void fail_log_commit(void)
{
    assert(bootlog_try_sync()==-FS_EAGAIN);
    fixed_flush_error=true;assert(bootlog_drain()==-FS_EIO);fixed_flush_error=false;
    struct bootlog_stats stats;bootlog_snapshot(&stats);
    assert(stats.error==-FS_EIO && !stats.active);
}
static void logging_backpressure(const char *path,unsigned kind)
{
    setup(path,true);activate_log();use_uhci();
    uint8_t *original=m.image;size_t original_bytes=m.bytes;
    small_stick();
    /* Address 1 occupies root port 1 before address 2 starts on port 2. */
    m.h.devices[1]=m.h.devices[0];m.storage_slot=1;
    device()->address=2;device()->root_port=device()->port=1;
    m.h.devices[0]=(struct usb_device){.host=&m.h,.present=true,.configured=true,.address=1};
    struct usb_device *receiver=&m.h.devices[0];
    receiver->hid[0]=(struct usb_hid){.active=true,.protocol=1,.endpoint=1,.packet=8,.interval=8};
    receiver->hid[1]=(struct usb_hid){.active=true,.protocol=2,.interface=1,.endpoint=2,.packet=8,.interval=2};
    m.h.device_count=2;m.h.ports=2;
    for(unsigned i=0;i<2;i++) {
        uint32_t *qh=(uint32_t *)(m.dma+USB_HID_BASE+i*USB_HID_STRIDE);
        qh[0]=(m.h.dma_phys+(i ? USB_UHCI_CONTROL_QH : USB_HID_BASE+USB_HID_STRIDE))|2;qh[1]=1;
    }
    for(unsigned i=0;i<1024;i++)((uint32_t *)m.dma)[i]=(m.h.dma_phys+USB_HID_BASE)|2;
    hid_polls=hid_naks=hid_reports=0;hid_poll_hook=receiver_poll;private_transaction_hook=receiver_transaction;
    desktop_task=(struct task){.prio=P_NORMAL};writer_task=(struct task){.prio=P_DEVICE};
    other_usb_task=(struct task){.prio=P_DEVICE};
    desktop_turns=writer_turns=other_turns=0;desktop_holding=other_reserving=false;
    writer_paused=true;sleep_hook=replay;
    for(unsigned i=0;i<300;i++) {
        usb_host_take(&m.h);usb_hid_service(&m.h);usb_sleep(&m.h,1);usb_host_drop(&m.h);
    }
    assert(hid_polls && hid_naks && hid_reports && strstr(records,"hid poll addr=1"));
    if(kind==1) {
        static const char bytes[1024]={0};
        for(unsigned i=0;i<BOOTLOG_RAM/sizeof(bytes)+1;i++)bootlog_capture_ring(bytes,sizeof(bytes));
    }
    if(kind==2)fail_log_commit();
    if(kind==4)assert(!bootlog_shutdown());
    /* The real logger has pending bytes and cannot commit for over 2 seconds.
     * Previously max_lun quarantined here with -EAGAIN and no submitted TD. */
    uint64_t begin=m.now;unsigned polls=hid_polls,reports_before=hid_reports;
    assert(!usb_storage_attach(device()));
    uint64_t attach_ms=m.now-begin;
    assert(attach_ms<BOOTLOG_SYNC_TIMEOUT_MS && m.inquiries==1);
    assert(strstr(records,"[usb-storage] stage=inquiry addr=2\n"));
    if(kind==3)fail_log_commit(); /* logger fails only after transport probing */
    usb_storage_service(&m.h);
    assert(device()->storage.published && storage_get()->vfs.volumes[4]);
    assert(!device()->quarantined && !block()->quarantined && !m.h.quarantined && !m.h.busy);
    assert(!strstr(records,"[usb-storage] quarantine") && hid_polls>polls && hid_reports>reports_before);
    assert(strstr(records,kind==2 ? "stage_record addr=2 stage=max_lun record=lost result=-5" :
                                 "stage_record addr=2 stage=max_lun record=lost result=-11"));
    if(kind==1) {
        char occupancy[96];
        snprintf(occupancy,sizeof(occupancy),"ring_queued=%u ring_capacity=%u",BOOTLOG_RAM,BOOTLOG_RAM);
        assert(strstr(records,occupancy));
    }
    if(kind==3)assert(strstr(records,"stage_record addr=2 stage=mount record=lost result=-5"));
    char capacity[32];snprintf(capacity,sizeof(capacity),"ring_capacity=%u",BOOTLOG_RAM);
    assert(strstr(records,"ring_queued=") && strstr(records,capacity) && strstr(records,"boot_queued="));
    writer_paused=false;sleep_hook=NULL;
    struct vfs_table table;assert(!vfs_table_init(&storage_get()->vfs,&table,4));
    int fd=vfs_open(&table,"E:/SHORT.BIN",VFS_READ,VFS_DENY_NONE,0);assert(fd>=0);
    uint8_t data[512];size_t got;assert(!vfs_read(&table,fd,data,sizeof(data),&got) && got==sizeof(data));
    for(unsigned i=0;i<sizeof(data);i++)assert(data[i]==0x5a);
    assert(!vfs_close(&table,fd));vfs_table_destroy(&table);
    if(kind<2) {
        struct bootlog_stats stats;
        /* Request the final captured watermark before checking disk records;
         * an ordinary drain can be deferred by f0-22's one-second cadence. */
        int e=bootlog_try_sync();assert(!e || e==-FS_EAGAIN);
        do {assert(!bootlog_drain());bootlog_snapshot(&stats);}while(stats.queued);
        check_disk_record("[usb-storage] stage=inquiry addr=2\n");
        check_disk_record("[usb-storage] stage=mount addr=2 elapsed_ms=");
    }
    assert(!usb_storage_eject(storage_get(),4,block()->media_generation));
    assert(bootlog_shutdown()==(kind==2 || kind==3 ? -FS_EIO : 0));log_hook=NULL;bootlog_reset();
    hid_poll_hook=NULL;private_transaction_hook=NULL;
    m.image=original;m.bytes=original_bytes;cleanup();
    const char *names[]={"paused","ring_full","failed_before_probe","failed_at_mount","inactive"};
    printf("PASS F1-64 HID addr=1 then storage addr=2 on one UHCI, logger=%s, inquiry/mount/read, lost record occupancy; attach_ms=%llu HID polls=%u NAKs=%u reports=%u\n",
           names[kind],(unsigned long long)attach_ms,hid_polls,hid_naks,hid_reports);
}
int main(int argc,char **argv)
{
    assert(argc==4);
    for(unsigned kind=0;kind<5;kind++)logging_backpressure(argv[2],kind);
    for(unsigned kind=0;kind<11;kind++)scenario(argv[2],kind);
    successful_bind(argv[2],false);successful_bind(argv[2],true);
    puts("PASS F1-59 hostile bind, VFS client/disk writer, scheduler queues and durable stage records");
    return 0;
}
