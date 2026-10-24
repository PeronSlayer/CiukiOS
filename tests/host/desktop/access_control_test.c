/* G25: Control Panel Accessibility Options through the production toolkit: the five tabs, the Settings pages with
 * OK and Cancel, what is written to desktop.cfg, the merge with changes the desktop made meanwhile, the periodic
 * refresh and the high-contrast theme the written file names.
 * usage: access_control_test portrait.xrgb [renders-dir]
 * SPDX-License-Identifier: MIT */
#include "control.h"
#include "acc_test.h"
#include "../../../apps/desktop/theme.h"
#include <errno.h>

static char saved[CFG_TEXT_MAX]; static size_t saved_n; static unsigned saves, applies;
static int load(void *c, char *t, size_t s, size_t *n) { (void)c; if (!saved_n || saved_n > s) return -1; memcpy(t, saved, saved_n); *n = saved_n; return 0; }
static int save(void *c, const char *t, size_t n) { (void)c; if (n >= sizeof(saved)) return -1; memcpy(saved, t, n); saved[n] = 0; saved_n = n; saves++; return 0; }
static int apply(void *c, unsigned r, int v, int m) { (void)c; (void)v; (void)m; assert(r == DESK_SETTINGS_RELOAD); applies++; return 0; }
static struct cp_app app; static struct du_ui ui;
static uint32_t pixels[CP_WIDTH * CP_HEIGHT], portrait[256 * 256];
static const char *renders;

static struct ciuki_settings file(void)
{
    struct ciuki_settings s; cfg_defaults(&s); cfg_parse(&s, saved, saved_n); return s;
}
static void put(const struct ciuki_settings *s)
{ char out[CFG_TEXT_MAX]; size_t n = cfg_format(s, out, sizeof(out)); assert(n && !save(NULL, out, n)); }
static struct du_widget *widget(int id) { struct du_widget *w = du_get(&ui, id); assert(w); return w; }
static int has(int id) { return du_get(&ui, id) != NULL; }
static void click(int id) { at_click_widget(&ui, id); }
/* Controls never overlap one another and stay inside the tab page (the F_MODE2 click once hit a check box). */
static void layout(void)
{
    for (unsigned i = 0; i < ui.count; i++) {
        const struct du_widget *p = &ui.widgets[i];
        if (p->kind == DU_TABS || p->kind == DU_GROUP || p->kind == DU_LABEL || p->hidden) continue;
        if (p->id == 100 || p->id == 2 || p->id == 3) continue;     /* Control Panel button, Close, status line */
        assert(p->box.x >= 8 && p->box.y >= 28 && p->box.x + p->box.w <= CP_WIDTH - 8 && p->box.y + p->box.h <= CP_HEIGHT - 60);
        for (unsigned j = i + 1; j < ui.count; j++) {
            const struct du_widget *q = &ui.widgets[j];
            if (q->kind == DU_TABS || q->kind == DU_GROUP || q->kind == DU_LABEL || q->hidden || q->id == 100 || q->id == 2 || q->id == 3) continue;
            if (p->box.x < q->box.x + q->box.w && q->box.x < p->box.x + p->box.w && p->box.y < q->box.y + q->box.h && q->box.y < p->box.y + p->box.h) {
                fprintf(stderr, "widgets %d and %d overlap\n", p->id, q->id); assert(0);
            }
        }
    }
}
static void render(const char *name) { layout(); du_render(&ui); at_save(renders, name, &ui); }
static void slide(int id, int value) { struct du_widget *w = widget(id); w->value = value; du_post(&ui, DU_EV_CHANGE, id, value); }
static void tab(int index) { du_post(&ui, DU_EV_SELECT, widget(2000)->id, index); }
static unsigned count_kind(enum du_kind k) { unsigned n = 0; for (unsigned i = 0; i < ui.count; i++) n += ui.widgets[i].kind == k; return n; }

