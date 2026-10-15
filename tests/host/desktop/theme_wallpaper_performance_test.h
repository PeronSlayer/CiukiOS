/* SPDX-License-Identifier: MIT
 * Production service/worker/compositor replay at the T23 geometry. */
static pthread_t perf_main_thread;
static unsigned perf_main_renders, perf_decodes, perf_scales;
static uint64_t perf_max_turn;
static struct desktop *perf_desktop;
static uint32_t *perf_scanout;
static unsigned perf_full_frames;
static int perf_present(int display,int output,const struct ciuki_rect *r)
{
    int result=fake_ops.present(display,output,r);
    if (!result) for (unsigned y=0;y<r->height;y++)
        memcpy(perf_scanout+(size_t)(r->dst_y+y)*1024+r->dst_x,
               perf_desktop->pixels+(size_t)(r->src_y+y)*1024+r->src_x,(size_t)r->width*4);
    return result;
}
int __real_desk_wallpaper_render(uint32_t *,uint32_t,uint32_t,uint32_t,uint32_t,
    const char *,int,uint32_t,const struct desk_image *,const uint32_t *);
int __wrap_desk_wallpaper_render(uint32_t *out,uint32_t w,uint32_t h,uint32_t top,uint32_t bottom,
    const char *spec,int mode,uint32_t colour,const struct desk_image *image,const uint32_t *portrait)
{
    if (pthread_equal(pthread_self(),perf_main_thread)) perf_main_renders++;
    return __real_desk_wallpaper_render(out,w,h,top,bottom,spec,mode,colour,image,portrait);
}
static void perf_stage(const char *name)
{
    if (!strcmp(name,"wallpaper_decoded")) perf_decodes++;
    if (!strcmp(name,"wallpaper_scaled")) perf_scales++;
    /* A slow source must leave service turns free to handle input/frames. */
    if (!strcmp(name,"wallpaper_file_read")) {
        struct timespec pause={0,150000000}; nanosleep(&pause,NULL);
    }
}
static void perf_turn(struct desktop *d)
{
    for (int i=0;i<3;i++) {
        struct ciuki_message m; desk_message(&m,DESK_PONG,8);
        desk_put32(m.data+4,d->clients[i].serial); v2(d,i,500+(unsigned)i,&m); drain(d,i);
    }
    d->mouse_x=900+(int)(d->inputs&1);
    desk_damage_add(&d->damage,(struct desk_box){80,110,20,20},d->width,d->height);
    uint64_t begin=monotonic(); sequence_turn(d);
    uint64_t elapsed=monotonic()-begin;
    if (elapsed>perf_max_turn) perf_max_turn=elapsed;
    assert(elapsed<100000000ull);
    if (d->frame_stats.damage_px==1024u*768u) {
        assert(d->frame_stats.presents==1 && d->frame_stats.lfb_bytes==1024u*768u*4);
        perf_full_frames++;
    }
}
static void perf_wait(struct desktop *d)
{
    uint64_t deadline=monotonic()+5000000000ull;
    do {
        perf_turn(d);
        assert(monotonic()<deadline);
        struct timespec pause={0,16000000}; nanosleep(&pause,NULL);
    } while (settings_reload_pending || (d->sh.want_reload&1) || d->sh.wallpaper_pending);
    assert(!d->sh.image_failed && d->sh.wall_valid && d->scanout_valid);
}
static struct desk_frame_stats perf_window(struct desktop *d,const char *name)
{
    memset(&d->stats,0,sizeof(d->stats));
    unsigned rebuilds=d->sh.background_rebuilds;
    uint64_t chrome=d->chrome_rebuilds;
    uint64_t begin=monotonic();
    for (unsigned i=0;i<60;i++) {
        perf_turn(d);
        struct timespec pause={0,16000000}; nanosleep(&pause,NULL);
    }
    printf("REPLAY phase=%s frames=%llu presents=%llu lfb_bytes=%llu composite_us=%llu present_us=%llu elapsed_ms=%llu max_turn_us=%llu main_renders=%u decodes=%u scales=%u\n",
        name,(unsigned long long)d->stats.frames,(unsigned long long)d->stats.presents,
        (unsigned long long)d->stats.lfb_bytes,(unsigned long long)(d->stats.composite_ns/1000),
        (unsigned long long)(d->stats.present_ns/1000),(unsigned long long)((monotonic()-begin)/1000000),
        (unsigned long long)(perf_max_turn/1000),perf_main_renders,perf_decodes,perf_scales);
    fflush(stdout);
    assert(d->stats.frames==60 && d->sh.background_rebuilds==rebuilds && d->chrome_rebuilds==chrome);
    assert(!d->damage.count && !d->present_damage.count && !d->move_damage.count && !d->copy_pending);
    assert(d->scanout_valid && !d->hw_cursor && !desk_wall_worker_busy(&wallpaper_worker));
    return d->stats;
}
static void perf_exact(struct desktop *d,const char *name)
{
    size_t bytes=(size_t)d->width*d->height*4;
    uint32_t *reference=calloc((size_t)d->width*d->height,4); assert(reference);
    uint32_t *out=d->pixels, *background=d->sh.background;
    int valid=d->sh.background_valid,prepared=d->render_cache_prepared;
    unsigned rebuilds=d->sh.background_rebuilds;
    struct desk_render_cache *cache=d->render_cache;
    d->pixels=reference; d->sh.background=NULL; d->sh.background_valid=0;
    d->render_cache=NULL; d->render_cache_prepared=1; /* uncached theme painter oracle */
    desk_composite(d,(struct desk_box){0,0,(int)d->width,(int)d->height});
    free(d->sh.background); d->sh.background=background; d->sh.background_valid=valid;
    d->sh.background_rebuilds=rebuilds; d->render_cache=cache; d->render_cache_prepared=prepared; d->pixels=out;
    assert(!memcmp(out,reference,bytes)); free(reference);
    assert(!memcmp(out,perf_scanout,bytes));
    const char *dir=getenv("CIUKI_F270_FRAMES");
    if (dir) {
        char path[1024]; snprintf(path,sizeof(path),"%s/%s.xrgb",dir,name);
        FILE *f=fopen(path,"wb"); assert(f);
        assert(fwrite(out,1,bytes,f)==bytes && !fclose(f));
    }
}
static void test_theme_wallpaper_performance(const char *directory)
{
    alarm(23); perf_main_thread=pthread_self();
    static struct desktop d; fake_reset(); desk_init(&d,&fake_ops,1024,768);
    perf_desktop=&d; perf_scanout=calloc(1024u*768u,4); assert(perf_scanout);
    d.ops.present=perf_present;
    d.time_ns=monotonic; d.icon=store_icon; desk_shell_enable(&d); d.sh.gate=0;
    d.pixels=calloc((size_t)d.width*d.height,4); assert(d.pixels);
    uint32_t *portrait=malloc(256u*256u*4); assert(portrait);
    FILE *portrait_file=fopen(DU_ICON_PORTRAIT_PATH,"rb"); assert(portrait_file);
    assert(fread(portrait,4,256u*256u,portrait_file)==256u*256u && !fclose(portrait_file));
    d.portrait=portrait;
    for (unsigned i=0;i<3;i++) {
        fake_channel(60+(int)i); assert(desk_add_client(&d,60+(int)i)==(int)i);
        const char *title=i ? "Replay window" : "Control Panel";
        struct ciuki_message m; desk_message(&m,DESK_HELLO,4); v2(&d,(int)i,500+i,&m);
        fake_surface(70+(int)i,300,200,0x123456);
        unsigned n=(unsigned)strlen(title);
        desk_message(&m,DESK_CREATE_WINDOW,16+n); m.fd_count=1; m.fds[0]=70+(int)i;
        desk_put32(m.data+4,300); desk_put32(m.data+8,200); desk_put32(m.data+12,n);
        memcpy(m.data+16,title,n); v2(&d,(int)i,500+i,&m);
        d.clients[i].frame.x=60+(int)i*200; d.clients[i].frame.y=100+(int)i*100;
        drain(&d,(int)i);
    }
    d.clients[0].program=DESK_APP_CONTROL;
    char path[512],cache[512],cwd[512]; assert(getcwd(cwd,sizeof(cwd)));
    snprintf(path,sizeof(path),"%s/performance.cfg",directory);
    snprintf(cache,sizeof(cache),"%s/performance-cache",directory);
    sequence_settings_path=path;
    assert(!cfg_writer_start(&settings_writer,path,NULL)); settings_writer_started=1;
    assert(!desk_wall_worker_start(&wallpaper_worker,cache,perf_stage)); wallpaper_worker_started=1;
    snprintf(d.sh.cfg.wallpaper,sizeof(d.sh.cfg.wallpaper),"%s/assets/wallpapers/ciuki/Ciuk2.png",cwd);
    sequence_save(&d.sh.cfg); startup_stage("first_frame_presented"); wallpaper_load(&d); perf_wait(&d);
    perf_exact(&d,"baseline"); perf_main_renders=0;
    const struct desk_font *fonts[DESK_FONTS];
    for (int i=0;i<DESK_FONTS;i++) fonts[i]=desk_font(i);
    struct desk_frame_stats baseline=perf_window(&d,"baseline");
    const char *names[]={"theme","wallpaper","return","builtin","portrait"};
    for (unsigned change=0;change<5;change++) {
        struct ciuki_settings next=d.sh.cfg;
        if (!change) strcpy(next.theme,"Platinum");
        else if (change==1) { next.wallpaper_mode=CFG_WALL_FIT;
            snprintf(next.wallpaper,sizeof(next.wallpaper),"%s/assets/wallpapers/ciuki/Ciuk3.png",cwd); }
        else if (change==2) { strcpy(next.theme,DESK_THEME_DEFAULT); next.wallpaper_mode=CFG_WALL_FILL;
            snprintf(next.wallpaper,sizeof(next.wallpaper),"%s/assets/wallpapers/ciuki/Ciuk2.png",cwd); }
        else strcpy(next.wallpaper,change==3 ? "builtin:deepblue" : "ciuki");
        sequence_save(&next); sequence_reload(&d);
        perf_full_frames=0;
        uint64_t begin=monotonic(); perf_wait(&d);
        printf("CHANGE phase=%s worker_window_ms=%llu prepare_us=%llu full_frames=%u chrome_rebuilds=%llu main_renders=%u decodes=%u scales=%u\n",names[change],
            (unsigned long long)((monotonic()-begin)/1000000),(unsigned long long)(wallpaper_worker.prepare_ns/1000),
            perf_full_frames,(unsigned long long)d.chrome_rebuilds,perf_main_renders,perf_decodes,perf_scales);
        if (!change) assert(perf_decodes==1 && perf_scales==1 && perf_full_frames==1);
        if (change==1 || change>=3) assert(perf_full_frames==1);
        if (change==2) assert(perf_decodes==2 && perf_scales==2);
        perf_exact(&d,names[change]);
        begin=monotonic(); perf_turn(&d);
        assert(monotonic()-begin<1000000000ull);
        assert(d.frame_stats.lfb_bytes==baseline.lfb_bytes/60 && d.frame_stats.presents==baseline.presents/60);
        struct desk_frame_stats after=perf_window(&d,names[change]);
        assert(after.lfb_bytes==baseline.lfb_bytes && after.presents==baseline.presents && after.scene_px==baseline.scene_px);
        for (int i=0;i<DESK_FONTS;i++) assert(fonts[i]==desk_font(i));
    }
    printf("REPLAY settled main_renders=%u\n",perf_main_renders); fflush(stdout);
    assert(!perf_main_renders);
    uint64_t begin=monotonic();
    desk_damage_add(&d.damage,(struct desk_box){0,0,1024,768},1024,768);
    fake_fail_present=1; assert(desk_redraw(&d)==-1); fake_fail_present=0;
    assert(!d.scanout_valid && d.damage.count);
    perf_turn(&d); perf_turn(&d);
    assert(monotonic()-begin<1000000000ull && d.frame_stats.lfb_bytes==baseline.lfb_bytes/60);
    perf_exact(&d,"recovered");
    footprint_wait_writer(); desk_wall_worker_stop(&wallpaper_worker); wallpaper_worker_started=0;
    cfg_writer_stop(&settings_writer); settings_writer_started=0;
    drop_all(&d); desk_render_release(&d); free(d.sh.wall); free(d.pixels); free(portrait); free(perf_scanout); fake_reset(); alarm(0);
    puts("PASS theme/wallpaper performance: three windows, bounded turns, exact frames, baseline restored within one second, immutable font atlases");
}
