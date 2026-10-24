/* SPDX-License-Identifier: MIT
 * G25 accessibility engine: StickyKeys, FilterKeys, ToggleKeys, MouseKeys,
 * the emergency hot keys, the idle time-out, the tones and the Magnifier's
 * geometry. Pure and integer-only; see access.h and
 * docs/design/accessibility.md for the Windows 98 behaviour it follows. */
#include "access.h"
#include <string.h>

#define MS 1000000ull
#define BIT(m) (1u << (m))
#define TICK_NS (16 * MS)
#define KEY_NUMLOCK 0x45
#define KEY_CAPS 0x3a
#define KEY_SCROLL 0x46
#define KEY_PRINT 0x137
#define KEY_SYSRQ 0x54
#define KEY_RSHIFT 0x36

const int acc_mod_code[ACC_MODS] = {0x2a, 0x36, 0x1d, 0x11d, 0x38, 0x138, 0x15b, 0x15c};


/* ---- tones ----------------------------------------------------------------- */
unsigned access_tone_steps(unsigned tone, struct access_tone_step st[8])
{
    memset(st, 0, 8 * sizeof(*st));
    switch (tone) {
    case ACC_TONE_SIREN_UP: st[0] = (struct access_tone_step){600, 1800, 350}; return 1;
    case ACC_TONE_SIREN_DOWN: st[0] = (struct access_tone_step){1800, 600, 350}; return 1;
    case ACC_TONE_SIREN_UP2:
        st[0] = (struct access_tone_step){600, 1800, 250}; st[1] = (struct access_tone_step){0, 0, 80};
        st[2] = (struct access_tone_step){600, 1800, 250}; return 3;
    case ACC_TONE_SIREN_UP3:
        for (unsigned i = 0; i < 3; i++) {
            st[i * 2] = (struct access_tone_step){600, 1800, 200};
            if (i < 2) st[i * 2 + 1] = (struct access_tone_step){0, 0, 80};
        }
        return 5;
    case ACC_TONE_BEEP_HIGH: st[0] = (struct access_tone_step){1500, 1500, 90}; return 1;
    case ACC_TONE_BEEP_LOW: st[0] = (struct access_tone_step){500, 500, 90}; return 1;
    case ACC_TONE_LATCH:    /* "a short low beep/high beep" */
        st[0] = (struct access_tone_step){500, 500, 50}; st[1] = (struct access_tone_step){0, 0, 20};
        st[2] = (struct access_tone_step){1500, 1500, 50}; return 3;
    case ACC_TONE_LOCK: st[0] = (struct access_tone_step){1500, 1500, 70}; return 1;
    case ACC_TONE_UNLATCH: st[0] = (struct access_tone_step){500, 500, 70}; return 1;
    case ACC_TONE_WARNING:  /* "three short warning beeps" */
        for (unsigned i = 0; i < 3; i++) {
            st[i * 2] = (struct access_tone_step){900, 900, 60};
            if (i < 2) st[i * 2 + 1] = (struct access_tone_step){0, 0, 60};
        }
        return 5;
    case ACC_TONE_CLICK: st[0] = (struct access_tone_step){1000, 1000, 8}; return 1;
    default: return 0;
    }
}
unsigned access_tone_ms(unsigned tone)
{
    struct access_tone_step st[8]; unsigned n = access_tone_steps(tone, st), total = 0;
    for (unsigned i = 0; i < n; i++) total += st[i].ms;
    return total;
}
static const int16_t sine[64] = {
    0,803,1598,2378,3135,3861,4551,5196,5792,6332,6811,7224,7567,7838,8034,8152,8191,8152,8034,7838,7567,7224,6811,6332,5792,5196,4551,3861,3135,2378,1598,803,
    0,-803,-1598,-2378,-3135,-3861,-4551,-5196,-5792,-6332,-6811,-7224,-7567,-7838,-8034,-8152,-8191,-8152,-8034,-7838,-7567,-7224,-6811,-6332,-5792,-5196,-4551,-3861,-3135,-2378,-1598,-803};
unsigned access_tone_render(unsigned tone, int16_t *pcm, unsigned max_frames, unsigned rate)
{
    struct access_tone_step st[8]; unsigned n = access_tone_steps(tone, st), written = 0;
    if (!rate) return 0;
    uint32_t phase = 0;
    for (unsigned i = 0; i < n; i++) {
        unsigned frames = (unsigned)((uint64_t)st[i].ms * rate / 1000u);
        unsigned fade = rate / 500u;   /* 2 ms in and out against clicks */
        if (fade * 2 > frames) fade = frames / 2;
        for (unsigned k = 0; k < frames && written < max_frames; k++, written++) {
            if (!st[i].from_hz) { pcm[written] = 0; continue; }
            int64_t hz = st[i].from_hz + ((int64_t)st[i].to_hz - st[i].from_hz) * (int64_t)k / (int64_t)(frames ? frames : 1);
            phase += (uint32_t)((uint64_t)hz * 0x100000000ull / rate);
            int32_t v = sine[phase >> 26];
            if (k < fade) v = v * (int32_t)k / (int32_t)fade;
            else if (frames - k <= fade) v = v * (int32_t)(frames - k) / (int32_t)fade;
            pcm[written] = (int16_t)v;
        }
    }
    return written;
}