enum { TABS = 2000, OK, CANCEL, STICKY_ON, STICKY_SET, FILTER_ON, FILTER_SET, TOGGLE_ON, TOGGLE_SET, SENTRY_ON, SENTRY_SET, SHOW_ON,
       HC_ON, HC_SET, POINTERS, MAG_ON, MAG_SET, MOUSE_ON, MOUSE_SET, TIMEOUT_ON, TIMEOUT_MIN, TIMEOUT_V, FEEDBACK, STATUS_ON, SERIAL,
       NOTE0, NOTE1, NOTE2, NOTE3, NOTE4, HOTKEY, CONFIRM, SOUND, INDICATOR, S_LOCK, S_TWO, S_AUDIBLE,
       F_MODE0, F_MODE1, F_MODE2, F_WAIT, F_WAIT_V, F_DELAY, F_DELAY_V, F_REPEAT, F_REPEAT_V, F_BOUNCE, F_BOUNCE_V, F_NOREPEAT, F_CLICK,
       SS0, SS1, SS2, SS3, SCHEMES, M_NUM0, M_NUM1, M_MOD, M_SPEED, M_SPEED_V, M_TIME, M_TIME_V, M_CTRL, M_CTRL_V,
       G_LEVEL, G_LEVEL_V, G_MOUSE, G_FOCUS, G_TEXT, G_INVERT, G_TOP, G_BOTTOM, G_SIZE, G_SIZE_V, G_ON };

static void main_tabs(void)
{
    struct du_widget *t = widget(TABS);
    assert(t->kind == DU_TABS && t->count == 5);
    static const char *const names[5] = {"Keyboard", "Sound", "Display", "Mouse", "General"};
    for (unsigned i = 0; i < 5; i++) assert(!strcmp(t->items[i].text[0], names[i]));
}

