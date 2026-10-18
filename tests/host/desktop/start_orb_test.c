/* Production orb geometry, layering and incremental damage. SPDX-License-Identifier: MIT */
#include "fake.h"
#include "../../../apps/libdeskui/deskui.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct desktop d;
static uint32_t portrait[256*256];
static uint32_t window_pixel;
static size_t bytes;
static void full(void) { desk_composite(&d,(struct desk_box){0,0,d.width,d.height}); }
static void parity(void)
{
    static unsigned step;
    step++;
    assert(d.damage.count);
    for (unsigned i=0; i<d.damage.count; i++) desk_composite(&d,d.damage.boxes[i]);
    uint32_t *copy=malloc(bytes); assert(copy); memcpy(copy,d.pixels,bytes);
    full();
    if (memcmp(copy,d.pixels,bytes)) for (size_t k=0; k<bytes/4; k++) if (copy[k]!=d.pixels[k]) {
        fprintf(stderr,"orb parity step=%u %s %ux%u at %zu,%zu incremental=%06x full=%06x\n",
            step,desk_theme->name,d.width,d.height,k%d.width,k/d.width,copy[k],d.pixels[k]); break;
    }
    assert(!memcmp(copy,d.pixels,bytes)); free(copy); d.damage.count=0;
}
static void frame(const char *directory, const char *state)
{
    char path[1024];
    snprintf(path,sizeof(path),"%s/%s-%ux%u-%s.xrgb",directory,
        !strcmp(desk_theme->name,"Platinum") ? "platinum" : "ciuki-blue",d.width,d.height,state);
    FILE *f=fopen(path,"wb"); assert(f && fwrite(d.pixels,1,bytes,f)==bytes && !fclose(f));
}
static void point(int x, int y)
{
    struct ciuki_input_event e={.type=CIUKI_INPUT_MOTION,.value=x-d.mouse_x,.value2=y-d.mouse_y};
    desk_input(&d,&e);
}
static void click(void)
{
    struct ciuki_input_event e={.type=CIUKI_INPUT_BUTTON,.code=CIUKI_BUTTON_LEFT,.value=1};
    desk_input(&d,&e); e.value=0; desk_input(&d,&e);
}
static void window(void)
{
    struct desk_client *c=&d.clients[0];
    c->window=1; c->order=1; c->resizable=1; c->channel=-1;
    c->min_width=c->min_height=c->step_width=c->step_height=1;
    c->max_width=c->max_height=2048;
    window_pixel=desk_theme->window; c->pixels=&window_pixel;
    c->info.width=c->info.height=1;
    c->frame=desk_frame(320,180,100,100);
    strcpy(c->title,"Window under the Start orb");
    desk_damage_add(&d.damage,desk_taskbar(&d),d.width,d.height);
    desk_zoom(&d,0);
    assert(c->zoomed && c->frame.y==DESK_BAR && c->frame.h==(int)d.height-2*DESK_BAR);
}
static void exercise(const char *directory)
{
    struct desk_box b=desk_start_button(&d), shadow=desk_start_bounds(&d);
    assert(b.x==6 && b.y==0 && b.w==38 && b.h==38 && shadow.h==39);
    int cx=b.x+b.w/2, cy=b.y+b.h/2, py=b.y+b.h-4;
    /* Entire circle, including both sides of the bar boundary; corners pass through. */
    assert(desk_start_contains(&d,cx,0) && desk_start_contains(&d,cx,b.h-1));
    assert(desk_start_contains(&d,6,cy) && desk_start_contains(&d,b.x+b.w-1,cy));
    assert(!desk_start_contains(&d,6,0) && !desk_start_contains(&d,6,b.h-1));
    assert(!desk_start_contains(&d,cx,b.h) && !desk_start_contains(&d,b.x+b.w,cy));
    assert(desk_shell_chrome_intersects(&d,(struct desk_box){20,b.h,1,1}));
    assert(!desk_shell_chrome_intersects(&d,(struct desk_box){b.x+b.w+1,b.h,1,1}));
    full(); d.damage.count=0; frame(directory,"idle");
    point(cx,py); assert(d.sh.start_hot); parity(); frame(directory,"hover");
    click(); assert(d.menu_open && d.sh.levels==1); parity(); frame(directory,"pressed");
    assert(desk_menu_box(&d).x==b.x && desk_menu_box(&d).y==DESK_BAR);
    /* Click the protrusion again; the overlaid menu must not steal it. */
    click(); assert(!d.menu_open); parity();
    point(100,80); parity();
    /* Keyboard activation and dismissal preserve their existing contract. */
    assert(desk_shell_key(&d,0x15b,1));
    assert(desk_shell_key(&d,0x15b,0)); assert(d.menu_open); parity();
    assert(desk_shell_key(&d,DU_KEY_ESC,1)); assert(!d.menu_open); parity();
    desk_menu_open(&d,SH_MENU_DESKTOP,100,100); parity();
    point(cx,py); parity(); click();
    assert(d.menu_open && d.sh.menus[0].kind==SH_MENU_START); parity();
    click(); assert(!d.menu_open); parity(); point(100,80); parity();
    window(); parity(); frame(directory,"maximized");
    assert(desk_hit(&d,cx,py)==-1 && desk_title_code(&d,cx,py)==0);
    assert(desk_hit(&d,6,b.h-1)==0);  /* bounding-box corner belongs to the window */
    uint32_t orb=d.pixels[py*d.width+cx];
    desk_composite(&d,(struct desk_box){20,DESK_BAR,16,13});
    assert(d.pixels[py*d.width+cx]==orb);
    /* The copy source or destination would contain orb/shadow pixels. */
    d.frame_valid=1; d.damage.count=0;
    desk_zoom(&d,0); parity();
    desk_move(&d,0,0,DESK_BAR); parity();
    d.damage.count=0; desk_move(&d,0,100,100); assert(!d.copy_pending); parity();
    d.damage.count=0; desk_move(&d,0,0,DESK_BAR); assert(!d.copy_pending); parity();
    desk_zoom(&d,0); parity();
    desk_shell_notify(&d,"Notification","Shell chrome over the maximized window",DU_DLG_ICON_INFO,SH_NOTIFY_DETAILS);
    parity(); frame(directory,"notification");
    assert(d.pixels[py*d.width+cx]==orb);
    assert(desk_shell_key(&d,DU_KEY_ESC,1)); parity();
    /* At 320 px the notification and orb really overlap. */
    unsigned width=d.width; d.width=320;
    d.sh.background_valid=0;
    full(); d.damage.count=0;
    desk_shell_notify(&d,"Overlap","The orb retains its upper shell layer",DU_DLG_ICON_INFO,SH_NOTIFY_DETAILS);
    d.sh.notification.slide=0;
    desk_damage_add(&d.damage,desk_shell_notification_bounds(&d),d.width,d.height);
    struct desk_box overlap;
    assert(desk_intersect(desk_shell_notification_bounds(&d),b,&overlap));
    parity();
    assert(d.pixels[py*d.width+cx]==orb);
    point(cx,py); parity(); click();
    assert(d.menu_open && !d.sh.notification.active); parity();
    click(); assert(!d.menu_open); parity(); d.width=width;
}
int main(int argc, char **argv)
{
    assert(argc==4);
    FILE *f=fopen(argv[1],"rb"); assert(f && fread(portrait,4,256*256,f)==256*256 && !fclose(f));
    const unsigned widths[]={1024,800}, heights[]={768,600};
    for (int theme=0; theme<desk_theme_count(); theme++) for (int size=0; size<2; size++) {
        fake_reset(); desk_init(&d,&fake_ops,widths[size],heights[size]);
        d.portrait=portrait; d.hw_cursor=1; desk_shell_enable(&d);
        strcpy(d.sh.cfg.theme,desk_theme_at(theme)->name); desk_shell_apply(&d);
        f=fopen(argv[3],"rb"); assert(f && !fseek(f,0,SEEK_END));
        long length=ftell(f); assert(length>0 && !fseek(f,0,SEEK_SET));
        uint8_t *data=malloc((size_t)length); assert(data && fread(data,1,(size_t)length,f)==(size_t)length && !fclose(f));
        assert(!desk_image_decode(data,(size_t)length,&d.sh.image)); free(data);
        desk_shell_wallpaper_update(&d);
        d.realtime_second=12*3600; d.sh.cfg.clock_24h=1;
        bytes=(size_t)d.width*d.height*4;
        uint32_t *guarded=calloc((size_t)d.width*d.height+2,4); assert(guarded);
        guarded[0]=guarded[(size_t)d.width*d.height+1]=0xfeedface; d.pixels=guarded+1;
        exercise(argv[2]);
        assert(guarded[0]==0xfeedface && guarded[(size_t)d.width*d.height+1]==0xfeedface);
        free(d.sh.background); free(d.sh.wall); desk_image_free(&d.sh.image); free(guarded);
    }
    puts("PASS orb: circle/protrusion hits, menu/keyboard, maximization, shadow damage, copy exclusions, notification overlap");
    return 0;
}
