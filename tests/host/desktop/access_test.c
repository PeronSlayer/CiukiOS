/* SPDX-License-Identifier: MIT
 * G25: the accessibility engine against the Windows 98 behaviour of the
 * Resource Kit (appendix H) and the Win32 documentation. */
#include "../../../apps/desktop/access.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define MS 1000000ull
static struct access A;
static struct access_out O;
static uint64_t now;
static struct access_settings base;

static void fresh(void)
{
    access_defaults(&base);
    memset(&A, 0, sizeof(A)); memset(&O, 0, sizeof(O));
    now = 1000 * MS;
    access_init(&A, &base, now);
}
static void clear(void) { memset(&O, 0, sizeof(O)); }
static void key(int code, int value) { clear(); access_key(&A, now, code, value, &O); }
static void tap(int code) { key(code, 1); key(code, 0); }
static void tick(void) { clear(); access_tick(&A, now, &O); }
static void advance(unsigned ms) { now += (uint64_t)ms * MS; }
static int has_tone(unsigned t) { for (unsigned i = 0; i < O.tones; i++) if (O.tone[i] == t) return 1; return 0; }
static int ev_is(unsigned i, int code, int value) { return i < O.count && O.ev[i].type == ACC_EV_KEY && O.ev[i].code == code && O.ev[i].value == value; }

/* Event log across several calls, to check delivery order. */
static struct { int code, value; } log_[64]; static unsigned log_n;
static void log_reset(void) { log_n = 0; }
static void log_take(void)
{
    for (unsigned i = 0; i < O.count; i++) if (O.ev[i].type == ACC_EV_KEY && log_n < 64) { log_[log_n].code = O.ev[i].code; log_[log_n].value = O.ev[i].value; log_n++; }
}
static void kk(int code, int value) { key(code, value); log_take(); }
static void expect(unsigned n, const int (*want)[2])
{
    if (log_n != n) { fprintf(stderr, "log has %u events, wanted %u:", log_n, n); for (unsigned i = 0; i < log_n; i++) fprintf(stderr, " %x/%d", log_[i].code, log_[i].value); fputc('\n', stderr); }
    assert(log_n == n);
    for (unsigned i = 0; i < n; i++) {
        if (log_[i].code != want[i][0] || log_[i].value != want[i][1]) fprintf(stderr, "event %u is %x/%d, wanted %x/%d\n", i, log_[i].code, log_[i].value, want[i][0], want[i][1]);
        assert(log_[i].code == want[i][0] && log_[i].value == want[i][1]);
    }
}
#define LSHIFT 0x2a
#define RSHIFT 0x36
#define LCTRL 0x1d
#define LALT 0x38
#define KEY_A 0x1e
#define KEY_B 0x30
#define KEY_C 0x2e
#define NUMLOCK 0x45
#define CAPS 0x3a
#define PRTSC 0x137

static void sticky_on(void)
{
    struct access_settings s = A.s; s.sticky |= ACC_ON;
    struct access_out o = {0}; access_configure(&A, &s, &o);
}

static void defaults(void)
{
    fresh();
    assert(base.sticky == 0x1fe && base.filter == 0x7e && base.toggle == 0x3e && base.mouse == 0x7e && base.contrast == 0x7e);
    assert(base.filter_wait == 1000 && base.filter_delay == 1000 && base.filter_repeat == 500 && base.filter_bounce == 0);
    assert(base.mouse_max == 80 && base.mouse_time == 3000 && base.mouse_ctrl == 8 && base.timeout_ms == 300000);
    assert(access_scheme_valid("High Contrast Black") && !access_scheme_valid("Nope"));
    for (unsigned f = ACC_F_STICKY; f < ACC_F_COUNT; f++) assert(!access_feature_on(&A, f));
    assert(!access_indicator_visible(&A) && access_wake_ns(&A, now) == 0);
    /* Nothing is filtered while every feature is off. */
    log_reset(); kk(KEY_A, 1); kk(KEY_A, 2); kk(KEY_A, 0); kk(LSHIFT, 1); kk(LSHIFT, 0);
    const int want[][2] = {{KEY_A, 1}, {KEY_A, 2}, {KEY_A, 0}, {LSHIFT, 1}, {LSHIFT, 0}};
    expect(5, want);
}

