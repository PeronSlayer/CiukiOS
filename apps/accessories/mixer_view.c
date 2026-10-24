/* SPDX-License-Identifier: MIT
 * Volume Control window (Windows 98 "Volume Control" master column). */
#include "mixer.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

void mixer_split(int volume, int balance, uint32_t *left, uint32_t *right)
{
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    if (balance < -100) balance = -100;
    if (balance > 100) balance = 100;
    *left = (uint32_t)(balance > 0 ? volume * (100 - balance) / 100 : volume);
    *right = (uint32_t)(balance < 0 ? volume * (100 + balance) / 100 : volume);
}
void mixer_join(uint32_t left, uint32_t right, int *volume, int *balance)
{
    uint32_t v = left > right ? left : right;
    *volume = (int)v;
    if (!v || left == right) *balance = 0;
    else if (right > left) *balance = 100 - (int)(left * 100 / v);
    else *balance = -(100 - (int)(right * 100 / v));
}

static const char *const line_names[]={"Master","Wave","MIDI","CD Audio","Line In","Microphone"};
static int line_id(unsigned line,unsigned kind) { return MX_LINE_BASE+(int)line*10+(int)kind; }
static void lines_show(struct mixer_app *a)
{
    for (unsigned i=1;i<=5;i++) {
        struct ciuki_mixer_line *l=&a->lines[i];
        struct du_widget *v=du_get(a->ui,line_id(i,0)), *b=du_get(a->ui,line_id(i,1));
        struct du_widget *m=du_get(a->ui,line_id(i,2)), *label=du_get(a->ui,line_id(i,3));
        if (!v || !b || !m || !label) continue;
        int changed=v->value!=100-a->line_volume[i] || b->value!=a->line_balance[i]+100 ||
            v->disabled!=!l->available;
        v->value=100-a->line_volume[i]; b->value=a->line_balance[i]+100;
        v->disabled=m->disabled=!l->available;
        b->disabled=!l->available || a->recording || i==CIUKI_MIXER_MIC;
        char text[64];
        snprintf(text,sizeof(text),a->recording ? (l->selected ? "[x] Select" : "[ ] Select") :
                 (l->mute ? "[x] Mute" : "[ ] Mute"));
        changed|=strcmp(m->text,text)!=0; du_set_text(m,text);
        snprintf(text,sizeof(text),l->available ? "%d%%" : "Unavailable",a->line_volume[i]);
        changed|=strcmp(label->text,text)!=0; du_set_text(label,text);
        struct du_widget *group=du_get(a->ui,line_id(i,4));
        const char *name=a->recording && i==CIUKI_MIXER_WAVE ? "Stereo Mix" : line_names[i];
        if (group && strcmp(group->text,name)) { changed=1; du_set_text(group,name); }
        if (changed) du_invalidate(a->ui,(struct du_rect){108+(int)(i-1)*102,58,102,230});
    }
    struct du_widget *mode=du_get(a->ui,MX_MODE);
    const char *caption=a->recording ? "&Playback controls" : "&Recording controls";
    if (mode && strcmp(mode->text,caption)) { du_set_text(mode,caption); du_invalidate(a->ui,mode->box); }
}
static void lines_refresh(struct mixer_app *a)
{
    for (unsigned i=1;i<=5;i++) {
        if (a->line_pending & (1u<<i)) continue;
        struct ciuki_mixer_line l={.version=1,.size=sizeof(l),.line=i,.recording=(unsigned)a->recording};
        int e=a->backend && a->backend->line_get ? a->backend->line_get(a->backend->ctx,&l) : -ENODEV;
        if (e) l.available=0;
        a->lines[i]=l; mixer_join(l.left,l.right,&a->line_volume[i],&a->line_balance[i]);
    }
    lines_show(a);
}
static void lines_apply(struct mixer_app *a)
{
    for (unsigned i=1;i<=5;i++) if (a->line_pending & (1u<<i)) {
        struct ciuki_mixer_line l=a->lines[i];
        mixer_split(a->line_volume[i],a->line_balance[i],&l.left,&l.right);
        l.available=l.reserved=0;
        int e=a->backend && a->backend->line_set ? a->backend->line_set(a->backend->ctx,&l) : -ENODEV;
        if (e) snprintf(a->status,sizeof(a->status),"The %s control could not be changed (error %d)",line_names[i],e);
    }
    a->line_pending=0;
}
static void show(struct mixer_app *a)
{
    struct du_ui *u=a->ui;
    for (unsigned i=0;i<u->count;i++) {
        struct du_widget *w=&u->widgets[i];
        int value=w->value,disabled=w->disabled; char text[sizeof(w->text)];
        snprintf(text,sizeof(text),"%s",w->text);
        if (w->id==MX_VOLUME) value=100-a->volume;
        if (w->id==MX_BALANCE) value=a->balance+100;
        if (w->id==MX_MUTE) snprintf(text,sizeof(text),a->mute ? "[x] &Mute" : "[ ] &Mute");
        if (w->id==MX_LEVEL) {
            uint32_t l,r; mixer_split(a->volume,a->balance,&l,&r);
            if (l==r) snprintf(text,sizeof(text),"%d%%",a->volume);
            else snprintf(text,sizeof(text),"L %u  R %u",(unsigned)l,(unsigned)r);
        }
        if (w->id>=MX_VOLUME && w->id<=MX_MUTE) disabled=!a->available || a->recording;
        if (w->id==MX_STATUS) snprintf(text,sizeof(text),"%s",a->status);
        if (value!=w->value || disabled!=w->disabled || strcmp(text,w->text)) {
            w->value=value; w->disabled=(uint8_t)disabled; du_set_text(w,text); du_invalidate(u,w->box);
        }
    }
}
static void describe(struct mixer_app *a)
{
    if (!a->available) snprintf(a->status, sizeof(a->status), "No mixer: /dev/mixer is unavailable (error %d)", a->error);
    else if (a->waiting || a->pending) snprintf(a->status, sizeof(a->status), "Applying...");
    else if (a->master.save_state == CIUKI_MIXER_SAVED) snprintf(a->status, sizeof(a->status), "Saved in C:\\SYSTEM\\AUDIO.CFG");
    /* fx-9b: a volume-key change is applied at once and saved by the desktop
     * once the keys are quiet (error 0). */
    else if (a->master.save_state == CIUKI_MIXER_UNSAVED && !a->master.save_error) snprintf(a->status, sizeof(a->status), "Applied by the volume keys; saved shortly");
    else if (a->master.save_state == CIUKI_MIXER_UNSAVED) snprintf(a->status, sizeof(a->status), "Applied, not saved (error %d)", (int)a->master.save_error);
    else snprintf(a->status, sizeof(a->status), "Default setting (nothing saved yet)");
}
void mixer_refresh(struct mixer_app *a)
{
    struct ciuki_mixer_master m;
    memset(&m, 0, sizeof(m)); m.version = 1; m.size = sizeof(m);
    int err = a->backend && a->backend->get ? a->backend->get(a->backend->ctx, &m) : -ENODEV;
    a->available = !err; a->error = err;
    if (!err) {
        a->master = m;
        if (!a->pending && !a->waiting) { mixer_join(m.left, m.right, &a->volume, &a->balance); a->mute = m.mute != 0; }
    }
    describe(a); show(a); lines_refresh(a);
}
static void apply(struct mixer_app *a)
{
    uint32_t l, r; mixer_split(a->volume, a->balance, &l, &r);
    a->pending = 0;
    int err = a->backend && a->backend->set ? a->backend->set(a->backend->ctx, l, r, (uint32_t)a->mute) : -ENODEV;
    if (err) { snprintf(a->status, sizeof(a->status), "The volume could not be changed (error %d)", err); show(a); return; }
    a->waiting = 1; describe(a); show(a);
}
void mixer_result(struct mixer_app *a, int result)
{
    a->waiting = 0;
    if (result) {
        mixer_refresh(a);
        snprintf(a->status, sizeof(a->status), result == -EPERM ? "Only the desktop can change the volume (access denied)" :
                 result == -ENODEV ? "The desktop has no mixer control handle" : "The volume could not be changed (error %d)", result);
        show(a);
        return;
    }
    mixer_refresh(a);
}