/* ---- helpers --------------------------------------------------------------- */
static int mod_of(int code)
{
    for (int i = 0; i < ACC_MODS; i++) if (acc_mod_code[i] == code) return i;
    return -1;
}
static void tone(struct access_out *o, unsigned t)
{ if (o->tones < ACC_OUT_TONES) o->tone[o->tones++] = (uint8_t)t; }
static void emit(struct access_out *o, int type, int code, int value, int value2)
{
    if (o->count < ACC_OUT_EVENTS) o->ev[o->count++] = (struct access_event){type, code, value, value2};
}
static uint32_t *flags_in(struct access_settings *s, unsigned f)
{
    switch (f) {
    case ACC_F_STICKY: return &s->sticky;
    case ACC_F_FILTER: return &s->filter;
    case ACC_F_TOGGLE: return &s->toggle;
    case ACC_F_MOUSE: return &s->mouse;
    case ACC_F_CONTRAST: return &s->contrast;
    default: return NULL;
    }
}
static uint32_t *flags_of(struct access *a, unsigned f) { return flags_in(&a->s, f); }
int access_feature_on(const struct access *a, unsigned f)
{ uint32_t *w = flags_of((struct access *)a, f); return w && (*w & ACC_ON); }
static void key_mark(struct access *a, int code, int down)
{
    if (code < 0 || code >= ACC_KEYS) return;
    if (down) a->down[code >> 3] |= (uint8_t)(1u << (code & 7)); else a->down[code >> 3] &= (uint8_t)~(1u << (code & 7));
}
static int any_key_down(const struct access *a)
{ for (unsigned i = 0; i < sizeof(a->down); i++) if (a->down[i]) return 1; return 0; }
static uint8_t effective(const struct access *a) { return (uint8_t)(a->phys | a->latched | a->locked); }
static void swallow_add(struct access *a, int code)
{
    for (unsigned i = 0; i < 4; i++) if (a->swallow_code[i] == code) return;
    for (unsigned i = 0; i < 4; i++) if (!a->swallow_code[i]) { a->swallow_code[i] = code; return; }
    a->swallow_code[0] = code;
}
static int swallowed(struct access *a, int code, int value)
{
    for (unsigned i = 0; i < 4; i++) if (a->swallow_code[i] == code) {
        if (value == 0) a->swallow_code[i] = 0;
        return 1;
    }
    return 0;
}

/* ---- StickyKeys -------------------------------------------------------------- */
static void sticky_release_latched(struct access *a, struct access_out *o)
{
    for (unsigned m = 0; m < ACC_MODS; m++) if (a->latched & BIT(m)) {
        a->latched &= (uint8_t)~BIT(m);
        if (!(a->phys & BIT(m)) && (a->sent & BIT(m))) { emit(o, ACC_EV_KEY, acc_mod_code[m], 0, 0); a->sent &= (uint8_t)~BIT(m); }
    }
}
static void sticky_release_all(struct access *a, struct access_out *o)
{
    a->locked = 0; sticky_release_latched(a, o);
    for (unsigned m = 0; m < ACC_MODS; m++) if ((a->sent & BIT(m)) && !(a->phys & BIT(m))) {
        emit(o, ACC_EV_KEY, acc_mod_code[m], 0, 0); a->sent &= (uint8_t)~BIT(m);
    }
}
static void feature_off_effects(struct access *a, unsigned f, struct access_out *o);
static void feature_switch(struct access *a, unsigned f, int on, int hotkey, struct access_out *o)
{
    uint32_t *w = flags_of(a, f);
    if (!w || !!(*w & ACC_ON) == !!on) return;
    if (on) *w |= ACC_ON; else *w &= ~ACC_ON;
    o->changed |= BIT(f); o->redraw = 1;
    if (f == ACC_F_CONTRAST) o->contrast_changed = 1;
    if (!on) feature_off_effects(a, f, o);
    else if (f == ACC_F_STICKY) { a->latched = a->locked = 0; }
    else if (f == ACC_F_MOUSE) { a->mk_dir = 0; a->mk_drag = 0; if (!a->mk_button) a->mk_button = 1; }
    if (hotkey) {
        if (*w & ACC_SOUND) tone(o, on ? ACC_TONE_SIREN_UP : ACC_TONE_SIREN_DOWN);
        if (on && (*w & ACC_CONFIRM)) { o->confirm = f; o->confirm_on = 1; a->pending_confirm = f; }
    }
}
static void feature_off_effects(struct access *a, unsigned f, struct access_out *o)
{
    if (f == ACC_F_STICKY) sticky_release_all(a, o);
    else if (f == ACC_F_FILTER) memset(a->slot, 0, sizeof(a->slot));
    else if (f == ACC_F_MOUSE) {
        if (a->mk_drag) {
            if (a->mk_button & 1) emit(o, ACC_EV_BUTTON, 1, 0, 0);
            if (a->mk_button & 2) emit(o, ACC_EV_BUTTON, 2, 0, 0);
            a->mk_drag = 0;
        }
        a->mk_dir = 0;
    }
}
void access_set(struct access *a, unsigned feature, int on, struct access_out *o)
{ feature_switch(a, feature, on, 0, o); }
void access_confirm(struct access *a, unsigned feature, int keep, struct access_out *o)
{
    if (a->pending_confirm != feature) return;
    a->pending_confirm = 0;
    if (!keep) feature_switch(a, feature, 0, 0, o);
}

