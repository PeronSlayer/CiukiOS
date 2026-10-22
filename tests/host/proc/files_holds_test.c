/* Native desktop entry points on the same 111 ms block-service model.
 * Reuse the production process/path/fd fixture, not a second native model.
 * f1-93 adds the read-side entry points of the 1.0.0 alpha T23 trial (stat
 * of a long chain, executable image read, readdir, open) on a cold cache
 * with the measured 106 ms command: none may hold drive C across a
 * device command. SPDX-License-Identifier: GPL-2.0-only */
#define FD_TABLE_STACK_TEST
#include "files_test.c"
#include "../t23_trial.h"

static uint32_t model_ms, mutex_since[27], logical_since, max_mutex, max_logical, service_ms=T23_SERVICE_MS;
static unsigned mutex_depth, commands, releases, namespace_takes, hold_records;
static uint32_t hold_record_max, warm_base, fallback_base;
static bool measured;
static int (*saved_read)(struct blkdev *,uint64_t,uint32_t,void *);
static int (*saved_write)(struct blkdev *,uint64_t,uint32_t,const void *);
static int (*saved_flush)(struct blkdev *);
uint32_t fs_host_clock_ms(void) { return model_ms; }
/* f1-93b: the starved holder of the T23 b453bc92 trial. fs_service is the
 * kernel's only preemption point inside filesystem work (the kernel is not
 * preemptible elsewhere, arch/trap.c); a desktop worker starved by the
 * foreground DOS VM loses the CPU there and runs again only at its
 * starvation boost, T23B_PREEMPT_MS later. Reached while drive C is held,
 * that time is part of the hold. */
static bool starved;
static unsigned held_preemptions, service_points;
static bool logical;
/* Host CPU time of the longest drive C hold (ASAN build): the work bound of
 * a section, not a T23 duration. */
