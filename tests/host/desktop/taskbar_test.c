/* G4: the taskbar menu, window arrangement and the Windows-key shortcuts of
 * the production desktop (apps/desktop), one assertion per Windows 98 behaviour
 * (docs/design/os-feature-inventory.md WM-11, WM-12, WM-13, KB-5, KB-8, KB-9,
 * KB-12, KB-13, SM-9). Every state is also compared with a full redraw.
 * SPDX-License-Identifier: MIT */
#include "fake.h"
#include <deskui.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 800
#define H 600
#define KEY_WIN 0x15b
#define KEY_RWIN 0x15c
#define KEY_MENU 0x15d
#define KEY_PAUSE 0x200
#define KEY_F3 0x3d
#define KEY_F6 0x40
#define KEY_M 0x32
static struct desktop d;
static unsigned checked;
static char launched[64], launched_arg[96];
static unsigned launch_count;

static void event(unsigned type,int code,int value,int value2)
{
    struct ciuki_input_event e={.type=type,.code=code,.value=value,.value2=value2,.monotonic_ns=d.now_ns};
    desk_input(&d,&e);
}
static void point(int x,int y) { event(CIUKI_INPUT_MOTION,0,x-d.mouse_x,y-d.mouse_y); }
static void key(int code,int value) { event(CIUKI_INPUT_KEY,code,value,0); }
static void tap(int code) { key(code,1); key(code,0); }
static void click_at(int x,int y,int button) { point(x,y); event(CIUKI_INPUT_BUTTON,button,1,0); event(CIUKI_INPUT_BUTTON,button,0,0); }
static void left(int x,int y) { click_at(x,y,CIUKI_BUTTON_LEFT); }
static void right(int x,int y) { click_at(x,y,CIUKI_BUTTON_RIGHT); }
/* modifier held around one key */
static void with(int modifier,int code) { key(modifier,1); tap(code); key(modifier,0); }
static void win(int code) { with(KEY_WIN,code); }
static void alt(int code) { with(DU_KEY_LALT,code); }

/* every state must equal a full redraw */
static void frame_matches(const char *what)
{
    fake_presents=0; assert(!desk_redraw(&d));
    uint32_t *saved=malloc((size_t)W*H*4); assert(saved); memcpy(saved,d.pixels,(size_t)W*H*4);
    desk_composite(&d,(struct desk_box){0,0,W,H});
    if (memcmp(saved,d.pixels,(size_t)W*H*4)) { fprintf(stderr,"frame differs from a full redraw: %s\n",what); abort(); }
    free(saved); checked++;
}
static int fake_launch(struct desktop *dd,const struct desk_program *p)
{
    snprintf(launched,sizeof(launched),"%s",p->name);
    snprintf(launched_arg,sizeof(launched_arg),"%s",dd->launch_arg ? dd->launch_arg : "");
    return (int)(100+ ++launch_count);
}
static int next_channel=10;
static void setup(void)
{
    next_channel=10; fake_reset(); desk_init(&d,&fake_ops,W,H); desk_shell_enable(&d);
    d.pixels=calloc((size_t)W*H,4); assert(d.pixels); d.output=0;
    d.now_ns=1000000000ull; d.launch=fake_launch;
    static const char *const names[]={"files","control","help","tasks","ciuknote"};
    for (unsigned k=0;k<5;k++) {
        snprintf(d.programs[k].name,sizeof(d.programs[k].name),"%s",names[k]);
        snprintf(d.programs[k].path,sizeof(d.programs[k].path),"/Programs/%s",names[k]);
    }
    d.program_count=5; launched[0]=launched_arg[0]=0;
    frame_matches("empty desktop");
}
static void clean(void)
{
    for (int i=0;i<DESK_CLIENTS;i++) desk_drop_client(&d,i);
    desk_render_release(&d); free(d.sh.icons); d.sh.icons=NULL; free(d.pixels); assert(!fake_live());
}
static int window_new(unsigned pid,const char *application)
{
    int channel=next_channel, surface=next_channel+1; next_channel+=2;
    fake_channel(channel); int i=desk_add_client(&d,channel); assert(i>=0);
    struct ciuki_message m; desk_message(&m,DESK_HELLO,4); m.sender_pid=pid; m.data[0]=2; m.data[2]=1;
    desk_receive(&d,i,&m); fake_surface(surface,320,240,0x426a83);
    desk_message(&m,DESK_CREATE_WINDOW,48); m.data[0]=2; m.data[2]=1; m.sender_pid=pid;
    m.fd_count=1; m.fds[0]=surface;
    desk_put32(m.data+4,320); desk_put32(m.data+8,240); desk_put32(m.data+12,4); memcpy(m.data+16,"Test",4);
    const unsigned values[]={DESK_RESIZABLE,160,120,768,544,8,8};
    for (unsigned k=0;k<7;k++) desk_put32(m.data+20+k*4,values[k]);
    desk_receive(&d,i,&m); assert(d.clients[i].window);
    snprintf(d.clients[i].application,sizeof(d.clients[i].application),"%s",application);
    uint32_t *p=(uint32_t *)d.clients[i].pixels;
    for (unsigned y=0;y<240;y++) for (unsigned x=0;x<320;x++) p[y*320+x]=0x284052u+((x/8+y/8)%2)*0x527282u;
    return i;
}
/* key events of one code queued for a client */
static unsigned key_messages(int i,int code)
{
    unsigned n=0; const struct desk_client *c=&d.clients[i];
    for (unsigned k=0;k<c->tx_count;k++) {
        const struct ciuki_message *m=&c->tx[(c->tx_head+k)%DESK_TX_MAX];
        n+=m->data[1]==DESK_KEY && desk_get32(m->data+28)==(uint32_t)code;
    }
    return n;
}
static struct desk_box work(void) { return (struct desk_box){0,DESK_BAR,W,H-2*DESK_BAR}; }
static int inside(struct desk_box f,struct desk_box w)
{ return f.x>=w.x && f.y>=w.y && f.x+f.w<=w.x+w.w && f.y+f.h<=w.y+w.h; }
static int menu_item(const char *label)
{
    for (int k=0;k<d.sh.menus[0].count;k++) if (!strncmp(d.sh.menus[0].items[k].label,label,strlen(label))) return k;
    return -1;
}
static void menu_click(int level,int k)
{
    struct sh_menu *m=&d.sh.menus[level];
    int y=m->box.y+3; for (int j=0;j<k;j++) y+=m->items[j].flags&SH_ITEM_SEPARATOR ? SH_SEPARATOR : m->row_h;
    left(m->box.x+20,y+m->row_h/2);
}
static int task_menu_visible(void) { return d.sh.levels && d.sh.menus[0].kind==SH_MENU_TASKBAR; }
static int empty_bar_x(void) { return W-60; }

