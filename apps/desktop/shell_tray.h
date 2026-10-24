/* G18 private implementation, included after tray geometry helpers. */
static struct desk_box program_tray_box(const struct desktop *d,unsigned slot)
{
    if (slot>=SH_PROGRAM_TRAY || !d->sh.program_tray[slot].window) return (struct desk_box){0};
    struct desk_box b=tray_box(d);
    unsigned before=0;
    for (unsigned k=0;k<slot;k++) before+=!!d->sh.program_tray[k].window;
    return (struct desk_box){b.x+SH_TRAY_MARGIN+(int)before*(SH_TRAY_ICON_SIZE+SH_TRAY_GAP),
        b.y+(b.h-SH_TRAY_ICON_SIZE)/2,SH_TRAY_ICON_SIZE,SH_TRAY_ICON_SIZE};
}
static int program_tray_find(const struct desktop *d,uint32_t window,uint32_t id)
{
    for (unsigned k=0;k<SH_PROGRAM_TRAY;k++)
        if (d->sh.program_tray[k].window==window && d->sh.program_tray[k].id==id) return (int)k;
    return -1;
}
static void program_tray_event(struct desktop *d,unsigned slot,uint32_t event)
{
    const struct sh_program_tray *t=&d->sh.program_tray[slot];
    if (!t->window || !t->callback) return;
    for (int i=0;i<DESK_CLIENTS;i++) if (d->clients[i].window_id==t->window && d->clients[i].window) {
        struct ciuki_message m;
        desk_v3_message(&m,DESK_V3_TRAY_EVENT,20,0,d->clients[i].v3_id,d->clients[i].v3_generation);
        desk_put32(m.data+16,t->id); desk_put32(m.data+20,t->callback); desk_put32(m.data+24,event);
        desk_put32(m.data+28,(uint32_t)d->mouse_x); desk_put32(m.data+32,(uint32_t)d->mouse_y);
        desk_queue(d,i,&m); break;
    }
}
static void program_balloon_cancel(struct desktop *d,uint32_t window,uint32_t id,int all)
{
    unsigned n=0;
    for (unsigned k=0;k<d->sh.program_balloon_count;k++) {
        struct sh_program_balloon *b=&d->sh.program_balloons[k];
        if (b->window!=window || (!all && b->id!=id)) d->sh.program_balloons[n++]=*b;
    }
    d->sh.program_balloon_count=n;
    if (d->sh.balloon_window==window && (all || d->sh.balloon_id==id)) {
        int slot=program_tray_find(d,window,d->sh.balloon_id);
        if (slot>=0) program_tray_event(d,(unsigned)slot,0x403);
        if (d->sh.notification.active) damage(d,desk_shell_notification_bounds(d));
        d->sh.notification.active=0; d->sh.balloon_window=0;
        damage(d,notification_icon_box(d));
    }
}
static void program_balloon_dismiss(struct desktop *d,uint32_t event)
{
    if (!d->sh.balloon_window) return;
    int slot=program_tray_find(d,d->sh.balloon_window,d->sh.balloon_id);
    if (slot>=0) program_tray_event(d,(unsigned)slot,event);
    d->sh.balloon_window=0;
}
void desk_shell_program_remove(struct desktop *d,uint32_t window)
{
    int previous=tray_box(d).w, changed=0;
    for (unsigned k=0;k<SH_PROGRAM_TRAY;k++) if (d->sh.program_tray[k].window==window) {
        memset(&d->sh.program_tray[k],0,sizeof(d->sh.program_tray[k])); changed=1;
    }
    program_balloon_cancel(d,window,0,1);
    if (changed && d->shell) { tooltip_hide(d); desk_shell_tray_damage(d,previous); }
}
int desk_shell_program_icon(struct desktop *d,int owner,const struct desk_packet *p,const struct ciuki_surface_info *info,int fd)
{
    uint32_t window=d->clients[owner].window_id;
    int slot=program_tray_find(d,window,p->index), previous=tray_box(d).w;
    int result=0;
    if (!d->shell) result=-EOPNOTSUPP;
    else if (p->flags==0 && slot>=0) result=-EEXIST;
    else if (p->flags && slot<0) result=-ENOENT;
    if (!result && slot<0) {
        for (unsigned k=0;k<SH_PROGRAM_TRAY;k++) if (!d->sh.program_tray[k].window) { slot=(int)k; break; }
        if (slot<0) result=-ENOSPC;
    }
    const uint8_t *pixels=NULL;
    if (!result && fd>=0) {
        pixels=d->ops.map(fd,PROT_READ);
        if (!pixels || pixels==MAP_FAILED) { pixels=NULL; result=-ENOMEM; }
    }
    if (!result) {
        tooltip_hide(d);
        struct sh_program_tray *t=&d->sh.program_tray[slot];
        if (p->flags==2) {
            program_balloon_cancel(d,window,p->index,0); memset(t,0,sizeof(*t));
        } else {
            if (!p->flags) { memset(t,0,sizeof(*t)); t->window=window; t->id=p->index; }
            if (p->style&1u) t->callback=p->serial;
            if (p->style&4u) memcpy(t->tip,p->tooltip,sizeof(t->tip));
            if (pixels) for (unsigned y=0;y<16;y++) memcpy(t->pixels+y*16,pixels+y*info->stride,64);
        }
        desk_shell_tray_damage(d,previous);
    }
    if (pixels) d->ops.unmap((void *)pixels,info->allocation_bytes);
    return result;
}
int desk_shell_program_balloon(struct desktop *d,int owner,const struct desk_packet *p)
{
    uint32_t window=d->clients[owner].window_id;
    if (program_tray_find(d,window,p->index)<0) return -ENOENT;
    if (!p->argument[0]) { program_balloon_cancel(d,window,p->index,0); return 0; }
    if (d->sh.program_balloon_count==SH_PROGRAM_TRAY) return -ENOSPC;
    struct sh_program_balloon *b=&d->sh.program_balloons[d->sh.program_balloon_count++];
    b->window=window; b->id=p->index; b->timeout=p->serial; b->icon=p->style;
    memcpy(b->title,p->title,sizeof(b->title)); memcpy(b->text,p->argument,sizeof(b->text));
    return 0;
}
static void program_balloon_tick(struct desktop *d)
{
    if (d->sh.notification.active || !d->sh.program_balloon_count) return;
    struct sh_program_balloon b=d->sh.program_balloons[0];
    memmove(d->sh.program_balloons,d->sh.program_balloons+1,--d->sh.program_balloon_count*sizeof(b));
    int slot=program_tray_find(d,b.window,b.id);
    if (slot<0) return;
    int icon=b.icon==2 ? DU_DLG_ICON_WARNING : b.icon==3 ? DU_DLG_ICON_ERROR : b.icon==1 ? DU_DLG_ICON_INFO : DU_DLG_ICON_NONE;
    desk_shell_notify(d,b.title,b.text,icon,SH_NOTIFY_DETAILS);
    d->sh.balloon_window=b.window; d->sh.balloon_id=b.id;
    d->sh.notification.expires=d->now_ns+(uint64_t)b.timeout*1000000ull;
    program_tray_event(d,(unsigned)slot,0x402); /* NIN_BALLOONSHOW */
}