static void sticky_hotkey(void)
{
    fresh();
    /* Shift five times: the first four presses pass, the fifth switches StickyKeys on with a siren and asks. */
    for (int i = 0; i < 4; i++) { tap(i & 1 ? RSHIFT : LSHIFT); assert(!access_feature_on(&A, ACC_F_STICKY)); }
    assert(!O.changed);
    key(LSHIFT, 1);
    assert(access_feature_on(&A, ACC_F_STICKY));
    assert(has_tone(ACC_TONE_SIREN_UP) && O.confirm == ACC_F_STICKY && O.confirm_on && (O.changed & (1u << ACC_F_STICKY)) && O.redraw);
    assert(O.count == 0);   /* the press itself is consumed */
    key(LSHIFT, 0); assert(O.count == 0);
    assert(access_indicator_visible(&A) && (access_indicator(&A) & (1u << ACC_F_STICKY)));
    /* Declining the confirmation switches it off again. */
    access_confirm(&A, ACC_F_STICKY, 0, &O);
    assert(!access_feature_on(&A, ACC_F_STICKY));
    /* A key between the presses restarts the count. */
    fresh();
    for (int i = 0; i < 3; i++) tap(LSHIFT);
    tap(KEY_A);
    for (int i = 0; i < 4; i++) tap(LSHIFT);
    assert(!access_feature_on(&A, ACC_F_STICKY));
    tap(LSHIFT); assert(access_feature_on(&A, ACC_F_STICKY));
    /* Keeping it (OK) leaves it on and clears the pending state. */
    access_confirm(&A, ACC_F_STICKY, 1, &O); assert(access_feature_on(&A, ACC_F_STICKY) && !A.pending_confirm);
    /* Hot key disabled: nothing happens. */
    fresh(); { struct access_settings s = A.s; s.sticky &= ~ACC_HOTKEY; struct access_out o = {0}; access_configure(&A, &s, &o); }
    for (int i = 0; i < 6; i++) tap(LSHIFT);
    assert(!access_feature_on(&A, ACC_F_STICKY));
    /* No siren and no dialog without SOUND and CONFIRM. */
    fresh(); { struct access_settings s = A.s; s.sticky &= ~(ACC_SOUND | ACC_CONFIRM); struct access_out o = {0}; access_configure(&A, &s, &o); }
    for (int i = 0; i < 5; i++) tap(RSHIFT);
    assert(access_feature_on(&A, ACC_F_STICKY) && !has_tone(ACC_TONE_SIREN_UP) && !O.confirm);
    /* Five presses switch it off again with a falling siren. */
    fresh(); sticky_on();
    for (int i = 0; i < 5; i++) { key(LSHIFT, 1); key(LSHIFT, 0); }
    assert(!access_feature_on(&A, ACC_F_STICKY));
}

static void sticky_latch_and_lock(void)
{
    fresh(); sticky_on();
    /* Latched: Shift, a, then the modifier is released after the key. */
    log_reset();
    kk(LSHIFT, 1); assert(has_tone(ACC_TONE_LATCH)); kk(LSHIFT, 0); kk(KEY_A, 1); kk(KEY_A, 0);
    { const int want[][2] = {{LSHIFT, 1}, {KEY_A, 1}, {LSHIFT, 0}, {KEY_A, 0}}; expect(4, want); }
    assert(!A.latched);
    kk(KEY_B, 1); kk(KEY_B, 0);   /* unshifted */
    assert(log_n == 6 && log_[4].code == KEY_B && log_[5].code == KEY_B);
    /* Locked: pressing the modifier twice keeps it down for every key until the third press. */
    fresh(); sticky_on(); log_reset();
    kk(LSHIFT, 1); kk(LSHIFT, 0);
    kk(LSHIFT, 1); assert(has_tone(ACC_TONE_LOCK)); kk(LSHIFT, 0);
    assert(A.locked & 1u);
    log_reset();
    kk(KEY_A, 1); kk(KEY_A, 0); kk(KEY_B, 1); kk(KEY_B, 0);
    { const int want[][2] = {{KEY_A, 1}, {KEY_A, 0}, {KEY_B, 1}, {KEY_B, 0}}; expect(4, want); }
    assert(access_indicator(&A) & ACC_IND_LOCK_SHIFT);
    kk(LSHIFT, 1); assert(has_tone(ACC_TONE_UNLATCH));
    { const int want[][2] = {{KEY_A, 1}, {KEY_A, 0}, {KEY_B, 1}, {KEY_B, 0}, {LSHIFT, 0}}; expect(5, want); }
    kk(LSHIFT, 0);
    assert(!A.locked && !A.sent);
    /* Without "press twice to lock" a second press unlatches. */
    fresh(); { struct access_settings s = A.s; s.sticky |= ACC_ON; s.sticky &= ~ACC_SKF_TRISTATE; struct access_out o = {0}; access_configure(&A, &s, &o); }
    log_reset(); kk(LCTRL, 1); kk(LCTRL, 0); kk(LCTRL, 1); kk(LCTRL, 0);
    { const int want[][2] = {{LCTRL, 1}, {LCTRL, 0}}; expect(2, want); }
    /* Several modifiers latch together and all release after the key. */
    fresh(); sticky_on(); log_reset();
    kk(LCTRL, 1); kk(LCTRL, 0); kk(LALT, 1); kk(LALT, 0); kk(KEY_A, 1);
    { const int want[][2] = {{LCTRL, 1}, {LALT, 1}, {KEY_A, 1}, {LCTRL, 0}, {LALT, 0}}; expect(5, want); }
    /* A modifier still held physically stays down and releases with the finger. */
    fresh(); sticky_on(); log_reset();
    kk(LSHIFT, 1); kk(KEY_A, 1); kk(KEY_A, 0); kk(KEY_B, 1); kk(KEY_B, 0); kk(LSHIFT, 0);
    assert(log_[log_n - 1].code == LSHIFT && log_[log_n - 1].value == 0);
    /* The Windows keys are not sticky (Resource Kit: Shift, Ctrl and Alt). */
    fresh(); sticky_on(); log_reset(); kk(0x15b, 1); kk(0x15b, 0);
    { const int want[][2] = {{0x15b, 1}, {0x15b, 0}}; expect(2, want); }
    assert(!A.latched);
    /* Repeats of a modifier are swallowed. */
    fresh(); sticky_on(); key(LSHIFT, 1); key(LSHIFT, 2); assert(O.count == 0);
}

