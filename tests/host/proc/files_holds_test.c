/* Native desktop entry points on the same 111 ms block-service model.
 * Reuse the production process/path/fd fixture, not a second native model.
 * SPDX-License-Identifier: GPL-2.0-only */
#define FD_TABLE_STACK_TEST
#include "files_test.c"
#include "../t23_trial.h"

static uint32_t model_ms, mutex_since[27], logical_since, max_mutex, max_logical;
static unsigned mutex_depth, commands, releases;
static bool measured, logical;
static int (*saved_read)(struct blkdev *,uint64_t,uint32_t,void *);
static int (*saved_write)(struct blkdev *,uint64_t,uint32_t,const void *);
static int (*saved_flush)(struct blkdev *);
uint32_t fs_host_clock_ms(void) { return model_ms; }
void __real_fs_lock_acquired(fs_lock *);
void __real_fs_lock_released(fs_lock *);
void __wrap_fs_lock_acquired(fs_lock *lock) {
    __real_fs_lock_acquired(lock);
    for (unsigned i=0;i<27;i++) if (lock==(i==26 ? &vfs.lock : &vfs.volume_lock[i])) {
        mutex_depth++; mutex_since[i]=model_ms;
        if (i==2 && !logical) { logical=true; logical_since=model_ms; }
    }
}
void __wrap_fs_lock_released(fs_lock *lock) {
    for (unsigned i=0;i<27;i++) if (lock==(i==26 ? &vfs.lock : &vfs.volume_lock[i])) {
        CHECK(mutex_depth); mutex_depth--;
        uint32_t held=model_ms-mutex_since[i]; if (measured && held>max_mutex) max_mutex=held;
        if (i==26 && logical && !((vfs.reserved|vfs.mutating)&(1u<<2))) {
            held=model_ms-logical_since; logical=false;
            if (measured) { releases++; if (held>max_logical) max_logical=held; }
        }
    }
    __real_fs_lock_released(lock);
}
static void modeled_transfer(void) {
    if (measured) { CHECK(!mutex_depth); commands++; model_ms+=T23_SERVICE_MS; }
}
static int timed_read(struct blkdev *d,uint64_t lba,uint32_t n,void *p) {
    modeled_transfer(); return saved_read(d,lba,n,p);
}
static int timed_write(struct blkdev *d,uint64_t lba,uint32_t n,const void *p) {
    modeled_transfer(); return saved_write(d,lba,n,p);
}
static int timed_flush(struct blkdev *d) { modeled_transfer(); return saved_flush(d); }
static void begin(void) {
    CHECK(!cache.dirty_bytes); cache_invalidate(&cache,&disk[0].dev,false);
    max_mutex=max_logical=commands=releases=0; measured=true;
}
static void finish(const char *operation) {
    measured=false;
    printf("F1-89 native operation=%s service_ms=111 commands=%u releases=%u physical_mutex_max_ms=%u logical_hold_max_ms=%u\n",
        operation,commands,releases,max_mutex,max_logical);
    CHECK(commands && releases && !max_mutex && max_logical<=20);
    CHECK(!volumes[0].transactions && !cache.reserved_bytes && !vfs.transaction_users[2]);
}
int main(int argc,char **argv) {
    CHECK(argc==3); ram=calloc(HOST_PAGES,PAGE_SIZE); CHECK(ram);
    controller.state=T_RUNNING; g_current=&controller; proc_init();
    CHECK(!fake_open(&disk[0],argv[1]) && !fake_open(&disk[1],argv[2]));
    disk[0].undo_enabled=disk[1].undo_enabled=true;
    CHECK(!cache_init(&cache,0)); vfs_init(&vfs);
    for (unsigned i=0;i<2;i++) {
        CHECK(!fat_mount(&volumes[i],&cache,&disk[i].dev,0,disk[i].dev.capacity,true));
        CHECK(!vfs_attach(&vfs,i+2,&volumes[i]));
    }
    CHECK(!files_attach(&vfs,&space));
    client_thread=make_process(proc_supervisor(),&client); g_current=client_thread->task; g_current->state=T_RUNNING;
    client->cwd=space->root; px_retain(client->cwd); client->cwd_retain=px_retain; client->cwd_release=px_release;
    saved_read=disk[0].dev.read; saved_write=disk[0].dev.write; saved_flush=disk[0].dev.flush;
    disk[0].dev.read=timed_read; disk[0].dev.write=timed_write; disk[0].dev.flush=timed_flush;
    begin(); CHECK(!make_dir("/Bounded")); finish("mkdir");
    begin(); int fd=file_open(client,"/Bounded/File",O_CREAT|O_RDWR,0666); CHECK(fd>=0); finish("create");
    uint8_t bytes[4096]; memset(bytes,0x89,sizeof(bytes));
    begin(); CHECK(io(fd,bytes,sizeof(bytes),true,false,0)==sizeof(bytes)); finish("write");
    begin(); CHECK(!locked_truncate(file_fd(client,fd),8192)); finish("extend");
    begin(); CHECK(!locked_sync(file_fd(client,fd))); finish("commit");
    begin(); CHECK(!rename_path("/Bounded/File","/Bounded/Renamed")); finish("rename");
    begin(); CHECK(!remove_path_native("/Bounded/Renamed",false)); finish("unlink-open");
    begin(); CHECK(!file_close(client,fd)); finish("reclaim");
    begin(); CHECK(!remove_path_native("/Bounded",true)); finish("rmdir");
    all_closed(); stop_collect(client);
    struct file_ledger ledger; files_snapshot(space,&ledger); CHECK(!ledger.descriptions && !ledger.pins);
    for (unsigned i=0;i<2;i++) CHECK(!vfs_detach(&vfs,i+2));
    vfs_destroy(&vfs); cache_destroy(&cache);
    for (unsigned i=0;i<2;i++) { fake_reset(&disk[i]); fake_close(&disk[i]); }
    CHECK(!pages_used && heap_blocks==1 && !fs_host_contig_live);
    free(ram); puts("PASS native bounded transactions and teardown ledger"); return 0;
}
