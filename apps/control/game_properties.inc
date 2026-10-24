/* Existing per-game controller properties, extended with VMM port bindings.
 * Storage is a copied worker request; edits never touch a running VM.
 * SPDX-License-Identifier: MIT */
#include "../desktop/catalog.h"
enum { G_GAME=1500,G_JOY,G_COM1,G_COM2,G_COM3,G_COM4,G_LPT,G_PORT,G_PATH,G_APPLY,G_BACK,G_NOTE,G_JOY_NOTE,G_REFRESH,G_DOS };
static void game_build(struct cp_app *a);
static int game_ready(struct cp_app *a)
{ return a->game_loaded && a->game_joy_ready && !a->game_busy && !a->joystick_busy && !a->joystick_save_pending; }
static void game_note(struct cp_app *a,const char *s)
{ snprintf(a->game_note,sizeof(a->game_note),"%s",s);text(a,G_NOTE,s); }
static void game_inventory(struct cp_app *a)
{
    a->devices_ok=a->host.devices && !a->host.devices(a->host.ctx,&a->devices) && cp_snapshot_valid(&a->devices);
}
static const struct ciuki_device_record *game_port(struct cp_app *a,const char *driver,uint32_t base)
{
    if (a->devices_ok) for (unsigned i=0;i<a->devices.count;i++) {
        const struct ciuki_device_record *d=&a->devices.devices[i];
        if (strcmp(d->driver,driver)) continue;
        for (unsigned r=0;r<d->resource_count;r++)
            if (d->resources[r].type==CIUKI_RESOURCE_IO && d->resources[r].start==base) return d;
    }
    return NULL;
}
static int game_conflict(const struct ciuki_device_record *d)
{ return d && (d->state!=CIUKI_DEVICE_READY || strstr(d->reason,"Owner:") || strstr(d->reason,"Exclusive") || strstr(d->name,"kernel log")); }
static void game_port_label(struct cp_app *a,char *out,size_t n,const char *driver,uint32_t base)
{
    const struct ciuki_device_record *d=game_port(a,driver,base);
    if (d) snprintf(out,n,"%s (%04X)%s",d->name,base,game_conflict(d) ? " - in use/unavailable" : "");
    else snprintf(out,n,"%04X - missing; %s",base,!strcmp(driver,"uart") ? "uses null" : "off at start");
}
static uint32_t game_next_port(struct cp_app *a,const char *driver,uint32_t current,int serial)
{
    uint32_t next=UINT32_MAX,first=UINT32_MAX;
    if (serial && current<GAME_PORTS_NULL) return GAME_PORTS_NULL;
    if (a->devices_ok) for (unsigned i=0;i<a->devices.count;i++) {
        const struct ciuki_device_record *d=&a->devices.devices[i];
        if (strcmp(d->driver,driver)) continue;
        for (unsigned r=0;r<d->resource_count;r++) if (d->resources[r].type==CIUKI_RESOURCE_IO) {
            uint64_t base=d->resources[r].start;
            if (base>=0x100 && base<=0xfff8) {
                if (base<first) first=(uint32_t)base;
                if (base>current && base<next) next=(uint32_t)base;
            }
            break; /* high ECP range is not another port */
        }
    }
    return next!=UINT32_MAX ? next : !serial && first!=UINT32_MAX ? first : 0;
}
static void game_notes(struct cp_app *a)
{
    char note[128]="Changes apply at the next start of this game.";
    for (unsigned i=0;i<5;i++) {
        uint32_t base=i<4 ? a->game_ports.com[i] : a->game_ports.lpt_base;
        if (i<4 ? base<=1 : (a->game_ports.lpt_mode!=GAME_LPT_PRINTER && a->game_ports.lpt_mode!=GAME_LPT_RAW)) continue;
        const struct ciuki_device_record *d=game_port(a,i<4 ? "uart" : "lpt",base);
        if (game_conflict(d)) { snprintf(note,sizeof(note),"%s: %.92s",i<4 ? "COM conflict" : "LPT conflict",d->reason);break; }
        if (!d) snprintf(note,sizeof(note),"Selected port is missing. COM falls back to null; LPT is off at start.");
        for (unsigned j=0;i<4 && j<i;j++) if (base==a->game_ports.com[j])
            snprintf(note,sizeof(note),"Conflict: two guest COM slots select the same physical port.");
    }
    game_note(a,note);
}
static void game_load(struct cp_app *a)
{
    a->game_loaded=a->game_joy_ready=0;a->game_joy_dirty=a->game_save=0;
    game_inventory(a);
    int e=a->host.game_begin ? a->host.game_begin(a->host.ctx,a->joystick_game,NULL) : -ENOSYS;
    a->game_busy=!e;
    game_note(a,e ? "Game properties could not be loaded. Refresh to try again." : "Loading game properties...");
    struct ciuki_joystick_control c={.version=1,.size=sizeof(c),.operation=CIUKI_JOY_GET_GAME,.axis=a->joystick_game};
    e=a->host.joystick ? a->host.joystick(a->host.ctx,&c) : -ENODEV;
    a->joystick_busy=!e;
    if (e) game_note(a,"Joystick properties unavailable. Refresh to try again.");
}
void cp_game_open(struct cp_app *a,unsigned game)
{
    if (!game || game>DESK_GAME_COUNT || a->joystick_busy || a->game_busy || a->joystick_save_pending) return;
    a->page=CP_JOYSTICK;a->joystick_game=game;a->game_open=1;game_load(a);build(a);
}
static void game_poll(struct cp_app *a)
{
    if (a->building || !a->game_busy || !a->host.game_poll) return;
    struct cp_game_result r;
    if (a->host.game_poll(a->host.ctx,&r)<=0) return;
    a->game_busy=0;
    if (r.game!=a->joystick_game) return;
    if (r.error) { a->game_save=0;game_note(a,"Port settings could not be saved or read. Refresh or try Apply again."); }
    else {
        a->game_ports=r.ports;a->game_loaded=1;
        if (!r.saved) {
            if (r.rejected) game_note(a,"Corrupt port settings rejected. Using previous settings or defaults.");
            else game_notes(a);
        }
        else if (a->game_joy_dirty) {
            const struct ciuki_joystick_control *j=&a->game_joystick;
            struct ciuki_joystick_control c={.version=1,.size=sizeof(c),.operation=CIUKI_JOY_SAVE_GAME,.axis=a->joystick_game,
                .device=j->device,.generation=j->generation,.second_device=j->second_device,
                .second_generation=j->second_generation,.mode=j->mode,.session=j->session};
            int e=a->host.joystick_save_game ? a->host.joystick_save_game(a->host.ctx,a->joystick_game,&c) : -ENODEV;
            if (!e) a->joystick_busy=1;
            else { a->game_save=0;game_note(a,"Ports saved; joystick change was refused. Reconnect and try Apply again."); }
        } else { a->game_save=0;game_note(a,"Game properties saved. Changes apply at the next start."); }
    }
    if (a->game_open && a->page==CP_JOYSTICK) build(a);
}
static int game_joystick_result(struct cp_app *a,const struct ciuki_joystick_control *c,int error)
{
    if (!a->game_open) return 0;
    if (c->operation==CIUKI_JOY_GET_GAME) {
        if (!error) {
            a->game_joystick=(struct ciuki_joystick_control){.version=1,.size=sizeof(*c),.device=c->device,.generation=c->generation,
                .second_device=c->second_device,.second_generation=c->second_generation,.mode=c->mode,.session=c->session,.attached=c->attached};
            a->game_joy_ready=1;
        } else game_note(a,"Joystick properties unavailable. Refresh to try again.");
    } else if (c->operation==CIUKI_JOY_NEXT) {
        a->game_joystick=(struct ciuki_joystick_control){.version=1,.size=sizeof(*c)};
        if (!error) {
            a->game_joystick.device=c->device;a->game_joystick.generation=c->generation;
            a->game_joystick.session=c->session;a->game_joystick.attached=2;
        } else if (error!=-ENODEV) { game_note(a,"Controller discovery failed. Refresh to try again."); }
        a->game_joy_dirty=1;
    } else if (c->operation==CIUKI_JOY_SAVE_GAME) {
        if (error) { a->game_save=0;game_note(a,"Ports saved; joystick change failed. Try Apply again."); }
        else { a->joystick_save_pending=1;game_note(a,"Saving the joystick choice..."); }
    } else if (c->operation==CIUKI_JOY_SAVE_STATUS) {
        if (c->minimum) { a->joystick_save_pending=0;a->game_save=0;game_note(a,"Ports saved; joystick storage failed. Try Apply again."); }
        else if (!c->buttons) { a->joystick_save_pending=0;a->game_save=0;a->game_joy_dirty=0;
            game_note(a,"Game properties saved. Changes apply at the next start."); }
    } else return 0;
    if (a->page==CP_JOYSTICK) build(a);
    return 1;
}
static int game_read_path(struct cp_app *a)
{
    struct du_widget *w=du_get(a->ui,G_PATH);if (!w) return 1;
    if (strlen(w->text)>=sizeof(a->game_ports.path)) { game_note(a,"The output path is too long.");return 0; }
    memset(a->game_ports.path,0,sizeof(a->game_ports.path));strcpy(a->game_ports.path,w->text);return 1;
}
static int game_event(struct cp_app *a,const struct du_event *e)
{
    if (!a->game_open || a->page!=CP_JOYSTICK || e->id<G_GAME || e->id>G_DOS) return 0;
    if (e->type!=DU_EV_ACTIVATE) return 1;
    if (e->id==G_REFRESH && !a->game_busy && !a->joystick_busy && !a->joystick_save_pending) { game_load(a);build(a);return 1; }
    if (e->id==G_BACK && !a->game_busy && !a->joystick_busy && !a->joystick_save_pending) { a->game_open=0;build(a);return 1; }
    if (!game_ready(a) || !game_read_path(a)) return 1;
    if (e->id==G_GAME) { cp_game_open(a,a->joystick_game%DESK_GAME_COUNT+1);return 1; }
    if (e->id==G_BACK) { a->game_open=0;build(a);return 1; }
    if (e->id==G_JOY) {
        struct ciuki_joystick_control *j=&a->game_joystick;
        if (!j->attached) { *j=(struct ciuki_joystick_control){.version=1,.size=sizeof(*j),.device=CIUKI_JOY_DISABLED,.attached=1};a->game_joy_dirty=1; }
        else {
            struct ciuki_joystick_control c={.version=1,.size=sizeof(c),.operation=CIUKI_JOY_NEXT,
                .device=j->attached==1 ? 0 : j->device};
            int error=a->host.joystick ? a->host.joystick(a->host.ctx,&c) : -ENODEV;
            if (!error) a->joystick_busy=1;else game_note(a,"Controller discovery unavailable.");
        }
    } else if (e->id==G_DOS) {
        if (a->joystick_dos.device) { a->game_joystick=a->joystick_dos;a->game_joystick.attached=2;a->game_joy_dirty=1; }
        else game_note(a,"Choose DOS stick A/B in Game controllers first.");
    } else if (e->id>=G_COM1 && e->id<=G_COM4) {
        unsigned slot=(unsigned)(e->id-G_COM1);a->game_ports.com[slot]=game_next_port(a,"uart",a->game_ports.com[slot],1);game_notes(a);
    } else if (e->id==G_LPT) { a->game_ports.lpt_mode=(a->game_ports.lpt_mode+1)%4;
        if (a->game_ports.lpt_mode==GAME_LPT_FILE && !a->game_ports.path[0])
            snprintf(a->game_ports.path,sizeof(a->game_ports.path),"/Documents/print-game-%u.prn",a->joystick_game);
        game_notes(a);
    } else if (e->id==G_PORT) { a->game_ports.lpt_base=game_next_port(a,"lpt",a->game_ports.lpt_base,0);game_notes(a); }
    else if (e->id==G_APPLY) {
        if (!game_ports_valid(&a->game_ports)) { game_note(a,"Choose a port or a file under /Documents without dot or parent components.");return 1; }
        int error=a->host.game_begin ? a->host.game_begin(a->host.ctx,a->joystick_game,&a->game_ports) : -ENOSYS;
        if (!error) { a->game_busy=1;a->game_save=1;game_note(a,"Saving game properties..."); }
        else game_note(a,"Saving could not start. Try Apply again.");
    }
    build(a);return 1;
}
static void game_build(struct cp_app *a)
{
    a->tab_items[0].text[0]="Game properties";
    char label[128];snprintf(label,sizeof(label),"Game: %s",desk_games[a->joystick_game-1].title);
    add(a,DU_BUTTON,G_GAME,24,44,350,24,label);
    add(a,DU_BUTTON,G_BACK,382,44,150,24,"Game controllers");
    const struct ciuki_joystick_control *j=&a->game_joystick;
    if (!a->game_joy_ready) snprintf(label,sizeof(label),"Joystick: loading...");
    else if (!j->attached) snprintf(label,sizeof(label),"Joystick: DOS default");
    else if (j->attached==1) snprintf(label,sizeof(label),"Joystick: none");
    else if (!j->device) snprintf(label,sizeof(label),"Joystick: none (saved controller missing)");
    else snprintf(label,sizeof(label),"Joystick: %s %u%s",j->device>=CIUKI_GAMEPORT_FIRST ? "analog" : "USB",j->device,j->second_device ? " + stick B" : "");
    add(a,DU_BUTTON,G_JOY,24,80,350,24,label);
    add(a,DU_BUTTON,G_DOS,382,80,150,24,"Use DOS selection");
    add(a,DU_LABEL,G_JOY_NOTE,24,108,508,20,j->attached==3 ? "Saved controller missing; first available joystick (or none) at start." : "Click to cycle default, none and connected controllers.");
    for (unsigned i=0;i<4;i++) {
        uint32_t base=a->game_ports.com[i];char target[100];
        if (base<=1) snprintf(target,sizeof(target),"%s",base ? "Null device" : "None");
        else game_port_label(a,target,sizeof(target),"uart",base);
        snprintf(label,sizeof(label),"COM%u: %s",i+1,target);
        add(a,DU_BUTTON,G_COM1+(int)i,24,134+(int)i*28,508,24,label);
    }
    static const char *const modes[]={"None","Native print spooler","Raw physical lease","INT 17h output file"};
    snprintf(label,sizeof(label),"LPT1: %s",modes[a->game_ports.lpt_mode<=3 ? a->game_ports.lpt_mode : 0]);
    add(a,DU_BUTTON,G_LPT,24,250,260,24,label);
    game_port_label(a,label,sizeof(label),"lpt",a->game_ports.lpt_base);
    add(a,DU_BUTTON,G_PORT,292,250,240,24,label)->disabled=a->game_ports.lpt_mode!=GAME_LPT_RAW;
    add(a,DU_EDIT,G_PATH,24,280,508,24,a->game_ports.path)->disabled=a->game_ports.lpt_mode!=GAME_LPT_FILE;
    add(a,DU_LABEL,G_NOTE,24,310,508,38,a->game_note);
    add(a,DU_BUTTON,G_APPLY,352,CP_HEIGHT-34,90,24,"Apply")->anchors=DU_ANCHOR_RIGHT|DU_ANCHOR_BOTTOM;
    add(a,DU_BUTTON,G_REFRESH,248,CP_HEIGHT-34,96,24,"Refresh")->anchors=DU_ANCHOR_RIGHT|DU_ANCHOR_BOTTOM;
    if (!game_ready(a)) for (unsigned i=0;i<a->ui->count;i++) {
        struct du_widget *w=&a->ui->widgets[i];
        if (w->id>=G_GAME && w->id<=G_DOS) w->disabled=1;
    }
    du_get(a->ui,G_BACK)->disabled=a->game_busy || a->joystick_busy || a->joystick_save_pending;
    du_get(a->ui,G_REFRESH)->disabled=a->game_busy || a->joystick_busy || a->joystick_save_pending;
}