static uint64_t logical_since_ns, max_logical_ns;
static uint64_t host_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec; }
void fs_host_service(void) {
    if (!starved || !measured) return;
    service_points++; model_ms+=T23B_PREEMPT_MS;
    if (logical) held_preemptions++;
}
void __real_fs_lock_acquired(fs_lock *);
void __real_fs_lock_released(fs_lock *);
void __wrap_fs_lock_acquired(fs_lock *lock) {
    __real_fs_lock_acquired(lock);
    for (unsigned i=0;i<27;i++) if (lock==(i==26 ? &vfs.lock : &vfs.volume_lock[i])) {
        mutex_depth++; mutex_since[i]=model_ms;
        if (i==26 && measured) namespace_takes++;
        if (i==2 && !logical) { logical=true; logical_since=model_ms; if (measured) logical_since_ns=host_ns(); }
    }
}
void __wrap_fs_lock_released(fs_lock *lock) {
    for (unsigned i=0;i<27;i++) if (lock==(i==26 ? &vfs.lock : &vfs.volume_lock[i])) {
        CHECK(mutex_depth); mutex_depth--;
        uint32_t held=model_ms-mutex_since[i]; if (measured && held>max_mutex) max_mutex=held;
        if (i==26 && logical && !((vfs.reserved|vfs.mutating)&(1u<<2))) {
            held=model_ms-logical_since; logical=false;
            if (measured) {
                releases++; if (held>max_logical) max_logical=held;
                uint64_t ns=host_ns()-logical_since_ns; if (logical_since_ns && ns>max_logical_ns) max_logical_ns=ns;
            }
        }
    }
    __real_fs_lock_released(lock);
}
static void modeled_transfer(void) {
    if (measured) { CHECK(!mutex_depth); commands++; model_ms+=service_ms; }
}
/* Every production vfs_hold record (>= 50 ms) during a measured call. */
void vfs_hold_observer(char drive,uint32_t ms,const struct vfs_hold_info *owner,uint32_t waiters) {
    (void)drive; (void)waiters;
    if (!measured) return;
    printf("F1-93 vfs_hold ms=%u op=%s site=%s\n",ms,owner->op,owner->site);
    hold_records++; if (ms>hold_record_max) hold_record_max=ms;
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
    max_mutex=max_logical=commands=releases=namespace_takes=hold_records=hold_record_max=0;
    warm_base=vfs.warm_restarts; fallback_base=vfs.cold_fallbacks; measured=true;
}
static void finish(const char *operation) {
    measured=false;
    printf("F1-89 native operation=%s service_ms=111 commands=%u releases=%u physical_mutex_max_ms=%u logical_hold_max_ms=%u\n",
        operation,commands,releases,max_mutex,max_logical);
    CHECK(commands && releases && !max_mutex && max_logical<=20);
    CHECK(!volumes[0].transactions && !cache.reserved_bytes && !vfs.transaction_users[2]);
}
/* f1-93 rows: the T23 1.0.0 alpha read paths, cold, 106 ms per command. */
static void finish_cold(const char *operation,const char *t23) {
    measured=false;
    printf("F1-93 native operation=%s t23_op=%s service_ms=%u commands=%u releases=%u physical_mutex_max_ms=%u "
        "logical_hold_max_ms=%u vfs_hold_records=%u vfs_hold_max_ms=%u namespace_takes=%u warm_restarts=%u cold_fallbacks=%u\n",
        operation,t23,service_ms,commands,releases,max_mutex,max_logical,hold_records,hold_record_max,namespace_takes,
        vfs.warm_restarts-warm_base,vfs.cold_fallbacks-fallback_base);
    CHECK(commands && !max_mutex);
    CHECK(max_logical<=T23A_HOLD_GATE_MS && !hold_records && vfs.cold_fallbacks==fallback_base);
    CHECK(!volumes[0].transactions && !cache.reserved_bytes && !vfs.transaction_users[2]);
}
static int stat_path(const char *path,struct ciuki_stat *st) {
    vfs_lock(&vfs,0); struct px_path r;
    int e=px_resolve_locked(space,client->cwd,path,false,&r);
    if (!e) e=px_stat_locked(r.node,st);
    vfs_unlock(&vfs); return e;
}
#define COLD_IMAGE_BYTES (12u<<20) /* 24576 clusters of 512 bytes: 192 FAT sectors */
#define COLD_APP_BYTES (1u<<20)
#define COLD_ENTRIES 120u
static void cold_fixture(void) {
    CHECK(!make_dir("/Cold")); CHECK(!make_dir("/Cold/Many"));
    static uint8_t chunk[65536];
    int fd=file_open(client,"/Cold/Image.bin",O_CREAT|O_RDWR,0666); CHECK(fd>=0);
    memset(chunk,0x93,sizeof(chunk));
    for (uint32_t at=0;at<COLD_IMAGE_BYTES;at+=sizeof(chunk)) CHECK(io(fd,chunk,sizeof(chunk),true,false,0)==(int)sizeof(chunk));
    CHECK(!locked_sync(file_fd(client,fd))); CHECK(!file_close(client,fd));
    fd=file_open(client,"/Cold/App.elf",O_CREAT|O_RDWR,0666); CHECK(fd>=0);
    for (uint32_t at=0;at<COLD_APP_BYTES;at+=sizeof(chunk)) {
        for (unsigned i=0;i<sizeof(chunk);i++) chunk[i]=(uint8_t)((at+i)*7u>>3);
        CHECK(io(fd,chunk,sizeof(chunk),true,false,0)==(int)sizeof(chunk));
    }
    CHECK(!locked_sync(file_fd(client,fd))); CHECK(!file_close(client,fd));
    for (unsigned i=0;i<COLD_ENTRIES;i++) {
        char name[32]; snprintf(name,sizeof(name),"/Cold/Many/Entry %03u.txt",i);
        fd=file_open(client,name,O_CREAT|O_RDWR,0666); CHECK(fd>=0); CHECK(!file_close(client,fd));
    }
}
static void cold_rows(void) {
    service_ms=T23A_SERVICE_MS;
    struct ciuki_stat st;
    begin(); CHECK(!stat_path("/Cold/Image.bin",&st)); finish_cold("stat","stat");
    CHECK(st.st_size==COLD_IMAGE_BYTES && st.st_blocks==COLD_IMAGE_BYTES/512);
    int fd=file_open(client,"/Cold/Image.bin",O_RDONLY,0); CHECK(fd>=0);
    begin(); { struct file_description *d=file_fd(client,fd); vfs_lock(&vfs,vfs_drive_mask(2)); CHECK(!px_stat_locked(d->node,&st)); vfs_unlock(&vfs); }
    finish_cold("fstat","stat"); CHECK(st.st_blocks==COLD_IMAGE_BYTES/512); CHECK(!file_close(client,fd));
    /* The executable image read of a launch (tagged lookup in the trial). */
    struct ciuki_file image;
    begin(); CHECK(!image_open(client->cwd,"/Cold/App.elf",&image)); finish_cold("exec-image","lookup");
    CHECK(image.bytes==COLD_APP_BYTES);
    uint8_t probe[4096];
    for (uint32_t at=0;at<COLD_APP_BYTES;at+=257*1024) {
        uint32_t take=at+sizeof(probe)<=COLD_APP_BYTES ? sizeof(probe) : COLD_APP_BYTES-at;
        CHECK(!image.read(image.cookie,at,probe,take));
        for (unsigned i=0;i<take;i++) CHECK(probe[i]==(uint8_t)((at+i)*7u>>3));
    }
    image.close(image.cookie);
    /* One getdents syscall fills its buffer under one reservation. */
    fd=file_open(client,"/Cold/Many",O_RDONLY|O_DIRECTORY,0); CHECK(fd>=0);
    struct file_description *dir=file_fd(client,fd); unsigned listed=0;
    begin(); vfs_lock(&vfs,vfs_drive_mask(2));
    for (;;) { struct ciuki_dirent ent; int n=px_getdents_locked(dir,&ent); CHECK(n>=0); if (!n) break; listed++; }
    vfs_unlock(&vfs); finish_cold("readdir","readdir");
    CHECK(listed==COLD_ENTRIES+2); CHECK(!file_close(client,fd));
    begin(); fd=file_open(client,"/Cold/Many/Entry 077.txt",O_RDONLY,0); CHECK(fd>=0); finish_cold("open","open");
    CHECK(!file_close(client,fd));
    begin(); CHECK(!make_dir("/Cold/Many/Sub")); finish_cold("mkdir","mkdir");
    CHECK(!remove_path_native("/Cold/Many/Sub",true));
    service_ms=T23_SERVICE_MS;
}
/* The trial's read-side record mix on drive C, every request cold: 240
 * stat (long chain), 36 readdir, 10 executable image reads, interleaved. */
