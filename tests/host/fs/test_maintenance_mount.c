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
#include <pthread.h>
unsigned int alarm(unsigned int); /* <unistd.h> clashes with the ABI header names */
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"MAINTENANCE %d: %s\n",__LINE__,#x); abort(); } } while (0)
#define OK(x) do { int e_=(x); if (e_) fprintf(stderr,"MAINTENANCE error %d at %d\n",e_,__LINE__); CHECK(!e_); } while(0)
struct ciuki_boot_info g_boot;
void klog(const char *format, ...) { (void)format; }
static struct fake disk;
static struct fat_volume seed_volume;
static struct block_cache seed_cache;
static struct storage *observed;
static unsigned exclusive;
static struct vfs_table table;
void fs_host_service(void) {
    if (observed && observed->volumes[3].formatting && !observed->vfs.volumes[3]) {
        CHECK(observed->vfs.mutating&vfs_drive_mask(3)); exclusive++;
    }
}
/* dec1: a start-log flush in progress, stalled where a stuck C: commit would
 * stall it (here: on the namespace this test holds). */
static int flush_result;
static void *flush_main(void *arg) {
    struct storage *s=arg; flush_result=vfs_startup_flush(&s->vfs,true); return NULL;
}
static void startup_log_refusals(struct storage *s) {
    struct fat_entry entry; OK(vfs_stat(&table,"/New.txt",&entry));
    const uint32_t order[1]={0};
    vfs_lock(&s->vfs,0); vfs_startup_record_locked(&s->vfs,&s->volumes[3].fat,&entry,order,1); vfs_unlock(&s->vfs);
    CHECK(s->vfs.startup_count==1);
    vfs_lock(&s->vfs,0);
    pthread_t flusher; CHECK(!pthread_create(&flusher,NULL,flush_main,s));
    for (unsigned i=0;i<5000 && !__atomic_load_n(&s->vfs.startup_flushing,__ATOMIC_ACQUIRE);i++) {
        struct timespec t={0,1000000}; nanosleep(&t,NULL);
    }
    CHECK(__atomic_load_n(&s->vfs.startup_flushing,__ATOMIC_ACQUIRE));
    /* Shut Down and the defragmenter's analysis refuse at once (a wait
     * would deadlock here; the alarm ends a blocked run). */
    alarm(20); uint32_t start=fs_now_ms();
    CHECK(storage_startup_log_shutdown(s)==-FS_EBUSY);
    struct ciuki_media_defrag q={.version=CIUKI_MEDIA_VERSION,.size=sizeof(q),.drive=3,.flags=CIUKI_DEFRAG_STARTUP};
    CHECK(storage_defrag(s,&q,false)==-FS_EBUSY);
    CHECK(vfs_startup_flush(&s->vfs,false)==-FS_EBUSY);
    CHECK(fs_now_ms()-start<1000); alarm(0);
    vfs_unlock(&s->vfs);
    CHECK(!pthread_join(flusher,NULL) && flush_result==1 && !s->vfs.startup_flushing);
    OK(storage_startup_log_shutdown(s)); OK(storage_defrag(s,&q,false)); CHECK(q.clusters);
    struct fat_entry log; OK(vfs_stat(&table,"/CIUKIST.LOG",&log)); CHECK(log.size==fat_startup_size());
}
static void one(bool writable) {
    struct storage *s=calloc(1,sizeof(*s)); CHECK(s); OK(storage_setup(s,0));
    OK(storage_add_disk(s,1,&disk.dev)); CHECK(s->vfs.volumes[3]);
    struct ciuki_media_convert c={.version=CIUKI_MEDIA_VERSION,.size=sizeof(c),.drive=3};
    unsigned writes=disk.writes;
    OK(storage_convert(s,&c,false)); CHECK(c.filesystem==16 && c.cluster_bytes==512 && disk.writes==writes);
    c.drive=2; CHECK(storage_convert(s,&c,false)==-FS_ENOENT); c.drive=3;
    c.generation=99; CHECK(storage_convert(s,&c,false)==-FS_EIO); c.generation=0;
    s->volumes[3].removable=true; CHECK(storage_convert(s,&c,false)==-FS_EROFS); s->volumes[3].removable=false;
    if (writable) OK(storage_enable_write(s,3));
    OK(vfs_table_init(&s->vfs,&table,3));
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
        startup_log_refusals(s);
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
static void boot_one(const char *mbr_path,const char *loader_path,bool dirty) {
    uint8_t b[512]; FILE *f=fopen(mbr_path,"rb"); CHECK(f && fread(b,512,1,f)==1); fclose(f);
    b[446]=0x80; b[450]=0x0e; fs_wr32(b+454,2048); fs_wr32(b+458,66640);
    OK(fake_raw_write(&disk,0,b)); f=fopen(loader_path,"rb"); CHECK(f);
    for (unsigned i=1;i<=48;i++) { CHECK(fread(b,512,1,f)==1); OK(fake_raw_write(&disk,i,b)); } fclose(f);
    struct storage *s=calloc(1,sizeof(*s)); CHECK(s); OK(storage_setup(s,0));
    OK(storage_add_disk(s,0,&disk.dev)); CHECK(s->vfs.volumes[2] && s->volumes[2].fat.type==16);
    struct ciuki_media_convert c={.version=CIUKI_MEDIA_VERSION,.size=sizeof(c),.drive=2};
    OK(storage_convert(s,&c,false)); CHECK(c.reserved==CIUKI_CONVERT_NEXT_BOOT && c.filesystem==16);
    bool pending=true; OK(fat_convert_boot_pending(&disk.dev,2048,66640,&pending)); CHECK(!pending);
    unsigned writes=disk.writes; c.reserved=0;
    CHECK(storage_convert(s,&c,true)==-FS_EROFS && disk.writes==writes); /* live conversion refused */
    c.reserved=CIUKI_CONVERT_NEXT_BOOT;
    OK(vfs_table_init(&s->vfs,&table,2));
    int fd=vfs_open(&table,"/Seed.txt",VFS_READ,VFS_DENY_NONE,0); CHECK(fd>=0);
    OK(storage_convert(s,&c,true)); CHECK(s->vfs.volumes[2] && s->volumes[2].fat.type==16 && c.filesystem==16);
    OK(fat_convert_boot_pending(&disk.dev,2048,66640,&pending)); CHECK(pending);
    char text[4]; size_t done; OK(vfs_read(&table,fd,text,4,&done)); CHECK(done==4 && !memcmp(text,"seed",4));
    OK(vfs_close(&table,fd)); vfs_table_destroy(&table);
    OK(storage_shutdown(s)); storage_destroy(s); free(s);
    if (dirty) {
        uint8_t boot[512]; OK(fake_raw_read(&disk,2048,boot));
        for (unsigned copy=0;copy<2;copy++) {
            uint32_t lba=2048+fs_rd16(boot+14)+copy*fs_rd16(boot+22);
            OK(fake_raw_read(&disk,lba,b)); fs_wr16(b+2,fs_rd16(b+2)&~0x8000u);
            OK(fake_raw_write(&disk,lba,b));
        }
    }
    s=calloc(1,sizeof(*s)); CHECK(s); OK(storage_setup(s,0));
    OK(storage_add_disk(s,0,&disk.dev)); CHECK(s->vfs.volumes[2] && s->volumes[2].fat.type==(dirty ? 16 : 32) && !s->volumes[2].error);
    OK(vfs_table_init(&s->vfs,&table,2)); fd=vfs_open(&table,"/Seed.txt",VFS_READ,VFS_DENY_NONE,0); CHECK(fd>=0);
    OK(vfs_read(&table,fd,text,4,&done)); CHECK(done==4 && !memcmp(text,"seed",4));
    OK(vfs_close(&table,fd)); vfs_table_destroy(&table);
    OK(fat_convert_boot_pending(&disk.dev,2048,66640,&pending)); CHECK(!pending);
    storage_destroy(s); free(s); fake_reset(&disk);
    puts("PASS boot conversion: explicit deferred consent, live handles retained, no immediate conversion, conversion before C: publication on next storage start; safe FAT16 fallback for pre-commit refusal");
}
int main(int argc,char **argv) {
    CHECK(argc==4); OK(fake_open(&disk,argv[1]));
    uint8_t mbr[512]={0}; mbr[450]=0x06; fs_wr32(mbr+454,2048); fs_wr32(mbr+458,66640); fs_wr16(mbr+510,0xaa55);
    OK(fake_raw_write(&disk,0,mbr));
    OK(cache_init(&seed_cache,2*1024*1024)); OK(fat_mount(&seed_volume,&seed_cache,&disk.dev,2048,66640,true));
    struct fat_entry file; size_t done; OK(fat_create(&seed_volume,0,"Seed.txt",0,&file));
    OK(fat_write(&seed_volume,&file,0,"seed",4,&done)); CHECK(done==4);
    OK(fat_unmount(&seed_volume)); cache_destroy(&seed_cache);
    disk.events=disk.writes=disk.flushes=0; disk.undo_enabled=true; one(false); one(true); boot_one(argv[2],argv[3],false); boot_one(argv[2],argv[3],true);
    fake_close(&disk); puts("PASS G29b storage: fixed generation=0, read-only query, removable/system/stale/busy refusals, exclusive conversion and RO/RW remount, data retained; a start-log flush in progress: Shut Down and defrag analysis refuse EBUSY at once, succeed after it"); return 0;
}
