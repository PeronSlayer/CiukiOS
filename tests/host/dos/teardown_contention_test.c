/* DOS Box close under ordinary C: load, over the production cache, FAT, VFS
 * and private logical volume (QEMU run of image 56e58374: the window stayed
 * "Closing..." after "[dosvm] cdaudio close"; the next record, the overlay's
 * "private-data ... action=discard", never came). The close performs what
 * dosvm_destroy does after cdaudio close for a transient session: the
 * logical C: (dos_file_volume) is closed, flushing its metadata through the
 * provider into the overlay file on the outer C:, then the overlay is
 * committed, both files are closed and the overlay is deleted. Meanwhile
 * the storage writer writes back and appends the boot log, the print
 * spooler probes job folders and the desktop saves a settings file.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/abi.h>
#include "../../../src/kernel/vm/dos_file_volume.h"
#include <ciuki/storage.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define OK(x) do { int ok_=(x); if (ok_) { fprintf(stderr,"%s:%d: %s = %d\n",__FILE__,__LINE__,#x,ok_); abort(); } } while (0)

#define OUTER_SECTORS 65535u
#define BASE_SECTORS 8192u
static uint8_t outer_disk[OUTER_SECTORS*512u], base_image[BASE_SECTORS*512u];
static struct storage outer_storage;
#define cache outer_storage.cache
#define space outer_storage.vfs
struct storage *storage_get(void) { return &outer_storage; }
static unsigned device_delay_us;
static void pause_us(unsigned us) { struct timespec t={0,(long)us*1000}; if (us) nanosleep(&t,NULL); }
static int memory_read(struct blkdev *d,uint64_t lba,uint32_t count,void *buf)
{ int e=blkdev_range(d,lba,count); if (!e) { pause_us(device_delay_us); memcpy(buf,(uint8_t *)d->ctx+lba*512u,(size_t)count*512u); } return e; }
static int memory_write(struct blkdev *d,uint64_t lba,uint32_t count,const void *buf)
{ int e=blkdev_range(d,lba,count); if (!e) { pause_us(device_delay_us); memcpy((uint8_t *)d->ctx+lba*512u,buf,(size_t)count*512u); } return e; }
static int memory_flush(struct blkdev *d) { (void)d; pause_us(device_delay_us); return 0; }
static void format(uint8_t *disk,unsigned sectors,unsigned spc)
{
    unsigned clusters=sectors/spc, fat_sectors=(clusters*2u+511u)/512u;
    memset(disk,0,sectors*512u); uint8_t *b=disk;
    b[0]=0xeb; b[1]=0x3c; b[2]=0x90; memcpy(b+3,"CIUTEST ",8);
    fs_wr16(b+11,512); b[13]=(uint8_t)spc; fs_wr16(b+14,1); b[16]=2;
    fs_wr16(b+17,512); fs_wr16(b+19,(uint16_t)sectors); b[21]=0xf8; fs_wr16(b+22,(uint16_t)fat_sectors);
    fs_wr16(b+24,32); fs_wr16(b+26,4); b[38]=0x29; fs_wr32(b+39,0x640064);
    memcpy(b+43,"TEARDOWN   ",11); memcpy(b+54,"FAT16   ",8); b[510]=0x55; b[511]=0xaa;
    for (unsigned i=0;i<2;i++) { fs_wr16(disk+(1+i*fat_sectors)*512,0xfff8); fs_wr16(disk+(1+i*fat_sectors)*512+2,0xffff); }
}
static uint64_t now_ms(void) { return fs_now_ms(); }

/* ---- The DOS Box's transient C: (dosvm_disk.c's overlay provider) ---- */
static struct fat_volume outer;
static struct vfs_table dosbox_table;     /* the VM's disk_table on the outer C: */
static int base_fd=-1, overlay_fd=-1;
static uint32_t overlay_lba[4096]; static unsigned overlay_count;
static pthread_mutex_t disk_gate=PTHREAD_MUTEX_INITIALIZER;
static struct blkdev provider;
static int backing_io(int fd,uint32_t offset,void *data,bool write)
{
    uint64_t at=0; size_t done=0;
    int e=vfs_seek(&dosbox_table,fd,offset,VFS_SEEK_SET,&at); if (e) return e;
    if (at!=offset) return -FS_EIO;
    e=write ? vfs_write(&dosbox_table,fd,data,512,&done) : vfs_read(&dosbox_table,fd,data,512,&done);
    return e ? e : done==512 ? 0 : -FS_EIO;
}
static int sector_io(uint32_t lba,void *sector,bool write)
{
    unsigned i=0; while (i<overlay_count && overlay_lba[i]!=lba) i++;
    if (write) {
        if (i==sizeof(overlay_lba)/sizeof(overlay_lba[0])) return -FS_ENOSPC;
        int e=backing_io(overlay_fd,i*512u,sector,true); if (e) return e;
        if (i==overlay_count) overlay_lba[overlay_count++]=lba;
        return 0;
    }
    return i<overlay_count ? backing_io(overlay_fd,i*512u,sector,false) : backing_io(base_fd,lba*512u,sector,false);
}
static int provider_read(struct blkdev *d,uint64_t lba,uint32_t count,void *buf)
{
    int e=blkdev_range(d,lba,count); if (e) return e;
    pthread_mutex_lock(&disk_gate);
    for (uint32_t i=0;!e && i<count;i++) e=sector_io((uint32_t)lba+i,(uint8_t *)buf+i*512u,false);
    pthread_mutex_unlock(&disk_gate); return e;
}
static int provider_write(struct blkdev *d,uint64_t lba,uint32_t count,const void *buf)
{
    int e=blkdev_range(d,lba,count); if (e) return e;
    pthread_mutex_lock(&disk_gate);
    for (uint32_t i=0;!e && i<count;i++) e=sector_io((uint32_t)lba+i,(void *)((const uint8_t *)buf+i*512u),true);
    pthread_mutex_unlock(&disk_gate); return e;
}
static int provider_flush(struct blkdev *d)
{ (void)d; pthread_mutex_lock(&disk_gate); int e=vfs_commit(&dosbox_table,overlay_fd); pthread_mutex_unlock(&disk_gate); return e; }

