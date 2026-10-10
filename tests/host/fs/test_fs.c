/* Production core tests with independent file-backed faults and oracle.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "vfs.h"
#include "partition.h"
#include "fake.h"
#include "scan.h"
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sys/wait.h>
static unsigned checks,groups;
static bool test_clock(uint16_t *date, uint16_t *time, uint8_t *tenths) {
    *date=(uint16_t)((2026-1980)*512+10*32+10); *time=10*2048+10*32; *tenths=17; return true;
}
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
#define OK(x) do { int result_=(x); checks++; if (result_) { fprintf(stderr,"FAIL %s:%d: %s => %d\n",__FILE__,__LINE__,#x,result_); exit(1); } } while (0)
static struct block_cache cache;
static struct fat_volume vol;
static struct vfs vfs;
static struct vfs_table table,child_table;
static uint8_t big[4*1024*1024],readback[4*1024*1024];
static void mounted(struct fake *f, bool wr) {
    OK(cache_init(&cache,0)); OK(fat_mount(&vol,&cache,&f->dev,0,f->dev.capacity,wr));
    vfs_init(&vfs); OK(vfs_attach(&vfs,2,&vol)); OK(vfs_table_init(&vfs,&table,2));
}
static void unmounted(void) { vfs_table_destroy(&table); OK(vfs_detach(&vfs,2)); vfs_destroy(&vfs); cache_destroy(&cache); }
static void abandon(void) { vfs_table_destroy(&table); vfs_destroy(&vfs); cache_destroy(&cache); }
static uint8_t seed(unsigned i) { return (uint8_t)(i*37+(i>>8)+0xc1a001); }
static void read_tests(struct fake *f) {
    mounted(f,false); CHECK(vol.readonly); CHECK(!f->writes);
    struct fat_entry e; OK(vfs_stat(&table,"c:\\seed DATA with spaces.BIN",&e)); CHECK(e.size==12345); CHECK(path_equal(e.alias,"SEEDDA~1.BIN"));
    int h=vfs_open(&table,"C:/SEEDDA~1.BIN",VFS_READ,VFS_DENY_NONE,0); CHECK(h>=0);
    size_t n; OK(vfs_read(&table,h,readback,sizeof(readback),&n)); CHECK(n==12345); for (unsigned i=0;i<n;i++) CHECK(readback[i]==seed(i)); OK(vfs_close(&table,h));
    OK(vfs_stat(&table,"C:/CAFÉ.TXT",&e)); CHECK(e.size==12345);
    OK(vfs_chdir(&table,"C:/fixture DIR")); char cwd[FS_PATH_BYTES]; OK(vfs_getcwd(&table,2,cwd)); CHECK(!strcmp(cwd,"/Fixture dir"));
    OK(vfs_stat(&table,"C:Nested.bin",&e)); OK(vfs_stat(&table,"../short.bin",&e));
    struct vfs_find find; unsigned count=0; int err=vfs_find_first(&table,"C:/*.*",0,&find,&e); while (!err) { count++; err=vfs_find_next(&table,&find,&e); } CHECK(err==-FS_ENOENT && count==(vol.type==12 ? 4u : 3u));
    CHECK(vfs_open(&table,"C:/SHORT.BIN",VFS_WRITE,0,0)==-FS_EROFS);
    CHECK(vfs_open(&table,"C:/bad\xf0\x9f\x90\xb6",VFS_READ,0,0)==-FS_EILSEQ);
    char longpath[300]; memset(longpath,'a',sizeof(longpath)); longpath[sizeof(longpath)-1]=0; CHECK(vfs_stat(&table,longpath,&e)==-FS_ENAMETOOLONG);
    if (vol.type==12) {
        h=vfs_open(&table,"C:/Boundary.bin",VFS_READ,0,0); CHECK(h>=0);
        OK(vfs_read(&table,h,readback,sizeof(readback),&n)); CHECK(n==350*4096+123);
        uint32_t expected=2166136261u,actual=expected;
        for (unsigned i=0;i<n;i++) { expected=(expected^seed(i))*16777619u; actual=(actual^readback[i])*16777619u; }
        CHECK(expected==actual); OK(vfs_close(&table,h));
    }
    CHECK(!f->writes); unmounted(); CHECK(!f->writes); groups++;
    printf("PASS FAT%u read/names/aliases/UTF-8/cwd/masks zero_writes\n",vol.type);
}
static int create(const char *name) { int h=vfs_open(&table,name,VFS_READ|VFS_WRITE|VFS_CREATE|VFS_EXCLUSIVE,VFS_DENY_NONE,0); CHECK(h>=0); return h; }
static void ordering(struct fake *f, unsigned begin) {
    unsigned phase=0,copies=0; bool fence=true;
    uint32_t rootsec=vol.type==32 ? vol.data_sector+(vol.root-2)*vol.spc : vol.root_sector;
    unsigned rootsectors=vol.type==32 ? vol.spc : vol.root_entries/16;
    for (unsigned i=begin;i<f->events;i++) {
        struct fake_event *event=&f->trace[i];
        if (event->kind=='B') { fence=true; continue; }
        uint64_t lba=event->lba; unsigned nextphase;
        if (lba>=vol.reserved && lba<vol.reserved+vol.fats*vol.fat_sectors) {
            nextphase=1; copies|=1u<<((lba-vol.reserved)/vol.fat_sectors);
        } else if (lba>=rootsec && lba<rootsec+rootsectors) nextphase=3;
        else if (vol.type==32 && lba==vol.fsinfo) nextphase=4;
        else nextphase=2;
        CHECK(nextphase>=phase);
        if (nextphase!=phase) CHECK(fence);
        phase=nextphase; fence=false;
    }
    CHECK(phase==(vol.type==32 ? 4u : 3u) && copies==3);
}
static void write_file(struct fake *f, const char *name, unsigned bytes) {
    int h=create(name); size_t n; unsigned begin=f->events; OK(vfs_write(&table,h,big,bytes,&n)); CHECK(n==bytes); if (bytes) ordering(f,begin); OK(vfs_commit(&table,h));
    uint64_t pos; OK(vfs_seek(&table,h,0,VFS_SEEK_SET,&pos)); OK(vfs_read(&table,h,readback,sizeof(readback),&n)); CHECK(n==bytes && !memcmp(big,readback,bytes)); OK(vfs_close(&table,h));
}
struct reader { int h,error; size_t bytes; uint64_t sum; };
static void *parallel_reader(void *arg) {
    struct reader *r=arg; uint8_t b[97];
    for (;;) {
        size_t n; r->error=vfs_read(&table,r->h,b,sizeof(b),&n); if (r->error || !n) break;
        r->bytes+=n; for (size_t i=0;i<n;i++) r->sum+=b[i];
    }
    return 0;
}
static void concurrent_reads(void) {
    int h=vfs_open(&table,"C:/SHORT.BIN",VFS_READ,0,0); CHECK(h>=0);
    struct reader readers[4]={{0}}; pthread_t threads[4];
    for (unsigned i=0;i<4;i++) { readers[i].h=h; CHECK(!pthread_create(&threads[i],0,parallel_reader,&readers[i])); }
    size_t bytes=0; uint64_t sum=0,expected=0;
    for (unsigned i=0;i<4;i++) { CHECK(!pthread_join(threads[i],0)); CHECK(!readers[i].error); bytes+=readers[i].bytes; sum+=readers[i].sum; }
    for (unsigned i=0;i<12345;i++) expected+=seed(i);
    CHECK(bytes==12345 && sum==expected); OK(vfs_close(&table,h));
}
static void write_tests(struct fake *f) {
    mounted(f,true); CHECK(!vol.readonly); unsigned cb=vol.spc*512; concurrent_reads();
    for (unsigned i=0;i<sizeof(big);i++) big[i]=seed(i);
    write_file(f,"C:/Zero bytes.txt",0); write_file(f,"C:/One cluster.bin",cb); write_file(f,"C:/Four MiB.bin",sizeof(big));
    OK(vfs_mkdir(&table,"C:/Growing directory",0));
    for (unsigned i=0;i<vol.spc*16/3+10;i++) { char p[100]; snprintf(p,sizeof(p),"C:/Growing directory/Collision example %03u.txt",i); int h=create(p); OK(vfs_close(&table,h)); }
    struct fat_entry e; OK(vfs_stat(&table,"C:/Growing directory",&e)); uint32_t next; OK(fat_get_cluster(&vol,e.first,&next)); CHECK(next<vol.clusters+2);
    const char *boundaries[]={"C:/abcdefghijklm","C:/abcdefghijklmnopqrstuvwxyz"};
    for (unsigned k=0;k<2;k++) { int boundary=create(boundaries[k]); OK(vfs_close(&table,boundary)); OK(vfs_stat(&table,boundaries[k],&e)); OK(vfs_delete(&table,boundaries[k])); }
    char maxname[FS_PATH_BYTES]; memcpy(maxname,"C:/",3);
    for (unsigned i=0;i<255;i++) { maxname[3+i*2]=(char)0xc3; maxname[4+i*2]=(char)0xa9; } maxname[513]=0;
    int maximum=create(maxname); OK(vfs_close(&table,maximum)); OK(vfs_stat(&table,maxname,&e)); CHECK(!strcmp(e.name,maxname+3)); OK(vfs_delete(&table,maxname));
    OK(vfs_mkdir(&table,"C:/Target",0)); OK(vfs_rename(&table,"C:/One cluster.bin","C:/Target/Moved cluster.bin"));
    OK(vfs_rename(&table,"C:/Zero bytes.txt","C:/Empty.txt"));
    CHECK(vfs_rename(&table,"C:/Target","C:/Target/Bad")==-FS_EINVAL);
    CHECK(vfs_rmdir(&table,"C:/Target")==-FS_ENOTEMPTY);
    int h=vfs_open(&table,"C:/Four MiB.bin",VFS_READ|VFS_WRITE,VFS_DENY_NONE,0); CHECK(h>=0);
    int other=vfs_open(&table,"C:/Four MiB.bin",VFS_READ,VFS_DENY_NONE,0); CHECK(other>=0);
    int dup=vfs_dup(&table,h,&table,-1); CHECK(dup>=0); OK(vfs_table_init(&vfs,&child_table,2)); int inherited=vfs_dup(&table,h,&child_table,-1); CHECK(inherited>=0);
    size_t n; uint64_t pos; OK(vfs_read(&table,h,readback,7,&n)); OK(vfs_seek(&table,dup,0,VFS_SEEK_CUR,&pos)); CHECK(pos==7);
    OK(vfs_seek(&child_table,inherited,0,VFS_SEEK_CUR,&pos)); CHECK(pos==7);
    CHECK(vfs_open(&table,"C:/Four MiB.bin",VFS_READ,VFS_DENY_WRITE,0)==-FS_EACCES);
    CHECK(vfs_delete(&table,"C:/Four MiB.bin")==-FS_EBUSY);
    OK(vfs_seek(&table,h,19,VFS_SEEK_SET,&pos)); OK(vfs_write(&table,h,"overwrite",9,&n)); CHECK(n==9);
    OK(vfs_seek(&table,other,19,VFS_SEEK_SET,&pos)); OK(vfs_read(&table,other,readback,9,&n)); CHECK(n==9 && !memcmp(readback,"overwrite",9));
    OK(vfs_truncate(&table,h,cb+3)); OK(vfs_stat(&table,"C:/Four MiB.bin",&e)); CHECK(e.size==cb+3);
    OK(vfs_truncate(&table,h,cb+103)); OK(vfs_seek(&table,other,cb+3,VFS_SEEK_SET,&pos)); OK(vfs_read(&table,other,readback,100,&n)); CHECK(n==100); for (unsigned i=0;i<n;i++) CHECK(!readback[i]);
    OK(vfs_seek(&table,h,(int64_t)UINT32_MAX+1,VFS_SEEK_SET,&pos)); CHECK(vfs_write(&table,h,"x",1,&n)==-FS_EFBIG && !n); CHECK(vfs_truncate(&table,h,(uint64_t)UINT32_MAX+1)==-FS_EFBIG);
    CHECK(vfs_seek(&table,h,INT64_MIN,VFS_SEEK_CUR,&pos)==-FS_EINVAL);
    struct fat_times times={.create_date=0x5021,.create_time=0x1234,.access_date=0x5022,.write_date=0x5023,.write_time=0x5678,.create_tenths=17}; OK(vfs_times(&table,"C:/Four MiB.bin",&times));
    OK(vfs_stat(&table,"C:/Four MiB.bin",&e)); CHECK(e.times.write_time==0x5678);
    OK(vfs_close(&table,h)); OK(vfs_close(&table,dup)); OK(vfs_close(&table,other)); vfs_table_destroy(&child_table);
    h=vfs_open(&table,"C:/Four MiB.bin",VFS_READ,VFS_DENY_WRITE,0); CHECK(h>=0); CHECK(vfs_open(&table,"C:/Four MiB.bin",VFS_WRITE,0,0)==-FS_EACCES); OK(vfs_close(&table,h));
    OK(vfs_attrib(&table,"C:/Four MiB.bin",FAT_ATTR_RO|FAT_ATTR_HIDDEN)); CHECK(vfs_open(&table,"C:/Four MiB.bin",VFS_WRITE,0,0)==-FS_EACCES); OK(vfs_attrib(&table,"C:/Four MiB.bin",FAT_ATTR_ARCHIVE));
    h=create("C:/Delete me.bin"); OK(vfs_write(&table,h,big,3000,&n)); OK(vfs_close(&table,h)); OK(vfs_delete(&table,"C:/Delete me.bin"));
    OK(vfs_mkdir(&table,"C:/Empty dir",0)); OK(vfs_rmdir(&table,"C:/Empty dir"));
    CHECK(vfs_open(&table,"C:/unmapped \xe4\xb8\xad.txt",VFS_CREATE|VFS_WRITE,0,0)==-FS_EILSEQ);
    CHECK(vfs_open(&table,"C:/bad?.txt",VFS_CREATE|VFS_WRITE,0,0)==-FS_EINVAL);
    uint64_t freebytes; OK(vfs_free_space(&table,2,&freebytes,&cb)); CHECK(freebytes>0);
    unmounted(); struct scan_result s=independent_scan(f->fd,0); CHECK(!s.crosslinks && !s.corrupt && !s.lost && !s.divergent && !s.dirty);
    mounted(f,false); OK(vfs_stat(&table,"C:/Target/Moved cluster.bin",&e)); CHECK(e.size==cb); unmounted();
    groups++; printf("PASS FAT%u write/4MiB/overwrite/truncate/dir-growth/collisions/rename/delete/shared-handles\n",vol.type);
}
/* Small, mixed workload replayed with a new cache and VFS at EVERY cut. */
static int crash_workload(struct fake *f) {
    int e=cache_init(&cache,2*1024*1024); if (e) return e;
    e=fat_mount(&vol,&cache,&f->dev,0,f->dev.capacity,true);
    if (e || vol.readonly) { cache_destroy(&cache); return e ? e : -FS_EROFS; }
    struct fat_entry a,b,file; size_t n;
    if (vol.type==12) vol.next_free=680;
    e=fat_create(&vol,vol.root,"Crash A",FAT_ATTR_DIR,&a);
    if (!e) e=fat_create(&vol,vol.root,"Crash B",FAT_ATTR_DIR,&b);
    if (!e) e=fat_create(&vol,a.first,"Crash file long.bin",0,&file);
    if (!e) e=fat_write(&vol,&file,0,big,vol.spc*1024+300,&n);
    if (!e) e=fat_write(&vol,&file,19,big+19,30,&n);
    if (!e) e=fat_rename(&vol,&file,b.first,"Renamed file.bin");
    if (!e) e=fat_truncate(&vol,&file,17);
    if (!e) e=fat_remove(&vol,&file,false);
    if (!e) e=fat_unmount(&vol);
    cache_destroy(&cache); return e;
}
static void crash_fsck(struct fake *f, unsigned mode, unsigned cut, unsigned bits, const struct scan_result *scan) {
    int stream[2]; CHECK(!pipe(stream)); pid_t child=fork(); CHECK(child>=0);
    if (!child) {
        char path[64]; snprintf(path,sizeof(path),"/proc/self/fd/%d",f->fd);
        close(stream[0]); if (dup2(stream[1],1)<0 || dup2(stream[1],2)<0) _exit(126);
        close(stream[1]); execlp("fsck.fat","fsck.fat","-n",path,(char *)0); _exit(127);
    }
    close(stream[1]); FILE *input=fdopen(stream[0],"r"), *log=fopen("build/host/fs/crash-fsck.log","a"); CHECK(input && log);
    fprintf(log,"FAT%u mode=%u cut=%u\n",bits,mode,cut);
    fprintf(log,"oracle lost=%u divergent=%u dirty=%u crosslinks=%u corrupt=%u\n",scan->lost,scan->divergent,scan->dirty,scan->crosslinks,scan->corrupt);
    char line[2048]; while (fgets(line,sizeof(line),input)) fputs(line,log);
    fclose(input); int status; CHECK(waitpid(child,&status,0)==child); CHECK(WIFEXITED(status));
    fprintf(log,"exit=%d\n",WEXITSTATUS(status)); fclose(log);
    CHECK(WEXITSTATUS(status)==0 || WEXITSTATUS(status)==1);
}
static void crash_tests(struct fake *f) {
    f->undo_enabled=true; unsigned trials=0,divergent=0,lost=0,dirty=0;
    /* 2 MiB cache provides mount ownership scratch for all fixtures. */
    for (unsigned mode=0;mode<4;mode++) {
        fake_reset(f); f->reorder=mode!=0; f->cut_mode=mode-1;
        OK(crash_workload(f)); unsigned events=f->events; CHECK(events>20); fake_reset(f);
        for (unsigned cut=1;cut<=events;cut++) {
            f->cut_at=cut; int e=crash_workload(f); CHECK(e<0 && f->cut);
            fake_power_loss(f);
            struct scan_result s=independent_scan(f->fd,0),second=independent_scan(f->fd,1);
            crash_fsck(f,mode,cut,vol.type,&s);
            if (s.crosslinks || second.crosslinks || s.corrupt || second.corrupt) fprintf(stderr,"crash details mode=%u cut=%u/%u cross=%u/%u corrupt=%u/%u\n",mode,cut,events,s.crosslinks,second.crosslinks,s.corrupt,second.corrupt);
            CHECK(!s.crosslinks && !second.crosslinks && !s.corrupt && !second.corrupt);
            OK(cache_init(&cache,2*1024*1024)); OK(fat_mount(&vol,&cache,&f->dev,0,f->dev.capacity,true));
            if (s.divergent || s.dirty || s.lost) CHECK(vol.readonly);
            if (s.divergent) { CHECK(vol.ro_reasons&FAT_RO_COPIES); divergent++; }
            if (s.dirty) dirty++; if (s.lost) lost++;
            cache_destroy(&cache); fake_reset(f); trials++;
        }
    }
    f->reorder=false; f->undo_enabled=false;
    groups++; printf("PASS FAT%u crash: cuts=%u modes=4 copies_diverged=%u lost=%u dirty=%u crosslinks=0 corrupt_owners=0\n",vol.type,trials,divergent,lost,dirty);
}
static void patch_fat(struct fake *f, const struct fat_volume *v, unsigned copy, uint32_t cluster, uint32_t value) {
    uint32_t off=v->type==12 ? cluster+cluster/2 : cluster*(v->type/8); uint8_t b[1024];
    uint64_t sec=v->reserved+copy*v->fat_sectors+off/512;
    OK(fake_raw_read(f,sec,b)); if (off%512>508) OK(fake_raw_read(f,sec+1,b+512));
    uint8_t *p=b+off%512;
    if (v->type==12) { uint16_t x=fs_rd16(p); x=(uint16_t)((cluster&1) ? (x&15)|(value<<4) : (x&0xf000)|value); fs_wr16(p,x); }
    else if (v->type==16) fs_wr16(p,(uint16_t)value); else fs_wr32(p,(fs_rd32(p)&0xf0000000)|value);
    OK(fake_raw_write(f,sec,b)); if (off%512>508) OK(fake_raw_write(f,sec+1,b+512));
}
static void assert_ro(struct fake *f, unsigned reason) {
    OK(cache_init(&cache,2*1024*1024)); OK(fat_mount(&vol,&cache,&f->dev,0,f->dev.capacity,true));
    CHECK(vol.readonly && (vol.ro_reasons&reason)); struct fat_entry e; CHECK(fat_create(&vol,vol.root,"Forbidden",0,&e)==-FS_EROFS);
    CHECK(!f->writes); OK(fat_unmount(&vol)); cache_destroy(&cache);
}
static void fault_tests(struct fake *f) {
    f->undo_enabled=true; mounted(f,false); struct fat_volume geometry=vol; struct fat_entry entry;
    OK(vfs_stat(&table,"C:/Seed data with spaces.bin",&entry)); unmounted();
    unsigned bits=geometry.type;
    uint8_t b[512]; OK(fake_raw_read(f,0,b)); b[13]=3; OK(fake_raw_write(f,0,b));
    OK(cache_init(&cache,2*1024*1024)); CHECK(fat_mount(&vol,&cache,&f->dev,0,f->dev.capacity,true)==-FS_EINVAL); CHECK(!f->writes); cache_destroy(&cache); fake_reset(f);
    for (unsigned bad=0;bad<5;bad++) {
        OK(fake_raw_read(f,0,b));
        if (bad==0) b[16]=0;
        else if (bad==1) fs_wr16(b+14,0);
        else if (bad==2) { if (bits==32) fs_wr32(b+36,1); else fs_wr16(b+22,1); }
        else if (bad==3) fs_wr16(b+11,4096);
        else fs_wr16(b+510,0);
        OK(fake_raw_write(f,0,b)); OK(cache_init(&cache,2*1024*1024));
        CHECK(fat_mount(&vol,&cache,&f->dev,0,f->dev.capacity,true)==-FS_EINVAL);
        CHECK(!f->writes); cache_destroy(&cache); fake_reset(f);
    }
    patch_fat(f,&geometry,0,0,0); OK(cache_init(&cache,2*1024*1024));
    CHECK(fat_mount(&vol,&cache,&f->dev,0,f->dev.capacity,true)==-FS_EINVAL); cache_destroy(&cache); fake_reset(f);
    patch_fat(f,&geometry,0,1,0); OK(cache_init(&cache,2*1024*1024));
    CHECK(fat_mount(&vol,&cache,&f->dev,0,f->dev.capacity,true)==-FS_EINVAL); cache_destroy(&cache); fake_reset(f);
    if (bits==32) {
        OK(fake_raw_read(f,geometry.fsinfo,b)); b[0]^=1; OK(fake_raw_write(f,geometry.fsinfo,b));
        OK(cache_init(&cache,2*1024*1024)); CHECK(fat_mount(&vol,&cache,&f->dev,0,f->dev.capacity,true)==-FS_EINVAL); cache_destroy(&cache); fake_reset(f);
        OK(fake_raw_read(f,0,b)); fs_wr32(b+44,geometry.clusters+2); OK(fake_raw_write(f,0,b));
        OK(cache_init(&cache,2*1024*1024)); CHECK(fat_mount(&vol,&cache,&f->dev,0,f->dev.capacity,true)==-FS_EINVAL); cache_destroy(&cache); fake_reset(f);
    }
    f->dev.write_cache_state=BLKDEV_CACHE_UNKNOWN; assert_ro(f,FAT_RO_DURABILITY); f->dev.write_cache_state=BLKDEV_CACHE_ENABLED;
    int (*flush)(struct blkdev *)=f->dev.flush; f->dev.flush=0; assert_ro(f,FAT_RO_DURABILITY); f->dev.flush=flush;
    if (bits!=12) {
        for (unsigned flag=0;flag<2;flag++) {
            unsigned m=bits==16 ? 0xffffu : 0xfffffffu, bit=bits==16 ? 0x8000u : 0x8000000u; if (flag) bit>>=1;
            for (unsigned copy=0;copy<2;copy++) patch_fat(f,&geometry,copy,1,m&~bit);
            assert_ro(f,flag ? FAT_RO_IO_FLAG : FAT_RO_DIRTY); fake_reset(f);
        }
    }
    patch_fat(f,&geometry,1,entry.first,bits==12 ? 0xfff : bits==16 ? 0xffff : 0xfffffff); assert_ro(f,FAT_RO_COPIES); fake_reset(f);
    for (unsigned copy=0;copy<2;copy++) patch_fat(f,&geometry,copy,entry.first,entry.first);
    assert_ro(f,FAT_RO_CORRUPT); fake_reset(f);
    /* Simulated torn FAT sector: persist an arbitrary prefix and lose power. */
    uint64_t sec=geometry.reserved; OK(fake_raw_read(f,sec,b)); b[20]^=0x33; f->torn_lba=sec; f->torn_bytes=23;
    CHECK(f->dev.write(&f->dev,sec,1,b)==-FS_EIO); fake_power_loss(f); f->writes=0; assert_ro(f,FAT_RO_COPIES); fake_reset(f);
    /* Bad and orphan LFNs must fall back to the unmodified short alias. */
    uint32_t rootsec=geometry.type==32 ? geometry.data_sector+(geometry.root-2)*geometry.spc : geometry.root_sector;
    OK(fake_raw_read(f,rootsec,b)); CHECK(b[11]==15); b[13]^=1; OK(fake_raw_write(f,rootsec,b));
    mounted(f,false); CHECK(vfs_stat(&table,"C:/Seed data with spaces.bin",&entry)==-FS_ENOENT); OK(vfs_stat(&table,"C:/SEEDDA~1.BIN",&entry)); CHECK(!strcmp(entry.name,entry.alias)); unmounted(); fake_reset(f);
    OK(fake_raw_read(f,rootsec,b)); b[0]=0xe5; OK(fake_raw_write(f,rootsec,b)); mounted(f,false); OK(vfs_stat(&table,"C:/SEEDDA~1.BIN",&entry)); CHECK(!strcmp(entry.name,entry.alias)); unmounted(); fake_reset(f);
    /* Unspecified bytes after a 00 directory terminator may look like live
     * entries. They must never acquire ownership when the directory grows. */
    OK(fake_raw_read(f,rootsec,b)); unsigned terminator=0,short_index=0;
    while (b[short_index*32+11]==15) short_index++;
    while (terminator<16 && b[terminator*32]) terminator++;
    CHECK(terminator+2<16); memcpy(b+(terminator+2)*32,b+short_index*32,32);
    OK(fake_raw_write(f,rootsec,b)); mounted(f,true); CHECK(!vol.readonly);
    int stale_handle=create("C:/X"); OK(vfs_close(&table,stale_handle)); unmounted();
    struct scan_result stale=independent_scan(f->fd,0); CHECK(!stale.crosslinks && !stale.corrupt); fake_reset(f);
    /* Explicit failed read returns EIO, including cache miss after mount. */
    mounted(f,false); f->fail_read=geometry.data_sector+(entry.first-2)*geometry.spc;
    cache_invalidate(&cache,&f->dev,false); int h=vfs_open(&table,"C:/SHORT.BIN",VFS_READ,0,0); CHECK(h>=0);
    struct fat_entry shortfile; OK(vfs_stat(&table,"C:/SHORT.BIN",&shortfile)); f->fail_read=geometry.data_sector+(shortfile.first-2)*geometry.spc;
    size_t n; CHECK(vfs_read(&table,h,readback,10,&n)==-FS_EIO); CHECK(!n); OK(vfs_close(&table,h)); f->fail_read=UINT64_MAX; unmounted(); fake_reset(f);
    mounted(f,true); f->fail_write=geometry.reserved+(bits==12 ? vol.next_free+vol.next_free/2 : vol.next_free*(bits/8))/512;
    h=create("C:/Will fail.bin"); CHECK(vfs_write(&table,h,big,1024,&n)==-FS_EIO); CHECK(vol.readonly);
    CHECK(vfs_commit(&table,h)==-FS_EIO); CHECK(fat_unmount(&vol)==-FS_EIO); abandon(); fake_power_loss(f); fake_reset(f);
    mounted(f,true); f->fail_flush=true; h=vfs_open(&table,"C:/Flush fails",VFS_WRITE|VFS_CREATE,0,0); CHECK(h<0); CHECK(vol.readonly); CHECK(fat_unmount(&vol)==-FS_EIO); abandon(); fake_power_loss(f); fake_reset(f);
    /* ENOSPC must not publish a partially allocated file. */
    mounted(f,true); h=create("C:/Full.bin"); uint64_t too_large=(uint64_t)(vol.free_clusters+1)*vol.spc*512;
    CHECK(vfs_truncate(&table,h,too_large)==-FS_ENOSPC); OK(vfs_stat(&table,"C:/Full.bin",&entry)); CHECK(!entry.size && !entry.first); OK(vfs_close(&table,h)); OK(vfs_delete(&table,"C:/Full.bin")); unmounted(); fake_reset(f);
    /* Verified disabled hardware cache works without a flush operation. */
    f->dev.write_cache_state=BLKDEV_CACHE_DISABLED; f->dev.flush=0; mounted(f,true); CHECK(!vol.readonly); h=create("C:/No flush.txt"); OK(vfs_close(&table,h)); unmounted(); CHECK(!f->flushes); fake_reset(f); f->dev.write_cache_state=BLKDEV_CACHE_ENABLED; f->dev.flush=flush;
    f->undo_enabled=false; groups++; printf("PASS FAT%u faults: BPB/dirty/error/divergence/loop/torn/LFN/read/write/flush/ENOSPC/durability\n",bits);
}
static void cache_tests(struct fake *f) {
    f->undo_enabled=true; fake_reset(f); OK(cache_init(&cache,8192)); uint8_t b[512],a[512]; memset(b,0xa7,sizeof(b));
    uint32_t before=fs_now_ms(); OK(cache_write(&cache,&f->dev,200,b)); CHECK(!f->writes);
    OK(cache_writeback_tick(&cache,before+4999)); CHECK(!f->writes);
    OK(cache_writeback_tick(&cache,fs_now_ms()+5000)); CHECK(f->writes==1 && f->flushes==1); OK(fake_raw_read(f,200,a)); CHECK(!memcmp(a,b,512));
    f->reorder=true; for (unsigned i=201;i<220;i++) { b[0]=(uint8_t)i; OK(cache_write(&cache,&f->dev,i,b)); }
    CHECK(f->writes>1); OK(cache_barrier(&cache,&f->dev));
    for (unsigned i=201;i<220;i++) { OK(fake_raw_read(f,i,a)); CHECK(a[0]==i); }
    f->fail_write=220; OK(cache_write(&cache,&f->dev,220,b)); CHECK(cache_barrier(&cache,&f->dev)==-FS_EIO);
    f->fail_write=UINT64_MAX; CHECK(cache_barrier(&cache,&f->dev)==-FS_EIO); CHECK(cache_error(&cache,&f->dev)==-FS_EIO);
    struct blkdev second=f->dev;
    for (unsigned i=230;i<250;i++) { b[0]=(uint8_t)i; OK(cache_write(&cache,&second,i,b)); }
    OK(cache_barrier(&cache,&second));
    CHECK(cache_barrier(&cache,&f->dev)==-FS_EIO);
    cache_destroy(&cache); fake_reset(f); f->reorder=false; f->undo_enabled=false; groups++; printf("PASS cache: LRU/5-second/barriers/reorder/sticky-errors/multiple-devices\n");
}
static void part_entry(uint8_t *b, unsigned i, uint8_t type, uint32_t start, uint32_t n) {
    uint8_t *p=b+446+i*16; p[4]=type; fs_wr32(p+8,start); fs_wr32(p+12,n); fs_wr16(b+510,0xaa55);
}
static void partition_tests(struct fake *f) {
    f->undo_enabled=true; uint8_t b[512]={0}; struct partition_table t;
    part_entry(b,0,0x0c,100,100); part_entry(b,1,0x0f,1000,4000); OK(fake_raw_write(f,0,b));
    memset(b,0,sizeof(b)); part_entry(b,0,6,1,100); part_entry(b,1,15,1000,3000); OK(fake_raw_write(f,1000,b));
    memset(b,0,sizeof(b)); part_entry(b,0,6,7,100); OK(fake_raw_write(f,2000,b));
    OK(partition_scan(&f->dev,&t)); CHECK(t.count==3 && t.entries[1].start==1001 && t.entries[2].start==2007 && t.ebr_reads==2);
    part_entry(b,1,15,0,4000); OK(fake_raw_write(f,2000,b)); CHECK(partition_scan(&f->dev,&t)==-FS_ELOOP && !t.count);
    memset(b,0,sizeof(b)); part_entry(b,0,6,UINT32_MAX,UINT32_MAX); OK(fake_raw_write(f,0,b)); CHECK(partition_scan(&f->dev,&t)==-FS_EINVAL);
    memset(b,0,sizeof(b)); part_entry(b,0,6,100,200); part_entry(b,1,6,150,20); OK(fake_raw_write(f,0,b)); CHECK(partition_scan(&f->dev,&t)==-FS_EINVAL);
    memset(b,0,sizeof(b)); part_entry(b,0,0xee,1,100); OK(fake_raw_write(f,0,b)); CHECK(partition_scan(&f->dev,&t)==-FS_EOPNOTSUPP);
    b[510]=0; OK(fake_raw_write(f,0,b)); CHECK(partition_scan(&f->dev,&t)==-FS_EINVAL);
    memset(b,0,sizeof(b)); part_entry(b,0,15,1000,1000); OK(fake_raw_write(f,0,b)); memset(b,0,sizeof(b)); part_entry(b,0,6,0,10); OK(fake_raw_write(f,1000,b)); CHECK(partition_scan(&f->dev,&t)==-FS_EINVAL);
    fake_reset(f); f->undo_enabled=false; groups++; printf("PASS partitions: primary/EBR-relative/loop/overflow/overlap/GPT/signature/EBR-overlap\n");
}
static void manifest_dir(uint32_t dir, const char *prefix, unsigned depth) {
    CHECK(depth<32); uint32_t cursor=0; struct fat_entry e; int err;
    while (!(err=fat_next(&vol,dir,&cursor,&e))) {
        if (!strcmp(e.alias,".") || !strcmp(e.alias,"..")) continue;
        char name[FS_PATH_BYTES]; int len=snprintf(name,sizeof(name),"%s/%s",prefix,e.name); CHECK(len>0 && len<(int)sizeof(name));
        if (e.attr&FAT_ATTR_DIR) { printf("D\t%s\n",name); manifest_dir(e.first,name,depth+1); }
        else {
            uint32_t hash=2166136261u; uint64_t pos=0;
            while (pos<e.size) { size_t n; OK(fat_read(&vol,&e,pos,readback,sizeof(readback),&n)); CHECK(n); for (size_t i=0;i<n;i++) hash=(hash^readback[i])*16777619u; pos+=n; }
            printf("F\t%s\t%u\t%08x\n",name,e.size,hash);
        }
    }
    CHECK(err==-FS_ENOENT);
}
int main(int argc,char **argv) {
    if (argc!=3) { fprintf(stderr,"usage: test_fs read|write|fault|crash|cache|partition|manifest image\n"); return 2; }
    fs_set_calendar_clock(test_clock);
    struct fake f; OK(fake_open(&f,argv[2]));
    if (!strcmp(argv[1],"read")) read_tests(&f);
    else if (!strcmp(argv[1],"write")) write_tests(&f);
    else if (!strcmp(argv[1],"fault")) fault_tests(&f);
    else if (!strcmp(argv[1],"crash")) crash_tests(&f);
    else if (!strcmp(argv[1],"cache")) cache_tests(&f);
    else if (!strcmp(argv[1],"partition")) partition_tests(&f);
    else if (!strcmp(argv[1],"manifest")) { mounted(&f,false); manifest_dir(vol.root,"",0); unmounted(); fake_close(&f); return 0; }
    else return 2;
    fake_close(&f); printf("RESULT groups=%u checks=%u failures=0 ASan/UBSan=enabled\n",groups,checks); return 0;
}
