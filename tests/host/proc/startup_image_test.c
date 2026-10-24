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
    test_launch_reads(); uint32_t before=log_clock(); CHECK(before);
    int fd=file_open(client,"/CIUKIST.LOG",O_RDONLY,0); CHECK(fd>=0);
    struct ciuki_file image;
    CHECK(!image_open(client->cwd,"/SHORT.BIN",&image)); image.close(image.cookie);
    CHECK(log_clock()==before); /* native log reader's sharing is respected */
    CHECK(!file_close(client,fd));
    CHECK(!image_open(client->cwd,"/SHORT.BIN",&image)); image.close(image.cookie);
    CHECK(log_clock()==before+1 && !maintenance_calls);
    fd=file_open(client,"/dev/media",O_RDONLY,0); CHECK(fd>=0);
    struct ciuki_media_defrag request={.version=CIUKI_MEDIA_VERSION,.size=sizeof(request),
        .drive=2,.flags=CIUKI_DEFRAG_STARTUP,.placement=2};
    CHECK(!ua_write(client->memory,USER,&request,sizeof(request)));
    CHECK(call(CIUKI_SYS_DEVCTL,fd,MEDIA_DEFRAG_ANALYZE,USER,sizeof(request),0)==-EOPNOTSUPP);
    CHECK(call(CIUKI_SYS_DEVCTL,fd,MEDIA_DEFRAG_STEP,USER,sizeof(request),0)==-EOPNOTSUPP);
    CHECK(maintenance_calls==2 && !file_close(client,fd));
    all_closed(); stop_collect(client);
    struct file_ledger ledger; files_snapshot(space,&ledger); CHECK(!ledger.descriptions && !ledger.pins);
    for (unsigned i=0;i<mounted_volumes;i++) CHECK(!vfs_detach(&vfs,volume_drive(i)));
    vfs_destroy(&vfs); cache_destroy(&cache);
    for (unsigned i=0;i<mounted_volumes;i++) { fake_reset(&disk[i]); fake_close(&disk[i]); }
    CHECK(!pages_used && heap_blocks==1); free(ram);
    puts("PASS production startup image log: actual snapshot reads, persistent volume-bound hint, open-log sharing, launch remains successful, new devctl dispatch, native descriptor/identity cleanup");
    return 0;
}