/* ---- Background C: load ---- */
static volatile int stop_load;
/* Native file system calls carry a request (syscalls_file.c: file_pause
 * sleeps 1 ms); kernel tasks such as the storage writer carry none. */
static uint64_t request_now(void *arg) { (void)arg; return fs_now_ms(); }
static void request_pause(void *arg) { (void)arg; struct timespec t={0,1000000}; nanosleep(&t,NULL); }
static const struct vfs_request native_request={.deadline=UINT64_MAX/2,.now=request_now,.pause=request_pause,
                                                .operation="native",.site="native"};
static unsigned writer_passes, log_appends, spool_probes, settings_saves;
static void *storage_writer(void *arg)
{
    (void)arg; struct vfs_table t; OK(vfs_table_init(&space,&t,2));
    int log=vfs_open(&t,"C:\\SYSTEM\\LOGS\\BOOTLOG.TXT",VFS_READ|VFS_WRITE|VFS_CREATE,VFS_DENY_WRITE,0); assert(log>=0);
    uint8_t text[4096]; memset(text,'L',sizeof(text)); uint64_t last=0, at=0;
    while (!__atomic_load_n(&stop_load,__ATOMIC_ACQUIRE)) {
        cache_writer_wait(&cache,10);
        vfs_lock(&space,0); struct vfs_io_state io=vfs_io_begin_locked(&space);
        (void)cache_writeback_tick(&cache,(uint32_t)now_ms());
        vfs_io_end_locked(&space,io); vfs_unlock(&space);
        writer_passes++;
        if (now_ms()-last>=50) {        /* bootlog_drain: append, then publish the size */
            last=now_ms(); size_t done=0; uint64_t pos=0;
            if (at>=2u*1024u*1024u) { OK(vfs_truncate(&t,log,0)); at=0; }
            OK(vfs_seek(&t,log,(int64_t)at,VFS_SEEK_SET,&pos)); OK(vfs_write(&t,log,text,sizeof(text),&done));
            at+=done; OK(vfs_commit(&t,log)); log_appends++;
        }
    }
    OK(vfs_close(&t,log)); vfs_table_destroy(&t); return NULL;
}
static void *print_spooler(void *arg)
{
    (void)arg; struct vfs_table t; OK(vfs_table_init(&space,&t,2));
    struct fs_request_scope scope; fs_request_enter(&scope,&native_request); t.request=&native_request;
    while (!__atomic_load_n(&stop_load,__ATOMIC_ACQUIRE)) {
        for (unsigned id=1;id<=128 && !__atomic_load_n(&stop_load,__ATOMIC_ACQUIRE);id++) {
            char p[64]; snprintf(p,sizeof(p),"C:\\WINDOWS\\SPOOL\\J%03u\\JOB",id);
            int fd=vfs_open(&t,p,VFS_READ,VFS_DENY_NONE,0); assert(fd<0);
            snprintf(p,sizeof(p),"C:\\WINDOWS\\SPOOL\\J%03u\\JOB.BAK",id);
            fd=vfs_open(&t,p,VFS_READ,VFS_DENY_NONE,0); assert(fd<0);
            spool_probes+=2;
        }
    }
    fs_request_leave(&scope); vfs_table_destroy(&t); return NULL;
}
static void *desktop_settings(void *arg)
{
    (void)arg; struct vfs_table t; OK(vfs_table_init(&space,&t,2));
    struct fs_request_scope scope; fs_request_enter(&scope,&native_request); t.request=&native_request;
    char text[700]; memset(text,'s',sizeof(text));
    while (!__atomic_load_n(&stop_load,__ATOMIC_ACQUIRE)) {
        int fd=vfs_open(&t,"C:\\SYSTEM\\SETTINGS\\DESKTOP.CFG",VFS_WRITE|VFS_CREATE|VFS_TRUNCATE,VFS_DENY_ALL,0); assert(fd>=0);
        size_t done=0; OK(vfs_write(&t,fd,text,sizeof(text),&done)); OK(vfs_commit(&t,fd)); OK(vfs_close(&t,fd));
        settings_saves++;
        struct timespec gap={0,20000000}; nanosleep(&gap,NULL);
    }
    fs_request_leave(&scope); vfs_table_destroy(&t); return NULL;
}

