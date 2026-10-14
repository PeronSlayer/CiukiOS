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
static uint64_t hold_until;
static bool desktop_holding;
static bool other_reserving;
static uint64_t reservation_until;
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
static int fixed_flush(struct blkdev *b) {(void)b;durable_flushes++;return 0;}
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
            if(!desktop_holding) {int e=bootlog_drain();assert(!e || e==-FS_EBUSY);}
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
    char bytes[16384]={0};size_t n;
    assert(!vfs_read(&table,fd,bytes,sizeof(bytes)-1,&n));
    assert(strstr(bytes,needle));assert(!vfs_close(&table,fd));vfs_table_destroy(&table);
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
    unsigned commands=m.commands;uint8_t b[512];
    assert(block()->read(block(),0,1,b)<0 && m.commands==commands);
    if(other_reserving) {m.h.busy=false;other_reserving=false;}
    assert(!bootlog_shutdown());log_hook=NULL;bootlog_reset();cleanup();
    printf("PASS F1-59 bind fault=%u elapsed_ms=%llu desktop=%u writer=%u other_usb=%u durable_flushes=%u\n",
           kind,(unsigned long long)(m.now-begin),desktop_turns,writer_turns,other_turns,durable_flushes);
}
static void successful_bind(const char *path)
{
    setup(path,true);activate_log();use_uhci();
    uint8_t *original=m.image;size_t original_bytes=m.bytes;
    static uint8_t stick[64*512];memset(stick,0,sizeof(stick));
    stick[0]=0xeb;stick[1]=0x3c;stick[2]=0x90;
    fs_wr16(stick+11,512);stick[13]=1;fs_wr16(stick+14,1);stick[16]=2;
    fs_wr16(stick+17,16);fs_wr16(stick+19,64);stick[21]=0xf8;fs_wr16(stick+22,1);
    fs_wr16(stick+510,0xaa55);
    for(unsigned i=1;i<=2;i++) {stick[i*512]=0xf8;stick[i*512+1]=stick[i*512+2]=0xff;}
    m.image=stick;m.bytes=sizeof(stick);m.mbr[450]=1;fs_wr32(m.mbr+458,64);
    desktop_holding=false;desktop_turns=writer_turns=other_turns=0;sleep_hook=replay;
    assert(!usb_storage_attach(device()));usb_storage_service(&m.h);sleep_hook=NULL;
    assert(device()->storage.online && !block()->quarantined && !device()->storage.binding);
    assert(m.now<=USB_BIND_TIMEOUT_MS && storage_get()->vfs.volumes[4]);
    check_disk_record("[usb-storage] stage=capacity addr=1\n");
    check_disk_record("[usb-storage] stage=flush addr=1\n");
    check_disk_record("[usb-storage] stage=mount addr=1\n");
    assert(!usb_storage_eject(storage_get(),4,block()->media_generation));
    assert(!bootlog_shutdown());log_hook=NULL;bootlog_reset();
    m.image=original;m.bytes=original_bytes;cleanup();
    printf("PASS F1-59 successful real-UHCI FAT12 bind elapsed_ms=%llu volume=E: desktop=%u writer=%u\n",
           (unsigned long long)m.now,desktop_turns,writer_turns);
}
int main(int argc,char **argv)
{
    assert(argc==4);
    for(unsigned kind=0;kind<10;kind++)scenario(argv[2],kind);
    successful_bind(argv[2]);
    puts("PASS F1-59 hostile bind, VFS client/disk writer, scheduler queues and durable stage records");
    return 0;
}
