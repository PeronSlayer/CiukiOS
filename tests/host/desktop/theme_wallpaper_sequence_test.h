/* SPDX-License-Identifier: MIT
 * Production desktop.c harness: Control Panel reload, live settings/mixer,
 * sound and wallpaper workers. Included after memory_footprint_test.h. */
static pthread_mutex_t sequence_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sequence_changed=PTHREAD_COND_INITIALIZER;
static int sequence_job_entered, sequence_job_release;
static int sequence_wall_hold, sequence_wall_entered;
static void sequence_wall_stage(const char *name)
{
    startup_stage(name);
    if (strcmp(name,"wallpaper_scaled") && strcmp(name,"wallpaper_cache_read") &&
        strcmp(name,"wallpaper_memory_read") && strcmp(name,"wallpaper_layer_reused")) return;
    pthread_mutex_lock(&sequence_lock); sequence_wall_entered=1;
    pthread_cond_signal(&sequence_changed);
    while (sequence_wall_hold) pthread_cond_wait(&sequence_changed,&sequence_lock);
    pthread_mutex_unlock(&sequence_lock);
}
static int sequence_mixer_job(void *record,size_t n)
{
    pthread_mutex_lock(&sequence_lock); sequence_job_entered=1;
    pthread_cond_signal(&sequence_changed);
    while (!sequence_job_release) pthread_cond_wait(&sequence_changed,&sequence_lock);
    pthread_mutex_unlock(&sequence_lock);
    return mixer_job_run(record,n);
}
static void sequence_save(const struct ciuki_settings *cfg)
{
    char text[CFG_TEXT_MAX]; size_t n=cfg_format(cfg,text,sizeof(text)); assert(n);
    FILE *f=fopen(sequence_settings_path,"wb"); assert(f);
    assert(fwrite(text,1,n,f)==n && !fclose(f));
}
static void sequence_reload(struct desktop *d)
{
    struct ciuki_message m; desk_message(&m,DESK_SETTINGS,24);
    desk_put32(m.data+4,42); desk_put32(m.data+8,DESK_SETTINGS_RELOAD);
    v2(d,0,500,&m); assert(d->sh.want_reload&1); drain(d,0);
}
static void sequence_turn(struct desktop *d)
{
    uint64_t before=d->presents; desktop_stage(STALL_INPUT);
    d->now_ns=monotonic(); desk_heartbeat(d,d->now_ns);
    struct ciuki_input_event event={.type=CIUKI_INPUT_MOTION,.value=1,.monotonic_ns=d->now_ns};
    desk_input(d,&event); desk_sound_results(d); desktop_stage(STALL_THEME);
    if (!(d->sh.want_reload&1) && theme_poll_async(d->now_ns)) {
        snprintf(d->sh.cfg.theme,sizeof(d->sh.cfg.theme),"%s",desk_theme->name);
        desk_theme_changed(d); d->sh.wall_valid=0; wallpaper_update(d);
    }
    desktop_stage(STALL_SHELL); shell_service(d);
    desktop_stage(STALL_RENDER);
    fake_presents=0; assert(!desk_redraw(d));
    desktop_watchdog_frame(d,before,0);
    __atomic_store_n(&desktop_watchdog.loop_ms,(uint32_t)(monotonic()/1000000ull),__ATOMIC_RELEASE);
}
static void sequence_wait_theme(struct desktop *d,const char *name)
{
    uint64_t deadline=monotonic()+5000000000ull;
    do {
        sequence_turn(d); assert(monotonic()<deadline);
        struct timespec pause={0,1000000}; nanosleep(&pause,NULL);
    } while (strcmp(d->sh.cfg.theme,name) || settings_reload_pending);
    assert(!strcmp(desk_theme->name,name));
}
static void sequence_watchdog_test(struct desktop *d)
{
    char line[320];
    __atomic_store_n(&desktop_watchdog.frame_ms,1000,__ATOMIC_RELEASE);
    __atomic_store_n(&desktop_watchdog.loop_ms,3000,__ATOMIC_RELEASE);
    __atomic_store_n(&desktop_watchdog.pending,0,__ATOMIC_RELEASE);
    __atomic_store_n(&desktop_watchdog.work,0,__ATOMIC_RELEASE);
    assert(!desktop_stall_record(3000,line,sizeof(line))); /* healthy idle loop */
    __atomic_store_n(&desktop_watchdog.work,1,__ATOMIC_RELEASE);
    assert(desktop_stall_record(1501,line,sizeof(line))>0 && strstr(line,"input_pending=0 work_pending=1"));
    __atomic_store_n(&desktop_watchdog.work,0,__ATOMIC_RELEASE);
    desktop_input_pending(); desktop_stage(STALL_RENDER);
    assert(!desktop_stall_record(1500,line,sizeof(line)));
    assert(desktop_stall_record(1501,line,sizeof(line))>0);
    assert(strstr(line,"[desktop] stall stage=render worker=") && strstr(line,"input_pending=1"));
    assert(strstr(line,"wait=render") && strstr(line,"wallpaper_wait="));
    desktop_stage(STALL_SHELL); desktop_wait("folder-service");
    assert(desktop_stall_record(1501,line,sizeof(line))>0 && strstr(line,"wait=folder-service"));
    pthread_mutex_lock(&wallpaper_worker.lock);
    assert(desktop_stall_record(1501,line,sizeof(line))>0 && strstr(line,"worker=wallpaper-lock/"));
    pthread_mutex_unlock(&wallpaper_worker.lock);
    assert(!desktop_stall_record(1501,line,8)); /* never return an oversized raw write length */
    __atomic_store_n(&desktop_watchdog.frame_ms,UINT32_MAX-999,__ATOMIC_RELEASE);
    assert(desktop_stall_record(1000,line,sizeof(line))>0); /* millisecond wrap */
    __atomic_store_n(&desktop_watchdog.pending,0,__ATOMIC_RELEASE);
    __atomic_store_n(&desktop_watchdog.loop_ms,1000,__ATOMIC_RELEASE);
    __atomic_store_n(&desktop_watchdog.frame_ms,1000,__ATOMIC_RELEASE);
    assert(desktop_stall_record(3000,line,sizeof(line))>0); /* blocked before draining input */

    /* Exercise the actual observer while the main thread produces no frame;
     * its console write must bypass the busy settings/mixer worker. */
    int capture[2]; assert(!pipe(capture)); int saved=dup(STDERR_FILENO); assert(saved>=0);
    assert(dup2(capture[1],STDERR_FILENO)>=0); close(capture[1]);
    desktop_watchdog_start(d); assert(desktop_watchdog.live);
    desktop_input_pending(); desktop_stage(STALL_RENDER);
    struct timespec pause={0,750000000}; nanosleep(&pause,NULL);
    __atomic_store_n(&desktop_watchdog.stop,1,__ATOMIC_RELEASE);
    assert(!pthread_join(desktop_watchdog.thread,NULL)); desktop_watchdog.live=0;
    assert(dup2(saved,STDERR_FILENO)>=0); close(saved);
    ssize_t n=read(capture[0],line,sizeof(line)-1); close(capture[0]); assert(n>0); line[n]=0;
    assert(strstr(line,"[desktop] stall stage=render worker=idle/mixer") && strstr(line,"input_pending=1"));
    printf("WATCHDOG %s",line);
    d->input_pending=NULL;
}
static void sequence_wait_wall(struct desktop *d)
{
    uint64_t deadline=monotonic()+5000000000ull;
    do {
        sequence_turn(d); assert(monotonic()<deadline);
        struct timespec pause={0,1000000}; nanosleep(&pause,NULL);
    } while (d->sh.wallpaper_pending);
    assert(!d->sh.image_failed && d->sh.wall_valid);
}
static void test_theme_wallpaper(const char *directory)
{
    alarm(20); /* An actual deadlock must fail even if the service never returns. */
    static struct desktop d; fake_reset(); desk_init(&d,&fake_ops,1024,768);
    d.icon=store_icon; desk_shell_enable(&d); d.sh.gate=0;
    d.pixels=calloc((size_t)d.width*d.height,4); assert(d.pixels);
    fake_channel(60); assert(desk_add_client(&d,60)==0);
    window(&d,0,500,70,"Control Panel"); d.clients[0].program=DESK_APP_CONTROL;
    char settings_path[512]; snprintf(settings_path,sizeof(settings_path),"%s/desktop.cfg",directory);
    sequence_settings_path=settings_path; footprint_registry_directory=directory;
    settings_writer.write=footprint_settings_write; settings_writer.run_job=sequence_mixer_job;
    assert(!cfg_writer_start(&settings_writer,settings_path,NULL)); settings_writer_started=1;
    assert(!desk_wall_worker_start(&wallpaper_worker,NULL,sequence_wall_stage)); wallpaper_worker_started=1;
    desk_sound_init(&d); assert(d.sound_event && d.sound_request); /* real third thread */
    d.mixer_fd=MIXER_FD;
    mixer_fixture=(struct ciuki_mixer_master){.version=1,.size=sizeof(mixer_fixture),.left=20,.right=20};
    char cwd[512]; assert(getcwd(cwd,sizeof(cwd)));
    int path_length=snprintf(d.sh.cfg.wallpaper,sizeof(d.sh.cfg.wallpaper),"%s/assets/wallpapers/ciuki/Ciuk2.png",cwd);
    assert(path_length>0 && (size_t)path_length<sizeof(d.sh.cfg.wallpaper));
    sequence_save(&d.sh.cfg); startup_stage("first_frame_presented"); wallpaper_load(&d);
    sequence_wait_wall(&d); assert(d.presents);
    size_t bytes=(size_t)d.width*d.height*4;
    uint32_t *retained=malloc(bytes); assert(retained); memcpy(retained,d.sh.wall,bytes);
    uint32_t *original=d.sh.wall;
    struct du_icon_stats before,after; du_icon_snapshot(&before);
    strcpy(d.sh.cfg.theme,"Ciuki Blue");
    uint64_t begin=monotonic(); desk_shell_apply(&d); wallpaper_update(&d);
    uint64_t theme_ns=monotonic()-begin;
    du_icon_snapshot(&after);
    printf("THEME switch_us=%llu icon_cache_before=%u icon_cache_after=%u retained=%d\n",
        (unsigned long long)(theme_ns/1000),before.cached,after.cached,
        d.sh.wall==original && !memcmp(retained,d.sh.wall,bytes)); fflush(stdout);
    assert(d.sh.wall==original && !memcmp(retained,d.sh.wall,bytes));
    assert(theme_ns<100000000ull && before.cached==after.cached);
    begin=monotonic(); sequence_turn(&d);
    printf("THEME frame_us=%llu\n",(unsigned long long)((monotonic()-begin)/1000));
    assert(monotonic()-begin<500000000ull);
    sequence_wait_wall(&d);

    struct ciuki_settings changed=d.sh.cfg; strcpy(changed.theme,"Platinum");
    sequence_save(&changed); sequence_reload(&d); sequence_wait_theme(&d,"Platinum"); sequence_wait_wall(&d);

    /* A mixer durable save occupying the settings thread must not delay a
     * Control Panel settings reload on the input/render thread. */
    assert(!mixer_apply(&d,35,65,0));
    pthread_mutex_lock(&sequence_lock);
    while (!sequence_job_entered) pthread_cond_wait(&sequence_changed,&sequence_lock);
    pthread_mutex_unlock(&sequence_lock);
    strcpy(changed.theme,"Ciuki Blue"); sequence_save(&changed); sequence_reload(&d);
    uint64_t presents=d.presents, inputs=d.inputs;
    for (unsigned i=0;i<8;i++) {
        desk_damage_add(&d.damage,(struct desk_box){0,0,16,16},d.width,d.height);
        sequence_turn(&d);
    }
    assert(!sequence_main_reads && d.presents>presents && d.inputs>inputs);
    assert(!strcmp(d.sh.cfg.theme,"Platinum"));
    sequence_watchdog_test(&d);
    pthread_mutex_lock(&sequence_lock); sequence_wall_hold=1; sequence_wall_entered=0;
    pthread_mutex_unlock(&sequence_lock);
    pthread_mutex_lock(&sequence_lock); sequence_job_release=1;
    pthread_cond_signal(&sequence_changed); pthread_mutex_unlock(&sequence_lock);
    sequence_wait_theme(&d,"Ciuki Blue"); assert(d.sh.wallpaper_pending);
    pthread_mutex_lock(&sequence_lock);
    while (!sequence_wall_entered) pthread_cond_wait(&sequence_changed,&sequence_lock);
    pthread_mutex_unlock(&sequence_lock); /* supersede an actual in-flight render */
    uint32_t *theme_wall=d.sh.wall; memcpy(retained,d.sh.wall,bytes);
    unsigned theme_serial=wallpaper_serial;
    /* The wallpaper dialog now selects a different actual photograph. Its
     * latest request must supersede the still-blocked theme recolouring. */
    changed=d.sh.cfg; changed.wallpaper_mode=CFG_WALL_FIT;
    path_length=snprintf(changed.wallpaper,sizeof(changed.wallpaper),"%s/assets/wallpapers/ciuki/Ciuk3.png",cwd);
    assert(path_length>0 && (size_t)path_length<sizeof(changed.wallpaper));
    sequence_save(&changed); sequence_reload(&d);
    uint64_t deadline=monotonic()+5000000000ull;
    while (strcmp(d.sh.cfg.wallpaper,changed.wallpaper) || settings_reload_pending) {
        sequence_turn(&d); assert(monotonic()<deadline);
        struct timespec pause={0,1000000}; nanosleep(&pause,NULL);
    }
    assert(d.sh.wall==theme_wall && !memcmp(retained,d.sh.wall,bytes));
    assert(wallpaper_serial>theme_serial && d.sh.wallpaper_pending);
    pthread_mutex_lock(&sequence_lock); sequence_wall_hold=0;
    pthread_cond_signal(&sequence_changed); pthread_mutex_unlock(&sequence_lock);
    sequence_wait_wall(&d); assert(!strcmp(desk_theme->name,"Ciuki Blue"));
    assert(memcmp(retained,d.sh.wall,bytes));
    struct desk_wall_request expected=wallpaper_request; uint32_t *pixels=NULL; int hit=0;
    assert(!desk_wall_prepare(&expected,NULL,&pixels,&hit,NULL));
    assert(!memcmp(pixels,d.sh.wall,bytes)); free(pixels); /* exact final photograph/colour/mode */
    /* Retry a latest wallpaper request after the worker is suspended with
     * its mailbox locked. A failed submission must not reuse an old serial. */
    pthread_mutex_lock(&wallpaper_worker.lock);
    wallpaper_load(&d); assert(!wallpaper_serial && d.sh.wallpaper_pending);
    uint32_t *retry_wall=d.sh.wall;
    wallpaper_poll(&d); assert(!wallpaper_serial && d.sh.wall==retry_wall);
    pthread_mutex_unlock(&wallpaper_worker.lock);
    sequence_wait_wall(&d); assert(wallpaper_serial);

    /* An explicit settings reload must retry rather than wait on a lock held
     * by a suspended writer. Never consume its previous ready snapshot. */
    footprint_wait_writer();
    pthread_mutex_lock(&settings_writer.lock);
    settings_writer.read_ready=1; settings_writer.read_length=14;
    memcpy(settings_writer.read_text,"theme=Platinum",14);
    sequence_reload(&d); sequence_turn(&d);
    assert(settings_reload_request && settings_reload_pending && !sequence_main_reads);
    pthread_mutex_unlock(&settings_writer.lock);
    sequence_wait_theme(&d,"Ciuki Blue"); assert(!sequence_main_reads);
    assert(!sequence_main_reads);
    free(retained); footprint_wait_writer();
    desk_wall_worker_stop(&wallpaper_worker); wallpaper_worker_started=0;
    cfg_writer_stop(&settings_writer); settings_writer_started=0;
    desk_drop_client(&d,0); desk_render_release(&d); free(d.sh.wall); free(d.sh.background); free(d.pixels);
    fake_reset(); alarm(0);
    puts("PASS theme/wallpaper sequence: retained photo, bounded switch, unchanged icon cache, live workers, blocked mixer save leaves input/frames alive, latest settings applied");
}
