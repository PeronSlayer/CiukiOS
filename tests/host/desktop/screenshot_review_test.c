/* SPDX-License-Identifier: MIT
 * fx-28: real Log Viewer + toolkit + desktop transport; virtual shell time. */
#include "resize_transport.h"
#include "../../../apps/accessories/logview.h"
#include "../../../apps/desktop/clipboard.h"
#include "../../../apps/control/control.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
static struct desktop d;
static struct du_ui ui;
static struct du_window window;
static struct lv_app app;
static uint64_t now=1000000000ull;
static int blocked;
static unsigned v3_pings;
static unsigned launches;
static int launch_applet(struct desktop *desktop,const struct desk_program *program)
{
    assert(!strcmp(program->name,"control"));
    assert(desktop->launch_arg && (!strcmp(desktop->launch_arg,"--panel=display") || !strcmp(desktop->launch_arg,"--panel=power")));
    launches++;return 99;
}
static uint64_t clock_now(void) { return now; }
static int send_msg(int fd,const struct ciuki_message *m,uint32_t flags)
{
    if (blocked) { errno=EAGAIN; return -1; }
    if (fd==4) {
        assert(m->data[0]==DESK_V3_VERSION && m->data[1]==DESK_PING && desk_get32(m->data+8)==7);
        v3_pings++; return 0;
    }
    return rt_ops.send(fd,m,flags);
}
static void deliver(void)
{
    struct ciuki_message m;
    while (rt_pop(&m)) desk_receive(&d,0,&m);
    desk_flush(&d,0);
}
static void turn(void)
{
    d.now_ns=now;
    for (unsigned i=0;i<4;i++) { deliver(); assert(du_window_turn(&window,now)==1); }
    deliver();
    d.drain_ns=now; desk_heartbeat(&d,now);
    assert(!d.clients[0].unresponsive);
}
static void key(int code,int value)
{ struct ciuki_input_event e={.type=CIUKI_INPUT_KEY,.code=(unsigned)code,.value=value,.monotonic_ns=now};desk_input(&d,&e); }
int main(int argc,char **argv)
{
    assert(argc==2);
    for (int font=DESK_FONT_UI;font<=DESK_FONT_TITLE;font++) {
        const struct desk_font *face=desk_font(font);
        for (unsigned i=0;i<sizeof(desk_font_extra_codes)/sizeof(*desk_font_extra_codes);i++) {
            int index=desk_font_code_index(desk_font_extra_codes[i]);
            assert(index>=191 && face->glyphs[index].w && face->glyphs[index].h);
        }
        const unsigned latin[]={0xb0,0xd7,0xa9};
        for (unsigned i=0;i<3;i++) assert(face->glyphs[desk_font_code_index(latin[i])].w);
    }
    char path[1024];
    snprintf(path,sizeof(path),"%s/system",argv[1]); assert(!mkdir(path,0700));
    snprintf(path,sizeof(path),"%s/system/logs",argv[1]); assert(!mkdir(path,0700));
    snprintf(path,sizeof(path),"%s/system/logs/BOOT.LOG",argv[1]);
    FILE *f=fopen(path,"wb"); assert(f);
    for (unsigned k=0;k<8000;k++) assert(fputs("[boot] line of text\r\n",f)>=0);
    assert(!fclose(f));
    rt_reset(); struct desk_ops ops=rt_ops; ops.send=send_msg;
    desk_init(&d,&ops,1024,768); d.time_ns=clock_now; d.now_ns=now;
    d.pixels=calloc(1024*768,4); assert(d.pixels); desk_shell_enable(&d);
    assert(desk_add_client(&d,3)==0);
    assert(!du_window_open_resizable(&window,&ui,3,LV_W,LV_H,"Log Viewer",440,280));
    lv_root=argv[1]; assert(!lv_attach(&app,&ui)); assert(app.ta.len>55000);
    turn();
    /* Only a desktop-authenticated Control Panel may open these two applets.
     * Power Management must not open the shell's shutdown confirmation. */
    d.program_count=1;strcpy(d.programs[0].name,"control");d.launch=launch_applet;
    struct ciuki_message request;
    assert(!cp_applet_message(&request,400,CP_POWER));request.sender_pid=42;
    desk_receive(&d,0,&request);assert(!launches);
    d.clients[0].program=DESK_APP_CONTROL;
    desk_receive(&d,0,&request);assert(launches==1 && !d.power.state && !d.session.state);
    assert(!cp_applet_message(&request,401,CP_DISPLAY));request.sender_pid=42;
    desk_receive(&d,0,&request);assert(launches==2);
    request.data[request.length-1]='X';desk_receive(&d,0,&request);assert(launches==2);
    d.clients[0].program=0;turn();
    key(DU_KEY_LALT,1); key(DU_KEY_SPACE,1); key(DU_KEY_SPACE,0); key(DU_KEY_LALT,0);
    assert(d.sh.levels);
    for (unsigned i=0;i<6;i++) { now+=1000000000ull; turn(); assert(d.sh.levels); }
    /* PING queued, then shell-owned work takes six seconds before delivery.
     * A fully blocked transport adds another six seconds. Neither is app time. */
    now+=1000000000ull; d.now_ns=now; desk_heartbeat(&d,now);
    assert(d.clients[0].ping_pending);
    blocked=1; now+=6000000000ull; d.now_ns=now; deliver(); d.drain_ns=now; desk_heartbeat(&d,now);
    assert(!d.clients[0].unresponsive);
    key(DU_KEY_ESC,1); key(DU_KEY_ESC,0); assert(!d.sh.levels);
    assert(!desk_redraw(&d));
    assert(desk_clip_key(&d,0x137,1,0)==1); assert(desk_clip_key(&d,0x137,0,0)==1);
    now+=6000000000ull; d.now_ns=now; blocked=0; deliver();
    d.drain_ns=now; desk_heartbeat(&d,now); assert(!d.clients[0].unresponsive);
    turn();
    for (unsigned i=0;i<20;i++) { now+=1000000000ull; turn(); }
    /* A genuinely hung client still times out after delivery, then recovers. */
    now+=1000000000ull; d.now_ns=now; desk_heartbeat(&d,now); deliver();
    now+=5000000000ull; d.drain_ns=now; desk_heartbeat(&d,now); assert(d.clients[0].unresponsive);
    assert(du_window_turn(&window,now)==1); deliver(); assert(!d.clients[0].unresponsive);
    /* A shared v3 transport stamps the addressed window, never its anchor. */
    d.clients[1]=(struct desk_client){.channel=4,.version=DESK_V3_VERSION};
    d.clients[2]=(struct desk_client){.channel=4,.version=DESK_V3_VERSION,.v3_peer=2,
        .v3_id=7,.v3_generation=9,.ping_pending=1,.ping_queued=1,.serial=77};
    struct ciuki_message ping;desk_message(&ping,DESK_PING,8);desk_put32(ping.data+4,77);
    desk_queue(&d,2,&ping);now+=6000000000ull;d.now_ns=now;desk_flush(&d,1);
    assert(v3_pings==1 && !d.clients[2].ping_queued && d.clients[2].ping_sent_ns==now && !d.clients[1].ping_sent_ns);
    memset(&d.clients[1],0,sizeof(d.clients[1]));d.clients[1].channel=-1;
    memset(&d.clients[2],0,sizeof(d.clients[2]));d.clients[2].channel=-1;
    lv_detach(&app); du_window_destroy(&window); desk_drop_client(&d,0);
    desk_clip_destroy(&d); desk_render_release(&d); free(d.sh.icons); free(d.sh.wall); free(d.sh.background); free(d.sh.autoplay.workspace); free(d.pixels);
    assert(!rt_handles() && !rt_mappings() && !rt_surfaces());
    puts("PASS fx-28 Log Viewer: 160 KB log, idle shell menu, Print Screen, queued/backpressured PING, real timeout and recovery; bounded Display/Power applet launches"); fflush(stdout);
    return 0;
}
