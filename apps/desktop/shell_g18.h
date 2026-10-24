/* SPDX-License-Identifier: MIT
 * G18 private shell helpers. Included by shell.c after its painting helpers. */
#ifndef CIUKI_SHELL_G18_H
#define CIUKI_SHELL_G18_H
#define SH_LINK_MENU_BASE 128
#define SH_LINK_MENU_DEPTH 16
static unsigned shortcut_vk(int code)
{
    unsigned ch=ciuki_keymap_char(CFG_KEYS_US,(unsigned)code,0);
    if (ch>='a' && ch<='z') return ch-'a'+'A';
    if ((ch>='0' && ch<='9') || ch==' ') return ch;
    if (code>=0x3b && code<=0x44) return 0x70u+(unsigned)code-0x3b;
    if (code==0x57 || code==0x58) return 0x7au+(unsigned)code-0x57;
    switch (code) {
    case DU_KEY_LEFT: return 0x25; case DU_KEY_UP: return 0x26;
    case DU_KEY_RIGHT: return 0x27; case DU_KEY_DOWN: return 0x28;
    case DU_KEY_HOME: return 0x24; case DU_KEY_END: return 0x23;
    case DU_KEY_PGUP: return 0x21; case DU_KEY_PGDN: return 0x22;
    case DU_KEY_INSERT: return 0x2d; case DU_KEY_DELETE: return 0x2e;
    default: return 0;
    }
}
static unsigned shortcut_modifiers(const struct desktop *d,int code)
{
    return (d->sh.modifiers&DU_SHIFT ? 1u : 0u) |
        (d->sh.modifiers&DU_CTRL ? 2u : 0u) |
        (d->sh.modifiers&DU_ALT ? 4u : 0u) | (code>=0x100 ? 8u : 0u);
}
static int shortcut_hotkey(struct desktop *d,int code,int value)
{
    if (d->focus>=0 && d->clients[d->focus].internal==DESK_INTERNAL_SHORTCUT) return 0;
    unsigned vk=shortcut_vk(code), mods=shortcut_modifiers(d,code);
    if (code==0x153 && (mods&6u)==6u) return 0; /* system Ctrl+Alt+Del */
    if (!vk || !(mods&7u)) return 0;
    for (unsigned k=0;k<d->v3_link_count;k++) if (d->v3_links[k].launch.hotkey==(vk|(mods<<8))) {
        if (value==1) {
            d->sh.alt_used=1;
            if (desk_link_launch(d,k)<0) desk_shortcut_properties(d,k,1);
        }
        return 1;
    }
    return 0;
}
void desk_shortcut_properties(struct desktop *d,unsigned link,int repair)
{
    if (link>=d->v3_link_count) return;
    d->sh.shortcut_record=d->v3_links[link].launch;
    snprintf(d->sh.shortcut_source,sizeof(d->sh.shortcut_source),"%s",d->v3_links[link].source);
    snprintf(d->sh.shortcut_number,sizeof(d->sh.shortcut_number),"%d",d->sh.shortcut_record.icon_index);
    snprintf(d->sh.shortcut_status,sizeof(d->sh.shortcut_status),"%s",repair ?
        "The target could not be started. Enter its new location, then Repair Target." : "");
    d->sh.shortcut_index=(int)link; d->sh.shortcut_field=0; d->sh.shortcut_repair=(unsigned)repair;
    desk_menu_close(d);
    int i=desk_internal_open(d,DESK_INTERNAL_SHORTCUT);
    if (i>=0) {
        snprintf(d->clients[i].title,sizeof(d->clients[i].title),"%.48s Properties",d->sh.shortcut_record.label);
        damage(d,desk_outer(&d->clients[i])); damage(d,desk_taskbar(d));
    }
}
static char *shortcut_field(struct desktop *d,unsigned field,size_t *cap)
{
    switch (field) {
    case 0: *cap=sizeof(d->sh.shortcut_record.target); return d->sh.shortcut_record.target;
    case 1: *cap=sizeof(d->sh.shortcut_record.arguments); return d->sh.shortcut_record.arguments;
    case 2: *cap=sizeof(d->sh.shortcut_record.directory); return d->sh.shortcut_record.directory;
    case 5: *cap=sizeof(d->sh.shortcut_record.icon); return d->sh.shortcut_record.icon;
    case 6: *cap=sizeof(d->sh.shortcut_number); return d->sh.shortcut_number;
    default: *cap=0; return NULL;
    }
}
static struct desk_box shortcut_field_box(struct desk_box c,unsigned row)
{ return (struct desk_box){c.x+116,c.y+48+(int)row*32,c.w-130,24}; }
static struct desk_box shortcut_action_box(struct desk_box c,unsigned row)
{ return (struct desk_box){c.x+12+(int)row*172,c.y+282,164,26}; }
static void shortcut_hotkey_text(uint32_t key,char *out,size_t cap)
{
    unsigned vk=key&255u,mods=key>>8;
    if (!vk) { snprintf(out,cap,"None"); return; }
    char name[24];
    if (vk>=0x70 && vk<=0x7b) snprintf(name,sizeof(name),"F%u",vk-0x6f);
    else if ((vk>='A' && vk<='Z') || (vk>='0' && vk<='9')) snprintf(name,sizeof(name),"%c",(char)vk);
    else snprintf(name,sizeof(name),"VK %02X",vk);
    snprintf(out,cap,"%s%s%s%s",mods&2 ? "Ctrl+" : "",mods&4 ? "Alt+" : "",mods&1 ? "Shift+" : "",name);
}
static void shortcut_paint(struct desktop *d,int index,struct du_canvas *c,struct desk_box content)
{
    static const char *const labels[]={"Target:","Arguments:","Start in:","Shortcut key:","Run:","Icon path:","Icon index:"};
    const uint32_t *icon=d->v3_link_icon ? d->v3_link_icon(d,d->sh.shortcut_source,32) : NULL;
    du_icon_blit(c,content.x+12,content.y+8,icon ? icon : desk_shell_icon("application-x-executable",32),32,DU_ICON_NORMAL);
    bold(c,content.x+56,content.y+14,d->sh.shortcut_record.label,SH_INK);
    for (unsigned k=0;k<7;k++) {
        struct desk_box b=shortcut_field_box(content,k);
        du_text(c,content.x+12,b.y+5,labels[k],SH_INK);
        du_well(c,R(b),SH_WINDOW);
        if (d->focus==index && d->sh.shortcut_field==k) du_focus_ring(c,R(b));
        char hotkey[64]; size_t cap;
        const char *text=shortcut_field(d,k,&cap);
        if (k==3) { shortcut_hotkey_text(d->sh.shortcut_record.hotkey,hotkey,sizeof(hotkey)); text=hotkey; }
        if (k==4) text=d->sh.shortcut_record.show==3 ? "Maximized" : d->sh.shortcut_record.show==7 ? "Minimized" : "Normal window";
        struct du_canvas fc=*c; fc.clip=du_intersection(c->clip,(struct du_rect){b.x+3,b.y+3,b.w-6,b.h-6});
        while (*text && du_text_width(text)>b.w-12) text++;
        du_text(&fc,b.x+5,b.y+5,text,SH_INK);
    }
    push_button(c,shortcut_action_box(content,0),"Find Target",d->sh.shortcut_field==7);
    push_button(c,shortcut_action_box(content,1),"Change Icon",d->sh.shortcut_field==8);
    push_button(c,shortcut_action_box(content,2),"Repair Target",d->sh.shortcut_field==9);
    du_text_wrap(c,(struct du_rect){content.x+12,content.y+316,content.w-24,28},d->sh.shortcut_status,SH_INK);
    push_button(c,button_box(content,0,3),"OK",d->sh.shortcut_field==10);
    push_button(c,button_box(content,1,3),"Cancel",d->sh.shortcut_field==11);
    push_button(c,button_box(content,2,3),"Apply",d->sh.shortcut_field==12);
}
static int shortcut_save(struct desktop *d,int index,int close)
{
    char *end=NULL; errno=0; long n=strtol(d->sh.shortcut_number,&end,10);
    if (errno==ERANGE || !d->sh.shortcut_number[0] || *end || n<(-2147483647L-1L) || n>2147483647L || !desk_launch_valid(&d->sh.shortcut_record)) {
        snprintf(d->sh.shortcut_status,sizeof(d->sh.shortcut_status),"Enter a valid absolute target and signed icon index.");
        damage(d,desk_outer(&d->clients[index])); return 0;
    }
    d->sh.shortcut_record.icon_index=(int32_t)n;
    int error=d->v3_link_save ? d->v3_link_save(d,d->sh.shortcut_source,&d->sh.shortcut_record) : -EOPNOTSUPP;
    snprintf(d->sh.shortcut_status,sizeof(d->sh.shortcut_status),"%s",error ?
        "The shortcut could not be saved. Try again after the current operation." : "Changes queued for saving.");
    if (!error && close) desk_request_close(d,index);
    else damage(d,desk_outer(&d->clients[index]));
    return !error;
}
static void shortcut_action(struct desktop *d,int index,unsigned action)
{
    if (action==7) {
        char path[260]; const char *target=d->sh.shortcut_record.target;
        if ((target[0]=='C' || target[0]=='c') && target[1]==':' && (target[2]=='/' || target[2]=='\\')) target+=2;
        if (*target!='/' && (target==d->sh.shortcut_record.target || *target!='\\')) {
            snprintf(d->sh.shortcut_status,sizeof(d->sh.shortcut_status),"Find Target supports the system C: drive and native absolute paths.");
            damage(d,desk_outer(&d->clients[index])); return;
        }
        snprintf(path,sizeof(path),"%s",target);
        for (char *p=path;*p;p++) if (*p=='\\') *p='/';
        char *last=strrchr(path,'/');
        if (last) { if (last==path) last[1]=0; else *last=0; }
        launch_named(d,"files",path);
    } else if (action==8) {
        d->sh.shortcut_field=5;
        snprintf(d->sh.shortcut_status,sizeof(d->sh.shortcut_status),"Enter an .ico path and its icon index, then Apply.");
    } else if (action==9 || action==12) (void)shortcut_save(d,index,0);
    else if (action==10) (void)shortcut_save(d,index,1);
    else if (action==11) desk_request_close(d,index);
    damage(d,desk_outer(&d->clients[index]));
}
static int shortcut_click(struct desktop *d,int index,int32_t x,int32_t y)
{
    struct desk_box content=desk_content(&d->clients[index]);
    for (unsigned k=0;k<7;k++) if (desk_contains(shortcut_field_box(content,k),x,y)) {
        d->sh.shortcut_field=k;
        if (k==4) d->sh.shortcut_record.show=d->sh.shortcut_record.show==1 ? 7 : d->sh.shortcut_record.show==7 ? 3 : 1;
        damage(d,desk_outer(&d->clients[index])); return 1;
    }
    for (unsigned k=0;k<3;k++) {
        if (desk_contains(shortcut_action_box(content,k),x,y)) { d->sh.shortcut_field=k+7; shortcut_action(d,index,k+7); return 1; }
        if (desk_contains(button_box(content,(int)k,3),x,y)) { d->sh.shortcut_field=k+10; shortcut_action(d,index,k+10); return 1; }
    }
    return 1;
}
static int shortcut_key(struct desktop *d,int index,int code,int value)
{
    if (!value) return 1;
    unsigned field=d->sh.shortcut_field;
    if (code==DU_KEY_ESC) desk_request_close(d,index);
    else if (code==DU_KEY_TAB) d->sh.shortcut_field=(field+(d->sh.modifiers&DU_SHIFT ? 12u : 1u))%13u;
    else if (code==DU_KEY_ENTER) shortcut_action(d,index,field>=7 ? field : 10u);
    else if (field==3) {
        unsigned vk=shortcut_vk(code);
        if (code==DU_KEY_BACKSPACE || code==DU_KEY_DELETE) d->sh.shortcut_record.hotkey=0;
        else if (vk) {
            unsigned mods=shortcut_modifiers(d,code); if (!(mods&6u)) mods|=6u;
            d->sh.shortcut_record.hotkey=vk|(mods<<8);
        }
    } else if (field==4 && (code==DU_KEY_SPACE || code==DU_KEY_LEFT || code==DU_KEY_RIGHT || code==DU_KEY_UP || code==DU_KEY_DOWN))
        d->sh.shortcut_record.show=d->sh.shortcut_record.show==1 ? 7 : d->sh.shortcut_record.show==7 ? 3 : 1;
    else if (field>=7 && code==DU_KEY_SPACE) shortcut_action(d,index,field);
    else {
        size_t cap; char *text=shortcut_field(d,field,&cap);
        if (text) {
            size_t n=strlen(text);
            if ((d->sh.modifiers&DU_CTRL) && code==0x1e) text[0]=0; /* replace whole field */
            else if (code==DU_KEY_BACKSPACE && n) { do { n--; } while (n && ((unsigned char)text[n]&0xc0u)==0x80u); text[n]=0; }
            else if (!(d->sh.modifiers&(DU_CTRL|DU_ALT))) {
                unsigned mods=d->sh.modifiers&DU_SHIFT ? CIUKI_KEYMAP_SHIFT : 0;
                unsigned ch=ciuki_keymap_char(d->sh.cfg.keyboard,(unsigned)code,mods);
                char encoded[5]; size_t bytes=ciuki_utf8_encode(ch,encoded);
                if (ch>=32 && ch!=127 && bytes && n+bytes<cap) { memcpy(text+n,encoded,bytes); text[n+bytes]=0; }
            }
        }
    }
    damage(d,desk_outer(&d->clients[index])); return 1;
}
/* Dynamic submenu identity: a representative link plus the full component
 * depth, rather than the visible (possibly duplicate/truncated) folder name. */
