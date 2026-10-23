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
            snprintf(text,sizeof(text),"%d%%  (L %u, R %u)",a->volume,(unsigned)l,(unsigned)r);
        }
        if (w->id>=MX_VOLUME && w->id<=MX_MUTE) disabled=!a->available;
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
    describe(a); show(a);
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
    du_add(u, DU_GROUP, MX_GROUP, (struct du_rect){8, 26, 120, 180}, "Volume");
    struct du_widget *v = du_add(u, DU_SCROLLBAR, MX_VOLUME, (struct du_rect){58, 46, 18, 110}, "");
    v->vertical = 1; v->total = 110; v->page = 10;
    du_add(u, DU_LABEL, MX_LEVEL, (struct du_rect){16, 162, 108, 16}, "");
    du_add(u, DU_BUTTON, MX_MUTE, (struct du_rect){26, 180, 84, 22}, "[ ] &Mute");
    du_add(u, DU_GROUP, MX_BGROUP, (struct du_rect){136, 26, 156, 80}, "Balance");
    struct du_widget *b = du_add(u, DU_SCROLLBAR, MX_BALANCE, (struct du_rect){160, 56, 108, 18}, "");
    b->total = 210; b->page = 10;
    du_add(u, DU_LABEL, MX_LEFT, (struct du_rect){146, 56, 10, 18}, "L");
    du_add(u, DU_LABEL, MX_RIGHT, (struct du_rect){274, 56, 10, 18}, "R");
    du_add(u, DU_STATUS, MX_STATUS, (struct du_rect){0, MIXER_H - 20, MIXER_W, 20}, "");
    mixer_refresh(a);
    u->focus = -1;
}

void mixer_event(struct du_ui *u, const struct du_event *e, void *user)
{
    struct mixer_app *a = user;
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
        if (e->id == MX_T_APPLY && a->pending) apply(a);
        else if (e->id == MX_T_POLL && !a->pending && !a->waiting && u->capture < 0) mixer_refresh(a);
        return;
    case DU_EV_ACTIVATE:
        if (e->id == MX_MUTE) { a->mute = !a->mute; apply(a); }
        else if (e->id == MX_M_REFRESH) mixer_refresh(a);
        else if (e->id == MX_M_ABOUT)
            du_message(u, 1, "About Volume Control", "CiukiOS Volume Control - Classic computing, one woof at a time.\n"
                       "Master volume of the F4 mixer (0-100, balance, mute).\nCode: MIT.", DU_DLG_ICON_INFO, NULL, 0, 0, 0);
        u->focus = -1;
        break;
    case DU_EV_KEY: {
        /* Up/Down or PgUp/PgDn: volume; Left/Right: balance; M: mute. */
        int dv = e->key == DU_KEY_UP ? 2 : e->key == DU_KEY_DOWN ? -2 : e->key == DU_KEY_PGUP ? 10 : e->key == DU_KEY_PGDN ? -10 : 0;
        int db = e->key == DU_KEY_LEFT ? -5 : e->key == DU_KEY_RIGHT ? 5 : 0;
        if (!a->available) return;
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
