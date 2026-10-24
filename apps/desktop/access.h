/* SPDX-License-Identifier: MIT
 * G25 accessibility: settings and input filters (docs/design/accessibility.md).
 *
 * The record layout and flag values are those of the Win32 accessibility
 * structures (STICKYKEYS, FILTERKEYS, TOGGLEKEYS, MOUSEKEYS, SOUNDSENTRY,
 * HIGHCONTRAST, ACCESSTIMEOUT), so SystemParametersInfo is a field copy.
 * The engine (access.c) is pure: it turns one raw input event or one clock
 * tick into the events the desktop delivers, the tones to play and the state
 * changes to persist. No file, window or clock is touched here; the desktop
 * glue is access_desktop.c. */
#ifndef CIUKI_DESKTOP_ACCESS_H
#define CIUKI_DESKTOP_ACCESS_H
#include <stddef.h>
#include <stdint.h>

/* Flag values shared by every feature (Win32 *_ON, *_AVAILABLE, ...). */
#define ACC_ON 0x1u
#define ACC_AVAILABLE 0x2u
#define ACC_HOTKEY 0x4u              /* the emergency hot key is enabled */
#define ACC_CONFIRM 0x8u             /* a confirmation dialog follows the hot key */
#define ACC_SOUND 0x10u              /* a siren plays for the hot key */
#define ACC_INDICATOR 0x20u          /* show the status indicator */
#define ACC_SKF_AUDIBLE 0x40u        /* tones when a modifier latches or locks */
#define ACC_SKF_TRISTATE 0x80u       /* pressing a modifier twice locks it */
#define ACC_SKF_TWOKEYSOFF 0x100u    /* two keys pressed at once turn StickyKeys off */
#define ACC_FKF_CLICK 0x40u          /* a click when a key is pressed or accepted */
#define ACC_FKF_NOREPEAT 0x1000u     /* CiukiOS: RepeatKeys switched repeat off (not a Win32 bit) */
#define ACC_MKF_MODIFIERS 0x40u      /* Ctrl speeds up, Shift moves one pixel */
#define ACC_MKF_REPLACENUMBERS 0x80u /* the keypad moves the pointer with NumLock on */
#define ACC_MKF_LEFTBUTTONSEL 0x10000000u
#define ACC_MKF_RIGHTBUTTONSEL 0x20000000u
#define ACC_MKF_LEFTBUTTONDOWN 0x01000000u
#define ACC_MKF_RIGHTBUTTONDOWN 0x02000000u
#define ACC_MKF_MOUSEMODE 0x80000000u
#define ACC_HCF_HOTKEYAVAILABLE 0x40u
#define ACC_SKF_STATE_MASK 0xffff0000u /* Win32 *_LATCHED and *_LOCKED bits */
/* Win32 SKF_* latched and locked bits for the eight modifier keys. */
#define ACC_SKF_LSHIFTLOCKED 0x00010000u
#define ACC_SKF_LSHIFTLATCHED 0x01000000u
#define ACC_ATF_ON 0x1u
#define ACC_ATF_FEEDBACK 0x2u
#define ACC_SS_NONE 0u               /* SSWF_* */
#define ACC_SS_TITLE 1u
#define ACC_SS_WINDOW 2u
#define ACC_SS_DISPLAY 3u
#define ACC_SS_CUSTOM 4u
#define ACC_SS_MAX 3u                /* CiukiOS has no custom effect DLLs */
#define ACC_SCHEME_MAX 32
#define ACC_FILTER_MAX_MS 20000u
#define ACC_MOUSE_MAX_SPEED_MIN 10u  /* MOUSEKEYS.iMaxSpeed, Windows NT range */
#define ACC_MOUSE_MAX_SPEED_MAX 360u
#define ACC_MOUSE_TIME_MIN 1000u     /* iTimeToMaxSpeed */
#define ACC_MOUSE_TIME_MAX 5000u
#define ACC_TIMEOUT_MIN_MS 60000u    /* the panel offers 5 to 30 minutes; 1 min is the file floor */
#define ACC_TIMEOUT_MAX_MS 3600000u