static void sticky_two_keys_and_click(void)
{
    fresh(); sticky_on();
    /* Holding Shift while pressing a key, then releasing Shift, switches StickyKeys off (SKF_TWOKEYSOFF). */
    key(LSHIFT, 1); key(KEY_A, 1); key(KEY_A, 0); key(LSHIFT, 0);
    assert(!access_feature_on(&A, ACC_F_STICKY));
    assert(O.changed & (1u << ACC_F_STICKY));
    /* Without the option it stays on. */
    fresh(); { struct access_settings s = A.s; s.sticky |= ACC_ON; s.sticky &= ~ACC_SKF_TWOKEYSOFF; struct access_out o = {0}; access_configure(&A, &s, &o); }
    key(LSHIFT, 1); key(KEY_A, 1); key(KEY_A, 0); key(LSHIFT, 0);
    assert(access_feature_on(&A, ACC_F_STICKY));
    /* A mouse click releases a latched modifier; a lock stays. */
    fresh(); sticky_on(); tap(LCTRL); assert(A.latched);
    clear(); access_button(&A, now, 1, 1, &O); assert(O.count == 0 && A.latched);
    clear(); access_button(&A, now, 1, 0, &O); assert(O.count == 1 && ev_is(0, LCTRL, 0) && !A.latched);
    /* Turning the feature off from the panel releases what is held. */
    fresh(); sticky_on(); tap(LSHIFT); tap(LCTRL); tap(LCTRL);
    assert(A.latched && A.locked);
    { struct access_settings s = A.s; s.sticky &= ~ACC_ON; clear(); access_configure(&A, &s, &O); }
    assert(!A.latched && !A.locked && O.count == 2);
}

static void filter_slow(void)
{
    fresh();
    { struct access_settings s = A.s; s.filter |= ACC_ON; s.filter_wait = 1000; s.filter_delay = 1000; s.filter_repeat = 500; struct access_out o = {0}; access_configure(&A, &s, &o); }
    key(KEY_A, 1); assert(O.count == 0);
    advance(999); tick(); assert(O.count == 0);
    assert(access_wake_ns(&A, now) == now + 1 * MS);
    advance(1); tick(); assert(O.count == 1 && ev_is(0, KEY_A, 1));
    key(KEY_A, 0); assert(O.count == 1 && ev_is(0, KEY_A, 0));
    /* A key released before the acceptance delay never happened. */
    key(KEY_B, 1); advance(500); tick(); key(KEY_B, 0); assert(O.count == 0);
    advance(2000); tick(); assert(O.count == 0);
    /* Hardware repeats during the delay are dropped. */
    key(KEY_B, 1); key(KEY_B, 2); assert(O.count == 0); key(KEY_B, 0);
    /* Beep when a key is pressed or accepted. */
    { struct access_settings s = A.s; s.filter |= ACC_FKF_CLICK; struct access_out o = {0}; access_configure(&A, &s, &o); }
    key(KEY_A, 1); assert(has_tone(ACC_TONE_CLICK));
    advance(1000); tick(); assert(has_tone(ACC_TONE_CLICK) && O.count == 1);
    key(KEY_A, 0);
}

static void filter_bounce(void)
{
    fresh();
    { struct access_settings s = A.s; s.filter |= ACC_ON; s.filter_wait = s.filter_delay = s.filter_repeat = 0; s.filter_bounce = 500; struct access_out o = {0}; access_configure(&A, &s, &o); }
    key(KEY_A, 1); assert(O.count == 1 && ev_is(0, KEY_A, 1));
    advance(50); key(KEY_A, 0); assert(O.count == 1);
    advance(200); key(KEY_A, 1); assert(O.count == 0);       /* a bounce: ignored */
    advance(10); key(KEY_A, 2); assert(O.count == 0);       /* the ignored press's repeats go with it */
    key(KEY_A, 0); assert(O.count == 0);
    key(KEY_B, 1); key(KEY_B, 2); assert(O.count == 1 && ev_is(0, KEY_B, 2));   /* repeats of an accepted key pass (RepeatKeys is off) */
    key(KEY_B, 0);
    advance(100); key(KEY_B, 1); assert(O.count == 0);       /* BounceKeys applies per key: B was released just now */
    advance(600); key(KEY_B, 0);
    advance(100); key(KEY_C, 1); assert(O.count == 1);       /* another key is not affected */
    key(KEY_C, 0);
    key(KEY_B, 0);
    advance(600); key(KEY_A, 1); assert(O.count == 1);       /* long enough after the last release */
    key(KEY_A, 0);
}

