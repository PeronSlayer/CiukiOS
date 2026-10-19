/* Production FAT, boot logger and DOS disk service over interrupted media.
 * No guest execution: the DOS service opens its base/overlay and loads program
 * sectors through the real VFS/cache. SPDX-License-Identifier: GPL-2.0-only */
#define main storage_fixture_main
#include "test_storage.c"
#undef main
#undef CLOCKS_PER_SEC
#undef WIFEXITED
#undef WEXITSTATUS
#undef WIFSIGNALED
#undef WTERMSIG
#define CIUKI_CPU_H
static uint32_t irq_save(void) { return 0x200; }
static void irq_restore(uint32_t flags) { (void)flags; }
#include <ciuki/task.h>
#include <ciuki/sync.h>
static struct task disk_caller;
struct task *g_current = &disk_caller;
uint64_t deadline_after_ms(uint32_t ms) { return fs_now_ms()+ms; }
bool kwait_wait_until(struct kwait *q, kwait_cond_fn cond, void *ctx, uint64_t end)
{ (void)q; (void)end; CHECK(cond(ctx)); return true; }
void kwait_wake_all(struct kwait *q) { (void)q; }
void task_sleep_ms(uint32_t ms) { (void)ms; CHECK(false); }
#include "../../../src/kernel/vm/dosvm_disk.c"
static struct dosvm vm;
void kpreempt_point(void) { CHECK(!vm.disk_busy); }
static const uint8_t program[]={0xb8,0x00,0x4c,0xcd,0x21};