/* Features, in the order of the tabs and of the status indicator. */
enum { ACC_F_NONE, ACC_F_STICKY, ACC_F_FILTER, ACC_F_TOGGLE, ACC_F_MOUSE, ACC_F_CONTRAST, ACC_F_COUNT };

/* Pointer schemes (Mouse > Pointers, WM-21). */
enum { ACC_POINTER_STANDARD, ACC_POINTER_LARGE, ACC_POINTER_INVERTED, ACC_POINTER_LARGE_INVERTED, ACC_POINTERS };
static const char *const acc_pointer_names[ACC_POINTERS] = {"Ciuki Standard", "Ciuki Large", "Ciuki Inverted", "Ciuki Large Inverted"};
static const char *const acc_high_contrast_names[] = {"High Contrast Black", "High Contrast White", "High Contrast #1", "High Contrast #2", NULL};

/* Magnifier tracking bits and docking. */
#define ACC_MAG_MOUSE 1u
#define ACC_MAG_FOCUS 2u
#define ACC_MAG_TEXT 4u
#define ACC_MAG_LEVEL_MIN 1u
#define ACC_MAG_LEVEL_MAX 9u        /* "up to nine times", Windows 98 Resource Kit */
enum { ACC_DOCK_TOP, ACC_DOCK_BOTTOM };
#define ACC_MAG_PERCENT_MIN 10u
#define ACC_MAG_PERCENT_MAX 50u

struct access_settings {
    uint32_t sticky, filter, toggle, mouse, sentry, contrast, timeout;   /* flag words */
    uint32_t filter_wait, filter_delay, filter_repeat, filter_bounce;    /* ms */
    uint32_t mouse_max, mouse_time, mouse_ctrl;
    uint32_t sentry_effect;       /* ACC_SS_* for windows and DOS windows */
    uint32_t timeout_ms;
    uint32_t show_sounds;
    uint32_t status;              /* 1: the tray status indicator is shown */
    uint32_t pointer;             /* ACC_POINTER_* */
    char scheme[ACC_SCHEME_MAX];  /* the high-contrast theme name */
    uint32_t mag_on, mag_level, mag_follow, mag_invert, mag_dock, mag_percent;
};
/* Header-only: settings.c (also linked into Setup) needs them without the engine. */
static inline int access_scheme_valid(const char *name)
{
    for (unsigned i = 0; acc_high_contrast_names[i]; i++) {
        const char *a = name, *b = acc_high_contrast_names[i];
        while (*a && *a == *b) { a++; b++; }
        if (!*a && !*b) return 1;
    }
    return 0;
}
static inline void access_defaults(struct access_settings *s)
{
    unsigned char *p = (unsigned char *)s;
    for (size_t i = 0; i < sizeof(*s); i++) p[i] = 0;
    /* The Windows defaults (Control Panel\Accessibility, StickyKeys Flags 510, FilterKeys 126, ToggleKeys 62,
     * High Contrast 126; MouseKeys 126 adds the Ctrl and Shift modifiers) with the hot keys enabled. */
    s->sticky = 0x1feu; s->filter = 0x7eu; s->toggle = 0x3eu; s->mouse = 0x7eu; s->contrast = 0x7eu;
    s->sentry = ACC_AVAILABLE;
    s->filter_wait = 1000; s->filter_delay = 1000; s->filter_repeat = 500; s->filter_bounce = 0;
    s->mouse_max = 80; s->mouse_time = 3000; s->mouse_ctrl = 8;
    s->timeout_ms = 300000; s->status = 1;
    for (unsigned i = 0; acc_high_contrast_names[0][i] && i + 1 < ACC_SCHEME_MAX; i++) s->scheme[i] = acc_high_contrast_names[0][i];
    s->mag_level = 2; s->mag_follow = ACC_MAG_MOUSE | ACC_MAG_FOCUS; s->mag_percent = 25;
}

