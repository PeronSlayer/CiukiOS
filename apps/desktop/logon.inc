/* SPDX-License-Identifier: MIT
 * Opaque desktop-owned gate; never pass password input to application windows. */
static struct desk_box logon_box(const struct desktop *d)
{
    int w=(int)d->width-16; if (w>420) w=420;
    int h=250; if (h>(int)d->height-16) h=(int)d->height-16;
    return (struct desk_box){((int)d->width-w)/2,((int)d->height-h)/2,w,h};
}
static void logon_clear(struct desktop *d)
{
    volatile char *p=(volatile char *)&d->sh.user_request;
    for (size_t i=0;i<sizeof(d->sh.user_request);i++) p[i]=0;
}
void desk_logon_open(struct desktop *d,int mode)
{
    if (!d->shell || (mode!=1 && mode!=2)) return;
    desk_v3_revoke(d); desk_v3_pointer_release(d); desk_shell_drag_cancel(d); desk_menu_close(d);
    d->sh.user_previous_focus=d->focus;
    desk_focus(d,-1);
    memset(d->keys,0,sizeof(d->keys)); d->buttons=d->routed_buttons=0; d->drag=-1;
    d->sh.logo_down=d->sh.alt_routed=d->sh.alt_used=d->sh.alt_code=0;
    d->sh.user_gate=mode; d->sh.user_focus=mode==2 ? 1 : 0;
    d->sh.user_choose=0; d->sh.user_pending=0; logon_clear(d);
    d->sh.user_status[0]=0;
    d->sh.volume_open=d->sh.network_open=d->sh.media_open=d->sh.switcher=d->sh.tooltip=0;
    d->sh.modifiers=0;
    if (mode==2) snprintf(d->sh.user_request.name,USERS_NAME,"%s",d->sh.profile.name);
    else if (d->sh.users.count) snprintf(d->sh.user_request.name,USERS_NAME,"%s",d->sh.users.names[0]);
    desk_cursor_update(d); damage_all(d);
}
void desk_session_lock(struct desktop *d)
{
    if (!d->sh.user_gate && (d->sh.users.flags&USERS_LOCK) && d->sh.profile.name[0]) desk_logon_open(d,2);
}
void desk_logon_done(struct desktop *d)
{
    int visible=d->sh.user_gate>0;
    d->sh.user_gate=d->sh.user_choose=d->sh.user_pending=0;
    d->sh.modifiers=0; d->sh.user_status[0]=0; logon_clear(d);
    d->last_input_ns=d->now_ns;
    desk_focus(d,d->sh.user_previous_focus>=0 && d->sh.user_previous_focus<DESK_CLIENTS && d->clients[d->sh.user_previous_focus].window ? d->sh.user_previous_focus : -1);
    desk_cursor_update(d); if (visible) damage_all(d);
}
void desk_logon_result(struct desktop *d,const struct users_view *v,int error)
{
    struct desk_shell *s=&d->sh; s->user_pending=0;
    memset(s->user_request.password,0,USERS_PASSWORD);
    if (error) {
        snprintf(s->user_status,sizeof(s->user_status),"%s",error==-EACCES ? "The user name or password is incorrect." : "User settings could not be loaded. Try again.");
    } else {
        s->users=*v; s->user_status[0]=0;
        if (v->authenticated) {
            if (s->user_gate==2) desk_logon_done(d);
            else s->user_choose=1;
        } else if (s->user_gate==1 && !s->user_request.name[0] && v->count)
            snprintf(s->user_request.name,USERS_NAME,"%s",v->names[0]);
    }
    damage_all(d);
}
static void logon_submit(struct desktop *d)
{
    struct desk_shell *s=&d->sh;
    if (s->user_pending || s->user_want || s->user_choose) return;
    s->user_request.operation=USERS_AUTH; s->user_want=1;
    snprintf(s->user_status,sizeof(s->user_status),"Checking password..."); damage_all(d);
}
static void logon_cancel(struct desktop *d)
{
    if (d->sh.user_pending || d->sh.user_want) return;
    memset(d->sh.user_request.password,0,USERS_PASSWORD);
    if (d->sh.user_gate==1) d->sh.user_choose=2;
    else d->sh.user_status[0]=0;
    damage_all(d);
}
static void logon_paint(struct desktop *d,struct desk_box clip)
{
    struct du_canvas c=canvas(d,clip); struct desk_box b=logon_box(d);
    du_fill(&c,(struct du_rect){0,0,(int)d->width,(int)d->height},CFG_DESKTOP_COLOR);
    popup_face(d,&c,b);
    du_icon_blit(&c,b.x+12,b.y+12,desk_shell_icon("ciuki",32),32,DU_ICON_NORMAL);
    bold(&c,b.x+54,b.y+14,d->sh.user_gate==2 ? "Unlock CiukiOS" : "Welcome to CiukiOS",SH_INK);
    du_text_fit(&c,b.x+12,b.y+52,b.w-24,"User name (Up/Down selects a user):",SH_INK);
    for (int i=0;i<2;i++) {
        struct du_rect r={b.x+12,b.y+76+i*55,b.w-24,24};
        if (i) du_text(&c,r.x,r.y-18,"Password:",SH_INK);
        du_fill(&c,r,SH_WINDOW); du_edge(&c,r,DU_SUNKEN);
        if (d->sh.user_focus==i) du_outline(&c,(struct du_rect){r.x+2,r.y+2,r.w-4,r.h-4},SH_HOT);
        char mask[USERS_PASSWORD]; size_t n=strlen(d->sh.user_request.password);
        memset(mask,'*',n); mask[n]=0;
        du_text_fit(&c,r.x+6,r.y+5,r.w-12,i ? mask : d->sh.user_request.name,SH_INK);
    }
    du_text_fit(&c,b.x+12,b.y+167,b.w-24,d->sh.user_status,SH_INK);
    push_button(&c,(struct desk_box){b.x+b.w-174,b.y+b.h-36,75,23},"OK",1);
    push_button(&c,(struct desk_box){b.x+b.w-90,b.y+b.h-36,75,23},"Cancel",0);
}
int desk_logon_input(struct desktop *d,const struct ciuki_input_event *e)
{
    struct desk_shell *s=&d->sh;
    if (!s->user_gate) return 0;
    if (s->user_gate<0) return 1;
    if (e->type==CIUKI_INPUT_RESYNC) { s->modifiers=0; return 1; }
    if (e->type==CIUKI_INPUT_MOTION) {
        int64_t x=(int64_t)d->mouse_x+e->value,y=(int64_t)d->mouse_y+e->value2;
        d->mouse_x=x<0 ? 0 : x>=d->width ? (int)d->width-1 : (int)x;
        d->mouse_y=y<0 ? 0 : y>=d->height ? (int)d->height-1 : (int)y;
        d->cursor_dirty=1; return 1;
    }
    if (s->user_pending || s->user_want || s->user_choose) return 1;
    if (e->type==CIUKI_INPUT_BUTTON && e->code==CIUKI_BUTTON_LEFT && e->value) {
        struct desk_box b=logon_box(d); int x=d->mouse_x,y=d->mouse_y;
        if (desk_contains((struct desk_box){b.x+b.w-174,b.y+b.h-36,75,23},x,y)) logon_submit(d);
        else if (desk_contains((struct desk_box){b.x+b.w-90,b.y+b.h-36,75,23},x,y)) logon_cancel(d);
        else if (desk_contains((struct desk_box){b.x+12,b.y+76,b.w-24,24},x,y) && s->user_gate==1) s->user_focus=0;
        else if (desk_contains((struct desk_box){b.x+12,b.y+131,b.w-24,24},x,y)) s->user_focus=1;
        damage_all(d); return 1;
    }
    if (e->type!=CIUKI_INPUT_KEY) return 1; /* text, wheel and consumer keys never escape the gate */
    unsigned bit=e->code==0x2a || e->code==0x36 ? DU_SHIFT : e->code==0x1d || e->code==0x11d ? DU_CTRL : e->code==0x38 || e->code==0x138 ? DU_ALT : 0;
    if (bit) { if (e->value) s->modifiers|=bit; else s->modifiers&=~bit; return 1; }
    if (!e->value) return 1;
    if (e->code==DU_KEY_ENTER) { logon_submit(d); return 1; }
    if (e->code==DU_KEY_ESC) { logon_cancel(d); return 1; }
    if (e->code==DU_KEY_TAB) s->user_focus=s->user_gate==2 ? 1 : !s->user_focus;
    else if ((e->code==DU_KEY_UP || e->code==DU_KEY_DOWN) && s->user_gate==1 && s->users.count) {
        unsigned i=0; while (i<s->users.count && strcmp(s->user_request.name,s->users.names[i])) i++;
        i=e->code==DU_KEY_UP ? (i ? i-1 : s->users.count-1) : (i+1)%s->users.count;
        strcpy(s->user_request.name,s->users.names[i]); memset(s->user_request.password,0,USERS_PASSWORD);
    } else {
        char *text=s->user_focus ? s->user_request.password : s->user_request.name;
        size_t n=strlen(text), cap=s->user_focus ? USERS_PASSWORD : USERS_NAME;
        if (e->code==DU_KEY_BACKSPACE && n) text[n-1]=0;
        else {
            unsigned ch=ciuki_keymap_char(s->cfg.keyboard,(unsigned)e->code,keymap_mods(d));
            if (ch>=32 && ch<=126 && n+1<cap) { text[n]=(char)ch; text[n+1]=0; }
        }
    }
    damage_all(d); return 1;
}
