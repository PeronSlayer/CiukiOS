/* Removable tray production shell, no kernel/QEMU. SPDX-License-Identifier: MIT */
#include "fake.h"
#include "../../../apps/libdeskui/deskui.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct desktop d;
static unsigned rows, icon_calls, events[SOUND_EVENTS];
static void sound(struct desktop *desk,unsigned e) { (void)desk;assert(e<SOUND_EVENTS);events[e]++; }
static uint32_t icon[256];
void __real_du_text(struct du_canvas *,int,int,const char *,uint32_t);
void __wrap_du_text(struct du_canvas *c,int x,int y,const char *s,uint32_t col)
{if(!strncmp(s,"Safely remove ",14))rows++;__real_du_text(c,x,y,s,col);}
static const uint32_t *store(const char *name,unsigned size)
{if(!strcmp(name,"drive-removable-media")){assert(size==16);icon_calls++;return icon;}return NULL;}
static void click(int x,int y)
{d.mouse_x=x;d.mouse_y=y;assert(desk_shell_button(&d,CIUKI_BUTTON_LEFT,1));desk_shell_button(&d,CIUKI_BUTTON_LEFT,0);}
int main(void)
{
    fake_reset();desk_init(&d,&fake_ops,640,480);d.icon=store;d.sound_event=sound;desk_shell_enable(&d);
    d.pixels=calloc(d.width*d.height,4);assert(d.pixels);
    struct ciuki_media_state s={.version=1,.size=sizeof(s),.removable_mask=(1u<<4)|(1u<<5)};
    s.generation[4]=99;s.generation[5]=100;
    desk_shell_media_update(&d,&s);assert(d.damage.count);
    assert(!events[SOUND_DEVICE_CONNECT] && !events[SOUND_DEVICE_DISCONNECT]);
    s.generation[4]++;desk_shell_media_update(&d,&s);assert(events[SOUND_DEVICE_CONNECT]==1 && events[SOUND_DEVICE_DISCONNECT]==1);
    int x=640-SH_TRAY_W+44,y=464;
    click(x,y);assert(d.sh.media_open && (d.sh.want_reload&32));
    desk_shell_overlay(&d,(struct desk_box){0,0,640,480});assert(rows==2 && icon_calls);
    /* popup: bottom 448, height 72, first action starts y=404. */
    click(430,410);assert(!d.sh.media_open && d.sh.want_remove==4 && d.sh.remove_generation==100);
    click(x,y);assert(d.sh.media_open);assert(desk_shell_key(&d,DU_KEY_ESC,1)&&!d.sh.media_open);
    click(x,y);s.removable_mask=0;desk_shell_media_update(&d,&s);assert(!d.sh.media_open && events[SOUND_DEVICE_DISCONNECT]==2);
    d.now_ns=1;desk_shell_tick(&d);assert(d.sh.want_reload&32);
    s.removable_mask=0x03fffff0;desk_shell_media_update(&d,&s);click(x,y);
    assert(desk_shell_key(&d,DU_KEY_DOWN,1)&&d.sh.media_scroll==1);
    assert(desk_shell_key(&d,DU_KEY_UP,1)&&!d.sh.media_scroll);
    free(d.pixels);d.pixels=NULL;desk_render_release(&d);assert(!fake_live());
    puts("PASS removable tray: runtime Tango lookup, letters/generations, removal request, disappearance, Escape, long-list scroll, polling and damage");return 0;
}
