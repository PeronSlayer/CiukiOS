/* G29 production formatter and removable mount path, file-backed media.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdlib.h>
#include <string.h>
#include <ciuki/storage.h>
#undef CLOCKS_PER_SEC
#undef WIFEXITED
#undef WEXITSTATUS
#undef WIFSIGNALED
#undef WTERMSIG
#include <ciuki/abi.h>
#include <ciuki/kernel.h>
#include "fake.h"
#include "scan.h"
#include <stdarg.h>
#include <stdio.h>

#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"FORMAT line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
#define OK(x) do { int result=(x); checks++; if (result) { fprintf(stderr,"FORMAT line %d: %s => %d\n",__LINE__,#x,result); exit(1); } } while (0)
static unsigned checks;
static unsigned formatted_tracks;
static struct fake media;
static struct storage *observed_storage;
static unsigned observed_drive, observed_guards;
static bool replace_medium, guard_checked;
struct ciuki_boot_info g_boot;
void klog(const char *fmt, ...) { (void)fmt; }
void fs_host_service(void)
{
    struct storage *s=observed_storage;
    if (!s) return;
    struct storage_volume *v=&s->volumes[observed_drive];
    if (!v->formatting || v->present || guard_checked) return;
    guard_checked=true;
    CHECK(s->vfs.mutating&vfs_drive_mask(observed_drive));
    CHECK(!s->vfs.volumes[observed_drive]); observed_guards++;
    /* A concurrent insertion/retry cannot reuse the retained raw view. */
    if (observed_drive<2) CHECK(storage_add_floppy(s,&media.dev,observed_drive)==-FS_EBUSY);
    else CHECK(storage_add_removable(s,&media.dev)==-FS_EBUSY);
    if (replace_medium) { media.dev.media_generation++; replace_medium=false; }
}
int floppy_format_track(struct blkdev *dev,unsigned cylinder,unsigned head)
{
    CHECK(dev==&media.dev && cylinder<80 && head<2);
    uint8_t fill[512]; memset(fill,0xf6,sizeof(fill)); formatted_tracks++;
    uint32_t lba=(cylinder*2+head)*18;
    for (unsigned sector=0;sector<18;sector++) { int e=dev->write(dev,lba+sector,1,fill); if(e)return e; }
    return 0;
}
static struct ciuki_media_format request(unsigned drive)
{
    return (struct ciuki_media_format){.version=CIUKI_MEDIA_VERSION,.size=sizeof(struct ciuki_media_format),
        .drive=drive,.generation=media.dev.media_generation};
}
static struct storage *start(unsigned drive)
{
    struct storage *s=storage_get();
    OK(storage_setup(s,0));
    if (drive<2) OK(storage_add_floppy(s,&media.dev,drive));
    else OK(storage_add_removable(s,&media.dev));
    CHECK(s->vfs.volumes[drive]);
    return s;
}
static void stop(struct storage *s)
{
    storage_destroy(s);
    fake_reset(&media);
}
static void success(unsigned family, unsigned drive, bool quick)
{
    struct storage *s=start(drive);
    struct ciuki_media_format r=request(drive);
    unsigned writes=media.writes;
    OK(storage_format_removable(s,&r,false));
    CHECK(media.writes==writes && r.sectors==media.dev.capacity && r.filesystem==family && r.supported_filesystems);
    r.filesystem=family; r.flags=quick ? CIUKI_FORMAT_QUICK : 0;
    memcpy(r.label,"test label",11);
    unsigned reads=media.reads;
    uint32_t previous=s->vfs.generation[drive];
    observed_storage=s; observed_drive=drive; guard_checked=false; unsigned guards=observed_guards;
    OK(storage_format_removable(s,&r,true));
    observed_storage=0; CHECK(observed_guards>guards && !s->volumes[drive].formatting);
    CHECK(s->volumes[drive].present && !s->volumes[drive].fat.readonly);
    CHECK(s->vfs.generation[drive]>previous && s->volumes[drive].fat.type==family);
    CHECK(!strcmp(r.label,"TEST LABEL") && r.cluster_bytes);
    if (!quick) CHECK(media.reads-reads>=media.dev.capacity);
    else CHECK(media.reads-reads<media.dev.capacity);
    uint8_t boot[512], root[512], mirror[512], first[512];
    OK(fake_raw_read(&media,0,boot));
    CHECK(fs_rd16(boot+510)==0xaa55 && fs_rd16(boot+11)==512);
    CHECK(!memcmp(boot+(family==32 ? 71 : 43),"TEST LABEL ",11));
    struct fat_volume *v=&s->volumes[drive].fat;
    OK(fake_raw_read(&media,v->reserved,first));
    OK(fake_raw_read(&media,v->reserved+v->fat_sectors,mirror));
    CHECK(!memcmp(first,mirror,512));
    OK(fake_raw_read(&media,family==32 ? v->data_sector : v->root_sector,root));
    CHECK(root[11]==8 && !memcmp(root,"TEST LABEL ",11));
    if (family==32) {
        OK(fake_raw_read(&media,6,mirror)); CHECK(!memcmp(boot,mirror,512));
        OK(fake_raw_read(&media,1,first)); CHECK(fs_rd32(first)==0x41615252 && fs_rd32(first+484)==0x61417272);
    }
    static struct vfs_table table; OK(vfs_table_init(&s->vfs,&table,drive));
    int fd=vfs_open(&table,"/new.txt",VFS_CREATE|VFS_READ|VFS_WRITE,VFS_DENY_NONE,0); CHECK(fd>=0);
    size_t done; OK(vfs_write(&table,fd,"hello",5,&done)); CHECK(done==5); OK(vfs_close(&table,fd));
    OK(vfs_flush_drive(&table,drive)); vfs_table_destroy(&table);
    OK(storage_remove_removable(s,&media.dev,false));
    struct scan_result scan=independent_scan(media.fd,0);
    CHECK(!scan.corrupt && !scan.crosslinks && !scan.lost && !scan.divergent && !scan.dirty);
    if (drive<2) OK(storage_add_floppy(s,&media.dev,drive));
    else { s->next_drive=drive; OK(storage_add_removable(s,&media.dev)); }
    CHECK(s->vfs.volumes[drive]->type==family);
    stop(s);
}
static void refusals(void)
{
    struct storage *s=start(0);
    struct ciuki_media_format good=request(0), r;
    OK(storage_format_removable(s,&good,false)); good.flags=CIUKI_FORMAT_QUICK;
    unsigned writes=media.writes;
    r=good; r.generation++; CHECK(storage_format_removable(s,&r,true)==-FS_EIO);
    r=good; r.filesystem=7; CHECK(storage_format_removable(s,&r,true)==-FS_EINVAL);
    r=good; r.flags=8; CHECK(storage_format_removable(s,&r,true)==-FS_EINVAL);
    r=good; memset(r.label,'X',12); CHECK(storage_format_removable(s,&r,true)==-FS_EINVAL);
    r=good; memcpy(r.label,"bad/name",9); CHECK(storage_format_removable(s,&r,true)==-FS_EINVAL);
    r=good; r.filesystem=32; CHECK(storage_format_removable(s,&r,true)==-FS_EOPNOTSUPP);
    s->volumes[0].removable=false; r=good; CHECK(storage_format_removable(s,&r,true)==-FS_EROFS);
    s->volumes[0].removable=true;
    media.dev.write_cache_state=BLKDEV_CACHE_UNKNOWN; r=good;
    CHECK(storage_format_removable(s,&r,true)==-FS_EROFS);
    media.dev.write_cache_state=BLKDEV_CACHE_ENABLED;
    CHECK(media.writes==writes && s->volumes[0].present);
    CHECK(storage_activate_removable(s,&media.dev)==1);
    static struct vfs_table table; OK(vfs_table_init(&s->vfs,&table,0));
    int fd=vfs_open(&table,"/busy.txt",VFS_CREATE|VFS_READ|VFS_WRITE,VFS_DENY_NONE,0); CHECK(fd>=0);
    writes=media.writes;
    r=good; CHECK(storage_format_removable(s,&r,true)==-FS_EBUSY);
    CHECK(media.writes==writes); OK(vfs_close(&table,fd));
    /* The system's unrelated C: cwd does not block this drive. A cwd inside
     * this drive does, even without an open file descriptor. */
    table.cwd_cluster[0]=2; r=good; CHECK(storage_format_removable(s,&r,true)==-FS_EBUSY);
    CHECK(media.writes==writes); table.cwd_cluster[0]=0; vfs_table_destroy(&table);
    s->volumes[5].present=true; s->volumes[5].removable=true; s->volumes[5].part.parent=&media.dev;
    r=good; CHECK(storage_format_removable(s,&r,true)==-FS_EBUSY);
    CHECK(media.writes==writes); s->volumes[5].present=false;
    stop(s);
}
static void failures(void)
{
    /* Failures before destructive metadata, during writes, and at flush
     * never publish an uncertain FAT. No write retry or forced downgrade. */
    for (unsigned fault=0;fault<4;fault++) {
        struct storage *s=start(0);
        struct ciuki_media_format r=request(0); OK(storage_format_removable(s,&r,false));
        r.flags=fault ? CIUKI_FORMAT_QUICK : 0;
        if (!fault) media.fail_read=2;
        if (fault==1) media.fail_write=1;
        if (fault==2) media.fail_flush_call=media.flush_calls+2;
        if (fault==3) { observed_storage=s; observed_drive=0; replace_medium=true; guard_checked=false; r.flags=0; }
        CHECK(storage_format_removable(s,&r,true)<0);
        observed_storage=0;
        CHECK(!s->volumes[0].present && !s->vfs.volumes[0]);
        stop(s);
    }
}
static int partition_read(struct blkdev *dev, uint64_t lba, uint32_t n, void *buf)
{
    struct fake *f=dev->ctx;
    if (!lba && n==1) {
        uint8_t *b=buf; memset(b,0,512); b[450]=6;
        fs_wr32(b+454,2048); fs_wr32(b+458,(uint32_t)f->dev.capacity); fs_wr16(b+510,0xaa55); return 0;
    }
    return lba>=2048 ? f->dev.read(&f->dev,lba-2048,n,buf) : -FS_EIO;
}
static int partition_write(struct blkdev *dev, uint64_t lba, uint32_t n, const void *buf)
{
    struct fake *f=dev->ctx; CHECK(lba>=2048); /* Format must never alter the partition table. */
    return f->dev.write(&f->dev,lba-2048,n,buf);
}
static int partition_flush(struct blkdev *dev)
{ struct fake *f=dev->ctx; return f->dev.flush(&f->dev); }
static void editor_lease(void)
{
    struct storage *s=storage_get(); OK(storage_setup(s,0));
    struct blkdev boot={.sector_size=512,.capacity=4096};
    s->raw[0].dev=&boot;s->raw[1].dev=&media.dev;
    struct storage_caller owner={.pid=77,.administrator=true},other={.pid=78,.administrator=true};
    struct ciuki_disk_info info={.version=CIUKI_DISK_VERSION,.size=sizeof(info)};
    CHECK(storage_disk_control(s,0,&owner,DISK_PARTITION_ACQUIRE,&info,sizeof(info))==-EBUSY);
    CHECK(storage_disk_control(s,1,&owner,DISK_LEASE_ACQUIRE,&info,sizeof(info))==-EPERM);
    OK(storage_disk_control(s,1,&owner,DISK_PARTITION_ACQUIRE,&info,sizeof(info)));
    uint8_t data[512]={0};
    CHECK(storage_disk_io(s,1,&other,true,100*512,data,512)==-EACCES);
    CHECK(storage_disk_io(s,1,&owner,true,100*512,data,512)==512);
    struct ciuki_disk_lease lease={.version=CIUKI_DISK_VERSION,.size=sizeof(lease)};
    CHECK(storage_disk_control(s,1,&owner,DISK_PUBLISH,&lease,sizeof(lease))==-EPERM);
    CHECK(storage_disk_control(s,1,&owner,DISK_SET_PHASE,&lease,sizeof(lease))==-EPERM);
    struct ciuki_disk_request release={.version=CIUKI_DISK_VERSION,.size=sizeof(release)};
    OK(storage_disk_control(s,1,&owner,DISK_LEASE_RELEASE,&release,sizeof(release)));
    info=(struct ciuki_disk_info){.version=CIUKI_DISK_VERSION,.size=sizeof(info)};
    s->volumes[4].present=true; s->volumes[4].disk=1; s->volumes[4].part.parent=&media.dev;
    s->volumes[4].fat.mounted=true; s->volumes[4].fat.readonly=false;
    CHECK(storage_disk_control(s,1,&owner,DISK_PARTITION_ACQUIRE,&info,sizeof(info))==-EBUSY);
    s->volumes[4].present=false; s->volumes[4].fat.mounted=false;
    OK(storage_disk_control(s,1,&owner,DISK_PARTITION_ACQUIRE,&info,sizeof(info)));
    storage_disk_exit(s,owner.pid);
    CHECK(storage_disk_io(s,1,&owner,true,100*512,data,512)==-EACCES);
    stop(s);
}
static void readonly_copy(void)
{
    struct storage *s=start(0);
    CHECK(s->volumes[0].fat.readonly && !s->volumes[0].fat.writable_session);
    void *image=malloc((size_t)media.dev.capacity*512); CHECK(image);
    struct ciuki_floppy_image r={.version=CIUKI_MEDIA_VERSION,.size=sizeof(r),.drive=0,
        .generation=media.dev.media_generation,.sectors=(uint32_t)media.dev.capacity};
    unsigned writes=media.writes;
    OK(storage_floppy_image(s,&r,image,false));
    CHECK(s->volumes[0].fat.readonly && media.writes==writes);
    free(image); stop(s);
}
static void blank_and_copy(void)
{
    struct storage *s=storage_get(); OK(storage_setup(s,0));
    uint8_t blank[512]={0}; OK(fake_raw_write(&media,0,blank));
    OK(storage_add_floppy(s,&media.dev,0));
    CHECK(s->volumes[0].unformatted && s->volumes[0].present && !s->vfs.volumes[0]);
    struct ciuki_media_format r=request(0); OK(storage_format_removable(s,&r,false));
    CHECK(r.flags==CIUKI_FORMAT_UNFORMATTED && r.filesystem==12);
    r.flags=CIUKI_FORMAT_QUICK; OK(storage_format_removable(s,&r,true));
    CHECK(!s->volumes[0].unformatted && s->vfs.volumes[0]);
    void *image=malloc((size_t)media.dev.capacity*512); CHECK(image);
    struct ciuki_floppy_image c={.version=CIUKI_MEDIA_VERSION,.size=sizeof(c),.drive=0,
        .generation=media.dev.media_generation,.sectors=(uint32_t)media.dev.capacity};
    s->volumes[0].formatting=true;
    CHECK(storage_floppy_image(s,&c,image,false)==-FS_EBUSY && s->volumes[0].formatting);
    s->volumes[0].formatting=false;
    OK(storage_floppy_image(s,&c,image,false));
    unsigned writes=media.writes;
    c.sectors--; CHECK(storage_floppy_image(s,&c,image,true)==-FS_EINVAL); c.sectors++;
    c.generation++; CHECK(storage_floppy_image(s,&c,image,true)==-FS_EIO); c.generation--;
    CHECK(writes==media.writes);
    OK(storage_floppy_image(s,&c,image,true));
    CHECK(s->vfs.volumes[0] && !s->volumes[0].unformatted);
    media.fail_write=20; CHECK(storage_floppy_image(s,&c,image,true)<0);
    CHECK(!s->vfs.volumes[0] && s->volumes[0].unformatted);
    free(image); stop(s);
}
static void low_level_format(void)
{
    struct storage *s=start(0); struct ciuki_media_format r=request(0);
    OK(storage_format_removable(s,&r,false)); r.flags=CIUKI_FORMAT_LOW_LEVEL;
    unsigned before=formatted_tracks; OK(storage_format_removable(s,&r,true));
    CHECK(formatted_tracks==before+160 && s->vfs.volumes[0]);
    stop(s);
}
static void bad_clusters(void)
{
    struct storage *s=start(0);
    struct ciuki_media_format r=request(0); OK(storage_format_removable(s,&r,false)); r.flags=0;
    uint32_t lba=s->volumes[0].fat.data_sector+17;
    media.fail_read=lba;
    OK(storage_format_removable(s,&r,true));
    struct fat_volume *v=&s->volumes[0].fat;
    uint32_t cluster=2+(lba-v->data_sector)/v->spc, offset=cluster+cluster/2;
    uint8_t fat[1024];
    for (unsigned copy=0;copy<2;copy++) {
        OK(fake_raw_read(&media,v->reserved+copy*v->fat_sectors+offset/512,fat));
        OK(fake_raw_read(&media,v->reserved+copy*v->fat_sectors+offset/512+1,fat+512));
        uint16_t value=fs_rd16(fat+offset%512);
        CHECK((cluster&1 ? value>>4 : value&0xfff)==0xff7);
    }
    media.fail_read=UINT64_MAX;
    stop(s);
}
static void partition_and_family(void)
{
    struct storage *s=storage_get(); OK(storage_setup(s,0));
    struct blkdev partitioned={.read=partition_read,.write=partition_write,.flush=partition_flush,
        .capacity=media.dev.capacity+2048,.sector_size=512,.media_generation=media.dev.media_generation,
        .write_cache_state=BLKDEV_CACHE_ENABLED,.ctx=&media};
    OK(storage_add_removable(s,&partitioned));
    struct ciuki_media_format r=request(4); OK(storage_format_removable(s,&r,false));
    CHECK(r.supported_filesystems==CIUKI_FORMAT_FAT16 && s->volumes[4].partition);
    r.filesystem=12; r.flags=CIUKI_FORMAT_QUICK; unsigned writes=media.writes;
    CHECK(storage_format_removable(s,&r,true)==-FS_EOPNOTSUPP && media.writes==writes);
    r.filesystem=16; OK(storage_format_removable(s,&r,true));
    uint8_t boot[512]; OK(fake_raw_read(&media,0,boot)); CHECK(fs_rd32(boot+28)==2048);
    OK(storage_remove_removable(s,&partitioned,false));
    stop(s);
    /* Whole USB media may change family without an inconsistent MBR. */
    s=start(4); r=request(4); OK(storage_format_removable(s,&r,false));
    CHECK(r.supported_filesystems&CIUKI_FORMAT_FAT12);
    r.filesystem=12; r.flags=CIUKI_FORMAT_QUICK;
    OK(storage_format_removable(s,&r,true)); CHECK(s->vfs.volumes[4]->type==12);
    OK(storage_remove_removable(s,&media.dev,false));
    struct scan_result scan=independent_scan(media.fd,0);
    CHECK(!scan.corrupt && !scan.crosslinks && !scan.lost && !scan.divergent && !scan.dirty);
    stop(s);
}
int main(int argc, char **argv)
{
    CHECK(argc==4);
    for (unsigned i=0;i<3;i++) {
        OK(fake_open(&media,argv[i+1])); media.undo_enabled=true; media.dev.media_generation=19+i;
        unsigned family=i==0 ? 12 : i==1 ? 16 : 32;
        success(family,i==0 ? 0 : 4,true);
        success(family,i==0 ? 0 : 4,false);
        if (!i) { editor_lease(); readonly_copy(); blank_and_copy(); low_level_format(); bad_clusters(); refusals(); failures(); }
        if (i==1) partition_and_family();
        fake_close(&media);
    }
    printf("PASS removable format: FAT12/16/32 quick/full, labels, remount/write, stale/fixed/busy/durability/failure refusals checks=%u\n",checks);
    return 0;
}