static void filter_repeat(void)
{
    fresh();
    { struct access_settings s = A.s; s.filter |= ACC_ON; s.filter_wait = 0; s.filter_delay = 1000; s.filter_repeat = 500; struct access_out o = {0}; access_configure(&A, &s, &o); }
    key(KEY_A, 1); assert(O.count == 1);
    advance(500); key(KEY_A, 2); assert(O.count == 0);
    advance(500); key(KEY_A, 2); assert(O.count == 1 && ev_is(0, KEY_A, 2));
    advance(200); key(KEY_A, 2); assert(O.count == 0);
    advance(300); key(KEY_A, 2); assert(O.count == 1);
    key(KEY_A, 0); assert(O.count == 1);
    /* Repeat switched off. */
    { struct access_settings s = A.s; s.filter |= ACC_FKF_NOREPEAT; struct access_out o = {0}; access_configure(&A, &s, &o); }
    key(KEY_A, 1); assert(O.count == 1);
    advance(5000); key(KEY_A, 2); assert(O.count == 0);
    key(KEY_A, 0); assert(O.count == 1);
}

static void filter_hotkey(void)
{
    fresh();
    /* Right Shift held: nothing before four seconds, three warning beeps at four, FilterKeys on at eight. */
    key(RSHIFT, 1); assert(O.count == 1);
    advance(3999); tick(); assert(!O.tones);
    assert(access_wake_ns(&A, now) == now + 1 * MS);
    advance(1); tick(); assert(has_tone(ACC_TONE_WARNING));
    advance(3999); tick(); assert(!O.tones && !access_feature_on(&A, ACC_F_FILTER));
    advance(1); tick(); assert(access_feature_on(&A, ACC_F_FILTER) && has_tone(ACC_TONE_SIREN_UP) && O.confirm == ACC_F_FILTER);
    assert(A.s.filter_wait == 1000);   /* previously chosen settings */
    advance(4000); tick(); assert(has_tone(ACC_TONE_SIREN_UP2));
    assert(A.s.filter_bounce > 0 && !A.s.filter_wait && (A.s.filter & ACC_FKF_NOREPEAT));
    advance(4000); tick(); assert(has_tone(ACC_TONE_SIREN_UP3));
    assert(A.s.filter_wait == 2000 && !A.s.filter_bounce && (A.s.filter & ACC_FKF_NOREPEAT));
    key(RSHIFT, 0);
    /* Released early: nothing. */
    fresh(); key(RSHIFT, 1); advance(7000); tick(); key(RSHIFT, 0); advance(5000); tick(); assert(!access_feature_on(&A, ACC_F_FILTER));
    /* Another key cancels the hold. */
    fresh(); key(RSHIFT, 1); advance(3000); key(KEY_A, 1); advance(6000); tick(); assert(!access_feature_on(&A, ACC_F_FILTER));
    /* Holding again with FilterKeys on switches it off with a falling siren. */
    fresh(); { struct access_settings s = A.s; s.filter |= ACC_ON; struct access_out o = {0}; access_configure(&A, &s, &o); }
    key(RSHIFT, 1); advance(8000); tick(); assert(!access_feature_on(&A, ACC_F_FILTER) && has_tone(ACC_TONE_SIREN_DOWN));
    advance(10000); tick(); assert(!(A.s.filter & ACC_FKF_NOREPEAT));
}

static void toggle_keys(void)
{
    fresh();
    { struct access_settings s = A.s; s.toggle |= ACC_ON; struct access_out o = {0}; access_configure(&A, &s, &o); }
    key(CAPS, 1); assert(has_tone(ACC_TONE_BEEP_HIGH) && ev_is(0, CAPS, 1));
    key(CAPS, 0);
    key(CAPS, 1); assert(has_tone(ACC_TONE_BEEP_LOW));
    key(CAPS, 0); key(CAPS, 1); key(CAPS, 2); assert(!O.tones);   /* repeats are not toggles */
    key(CAPS, 0);
    /* The real state can be told. */
    access_set_locks(&A, 1, 0, 0); key(CAPS, 1); assert(has_tone(ACC_TONE_BEEP_LOW)); key(CAPS, 0);
    key(NUMLOCK, 1); assert(has_tone(ACC_TONE_BEEP_HIGH)); key(NUMLOCK, 0);
    /* NumLock held for five seconds switches the feature (siren), four does not. */
    fresh(); key(NUMLOCK, 1);
    advance(4999); tick(); assert(!access_feature_on(&A, ACC_F_TOGGLE));
    advance(1); tick(); assert(access_feature_on(&A, ACC_F_TOGGLE) && has_tone(ACC_TONE_SIREN_UP));
    key(NUMLOCK, 0);
    fresh(); key(NUMLOCK, 1); advance(4000); key(NUMLOCK, 0); advance(2000); tick(); assert(!access_feature_on(&A, ACC_F_TOGGLE));
    /* Off: no tones. */
    fresh(); key(CAPS, 1); assert(!O.tones);
}

