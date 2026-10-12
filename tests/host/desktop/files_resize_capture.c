/* Channel/surface replay of live Files resizing with production layout,
 * toolkit glue, WM and compositor. Host evidence only; SPDX-License-Identifier: MIT */
#include "fake_vfs.h"
#include "resize_transport.h"
#include "../../../apps/files/app.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct desktop desktop;
static struct du_ui ui;
static struct du_window window;
static struct files_app app;
static void deliver(void)
{
    struct ciuki_message m;
    while (rt_pop(&m)) desk_receive(&desktop,0,&m);
    desk_flush(&desktop,0);
}
static void roundtrip(void)
{
    for (unsigned i=0;i<4;i++) { deliver(); assert(du_window_turn(&window,i+1)==1); }
    deliver();
    assert(!window.replacement && !window.request_serial && rt_surfaces()==1 && rt_mappings()==2 && rt_handles()==2);
}
static void check_frame(void)
{
    struct desk_client *c=&desktop.clients[0];
    struct desk_box b=desk_content(c);
    assert((unsigned)b.w==c->info.width && (unsigned)b.h==c->info.height);
    for(int y=0;y<b.h;y++) for(int x=0;x<b.w;x++)
        assert(desktop.pixels[(y+b.y)*1024+x+b.x]==(window.back[y*b.w+x]&0xffffffu));
    uint32_t *incremental=malloc(1024*768*4); assert(incremental);
    memcpy(incremental,desktop.pixels,1024*768*4);
    desk_composite(&desktop,(struct desk_box){0,0,1024,768});
    assert(!memcmp(incremental,desktop.pixels,1024*768*4));free(incremental);
    assert(desktop.pixels[1024*768]==0xdeadbeef);
}
int main(int argc,char **argv)
{
    assert(argc==3);
    uint32_t *portrait=malloc(256*256*4);
    FILE *f=fopen(argv[1],"rb");
    assert(portrait && f && fread(portrait,4,256*256,f)==256*256 && !fclose(f));
    rt_reset();desk_init(&desktop,&rt_ops,1024,768);
    desktop.portrait=portrait;desktop.output=0;desktop.display=0;
    desktop.pixels=calloc(1024*768+1,4);assert(desktop.pixels);
    desktop.pixels[1024*768]=0xdeadbeef;
    desktop.realtime_second=43200;desktop.mouse_x=950;desktop.mouse_y=700;
    desk_shell_enable(&desktop);assert(desk_add_client(&desktop,3)==0);
    assert(!du_window_open_resizable(&window,&ui,3,760,520,"Files",600,360));
    fake_reset();fake_image_tree();assert(!files_setup(&app,&ui,&fake_vfs,"/system"));
    roundtrip();desktop.clients[0].program=DESK_APP_FILES;desk_move(&desktop,0,128,72);
    const char *themes[]={"Platinum","Ciuki Blue","Classic 98"};
    const char *names[]={"platinum","ciuki-blue","classic-98"};
    for(unsigned t=0;t<3;t++) {
        assert(!desk_theme_use(themes[t]));desk_theme_changed(&desktop);ui.dirty=1;roundtrip();
        assert(!desk_redraw(&desktop));
        for(unsigned size=0;size<2;size++) {
            int w=size?760:600,h=size?520:380;
            unsigned count=app.model.count,active=app.model.active;
            int old_surface=desktop.clients[0].surface;
            desktop.damage.count=0;
            desk_resize(&desktop,0,desk_frame((unsigned)w,(unsigned)h,128,72));
            assert(desktop.clients[0].surface==old_surface && rt_surfaces()==1);
            /* The pending frame clears/clips its old backing without overread. */
            assert(!desk_redraw(&desktop));
            roundtrip();assert(desktop.clients[0].surface!=old_surface);
            assert(app.width==w && app.height==h && app.model.count==count && app.model.active==active);
            assert(!strcmp(app.model.path,"/system") && du_focused(&ui)==107);
            struct du_widget *list=du_get(&ui,107),*status=du_get(&ui,108);
            assert(list->box.x+list->box.w==w && status->box.y+status->box.h==h);
            assert(!desk_redraw(&desktop));check_frame();
            char path[1024];snprintf(path,sizeof(path),"%s/files-%s-%dx%d.xrgb",argv[2],names[t],w,h);
            f=fopen(path,"wb");assert(f && fwrite(desktop.pixels,4,1024*768,f)==1024*768 && !fclose(f));
        }
    }
    /* Keep the compositor cursor outside the content pixel comparison after
     * zooming, and damage both cursor positions through production input. */
    struct ciuki_input_event motion={.type=CIUKI_INPUT_MOTION,
        .value=10-desktop.mouse_x,.value2=8-desktop.mouse_y};
    desk_input(&desktop,&motion);roundtrip();
    struct desk_box user=desktop.clients[0].frame;
    desk_zoom(&desktop,0);roundtrip();assert(!desk_redraw(&desktop));check_frame();
    assert(desktop.clients[0].frame.x==0 && desktop.clients[0].frame.y==32 &&
        desktop.clients[0].frame.w==1024 && desktop.clients[0].frame.h==704);
    desk_collapse(&desktop,0);desk_collapse(&desktop,0); /* shade preserves zoom restore geometry */
    desk_zoom(&desktop,0);roundtrip();assert(!desk_redraw(&desktop));check_frame();
    assert(!memcmp(&user,&desktop.clients[0].frame,sizeof(user)));
    files_destroy(&app);fake_assert_clean();
    du_window_close(&window);assert(!du_window_turn(&window,10));deliver();
    du_window_destroy(&window);
    assert(!rt_surfaces() && !rt_handles() && !rt_mappings());rt_reset();
    free(desktop.pixels);free(portrait);
    puts("PASS Files resize replay: six live swaps/two sizes/three themes plus zoom/restore, exact content, incremental/full render agreement, model/focus preserved, surface counts 1->2->1->0");
    return 0;
}