/* ---- WM-12: Cascade Windows ---------------------------------------------- */
static void cascade(void)
{
    setup(); int a=window_new(42,"files"), b=window_new(43,"calc"), c=window_new(44,"ciuknote");
    desk_focus(&d,a); desk_focus(&d,b); desk_focus(&d,c);
    uint64_t order[3]={d.clients[a].order,d.clients[b].order,d.clients[c].order};
    assert(desk_arrange_undo_kind(&d)==DESK_ARRANGE_NONE);
    assert(desk_arrange(&d,DESK_ARRANGE_CASCADE)==3);
    struct desk_box wa=work(), fa=d.clients[a].frame, fb=d.clients[b].frame, fc=d.clients[c].frame;
    /* bottom window at the top left, each next one a caption lower and further right */
    assert(fa.x==wa.x && fa.y==wa.y);
    assert(fb.x==fa.x+DESK_TITLE && fb.y==fa.y+DESK_TITLE && fc.x==fb.x+DESK_TITLE && fc.y==fb.y+DESK_TITLE);
    assert(fa.w==fb.w && fb.w==fc.w && fa.h==fb.h && fb.h==fc.h);
    assert(fa.w<=wa.w*3/4 && fa.w>wa.w*3/4-16 && fa.h<=wa.h*3/4 && fa.h>wa.h*3/4-16);
    assert(inside(fa,wa) && inside(fb,wa) && inside(fc,wa));
    /* the stacking does not change: the window that was in front still is */
    assert(d.clients[a].order==order[0] && d.clients[b].order==order[1] && d.clients[c].order==order[2] && d.focus==c);
    assert(desk_arrange_undo_kind(&d)==DESK_ARRANGE_CASCADE);
    frame_matches("cascade");
    clean();
    /* seven windows: the cascade starts over at the top left once the next offset leaves the work area */
    setup(); int w[7]; for (int k=0;k<7;k++) w[k]=window_new(50+(unsigned)k,"calc");
    assert(desk_arrange(&d,DESK_ARRANGE_CASCADE)==7);
    for (int k=0;k<7;k++) assert(inside(d.clients[w[k]].frame,wa=work()));
    assert(d.clients[w[6]].frame.x==wa.x && d.clients[w[6]].frame.y==wa.y);
    assert(d.clients[w[5]].frame.y>d.clients[w[4]].frame.y);
    frame_matches("cascade of seven");
    clean();
}

/* ---- WM-12: Tile Windows Horizontally / Vertically ----------------------- */
static struct desk_box cell(int n,int vertical,int index)
{
    struct desk_box wa=work(); int cols, rows;
    if (n<=3) cols=vertical ? n : 1;
    else { int a=1; while (a*a<n) a++; int b=(n+a-1)/a; cols=vertical ? a : b; }
    rows=(n+cols-1)/cols;
    int r=0, first=0;
    while (r<rows-1 && index>=first+cols) { first+=cols; r++; }
    int count=r<rows-1 ? cols : n-(rows-1)*cols, col=index-first;
    return (struct desk_box){wa.x+wa.w*col/count,wa.y+wa.h*r/rows,wa.w*(col+1)/count-wa.w*col/count,wa.h*(r+1)/rows-wa.h*r/rows};
}
static void tile(void)
{
    for (int n=1;n<=7;n++) for (int vertical=0;vertical<2;vertical++) {
        setup(); int w[8];
        for (int k=0;k<n;k++) w[k]=window_new(60+(unsigned)k,"calc");
        desk_focus(&d,w[0]);   /* the window in front takes the first cell */
        assert(desk_arrange(&d,vertical ? DESK_ARRANGE_TILE_V : DESK_ARRANGE_TILE_H)==n);
        assert(desk_arrange_undo_kind(&d)==(vertical ? DESK_ARRANGE_TILE_V : DESK_ARRANGE_TILE_H));
        /* windows in stacking order, top first */
        int ordered[8], count=0;
        for (int k=0;k<n;k++) ordered[count++]=w[k];
        for (int x=1;x<count;x++) for (int y=x;y>0 && d.clients[ordered[y-1]].order<d.clients[ordered[y]].order;y--)
            { int t=ordered[y]; ordered[y]=ordered[y-1]; ordered[y-1]=t; }
        assert(ordered[0]==w[0]);
        for (int k=0;k<n;k++) {
            struct desk_box f=d.clients[ordered[k]].frame, c=cell(n,vertical,k);
            /* the cell, less what the window's size step leaves over */
            int widest=f.w==768+2*DESK_BORDER;   /* the program's own maximum */
            assert(f.x==c.x && f.y==c.y && f.w<=c.w && (f.w>c.w-14 || widest) && f.h<=c.h && f.h>c.h-14);
            assert(inside(f,work()));
        }
        if (n==2 && vertical) assert(d.clients[w[0]].frame.y==d.clients[w[1]].frame.y);
        if (n==2 && !vertical) assert(d.clients[w[0]].frame.x==d.clients[w[1]].frame.x);
        if (n==5 && !vertical) {   /* three rows of two, the last one a single wide window */
            assert(cell(5,0,4).w==W && cell(5,0,0).w==W/2);
        }
        frame_matches("tile");
        clean();
    }
}

