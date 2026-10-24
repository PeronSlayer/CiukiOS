/* SPDX-License-Identifier: MIT
 * G25 accessibility, desktop side: the input filters in front of the event
 * stream, the tray status indicator, the hot-key confirmation dialog, tones,
 * SoundSentry and ShowSounds, high contrast and the Magnifier's state.
 * The engine is access.c; docs/design/accessibility.md has the behaviour. */
#include "desktop.h"
#include "shell.h"
#include <ciuki/abi.h>
#include "../libdeskui/deskui.h"
#include <stdio.h>
#include <string.h>

#define FLASH_NS 250000000ull

static int live(const struct desktop *d) { return d->shell && !d->sh.gate; }
static void start(struct desktop *d)
{
    struct desk_access *x = &d->acc;
    if (x->started) return;
    access_init(&x->eng, &d->sh.cfg.access, d->now_ns);
    x->started = 1;
    x->mag_side = (int)d->sh.cfg.access.mag_dock;
}

void desk_access_reconcile(struct desktop *d);

/* ---- tray indicator ----------------------------------------------------------- */
static void indicator(struct desktop *d)
{
    struct desk_access *x = &d->acc;
    int visible = access_indicator_visible(&x->eng);
    unsigned bits = access_indicator(&x->eng);
    if (visible == x->shown_visible && bits == x->shown_bits) return;
    int before = desk_shell_tray_box(d).w;
    x->shown_visible = visible; x->shown_bits = bits;
    desk_shell_tray_damage(d, before);
}
int desk_access_indicator_visible(const struct desktop *d) { return d->acc.shown_visible; }
void desk_access_indicator_draw(struct desktop *d, struct du_canvas *c, struct desk_box b)
{
    unsigned bits = d->acc.shown_bits;
    uint32_t ink = desk_theme->topbar_text, fill = desk_theme->topbar_accent;
    if (b.w <= 0) return;
    if (bits & (1u << ACC_F_STICKY)) {
        /* The three rectangles are the Shift, Ctrl and Alt keys: filled while latched, solid ink while locked. */
        static const unsigned latched[3] = {ACC_IND_SHIFT, ACC_IND_CTRL, ACC_IND_ALT};
        static const unsigned locked[3] = {ACC_IND_LOCK_SHIFT, ACC_IND_LOCK_CTRL, ACC_IND_LOCK_ALT};
        for (int k = 0; k < 3; k++) {
            struct du_rect r = {b.x + 1 + k * 5, b.y + 1, 4, 4};
            du_fill(c, r, bits & locked[k] ? ink : bits & latched[k] ? fill : desk_theme->topbar);
            du_fill(c, (struct du_rect){r.x, r.y, r.w, 1}, ink); du_fill(c, (struct du_rect){r.x, r.y + r.h - 1, r.w, 1}, ink);
            du_fill(c, (struct du_rect){r.x, r.y, 1, r.h}, ink); du_fill(c, (struct du_rect){r.x + r.w - 1, r.y, 1, r.h}, ink);
        }
    }
    if (bits & (1u << ACC_F_MOUSE)) {
        /* A mouse: its left and right button shaded for the selected one, filled while held. */
        struct du_rect body = {b.x + 1, b.y + 6, 7, 9};
        du_fill(c, (struct du_rect){body.x, body.y, body.w, 1}, ink); du_fill(c, (struct du_rect){body.x, body.y + body.h - 1, body.w, 1}, ink);
        du_fill(c, (struct du_rect){body.x, body.y, 1, body.h}, ink); du_fill(c, (struct du_rect){body.x + body.w - 1, body.y, 1, body.h}, ink);
        du_fill(c, (struct du_rect){body.x + 3, body.y, 1, 4}, ink);
        if (bits & ACC_IND_LEFT) du_fill(c, (struct du_rect){body.x + 1, body.y + 1, 2, 3}, bits & ACC_IND_LEFT_HELD ? ink : fill);
        if (bits & ACC_IND_RIGHT) du_fill(c, (struct du_rect){body.x + 4, body.y + 1, 2, 3}, bits & ACC_IND_RIGHT_HELD ? ink : fill);
    }
    if (bits & ACC_IND_STOPWATCH) {
        /* A stopwatch: SlowKeys, BounceKeys or RepeatKeys changes the keyboard's response. */
        int cx = b.x + 12, cy = b.y + 10;
        for (int y = -4; y <= 4; y++) for (int xx = -4; xx <= 4; xx++) {
            int r2 = xx * xx + y * y;
            if (r2 <= 20 && r2 >= 11) du_fill(c, (struct du_rect){cx + xx, cy + y, 1, 1}, ink);
        }
        du_fill(c, (struct du_rect){cx, cy - 3, 1, 4}, ink); du_fill(c, (struct du_rect){cx, cy, 3, 1}, ink);
        du_fill(c, (struct du_rect){cx - 1, b.y + 5, 3, 1}, ink);
    }
}

