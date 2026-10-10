/* Production mount/log/probe integration with real mkfs fixtures and a
 * synthesized MBR (no full disk image copy). SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/storage.h>
#include <ciuki/bootlog.h>
#include <ciuki/kernel.h>
#include <ciuki/probe.h>
#include "fake.h"
#include "scan.h"
#include <stdio.h>
#include "../storage_ledger_fixture.h"

static unsigned checks, records, max_record;
static unsigned fixture_present, fixture_absent;
static unsigned fat_read_not_run, fat_read_terminal_not_run;
static bool cut_probe_after_arm;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"STORAGE line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
#define OK(x) CHECK((x)==0)
struct ciuki_boot_info g_boot;
static struct vfs_table table;
static struct fake media[3];
struct disk { struct blkdev dev; struct fake *media; };
static struct disk disks[3];
void klog(const char *fmt, ...) {
    char line[256]; va_list ap; va_start(ap,fmt); vsnprintf(line,sizeof(line),fmt,ap); va_end(ap);
    bootlog_capture(line,strlen(line)); bootlog_capture("\n",1);
}
void rec_emit(const char *probe, const char *event, const char *fmt, ...) {
    char line[1024]; va_list ap; va_start(ap,fmt);
    int n=fmt ? vsnprintf(line,sizeof(line),fmt,ap) : 0; va_end(ap);
    unsigned length=51u+(unsigned)strlen(probe)+(unsigned)strlen(event)+(unsigned)n;
    if (length>max_record) max_record=length;
    if (length>240) fprintf(stderr,"oversized: %s %s %s (%u)\n",probe,event,line,length);
    CHECK(n>=0 && length<=240); records++;
    if (!strcmp(probe,"mount-crash") && !strcmp(event,"ARM") && cut_probe_after_arm) {
        media[0].cut_at=media[0].events+4;
    }
    if (!strcmp(probe,"fat-read") && !strcmp(event,"DATA") && !strncmp(line,"case=fixture ",13)) {
        unsigned disk; char status[16];
        CHECK(sscanf(line,"case=fixture disk=%u status=%15s",&disk,status)==2 && disk>0 && disk<STORAGE_DISKS);
        CHECK(!strcmp(status,"present") || !strcmp(status,"absent"));
        unsigned *mask=!strcmp(status,"present") ? &fixture_present : &fixture_absent;
        CHECK(!((fixture_present|fixture_absent)&(1u<<disk))); *mask|=1u<<disk;
    }
    if (!strcmp(probe,"fat-read") && !strcmp(event,"ERROR")) {
        CHECK(strstr(line,"status=not_run reason=fixtures_absent")); fat_read_not_run++;
    }
    if (!strcmp(probe,"fat-read") && !strcmp(event,"END") && strstr(line,"status=NOT_RUN"))
        fat_read_terminal_not_run++;
}
static int disk_read(struct blkdev *dev, uint64_t lba, uint32_t n, void *buf) {
    struct disk *d=dev->ctx; int e=blkdev_range(dev,lba,n); if(e) return e;
    uint8_t *b=buf;
    for(unsigned i=0;i<n;i++,b+=512,lba++) {
        if(lba>=2048) { e=d->media->dev.read(&d->media->dev,lba-2048,1,b); if(e) return e; }
        else {
            memset(b,0,512);
            if(!lba) { b[446]=0x80; b[450]=d==&disks[0] ? 0x0c : d==&disks[1] ? 1 : 6;
                fs_wr32(b+454,2048); fs_wr32(b+458,(uint32_t)d->media->dev.capacity); fs_wr16(b+510,0xaa55); }
        }
    }
    return 0;
}
static int disk_write(struct blkdev *dev,uint64_t lba,uint32_t n,const void *buf) {
    struct disk *d=dev->ctx; CHECK(lba>=2048); return d->media->dev.write(&d->media->dev,lba-2048,n,buf);
}
static int disk_flush(struct blkdev *dev) { struct disk *d=dev->ctx; return d->media->dev.flush(&d->media->dev); }
static void disk_init(unsigned i) {
    disks[i]=(struct disk){.media=&media[i]};
    disks[i].dev=(struct blkdev){.read=disk_read,.write=disk_write,.flush=disk_flush,.capacity=media[i].dev.capacity+2048,
        .sector_size=512,.write_cache_state=BLKDEV_CACHE_ENABLED,.ctx=&disks[i]};
}
static struct storage *start(void) {
    struct storage *s=storage_get(); disk_init(0); OK(storage_setup(s,0)); OK(storage_add_disk(s,0,&disks[0].dev));
    host_mount_snapshot(s);
    OK(vfs_table_init(&s->vfs,&table,2)); return s;
}
static void stop(struct storage *s,bool clean) {
    vfs_table_destroy(&table); if(clean) OK(storage_shutdown(s)); storage_destroy(s);
}
static void baseline(void) {
    /* Model the image builder's SYSTEM directory, before mount's LOGS hook. */
    struct storage *s=start(); OK(fat_enable_write(&s->volumes[2].fat)); OK(vfs_mkdir(&table,"C:/SYSTEM",0)); stop(s,true);
    media[0].reads=media[0].writes=media[0].flushes=media[0].events=0; media[0].undo_enabled=true;
}
static void reset(void) { fake_reset(&media[0]); bootlog_reset(); }
static void superfloppy_tests(void) {
    /* The generator's unwrapped FAT12/16 volumes use LBA zero directly. */
    for(unsigned i=1;i<3;i++) {
        struct fake *f=&media[i]; unsigned writes=f->writes, flushes=f->flushes;
        struct storage *s=start(); OK(storage_add_disk(s,i,&f->dev));
        struct storage_volume *v=storage_volume(s,3);
        CHECK(v && !v->error && v->disk==i && !v->partition && !v->part.start);
        CHECK(v->io.capacity==f->dev.capacity && v->fat.type==(i==1 ? 12 : 16));
        CHECK(v->fat.readonly && v->read_gate && v->read_sequence && !v->writes_before_gate);
        CHECK(f->writes==writes && f->flushes==flushes);
        CHECK(storage_add_disk(s,i,&f->dev)==-FS_EINVAL);
        stop(s,true); reset();
        /* C: still requires the boot MBR, even with a valid FAT BPB. */
        OK(storage_setup(s,0)); int e=storage_add_disk(s,0,&f->dev);
        CHECK(e==0 || e==-FS_EINVAL);
        CHECK(!storage_volume(s,2) && !storage_volume(s,3)); storage_destroy(s);
    }
    struct fake *f=&media[1]; f->undo_enabled=true;
    uint8_t bpb[512],bad[512]; OK(fake_raw_read(f,0,bpb));
    for(unsigned fault=0;fault<5;fault++) {
        memcpy(bad,bpb,512);
        if(!fault) memset(bad,0,512);                 /* neither MBR nor BPB */
        if(fault==1) bad[13]=3;                     /* invalid cluster size */
        if(fault==2) { fs_wr16(bad+19,0); fs_wr32(bad+32,(uint32_t)f->dev.capacity+1); }
        if(fault==3) { bad[0]=0; }                 /* invalid jump */
        if(fault==4) fs_wr16(bad+11,1024);           /* unsupported BPB sectors */
        OK(fake_raw_write(f,0,bad));
        struct storage *s=start(); int e=storage_add_disk(s,1,&f->dev);
        struct storage_volume *v=storage_volume(s,3);
        CHECK(e<0 || !v || (v->error && !v->fat.mounted));
        if(!fault) CHECK(e==-FS_EINVAL && !v);
        CHECK(!s->vfs.volumes[3] && !f->writes && !f->flushes);
        stop(s,true); fake_reset(f); reset();
    }
    /* A valid MBR wins over a plausible BPB, including a bad EBR chain. */
    for(unsigned layout=0;layout<3;layout++) {
        memcpy(bad,bpb,512); memset(bad+446,0,64); bad[450]=layout==2 ? 5 : layout==1 ? 0xee : 0x83;
        fs_wr32(bad+454,1); fs_wr32(bad+458,(uint32_t)f->dev.capacity-1);
        OK(fake_raw_write(f,0,bad)); struct storage *s=start();
        CHECK(storage_add_disk(s,1,&f->dev)==(layout==2 ? -FS_EINVAL : layout==1 ? -FS_EOPNOTSUPP : 0));
        CHECK(!storage_volume(s,3) && !f->writes && !f->flushes);
        stop(s,true); fake_reset(f); reset();
    }
    /* An issued read error must remain an error, with no fallback retry. */
    f->fail_read=0; unsigned reads=f->reads; struct storage *s=start();
    CHECK(storage_add_disk(s,1,&f->dev)==-FS_EIO && f->reads==reads+1);
    CHECK(!storage_volume(s,3)); stop(s,true); fake_reset(f); reset();
    printf("PASS superfloppy: FAT12/16 whole-disk RO mounts, boot MBR rule, invalid BPB/capacity/blank disk, no I/O retry\n");
}
static void mount_tests(void) {
    struct storage *s=start(); struct storage_volume *v=storage_volume(s,2);
    CHECK(v && v->fat.type==32 && v->fat.readonly && v->read_gate && v->read_sequence && !v->writes && !media[0].writes && !media[0].flushes);
    CHECK(v->fat.ro_reasons==FAT_RO_REQUEST);
    struct fat_entry directory;
    CHECK(vfs_stat(&table,"C:/SYSTEM/LOGS",&directory)==-FS_ENOENT && !media[0].writes);
    uint8_t sector[512]; OK(cache_read(&s->cache,&v->io,0,sector)); unsigned reads=media[0].reads;
    disks[0].dev.quarantined=true;
    CHECK(cache_read(&s->cache,&v->io,0,sector)==-FS_EQUARANTINED && media[0].reads==reads);
    CHECK(storage_enable_write(s,2)==-FS_EIO && !media[0].writes);
    stop(s,false); reset();
    s=start(); v=storage_volume(s,2);
    disks[0].dev.flush=0;
    CHECK(storage_enable_write(s,2)==-FS_EROFS && !media[0].writes && v->fat.readonly);
    disks[0].dev.write_cache_state=BLKDEV_CACHE_DISABLED;
    OK(storage_enable_write(s,2)); CHECK(!v->fat.readonly && v->write_sequence>v->read_sequence);
    OK(vfs_stat(&table,"C:/SYSTEM/LOGS",&directory)); CHECK(directory.attr&FAT_ATTR_DIR);
    CHECK(media[0].writes && !media[0].flushes);
    unsigned directory_writes=media[0].writes;
    OK(storage_enable_write(s,2)); CHECK(media[0].writes==directory_writes);
    uint32_t before=s->writer_ticks; OK(storage_writeback(s,fs_now_ms())); CHECK(s->writer_ticks==before+1);
    stop(s,true); reset();
    s=start(); v=storage_volume(s,2); OK(storage_enable_write(s,2));
    uint32_t age=fs_now_ms(); memset(sector,0x71,512); OK(cache_write(&s->cache,&v->io,2,sector));
    unsigned writes=media[0].writes;
    OK(storage_writeback(s,age+4999)); CHECK(media[0].writes==writes);
    OK(storage_writeback(s,age+5001)); CHECK(media[0].writes==writes+1);
    stop(s,true); reset();
    /* Dirty-only opens the gate; divergence and bad BPB preserve zero writes. */
    for(unsigned fault=0;fault<3;fault++) {
        uint8_t bpb[512]; OK(fake_raw_read(&media[0],0,bpb));
        uint32_t reserved=fs_rd16(bpb+14), fat=fs_rd32(bpb+36);
        if(fault==2) { bpb[13]=3; OK(fake_raw_write(&media[0],0,bpb)); }
        else for(unsigned f=0;f<(fault ? 1u : 2u);f++) {
            OK(fake_raw_read(&media[0],reserved+f*fat,sector));
            fs_wr32(sector+4,fs_rd32(sector+4)&~0x08000000u); OK(fake_raw_write(&media[0],reserved+f*fat,sector));
        }
        s=start(); v=storage_volume(s,2);
        CHECK(v && (fault==2 ? v->error!=0 : v->fat.readonly));
        if(!fault) { CHECK(v->read_gate); OK(storage_enable_write(s,2)); CHECK(v->fat.dirty_recovered && !v->fat.readonly); }
        else CHECK(storage_enable_write(s,2)<0 && !media[0].writes);
        if(fault==1) CHECK(v->fat.ro_reasons&FAT_RO_COPIES);
        stop(s,false); reset();
    }
    s=start(); OK(storage_enable_write(s,2)); OK(vfs_rmdir(&table,"C:/SYSTEM/LOGS"));
    int h=vfs_open(&table,"C:/SYSTEM/LOGS",VFS_WRITE|VFS_CREATE,VFS_DENY_NONE,0); CHECK(h>=0); OK(vfs_close(&table,h));
    CHECK(storage_enable_write(s,2)==-FS_ENOTDIR);
    CHECK(bootlog_activate(&s->vfs,true,s->sequence)==-FS_ENOTDIR);
    CHECK(bootlog_shutdown()==-FS_ENOTDIR); stop(s,true); reset();
    printf("PASS storage mounts: RO-first, gate/durability, refreshed quarantine/cache hits, dirty/divergent/BPB, writer hook\n");
}
static void patch_entry(struct fake *f, const struct fat_volume *g, unsigned copy, uint32_t cluster, uint32_t value) {
    uint32_t off=cluster*(g->type/8), sec=g->reserved+copy*g->fat_sectors+off/512;
    uint8_t b[512]; OK(fake_raw_read(f,sec,b));
    if(g->type==32) fs_wr32(b+off%512,value); else fs_wr16(b+off%512,(uint16_t)value);
    OK(fake_raw_write(f,sec,b));
}
static struct storage *recovery_start(unsigned i, struct storage_volume **v) {
    struct storage *s=start();
    if(i) { disk_init(i); OK(storage_add_disk(s,1,&disks[i].dev)); }
    *v=storage_volume(s,i ? 3 : 2); CHECK(*v && !(*v)->error); return s;
}
static void dirty_recovery_tests(void) {
    for(unsigned i=0;i<3;i+=2) {
        struct fake *f=&media[i]; f->undo_enabled=true;
        struct storage_volume *v; struct storage *s=recovery_start(i,&v);
        struct fat_volume g=v->fat; unsigned d=v->drive; stop(s,true); reset();
        uint32_t bit=g.type==32 ? 0x08000000u : 0x8000u;
        uint32_t clean=g.type==32 ? 0xafffffffu : 0xffffu; /* reserved upper bits survive */
        for(unsigned fault=0;fault<5;fault++) {
            uint32_t one=fault==4 ? clean&~(bit>>1) : clean&~bit;
            if(fault==3) one&=~(bit>>1);
            for(unsigned copy=0;copy<2;copy++) patch_entry(f,&g,copy,1,fault==2 && copy ? clean : one);
            if(fault==1) for(unsigned copy=0;copy<2;copy++) patch_entry(f,&g,copy,g.next_free,0x0fffffff);
            s=recovery_start(i,&v);
            CHECK(v->fat.readonly && !v->fat.dirty_recovered && !v->writes && !f->flushes && !v->writes_before_gate);
            if(!fault) {
                CHECK(v->read_gate); f->reorder=true;
                OK(storage_enable_write(s,d));
                CHECK(v->fat.dirty_recovered && !v->fat.readonly && !v->fat.ro_reasons && !v->refused);
                CHECK(v->write_sequence>v->read_sequence && !v->writes_before_gate);
                /* Recovery sets clean, then writable-session setup clears it.
                 * Each copy persists and flushes before the next is issued. */
                for(unsigned phase=0;phase<2;phase++) for(unsigned copy=0;copy<2;copy++) {
                    unsigned j=phase*6+copy*3;
                    CHECK(f->trace[j].kind=='W' && f->trace[j].lba==g.reserved+copy*g.fat_sectors);
                    CHECK(f->trace[j+1].kind=='P' && f->trace[j+1].lba==f->trace[j].lba);
                    CHECK(f->trace[j+2].kind=='B');
                    uint8_t b[512]; OK(fake_raw_read(f,f->trace[j].lba,b));
                    CHECK((g.type==32 ? fs_rd32(b+4) : fs_rd16(b+2))==(clean&~bit));
                }
                stop(s,true);
                for(unsigned copy=0;copy<2;copy++) {
                    uint8_t b[512]; OK(fake_raw_read(f,g.reserved+copy*g.fat_sectors,b));
                    CHECK((g.type==32 ? fs_rd32(b+4) : fs_rd16(b+2))==clean);
                    struct scan_result scan=independent_scan(f->fd,copy);
                    CHECK(!scan.dirty && !scan.divergent && !scan.lost && !scan.crosslinks && !scan.corrupt);
                }
                f->reorder=false;
            } else {
                CHECK(!v->read_gate && storage_enable_write(s,d)==-FS_EROFS && !f->writes && !f->flushes);
                CHECK(v->fat.ro_reasons&(fault==1 ? FAT_RO_LOST : fault==2 ? FAT_RO_COPIES : FAT_RO_IO_FLAG));
                CHECK(!v->fat.dirty_recovered); stop(s,true);
            }
            fake_reset(f); reset();
        }
        /* Fail at every issuance/persistence/flush in recovery. Loss of the
         * first copy before its barrier leaves a consistent dirty volume;
         * a cut after its persistence leaves divergence and MUST be refused. */
        for(unsigned cut=1;cut<=6;cut++) {
            for(unsigned copy=0;copy<2;copy++) patch_entry(f,&g,copy,1,clean&~bit);
            s=recovery_start(i,&v); f->reorder=true; f->cut_at=cut;
            CHECK(storage_enable_write(s,d)==-FS_EIO && f->cut && v->fat.readonly && !v->fat.dirty_recovered);
            CHECK(v->fat.ro_reasons&FAT_RO_WRITE_ERROR); stop(s,false); fake_power_loss(f);
            unsigned writes=f->writes;
            s=recovery_start(i,&v);
            if(cut>=2 && cut<=4) {
                CHECK(v->fat.ro_reasons&FAT_RO_COPIES);
                CHECK(!v->read_gate && storage_enable_write(s,d)==-FS_EROFS && f->writes==writes && !v->fat.dirty_recovered);
            } else {
                CHECK(v->read_gate); OK(storage_enable_write(s,d));
                CHECK(!v->fat.readonly && v->fat.dirty_recovered==(cut==1));
            }
            stop(s,true); f->reorder=false; fake_reset(f); reset();
        }
        printf("PASS FAT%u dirty recovery: RO-first/gate, clean scan, lost/divergence/error refusal, reserved bits, per-copy W/P/B, six recovery cuts\n",g.type);
    }
}
static void log_tests(void) {
    struct storage *s=start(); struct bootlog_stats st;
    bootlog_capture("activation before mount\n",24); bootlog_snapshot(&st); CHECK(!st.writes && !st.storage_calls && st.queued==24);
    CHECK(bootlog_activate(&s->vfs,false,0)==-FS_EROFS); CHECK(!media[0].writes);
    OK(storage_enable_write(s,2)); OK(bootlog_activate(&s->vfs,true,s->sequence));
    CHECK(BOOTLOG_LIMIT==131072 && !strcmp(BOOTLOG_PATH,"C:/SYSTEM/LOGS/BOOT.LOG"));
    char text[4096]; memset(text,'a',sizeof(text));
    OK(bootlog_drain());
    for(unsigned i=0;i<BOOTLOG_LIMIT/sizeof(text)-1;i++) { bootlog_capture(text,sizeof(text)); OK(bootlog_drain()); }
    bootlog_capture(text,sizeof(text)-24); OK(bootlog_drain());
    bootlog_snapshot(&st); CHECK(st.active && st.size==BOOTLOG_LIMIT && !st.rotations && st.first_write_sequence>st.qualification_sequence);
    bootlog_capture("z",1); OK(bootlog_drain());
    bootlog_snapshot(&st); CHECK(st.size==1 && st.rotations==1);
    OK(bootlog_shutdown()); uint8_t digest[32],again[32]; uint32_t size,size2;
    OK(storage_file_digest(&table,BOOTLOG_PATH,digest,&size)); CHECK(size<=BOOTLOG_LIMIT && size);
    stop(s,true); bootlog_reset();
    unsigned writes=media[0].writes; s=start(); OK(storage_file_digest(&table,BOOTLOG_PATH,again,&size2));
    CHECK(size==size2 && !memcmp(digest,again,32) && writes==media[0].writes);
    bootlog_capture("next boot\n",10); OK(storage_enable_write(s,2)); OK(bootlog_activate(&s->vfs,true,s->sequence));
    OK(bootlog_shutdown()); struct fat_entry stat; OK(vfs_stat(&table,BOOTLOG_PATH,&stat)); CHECK(stat.size==size+10);
    stop(s,true); reset();
    /* Bounded RAM capture and unavailable path never call a filesystem. */
    for(unsigned i=0;i<5;i++) bootlog_capture(text,sizeof(text));
    bootlog_snapshot(&st); CHECK(st.queued==BOOTLOG_RAM && st.dropped==3*sizeof(text) && !st.storage_calls);
    OK(bootlog_shutdown()); bootlog_reset();
    printf("PASS boot log: SYSTEM/LOGS after gate, 128KiB exact boundary/truncation, bounded RAM, durable reopen/append\n");
}
static void failure_tests(void) {
    for(unsigned fault=0;fault<2;fault++) {
        struct storage *s=start(); struct storage_volume *v=storage_volume(s,2); OK(storage_enable_write(s,2));
        uint8_t buf[512]={0}; OK(cache_write(&s->cache,&v->io,2,buf));
        if(fault) media[0].fail_flush=true; else media[0].fail_write=2;
        CHECK(storage_writeback(s,fs_now_ms()+5001)==-FS_EIO);
        CHECK(v->fat.readonly && v->fat.ro_reasons&FAT_RO_WRITE_ERROR);
        vfs_table_destroy(&table); CHECK(storage_shutdown(s)==-FS_EIO && v->fat.mounted);
        OK(fake_raw_read(&media[0],v->fat.reserved,buf)); CHECK(!(fs_rd32(buf+4)&0x08000000));
        storage_destroy(s); reset();
    }
    struct storage *s=start(); OK(storage_enable_write(s,2)); OK(bootlog_activate(&s->vfs,true,s->sequence));
    bootlog_capture("failure\n",8); media[0].fail_flush=true; CHECK(bootlog_drain()==-FS_EIO);
    struct bootlog_stats st; bootlog_snapshot(&st); CHECK(!st.active && st.error==-FS_EIO);
    uint64_t calls=st.storage_calls; CHECK(bootlog_drain()==-FS_EIO); bootlog_snapshot(&st); CHECK(st.storage_calls==calls);
    CHECK(bootlog_shutdown()==-FS_EIO); stop(s,false); reset();
    printf("PASS delayed write/flush errors: sticky caller result, RO revocation, no clean unmount; logger no recursion\n");
}
int probe_fat_read(void);
int probe_fat_write(void);
int probe_cache(void);
int probe_bootlog(void);
int probe_mount_crash(void);
static void mount_probe_tests(void) {
    /* A dirty FAT16 fixture on a nonboot slot recovers after the read gate. */
    struct fake *f=&media[2]; f->undo_enabled=true;
    uint8_t bpb[512],sector[512]; OK(fake_raw_read(f,0,bpb));
    uint32_t reserved=fs_rd16(bpb+14), fat=fs_rd16(bpb+22);
    for(unsigned layout=0;layout<2;layout++) {
        for(unsigned i=0;i<2;i++) {
            OK(fake_raw_read(f,reserved+i*fat,sector)); fs_wr16(sector+2,fs_rd16(sector+2)&~0x8000u);
            OK(fake_raw_write(f,reserved+i*fat,sector));
        }
        struct storage *s=start(); disk_init(2);
        OK(storage_add_disk(s,1,layout ? &f->dev : &disks[2].dev));
        CHECK(s->volumes[3].read_gate && s->volumes[3].fat.ro_reasons&FAT_RO_DIRTY);
        host_mount_snapshot(s);
        OK(probe_mount_crash()); CHECK(f->writes && !media[0].writes && s->volumes[3].fat.dirty_recovered);
        stop(s,true); fake_reset(f); reset();
    }
    fake_reset(f);
    /* Driver failure stands in for termination at an issued sector, followed
     * by loss of all caches. Actual QEMU process termination remains T3. */
    struct storage *s=start(); OK(storage_enable_write(s,2));
    int h=vfs_open(&table,"C:/F109CUT.ARM",VFS_WRITE|VFS_CREATE|VFS_EXCLUSIVE,0,0); CHECK(h>=0); OK(vfs_close(&table,h));
    stop(s,true);
    s=start(); cut_probe_after_arm=true;
    CHECK(probe_mount_crash()!=0 && media[0].cut);
    cut_probe_after_arm=false;
    stop(s,false); fake_power_loss(&media[0]);
    for(unsigned i=0;i<2;i++) { struct scan_result scan=independent_scan(media[0].fd,i); CHECK(!scan.crosslinks && !scan.corrupt); }
    unsigned writes=media[0].writes;
    s=start(); OK(probe_mount_crash()); CHECK(media[0].writes>writes && s->volumes[2].fat.dirty_recovered);
    stop(s,true); reset();
    printf("PASS mount-crash probe: dirty fixture recovery, ARM/write cut trace, cache-loss reboot recovery, independent copies crosslinks=0\n");
}
static void cfg_tests(void) {
    extern int storage_boot_cfg(struct storage *, const char *);
    struct storage *s=start(); OK(storage_enable_write(s,2));
    const char original[]="safe=0 serial=1\nmode=0x0118\nprobe=f0:sweep run=12345678\n";
    int h=vfs_open(&table,"C:/SYSTEM/BOOT.CFG",VFS_WRITE|VFS_CREATE,VFS_DENY_NONE,0); CHECK(h>=0);
    size_t done; OK(vfs_write(&table,h,original,sizeof(original)-1,&done)); CHECK(done==sizeof(original)-1);
    OK(vfs_close(&table,h)); vfs_table_destroy(&table); OK(storage_shutdown(s));
    const char request[]="f1:sweep run=12345678 step=10 state=00001009";
    OK(storage_boot_cfg(s,request)); CHECK(s->stopped && !s->volumes[2].fat.mounted);
    storage_destroy(s); s=start();
    char bytes[128]={0};
    h=vfs_open(&table,"C:/SYSTEM/BOOT.CFG",VFS_READ,VFS_DENY_NONE,0); CHECK(h>=0);
    OK(vfs_read(&table,h,bytes,127,&done)); OK(vfs_close(&table,h));
    CHECK(strstr(bytes,request) && strstr(bytes,"safe=0 serial=1\nmode=0x0118\n"));
    CHECK(s->volumes[2].fat.readonly);
    unsigned flushes=media[0].flushes;
    OK(storage_boot_cfg(s,NULL)); CHECK(media[0].flushes>flushes && s->volumes[2].fat.readonly);
    memset(bytes,0,sizeof(bytes)); h=vfs_open(&table,"C:/SYSTEM/BOOT.CFG",VFS_READ,VFS_DENY_NONE,0); CHECK(h>=0);
    OK(vfs_read(&table,h,bytes,127,&done)); OK(vfs_close(&table,h));
    CHECK(!strstr(bytes,"probe=") && strstr(bytes,"mode=0x0118"));
    unsigned writes=media[0].writes; disks[0].dev.quarantined=true;
    CHECK(storage_boot_cfg(s,request)==-FS_EROFS); CHECK(media[0].writes==writes);
    disks[0].dev.quarantined=false; stop(s,true); reset();
    printf("PASS BOOT.CFG: durable cursor round trip after shutdown, preserve options, remove selector, closed/read-only gate\n");
}
static void probe_tests(void) {
    struct storage *s=start();
    for(unsigned i=1;i<3;i++) OK(storage_add_disk(s,i,&media[i].dev));
    host_mount_snapshot(s);
    CHECK(s->volumes[3].fat.type==12 && s->volumes[4].fat.type==16);
    fixture_present=fixture_absent=fat_read_not_run=fat_read_terminal_not_run=0;
    OK(probe_fat_read()); CHECK(fixture_present==6 && fixture_absent==8);
    CHECK(!fat_read_not_run && !fat_read_terminal_not_run);
    CHECK(!media[0].writes && !media[1].writes && !media[2].writes);
    stop(s,true); reset();
    s=start(); fixture_present=fixture_absent=fat_read_not_run=fat_read_terminal_not_run=0;
    OK(probe_fat_read()); CHECK(!fixture_present && fixture_absent==14);
    CHECK(fat_read_not_run==1 && fat_read_terminal_not_run==1);
    CHECK(!media[0].writes); stop(s,true); reset();
    s=start(); bool reboot=true; OK(storage_write_workload(s,&table,&reboot)); CHECK(!reboot);
    stop(s,true); unsigned writes=media[0].writes;
    s=start(); OK(storage_write_workload(s,&table,&reboot)); CHECK(reboot && media[0].writes==writes);
    stop(s,true); reset();
    s=start(); vfs_table_destroy(&table); OK(probe_cache()); storage_destroy(s); reset();
    s=start(); vfs_table_destroy(&table); OK(probe_bootlog()); storage_destroy(s); bootlog_reset();
    writes=media[0].writes; s=start(); vfs_table_destroy(&table); OK(probe_bootlog()); CHECK(media[0].writes==writes);
    storage_destroy(s); reset();
    printf("PASS production FAT probes: FAT12/16/32 listings/hashes, 4MiB lifecycle, aliases/dir growth, cold reopen, cache, bootlog\n");
}
int main(int argc,char **argv) {
    CHECK(argc==4);
    for(unsigned i=0;i<3;i++) OK(fake_open(&media[i],argv[i+1]));
    baseline(); superfloppy_tests(); mount_tests(); dirty_recovery_tests(); log_tests(); failure_tests(); cfg_tests(); probe_tests(); mount_probe_tests();
    for(unsigned i=0;i<3;i++) fake_close(&media[i]);
    printf("STORAGE RESULT checks=%u records=%u max_record=%u failures=0 ASan/UBSan=enabled\n",checks,records,max_record);
    return 0;
}
