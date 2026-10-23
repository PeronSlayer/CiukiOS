/* Native FAT12 through production mount/cache/VFS, using the storage harness
 * only for kernel service stubs. SPDX-License-Identifier: GPL-2.0-only */
#define main storage_fixture_main
#include "test_storage.c"
#undef main

static struct storage *fat12_start(bool writable) {
    struct storage *s=calloc(1,sizeof(*s)); CHECK(s);
    OK(storage_setup(s,0)); OK(storage_add_removable(s,&media[0].dev));
    struct storage_volume *v=storage_volume(s,4); CHECK(v && v->fat.type==12);
    CHECK(v->fat.root==0 && v->fat.scan_deferred && v->fat.write_through);
    CHECK(!media[0].writes && !media[0].flushes);
    OK(vfs_table_init(&s->vfs,&table,4));
    if (writable) { OK(storage_enable_write(s,4)); CHECK(!v->fat.readonly); }
    return s;
}
static void fat12_stop(struct storage *s) {
    vfs_table_destroy(&table);
    for (unsigned d=0;d<26;d++) if (s->vfs.volumes[d]) OK(vfs_detach(&s->vfs,d));
    storage_destroy(s); free(s);
}
static int fat12_create(const char *path) {
    int h=vfs_open(&table,path,VFS_READ|VFS_WRITE|VFS_CREATE|VFS_EXCLUSIVE,VFS_DENY_NONE,0);
    CHECK(h>=0); return h;
}
static uint8_t payload_byte(unsigned i) { return (uint8_t)(i*29u+(i>>8)); }
static void seed_read(const char *path, size_t expected) {
    int h=vfs_open(&table,path,VFS_READ,VFS_DENY_NONE,0); CHECK(h>=0);
    uint8_t b[4096]; size_t total=0,n;
    do {
        OK(vfs_read(&table,h,b,sizeof(b),&n));
        for (size_t i=0;i<n;i++) CHECK(b[i]==payload_byte((unsigned)(total+i)));
        total+=n;
    } while (n);
    CHECK(total==expected); OK(vfs_close(&table,h));
}
static void operations(void) {
    struct storage *s=fat12_start(true); seed_read("E:/SEED.BIN",4099);
    uint8_t b[1500],out[1500]; for (unsigned i=0;i<sizeof(b);i++) b[i]=payload_byte(i);
    int h=fat12_create("E:/NEW.BIN"); size_t n; uint64_t pos;
    OK(vfs_write(&table,h,b,sizeof(b),&n)); CHECK(n==sizeof(b)); OK(vfs_commit(&table,h));
    OK(vfs_seek(&table,h,0,VFS_SEEK_SET,&pos));
    OK(vfs_read(&table,h,out,sizeof(out),&n)); CHECK(n==sizeof(out) && !memcmp(b,out,n));
    OK(vfs_close(&table,h)); OK(vfs_rename(&table,"E:/NEW.BIN","E:/RENAMED.BIN"));
    h=fat12_create("E:/DELETE.BIN"); OK(vfs_close(&table,h)); OK(vfs_delete(&table,"E:/DELETE.BIN"));
    OK(vfs_mkdir(&table,"E:/GROW",0));
    unsigned entries=s->volumes[4].fat.spc*16+3;
    for (unsigned i=0;i<entries;i++) {
        char path[40]; snprintf(path,sizeof(path),"E:/GROW/F%03u.TXT",i);
        h=fat12_create(path); OK(vfs_close(&table,h));
    }
    struct fat_entry dir; OK(vfs_stat(&table,"E:/GROW",&dir));
    uint32_t next; OK(fat_get_cluster(&s->volumes[4].fat,dir.first,&next)); CHECK(next<0xff8);
    OK(fat_commit(&s->volumes[4].fat)); fat12_stop(s);
    printf("PASS FAT12 read/write/rename/delete/subdirectory growth\n");
}
static void root_full(void) {
    struct storage *s=fat12_start(true); struct fat_volume *v=&s->volumes[4].fat;
    uint32_t free=v->free_clusters; unsigned created=0;
    for (unsigned i=0;i<=v->root_entries;i++) {
        char path[40]; snprintf(path,sizeof(path),"E:/R%03u.TXT",i);
        int h=vfs_open(&table,path,VFS_WRITE|VFS_CREATE|VFS_EXCLUSIVE,VFS_DENY_NONE,0);
        if (h==-FS_ENOSPC) break;
        CHECK(h>=0); created++; OK(vfs_close(&table,h));
    }
    CHECK(created>16 && created<=v->root_entries && v->free_clusters==free && !v->readonly);
    OK(vfs_delete(&table,"E:/R000.TXT"));
    int h=fat12_create("E:/REUSED.TXT"); OK(vfs_close(&table,h));
    OK(fat_commit(&s->volumes[4].fat)); fat12_stop(s);
    printf("PASS FAT12 fixed root fills/reuses slots, ENOSPC without root chain\n");
}
static void disk_full(void) {
    struct storage *s=fat12_start(true); int h=fat12_create("E:/FULL.BIN");
    uint8_t b[1024]; memset(b,0x5a,sizeof(b)); size_t n; unsigned calls=0; int e;
    do { e=vfs_write(&table,h,b,sizeof(b),&n); CHECK(e || n==sizeof(b)); calls++; }
    while (!e && calls<6000);
    CHECK(e==-FS_ENOSPC && calls<6000 && !s->volumes[4].fat.readonly);
    CHECK(s->volumes[4].fat.free_clusters<16);
    uint64_t pos; OK(vfs_seek(&table,h,0,VFS_SEEK_SET,&pos));
    OK(vfs_write(&table,h,b,1,&n)); CHECK(n==1); OK(vfs_commit(&table,h));
    OK(vfs_close(&table,h)); OK(vfs_delete(&table,"E:/FULL.BIN"));
    fat12_stop(s); printf("PASS FAT12 full disk ENOSPC and in-place overwrite/delete\n");
}
/* Independent packed encoder: use raw sectors, not the driver's decoder. */
static void raw_link(uint32_t c, uint32_t value) {
    uint8_t boot[512],b[1024]; OK(fake_raw_read(&media[0],0,boot));
    uint32_t off=c+c/2, sec=off/512, at=off%512;
    unsigned nf=boot[16], fat=fs_rd16(boot+22), reserved=fs_rd16(boot+14);
    for (unsigned f=0;f<nf;f++) {
        uint64_t lba=reserved+f*fat+sec;
        OK(fake_raw_read(&media[0],lba,b));
        if (at==511) OK(fake_raw_read(&media[0],lba+1,b+512));
        uint16_t x=fs_rd16(b+at);
        fs_wr16(b+at,(uint16_t)((c&1) ? (x&15)|(value<<4) : (x&0xf000)|value));
        OK(fake_raw_write(&media[0],lba,b));
        if (at==511) OK(fake_raw_write(&media[0],lba+1,b+512));
    }
}
static void corruption(void) {
    media[0].undo_enabled=true;
    struct storage *s=fat12_start(false); struct fat_entry ent;
    OK(vfs_stat(&table,"E:/SEED.BIN",&ent)); fat12_stop(s);
    const uint32_t links[]={ent.first,0,0xff7,0xff0,0xefe};
    for (unsigned i=0;i<sizeof(links)/sizeof(*links);i++) {
        fake_reset(&media[0]); raw_link(ent.first,links[i]);
        s=fat12_start(false); unsigned writes=media[0].writes;
        int e=storage_enable_write(s,4); CHECK(e && s->volumes[4].fat.readonly);
        CHECK(media[0].writes==writes); fat12_stop(s);
    }
    fake_reset(&media[0]);
    /* Header refusal, independently in BPB and in each reserved FAT copy. */
    for (unsigned i=0;i<5;i++) {
        uint8_t b[512]; OK(fake_raw_read(&media[0],0,b));
        uint32_t sec=0;
        if (i==0) b[21]=0xf1;
        else if (i==1) b[13]=3;
        else if (i==2) fs_wr16(b+22,0xffff);
        else {
            sec=fs_rd16(b+14)+(i-3)*fs_rd16(b+22);
            OK(fake_raw_read(&media[0],sec,b)); b[0]^=1;
        }
        OK(fake_raw_write(&media[0],sec,b));
        s=calloc(1,sizeof(*s)); CHECK(s); OK(storage_setup(s,0));
        CHECK(storage_add_removable(s,&media[0].dev)==-FS_EINVAL);
        CHECK(!vfs_drive_present(&s->vfs,4) && !media[0].writes && !media[0].flushes);
        storage_destroy(s); free(s); fake_reset(&media[0]);
    }
    printf("PASS FAT12 cycles/free/bad/reserved/out-of-range chains and BPB/media refusals\n");
}
static void readonly_case(bool dmf) {
    if (!dmf) media[0].dev.write=0;
    struct storage *s=fat12_start(false); struct fat_volume *v=&s->volumes[4].fat;
    CHECK(v->readonly && (v->ro_reasons&(dmf ? FAT_RO_DMF : FAT_RO_WRITE_PROTECTED)));
    CHECK(!strcmp(fat_readonly_reason(v),dmf ? "dmf_read_only" : "write_protected"));
    CHECK(storage_enable_write(s,4)==-FS_EROFS);
    seed_read("E:/SEED.BIN",4099);
    CHECK(vfs_open(&table,"E:/NO.TXT",VFS_WRITE|VFS_CREATE,VFS_DENY_NONE,0)==-FS_EROFS);
    fat12_stop(s); CHECK(!media[0].writes && !media[0].flushes);
    printf("PASS FAT12 %s zero writes/flushes, refused writable upgrade\n",dmf ? "DMF read-only" : "write protection");
}
static unsigned protected_calls;
static int protected_write(struct blkdev *dev,uint64_t lba,uint32_t count,const void *buf) {
    (void)dev; (void)lba; (void)count; (void)buf;
    protected_calls++; return -FS_EROFS;
}
static void late_protection(bool absent) {
    struct storage *s=fat12_start(true); int h=fat12_create("E:/LATE.BIN");
    unsigned writes=media[0].writes;
    media[0].dev.write=absent ? 0 : protected_write;
    uint8_t b[512]={0}; size_t n;
    CHECK(vfs_write(&table,h,b,sizeof(b),&n)==-FS_EROFS && !n);
    CHECK(absent ? !protected_calls && media[0].writes==writes : protected_calls!=0);
    struct fat_volume *v=&s->volumes[4].fat;
    CHECK(v->readonly && (v->ro_reasons&FAT_RO_WRITE_ERROR) && (v->ro_reasons&FAT_RO_WRITE_PROTECTED));
    CHECK(!strcmp(fat_readonly_reason(v),"write_protected"));
    unsigned calls=protected_calls;
    CHECK(vfs_write(&table,h,b,sizeof(b),&n)==-FS_EROFS);
    CHECK(vfs_commit(&table,h)==-FS_EROFS && protected_calls==calls);
    vfs_table_destroy(&table); storage_destroy(s); free(s); /* abandon failed cache */
    printf("PASS FAT12 %s revokes writes with sticky failure and protection reason, no retry\n",
           absent ? "withdrawn write callback" : "issued EROFS");
}
static void removal(void) {
    struct storage *s=fat12_start(true); int h=fat12_create("E:/OLD.BIN");
    uint8_t b[512]; memset(b,0x44,sizeof(b)); size_t n; uint64_t pos;
    OK(vfs_write(&table,h,b,sizeof(b),&n)); CHECK(n==sizeof(b));
    OK(vfs_seek(&table,h,0,VFS_SEEK_SET,&pos)); OK(vfs_read(&table,h,b,sizeof(b),&n));
    CHECK(storage_remove_removable(s,&media[0].dev,false)==-FS_EBUSY);
    struct blkdev *old=&s->volumes[4].io;
    struct fat_entry ent; OK(vfs_stat(&table,"E:/OLD.BIN",&ent));
    uint64_t cached=s->volumes[4].fat.data_sector+(uint64_t)(ent.first-2)*s->volumes[4].fat.spc;
    CHECK(!cache_peek(&s->cache,old,cached,b));
    uint32_t generation=s->vfs.generation[4]; unsigned writes=media[0].writes,flushes=media[0].flushes;
    OK(storage_remove_removable(s,&media[0].dev,true));
    CHECK(s->vfs.generation[4]!=generation && !vfs_drive_present(&s->vfs,4));
    CHECK(cache_peek(&s->cache,old,cached,b)!=0);
    CHECK(vfs_read(&table,h,b,sizeof(b),&n)==-FS_EIO);
    CHECK(vfs_write(&table,h,b,sizeof(b),&n)==-FS_EIO);
    CHECK(vfs_commit(&table,h)==-FS_EIO);
    CHECK(media[0].writes==writes && media[0].flushes==flushes);
    media[0].dev.quarantined=false; media[0].dev.media_generation++;
    OK(storage_add_removable(s,&media[0].dev)); CHECK(vfs_drive_present(&s->vfs,5));
    seed_read("F:/SEED.BIN",4099);
    CHECK(vfs_read(&table,h,b,sizeof(b),&n)==-FS_EIO);
    (void)vfs_close(&table,h); fat12_stop(s);
    printf("PASS FAT12 removal invalidates cache/handles, reinsertion has fresh generation\n");
}
static void split_entries(void) {
    struct storage *s=fat12_start(true); seed_read("E:/SPLIT.BIN",690*1024);
    uint32_t odd,even;
    OK(fat_get_cluster(&s->volumes[4].fat,341,&odd));
    OK(fat_get_cluster(&s->volumes[4].fat,682,&even));
    CHECK(odd==342 && even==683);
    OK(vfs_delete(&table,"E:/SPLIT.BIN"));
    OK(fat_get_cluster(&s->volumes[4].fat,341,&odd));
    OK(fat_get_cluster(&s->volumes[4].fat,682,&even)); CHECK(!odd && !even);
    fat12_stop(s); struct scan_result r=independent_scan(media[0].fd,0);
    CHECK(!r.crosslinks && !r.corrupt && !r.lost && !r.divergent);
    printf("PASS FAT12 odd/even split-sector entries read and reclaimed in both FAT copies\n");
}
int main(int argc,char **argv) {
    CHECK(argc==3); OK(fake_open(&media[0],argv[2])); media[0].dev.media_generation=7;
    if (!strcmp(argv[1],"rw")) operations();
    else if (!strcmp(argv[1],"root")) root_full();
    else if (!strcmp(argv[1],"full")) disk_full();
    else if (!strcmp(argv[1],"corrupt")) corruption();
    else if (!strcmp(argv[1],"dmf")) readonly_case(true);
    else if (!strcmp(argv[1],"protected")) readonly_case(false);
    else if (!strcmp(argv[1],"late-protected")) late_protection(false);
    else if (!strcmp(argv[1],"changed-protected")) late_protection(true);
    else if (!strcmp(argv[1],"remove")) removal();
    else if (!strcmp(argv[1],"split")) split_entries();
    else CHECK(false);
    fake_close(&media[0]); OK(blk_request_pool_host_cleanup());
    printf("PASS FAT12 native checks=%u\n",checks); return 0;
}