/* ---- One DOS Box session: open, guest writes, close ---- */
static volatile int close_step, close_done;
static struct dos_file_volume *guest;
static void session_open(unsigned round)
{
    OK(vfs_table_init(&space,&dosbox_table,2));
    base_fd=vfs_open(&dosbox_table,"C:\\SYSTEM\\DOS\\UNIFIED.IMG",VFS_READ,VFS_DENY_WRITE,0); assert(base_fd>=0);
    char name[64]; snprintf(name,sizeof(name),"C:\\SYSTEM\\DOS\\VM%u.OVL",round%4u+1);
    overlay_fd=vfs_open(&dosbox_table,name,VFS_READ|VFS_WRITE|VFS_CREATE|VFS_TRUNCATE,VFS_DENY_ALL,FAT_ATTR_HIDDEN); assert(overlay_fd>=0);
    overlay_count=0;
    guest=calloc(1,sizeof(*guest)); assert(guest);
    OK(dos_file_volume_open(guest,&cache,&provider,true));
    /* The session's own writes (DOS boot, a program's files) stay dirty in
     * the cache until the close flushes them through the provider. */
    for (unsigned f=0;f<6;f++) {
        char p[48]; snprintf(p,sizeof(p),"C:\\TEMP%u.DAT",f);
        int fd=vfs_open(&guest->table,p,VFS_WRITE|VFS_CREATE|VFS_TRUNCATE,VFS_DENY_ALL,0); assert(fd>=0);
        static uint8_t data[8192]; memset(data,(int)(f+round),sizeof(data));
        size_t done=0; OK(vfs_write(&guest->table,fd,data,sizeof(data),&done)); OK(vfs_close(&guest->table,fd));
    }
}
static void *session_close(void *arg)
{
    unsigned round=(unsigned)(uintptr_t)arg;
    close_step=1; OK(dos_file_volume_close(guest));             /* dosvm_file_destroy */
    close_step=2; OK(vfs_commit(&dosbox_table,overlay_fd));      /* dosvm_disk_close: dirty transient overlay */
    close_step=3; OK(vfs_close(&dosbox_table,overlay_fd)); OK(vfs_close(&dosbox_table,base_fd));
    char name[64]; snprintf(name,sizeof(name),"C:\\SYSTEM\\DOS\\VM%u.OVL",round%4u+1);
    close_step=4; OK(vfs_delete(&dosbox_table,name));            /* "private-data ... action=discard" */
    close_step=5; vfs_table_destroy(&dosbox_table);
    free(guest); guest=NULL;
    __atomic_store_n(&close_done,1,__ATOMIC_RELEASE);
    return NULL;
}

