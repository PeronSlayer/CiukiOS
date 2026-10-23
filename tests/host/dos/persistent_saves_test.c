/* Production journal and FAT/VFS store; deterministic power cuts, no guest.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/abi.h>
#include "../../../src/kernel/vm/dos_save_store.h"
#include "../../../src/kernel/vm/dos_file_volume.h"
#include <ciuki/storage.h>
#include <ciuki/blkpart.h>
#include <assert.h>
#include <stdio.h>
#define OK(x) assert((x)==0)
#define SMALL (4u*1024u*1024u)
struct cut_file {
    uint8_t live[SMALL],stable[SMALL];
    uint32_t size,stable_size;
    unsigned events,cut,mode;
    bool stopped;
};
static int cut_read(void *arg,uint32_t at,void *buf,unsigned n)
{ struct cut_file *f=arg; if (at>f->size || n>f->size-at) return -FS_EIO; memcpy(buf,f->live+at,n); return 0; }
static int cut_write(void *arg,uint32_t at,const void *buf,unsigned n)
{
    struct cut_file *f=arg; assert(at+n<=SMALL);
    if (f->stopped) return -FS_EIO;
    if (++f->events==f->cut) {
        /* None, all, or a torn prefix can reach media before power goes. */
        unsigned prefix=f->mode==1 ? n : f->mode==2 ? 173u : 0;
        if (prefix) memcpy(f->live+at,buf,prefix);
        if (prefix && f->size<at+prefix) f->size=at+prefix;
        f->stopped=true; return -FS_EIO;
    }
    memcpy(f->live+at,buf,n); if (f->size<at+n) f->size=at+n; return 0;
}
static int cut_flush(void *arg)
{
    struct cut_file *f=arg; if (f->stopped) return -FS_EIO;
    if (++f->events==f->cut) { f->stopped=true; return -FS_EIO; }
    memcpy(f->stable,f->live,f->size); f->stable_size=f->size; return 0;
}
static int cut_resize(void *arg,uint32_t n)
{ struct cut_file *f=arg; assert(n<=f->size); f->size=n; return 0; }
static struct dos_save_identity identity(unsigned game,char content)
{
    struct dos_save_identity id={.game=game,.kernel=2}; memset(id.base,content,64); id.base[64]=0; return id;
}
static struct dos_save_io cut_ops(struct cut_file *f)
{ return (struct dos_save_io){cut_read,cut_write,cut_flush,cut_resize,f}; }
static void crash_tests(void)
{
    struct cut_file *baseline=calloc(1,sizeof(*baseline)),*f=calloc(1,sizeof(*f)); assert(f && baseline);
    struct dos_save_identity id=identity(1,'a'); struct dos_save s={0}; struct dos_save_io io=cut_ops(baseline);
    OK(dos_save_open(&s,&io,&id,0)); uint8_t old[512],next[512],got[512];
    memset(old,0x31,512); memset(next,0xa2,512);
    OK(dos_save_write(&s,7,old)); OK(dos_save_write(&s,8,old)); OK(dos_save_commit(&s,true));
    dos_save_release(&s);
    /* Creation itself has two writes and two barriers. An incomplete first
     * generation is classified for preservation, never accepted as data. */
    for (unsigned cut=1;cut<=4;cut++) for (unsigned mode=0;mode<3;mode++) {
        memset(f,0,sizeof(*f)); f->cut=cut; f->mode=mode;
        s=(struct dos_save){0}; io=cut_ops(f);
        assert(dos_save_open(&s,&io,&id,0)<0); dos_save_release(&s);
        f->cut=0; f->stopped=false; s=(struct dos_save){0};
        int e=dos_save_open(&s,&io,&id,f->size);
        assert(!e || e==DOS_SAVE_MISMATCH); assert(!s.rows); dos_save_release(&s);
    }
    /* Four record writes, data flush, commit write, final flush. */
    for (unsigned cut=1;cut<=7;cut++) for (unsigned mode=0;mode<3;mode++) for (unsigned loss=0;loss<2;loss++) {
        *f=*baseline; f->events=0; f->cut=cut; f->mode=mode;
        s=(struct dos_save){0}; io=cut_ops(f); OK(dos_save_open(&s,&io,&id,f->size));
        int e=dos_save_write(&s,7,next); if (!e) e=dos_save_write(&s,8,next);
        if (!e) e=dos_save_commit(&s,false);
        assert(e && f->stopped); dos_save_release(&s);
        if (loss) { memcpy(f->live,f->stable,f->stable_size); f->size=f->stable_size; }
        f->stopped=false; f->cut=0; s=(struct dos_save){0};
        OK(dos_save_open(&s,&io,&id,f->size));
        OK(dos_save_read(&s,7,got)); bool newer=!memcmp(got,next,512);
        assert(newer || !memcmp(got,old,512));
        OK(dos_save_read(&s,8,got)); assert(!memcmp(got,newer ? next : old,512));
        /* A recovered transaction remains appendable after its torn tail. */
        OK(dos_save_write(&s,9,next)); OK(dos_save_commit(&s,true)); dos_save_release(&s);
    }
    *f=*baseline; s=(struct dos_save){.quota=8192}; io=cut_ops(f);
    OK(dos_save_open(&s,&io,&id,f->size));
    while (dos_save_write(&s,7,next)==0) { }
    unsigned writes=f->events; assert(s.full && s.readonly && s.end<=s.quota-512);
    assert(dos_save_write(&s,8,next)==-FS_EROFS && f->events==writes);
    OK(dos_save_read(&s,7,got)); OK(dos_save_commit(&s,false)); OK(dos_save_commit(&s,true)); dos_save_release(&s);
    s=(struct dos_save){.quota=8192}; OK(dos_save_open(&s,&io,&id,f->size));
    OK(dos_save_read(&s,7,got)); assert(!memcmp(got,next,512)); dos_save_release(&s);
    struct dos_save_identity changed=id; changed.kernel=1;
    s=(struct dos_save){0}; writes=f->events;
    assert(dos_save_open(&s,&io,&changed,f->size)==DOS_SAVE_MISMATCH && f->events==writes);
    *f=*baseline; f->events=f->cut=0; s=(struct dos_save){0};
    OK(dos_save_open(&s,&io,&id,f->size)); assert(s.quota==64u*1024u*1024u);
    for (unsigned lba=1024;lba<3073;lba++) OK(dos_save_write(&s,lba,next));
    OK(dos_save_write(&s,DOS_SAVE_SECTORS-1,next));
    OK(dos_save_commit(&s,true)); dos_save_release(&s);
    s=(struct dos_save){0}; OK(dos_save_open(&s,&io,&id,f->size)); assert(s.rows==2052);
    OK(dos_save_read(&s,3072,got)); assert(!memcmp(got,next,512));
    OK(dos_save_read(&s,DOS_SAVE_SECTORS-1,got)); assert(!memcmp(got,next,512)); dos_save_release(&s);
    free(f); free(baseline);
    puts("PASS journal: every record/marker write and both flush cuts, lost/reordered/torn tails, atomic two-sector recovery, quota, identity");
}
#define DISK_SECTORS 8192u
#define SYSTEM_SECTORS 32768u
static uint8_t system_disk[SYSTEM_SECTORS*512u],base_disk[DISK_SECTORS*512u];
static unsigned physical_writes,physical_cut_at,physical_torn;
static bool physical_stopped;
static int memory_read(struct blkdev *d,uint64_t lba,uint32_t count,void *buf)
{ int e=blkdev_range(d,lba,count); if (!e) memcpy(buf,(uint8_t *)d->ctx+lba*512u,(size_t)count*512u); return e; }
static int memory_write(struct blkdev *d,uint64_t lba,uint32_t count,const void *buf)
{
    int e=blkdev_range(d,lba,count); if (e) return e;
    for (unsigned i=0;i<count;i++) {
        if (physical_stopped) return -FS_EIO;
        if (++physical_writes==physical_cut_at) {
            if (physical_torn) memcpy((uint8_t *)d->ctx+(lba+i)*512u,(const uint8_t *)buf+i*512u,physical_torn);
            physical_stopped=true; return -FS_EIO;
        }
        memcpy((uint8_t *)d->ctx+(lba+i)*512u,(const uint8_t *)buf+i*512u,512);
    }
    return 0;
}
static int memory_flush(struct blkdev *d) { (void)d; return 0; }
static void format(uint8_t *disk,unsigned sectors)
{
    unsigned fat_sectors=sectors/256u;
    memset(disk,0,sectors*512u); uint8_t *b=disk;
    b[0]=0xeb; b[1]=0x3c; b[2]=0x90; memcpy(b+3,"CIUSAVE ",8);
    fs_wr16(b+11,512); b[13]=1; fs_wr16(b+14,1); b[16]=2;
    fs_wr16(b+17,512); fs_wr16(b+19,(uint16_t)sectors); b[21]=0xf8; fs_wr16(b+22,(uint16_t)fat_sectors);
    fs_wr16(b+24,32); fs_wr16(b+26,4); b[38]=0x29; fs_wr32(b+39,0x640064);
    memcpy(b+43,"SAVE TEST  ",11); memcpy(b+54,"FAT16   ",8); b[510]=0x55; b[511]=0xaa;
    for (unsigned i=0;i<2;i++) { fs_wr16(disk+(1+i*fat_sectors)*512,0xfff8); fs_wr16(disk+(1+i*fat_sectors)*512+2,0xffff); }
}
static struct storage manager_storage;
#define cache manager_storage.cache
#define space manager_storage.vfs
static bool manager_busy;
static unsigned reservations;
struct storage *storage_get(void) { return &manager_storage; }
int dosvm_game_remove_reserve(unsigned game)
{ assert(game && game<=3); if (manager_busy) return -FS_EBUSY; reservations++; return 0; }
void dosvm_game_remove_release(unsigned game) { assert(game && reservations); reservations--; }
int dosvm_game_data(unsigned,unsigned,struct ciuki_dosvm_command *);
static struct fat_volume native;
static struct vfs_table table;
static struct blkdev system_device,base_device,merged_device;
static struct dos_save save;
static struct dos_save_file save_file;
static int merged_read(struct blkdev *d,uint64_t lba,uint32_t count,void *buf)
{
    (void)d; int e=0;
    for (unsigned i=0;!e && i<count;i++) {
        e=dos_save_read(&save,(uint32_t)lba+i,(uint8_t *)buf+i*512u);
        if (e==-FS_ENOENT) e=memory_read(&base_device,lba+i,1,(uint8_t *)buf+i*512u);
    }
    return e;
}
static int merged_write(struct blkdev *d,uint64_t lba,uint32_t count,const void *buf)
{ (void)d; int e=0; for (unsigned i=0;!e && i<count;i++) e=dos_save_write(&save,(uint32_t)lba+i,(const uint8_t *)buf+i*512u); return e; }
static int merged_flush(struct blkdev *d) { (void)d; return dos_save_commit(&save,false); }
static void namespace_open(struct dos_save_identity *id,char path[128],unsigned *archived)
{
    save=(struct dos_save){0}; save_file=(struct dos_save_file){.table=&table};
    OK(dos_save_store_open(&save_file,&save,id,path,archived));
}
static void namespace_close(bool orderly)
{
    if (orderly) OK(dos_save_commit(&save,true));
    dos_save_release(&save); OK(vfs_close(&table,save_file.fd));
}
static void write_file(struct vfs_table *t,const char *path,const char *text)
{
    int fd=vfs_open(t,path,VFS_WRITE|VFS_CREATE,VFS_DENY_ALL,0); assert(fd>=0);
    size_t done=0; OK(vfs_write(t,fd,text,strlen(text),&done)); assert(done==strlen(text));
    OK(vfs_commit(t,fd)); OK(vfs_close(t,fd));
}
static void read_file(struct vfs_table *t,const char *path,const char *text)
{
    int fd=vfs_open(t,path,VFS_READ,VFS_DENY_WRITE,0); assert(fd>=0);
    char buf[80]={0}; size_t done=0; OK(vfs_read(t,fd,buf,sizeof(buf),&done));
    assert(done==strlen(text) && !memcmp(buf,text,done)); OK(vfs_close(t,fd));
}
static void store_tests(void)
{
    format(system_disk,SYSTEM_SECTORS); format(base_disk,DISK_SECTORS);
    system_device=(struct blkdev){.read=memory_read,.write=memory_write,.flush=memory_flush,
        .capacity=SYSTEM_SECTORS,.sector_size=512,.write_cache_state=BLKDEV_CACHE_DISABLED,.ctx=system_disk};
    base_device=system_device; base_device.capacity=DISK_SECTORS; base_device.ctx=base_disk; base_device.logical=true;
    merged_device=base_device; merged_device.read=merged_read; merged_device.write=merged_write; merged_device.flush=merged_flush;
    OK(cache_init(&cache,65536)); OK(fat_mount(&native,&cache,&system_device,0,SYSTEM_SECTORS,true));
    vfs_init(&space); OK(vfs_attach(&space,2,&native)); OK(vfs_table_init(&space,&table,2));
    OK(vfs_mkdir(&table,"C:\\SYSTEM",0)); OK(vfs_mkdir(&table,"C:\\SYSTEM\\DOS",0)); OK(vfs_mkdir(&table,"C:\\Documents",0));
    struct dos_file_volume *original=calloc(1,sizeof(*original)); assert(original);
    OK(dos_file_volume_open(original,&cache,&base_device,true));
    write_file(&original->table,"C:\\UNCHANGED.BIN","unchanged base file");
    OK(dos_file_volume_close(original)); free(original);
    int image=vfs_open(&table,"C:\\SYSTEM\\DOS\\UNIFIED.IMG",VFS_WRITE|VFS_CREATE,VFS_DENY_ALL,0); assert(image>=0);
    size_t done=0; OK(vfs_write(&table,image,base_disk,sizeof(base_disk),&done)); assert(done==sizeof(base_disk));
    OK(vfs_commit(&table,image)); OK(vfs_close(&table,image));
    manager_storage.ready=true;
    struct dos_save_identity id=identity(1,'a'); unsigned archived; char path[128];
    uint8_t digest[32]; sha256(base_disk,sizeof(base_disk),digest); sha256_hex(digest,id.base);
    namespace_open(&id,path,&archived); assert(!archived);
    struct dos_file_volume *guest=calloc(1,sizeof(*guest)); assert(guest);
    OK(dos_file_volume_open(guest,&cache,&merged_device,true));
    OK(vfs_mkdir(&guest->table,"C:\\GAME",0));
    write_file(&guest->table,"C:\\GAME\\SAVE.DAT","saved level 7");
    write_file(&guest->table,"C:\\GAME\\SETUP.CFG","sound=sb16");
    OK(dos_file_volume_close(guest)); namespace_close(true);
    /* Reboot: all volatile cache/namespace ownership disappears. */
    vfs_table_destroy(&table); OK(vfs_detach(&space,2)); vfs_destroy(&space); cache_destroy(&cache);
    OK(cache_init(&cache,65536)); OK(fat_mount(&native,&cache,&system_device,0,SYSTEM_SECTORS,true));
    vfs_init(&space); OK(vfs_attach(&space,2,&native)); OK(vfs_table_init(&space,&table,2));
    namespace_open(&id,path,&archived); assert(save.rows && save.sealed && !archived);
    /* Forced close preserves only the previously committed frontier. */
    uint8_t pending[512]; memset(pending,0x64,sizeof(pending));
    OK(dos_save_write(&save,7000,pending)); namespace_close(false);
    namespace_open(&id,path,&archived); assert(save.recovered_bytes==1024);
    assert(dos_save_read(&save,7000,pending)==-FS_ENOENT);
    OK(dos_file_volume_open(guest,&cache,&merged_device,true));
    read_file(&guest->table,"C:\\GAME\\SAVE.DAT","saved level 7");
    read_file(&guest->table,"C:\\GAME\\SETUP.CFG","sound=sb16");
    OK(dos_file_volume_close(guest));
    char exported[32]; uint32_t files;
    int exported_error=dos_save_store_export(&table,&merged_device,&base_device,&cache,1,exported,&files);
    if (exported_error) fprintf(stderr,"export error %d\n",exported_error);
    OK(exported_error); assert(files==2);
    char saved[128]; snprintf(saved,sizeof(saved),"%s\\GAME\\SAVE.DAT",exported); read_file(&table,saved,"saved level 7");
    namespace_close(true);
    struct ciuki_dosvm_command command={0};
    manager_busy=true;
    OK(dosvm_game_data(1,CIUKI_DOSVM_SAVE_QUERY,&command)); assert(command.changed && command.reason==DOS_SAVE_QUOTA);
    assert(dosvm_game_data(1,CIUKI_DOSVM_SAVE_RESET,&command)==-FS_EBUSY);
    assert(dosvm_game_data(1,CIUKI_DOSVM_SAVE_EXPORT,&command)==-FS_EBUSY);
    manager_busy=false; command=(struct ciuki_dosvm_command){0};
    OK(dosvm_game_data(1,CIUKI_DOSVM_SAVE_EXPORT,&command)); assert(command.changed==2 && !reservations);
    snprintf(saved,sizeof(saved),"%s\\GAME\\SAVE.DAT",command.name); read_file(&table,saved,"saved level 7");
    struct dos_save_identity other=identity(2,'a'); namespace_open(&other,path,&archived);
    OK(dos_file_volume_open(guest,&cache,&merged_device,true));
    struct fat_entry entry; assert(vfs_stat(&guest->table,"C:\\GAME\\SAVE.DAT",&entry)==-FS_ENOENT);
    write_file(&guest->table,"C:\\OTHER.SAV","title two"); OK(dos_file_volume_close(guest)); namespace_close(true);
    id=identity(1,'b'); namespace_open(&id,path,&archived); assert(archived==1 && !save.rows); namespace_close(true);
    struct vfs_find find; OK(vfs_find_first(&table,DOS_SAVE_ROOT "\\G1-*.BAK",0,&find,&entry));
    assert(entry.size>1024); uint32_t bytes=0; OK(dos_save_store_query(&table,1,&bytes)); assert(bytes==1024);
    id.kernel=1; namespace_open(&id,path,&archived); assert(archived==1 && !save.rows); namespace_close(true);
    /* Same namespace, incompatible filesystem identity with a valid header
     * checksum: preserve it as well, without attaching any old sectors. */
    int incompatible=vfs_open(&table,path,VFS_READ|VFS_WRITE,VFS_DENY_ALL,0); assert(incompatible>=0);
    uint8_t header[512]; size_t read=0; uint64_t pos;
    OK(vfs_read(&table,incompatible,header,sizeof(header),&read)); assert(read==512);
    header[24]='X'; sha256(header,480,header+480);
    OK(vfs_seek(&table,incompatible,0,VFS_SEEK_SET,&pos)); OK(vfs_write(&table,incompatible,header,512,&done)); assert(done==512);
    OK(vfs_commit(&table,incompatible)); OK(vfs_close(&table,incompatible));
    namespace_open(&id,path,&archived); assert(archived==1 && !save.rows); namespace_close(true);
    OK(dosvm_game_data(1,CIUKI_DOSVM_SAVE_RESET,&command)); assert(!reservations);
    OK(dos_save_store_query(&table,1,&bytes)); assert(!bytes);
    namespace_open(&other,path,&archived); OK(dos_file_volume_open(guest,&cache,&merged_device,true));
    read_file(&guest->table,"C:\\OTHER.SAV","title two"); OK(dos_file_volume_close(guest)); namespace_close(true);
    free(guest);
    vfs_table_destroy(&table); OK(vfs_detach(&space,2)); vfs_destroy(&space); cache_destroy(&cache);
    /* A read-only host can replay existing data, but cannot silently bypass
     * a stale identity when it cannot preserve that file under a dated name. */
    OK(cache_init(&cache,65536)); OK(fat_mount(&native,&cache,&system_device,0,SYSTEM_SECTORS,false));
    vfs_init(&space); OK(vfs_attach(&space,2,&native)); OK(vfs_table_init(&space,&table,2));
    namespace_open(&other,path,&archived); assert(save.readonly && !save_file.writable); namespace_close(false);
    other.kernel=1; save=(struct dos_save){0}; save_file=(struct dos_save_file){.table=&table};
    assert(dos_save_store_open(&save_file,&save,&other,path,&archived)==-FS_EROFS && !save_file.absent && !archived);
    other=identity(3,'a'); save=(struct dos_save){0}; save_file=(struct dos_save_file){.table=&table};
    assert(dos_save_store_open(&save_file,&save,&other,path,&archived)==-FS_EROFS && save_file.absent);
    vfs_table_destroy(&table); OK(vfs_detach(&space,2)); vfs_destroy(&space); cache_destroy(&cache);
    puts("PASS FAT/VFS: save/close/reboot/reopen, setup, identity archive, two titles isolated, changed-file export, reset");
}
/* fx-4: the canonical UNIFIED.IMG is partitioned (MBR, one active FAT16
 * partition of type 0Eh at LBA 63). Export mounted only type 06h and
 * refused that layout with EIO; it now mounts the volume the VM uses as C:. */
