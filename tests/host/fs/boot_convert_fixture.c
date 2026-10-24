/* Production converter fixture for assembled loader tests; no boot image. */
#include "fat.h"
#include "fake.h"
#include <stdio.h>
#define START 2048u
static uint32_t sectors;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"BOOT CONVERT %d: %s\n",__LINE__,#x); abort(); } } while (0)
#define OK(x) do { int e_=(x); if (e_) fprintf(stderr,"boot error %d: %s\n",e_,#x); CHECK(!e_); } while(0)
static struct fake disk;
static struct block_cache cache;
static struct fat_volume v;
static uint8_t payload[65539];
static void mount_volume(bool write) {
    OK(cache_init(&cache,2*1024*1024)); OK(fat_mount(&v,&cache,&disk.dev,START,sectors,write));
}
static void install(const char *mbr,const char *loader) {
    uint8_t b[512]; FILE *f=fopen(mbr,"rb"); CHECK(f && fread(b,512,1,f)==1); fclose(f);
    b[446]=0x80; b[450]=0x0e; fs_wr32(b+454,START); fs_wr32(b+458,sectors);
    OK(fake_raw_write(&disk,0,b)); f=fopen(loader,"rb"); CHECK(f);
    for (unsigned i=1;i<=48;i++) { CHECK(fread(b,512,1,f)==1); OK(fake_raw_write(&disk,i,b)); }
    fclose(f);
}
int main(int argc,char **argv) {
    CHECK(argc==6); sectors=(uint32_t)strtoul(argv[5],0,10); OK(fake_open(&disk,argv[1]));
    for (unsigned i=0;i<sizeof(payload);i++) payload[i]=(uint8_t)(i*29+(i>>8));
    if (!strcmp(argv[4],"seed")) {
        install(argv[2],argv[3]); mount_volume(true);
        struct fat_entry dir,file; size_t done;
        OK(fat_create(&v,0,"SYSTEM",FAT_ATTR_DIR,&dir));
        OK(fat_create(&v,dir.first,"VMM.ELF",0,&file));
        OK(fat_write(&v,&file,0,payload,sizeof(payload),&done)); CHECK(done==sizeof(payload));
        OK(fat_convert_boot_schedule(&v,&disk.dev,START));
        bool pending=false; OK(fat_convert_boot_pending(&disk.dev,START,sectors,&pending)); CHECK(pending);
        OK(fat_unmount(&v)); cache_destroy(&cache);
    } else if (!strcmp(argv[4],"commit")) {
        disk.undo_enabled=true; mount_volume(false); OK(fat_convert_boot(&v,&disk.dev,START));
        unsigned cut=0;
        for (unsigned i=0;i<disk.events;i++) if (disk.trace[i].kind=='W' && disk.trace[i].lba==START) { cut=i+2; break; }
        CHECK(cut); cache_destroy(&cache); fake_power_loss(&disk); fake_reset(&disk);
        mount_volume(false); disk.cut_at=disk.events+cut;
        (void)fat_convert_boot(&v,&disk.dev,START); cache_destroy(&cache); fake_power_loss(&disk);
        uint8_t b[512]; OK(fake_raw_read(&disk,START,b)); CHECK(!memcmp(b+3,"CIUKICVT",8));
    } else if (!strcmp(argv[4],"recover")) {
        OK(fat_convert_recover(&disk.dev,START,sectors)); mount_volume(false); CHECK(v.type==32);
        struct fat_entry dir,file; size_t done; uint8_t b[1024];
        OK(fat_lookup(&v,0,"SYSTEM",&dir)); OK(fat_lookup(&v,dir.first,"VMM.ELF",&file));
        for (unsigned off=0;off<sizeof(payload);) {
            unsigned bytes=sizeof(payload)-off; if (bytes>sizeof(b)) bytes=sizeof(b);
            OK(fat_read(&v,&file,off,b,bytes,&done)); CHECK(done==bytes && !memcmp(b,payload+off,bytes)); off+=bytes;
        }
        OK(fat_unmount(&v)); cache_destroy(&cache);
    } else CHECK(false);
    fake_close(&disk); puts("PASS boot conversion fixture"); return 0;
}