/* ---- tones and visual cues ------------------------------------------------------ */
static void play(struct desktop *d, unsigned tone)
{ if (d->sound_tone) d->sound_tone(d, tone); }

static const char *const sound_caption[] = {
    [SOUND_STARTUP] = "CiukiOS is starting.", [SOUND_SHUTDOWN] = "CiukiOS is shutting down.",
    [SOUND_ERROR] = "An error message appeared.", [SOUND_INFORMATION] = "A message appeared.",
    [SOUND_QUESTION] = "A question appeared.", [SOUND_END_TASK] = "A program was ended.",
    [SOUND_EMPTY_RECYCLE] = "The Recycle Bin was emptied.", [SOUND_LOGON] = "CiukiOS is ready.",
    [SOUND_DEVICE_CONNECT] = "A device was connected.", [SOUND_DEVICE_DISCONNECT] = "A device was disconnected.",
    [SOUND_AUTOPLAY] = "A disc or drive was inserted.",
};
/* Window and menu feedback sounds are interface clicks, not alerts: SoundSentry and ShowSounds follow the others. */
static int alert_event(unsigned e) { return e < sizeof(sound_caption) / sizeof(sound_caption[0]) && sound_caption[e]; }

static struct desk_box flash_box(const struct desktop *d, unsigned effect)
{
    struct desk_box screen = {0, 0, (int32_t)d->width, (int32_t)d->height};
    if (effect == ACC_SS_DISPLAY || d->focus < 0 || !d->clients[d->focus].window || d->clients[d->focus].minimized) return screen;
    const struct desk_client *c = &d->clients[d->focus];
    if (effect == ACC_SS_TITLE) return (struct desk_box){c->frame.x, c->frame.y, c->frame.w, DESK_TITLE};
    return c->frame;
}
void desk_access_sound_cue(struct desktop *d, unsigned event)
{
    if (!live(d) || !alert_event(event)) return;
    const struct access_settings *s = &d->sh.cfg.access;
    struct desk_access *x = &d->acc;
    if ((s->sentry & ACC_ON) && s->sentry_effect && s->sentry_effect <= ACC_SS_MAX) {
        if (x->flash_active) desk_damage_add(&d->damage, x->flash, d->width, d->height);
        x->flash = flash_box(d, s->sentry_effect); x->flash_active = 1; x->flash_until = d->now_ns + FLASH_NS;
        desk_damage_add(&d->damage, x->flash, d->width, d->height);
    }
    if (s->show_sounds)
        desk_shell_notify(d, "Sound", sound_caption[event], DU_DLG_ICON_INFO, SH_NOTIFY_DETAILS);
}
/* The screen flash: every colour of the box inverted, painted last (shell overlay). */
void desk_access_flash_draw(struct desktop *d, struct desk_box clip)
{
    struct desk_access *x = &d->acc;
    struct desk_box b;
    if (!x->flash_active || !desk_intersect(x->flash, clip, &b)) return;
    for (int y = b.y; y < b.y + b.h; y++) {
        uint32_t *row = d->pixels + (size_t)y * d->width + (size_t)b.x;
        for (int xx = 0; xx < b.w; xx++) row[xx] = ~row[xx] & 0xffffffu;
    }
}
/* Boxes the compositor must treat as overlays: screen copies and client fast paths never cross them. */
unsigned desk_access_overlay(const struct desktop *d, struct desk_box boxes[2])
{
    unsigned n = 0;
    if (d->acc.flash_active) boxes[n++] = d->acc.flash;
    if (d->acc.mag_shown) boxes[n++] = (struct desk_box){d->acc.mag_panel.x, d->acc.mag_panel.y, d->acc.mag_panel.w, d->acc.mag_panel.h};
    return n;
}

/* ---- theme and pointer ------------------------------------------------------------ */
static void retheme(struct desktop *d)
{
    if (desk_theme_use(cfg_effective_theme(&d->sh.cfg))) desk_theme_use(DESK_THEME_DEFAULT);
    desk_theme_changed(d);
}
static void pointer_scheme(struct desktop *d)
{
    d->cursor_loaded = DESK_CURSOR_COUNT; d->cursor_dirty = 1;   /* the pointer is loaded again from the scheme */
}