static const struct du_menu_item options_template[] = { {"&Refresh", MX_M_REFRESH, DU_KEY_F5, 0, 0} };
static const struct du_menu_item help_template[] = { {"&About Volume Control", MX_M_ABOUT, 0, 0, 0} };

void mixer_attach(struct mixer_app *a, struct du_ui *u, const struct mixer_backend *backend)
{
    a->ui = u; a->backend = backend; u->event = mixer_event; u->user = a;
    memcpy(a->options_items, options_template, sizeof(options_template));
    memcpy(a->help_items, help_template, sizeof(help_template));
    a->menus[0] = (struct du_menu){"&Options", a->options_items, 1};
    a->menus[1] = (struct du_menu){"&Help", a->help_items, 1};
    u->menus = a->menus; u->menu_count = 2;
    du_add(u, DU_GROUP, MX_GROUP, (struct du_rect){8, 28, 96, 230}, "Master");
    struct du_widget *v = du_add(u, DU_TRACKBAR, MX_VOLUME, (struct du_rect){44, 86, 26, 110}, "");
    du_trackbar_set(v, 0, 100, 0, 2, 10, 0);
    du_add(u, DU_LABEL, MX_LEVEL, (struct du_rect){14, 200, 86, 16}, "");
    du_add(u, DU_BUTTON, MX_MUTE, (struct du_rect){18, 224, 76, 22}, "[ ] &Mute");
    du_add(u,DU_BUTTON,MX_MODE,(struct du_rect){8,2,220,24},"&Recording controls");
    struct du_widget *b = du_add(u, DU_TRACKBAR, MX_BALANCE, (struct du_rect){24, 56, 64, 24}, "");
    du_trackbar_set(b, 0, 200, 100, 5, 20, 0);
    du_add(u, DU_LABEL, MX_LEFT, (struct du_rect){14, 60, 10, 18}, "L");
    du_add(u, DU_LABEL, MX_RIGHT, (struct du_rect){90, 60, 10, 18}, "R");
    du_add(u, DU_STATUS, MX_STATUS, (struct du_rect){0, MIXER_H - 20, MIXER_W, 20}, "");
    for (unsigned i=1;i<=5;i++) {
        int x=108+(int)(i-1)*102;
        du_add(u,DU_GROUP,line_id(i,4),(struct du_rect){x,28,100,230},line_names[i]);
        struct du_widget *vol=du_add(u,DU_TRACKBAR,line_id(i,0),(struct du_rect){x+37,86,26,110},"");
        du_trackbar_set(vol,0,100,0,2,10,0);
        struct du_widget *bal=du_add(u,DU_TRACKBAR,line_id(i,1),(struct du_rect){x+8,56,84,24},"");
        du_trackbar_set(bal,0,200,100,5,20,0);
        du_add(u,DU_LABEL,line_id(i,3),(struct du_rect){x+6,200,88,18},"");
        du_add(u,DU_BUTTON,line_id(i,2),(struct du_rect){x+6,224,88,22},"[ ] Mute");
    }
    for (unsigned i=0;i<u->count;i++) if (u->widgets[i].id!=MX_STATUS) u->widgets[i].box.y+=30;
    mixer_refresh(a);
    u->focus = -1;
}

