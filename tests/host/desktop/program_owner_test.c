/* Actual owner provider over nested fixture FAT/VFS volumes. */
#define CIUKI_CPU_H
#include <stdint.h>
static uint32_t irq_save(void){return 0;}
static void irq_restore(uint32_t flags){(void)flags;}
#include "../../../src/kernel/vm/dosvm_programs.c"
#include "../fs/fake.h"
#include <assert.h>
#include <stdio.h>
static struct storage storage;
static struct fake disk;
static unsigned reservations,invalidations;
static int reserve_error;
struct storage *storage_get(void){return &storage;}
int dosvm_game_remove_reserve(unsigned game){assert(game==2);if(reserve_error)return reserve_error;reservations++;return 0;}
void dosvm_game_remove_release(unsigned game){assert(game==2 && reservations);reservations--;}
void dosvm_game_digest_invalidate(void){invalidations++;}
int main(int argc,char **argv)
{
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
