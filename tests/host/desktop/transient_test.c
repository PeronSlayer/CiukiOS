/* Production transient UI replay and review captures. SPDX-License-Identifier: MIT */
#include "fake.h"
#include "../../../apps/libdeskui/deskui.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../apps/dosbox/loading.h"

static struct desktop d;
static struct du_ui u;
static uint32_t pixels[640*480], portrait[256*256];
static unsigned sounds[SOUND_EVENTS];
static int dialog_result=-1;
static void sound(void *ctx, unsigned event) { (void)ctx; assert(event<SOUND_EVENTS); sounds[event]++; }
static void event(struct du_ui *ui,const struct du_event *e,void *ctx)
{ (void)ui; (void)ctx; if (e->type==DU_EV_DIALOG) dialog_result=e->index; }
static const uint32_t *icon(const char *name, unsigned size) { return du_icon_argb(du_icon_name(name),size); }
static void frame(const char *dir,const char *name)
{
    char path[512]; snprintf(path,sizeof(path),"%s/%s.xrgb",dir,name);
    FILE *f=fopen(path,"wb"); assert(f); assert(fwrite(pixels,4,640*480,f)==640*480); assert(!fclose(f));
}
static void composite(void) { desk_composite(&d,(struct desk_box){0,0,640,480}); }
static void key(unsigned code)
{
    struct ciuki_input_event e={.type=CIUKI_INPUT_KEY,.code=code,.value=1}; du_input(&u,&e);
    e.value=0; du_input(&u,&e);
}
static void repaint(void)
{
    static unsigned turn; turn++;
    for (unsigned i=0;i<d.damage.count;i++) desk_composite(&d,d.damage.boxes[i]);
    static uint32_t incremental[640*480]; memcpy(incremental,pixels,sizeof(pixels));
    composite();
    for (unsigned i=0;i<640*480;i++) if (incremental[i]!=pixels[i]) {
        fprintf(stderr,"Repaint %u differs at %u,%u: %06x vs %06x (tip=%d notification=%d)\n",
                turn,i%640,i/640,incremental[i],pixels[i],d.sh.tooltip,d.sh.notification.active);
        assert(0);
    }
    d.damage.count=0;
}
static void hover(struct desk_box b,int target)
{
    d.mouse_x=b.x+2; d.mouse_y=b.y+2; desk_shell_motion(&d);
    assert(!d.sh.tooltip);
    d.now_ns+=599000000ull; desk_shell_tick(&d); assert(!d.sh.tooltip);
    d.now_ns+=1000000ull; desk_shell_tick(&d); assert(d.sh.tooltip==target); repaint();
}
static void interactions(const char *dir)
{
    /* Reference scene parity; cursor movement has its own compositor tests. */
    d.hw_cursor=1;
    assert(sounds[SOUND_INFORMATION]==1 && sounds[SOUND_ERROR]==2 && sounds[SOUND_QUESTION]==1);
    assert(du_text_wrap(NULL,(struct du_rect){0,0,100,0},"one\ntwo\n",0)==3);
    assert(du_text_wrap(NULL,(struct du_rect){0,0,1,0},"caf\xc3\xa9",0)==4);
    static const char *const buttons[]={"&Yes","&No","Cancel"};
    du_add(&u,DU_EDIT,55,(struct du_rect){8,8,100,22},"Focus returns here"); du_focus(&u,55);
    du_message(&u,30,"Question","Proceed?",DU_DLG_ICON_QUESTION,buttons,3,1,2);
    key(DU_KEY_ENTER); assert(dialog_result==1 && du_focused(&u)==55);
    du_message(&u,31,"Question","Proceed?",DU_DLG_ICON_QUESTION,buttons,3,0,2);
    key(DU_KEY_ESC); assert(dialog_result==2 && du_focused(&u)==55);
    du_message(&u,32,"Question","Proceed?",DU_DLG_ICON_QUESTION,buttons,3,0,2);
    key(0x31); assert(dialog_result==1); /* N mnemonic */

    composite(); d.damage.count=0;
    desk_shell_notify_error(&d,"Settings not saved",EROFS); repaint();
    /* The invisible part of a sliding notice must not steal tray clicks. */
    struct desk_box network=desk_shell_tray_icon_box(&d,SH_TRAY_NETWORK);
    d.mouse_x=network.x+2; d.mouse_y=network.y+2;
    assert(desk_shell_button(&d,CIUKI_BUTTON_LEFT,1));
    assert(!d.sh.notification.active && d.sh.network_open && d.focus==-1); repaint();
    desk_shell_key(&d,DU_KEY_ESC,1); repaint();
    desk_shell_notify(&d,"Network","The network cable is connected.",DU_DLG_ICON_INFO,SH_NOTIFY_NETWORK);
    assert(d.focus==-1); repaint();
    for (int i=0;i<5;i++) { d.now_ns+=40000000ull; desk_shell_tick(&d); repaint(); }
    int top=DESK_BAR+SH_TRAY_GAP;
    d.mouse_x=desk_shell_tray_icon_box(&d,SH_TRAY_NOTIFICATION).x-100; d.mouse_y=top+30;
    assert(desk_shell_button(&d,CIUKI_BUTTON_LEFT,1));
    assert(!d.sh.notification.active && d.sh.network_open && d.focus==-1); repaint();
    desk_shell_key(&d,DU_KEY_ESC,1); repaint();
    desk_shell_notify_error(&d,"Settings not saved",-EROFS); repaint();
    d.now_ns+=200000000ull; desk_shell_tick(&d); repaint();
    d.mouse_x=desk_shell_tray_icon_box(&d,SH_TRAY_NOTIFICATION).x-100; d.mouse_y=top+30;
    assert(desk_shell_button(&d,CIUKI_BUTTON_LEFT,1));
    assert(!d.sh.notification.active && d.focus>=0);
    assert(!strcmp(d.sh.message_text,"Read-only file system (30)"));
    desk_drop_client(&d,d.focus); composite(); d.damage.count=0;

    d.sh.mixer_state=2; d.sh.volume=20;
    assert(desk_shell_key(&d,0x130,1)); assert(d.sh.volume==21 && d.sh.notification.active);
    d.now_ns+=200000000ull; desk_shell_tick(&d); repaint();
    uint64_t started=d.sh.notification.started;
    assert(desk_shell_key(&d,0x130,2)); assert(d.sh.notification.started==started);
    assert(strstr(d.sh.notification.text,"22%")); repaint();
    d.mouse_x=desk_shell_tray_icon_box(&d,SH_TRAY_NOTIFICATION).x-100; d.mouse_y=top+30;
    assert(desk_shell_button(&d,CIUKI_BUTTON_LEFT,1));
    assert(!d.sh.notification.active && d.sh.volume_open && d.focus==-1); repaint();
    desk_shell_key(&d,DU_KEY_ESC,1); assert(!d.sh.volume_open); repaint();
    hover(desk_shell_tray_icon_box(&d,SH_TRAY_VOLUME),3);
    frame(dir,"tooltip-volume");
    d.sh.volume=100; desk_shell_volume_damage(&d); repaint();

    struct ciuki_net_state n={.version=1,.size=sizeof(n),.present=1,.link=CIUKI_NET_LINK_UP,
        .address_configured=1,.ipv4={192,168,1,23}};
    desk_shell_network_update(&d,&n);
    hover(desk_shell_tray_icon_box(&d,SH_TRAY_NETWORK),2); frame(dir,"tooltip-network");
    d.mouse_x=0;d.mouse_y=100;desk_shell_motion(&d);repaint();
    struct ciuki_media_state m={.version=CIUKI_MEDIA_VERSION,.size=sizeof(m),.removable_mask=1u<<4};
    desk_shell_media_update(&d,&m); m.generation[4]++;desk_shell_media_update(&d,&m);
    assert(d.sh.notification.active && d.sh.notification.action==SH_NOTIFY_MEDIA);
    d.mouse_x=desk_shell_tray_icon_box(&d,SH_TRAY_NOTIFICATION).x-100; d.mouse_y=top+30;
    assert(desk_shell_button(&d,CIUKI_BUTTON_LEFT,1));
    assert(!d.sh.notification.active && d.sh.media_open); repaint();
    desk_shell_key(&d,DU_KEY_ESC,1); assert(!d.sh.media_open);
    static struct ciuki_media_volume volumes[26]; strcpy(volumes[4].label,"CIUKI");
    desk_shell_media_labels(&d,volumes); repaint();
    hover(desk_shell_tray_icon_box(&d,SH_TRAY_MEDIA),4); frame(dir,"tooltip-media");
    desk_internal_open(&d,DESK_INTERNAL_ABOUT);
    strcpy(d.clients[d.focus].title,"A long window title that does not fit inside its taskbar button");
    composite();d.damage.count=0;
    hover(desk_task_button(&d,d.focus),16+d.focus);frame(dir,"tooltip-task");
    strcpy(d.clients[d.focus].title,"Short title");
    desk_damage_add(&d.damage,desk_outer(&d.clients[d.focus]),640,480);
    desk_damage_add(&d.damage,(struct desk_box){0,0,640,DESK_BAR},640,480);
    desk_damage_add(&d.damage,desk_taskbar(&d),640,480);
    desk_shell_tick(&d); repaint(); /* erase the previous, wider title tip */
}
int main(int argc,char **argv)
{
    assert(argc==3); FILE *f=fopen(argv[2],"rb");
    assert(f && fread(portrait,4,256*256,f)==256*256 && !fclose(f));
    fake_reset(); desk_init(&d,&fake_ops,640,480); d.pixels=pixels; d.portrait=portrait; d.icon=icon;
    desk_shell_enable(&d); d.now_ns=1000000000ull;
    du_init(&u,pixels,640,480); u.sound=sound; u.event=event;
    static const char *const names[]={"info","warning","error","question"};
    static const char *const titles[]={"Removable media","Unsaved changes","Program did not start","Shut down CiukiOS?"};
    static const char *const texts[]={"E: CIUKI is ready to use. Open Files to browse the drive.",
        "Your changes have not been saved. Save your work before closing this program.",
        "The program could not be started. Check that the file exists and that you have permission to open it, then try again.",
        "Your settings will be saved. Open programs and virtual machines will be closed. Do you want to continue?"};
    static const char *const buttons[]={"&Continue","&Cancel"};
    for (int i=0;i<4;i++) {
        assert(du_icon_argb(du_message_icon_name(i+1),32));
        du_message(&u,10+i,titles[i],texts[i],i+1,buttons,2,0,1);
        du_render(&u); frame(argv[1],names[i]); du_dialog_close(&u);
    }
    composite();
    desk_shell_notify(&d,"Removable media","E: CIUKI is ready to use.",DU_DLG_ICON_INFO,SH_NOTIFY_MEDIA);
    d.now_ns+=200000000ull; desk_shell_tick(&d);
    composite(); frame(argv[1],"notification");
    desk_shell_notify_error(&d,"Settings not saved",EROFS);
    assert(strstr(d.sh.notification.text,"Read-only file system (30)"));
    d.now_ns+=200000000ull; desk_shell_tick(&d);
    composite(); frame(argv[1],"balloon");
    d.now_ns+=9000000000ull; desk_shell_tick(&d);
    assert(!d.sh.notification.active);
    struct desk_box clock=desk_shell_clock_box(&d);
    d.mouse_x=clock.x+2; d.mouse_y=clock.y+2; desk_shell_motion(&d);
    d.now_ns+=700000000ull; desk_shell_tick(&d); composite(); frame(argv[1],"tooltip");
    struct du_canvas c={pixels,640,480,{0,0,640,480}};
    dosbox_panel(&c,"DOOM",dosbox_launch_icon("doom"),NULL,"Preparing the game. Please wait...",900);
    frame(argv[1],"loading");
    interactions(argv[1]);
    puts("PASS transient UI"); return 0;
}