static void sticky_key(struct access *a, int code, int value, struct access_out *o)
{
    int m = mod_of(code);
    /* The Resource Kit's StickyKeys covers Shift, Ctrl and Alt; a latched Windows key would also keep the Start menu from
     * opening on its release, so the Windows keys pass straight through. */
    int on = (a->s.sticky & ACC_ON) && m < ACC_M_LWIN, audible = !!(a->s.sticky & ACC_SKF_AUDIBLE);
    if (m >= 0) {
        if (!on) {
            if (value == 0) { a->phys &= (uint8_t)~BIT(m); a->sent &= (uint8_t)~BIT(m); }
            else { a->phys |= (uint8_t)BIT(m); a->sent |= (uint8_t)BIT(m); }
            emit(o, ACC_EV_KEY, code, value, 0);
            return;
        }
        if (value == 2) return;
        if (value == 1) {
            /* Two keys at once: remember the combination, StickyKeys may switch off when it ends. */
            if (a->phys) { a->combo |= (uint8_t)(a->phys | BIT(m)); }
            else if (any_key_down(a)) a->combo |= (uint8_t)BIT(m);
            a->phys |= (uint8_t)BIT(m);
            if (a->locked & BIT(m)) {
                a->locked &= (uint8_t)~BIT(m);
                if (a->sent & BIT(m)) { emit(o, ACC_EV_KEY, code, 0, 0); a->sent &= (uint8_t)~BIT(m); }
                if (audible) tone(o, ACC_TONE_UNLATCH);
            } else if (a->latched & BIT(m)) {
                if (a->s.sticky & ACC_SKF_TRISTATE) {
                    a->latched &= (uint8_t)~BIT(m); a->locked |= (uint8_t)BIT(m);
                    if (audible) tone(o, ACC_TONE_LOCK);
                } else {
                    a->latched &= (uint8_t)~BIT(m);
                    if (a->sent & BIT(m)) { emit(o, ACC_EV_KEY, code, 0, 0); a->sent &= (uint8_t)~BIT(m); }
                    if (audible) tone(o, ACC_TONE_UNLATCH);
                }
            } else {
                if (!(a->sent & BIT(m))) { emit(o, ACC_EV_KEY, code, 1, 0); a->sent |= (uint8_t)BIT(m); }
                a->latched |= (uint8_t)BIT(m);
                if (audible) tone(o, ACC_TONE_LATCH);
            }
            o->redraw = 1;
            return;
        }
        /* release */
        a->phys &= (uint8_t)~BIT(m);
        int combo = !!(a->combo & BIT(m)); a->combo &= (uint8_t)~BIT(m);
        if (combo && (a->s.sticky & ACC_SKF_TWOKEYSOFF)) { feature_switch(a, ACC_F_STICKY, 0, 0, o); return; }
        if (!((a->latched | a->locked) & BIT(m)) && (a->sent & BIT(m))) { emit(o, ACC_EV_KEY, code, 0, 0); a->sent &= (uint8_t)~BIT(m); }
        return;
    }
    emit(o, ACC_EV_KEY, code, value, 0);
    if (!on) return;
    if (value == 1) {
        a->combo |= a->phys;
        if (a->latched) { sticky_release_latched(a, o); o->redraw = 1; }
    }
}

