/* G29b file-backed fixed partition conversion/recovery qualification.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "fat.h"
#include "fake.h"
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"CONVERT %d: %s\n",__LINE__,#x); abort(); } } while (0)
#define OK(x) do { int e_=(x); if(e_) fprintf(stderr,"convert error %d at %d: %s\n",e_,__LINE__,#x); CHECK(!e_); } while(0)
#define START 2048u
#define SECTORS 66640u
static struct fake disk;
static struct fat_volume v;
static struct block_cache cache;
static uint8_t payload[4101], readback[4101];
static bool logical;
static void mount_volume(bool write) {
    OK(cache_init(&cache,2*1024*1024));
    OK(fat_mount(&v,&cache,&disk.dev,START,SECTORS,write));
    if (write) CHECK(!v.readonly);
}
static void close_volume(void) { OK(fat_unmount(&v)); cache_destroy(&cache); }
static void seed(void) {
    uint8_t mbr[512]={0}; mbr[450]=0x0e; fs_wr32(mbr+454,START); fs_wr32(mbr+458,SECTORS);
    fs_wr16(mbr+510,0xaa55); OK(fake_raw_write(&disk,0,mbr));
    mount_volume(true); CHECK(v.type==16 && v.spc==2);
    struct fat_entry dir,sub,file; size_t done;
    OK(fat_create(&v,0,"Long directory name",FAT_ATTR_DIR,&dir));
    OK(fat_create(&v,dir.first,"Child",FAT_ATTR_DIR,&sub));
    OK(fat_create(&v,sub.first,"Long file name.bin",FAT_ATTR_ARCHIVE,&file));
    OK(fat_write(&v,&file,0,payload,sizeof(payload),&done)); CHECK(done==sizeof(payload));
    OK(fat_create(&v,0,"EMPTY",0,&file));
    v.next_free=v.clusters-8; /* maps above 65535 after splitting: exercise DIR_FstClusHI */
    OK(fat_create(&v,0,"Tail.bin",0,&file));
    OK(fat_write(&v,&file,0,payload,1001,&done)); CHECK(done==1001);
    close_volume(); disk.events=disk.writes=disk.flushes=0; disk.undo_enabled=true;
}
static void verify(void) {
    mount_volume(false);
    CHECK(v.type==16 || v.type==32);
    CHECK(!(v.ro_reasons&~FAT_RO_REQUEST));
    struct fat_entry dir,sub,file,parent; size_t done; uint8_t *b=readback;
    OK(fat_lookup(&v,0,"Long directory name",&dir));
    OK(fat_lookup(&v,dir.first,"Child",&sub));
    OK(fat_lookup(&v,sub.first,"..",&parent)); CHECK(parent.first==dir.first);
    OK(fat_lookup(&v,sub.first,"Long file name.bin",&file)); CHECK(file.size==sizeof(payload));
    OK(fat_read(&v,&file,0,b,sizeof(readback),&done)); CHECK(done==sizeof(readback) && !memcmp(b,payload,sizeof(readback)));
    OK(fat_lookup(&v,0,"Tail.bin",&file)); CHECK(file.size==1001);
    if (v.type==32) CHECK(file.first>65535);
    OK(fat_read(&v,&file,0,b,sizeof(readback),&done)); CHECK(done==1001 && !memcmp(b,payload,done));
    OK(fat_lookup(&v,0,"EMPTY",&file)); CHECK(!file.first && !file.size);
    if (v.type==32) {
        CHECK(v.spc==1 && v.root>=2 && v.fsinfo==1 && v.backup==6);
        uint8_t boot[512],backup[512],info[512],mbr[512];
        OK(fake_raw_read(&disk,START,boot)); OK(fake_raw_read(&disk,START+6,backup)); CHECK(!memcmp(boot,backup,512));
        OK(fake_raw_read(&disk,START+1,info)); CHECK(fs_rd32(info+488)==v.free_clusters);
        OK(fake_raw_read(&disk,logical ? 1024 : 0,mbr)); CHECK(mbr[450]==(logical ? 0x0b : 0x0c) && fs_rd32(mbr+454)==(logical ? START-1024 : START) && fs_rd32(mbr+458)==SECTORS);
    }
    close_volume();
}
int main(int argc,char **argv) {
    CHECK(argc==2); for (unsigned i=0;i<sizeof(payload);i++) payload[i]=(uint8_t)(i*37+(i>>8));
    OK(fake_open(&disk,argv[1])); seed();
    mount_volume(false); uint32_t bytes;
    OK(fat_convert_plan(&v,&bytes)); CHECK(bytes==512);
    struct fat_volume geometry=v; geometry.sectors=66600; CHECK(fat_convert_plan(&geometry,&bytes)==-FS_EINVAL);
    geometry.sectors=532480; geometry.spc=16; OK(fat_convert_plan(&geometry,&bytes)); CHECK(bytes==512);
    geometry.sectors=532481; geometry.spc=16; OK(fat_convert_plan(&geometry,&bytes)); CHECK(bytes==4096);
    geometry.type=12; CHECK(fat_convert_plan(&geometry,&bytes)==-FS_EINVAL);
    OK(fat_convert(&v,&disk.dev,START)); unsigned events=disk.events;
    unsigned locator_cut=0;
    for (unsigned i=0;i+1<events;i++) if (disk.trace[i].kind=='W' && disk.trace[i].lba==START) { locator_cut=i+2; break; }
    CHECK(locator_cut);
    cache_destroy(&cache); fake_power_loss(&disk); verify(); fake_reset(&disk);
    printf("converter baseline: %u sector-write/flush events\n",events); fflush(stdout);
    /* Every write/flush with synchronous sectors; restart with a fresh cache.
     * Additional stage cuts exercise a volatile/reordering device below. */
    unsigned trials=0;
    for (unsigned cut=1;cut<=events;cut++) {
        mount_volume(false); disk.cut_at=disk.events+cut;
        (void)fat_convert(&v,&disk.dev,START);
        cache_destroy(&cache); fake_power_loss(&disk);
        OK(fat_convert_recover(&disk.dev,START,SECTORS)); verify();
        fake_reset(&disk); trials++;
        if (!(cut%500)) { printf("converter cuts: %u/%u\n",cut,events); fflush(stdout); }
    }
    for (unsigned mode=0;mode<3;mode++) {
        mount_volume(false); disk.reorder=true; disk.cut_mode=mode;
        OK(fat_convert(&v,&disk.dev,START)); unsigned cached_events=disk.events;
        cache_destroy(&cache); fake_power_loss(&disk); fake_reset(&disk);
        for (unsigned cut=1;cut<=cached_events;cut+=97) {
            mount_volume(false); disk.cut_at=disk.events+cut;
            (void)fat_convert(&v,&disk.dev,START); cache_destroy(&cache); fake_power_loss(&disk);
            OK(fat_convert_recover(&disk.dev,START,SECTORS)); verify(); fake_reset(&disk); trials++;
        }
    }
    disk.reorder=false;
    mount_volume(false); disk.cut_at=disk.events+locator_cut;
    (void)fat_convert(&v,&disk.dev,START); cache_destroy(&cache); fake_power_loss(&disk);
    uint8_t locator[512],saved[512],damaged[512];
    OK(fake_raw_read(&disk,START,locator)); CHECK(!memcmp(locator+3,"CIUKICVT",8));
    uint64_t record=START+fs_rd32(locator+100)+1;
    OK(fake_raw_read(&disk,record,saved)); memcpy(damaged,saved,512); damaged[25]^=1;
    OK(fake_raw_write(&disk,record,damaged)); unsigned before=disk.writes;
    CHECK(fat_convert_recover(&disk.dev,START,SECTORS)==-FS_EIO && disk.writes==before);
    OK(fake_raw_write(&disk,record,saved)); OK(fat_convert_recover(&disk.dev,START,SECTORS)); verify(); fake_reset(&disk);
    /* Extended/logical partition: update the EBR entry, preserve the MBR. */
    uint8_t mbr[512]={0},ebr[512]={0}; mbr[450]=0x0f; fs_wr32(mbr+454,1024); fs_wr32(mbr+458,SECTORS+1024); fs_wr16(mbr+510,0xaa55);
    ebr[450]=0x06; fs_wr32(ebr+454,1024); fs_wr32(ebr+458,SECTORS); fs_wr16(ebr+510,0xaa55);
    OK(fake_raw_write(&disk,0,mbr)); OK(fake_raw_write(&disk,1024,ebr)); logical=true;
    mount_volume(false); OK(fat_convert(&v,&disk.dev,START)); cache_destroy(&cache); verify();
    OK(fake_raw_read(&disk,0,ebr)); CHECK(!memcmp(mbr,ebr,512)); fake_reset(&disk); logical=false;
    /* Finish one conversion for fsck.fat/mtools, outside the production code. */
    mount_volume(false); OK(fat_convert(&v,&disk.dev,START)); cache_destroy(&cache); verify();
    printf("PASS FAT16->FAT32: cluster table, root/subdirectories/LFN/empty/slack, primary+backup boot/FSInfo, 0Ch MBR, %u restart trials\n",trials);
    fake_close(&disk); return 0;
}