/* ---- confirmation ------------------------------------------------------------------ */
static void confirm_dialog(struct desktop *d, unsigned feature)
{
    static const struct { const char *title, *text; } words[ACC_F_COUNT] = {
        [ACC_F_STICKY] = {"StickyKeys",
            "You pressed the SHIFT key 5 times. This is the keyboard shortcut for the StickyKeys feature.\n\n"
            "StickyKeys lets you press one key at a time for keyboard shortcuts such as CTRL+ALT+DEL.\n\n"
            "Do you want to use StickyKeys?"},
        [ACC_F_FILTER] = {"FilterKeys",
            "You held down the right SHIFT key for 8 seconds. This is the keyboard shortcut for the FilterKeys feature, "
            "which changes the way your keyboard responds to keystrokes.\n\nDo you want to use FilterKeys?"},
        [ACC_F_TOGGLE] = {"ToggleKeys",
            "You held down the NUM LOCK key for 5 seconds. This is the keyboard shortcut for the ToggleKeys feature.\n\n"
            "ToggleKeys plays a high tone when you turn on CAPS LOCK, NUM LOCK or SCROLL LOCK and a low tone when you turn it off.\n\n"
            "Do you want to use ToggleKeys?"},
        [ACC_F_MOUSE] = {"MouseKeys",
            "You pressed the left ALT, left SHIFT and NUM LOCK keys together. This is the keyboard shortcut for the MouseKeys feature.\n\n"
            "MouseKeys lets you control the pointer with the numeric keypad.\n\nDo you want to use MouseKeys?"},
        [ACC_F_CONTRAST] = {"High Contrast",
            "You pressed the left ALT, left SHIFT and PRINT SCREEN keys together. This is the keyboard shortcut for the High Contrast feature.\n\n"
            "High Contrast shows the screen in colours that are easier to read.\n\nDo you want to use High Contrast?"},
    };
    if (feature <= ACC_F_NONE || feature >= ACC_F_COUNT) return;
    char text[640];
    snprintf(text, sizeof(text), "%s\n\nThe settings are in Accessibility Options in Control Panel.", words[feature].text);
    desk_shell_confirm(d, words[feature].title, text, DU_DLG_ICON_QUESTION, SH_CONFIRM_ACCESS + (int)feature);
}

/* ---- engine output ---------------------------------------------------------------- */
static void process(struct desktop *d, struct access_out *o, const struct ciuki_input_event *base)
{
    struct desk_access *x = &d->acc;
    for (unsigned i = 0; i < o->count; i++) {
        struct ciuki_input_event e;
        if (base) e = *base; else memset(&e, 0, sizeof(e));
        e.monotonic_ns = base && base->monotonic_ns ? base->monotonic_ns : d->now_ns;
        e.type = (uint32_t)o->ev[i].type; e.code = o->ev[i].code; e.value = o->ev[i].value; e.value2 = o->ev[i].value2;
        e.lost_count = 0;
        if (e.type == CIUKI_INPUT_MOTION) { e.code = 0; e.source = DESK_ACCESS_SOURCE; }
        else if (e.type == CIUKI_INPUT_BUTTON) { e.value2 = 0; e.source = DESK_ACCESS_SOURCE; }
        else e.value2 = 0;
        desk_input_unfiltered(d, &e);
    }
    for (unsigned i = 0; i < o->tones; i++) play(d, o->tone[i]);
    if (o->changed) { d->sh.cfg.access = x->eng.s; d->sh.want_save = 1; }
    if (o->contrast_changed) retheme(d);
    if (o->redraw || o->changed) indicator(d);
    if (o->mouse_keys_moved) x->mouse_stamp = d->now_ns;
    if (o->confirm) confirm_dialog(d, o->confirm);
}
/* Settings arrive from the Control Panel (a reload replaced d->sh.cfg): the engine follows. */
static void sync_in(struct desktop *d)
{
    struct desk_access *x = &d->acc;
    if (!memcmp(&x->eng.s, &d->sh.cfg.access, sizeof(x->eng.s))) return;
    struct access_settings old = x->eng.s;
    struct access_out o; memset(&o, 0, sizeof(o));
    access_configure(&x->eng, &d->sh.cfg.access, &o);
    if (old.pointer != d->sh.cfg.access.pointer) pointer_scheme(d);
    process(d, &o, NULL);
    x->mag_pending |= d->sh.cfg.access.mag_on != 0;
}
void desk_access_settings_changed(struct desktop *d)
{
    if (!live(d)) return;
    start(d); sync_in(d); indicator(d);
    desk_access_reconcile(d);   /* the Magnifier opens or closes now, not at the next input */
}

