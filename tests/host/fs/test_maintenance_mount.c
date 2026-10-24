/* G29b production storage reservation/refusal and remount path. */
#include <ciuki/storage.h>
#undef CLOCKS_PER_SEC
#undef WIFEXITED
#undef WEXITSTATUS
#undef WIFSIGNALED
#undef WTERMSIG
#include <ciuki/abi.h>
#include <ciuki/kernel.h>
#include "fake.h"
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"MAINTENANCE %d: %s\n",__LINE__,#x); abort(); } } while (0)
#define OK(x) do { int e_=(x); if (e_) fprintf(stderr,"MAINTENANCE error %d at %d\n",e_,__LINE__); CHECK(!e_); } while(0)
struct ciuki_boot_info g_boot;
void klog(const char *format, ...) { (void)format; }
static struct fake disk;
static struct fat_volume seed_volume;
static struct block_cache seed_cache;
static struct storage *observed;
static unsigned exclusive;
void fs_host_service(void) {
    if (observed && observed->volumes[3].formatting && !observed->vfs.volumes[3]) {
        CHECK(observed->vfs.mutating&vfs_drive_mask(3)); exclusive++;
    }
}
static void one(bool writable) {
    struct storage *s=calloc(1,sizeof(*s)); CHECK(s); OK(storage_setup(s,0));
    OK(storage_add_disk(s,1,&disk.dev)); CHECK(s->vfs.volumes[3]);
    struct ciuki_media_convert c={.version=CIUKI_MEDIA_VERSION,.size=sizeof(c),.drive=3};
    unsigned writes=disk.writes;
    OK(storage_convert(s,&c,false)); CHECK(c.filesystem==16 && c.cluster_bytes==512 && disk.writes==writes);
    c.drive=2; CHECK(storage_convert(s,&c,false)==-FS_EROFS); c.drive=3;
    c.generation=99; CHECK(storage_convert(s,&c,false)==-FS_EIO); c.generation=0;
    s->volumes[3].removable=true; CHECK(storage_convert(s,&c,false)==-FS_EROFS); s->volumes[3].removable=false;
    if (writable) OK(storage_enable_write(s,3));
    struct vfs_table table; OK(vfs_table_init(&s->vfs,&table,3));
    int fd=vfs_open(&table,"/Seed.txt",VFS_READ,VFS_DENY_NONE,0); CHECK(fd>=0);
    struct ciuki_media_defrag r={.version=CIUKI_MEDIA_VERSION,.size=sizeof(r),.drive=3};
    OK(storage_defrag(s,&r,false)); CHECK(r.clusters && r.generation==0);
    CHECK(storage_defrag(s,&r,true)==-FS_EBUSY);
    writes=disk.writes; CHECK(storage_convert(s,&c,true)==-FS_EBUSY); CHECK(disk.writes==writes && s->vfs.volumes[3]);
    OK(vfs_close(&table,fd));
    if (writable) {
        struct ciuki_media_defrag old=r;
        fd=vfs_open(&table,"/New.txt",VFS_READ|VFS_WRITE|VFS_CREATE,VFS_DENY_NONE,0); CHECK(fd>=0);
        size_t done; OK(vfs_write(&table,fd,"kept",4,&done)); CHECK(done==4); OK(vfs_close(&table,fd));
        CHECK(storage_defrag(s,&old,true)==-FS_EAGAIN);
        OK(storage_defrag(s,&r,false)); OK(storage_defrag(s,&r,true)); CHECK(!r.moved);
    }
    uint32_t generation=s->vfs.generation[3]; observed=s; exclusive=0;
    OK(storage_convert(s,&c,true)); observed=0;
    CHECK(exclusive && s->vfs.generation[3]>generation && s->vfs.volumes[3] && s->volumes[3].fat.type==32);
    CHECK(s->volumes[3].fat.readonly==!writable && c.filesystem==32 && !s->volumes[3].formatting);
    if (writable) {
        fd=vfs_open(&table,"/New.txt",VFS_READ,VFS_DENY_NONE,0); CHECK(fd>=0);
        char b[4]; size_t done; OK(vfs_read(&table,fd,b,sizeof(b),&done)); CHECK(done==4 && !memcmp(b,"kept",4)); OK(vfs_close(&table,fd));
    }
    CHECK(storage_convert(s,&c,false)==-FS_EINVAL); /* already FAT32 */
    vfs_table_destroy(&table); storage_destroy(s); free(s); fake_reset(&disk);
}
int main(int argc,char **argv) {
    CHECK(argc==2); OK(fake_open(&disk,argv[1]));
    uint8_t mbr[512]={0}; mbr[450]=0x06; fs_wr32(mbr+454,2048); fs_wr32(mbr+458,66640); fs_wr16(mbr+510,0xaa55);
    OK(fake_raw_write(&disk,0,mbr));
    OK(cache_init(&seed_cache,2*1024*1024)); OK(fat_mount(&seed_volume,&seed_cache,&disk.dev,2048,66640,true));
    struct fat_entry file; size_t done; OK(fat_create(&seed_volume,0,"Seed.txt",0,&file));
    OK(fat_write(&seed_volume,&file,0,"seed",4,&done)); CHECK(done==4);
    OK(fat_unmount(&seed_volume)); cache_destroy(&seed_cache);
    disk.events=disk.writes=disk.flushes=0; disk.undo_enabled=true; one(false); one(true);
    fake_close(&disk); puts("PASS G29b storage: fixed generation=0, read-only query, removable/system/stale/busy refusals, exclusive conversion and RO/RW remount, data retained"); return 0;
}
