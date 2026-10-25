/* SPDX-License-Identifier: MIT
 * Included by window_model_test.c; production shell paths, five presenters. */
#ifndef CIUKI_G18_SHORTCUT_TEST_H
#define CIUKI_G18_SHORTCUT_TEST_H
static unsigned g18_shortcut_saves,g18_hotkey_launches;
static struct desk_launch_record g18_saved_record;
static uint32_t g18_icon16[256],g18_icon32[1024];
static unsigned g18_snapshot_polls;
static char g18_find_folder[260];
static int g18_find_target_launch(struct desktop *desktop,const struct desk_program *program)
{
    assert(!strcmp(program->name,"files") && desktop->launch_arg);
    snprintf(g18_find_folder,sizeof(g18_find_folder),"%s",desktop->launch_arg); return 1234;
}
static int g18_pending_snapshot(struct desktop *desktop,struct desk_link_entry *out,unsigned cap)
{ (void)desktop; (void)out; (void)cap; g18_snapshot_polls++; return -EAGAIN; }
static const uint32_t *g18_icon_snapshot(struct desktop *desktop,const char *source,unsigned size)
{ (void)desktop; (void)source; return size==16 ? g18_icon16 : size==32 ? g18_icon32 : NULL; }
static int g18_save_shortcut(struct desktop *desktop,const char *source,const struct desk_launch_record *record)
{
    (void)desktop;
    assert(!strcmp(source,"/Users/Test/Start Menu/Programs/Vendor/Tools/Editor.lnk"));
    g18_shortcut_saves++; g18_saved_record=*record; return 0;
}
static int g18_read_shortcut(struct desktop *desktop,const char *source,struct desk_launch_record *record)
{
    assert(!strcmp(source,desktop->v3_links[0].source));
    *record=desktop->v3_links[0].launch; return 0;
}
static int g18_launch_shortcut(struct desktop *desktop,const struct desk_launch_record *record)
{
    (void)desktop; assert(!strcmp(record->target,"C:\\Programs\\EDITOR.EXE"));
    g18_hotkey_launches++; return 0;
}
static int g18_menu_launch(struct desktop *desktop,const struct desk_launch_record *record)
{ (void)desktop; (void)record; return 0; }
static unsigned g18_menu_count(const struct sh_menu *m,const char *label)
{
    unsigned n=0; for (int k=0;k<m->count;k++) if (!strcasecmp(m->items[k].label,label)) n++;
    return n;
}
static void g18_menu_link(unsigned k,const char *source,const char *label,const char *target,const char *arguments)
{
    memset(&d.v3_links[k],0,sizeof(d.v3_links[k]));
    snprintf(d.v3_links[k].source,sizeof(d.v3_links[k].source),"%s",source);
    snprintf(d.v3_links[k].launch.label,sizeof(d.v3_links[k].launch.label),"%s",label);
    snprintf(d.v3_links[k].launch.target,sizeof(d.v3_links[k].launch.target),"%s",target);
    snprintf(d.v3_links[k].launch.arguments,sizeof(d.v3_links[k].launch.arguments),"%s",arguments);
    d.v3_links[k].launch.show=1;
}
static void row_g18_shortcuts(unsigned m)
{
    row_name="G18 shortcut properties and installed menus"; wm_begin(m,1);
    for (unsigned k=0;k<256;k++) g18_icon16[k]=0xff52a984u;
    for (unsigned k=0;k<1024;k++) g18_icon32[k]=0xff52a984u;
    strcpy(d.sh.profile.programs,"/Users/Test/Start Menu/Programs");
    d.v3_link_icon=g18_icon_snapshot;
    d.v3_links_list=NULL;
    if (!d.v3_links) d.v3_links=calloc(DESK_LINKS,sizeof(*d.v3_links));
    assert(d.v3_links); d.v3_link_count=3;
    for (unsigned k=0;k<3;k++) {
        memset(&d.v3_links[k],0,sizeof(d.v3_links[k]));
        strcpy(d.v3_links[k].launch.label,k ? "Viewer" : "Editor");
        strcpy(d.v3_links[k].launch.target,"C:\\Programs\\EDITOR.EXE");
        d.v3_links[k].launch.show=1;
    }
    strcpy(d.v3_links[0].source,"/Users/Test/Start Menu/Programs/Vendor/Tools/Editor.lnk");
    strcpy(d.v3_links[1].source,"/Windows/Start Menu/Programs/Vendor/Viewer.lnk");
    strcpy(d.v3_links[2].source,"/Users/Test/Start Menu/Programs/Other/Tools/Viewer.lnk");
    desk_menu_open(&d,SH_MENU_PROGRAMS,4,DESK_BAR); step("installed top-level folders");
    int vendor=0,other=0;
    for (int k=0;k<d.sh.menus[0].count;k++) {
        struct sh_item *it=&d.sh.menus[0].items[k];
        if (!strcmp(it->label,"Vendor")) { vendor++; assert(it->action==SH_ACT_SUBMENU); }
        if (!strcmp(it->label,"Other")) other++;
        assert(strcmp(it->label,"Editor"));
    }
    assert(vendor==1 && other==1);
    desk_menu_open(&d,128+1,30,60); step("installed Vendor submenu");
    assert(d.sh.menus[0].count==2);
    assert(!strcmp(d.sh.menus[0].items[0].label,"Tools"));
    assert(d.sh.menus[0].items[0].arg==130);
    desk_menu_open(&d,130,60,80); step("installed nested Tools submenu");
    assert(d.sh.menus[0].count==1 && d.sh.menus[0].items[0].action==SH_ACT_WIN32_LINK && d.sh.menus[0].items[0].arg==0);
    d.v3_links_list=g18_pending_snapshot; d.v3_links_next=0; g18_snapshot_polls=0;
    desk_links_refresh(&d); assert(!g18_snapshot_polls); step("displayed installed menu retains its link identities");
    desk_menu_close(&d);
    desk_links_refresh(&d); assert(g18_snapshot_polls==1); d.v3_links_list=NULL;
    d.v3_link_save=g18_save_shortcut; g18_shortcut_saves=0;
    desk_shortcut_properties(&d,0,0);
    int index=d.focus; assert(index>=0 && d.clients[index].internal==DESK_INTERNAL_SHORTCUT);
    assert(!strcmp(d.sh.shortcut_source,d.v3_links[0].source));
    strcpy(d.sh.shortcut_number,"-2"); step("shortcut properties opened");
    struct desk_box preview=desk_content(&d.clients[index]);
    assert((d.pixels[(size_t)(preview.y+8)*d.width+preview.x+12]&0xffffffu)==0x52a984u);
    int (*saved_launch)(struct desktop *,const struct desk_program *)=d.launch;
    d.launch=g18_find_target_launch; g18_find_folder[0]=0;
    desk_internal_click(&d,index,preview.x+20,preview.y+290);
    assert(!strcmp(g18_find_folder,"/Programs")); d.launch=saved_launch; step("Find Target opens the containing folder");
    desk_internal_click(&d,index,preview.x+120,preview.y+55); step("Target focus after Find Target");
    /* The input path must repaint a modified Target and moving field focus. */
    tap(DU_KEY_BACKSPACE); step("shortcut target edit");
    assert(!strcmp(d.sh.shortcut_record.target,"C:\\Programs\\EDITOR.EX"));
    strcpy(d.sh.shortcut_record.target,"C:\\Programs\\EDITOR.EXE");
    tap(DU_KEY_TAB); step("shortcut arguments focus"); tap(0x1e); step("shortcut arguments edit");
    assert(!strcmp(d.sh.shortcut_record.arguments,"a"));
    tap(DU_KEY_TAB); step("shortcut Start in focus"); tap(0x30); step("shortcut Start in edit");
    assert(!strcmp(d.sh.shortcut_record.directory,"b"));
    tap(DU_KEY_TAB); tap(0x12); step("shortcut key capture"); assert(d.sh.shortcut_record.hotkey==0x645u);
    tap(DU_KEY_BACKSPACE); step("shortcut key removed"); assert(!d.sh.shortcut_record.hotkey);
    tap(DU_KEY_TAB); tap(DU_KEY_SPACE); step("shortcut Run minimized"); assert(d.sh.shortcut_record.show==7);
    tap(DU_KEY_SPACE); step("shortcut Run maximized"); assert(d.sh.shortcut_record.show==3);
    tap(DU_KEY_SPACE); step("shortcut Run normal"); assert(d.sh.shortcut_record.show==1);
    struct desk_box content=desk_content(&d.clients[index]);
    desk_internal_click(&d,index,content.x+12+172+8,content.y+290); step("Change Icon focuses icon path"); assert(d.sh.shortcut_field==5);
    tap(0x17); step("shortcut icon path edit"); assert(!strcmp(d.sh.shortcut_record.icon,"i"));
    desk_internal_click(&d,index,content.x+12+344+8,content.y+290); step("explicit target repair saves");
    assert(g18_shortcut_saves==1 && g18_saved_record.icon_index==-2 && !strcmp(g18_saved_record.arguments,"a"));
    tap(DU_KEY_ESC); step("shortcut properties cancelled and exposed"); assert(!d.clients[index].window);
    /* An invalid replacement never reaches the worker and keeps the sheet. */
    desk_shortcut_properties(&d,0,1); index=d.focus; step("missing shortcut target repair dialog");
    strcpy(d.sh.shortcut_number,"9999999999999999999999");
    d.sh.shortcut_field=12; tap(DU_KEY_ENTER); step("overflowing icon index rejected");
    assert(g18_shortcut_saves==1 && d.clients[index].window);
    strcpy(d.sh.shortcut_number,"0"); strcpy(d.sh.shortcut_record.target,"relative.exe");
    tap(DU_KEY_ENTER); step("relative target rejected"); assert(g18_shortcut_saves==1);
    tap(DU_KEY_ESC); step("invalid replacement cancelled");
    d.v3_link_save=NULL;
    d.v3_links[0].launch.hotkey=0x645;
    d.v3_link_read=g18_read_shortcut; d.v3_launch=g18_launch_shortcut; g18_hotkey_launches=0;
    key(0x1d,1); key(0x38,1); key(0x12,1); step("global shortcut hotkey launches");
    assert(g18_hotkey_launches==1);
    key(0x12,2); step("shortcut hotkey repeat suppressed"); assert(g18_hotkey_launches==1);
    key(0x12,0); key(0x38,0); key(0x1d,0); step("shortcut hotkey released");
    tap(0x12); step("plain typing does not launch shortcut"); assert(g18_hotkey_launches==1);
    /* A Start Menu tree created by an installer merges into the built-in groups: one Games group, one entry per
     * program (same name or same target), no "Programs folder" pseudo-item, and the folder's own shortcut is listed. */
    d.v3_launch=g18_menu_launch; d.v3_link_read=NULL; d.v3_link_count=4;
    g18_menu_link(0,"/Users/Test/Start Menu/Programs/Games/Quake III Arena Demo.lnk","Quake III Arena Demo",
                  "C:\\Program Files\\Quake III Arena Demo\\quake3.exe","");
    g18_menu_link(1,"/Users/Test/Start Menu/Programs/Games/DOOM (shareware).lnk","DOOM (shareware)","/Programs/dosbox",
                  "\"--name=DOOM\" \"\\DOOM\\DOOM.EXE -config CIUKI.CFG\"");
    g18_menu_link(2,"/Users/Test/Start Menu/Programs/Games/Doom again.lnk","Doom again","/Programs/dosbox",
                  "\"--name=DOOM\" \"\\DOOM\\DOOM.EXE -config CIUKI.CFG\"");
    g18_menu_link(3,"/Users/Test/Start Menu/Programs/Games/Vendor Game.lnk","Vendor Game","C:\\Vendor\\GAME.EXE","");
    desk_menu_open(&d,SH_MENU_PROGRAMS,4,DESK_BAR); step("merged Programs menu");
    assert(g18_menu_count(&d.sh.menus[0],"Games")==1);
    assert(!g18_menu_count(&d.sh.menus[0],"Programs folder"));
    desk_menu_open(&d,SH_MENU_GAMES,60,80); step("merged Games menu");
    assert(g18_menu_count(&d.sh.menus[0],"Quake III Arena Demo")==1);   /* installer-created shortcut; no built-in record */
    assert(!g18_menu_count(&d.sh.menus[0],"Install Quake III Arena Demo"));
    assert(g18_menu_count(&d.sh.menus[0],"DOOM (shareware)")==1);
    assert(!g18_menu_count(&d.sh.menus[0],"Doom again"));
    assert(g18_menu_count(&d.sh.menus[0],"Vendor Game")==1);
    /* The classic Windows-style games sit in Games > Classic Games, never in Programs or directly in Games. */
    assert(!g18_menu_count(&d.sh.menus[0],"Minesweeper") && !g18_menu_count(&d.sh.menus[0],"Solitaire"));
    desk_menu_close(&d);
    d.v3_launch=NULL;
    d.v3_link_read=NULL;
    wm_end();
}

#endif