int main(int argc, char **argv)
{
    assert(argc >= 2);
    FILE *f = fopen(argv[1], "rb"); assert(f && fread(portrait, 4, 256 * 256, f) == 256 * 256); fclose(f);
    renders = argc > 2 ? argv[2] : NULL;
    app.host = (struct cp_host){.load = load, .save = save, .apply = apply, .portrait = portrait, .screen_w = 1024, .screen_h = 768};
    du_init(&ui, pixels, CP_WIDTH, CP_HEIGHT); ui.focused_window = 1;
    assert(cp_page_index("access") == CP_ACCESS && CP_ACCESS < CP_PAGES);
    assert(!cp_attach(&app, &ui, cp_page_index("access")));
    assert(app.page == CP_ACCESS);
    /* The applet is listed under Input in the Control Panel. */
    unsigned listed = 0; for (unsigned k = 0; k < CP_APPLETS; k++) listed += cp_applets[k].page == CP_ACCESS && cp_applets[k].category == CP_CAT_INPUT;
    assert(listed == 1);
    main_tabs();
    assert(count_kind(DU_CHECKBOX) == 3 && count_kind(DU_BUTTON) >= 5);
    assert(!widget(STICKY_ON)->value && !widget(FILTER_ON)->value && !widget(TOGGLE_ON)->value);
    render("access-keyboard");

    /* StickyKeys on: written as the Win32 flags, applied through the reload request. */
    unsigned before = saves;
    click(STICKY_ON);
    assert(saves == before + 1 && applies >= 1);
    assert(file().access.sticky & ACC_ON);
    assert(strstr(saved, "access_sticky=1ff\n"));
    /* Its settings page: hot key, warning, sound, lock, two keys, tones, indicator, with OK and Cancel. */
    click(STICKY_SET);
    assert(widget(TABS)->count == 1 && !strcmp(widget(TABS)->items[0].text[0], "Settings for StickyKeys"));
    assert(widget(HOTKEY)->value && widget(CONFIRM)->value && widget(SOUND)->value && widget(S_LOCK)->value && widget(S_TWO)->value && widget(S_AUDIBLE)->value && widget(INDICATOR)->value);
    render("access-sticky");
    click(HOTKEY); click(S_LOCK); click(CONFIRM);
    assert(!(file().access.sticky & (ACC_HOTKEY | ACC_SKF_TRISTATE | ACC_CONFIRM)) && (file().access.sticky & ACC_ON));
    click(CANCEL);       /* Cancel restores what the page was opened with */
    assert((file().access.sticky & (ACC_HOTKEY | ACC_SKF_TRISTATE | ACC_CONFIRM)) == (ACC_HOTKEY | ACC_SKF_TRISTATE | ACC_CONFIRM));
    assert(has(STICKY_ON) && widget(TABS)->count == 5);
    click(STICKY_SET); click(S_TWO); click(OK);
    assert(!(file().access.sticky & ACC_SKF_TWOKEYSOFF));
    assert(has(STICKY_ON));

    /* FilterKeys: SlowKeys/RepeatKeys and BounceKeys exclude each other (FILTERKEYS: zero the others). */
    click(FILTER_ON); assert(file().access.filter & ACC_ON);
    click(FILTER_SET);
    assert(widget(F_MODE1)->value && !widget(F_MODE2)->value && widget(F_BOUNCE)->disabled && !widget(F_WAIT)->disabled);
    render("access-filter");
    click(F_MODE2);
    { struct ciuki_settings s = file(); assert(s.access.filter_bounce == 500 && !s.access.filter_wait && !s.access.filter_delay && !s.access.filter_repeat); }
    assert(!widget(F_BOUNCE)->disabled && widget(F_WAIT)->disabled);
    slide(F_BOUNCE, 3); assert(file().access.filter_bounce == 1400);
    click(F_MODE1);
    { struct ciuki_settings s = file(); assert(!s.access.filter_bounce && s.access.filter_wait == 1000 && s.access.filter_delay == 1000 && s.access.filter_repeat == 500); }
    slide(F_WAIT, 0); slide(F_DELAY, 6); slide(F_REPEAT, 2);
    { struct ciuki_settings s = file(); assert(s.access.filter_wait == 300 && s.access.filter_delay == 2000 && s.access.filter_repeat == 700); }
    click(F_NOREPEAT); assert(file().access.filter & ACC_FKF_NOREPEAT && widget(F_REPEAT)->disabled);
    click(F_NOREPEAT);
    assert(widget(F_CLICK)->value); click(F_CLICK); assert(!(file().access.filter & ACC_FKF_CLICK)); click(F_CLICK); assert(file().access.filter & ACC_FKF_CLICK);   /* on by default (Flags 126) */
    click(OK);

    /* ToggleKeys. */
    click(TOGGLE_ON); assert(file().access.toggle & ACC_ON);
    click(TOGGLE_SET); assert(widget(TABS)->count == 1 && widget(HOTKEY)->value); click(OK);

    /* Sound tab: SoundSentry defaults to flashing the desktop when switched on; ShowSounds. */
    tab(1); main_tabs();
    assert(widget(TABS)->active == 1 && has(SENTRY_ON) && has(SHOW_ON) && !has(STICKY_ON));
    render("access-sound");
    click(SENTRY_ON); assert((file().access.sentry & ACC_ON) && file().access.sentry_effect == ACC_SS_DISPLAY);
    click(SENTRY_SET);
    assert(widget(SS3)->value && !widget(SS0)->value);
    click(SS1); assert(file().access.sentry_effect == ACC_SS_TITLE);
    click(SS0); assert(file().access.sentry_effect == ACC_SS_NONE);
    click(OK);
    click(SHOW_ON); assert(file().access.show_sounds == 1);
    click(SHOW_ON); assert(file().access.show_sounds == 0);

    /* Display tab: High Contrast, pointer schemes, Magnifier. */
    tab(2); assert(widget(TABS)->active == 2 && has(HC_ON) && has(POINTERS) && has(MAG_ON));
    render("access-display");
    click(HC_ON); assert(file().access.contrast & ACC_ON);
    { char name[32]; assert(!desk_settings_theme(saved, saved_n, name, sizeof(name)) && !strcmp(name, "High Contrast Black")); }
    click(HC_SET);
    assert(widget(SCHEMES)->count == 4 && widget(HOTKEY)->value);
    render("access-contrast");
    du_post(&ui, DU_EV_SELECT, SCHEMES, 1);
    { char name[32]; assert(!strcmp(file().access.scheme, "High Contrast White") && !desk_settings_theme(saved, saved_n, name, sizeof(name)) && !strcmp(name, "High Contrast White")); }
    click(OK);
    click(HC_ON); assert(!(file().access.contrast & ACC_ON));
    { char name[32]; assert(!desk_settings_theme(saved, saved_n, name, sizeof(name)) && !strcmp(name, "Ciuki Blue")); }
    du_post(&ui, DU_EV_SELECT, POINTERS, 3); assert(file().access.pointer == ACC_POINTER_LARGE_INVERTED);
    du_post(&ui, DU_EV_SELECT, POINTERS, 1); assert(file().access.pointer == ACC_POINTER_LARGE);
    du_post(&ui, DU_EV_SELECT, POINTERS, 0); assert(file().access.pointer == ACC_POINTER_STANDARD);
    click(MAG_SET);
    assert(widget(G_LEVEL)->value == 1 && widget(G_MOUSE)->value && widget(G_FOCUS)->value && !widget(G_TEXT)->value && widget(G_TOP)->value);
    render("access-magnifier");
    slide(G_LEVEL, 4); assert(file().access.mag_level == 5 && !strcmp(widget(G_LEVEL_V)->text, "5x"));
    slide(G_SIZE, 4); assert(file().access.mag_percent == 30);
    click(G_TEXT); click(G_MOUSE); assert(file().access.mag_follow == (ACC_MAG_FOCUS | ACC_MAG_TEXT));
    click(G_INVERT); assert(file().access.mag_invert == 1);
    click(G_BOTTOM); assert(file().access.mag_dock == ACC_DOCK_BOTTOM);
    click(G_TOP); assert(file().access.mag_dock == ACC_DOCK_TOP);
    click(G_ON); assert(file().access.mag_on == 1);
    click(CANCEL);       /* Cancel puts the Magnifier back as it was before the page */
    assert(!file().access.mag_on && file().access.mag_level == 2 && file().access.mag_follow == (ACC_MAG_MOUSE | ACC_MAG_FOCUS));
    click(MAG_ON); assert(file().access.mag_on == 1);
    click(MAG_ON); assert(file().access.mag_on == 0);

    /* Mouse tab. */
    tab(3); assert(has(MOUSE_ON) && !has(MAG_ON));
    click(MOUSE_ON); assert(file().access.mouse & ACC_ON);
    click(MOUSE_SET);
    assert(widget(M_NUM1)->value && !widget(M_NUM0)->value && widget(M_MOD)->value);
    assert(!strcmp(widget(M_SPEED_V)->text, "80 pixels/s") && !strcmp(widget(M_TIME_V)->text, "3.0 s") && !strcmp(widget(M_CTRL_V)->text, "8 times"));
    render("access-mouse");
    click(M_NUM0); assert(file().access.mouse & ACC_MKF_REPLACENUMBERS);
    click(M_NUM1); assert(!(file().access.mouse & ACC_MKF_REPLACENUMBERS));
    slide(M_SPEED, 8); slide(M_TIME, 0); slide(M_CTRL, 1);
    { struct ciuki_settings s = file(); assert(s.access.mouse_max == 360 && s.access.mouse_time == 1000 && s.access.mouse_ctrl == 4); }
    click(M_MOD); assert(!(file().access.mouse & ACC_MKF_MODIFIERS) && widget(M_CTRL)->disabled);
    click(OK);

    /* General tab: the time-out and the status indicator. */
    tab(4); assert(has(TIMEOUT_ON) && has(SERIAL) && widget(SERIAL)->disabled);
    assert(widget(TIMEOUT_MIN)->disabled && !strcmp(widget(TIMEOUT_V)->text, "5 minutes") && widget(STATUS_ON)->value);
    render("access-general");
    click(TIMEOUT_ON); assert((file().access.timeout & ACC_ATF_ON) && !widget(TIMEOUT_MIN)->disabled);
    slide(TIMEOUT_MIN, 2); assert(file().access.timeout_ms == 900000 && !strcmp(widget(TIMEOUT_V)->text, "15 minutes"));
    click(FEEDBACK); assert(file().access.timeout & ACC_ATF_FEEDBACK);
    click(STATUS_ON); assert(!file().access.status);
    assert(strstr(saved, "access_timeout=3,900\n") && strstr(saved, "access_general=0,0,0\n"));

    /* What the desktop changed meanwhile (a hot key) survives a save of an unrelated setting. */
    { struct ciuki_settings s = file(); s.access.sticky &= ~ACC_ON; s.access.toggle |= ACC_ON; s.access.mouse |= ACC_ON; put(&s); }
    click(STATUS_ON); assert(file().access.status == 1);
    { struct ciuki_settings s = file(); assert(!(s.access.sticky & ACC_ON) && (s.access.mouse & ACC_ON) && s.access.timeout_ms == 900000); }
    /* ... and the periodic refresh shows the desktop's state. */
    tab(0); assert(!widget(STICKY_ON)->value);
    { struct ciuki_settings s = file(); s.access.sticky |= ACC_ON; put(&s); }
    cp_tick(&app);
    assert(widget(STICKY_ON)->value);
    /* A page does not lose its sub-view to a refresh that changed nothing. */
    click(STICKY_SET); cp_tick(&app); assert(widget(TABS)->count == 1); click(OK);

    /* Back to the Control Panel. */
    click(100);   /* C_BACK */
    assert(app.page == CP_HOME);
    puts("PASS accessibility options: tabs, settings pages with OK and Cancel, Win32 flags in desktop.cfg, merge with the desktop, refresh, high-contrast theme");
    return 0;
}