int desk_access_filter(struct desktop *d, const struct ciuki_input_event *in)
{
    if (!live(d) || d->power.state) return 0;
    struct desk_access *x = &d->acc;
    start(d); sync_in(d);
    struct access_out o; memset(&o, 0, sizeof(o));
    switch (in->type) {
    case CIUKI_INPUT_KEY:
        if (in->code < 0 || in->code >= ACC_KEYS || in->value < 0 || in->value > 2) return 0;
        access_key(&x->eng, d->now_ns, in->code, in->value, &o);
        process(d, &o, in);
        return 1;
    case CIUKI_INPUT_BUTTON:
        desk_input_unfiltered(d, in);
        access_button(&x->eng, d->now_ns, in->code, in->value, &o);
        process(d, &o, in);
        return 1;
    case CIUKI_INPUT_MOTION: access_motion(&x->eng, d->now_ns); x->mouse_stamp = d->now_ns; return 0;
    case CIUKI_INPUT_RESYNC: return 0;
    default: access_note_input(&x->eng, d->now_ns); return 0;
    }
}
void desk_access_resync(struct desktop *d)
{
    if (!d->acc.started) return;
    access_resync(&d->acc.eng);
    indicator(d);
}
void desk_access_tick(struct desktop *d)
{
    struct desk_access *x = &d->acc;
    if (!live(d) || d->power.state) return;
    start(d); sync_in(d);
    struct access_out o; memset(&o, 0, sizeof(o));
    access_tick(&x->eng, d->now_ns, &o);
    process(d, &o, NULL);
    if (x->flash_active && d->now_ns >= x->flash_until) {
        desk_damage_add(&d->damage, x->flash, d->width, d->height);
        x->flash_active = 0;
    }
}
uint64_t desk_access_wake(const struct desktop *d, uint64_t now)
{
    const struct desk_access *x = &d->acc;
    if (!x->started) return 0;
    uint64_t w = access_wake_ns(&x->eng, now);
    if (x->flash_active && (!w || x->flash_until < w)) w = x->flash_until > now ? x->flash_until : now + 1;
    return w;
}
/* The user answered the hot-key dialog of `feature`. */
void desk_access_confirmed(struct desktop *d, int feature, int yes)
{
    if (!d->acc.started || feature <= ACC_F_NONE || feature >= ACC_F_COUNT) return;
    struct access_out o; memset(&o, 0, sizeof(o));
    access_confirm(&d->acc.eng, (unsigned)feature, yes, &o);
    process(d, &o, NULL);
}