static unsigned motion_total(int *x, int *y)
{
    unsigned n = 0;
    for (unsigned i = 0; i < O.count; i++) if (O.ev[i].type == ACC_EV_MOTION) { *x += O.ev[i].value; *y += O.ev[i].value2; n++; }
    return n;
}
static void mouse_keys(void)
{
    fresh();
    /* Left Alt + Left Shift + NumLock: on, with a siren and a dialog; the NumLock press is consumed. */
    key(LALT, 1); key(LSHIFT, 1); key(NUMLOCK, 1);
    assert(access_feature_on(&A, ACC_F_MOUSE) && has_tone(ACC_TONE_SIREN_UP) && O.confirm == ACC_F_MOUSE && !O.count);
    key(NUMLOCK, 0); assert(!O.count);
    key(LSHIFT, 0); key(LALT, 0);
    assert(A.locks == 0);   /* the consumed NumLock did not toggle the lock state */
    assert(access_mouse_mode(&A));
    /* A direction key moves one pixel at once and is consumed. */
    int x = 0, y = 0;
    key(0x4d, 1); assert(motion_total(&x, &y) == 1 && x == 1 && y == 0);
    assert(!(O.count > 1) || O.ev[0].type == ACC_EV_MOTION);
    /* Held: nothing for 250 ms, then acceleration to the top speed over TimeToMaxSpeed. */
    unsigned long total = 1; x = y = 0;
    uint64_t start = now;
    for (; now < start + 250 * MS; now += 16 * MS) { tick(); motion_total(&x, &y); }
    assert(x == 0);
    uint64_t t0 = now; int at_one_second = 0, at_two = 0, at_end = 0;
    for (; now < start + 5000 * MS; now += 16 * MS) {
        tick(); motion_total(&x, &y);
        if (now - t0 < 1000 * MS) at_one_second = x;
        if (now - t0 < 2000 * MS) at_two = x;
        at_end = x; (void)total;
    }
    assert(y == 0 && at_one_second > 0 && at_two > at_one_second && at_end > at_two);
    /* Final speed: the last second moves max speed pixels (80), within a pixel or two of rounding. */
    { int before = x; uint64_t t = now; for (; now < t + 1000 * MS; now += 16 * MS) { tick(); motion_total(&x, &y); } assert(x - before >= 76 && x - before <= 84); }
    key(0x4d, 0);
    advance(100); tick(); assert(!O.count);
    /* Ctrl multiplies the speed, Shift moves single pixels. */
    key(LCTRL, 1);
    key(0x50, 1); advance(300); { x = y = 0; uint64_t t = now; for (; now < t + 1000 * MS; now += 16 * MS) { tick(); motion_total(&x, &y); } assert(y > 100 && x == 0); }
    key(0x50, 0); key(LCTRL, 0);
    key(LSHIFT, 1); key(0x4b, 1); advance(300);
    { x = y = 0; uint64_t t = now; for (; now < t + 1000 * MS; now += 16 * MS) { tick(); motion_total(&x, &y); } assert(x < 0 && x >= -8); }
    key(0x4b, 0); key(LSHIFT, 0);
    /* Diagonals. */
    x = y = 0; key(0x51, 1); motion_total(&x, &y); assert(x == 1 && y == 1); key(0x51, 0);
    x = y = 0; key(0x47, 1); motion_total(&x, &y); assert(x == -1 && y == -1); key(0x47, 0);
    /* 5 clicks the selected button, + double-clicks. */
    key(0x4c, 1);
    assert(O.count == 2 && O.ev[0].type == ACC_EV_BUTTON && O.ev[0].code == 1 && O.ev[0].value == 1 && O.ev[1].code == 1 && O.ev[1].value == 0);
    key(0x4c, 0); assert(!O.count);
    key(0x4e, 1); assert(O.count == 4 && O.ev[2].value == 1);
    key(0x4e, 0);
    /* - selects the right button, * both (the middle button), / the left again. */
    key(0x4a, 1); assert(!O.count); key(0x4a, 0);
    key(0x4c, 1); assert(O.ev[0].code == 2); key(0x4c, 0);
    key(0x37, 1); key(0x37, 0); key(0x4c, 1); assert(O.ev[0].code == 3); key(0x4c, 0);
    key(0x135, 1); key(0x135, 0); key(0x4c, 1); assert(O.ev[0].code == 1); key(0x4c, 0);
    /* Insert holds the button down for a drag, Delete releases it. */
    key(0x52, 1); assert(O.count == 1 && O.ev[0].type == ACC_EV_BUTTON && O.ev[0].value == 1);
    assert(access_indicator(&A) & ACC_IND_LEFT_HELD);
    key(0x52, 0); key(0x53, 1); assert(O.count == 1 && O.ev[0].value == 0); key(0x53, 0);
    assert(!(access_indicator(&A) & ACC_IND_LEFT_HELD));
    /* NumLock hands the keypad back to the numbers, and back again. */
    tap(NUMLOCK); assert(A.locks & 2);
    assert(!access_mouse_mode(&A));
    key(0x4d, 1); assert(ev_is(0, 0x4d, 1)); key(0x4d, 0);
    tap(NUMLOCK); assert(access_mouse_mode(&A));
    /* "Use MouseKeys when NumLock is on". */
    { struct access_settings s = A.s; s.mouse |= ACC_MKF_REPLACENUMBERS; struct access_out o = {0}; access_configure(&A, &s, &o); }
    assert(!access_mouse_mode(&A)); tap(NUMLOCK); assert(access_mouse_mode(&A));
    /* Hot key again switches it off and releases a held button. */
    key(0x52, 1); assert(O.count == 1);
    clear(); { struct access_settings s = A.s; s.mouse &= ~ACC_ON; access_configure(&A, &s, &O); }
    assert(O.count == 1 && O.ev[0].type == ACC_EV_BUTTON && O.ev[0].value == 0 && !A.mk_drag);
    /* Via StickyKeys: latched modifiers count as held for the hot key. */
    fresh(); sticky_on();
    tap(LALT); tap(LSHIFT); key(NUMLOCK, 1);
    assert(access_feature_on(&A, ACC_F_MOUSE) && !A.latched);
    /* SlowKeys applies to the MouseKeys keys. */
    fresh();
    { struct access_settings s = A.s; s.mouse |= ACC_ON; s.filter |= ACC_ON; s.filter_wait = 400; struct access_out o = {0}; access_configure(&A, &s, &o); }
    key(0x4d, 1); assert(!O.count); advance(400); tick(); x = y = 0; assert(motion_total(&x, &y) == 1);
}

