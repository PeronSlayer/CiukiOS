/* SPDX-License-Identifier: MIT
 * Production shell service with deliberately stalled media/settings/wallpaper
 * I/O. Included in shell_launch_test.c; no guest or device emulation. */
static pthread_mutex_t autoplay_test_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t autoplay_test_changed=PTHREAD_COND_INITIALIZER;
static pthread_t autoplay_test_main;
static unsigned autoplay_test_scans, autoplay_test_reads, autoplay_test_writes;
static int autoplay_test_scan_release, autoplay_test_read_release, autoplay_test_write_release;
static uint64_t autoplay_test_service_max;
static int stalled_media_list(void *ctx,const char *path,unsigned flags,fm_emit emit,void *user)
{
    (void)ctx; (void)flags; (void)emit; (void)user; assert(!strcmp(path,"/mnt/e"));
    /* The old synchronous service fails here: its caller is the frame thread.
     * Waiting for a frame/input-dependent release there cannot make progress. */
    assert(!pthread_equal(pthread_self(),autoplay_test_main));
    pthread_mutex_lock(&autoplay_test_lock); autoplay_test_scans++;
    while (!autoplay_test_scan_release) pthread_cond_wait(&autoplay_test_changed,&autoplay_test_lock);
    pthread_mutex_unlock(&autoplay_test_lock); return 0;
}
static int stalled_media_preference(struct desktop *d,const char *id,const char *label,int action)
{
    (void)d; assert(!strcmp(id,"fat-12345678-1234"));
    assert(!pthread_equal(pthread_self(),autoplay_test_main));
    pthread_mutex_lock(&autoplay_test_lock);
    if (action<0) {
        assert(!label); autoplay_test_reads++;
        while (!autoplay_test_read_release) pthread_cond_wait(&autoplay_test_changed,&autoplay_test_lock);
    } else {
        assert(action==AP_NOTHING && label); autoplay_test_writes++;
        while (!autoplay_test_write_release) pthread_cond_wait(&autoplay_test_changed,&autoplay_test_lock);
    }
    pthread_mutex_unlock(&autoplay_test_lock); return action<0 ? -1 : 0;
}
static const struct fm_vfs stalled_media_fs={.list=stalled_media_list};
static void autoplay_test_wait(unsigned *counter,unsigned value)
{
    for (unsigned i=0;i<3000;i++) {
        pthread_mutex_lock(&autoplay_test_lock); int ready=*counter>=value; pthread_mutex_unlock(&autoplay_test_lock);
        if (ready) return;
        struct timespec pause={0,1000000}; nanosleep(&pause,NULL);
    }
    assert(!"AutoPlay operation did not start");
}
static void autoplay_test_release(int *release)
{
    pthread_mutex_lock(&autoplay_test_lock); *release=1;
    pthread_cond_broadcast(&autoplay_test_changed); pthread_mutex_unlock(&autoplay_test_lock);
}
static void autoplay_test_service(struct desktop *d)
{
    uint64_t begin=monotonic(); shell_service(d); uint64_t elapsed=monotonic()-begin;
    if (elapsed>autoplay_test_service_max) autoplay_test_service_max=elapsed;
    assert(elapsed<50000000ull); /* host scheduling margin; held I/O never releases here */
    assert(strcmp(__atomic_load_n(&desktop_watchdog.wait,__ATOMIC_ACQUIRE),"autoplay-service"));
    /* Confirmation now waits for a fresh asynchronous media observation.
     * Keep measuring each individual production frame turn. */
    for (unsigned i=0;media_worker.confirm_pending && i<3000;i++) {
        struct timespec pause={0,1000000};nanosleep(&pause,NULL);
        begin=monotonic();shell_service(d);elapsed=monotonic()-begin;
        assert(elapsed<50000000ull);
        if (elapsed>autoplay_test_service_max) autoplay_test_service_max=elapsed;
    }
    assert(!media_worker.confirm_pending);
}
static void autoplay_test_mount(struct desktop *d,unsigned generation)
{
    media_fixture.removable_mask=1u<<4;
    media_fixture.type[4]=CIUKI_MEDIA_USB; media_fixture.generation[4]=generation;
    media_volumes[4]=(struct ciuki_media_volume){.version=CIUKI_MEDIA_VERSION,.size=sizeof(media_volumes[4]),
        .drive=4,.flags=CIUKI_MEDIA_REMOVABLE,.generation=generation};
    strcpy(media_volumes[4].id,"fat-12345678-1234"); strcpy(media_volumes[4].label,"BOOT STICK");
    assert(media_read(d));
}
static void test_autoplay_startup(void)
{
    static struct desktop d;
    autoplay_test_main=pthread_self();
    fake_reset(); desk_init(&d,&fake_ops,640,480);
    d.autoplay_fs=&stalled_media_fs; d.autoplay_preference=stalled_media_preference;
    media_available=1;
    media_fixture=(struct ciuki_media_state){.version=CIUKI_MEDIA_VERSION,.size=sizeof(media_fixture)};
    /* Discovery during setup precedes both shell enable and presentation. */
    autoplay_test_mount(&d,77); assert(d.sh.autoplay.waiting==(1u<<4));
    desk_autoplay_service(&d); assert(!autoplay_test_scans && !d.sh.autoplay.open);
    desk_shell_enable(&d); assert(!desk_autoplay_prepare(&d));
    desk_autoplay_service(&d); assert(!autoplay_test_scans && !d.sh.autoplay.open);
    d.pixels=calloc(640*480,4); assert(d.pixels);
    /* Three independently held operations: the real wallpaper decoder's
     * pre-decode hook, cfg_writer's pipe-backed read, and media enumeration. */
    unsigned char bmp[70]={'B','M',70,0,0,0,0,0,0,0,54,0,0,0,40,0,0,0,
        2,0,0,0,2,0,0,0,1,0,24,0,0,0,0,0,16,0,0,0};
    char image[]="/tmp/f2-78-XXXXXX"; int image_fd=mkstemp(image); assert(image_fd>=0);
    assert(write(image_fd,bmp,sizeof(bmp))==sizeof(bmp) && !close(image_fd));
    snprintf(d.sh.cfg.wallpaper,sizeof(d.sh.cfg.wallpaper),"%s",image);
    memset(startup_us,0,sizeof(startup_us)); startup_begin=monotonic();
    wallpaper_load(&d); assert(!desk_redraw(&d) && d.presents); startup_stage("first_frame_presented");
    assert(!desk_wall_worker_start(&wallpaper_worker,NULL,wallpaper_test_stage)); wallpaper_worker_started=1;
    wallpaper_serial=desk_wall_worker_submit(&wallpaper_worker,&wallpaper_request);
    pthread_mutex_lock(&wallpaper_test_lock);
    while (!wallpaper_test_entered) pthread_cond_wait(&wallpaper_test_changed,&wallpaper_test_lock);
    pthread_mutex_unlock(&wallpaper_test_lock);
    int settings_pipe[2]; assert(!pipe(settings_pipe)); char settings_path[64];
    snprintf(settings_path,sizeof(settings_path),"/proc/self/fd/%d",settings_pipe[0]);
    assert(!cfg_writer_start(&settings_writer,settings_path,NULL)); settings_writer_started=1;
    assert(!cfg_writer_read(&settings_writer)); settings_reload_pending=1; settings_reload_request=0;
    for (unsigned i=0;;i++) {
        pthread_mutex_lock(&settings_writer.lock); int reading=settings_writer.activity==2; pthread_mutex_unlock(&settings_writer.lock);
        if (reading) break;
        assert(i<3000); struct timespec pause={0,1000000}; nanosleep(&pause,NULL);
    }
    autoplay_test_service(&d); autoplay_test_wait(&autoplay_test_scans,1);
    assert(!strcmp(desk_autoplay_activity(&d),"media-scan:root") && desk_autoplay_busy(&d));
    uint64_t frames=d.presents;
    for (unsigned i=0;i<8;i++) {
        input_tap(&d,0x15b); /* Start opens/closes using the production input path */
        autoplay_test_service(&d); assert(!d.sh.autoplay.open);
        fake_presents=0; assert(!desk_redraw(&d));
    }
    assert(d.presents>frames && d.sh.wallpaper_pending && settings_reload_pending);
    desk_menu_open(&d,SH_MENU_START,0,0);
    autoplay_test_release(&autoplay_test_scan_release); autoplay_test_wait(&autoplay_test_reads,1);
    assert(!strcmp(desk_autoplay_activity(&d),"preference-read"));
    autoplay_test_service(&d); assert(d.menu_open && !d.sh.autoplay.open);
    autoplay_test_release(&autoplay_test_read_release); autoplay_drain(&d);
    assert(d.sh.autoplay.phase==AP_READY && d.menu_open && !d.sh.autoplay.open);
    /* All I/O is complete, but an open menu still owns input. */
    autoplay_test_service(&d); assert(d.menu_open && !d.sh.autoplay.open);
    desk_menu_close(&d); autoplay_test_service(&d);
    assert(d.sh.autoplay.open && d.sh.autoplay.active.generation==77);
    input_tap(&d,DU_KEY_ESC);
    autoplay_test_mount(&d,78);
    d.clients[0].awaiting_frame=1;
    autoplay_test_service(&d); assert(d.sh.autoplay.waiting==(1u<<4) && !d.sh.autoplay.open);
    d.clients[0].awaiting_frame=0; d.clients[0].launch_frame_pending=1;
    autoplay_test_service(&d); assert(d.sh.autoplay.waiting==(1u<<4) && !d.sh.autoplay.open);
    d.clients[0].launch_frame_pending=0; autoplay_drain(&d);
    assert(d.sh.autoplay.open && d.sh.autoplay.active.generation==78);
    /* Eject/replacement during a blocked scan must discard the old result. */
    input_tap(&d,DU_KEY_ESC);
    pthread_mutex_lock(&autoplay_test_lock); autoplay_test_scan_release=0; pthread_mutex_unlock(&autoplay_test_lock);
    autoplay_test_mount(&d,79); autoplay_test_service(&d); autoplay_test_wait(&autoplay_test_scans,3);
    autoplay_test_mount(&d,80); autoplay_test_service(&d); assert(!d.sh.autoplay.open);
    autoplay_test_release(&autoplay_test_scan_release); autoplay_drain(&d);
    assert(!d.sh.autoplay.open && d.sh.autoplay.waiting==(1u<<4));
    autoplay_drain(&d); assert(d.sh.autoplay.open && d.sh.autoplay.active.generation==80);
    input_tap(&d,DU_KEY_ESC);
    pthread_mutex_lock(&autoplay_test_lock); autoplay_test_scan_release=0; pthread_mutex_unlock(&autoplay_test_lock);
    autoplay_test_mount(&d,81); autoplay_test_service(&d); autoplay_test_wait(&autoplay_test_scans,5);
    memset(&media_volumes[4],0,sizeof(media_volumes[4])); assert(media_read(&d));
    autoplay_test_release(&autoplay_test_scan_release); autoplay_drain(&d);
    assert(!d.sh.autoplay.open && !d.sh.autoplay.waiting);
    autoplay_test_mount(&d,82); autoplay_drain(&d); assert(d.sh.autoplay.open);
    d.sh.autoplay.selected=AP_NOTHING; input_tap(&d,DU_KEY_SPACE); input_tap(&d,DU_KEY_ENTER);
    autoplay_test_service(&d); assert(!d.sh.autoplay.open);
    /* A shutdown begun just after confirmation must still submit its save. */
    d.power.state=1; assert(!shutdown_wait(&d)); autoplay_test_wait(&autoplay_test_writes,1);
    assert(!strcmp(desk_autoplay_activity(&d),"preference-write"));
    d.power.state=0; desk_menu_open(&d,SH_MENU_START,0,0);
    autoplay_test_service(&d); assert(d.menu_open && !d.sh.autoplay.open);
    autoplay_test_release(&autoplay_test_write_release); autoplay_drain(&d);
    assert(d.menu_open && !desk_autoplay_busy(&d));
    char *settings_text=malloc(CFG_TEXT_MAX); assert(settings_text);
    size_t settings_size=cfg_format(&d.sh.cfg,settings_text,CFG_TEXT_MAX); assert(settings_size && settings_size<4096);
    assert(write(settings_pipe[1],settings_text,settings_size)==(ssize_t)settings_size); free(settings_text);
    assert(!close(settings_pipe[1]));
    pthread_mutex_lock(&wallpaper_test_lock); wallpaper_test_release=1;
    pthread_cond_signal(&wallpaper_test_changed); pthread_mutex_unlock(&wallpaper_test_lock);
    for (unsigned i=0;d.sh.wallpaper_pending || settings_reload_pending;i++) {
        assert(i<3000); autoplay_test_service(&d);
        struct timespec pause={0,1000000}; nanosleep(&pause,NULL);
    }
    assert(!d.sh.image_failed && d.menu_open);
    cfg_writer_stop(&settings_writer); settings_writer_started=0; assert(!close(settings_pipe[0]));
    desk_wall_worker_stop(&wallpaper_worker); wallpaper_worker_started=0; desk_autoplay_stop(&d);
    observers_close(&d); desk_menu_close(&d); desk_render_release(&d);
    free(d.pixels); free(d.sh.wall); free(d.sh.icons); free(d.sh.autoplay.workspace);
    media_available=0; memset(media_volumes,0,sizeof(media_volumes)); fake_reset(); assert(!unlink(image));
    printf("PASS F2-78 AutoPlay: setup media, concurrent held media/settings/wallpaper, menu and first launch presentation, stale replacement/eject, async save/shutdown; service_max_us=%llu\n",
        (unsigned long long)(autoplay_test_service_max/1000));
}
/* Ten seconds of actual shell frames: each listing call and entry costs one
 * 25 ms slow-stick command. Both workers must leave the frame thread free. */
