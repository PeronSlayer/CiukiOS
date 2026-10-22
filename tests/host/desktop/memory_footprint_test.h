/* SPDX-License-Identifier: MIT
 * Included by the production desktop.c launcher harness. Fixture memory is
 * allocated before tracing; only production units use footprint_alloc.h. */
#include "footprint_alloc.h"
#include "autoplay_wait.h"
#undef malloc
#undef calloc
#undef realloc
#undef strdup
static int footprint_list(void *ctx,const char *path,unsigned flags,fm_emit emit,void *arg)
{
    (void)ctx; (void)flags;
    struct fm_meta dir={.type=FM_DIR};
    if (!strcmp(path,"/")) return emit("Documents",&dir,arg);
    if (!strcmp(path,"/Documents")) return emit("Desktop",&dir,arg);
    if (!strcmp(path,DESK_FOLDER)) {
        struct fm_meta meta={.type=FM_FILE,.size=4};
        return emit("tune.wav",&meta,arg);
    }
    assert(!strcmp(path,"/mnt/e"));
    struct fm_meta meta={.type=FM_FILE,.size=4};
    return emit("tune.wav",&meta,arg);
}
static int footprint_stat(void *ctx,const char *path,struct fm_meta *meta)
{ (void)ctx; assert(!strcmp(path,DESK_FOLDER)); *meta=(struct fm_meta){.type=FM_DIR}; return 0; }
static const struct fm_vfs footprint_fs={.list=footprint_list,.stat_path=footprint_stat};
static void footprint_wait_writer(void)
{
    uint64_t deadline=monotonic()+3000000000ull;
    while (cfg_writer_busy(&settings_writer)) {
        assert(monotonic()<deadline);
        struct timespec pause={0,1000000}; nanosleep(&pause,NULL);
    }
}
static unsigned footprint_settings_saves;
static int footprint_settings_write(struct cfg_writer *w,const char *text,size_t n)
{
    assert(w==&settings_writer && n && n<CFG_TEXT_MAX);
    assert(pthread_equal(pthread_self(),w->thread));
    FILE *f=fopen(w->path,"wb"); assert(f);
    assert(fwrite(text,1,n,f)==n && !fclose(f)); footprint_settings_saves++; return 0;
}
static unsigned footprint_sounds[SOUND_EVENTS];
static void footprint_sound(struct desktop *d,unsigned event)
{ (void)d; assert(event<SOUND_EVENTS); footprint_sounds[event]++; }
static void footprint_compare_painter(struct desktop *d)
{
    static struct desktop reference;
    reference=*d; reference.render_cache=NULL; reference.render_cache_prepared=1;
    size_t bytes=(size_t)d->width*d->height*4;
    reference.pixels=malloc(bytes); assert(reference.pixels);
    desk_composite(&reference,(struct desk_box){0,0,(int)d->width,(int)d->height});
    assert(!memcmp(d->pixels,reference.pixels,bytes)); free(reference.pixels);
}
static void footprint_frame(struct desktop *d)
{
    fake_presents=0;
    desk_damage_add(&d->damage,(struct desk_box){0,0,(int)d->width,(int)d->height},d->width,d->height);
    assert(!desk_redraw(d));
    assert(d->presents);
}
static void footprint_client(struct desktop *d,int slot,unsigned w,unsigned h)
{
    int channel=20+slot, surface=60+slot;
    fake_channel(channel); assert(desk_add_client(d,channel)==slot);
    struct ciuki_message m;
    desk_message(&m,DESK_HELLO,4); v2(d,slot,200+slot,&m);
    /* Surface creation belongs to the client/kernel, not the desktop heap. */
    fake_surface(surface,w,h,0x123456);
    static unsigned victims;
    const char *title=slot ? gate_faults[victims++%5] : "Ciuki Demo";
    unsigned length=(unsigned)strlen(title);
    desk_message(&m,DESK_CREATE_WINDOW,16+length); m.fd_count=1; m.fds[0]=surface;
    desk_put32(m.data+4,w); desk_put32(m.data+8,h); desk_put32(m.data+12,length); memcpy(m.data+16,title,length);
    v2(d,slot,200+slot,&m); assert(d->clients[slot].window);
    footprint_frame(d);
}
static void test_memory_footprint(const char *directory)
{
    static struct desktop d;
    int audit=getenv("CIUKI_FOOTPRINT_AUDIT")!=NULL;
    fake_reset(); desk_init(&d,&fake_ops,1024,768); d.sh.gate=1; d.icon=store_icon; desk_shell_enable(&d);
    footprint_registry_directory=directory;
    char settings_path[512]; snprintf(settings_path,sizeof(settings_path),"%s/desktop.cfg",directory);
    settings_writer.write=footprint_settings_write; settings_writer.run_job=mixer_job_run;
    assert(!cfg_writer_start(&settings_writer,settings_path,NULL)); settings_writer_started=1;
    assert(!desk_wall_worker_start(&wallpaper_worker,NULL,NULL)); wallpaper_worker_started=1;
    assert(!desk_folder_init(&d,&footprint_fs));
    d.pixels=calloc((size_t)d.width*d.height,4); assert(d.pixels);
    d.sh.gate=0; d.autoplay_fs=&footprint_fs;
    assert(!desk_autoplay_prepare(&d)); d.sh.gate=1;
    (void)desk_shell_wallpaper_update(&d); assert(d.sh.wall_valid); footprint_frame(&d);
    struct du_icon_stats prepared_icons; du_icon_snapshot(&prepared_icons);
    assert(prepared_icons.cached);
    footprint_begin("survivor_after_first_frame");
    footprint_client(&d,0,240,160);
    unsigned allocations=footprint_end();
    /* The probe snapshots here, after the survivor's first frame. */
    footprint_begin("crash_cycle");
    footprint_client(&d,1,240,160); desk_drop_client(&d,1); footprint_frame(&d);
    footprint_client(&d,1,400,300); desk_drop_client(&d,1); footprint_frame(&d);
    assert(d.clients[0].window && !d.clients[1].window);
    for (unsigned tick=1;tick<=16;tick++) {
        desk_heartbeat(&d,(uint64_t)tick*250000000ull);
        desk_shell_tick(&d); shell_service(&d); footprint_frame(&d);
        gate_report(&d,0,tick,200,0,tick);
    }
    allocations+=footprint_end();
    /* Exercise the ordinary shell's actual periodic services and copied jobs.
     * Gate mode skips these writers; each was missing from the old audit. */
    d.sh.gate=0; d.mixer_fd=MIXER_FD;
    mixer_fixture=(struct ciuki_mixer_master){.version=CIUKI_AUDIO_VERSION,.size=sizeof(mixer_fixture),
        .left=20,.right=20,.save_state=CIUKI_MIXER_SAVED};
    network_available=1;
    network_fixture=(struct ciuki_net_state){.version=CIUKI_NET_VERSION,.size=sizeof(network_fixture),
        .present=1,.link=CIUKI_NET_LINK_UP,.state=CIUKI_NET_RUNNING,.driver="e100"};
    footprint_begin("periodic_services");
    startup_stage("first_frame_presented");
    struct desk_wall_request missing={.width=d.width,.height=d.height,.mode=CFG_WALL_FILL};
    snprintf(missing.path,sizeof(missing.path),"%s/missing.png",directory);
    unsigned wall_serial=desk_wall_worker_submit(&wallpaper_worker,&missing);
    uint32_t *wall_pixels=NULL; int wall_error=0,wall_hit=0;
    uint64_t wall_deadline=monotonic()+3000000000ull;
    while (!desk_wall_worker_take(&wallpaper_worker,wall_serial,&wall_pixels,&wall_error,&wall_hit)) {
        assert(monotonic()<wall_deadline); struct timespec pause={0,1000000}; nanosleep(&pause,NULL);
    }
    assert(wall_error==ENOENT && !wall_pixels && !wall_hit);
    uint64_t poll_next=0;
    unsigned before_queries=mixer_queries;
    for (unsigned tick=0;tick<20;tick++) {
        uint64_t now=5000000000ull+(uint64_t)tick*250000000ull;
        desk_heartbeat(&d,now); mixer_poll(&d,now,&poll_next);
        desk_shell_tick(&d); shell_service(&d); display_poll(&d);
        uint64_t previous=d.realtime_second; d.realtime_second++;
        desk_shell_clock_damage(&d,previous); footprint_frame(&d);
        mixer_poll(&d,now+249999999ull,&poll_next); assert(!(d.sh.want_reload&4));
    }
    assert(mixer_queries==before_queries+20 && !footprint_master_saves);
    /* f1-100: the 250 ms observation never asks for a hardware (EC) read;
     * opening the volume popup asks once, on the control handle only. */
    assert(!mixer_refreshes);
    d.sh.want_reload|=4|8; shell_service(&d);
    assert(mixer_refreshes==1 && mixer_queries==before_queries+21 && !d.sh.want_reload);
    mixer_read(&d); assert(mixer_refreshes==1);
    assert(network_queries && d.sh.network.present && d.sh.network.link==CIUKI_NET_LINK_UP);
    d.sh.volume_open=1; mixer_fixture.left=60; mixer_fixture.right=80; mixer_fixture.mute=1;
    mixer_fixture.save_state=CIUKI_MIXER_UNSAVED;
    mixer_read(&d); assert(d.sh.volume==70 && d.sh.muted && d.damage.count);
    footprint_wait_writer(); shell_service(&d);
    assert(footprint_master_saves==1 && !mixer_sets);
    mixer_read(&d); footprint_wait_writer(); shell_service(&d);
    assert(footprint_master_saves==1); /* unchanged EC observation never saves twice */
    assert(!mixer_apply(&d,25,75,0)); footprint_wait_writer(); shell_service(&d);
    assert(footprint_master_saves==2 && mixer_sets==1 && d.sh.volume==50 && !d.sh.muted);
    char missing_registry[512]; snprintf(missing_registry,sizeof(missing_registry),"%s/missing/registry",directory);
    footprint_registry_directory=missing_registry;
    assert(!mixer_apply(&d,40,40,0)); footprint_wait_writer(); shell_service(&d);
    assert(d.sh.notification.active && strstr(d.sh.notification.title,"Volume applied but not saved"));
    assert(strstr(d.sh.notification.text,"No such file or directory (2)"));
    footprint_registry_directory=directory;
    assert(!mixer_apply(&d,25,75,0)); footprint_wait_writer(); shell_service(&d);
    assert(footprint_master_saves==4 && mixer_sets==3);
    assert(!settings_save(&d)); footprint_wait_writer(); shell_service(&d);
    assert(footprint_settings_saves==1);
    (void)theme_poll_async(d.now_ns); footprint_wait_writer(); (void)theme_poll_async(d.now_ns);
    for (unsigned stage=0;stage<CFG_STARTUP_STAGES;stage++) cfg_writer_stage(&settings_writer,stage,stage,0);
    footprint_wait_writer();
    char log[CFG_LOG_MAX+200]; memset(log,'L',sizeof(log)-1); log[sizeof(log)-1]=0;
    cfg_writer_log(&settings_writer,log); footprint_wait_writer();
    assert(strlen(settings_writer.log)==CFG_LOG_MAX-1);
    footprint_client(&d,1,400,300); d.clients[1].gfxdemo=1; d.clients[1].minor=1;
    struct ciuki_message frame; desk_message(&frame,DESK_GFX_FRAME,84); frame.data[2]=1;
    desk_put32(frame.data+4,1); desk_put32(frame.data+16,320); desk_put32(frame.data+20,240);
    desk_put32(frame.data+24,320); desk_put32(frame.data+28,240);
    desk_put32(frame.data+40,320); desk_put32(frame.data+44,240);
    desk_put32(frame.data+68,2); desk_put32(frame.data+72,11);
    desk_put32(frame.data+76,12); desk_put32(frame.data+80,13);
    v2(&d,1,201,&frame); assert(d.clients[1].gfx_pending); footprint_frame(&d);
    assert(!d.clients[1].gfx_pending && d.gfx_sample.app_serial==2);
    verbose_boot=1; performance_sample(&d,d.now_ns); performance_sample(&d,d.now_ns+2000000000ull);
    footprint_wait_writer(); assert(strstr(settings_writer.log,"app_serial=2")); verbose_boot=0;
    desk_drop_client(&d,1); d.sh.volume_open=0; footprint_frame(&d);
    allocations+=footprint_end();
    struct reg_root *saved=reg_open_root_at("System",directory); assert(saved);
    int32_t packed; assert(!reg_get_int32(saved,CIUKI_MASTER_KEY,CIUKI_MASTER_VALUE,&packed));
    assert(packed==ciuki_master_pack(25,75,0)); reg_close_root(saved);
    cfg_writer_stop(&settings_writer); settings_writer_started=0;
    desk_wall_worker_stop(&wallpaper_worker); wallpaper_worker_started=0;
    desk_folder_destroy(&d);
    network_available=0;
    footprint_begin("all_slots_resize_recover");
    for (int i=1;i<DESK_CLIENTS;i++) footprint_client(&d,i,240,160);
    for (int i=1;i<DESK_CLIENTS;i++) desk_drop_client(&d,i);
    footprint_frame(&d);
    for (int i=1;i<DESK_CLIENTS;i++) footprint_client(&d,i,640,480);
    for (int i=1;i<DESK_CLIENTS;i++) desk_drop_client(&d,i);
    footprint_frame(&d);
    footprint_compare_painter(&d);
    footprint_client(&d,1,1018,741); footprint_compare_painter(&d); /* exact pool bounds */
    desk_drop_client(&d,1);
    footprint_client(&d,1,1064,480); footprint_compare_painter(&d); /* too wide: painter */
    desk_drop_client(&d,1);
    footprint_client(&d,1,400,800); footprint_compare_painter(&d); /* too tall: painter */
    desk_drop_client(&d,1); footprint_frame(&d);
    allocations+=footprint_end();
    /* Gate disables these services; explicitly exercise the normal shell too. */
    d.sh.gate=0; d.autoplay_fs=&footprint_fs; d.sound_event=footprint_sound; d.media_sound=media_sound;
    footprint_begin("menu_volume_media");
    desk_menu_open(&d,SH_MENU_START,0,0); footprint_frame(&d); desk_menu_close(&d); footprint_frame(&d);
    d.sh.volume_open=1; d.sh.mixer_state=2; d.sh.volume=60; footprint_frame(&d);
    desk_sound(&d,SOUND_VOLUME); d.sh.volume_open=0;
    struct ciuki_media_state state={.version=CIUKI_MEDIA_VERSION,.size=sizeof(state),.removable_mask=1u<<4};
    state.type[4]=CIUKI_MEDIA_REMOVABLE; state.generation[4]=1;
    desk_shell_media_update(&d,&state);
    struct ciuki_media_volume volumes[26]={{0}};
    volumes[4]=(struct ciuki_media_volume){.version=CIUKI_MEDIA_VERSION,.size=sizeof(volumes[4]),
        .drive=4,.flags=CIUKI_MEDIA_REMOVABLE,.generation=1};
    desk_autoplay_update(&d,volumes); desk_autoplay_service(&d);
    assert(!d.sh.autoplay.open && d.sh.autoplay.waiting==(1u<<4));
    /* The surviving client retains input until focus returns to the desktop. */
    desk_focus(&d,-1); autoplay_drain(&d); assert(d.sh.autoplay.open);
    assert(!strcmp(d.sh.autoplay.content.music,"/mnt/e")); footprint_frame(&d);
    memset(volumes,0,sizeof(volumes)); desk_autoplay_update(&d,volumes); desk_autoplay_service(&d);
    assert(!d.sh.autoplay.open); footprint_frame(&d);
    assert(footprint_sounds[SOUND_MENU_OPEN] && footprint_sounds[SOUND_VOLUME] &&
        footprint_sounds[SOUND_DEVICE_CONNECT] && footprint_sounds[SOUND_DEVICE_DISCONNECT]);
    allocations+=footprint_end();
    if (!audit) assert(!allocations);
    struct du_icon_stats final_icons; du_icon_snapshot(&final_icons);
    assert(final_icons.heap_bytes==prepared_icons.heap_bytes && final_icons.cached==prepared_icons.cached &&
        final_icons.store_bytes==prepared_icons.store_bytes);
    printf("ICON_POOL cached=%u heap_bytes=%u peak_bytes=%u\n",prepared_icons.cached,
        prepared_icons.heap_bytes,prepared_icons.peak_bytes);
    desk_autoplay_stop(&d);
    drop_all(&d); free(d.pixels); free(d.sh.icons); free(d.sh.wall); free(d.sh.autoplay.workspace); fake_reset();
    printf("%s desktop footprint: allocations_after_first_frame=%u\n",audit ? "AUDIT" : "PASS",allocations);
    if (!audit) {
        /* Allocation failure must stay on the identical painter fallback;
         * client arrivals must not retry and grow the heap later. */
        desk_init(&d,&fake_ops,640,480); d.sh.gate=1; desk_shell_enable(&d);
        d.pixels=calloc((size_t)d.width*d.height,4); assert(d.pixels);
        (void)desk_shell_wallpaper_update(&d);
        footprint_fail_once("compositor.c"); footprint_frame(&d);
        assert(!d.render_cache && d.render_cache_prepared);
        footprint_begin("pool_failure_no_retry");
        footprint_client(&d,0,240,160); footprint_compare_painter(&d);
        desk_drop_client(&d,0); footprint_frame(&d); footprint_client(&d,0,400,300);
        footprint_compare_painter(&d); assert(!footprint_end());
        drop_all(&d); free(d.pixels); free(d.sh.icons); free(d.sh.wall); free(d.sh.autoplay.workspace); fake_reset();
        puts("PASS desktop footprint: bounded/fallback pixels and allocation-failure recovery");
    }
}