/* ---- which windows an arrangement leaves alone ---------------------------- */
static void exclusions(void)
{
    setup(); int a=window_new(42,"files"), b=window_new(43,"calc"), c=window_new(44,"ciuknote"), e=window_new(45,"terminal");
    desk_move(&d,e,500,300);
    desk_minimize(&d,b); assert(d.clients[b].minimized);
    struct desk_box fb=d.clients[b].frame;
    /* a modal question owned by the first window: both stay where they are */
    desk_focus(&d,a); desk_shell_message(&d,"Question","Owned.",0); int owned=d.focus; assert(d.clients[owned].internal && d.clients[owned].owner_id);
    desk_focus(&d,c);
    int run=desk_internal_open(&d,DESK_INTERNAL_RUN); struct desk_box frun=d.clients[run].frame;
    /* Win32 windows: topmost, tool and popup styles keep their place */
    d.clients[e].version=DESK_V3_VERSION; d.clients[e].v3_style=DESK_V3_OVERLAPPED|DESK_V3_TOPMOST;
    assert(!desk_arrangeable(&d,e) && !desk_arrangeable(&d,b) && !desk_arrangeable(&d,run) && !desk_arrangeable(&d,owned));
    d.clients[e].v3_style=DESK_V3_TOOL; assert(!desk_arrangeable(&d,e));
    d.clients[e].v3_style=DESK_V3_POPUP; assert(!desk_arrangeable(&d,e));
    d.clients[e].v3_style=DESK_V3_OVERLAPPED; assert(desk_arrangeable(&d,e));
    d.clients[e].v3_style=DESK_V3_OVERLAPPED|DESK_V3_TOPMOST;
    struct desk_box fe=d.clients[e].frame;
    assert(!desk_arrangeable(&d,a));   /* the owner of the question stays with it */
    struct desk_box fa=d.clients[a].frame;
    int moved=desk_arrange(&d,DESK_ARRANGE_CASCADE);
    assert(moved==1 && d.clients[c].frame.x==0 && !memcmp(&fa,&d.clients[a].frame,sizeof(fa)));
    assert(!memcmp(&fb,&d.clients[b].frame,sizeof(fb)) && d.clients[b].minimized);
    assert(!memcmp(&frun,&d.clients[run].frame,sizeof(frun)) && !memcmp(&fe,&d.clients[e].frame,sizeof(fe)));
    assert(!desk_arrangeable(&d,owned));
    frame_matches("exclusions");
    /* shutdown in progress: nothing is arranged */
    d.power.state=DESK_POWER_CLOSING; assert(!desk_arrange(&d,DESK_ARRANGE_TILE_V) && !desk_minimize_all(&d)); d.power.state=0;
    clean();
    /* a window that cannot be resized keeps its size; a maximised or shaded one is restored first */
    setup(); a=window_new(42,"files"); b=window_new(43,"calc"); c=window_new(44,"ciuknote");
    d.clients[a].resizable=0; struct desk_box fixed=d.clients[a].frame;
    desk_zoom(&d,b); assert(d.clients[b].zoomed);
    desk_collapse(&d,c); assert(d.clients[c].collapsed);
    assert(desk_arrange(&d,DESK_ARRANGE_TILE_V)==3);
    assert(d.clients[a].frame.w==fixed.w && d.clients[a].frame.h==fixed.h && inside(d.clients[a].frame,work()));
    assert(!d.clients[b].zoomed && !d.clients[c].collapsed);
    assert(d.clients[b].frame.w<W && d.clients[c].frame.h>DESK_TITLE);
    frame_matches("restored before arranging");
    /* Undo returns every window to the place, size and state it had */
    assert(desk_arrange_undo(&d)==3);
    assert(d.clients[b].zoomed && d.clients[c].collapsed && d.clients[c].frame.h==DESK_TITLE);
    assert(d.clients[a].frame.x==fixed.x && d.clients[a].frame.y==fixed.y);
    assert(desk_arrange_undo_kind(&d)==DESK_ARRANGE_NONE);
    frame_matches("undo restores state");
    clean();
}

/* ---- WM-12/WM-13: Minimize All, Show Desktop, Undo ----------------------- */
static void minimize_all(void)
{
    setup(); int a=window_new(42,"files"), b=window_new(43,"calc"), c=window_new(44,"ciuknote");
    desk_focus(&d,b);
    desk_shell_message(&d,"Owned","A dialog of the focused window.",0); int owned=d.focus;
    assert(d.clients[owned].owner_id==d.clients[b].window_id);
    struct desk_box fa=d.clients[a].frame;
    assert(desk_minimize_all(&d)==3);
    assert(d.clients[a].minimized && d.clients[b].minimized && d.clients[c].minimized && d.clients[owned].minimized);
    assert(d.focus<0 && !memcmp(&fa,&d.clients[a].frame,sizeof(fa)));
    assert(desk_arrange_undo_kind(&d)==DESK_ARRANGE_MINIMIZE_ALL);
    frame_matches("minimize all");
    assert(desk_minimize_all(&d)==0);   /* nothing left to minimise */
    assert(desk_arrange_undo(&d)==4);
    assert(!d.clients[a].minimized && !d.clients[b].minimized && !d.clients[c].minimized && !d.clients[owned].minimized);
    assert(d.focus==owned);   /* the window that was active is active again */
    frame_matches("undo minimize all");
    /* Show Desktop toggles */
    assert(desk_show_desktop(&d)==3 && d.clients[a].minimized && d.focus<0);
    frame_matches("show desktop");
    assert(desk_show_desktop(&d)>0 && !d.clients[a].minimized && !d.clients[c].minimized);
    frame_matches("show desktop again");
    /* a window restored by hand stays; a window closed meanwhile is skipped */
    assert(desk_minimize_all(&d)>0);
    struct desk_box t=desk_task_button(&d,a); left(t.x+5,t.y+5); assert(!d.clients[a].minimized && d.focus==a);
    desk_drop_client(&d,c);
    assert(desk_arrange_undo(&d)>0 && !d.clients[b].minimized);
    frame_matches("undo after a close");
    clean();
}

