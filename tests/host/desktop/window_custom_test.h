/* SPDX-License-Identifier: MIT — fx-15 rows appended for G20 continuation. */
static void wm_custom_snapshot(void)
{
    d.sh.custom=calloc(1,sizeof(*d.sh.custom));assert(d.sh.custom);
    struct sh_folder_snapshot *s=d.sh.custom;s->native_seeded=1;s->folders_count=SH_ROOTS;s->count=3;
    strcpy(s->folders[SH_ROOT_START],SH_START_FOLDER);strcpy(s->folders[SH_ROOT_QUICK],SH_QUICK_FOLDER);
    for (int k=0;k<3;k++) {
        s->entries[k].parent=SH_ROOT_START;s->entries[k].folder=-1;
        snprintf(s->entries[k].label,64,"Start pin %d",k);snprintf(s->entries[k].path,CFG_PATH_MAX,SH_START_FOLDER "/Pin %d.lnk",k);
    }
}
static void row_taskbar_dock(unsigned m)
{
    row_name="G20 dock size and z-order";wm_begin(m,1);
    int i=wm_v3_window(DESK_V3_OVERLAPPED,-1,100,130,300,230);assert(i>=0);step("window before taskbar docking");
    for (int edge=0;edge<4;edge++) {
        d.sh.cfg.taskbar_edge=edge;d.sh.cfg.taskbar_size=edge>=CFG_BAR_LEFT ? 100 : 64;desk_shell_geometry(&d);step("taskbar dock and multirow/column layout");
        struct desk_box b=desk_taskbar(&d);
        int x=edge==CFG_BAR_LEFT ? b.x+b.w-1 : edge==CFG_BAR_RIGHT ? b.x : (int)d.width-5;
        int y=edge==CFG_BAR_BOTTOM ? b.y : edge==CFG_BAR_TOP ? b.y+b.h-1 : (int)d.height-5;
        pointer_to(x,y);press(1,1);assert(d.sh.bar_resize);
        pointer_to(x+(edge==CFG_BAR_LEFT ? 8 : edge==CFG_BAR_RIGHT ? -8 : 0),y+(edge==CFG_BAR_TOP ? 8 : edge==CFG_BAR_BOTTOM ? -8 : 0));
        step("taskbar resizing by inner border");press(1,0);step("taskbar resize committed");
    }
    d.sh.cfg.taskbar_top=0;d.sh.bar_raised=0;desk_shell_geometry(&d);
    d.clients[i].frame=(struct desk_box){450,180,190,290};desk_damage_add(&d.damage,(struct desk_box){0,0,(int)d.width,(int)d.height},d.width,d.height);step("window can cover taskbar without Always on Top");
    d.sh.cfg.taskbar_top=1;desk_shell_geometry(&d);step("taskbar raised above overlapping window");wm_end();
}
static void row_taskbar_autohide(unsigned m)
{
    row_name="G20 auto hide";wm_begin(m,1);(void)wm_v3_window(DESK_V3_OVERLAPPED,-1,90,150,340,240);
    for (int edge=0;edge<4;edge++) {
        d.sh.cfg.taskbar_edge=edge;d.sh.cfg.taskbar_autohide=1;desk_shell_geometry(&d);
        pointer_to(320,240);turn(1000000000ull);turn(600000000ull);assert(d.sh.bar_hidden);step("taskbar hides leaving narrow activation edge");
        struct desk_box b=desk_taskbar(&d);pointer_to(b.x+b.w/2,b.y+b.h/2);assert(!d.sh.bar_hidden);step("edge reveals auto hidden taskbar");
    }
    wm_end();
}
static void row_taskbar_toolbars(unsigned m)
{
    row_name="G20 toolbars";wm_begin(m,1);wm_custom_snapshot();
    for (unsigned k=0;k<d.sh.custom->count;k++) d.sh.custom->entries[k].parent=SH_ROOT_QUICK;
    d.sh.cfg.toolbar_address=d.sh.cfg.toolbar_links=d.sh.cfg.toolbar_desktop=1;
    strcpy(d.sh.cfg.toolbar_folder[0],"/Documents/Tools");strcpy(d.sh.custom->folders[SH_ROOT_CUSTOM],"/Documents/Tools");
    desk_shell_geometry(&d);step("folder backed Quick Launch Address Links Desktop and custom toolbar");
    struct desk_toolbar_slot slots[DESK_TOOLBAR_SLOTS];int n=desk_toolbar_slots(&d,slots);
    for (int k=0;k<n;k++) if (slots[k].kind==DESK_TOOLBAR_FOLDER || slots[k].kind==DESK_TOOLBAR_OVERFLOW) {
        click(slots[k].box.x+4,slots[k].box.y+4,1);step("folder toolbar overflow menu");tap(DU_KEY_ESC);step("toolbar popup dismissed");
    }
    d.sh.cfg.toolbar_links=0;desk_shell_geometry(&d);step("toolbar removed and tasks reflow");
    free(d.sh.custom);d.sh.custom=NULL;wm_end();
}
static void row_start_custom(unsigned m)
{
    row_name="G20 Start drag context icons and scroll";wm_begin(m,1);wm_custom_snapshot();
    desk_menu_open(&d,SH_MENU_START,0,0);step("pinned Start menu large icons");
    struct sh_menu *s=&d.sh.menus[0];int x=s->box.x+60,y=s->box.y+3+s->row_h/2;
    pointer_to(x,y);press(1,1);pointer_to(x,y+2*s->row_h);step("Start menu item being dragged");press(1,0);
    assert(d.sh.edit.operation==SH_EDIT_ORDER);step("Start menu reordered");d.sh.edit.operation=0;
    d.sh.cfg.start_small=1;desk_menu_open(&d,SH_MENU_START,0,0);step("small Start menu icons");
    s=&d.sh.menus[0];click(s->box.x+60,s->box.y+3+s->row_h/2,2);assert(d.sh.menus[0].kind==SH_MENU_START_ITEM);step("Start item context menu");tap(DU_KEY_ESC);
    d.sh.custom->count=24;
    for (unsigned k=3;k<24;k++) {
        d.sh.custom->entries[k]=d.sh.custom->entries[0];snprintf(d.sh.custom->entries[k].label,64,"Pin %u",k);
    }
    d.sh.cfg.start_small=0;desk_menu_open(&d,SH_MENU_START,0,0);assert(d.sh.menus[0].scroll_max>0);step("long Start menu scroll arrows");
    tap(DU_KEY_END);assert(d.sh.menus[0].scroll>0);step("keyboard scroll exposes last Start item");
    tap(DU_KEY_HOME);step("Start menu scroll back to top");
    desk_menu_close(&d);
    struct sh_dnd *g=&d.sh.dnd;g->active=1;g->button=1;g->source=-1;g->count=1;
    g->paths=calloc(1,sizeof(char *));g->paths[0]=strdup("/Documents/source.txt");g->types=malloc(1);g->types[0]='F';
    struct desk_box start=desk_start_button(&d);pointer_to(start.x+start.w/2,start.y+start.h/2);turn(600000000ull);
    assert(d.menu_open);step("external drag hover opens Start");
    press(1,0);step("shortcut drop closes its Start menu");
    free(d.sh.custom);d.sh.custom=NULL;wm_end();
}
static void row_shell_custom_prompts(unsigned m)
{
    row_name="G20 Address New Toolbar and Rename dialogs";wm_begin(m,1);wm_custom_snapshot();
    d.sh.cfg.toolbar_address=1;desk_shell_geometry(&d);
    struct desk_toolbar_slot slots[DESK_TOOLBAR_SLOTS];int n=desk_toolbar_slots(&d,slots);
    for (int k=0;k<n;k++) if (slots[k].kind==DESK_TOOLBAR_ADDRESS) { click(slots[k].box.x+5,slots[k].box.y+5,1);break; }
    assert(d.sh.address_focus);step("Address toolbar edit");tap(0x1e);step("Address text changed");tap(DU_KEY_ESC);step("Address dismissed");
    desk_menu_open(&d,SH_MENU_TOOLBARS,100,100);struct sh_menu *s=&d.sh.menus[0];
    int y=s->box.y+3;for (int k=0;k<s->count;k++) { if (s->items[k].action==SH_ACT_NEW_TOOLBAR) { click(s->box.x+60,y+s->row_h/2,1);break; }y+=s->items[k].flags&SH_ITEM_SEPARATOR ? SH_SEPARATOR : s->row_h; }
    assert(d.sh.prompt_mode==2);step("New Toolbar folder dialog");tap(DU_KEY_ESC);step("New Toolbar cancelled");
    desk_menu_open(&d,SH_MENU_START,0,0);s=&d.sh.menus[0];click(s->box.x+60,s->box.y+3+s->row_h/2,2);
    s=&d.sh.menus[0];click(s->box.x+60,s->box.y+3+2*s->row_h+s->row_h/2,1);
    assert(d.sh.prompt_mode==3);step("Start item Rename dialog");tap(DU_KEY_ESC);step("Rename cancelled");
    free(d.sh.custom);d.sh.custom=NULL;wm_end();
}
