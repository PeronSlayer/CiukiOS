/* Production bridge + native FAT/VFS, fake guest memory/time/block transport.
 * No Python filesystem model, guest boot, QEMU or hardware claim.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "../../src/kernel/vm/dos_file_hook.h"
#include "../../src/kernel/vm/dos_file_volume.h"
#include <assert.h>
#include <stdio.h>
#include <inttypes.h>
static unsigned checks;
static unsigned allocation_calls,allocation_fail,allocations_live;
void *__real_malloc(size_t);
void *__real_calloc(size_t,size_t);
void *__real_aligned_alloc(size_t,size_t);
void __real_free(void *);
static bool allocation_fault(void) { return ++allocation_calls==allocation_fail; }
void *__wrap_malloc(size_t n) { if (allocation_fault()) return 0; void *p=__real_malloc(n); if (p) allocations_live++; return p; }
void *__wrap_calloc(size_t n,size_t m) { if (allocation_fault()) return 0; void *p=__real_calloc(n,m); if (p) allocations_live++; return p; }
void *__wrap_aligned_alloc(size_t align,size_t n) { if (allocation_fault()) return 0; void *p=__real_aligned_alloc(align,n); if (p) allocations_live++; return p; }
void __wrap_free(void *p) { if (p) { assert(allocations_live); allocations_live--; } __real_free(p); }
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"FAIL line %u: %s\n",__LINE__,#x); exit(1); } } while (0)
#define OK(x) CHECK((x)==0)
static struct dos_file bridge;
static struct vfs space;
static struct block_cache cache;
static struct fat_volume volume;
static struct vfs_table table,native_table;
static uint8_t disk[8192*512],guest[0x110000];
static uint64_t wall_time,monitor_time;
static unsigned reads,writes,barriers,fail_read,fail_write,callbacks,aborts,pins;
static uint64_t callback_token;
static uint32_t fail_lba;
static unsigned fixture_generation=48;
static bool cancel_request;
static bool stop_on_read;
static int block_read(struct blkdev *dev,uint64_t lba,uint32_t count,void *buf) {
    CHECK(!blkdev_range(dev,lba,count)); reads++; wall_time+=3; monitor_time++;
    if (fail_lba && lba<=fail_lba && count>fail_lba-lba) { fail_lba=0; return -FS_EIO; }
    if (fail_read && !--fail_read) return -FS_EIO;
    memcpy(buf,disk+lba*512,(size_t)count*512);
    if (stop_on_read) { stop_on_read=false; cancel_request=true; }
    return 0;
}
static int block_write(struct blkdev *dev,uint64_t lba,uint32_t count,const void *buf) {
    CHECK(!blkdev_range(dev,lba,count)); writes++; wall_time+=3; monitor_time++;
    if (fail_write && !--fail_write) return -FS_EIO;
    memcpy(disk+lba*512,buf,(size_t)count*512); return 0;
}
static int block_flush(struct blkdev *dev) { (void)dev; barriers++; return 0; }
static struct blkdev device={.read=block_read,.write=block_write,.flush=block_flush,
    .capacity=8192,.sector_size=512,.write_cache_state=BLKDEV_CACHE_DISABLED,.logical=true};
static bool memory_check(void *arg,uint32_t linear,size_t n,bool write) {
    (void)arg; pins++;
    if (write && linear>=0xf0000 && linear<0x100000) return false;
    return linear<sizeof(guest) && n<=sizeof(guest)-linear && !(linear<=0xf0000 && n>0xf0000-linear);
}
static void memory_read(void *arg,uint32_t linear,void *buf,size_t n) { (void)arg; CHECK(linear+n<=sizeof(guest)); memcpy(buf,guest+linear,n); }
static void memory_write(void *arg,uint32_t linear,const void *buf,size_t n) { (void)arg; CHECK(linear+n<=sizeof(guest)); memcpy(guest+linear,buf,n); }
static void memory_release(void *arg) { (void)arg; pins=0; }
static uint64_t wall(void *arg) { (void)arg; return wall_time; }
static uint64_t monitor(void *arg) { (void)arg; return monitor_time; }
static void pause_time(void *arg) { (void)arg; wall_time+=1000; }
static bool critical(void *arg,uint64_t token,const struct dos_file_frame *f,uint8_t flags,uint8_t cause) {
    (void)arg; CHECK(f->ax && (flags&0x18)==0x18 && cause<18); CHECK(!pins);
    CHECK(!space.active && !space.reserved); callback_token=token; callbacks++; return true;
}
static void abort_process(void *arg,uint16_t psp) { (void)arg; CHECK(psp==bridge.owning); aborts++; }
static bool fixture_stopped(void *arg) { (void)arg; return cancel_request; }
static struct dos_file_ops ops={memory_check,memory_read,memory_write,memory_release,wall,monitor,pause_time,critical,abort_process,fixture_stopped,0};
static void format(void) {
    memset(disk,0,sizeof(disk)); uint8_t *b=disk;
    b[0]=0xeb; b[1]=0x3c; b[2]=0x90; memcpy(b+3,"CIUKI49 ",8);
    fs_wr16(b+11,512); b[13]=1; fs_wr16(b+14,1); b[16]=2;
    fs_wr16(b+17,512); fs_wr16(b+19,8192); b[21]=0xf8; fs_wr16(b+22,32);
    fs_wr16(b+24,32); fs_wr16(b+26,4); b[38]=0x29; fs_wr32(b+39,0x490049);
    memcpy(b+43,"CIUKIDOS   ",11); memcpy(b+54,"FAT16   ",8); b[510]=0x55; b[511]=0xaa;
    for (unsigned i=0;i<2;i++) { fs_wr16(disk+(1+i*32)*512,0xfff8); fs_wr16(disk+(1+i*32)*512+2,0xffff); }
    memcpy(disk+65*512,"CIUKIDOS   ",11); disk[65*512+11]=FAT_ATTR_VOLUME;
}
static struct dos_file_far expanded(unsigned psp) { return (struct dos_file_far){0,(uint16_t)(psp+0x100)}; }
static void setup(void) {
    format(); memset(guest,0xa7,sizeof(guest));
    OK(cache_init(&cache,65536)); OK(fat_mount(&volume,&cache,&device,0,8192,true));
    CHECK(volume.mounted && !volume.readonly && volume.type==16);
    vfs_init(&space); OK(vfs_attach(&space,2,&volume));
    OK(vfs_table_init(&space,&table,2)); OK(vfs_table_init(&space,&native_table,2));
    dos_file_init(&bridge,1,++fixture_generation,&ops);
    struct dos_file_binding binding={{0,0x8000},{0,0x8100},{0,0x8200},{0,0x8300},0x30,128,
        {0x1050,0xf000},{0,0x8400},{0,0x8500},{0,0x8600},{0x10,0x8600}};
    fs_wr16(guest+0x24*4,0x1234); fs_wr16(guest+0x24*4+2,0x7000);
    OK(dos_file_bind(&bridge,&binding)); OK(dos_file_map(&bridge,2,&table,2,false));
    OK(dos_file_map(&bridge,3,&table,2,true));
    static const char *names[]={"CON","AUX","PRN","NUL"};
    for (unsigned i=0;i<4;i++) {
        uint32_t at=0x87000+0x20*i; memset(guest+at,0,18); fs_wr16(guest+at+4,0x8000);
        memcpy(guest+at+10,names[i],strlen(names[i]));
        OK(dos_file_register_device(&bridge,names[i],(struct dos_file_far){(uint16_t)(0x20*i),0x8700},i==3 ? 0x84 : 0x83));
    }
    OK(dos_file_register_dpb(&bridge,2,(struct dos_file_far){0,0x8800}));
    OK(dos_file_lifecycle(&bridge,DOS_FILE_PSP_CREATE,0x2000,0,expanded(0x2000)));
    CHECK(guest[0x20018]==0 && guest[0x2001c]==4 && guest[0x2001d]==255);
    CHECK(fs_rd16(guest+0x20032)==20 && fs_rd16(guest+0x82004)==128);
    CHECK(fs_rd16(guest+0x81004)==0 && fs_rd16(guest+0x81006)==0x8200);
    CHECK(fs_rd16(guest+0x82006+7)==0 && fs_rd16(guest+0x82006+9)==0x8700);
    CHECK(!memcmp(guest+0x82006+59*3+0x20,"AUX        ",11));
    CHECK(!memcmp(guest+0x82006+59*4+0x20,"PRN        ",11));
}
static void teardown(void) {
    dos_file_cancel(&bridge); CHECK(!pins);
    vfs_table_destroy(&table); vfs_table_destroy(&native_table); OK(vfs_detach(&space,2));
    for (unsigned i=0;i<VFS_DESCRIPTIONS;i++) CHECK(!space.descriptions[i].refs);
    for (unsigned i=0;i<VFS_NODES;i++) CHECK(!space.nodes[i].refs);
    for (unsigned i=0;i<VFS_RANGE_LOCKS;i++) CHECK(!space.range_locks[i].description);
    vfs_destroy(&space); cache_destroy(&cache); CHECK(!fs_host_contig_live);
}
static struct dos_file_frame frame(unsigned ax) {
    return (struct dos_file_frame){(uint16_t)ax,0xb1b1,0xc2c2,0xd3d3,0x5000,0x5000,0x6161,0x7171,0x8181,0x0ed4,0x1000,0x1234,0x6000,0x8000};
}
static struct dos_file_frame call(struct dos_file_frame f) {
    CHECK(dos_file_dispatch(&bridge,&f,DOS_FILE_V86,0x21)==DOS_FILE_DONE); CHECK(!pins); return f;
}
static void path_set(const char *s) { CHECK(strlen(s)<1024); strcpy((char *)guest+0x50100,s); }
static struct dos_file_frame path_call(unsigned ax,const char *s) {
    path_set(s); struct dos_file_frame f=frame(ax); f.dx=0x100; f.cx=0; return call(f);
}
static unsigned create_file(const char *s) {
    struct dos_file_frame f=path_call(0x3c00,s); CHECK(!(f.flags&1)); return f.ax;
}
static void close_file(unsigned h) { struct dos_file_frame f=frame(0x3e00); f.bx=(uint16_t)h; f=call(f); CHECK(!(f.flags&1)); }
static struct dos_file_frame seek_file(unsigned h,unsigned origin,int32_t position) {
    struct dos_file_frame f=frame(0x4200|origin); f.bx=(uint16_t)h; f.cx=(uint16_t)((uint32_t)position>>16); f.dx=(uint16_t)position; return call(f);
}
static struct dos_file_frame transfer_file(unsigned ax,unsigned h,unsigned count) {
    struct dos_file_frame f=frame(ax); f.bx=(uint16_t)h; f.cx=(uint16_t)count; f.dx=0x400; return call(f);
}
static void frame_tests(void) {
    unsigned served[]={0x0d,0x0e,0x19,0x1a,0x1b,0x1c,0x2f,0x36,0x39,0x3a,0x3b,0x3c,0x3d,0x3e,0x3f,0x40,0x41,0x42,0x43,0x44,0x45,0x46,0x47,0x4e,0x4f,0x56,0x57,0x59,0x5a,0x5b,0x5c,0x60,0x67,0x68,0x6c};
    for (unsigned i=0;i<sizeof(served)/sizeof(served[0]);i++) CHECK(dos_file_serves(0x21,(uint16_t)(served[i]<<8)));
    unsigned chained[]={0x2500,0x3500,0x2600,0x3100,0x4800,0x4900,0x4a00,0x4b00,0x4c00,0x4d00,0x3400,0x5000,0x5100,0x5200,0x5500,0x6200,0xf100,0x1800,0x4302,0x5702,0x5c02,0x6c01};
    uint8_t stack[32]; memcpy(stack,guest+0x68000,32);
    for (unsigned i=0;i<sizeof(chained)/sizeof(chained[0]);i++) {
        struct dos_file_frame f=frame(chained[i]),saved=f;
        CHECK(dos_file_dispatch(&bridge,&f,DOS_FILE_V86,0x21)==DOS_FILE_CHAIN); CHECK(!memcmp(&f,&saved,sizeof(f)));
        CHECK(!memcmp(stack,guest+0x68000,32));
    }
    for (unsigned origin=0;origin<=DOS_FILE_HOOK;origin++) {
        struct dos_file_frame f=frame(0x1900),saved=f;
        enum dos_file_result r=dos_file_dispatch(&bridge,&f,(enum dos_file_origin)origin,0x21);
        CHECK(r==DOS_FILE_DONE); saved.ax=0x1902;
        CHECK(!memcmp(&f,&saved,sizeof(f)));
    }
    struct dos_file_frame f=frame(0x3f00),saved=f; f.bx=0; saved=f;
    CHECK(dos_file_dispatch(&bridge,&f,DOS_FILE_V86,0x21)==DOS_FILE_CHAIN); CHECK(!memcmp(&f,&saved,sizeof(f)));
    f=frame(0x4402); f.bx=0; saved=f;
    CHECK(dos_file_dispatch(&bridge,&f,DOS_FILE_V86,0x21)==DOS_FILE_CHAIN); CHECK(!memcmp(&f,&saved,sizeof(f)));
    f=frame(0x1100); saved=f; CHECK(dos_file_dispatch(&bridge,&f,DOS_FILE_V86,0x2f)==DOS_FILE_CHAIN); CHECK(!memcmp(&f,&saved,sizeof(f)));
    puts("H49-01 frame/selection: PASS (direct origins; live monitor binding remains separate)");
}
static void state_tests(void) {
    struct dos_file_frame f=frame(0x6700); f.bx=232; f=call(f); CHECK(!(f.flags&1));
    CHECK(fs_rd16(guest+0x20032)==232 && fs_rd16(guest+0x20036)==0x2100);
    unsigned h=create_file("C:/STATE.DAT"); CHECK(h==5); unsigned sft=guest[0x21000+h];
    CHECK(fs_rd16(guest+0x82006+59*sft)==1);
    CHECK(fs_rd16(guest+0x82006+59*sft+7)==0 && fs_rd16(guest+0x82006+59*sft+9)==0x8800);
    f=frame(0x4500); f.bx=(uint16_t)h; f=call(f); CHECK(!(f.flags&1) && f.ax==6);
    CHECK(fs_rd16(guest+0x82006+59*sft)==2);
    close_file(6);
    uint8_t saved=guest[0x21000+h]; guest[0x21000+h]=127;
    f=transfer_file(0x4000,h,0); CHECK((f.flags&1) && f.ax==6); guest[0x21000+h]=saved;
    unsigned before=writes; path_set("C:/FORGED.DAT"); f=frame(0x3c00); f.dx=0xfffd; f.ds=0xffff;
    f=call(f); CHECK((f.flags&1) && f.ax==87 && before==writes);
    bridge.a20=false; f=frame(0x3c00); f.ds=0xffff; f.dx=0x20; f=call(f); CHECK(f.flags&1); bridge.a20=true;
    OK(dos_file_lifecycle(&bridge,DOS_FILE_EXEC_BEGIN,0x3000,0x2000,expanded(0x3000)));
    CHECK(bridge.selected==0x2000 && bridge.owning==0x2000);
    OK(dos_file_lifecycle(&bridge,DOS_FILE_EXEC_COMMIT,0x3000,0,expanded(0x3000)));
    OK(dos_file_lifecycle(&bridge,DOS_FILE_PSP_SELECT,0x2000,0,expanded(0x2000)));
    CHECK(bridge.owning==0x3000 && fs_rd16(guest+0x86000)==0x2000 && fs_rd16(guest+0x8001c)==0x3000);
    CHECK(fs_rd16(guest+0x82006+59*sft)==2);
    OK(dos_file_lifecycle(&bridge,DOS_FILE_EXEC_ROLLBACK,0x3000,0,expanded(0x3000)));
    CHECK(bridge.selected==0x2000 && fs_rd16(guest+0x82006+59*sft)==1);
    memcpy(guest+0x22000,guest+0x21000,232); fs_wr16(guest+0x20034,0); fs_wr16(guest+0x20036,0x2200);
    f=call(frame(0x1900)); CHECK(!(f.flags&1) && bridge.process[0].jft.seg==0x2200);
    close_file(h); f=frame(0x6700); f.bx=20; f=call(f); CHECK(!(f.flags&1));
    f=frame(0x6700); f.bx=233; f=call(f); CHECK((f.flags&1) && f.ax==4);
    f=frame(0x6700); f.bx=232; f=call(f); CHECK(!(f.flags&1));
    h=create_file("C:/CAPACITY.DAT"); memset(guest+0x50400,0x43,17);
    f=transfer_file(0x4000,h,17); CHECK(!(f.flags&1)); close_file(h);
    unsigned opened[64];
    for (unsigned i=0;i<64;i++) { f=path_call(0x3d42,"C:/CAPACITY.DAT"); CHECK(!(f.flags&1)); opened[i]=f.ax; }
    before=writes; f=path_call(0x3c00,"C:/CAPACITY.DAT"); CHECK((f.flags&1) && f.ax==4 && writes==before);
    struct fat_entry cap; OK(vfs_stat(&native_table,"C:/CAPACITY.DAT",&cap)); CHECK(cap.size==17);
    for (unsigned i=0;i<64;i++) close_file(opened[i]);
    h=create_file("C:/DUPCAP.DAT");
    for (unsigned i=0;i<226;i++) { f=frame(0x4500); f.bx=(uint16_t)h; f=call(f); CHECK(!(f.flags&1)); }
    f=frame(0x4500); f.bx=(uint16_t)h; f=call(f); CHECK((f.flags&1) && f.ax==4);
    for (unsigned i=5;i<232;i++) close_file(i);
    for (unsigned i=0;i<123;i++) { f=path_call(0x3d42,"NUL"); CHECK(!(f.flags&1) && f.ax==i+5); }
    before=writes; f=path_call(0x3d42,"NUL"); CHECK((f.flags&1) && f.ax==4 && before==writes);
    for (unsigned i=5;i<128;i++) close_file(i);
    f=frame(0x6700); f.bx=20; f=call(f); CHECK(!(f.flags&1));
    h=create_file("C:/LIFETIME.DAT"); sft=guest[0x20018+h];
    OK(dos_file_lifecycle(&bridge,DOS_FILE_EXEC_BEGIN,0x3000,0x2000,expanded(0x3000)));
    OK(dos_file_lifecycle(&bridge,DOS_FILE_EXEC_ROLLBACK,0x3000,0,expanded(0x3000)));
    CHECK(fs_rd16(guest+0x82006+59*sft)==1);
    OK(dos_file_lifecycle(&bridge,DOS_FILE_EXEC_BEGIN,0x3000,0x2000,expanded(0x3000)));
    OK(dos_file_lifecycle(&bridge,DOS_FILE_EXEC_COMMIT,0x3000,0,expanded(0x3000)));
    OK(dos_file_lifecycle(&bridge,DOS_FILE_TSR,0x3000,0,expanded(0x3000)));
    CHECK(bridge.selected==0x2000 && bridge.process[1].resident && fs_rd16(guest+0x82006+59*sft)==2);
    guest[0x30018+h]=127; /* Teardown uses trusted ownership despite a forged guest JFT. */
    OK(dos_file_lifecycle(&bridge,DOS_FILE_PROCESS_EXIT,0x3000,0,expanded(0x3000)));
    CHECK(fs_rd16(guest+0x82006+59*sft)==1); close_file(h);
    f=path_call(0x3dc2,"C:/LIFETIME.DAT"); CHECK(!(f.flags&1)); h=f.ax;
    OK(dos_file_lifecycle(&bridge,DOS_FILE_EXEC_BEGIN,0x3000,0x2000,expanded(0x3000)));
    CHECK(guest[0x30018+h]==255);
    OK(dos_file_lifecycle(&bridge,DOS_FILE_EXEC_ROLLBACK,0x3000,0,expanded(0x3000))); close_file(h);
    puts("H49-02 state: PASS checked expansion, repoint validation, dup, EXEC rollback, guards");
}
static void mutation_tests(void) {
    unsigned h=create_file("C:/MUTATE.DAT"); memset(guest+0x50400,0x6d,128);
    struct dos_file_frame f=transfer_file(0x4000,h,128); CHECK(!(f.flags&1) && f.ax==128);
    f=seek_file(h,0,0); CHECK(!(f.flags&1)); memset(guest+0x50400,0,129);
    f=transfer_file(0x3f00,h,129); CHECK(!(f.flags&1) && f.ax==128 && guest[0x50400]==0x6d);
    f=transfer_file(0x3f00,h,1); CHECK(!(f.flags&1) && f.ax==0);
    f=seek_file(h,0,64); CHECK(!(f.flags&1)); f=transfer_file(0x4000,h,0); CHECK(!(f.flags&1));
    struct fat_entry ent; OK(vfs_stat(&native_table,"C:/MUTATE.DAT",&ent)); CHECK(ent.size==64);
    f=seek_file(h,0,512); CHECK(!(f.flags&1)); f=transfer_file(0x4000,h,0); CHECK(!(f.flags&1));
    OK(vfs_stat(&native_table,"C:/MUTATE.DAT",&ent)); CHECK(ent.size==512);
    f=seek_file(h,1,-513); CHECK(!(f.flags&1) && f.ax==65535 && f.dx==65535);
    f=transfer_file(0x3f00,h,1); CHECK((f.flags&1) && f.ax==5);
    f=seek_file(h,0,0); CHECK(!(f.flags&1));
    f=frame(0x5701); f.bx=(uint16_t)h; f.cx=0x6421; f.dx=0x5d53; f=call(f); CHECK(!(f.flags&1)); close_file(h);
    f=path_call(0x3d40,"C:/MUTATE.DAT"); CHECK(!(f.flags&1)); h=f.ax;
    f=frame(0x5700); f.bx=(uint16_t)h; f=call(f); CHECK(!(f.flags&1) && f.cx==0x6421 && f.dx==0x5d53); close_file(h);
    f=path_call(0x5b00,"C:/MUTATE.DAT"); CHECK((f.flags&1) && f.ax==80);
    f=frame(0x5900); f.bx=0; f=call(f); CHECK(f.ax==80 && (f.bx>>8)==12);
    f=path_call(0x3900,"C:/DIR"); CHECK(!(f.flags&1));
    f=path_call(0x5600,"C:/MUTATE.DAT"); /* invalid output string must fail without rename */
    CHECK(f.flags&1);
    path_set("C:/MUTATE.DAT"); strcpy((char *)guest+0x50200,"C:/DIR/MOVED.DAT");
    f=frame(0x5600); f.dx=0x100; f.di=0x200; f=call(f); CHECK(!(f.flags&1));
    OK(vfs_stat(&native_table,"C:/DIR/MOVED.DAT",&ent));
    path_set("C:/EXTENDED.DAT"); f=frame(0x6c00); f.si=0x100; f.bx=0x42; f.cx=0; f.dx=0x11; f=call(f);
    CHECK(!(f.flags&1) && f.cx==2); close_file(f.ax);
    path_set("C:/EXTENDED.DAT"); f=frame(0x6c00); f.si=0x100; f.bx=0x42; f.cx=0; f.dx=0x11; f=call(f);
    CHECK(!(f.flags&1) && f.cx==1); close_file(f.ax);
    f=frame(0x6c00); f.si=0x100; f.bx=0x42; f.cx=0; f.dx=0x12; f=call(f); CHECK(!(f.flags&1) && f.cx==3); close_file(f.ax);
    f=path_call(0x3b00,"C:/DIR"); CHECK(!(f.flags&1));
    f=frame(0x4700); f.dx=0; f.si=0x300; f=call(f); CHECK(!(f.flags&1) && f.ax==0x0100 && !strcmp((char *)guest+0x50300,"DIR"));
    f=path_call(0x3b00,"C:/"); CHECK(!(f.flags&1));
    f=frame(0x6c00); path_set("C:/EXTENDED.DAT"); f.si=0x100; f.bx=0x42; f.cx=0; f.dx=0; f=call(f); CHECK((f.flags&1) && f.ax==80);
    path_set("C:/NEVER.DAT"); f=frame(0x6c00); f.si=0x100; f.bx=0x42; f.cx=0; f.dx=0; f=call(f); CHECK((f.flags&1) && f.ax==2);
    path_set("C:/UNIQUE.DAT"); f=frame(0x5b00); f.dx=0x100; f.cx=0; f=call(f); CHECK(!(f.flags&1)); close_file(f.ax);
    path_set("C:/DIR/"); f=frame(0x5a00); f.dx=0x100; f.cx=0; f=call(f); CHECK(!(f.flags&1));
    CHECK(!strncmp((char *)guest+0x50100,"C:\\DIR\\",7) && strlen((char *)guest+0x50100)==15); close_file(f.ax);
    f=frame(0x6000); path_set("C:/DIR/../UNIQUE.DAT"); f.si=0x100; f.di=0x300; f=call(f);
    CHECK(!(f.flags&1) && !strcmp((char *)guest+0x50300,"C:\\UNIQUE.DAT"));
    f=path_call(0x3900,"C:/REMOVE"); CHECK(!(f.flags&1)); f=path_call(0x3a00,"C:/REMOVE"); CHECK(!(f.flags&1));
    f=path_call(0x4100,"C:/UNIQUE.DAT"); CHECK(!(f.flags&1));
    puts("H49-06 mutation: PASS transfers, EOF, resize, seeks, timestamp, dispositions, rename/cwd");
}
static void sharing_tests(void) {
    static const char *matrix[15]={
        "YYYNNN1NNNNN1NN","YYYNNNNNNNNNNNN","YYYNNNNNNNNNNNN",
        "CCCNNNNNNNNNNNN","CCCNNNNNNNNNNNN","CCCNNNNNNNNNNNN",
        "2CCNNNYNNNNNYNN","CCCNNNNNNYNNYNN","CCCNNNNNNNNNYNN",
        "CCCNNNNYNNNNNYN","CCCNNNNNNNYNNYN","CCCNNNNNNNNNNYN",
        "2CCNNNYYYNNNYYY","CCCNNNNNNYYYYYY","CCCNNNNNNNNNYYY"};
    static const unsigned access[]={VFS_READ,VFS_WRITE,VFS_READ|VFS_WRITE};
    for (unsigned same=0;same<2;same++) for (unsigned ro=0;ro<2;ro++)
    for (unsigned old=0;old<15;old++) for (unsigned next=0;next<15;next++) {
        char cell=matrix[old][next]; int expected=cell=='Y' ? 0 : cell=='C' ? -VFS_ECRITICAL_SHARE : -VFS_ESHARE;
        if (cell=='1') expected=ro ? 0 : -VFS_ESHARE;
        if (cell=='2') expected=ro ? 0 : -VFS_ECRITICAL_SHARE;
        if (old<3 && next<3 && !same) expected=ro && old==0 && next==0 ? 0 : -VFS_ECRITICAL_SHARE;
        CHECK(vfs_dos_share(old/3,access[old%3],1,next/3,access[next%3],same ? 1 : 2,ro)!=999);
        int actual=vfs_dos_share(old/3,access[old%3],1,next/3,access[next%3],same ? 1 : 2,ro);
        if (actual!=expected) { fprintf(stderr,"matrix old=%u next=%u ro=%u same=%u actual=%d expected=%d\n",old,next,ro,same,actual,expected); exit(1); }
    }
    unsigned h=create_file("C:/LOCK.DAT");
    struct dos_file_frame f=transfer_file(0x4000,h,64); CHECK(!(f.flags&1));
    f=frame(0x5c00); f.bx=(uint16_t)h; f.cx=0; f.dx=16; f.si=0; f.di=32;
    f=call(f); CHECK(!(f.flags&1));
    /* A native VFS client sees the exact same node and mandatory lock. */
    int n=vfs_open(&native_table,"C:/LOCK.DAT",VFS_READ,VFS_DENY_NONE,0);
    CHECK(n<0); /* compatibility open denies a native client */
    f=frame(0x5c01); f.bx=(uint16_t)h; f.cx=0; f.dx=16; f.si=0; f.di=31; f=call(f); CHECK((f.flags&1) && f.ax==33);
    f=frame(0x5c01); f.bx=(uint16_t)h; f.cx=0; f.dx=16; f.si=0; f.di=32; f=call(f); CHECK(!(f.flags&1));
    f=frame(0x5c00); f.bx=(uint16_t)h; f.cx=65535; f.dx=65535; f.si=0; f.di=1; f=call(f); CHECK(f.flags&1);
    close_file(h);
    n=vfs_open(&native_table,"C:/LOCK.DAT",VFS_READ|VFS_WRITE,VFS_DENY_NONE,0); CHECK(n>=0);
    path_set("C:/LOCK.DAT"); f=frame(0x3d42); f.dx=0x100; f=call(f); CHECK(!(f.flags&1)); h=f.ax;
    struct vfs_description *nd=native_table.handles[n],*dd=table.handles[bridge.sft[bridge.process[0].handles[h]].handle]; CHECK(nd->node==dd->node);
    f=frame(0x5c00); f.bx=(uint16_t)h; f.cx=0; f.dx=0; f.si=0; f.di=16; f=call(f); CHECK(!(f.flags&1));
    size_t done; uint8_t data[16]; CHECK(vfs_read(&native_table,n,data,16,&done)==-VFS_ELOCK);
    CHECK(vfs_truncate(&native_table,n,0)==-VFS_ELOCK);
    CHECK(vfs_open(&native_table,"C:/LOCK.DAT",VFS_WRITE|VFS_TRUNCATE,VFS_DENY_NONE,0)==-VFS_ELOCK);
    f=frame(0x5c00); f.bx=(uint16_t)h; f.dx=16; f.cx=0; f.si=0; f.di=16; f=call(f); CHECK(!(f.flags&1));
    f=frame(0x4500); f.bx=(uint16_t)h; f=call(f); CHECK(!(f.flags&1)); unsigned duplicate=f.ax;
    close_file(duplicate); /* Any duplicate close clears this PSP's locks. */
    OK(vfs_read(&native_table,n,data,16,&done));
    for (unsigned i=0;i<VFS_RANGE_LOCKS;i++) {
        f=frame(0x5c00); f.bx=(uint16_t)h; f.cx=0; f.dx=(uint16_t)(1024+2*i); f.si=0; f.di=1; f=call(f); CHECK(!(f.flags&1));
    }
    f=frame(0x5c00); f.bx=(uint16_t)h; f.cx=0; f.dx=8000; f.si=0; f.di=0; f=call(f); CHECK((f.flags&1) && f.ax==36);
    f=frame(0x5c01); f.bx=(uint16_t)h; f.cx=0; f.dx=1024; f.si=0; f.di=1; f=call(f); CHECK(!(f.flags&1));
    f=frame(0x5c00); f.bx=(uint16_t)h; f.cx=0; f.dx=8000; f.si=0; f.di=0; f=call(f); CHECK(!(f.flags&1));
    f=frame(0x5c01); f.bx=(uint16_t)h; f.cx=0; f.dx=8000; f.si=0; f.di=0; f=call(f); CHECK(!(f.flags&1));
    close_file(h); OK(vfs_read(&native_table,n,data,16,&done)); OK(vfs_close(&native_table,n));
    puts("H49-04 sharing: PASS 15x15 x owner x readonly; asymmetric critical cells; common-node locks");
}
static void search_tests(void) {
    for (unsigned i=0;i<300;i++) { char p[64]; snprintf(p,sizeof(p),"C:/DIR/F%03u.DAT",i); int h=vfs_open(&native_table,p,VFS_READ|VFS_WRITE|VFS_CREATE,VFS_DENY_NONE,0); CHECK(h>=0); OK(vfs_close(&native_table,h)); }
    struct dos_file_frame f=frame(0x1a00); f.ds=0x5000; f.dx=0x800; f=call(f); CHECK(!(f.flags&1));
    memset(guest+0x507ff,0x4a,45); path_set("C:/DIR/*.*"); f=frame(0x4e00); f.cx=0; f.dx=0x100; f=call(f); CHECK(!(f.flags&1));
    CHECK(guest[0x507ff]==0x4a && guest[0x5082b]==0x4a); unsigned count=1;
    while (!(f.flags&1)) { f=call(frame(0x4f00)); if (!(f.flags&1)) count++; }
    CHECK(f.ax==18 && count==302 && guest[0x507ff]==0x4a && guest[0x5082b]==0x4a);
    for (unsigned i=0;i<DOS_FILE_SEARCHES;i++) CHECK(!bridge.cursor[i].used);
    path_set("C:/DIR/F00?.DAT"); f=frame(0x4e00); f.cx=0; f.dx=0x100; f=call(f); CHECK(!(f.flags&1));
    memcpy(guest+0x50900,guest+0x50800,43);
    f=frame(0x1a00); f.dx=0x900; f=call(f); CHECK(!(f.flags&1));
    f=call(frame(0x4f00)); CHECK(!(f.flags&1)); guest[0x50906]^=1;
    f=call(frame(0x4f00)); CHECK((f.flags&1) && f.ax==18);
    path_set("C:/DIR/ATTR"); f=frame(0x3900); f.dx=0x100; f=call(f); CHECK(!(f.flags&1));
    const char *special[]={"C:/DIR/HIDDEN.DAT","C:/DIR/SYSTEM.DAT"};
    for (unsigned i=0;i<2;i++) { unsigned h=create_file(special[i]); close_file(h); f=path_call(0x4301,special[i]);
        /* path_call uses CX=0; set the actual attributes with a separate call. */
        f=frame(0x4301); f.dx=0x100; f.cx=(uint16_t)(i ? FAT_ATTR_SYSTEM : FAT_ATTR_HIDDEN); f=call(f); CHECK(!(f.flags&1)); }
    for (unsigned i=0;i<2;i++) {
        path_set(special[i]); f=frame(0x4e00); f.dx=0x100; f.cx=0; f=call(f); CHECK((f.flags&1) && f.ax==18);
        f=frame(0x4e00); f.dx=0x100; f.cx=(uint16_t)(i ? FAT_ATTR_SYSTEM : FAT_ATTR_HIDDEN); f=call(f); CHECK(!(f.flags&1));
        CHECK(guest[0x50915]==(i ? FAT_ATTR_SYSTEM : FAT_ATTR_HIDDEN)); f=call(frame(0x4f00)); CHECK(f.flags&1);
    }
    path_set("C:/DIR/ATTR"); f=frame(0x4e00); f.dx=0x100; f.cx=0; f=call(f); CHECK((f.flags&1) && f.ax==18);
    f=frame(0x4e00); f.dx=0x100; f.cx=FAT_ATTR_DIR; f=call(f); CHECK(!(f.flags&1) && (guest[0x50915]&FAT_ATTR_DIR));
    f=call(frame(0x4f00)); CHECK(f.flags&1);
    path_set("C:/*.*"); f=frame(0x4e00); f.dx=0x100; f.cx=FAT_ATTR_VOLUME; f=call(f);
    CHECK(!(f.flags&1) && guest[0x50915]==FAT_ATTR_VOLUME && !strcmp((char *)guest+0x5091e,"CIUKIDOS"));
    f=call(frame(0x4f00)); CHECK((f.flags&1) && f.ax==18);
    f=path_call(0x4e00,"C:/MISSING/PATH/*.*"); CHECK((f.flags&1) && f.ax==3);
    OK(vfs_mkdir(&native_table,"C:/EMPTY",0)); f=path_call(0x4e00,"C:/EMPTY/*.*"); CHECK((f.flags&1) && f.ax==18);
    puts("H49-05 search: PASS 300 entries, masks, 43-byte guards, copied/tampered cursor");
}
static void device_tests(void) {
    struct dos_file_frame f=path_call(0x3d02,"C:/DIR/NUL.TXT"); CHECK(!(f.flags&1)); unsigned h=f.ax;
    f=frame(0x4400); f.bx=(uint16_t)h; f=call(f); CHECK(!(f.flags&1) && f.dx==0x84);
    f=transfer_file(0x4000,h,9); CHECK(!(f.flags&1) && f.ax==9);
    f=transfer_file(0x3f00,h,9); CHECK(!(f.flags&1) && f.ax==0); close_file(h);
    f=path_call(0x3d40,"C:/EXTENDED.DAT"); CHECK(!(f.flags&1)); h=f.ax;
    f=frame(0x4600); f.bx=(uint16_t)h; f.cx=0; f=call(f); CHECK(!(f.flags&1));
    f=frame(0x4400); f.bx=0; f=call(f); CHECK(!(f.flags&1) && f.dx==0x842);
    f=frame(0x4406); f.bx=0; f=call(f); CHECK(!(f.flags&1) && (f.ax&255)==0);
    f=frame(0x4402); f.bx=0; f=call(f); CHECK((f.flags&1) && f.ax==1);
    close_file(0); close_file(h);
    f=frame(0x4408); f.bx=3; f=call(f); CHECK(!(f.flags&1) && f.ax==1);
    f=path_call(0x3c00,"D:/DENIED.DAT"); CHECK((f.flags&1) && f.ax==5);
    f=frame(0x0e00); f.dx=3; f=call(f); CHECK(!(f.flags&1) && (f.ax&255)==4);
    f=call(frame(0x1900)); CHECK((f.ax&255)==3);
    f=frame(0x0e00); f.dx=2; f=call(f); CHECK(!(f.flags&1));
    f=call(frame(0x1b00)); CHECK(!(f.flags&1) && (f.ax&255)==1 && f.cx==512 && guest[((unsigned)f.ds<<4)+f.bx]==0xf8);
    f=frame(0x1c00); f.dx=3; f=call(f); CHECK(!(f.flags&1) && (f.ax&255)==1);
    f=frame(0x1c00); f.dx=26; f=call(f); CHECK(!(f.flags&1) && (f.ax&255)==255);
    f=frame(0x3600); f.dx=3; f=call(f); CHECK(!(f.flags&1) && f.ax==1 && f.cx==512 && f.bx<f.dx);
    f=frame(0x2f00); f=call(f); CHECK(f.es==0x5000 && f.bx==0x900);
    f=frame(0x440b); f.cx=7; f.dx=0; f=call(f); CHECK(!(f.flags&1) && bridge.retry_pause==7 && bridge.retry_count==3);
    f=frame(0x440b); f.cx=1; f.dx=3; f=call(f); CHECK(!(f.flags&1));
    f=frame(0x0d00); f=call(f); CHECK(!(f.flags&1));
    f=path_call(0x3d40,"C:/EXTENDED.DAT"); CHECK(!(f.flags&1)); h=f.ax;
    f=frame(0x6800); f.bx=(uint16_t)h; f=call(f); CHECK(!(f.flags&1)); close_file(h);
    puts("H49-07 devices: PASS NUL, redirected handle zero, unsupported IOCTL, read-only view");
}
static void error_tests(void) {
    static const struct {int fs;unsigned code;} cases[]={
        {FS_EOPNOTSUPP,1},{FS_EINVAL,87},{FS_ENAMETOOLONG,87},{FS_EILSEQ,87},{FS_ELOOP,87},
        {FS_ENOENT,2},{FS_ENOTDIR,3},{FS_EMFILE,4},{FS_EBADF,6},{FS_ENOMEM,8},{FS_EXDEV,17},
        {FS_EROFS,19},{FS_EIO,30},{FS_EUCLEAN,30},{FS_EQUARANTINED,30},{FS_EAGAIN,21},
        {FS_EBUSY,32},{VFS_ESHARE,32},{VFS_ECRITICAL_SHARE,32},{VFS_ELOCK,33},{VFS_ELOCKFULL,36},
        {FS_ENOSPC,39},{FS_EFBIG,39},{FS_EEXIST,80},{FS_EACCES,5},{FS_EISDIR,5},{FS_ENOTEMPTY,5}};
    for (unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++) { struct dos_file_error e; dos_file_error_from_fs(&e,-cases[i].fs,false); CHECK(e.code==cases[i].code && e.class_ && e.action && e.locus); }
    path_set("C:/EXTENDED.DAT"); struct dos_file_frame f=frame(0x3d40); f.dx=0x100;
    fail_read=1; CHECK(dos_file_dispatch(&bridge,&f,DOS_FILE_V86,0x21)==DOS_FILE_PENDING); CHECK(callbacks && callback_token);
    CHECK(bridge.callback.cs==0x7000 && bridge.callback.ip==0x1234 && bridge.callback.sp==0x7fe2);
    CHECK(fs_rd16(guest+0x67fe2)==0x1050 && fs_rd16(guest+0x67fe4)==0xf000);
    CHECK(fs_rd16(guest+0x67fe8)==0x3d40 && fs_rd16(guest+0x67ffe)==0x0ed4);
    struct dos_file_frame query=frame(0x5900); query.bx=0; query=call(query); CHECK(query.ax==30);
    query=frame(0x3c00); query=call(query); CHECK(query.ax==5 && (query.flags&1) && bridge.critical);
    CHECK(guest[0x80000]==1 && guest[0x80001]==0 && guest[0x83000]==0);
    CHECK(dos_file_complete(&bridge,callback_token,1,&f)==DOS_FILE_DONE); CHECK(!(f.flags&1)); close_file(f.ax);
    path_set("C:/EXTENDED.DAT"); f=frame(0x3d40); f.dx=0x100; fail_read=1;
    CHECK(dos_file_dispatch(&bridge,&f,DOS_FILE_V86,0x21)==DOS_FILE_PENDING);
    CHECK(dos_file_complete(&bridge,callback_token,3,&f)==DOS_FILE_DONE && f.ax==83 && (f.flags&1));
    query=frame(0x5900); query.bx=0; query=call(query); CHECK(query.ax==30);
    /* A nonreturning handler consumes the original budget and yields a
     * completion without invoking a second callback or extending its time. */
    path_set("C:/EXTENDED.DAT"); f=frame(0x3d40); f.dx=0x100; fail_read=1;
    CHECK(dos_file_dispatch(&bridge,&f,DOS_FILE_V86,0x21)==DOS_FILE_PENDING);
    wall_time=bridge.deadline; CHECK(dos_file_poll(&bridge,&f) && f.ax==83 && !bridge.critical);
    OK(dos_file_lifecycle(&bridge,DOS_FILE_EXEC_BEGIN,0x3000,0x2000,expanded(0x3000)));
    OK(dos_file_lifecycle(&bridge,DOS_FILE_EXEC_COMMIT,0x3000,0,expanded(0x3000)));
    OK(dos_file_lifecycle(&bridge,DOS_FILE_PSP_SELECT,0x2000,0,expanded(0x2000)));
    path_set("C:/EXTENDED.DAT"); f=frame(0x3d40); f.dx=0x100; fail_read=1;
    CHECK(dos_file_dispatch(&bridge,&f,DOS_FILE_V86,0x21)==DOS_FILE_PENDING);
    CHECK(dos_file_complete(&bridge,callback_token,2,&f)==DOS_FILE_PENDING && aborts==1);
    CHECK(bridge.process[0].used && !bridge.process[1].used && bridge.owning==0x2000 && bridge.selected==0x2000);
    OK(dos_file_lifecycle(&bridge,DOS_FILE_PROCESS_EXIT,0x3000,0,expanded(0x3000)));
    f=path_call(0x3d40,"C:/CAPACITY.DAT"); CHECK(!(f.flags&1)); unsigned handle=f.ax;
    memset(guest+0x50400,0xcc,16); stop_on_read=true;
    f=frame(0x3f00); f.bx=(uint16_t)handle; f.cx=16; f.dx=0x400;
    CHECK(dos_file_dispatch(&bridge,&f,DOS_FILE_V86,0x21)==DOS_FILE_PENDING);
    for (unsigned i=0;i<16;i++) CHECK(guest[0x50400+i]==0xcc);
    CHECK(!space.active && !space.reserved && !pins);
    cancel_request=false; close_file(handle);
    puts("H49-08 errors/critical: PASS mapping, nested 59h, one-shot retry, fail/cause preservation");
}
static bool request_cancel(void *arg) { (void)arg; return cancel_request; }
static void wait_tests(void) {
    struct vfs_wait_info evidence={0};
    struct vfs_request r={0,wall_time+2000000,wall,request_cancel,pause_time,0x49,&evidence};
    fs_lock_take(&space.lock); uint64_t begin=wall_time;
    CHECK(vfs_admit(&space,1u<<2,&r)==-VFS_ETIMEOUT); CHECK(wall_time-begin==2000000);
    CHECK(evidence.valid && evidence.owner==0x49 && evidence.elapsed_us==2000000 && evidence.reason==VFS_WAIT_NAMESPACE && evidence.outcome==-VFS_ETIMEOUT);
    fs_lock_drop(&space.lock);
    r.deadline=wall_time+2000000; cancel_request=true;
    CHECK(vfs_admit(&space,1u<<2,&r)==-VFS_ECANCELLED); cancel_request=false;
    r.deadline=wall_time+2000000; OK(vfs_admit(&space,1u<<2,&r)); vfs_unlock(&space);
    CHECK(space.next[2]==space.serving[2] && !space.waiters);
    /* The predecessor owns a real production volume reservation during I/O. */
    vfs_lock(&space,1u<<2); struct vfs_io_state io=vfs_io_begin_locked(&space);
    r.deadline=wall_time+2000000; begin=wall_time;
    CHECK(vfs_admit(&space,1u<<2,&r)==-VFS_ETIMEOUT && wall_time-begin==2000000);
    CHECK(evidence.reason==VFS_WAIT_VOLUME && evidence.elapsed_us==2000000 && evidence.drives==(1u<<2));
    vfs_io_end_locked(&space,io); vfs_unlock(&space);
    r.deadline=wall_time+2000000; OK(vfs_admit(&space,1u<<2,&r)); vfs_unlock(&space);
    CHECK(space.next[2]==space.serving[2] && !space.waiters);
    puts("H49-08 admission: PASS exact fake deadline, cancellation, successor and no ticket holes");
}
static void trace_tests(void) {
    bridge.trace_enabled=true; struct dos_file_frame f=frame(0x1900); f=call(f);
    struct dos_file_trace enter,ret; CHECK(dos_file_trace_pop(&bridge,&enter)); CHECK(dos_file_trace_pop(&bridge,&ret));
    CHECK(!enter.returning && ret.returning && enter.sequence==ret.sequence && ret.frame.ax==0x1902);
    f=frame(0x3000); CHECK(dos_file_dispatch(&bridge,&f,DOS_FILE_V86,0x21)==DOS_FILE_CHAIN);
    wall_time+=500; monitor_time+=10; f.ax=0x0500; dos_file_observe_return(&bridge,&f);
    CHECK(dos_file_trace_pop(&bridge,&enter) && dos_file_trace_pop(&bridge,&ret));
    CHECK(ret.returning && ret.wall_us==500 && ret.monitor_us==10 && ret.sequence==enter.sequence);
    f=frame(0x1900);
    CHECK(dos_file_dispatch(&bridge,&f,DOS_FILE_DPMI_0300,0x21)==DOS_FILE_DONE);
    CHECK(bridge.trace_count==1); wall_time+=77; monitor_time+=3;
    dos_file_observe_return(&bridge,&f);
    CHECK(dos_file_trace_pop(&bridge,&enter) && dos_file_trace_pop(&bridge,&ret));
    CHECK(ret.returning && ret.wall_us==77 && ret.monitor_us==3 && ret.sequence==enter.sequence);
    for (unsigned i=0;i<100;i++) (void)call(frame(0x1900));
    CHECK(bridge.trace_count==DOS_FILE_TRACE && bridge.dropped==72);
    while (dos_file_trace_pop(&bridge,&ret)) { }
    printf("group=dos_api vm=%u gen=%u seq=%" PRIu64 " phase=return psp=%04x origin=%u vector=21 ax=%04x bx=%04x cx=%04x dx=%04x ds=%04x es=%04x si=%04x di=%04x cf=%u bytes=%u monitor_us=%" PRIu64 " wall_us=%" PRIu64 " dropped=%u\n",
        bridge.id,ret.generation,ret.sequence,ret.psp,ret.origin,ret.frame.ax,ret.frame.bx,ret.frame.cx,ret.frame.dx,ret.frame.ds,ret.frame.es,ret.frame.si,ret.frame.di,ret.frame.flags&1,ret.bytes,ret.monitor_us,ret.wall_us,bridge.dropped);
    puts("H49-09 trace: PASS paired/chained return, monitor/wall distinction, bounded drop accounting");
}
static void provider_tests(void) {
    static struct dos_file_volume private;
    struct cache_stats before,after;
    OK(cache_init(&cache,65536)); cache_stats(&cache,&before);
    OK(dos_file_volume_open(&private,&cache,&device,true));
    int h=vfs_open(&private.table,"C:/PROVIDER.DAT",VFS_READ|VFS_WRITE|VFS_CREATE,VFS_DENY_NONE,0); CHECK(h>=0);
    uint8_t bytes[512]; memset(bytes,0x37,sizeof(bytes)); size_t n; uint64_t position;
    OK(vfs_write(&private.table,h,bytes,sizeof(bytes),&n)); CHECK(n==512);
    struct fat_entry entry; OK(vfs_handle_stat(&private.table,h,&entry,0));
    uint32_t sector=private.fat.data_sector+(entry.first-2)*private.fat.spc;
    bytes[0]=0x48; OK(dos_file_volume_raw(&private,sector,bytes,true,0));
    for (unsigned i=0;i<10000;i++) {
        OK(vfs_seek(&private.table,h,0,VFS_SEEK_SET,&position));
        OK(vfs_read(&private.table,h,bytes,512,&n)); CHECK(n==512 && bytes[0]==0x48 && bytes[511]==0x37);
    }
    /* Raw and file writes share one mount/provider and observe each other. */
    OK(vfs_seek(&private.table,h,0,VFS_SEEK_SET,&position)); bytes[1]=0x59;
    OK(vfs_write(&private.table,h,bytes,512,&n));
    OK(dos_file_volume_raw(&private,sector,bytes,false,0)); CHECK(bytes[1]==0x59);
    int moved=vfs_open(&private.table,"C:/RELOCATE.DAT",VFS_READ|VFS_WRITE|VFS_CREATE,VFS_DENY_NONE,0); CHECK(moved>=0);
    memset(bytes,0xa1,sizeof(bytes)); OK(vfs_write(&private.table,moved,bytes,sizeof(bytes),&n));
    struct fat_entry relocation; OK(vfs_handle_stat(&private.table,moved,&relocation,0)); OK(vfs_close(&private.table,moved));
    uint32_t owning=private.fat.root_sector+entry.index/16,other=private.fat.root_sector+relocation.index/16;
    OK(dos_file_volume_raw(&private,owning,bytes,false,0)); fs_wr16(bytes+(entry.index%16)*32+26,(uint16_t)relocation.first);
    if (owning==other) fs_wr16(bytes+(relocation.index%16)*32+26,(uint16_t)entry.first);
    OK(dos_file_volume_raw(&private,owning,bytes,true,0));
    if (owning!=other) { OK(dos_file_volume_raw(&private,other,bytes,false,0)); fs_wr16(bytes+(relocation.index%16)*32+26,(uint16_t)entry.first); OK(dos_file_volume_raw(&private,other,bytes,true,0)); }
    OK(vfs_seek(&private.table,h,0,VFS_SEEK_SET,&position)); OK(vfs_read(&private.table,h,bytes,512,&n)); CHECK(n==512 && bytes[0]==0xa1);
    OK(dos_file_volume_raw(&private,owning,bytes,false,0)); bytes[(entry.index%16)*32]='X';
    uint32_t generation=private.space.generation[2]; OK(dos_file_volume_raw(&private,owning,bytes,true,0));
    CHECK(private.space.generation[2]!=generation && vfs_read(&private.table,h,bytes,1,&n)==-FS_EIO);
    OK(vfs_close(&private.table,h)); OK(dos_file_volume_close(&private));
    cache_stats(&cache,&after);
    CHECK(before.data_pages==after.data_pages && before.meta_pages==after.meta_pages && before.dirty_bytes==after.dirty_bytes);
    for (unsigned i=0;i<CACHE_DEVICES;i++) CHECK(!cache.devices[i].dev);
    printf("H49 ledger cache_data_pages=%u->%u cache_meta_pages=%u->%u dirty_bytes=%u->%u\n",
        before.data_pages,after.data_pages,before.meta_pages,after.meta_pages,before.dirty_bytes,after.dirty_bytes);
    cache_destroy(&cache); CHECK(!fs_host_contig_live);
    puts("H49-03 provider: PASS one native FAT mount, raw/file interleave, 10000 reads, no duplicate cache pages");
}
static bool alias_reverse;
static unsigned alias_enumerations;
static uint32_t alias_generation(struct fs_ro_volume *v) { (void)v; return 1; }
static void alias_unmount(struct fs_ro_volume *v) { (void)v; }
static int alias_readdir(struct fs_ro_volume *v,uint64_t directory,struct fs_ro_cursor *cursor,struct fs_ro_node *out) {
    (void)v; CHECK(directory==1); unsigned index=(unsigned)cursor->opaque[0]; alias_enumerations++;
    if (index==302) return -FS_ENOENT;
    cursor->opaque[0]++; if (alias_reverse) index=301-index;
    memset(out,0,sizeof(*out)); out->id=index+2; out->parent=1; out->attr=1;
    if (!index) strcpy(out->name,"COLLIS~1.TXT");
    else if (index==1) { strcpy(out->name,"Preserved supplied alias.bin"); strcpy(out->alias,"KEEP~9.BIN"); }
    else snprintf(out->name,sizeof(out->name),"Collision long name %03u.txt",index-2);
    return 0;
}
static const struct fs_ro_ops alias_ops={.unmount=alias_unmount,.readdir=alias_readdir,.generation=alias_generation};
static void alias_tests(void) {
    OK(cache_init(&cache,65536));
    for (unsigned reverse=0;reverse<2;reverse++) {
        alias_reverse=reverse; struct fs_ro_volume ro={.ops=&alias_ops,.cache=&cache,.dev=&device,.mounted=true,.mount_generation=1};
        struct fs_ro_node node;
        OK(fs_ro_lookup(&ro,1,"COLLIS~2.TXT",&node)); CHECK(!strcmp(node.name,"Collision long name 000.txt"));
        OK(fs_ro_lookup(&ro,1,"COLLIS~1.TXT",&node)); CHECK(!strcmp(node.name,"COLLIS~1.TXT"));
        OK(fs_ro_lookup(&ro,1,"KEEP~9.BIN",&node)); CHECK(!strcmp(node.alias,"KEEP~9.BIN"));
        unsigned calls=alias_enumerations; uint32_t cursor=0,count=0;
        while (!fs_ro_next(&ro,1,&cursor,&node)) count++;
        CHECK(count==302 && alias_enumerations==calls && ro.view_count==1);
        for (unsigned i=0;i<300;i++) {
            char name[64]; snprintf(name,sizeof(name),"Collision long name %03u.txt",i);
            OK(fs_ro_lookup(&ro,1,name,&node)); CHECK(node.id==i+4 && node.alias[0]);
        }
        fs_ro_unmount(&ro); CHECK(!ro.views && !ro.view_count);
    }
    cache_destroy(&cache);
    puts("H49-05 aliases: PASS 302 read-only names, reserved/supplied aliases, reversed source order, one map per generation");
}
static void failure_and_cycle_tests(void) {
    unsigned injected=0;
    for (unsigned nth=1;nth<256;nth++) {
        format(); memset(&cache,0,sizeof(cache)); static struct dos_file_volume private;
        allocation_calls=0; allocation_fail=nth;
        int e=cache_init(&cache,65536);
        if (!e) {
            e=dos_file_volume_open(&private,&cache,&device,true);
            if (!e) OK(dos_file_volume_close(&private));
            cache_destroy(&cache);
        }
        unsigned calls=allocation_calls; allocation_fail=0;
        CHECK(!allocations_live && !fs_host_contig_live);
        if (calls<nth) break;
        injected++;
    }
    CHECK(injected>10);
    for (unsigned cycle=0;cycle<100;cycle++) {
        setup(); unsigned h=create_file("C:/CYCLE.DAT");
        struct dos_file_frame f=transfer_file(0x4000,h,16); CHECK(!(f.flags&1));
        OK(dos_file_lifecycle(&bridge,DOS_FILE_EXEC_BEGIN,0x3000,0x2000,expanded(0x3000)));
        OK(dos_file_lifecycle(&bridge,DOS_FILE_EXEC_COMMIT,0x3000,0,expanded(0x3000)));
        f=seek_file(h,0,0); CHECK(!(f.flags&1));
        OK(dos_file_lifecycle(&bridge,DOS_FILE_PROCESS_EXIT,0x3000,0,expanded(0x3000)));
        close_file(h);
        struct dos_file_frame before=frame(0x3d40),after=before;
        teardown();
        CHECK(dos_file_complete(&bridge,((uint64_t)cycle<<32)|bridge.generation,1,&after)==DOS_FILE_PENDING);
        CHECK(!memcmp(&before,&after,sizeof(before)) && !allocations_live);
    }
    printf("H49-09 lifetime: PASS cycles=100 allocation_failures=%u live_allocations=%u stale_completion=no_guest_write\n",injected,allocations_live);
}
#include "dos_file_h50_test.inc"
int main(void) {
    setup(); frame_tests(); state_tests(); mutation_tests(); sharing_tests(); search_tests(); device_tests(); error_tests(); wait_tests(); trace_tests(); CHECK(aborts==1); teardown();
    setup(); h50_nls(); h50_fcbs(); h50_internal(); teardown();
    setup(); h50_hook(); teardown();
    setup(); h50_partial_retry(); teardown();
    provider_tests(); alias_tests(); failure_and_cycle_tests();
    printf("PASS dos_file_api checks=%u reads=%u writes=%u flushes=%u bridge_bytes=%zu\n",checks,reads,writes,barriers,sizeof(bridge)); return 0;
}