static void high_contrast_and_timeout(void)
{
    fresh();
    key(LALT, 1); key(LSHIFT, 1); key(PRTSC, 1);
    assert(access_feature_on(&A, ACC_F_CONTRAST) && O.contrast_changed && has_tone(ACC_TONE_SIREN_UP) && O.confirm == ACC_F_CONTRAST && !O.count);
    key(PRTSC, 0); assert(!O.count);
    key(LSHIFT, 0); key(LALT, 0);
    /* Again: off. */
    key(LALT, 1); key(LSHIFT, 1); key(PRTSC, 1);
    assert(!access_feature_on(&A, ACC_F_CONTRAST) && O.contrast_changed && has_tone(ACC_TONE_SIREN_DOWN));
    key(PRTSC, 0); key(LSHIFT, 0); key(LALT, 0);
    /* Right Alt does not trigger it. */
    key(0x138, 1); key(LSHIFT, 1); key(PRTSC, 1); assert(!access_feature_on(&A, ACC_F_CONTRAST));
    key(PRTSC, 0); key(LSHIFT, 0); key(0x138, 0);
    /* Declining the dialog restores the previous (off) state. */
    fresh(); key(LALT, 1); key(LSHIFT, 1); key(PRTSC, 1); access_confirm(&A, ACC_F_CONTRAST, 0, &O);
    assert(!access_feature_on(&A, ACC_F_CONTRAST) && O.contrast_changed);
    /* Idle time-out: FilterKeys, MouseKeys, ToggleKeys, StickyKeys and high contrast go off. */
    fresh();
    { struct access_settings s = A.s; s.sticky |= ACC_ON; s.filter |= ACC_ON; s.toggle |= ACC_ON; s.mouse |= ACC_ON; s.contrast |= ACC_ON;
      s.timeout = ACC_ATF_ON | ACC_ATF_FEEDBACK; s.timeout_ms = 300000; struct access_out o = {0}; access_configure(&A, &s, &o); }
    assert(access_wake_ns(&A, now) == A.last_input_ns + 300000 * MS);
    advance(299000); tick(); assert(access_feature_on(&A, ACC_F_STICKY));
    key(KEY_A, 1); key(KEY_A, 0);     /* input resets the time-out */
    advance(299000); tick(); assert(access_feature_on(&A, ACC_F_STICKY));
    advance(2000); tick();
    for (unsigned f = ACC_F_STICKY; f < ACC_F_COUNT; f++) assert(!access_feature_on(&A, f));
    assert(has_tone(ACC_TONE_SIREN_DOWN) && O.contrast_changed && O.changed);
    /* Not enabled: nothing times out. */
    fresh(); { struct access_settings s = A.s; s.sticky |= ACC_ON; struct access_out o = {0}; access_configure(&A, &s, &o); }
    advance(10000000); tick(); assert(access_feature_on(&A, ACC_F_STICKY));
    /* Without feedback: silent. */
    fresh(); { struct access_settings s = A.s; s.sticky |= ACC_ON; s.timeout = ACC_ATF_ON; s.timeout_ms = 60000; struct access_out o = {0}; access_configure(&A, &s, &o); }
    advance(61000); tick(); assert(!access_feature_on(&A, ACC_F_STICKY) && !has_tone(ACC_TONE_SIREN_DOWN));
}

