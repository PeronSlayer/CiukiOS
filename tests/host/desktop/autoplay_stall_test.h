/* SPDX-License-Identifier: MIT
 * Production shell service with deliberately stalled media/settings/wallpaper
 * I/O. Included in shell_launch_test.c; no guest or device emulation. */
static pthread_mutex_t autoplay_test_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t autoplay_test_changed=PTHREAD_COND_INITIALIZER;
static pthread_t autoplay_test_main;
static unsigned autoplay_test_scans, autoplay_test_reads, autoplay_test_writes;
static int autoplay_test_scan_release, autoplay_test_read_release, autoplay_test_write_release;
static uint64_t autoplay_test_service_max;
static int stalled_media_list(void *ctx,const char *path,fm_emit emit,void *user)
{
    (void)ctx; (void)emit; (void)user; assert(!strcmp(path,"/mnt/e"));
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
    assert(!strcmp(desk_autoplay_activity(&d),"media-scan") && desk_autoplay_busy(&d));
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