static void alpha_mix(void) {
    service_ms=T23A_SERVICE_MS;
    unsigned total=T23A_STAT_RECORDS+T23A_READDIR_RECORDS+T23A_LOOKUP_RECORDS, counts[3]={0};
    uint32_t worst=0, worst_records=0, commands_total=0;
    for (unsigned i=0;i<total;i++) {
        unsigned slot=i*197u%total, kind=slot<T23A_STAT_RECORDS ? 0 : slot<T23A_STAT_RECORDS+T23A_READDIR_RECORDS ? 1 : 2;
        begin();
        if (kind==0) { struct ciuki_stat st; CHECK(!stat_path("/Cold/Image.bin",&st) && st.st_blocks==COLD_IMAGE_BYTES/512); }
        else if (kind==1) {
            int fd=file_open(client,"/Cold/Many",O_RDONLY|O_DIRECTORY,0); CHECK(fd>=0);
            struct file_description *dir=file_fd(client,fd); unsigned listed=0;
            vfs_lock(&vfs,vfs_drive_mask(2));
            for (;;) { struct ciuki_dirent ent; int n=px_getdents_locked(dir,&ent); CHECK(n>=0); if (!n) break; listed++; }
            vfs_unlock(&vfs); CHECK(listed==COLD_ENTRIES+2); CHECK(!file_close(client,fd));
        } else {
            struct ciuki_file image; CHECK(!image_open(client->cwd,"/Cold/App.elf",&image)); image.close(image.cookie);
        }
        measured=false; counts[kind]++; commands_total+=commands;
        CHECK(!max_mutex && vfs.cold_fallbacks==fallback_base);
        if (max_logical>worst) worst=max_logical;
        worst_records+=hold_records;
    }
    printf("F1-93 T23 alpha replay (cold, %u ms per command): stat=%u readdir=%u exec_image=%u commands=%u "
        "logical_hold_max_ms=%u vfs_hold_records=%u gate_ms=%u\n",
        T23A_SERVICE_MS,counts[0],counts[1],counts[2],commands_total,worst,worst_records,T23A_HOLD_GATE_MS);
    CHECK(worst<=T23A_HOLD_GATE_MS && !worst_records);
    service_ms=T23_SERVICE_MS;
}
/* ---- f1-93b: the desktop#31 directory compare, T23 b453bc92 ---------- */
static uint32_t row_model_start, row_yields, row_restarts;
static bool t23b_failed;
static void begin_t23b(bool cold) {
    CHECK(!cache.dirty_bytes); if (cold) cache_invalidate(&cache,&disk[0].dev,false);
    max_mutex=max_logical=commands=releases=namespace_takes=hold_records=hold_record_max=0;
    held_preemptions=service_points=0; row_model_start=model_ms; max_logical_ns=logical_since_ns=0;
    row_yields=vfs.step_yields; row_restarts=vfs.step_restarts;
    warm_base=vfs.warm_restarts; fallback_base=vfs.cold_fallbacks; measured=true;
}
static void finish_t23b(const char *operation,const char *t23,unsigned items) {
    measured=false;
    printf("F1-93B T23 b453bc92 desktop#31 operation=%s t23_op=%s items=%u service_ms=%u preempt_ms=%u commands=%u releases=%u "
        "physical_mutex_max_ms=%u logical_hold_max_ms=%u vfs_hold_records=%u vfs_hold_max_ms=%u held_preemptions=%u "
        "service_points=%u warm_restarts=%u step_yields=%u step_restarts=%u cold_fallbacks=%u model_ms=%u "
        "host_hold_max_us=%llu gate_ms=%u\n",
        operation,t23,items,service_ms,T23B_PREEMPT_MS,commands,releases,max_mutex,max_logical,hold_records,hold_record_max,
        held_preemptions,service_points,vfs.warm_restarts-warm_base,vfs.step_yields-row_yields,vfs.step_restarts-row_restarts,
        vfs.cold_fallbacks-fallback_base,model_ms-row_model_start,(unsigned long long)(max_logical_ns/1000),T23A_HOLD_GATE_MS);
    if (max_mutex || max_logical>T23A_HOLD_GATE_MS || hold_records || held_preemptions || vfs.cold_fallbacks!=fallback_base) {
        printf("F1-93B NOT ACCEPTED operation=%s: drive C held %u ms (%u preemption points inside a hold, %u cold fallbacks)\n",
            operation,max_logical,held_preemptions,vfs.cold_fallbacks-fallback_base);
        t23b_failed=true;
    }
    CHECK(!volumes[0].transactions && !cache.reserved_bytes && !vfs.transaction_users[2]);
}
/* apps/desktop/folder.c folder_poll_worker -> apps/files/vfs.c list ->
 * sdk/libciuki/posix.c opendir (open O_DIRECTORY, fstat) and readdir (one
 * CIUKI_SYS_GETDENTS record per call, syscalls_file.c fd_call), closedir. */