static void readable_tree(struct fat_volume *v, uint32_t dir, unsigned depth)
{
    CHECK(depth<8);
    struct fat_entry entry;uint32_t cursor=0;int e;
    while (!(e=fat_next(v,dir,&cursor,&entry))) {
        if (!strcmp(entry.alias,".") || !strcmp(entry.alias,"..")) continue;
        if (entry.attr&FAT_ATTR_DIR) readable_tree(v,entry.first,depth+1);
        else for (uint32_t pos=0;pos<entry.size;) {
            uint8_t bytes[4096];size_t done;
            OK(fat_read(v,&entry,pos,bytes,sizeof(bytes),&done));CHECK(done);pos+=(uint32_t)done;
        }
    }
    CHECK(e==-FS_ENOENT && !v->readonly);
}
static void seed_workload(void)
{
    struct storage *s=start();OK(storage_enable_write(s,2));
    OK(vfs_mkdir(&table,"C:/SYSTEM/DOS",0));OK(vfs_mkdir(&table,"C:/GAMES",0));
    int h=vfs_open(&table,DOSVM_BUNDLED_BASE,VFS_WRITE|VFS_CREATE,0,0);CHECK(h>=0);
    OK(vfs_truncate(&table,h,DOSVM_BASE_MIN_SECTORS*512));
    uint64_t at;size_t done;
    OK(vfs_seek(&table,h,128*512,VFS_SEEK_SET,&at));
    OK(vfs_write(&table,h,program,sizeof(program),&done));CHECK(done==sizeof(program));
    OK(vfs_close(&table,h));stop(s,true);bootlog_reset();
}
static void ordinary_operations(struct storage *s)
{
    readable_tree(&s->volumes[2].fat,s->volumes[2].fat.root,0);
    int h=vfs_open(&table,"C:/SYSTEM/SETTINGS.CFG",VFS_WRITE|VFS_READ|VFS_CREATE,0,0);CHECK(h>=0);
    size_t done;OK(vfs_write(&table,h,"volume=17\n",10,&done));CHECK(done==10);OK(vfs_close(&table,h));
    char path[24];OK(dosvm_window_base(path));CHECK(!strcmp(path,DOSVM_BUNDLED_BASE));
    memset(&vm,0,sizeof(vm));vm.info.key.generation=1;
    OK(dosvm_disk_open(&vm,path));uint8_t sector[512];
    OK(dosvm_disk_read(&vm,128,sector));CHECK(!memcmp(sector,program,sizeof(program)));
    memset(sector,0x53,sizeof(sector));OK(dosvm_disk_write(&vm,300,sector));
    memset(sector,0,sizeof(sector));OK(dosvm_disk_read(&vm,300,sector));CHECK(sector[0]==0x53);
    OK(dosvm_disk_close(&vm,false));CHECK(!s->volumes[2].fat.readonly);
}
static void crash_close(struct storage *s)
{
    /* Volatile state vanishes without finalizing the file's tail. Any cleanup
     * I/O is refused before issuance; no metadata reaches durable storage. */
    media[0].cut=true;
    bootlog_capture("lost RAM\n",9);CHECK(bootlog_shutdown()==-FS_EIO);
    stop(s,false);fake_power_loss(&media[0]);bootlog_reset();
}
static void recovery_cases(void)
{
    for (unsigned mode=0;mode<4;mode++) {
        seed_workload();
        struct storage *s=start();OK(storage_enable_write(s,2));
        OK(bootlog_activate(&s->vfs,true,s->sequence));
        if (mode) {
            bootlog_capture("crash boot\n",11);OK(bootlog_drain());
            if (mode>1) OK(bootlog_sync(log_no_cancel,log_storage_turn,NULL));
        }
        if (mode>1) {
            char block[BOOTLOG_BATCH];memset(block,'X',sizeof(block));
            bootlog_capture_ring(block,sizeof(block));CHECK(bootlog_try_sync()==-FS_EAGAIN);
            struct fat_entry log;OK(vfs_stat(&table,BOOTLOG_PATH,&log));
            uint32_t c=log.first;
            for (unsigned i=0;i<BOOTLOG_PROTECTED/(s->volumes[2].fat.spc*512);i++)
                OK(fat_get_cluster(&s->volumes[2].fat,c,&c));
            if (mode==2) media[0].cut_at=media[0].events+17;
            else { media[0].torn_lba=s->volumes[2].fat.data_sector+(c-2)*s->volumes[2].fat.spc+7;media[0].torn_bytes=173; }
            CHECK(bootlog_drain()==-FS_EIO);
        }
        crash_close(s);
        s=start();CHECK(s->volumes[2].read_gate && !(s->volumes[2].fat.ro_reasons&FAT_RO_CORRUPT));
        OK(storage_enable_write(s,2));ordinary_operations(s);
        bootlog_capture("recovered boot\n",15);OK(bootlog_activate(&s->vfs,true,s->sequence));
        OK(bootlog_shutdown());struct fat_entry log;OK(vfs_stat(&table,BOOTLOG_PREVIOUS,&log));
        CHECK(mode ? log.size>=11 : !log.size);
        if (mode==1) CHECK(log.size==11); /* unpublished prefix is recovered */
        if (mode>1) CHECK(log.size>BOOTLOG_PROTECTED && log.size<=BOOTLOG_PROTECTED+BOOTLOG_BATCH);
        if (!log.size) CHECK(!log.first); /* zero-length reservations are released */
        else { uint8_t text[32];size_t done;OK(fat_read(&s->volumes[2].fat,&log,0,text,sizeof(text),&done));CHECK(done>=11 && !memcmp(text,"crash boot\n",11)); }
        ordinary_operations(s);stop(s,true);bootlog_reset();
        for (unsigned copy=0;copy<2;copy++) {
            struct scan_result scan=independent_scan(media[0].fd,copy);
            CHECK(!scan.lost && !scan.crosslinks && !scan.corrupt && !scan.divergent && !scan.dirty);
        }
        reset();
    }
    puts("PASS log power loss: zero EOF/full reserved tail, unpublished 32KiB, partial 32KiB/torn data sector; writable cold mount, all files readable, settings save, DOS base/program/overlay I/O, clean ownership scans");
}
static void rotation_cases(void)
{
    const char *paths[]={BOOTLOG_PATH,BOOTLOG_PREVIOUS,BOOTLOG_OLDEST};
    for (unsigned boot=1;boot<=4;boot++) {
        struct storage *s=start();OK(storage_enable_write(s,2));
        char text[32];int length=snprintf(text,sizeof(text),"boot=%u\n",boot);
        bootlog_capture(text,(size_t)length);OK(bootlog_activate(&s->vfs,true,s->sequence));
        OK(bootlog_sync(log_no_cancel,log_storage_turn,NULL));
        char block[BOOTLOG_BATCH];memset(block,'0'+boot,sizeof(block));
        for (unsigned n=0;n<BOOTLOG_RING/BOOTLOG_BATCH;n++) {
            bootlog_capture_ring(block,sizeof(block));
            OK(bootlog_sync(log_no_cancel,log_storage_turn,NULL));
        }
        uint32_t used=s->volumes[2].fat.clusters-s->volumes[2].fat.free_clusters;
        CHECK((uint64_t)used*s->volumes[2].fat.spc*512<BOOTLOG_BUDGET+65536);
        OK(bootlog_shutdown());
        for (unsigned age=0;age<3 && age<boot;age++) {
            int h=vfs_open(&table,paths[age],VFS_READ,0,0);CHECK(h>=0);
            uint8_t actual[32];size_t done;OK(vfs_read(&table,h,actual,sizeof(actual),&done));
            length=snprintf(text,sizeof(text),"boot=%u\n",boot-age);
            CHECK(done>=(size_t)length && !memcmp(actual,text,(size_t)length));OK(vfs_close(&table,h));
        }
        if (boot==4) {
            int held=vfs_open(&table,BOOTLOG_PREVIOUS,VFS_READ,0,0);CHECK(held>=0);
            unsigned writes=media[0].writes;
            CHECK(vfs_log_rotate(&table,paths[0],paths[1],paths[2],BOOTLOG_LIMIT)==-FS_EBUSY);
            CHECK(media[0].writes==writes);OK(vfs_close(&table,held));
        }
        stop(s,true);bootlog_reset();
    }
    reset();puts("PASS log rotation: four cold boots retain BOOT/BOOT1/BOOT2 in order within the original 1MiB budget");
}
static void reservation_cases(void)
{
    struct storage *s=start();OK(storage_enable_write(s,2));
    int h=vfs_open(&table,BOOTLOG_PATH,VFS_READ|VFS_WRITE|VFS_CREATE,VFS_DENY_WRITE,0);CHECK(h>=0);
    OK(vfs_log_reserve(&table,h,BOOTLOG_LIMIT));
    OK(vfs_close(&table,h));stop(s,false);fake_power_loss(&media[0]);bootlog_reset();
    s=start();OK(storage_enable_write(s,2));
    struct fat_entry before,after;OK(vfs_stat(&table,BOOTLOG_PATH,&before));CHECK(!before.size && !before.first); /* mount upgrade reclaimed the crash tail */
    uint32_t free=s->volumes[2].fat.free_clusters;
    h=vfs_open(&table,BOOTLOG_PATH,VFS_WRITE,VFS_DENY_WRITE,0);CHECK(h>=0);
    OK(vfs_truncate(&table,h,0));OK(vfs_close(&table,h));
    OK(vfs_stat(&table,BOOTLOG_PATH,&after));CHECK(!after.size && !after.first);
    CHECK(s->volumes[2].fat.free_clusters==free);
    /* Transition from the previous 1MiB reservation, without file COW. */
    h=vfs_open(&table,BOOTLOG_PATH,VFS_READ|VFS_WRITE,VFS_DENY_WRITE,0);CHECK(h>=0);
    OK(vfs_truncate(&table,h,BOOTLOG_BUDGET));OK(vfs_close(&table,h));
    uint32_t oldfree=s->volumes[2].fat.free_clusters;
    OK(bootlog_activate(&s->vfs,true,s->sequence));
    OK(vfs_stat(&table,BOOTLOG_PREVIOUS,&after));CHECK(after.size==BOOTLOG_LIMIT);
    CHECK(s->volumes[2].fat.free_clusters>oldfree);OK(bootlog_shutdown());
    stop(s,true);reset();
    puts("PASS reservation recovery: zero-to-zero truncate releases crash chain; imported 1MiB log trimmed in place before new reservation; busy history preflight performs zero writes");
}
static void diagnostic_cases(void)
{
    struct storage *s=start();
    char text[80];CHECK(!storage_readonly_info(2,text,sizeof(text)));
    disks[0].dev.write_cache_state=BLKDEV_CACHE_UNKNOWN;
    CHECK(storage_enable_write(s,2)==-FS_EROFS);
    CHECK(s->volumes[2].fat.readonly_reason && !strcmp(s->volumes[2].fat.readonly_reason,"durability"));
    disks[0].dev.write_cache_state=BLKDEV_CACHE_ENABLED;
    OK(storage_enable_write(s,2));
    CHECK(!s->volumes[2].fat.readonly && !s->volumes[2].fat.readonly_reason);
    struct fat_volume *v=&s->volumes[2].fat;
    struct fat_entry file;OK(vfs_stat(&table,"C:/SHORT.BIN",&file));
    file.first=v->clusters+2;uint8_t bytes[512];size_t done;
    readonly_reports=0;
    CHECK(fat_read(v,&file,0,bytes,sizeof(bytes),&done)==-FS_EUCLEAN);
    CHECK(readonly_reports==1 && strstr(readonly_record,"reason=chain_bounds file=SHORT.BIN cluster="));
    uint32_t first=v->readonly_cluster;
    file.first++;CHECK(fat_read(v,&file,0,bytes,sizeof(bytes),&done)==-FS_EUCLEAN);
    CHECK(readonly_reports==1 && v->readonly_cluster==first);
    CHECK(storage_readonly_info(2,text,sizeof(text)));
    CHECK(strstr(text,"chain_bounds") && strstr(text,"file=SHORT.BIN") && strstr(text,"cluster="));
    CHECK(fat_write(v,&file,0,bytes,sizeof(bytes),&done)==-FS_EROFS && readonly_reports==1);
    stop(s,false);reset();
    s=start();OK(storage_enable_write(s,2));v=&s->volumes[2].fat;
    OK(vfs_stat(&table,"C:/SHORT.BIN",&file));
    disks[0].dev.quarantined=true;readonly_reports=0;
    CHECK(fat_read(v,&file,0,bytes,sizeof(bytes),&done)==-FS_EIO);
    CHECK(readonly_reports==1 && !strcmp(v->readonly_reason,"device_quarantined"));
    CHECK(storage_readonly_info(2,text,sizeof(text)) && strstr(text,"device_quarantined"));
    stop(s,false);reset();
    puts("PASS readonly cause: one reason/file/cluster record, first cause retained after repeated failures, live UI snapshot and writes refused");
}
#ifndef LOG_RECOVERY_MAIN
#define LOG_RECOVERY_MAIN main
#endif
int LOG_RECOVERY_MAIN(int argc,char **argv)
{
    CHECK(argc==4);for (unsigned i=0;i<3;i++) OK(fake_open(&media[i],argv[i+1]));
    baseline();recovery_cases();rotation_cases();reservation_cases();diagnostic_cases();
    for (unsigned i=0;i<3;i++) fake_close(&media[i]);
    printf("LOG RECOVERY RESULT checks=%u failures=0 ASan/UBSan=enabled\n",checks);return 0;
}
