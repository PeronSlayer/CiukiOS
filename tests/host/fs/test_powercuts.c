/* F0-25: cut every device event, using production log/settings/DOS paths. */
#define LOG_RECOVERY_MAIN previous_log_fixture_main
#include "test_log_recovery.c"
#undef LOG_RECOVERY_MAIN
#include <ciuki/blackbox.h>
static void interrupted_turn(void *unused) { (void)unused;(void)bootlog_drain(); }
static int interrupted_work(struct storage *s,unsigned phase)
{
    int e;size_t done;
    if (phase==0) return bootlog_activate(&s->vfs,true,s->sequence);
    if (phase==1) {
        bootlog_capture("published boot\n",15);
        e=bootlog_drain();if (e) return e;
        return bootlog_sync(log_no_cancel,interrupted_turn,NULL);
    }
    if (phase==2) {
        char block[BOOTLOG_BATCH];memset(block,'R',sizeof(block));
        bootlog_capture_ring(block,sizeof(block));
        return bootlog_sync(log_no_cancel,interrupted_turn,NULL);
    }
    if (phase==3) return bootlog_shutdown();
    if (phase==4) return vfs_log_rotate(&table,BOOTLOG_PATH,BOOTLOG_PREVIOUS,BOOTLOG_OLDEST,BOOTLOG_LIMIT);
    int h=vfs_open(&table,"C:/SYSTEM/SETTINGS.CFG",VFS_READ|VFS_WRITE,0,0);
    if (h<0) return h;
    if (phase>=6) {
        uint64_t position;
        e=vfs_seek(&table,h,phase==6 ? 0 : 2001,phase==6 ? VFS_SEEK_END : VFS_SEEK_SET,&position);
        if (e) return e;
    }
    char settings[1537];memset(settings,'N',sizeof(settings));
    e=vfs_write(&table,h,settings,sizeof(settings),&done);
    int close=vfs_close(&table,h);return e ? e : close;
}
static void seed(unsigned phase)
{
    seed_workload();struct storage *s=start();OK(storage_enable_write(s,2));
    int h=vfs_open(&table,"C:/SYSTEM/SETTINGS.CFG",VFS_WRITE|VFS_CREATE,0,0);CHECK(h>=0);
    size_t done;OK(vfs_write(&table,h,"old settings\n",13,&done));OK(vfs_close(&table,h));
    if (phase) {
        OK(bootlog_activate(&s->vfs,true,s->sequence));
        bootlog_capture("published boot\n",15);OK(bootlog_sync(log_no_cancel,log_storage_turn,NULL));
        if (phase>=4) OK(bootlog_shutdown());
    }
    /* Freeze the durable fixture and restore it with sector undo per trial.
     * No image copies and no build-time kernel variants. */
    if (phase && phase<4) {
        /* Keep its reservation on disk; discard only volatile logger state. */
        media[0].cut=true;(void)bootlog_shutdown();media[0].cut=false;
    }
    stop(s,false);bootlog_reset();media[0].undo_count=0;fake_power_loss(&media[0]);
}
static struct storage *resume(unsigned phase)
{
    struct storage *s=start();OK(storage_enable_write(s,2));
    if (phase && phase<4) {
        OK(bootlog_activate(&s->vfs,true,s->sequence));
        bootlog_capture("current boot\n",13);OK(bootlog_sync(log_no_cancel,log_storage_turn,NULL));
    }
    return s;
}
static void discard(struct storage *s)
{
    media[0].cut=true;(void)bootlog_shutdown();stop(s,false);
    fake_power_loss(&media[0]);bootlog_reset();
}
static void verify_reboot(void)
{
    struct storage *s=start();
    CHECK(s->volumes[2].read_gate && !s->volumes[2].error);
    /* All reads work before write activation, regardless of dirty/lost state. */
    struct fat_volume *v=&s->volumes[2].fat;struct fat_entry entry;
    OK(vfs_stat(&table,"C:/SHORT.BIN",&entry));uint8_t bytes[512];size_t done;
    OK(fat_read(v,&entry,0,bytes,sizeof(bytes),&done));CHECK(done==sizeof(bytes));
    OK(storage_enable_write(s,2));CHECK(!v->readonly);ordinary_operations(s);
    OK(bootlog_activate(&s->vfs,true,s->sequence));OK(bootlog_shutdown());
    stop(s,true);bootlog_reset();
    for (unsigned copy=0;copy<2;copy++) {
        struct scan_result scan=independent_scan(media[0].fd,copy);
        CHECK(!scan.lost && !scan.crosslinks && !scan.corrupt && !scan.divergent && !scan.dirty);
    }
}
static void all_cuts(void)
{
    for (unsigned phase=0;phase<8;phase++) {
        seed(phase);
        for (unsigned mode=0;mode<(phase>=5 ? 4u : 1u);mode++) {
            media[0].reorder=mode!=0;media[0].cut_mode=mode ? mode-1 : 0;
            struct storage *s=resume(phase);unsigned base=media[0].events;
            OK(interrupted_work(s,phase));unsigned events=media[0].events-base;CHECK(events);
            discard(s);fake_reset(&media[0]);
            for (unsigned cut=1;cut<=events;cut++) {
                s=resume(phase);base=media[0].events;media[0].cut_at=base+cut;
                int e=interrupted_work(s,phase);
                CHECK(e<0 && media[0].cut);discard(s);
                if (phase>=5) {
                    struct storage *check=start();struct fat_entry file;
                    OK(vfs_stat(&table,"C:/SYSTEM/SETTINGS.CFG",&file));
                    unsigned length=phase==5 ? 1537 : phase==6 ? 1550 : 3538;
                    CHECK(file.size==13 || file.size==length);
                    if (file.size==length) {
                        char text[3538];size_t done;
                        OK(fat_read(&check->volumes[2].fat,&file,0,text,file.size,&done));CHECK(done==file.size);
                        unsigned begin=phase==5 ? 0 : phase==6 ? 13 : 2001;
                        if (phase>=6) CHECK(!memcmp(text,"old settings\n",13));
                        for (unsigned i=13;i<begin;i++) CHECK(!text[i]);
                        for (unsigned i=begin;i<length;i++) CHECK(text[i]=='N');
                    }
                    stop(check,false);bootlog_reset();
                }
                verify_reboot();fake_reset(&media[0]);
            }
            printf("PASS power cuts phase=%u mode=%u device_events=%u: RO reads, bounded repair, all files, settings, DOS games/base/overlay, log rotation, two clean FAT scans\n",phase,mode,events);
            fflush(stdout);
        }
        media[0].reorder=false;
        /* Rebuild the logical baseline for the next phase, without retaining
         * a second image. Existing seed paths are removed by reset below. */
        struct storage *cleanup=start();OK(storage_enable_write(cleanup,2));
        const char *paths[]={BOOTLOG_PATH,BOOTLOG_PREVIOUS,BOOTLOG_OLDEST,"C:/SYSTEM/SETTINGS.CFG",DOSVM_BUNDLED_BASE};
        for (unsigned i=0;i<sizeof(paths)/sizeof(paths[0]);i++) {
            int e=vfs_delete(&table,paths[i]);CHECK(!e || e==-FS_ENOENT);
        }
        OK(vfs_rmdir(&table,"C:/SYSTEM/DOS"));OK(vfs_rmdir(&table,"C:/GAMES"));
        stop(cleanup,true);bootlog_reset();media[0].undo_count=0;
    }
}
static void repair_cuts(void) {
    seed_workload();struct storage *s=start();OK(storage_enable_write(s,2));
    OK(bootlog_activate(&s->vfs,true,s->sequence));bootlog_capture("repair boot\n",12);
    OK(bootlog_sync(log_no_cancel,log_storage_turn,NULL));
    struct fat_volume geometry=s->volumes[2].fat;
    int h=vfs_open(&table,"C:/SYSTEM/TAIL.CFG",VFS_WRITE|VFS_CREATE,0,0);CHECK(h>=0);
    size_t done;OK(vfs_write(&table,h,"old settings\n",13,&done));
    char appended[1537];memset(appended,'A',sizeof(appended));
    OK(vfs_write(&table,h,appended,sizeof(appended),&done));OK(vfs_close(&table,h));
    struct fat_entry tail;OK(vfs_stat(&table,"C:/SYSTEM/TAIL.CFG",&tail));
    geometry.next_free=s->volumes[2].fat.next_free;
    discard(s);
    /* An ordinary append with durable new allocation and only one live FAT
     * mirror updated, before the new directory length was published. */
    uint32_t sec=geometry.data_sector+(tail.parent-2)*geometry.spc+tail.index/16;
    uint8_t raw[512];OK(fake_raw_read(&media[0],sec,raw));
    fs_wr32(raw+(tail.index%16)*32+28,13);OK(fake_raw_write(&media[0],sec,raw));
    patch_entry(&media[0],&geometry,1,tail.first,0x0fffffff);
    /* Add an unowned allocation in one FAT copy only, as at a COW cut. */
    patch_entry(&media[0],&geometry,1,geometry.next_free,0x0fffffff);
    media[0].undo_count=0;
    for (unsigned mode=0;mode<2;mode++) {
        media[0].reorder=!!mode;s=start();unsigned begin=media[0].events;
        OK(storage_enable_write(s,2));unsigned events=media[0].events-begin;
        discard(s);fake_reset(&media[0]);
        for (unsigned cut=1;cut<=events;cut++) {
            s=start();media[0].cut_at=media[0].events+cut;
            CHECK(storage_enable_write(s,2)==-FS_EIO && media[0].cut);
            discard(s);verify_reboot();fake_reset(&media[0]);
        }
        printf("PASS repeated repair cuts mode=%u device_events=%u: ordinary append/log-tail detach, divergent orphan reclaim, flags/hints/barriers, subsequent writable boot\n",mode,events);fflush(stdout);
    }
    media[0].reorder=false;
    s=start();OK(storage_enable_write(s,2));
    const char *paths[]={BOOTLOG_PATH,BOOTLOG_PREVIOUS,BOOTLOG_OLDEST,DOSVM_BUNDLED_BASE,"C:/SYSTEM/TAIL.CFG"};
    for (unsigned i=0;i<sizeof(paths)/sizeof(paths[0]);i++) { int e=vfs_delete(&table,paths[i]);CHECK(!e || e==-FS_ENOENT); }
    OK(vfs_rmdir(&table,"C:/SYSTEM/DOS"));OK(vfs_rmdir(&table,"C:/GAMES"));
    stop(s,true);bootlog_reset();media[0].undo_count=0;
}
static void imported_rotation(void)
{
    struct storage *s=start();OK(storage_enable_write(s,2));
    int h=vfs_open(&table,BOOTLOG_PATH,VFS_WRITE|VFS_CREATE,VFS_DENY_WRITE,0);CHECK(h>=0);
    /* The capture has 570 lines / 56402 bytes. Use synthetic records with
     * that published length and the current 320KiB unfinalized reservation;
     * the private capture itself never enters this repository. */
    static char capture[56402],block[BOOTLOG_BATCH];unsigned at=0;
    for (unsigned i=0;i<570;i++) {
        unsigned length=98+(i<542);memset(capture+at,'S',length);capture[at+length-1]='\n';at+=length;
    }
    CHECK(at==sizeof(capture));size_t done;uint64_t position;
    OK(vfs_log_reserve(&table,h,BOOTLOG_LIMIT));
    for (unsigned offset=0;offset<sizeof(capture);offset+=sizeof(block)) {
        unsigned take=sizeof(capture)-offset;if (take>sizeof(block)) take=sizeof(block);
        memset(block,0,sizeof(block));memcpy(block,capture+offset,take);
        OK(vfs_seek(&table,h,offset,VFS_SEEK_SET,&position));
        OK(vfs_log_write(&table,h,block,sizeof(block),offset+take,&done));
    }
    OK(vfs_log_publish(&table,h,false));OK(vfs_close(&table,h));
    struct fat_entry log;OK(vfs_stat(&table,BOOTLOG_PATH,&log));
    /* Remove the optional LFN and fill the preceding slot with a live entry,
     * reproducing an imported short owner with no rename expansion room. */
    uint32_t sec=s->volumes[2].fat.data_sector+(log.parent-2)*s->volumes[2].fat.spc+log.index/16;
    uint8_t raw[512];OK(cache_read(&s->cache,&s->volumes[2].io,sec,raw));CHECK(log.index%16);
    uint8_t *prior=raw+(log.index%16-1)*32;memset(prior,0,32);memcpy(prior,"NEIGHBORLOG",11);
    OK(cache_write(&s->cache,&s->volumes[2].io,sec,raw));OK(cache_barrier(&s->cache,&s->volumes[2].io));
    stop(s,false);bootlog_reset();s=start();OK(storage_enable_write(s,2));
    OK(bootlog_activate(&s->vfs,true,s->sequence));OK(bootlog_shutdown());
    OK(vfs_stat(&table,BOOTLOG_PREVIOUS,&log));char text[24];
    OK(fat_read(&s->volumes[2].fat,&log,0,text,sizeof(text),&done));CHECK(log.size==sizeof(capture) && done==sizeof(text) && !memcmp(text,capture,sizeof(text)));
    stop(s,true);bootlog_reset();media[0].undo_count=0;
    puts("PASS 5e018550-state fixture: synthetic 570-line/56402-byte log, unpublished 320KiB chain, imported 8.3 owner/occupied neighbor, dirty mount, BOOT1 retained before new reservation");
}
static void unsafe_and_bad_media(void) {
    seed_workload();struct storage *s=start();struct fat_volume geometry=s->volumes[2].fat;
    struct fat_entry entry;OK(vfs_stat(&table,"C:/SHORT.BIN",&entry));stop(s,true);bootlog_reset();
    patch_entry(&media[0],&geometry,1,entry.first,0x0fffffff);
    s=start();CHECK(s->volumes[2].fat.readonly && !s->volumes[2].read_gate);
    CHECK(storage_enable_write(s,2)==-FS_EROFS);
    int h=vfs_open(&table,"C:/SHORT.BIN",VFS_READ,0,0);CHECK(h>=0);uint8_t bytes[512];size_t done;
    OK(vfs_read(&table,h,bytes,sizeof(bytes),&done));CHECK(done==sizeof(bytes));OK(vfs_close(&table,h));
    memset(&vm,0,sizeof(vm));vm.info.key.generation=3;
    OK(dosvm_disk_open(&vm,DOSVM_BUNDLED_BASE));OK(dosvm_disk_read(&vm,128,bytes));CHECK(!memcmp(bytes,program,sizeof(program)));
    OK(dosvm_disk_close(&vm,false));stop(s,true);bootlog_reset();fake_reset(&media[0]);
    s=start();geometry=s->volumes[2].fat;stop(s,true);bootlog_reset();
    patch_entry(&media[0],&geometry,1,geometry.next_free,0x0ffffff7);
    s=start();OK(storage_enable_write(s,2));uint32_t bad;
    OK(fat_get_cluster(&s->volumes[2].fat,geometry.next_free,&bad));CHECK(bad==0x0ffffff7);
    CHECK(s->volumes[2].fat.repaired_copies==1 && !s->volumes[2].fat.readonly);
    stop(s,true);bootlog_reset();fake_reset(&media[0]);
    puts("PASS unsafe metadata: live FAT disagreement keeps write gate and readable files/DOS games; bad-media markers in either copy remain reserved");
}
static unsigned replay_lines;
void blackbox_serial_replay(const char *s,size_t n) {
    CHECK(n && !strncmp(s,"[blackbox] ",11));replay_lines++;
}
static uint32_t raw_crc(const uint8_t *b,unsigned n) {
    uint32_t crc=~0u;
    for (unsigned i=0;i<n;i++) { crc^=b[i];for (unsigned j=0;j<8;j++) crc=(crc>>1)^(0xedb88320u&-(crc&1)); }
    return ~crc;
}
static void raw_integration(void) {
    seed_workload();uint8_t desc[512]={0};
    memcpy(desc,"CBB1",4);fs_wr32(desc+4,1);fs_wr32(desc+8,32);fs_wr32(desc+12,128);fs_wr32(desc+508,raw_crc(desc,508));
    OK(fake_raw_write(&media[0],8,desc));
    struct storage *s=start();CHECK(!replay_lines);OK(storage_enable_write(s,2));
    blackbox_capture("[kernel] last before lock\n",26);OK(blackbox_flush());
    struct fat_volume *v=&s->volumes[2].fat;uint64_t writes=s->volumes[2].writes;
    fat_revoke_write(v,"crosslink_fixture",17);
    CHECK(v->readonly && s->volumes[2].writes==writes);
    /* DOS reads remain available even when its COW file cannot be created. */
    memset(&vm,0,sizeof(vm));vm.info.key.generation=2;
    OK(dosvm_disk_open(&vm,DOSVM_BUNDLED_BASE));uint8_t sector[512];
    OK(dosvm_disk_read(&vm,128,sector));CHECK(!memcmp(sector,program,sizeof(program)));
    CHECK(dosvm_disk_write(&vm,128,sector)==-FS_EROFS);OK(dosvm_disk_close(&vm,false));
    discard(s);s=start();CHECK(replay_lines && !s->volumes[2].writes);
    OK(storage_enable_write(s,2));bootlog_capture("new boot\n",9);
    OK(bootlog_activate(&s->vfs,true,s->sequence));OK(bootlog_shutdown());
    int h=vfs_open(&table,BOOTLOG_PATH,VFS_READ,0,0);CHECK(h>=0);char head[4096]={0};size_t done;
    OK(vfs_read(&table,h,head,sizeof(head)-1,&done));OK(vfs_close(&table,h));
    CHECK(!strncmp(head,"[blackbox] ",11) && strstr(head,"last before lock") && strstr(head,"readonly reason=crosslink_fixture"));
    vfs_table_destroy(&table);OK(storage_sync());storage_destroy(s);bootlog_reset();
    s=start();size_t history_size;const char *history=blackbox_replay(&history_size);
    CHECK(history_size && strstr(history,"shutdown requested") && strstr(history,"shutdown complete"));
    stop(s,true);bootlog_reset();
    puts("PASS blackbox integration: raw readonly reason survives filesystem refusal, serial replay before write gate, new BOOT.LOG head, readonly DOS game sector reads, shutdown request/completion replay");
}
int main(int argc,char **argv)
{
    CHECK(argc==4);for (unsigned i=0;i<3;i++) OK(fake_open(&media[i],argv[i+1]));
    baseline();all_cuts();repair_cuts();unsafe_and_bad_media();imported_rotation();
    /* Imported history is intentionally retained while seeding game files. */
    raw_integration();
    for (unsigned i=0;i<3;i++) fake_close(&media[i]);
    printf("POWER CUT RESULT checks=%u failures=0 ASan/UBSan=enabled\n",checks);return 0;
}