/* ---- WM-12: the taskbar menu --------------------------------------------- */
static void taskbar_menu(void)
{
    setup();
    /* no windows: arrangements are disabled, Properties is enabled */
    right(empty_bar_x(),H-16);
    assert(task_menu_visible() && d.sh.menus[0].count==6);
    assert(d.sh.menus[0].items[0].flags&SH_ITEM_DISABLED && d.sh.menus[0].items[menu_item("Minimize All Windows")].flags&SH_ITEM_DISABLED);
    assert(!(d.sh.menus[0].items[menu_item("Properties")].flags&SH_ITEM_DISABLED));
    frame_matches("taskbar menu, no windows");
    tap(DU_KEY_ESC); assert(!d.sh.levels);
    int a=window_new(42,"files"), b=window_new(43,"calc"), c=window_new(44,"ciuknote");
    /* a right click on a task button is still the window's own menu */
    struct desk_box t=desk_task_button(&d,b); right(t.x+4,t.y+4); assert(d.sh.levels && d.sh.menus[0].kind==SH_MENU_TASK); tap(DU_KEY_ESC);
    /* the empty part of the taskbar, and of the top bar between the Start button and the tray */
    right(empty_bar_x(),H-16);
    assert(task_menu_visible() && d.sh.menus[0].box.y+d.sh.menus[0].box.h<=H-DESK_BAR);
    const char *expected[]={"Cascade Windows","Tile Windows Horizontally","Tile Windows Vertically","Minimize All Windows"};
    for (unsigned k=0;k<4;k++) assert(!strcmp(d.sh.menus[0].items[k].label,expected[k]) && !(d.sh.menus[0].items[k].flags&SH_ITEM_DISABLED));
    assert(d.sh.menus[0].items[4].flags&SH_ITEM_SEPARATOR && !strcmp(d.sh.menus[0].items[5].label,"Properties"));
    frame_matches("taskbar menu");
    tap(DU_KEY_ESC);
    right(300,10); assert(task_menu_visible() && d.sh.menus[0].box.y>=DESK_BAR); tap(DU_KEY_ESC);
    /* the Start button and left clicks on the empty bar do nothing */
    left(empty_bar_x(),H-16); assert(!d.sh.levels);
    /* Cascade Windows by mouse; the menu then offers Undo Cascade */
    right(empty_bar_x(),H-16); menu_click(0,menu_item("Cascade Windows"));
    assert(!d.sh.levels && d.clients[b].frame.x==d.clients[a].frame.x+DESK_TITLE);
    frame_matches("cascade by menu");
    right(empty_bar_x(),H-16);
    assert(menu_item("Undo Cascade")>=0 && menu_item("Minimize All Windows")>=0);
    menu_click(0,menu_item("Tile Windows Vertically"));
    assert(d.clients[a].frame.y==d.clients[b].frame.y && d.clients[b].frame.y==d.clients[c].frame.y);
    right(empty_bar_x(),H-16); assert(menu_item("Undo Tile")>=0 && menu_item("Undo Cascade")<0);
    menu_click(0,menu_item("Tile Windows Horizontally"));
    assert(d.clients[a].frame.x==d.clients[b].frame.x);
    right(empty_bar_x(),H-16); menu_click(0,menu_item("Undo Tile"));
    assert(d.clients[a].frame.y==d.clients[b].frame.y && d.clients[b].frame.x!=d.clients[a].frame.x);   /* the tiling before: side by side */
    /* Minimize All Windows replaces its item by Undo Minimize All */
    right(empty_bar_x(),H-16); menu_click(0,menu_item("Minimize All Windows"));
    assert(d.clients[a].minimized && d.clients[b].minimized && d.clients[c].minimized);
    frame_matches("minimize all by menu");
    right(empty_bar_x(),H-16);
    assert(menu_item("Undo Minimize All")>=0 && menu_item("Minimize All Windows")<0);
    menu_click(0,menu_item("Undo Minimize All"));
    assert(!d.clients[a].minimized && !d.clients[b].minimized && !d.clients[c].minimized);
    /* Properties: the Control Panel until the taskbar properties exist (G20) */
    right(empty_bar_x(),H-16); menu_click(0,menu_item("Properties"));
    assert(!strcmp(launched,"control"));
    clean();
}