static uint8_t part_disk[DISK_SECTORS*512u];
static void partitioned_export_test(void)
{
    const unsigned volume=DISK_SECTORS-256u; /* format()'s FAT sizing needs whole 256-sector units */
    memset(part_disk,0,sizeof(part_disk)); format(part_disk+63u*512u,volume);
    part_disk[446]=0x80; part_disk[450]=0x0e; fs_wr32(part_disk+454,63); fs_wr32(part_disk+458,volume);
    part_disk[510]=0x55; part_disk[511]=0xaa;
    format(system_disk,SYSTEM_SECTORS);
    OK(cache_init(&cache,65536)); OK(fat_mount(&native,&cache,&system_device,0,SYSTEM_SECTORS,true));
    vfs_init(&space); OK(vfs_attach(&space,2,&native)); OK(vfs_table_init(&space,&table,2));
    OK(vfs_mkdir(&table,"C:\\SYSTEM",0)); OK(vfs_mkdir(&table,"C:\\SYSTEM\\DOS",0)); OK(vfs_mkdir(&table,"C:\\Documents",0));
    base_device.ctx=part_disk;
    struct dos_save_identity id=identity(2,'e'); unsigned archived; char path[128];
    namespace_open(&id,path,&archived); assert(!archived);
    /* The guest writes through the same C: view dos_file_vm.c builds. */
    static struct partition_table parts; static struct blkpart view;
    OK(partition_scan(&merged_device,&parts));
    const struct partition *c=partition_dos_primary(&parts); assert(c && c->type==0x0e && c->start==63 && c->bootable);
    OK(blkpart_init(&view,&merged_device,c)); struct blkdev *drive=blkpart_device(&view); drive->logical=true;
    struct dos_file_volume *guest=calloc(1,sizeof(*guest)); assert(guest);
    OK(dos_file_volume_open(guest,&cache,drive,true));
    OK(vfs_mkdir(&guest->table,"C:\\DOOM",0)); write_file(&guest->table,"C:\\DOOM\\DOOMSAV0.DSG","e1m1 saved");
    OK(dos_file_volume_close(guest)); free(guest);
    char exported[32]; uint32_t files=0;
    OK(dos_save_store_export(&table,&merged_device,&base_device,&cache,2,exported,&files)); assert(files==1);
    char saved[128]; snprintf(saved,sizeof(saved),"%s\\DOOM\\DOOMSAV0.DSG",exported); read_file(&table,saved,"e1m1 saved");
    namespace_close(true); base_device.ctx=base_disk;
    vfs_table_destroy(&table); OK(vfs_detach(&space,2)); vfs_destroy(&space); cache_destroy(&cache);
    printf("PASS fx-4 export: 0Eh partition at LBA 63 (unified layout), changed file exported as %s\n",saved);
}
static void physical_mount(void)
{
    physical_stopped=false; physical_cut_at=0;
    OK(cache_init(&cache,65536)); OK(fat_mount(&native,&cache,&system_device,0,SYSTEM_SECTORS,true));
    vfs_init(&space); OK(vfs_attach(&space,2,&native)); OK(vfs_table_init(&space,&table,2));
}
static void physical_abandon(void)
{
    dos_save_release(&save); vfs_table_destroy(&table); vfs_destroy(&space); cache_destroy(&cache);
}
static void physical_cut_tests(void)
{
    uint8_t old[512],next[512],got[512]; memset(old,0x11,512); memset(next,0x22,512);
    struct dos_save_identity id=identity(1,'c'); unsigned archived; char path[128];
    format(system_disk,SYSTEM_SECTORS); physical_mount();
    OK(vfs_mkdir(&table,"C:\\SYSTEM",0)); OK(vfs_mkdir(&table,"C:\\SYSTEM\\DOS",0));
    namespace_open(&id,path,&archived);
    OK(dos_save_write(&save,7,old)); OK(dos_save_write(&save,8,old)); namespace_close(true);
    vfs_table_destroy(&table); OK(vfs_detach(&space,2)); vfs_destroy(&space); cache_destroy(&cache);
    uint8_t *baseline=malloc(sizeof(system_disk)); assert(baseline); memcpy(baseline,system_disk,sizeof(system_disk));
    physical_mount(); namespace_open(&id,path,&archived); physical_writes=0;
    OK(dos_save_write(&save,7,next)); OK(dos_save_write(&save,8,next)); OK(dos_save_commit(&save,false));
    unsigned events=physical_writes; assert(events); physical_abandon();
    for (unsigned cut=1;cut<=events;cut++) for (unsigned torn=0;torn<2;torn++) {
        memcpy(system_disk,baseline,sizeof(system_disk)); physical_mount(); namespace_open(&id,path,&archived);
        physical_writes=0; physical_cut_at=cut; physical_torn=torn ? 173 : 0;
        int e=dos_save_write(&save,7,next); if (!e) e=dos_save_write(&save,8,next);
        if (!e) e=dos_save_commit(&save,false);
        assert(e && physical_stopped); physical_abandon();
        physical_mount(); namespace_open(&id,path,&archived); assert(!archived);
        OK(dos_save_read(&save,7,got)); bool newer=!memcmp(got,next,512);
        assert(newer || !memcmp(got,old,512)); OK(dos_save_read(&save,8,got));
        assert(!memcmp(got,newer ? next : old,512));
        physical_abandon();
    }
    free(baseline);
    printf("PASS outer FAT/VFS power cuts: every physical write, writes=%u cuts=%u (lost/torn), both saved sectors atomic after remount\n",events,events*2u);
}
int main(void) { crash_tests(); store_tests(); partitioned_export_test(); physical_cut_tests(); assert(!fs_host_contig_live); fflush(stdout); return 0; }