/* ---- Magnifier ---------------------------------------------------------------------- */
static void mag_damage(struct desktop *d, struct access_mag_box b)
{ desk_damage_add(&d->damage, (struct desk_box){b.x, b.y, b.w, b.h}, d->width, d->height); }
static void mag_clamp(struct access_settings *s)
{
    if (s->mag_level < ACC_MAG_LEVEL_MIN) s->mag_level = ACC_MAG_LEVEL_MIN;
    if (s->mag_level > ACC_MAG_LEVEL_MAX) s->mag_level = ACC_MAG_LEVEL_MAX;
}
void desk_access_magnifier_set(struct desktop *d, int on)
{
    if (!live(d)) return;
    d->sh.cfg.access.mag_on = on != 0; d->sh.want_save = 1;
    d->acc.mag_pending = 1;
    desk_access_reconcile(d);
}
int desk_access_magnifier_on(const struct desktop *d) { return d->shell && d->sh.cfg.access.mag_on; }
/* Windows logo key with Up/Down (level), Page Up (invert) and Page Down (mouse tracking). Returns 1 when consumed. */
int desk_access_shortcut(struct desktop *d, int code, int value)
{
    if (!live(d) || !d->sh.cfg.access.mag_on || !d->acc.started) return 0;
    int is_key = code == 0x148 || code == 0x150 || code == 0x149 || code == 0x151;
    if (!is_key) return 0;
    const struct access *a = &d->acc.eng;
    uint8_t held = (uint8_t)(a->phys | a->latched | a->locked);
    if (!(held & ((1u << ACC_M_LWIN) | (1u << ACC_M_RWIN)))) return 0;
    d->sh.logo_down = 0;   /* the Windows key was used with another key: releasing it must not open Start */
    if (value != 1) return 1;
    struct access_settings *s = &d->sh.cfg.access;
    if (code == 0x148 && s->mag_level < ACC_MAG_LEVEL_MAX) s->mag_level++;
    else if (code == 0x150 && s->mag_level > ACC_MAG_LEVEL_MIN) s->mag_level--;
    else if (code == 0x149) s->mag_invert = !s->mag_invert;
    else if (code == 0x151) s->mag_follow ^= ACC_MAG_MOUSE;
    mag_clamp(s);
    d->sh.want_save = 1; d->acc.mag_pending = 1;
    desk_access_reconcile(d);
    return 1;
}
/* Called with every frame decision (desk_scene_reconcile): which panel shows which part of the screen. */
void desk_access_reconcile(struct desktop *d)
{
    struct desk_access *x = &d->acc;
    int want = live(d) && d->sh.cfg.access.mag_on;
    if (!want) {
        if (x->mag_shown) { mag_damage(d, x->mag_panel); x->mag_shown = 0; }
        x->mag_pending = 0;
        return;
    }
    struct access_settings *s = &d->sh.cfg.access;
    mag_clamp(s);
    int sw = (int)d->width, sh = (int)d->height, bottom = d->shell ? DESK_BAR : 0;
    /* Where to look: the pointer, the keyboard focus, or whichever of the two moved last. */
    int mx = d->mouse_x, my = d->mouse_y;
    if (mx != x->mag_mx || my != x->mag_my) x->mouse_stamp = d->now_ns ? d->now_ns : 1;
    int cx = mx, cy = my;
    int follow_focus = (s->mag_follow & (ACC_MAG_FOCUS | ACC_MAG_TEXT)) && d->focus >= 0 && d->clients[d->focus].window;
    if (follow_focus) {
        struct desk_box f = d->clients[d->focus].frame;
        int fx = f.x + f.w / 2, fy = f.y + f.h / 2;
        if (fx != x->focus_cx || fy != x->focus_cy) { x->focus_cx = fx; x->focus_cy = fy; x->focus_stamp = d->now_ns ? d->now_ns : 1; }
        if (!(s->mag_follow & ACC_MAG_MOUSE) || x->focus_stamp > x->mouse_stamp) { cx = fx; cy = fy; }
    }
    unsigned side = access_mag_side(sw, sh, DESK_BAR, bottom, s->mag_percent, s->mag_dock, s->mag_level, cx, cy);
    struct access_mag_box panel = access_mag_panel(sw, sh, DESK_BAR, bottom, s->mag_percent, s->mag_dock, side != s->mag_dock), source;
    /* The pointer is never under the panel (it is painted after the pointer): the panel goes to the other edge. */
    if (mx >= panel.x && mx < panel.x + panel.w && my >= panel.y && my < panel.y + panel.h) {
        side = side == ACC_DOCK_TOP ? ACC_DOCK_BOTTOM : ACC_DOCK_TOP;
        panel = access_mag_panel(sw, sh, DESK_BAR, bottom, s->mag_percent, s->mag_dock, side != s->mag_dock);
    }
    if (!access_mag_source(sw, sh, panel, s->mag_level, cx, cy, &source)) {
        if (x->mag_shown) { mag_damage(d, x->mag_panel); x->mag_shown = 0; }
        return;
    }
    int hw = d->hw_cursor || d->hw_cursor_shown;
    int changed = !x->mag_shown || memcmp(&panel, &x->mag_panel, sizeof(panel)) || memcmp(&source, &x->mag_source, sizeof(source)) ||
        s->mag_level != x->mag_level || s->mag_invert != x->mag_invert || mx != x->mag_mx || my != x->mag_my ||
        d->cursor_shape != x->mag_shape || hw != x->mag_hw || x->mag_pending;
    if (!changed) {
        struct desk_box src = {source.x, source.y, source.w, source.h}, pan = {panel.x, panel.y, panel.w, panel.h}, ignored;
        for (unsigned i = 0; i < d->damage.count && !changed; i++)
            changed = desk_intersect(d->damage.boxes[i], src, &ignored) || desk_intersect(d->damage.boxes[i], pan, &ignored);
        for (unsigned i = 0; i < d->move_damage.count && !changed; i++)
            changed = desk_intersect(d->move_damage.boxes[i], src, &ignored) || desk_intersect(d->move_damage.boxes[i], pan, &ignored);
    }
    if (changed) {
        if (x->mag_shown && memcmp(&panel, &x->mag_panel, sizeof(panel))) mag_damage(d, x->mag_panel);
        mag_damage(d, panel);
        x->mag_pending = 1;
    }
    x->mag_shown = 1; x->mag_panel = panel; x->mag_source = source; x->mag_level = s->mag_level; x->mag_invert = s->mag_invert;
    x->mag_mx = mx; x->mag_my = my; x->mag_shape = d->cursor_shape; x->mag_hw = hw; x->mag_side = (int)side;
}