/* ---- SM-9: access keys in the shell's menus ------------------------------ */
static void mnemonics(void)
{
    setup(); int a=window_new(42,"files"); (void)a;
    tap(KEY_WIN); assert(d.menu_open && d.sh.menus[0].kind==SH_MENU_START);
    const struct sh_menu *m=&d.sh.menus[0];
    /* underlined letters, the Windows 98 assignments */
    const struct { const char *label; char key; } start[]={{"Programs",'p'},{"Documents",'d'},{"Settings",'s'},{"Help",'h'},{"Run...",'r'},{"Shut Down...",'u'}};
    for (unsigned k=0;k<6;k++) {
        int at=menu_item(start[k].label); assert(at>=0 && m->items[at].mark);
        assert(m->items[at].label[m->items[at].mark-1]==start[k].key || m->items[at].label[m->items[at].mark-1]==start[k].key-32);
    }
    frame_matches("Start menu with access keys");
    tap(DU_KEY_END); assert(m->hot==menu_item("Shut Down...")); tap(DU_KEY_HOME); assert(m->hot==0); frame_matches("Home and End");
    tap(0x13); assert(!d.sh.levels && d.focus>=0 && d.clients[d.focus].internal==DESK_INTERNAL_RUN);   /* R: Run */
    desk_request_close(&d,d.focus);
    tap(KEY_WIN); tap(0x16); assert(d.focus>=0 && d.clients[d.focus].internal==DESK_INTERNAL_SHUTDOWN);   /* U: Shut Down */
    desk_request_close(&d,d.focus);
    tap(KEY_WIN); tap(0x19); assert(d.sh.levels==2 && d.sh.menus[1].kind==SH_MENU_PROGRAMS);   /* P: the submenu opens */
    frame_matches("submenu by access key");
    tap(DU_KEY_ESC); tap(DU_KEY_ESC);
    /* a menu without access keys jumps to the next entry that starts with the letter */
    tap(KEY_WIN); tap(0x19);
    int hot=d.sh.menus[1].hot; (void)hot;
    tap(DU_KEY_ESC); tap(DU_KEY_ESC);
    /* the window menu: Alt+Space, then Mi&nimize / Ma&ximize / &Close */
    desk_focus(&d,a);
    alt(DU_KEY_SPACE); assert(d.sh.levels && d.sh.menus[0].kind==SH_MENU_TASK && d.sh.menu_owner==a);
    assert(d.sh.menus[0].items[menu_item("Minimize")].mark==3 && d.sh.menus[0].items[menu_item("Maximize")].mark==3);
    tap(0x2d); assert(d.clients[a].zoomed && !d.sh.levels);      /* X: Maximize */
    alt(DU_KEY_SPACE); tap(0x2e); assert(d.clients[a].closing);  /* C: Close */
    clean();
    /* two items with one letter: the highlight moves; disabled items never run */
    setup(); a=window_new(42,"files"); desk_focus(&d,a);
    right(empty_bar_x(),H-16); tap(0x16); /* U with no Undo item: nothing */
    assert(d.sh.levels);
    tap(0x2e); assert(!d.sh.levels && d.clients[a].frame.x==0);   /* C: Cascade Windows */
    clean();
}

/* ---- KB-12: Win key shortcuts -------------------------------------------- */
static void windows_keys(void)
{
    setup(); int a=window_new(42,"files"), b=window_new(43,"calc"); desk_focus(&d,b);
    /* a lone tap opens and closes Start, either Windows key */
    tap(KEY_WIN); assert(d.menu_open); tap(KEY_WIN); assert(!d.menu_open);
    tap(KEY_RWIN); assert(d.menu_open); tap(KEY_RWIN); assert(!d.menu_open);
    /* Win+D: Show Desktop, no Start menu; again, the windows return */
    win(0x20); assert(!d.menu_open && d.clients[a].minimized && d.clients[b].minimized && d.focus<0);
    frame_matches("Win+D");
    key(KEY_RWIN,1); tap(0x20); key(KEY_RWIN,0); assert(!d.clients[a].minimized && !d.clients[b].minimized && d.focus==b);
    /* Win+M and Shift+Win+M */
    win(KEY_M); assert(d.clients[a].minimized && d.clients[b].minimized);
    win(KEY_M); assert(d.clients[a].minimized);   /* nothing more to minimise */
    key(DU_KEY_LSHIFT,1); win(KEY_M); key(DU_KEY_LSHIFT,0); assert(!d.clients[a].minimized && !d.clients[b].minimized && d.focus==b);
    /* Win+E: Computer */
    win(0x12); assert(!strcmp(launched,"files") && !strcmp(launched_arg,"Computer"));
    /* Win+R */
    win(0x13); assert(d.focus>=0 && d.clients[d.focus].internal==DESK_INTERNAL_RUN); desk_request_close(&d,d.focus);
    /* Win+F and Win+Ctrl+F: Find is G7's; a message says so */
    win(0x21); assert(d.focus>=0 && d.clients[d.focus].internal==DESK_INTERNAL_MESSAGE && strstr(d.clients[d.focus].title,"Find"));
    assert(strstr(d.sh.message_title,"Files or Folders")); desk_request_close(&d,d.focus);
    key(KEY_WIN,1); key(DU_KEY_LCTRL,1); tap(0x21); key(DU_KEY_LCTRL,0); key(KEY_WIN,0);
    assert(strstr(d.sh.message_title,"Computer") && d.focus>=0 && d.clients[d.focus].internal==DESK_INTERNAL_MESSAGE);
    desk_request_close(&d,d.focus);
    /* Win+Break: System Properties */
    win(KEY_PAUSE); assert(!strcmp(launched,"control") && !strcmp(launched_arg,"--panel=system"));
    /* Win+F1: Help */
    win(DU_KEY_F1); assert(!strcmp(launched,"help"));
    /* none of the chords reached the program that had the focus */
    desk_focus(&d,b);
    const int chords[]={0x20,KEY_M,0x12,0x13,0x21,KEY_PAUSE,DU_KEY_F1,KEY_WIN,KEY_RWIN};
    for (unsigned k=0;k<sizeof(chords)/sizeof(*chords);k++) assert(!key_messages(b,chords[k]));
    /* a key the shortcuts do not use still reaches the program */
    key(KEY_WIN,1); tap(0x2c); key(KEY_WIN,0); assert(key_messages(b,0x2c)==2 && !d.menu_open);
    /* the Start menu open: Win+D closes it first */
    tap(KEY_WIN); assert(d.menu_open); win(0x20); assert(!d.sh.levels && !d.menu_open);
    clean();
    /* a cut input stream releases the Windows keys */
    setup(); window_new(42,"files"); key(KEY_WIN,1); assert(d.sh.logo_keys==1); event(CIUKI_INPUT_RESYNC,0,0,0); assert(!d.sh.logo_keys && !d.sh.logo_down);
    tap(KEY_WIN); assert(d.menu_open);
    clean();
}

