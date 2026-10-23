/* Common analog/USB calibration and DOS subscriptions. SPDX-License-Identifier: MIT */
#include "../desktop/catalog.h"
enum { J_NEXT=1100, J_CALIBRATE, J_CAPTURE, J_ROUTE, J_OFF, J_MODE, J_GAME, J_SAVE_GAME,
       J_NAME, J_POSITION, J_INSTRUCTIONS, J_SECOND, J_TWO, J_DEFAULT_GAME, J_GAME_OFF };
static struct ciuki_joystick_control joystick_request_record(struct cp_app *a, unsigned op)
{
    return (struct ciuki_joystick_control){.version=CIUKI_JOYSTICK_VERSION,.size=sizeof(a->joystick),
        .device=a->joystick.device,.generation=a->joystick.generation,.operation=op,
        .session=op==CIUKI_JOY_ROUTE ? a->joystick.session : 0};
}
static int joystick_send(struct cp_app *a, struct ciuki_joystick_control *c)
{
    if (a->joystick_busy) return -EBUSY;
    int e=a->host.joystick ? a->host.joystick(a->host.ctx,c) : -ENODEV;
    if (!e) a->joystick_busy=1;
    else text(a,C_STATUS,"The desktop joystick service is unavailable.");
    return e;
}
static void joystick_capture(struct cp_app *a);
static void joystick_query(struct cp_app *a, unsigned op)
{
    if (a->joystick_busy || a->joystick_calibrating==2) return;
    if (!a->joystick.device && op==CIUKI_JOY_QUERY) op=CIUKI_JOY_NEXT;
    struct ciuki_joystick_control c=joystick_request_record(a,op);
    if (op==CIUKI_JOY_NEXT) c.generation=0;
    (void)joystick_send(a,&c);
}
static void joystick_axis_calibrate(struct cp_app *a)
{
    while (a->joystick_axis<4 && !(a->joystick.axes&(1u<<a->joystick_axis))) a->joystick_axis++;
    if (a->joystick_axis==4) {
        a->joystick_calibrating=0; a->joystick_save_pending=1;
        text(a,C_STATUS,"Calibration applied. Saving controller settings..."); return;
    }
    unsigned axis=a->joystick_axis;
    struct ciuki_joystick_control c=joystick_request_record(a,CIUKI_JOY_CALIBRATE);
    c.axis=axis; c.minimum=a->joystick_points[axis][0];
    c.centre=a->joystick_points[axis][1]; c.maximum=a->joystick_points[axis][2];
    (void)joystick_send(a,&c);
}
void cp_joystick_result(struct cp_app *a, const struct ciuki_joystick_control *c, int error)
{
    a->joystick_busy=0;
    if (error) {
        a->joystick_calibrating=0; a->joystick_capture_pending=0;
        if (c->operation==CIUKI_JOY_NEXT) { a->joystick.device=0; a->joystick.generation=0; }
        text(a,C_STATUS,error==-ENODEV ? "No matching controller. Next restarts discovery." : "Change refused. Check connection and calibration points.");
        return;
    }
    if (c->operation==CIUKI_JOY_SAVE_STATUS) {
        if (c->minimum) {
            a->joystick_save_pending=0;
            text(a,C_STATUS,"Controller settings could not be saved. Try the change again.");
        } else if (!c->buttons) {
            a->joystick_save_pending=0;
            text(a,C_STATUS,"Controller settings saved for future sessions.");
        }
        return;
    }
    if (c->operation==CIUKI_JOY_SAVE_GAME) {
        a->joystick_save_pending=1;
        text(a,C_STATUS,"Saving game joystick properties..."); return;
    }
    if (c->operation==CIUKI_JOY_QUERY || c->operation==CIUKI_JOY_NEXT) {
        if (c->device!=a->joystick.device || c->generation!=a->joystick.generation) a->joystick_calibrating=0;
        a->joystick=*c;
        unsigned count=0; for (unsigned axis=0;axis<CIUKI_HID_AXES;axis++) count+=!!(c->axes&(1u<<axis));
        snprintf(a->joystick_line,sizeof(a->joystick_line),"%s controller %u (%u axes)",
            c->device>=CIUKI_GAMEPORT_FIRST ? "Analog" : "USB",c->device,count);
        text(a,J_NAME,a->joystick_line);
        snprintf(a->joystick_line,sizeof(a->joystick_line),"X %d   Y %d   Z %d   Rx %d   Buttons: %c %c %c %c",
            c->position[0],c->position[1],c->position[2],c->position[3],
            c->buttons&1 ? '*' : '-',c->buttons&2 ? '*' : '-',c->buttons&4 ? '*' : '-',c->buttons&8 ? '*' : '-');
        text(a,J_POSITION,a->joystick_line);
        if (a->joystick_capture_pending) { a->joystick_capture_pending=0; joystick_capture(a); }
    } else if (c->operation==CIUKI_JOY_CALIBRATE && a->joystick_calibrating==2) {
        a->joystick_axis++; joystick_axis_calibrate(a);
    } else if (c->operation==CIUKI_JOY_CONFIGURE) {
        a->joystick_save_pending=1;
        a->joystick.device=0; a->joystick.generation=0;
        text(a,C_STATUS,"Wiring changed. Next discovers the new generation.");
    } else {
        if (c->operation==CIUKI_JOY_ROUTE) { a->joystick_dos=*c; a->joystick_save_pending=1; }
        text(a,C_STATUS,"DOS default updated. Saving controller settings...");
    }
}
static void joystick_event(struct cp_app *a, int id)
{
    if (id==J_NEXT) { a->joystick_calibrating=0; joystick_query(a,CIUKI_JOY_NEXT); return; }
    if (id==J_GAME) {
        a->joystick_game=(a->joystick_game+1)%(DESK_GAME_COUNT+1);
        char label[96]; snprintf(label,sizeof(label),"Game properties: %s",a->joystick_game ? desk_games[a->joystick_game-1].title : "select a game");
        text(a,J_GAME,label); return;
    }
    struct ciuki_joystick_control c=joystick_request_record(a,CIUKI_JOY_ROUTE);
    if (id==J_OFF || id==J_DEFAULT_GAME || id==J_GAME_OFF) c.device=c.generation=c.session=0;
    else if (!c.device) { text(a,C_STATUS,"Select a connected controller first."); return; }
    if (id==J_SECOND) {
        if (!a->joystick_dos.device) { text(a,C_STATUS,"Choose joystick A first, then select joystick B."); return; }
        c=a->joystick_dos; c.mode=CIUKI_JOY_TWO_STICKS;
        c.second_device=a->joystick.device; c.second_generation=a->joystick.generation;
        (void)joystick_send(a,&c); return;
    }
    if (id==J_ROUTE || id==J_OFF) { (void)joystick_send(a,&c); return; }
    if (id==J_SAVE_GAME || id==J_DEFAULT_GAME || id==J_GAME_OFF) {
        if (id==J_SAVE_GAME && a->joystick_dos.device) c=a->joystick_dos;
        if (id==J_GAME_OFF) c.device=CIUKI_JOY_DISABLED;
        if (!a->joystick_game) { text(a,C_STATUS,"Select the game whose properties you want to save."); return; }
        int e=a->host.joystick_save_game ? a->host.joystick_save_game(a->host.ctx,a->joystick_game,&c) : -ENOSYS;
        if (!e) a->joystick_busy=1;
        text(a,C_STATUS,e ? "Game properties could not be saved." : "Saving game joystick properties..."); return;
    }
    if (id==J_MODE) { c.operation=CIUKI_JOY_CONFIGURE; c.mode=!a->joystick.mode; (void)joystick_send(a,&c); return; }
    if (id==J_CALIBRATE) {
        if (a->joystick_busy) return;
        a->joystick_calibrating=1; a->joystick_step=0;
        text(a,J_INSTRUCTIONS,"Move all axes to minimum, then Capture. Next: centre, then maximum."); return;
    }
    if (id==J_CAPTURE && a->joystick_calibrating==1 && !a->joystick_busy) {
        a->joystick_capture_pending=1; joystick_query(a,CIUKI_JOY_QUERY);
    }
}
static void joystick_capture(struct cp_app *a)
{
    if (a->joystick_calibrating!=1) return;
    for (unsigned i=0;i<4;i++) a->joystick_points[i][a->joystick_step]=a->joystick.raw[i];
    if (++a->joystick_step==3) {
        /* Validate every axis before changing any calibration. */
        for (unsigned i=0;i<4;i++) if ((a->joystick.axes&(1u<<i)) &&
            (a->joystick_points[i][0]>=a->joystick_points[i][1] || a->joystick_points[i][1]>=a->joystick_points[i][2])) {
            a->joystick_calibrating=0; text(a,C_STATUS,"Invalid points. Start again with minimum < centre < maximum."); return;
        }
        a->joystick_calibrating=2; a->joystick_axis=0; joystick_axis_calibrate(a);
    } else text(a,J_INSTRUCTIONS,a->joystick_step==1 ? "Release axes to their centre, then Capture." : "Move all axes to maximum, then Capture.");
}

