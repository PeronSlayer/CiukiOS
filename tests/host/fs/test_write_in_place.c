/* F0-32: production FAT writes, physical-sector accounting and byte oracle.
 * SPDX-License-Identifier: GPL-2.0-only */
#define main previous_fs_main
#include "test_fs.c"
#undef main

static unsigned data_writes(struct fake *f,unsigned begin,uint32_t first,unsigned sectors)
{
    unsigned n=0;
    for (unsigned i=begin;i<f->events;i++)
        if (f->trace[i].kind=='W' && f->trace[i].lba>=first && f->trace[i].lba<(uint64_t)first+sectors) n++;
    return n;
}
static void wallpaper(struct fake *f,unsigned chunk)
{
    mounted(f,true);CHECK(!vol.readonly);unsigned bits=vol.type;
    int h=create("C:/wallpaper.raw");unsigned begin=f->events,writes=f->writes;
    const unsigned bytes=3*1024*1024;
    for (unsigned pos=0;pos<bytes;pos+=chunk) {
        size_t done;OK(vfs_write(&table,h,big+pos,chunk,&done));CHECK(done==chunk);
    }
    unsigned payload=data_writes(f,begin,vol.data_sector+(vol.type==32 ? vol.spc : 0),vol.sectors-vol.data_sector);
    unsigned total=f->writes-writes;
    CHECK(payload==bytes/512);
    CHECK(total<bytes/512+6*(bytes/chunk)+256); /* mirrored allocation + live link + directory + FSInfo */
    uint64_t pos;size_t done;
    OK(vfs_seek(&table,h,0,VFS_SEEK_SET,&pos));OK(vfs_read(&table,h,readback,bytes,&done));
    CHECK(done==bytes && !memcmp(big,readback,bytes));OK(vfs_close(&table,h));
    unmounted();mounted(f,false);
    struct fat_entry entry;OK(vfs_stat(&table,"C:/wallpaper.raw",&entry));
    OK(fat_read(&vol,&entry,0,readback,bytes,&done));CHECK(done==bytes && !memcmp(big,readback,bytes));
    unmounted();struct scan_result scan=independent_scan(f->fd,0);
    CHECK(!scan.lost && !scan.crosslinks && !scan.corrupt && !scan.divergent && !scan.dirty);
    printf("PASS FAT%u wallpaper bytes=%u chunk=%u payload_sectors=%u metadata_sectors=%u total=%u cold_bytes=match\n",bits,bytes,chunk,payload,total-payload,total);
    fake_reset(f);
}
static void small_writes(struct fake *f)
{
    mounted(f,true);CHECK(!vol.readonly);
    struct fat_entry file;size_t done;uint32_t root=vol.type==32 ? vol.root : 0;
    OK(fat_create(&vol,root,"settings.cfg",0,&file));
    OK(fat_write(&vol,&file,0,big,8192,&done));CHECK(done==8192);
    uint32_t first=file.first,free=vol.free_clusters;
    unsigned begin=f->events;
    OK(fat_write(&vol,&file,509,"settings",8,&done));CHECK(done==8);
    memcpy(big+509,"settings",8);
    CHECK(file.first==first && vol.free_clusters==free && file.size==8192);
    CHECK(data_writes(f,begin,vol.data_sector+(vol.type==32 ? vol.spc : 0),vol.sectors-vol.data_sector)==2);
    /* Registry update within one sector and an aligned DOS overlay sector. */
    begin=f->events;OK(fat_write(&vol,&file,2053,"registry",8,&done));memcpy(big+2053,"registry",8);
    CHECK(data_writes(f,begin,vol.data_sector+(vol.type==32 ? vol.spc : 0),vol.sectors-vol.data_sector)==1);
    begin=f->events;OK(fat_write(&vol,&file,4096,big+9000,512,&done));memcpy(big+4096,big+9000,512);
    CHECK(data_writes(f,begin,vol.data_sector+(vol.type==32 ? vol.spc : 0),vol.sectors-vol.data_sector)==1);
    /* Full-volume overwrite requires no replacement allocation. */
    vol.free_clusters=0;OK(fat_write(&vol,&file,1,"X",1,&done));big[1]='X';vol.free_clusters=free;
    OK(fat_read(&vol,&file,0,readback,8192,&done));CHECK(done==8192 && !memcmp(big,readback,8192));
    /* Force a fragmented successor, zero a gap across old slack/new clusters. */
    struct fat_entry blocker;OK(fat_create(&vol,root,"blocker.bin",0,&blocker));
    OK(fat_write(&vol,&blocker,0,big,vol.spc*512,&done));
    OK(fat_write(&vol,&file,20001,"end",3,&done));CHECK(file.first==first && file.size==20004);
    OK(fat_read(&vol,&file,0,readback,20004,&done));CHECK(done==20004 && !memcmp(big,readback,8192));
    for (unsigned i=8192;i<20001;i++) CHECK(!readback[i]);
    CHECK(!memcmp(readback+20001,"end",3));
    /* Truncation preserves bytes and zero-fills a later explicit extension. */
    OK(fat_truncate(&vol,&file,517));OK(fat_truncate(&vol,&file,700));
    OK(fat_read(&vol,&file,0,readback,700,&done));CHECK(done==700 && !memcmp(big,readback,517));
    for (unsigned i=517;i<700;i++) CHECK(!readback[i]);
    unmounted();struct scan_result scan=independent_scan(f->fd,0);
    CHECK(!scan.lost && !scan.crosslinks && !scan.corrupt && !scan.divergent && !scan.dirty);
    printf("PASS FAT%u settings rewrite=2 sectors registry=1 overlay=1, unchanged first/free, full disk overwrite, fragmented gap, truncate/regrow\n",vol.type);
    fake_reset(f);
}
int main(int argc,char **argv)
{
    CHECK(argc==4);
    for (unsigned bits=0;bits<3;bits++) {
        struct fake f;OK(fake_open(&f,argv[bits+1]));f.undo_enabled=true;
        for (unsigned i=0;i<sizeof(big);i++) big[i]=seed(i);
        wallpaper(&f,4096);wallpaper(&f,65536);small_writes(&f);fake_close(&f);
    }
    CHECK(!fs_host_contig_live);
    printf("WRITE IN PLACE RESULT checks=%u failures=0 ASan/UBSan=enabled\n",checks);return 0;
}