/* ---- KB-12/KB-13: Win+Tab and the taskbar's keyboard focus --------------- */
static void taskbar_focus(void)
{
    setup(); int a=window_new(42,"files"), b=window_new(43,"calc"), c=window_new(44,"ciuknote");
    desk_focus(&d,b);
    /* Win+Tab moves to the button after the active window without activating */
    win(DU_KEY_TAB); assert(d.sh.taskbar_key && d.sh.task_pick==c && d.focus<0);
    frame_matches("Win+Tab");
    win(DU_KEY_TAB); assert(d.sh.task_pick==a);          /* wraps */
    key(DU_KEY_LSHIFT,1); win(DU_KEY_TAB); key(DU_KEY_LSHIFT,0); assert(d.sh.task_pick==c);   /* Shift+Win+Tab goes back */
    tap(DU_KEY_ENTER); assert(!d.sh.taskbar_key && d.focus==c);
    frame_matches("Enter on a task button");
    /* Esc gives the focus back */
    win(DU_KEY_TAB); assert(d.sh.taskbar_key); tap(DU_KEY_ESC); assert(!d.sh.taskbar_key && d.focus==c);
    /* Ctrl+Esc opens Start, Esc closes it and leaves the focus on Start; Tab moves on */
    with(DU_KEY_LCTRL,DU_KEY_ESC); assert(d.menu_open); tap(DU_KEY_ESC);
    assert(!d.menu_open && d.sh.taskbar_key && d.sh.task_pick==DESK_CLIENTS && d.focus<0);
    frame_matches("Ctrl+Esc, Esc");
    tap(DU_KEY_TAB); assert(d.sh.task_pick==a);
    tap(DU_KEY_RIGHT); assert(d.sh.task_pick==b);
    tap(DU_KEY_LEFT); tap(DU_KEY_LEFT); assert(d.sh.task_pick==DESK_CLIENTS);
    tap(DU_KEY_HOME); key(DU_KEY_LSHIFT,1); tap(DU_KEY_TAB); key(DU_KEY_LSHIFT,0); assert(!d.sh.taskbar_key);   /* Shift+Tab past Start: the desktop */
    with(DU_KEY_LCTRL,DU_KEY_ESC); tap(DU_KEY_ESC); assert(d.sh.taskbar_key && d.sh.task_pick==DESK_CLIENTS);
    tap(DU_KEY_END); assert(d.sh.task_pick==c); tap(DU_KEY_TAB); assert(!d.sh.taskbar_key && d.focus<0);   /* Tab past the last button: the desktop */
    with(DU_KEY_LCTRL,DU_KEY_ESC); tap(DU_KEY_ESC);
    tap(DU_KEY_HOME); assert(d.sh.task_pick==DESK_CLIENTS); tap(DU_KEY_END); assert(d.sh.task_pick==c);
    /* Enter on the Start button opens the menu; Esc returns to the taskbar */
    tap(DU_KEY_HOME); tap(DU_KEY_ENTER); assert(d.menu_open && !d.sh.taskbar_key); tap(DU_KEY_ESC); assert(d.sh.taskbar_key);
    /* the key for a context menu: a task button's window menu */
    tap(DU_KEY_TAB); assert(d.sh.task_pick==a);
    tap(KEY_MENU); assert(d.sh.levels && d.sh.menus[0].kind==SH_MENU_TASK && d.sh.menu_owner==a);
    tap(DU_KEY_ESC); assert(!d.sh.levels && d.sh.taskbar_key);
    key(DU_KEY_LSHIFT,1); tap(DU_KEY_F10); key(DU_KEY_LSHIFT,0); assert(d.sh.levels && d.sh.menus[0].kind==SH_MENU_TASK); tap(DU_KEY_ESC);
    /* a click anywhere ends the keyboard focus; a window activated by any means too */
    left(600,300); assert(!d.sh.taskbar_key);
    win(DU_KEY_TAB); assert(d.sh.taskbar_key); desk_focus(&d,a); assert(!d.sh.taskbar_key);
    /* Alt+M with the focus on the desktop or the taskbar minimises all windows */
    tap(DU_KEY_ESC); left(600,300); assert(d.focus<0);
    alt(KEY_M); assert(d.clients[a].minimized && d.clients[b].minimized && d.clients[c].minimized);
    clean();
}

/* ---- KB-1, KB-2, KB-3, KB-5, KB-9: Alt shortcuts ------------------------- */
static void alt_keys(void)
{
    setup(); int a=window_new(42,"files"), b=window_new(43,"calc"), c=window_new(44,"ciuknote");
    desk_focus(&d,a); desk_focus(&d,b); desk_focus(&d,c);
    /* Alt+Tab: the previous window; Shift goes the other way round */
    key(DU_KEY_LALT,1); tap(DU_KEY_TAB); assert(d.sh.switcher && d.sh.switch_count==3 && d.sh.switch_list[d.sh.switch_pick]==b);
    frame_matches("switcher");
    tap(DU_KEY_TAB); assert(d.sh.switch_list[d.sh.switch_pick]==a);
    key(DU_KEY_LALT,0); assert(!d.sh.switcher && d.focus==a);
    key(DU_KEY_LALT,1); key(DU_KEY_LSHIFT,1); tap(DU_KEY_TAB);
    assert(d.sh.switcher && d.sh.switch_pick==d.sh.switch_count-1);   /* Alt+Shift+Tab starts at the last window */
    key(DU_KEY_LSHIFT,0); key(DU_KEY_LALT,0); assert(d.focus==d.sh.switch_list[d.sh.switch_count-1]);
    /* Alt+Esc */
    int before=d.focus; alt(DU_KEY_ESC); assert(d.focus!=before);
    /* Alt+Space: the window menu of the active window */
    alt(DU_KEY_SPACE); assert(d.sh.levels && d.sh.menus[0].kind==SH_MENU_TASK && d.sh.menu_owner==d.focus); tap(DU_KEY_ESC);
    /* Alt+F4: the window; on the desktop, Shut Down */
    int active=d.focus; alt(DU_KEY_F4); assert(d.clients[active].closing);   /* Alt+F4: WM_CLOSE to the active window */
    for (int i=0;i<3;i++) d.clients[i].closing=0;
    left(600,300); assert(d.focus<0);
    alt(DU_KEY_F4); assert(d.focus>=0 && d.clients[d.focus].internal==DESK_INTERNAL_SHUTDOWN);
    desk_request_close(&d,d.focus);
    /* Alt+S with no window open and nothing selected: the Start menu */
    for (int i=0;i<3;i++) desk_drop_client(&d,i);
    left(600,300); assert(d.sh.icon_selected<0 && !desk_task_count(&d));
    alt(0x1f); assert(d.menu_open);
    clean();
    /* Alt+F6: the next window of the same program */
    setup(); a=window_new(42,"ciuknote"); b=window_new(42,"ciuknote"); c=window_new(77,"calc"); int e=window_new(42,"ciuknote");
    desk_focus(&d,a); desk_focus(&d,c); desk_focus(&d,e);
    alt(KEY_F6); assert(d.focus==b || d.focus==a);
    int first=d.focus; alt(KEY_F6); assert(d.focus!=first && d.focus!=c && d.clients[d.focus].pid==42);
    desk_focus(&d,c); alt(KEY_F6); assert(d.focus==c);   /* the only window of its program */
    clean();
}

