/* SPDX-License-Identifier: MIT — G20 shell interaction, no file I/O. */
static int toolbar_hit(const struct desktop *d,int x,int y)
{
    struct desk_toolbar_slot slots[DESK_TOOLBAR_SLOTS];int n=desk_toolbar_slots(d,slots);
    for (int k=0;k<n;k++) if (desk_contains(slots[k].box,x,y)) return k;
    return -1;
}
static void toolbar_activate(struct desktop *d,int slot)
{
    struct desk_toolbar_slot slots[DESK_TOOLBAR_SLOTS];int n=desk_toolbar_slots(d,slots);
    if (slot<0 || slot>=n) return;
    struct desk_toolbar_slot *s=&slots[slot];
    d->sh.custom_refresh=1;
    if (s->kind==DESK_TOOLBAR_OVERFLOW) { desk_menu_open(d,SH_MENU_TOOLBAR_ALL,s->box.x,s->box.y+s->box.h);return; }
    if (s->kind==DESK_TOOLBAR_ADDRESS) { d->sh.address_focus=1;d->sh.bar_raised=1;desk_focus(d,-1);damage(d,desk_taskbar(d));return; }
    if (s->kind==DESK_TOOLBAR_FOLDER) {
        desk_menu_open(d,SH_MENU_FOLDER_BASE+s->arg,s->box.x,s->box.y+s->box.h);return;
    }
    if (s->kind==DESK_TOOLBAR_ITEM && d->sh.custom && (unsigned)s->arg<d->sh.custom->count) {
        const struct sh_folder_item *e=&d->sh.custom->entries[s->arg];
        if (e->folder>=0) desk_menu_open(d,SH_MENU_FOLDER_BASE+e->folder,s->box.x,s->box.y+s->box.h);
        else snprintf(d->sh.custom_open,sizeof(d->sh.custom_open),"%s",e->path);
    }
}
static void draw_toolbars(struct desktop *d,struct desk_box clip)
{
    struct desk_toolbar_slot slots[DESK_TOOLBAR_SLOTS];int n=desk_toolbar_slots(d,slots);
    for (int k=0;k<n;k++) {
        struct desk_toolbar_slot *s=&slots[k];struct desk_box b;
        if (!desk_intersect(s->box,clip,&b)) continue;
        struct du_canvas c=canvas(d,b); struct desk_paint p=painter(&c);
        desk_paint_task(&p,s->box.x+1,s->box.y,s->box.w-2,s->box.h,0,d->lfb_bytes_per_pixel==2);
        if (s->kind==DESK_TOOLBAR_ADDRESS) {
            struct du_rect field={s->box.x+2,s->box.y+2,s->box.w-4,s->box.h-4};du_well(&c,field,SH_WINDOW);
            c.clip=du_intersection(c.clip,(struct du_rect){field.x+3,field.y+2,field.w-6,field.h-4});
            const char *shown=d->sh.address_text[0] ? d->sh.address_text : d->sh.address_focus ? "" : "Address";
            while (*shown && du_text_width(shown)>field.w-12) shown++;
            du_text(&c,field.x+4,field.y+4,shown,SH_INK);
            if (d->sh.address_focus) du_vline(&c,field.x+4+du_text_width(shown),field.y+3,field.h-6,SH_INK);
        } else if (s->kind==DESK_TOOLBAR_ITEM) {
            const uint32_t *ic=desk_shell_icon(d->sh.custom && d->sh.custom->entries[s->arg].icon[0] ? du_icon_name(d->sh.custom->entries[s->arg].icon) : d->sh.custom && d->sh.custom->entries[s->arg].folder>=0 ? "folder" : "application-x-executable",16);
            if (ic) du_icon_blit(&c,s->box.x+(s->box.w-16)/2,s->box.y+5,ic,16,DU_ICON_NORMAL);
        } else {
            const char *label=s->kind==DESK_TOOLBAR_OVERFLOW ? ">>" : s->arg==SH_ROOT_QUICK ? ">>" :
                s->arg==SH_ROOT_LINKS ? "Links >>" : s->arg==SH_ROOT_DESKTOP ? "Desktop >>" :
                s->arg>=SH_ROOT_CUSTOM ? custom_basename(d->sh.cfg.toolbar_folder[s->arg-SH_ROOT_CUSTOM]) : "Toolbar";
            desk_paint_text_ellipsis(&p,desk_theme->font_ui,s->box.x+5,s->box.y+17,label,s->box.w-10,desk_theme->taskbar_text);
        }
    }
}
void desk_shell_geometry(struct desktop *d)
{
    d->sh.bar_hidden=0;
    d->sh.bar_raised=d->sh.bar_drag || d->sh.address_focus;
    desk_menu_close(d);
    for (int k=0;k<DESK_CLIENTS;k++) if (d->clients[k].window && d->clients[k].zoomed && !d->clients[k].minimized)
        desk_resize(d,k,desk_work_area(d));
    damage_all(d);
}
static void bar_motion(struct desktop *d)
{
    struct desk_shell *s=&d->sh;int x=d->mouse_x,y=d->mouse_y;
    if (s->bar_resize) {
        int size=s->bar_origin_size;
        switch (s->bar_origin_edge) {
        case CFG_BAR_TOP: size+=y-s->bar_press_y;break;
        case CFG_BAR_LEFT: size+=x-s->bar_press_x;break;
        case CFG_BAR_RIGHT: size-=x-s->bar_press_x;break;
        default: size-=y-s->bar_press_y;break;
        }
        int limit=(s->bar_origin_edge>=CFG_BAR_LEFT ? (int)d->width : (int)d->height-DESK_BAR)/2;
        if (size<DESK_BAR) size=DESK_BAR;
        if (size>limit) size=limit;
        if (size>512) size=512;
        if (s->cfg.taskbar_size!=size) { s->cfg.taskbar_size=size;desk_shell_geometry(d); }
    } else if (abs(x-s->bar_press_x)>4 || abs(y-s->bar_press_y)>4) {
        int edge=CFG_BAR_BOTTOM,dist=(int)d->height-y;
        if (y<dist) { edge=CFG_BAR_TOP;dist=y; }
        if (x<dist) { edge=CFG_BAR_LEFT;dist=x; }
        if ((int)d->width-x<dist) edge=CFG_BAR_RIGHT;
        if (s->cfg.taskbar_edge!=edge) { s->cfg.taskbar_edge=edge;desk_shell_geometry(d); }
    }
}
static void bar_reveal(struct desktop *d)
{
    if (d->sh.cfg.taskbar_autohide && d->sh.bar_hidden && desk_contains(desk_taskbar(d),d->mouse_x,d->mouse_y)) {
        d->sh.bar_hidden=0;d->sh.bar_leave_ns=0;damage_all(d);
    }
}
static void bar_autohide(struct desktop *d)
{
    struct desk_shell *s=&d->sh;
    if (!s->cfg.taskbar_autohide) { if (s->bar_hidden) { s->bar_hidden=0;damage_all(d); }return; }
    bar_reveal(d);
    if (s->bar_drag || s->levels || s->taskbar_key || s->address_focus || s->dnd.active || desk_contains(desk_taskbar(d),d->mouse_x,d->mouse_y)) { s->bar_leave_ns=0;return; }
    if (!s->bar_leave_ns) s->bar_leave_ns=d->now_ns;
    if (!s->bar_hidden && d->now_ns-s->bar_leave_ns>=500000000ull) { s->bar_hidden=1;damage_all(d); }
}