static const char *shortcut_programs_path(const struct desktop *d,const struct desk_link_entry *e)
{
    const char *roots[]={d->sh.profile.programs,"/Windows/Start Menu/Programs"};
    for (unsigned k=0;k<2;k++) {
        size_t n=strlen(roots[k]);
        if (n && !strncmp(e->source,roots[k],n) && e->source[n]=='/') return e->source+n+1;
    }
    return NULL;
}
static const char *shortcut_component(const char *path,unsigned depth)
{
    while (depth-- && path) { path=strchr(path,'/'); if (path) path++; }
    return path;
}
static int shortcut_menu_compare(const struct sh_item *a,const struct sh_item *b)
{
    int folder_a=a->action==SH_ACT_SUBMENU,folder_b=b->action==SH_ACT_SUBMENU;
    if (folder_a!=folder_b) return folder_b-folder_a;
    const unsigned char *x=(const unsigned char *)a->label,*y=(const unsigned char *)b->label;
    while (*x && *y) {
        unsigned xx=*x>='A' && *x<='Z' ? *x+32u : *x;
        unsigned yy=*y>='A' && *y<='Z' ? *y+32u : *y;
        if (xx!=yy) return xx<yy ? -1 : 1;
        x++; y++;
    }
    return (*x!=0)-(*y!=0);
}
static void shortcut_programs_menu(struct desktop *d,struct sh_menu *m,int kind)
{
    const char *parent=NULL; unsigned depth=0, prefix=0;
    if (kind>=SH_LINK_MENU_BASE) {
        unsigned encoded=(unsigned)(kind-SH_LINK_MENU_BASE), index=encoded/SH_LINK_MENU_DEPTH;
        depth=encoded%SH_LINK_MENU_DEPTH;
        if (index>=d->v3_link_count || !depth) return;
        parent=shortcut_programs_path(d,&d->v3_links[index]);
        const char *at=parent ? shortcut_component(parent,depth) : NULL;
        if (!at) return;
        prefix=(unsigned)(at-parent);
    } else if (kind==SH_MENU_ACCESSORIES) parent="Accessories/";
    else if (kind==SH_MENU_SYSTEM_TOOLS) parent="System Tools/";
    else if (kind==SH_MENU_GAMES) parent="Games/";
    else if (kind!=SH_MENU_PROGRAMS) return;
    if (parent && !prefix) { prefix=(unsigned)strlen(parent); depth=1; }
    unsigned first=(unsigned)m->count;
    for (unsigned k=0;k<d->v3_link_count;k++) {
        const struct desk_link_entry *e=&d->v3_links[k];
        if (e->desktop) continue;
        const char *path=shortcut_programs_path(d,e), *child=path;
        if (parent) { if (!path || strncmp(path,parent,prefix)) continue; child=path+prefix; }
        if (!child) { if (!parent) item(m,e->launch.label,"application-x-executable",SH_ACT_WIN32_LINK,(int)k,0); continue; }
        const char *slash=strchr(child,'/');
        if (!slash) { item(m,e->launch.label,"application-x-executable",SH_ACT_WIN32_LINK,(int)k,0); continue; }
        size_t length=(size_t)(slash-child); if (!length || depth+1>=SH_LINK_MENU_DEPTH) continue;
        int duplicate=0;
        for (unsigned j=0;j<(unsigned)m->count;j++) if (m->items[j].action==SH_ACT_SUBMENU) {
            if (m->items[j].arg<SH_LINK_MENU_BASE) {
                if (strlen(m->items[j].label)==length && !strncmp(m->items[j].label,child,length)) duplicate=1;
                continue;
            }
            unsigned id=(unsigned)(m->items[j].arg-SH_LINK_MENU_BASE)/SH_LINK_MENU_DEPTH;
            if (id<d->v3_link_count) {
                const char *old=shortcut_programs_path(d,&d->v3_links[id]);
                old=old ? shortcut_component(old,depth) : NULL;
                if (old && !strncmp(old,child,length) && old[length]=='/') duplicate=1;
            }
        }
        if (!duplicate) {
            char label[44]; size_t n=length<sizeof(label)-1 ? length : sizeof(label)-1;
            memcpy(label,child,n);label[n]=0;
            item(m,label,"folder",SH_ACT_SUBMENU,SH_LINK_MENU_BASE+(int)k*SH_LINK_MENU_DEPTH+(int)depth+1,0);
        }
    }
    /* Windows 98 initially orders program groups alphabetically; keep the
     * native catalogue intact and sort the installed portion by folder/name. */
    for (unsigned k=first+1;k<(unsigned)m->count;k++) {
        struct sh_item saved=m->items[k]; unsigned j=k;
        while (j>first && shortcut_menu_compare(&saved,&m->items[j-1])<0) { m->items[j]=m->items[j-1];j--; }
        m->items[j]=saved;
    }
    if (kind>=SH_LINK_MENU_BASE && m->count==(int)first) item(m,"(Empty)",NULL,SH_ACT_NONE,0,SH_ITEM_DISABLED);
}
#endif