static void indicator_and_wake(void)
{
    fresh();
    assert(!access_indicator_visible(&A));
    sticky_on(); assert(access_indicator_visible(&A));
    tap(LSHIFT); assert(access_indicator(&A) & ACC_IND_SHIFT);
    tap(LCTRL); tap(LCTRL); assert(access_indicator(&A) & ACC_IND_LOCK_CTRL);
    { struct access_settings s = A.s; s.status = 0; struct access_out o = {0}; access_configure(&A, &s, &o); }
    assert(!access_indicator_visible(&A));
    fresh(); { struct access_settings s = A.s; s.filter |= ACC_ON; struct access_out o = {0}; access_configure(&A, &s, &o); }
    assert((access_indicator(&A) & ACC_IND_STOPWATCH) && access_indicator_visible(&A));
    /* Toggle and contrast alone show no indicator (Windows draws only the three). */
    fresh(); { struct access_settings s = A.s; s.toggle |= ACC_ON; s.contrast |= ACC_ON; struct access_out o = {0}; access_configure(&A, &s, &o); }
    assert(!access_indicator_visible(&A));
    /* Configure reports the high-contrast flip and a scheme change. */
    fresh(); { struct access_settings s = A.s; s.contrast |= ACC_ON; clear(); access_configure(&A, &s, &O); assert(O.contrast_changed); }
    { struct access_settings s = A.s; strcpy(s.scheme, "High Contrast White"); clear(); access_configure(&A, &s, &O); assert(O.contrast_changed); }
}

static void tones(void)
{
    static int16_t pcm[44100];
    for (unsigned t = ACC_TONE_SIREN_UP; t < ACC_TONES; t++) {
        struct access_tone_step st[8];
        unsigned steps = access_tone_steps(t, st), ms = access_tone_ms(t);
        assert(steps >= 1 && steps <= 8 && ms >= 8 && ms <= 1000);
        unsigned frames = access_tone_render(t, pcm, 44100, 22050);
        assert(frames == (unsigned)((uint64_t)ms * 22050 / 1000) || frames + 8 >= (unsigned)((uint64_t)ms * 22050 / 1000));
        int peak = 0; for (unsigned i = 0; i < frames; i++) { int v = pcm[i] < 0 ? -pcm[i] : pcm[i]; if (v > peak) peak = v; }
        assert(peak > 3000 && peak <= 8191);
        assert(pcm[0] == 0);   /* fades in: no click */
        assert(access_tone_render(t, pcm, 10, 22050) == 10);
    }
    assert(!access_tone_steps(ACC_TONE_NONE, (struct access_tone_step[8]){0}) && !access_tone_ms(999));
    /* The siren rises: more zero crossings late than early. */
    unsigned frames = access_tone_render(ACC_TONE_SIREN_UP, pcm, 44100, 22050);
    unsigned early = 0, late = 0;
    for (unsigned i = 1; i < frames / 4; i++) early += (pcm[i - 1] < 0) != (pcm[i] < 0);
    for (unsigned i = frames * 3 / 4; i < frames - 1; i++) late += (pcm[i - 1] < 0) != (pcm[i] < 0);
    assert(late > early + early / 2);   /* the siren rises */
    frames = access_tone_render(ACC_TONE_SIREN_DOWN, pcm, 44100, 22050);
    early = late = 0;
    for (unsigned i = 1; i < frames / 4; i++) early += (pcm[i - 1] < 0) != (pcm[i] < 0);
    for (unsigned i = frames * 3 / 4; i < frames - 1; i++) late += (pcm[i - 1] < 0) != (pcm[i] < 0);
    assert(early > late);
}