static int address_key(struct desktop *d,int code,int value)
{
    struct desk_shell *s=&d->sh;
    if (code==DU_KEY_LSHIFT || code==DU_KEY_RSHIFT || code==DU_KEY_LCTRL || code==DU_KEY_RCTRL) {
        unsigned mask=code==DU_KEY_LSHIFT || code==DU_KEY_RSHIFT ? DU_SHIFT : DU_CTRL;
        if (value) s->modifiers|=mask;else s->modifiers&=~mask;return 1;
    }
    if (!value) return 1;
    if (code==DU_KEY_ESC || code==DU_KEY_TAB) s->address_focus=0;
    else if (code==DU_KEY_ENTER) {
        if (s->address_length) snprintf(s->custom_open,sizeof(s->custom_open),"%s",s->address_text);
        s->address_focus=0;
    } else if (code==DU_KEY_BACKSPACE) { if (s->address_length) s->address_text[--s->address_length]=0; }
    else if ((s->modifiers&DU_CTRL) && code==0x1e) { s->address_length=0;s->address_text[0]=0; }
    else {
        unsigned ch=ciuki_keymap_char(s->cfg.keyboard,(unsigned)code,keymap_mods(d));
        if (ch>=32 && ch<127 && s->address_length+1<sizeof(s->address_text)) { s->address_text[s->address_length++]=(char)ch;s->address_text[s->address_length]=0; }
    }
    damage(d,desk_taskbar(d));return 1;
}
