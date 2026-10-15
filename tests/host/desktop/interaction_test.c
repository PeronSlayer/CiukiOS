/* Production window interaction replay and review frames. SPDX-License-Identifier: MIT */
#include "fake.h"
#include <deskui.h>
#include <textarea.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct desktop d;
static const char *frames;
static void event(unsigned type,int code,int value,int value2)
{
    struct ciuki_input_event e={.type=type,.code=code,.value=value,.value2=value2,.monotonic_ns=d.now_ns};
    desk_input(&d,&e);
}
static void point(int x,int y) { event(CIUKI_INPUT_MOTION,0,x-d.mouse_x,y-d.mouse_y); }
static void tap(int key) { event(CIUKI_INPUT_KEY,key,1,0); event(CIUKI_INPUT_KEY,key,0,0); }
static void click(void) { event(CIUKI_INPUT_BUTTON,1,1,0); event(CIUKI_INPUT_BUTTON,1,0,0); }
static void chord(int key) { event(CIUKI_INPUT_KEY,DU_KEY_LALT,1,0); tap(key); event(CIUKI_INPUT_KEY,DU_KEY_LALT,0,0); }
static void save(const char *name)
{
    if (!frames) return;
    char path[1024]; snprintf(path,sizeof(path),"%s/%s.ppm",frames,name);
    FILE *f=fopen(path,"wb"); assert(f); fprintf(f,"P6\n800 600\n255\n");
    desk_composite(&d,(struct desk_box){0,0,800,600});
    for (unsigned k=0;k<800*600;k++) {
        uint32_t p=d.pixels[k]; unsigned char rgb[3]={p>>16,p>>8,p}; assert(fwrite(rgb,1,3,f)==3);
    }
    assert(!fclose(f));
    /* Capture painting is separate from the runtime save-under buffer. */
    d.frame_valid=0; d.cursor_valid=0; d.scanout_valid=0;
    desk_damage_add(&d.damage,(struct desk_box){0,0,800,600},800,600);
}
static void frame_matches(void)
{
    fake_presents=0; assert(!desk_redraw(&d));
    uint32_t *saved=malloc(800*600*4); assert(saved); memcpy(saved,d.pixels,800*600*4);
    desk_composite(&d,(struct desk_box){0,0,800,600});
    assert(!memcmp(saved,d.pixels,800*600*4)); free(saved);
}
static void clean(void)
{
    for (int i=0;i<DESK_CLIENTS;i++) desk_drop_client(&d,i);
    desk_render_release(&d); free(d.pixels); assert(!fake_live());
}
static void setup(void)
{
    fake_reset(); desk_init(&d,&fake_ops,800,600); desk_shell_enable(&d);
    d.pixels=calloc(800*600,4); assert(d.pixels); d.output=0;
    d.now_ns=1000000000ull;
}
static int window_new(int channel,int surface,unsigned pid)
{
    fake_channel(channel); int i=desk_add_client(&d,channel); assert(i>=0);
    struct ciuki_message m; desk_message(&m,DESK_HELLO,4); m.sender_pid=pid; m.data[0]=2; m.data[2]=1;
    desk_receive(&d,i,&m); fake_surface(surface,320,240,0x426a83);
    desk_message(&m,DESK_CREATE_WINDOW,48); m.data[0]=2; m.data[2]=1; m.sender_pid=pid;
    m.fd_count=1; m.fds[0]=surface;
    desk_put32(m.data+4,320); desk_put32(m.data+8,240); desk_put32(m.data+12,4); memcpy(m.data+16,"Test",4);
    const unsigned values[]={DESK_RESIZABLE,160,120,768,544,8,8};
    for (unsigned k=0;k<7;k++) desk_put32(m.data+20+k*4,values[k]);
    desk_receive(&d,i,&m); assert(d.clients[i].window);
    uint32_t *p=(uint32_t *)d.clients[i].pixels;
    for (unsigned y=0;y<240;y++) for (unsigned x=0;x<320;x++) p[y*320+x]=0x284052u+((x/8+y/8)%2)*0x527282u;
    return i;
}
static void geometry(void)
{
    setup(); int i=window_new(10,11,42); struct desk_client *c=&d.clients[i];
    desk_move(&d,i,220,160); struct desk_box start=c->frame;
    const unsigned edges[]={1,2,4,8,5,6,9,10};
    const char *names[]={"resize-left","resize-right","resize-top","resize-bottom","resize-nw","resize-ne","resize-sw","resize-se"};
    for (unsigned n=0;n<8;n++) {
        c->frame=start;
        int x=edges[n]&1 ? start.x : edges[n]&2 ? start.x+start.w-1 : start.x+start.w/2;
        int y=edges[n]&4 ? start.y : edges[n]&8 ? start.y+start.h-1 : start.y+start.h/2;
        point(x,y);
#ifdef DESK_CURSOR_API
        const unsigned shapes[]={DESK_CURSOR_RESIZE_EW,DESK_CURSOR_RESIZE_EW,DESK_CURSOR_RESIZE_NS,DESK_CURSOR_RESIZE_NS,
            DESK_CURSOR_RESIZE_NWSE,DESK_CURSOR_RESIZE_NESW,DESK_CURSOR_RESIZE_NESW,DESK_CURSOR_RESIZE_NWSE};
        assert(d.cursor_shape==shapes[n]);
#endif
        save(names[n]); event(CIUKI_INPUT_BUTTON,1,1,0);
        assert(d.drag==i && d.resize_edges==edges[n]);
        point(x+(edges[n]&1 ? -24 : edges[n]&2 ? 24 : 0),y+(edges[n]&4 ? -24 : edges[n]&8 ? 24 : 0));
        save("resize-live"); event(CIUKI_INPUT_BUTTON,1,0,0);
#ifdef DESK_CURSOR_API
        assert(c->frame.w==start.w+((edges[n]&3)?24:0));
        assert(c->frame.h==start.h+((edges[n]&12)?24:0));
        if (edges[n]&1) assert(c->frame.x+c->frame.w==start.x+start.w);
        if (edges[n]&4) assert(c->frame.y+c->frame.h==start.y+start.h);
#endif
    }
    c->frame=start;
    point(start.x,start.y); event(CIUKI_INPUT_BUTTON,1,1,0); point(799,567);
#ifdef DESK_CURSOR_API
    assert(c->frame.w==166 && c->frame.h==147);
    assert(c->frame.x+c->frame.w==start.x+start.w && c->frame.y+c->frame.h==start.y+start.h);
#endif
    event(CIUKI_INPUT_BUTTON,1,0,0); c->frame=start;
    point(start.x+start.w-1,start.y+start.h-1); event(CIUKI_INPUT_BUTTON,1,1,0); point(799,567);
#ifdef DESK_CURSOR_API
    assert(c->frame.x+c->frame.w<=800 && c->frame.y+c->frame.h<=568);
    assert((c->frame.w-6-160)%8==0 && (c->frame.h-27-120)%8==0);
#endif
    event(CIUKI_INPUT_BUTTON,1,0,0); c->frame=start;
    point(start.x+90,start.y+12); click(); d.now_ns+=100000000; click(); save("double-title");
#ifdef DESK_CURSOR_API
    assert(c->zoomed && !c->collapsed && !desk_resize_hit(c,c->frame.x,c->frame.y));
    point(c->frame.x+90,c->frame.y+12);
    d.now_ns+=600000000; click(); d.now_ns+=100000000; click();
    assert(!c->zoomed && !memcmp(&start,&c->frame,sizeof(start)));
#else
    if (c->collapsed) desk_collapse(&d,i);
#endif
    chord(DU_KEY_SPACE); save("system-menu");
#ifdef DESK_CURSOR_API
    assert(d.sh.levels && d.sh.menu_owner==i);
    int row=-1; for (int k=0;k<d.sh.menus[0].count;k++) if (d.sh.menus[0].items[k].action==SH_ACT_TASK_SIZE) row=k;
    assert(row>=0); d.sh.menus[0].hot=row; tap(DU_KEY_ENTER);
    assert(d.keyboard_geometry==2 && d.drag==i);
    event(CIUKI_INPUT_BUTTON,1,0,0); assert(d.keyboard_geometry==2);
    tap(DU_KEY_RIGHT); tap(DU_KEY_DOWN); assert(c->frame.w==start.w+8 && c->frame.h==start.h+8);
    save("keyboard-size"); tap(DU_KEY_ESC); assert(!memcmp(&start,&c->frame,sizeof(start)));
    desk_geometry_begin(&d,i,0); tap(DU_KEY_LEFT); tap(DU_KEY_UP); tap(DU_KEY_ENTER);
    assert(c->frame.x==start.x-8 && c->frame.y==start.y-8 && d.drag==-1);
    desk_geometry_begin(&d,i,1); tap(DU_KEY_RIGHT); event(CIUKI_INPUT_RESYNC,0,0,0); assert(d.drag==-1 && !d.keyboard_geometry);
    struct desk_box stepped=c->frame;
    c->step_width=32; c->step_height=24;
    desk_geometry_begin(&d,i,1); tap(DU_KEY_RIGHT); tap(DU_KEY_DOWN);
    assert(c->frame.w==stepped.w+32 && c->frame.h==stepped.h+24);
    event(CIUKI_INPUT_KEY,DU_KEY_LALT,1,0); event(CIUKI_INPUT_KEY,DU_KEY_LALT,0,0);
    assert(!(d.sh.modifiers&DU_ALT)); tap(DU_KEY_ESC);
    assert(!memcmp(&stepped,&c->frame,sizeof(stepped)));
    c->step_width=c->step_height=8;
    point(c->frame.x+c->frame.w-1,c->frame.y+c->frame.h-1); event(CIUKI_INPUT_BUTTON,1,1,0);
    point(d.mouse_x+24,d.mouse_y+24); tap(DU_KEY_ESC); event(CIUKI_INPUT_BUTTON,1,0,0);
    assert(!memcmp(&stepped,&c->frame,sizeof(stepped)));
#else
    save("keyboard-size");
#endif
    desk_menu_close(&d); c->frame=start;
    frame_matches();
    desk_zoom(&d,i); d.now_ns+=75000000; desk_heartbeat(&d,d.now_ns); save("maximize-outline");
    d.now_ns+=200000000; desk_heartbeat(&d,d.now_ns); desk_zoom(&d,i);
    desk_minimize(&d,i); d.now_ns+=75000000; desk_heartbeat(&d,d.now_ns); save("minimize-outline");
    d.now_ns+=200000000; desk_heartbeat(&d,d.now_ns); desk_minimize(&d,i);
    d.now_ns+=75000000; desk_heartbeat(&d,d.now_ns); save("restore-outline");
#ifdef DESK_CURSOR_API
    assert(d.animation.active && d.animation.step==3);
    frame_matches();
    d.now_ns+=25000000; desk_heartbeat(&d,d.now_ns); frame_matches();
    assert(d.frame_stats.scene_px<30000); /* only old/new outline strips */
    d.now_ns+=200000000; desk_heartbeat(&d,d.now_ns); assert(!d.animation.active); frame_matches();
#endif
    desk_move(&d,i,-500,-500); assert(c->frame.y>=32); desk_move(&d,i,5000,5000);
    assert(c->frame.y<=568-DESK_TITLE); save("drag-reachable"); clean();
}
static void windows(void)
{
    setup(); int a=window_new(10,11,42); int b=window_new(12,13,42); int c=window_new(14,15,43);
    desk_focus(&d,a); desk_focus(&d,c); desk_focus(&d,a); desk_drop_client(&d,a); save("close-focus");
#ifdef DESK_CURSOR_API
    assert(d.focus==b);
#endif
    a=window_new(16,17,44);
#ifdef DESK_CURSOR_API
    assert(d.clients[a].frame.x!=20);
#endif
    save("cascade"); point(d.clients[b].frame.x+40,d.clients[b].frame.y+50);
    /* The active top window cannot receive another window's content click. */
    desk_move(&d,a,440,250); desk_move(&d,c,440,64);
    point(d.clients[b].frame.x+40,d.clients[b].frame.y+50); event(CIUKI_INPUT_BUTTON,2,1,0); event(CIUKI_INPUT_BUTTON,2,0,0);
#ifdef DESK_CURSOR_API
    assert(d.focus==b);
#endif
    save("secondary-activation");
    event(CIUKI_INPUT_KEY,DU_KEY_LALT,1,0); tap(DU_KEY_TAB); assert(d.sh.switcher); save("alt-tab");
    event(CIUKI_INPUT_KEY,DU_KEY_LALT,0,0); assert(!d.sh.switcher);
    chord(DU_KEY_ESC); save("alt-esc");
    desk_focus(&d,b); struct desk_box owner=d.clients[b].frame;
    desk_shell_message(&d,"Question","The owner waits for this dialog.",0); int modal=d.focus; save("modal-owner");
#ifdef DESK_CURSOR_API
    assert(d.clients[modal].owner_id==d.clients[b].window_id);
    struct desk_box box=d.clients[modal].frame;
    int x=owner.x+(owner.w-box.w)/2, y=owner.y+(owner.h-box.h)/2;
    if (y<32) y=32;
    assert(box.x==x && box.y==y);
    desk_focus(&d,b); assert(d.focus==modal);
    chord(DU_KEY_ESC); assert(d.focus!=modal && d.clients[modal].order>d.clients[b].order);
    desk_focus(&d,b); assert(d.focus==modal);
    desk_minimize(&d,b); assert(!d.clients[b].minimized && d.focus==modal);
    desk_request_close(&d,modal); assert(d.focus==b);
#else
    (void)owner; (void)modal;
#endif
    clean();
}
static void cursors(void)
{
    setup(); int i=window_new(10,11,42); desk_move(&d,i,200,160);
    const char *names[]={"arrow","busy","arrow-busy","text","forbidden","resize-ew","resize-ns","resize-nwse","resize-nesw","move","hand","crosshair","precision"};
    for (unsigned n=0;n<13;n++) {
        point(340,270);
#ifdef DESK_CURSOR_API
        struct ciuki_message m; desk_message(&m,DESK_SET_CURSOR,8); m.data[0]=2; m.data[2]=1; m.sender_pid=42;
        desk_put32(m.data+4,n); desk_receive(&d,i,&m); assert(d.cursor_shape==n);
        struct desk_packet p; assert(desk_parse(&m,1,NULL,&p)); assert(!desk_parse(&m,0,NULL,&p));
        desk_put32(m.data+4,DESK_CURSOR_COUNT); assert(!desk_parse(&m,1,NULL,&p));
#endif
        char name[64]; snprintf(name,sizeof(name),"cursor-%s",names[n]); save(name);
    }
    int run=desk_internal_open(&d,DESK_INTERNAL_RUN);
    struct desk_box field=desk_content(&d.clients[run]);
    point(field.x+60,field.y+60); save("cursor-run-field");
#ifdef DESK_CURSOR_API
    assert(d.cursor_shape==DESK_CURSOR_TEXT);
    point(field.x+60,field.y+35); assert(d.cursor_shape==DESK_CURSOR_ARROW);
#endif
#ifdef DESK_CURSOR_API
    struct du_ui u; uint32_t *pixels=calloc(400*300,4); assert(pixels); du_init(&u,pixels,400,300);
    du_add(&u,DU_EDIT,1,(struct du_rect){10,30,150,24},"Ciuki"); u.mouse_x=20; u.mouse_y=40;
    assert(du_cursor_at(&u)==DESK_CURSOR_TEXT);
    du_add(&u,DU_SPLITTER,2,(struct du_rect){180,30,5,220},"")->vertical=1; u.mouse_x=182;
    assert(du_cursor_at(&u)==DESK_CURSOR_RESIZE_EW);
    struct du_widget *canvas=du_add(&u,DU_CANVAS,3,(struct du_rect){200,30,180,220},""); canvas->pointer_cursor=DESK_CURSOR_HAND;
    u.mouse_x=240; assert(du_cursor_at(&u)==DESK_CURSOR_HAND);
    canvas->disabled=1; assert(du_cursor_at(&u)==DESK_CURSOR_FORBIDDEN);
    du_message(&u,1,"Modal","Owner blocked",0,NULL,0,0,0); u.mouse_x=1; u.mouse_y=1;
    assert(du_cursor_at(&u)==DESK_CURSOR_FORBIDDEN);
    struct du_rect old=u.dialog.box; du_resize(&u,pixels,380,280);
    assert(u.dialog.box.x==(380-old.w)/2 && u.dialog.box.y==(280-old.h)/2);
    du_dialog_close(&u); free(pixels);
#endif
    clean();
}
static void application_focus(void)
{
#ifdef DESK_CURSOR_API
    setup(); int a=window_new(10,11,42), b=window_new(12,13,43), c=window_new(14,15,44);
    strcpy(d.clients[a].application,"ciuknote"); strcpy(d.clients[b].application,"ciuknote");
    strcpy(d.clients[c].application,"files");
    desk_focus(&d,a); desk_drop_client(&d,a); assert(d.focus==b);
    desk_focus(&d,b); desk_minimize(&d,b); assert(d.focus==c);
    desk_minimize(&d,b); assert(d.focus==b); clean();
#endif
}
int main(int argc,char **argv)
{
    frames=argc>1 ? argv[1] : NULL;
    geometry(); windows(); cursors(); application_focus();
#ifdef DESK_CURSOR_API
    puts("PASS interaction replay: eight resize directions, stepped screen limits, title maximize/restore, keyboard accept/cancel/resync, outlines, cascade, same-process close focus, modal owner, shortcuts, thirteen cursors and toolkit hit tests");
#else
    puts("PASS baseline interaction capture: current behaviour recorded; refinement assertions unavailable");
#endif
    return 0;
}