static void build_outer(void)
{
    format(outer_disk,OUTER_SECTORS,4); format(base_image,BASE_SECTORS,4);
    struct blkdev device={.read=memory_read,.write=memory_write,.flush=memory_flush,.capacity=OUTER_SECTORS,
        .sector_size=512,.write_cache_state=BLKDEV_CACHE_ENABLED,.ctx=outer_disk};
    static struct blkdev outer_device; outer_device=device;
    OK(cache_init(&cache,2u*1024u*1024u));
    OK(fat_mount(&outer,&cache,&outer_device,0,OUTER_SECTORS,true));
    vfs_init(&space); OK(vfs_attach(&space,2,&outer)); outer_storage.ready=true;
    struct vfs_table t; OK(vfs_table_init(&space,&t,2));
    static const char *dirs[]={"C:\\WINDOWS","C:\\WINDOWS\\SPOOL","C:\\SYSTEM","C:\\SYSTEM\\DOS","C:\\SYSTEM\\LOGS","C:\\SYSTEM\\SETTINGS"};
    for (unsigned i=0;i<sizeof(dirs)/sizeof(dirs[0]);i++) OK(vfs_mkdir(&t,dirs[i],0));
    /* A Windows directory with a few hundred entries: every spool probe
     * walks it, as C:\WINDOWS\SPOOL\jNNN\job does on the image. */
    for (unsigned i=0;i<300;i++) {
        char p[64]; snprintf(p,sizeof(p),"C:\\WINDOWS\\System file number %03u.dll",i);
        int fd=vfs_open(&t,p,VFS_WRITE|VFS_CREATE,VFS_DENY_ALL,0); assert(fd>=0); OK(vfs_close(&t,fd));
    }
    /* The unified DOS image: a FAT16 volume as a file on C:. */
    int fd=vfs_open(&t,"C:\\SYSTEM\\DOS\\UNIFIED.IMG",VFS_WRITE|VFS_CREATE,VFS_DENY_ALL,0); assert(fd>=0);
    size_t done=0; OK(vfs_write(&t,fd,base_image,sizeof(base_image),&done)); assert(done==sizeof(base_image));
    OK(vfs_commit(&t,fd)); OK(vfs_close(&t,fd)); vfs_table_destroy(&t);
    provider=(struct blkdev){.read=provider_read,.write=provider_write,.flush=provider_flush,.capacity=BASE_SECTORS,
        .sector_size=512,.media_generation=1,.write_cache_state=BLKDEV_CACHE_ENABLED,.logical=true};
}

int main(int argc,char **argv)
{
    unsigned rounds=argc>1 ? (unsigned)atoi(argv[1]) : 40u;
    device_delay_us=argc>2 ? (unsigned)atoi(argv[2]) : 50u;
    build_outer();
    pthread_t writer,spool,desktop;
    OK(pthread_create(&writer,NULL,storage_writer,NULL));
    OK(pthread_create(&spool,NULL,print_spooler,NULL));
    OK(pthread_create(&desktop,NULL,desktop_settings,NULL));
    uint64_t worst=0; unsigned hung=0;
    for (unsigned round=0;round<rounds && !hung;round++) {
        provider.media_generation=round+1;
        session_open(round);
        close_done=0; close_step=0;
        pthread_t closer; uint64_t begin=now_ms();
        OK(pthread_create(&closer,NULL,session_close,(void *)(uintptr_t)round));
        while (!__atomic_load_n(&close_done,__ATOMIC_ACQUIRE) && now_ms()-begin<20000) { struct timespec t={0,1000000}; nanosleep(&t,NULL); }
        uint64_t took=now_ms()-begin;
        if (!close_done) {
            hung=1;
            printf("HANG DOS Box close round %u: step %d for %llu ms (writer passes %u, log appends %u, spool probes %u, settings saves %u)\n",
                   round,close_step,(unsigned long long)took,writer_passes,log_appends,spool_probes,settings_saves);
            fflush(stdout);
            if (getenv("TEARDOWN_ABORT")) abort();
            break;
        }
        OK(pthread_join(closer,NULL));
        if (took>worst) worst=took;
    }
    if (hung) return 2;
    __atomic_store_n(&stop_load,1,__ATOMIC_RELEASE);
    OK(pthread_join(writer,NULL)); OK(pthread_join(spool,NULL)); OK(pthread_join(desktop,NULL));
    printf("PASS DOS Box close under C: load: %u sessions closed (logical C: flush, overlay commit, close, delete), worst %llu ms; "
           "writer passes %u, boot-log appends %u, spool probes %u, settings saves %u\n",
           rounds,(unsigned long long)worst,writer_passes,log_appends,spool_probes,settings_saves);
    vfs_detach(&space,2); vfs_destroy(&space); cache_destroy(&cache);
    return 0;
}