/* ---- KB-8: Shift+F10 and the Menu key on the desktop; F3 ----------------- */
static void context_keys(void)
{
    setup(); left(600,300); assert(d.focus<0);
    tap(KEY_MENU); assert(d.sh.levels && d.sh.menus[0].kind==SH_MENU_DESKTOP); tap(DU_KEY_ESC);
    key(DU_KEY_LSHIFT,1); tap(DU_KEY_F10); key(DU_KEY_LSHIFT,0); assert(d.sh.levels && d.sh.menus[0].kind==SH_MENU_DESKTOP); tap(DU_KEY_ESC);
    struct desk_box icon=d.sh.icons[0].box; left(icon.x+icon.w/2,icon.y+20); assert(d.sh.icon_selected==0);
    tap(KEY_MENU); assert(d.sh.levels && d.sh.menus[0].kind==SH_MENU_ICON && d.sh.menu_owner==0);
    frame_matches("icon menu by key"); tap(DU_KEY_ESC);
    tap(KEY_F3); assert(d.focus>=0 && d.clients[d.focus].internal==DESK_INTERNAL_MESSAGE && strstr(d.sh.message_title,"Find"));
    desk_request_close(&d,d.focus);
    clean();
}

/* ---- WM-11: the caption icon --------------------------------------------- */
static void caption_icon(void)
{
    setup();
    assert(!desk_theme_use("Platinum"));
    int a=window_new(42,"files"); desk_move(&d,a,100,100);
    assert(!desk_caption_icon_box(&d,&d.clients[a]).w);   /* Platinum draws no caption icon */
    assert(!desk_theme_use("Ciuki Blue")); desk_theme_changed(&d);
    struct desk_box icon=desk_caption_icon_box(&d,&d.clients[a]); assert(icon.w==16 && icon.h==16);
    desk_focus(&d,-1);
    /* one click: the window menu below the icon */
    left(icon.x+8,icon.y+8); assert(d.focus==a && d.sh.levels && d.sh.menus[0].kind==SH_MENU_TASK && d.sh.menu_owner==a);
    assert(d.sh.menus[0].box.x==d.clients[a].frame.x && d.sh.menus[0].box.y>=d.clients[a].frame.y+DESK_TITLE);
    frame_matches("caption icon menu");
    /* a click after the double-click time closes the menu again */
    d.now_ns+=900000000ull; left(icon.x+8,icon.y+8); assert(!d.sh.levels && !d.clients[a].closing);
    /* a double click closes the window */
    d.now_ns+=900000000ull; left(icon.x+8,icon.y+8); assert(d.sh.levels && !d.clients[a].closing);
    d.now_ns+=100000000ull; left(icon.x+8,icon.y+8); assert(!d.sh.levels && d.clients[a].closing);
    assert(!desk_theme_use("Platinum")); desk_theme_changed(&d);
    clean();
}

