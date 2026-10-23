/* Production per-game panel, copied async requests, durable ports and raster.
 * SPDX-License-Identifier: MIT */
#include "control.h"
#include "../../../apps/dosbox/port_settings.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct cp_app app;
static struct du_ui ui;
static uint32_t pixels[CP_WIDTH*CP_HEIGHT];
static struct cp_game_result pending;
static int io_pending,io_hold,io_error,joy_pending,joy_writes,joy_storage_pending,joy_error;
static struct ciuki_joystick_control joy_request,saved_joy[4];
static struct ciuki_devices_info inventory;
static int devices(void *ctx,struct ciuki_devices_info *out) { (void)ctx;*out=inventory;return 0; }
static int begin(void *ctx,unsigned game,const struct game_ports_settings *p)
{
    (void)ctx;assert(!io_pending);pending=(struct cp_game_result){.game=game,.saved=p!=NULL};
    if (p) pending.ports=*p;io_pending=1;return 0;
}
static int poll_io(void *ctx,struct cp_game_result *r)
{
    (void)ctx;if (!io_pending || io_hold) return 0;io_pending=0;
    pending.error=io_error ? -EIO : pending.saved ? dosbox_ports_save(pending.game,&pending.ports) :
        dosbox_ports_load(pending.game,&pending.ports,&pending.rejected);*r=pending;return 1;
}
static int joystick(void *ctx,const struct ciuki_joystick_control *c)
{ (void)ctx;assert(!joy_pending);joy_pending=1;joy_request=*c;return 0; }
static int save_joystick(void *ctx,unsigned game,const struct ciuki_joystick_control *c)
{
    assert(game && game<=3 && c->axis==game && c->operation==6 && !c->attached && !c->buttons);
    saved_joy[game]=*c;joy_writes++;return joystick(ctx,c);
}
static void deliver(void)
{
    assert(joy_pending);joy_pending=0;struct ciuki_joystick_control c=joy_request;
    if (c.operation==CIUKI_JOY_GET_GAME) {
        unsigned game=c.axis;struct ciuki_joystick_control s=saved_joy[game];c=s;c.version=1;c.size=sizeof(c);c.operation=8;c.axis=game;
    } else if (c.operation==CIUKI_JOY_NEXT || c.operation==CIUKI_JOY_QUERY) {
        if (c.device>1) { cp_joystick_result(&app,&c,-ENODEV);return; }
        c.device=257;c.generation=12;c.axes=15;c.attached=1;c.session=44;
    } else if (c.operation==CIUKI_JOY_SAVE_GAME) joy_storage_pending=1;
    else if (c.operation==CIUKI_JOY_SAVE_STATUS) c.buttons=(unsigned)joy_storage_pending;
    cp_joystick_result(&app,&c,joy_error);
}
static void click(int id)
{
    struct du_widget *w=du_get(&ui,id);assert(w && !w->disabled);
    struct ciuki_input_event e={.type=CIUKI_INPUT_MOTION,.value=w->box.x+w->box.w/2,.value2=w->box.y+w->box.h/2};du_input(&ui,&e);
    e.type=CIUKI_INPUT_BUTTON;e.code=CIUKI_BUTTON_LEFT;e.value=1;du_input(&ui,&e);e.value=0;du_input(&ui,&e);
}
static void frame(const char *dir,const char *name)
{
    du_render(&ui);char path[512];snprintf(path,sizeof(path),"%s/%s.xrgb",dir,name);
    FILE *f=fopen(path,"wb");assert(f);assert(fwrite(pixels,4,CP_WIDTH*CP_HEIGHT,f)==CP_WIDTH*CP_HEIGHT);assert(!fclose(f));
    for (unsigned i=0;i<ui.count;i++) for (unsigned j=i+1;j<ui.count;j++) assert(ui.widgets[i].id!=ui.widgets[j].id);
}
static void open_game(unsigned game)
{ if (joy_pending) deliver();cp_game_open(&app,game);assert(app.game_busy && joy_pending);deliver();cp_tick(&app);assert(app.game_loaded && app.game_joy_ready && !app.game_busy); }
static void port(unsigned i,const char *name,const char *driver,uint32_t base,const char *why)
{
    struct ciuki_device_record *d=&inventory.devices[i];*d=(struct ciuki_device_record){.id=i+1,.class_id=CIUKI_DEVICE_PORT,.state=CIUKI_DEVICE_READY,.resource_count=1};
    snprintf(d->name,sizeof(d->name),"%s",name);snprintf(d->driver,sizeof(d->driver),"%s",driver);snprintf(d->reason,sizeof(d->reason),"%s",why);
    d->resources[0]=(struct ciuki_device_resource){.type=CIUKI_RESOURCE_IO,.start=base,.end=base+3};
}
int main(int argc,char **argv)
{
    assert(argc==2);assert(!mkdir(DOSBOX_SETTINGS_DIR,0700));
    struct game_ports_settings schema,decoded;game_ports_defaults(&schema);schema.sequence=1;
    char encoded[GAME_PORTS_TEXT_MAX];size_t bytes=game_ports_encode(&schema,encoded);
    assert(game_ports_decode(encoded,bytes,&decoded) && !memcmp(&schema,&decoded,sizeof(schema)));
    assert(!game_ports_decode(encoded,bytes-1,&decoded) && !game_ports_decode(encoded,bytes+1,&decoded));
    encoded[40]^=1;assert(!game_ports_decode(encoded,bytes,&decoded));
    schema.version=2;bytes=game_ports_encode(&schema,encoded);assert(!game_ports_decode(encoded,bytes,&decoded));
    inventory=(struct ciuki_devices_info){.version=CIUKI_DEVICES_VERSION,.size=sizeof(inventory),.count=3};
    port(0,"COM1","uart",0x3f8,"Available");port(1,"COM2","uart",0x2f8,"Owner: Native application id=17 lease=9");port(2,"LPT1","lpt",0x378,"Available");
    du_init(&ui,pixels,CP_WIDTH,CP_HEIGHT);ui.focused_window=1;
    app.host=(struct cp_host){.game_begin=begin,.game_poll=poll_io,.joystick=joystick,.joystick_save_game=save_joystick,.devices=devices};
    assert(!cp_attach(&app,&ui,CP_HOME));io_hold=1;cp_game_open(&app,1);deliver();frame(argv[1],"game-loading");
    assert(du_get(&ui,1509)->disabled);io_hold=0;cp_tick(&app);frame(argv[1],"game-defaults");
    assert(app.game_ports.com[0]==1 && !app.game_ports.lpt_mode && !joy_writes);
    click(1502);assert(app.game_ports.com[0]==0x2f8 && strstr(app.game_note,"Native application"));frame(argv[1],"game-conflict");
    click(1502);assert(app.game_ports.com[0]==0x3f8);click(1503);assert(app.game_ports.com[1]==0x2f8);click(1503);assert(app.game_ports.com[1]==0x3f8 && strstr(app.game_note,"two guest"));
    click(1503);assert(!app.game_ports.com[1]);
    click(1501);assert(app.game_joystick.device==CIUKI_JOY_DISABLED);click(1501);deliver();assert(app.game_joystick.device==257);
    click(1506);assert(app.game_ports.lpt_mode==1);click(1506);assert(app.game_ports.lpt_mode==2);frame(argv[1],"game-raw");
    click(1506);assert(app.game_ports.lpt_mode==3);du_set_text(du_get(&ui,1508),"/Documents/output.prn");frame(argv[1],"game-file");
    click(1509);assert(io_pending && app.game_busy && !joy_writes);
    assert(!cp_close(&app) && !app.quit);cp_open(&app,CP_HOME);assert(app.page==CP_JOYSTICK);cp_tick(&app);assert(joy_writes==1 && joy_pending);deliver();
    cp_tick(&app);assert(joy_pending);deliver();assert(app.joystick_save_pending);joy_storage_pending=0;cp_tick(&app);deliver();
    assert(!app.joystick_save_pending && strstr(app.game_note,"next start"));frame(argv[1],"game-saved");
    struct game_ports_settings stored;int rejected;assert(!dosbox_ports_load(1,&stored,&rejected) && stored.com[0]==0x3f8 && stored.com[1]==0 && stored.lpt_mode==3 && !strcmp(stored.path,"/Documents/output.prn"));
    /* Reopen reads stored values, absent devices stay visibly bound to their identity. */
    inventory.count=0;saved_joy[1].attached=3;open_game(1);
    assert(app.game_joystick.attached==3 && strstr(du_get(&ui,1502)->text,"missing"));frame(argv[1],"game-missing");
    click(1509);cp_tick(&app);assert(joy_writes==1); /* never persist the fallback as a new joystick choice */
    open_game(2);assert(app.game_ports.com[0]==1 && !app.game_ports.lpt_mode && !app.game_joystick.attached);frame(argv[1],"game-second");
    click(1502);click(1510);assert(!app.game_open);open_game(2);assert(app.game_ports.com[0]==1); /* staged edit discarded */
    io_error=1;click(1509);cp_tick(&app);assert(!app.game_busy && strstr(app.game_note,"could not"));frame(argv[1],"game-save-error");io_error=0;
    click(1510);cp_detach(&app);
    /* Byte-level persistence rejects checksum/version/oversize/path corruption. */
    stored.lpt_mode=3;strcpy(stored.path,"/Documents/../system/BOOT.CFG");assert(dosbox_ports_save(2,&stored)==-EINVAL);
    game_ports_defaults(&stored);assert(!dosbox_ports_save(3,&stored));stored.com[0]=0;assert(!dosbox_ports_save(3,&stored));
    char path[160];assert(!dosbox_ports_path(3,0,path));FILE *f=fopen(path,"wb");assert(f);assert(fwrite("torn",1,4,f)==4);assert(!fclose(f));
    assert(!dosbox_ports_load(3,&stored,&rejected) && rejected && stored.sequence==1 && stored.com[0]==1);
    puts("PASS game properties: async loading, per-game joystick/COM/LPT controls, owner/duplicate conflicts, staged edits, durable round trip, missing identity preserved, next-start notice, save failure and nine golden frames");
}