/* ---- ToggleKeys and MouseKeys --------------------------------------------------- */
int access_mouse_mode(const struct access *a)
{
    if (!(a->s.mouse & ACC_ON)) return 0;
    int num = !!(a->locks & 2), replace = !!(a->s.mouse & ACC_MKF_REPLACENUMBERS);
    return num == replace;
}
static void toggle_observe(struct access *a, int code, int value, struct access_out *o)
{
    unsigned bit = code == KEY_CAPS ? 1u : code == KEY_NUMLOCK ? 2u : code == KEY_SCROLL ? 4u : 0u;
    if (!bit || value != 1) return;
    a->locks ^= (uint8_t)bit;
    if (a->s.toggle & ACC_ON) tone(o, (a->locks & bit) ? ACC_TONE_BEEP_HIGH : ACC_TONE_BEEP_LOW);
}
void access_set_locks(struct access *a, unsigned caps, unsigned num, unsigned scroll)
{ a->locks = (uint8_t)((caps ? 1u : 0u) | (num ? 2u : 0u) | (scroll ? 4u : 0u)); }
static int mk_direction(int code, int *dx, int *dy)
{
    static const struct { int code, dx, dy; } d[8] = {{0x47, -1, -1}, {0x48, 0, -1}, {0x49, 1, -1}, {0x4b, -1, 0},
                                                      {0x4d, 1, 0}, {0x4f, -1, 1}, {0x50, 0, 1}, {0x51, 1, 1}};
    for (unsigned i = 0; i < 8; i++) if (d[i].code == code) { *dx = d[i].dx; *dy = d[i].dy; return (int)i; }
    return -1;
}
static void mk_buttons(struct access *a, struct access_out *o, int value)
{
    /* Both buttons act as the middle button of a three-button mouse (Resource Kit, appendix H). */
    if (a->mk_button == 3) emit(o, ACC_EV_BUTTON, 3, value, 0);
    else emit(o, ACC_EV_BUTTON, a->mk_button == 2 ? 2 : 1, value, 0);
}
static void mk_step(struct access_out *o, int dx, int dy)
{
    if (!dx && !dy) return;
    emit(o, ACC_EV_MOTION, 0, dx, dy); o->mouse_keys_moved = 1;
}
static int mousekeys_key(struct access *a, uint64_t now, int code, int value, struct access_out *o)
{
    if (!access_mouse_mode(a)) return 0;
    int dx = 0, dy = 0, dir = mk_direction(code, &dx, &dy);
    if (dir >= 0) {
        if (value == 1) {
            if (!a->mk_dir) { a->mk_since = now; a->mk_last = now; a->mk_acc_x = a->mk_acc_y = 0; }
            a->mk_dir |= (uint8_t)BIT(dir);
            mk_step(o, dx, dy);   /* one pixel at once, then the held-key acceleration */
        } else if (value == 0) {
            a->mk_dir &= (uint8_t)~BIT(dir);
        }
        return 1;
    }
    switch (code) {
    case 0x4c:   /* 5: click */
        if (value == 1) {
            if (a->mk_drag) { mk_buttons(a, o, 0); a->mk_drag = 0; }
            mk_buttons(a, o, 1); mk_buttons(a, o, 0);
        }
        return 1;
    case 0x4e:   /* +: double click */
        if (value == 1) {
            if (a->mk_drag) { mk_buttons(a, o, 0); a->mk_drag = 0; }
            for (unsigned i = 0; i < 2; i++) { mk_buttons(a, o, 1); mk_buttons(a, o, 0); }
        }
        return 1;
    case 0x52:   /* Insert: begin dragging */
        if (value == 1 && !a->mk_drag) { mk_buttons(a, o, 1); a->mk_drag = 1; o->redraw = 1; }
        return 1;
    case 0x53:   /* Delete: release */
        if (value == 1 && a->mk_drag) { mk_buttons(a, o, 0); a->mk_drag = 0; o->redraw = 1; }
        return 1;
    case 0x135: case 0x4a: case 0x37:   /* / - * select the left, right or both buttons */
        if (value == 1) {
            unsigned want = code == 0x135 ? 1u : code == 0x4a ? 2u : 3u;
            if (a->mk_drag && want != a->mk_button) { mk_buttons(a, o, 0); a->mk_drag = 0; }
            a->mk_button = (uint8_t)want; o->redraw = 1;
        }
        return 1;
    default: return 0;
    }
}
static unsigned mk_speed(const struct access *a, uint64_t now)
{
    uint64_t held = now - a->mk_since;
    uint8_t eff = effective(a);
    unsigned max = a->s.mouse_max ? a->s.mouse_max : 80, time = a->s.mouse_time ? a->s.mouse_time : 3000;
    if (a->s.mouse & ACC_MKF_MODIFIERS) {
        if (eff & (BIT(ACC_M_LSHIFT) | BIT(ACC_M_RSHIFT))) return 6;   /* single pixels, with a pause between them */
    }
    if (held < 250 * MS) return 0;
    unsigned start = max / 10 < 8 ? 8 : max / 10;
    uint64_t ramp = held - 250 * MS, span = (uint64_t)time * MS;
    unsigned v = ramp >= span ? max : start + (unsigned)((uint64_t)(max - start) * ramp / span);
    if ((a->s.mouse & ACC_MKF_MODIFIERS) && (eff & (BIT(ACC_M_LCTRL) | BIT(ACC_M_RCTRL))) && a->s.mouse_ctrl > 1)
        v *= a->s.mouse_ctrl;
    return v;
}
static void mousekeys_tick(struct access *a, uint64_t now, struct access_out *o)
{
    if (!a->mk_dir || !(a->s.mouse & ACC_ON)) return;
    if (now < a->mk_last + TICK_NS) return;
    uint64_t dt = now - a->mk_last > 100 * MS ? 100 * MS : now - a->mk_last;
    a->mk_last = now;
    unsigned v = mk_speed(a, now);
    if (!v) return;
    int dx = 0, dy = 0;
    for (unsigned i = 0; i < 8; i++) if (a->mk_dir & BIT(i)) {
        static const signed char vx[8] = {-1, 0, 1, -1, 1, -1, 0, 1}, vy[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
        dx += vx[i]; dy += vy[i];
    }
    dx = dx > 1 ? 1 : dx < -1 ? -1 : dx; dy = dy > 1 ? 1 : dy < -1 ? -1 : dy;
    int64_t milli = (int64_t)v * (int64_t)(dt / MS);          /* milli-pixels this tick */
    if (dx && dy) milli = milli * 181 / 256;
    a->mk_acc_x += (int32_t)(milli * dx); a->mk_acc_y += (int32_t)(milli * dy);
    int px = a->mk_acc_x / 1000, py = a->mk_acc_y / 1000;
    a->mk_acc_x -= px * 1000; a->mk_acc_y -= py * 1000;
    mk_step(o, px, py);
}

/* ---- FilterKeys -------------------------------------------------------------------- */
static void accepted(struct access *a, uint64_t now, int code, int value, struct access_out *o)
{
    if (mousekeys_key(a, now, code, value, o)) return;
    toggle_observe(a, code, value, o);
    sticky_key(a, code, value, o);
}
static struct access_slot *slot_find(struct access *a, int code)
{
    for (unsigned i = 0; i < ACC_SLOTS; i++) if (a->slot[i].used && a->slot[i].code == code) return &a->slot[i];
    return NULL;
}
static struct access_slot *slot_new(struct access *a, int code, uint64_t now)
{
    struct access_slot *pick = NULL;
    for (unsigned i = 0; i < ACC_SLOTS; i++) if (!a->slot[i].used) { pick = &a->slot[i]; break; }
    if (!pick) {
        pick = &a->slot[0];
        for (unsigned i = 1; i < ACC_SLOTS; i++) if (a->slot[i].pressed < pick->pressed) pick = &a->slot[i];
    }
    memset(pick, 0, sizeof(*pick));
    pick->used = 1; pick->code = code; pick->pressed = now;
    return pick;
}
static int released_recently(const struct access *a, int code, uint64_t now, uint64_t window)
{
    for (unsigned i = 0; i < ACC_RELEASES; i++)
        if (a->released[i].at && a->released[i].code == code && now >= a->released[i].at && now - a->released[i].at < window) return 1;
    return 0;
}
static void release_note(struct access *a, int code, uint64_t now)
{
    for (unsigned i = 0; i < ACC_RELEASES; i++) if (a->released[i].code == code) { a->released[i].at = now ? now : 1; return; }
    a->released[a->release_next % ACC_RELEASES] = (struct access_release){code, now ? now : 1};
    a->release_next++;
}
static void filter_key(struct access *a, uint64_t now, int code, int value, struct access_out *o)
{
    if (!(a->s.filter & ACC_ON)) { accepted(a, now, code, value, o); return; }
    uint32_t f = a->s.filter;
    int bounce = a->s.filter_bounce > 0, click = !!(f & ACC_FKF_CLICK), norepeat = !!(f & ACC_FKF_NOREPEAT);
    struct access_slot *s = slot_find(a, code);
    if (value == 1) {
        if (s) { memset(s, 0, sizeof(*s)); s = NULL; }
        s = slot_new(a, code, now);
        if (click) tone(o, ACC_TONE_CLICK);
        if (bounce) {
            if (released_recently(a, code, now, (uint64_t)a->s.filter_bounce * MS)) { s->ignored = 1; return; }
            s->accepted = 1; s->accepted_at = now;
            accepted(a, now, code, 1, o);
            return;
        }
        if (a->s.filter_wait > 0) { s->pending = 1; return; }
        s->accepted = 1; s->accepted_at = now;
        accepted(a, now, code, 1, o);
        return;
    }
    if (value == 2) {
        if (!s) { accepted(a, now, code, 2, o); return; }
        if (s->ignored || s->pending) return;
        if (norepeat) return;
        if (bounce) { accepted(a, now, code, 2, o); return; }
        uint64_t since = s->last_repeat ? s->last_repeat : s->accepted_at;
        uint64_t need = (s->last_repeat ? a->s.filter_repeat : a->s.filter_delay) * MS;
        if (now - since < need) return;
        s->last_repeat = now;
        if (click) tone(o, ACC_TONE_CLICK);
        accepted(a, now, code, 2, o);
        return;
    }
    if (!s) { accepted(a, now, code, 0, o); return; }
    int pass = !s->ignored && !s->pending;
    memset(s, 0, sizeof(*s));
    if (bounce) release_note(a, code, now);
    if (pass) accepted(a, now, code, 0, o);
}

/* ---- hot keys ------------------------------------------------------------------------ */
static int hotkeys(struct access *a, uint64_t now, int code, int value, struct access_out *o)
{
    int m = mod_of(code);
    if (swallowed(a, code, value)) return 1;
    if (value != 1) {
        if (value == 0 && code == KEY_RSHIFT) a->rshift_on = 0;
        if (value == 0 && code == KEY_NUMLOCK) a->numlock_on = 0;
        return 0;
    }
    uint8_t eff = effective(a);
    /* Any other key press ends a hold and the run of Shift presses. */
    if (code != KEY_RSHIFT) a->rshift_on = 0;
    if (code != KEY_NUMLOCK) a->numlock_on = 0;
    if (m != ACC_M_LSHIFT && m != ACC_M_RSHIFT) a->shift_run = 0;
    if ((m == ACC_M_LSHIFT || m == ACC_M_RSHIFT) && (a->s.sticky & ACC_HOTKEY)) {
        if (++a->shift_run >= 5) {
            a->shift_run = 0;
            feature_switch(a, ACC_F_STICKY, !(a->s.sticky & ACC_ON), 1, o);
            swallow_add(a, code);
            a->combo = 0;
            return 1;
        }
    } else if (m == ACC_M_LSHIFT || m == ACC_M_RSHIFT) a->shift_run = 0;
    if (code == KEY_RSHIFT && (a->s.filter & ACC_HOTKEY)) { a->rshift_on = 1; a->rshift_since = now; a->rshift_stage = 0; }
    uint8_t alt_shift = (uint8_t)(BIT(ACC_M_LALT) | BIT(ACC_M_LSHIFT));
    if (code == KEY_NUMLOCK) {
        if ((a->s.mouse & ACC_HOTKEY) && (eff & alt_shift) == alt_shift) {
            feature_switch(a, ACC_F_MOUSE, !(a->s.mouse & ACC_ON), 1, o);
            swallow_add(a, code); sticky_release_latched(a, o);
            return 1;
        }
        if (a->s.toggle & ACC_HOTKEY) { a->numlock_on = 1; a->numlock_since = now; }
    }
    if ((code == KEY_PRINT || code == KEY_SYSRQ) && (a->s.contrast & ACC_HOTKEY) && (eff & alt_shift) == alt_shift) {
        feature_switch(a, ACC_F_CONTRAST, !(a->s.contrast & ACC_ON), 1, o);
        swallow_add(a, code); sticky_release_latched(a, o);
        return 1;
    }
    return 0;
}

/* ---- the engine entry points ---------------------------------------------------------- */
void access_note_input(struct access *a, uint64_t now) { a->last_input_ns = now; }
void access_motion(struct access *a, uint64_t now) { a->last_input_ns = now; }
void access_resync(struct access *a)
{
    a->phys = a->sent = a->latched = a->locked = a->combo = 0;
    a->shift_run = a->rshift_on = a->numlock_on = 0;
    memset(a->down, 0, sizeof(a->down)); memset(a->slot, 0, sizeof(a->slot)); memset(a->swallow_code, 0, sizeof(a->swallow_code));
    a->mk_dir = 0;
}
void access_init(struct access *a, const struct access_settings *s, uint64_t now)
{
    memset(a, 0, sizeof(*a));
    a->s = *s; a->last_input_ns = now;
    a->mk_button = 1;
}
void access_configure(struct access *a, const struct access_settings *s, struct access_out *o)
{
    struct access_settings old = a->s;
    a->s = *s;
    for (unsigned f = ACC_F_STICKY; f < ACC_F_COUNT; f++) {
        uint32_t *was = flags_in(&old, f), *now = flags_of(a, f);
        if (!!(*was & ACC_ON) != !!(*now & ACC_ON)) {
            o->redraw = 1;
            if (f == ACC_F_CONTRAST) o->contrast_changed = 1;
            if (!(*now & ACC_ON)) feature_off_effects(a, f, o);
            else if (f == ACC_F_STICKY) a->latched = a->locked = 0;
            else if (f == ACC_F_MOUSE) { a->mk_dir = 0; a->mk_drag = 0; if (!a->mk_button) a->mk_button = 1; }
        }
    }
    if (old.scheme[0] != s->scheme[0] || strcmp(old.scheme, s->scheme)) { if (s->contrast & ACC_ON) o->contrast_changed = 1; }
    if (old.pointer != s->pointer) o->redraw = 1;
}
void access_key(struct access *a, uint64_t now, int code, int value, struct access_out *o)
{
    if (code < 0 || code >= ACC_KEYS || value < 0 || value > 2) return;
    a->last_input_ns = now;
    if (hotkeys(a, now, code, value, o)) return;
    filter_key(a, now, code, value, o);
    if (mod_of(code) < 0 && value != 2) key_mark(a, code, value != 0);
}
void access_button(struct access *a, uint64_t now, int button, int value, struct access_out *o)
{
    (void)button;
    a->last_input_ns = now;
    /* A click ends the latch of a modifier (STICKYKEYS: "until the user presses a non-modifier key or clicks a mouse button"). */
    if (!value && (a->s.sticky & ACC_ON) && a->latched) { sticky_release_latched(a, o); o->redraw = 1; }
}

static void hold_tick(struct access *a, uint64_t now, struct access_out *o)
{
    if (a->rshift_on) {
        uint64_t held = now - a->rshift_since;
        if (a->rshift_stage < 1 && held >= 4000 * MS) {
            a->rshift_stage = 1;
            if (a->s.filter & ACC_SOUND) tone(o, ACC_TONE_WARNING);
        }
        if (a->rshift_stage < 2 && held >= 8000 * MS) {
            a->rshift_stage = 2;
            feature_switch(a, ACC_F_FILTER, !(a->s.filter & ACC_ON), 1, o);
            if (!(a->s.filter & ACC_ON)) a->rshift_on = 0;
        }
        if (a->rshift_on && a->rshift_stage < 3 && held >= 12000 * MS) {
            /* First emergency level: no repeats, no acceptance delay, repeated keystrokes ignored. */
            a->rshift_stage = 3;
            a->s.filter_wait = a->s.filter_delay = a->s.filter_repeat = 0; a->s.filter_bounce = 500;
            a->s.filter |= ACC_FKF_NOREPEAT; o->changed |= BIT(ACC_F_FILTER); o->redraw = 1;
            memset(a->slot, 0, sizeof(a->slot));
            tone(o, ACC_TONE_SIREN_UP2);
        }
        if (a->rshift_on && a->rshift_stage < 4 && held >= 16000 * MS) {
            /* Second level: no repeats, a two-second acceptance delay. */
            a->rshift_stage = 4; a->rshift_on = 0;
            a->s.filter_bounce = 0; a->s.filter_wait = 2000; a->s.filter_delay = a->s.filter_repeat = 0;
            a->s.filter |= ACC_FKF_NOREPEAT; o->changed |= BIT(ACC_F_FILTER); o->redraw = 1;
            memset(a->slot, 0, sizeof(a->slot));
            tone(o, ACC_TONE_SIREN_UP3);
        }
    }
    if (a->numlock_on && now - a->numlock_since >= 5000 * MS) {
        a->numlock_on = 0;
        feature_switch(a, ACC_F_TOGGLE, !(a->s.toggle & ACC_ON), 1, o);
    }
}
void access_tick(struct access *a, uint64_t now, struct access_out *o)
{
    if (a->s.filter & ACC_ON) for (;;) {
        struct access_slot *pick = NULL;
        for (unsigned i = 0; i < ACC_SLOTS; i++) {
            struct access_slot *s = &a->slot[i];
            if (s->used && s->pending && now >= s->pressed + (uint64_t)a->s.filter_wait * MS && (!pick || s->pressed < pick->pressed)) pick = s;
        }
        if (!pick) break;
        pick->pending = 0; pick->accepted = 1; pick->accepted_at = now;
        if (a->s.filter & ACC_FKF_CLICK) tone(o, ACC_TONE_CLICK);
        accepted(a, now, pick->code, 1, o);
    }
    hold_tick(a, now, o);
    mousekeys_tick(a, now, o);
    if ((a->s.timeout & ACC_ATF_ON) && a->s.timeout_ms && now >= a->last_input_ns + (uint64_t)a->s.timeout_ms * MS) {
        int any = 0;
        for (unsigned f = ACC_F_STICKY; f < ACC_F_COUNT; f++) if (access_feature_on(a, f)) { feature_switch(a, f, 0, 0, o); any = 1; }
        if (any && (a->s.timeout & ACC_ATF_FEEDBACK)) tone(o, ACC_TONE_SIREN_DOWN);
        a->last_input_ns = now;
    }
}
uint64_t access_wake_ns(const struct access *a, uint64_t now)
{
    uint64_t best = 0;
#define WAKE(t) do { uint64_t _t = (t); if (_t <= now) _t = now + 1; if (!best || _t < best) best = _t; } while (0)
    if (a->s.filter & ACC_ON)
        for (unsigned i = 0; i < ACC_SLOTS; i++) if (a->slot[i].used && a->slot[i].pending)
            WAKE(a->slot[i].pressed + (uint64_t)a->s.filter_wait * MS);
    if (a->rshift_on) {
        static const unsigned stage_ms[4] = {4000, 8000, 12000, 16000};
        if (a->rshift_stage < 4) WAKE(a->rshift_since + (uint64_t)stage_ms[a->rshift_stage] * MS);
    }
    if (a->numlock_on) WAKE(a->numlock_since + 5000 * MS);
    if (a->mk_dir && (a->s.mouse & ACC_ON)) WAKE(a->mk_last + TICK_NS);
    if ((a->s.timeout & ACC_ATF_ON) && a->s.timeout_ms) {
        int any = 0;
        for (unsigned f = ACC_F_STICKY; f < ACC_F_COUNT; f++) any |= access_feature_on(a, f);
        if (any) WAKE(a->last_input_ns + (uint64_t)a->s.timeout_ms * MS);
    }
#undef WAKE
    return best;
}

unsigned access_indicator(const struct access *a)
{
    unsigned bits = 0;
    for (unsigned f = ACC_F_STICKY; f < ACC_F_COUNT; f++) if (access_feature_on(a, f)) bits |= BIT(f);
    uint8_t held = (uint8_t)(a->latched | a->phys), locked = a->locked;
    if (access_feature_on(a, ACC_F_STICKY)) {
        if (a->latched & (BIT(ACC_M_LSHIFT) | BIT(ACC_M_RSHIFT))) bits |= ACC_IND_SHIFT;
        if (a->latched & (BIT(ACC_M_LCTRL) | BIT(ACC_M_RCTRL))) bits |= ACC_IND_CTRL;
        if (a->latched & (BIT(ACC_M_LALT) | BIT(ACC_M_RALT))) bits |= ACC_IND_ALT;
        if (locked & (BIT(ACC_M_LSHIFT) | BIT(ACC_M_RSHIFT))) bits |= ACC_IND_LOCK_SHIFT;
        if (locked & (BIT(ACC_M_LCTRL) | BIT(ACC_M_RCTRL))) bits |= ACC_IND_LOCK_CTRL;
        if (locked & (BIT(ACC_M_LALT) | BIT(ACC_M_RALT))) bits |= ACC_IND_LOCK_ALT;
        (void)held;
    }
    if (access_feature_on(a, ACC_F_MOUSE)) {
        if (a->mk_button & 1) bits |= ACC_IND_LEFT;
        if (a->mk_button & 2) bits |= ACC_IND_RIGHT;
        if (a->mk_drag) bits |= (a->mk_button & 1 ? ACC_IND_LEFT_HELD : 0) | (a->mk_button & 2 ? ACC_IND_RIGHT_HELD : 0);
    }
    if (access_feature_on(a, ACC_F_FILTER)) bits |= ACC_IND_STOPWATCH;
    return bits;
}
int access_indicator_visible(const struct access *a)
{
    return a->s.status && (access_feature_on(a, ACC_F_STICKY) || access_feature_on(a, ACC_F_FILTER) || access_feature_on(a, ACC_F_MOUSE));
}

/* ---- Magnifier ------------------------------------------------------------------------ */
struct access_mag_box access_mag_panel(int sw, int sh, int top, int bottom, unsigned percent, unsigned dock, int flipped)
{
    if (percent < ACC_MAG_PERCENT_MIN) percent = ACC_MAG_PERCENT_MIN;
    if (percent > ACC_MAG_PERCENT_MAX) percent = ACC_MAG_PERCENT_MAX;
    int h = sh * (int)percent / 100;
    int usable = sh - top - bottom;
    if (h > usable / 2) h = usable / 2;
    if (h < 32) h = usable > 32 ? 32 : usable > 0 ? usable : 1;
    int at_bottom = (dock == ACC_DOCK_BOTTOM) != (flipped != 0);
    return (struct access_mag_box){0, at_bottom ? sh - bottom - h : top, sw, h};
}
static int boxes_overlap(struct access_mag_box a, struct access_mag_box b)
{ return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h; }
static void mag_size(struct access_mag_box panel, unsigned level, int *w, int *h)
{
    if (level < ACC_MAG_LEVEL_MIN) level = ACC_MAG_LEVEL_MIN;
    if (level > ACC_MAG_LEVEL_MAX) level = ACC_MAG_LEVEL_MAX;
    *w = panel.w / (int)level; *h = panel.h / (int)level;
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
}
int access_mag_source(int sw, int sh, struct access_mag_box panel, unsigned level, int cx, int cy, struct access_mag_box *src)
{
    int w, h; mag_size(panel, level, &w, &h);
    if (w > sw) w = sw;
    int lo = 0, hi = sh;                    /* the vertical range the source may use */
    if (panel.y + panel.h / 2 < sh / 2) lo = panel.y + panel.h; else hi = panel.y;
    if (hi - lo < h) return 0;
    int x = cx - w / 2, y = cy - h / 2;
    if (x < 0) x = 0;
    if (x > sw - w) x = sw - w;
    if (y < lo) y = lo;
    if (y > hi - h) y = hi - h;
    *src = (struct access_mag_box){x, y, w, h};
    return 1;
}
unsigned access_mag_side(int sw, int sh, int top, int bottom, unsigned percent, unsigned preferred, unsigned level, int cx, int cy)
{
    int w, h;
    struct access_mag_box want = access_mag_panel(sw, sh, top, bottom, percent, preferred, 0);
    mag_size(want, level, &w, &h);
    struct access_mag_box around = {cx - w / 2, cy - h / 2, w, h};
    if (!boxes_overlap(around, want)) return preferred;
    struct access_mag_box other = access_mag_panel(sw, sh, top, bottom, percent, preferred, 1);
    if (!boxes_overlap(around, other)) return preferred == ACC_DOCK_TOP ? ACC_DOCK_BOTTOM : ACC_DOCK_TOP;
    return preferred;
}
void access_mag_paint(uint32_t *dst, size_t dst_stride, struct access_mag_box panel, const uint32_t *screen, size_t screen_stride,
                      struct access_mag_box src, unsigned level, int invert, uint32_t border)
{
    if (level < 1) level = 1;
    for (int y = 0; y < panel.h; y++) {
        uint32_t *row = dst + (size_t)(panel.y + y) * dst_stride + (size_t)panel.x;
        int sy = src.y + y / (int)level;
        int row_ok = y / (int)level < src.h;
        for (int x = 0; x < panel.w; x++) {
            int sx = src.x + x / (int)level;
            uint32_t v = border;
            if (row_ok && x / (int)level < src.w) {
                v = screen[(size_t)sy * screen_stride + (size_t)sx];
                if (invert) v = ~v & 0xffffffu;
            }
            row[x] = v;
        }
    }
}
