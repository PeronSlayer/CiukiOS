/* G29D production executable snapshot + VFS sharing, using the native fixture. */
#define FD_TABLE_STACK_TEST
#include "files_test.c"
/* Bounded observation endpoints qualify native ABI dispatch here;
 * production storage bodies are covered by --maintenance-mount. */
static unsigned maintenance_calls;
int storage_defrag(struct storage *s,struct ciuki_media_defrag *r,bool step)
{ CHECK(s==&media_observation && r->size==sizeof(*r) && r->drive==2 &&
        r->flags==CIUKI_DEFRAG_STARTUP && r->placement==2);
  CHECK(step==(maintenance_calls==1)); maintenance_calls++; return -FS_EOPNOTSUPP; }
int storage_convert(struct storage *s,struct ciuki_media_convert *r,bool execute)
{ (void)s;(void)r;(void)execute; maintenance_calls++; return -FS_EOPNOTSUPP; }
int storage_format_removable(struct storage *s,struct ciuki_media_format *r,bool execute)
{ (void)s;(void)r;(void)execute; maintenance_calls++; return -FS_EOPNOTSUPP; }
int storage_floppy_copy(struct storage *s,struct ciuki_floppy_image *r,storage_floppy_copy_fn fn,void *arg,bool write)
{ (void)s;(void)r;(void)fn;(void)arg;(void)write; maintenance_calls++; return -FS_EOPNOTSUPP; }
static uint32_t log_clock(void) {
    vfs_lock(&vfs,vfs_drive_mask(volume_drive(0)));
    struct fat_entry log; uint8_t header[16]; size_t done;
    CHECK(!fat_lookup(&volumes[0],0,"CIUKIST.LOG",&log));
    CHECK(!fat_read(&volumes[0],&log,0,header,sizeof(header),&done) && done==sizeof(header));
    CHECK(fs_rd32(header)==0x314c5343 && fs_rd32(header+4)==volumes[0].volume_serial);
    vfs_unlock(&vfs); return fs_rd32(header+8);
}
static bool log_present(void) {
    vfs_lock(&vfs,vfs_drive_mask(volume_drive(0)));
    struct fat_entry log; int e=fat_lookup(&volumes[0],0,"CIUKIST.LOG",&log);
    vfs_unlock(&vfs); CHECK(!e || e==-FS_ENOENT); return !e;
}
int main(int argc,char **argv) {
    CHECK(argc==3); ram=calloc(HOST_PAGES,PAGE_SIZE); CHECK(ram);
    controller.state=T_RUNNING; g_current=&controller; proc_init();
    CHECK(!fake_open(&disk[0],argv[1]) && !fake_open(&disk[1],argv[2]));
    disk[0].undo_enabled=disk[1].undo_enabled=true;
    CHECK(!cache_init(&cache,0)); vfs_init(&vfs);
    for (unsigned i=0;i<mounted_volumes;i++) {
        CHECK(!fat_mount(&volumes[i],&cache,&disk[i].dev,0,disk[i].dev.capacity,true));
        CHECK(!vfs_attach(&vfs,volume_drive(i),&volumes[i]));
    }
    CHECK(!files_attach(&vfs,&space));
    client_thread=make_process(proc_supervisor(),&client); g_current=client_thread->task; g_current->state=T_RUNNING;
    client->cwd=space->root; px_retain(client->cwd); client->cwd_retain=px_retain; client->cwd_release=px_release;
    /* dec1 (QEMU 56e58374): exec queues the start; the background flusher
     * (the storage start-log task) writes and commits CIUKIST.LOG later. */
    test_launch_reads();
    CHECK(vfs.startup_count==1 && !log_present()); /* the launch wrote nothing */
    CHECK(vfs_startup_flush(&vfs,true)==1 && !vfs.startup_count && log_present());
    uint32_t before=log_clock(); CHECK(before);
    int fd=file_open(client,"/CIUKIST.LOG",O_RDONLY,0); CHECK(fd>=0);
    struct ciuki_file image;
    unsigned writes=disk[0].writes, flushes=disk[0].flushes; uint64_t mutations=volumes[0].mutation_sequence;
    CHECK(!image_open(client->cwd,"/SHORT.BIN",&image)); image.close(image.cookie);
    /* exec issues no C: write or commit */
    CHECK(disk[0].writes==writes && disk[0].flushes==flushes && volumes[0].mutation_sequence==mutations);
    CHECK(vfs.startup_count==1 && log_clock()==before);
    unsigned failed=vfs.startup_failed;
    CHECK(vfs_startup_flush(&vfs,true)<0 && vfs.startup_failed==failed+1);
    CHECK(log_clock()==before); /* native log reader's sharing is respected */
    CHECK(!file_close(client,fd));
    writes=disk[0].writes; flushes=disk[0].flushes; mutations=volumes[0].mutation_sequence;
    CHECK(!image_open(client->cwd,"/SHORT.BIN",&image)); image.close(image.cookie);
    CHECK(disk[0].writes==writes && disk[0].flushes==flushes && volumes[0].mutation_sequence==mutations);
    CHECK(log_clock()==before); /* queued, not yet written */
    CHECK(vfs_startup_flush(&vfs,true)==1 && disk[0].flushes>flushes);
    CHECK(log_clock()==before+1 && !maintenance_calls);
    /* A burst larger than the queue: the oldest starts are dropped, the rest
     * reach the log in one flush. */
    unsigned dropped=vfs.startup_dropped;
    for (unsigned i=0;i<VFS_STARTUP_QUEUE+3;i++) { CHECK(!image_open(client->cwd,"/SHORT.BIN",&image)); image.close(image.cookie); }
    CHECK(vfs.startup_count==VFS_STARTUP_QUEUE && vfs.startup_dropped==dropped+3 && log_clock()==before+1);
    CHECK(vfs_startup_flush(&vfs,true)==(int)VFS_STARTUP_QUEUE && !vfs.startup_count);
    CHECK(log_clock()==before+1+VFS_STARTUP_QUEUE && !vfs_startup_flush(&vfs,true));
    /* No memory for the flush: the queue is dropped once and counted as
     * failed, so the start-log task has nothing to retry (no log spam). */
    CHECK(!image_open(client->cwd,"/SHORT.BIN",&image)); image.close(image.cookie);
    failed=vfs.startup_failed; fs_host_contig_fail=1;
    CHECK(vfs_startup_flush(&vfs,true)==-FS_ENOMEM && !vfs.startup_count && vfs.startup_failed==failed+1);
    CHECK(!vfs_startup_flush(&vfs,true) && log_clock()==before+1+VFS_STARTUP_QUEUE);
    /* The task's settle delay: a burst reaching 3/4 of the queue is written
     * at once; a single start waits for the delay. */
    for (unsigned i=0;i<VFS_STARTUP_URGENT;i++) { CHECK(!image_open(client->cwd,"/SHORT.BIN",&image)); image.close(image.cookie); }
    uint32_t settle=fs_now_ms(); vfs_startup_settle(&vfs,60000,60000); CHECK(fs_now_ms()-settle<1000);
    CHECK(vfs_startup_flush(&vfs,true)==(int)VFS_STARTUP_URGENT);
    CHECK(!image_open(client->cwd,"/SHORT.BIN",&image)); image.close(image.cookie);
    settle=fs_now_ms(); vfs_startup_settle(&vfs,50,5000); CHECK(fs_now_ms()-settle>=50 && fs_now_ms()-settle<5000);
    /* fx-launch: the quiet period runs from the newest start, not from the
     * flusher's wake-up, and the maximum bounds a continuous burst. */
    CHECK(!image_open(client->cwd,"/SHORT.BIN",&image)); image.close(image.cookie);
    struct timespec pause={0,150000000}; nanosleep(&pause,0);
    settle=fs_now_ms(); vfs_startup_settle(&vfs,200,5000);
    CHECK(fs_now_ms()-settle<150 && vfs.startup_count==2);
    CHECK(!image_open(client->cwd,"/SHORT.BIN",&image)); image.close(image.cookie);
    settle=fs_now_ms(); vfs_startup_settle(&vfs,60000,80);
    CHECK(fs_now_ms()-settle>=80 && fs_now_ms()-settle<5000 && vfs.startup_count==3);
    CHECK(vfs_startup_flush(&vfs,true)==3 && log_clock()==before+4+VFS_STARTUP_QUEUE+VFS_STARTUP_URGENT);
    vfs_init(&media_observation.vfs);media_observation.ready=true;
    fd=file_open(client,"/dev/media",O_RDONLY,0); CHECK(fd>=0);
    struct ciuki_media_defrag request={.version=CIUKI_MEDIA_VERSION,.size=sizeof(request),
        .drive=2,.flags=CIUKI_DEFRAG_STARTUP,.placement=2};
    CHECK(!ua_write(client->memory,USER,&request,sizeof(request)));
    CHECK(call(CIUKI_SYS_DEVCTL,fd,MEDIA_DEFRAG_ANALYZE,USER,sizeof(request),0)==-EOPNOTSUPP);
    CHECK(call(CIUKI_SYS_DEVCTL,fd,MEDIA_DEFRAG_STEP,USER,sizeof(request),0)==-EOPNOTSUPP);
    CHECK(maintenance_calls==2 && !file_close(client,fd));
    vfs_destroy(&media_observation.vfs);
    all_closed(); stop_collect(client);
    struct file_ledger ledger; files_snapshot(space,&ledger); CHECK(!ledger.descriptions && !ledger.pins);
    for (unsigned i=0;i<mounted_volumes;i++) CHECK(!vfs_detach(&vfs,volume_drive(i)));
    vfs_destroy(&vfs); cache_destroy(&cache);
    for (unsigned i=0;i<mounted_volumes;i++) { fake_reset(&disk[i]); fake_close(&disk[i]); }
    CHECK(!pages_used && heap_blocks==1); free(ram);
    puts("PASS production startup image log: actual snapshot reads, exec queues without C: writes, background flush writes the persistent volume-bound hint, open-log sharing, bounded queue drops the oldest, out-of-memory drops once, 3/4-full burst written at once, launch remains successful, new devctl dispatch, native descriptor/identity cleanup");
    return 0;
}