static void joystick_build(struct cp_app *a)
{
    add(a,DU_LABEL,J_NAME,24,44,500,20,"Select Next to discover a game controller.");
    add(a,DU_LABEL,J_POSITION,24,70,510,20,"");
    add(a,DU_BUTTON,J_NEXT,24,100,100,24,"Next");
    add(a,DU_BUTTON,J_CALIBRATE,132,100,110,24,"Calibrate");
    add(a,DU_BUTTON,J_CAPTURE,250,100,100,24,"Capture");
    add(a,DU_BUTTON,J_MODE,358,100,160,24,"Analog wiring 1/2 sticks");
    add(a,DU_LABEL,J_INSTRUCTIONS,24,136,510,36,"Three-point calibration applies to the selected USB or analog controller.");
    add(a,DU_BUTTON,J_ROUTE,24,184,150,24,"Use as DOS stick A");
    add(a,DU_BUTTON,J_SECOND,182,184,150,24,"Use as DOS stick B");
    add(a,DU_BUTTON,J_OFF,340,184,164,24,"Disable DOS input");
    add(a,DU_BUTTON,J_GAME,24,230,320,24,"Game properties: select a game");
    add(a,DU_BUTTON,J_SAVE_GAME,24,264,230,24,"Save DOS selection for game");
    add(a,DU_BUTTON,J_DEFAULT_GAME,264,264,240,24,"Use DOS default for game");
    add(a,DU_BUTTON,J_GAME_OFF,24,300,230,24,"Disable joystick for game");
    joystick_query(a,CIUKI_JOY_QUERY);
}
