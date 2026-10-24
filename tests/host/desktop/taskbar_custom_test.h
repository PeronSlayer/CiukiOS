/* SPDX-License-Identifier: MIT — Windows 98 G20 behavioral regression cases. */
#include "../../../apps/desktop/customize_io.h"
static int g20_menu_index(const struct sh_menu *m,const char *label)
{ for (int k=0;k<m->count;k++) if (!strcmp(m->items[k].label,label)) return k;return -1; }
static int g20_menu_y(const struct sh_menu *m,int index)
{
    int y=m->box.y+3+(m->scroll_max ? 12 : 0)-m->scroll;
    for (int k=0;k<index;k++) y+=m->items[k].flags&SH_ITEM_SEPARATOR ? SH_SEPARATOR : m->row_h;
    return y+m->row_h/2;
}
static void g20_menu_click(const char *label)
{
    struct sh_menu *m=&d.sh.menus[d.sh.levels-1];int k=g20_menu_index(m,label);assert(k>=0);
    left(m->box.x+m->box.w/2,g20_menu_y(m,k));
}
static void g20_snapshot(void)
{
    d.sh.custom=calloc(1,sizeof(*d.sh.custom));assert(d.sh.custom);
    struct sh_folder_snapshot *s=d.sh.custom;s->native_seeded=1;s->folders_count=SH_ROOTS;
    strcpy(s->folders[SH_ROOT_START],SH_START_FOLDER);strcpy(s->folders[SH_ROOT_PROGRAMS],SH_PROGRAMS_FOLDER);
    strcpy(s->folders[SH_ROOT_QUICK],SH_QUICK_FOLDER);strcpy(s->folders[SH_ROOT_LINKS],SH_LINKS_FOLDER);strcpy(s->folders[SH_ROOT_DESKTOP],SH_DESKTOP_FOLDER);
    s->count=3;
    for (int k=0;k<3;k++) {
        s->entries[k].parent=SH_ROOT_START;s->entries[k].folder=-1;
        snprintf(s->entries[k].label,64,"Pin %d",k);
        snprintf(s->entries[k].path,CFG_PATH_MAX,SH_START_FOLDER "/Pin %d.lnk",k);
    }
}
__attribute__((noinline)) static void g20_storage(void)
{
    static char cwd[1024],dir[1200];char path[CFG_PATH_MAX],second[CFG_PATH_MAX];assert(getcwd(cwd,sizeof(cwd)));
    snprintf(dir,sizeof(dir),"%s/build/host/g20-storage-XXXXXX",cwd);assert(mkdtemp(dir));
    assert(!sh_shortcut_add(dir,"/Programs/ciuknote","Note","--example"));
    assert(sh_shortcut_add(dir,"/Programs/ciuknote","Note",NULL)==-EEXIST);
    assert(sh_shortcut_add(dir,"/Programs/ciuknote","../bad",NULL)==-EINVAL);
    assert(!sh_join(path,sizeof(path),dir,"Note.lnk"));
    struct desk_launch_record *r=malloc(sizeof(*r));assert(r);
    assert(!sh_shortcut_read(path,r) && !strcmp(r->target,"/Programs/ciuknote") && !strcmp(r->arguments,"--example"));
    assert(!sh_shortcut_add(dir,path,"Copy",NULL));
    assert(!sh_join(second,sizeof(second),dir,"Copy.lnk"));
    assert(!sh_shortcut_read(second,r) && !strcmp(r->arguments,"--example"));
    assert(!sh_shortcut_add(dir,"https://example.com/","Web",NULL));
    assert(!sh_join(second,sizeof(second),dir,"Web.lnk"));
    assert(!sh_shortcut_read(second,r) && !strcmp(r->target,"/Programs/netsurf") && !strcmp(r->arguments,"\"https://example.com/\""));
    static struct sh_edit_request edit;memset(&edit,0,sizeof(edit));
    edit.operation=SH_EDIT_RENAME;strcpy(edit.path,path);strcpy(edit.name,"Renamed");assert(!sh_edit_apply(&edit));
    assert(!sh_join(path,sizeof(path),dir,"Renamed.lnk"));assert(!sh_shortcut_read(path,r));
    strcpy(edit.name,"Copy");assert(sh_edit_apply(&edit)==-EEXIST);strcpy(edit.name,"../escape");assert(sh_edit_apply(&edit)==-EINVAL);
    edit.operation=SH_EDIT_TOOLBAR;strcpy(edit.path,path);assert(sh_edit_apply(&edit)==-ENOTDIR);
    strcpy(edit.path,dir);assert(!sh_edit_apply(&edit));
    edit.operation=SH_EDIT_ORDER;strcpy(edit.path,dir);edit.count=3;
    strcpy(edit.order[0],"Web.lnk");strcpy(edit.order[1],"Renamed.lnk");strcpy(edit.order[2],"Copy.lnk");assert(!sh_edit_apply(&edit));
    struct sh_folder_snapshot *s=calloc(1,sizeof(*s));assert(s);s->folders_count=SH_ROOTS;strcpy(s->folders[SH_ROOT_QUICK],dir);
    sh_folder_scan(s,SH_ROOT_QUICK);assert(s->count==3 && !strcmp(s->entries[0].label,"Web") && !strcmp(s->entries[1].label,"Renamed"));
    struct ciuki_settings *cfg=calloc(1,sizeof(*cfg));assert(cfg);cfg_defaults(cfg);cfg_recent_add(cfg,"/Documents/older.txt");
    assert(!sh_join(path,sizeof(path),dir,"recent.lst"));
    FILE *f=fopen(path,"wb");assert(f);
    fputs("relative\n/Documents/new.txt\n/Documents/new.txt\n",f);
    for (unsigned k=0;k<20;k++) fprintf(f,"/Documents/%02u.txt\n",k);
    assert(!fclose(f));assert(!sh_recent_merge(cfg,path) && cfg->recent_count==15 && !strcmp(cfg->recent[0],"/Documents/new.txt"));
    assert(!strcmp(cfg->recent[14],"/Documents/13.txt"));
    assert(!sh_recent_clear(path) && !sh_recent_clear(path));cfg->recent_count=0;assert(!sh_recent_merge(cfg,path) && !cfg->recent_count);
    cfg->taskbar_top=0;cfg->taskbar_autohide=cfg->start_small=1;cfg->taskbar_edge=CFG_BAR_RIGHT;cfg->taskbar_size=96;
    cfg->toolbar_address=cfg->toolbar_links=cfg->toolbar_desktop=1;strcpy(cfg->toolbar_folder[0],dir);
    char *text=malloc(CFG_TEXT_MAX);assert(text);size_t len=cfg_format(cfg,text,CFG_TEXT_MAX);assert(len);
    cfg_defaults(cfg);assert(!cfg_parse(cfg,text,len));assert(!cfg->taskbar_top && cfg->taskbar_autohide && cfg->start_small && cfg->taskbar_edge==CFG_BAR_RIGHT && cfg->taskbar_size==96 && !strcmp(cfg->toolbar_folder[0],dir));
    const char *bad="taskbar_edge=4\ntaskbar_size=31\ntaskbar_autohide=maybe\n";assert(cfg_parse(cfg,bad,strlen(bad))==3);
    assert(!sh_join(path,sizeof(path),dir,"Example.url"));
    assert(!sh_write_atomic(path,"[InternetShortcut]\nURL=https://example.com/\n",sizeof("[InternetShortcut]\nURL=https://example.com/\n")-1));
    char url[256];assert(!sh_url_read(path,url,sizeof(url)) && !strcmp(url,"https://example.com/"));
    free(text);free(cfg);free(r);free(s);
    assert(!sh_menu_remove(dir,0));
    puts("PASS G20 storage: shortcuts preserve arguments, duplicate refusal, URL targets, rename, persisted order, shared 15 documents/clear, preference round trip");
}
static void g20_dock(void)
{
    setup();int w=window_new(211,"ciuknote");
    /* Each edge is reached with a real press/motion/release, then resized from its inner border. */
    const int points[4][2]={{W/2,DESK_BAR+1},{1,H/2},{W-1,H/2},{W/2,H-1}};
    const int expected[4]={CFG_BAR_TOP,CFG_BAR_LEFT,CFG_BAR_RIGHT,CFG_BAR_BOTTOM};
    for (unsigned k=0;k<4;k++) {
        struct desk_box b=desk_taskbar(&d);int x=b.x+b.w-10,y=b.y+b.h-10;
        if (d.sh.cfg.taskbar_edge>=CFG_BAR_LEFT) { x=b.x+b.w/2;y=b.y+b.h-10; }
        point(x,y);event(CIUKI_INPUT_BUTTON,1,1,0);assert(d.sh.bar_drag && !d.sh.bar_resize);
        point(points[k][0],points[k][1]);assert(d.sh.cfg.taskbar_edge==expected[k]);event(CIUKI_INPUT_BUTTON,1,0,0);
        assert(!d.sh.bar_drag && d.sh.want_save);frame_matches("G20 taskbar dragged to edge");
        b=desk_taskbar(&d);int edge=d.sh.cfg.taskbar_edge;
        x=edge==CFG_BAR_LEFT ? b.x+b.w-1 : edge==CFG_BAR_RIGHT ? b.x : W-8;
        y=edge==CFG_BAR_TOP ? b.y+b.h-1 : edge==CFG_BAR_BOTTOM ? b.y : H-50;
        point(x,y);event(CIUKI_INPUT_BUTTON,1,1,0);assert(d.sh.bar_resize);
        int size=d.sh.cfg.taskbar_size;
        point(x+(edge==CFG_BAR_LEFT ? 20 : edge==CFG_BAR_RIGHT ? -20 : 0),y+(edge==CFG_BAR_TOP ? 20 : edge==CFG_BAR_BOTTOM ? -20 : 0));
        event(CIUKI_INPUT_BUTTON,1,0,0);assert(d.sh.cfg.taskbar_size>size);frame_matches("G20 taskbar border resize");
        desk_zoom(&d,w);assert(d.clients[w].zoomed);struct desk_box area=desk_work_area(&d);assert(d.clients[w].frame.x==area.x && d.clients[w].frame.y==area.y);desk_zoom(&d,w);
    }
    d.sh.cfg.taskbar_autohide=1;desk_shell_geometry(&d);point(W/2,H/2);d.now_ns+=1000000000ull;desk_shell_tick(&d);d.now_ns+=600000000ull;desk_shell_tick(&d);
    assert(d.sh.bar_hidden && desk_taskbar(&d).h==2);frame_matches("G20 auto hide");
    point(W-1,H-1);assert(!d.sh.bar_hidden && desk_taskbar(&d).h>2);frame_matches("G20 edge reveal");
    d.sh.cfg.taskbar_autohide=0;d.sh.cfg.taskbar_top=0;desk_shell_geometry(&d);desk_zoom(&d,w);
    assert(desk_work_area(&d).h==H-DESK_BAR);frame_matches("G20 Always on Top unchecked");
    d.sh.cfg.taskbar_top=1;desk_shell_geometry(&d);assert(desk_hit(&d,W-2,H-2)<0);frame_matches("G20 Always on Top restored");
    clean();
}
static void g20_start(void)
{
    setup();g20_snapshot();desk_menu_open(&d,SH_MENU_START,0,0);
    assert(!strcmp(d.sh.menus[0].items[0].label,"Pin 0") && d.sh.menus[0].icon==32);
    frame_matches("G20 folder backed Start pins");
    struct sh_menu *m=&d.sh.menus[0];int x=m->box.x+60,y=g20_menu_y(m,0),dest=g20_menu_y(m,2);
    point(x,y);event(CIUKI_INPUT_BUTTON,1,1,0);point(x,dest);event(CIUKI_INPUT_BUTTON,1,0,0);
    assert(d.sh.edit.operation==SH_EDIT_ORDER && !strcmp(d.sh.edit.order[2],"Pin 0.lnk"));assert(!strcmp(d.sh.custom->entries[2].label,"Pin 0"));frame_matches("G20 Start drag reordering");
    d.sh.edit.operation=0;desk_menu_open(&d,SH_MENU_START,0,0);m=&d.sh.menus[0];right(m->box.x+60,g20_menu_y(m,0));
    assert(d.sh.menus[0].kind==SH_MENU_START_ITEM);frame_matches("G20 Start right click");g20_menu_click("Delete");
    assert(d.sh.edit.operation==SH_EDIT_DELETE && !strcmp(d.sh.edit.path,SH_START_FOLDER "/Pin 1.lnk"));d.sh.edit.operation=0;
    d.sh.cfg.start_small=1;desk_menu_open(&d,SH_MENU_START,0,0);assert(d.sh.menus[0].icon==16 && d.sh.menus[0].row_h==SH_SUB_ROW);frame_matches("G20 small Start icons");
    d.sh.custom->count=4;struct sh_folder_item *favorite=&d.sh.custom->entries[3];
    *favorite=(struct sh_folder_item){.parent=SH_ROOT_FAVORITES,.folder=-1};strcpy(favorite->label,"Web");strcpy(favorite->path,SH_FAVORITES_FOLDER "/Web.url");
    desk_menu_close(&d);desk_menu_open(&d,SH_MENU_START,0,0);g20_menu_click("Favorites");g20_menu_click("Web");assert(!strcmp(d.sh.custom_open,SH_FAVORITES_FOLDER "/Web.url"));d.sh.custom_open[0]=0;
    desk_menu_close(&d);desk_menu_open(&d,SH_MENU_SETTINGS,100,60);g20_menu_click("Folder Options");assert(!strcmp(launched,"files") && !strcmp(launched_arg,"--folder-options"));
    /* The same drag gesture reorders Programs' folder-backed entries. */
    for (unsigned k=0;k<d.sh.custom->count;k++) d.sh.custom->entries[k].parent=SH_ROOT_PROGRAMS;
    desk_menu_open(&d,SH_MENU_PROGRAMS,100,60);m=&d.sh.menus[0];x=m->box.x+60;y=g20_menu_y(m,0);dest=g20_menu_y(m,2);
    point(x,y);event(CIUKI_INPUT_BUTTON,1,1,0);point(x,dest);event(CIUKI_INPUT_BUTTON,1,0,0);
    assert(d.sh.edit.operation==SH_EDIT_ORDER && !strcmp(d.sh.edit.path,SH_PROGRAMS_FOLDER));
    frame_matches("G20 Programs reorder");
    free(d.sh.custom);d.sh.custom=NULL;clean();
}
static void g20_toolbars(void)
{
    setup();g20_snapshot();
    struct sh_folder_snapshot *s=d.sh.custom;
    for (unsigned k=0;k<s->count;k++) s->entries[k].parent=SH_ROOT_QUICK;
    d.sh.cfg.toolbar_address=1;d.sh.cfg.toolbar_links=1;d.sh.cfg.toolbar_desktop=1;
    strcpy(d.sh.cfg.toolbar_folder[0],"/Documents/Tools");strcpy(s->folders[SH_ROOT_CUSTOM],"/Documents/Tools");
    desk_shell_geometry(&d);struct desk_toolbar_slot slots[DESK_TOOLBAR_SLOTS];int n=desk_toolbar_slots(&d,slots);assert(n>=6);
    int address=0,links=0,desktop=0,custom=0,quick=0;
    for (int k=0;k<n;k++) {
        address+=slots[k].kind==DESK_TOOLBAR_ADDRESS;
        links+=slots[k].kind==DESK_TOOLBAR_FOLDER && slots[k].arg==SH_ROOT_LINKS;
        desktop+=slots[k].kind==DESK_TOOLBAR_FOLDER && slots[k].arg==SH_ROOT_DESKTOP;
        custom+=slots[k].kind==DESK_TOOLBAR_FOLDER && slots[k].arg==SH_ROOT_CUSTOM;
        if (slots[k].kind==DESK_TOOLBAR_ITEM) {
            quick++;left(slots[k].box.x+5,slots[k].box.y+5);assert(!strcmp(d.sh.custom_open,s->entries[slots[k].arg].path));d.sh.custom_open[0]=0;
        }
    }
    assert(address && links && desktop && custom && quick==3);frame_matches("G20 all toolbars visible");
    for (int k=0;k<n;k++) if (slots[k].kind==DESK_TOOLBAR_ADDRESS) {
        left(slots[k].box.x+5,slots[k].box.y+5);assert(d.sh.address_focus);tap(0x1e);tap(0x30);tap(0x2e);tap(DU_KEY_BACKSPACE);tap(DU_KEY_ENTER);
        assert(!d.sh.address_focus && !strcmp(d.sh.custom_open,"ab"));d.sh.custom_open[0]=0;frame_matches("G20 inline Address input");
    }
    for (int k=0;k<n;k++) if (slots[k].kind==DESK_TOOLBAR_FOLDER) { left(slots[k].box.x+5,slots[k].box.y+5);assert(d.sh.levels);frame_matches("G20 folder toolbar menu");desk_menu_close(&d); }
    for (int k=0;k<n;k++) if (slots[k].kind==DESK_TOOLBAR_FOLDER && slots[k].arg==SH_ROOT_LINKS) {
        right(slots[k].box.x+5,slots[k].box.y+5);assert(d.sh.menus[0].kind==SH_MENU_TOOLBAR_CONTEXT);g20_menu_click("Close");assert(!d.sh.cfg.toolbar_links);frame_matches("G20 toolbar Close context command");break;
    }
    for (unsigned k=1;k<CFG_TOOLBARS;k++) snprintf(d.sh.cfg.toolbar_folder[k],CFG_PATH_MAX,"/Documents/Tools%u",k);
    desk_shell_geometry(&d);n=desk_toolbar_slots(&d,slots);int overflow=0;
    for (int k=0;k<n;k++) if (slots[k].kind==DESK_TOOLBAR_OVERFLOW) { overflow=1;left(slots[k].box.x+5,slots[k].box.y+5);assert(d.sh.menus[0].kind==SH_MENU_TOOLBAR_ALL);frame_matches("G20 toolbar overflow exposes all enabled folders");desk_menu_close(&d); }
    assert(overflow);for (unsigned k=1;k<CFG_TOOLBARS;k++) d.sh.cfg.toolbar_folder[k][0]=0;desk_shell_geometry(&d);
    /* New Toolbar accepts a folder, with validation deferred to the I/O worker. */
    desk_menu_open(&d,SH_MENU_TOOLBARS,200,100);g20_menu_click("New Toolbar...");assert(d.sh.prompt_mode==2);
    strcpy(d.sh.run_text,"/Documents/Tools");d.sh.run_length=(unsigned)strlen(d.sh.run_text);tap(DU_KEY_ENTER);
    assert(d.sh.edit.operation==SH_EDIT_TOOLBAR && !strcmp(d.sh.edit.path,"/Documents/Tools"));d.sh.edit.operation=0;
    /* External drags create shortcuts, preserving the source. */
    struct sh_dnd *g=&d.sh.dnd;
    g->active=1;g->button=1;g->source=-1;g->count=1;g->paths=calloc(1,sizeof(char *));g->paths[0]=strdup("/Documents/source.txt");g->types=malloc(1);g->types[0]='F';
    struct desk_box q=desk_quick_launch_button(&d);point(q.x+5,q.y+5);assert(g->kind==SH_DROP_QUICK && g->effect==FM_EFFECT_LINK);
    event(CIUKI_INPUT_BUTTON,1,0,0);assert(d.sh.edit.operation==SH_EDIT_ADD && d.sh.edit.sources_count==1 && !strcmp(d.sh.edit.destination,SH_QUICK_FOLDER));frame_matches("G20 drop shortcut into Quick Launch");
    d.sh.edit.operation=0;g->active=1;g->button=1;g->source=-1;g->count=1;g->paths=calloc(1,sizeof(char *));g->paths[0]=strdup("/Documents/source.txt");g->types=malloc(1);g->types[0]='F';
    struct desk_box b=desk_start_button(&d);point(b.x+b.w/2,b.y+b.h/2);assert(g->kind==SH_DROP_START);
    d.now_ns+=600000000ull;desk_shell_tick(&d);assert(d.menu_open && d.sh.menus[0].kind==SH_MENU_START);frame_matches("G20 hover opens Start during an external drag");
    struct sh_menu *menu=&d.sh.menus[0];point(menu->box.x+60,g20_menu_y(menu,g20_menu_index(menu,"Programs")));
    assert(d.sh.levels==2 && !strcmp(g->destination,SH_PROGRAMS_FOLDER));event(CIUKI_INPUT_BUTTON,1,0,0);
    assert(d.sh.edit.operation==SH_EDIT_ADD && !strcmp(d.sh.edit.destination,SH_PROGRAMS_FOLDER) && !d.menu_open);frame_matches("G20 drop shortcut into Programs");
    d.sh.edit.operation=0;g->active=1;g->button=1;g->source=-1;g->count=1;g->paths=calloc(1,sizeof(char *));g->paths[0]=strdup("/Documents/Tools");g->types=malloc(1);g->types[0]='D';
    point(W-5,H-10);assert(g->kind==SH_DROP_TOOLBAR);event(CIUKI_INPUT_BUTTON,1,0,0);assert(d.sh.edit.operation==SH_EDIT_TOOLBAR);frame_matches("G20 folder drag creates a toolbar");
    free(d.sh.custom);d.sh.custom=NULL;clean();
}
static void g20_all(void) { g20_storage();g20_dock();g20_start();g20_toolbars();puts("PASS G20 taskbar and Start menu customization"); }
