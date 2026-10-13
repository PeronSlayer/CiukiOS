/* SPDX-License-Identifier: MIT
 * Included by the production desktop.c launcher harness. Fixture memory is
 * allocated before tracing; only production units use footprint_alloc.h. */
#include "footprint_alloc.h"
#undef malloc
#undef calloc
#undef realloc
static int footprint_list(void *ctx,const char *path,fm_emit emit,void *arg)
{
    (void)ctx; assert(!strcmp(path,"/mnt/e"));
    struct fm_meta meta={.type=FM_FILE,.size=4};
    return emit("tune.wav",&meta,arg);
}
static const struct fm_vfs footprint_fs={.list=footprint_list};
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
    desk_message(&m,DESK_CREATE_WINDOW,20); m.fd_count=1; m.fds[0]=surface;
    desk_put32(m.data+4,w); desk_put32(m.data+8,h); desk_put32(m.data+12,4); memcpy(m.data+16,"Test",4);
    v2(d,slot,200+slot,&m); assert(d->clients[slot].window);
    footprint_frame(d);
}
static void test_memory_footprint(void)
{
    static struct desktop d;
    int audit=getenv("CIUKI_FOOTPRINT_AUDIT")!=NULL;
    fake_reset(); desk_init(&d,&fake_ops,1024,768); d.sh.gate=1; desk_shell_enable(&d);
    d.pixels=calloc((size_t)d.width*d.height,4); assert(d.pixels);
    (void)desk_shell_wallpaper_update(&d); assert(d.sh.wall_valid); footprint_frame(&d);
    footprint_begin("survivor_after_first_frame");
    footprint_client(&d,0,240,160);
    unsigned allocations=footprint_end();
    /* The probe snapshots here, after the survivor's first frame. */
    footprint_begin("crash_cycle");
    footprint_client(&d,1,240,160); desk_drop_client(&d,1); footprint_frame(&d);
    footprint_client(&d,1,400,300); desk_drop_client(&d,1); footprint_frame(&d);
    assert(d.clients[0].window && !d.clients[1].window);
    allocations+=footprint_end();
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
    desk_autoplay_update(&d,volumes); desk_autoplay_service(&d); assert(d.sh.autoplay.open);
    assert(!strcmp(d.sh.autoplay.content.music,"/mnt/e")); footprint_frame(&d);
    memset(volumes,0,sizeof(volumes)); desk_autoplay_update(&d,volumes); desk_autoplay_service(&d);
    assert(!d.sh.autoplay.open); footprint_frame(&d);
    assert(footprint_sounds[SOUND_MENU_OPEN] && footprint_sounds[SOUND_VOLUME] &&
        footprint_sounds[SOUND_DEVICE_CONNECT] && footprint_sounds[SOUND_DEVICE_DISCONNECT]);
    allocations+=footprint_end();
    if (!audit) assert(!allocations);
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