/* ---- tones ---------------------------------------------------------------
 * Windows 98 plays rising and falling sirens for the hot keys, high and low
 * beeps for ToggleKeys and for StickyKeys' latch and lock (Resource Kit,
 * appendix H). The desktop synthesises them (no sound files). */
enum {
    ACC_TONE_NONE, ACC_TONE_SIREN_UP, ACC_TONE_SIREN_DOWN, ACC_TONE_BEEP_HIGH, ACC_TONE_BEEP_LOW,
    ACC_TONE_LATCH, ACC_TONE_LOCK, ACC_TONE_UNLATCH, ACC_TONE_WARNING, ACC_TONE_CLICK,
    ACC_TONE_SIREN_UP2, ACC_TONE_SIREN_UP3, ACC_TONES
};
struct access_tone_step { uint16_t from_hz, to_hz, ms; };      /* from_hz 0: silence */
/* The steps of a tone (at most 8); returns their number, 0 for an unknown tone. */
unsigned access_tone_steps(unsigned tone, struct access_tone_step steps[8]);
/* Total duration in milliseconds. */
unsigned access_tone_ms(unsigned tone);
/* Renders mono S16 PCM at `rate`; returns the number of frames written. */
unsigned access_tone_render(unsigned tone, int16_t *pcm, unsigned max_frames, unsigned rate);

/* ---- engine --------------------------------------------------------------- */
enum { ACC_EV_KEY = 1, ACC_EV_MOTION = 3, ACC_EV_BUTTON = 4 };  /* CIUKI_INPUT_* */
struct access_event { int type, code, value, value2; };
#define ACC_OUT_EVENTS 24
#define ACC_OUT_TONES 8
/* What one engine call asks the desktop to do. The caller zeroes it. */
struct access_out {
    unsigned count;                        /* events to deliver, in order */
    struct access_event ev[ACC_OUT_EVENTS];
    unsigned tones;
    uint8_t tone[ACC_OUT_TONES];
    unsigned confirm, confirm_on;          /* ACC_F_* hot key to confirm, and whether it was switched on */
    unsigned changed;                      /* settings changed: persist (bit per feature) */
    unsigned redraw;                       /* the status indicator changed */
    unsigned contrast_changed;             /* the high-contrast flag flipped: re-theme */
    unsigned mouse_keys_moved;             /* a MouseKeys pointer event was produced */
};
#define ACC_KEYS 0x200
#define ACC_SLOTS 8
#define ACC_RELEASES 4
struct access_slot {
    int code; uint8_t used, pending, accepted, ignored;
    uint64_t pressed, accepted_at, last_repeat;
};
struct access_release { int code; uint64_t at; };
struct access {
    struct access_settings s;
    /* StickyKeys: bits of enum acc_mod */
    uint8_t phys, sent, latched, locked, combo;
    /* hot keys on the raw stream */
    uint8_t shift_run, rshift_on, numlock_on, rshift_stage;
    uint64_t rshift_since, numlock_since;
    int swallow_code[4];                   /* keys a hot key consumed: their release and repeats are dropped too */
    /* FilterKeys */
    struct access_slot slot[ACC_SLOTS];
    struct access_release released[ACC_RELEASES];
    unsigned release_next;
    uint8_t down[ACC_KEYS / 8];            /* non-modifier keys the user holds (raw) */
    /* ToggleKeys: caps, num, scroll as the desktop counts them */
    uint8_t locks;
    /* MouseKeys */
    uint8_t mk_dir;                         /* held keypad direction keys, one bit each */
    uint8_t mk_drag, mk_button;             /* drag lock; selected button 1 left, 2 right, 3 both */
    uint64_t mk_since, mk_last;
    int32_t mk_acc_x, mk_acc_y;
    /* idle and confirmation */
    uint64_t last_input_ns;
    unsigned pending_confirm;
    struct access_settings before;          /* settings to restore if the confirmation is declined */
    uint8_t indicator;                      /* what the tray indicator showed last */
};
enum acc_mod { ACC_M_LSHIFT, ACC_M_RSHIFT, ACC_M_LCTRL, ACC_M_RCTRL, ACC_M_LALT, ACC_M_RALT, ACC_M_LWIN, ACC_M_RWIN, ACC_MODS };
extern const int acc_mod_code[ACC_MODS];