static pthread_t slow_folder_frame;
static unsigned slow_folder_lists, slow_folder_revision;
static int slow_folder_polling;
static void slow_folder_pause(void)
{ struct timespec pause={0,25000000}; nanosleep(&pause,NULL); }
static int slow_folder_list(void *ctx,const char *path,unsigned flags,fm_emit emit,void *arg)
{
    (void)ctx; slow_folder_pause(); struct fm_meta meta={.type=FM_DIR};
    if (!strcmp(path,"/")) return emit("Documents",&meta,arg);
    if (!strcmp(path,"/Documents")) return emit("Desktop",&meta,arg);
    if (!strcmp(path,"/Documents/Trash") || !strcmp(path,"/RECYCLED")) return -ENOENT;   /* G2: the Recycle Bin icon's poll */
    int media=!strcmp(path,"/mnt/e");
    assert(media || !strcmp(path,DESK_FOLDER));
    if (media || (slow_folder_polling && flags==FM_LIST_NAMES_ONLY)) {
        assert(!pthread_equal(pthread_self(),slow_folder_frame));
        assert(flags==FM_LIST_NAMES_ONLY);
        if (!media) __atomic_add_fetch(&slow_folder_lists,1,__ATOMIC_RELAXED);
    }
    meta.type=FM_FILE; meta.size=100;
    unsigned revision=__atomic_load_n(&slow_folder_revision,__ATOMIC_RELAXED);
    for (unsigned i=0;i<(media ? 300u : 8u);i++) {
        char name[64]; snprintf(name,sizeof(name),"%s%u.txt",revision && !media ? "changed" : "file",i);
        slow_folder_pause(); int e=emit(name,&meta,arg); if (e) return e;
    }
    return 0;
}
static int slow_folder_stat(void *ctx,const char *path,struct fm_meta *meta)
{
    (void)ctx; if (!strcmp(path,"/RECYCLED/INFO2")) return -ENOENT;   /* G2: the Recycle Bin icon's poll */
    assert(!strcmp(path,DESK_FOLDER)); slow_folder_pause(); *meta=(struct fm_meta){.type=FM_DIR}; return 0;
}
static void test_slow_stick_folder(void)
{
    static const struct fm_vfs fs={.list=slow_folder_list,.stat_path=slow_folder_stat};
    static struct desktop d;
    fake_reset(); slow_folder_frame=pthread_self();
    desk_init(&d,&fake_ops,640,480); desk_shell_enable(&d);
    assert(!desk_folder_init(&d,&fs)); slow_folder_polling=1; d.autoplay_fs=&fs;
    assert(!desk_autoplay_prepare(&d));
    d.pixels=calloc(640*480,4); assert(d.pixels); assert(!desk_redraw(&d));
    desktop_watchdog.desktop=&d;
    struct ciuki_media_volume volumes[26]={{0}};
    uint64_t start=monotonic(), maximum=0; unsigned frames=0, mounted=0, changed=0;
    __atomic_store_n(&desktop_watchdog.frame_ms,(uint32_t)(start/1000000ull),__ATOMIC_RELEASE);
    while (monotonic()-start<10000000000ull) {
        uint64_t begin=monotonic(), elapsed=begin-start; desk_heartbeat(&d,begin);
        if (!mounted && elapsed>=1000000000ull) {
            mounted=1; volumes[4]=(struct ciuki_media_volume){.drive=4,.flags=CIUKI_MEDIA_REMOVABLE,.generation=1};
            desk_autoplay_update(&d,volumes);
        }
        if (!changed && elapsed>=4000000000ull) { changed=1; __atomic_store_n(&slow_folder_revision,1,__ATOMIC_RELAXED); }
        __atomic_store_n(&desktop_watchdog.loop_ms,(uint32_t)(begin/1000000ull),__ATOMIC_RELEASE);
        desktop_stage(STALL_SHELL); shell_service(&d);
        uint64_t service=monotonic()-begin; if (service>maximum) maximum=service;
        assert(service<100000000ull);
        char line[512]; int n=desktop_stall_record((uint32_t)(monotonic()/1000000ull),line,sizeof(line));
        assert(!n || !strstr(line,"wait=folder-service "));
        desktop_stage(STALL_RENDER);
        desk_damage_add(&d.damage,(struct desk_box){0,0,1,1},d.width,d.height);
        fake_presents=0;
        assert(!desk_redraw(&d)); frames++;
        __atomic_store_n(&desktop_watchdog.frame_ms,(uint32_t)(monotonic()/1000000ull),__ATOMIC_RELEASE);
        struct timespec pause={0,16000000}; nanosleep(&pause,NULL);
    }
    assert(frames>100 && mounted && __atomic_load_n(&slow_folder_lists,__ATOMIC_RELAXED)>=5);
    assert(d.sh.autoplay.open && !strcmp(d.sh.autoplay.content.budget,"time"));
    assert(!strcmp(d.sh.folder->model.entries[0].name,"changed0.txt"));
    for (int k=0;k<DESK_CLIENTS;k++) if (d.clients[k].internal) desk_drop_client(&d,k);
    desk_autoplay_stop(&d); desk_folder_destroy(&d); observers_close(&d); desk_render_release(&d);
    free(d.pixels); free(d.sh.icons); free(d.sh.autoplay.workspace); desktop_watchdog.desktop=NULL; fake_reset();
    printf("PASS F2-79 folder: 10 s slow-stick run, mount at 1 s, %u frames, service_max_us=%llu, no folder-service stall, external refresh\n",
        frames,(unsigned long long)(maximum/1000));
}
