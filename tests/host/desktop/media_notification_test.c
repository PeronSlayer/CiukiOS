/* Production media notifications over an updating opaque client. SPDX-License-Identifier: MIT */
#include "fake.h"
#include "../../../apps/libdeskui/deskui.h"
#include "../../../apps/desktop/media_notice.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct desktop d;
static uint32_t *expected;
static uint32_t portrait[256*256];
static const uint32_t *icon(const char *name,unsigned size) { return du_icon_argb(name,size); }
static void redraw(void)
{
    uint32_t *pixels=d.pixels; d.pixels=expected;
    desk_composite(&d,(struct desk_box){0,0,(int)d.width,(int)d.height}); d.pixels=pixels;
    fake_presents=0; assert(!desk_redraw(&d));
    if (memcmp(expected,pixels,(size_t)d.width*d.height*4)) {
        const char *path=getenv("CIUKI_MEDIA_FAILURE");
        if (path) { FILE *f=fopen(path,"wb");assert(f);assert(fwrite(pixels,4,(size_t)d.width*d.height,f)==(size_t)d.width*d.height);assert(!fclose(f)); }
        assert(!"notification differs from complete repaint");
    }
}
static void capture(const char *dir,const char *theme,const char *event)
{
    char path[512];snprintf(path,sizeof(path),"%s/%s-%ux%u-%s.xrgb",dir,theme,d.width,d.height,event);
    FILE *f=fopen(path,"wb");assert(f);assert(fwrite(d.pixels,4,(size_t)d.width*d.height,f)==(size_t)d.width*d.height);assert(!fclose(f));
}
static void settle(void) { d.now_ns+=160000000ull;desk_shell_tick(&d);assert(!d.sh.notification.slide);redraw(); }
static void blocker_overflow(unsigned width,unsigned height)
{
    for (int long_names=0;long_names<2;long_names++) {
        struct ciuki_media_blockers b={.count=long_names ? 64 : 30};
        for (unsigned i=0;i<b.count;i++) {
            b.programs[i].pid=i+1;
            snprintf(b.programs[i].name,sizeof(b.programs[i].name),long_names ? "WWWWWWWWWWWWWWWWWWWWWWWWWWW%02u" : "p%u",i);
        }
        char text[320];struct du_rect body=desk_media_notice_body(width,height);
        desk_media_blocker_text(text,sizeof(text),&b,body);
        assert(desk_media_text_fits(text,body));
        const char *tail=strstr(text,"\n... and ");
        unsigned shown=0,remaining=0;
        for (const char *s=text;*s;s++) if (*s=='(') shown++;
        if (tail) {
            assert(sscanf(tail,"\n... and %u more programs.",&remaining)==1 && shown+remaining==b.count && shown);
            assert(!strchr(tail+1,'\n')); /* the visible remaining count is last */
        } else assert(shown==b.count && !long_names && height>480);
        if (height==480 || long_names) assert(tail);
        if (long_names) {
            char first[64];snprintf(first,sizeof(first),"%s (%u)",b.programs[0].name,b.programs[0].pid);
            assert(du_text_wrap(NULL,body,first,0)>1); /* genuine wrapping */
        }
        desk_shell_notify(&d,"Drive is in use",text,DU_DLG_ICON_WARNING,SH_NOTIFY_DETAILS);
        settle(); /* incremental/full rendering parity with the fitted list */
        /* Its settled body has enough full rows, including the count. */
        int lines=du_text_wrap(NULL,body,d.sh.notification.text,0);
        assert(lines*(desk_font_line(desk_theme->font_ui)+1)<=body.h);
        desk_shell_key(&d,DU_KEY_ESC,1);redraw();
    }
}
static void replay(const char *dir,unsigned width,unsigned height,unsigned theme)
{
    fake_reset(); desk_init(&d,&fake_ops,width,height);
    desk_shell_enable(&d);desk_theme_use(desk_theme_at(theme)->name);d.mouse_x=-100;d.mouse_y=-100;d.now_ns=1000000000ull;d.icon=icon;d.portrait=portrait;
    size_t bytes=(size_t)width*height*4;d.pixels=calloc(1,bytes);expected=calloc(1,bytes);assert(d.pixels&&expected);
    struct desk_client *c=&d.clients[0];c->window=1;c->order=1;c->frame=(struct desk_box){40,40,(int)width-80,(int)height-80};
    struct desk_box content=desk_content(c);c->info.width=(unsigned)content.w;c->info.height=(unsigned)content.h;
    c->pixels=calloc((size_t)content.w*content.h,4);assert(c->pixels);strcpy(c->title,"Files - E:");
    struct ciuki_media_state m={.version=CIUKI_MEDIA_VERSION,.size=sizeof(m)};desk_shell_media_update(&d,&m);redraw();
    m.removable_mask=1u<<4;m.generation[4]=77;m.type[4]=CIUKI_MEDIA_USB;desk_shell_media_update(&d,&m);
    assert(d.sh.notification.active && d.sh.notification.slide==20);redraw();
    const char *name=theme==0 ? "platinum" : "ciuki-blue";
    capture(dir,name,"attach-enter");settle();capture(dir,name,"attach");
    /* This precise client-only damage used to overwrite the notice. */
    struct desk_box dirty={(int)width-280,(int)height-130,200,40};
    desk_damage_add(&d.damage,dirty,width,height);redraw();
    /* Writes/client redraw while the panel lives: keep all overlay pixels. */
    for (unsigned i=0;i<(unsigned)content.w*content.h;i++) ((uint32_t *)c->pixels)[i]=0x345678;
    desk_damage_add(&d.damage,content,width,height);redraw();capture(dir,name,"write");
    struct ciuki_media_blockers b={.count=2,.programs={{42,"ciuknote"},{43,"terminal"}}};char text[320];
    desk_media_blocker_text(text,sizeof(text),&b,desk_media_notice_body(width,height));desk_shell_notify(&d,"Drive is in use",text,DU_DLG_ICON_WARNING,SH_NOTIFY_DETAILS);
    settle();capture(dir,name,"busy");
    assert(desk_shell_key(&d,DU_KEY_ESC,1)&&!d.sh.notification.active);redraw();
    m.removable_mask=0;m.type[4]=CIUKI_MEDIA_SAFE_REMOVED;desk_shell_media_update(&d,&m);
    assert(strstr(d.sh.notification.text,"Safe to unplug") && !d.sh.notification.balloon);settle();capture(dir,name,"safe");
    uint64_t expires=d.sh.notification.expires;
    desk_shell_media_update(&d,&m);assert(d.sh.notification.expires==expires);redraw(); /* physical detach: no second event */
    d.now_ns=expires-1;desk_shell_tick(&d);assert(d.sh.notification.active);redraw();
    d.now_ns=expires;desk_shell_tick(&d);assert(!d.sh.notification.active);redraw();capture(dir,name,"dismissed");
    m.removable_mask=1u<<3;m.type[3]=CIUKI_MEDIA_OPTICAL;m.generation[3]=88;desk_shell_media_update(&d,&m);settle();capture(dir,name,"cd-insert");
    assert(strstr(d.sh.notification.text,"Disc inserted"));
    m.removable_mask=0;desk_shell_media_update(&d,&m);settle();capture(dir,name,"cd-eject");
    assert(strstr(d.sh.notification.text,"Disc ejected"));
    /* Close affordance dismisses without opening the media menu/dialog. */
    d.mouse_x=desk_shell_tray_icon_box(&d,SH_TRAY_NOTIFICATION).x+SH_TRAY_ICON_SIZE-18;
    d.mouse_y=DESK_BAR+SH_TRAY_GAP+20;
    assert(desk_shell_button(&d,CIUKI_BUTTON_LEFT,1));assert(!d.sh.notification.active && !d.sh.media_open);redraw();
    desk_shell_notify(&d,"Removable media","A notification to dismiss.",DU_DLG_ICON_INFO,SH_NOTIFY_MEDIA);settle();
    /* Outside click dismisses without stealing the following ordinary click. */
    d.mouse_x=0;d.mouse_y=100;desk_shell_button(&d,CIUKI_BUTTON_LEFT,1);assert(!d.sh.notification.active);redraw();
    m.removable_mask=1u<<4;m.type[4]=CIUKI_MEDIA_USB;m.generation[4]++;desk_shell_media_update(&d,&m);
    m.removable_mask=0;m.type[4]=0;desk_shell_media_update(&d,&m);settle();capture(dir,name,"unsafe");assert(d.sh.notification.balloon);
    blocker_overflow(width,height);
    free((void *)c->pixels);c->pixels=NULL;c->window=0;desk_render_release(&d);free(d.pixels);free(expected);assert(!fake_live());
}
int main(int argc,char **argv)
{
    assert(argc==3);FILE *f=fopen(argv[2],"rb");assert(f && fread(portrait,4,256*256,f)==256*256 && !fclose(f));
    for (unsigned theme=0;theme<(unsigned)desk_theme_count();theme++) { replay(argv[1],640,480,theme);replay(argv[1],1024,768,theme); }
    puts("PASS blocker notification rows: 30 short/64 long wrapped names, visible exact remainder, both themes and two resolutions");
    puts("PASS removable notification replay: attach/write/busy/safe/detach/CD/unsafe, entry/expiry/Escape/outside click, opaque client damage, both themes and two resolutions");
    return 0;
}