static unsigned folder_compare(const char *path) {
    int fd=file_open(client,path,O_RDONLY|O_DIRECTORY,0); CHECK(fd>=0);
    struct file_description *d=file_fd(client,fd); struct ciuki_stat st;
    vfs_lock(&vfs,vfs_drive_mask(2)); CHECK(!px_stat_locked(d->node,&st)); vfs_unlock(&vfs);
    CHECK((st.st_mode & S_IFMT)==S_IFDIR);
    unsigned listed=0;
    for (;;) {
        struct ciuki_dirent ent;
        vfs_lock(&vfs,vfs_drive_mask(2)); int n=px_getdents_locked(d,&ent); vfs_unlock(&vfs);
        CHECK(n>=0); if (!n) break;
        listed++;
    }
    CHECK(!file_close(client,fd));
    return listed;
}
static void desktop31_replay(void) {
    static const char folder[]="/Documents/Desktop", last[]="i1999 Desktop item.txt";
    service_ms=T23A_SERVICE_MS; starved=true;
    unsigned entries=T23B_FOLDER_ENTRIES+3; /* ., .. and Zz Sub */
    begin_t23b(true); CHECK(folder_compare(folder)==entries); finish_t23b("directory-compare-cold","readdir/stat/lookup",entries);
    begin_t23b(false); CHECK(folder_compare(folder)==entries); finish_t23b("directory-compare-warm","readdir/stat/lookup",entries);
    /* The links worker's per-entry stat, last entry of the folder. */
    char path[96]; snprintf(path,sizeof(path),"%s/%s",folder,last); struct ciuki_stat st;
    begin_t23b(true); CHECK(!stat_path(path,&st) && (st.st_mode & S_IFMT)==S_IFREG); finish_t23b("stat-last-entry","lookup/stat",1);
    /* The resolver's guarded attempts (after two unguarded ones overlapped
     * a publication), as px_resolve_locked: retried while EAGAIN. */
    vfs_lock(&vfs,0); struct px_path r; CHECK(!px_resolve_locked(space,client->cwd,folder,false,&r));
    struct px_node *dir=r.node; px_retain(dir); vfs_unlock(&vfs);
    begin_t23b(true);
    int e=-EAGAIN;
    for (unsigned attempt=0;e==-EAGAIN && attempt<PX_RESOLVE_GUARDED;attempt++) {
        struct px_node *n=0; vfs_lock(&vfs,vfs_drive_mask(2)); e=child_lookup(dir,last,&n,true); vfs_unlock(&vfs);
    }
    if (e) { printf("F1-93B NOT ACCEPTED operation=guarded-lookup-last-entry: %d after %u attempts\n",e,PX_RESOLVE_GUARDED); t23b_failed=true; }
    finish_t23b("guarded-lookup-last-entry","lookup",1);
    px_release(dir);
    /* DOS FindFirst/FindNext and chdir (canonical walk) of the same folder
     * through the legacy table, the dos/w32 path of vfs_dir_step_locked. */
    struct vfs_table dos; CHECK(!vfs_table_init(&vfs,&dos,2));
    struct vfs_find f; struct fat_entry ent; unsigned found=0;
    begin_t23b(true);
    e=vfs_find_first(&dos,"C:/Documents/Desktop/*.*",VFS_FIND_DOS,&f,&ent);
    while (!e) { found++; e=vfs_find_next(&dos,&f,&ent); }
    CHECK(e==-FS_ENOENT && found==T23B_FOLDER_ENTRIES); finish_t23b("dos-find","find",found);
    begin_t23b(true); CHECK(!vfs_chdir(&dos,"C:/Documents/Desktop/Zz Sub")); finish_t23b("dos-chdir-last-entry","chdir",1);
    CHECK(!vfs_chdir(&dos,"C:/"));
    vfs_table_destroy(&dos);
    starved=false; service_ms=T23_SERVICE_MS;
}
int main(int argc,char **argv) {
    bool t23b=argc==4 && !strcmp(argv[1],"--desktop31");
    if (t23b) { argv++; argc--; }
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
    if (t23b) {
        desktop31_replay();
        all_closed(); stop_collect(client);
        struct file_ledger ledger; files_snapshot(space,&ledger); CHECK(!ledger.descriptions && !ledger.pins);
        for (unsigned i=0;i<2;i++) CHECK(!vfs_detach(&vfs,i+2));
        vfs_destroy(&vfs); cache_destroy(&cache);
        for (unsigned i=0;i<2;i++) { fake_reset(&disk[i]); fake_close(&disk[i]); }
        CHECK(!pages_used && heap_blocks==1 && !fs_host_contig_live);
        free(ram);
        if (t23b_failed) { puts("F1-93B NOT ACCEPTED: a drive C hold spans a preemption point or a cold device read"); return 1; }
        puts("PASS f1-93b desktop#31 directory compare: no drive C hold above 20 ms"); return 0;
    }
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
    cold_fixture(); cold_rows(); alpha_mix();
    all_closed(); stop_collect(client);
    struct file_ledger ledger; files_snapshot(space,&ledger); CHECK(!ledger.descriptions && !ledger.pins);
    for (unsigned i=0;i<2;i++) CHECK(!vfs_detach(&vfs,i+2));
    vfs_destroy(&vfs); cache_destroy(&cache);
    for (unsigned i=0;i<2;i++) { fake_reset(&disk[i]); fake_close(&disk[i]); }
    CHECK(!pages_used && heap_blocks==1 && !fs_host_contig_live);
    free(ram); puts("PASS native bounded transactions and teardown ledger"); return 0;
}