static void magnifier(void)
{
    /* Panel: 25 % of a 1024x768 screen between the top bar and the taskbar. */
    struct access_mag_box p = access_mag_panel(1024, 768, 32, 32, 25, ACC_DOCK_TOP, 0);
    assert(p.x == 0 && p.y == 32 && p.w == 1024 && p.h == 192);
    struct access_mag_box q = access_mag_panel(1024, 768, 32, 32, 25, ACC_DOCK_BOTTOM, 0);
    assert(q.y == 768 - 32 - 192 && q.h == 192);
    assert(access_mag_panel(1024, 768, 32, 32, 25, ACC_DOCK_TOP, 1).y == q.y);   /* flipped */
    assert(access_mag_panel(1024, 768, 32, 32, 90, ACC_DOCK_TOP, 0).h == 352);   /* clamped to the percent maximum of half the usable height */
    /* Source: 2x makes a 512x96 rectangle, centred on the pointer and kept out of the panel. */
    struct access_mag_box s;
    assert(access_mag_source(1024, 768, p, 2, 500, 500, &s) && s.w == 512 && s.h == 96 && s.x == 244 && s.y == 452);
    assert(access_mag_source(1024, 768, p, 2, 0, 700, &s) && s.x == 0 && s.y + s.h <= 768);
    assert(access_mag_source(1024, 768, p, 2, 1023, 100, &s) && s.x == 1024 - 512 && s.y == p.y + p.h);   /* pushed below the panel */
    assert(access_mag_source(1024, 768, p, 9, 500, 500, &s) && s.w == 113 && s.h == 21);
    assert(access_mag_source(1024, 768, p, 1, 5, 5, &s) && s.w == 1024 && s.h == 192);
    /* The panel jumps to the other edge when the pointer comes near it. */
    assert(access_mag_side(1024, 768, 32, 32, 25, ACC_DOCK_TOP, 2, 500, 500) == ACC_DOCK_TOP);
    assert(access_mag_side(1024, 768, 32, 32, 25, ACC_DOCK_TOP, 2, 500, 200) == ACC_DOCK_BOTTOM);
    assert(access_mag_side(1024, 768, 32, 32, 25, ACC_DOCK_TOP, 2, 500, 260) == ACC_DOCK_BOTTOM);
    assert(access_mag_side(1024, 768, 32, 32, 25, ACC_DOCK_BOTTOM, 2, 500, 600) == ACC_DOCK_TOP);
    assert(access_mag_side(1024, 768, 32, 32, 25, ACC_DOCK_BOTTOM, 2, 500, 300) == ACC_DOCK_BOTTOM);
    /* Pixels: nearest neighbour at an integer level, optional inversion, border where the source is short. */
    static uint32_t screen[16 * 16], out[16 * 16];
    for (unsigned i = 0; i < 256; i++) screen[i] = 0x010000u * (i % 16) + 0x000100u * (i / 16);
    struct access_mag_box src = {2, 3, 4, 2}, panel = {1, 1, 8, 4};
    access_mag_paint(out, 16, panel, screen, 16, src, 2, 0, 0x00123456u);
    for (int y = 0; y < 4; y++) for (int x = 0; x < 8; x++) assert(out[(1 + y) * 16 + 1 + x] == screen[(3 + y / 2) * 16 + 2 + x / 2]);
    access_mag_paint(out, 16, panel, screen, 16, src, 2, 1, 0x00123456u);
    assert(out[16 + 1] == (~screen[3 * 16 + 2] & 0xffffffu));
    struct access_mag_box short_src = {2, 3, 3, 2};   /* 3 source columns fill 6 of 8 */
    access_mag_paint(out, 16, panel, screen, 16, short_src, 2, 0, 0x00123456u);
    assert(out[16 + 1 + 6] == 0x00123456u && out[16 + 1 + 5] == screen[3 * 16 + 4]);
    /* A source that does not fit (screen shorter than the panel) is refused. */
    assert(!access_mag_source(1024, 100, (struct access_mag_box){0, 0, 1024, 90}, 1, 0, 0, &s));
}

static void configure_keeps_state(void)
{
    fresh(); sticky_on(); tap(LSHIFT);
    struct access_settings s = A.s; s.mouse_max = 100; clear();
    access_configure(&A, &s, &O);
    assert(A.latched && O.count == 0);
    /* Held keys survive a settings change, and FilterKeys slots are dropped when it is switched off. */
    fresh(); { struct access_settings t = A.s; t.filter |= ACC_ON; struct access_out o = {0}; access_configure(&A, &t, &o); }
    key(KEY_A, 1);
    { struct access_settings t = A.s; t.filter &= ~ACC_ON; clear(); access_configure(&A, &t, &O); }
    advance(5000); tick(); assert(!O.count);
    key(KEY_A, 0); assert(O.count == 1 && ev_is(0, KEY_A, 0));
    /* Out of range key codes and values are ignored. */
    clear(); access_key(&A, now, -1, 1, &O); access_key(&A, now, 0x300, 1, &O); access_key(&A, now, KEY_A, 7, &O); assert(!O.count);
}

int main(void)
{
    defaults();
    sticky_hotkey();
    sticky_latch_and_lock();
    sticky_two_keys_and_click();
    filter_slow();
    filter_bounce();
    filter_repeat();
    filter_hotkey();
    toggle_keys();
    mouse_keys();
    high_contrast_and_timeout();
    indicator_and_wake();
    tones();
    magnifier();
    configure_keeps_state();
    puts("PASS g25 accessibility engine: StickyKeys, FilterKeys, ToggleKeys, MouseKeys, hot keys, time-out, tones, Magnifier geometry");
    return 0;
}
