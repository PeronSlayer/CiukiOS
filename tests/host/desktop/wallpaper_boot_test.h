/* SPDX-License-Identifier: MIT
 * Normal shell_setup, first placeholder present, production service and
 * damage-only pacing. Device/filesystem boundaries are host fixtures. */
static const struct fm_vfs *boot_native_vfs(void) { return &footprint_fs; }
int ap_preference_get(const char *dir,const char *id)
{ (void)dir; (void)id; return -1; }
int ap_preference_set(const char *dir,const char *id,const char *label,int action)
{ (void)dir; (void)id; (void)label; (void)action; return -1; }
static uint64_t boot_scaled;
static unsigned boot_writes,boot_syncs,boot_cache_opens;
static int boot_readonly;
static void boot_wall_stage(const char *name)
{
    startup_stage(name);
    if (!strcmp(name,"wallpaper_scaled")) __atomic_store_n(&boot_scaled,monotonic(),__ATOMIC_RELEASE);
}
static int boot_cache_path(const char *path)
{
    return boot_cache_directory && !strncmp(path,boot_cache_directory,strlen(boot_cache_directory));
}
int __real_open(const char *,int,...);
int __wrap_open(const char *path,int flags,...)
{
    mode_t mode=0;
    if (flags&O_CREAT) { va_list ap; va_start(ap,flags); mode=(mode_t)va_arg(ap,int); va_end(ap); }
    if (boot_cache_path(path) && (flags&(O_WRONLY|O_RDWR|O_CREAT))) {
        boot_cache_opens++;
        if (boot_readonly) { errno=EROFS; return -1; }
    }
    return __real_open(path,flags,mode);
}
int __real_mkdir(const char *,mode_t);
int __wrap_mkdir(const char *path,mode_t mode)
{
    if (boot_cache_path(path) && boot_readonly) { errno=EROFS; return -1; }
    return __real_mkdir(path,mode);
}
static int boot_cache_fd(int fd)
{
    char link[64],path[1024]; snprintf(link,sizeof(link),"/proc/self/fd/%d",fd);
    ssize_t n=readlink(link,path,sizeof(path)-1); if (n<0) return 0;
    path[n]=0; return boot_cache_path(path);
}
ssize_t __real_write(int,const void *,size_t);
ssize_t __wrap_write(int fd,const void *data,size_t n)
{
    if (boot_cache_fd(fd)) {
        /* A short native write replaces the growing FAT file while holding
         * the shared VFS lock. One slow write suffices to expose the old
         * pre-publication dependency; no disk benchmark is claimed. */
        if (!boot_writes++) { struct timespec delay={1,200000000}; nanosleep(&delay,NULL); }
        if (n>4096) n=4096;
    }
    return __real_write(fd,data,n);
}
int __real_fsync(int);
int __wrap_fsync(int fd)
{ if (boot_cache_fd(fd)) boot_syncs++; return __real_fsync(fd); }
static void boot_turn(struct desktop *d)
{
    int pending=d->sh.wallpaper_pending;
    d->now_ns=monotonic(); desktop_stage(STALL_SHELL); shell_service(d);
    int published=pending && !d->sh.wallpaper_pending && d->damage.count;
    uint64_t before=d->presents; desktop_stage(STALL_RENDER);
    int result=desk_frame_due(d,monotonic()) ? desk_redraw(d) : -1;
    if (published) assert(!result && d->presents>before); /* completion's next frame */
    desktop_watchdog_frame(d,before,result);
    __atomic_store_n(&desktop_watchdog.loop_ms,(uint32_t)(monotonic()/1000000ull),__ATOMIC_RELEASE);
}
static void boot_wait(struct desktop *d,const char *name)
{
    uint64_t deadline=monotonic()+5000000000ull;
    do {
        boot_turn(d);
        uint64_t scaled=__atomic_load_n(&boot_scaled,__ATOMIC_ACQUIRE);
        if (scaled && d->sh.wallpaper_pending && monotonic()-scaled>=1000000000ull) {
            fprintf(stderr,"BOOT FAIL phase=%s: no publication within one second of wallpaper_scaled\n",name);
            abort();
        }
        assert(monotonic()<deadline);
        struct timespec pause={0,16000000}; nanosleep(&pause,NULL);
    } while (settings_reload_pending || d->sh.want_reload || d->sh.wallpaper_pending || d->damage.count);
    assert(!d->sh.image_failed && d->scanout_valid);
    uint64_t scaled=__atomic_load_n(&boot_scaled,__ATOMIC_ACQUIRE);
    if (scaled) assert(monotonic()-scaled<1000000000ull);
    /* The turn that consumes completion must also present the frame. */
    assert(!d->damage.count && !d->present_damage.count);
    printf("BOOT phase=%s scaled_to_visible_us=%llu presents=%llu cache_writes=%u cache_fsyncs=%u\n",name,
        (unsigned long long)(scaled ? (monotonic()-scaled)/1000 : 0),
        (unsigned long long)d->presents,boot_writes,boot_syncs); fflush(stdout);
}
static void boot_exact(struct desktop *d,const char *name)
{
    uint32_t *expected=NULL; int hit=0;
    assert(!desk_wall_prepare(&wallpaper_request,NULL,&expected,&hit,NULL));
    assert(!memcmp(expected,d->sh.wall,1024u*768u*4)); free(expected);
    perf_exact(d,name); /* independent full painter AND separately tracked scanout */
    const char *directory=getenv("CIUKI_F273_FRAMES"); assert(directory);
    char path[1024]; snprintf(path,sizeof(path),"%s/%s.xrgb",directory,name);
    FILE *f=fopen(path,"wb"); assert(f);
    assert(fwrite(perf_scanout,4,1024u*768u,f)==1024u*768u && !fclose(f));
}
static void test_wallpaper_boot(const char *directory)
{
    alarm(35);
    char cwd[512]; assert(getcwd(cwd,sizeof(cwd)));
    for (unsigned combination=0;combination<6;combination++) {
        int present=combination!=0 && combination!=2, seeded=combination>=4;
        boot_readonly=combination==2 || combination==3 || combination==5;
        boot_writes=boot_syncs=boot_cache_opens=0; boot_scaled=0;
        char cache[768],settings[768],name[80];
        snprintf(cache,sizeof(cache),"%s/boot-cache-%u",directory,combination);
        snprintf(settings,sizeof(settings),"%s/boot-%u.cfg",directory,combination);
        /* Four cold combinations plus two existing-seed hits. */
        snprintf(name,sizeof(name),"%s-%s%s",present ? "present" : "absent",boot_readonly ? "ro" : "rw",seeded ? "-seeded" : "");
        if (present) assert(!mkdir(cache,0755));
        sequence_settings_path=settings;
        memset(&settings_writer,0,sizeof(settings_writer));
        memset(&desktop_watchdog,0,sizeof(desktop_watchdog));
        memset(startup_us,0,sizeof(startup_us)); startup_cached=startup_sent=0;
        startup_begin=monotonic(); wallpaper_worker_started=wallpaper_worker_failed=0;
        settings_writer_started=settings_reload_pending=settings_reload_request=settings_reload_wallpaper=theme_read_pending=0;
        static struct desktop d; fake_reset(); desk_init(&d,&fake_ops,1024,768);
        perf_desktop=&d; perf_scanout=calloc(1024u*768u,4); assert(perf_scanout);
        d.ops.present=perf_present; d.time_ns=monotonic; d.pixels=calloc(1024u*768u,4); assert(d.pixels);
        uint32_t *portrait=malloc(256u*256u*4); assert(portrait);
        FILE *f=fopen(DU_ICON_PORTRAIT_PATH,"rb"); assert(f);
        assert(fread(portrait,4,256u*256u,f)==256u*256u && !fclose(f)); d.portrait=portrait;
        struct ciuki_settings cfg; cfg_defaults(&cfg);
        snprintf(cfg.wallpaper,sizeof(cfg.wallpaper),"%s/assets/wallpapers/ciuki/Ciuk2.png",cwd); sequence_save(&cfg);
        if (seeded) {
            struct desk_wall_request request={.width=1024,.height=768,.top=DESK_BAR,.bottom=DESK_BAR,
                .mode=cfg.wallpaper_mode,.colour=desk_theme_named(cfg.theme)->desktop};
            strcpy(request.path,cfg.wallpaper); uint32_t *pixels=NULL; int hit;
            assert(!desk_wall_prepare(&request,cache,&pixels,&hit,NULL)); free(pixels);
        }
        boot_cache_directory=cache;
        startup_stage("entry"); shell_setup(&d,0); d.realtime_second=12*3600;
        assert(wallpaper_worker_started && !wallpaper_serial && d.sh.wallpaper_pending);
        assert(!desk_redraw(&d)); startup_stage("first_frame_presented");
        boot_wait(&d,name); assert(startup_us[7]);
        assert(!!startup_us[9]==seeded && !!(startup_cached&(1u<<6))==seeded);
        boot_exact(&d,name);
        uint64_t before=d.presents;
        for (unsigned i=0;i<4;i++) boot_turn(&d);
        assert(d.presents==before); /* No artificial input or damage to unstick publication. */
        if (!combination) {
            struct ciuki_settings changed=d.sh.cfg; strcpy(changed.theme,"Platinum");
            sequence_save(&changed); d.sh.want_reload|=1; boot_scaled=0;
            boot_wait(&d,"theme"); assert(!strcmp(desk_theme->name,"Platinum")); boot_exact(&d,"theme");
            changed=d.sh.cfg; snprintf(changed.wallpaper,sizeof(changed.wallpaper),"%s/assets/wallpapers/ciuki/Ciuk3.png",cwd);
            sequence_save(&changed); d.sh.want_reload|=1; boot_scaled=0;
            boot_wait(&d,"wallpaper-change"); boot_exact(&d,"wallpaper-change");
            /* An equal allocated photo is not proof it reached scanout. */
            d.scanout_valid=0; d.sh.background_valid=0; d.damage.count=0;
            wallpaper_load(&d); boot_scaled=0;
            boot_wait(&d,"identical-unpresented"); boot_exact(&d,"identical-unpresented");
        }
        assert(!boot_writes && !boot_syncs && !boot_cache_opens);
        __atomic_store_n(&desktop_watchdog.stop,1,__ATOMIC_RELEASE);
        assert(!pthread_join(desktop_watchdog.thread,NULL)); desktop_watchdog.live=0;
        desk_wall_worker_stop(&wallpaper_worker); wallpaper_worker_started=0;
        cfg_writer_stop(&settings_writer); settings_writer_started=0;
        desk_autoplay_stop(&d);
        observers_close(&d);
        desk_folder_destroy(&d); drop_all(&d); desk_render_release(&d);
        free(d.sh.wall); free(d.sh.background); free(d.pixels); free(portrait); free(perf_scanout);
        boot_cache_directory=NULL; fake_reset();
    }
    alarm(0);
    puts("PASS wallpaper boot: four cold cache/permission combinations, two seed hits, damage-only publication, theme and wallpaper changes, exact scanout, no runtime cache writes");
}
