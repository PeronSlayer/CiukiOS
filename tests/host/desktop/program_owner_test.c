/* Actual owner provider over nested fixture FAT/VFS volumes. */
#define CIUKI_CPU_H
#include <stdint.h>
static uint32_t irq_save(void){return 0;}
static void irq_restore(uint32_t flags){(void)flags;}
#include "../../../src/kernel/vm/dosvm_programs.c"
#include "../fs/fake.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct storage storage;
static struct fake disk;
static unsigned reservations,invalidations;
static int reserve_error;
void kpreempt_point(void) {}
struct storage *storage_get(void){return &storage;}
int dosvm_game_remove_reserve(unsigned game){assert(game==2);if(reserve_error)return reserve_error;reservations++;return 0;}
void dosvm_game_remove_release(unsigned game){assert(game==2 && reservations);reservations--;}
void dosvm_game_digest_invalidate(void){invalidations++;}
/* fx-4: UNIFIED.IMG shaped like the reference unified base (MBR, one
 * active FAT16 partition of type 0Eh at LBA 63). Before fx-4 the inventory
 * accepted only type 06h, mounted the MBR as a BPB and failed every launch
 * (T23 trial 9a60215f: DOOM/Quake error=-5 stage=vm-create). */
static void patch_type(struct vfs_table *table,uint8_t type)
{
    int fd=vfs_open(table,"C:\\SYSTEM\\DOS\\UNIFIED.IMG",VFS_READ|VFS_WRITE,VFS_DENY_ALL,0);assert(fd>=0);
    uint64_t at;size_t done=0;assert(!vfs_seek(table,fd,450,VFS_SEEK_SET,&at) && at==450);
    assert(!vfs_write(table,fd,&type,1,&done) && done==1);
    assert(!vfs_commit(table,fd) && !vfs_close(table,fd));
}
static int partitioned(void)
{
    assert(!cache_init(&storage.cache,128*1024));
    struct fat_volume *outer=&storage.volumes[2].fat;
    assert(!fat_mount(outer,&storage.cache,&disk.dev,0,disk.dev.capacity,false));
    vfs_init(&storage.vfs);assert(!vfs_attach(&storage.vfs,2,outer));storage.ready=true;
    struct vfs_table table;assert(!vfs_table_init(&storage.vfs,&table,2));
    uint32_t size,sizes[4]={0};const char *step,*detail;
    /* Every title is found inside the 0Eh partition, repeatedly (launch and
     * Programs-list queries), with no contiguous workspace left behind. */
    for(unsigned cycle=0;cycle<24;cycle++)
        for(unsigned game=1;game<=3;game++) {
            size=0;assert(!dosvm_game_program_checked(game,false,&size,&step,&detail));
            assert(size && !strcmp(step,"none") && !strcmp(detail,"none"));
            if(!cycle)sizes[game]=size;else assert(size==sizes[game]);
        }
    assert(!fs_host_contig_live && !owner.busy);
    printf("fx-4 partitioned unified inventory: WOLF3D=%u DOOM=%u QUAKE=%u bytes, 72 queries\n",sizes[1],sizes[2],sizes[3]);
    /* No FAT partition (83h): the whole-image view mounts the MBR, the
     * exact trial failure, now named: step=mount, FAT diagnostic. Since
     * fx-classaudit a missing boot jump is only a mount notice, so the MBR
     * (no 512 at offset 11) is refused for its signature/sector size. */
    assert(!fat_enable_write(outer));
    patch_type(&table,0x83);
    assert(dosvm_game_program_checked(2,false,&size,&step,&detail)==-FS_EINVAL);
    assert(!strcmp(step,"mount") && !strcmp(detail,"BPB signature/sector size") && !size);
    printf("fx-4 refusal without a FAT partition: error=%d step=%s detail=%s\n",-FS_EINVAL,step,detail);
    /* Microsoft FAT16 IDs 04h/06h address the same volume; 0Eh restored. */
    static const uint8_t types[]={0x06,0x04,0x0e};
    for(unsigned i=0;i<3;i++){patch_type(&table,types[i]);assert(!dosvm_game_program_checked(2,false,&size,&step,&detail) && size==sizes[2]);}
    /* Removal edits the partition's FAT, then republishes the identity. */
    assert(!dosvm_game_program(2,true,&size) && size==sizes[2] && !reservations && invalidations);
    assert(dosvm_game_program(2,false,&size)==1 && !size);   /* pending marker, as the superfloppy case */
    assert(!vfs_delete(&table,"C:\\SYSTEM\\REMOVE2.PND"));
    assert(dosvm_game_program_checked(2,false,&size,&step,&detail)==-FS_ENOENT && !strcmp(step,"program-tree"));
    assert(!dosvm_game_program(1,false,&size) && size==sizes[1]);
    assert(!dosvm_game_program(3,false,&size) && size==sizes[3]);
    vfs_table_destroy(&table);assert(!vfs_detach(&storage.vfs,2));vfs_destroy(&storage.vfs);
    cache_destroy(&storage.cache);fake_close(&disk);assert(!fs_host_contig_live);
    puts("PASS fx-4 owner provider: 0Eh partition at LBA 63 (unified layout), 04h/06h, named refusal without a FAT partition, removal inside the partition");fflush(stdout);
    return 0;
}
int main(int argc,char **argv)
{
    if(argc==3 && !strcmp(argv[2],"partitioned")){assert(!fake_open(&disk,argv[1]));return partitioned();}
    assert(argc==2 && !fake_open(&disk,argv[1]));
    assert(!cache_init(&storage.cache,128*1024));
    struct fat_volume *outer=&storage.volumes[2].fat;
    assert(!fat_mount(outer,&storage.cache,&disk.dev,0,disk.dev.capacity,false));
    vfs_init(&storage.vfs);assert(!vfs_attach(&storage.vfs,2,outer));storage.ready=true;
    struct vfs_table table;assert(!vfs_table_init(&storage.vfs,&table,2));
    uint32_t size;assert(!dosvm_game_program(2,false,&size) && size);
    unsigned writes=disk.writes;
    assert(dosvm_game_program(2,true,&size)==-FS_EROFS && writes==disk.writes && !reservations);
    assert(!fat_enable_write(outer));
    reserve_error=-FS_EBUSY;writes=disk.writes;
    assert(dosvm_game_program(2,true,&size)==-FS_EBUSY && writes==disk.writes);reserve_error=0;
    /* A live COW blocks cleanup, simulating failure after the game deletion. */
    int cow=vfs_open(&table,"C:\\SYSTEM\\DOS\\G2K1.COW",VFS_READ,VFS_DENY_ALL,0);assert(cow>=0);
    assert(dosvm_game_program(2,true,&size)==-FS_EBUSY && !reservations && invalidations);
    struct fat_entry entry;assert(!vfs_stat(&table,"C:\\SYSTEM\\REMOVE2.PND",&entry));
    assert(dosvm_game_program(2,false,&size)==1 && !size);
    assert(!vfs_close(&table,cow));
    assert(!dosvm_game_program(2,true,&size) && !reservations);
    assert(vfs_stat(&table,"C:\\SYSTEM\\DOS\\G2K1.COW",&entry)==-FS_ENOENT);
    assert(vfs_stat(&table,"C:\\SYSTEM\\DOS\\G2K0.COW",&entry)==-FS_ENOENT);
    assert(!vfs_stat(&table,"C:\\SYSTEM\\DOS\\G1K0.COW",&entry));
    /* Persist/remount before retry: the marker is not transient owner state. */
    vfs_table_destroy(&table);assert(!vfs_detach(&storage.vfs,2));vfs_destroy(&storage.vfs);
    cache_destroy(&storage.cache);assert(!cache_init(&storage.cache,128*1024));
    assert(!fat_mount(outer,&storage.cache,&disk.dev,0,disk.dev.capacity,false));
    assert(!fat_enable_write(outer));vfs_init(&storage.vfs);assert(!vfs_attach(&storage.vfs,2,outer));
    assert(!vfs_table_init(&storage.vfs,&table,2));
    assert(dosvm_game_program(2,false,&size)==1 && !size);
    assert(!dosvm_game_program(2,true,&size));
    assert(!vfs_delete(&table,"C:\\SYSTEM\\REMOVE2.PND"));
    assert(dosvm_game_program(2,false,&size)==-FS_ENOENT);
    assert(!dosvm_game_program(1,false,&size) && size);
    vfs_table_destroy(&table);assert(!vfs_detach(&storage.vfs,2));vfs_destroy(&storage.vfs);
    cache_destroy(&storage.cache);fake_close(&disk);assert(!fs_host_contig_live);
    puts("PASS owner provider: actual VFS seeks/read/write, write gate, prewrite reservation, FAT deletion, COW failure/retry, persistent pending identity, siblings");fflush(stdout);
}
