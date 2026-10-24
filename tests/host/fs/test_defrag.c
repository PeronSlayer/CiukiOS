/* G29b power-cut oracle: real production FAT plus file-backed volatile disk.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "fat.h"
#include "fake.h"
#include "scan.h"
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"DEFRAG %d: %s\n",__LINE__,#x); abort(); } } while (0)
#define OK(x) CHECK((x)==0)
static struct fat_volume v;
static struct block_cache cache;
static struct fake disk;
static unsigned char payload[1536];
static void mount_volume(void) {
    OK(cache_init(&cache,2*1024*1024));
    OK(fat_mount(&v,&cache,&disk.dev,0,disk.dev.capacity,true));
    CHECK(!v.readonly);
}
static void abandon(void) { cache_destroy(&cache); fake_power_loss(&disk); }
static void patch(unsigned c,unsigned n) {
    unsigned off=v.type==12 ? c+c/2 : c*(v.type/8);
    for (unsigned f=0;f<2;f++) {
        uint8_t b[1024]; uint64_t s=v.reserved+f*v.fat_sectors+off/512;
        OK(fake_raw_read(&disk,s,b)); OK(fake_raw_read(&disk,s+1,b+512));
        unsigned p=off%512;
        if (v.type==12) { unsigned x=fs_rd16(b+p); x=(c&1) ? (x&15)|(n<<4) : (x&0xf000)|n; fs_wr16(b+p,x); }
        else if (v.type==16) fs_wr16(b+p,n); else fs_wr32(b+p,n);
        OK(fake_raw_write(&disk,s,b)); OK(fake_raw_write(&disk,s+1,b+512));
    }
}
static void seed(void) {
    mount_volume(); CHECK(v.spc==1);
    struct fat_entry e,dir; size_t done;
    OK(fat_create(&v,0,"Folder",FAT_ATTR_DIR,&dir));
    OK(fat_create(&v,dir.first,"Fragment.bin",0,&e));
    OK(fat_write(&v,&e,0,payload,sizeof(payload),&done)); CHECK(done==sizeof(payload));
    unsigned chain[3],c=e.first;
    for (unsigned i=0;i<3;i++) { chain[i]=c; OK(fat_get_cluster(&v,c,&c)); }
    OK(fat_unmount(&v)); cache_destroy(&cache);
    for (unsigned i=0;i<3;i++) patch(chain[i],0);
    unsigned end=v.type==12 ? 0xfff : v.type==16 ? 0xffff : 0xfffffff;
    for (unsigned i=0;i<3;i++) {
        /* FAT12's second entry deliberately straddles a sector boundary. */
        unsigned dst=339+i*2;
        OK(fake_raw_write(&disk,v.data_sector+dst-2,payload+i*512));
        patch(dst,i==2 ? end : dst+2);
    }
    uint8_t b[512]; uint64_t root=v.data_sector+(e.parent-2)*v.spc;
    root+=e.index/16;
    OK(fake_raw_read(&disk,root,b)); fs_wr16(b+(e.index%16)*32+26,339);
    fs_wr16(b+(e.index%16)*32+20,0); OK(fake_raw_write(&disk,root,b));
    disk.events=disk.writes=disk.flushes=0; disk.undo_enabled=true;
}
static void lookup(struct fat_entry *file) {
    struct fat_entry dir; OK(fat_lookup(&v,0,"Folder",&dir));
    OK(fat_lookup(&v,dir.first,"Fragment.bin",file));
}
static void verify(bool contiguous) {
    struct fat_entry e; uint8_t b[1536]; size_t done;
    lookup(&e); CHECK(e.size==sizeof(payload));
    OK(fat_read(&v,&e,0,b,sizeof(b),&done)); CHECK(done==sizeof(b) && !memcmp(b,payload,sizeof(b)));
    struct fat_defrag_info info; OK(fat_defrag_analyze(&v,&info));
    CHECK(info.files==1 && info.clusters==v.clusters);
    CHECK(info.fragmented==(contiguous ? 0u : 1u));
    uint8_t bits=0; for (unsigned i=0;i<FAT_DEFRAG_CELLS;i++) bits|=info.map[i];
    CHECK((bits&(FAT_MAP_FREE|FAT_MAP_USED))==(FAT_MAP_FREE|FAT_MAP_USED));
}
int main(int argc,char **argv) {
    CHECK(argc==4);
    for (unsigned i=0;i<sizeof(payload);i++) payload[i]=(uint8_t)(i*31+(i>>8));
    for (int image=1;image<argc;image++) {
        OK(fake_open(&disk,argv[image])); seed();
        unsigned trials=0;
        bool moved;
        for (unsigned mode=0;mode<4;mode++) {
            disk.reorder=mode!=3; disk.cut_mode=mode;
            mount_volume(); verify(false); unsigned before=disk.events;
            OK(fat_defrag_step(&v,0,v.mutation_sequence,&moved)); CHECK(moved); verify(true);
            unsigned events=disk.events-before;
            CHECK(fat_defrag_step(&v,0,v.mutation_sequence+1,&moved)==-FS_EAGAIN);
            CHECK(fat_defrag_step(&v,1,v.mutation_sequence,&moved)==1);
            OK(fat_unmount(&v)); abandon(); fake_reset(&disk);
            for (unsigned cut=1;cut<=events;cut++) {
                mount_volume(); disk.cut_at=disk.events+cut;
                (void)fat_defrag_step(&v,0,v.mutation_sequence,&moved);
                abandon();
                mount_volume();
                struct fat_entry e; uint8_t b[1536]; size_t done;
                lookup(&e); OK(fat_read(&v,&e,0,b,sizeof(b),&done));
                CHECK(done==sizeof(b) && !memcmp(b,payload,sizeof(b)));
                CHECK(!(v.ro_reasons&(FAT_RO_CORRUPT|FAT_RO_COPIES|FAT_RO_LOST)));
                OK(fat_unmount(&v));
                struct scan_result scan=independent_scan(disk.fd,0);
                CHECK(!scan.crosslinks && !scan.corrupt && !scan.lost && !scan.divergent);
                abandon(); fake_reset(&disk); trials++;
            }
        }
        disk.reorder=false;
        OK(cache_init(&cache,2*1024*1024)); OK(fat_mount(&v,&cache,&disk.dev,0,disk.dev.capacity,false));
        unsigned writes=disk.writes; CHECK(fat_defrag_step(&v,0,v.mutation_sequence,&moved)==-FS_EROFS && disk.writes==writes);
        OK(fat_unmount(&v)); abandon(); fake_reset(&disk);
        if (v.type==12) {
            mount_volume();
            for (unsigned c=2;c<v.clusters+2;c++) { unsigned n; OK(fat_get_cluster(&v,c,&n)); if (!n) patch(c,0xff7); }
            abandon(); mount_volume();
            struct fat_defrag_info info; OK(fat_defrag_analyze(&v,&info)); CHECK(!info.free_clusters && info.fragmented==1);
            writes=disk.writes; OK(fat_defrag_step(&v,0,v.mutation_sequence,&moved)); CHECK(!moved && disk.writes==writes);
            OK(fat_unmount(&v)); abandon(); fake_reset(&disk);
        }
        if (v.type!=12) {
            mount_volume(); struct fat_entry file; lookup(&file);
            OK(fat_unmount(&v)); cache_destroy(&cache);
            uint8_t b[512]; uint64_t sector=v.data_sector+(file.parent-2)*v.spc+file.index/16;
            OK(fake_raw_read(&disk,sector,b)); unsigned slot=file.index%16; CHECK(slot<15);
            memcpy(b+(slot+1)*32,b+slot*32,32); memcpy(b+(slot+1)*32,"OTHER   BIN",11);
            OK(fake_raw_write(&disk,sector,b));
            mount_volume(); CHECK(v.scan_fast);
            unsigned before=disk.writes;
            CHECK(fat_defrag_step(&v,0,v.mutation_sequence,&moved)<0);
            CHECK(disk.writes==before && (v.ro_reasons&FAT_RO_CORRUPT));
            abandon(); fake_reset(&disk);
        }
        printf("PASS FAT%u defrag: analysis/map, COW move, stale sequence, end; %u power cuts, all file bytes intact and independent check clean\n",v.type,trials);
        fake_close(&disk);
    }
    return 0;
}