void access_init(struct access *, const struct access_settings *, uint64_t now_ns);
/* New settings arrive (Control Panel, SystemParametersInfo): the on/off state
 * and thresholds change, the keys the user holds stay tracked. */
void access_configure(struct access *, const struct access_settings *, struct access_out *);
void access_key(struct access *, uint64_t now_ns, int code, int value, struct access_out *);
void access_button(struct access *, uint64_t now_ns, int button, int value, struct access_out *);
void access_motion(struct access *, uint64_t now_ns);
/* The input stream was cut (a full-screen session ended): the keys are released elsewhere, forget every held key. */
void access_resync(struct access *);
void access_tick(struct access *, uint64_t now_ns, struct access_out *);
/* Absolute time of the next tick the engine needs; 0 when none. */
uint64_t access_wake_ns(const struct access *, uint64_t now_ns);
/* The user answered the confirmation dialog of `feature`. */
void access_confirm(struct access *, unsigned feature, int keep, struct access_out *);
/* Switches a feature (the panel's "on" check box, a hot key). */
void access_set(struct access *, unsigned feature, int on, struct access_out *);
int access_feature_on(const struct access *, unsigned feature);
/* A bitmask of what the tray indicator shows: one bit per ACC_F_*, plus the
 * modifier and mouse-button state in the upper bits (access_indicator_*). */
#define ACC_IND_SHIFT 8u
#define ACC_IND_CTRL 16u
#define ACC_IND_ALT 32u
#define ACC_IND_LOCK_SHIFT 64u
#define ACC_IND_LOCK_CTRL 128u
#define ACC_IND_LOCK_ALT 256u
#define ACC_IND_LEFT 512u
#define ACC_IND_RIGHT 1024u
#define ACC_IND_LEFT_HELD 2048u
#define ACC_IND_RIGHT_HELD 4096u
#define ACC_IND_STOPWATCH 8192u
unsigned access_indicator(const struct access *);
int access_indicator_visible(const struct access *);
/* The desktop reports the lock keys' real state when it learns it. */
void access_set_locks(struct access *, unsigned caps, unsigned num, unsigned scroll);
/* Idle: any input resets the accessibility time-out. */
void access_note_input(struct access *, uint64_t now_ns);
/* Windows 98 model: modifiers a user means; for tests and the panel. */
int access_mouse_mode(const struct access *);

/* ---- Magnifier (pure geometry and pixels) ------------------------------- */
struct access_mag_box { int x, y, w, h; };
/* The source rectangle of a panel of `panel` size at magnification `level`
 * centred on (cx, cy), clamped to the screen and kept out of `avoid`
 * (the panel itself). Returns 1 when a rectangle was found. */
int access_mag_source(int screen_w, int screen_h, struct access_mag_box panel, unsigned level,
                      int cx, int cy, struct access_mag_box *source);
/* The docked panel box for a screen. `flipped` mirrors the dock to the other
 * edge (the panel moves away from the pointer). */
struct access_mag_box access_mag_panel(int screen_w, int screen_h, int top_inset, int bottom_inset,
                                       unsigned percent, unsigned dock, int flipped);
/* Chooses the dock edge for the pointer: the preferred one unless the
 * pointer's source rectangle would overlap the panel there. */
unsigned access_mag_side(int screen_w, int screen_h, int top_inset, int bottom_inset, unsigned percent,
                         unsigned preferred, unsigned level, int cx, int cy);
/* Magnifies `src` (stride in pixels) into `dst` at integer `level` with
 * nearest-neighbour sampling; `invert` flips every colour. Writes
 * panel.w * panel.h pixels; pixels outside the source stay `border`. */
void access_mag_paint(uint32_t *dst, size_t dst_stride, struct access_mag_box panel,
                      const uint32_t *screen, size_t screen_stride, struct access_mag_box source,
                      unsigned level, int invert, uint32_t border);
#endif
