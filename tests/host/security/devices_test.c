/* H05: real native dispatch, disk driver/lease, media, time and recovery.
 * Only physical I/O and maintenance engines are spies. GPL-2.0-only */
#define CIUKI_LOCALTIME_HOST_MAIN unused_localtime_main
#include "../proc/localtime_test.c"
#undef main
#include "../../../src/kernel/drivers/disk_dev.c"
#include "../../../src/kernel/proc/syscalls_recovery.c"

static unsigned backend_calls, raw_reads, raw_writes, raw_flushes, config_calls;
static bool busy_pagefile;
static int spy_read(struct blkdev *d,uint64_t lba,uint32_t count,void *out)
{ (void)d;(void)lba;raw_reads++;memset(out,0x6c,count*512);return 0; }
static int spy_write(struct blkdev *d,uint64_t lba,uint32_t count,const void *in)
{ (void)d;(void)lba;(void)count;(void)in;raw_writes++;return 0; }
static int spy_flush(struct blkdev *d) { (void)d;raw_flushes++;return 0; }
bool storage_volume_pagefile(const struct storage *s,const struct fat_volume *v)
{ (void)s;(void)v;return busy_pagefile; }
int storage_publish_locked(struct storage *s,unsigned disk,unsigned drive,const struct partition *p,uint32_t *reasons)
{ (void)s;(void)disk;(void)drive;(void)p;(void)reasons;backend_calls++;return -FS_EOPNOTSUPP; }
int storage_writer_start(struct storage *s) { (void)s;return 0; }
int storage_convert(struct storage *s,struct ciuki_media_convert *r,bool apply)
{ (void)s;(void)r;(void)apply;backend_calls++;return -FS_EOPNOTSUPP; }
int storage_defrag(struct storage *s,struct ciuki_media_defrag *r,bool step)
{ (void)s;(void)r;(void)step;backend_calls++;return -FS_EOPNOTSUPP; }
int storage_format_removable(struct storage *s,struct ciuki_media_format *r,bool apply)
{ (void)s;(void)r;(void)apply;backend_calls++;return -FS_EOPNOTSUPP; }
int storage_floppy_copy(struct storage *s,struct ciuki_floppy_image *r,storage_floppy_copy_fn fn,void *ctx,bool write)
{ (void)s;(void)r;(void)fn;(void)ctx;(void)write;backend_calls++;return -FS_EOPNOTSUPP; }
int storage_scandisk(unsigned drive,struct ciuki_scandisk *r,bool *schedule)
{ (void)drive;(void)r;*schedule=false;backend_calls++;return -FS_EOPNOTSUPP; }
int storage_scandisk_report(unsigned drive,struct ciuki_scandisk *r)
{ (void)drive;memset(r->found,0,sizeof(r->found));return 0; }
int bootstat_schedule_scan(bool thorough) { (void)thorough;config_calls++;return 0; }
void bootstat_status(struct ciuki_recovery_status *s)
{ *s=(struct ciuki_recovery_status){.version=1,.size=sizeof(*s)}; }
int bootstat_configure(uint8_t disabled,uint8_t request)
{ (void)disabled;(void)request;config_calls++;return 0; }
int bootstat_enable(uint8_t skipped) { (void)skipped;config_calls++;return 0; }
static unsigned input_dispatch;
int grant_input_devctl(struct process *p,int32_t fd,uint32_t command,uint32_t address,uint32_t bytes)
{ (void)p;(void)fd;(void)address;(void)bytes;CHECK(command==INPUT_KEYBOARD_SET);input_dispatch++;return -EAGAIN; }
/* Admission is faked here; every predicate and enforcing operation is real. */
static void identity(struct process *p,uint32_t uid,bool admin)
{
    p->subject->credential=(struct ciuki_credential){.version=1,.size=64,.uid=uid,
        .primary_gid=uid==0 ? 0 : uid==501 ? 546 : 545,
        .session_id=uid ? 1 : 0,.flags=uid ? CIUKI_CRED_INTERACTIVE : 0,
        .group_count=uid && admin ? 2 : 1,
        .groups={uid==0 ? 0 : uid==501 ? 546 : admin ? 544 : 545,uid && admin ? 545 : 0}};
    p->subject->role=0;
}
static int device(int fd,uint32_t command,void *record,unsigned bytes)
{
    CHECK(!ua_write(client->memory,USER,record,bytes));
    int e=call(CIUKI_SYS_DEVCTL,fd,command,USER,bytes,0);
    if (!e) CHECK(!ua_read(client->memory,record,USER,bytes));
    return e;
}
static int recovery(unsigned op,void *record,unsigned bytes)
{
    CHECK(!ua_write(client->memory,USER,record,bytes));
    struct trap_frame tf={.ebx=op,.ecx=USER,.edx=bytes};
    int e=recovery_syscall(&tf);
    if (!e) CHECK(!ua_read(client->memory,record,USER,bytes));
    return e;
}
static void raw_tests(void)
{
    struct storage *s=storage_get();
    struct blkdev a={.capacity=4096,.sector_size=512,.write_cache_state=BLKDEV_CACHE_ENABLED,.read=spy_read,.write=spy_write,.flush=spy_flush},b=a;
    s->raw[0].dev=&a;s->raw[1].dev=&b;
    int fd=file_open(client,"/dev/ata0",O_RDWR,0),data=file_open(client,"/dev/ata1",O_RDWR,0);
    CHECK(fd>=0 && data>=0);
    uint8_t unchanged[512],after[512];memset(unchanged,0x83,sizeof(unchanged));
    unsigned before=raw_reads+raw_writes+raw_flushes;
    for(unsigned kind=0;kind<4;kind++) {
        identity(client,kind==0 ? 1001 : kind==1 ? 501 : kind==2 ? 500 : 1000,kind>=2);
        client->setup_capability=client->network_capability=client->recovery_capability=true;
        CHECK(!ua_write(client->memory,USER,unchanged,sizeof(unchanged)));
        CHECK(call(CIUKI_SYS_PREAD,fd,USER,512,0,0)==-EPERM);
        CHECK(call(CIUKI_SYS_PWRITE,fd,USER,512,0,0)==-EPERM);
        CHECK(!ua_read(client->memory,after,USER,sizeof(after)) && !memcmp(after,unchanged,sizeof(after)));
        if(kind<2) CHECK(call(CIUKI_SYS_PREAD,data,USER,512,0,0)==-EPERM);
    }
    CHECK(raw_reads+raw_writes+raw_flushes==before);
    CHECK(call(CIUKI_SYS_PREAD,data,USER,512,0,0)==512 && raw_reads==1);
    CHECK(call(CIUKI_SYS_PWRITE,data,USER,512,0,0)==-EPERM); /* no lease */
    struct ciuki_disk_info info={.version=1,.size=sizeof(info)};
    CHECK(!device(data,DISK_PARTITION_ACQUIRE,&info,sizeof(info)));
    CHECK(call(CIUKI_SYS_PWRITE,data,USER,512,0,0)==512 && raw_writes==1);
    CHECK(!call(CIUKI_SYS_FSYNC,data,0,0,0,0) && raw_flushes==1);
    identity(client,1001,false); /* descriptor/holder PID cannot retain admin rights */
    CHECK(call(CIUKI_SYS_PREAD,data,USER,512,0,0)==-EPERM);
    CHECK(call(CIUKI_SYS_PWRITE,data,USER,512,0,0)==-EPERM);
    CHECK(call(CIUKI_SYS_FSYNC,data,0,0,0,0)==-EPERM);
    identity(client,1000,true);
    struct ciuki_disk_request release={.version=1,.size=sizeof(release)};
    CHECK(!device(data,DISK_LEASE_RELEASE,&release,sizeof(release)));
    s->raw[1].dev=&a; /* physical system disk alias */
    CHECK(call(CIUKI_SYS_PREAD,data,USER,512,0,0)==-EPERM);
    CHECK(device(data,DISK_PARTITION_ACQUIRE,&info,sizeof(info))==-EBUSY);
    s->raw[1].dev=&b;
    s->volumes[4]=(struct storage_volume){.present=true,.disk=1,.part={.parent=&b},.fat={.mounted=true}};
    busy_pagefile=true;
    CHECK(device(data,DISK_PARTITION_ACQUIRE,&info,sizeof(info))==-EBUSY);
    busy_pagefile=false;memset(&s->volumes[4],0,sizeof(s->volumes[4]));
    CHECK(device(fd,DISK_LEASE_ACQUIRE,&info,sizeof(info))==-EPERM);
    identity(client,0,true);client->setup_capability=true;
    CHECK(device(fd,DISK_LEASE_ACQUIRE,&info,sizeof(info))==-EPERM); /* no Setup role */
    client->subject->role=CIUKI_SECURITY_ROLE_SETUP;
    CHECK(device(fd,DISK_LEASE_ACQUIRE,&info,sizeof(info))==-EPERM); /* not Setup boot */
    s->setup_mode=true;
    CHECK(!device(fd,DISK_LEASE_ACQUIRE,&info,sizeof(info)));
    CHECK(call(CIUKI_SYS_PWRITE,fd,USER,512,0,0)==512);
    CHECK(!device(fd,DISK_LEASE_RELEASE,&release,sizeof(release)));
    s->setup_mode=false;client->setup_capability=false;
    CHECK(!file_close(client,fd) && !file_close(client,data));
    memset(s->raw,0,sizeof(s->raw));
    puts("H05 raw: managed disk and alias denied, no denied I/O, actual issuer/lease/Setup role/boot/pagefile checks PASS");
}
static void management_tests(void)
{
    int fd=file_open(client,"/dev/media",O_RDONLY,0);CHECK(fd>=0);
    struct ciuki_media_format format={.version=2,.size=sizeof(format),.drive=4,.generation=1};
    struct ciuki_media_convert convert={.version=2,.size=sizeof(convert),.drive=2};
    struct ciuki_media_defrag defrag={.version=2,.size=sizeof(defrag),.drive=2};
    format.version=convert.version=defrag.version=CIUKI_MEDIA_VERSION;
    struct ciuki_scandisk scan={.version=CIUKI_SCANDISK_VERSION,.size=sizeof(scan),.drive=2};
    struct ciuki_recovery_status status={.version=1,.size=sizeof(status)};
    for(unsigned kind=0;kind<2;kind++) {
        identity(client,kind ? 501 : 1001,false);
        client->time_capability=client->recovery_capability=true;
        unsigned before=backend_calls+config_calls, writes=store_writes,rtcw=rtc_writes();
        struct ciuki_time_zone time={.version=1,.size=sizeof(time)};
        CHECK(!timezone_call(client,&time) && !(time.flags&CIUKI_TIME_AUTHORITY));
        for(unsigned op=1;op<=4;op++) { time.operation=op;CHECK(timezone_call(client,&time)==-EPERM); }
        CHECK(store_writes==writes && rtc_writes()==rtcw);
        CHECK(device(fd,MEDIA_FORMAT,&format,sizeof(format))==-EPERM);
        CHECK(device(fd,MEDIA_CONVERT,&convert,sizeof(convert))==-EPERM);
        CHECK(device(fd,MEDIA_DEFRAG_ANALYZE,&defrag,sizeof(defrag))==-EPERM);
        CHECK(device(fd,MEDIA_DEFRAG_STEP,&defrag,sizeof(defrag))==-EPERM);
        CHECK(device(fd,MEDIA_FLOPPY_READ,&format,sizeof(format))==-EPERM);
        CHECK(device(fd,MEDIA_FLOPPY_WRITE,&format,sizeof(format))==-EPERM);
        CHECK(recovery(CIUKI_RECOVERY_SCANDISK,&scan,sizeof(scan))==-EPERM);
        CHECK(recovery(CIUKI_RECOVERY_SET,&status,sizeof(status))==-EPERM);
        CHECK(recovery(CIUKI_RECOVERY_ENABLE,&status,sizeof(status))==-EPERM);
        CHECK(!recovery(CIUKI_RECOVERY_STATUS,&status,sizeof(status)) && !(status.flags&CIUKI_RECOVERY_AUTHORITY));
        CHECK(!recovery(CIUKI_RECOVERY_REPORT,&scan,sizeof(scan)));
        CHECK(backend_calls+config_calls==before);
        struct ciuki_media_label label={.version=CIUKI_MEDIA_VERSION,.size=sizeof(label),.drive=2,.label="SECURITY"};
        CHECK(device(fd,MEDIA_SET_LABEL,&label,sizeof(label))==-EPERM);
        CHECK(!device(fd,MEDIA_CD_STATE,&format,sizeof(format)));
        struct ciuki_media_remove remove={.version=CIUKI_MEDIA_VERSION,.size=sizeof(remove),.drive=4,.generation=123};
        CHECK(!device(fd,MEDIA_SAFE_REMOVE,&remove,sizeof(remove)));
    }
    identity(client,500,true);client->time_capability=client->recovery_capability=false;
    struct ciuki_time_zone time={.version=1,.size=sizeof(time),.operation=CIUKI_TIME_ACKNOWLEDGE};
    CHECK(!timezone_call(client,&time) && (time.flags&CIUKI_TIME_AUTHORITY));
    CHECK(!recovery(CIUKI_RECOVERY_SET,&status,sizeof(status)));
    CHECK(device(fd,MEDIA_DEFRAG_ANALYZE,&defrag,sizeof(defrag))==-EOPNOTSUPP);
    CHECK(device(fd,MEDIA_CONVERT,&convert,sizeof(convert))==-EOPNOTSUPP);
    CHECK(!recovery(CIUKI_RECOVERY_SCANDISK,&scan,sizeof(scan)));
    CHECK(device(fd,MEDIA_FORMAT,&format,sizeof(format))==-EOPNOTSUPP); /* admitted to fake engine */
    defrag.drive=4;CHECK(device(fd,MEDIA_DEFRAG_ANALYZE,&defrag,sizeof(defrag))==-EOPNOTSUPP);
    struct process *worker;struct proc_thread *wt=make_process(proc_supervisor(),&worker);
    worker->cwd=space->root;px_retain(worker->cwd);worker->cwd_retain=px_retain;worker->cwd_release=px_release;
    int wfd=file_open(worker,"/dev/media",O_RDONLY,0);CHECK(wfd>=0);
    const struct security_subject *request=client->subject;
    CHECK(!security_subject_get(client->subject));
    defrag.drive=2;CHECK(!ua_write(worker->memory,USER,&defrag,sizeof(defrag)));
    g_current=wt->task;g_current->state=T_RUNNING;
    CHECK(media_devctl_subject(worker,request,wfd,MEDIA_DEFRAG_ANALYZE,USER,sizeof(defrag))==-EOPNOTSUPP);
    identity(client,1001,false);unsigned denied_calls=backend_calls;
    CHECK(media_devctl_subject(worker,request,wfd,MEDIA_DEFRAG_ANALYZE,USER,sizeof(defrag))==-EPERM);
    CHECK(backend_calls==denied_calls);identity(client,500,true);
    client->subject->live=false;
    unsigned before=backend_calls;
    CHECK(media_devctl_subject(worker,request,wfd,MEDIA_DEFRAG_ANALYZE,USER,sizeof(defrag))==-EACCES);
    CHECK(backend_calls==before);client->subject->live=true;
    security_subject_put(client->subject);
    CHECK(!file_close(worker,wfd));stop_collect(worker);g_current=client_thread->task;g_current->state=T_RUNNING;
    CHECK(device(fd,0x43ff,&format,sizeof(format))==-ENOTTY);
    CHECK(device(fd,CIUKI_SETUP_NET_QUERY,&format,sizeof(format))==-ENOTTY);
    int netfd=file_open(client,"/dev/net",O_RDONLY,0);CHECK(netfd>=0);
    struct ciuki_setup_net query={.version=1,.size=sizeof(query)};
    client->setup_capability=true;setup_test_mode=true;
    CHECK(device(netfd,CIUKI_SETUP_NET_QUERY,&query,sizeof(query))==-EPERM);
    identity(client,0,true);client->subject->role=CIUKI_SECURITY_ROLE_SETUP;
    CHECK(!device(netfd,CIUKI_SETUP_NET_QUERY,&query,sizeof(query)) && !query.count);
    client->subject->role=0;
    CHECK(device(netfd,CIUKI_SETUP_NET_QUERY,&query,sizeof(query))==-EPERM);
    client->setup_capability=false;setup_test_mode=false;
    CHECK(!file_close(client,netfd));
    CHECK(device(fd,INPUT_KEYBOARD_SET,&format,sizeof(format))==-EAGAIN && input_dispatch==1);
    CHECK(!file_close(client,fd));
    puts("H05 management: Standard/Guest no effects, public status/media use, administrator setters, SYSTEM retained-client scan, stale client and dispatch PASS");
}
int main(int argc,char **argv)
{
    CHECK(argc==2);setvbuf(stdout,NULL,_IOLBF,0);
    ram=calloc(HOST_PAGES,PAGE_SIZE);CHECK(ram);
    controller.state=T_RUNNING;g_current=&controller;proc_init();
    CHECK(!fake_open(&disk[0],argv[1]));CHECK(!cache_init(&cache,0));vfs_init(&vfs);
    CHECK(!fat_mount(&volumes[0],&cache,&disk[0].dev,0,disk[0].dev.capacity,true));
    CHECK(!vfs_attach(&vfs,2,&volumes[0]));CHECK(!files_attach(&vfs,&space));
    client_thread=make_process(proc_supervisor(),&client);g_current=client_thread->task;g_current->state=T_RUNNING;
    client->cwd=space->root;px_retain(client->cwd);client->cwd_retain=px_retain;client->cwd_release=px_release;
    vfs_init(&media_observation.vfs);media_observation.ready=true;media_result=0;
    boot(60,civil(2026,1,15,12,0,0),1);
    raw_tests();management_tests();
    identity(client,0,true);all_closed();stop_collect(client);
    CHECK(!vfs_detach(&vfs,2));vfs_destroy(&vfs);vfs_destroy(&media_observation.vfs);cache_destroy(&cache);
    fake_reset(&disk[0]);fake_close(&disk[0]);CHECK(heap_blocks==1 && !pages_used);free(ram);
    printf("H05 devices: PASS checks=%u heap=%u pages=%u\n",checks,heap_blocks,pages_used);
    return 0;
}