/* ---- Win32 windows: the maximised state and WM_GETMINMAXINFO limits ------ */
static int v3_root=-1;
static unsigned v3_token;
static void v3_msg(struct ciuki_message *m,unsigned op,unsigned n,unsigned id,unsigned gen)
{ desk_v3_message(m,op,n,++v3_token,id,gen); m->sender_pid=d.clients[v3_root].pid; }
static int v3_window(int x,int y,unsigned w,unsigned h)
{
    int fd=next_channel++;
    if (v3_root<0) {
        int root=next_channel++; fake_channel(root); v3_root=desk_add_client(&d,root); assert(v3_root>=0);
        struct ciuki_message hello; desk_v3_message(&hello,DESK_HELLO,0,0,0,0); hello.sender_pid=77;
        assert(!desk_v3_receive(&d,v3_root,&hello));
    }
    fake_surface(fd,w,h,0x335577u);
    struct ciuki_message m; v3_msg(&m,DESK_CREATE_WINDOW,40,1,1); m.fd_count=1; m.fds[0]=fd;
    desk_put32(m.data+16,DESK_V3_OVERLAPPED); desk_put32(m.data+20,0); desk_put32(m.data+24,0);
    desk_put32(m.data+28,(uint32_t)x); desk_put32(m.data+32,(uint32_t)y);
    desk_put32(m.data+36,w); desk_put32(m.data+40,h); desk_put32(m.data+44,1); desk_put32(m.data+48,4); memcpy(m.data+52,"Wine",4);
    assert(!desk_v3_receive(&d,v3_root,&m));
    for (int k=0;k<DESK_CLIENTS;k++) if (d.clients[k].window && d.clients[k].version==DESK_V3_VERSION && d.clients[k].v3_peer==(unsigned)v3_root+1) return k;
    assert(0); return -1;
}
static int v3_place(int i,int x,int y,unsigned w,unsigned h,unsigned state)
{
    struct desk_client *c=&d.clients[i]; struct ciuki_message m; v3_msg(&m,DESK_V3_PLACE,28,c->v3_id,c->v3_generation);
    desk_put32(m.data+16,(uint32_t)x); desk_put32(m.data+20,(uint32_t)y); desk_put32(m.data+24,w); desk_put32(m.data+28,h);
    desk_put32(m.data+32,1); desk_put32(m.data+36,state); desk_put32(m.data+40,c->v3_style);
    return desk_v3_receive(&d,v3_root,&m);
}
static int v3_limits(int i,unsigned a,unsigned b,unsigned c2,unsigned e)
{
    struct desk_client *c=&d.clients[i]; struct ciuki_message m; v3_msg(&m,DESK_V3_LIMITS,16,c->v3_id,c->v3_generation);
    desk_put32(m.data+16,a); desk_put32(m.data+20,b); desk_put32(m.data+24,c2); desk_put32(m.data+28,e);
    return desk_v3_receive(&d,v3_root,&m);
}
/* the state word of the last CONFIGURE queued for a window, -1 when none */
static int last_configure_state(int i)
{
    /* a Win32 window's messages travel on its server's channel */
    int state=-1; const struct desk_client *root=&d.clients[v3_root];
    for (unsigned k=0;k<root->tx_count;k++) {
        const struct ciuki_message *m=&root->tx[(root->tx_head+k)%DESK_TX_MAX];
        if (m->data[1]==DESK_CONFIGURE && desk_get32(m->data+8)==d.clients[i].v3_id) state=(int)desk_get32(m->data+16+24);
    }
    return state;
}
static void v3_zoom_limits(void)
{
    setup(); v3_root=-1; v3_token=0;
    int w=v3_window(100,100,200,150); struct desk_client *c=&d.clients[w];
    struct desk_box normal=c->frame, wa=work();
    assert(!c->zoomed && last_configure_state(w)<=0);
    /* ShowWindow(SW_MAXIMIZE) reaches the desktop as state 3: the window zooms and is told */
    assert(!v3_place(w,100,100,200,150,3));
    assert(c->zoomed && c->frame.x==wa.x && c->frame.y==wa.y && c->frame.w==wa.w && c->frame.h==wa.h);
    assert(last_configure_state(w)==3);
    frame_matches("Win32 window maximised by the program");
    /* minimised while maximised: still maximised; back to 3 */
    assert(!v3_place(w,100,100,200,150,1) && c->zoomed && c->minimized==1 && last_configure_state(w)==1);
    assert(!v3_place(w,100,100,200,150,3) && c->zoomed && !c->minimized && last_configure_state(w)==3);
    /* SW_RESTORE: state 0 returns the window to where it was */
    assert(!v3_place(w,100,100,200,150,0));
    assert(!c->zoomed && !memcmp(&c->frame,&normal,sizeof(normal)) && last_configure_state(w)==0);
    frame_matches("Win32 window restored by the program");
    /* the title button maximises: the program learns it (SIZE_MAXIMIZED) */
    desk_zoom(&d,w); assert(c->zoomed && last_configure_state(w)==3);
    desk_zoom(&d,w); assert(!c->zoomed && last_configure_state(w)==0);
    /* WM_GETMINMAXINFO: the track sizes bound sizing and the maximised window */
    assert(!v3_limits(w,300,200,400,300));
    assert(c->min_width==300 && c->max_width==400 && c->max_height==300);
    struct desk_box big=c->frame; big.w=700; big.h=500; desk_resize(&d,w,big);
    assert(c->frame.w==400+2*DESK_BORDER && c->frame.h==300+DESK_TITLE+DESK_BORDER);
    struct desk_box small=c->frame; small.w=100; small.h=100; desk_resize(&d,w,small);
    assert(c->frame.w==300+2*DESK_BORDER && c->frame.h==200+DESK_TITLE+DESK_BORDER);
    desk_zoom(&d,w); assert(c->zoomed && c->frame.w==400+2*DESK_BORDER && c->frame.h==300+DESK_TITLE+DESK_BORDER);
    frame_matches("Win32 window maximised within its track size");
    desk_zoom(&d,w);
    /* the limits survive a PLACE; wrong ones are refused and change nothing */
    assert(!v3_place(w,50,60,320,240,0) && c->max_width==400);
    {   /* malformed limits do not parse (the desktop drops a peer that sends them) */
        struct ciuki_message m; struct desk_packet pkt;
        const unsigned bad[][4]={{500,100,400,300},{10,10,3000,10},{2049,1,0,0},{1,1,10,0}};
        for (unsigned k=0;k<4;k++) {
            v3_msg(&m,DESK_V3_LIMITS,16,c->v3_id,c->v3_generation);
            for (unsigned j=0;j<4;j++) desk_put32(m.data+16+j*4,bad[k][j]);
            assert(k==3 ? desk_parse(&m,1,NULL,&pkt) : !desk_parse(&m,1,NULL,&pkt));
        }
    }
    assert(!v3_limits(w,0,0,0,0) && c->max_width==2048 && c->min_width==1);
    clean(); v3_root=-1;
}

int main(void)
{
    cascade(); tile(); exclusions(); minimize_all(); taskbar_menu(); mnemonics(); windows_keys(); taskbar_focus();
    alt_keys(); context_keys(); caption_icon(); v3_zoom_limits();
    printf("PASS G4 taskbar: cascade and tile geometry, exclusions, undo, minimize all and Show Desktop, taskbar menu, access keys, Windows-key shortcuts, taskbar keyboard focus, Alt shortcuts, context-menu keys, caption icon, Win32 maximised state and track limits; %u frames equal to a full redraw\n",checked);
    return 0;
}