void mixer_event(struct du_ui *u, const struct du_event *e, void *user)
{
    struct mixer_app *a = user;
    if ((e->type==DU_EV_CHANGE || e->type==DU_EV_ACTIVATE) && e->id>=MX_LINE_BASE+10 && e->id<MX_LINE_BASE+60) {
        unsigned i=(unsigned)(e->id-MX_LINE_BASE)/10,kind=(unsigned)(e->id-MX_LINE_BASE)%10;
        if (!a->lines[i].available) return;
        if (kind==0) a->line_volume[i]=100-du_get(u,e->id)->value;
        else if (kind==1) a->line_balance[i]=du_get(u,e->id)->value-100;
        else if (kind==2) {
            if (a->recording) {
                for (unsigned j=1;j<=5;j++) a->lines[j].selected=j==i;
            }
            else a->lines[i].mute^=1;
        } else return;
        a->line_pending|=1u<<i;
        if (kind==2) { lines_apply(a); lines_show(a); show(a); }
        else { du_timer_start(u,MX_T_APPLY,120000000ull,0); lines_show(a); }
        return;
    }
    switch (e->type) {
    case DU_EV_CHANGE:
        if (e->id == MX_VOLUME) a->volume = 100 - du_get(u, MX_VOLUME)->value;
        else if (e->id == MX_BALANCE) a->balance = du_get(u, MX_BALANCE)->value - 100;
        else return;
        /* A drag produces many values: apply the last one 120 ms after it stops. */
        a->pending = 1; du_timer_start(u, MX_T_APPLY, 120000000ull, 0);
        describe(a); show(a);
        return;
    case DU_EV_TICK:
        if (e->id == MX_T_APPLY) { if (a->pending) apply(a); if (a->line_pending) { lines_apply(a); lines_show(a); show(a); } }
        else if (e->id == MX_T_POLL && !a->pending && !a->waiting && u->capture < 0) mixer_refresh(a);
        return;
    case DU_EV_ACTIVATE:
        if (e->id==MX_MODE) {
            if (a->line_pending) lines_apply(a);
            a->recording^=1; lines_refresh(a);
        }
        else if (e->id == MX_MUTE) { a->mute = !a->mute; apply(a); }
        else if (e->id == MX_M_REFRESH) mixer_refresh(a);
        else if (e->id == MX_M_ABOUT)
            du_message(u, 1, "About Volume Control", "CiukiOS Volume Control - Classic computing, one woof at a time.\n"
                       "Playback and recording lines supplied by the audio device.\nCode: MIT.", DU_DLG_ICON_INFO, NULL, 0, 0, 0);
        u->focus = -1;
        break;
    case DU_EV_KEY: {
        /* Up/Down or PgUp/PgDn: volume; Left/Right: balance; M: mute. */
        int dv = e->key == DU_KEY_UP ? 2 : e->key == DU_KEY_DOWN ? -2 : e->key == DU_KEY_PGUP ? 10 : e->key == DU_KEY_PGDN ? -10 : 0;
        int db = e->key == DU_KEY_LEFT ? -5 : e->key == DU_KEY_RIGHT ? 5 : 0;
        if (!a->available || a->recording) return;
        if (dv || db) {
            a->volume += dv; a->balance += db;
            if (a->volume < 0) a->volume = 0;
            if (a->volume > 100) a->volume = 100;
            if (a->balance < -100) a->balance = -100;
            if (a->balance > 100) a->balance = 100;
            a->pending = 1; du_timer_start(u, MX_T_APPLY, 120000000ull, 0);
        } else if (e->ch == 'm' || e->ch == 'M') { a->mute = !a->mute; apply(a); }
        else return;
        describe(a); show(a);
        break;
    }
    default: return;
    }
    show(a);
}
