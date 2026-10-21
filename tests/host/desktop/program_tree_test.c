/* Independent FAT fixture; production tree walker and FAT mutation code. */
#include "../../../src/kernel/fs/program_tree.h"
#include "../fs/fake.h"
#include <assert.h>
#include <stdio.h>
static struct block_cache cache;
static struct fat_volume volume;
static struct fake disk;
int main(int argc,char **argv)
{
    assert(argc==2);assert(!fake_open(&disk,argv[1]));assert(!cache_init(&cache,8192));
    assert(!fat_mount(&volume,&cache,&disk.dev,0,disk.dev.capacity,false));
    uint32_t bytes=0;assert(!program_tree(&volume,2,false,&bytes) && bytes>=2048);
    assert(program_tree(&volume,2,true,&bytes)==-FS_EROFS && !disk.writes);
    assert(program_tree(&volume,0,false,&bytes)==-FS_EINVAL);
    fs_host_contig_fail=1;assert(program_tree(&volume,2,false,&bytes)==-FS_ENOMEM);
    assert(!fat_enable_write(&volume));assert(!program_tree(&volume,2,true,&bytes));
    assert(program_tree(&volume,2,false,&bytes)==-FS_ENOENT);
    assert(!program_tree(&volume,1,false,&bytes) && bytes);
    assert(!program_tree(&volume,3,false,&bytes) && bytes);
    assert(!fat_unmount(&volume));cache_destroy(&cache);fake_close(&disk);
    assert(!fs_host_contig_live);
    puts("PASS bundled-game FAT removal: read-only and allocation refusal, nested files/LFN, removal, sibling preservation, durable unmount");fflush(stdout);
}
