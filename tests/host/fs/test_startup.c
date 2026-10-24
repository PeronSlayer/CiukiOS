/* G29D: actual cluster placement and every publication boundary, real FAT. */
#include "fat.h"
#include "fake.h"
#include "scan.h"
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"STARTUP %d: %s\n",__LINE__,#x); abort(); } } while (0)
#define OK(x) CHECK((x)==0)
static struct fake disk;
static struct block_cache cache;
static struct fat_volume v;
static uint8_t payload[4096], readback[4096];
static void mount_volume(void) {
    OK(cache_init(&cache,2*1024*1024));
    OK(fat_mount(&v,&cache,&disk.dev,0,disk.dev.capacity,true)); CHECK(!v.readonly);
}
static void close_volume(void) { OK(fat_unmount(&v)); cache_destroy(&cache); }
static struct fat_entry lookup(const char *name) {
    struct fat_entry f; OK(fat_lookup(&v,0,name,&f)); return f;
}
static void verify(void) {
    struct fat_entry f=lookup("PROGRAM.EXE"); uint8_t *b=readback; size_t done;
    OK(fat_read(&v,&f,0,b,sizeof(readback),&done)); CHECK(done==sizeof(readback) && !memcmp(b,payload,sizeof(readback)));
    f=lookup("LIBRARY.DLL");
    OK(fat_read(&v,&f,0,b,sizeof(readback),&done)); CHECK(done==sizeof(readback) && !memcmp(b,payload,sizeof(readback)));
}
int main(int argc,char **argv) {
    CHECK(argc==4);
    for (unsigned i=0;i<sizeof(payload);i++) payload[i]=(uint8_t)(i*29+(i>>8));
    for (int image=1;image<argc;image++) {
        OK(fake_open(&disk,argv[image])); mount_volume(); CHECK(v.spc==1);
        struct fat_entry program,library; size_t done;
        OK(fat_create(&v,0,"PROGRAM.EXE",0,&program));
        OK(fat_write(&v,&program,0,payload,sizeof(payload),&done));
        OK(fat_create(&v,0,"LIBRARY.DLL",0,&library));
        OK(fat_write(&v,&library,0,payload,sizeof(payload),&done));
        const uint32_t order[]={3,0,6};
        OK(fat_startup_record(&v,&program,order,3));
        OK(fat_startup_record(&v,&library,order,3));
        OK(fat_startup_record(&v,&program,order,3)); /* more frequent beats recency */
        CHECK(fat_startup_record(&v,&program,(uint32_t[]){8},1)==-FS_EINVAL);
        close_volume(); disk.undo_enabled=true; disk.events=disk.writes=disk.flushes=0;
        unsigned trials=0;
        for (unsigned mode=0;mode<4;mode++) {
            disk.reorder=mode!=3; disk.cut_mode=mode;
            mount_volume(); program=lookup("PROGRAM.EXE"); library=lookup("LIBRARY.DLL");
            uint32_t old_program=program.first, old_library=library.first, placement=2; bool moved;
            unsigned before=disk.events;
            OK(fat_defrag_startup_step(&v,0,v.mutation_sequence,&placement,&moved)); CHECK(moved);
            unsigned events=disk.events-before;
            program=lookup("PROGRAM.EXE"); library=lookup("LIBRARY.DLL");
            CHECK(program.first!=old_program && library.first==old_library);
            uint32_t chain[8],c=program.first;
            for (unsigned i=0;i<8;i++) { chain[i]=c; OK(fat_get_cluster(&v,c,&c)); }
            uint32_t first=chain[3];
            CHECK(chain[0]==first+1 && chain[6]==first+2 && chain[1]==first+3 && chain[2]==first+4);
            CHECK(placement==first+8); verify();
            uint64_t seq=v.mutation_sequence;
            CHECK(fat_defrag_startup_step(&v,1,seq+1,&placement,&moved)==-FS_EAGAIN);
            OK(fat_defrag_startup_step(&v,1,seq,&placement,&moved)); CHECK(moved); verify();
            OK(fat_defrag_startup_step(&v,2,v.mutation_sequence,&placement,&moved)); CHECK(!moved);
            CHECK(fat_defrag_startup_step(&v,3,v.mutation_sequence,&placement,&moved)==1);
            close_volume(); fake_power_loss(&disk); fake_reset(&disk);
            for (unsigned cut=1;cut<=events;cut++) {
                mount_volume(); placement=2; disk.cut_at=disk.events+cut;
                (void)fat_defrag_startup_step(&v,0,v.mutation_sequence,&placement,&moved);
                cache_destroy(&cache); fake_power_loss(&disk);
                mount_volume(); verify();
                CHECK(!(v.ro_reasons&(FAT_RO_CORRUPT|FAT_RO_COPIES|FAT_RO_LOST)));
                close_volume();
                struct scan_result scan=independent_scan(disk.fd,0);
                CHECK(!scan.crosslinks && !scan.corrupt && !scan.lost && !scan.divergent);
                fake_power_loss(&disk); fake_reset(&disk); trials++;
            }
        }
        mount_volume(); program=lookup("PROGRAM.EXE");
        /* Stale directory identities must not cause recorded-file placement. */
        OK(fat_set_metadata(&v,&program,FAT_ATTR_ARCHIVE,&(struct fat_times){.write_date=0x5921}));
        uint32_t placement=2; bool moved; unsigned writes=disk.writes;
        OK(fat_defrag_startup_step(&v,1,v.mutation_sequence,&placement,&moved));
        CHECK(!moved && disk.writes==writes); /* unprofiled contiguous program */
        /* Corrupt persistent hint is discarded, not treated as a disk map. */
        struct fat_entry log=lookup("CIUKIST.LOG"); uint8_t byte=0;
        OK(fat_write(&v,&log,0,&byte,1,&done)); writes=disk.writes;
        OK(fat_defrag_startup_step(&v,0,v.mutation_sequence,&placement,&moved));
        CHECK(!moved && disk.writes==writes); verify(); close_volume();
        fake_close(&disk);
        printf("PASS FAT%u startup patterns: persistent uses/order, physical permutation, logical bytes, stale/corrupt hints; %u power cuts\n",v.type,trials);
    }
    return 0;
}
