/* G25: Accessibility Options, included by app.c.
 * Windows 98 model (Resource Kit appendix H, "Configuring Accessibility Features Using Control Panel"): the applet
 * has Keyboard, Sound, Display, Mouse and General tabs; each feature has a check box and a Settings button that
 * opens its own dialog, here a page of its own inside the tab control with OK and Cancel. The settings are the
 * Win32 accessibility records (apps/desktop/access.h) stored as the access_* keys of desktop.cfg; the desktop
 * applies them at once through DESK_SETTINGS_RELOAD.
 * SPDX-License-Identifier: MIT */
enum {
    C_AC_TABS = 2000, C_AC_OK, C_AC_CANCEL,
    C_AC_STICKY_ON, C_AC_STICKY_SET, C_AC_FILTER_ON, C_AC_FILTER_SET, C_AC_TOGGLE_ON, C_AC_TOGGLE_SET,
    C_AC_SENTRY_ON, C_AC_SENTRY_SET, C_AC_SHOW_ON, C_AC_HC_ON, C_AC_HC_SET, C_AC_POINTERS, C_AC_MAG_ON, C_AC_MAG_SET,
    C_AC_MOUSE_ON, C_AC_MOUSE_SET,
    C_AC_TIMEOUT_ON, C_AC_TIMEOUT_MIN, C_AC_TIMEOUT_V, C_AC_FEEDBACK, C_AC_STATUS_ON, C_AC_SERIAL,
    C_AC_NOTE0, C_AC_NOTE1, C_AC_NOTE2, C_AC_NOTE3, C_AC_NOTE4,
    C_AC_HOTKEY, C_AC_CONFIRM, C_AC_SOUND, C_AC_INDICATOR,
    C_AC_S_LOCK, C_AC_S_TWO, C_AC_S_AUDIBLE,
    C_AC_F_MODE0, C_AC_F_MODE1, C_AC_F_MODE2, C_AC_F_WAIT, C_AC_F_WAIT_V, C_AC_F_DELAY, C_AC_F_DELAY_V,
    C_AC_F_REPEAT, C_AC_F_REPEAT_V, C_AC_F_BOUNCE, C_AC_F_BOUNCE_V, C_AC_F_NOREPEAT, C_AC_F_CLICK,
    C_AC_SS0, C_AC_SS1, C_AC_SS2, C_AC_SS3,
    C_AC_SCHEMES,
    C_AC_M_NUM0, C_AC_M_NUM1, C_AC_M_MOD, C_AC_M_SPEED, C_AC_M_SPEED_V, C_AC_M_TIME, C_AC_M_TIME_V, C_AC_M_CTRL, C_AC_M_CTRL_V,
    C_AC_G_LEVEL, C_AC_G_LEVEL_V, C_AC_G_MOUSE, C_AC_G_FOCUS, C_AC_G_TEXT, C_AC_G_INVERT, C_AC_G_TOP, C_AC_G_BOTTOM,
    C_AC_G_SIZE, C_AC_G_SIZE_V, C_AC_G_ON
};
enum { AC_KEYBOARD, AC_SOUND, AC_DISPLAY, AC_MOUSE, AC_GENERAL, AC_TABS };
enum { AC_V_MAIN, AC_V_STICKY, AC_V_FILTER, AC_V_TOGGLE, AC_V_SENTRY, AC_V_CONTRAST, AC_V_MOUSE, AC_V_MAGNIFIER };
static const char *const ac_tab_names[AC_TABS] = {"Keyboard", "Sound", "Display", "Mouse", "General"};
static const char *const ac_view_names[] = {"Accessibility Options", "Settings for StickyKeys", "Settings for FilterKeys",
    "Settings for ToggleKeys", "Settings for SoundSentry", "Settings for High Contrast", "Settings for MouseKeys", "Settings for Magnifier"};
/* The slider steps: the slowest and the fastest the Windows 98 dialogs offer, in milliseconds. */
static const unsigned ac_wait_ms[] = {300, 500, 700, 1000, 1400, 1700, 2000};
static const unsigned ac_bounce_ms[] = {500, 700, 1000, 1400, 1700, 2000};
static const unsigned ac_repeat_ms[] = {300, 500, 700, 1000, 1400, 1700, 2000};
static const unsigned ac_speed_steps[] = {20, 40, 80, 120, 160, 200, 240, 300, 360};
static const unsigned ac_time_ms[] = {1000, 1500, 2000, 2500, 3000, 3500, 4000, 4500, 5000};
static const unsigned ac_ctrl_steps[] = {2, 4, 8, 12, 16, 20};
#define AC_COUNT(x) ((unsigned)(sizeof(x) / sizeof((x)[0])))
static const char *const ac_scheme_notes[] = {"White text on a black desktop, cyan selection.", "Black text on a white desktop, purple selection.",
    "Yellow text on a black desktop, blue selection.", "Green text on a black desktop, yellow selection."};

static unsigned ac_nearest(const unsigned *steps, unsigned count, unsigned value)
{
    unsigned best = 0;
    for (unsigned i = 1; i < count; i++) {
        unsigned a = steps[i] > value ? steps[i] - value : value - steps[i], b = steps[best] > value ? steps[best] - value : value - steps[best];
        if (a < b) best = i;
    }
    return best;
}
static struct du_widget *ac_check(struct cp_app *a, int id, int x, int y, int w, const char *label, unsigned flags, unsigned bit)
{
    struct du_widget *c = add(a, DU_CHECKBOX, id, x, y, w, 22, label);
    c->value = (flags & bit) != 0;
    return c;
}
static struct du_widget *ac_slider(struct cp_app *a, int id, int x, int y, int w, unsigned steps, unsigned value)
{
    /* Win98 trackbar; the rows sit 22 px apart, so the box is 22 high (thumb 21) and centred on the 17 px row. */
    struct du_widget *s = add(a, DU_TRACKBAR, id, x, y - 3, w, 22, "");
    du_trackbar_set(s, 0, (int)steps - 1, (int)value, 1, 1, 0);
    return s;
}
static void ac_seconds(struct cp_app *a, int id, unsigned ms)
{
    char s[24]; snprintf(s, sizeof(s), "%u.%u s", ms / 1000, ms % 1000 / 100);
    text(a, id, s);
}

/* ---- merge and save ---- */
/* Only what this panel changed is written: the desktop may have switched a feature with a hot key meanwhile. */
static void ac_merge(struct cp_app *a, struct ciuki_settings *now)
{
    uint32_t *mine = (uint32_t *)&a->cfg.access, *theirs = (uint32_t *)&now->access, *was = (uint32_t *)&a->access_loaded;
    for (unsigned i = 0; i < sizeof(struct access_settings) / 4; i++) if (mine[i] != was[i]) theirs[i] = mine[i];
    a->cfg.access = now->access; a->access_loaded = now->access;
}
static void ac_save(struct cp_app *a) { changed(a); }
static void ac_refresh_from_file(struct cp_app *a)
{
    static struct ciuki_settings now;
    if (load(a,&now)) return;
    if (!memcmp(&now.access, &a->access_loaded, sizeof(now.access))) return;
    a->cfg.access = now.access; a->access_loaded = now.access;
}

/* ---- pages ---- */
/* Labels do not wrap: the note is broken at word boundaries into one label per line. */
static char ac_lines[16][128];
static unsigned ac_line_count;
static void ac_note(struct cp_app *a, int id, int x, int y, int w, const char *s)
{
    char line[128]; size_t length = 0;
    int first = 1;
    while (*s) {
        const char *end = s; size_t fit = 0;
        for (const char *p = s;; p++) {
            if (*p == ' ' || !*p) {
                size_t n = (size_t)(p - s);
                char probe[128]; if (n >= sizeof(probe)) break;
                memcpy(probe, s, n); probe[n] = 0;
                if (fit && du_text_width(probe) > w) break;
                fit = n; end = p;
            }
            if (!*p) break;
        }
        if (!fit) { fit = strlen(s); end = s + fit; }
        length = fit < sizeof(line) - 1 ? fit : sizeof(line) - 1;
        memcpy(line, s, length); line[length] = 0;
        if (ac_line_count < 16) {
            memcpy(ac_lines[ac_line_count], line, length + 1);
            add(a, DU_LABEL, first ? id : 2200 + (int)ac_line_count, x, y, w, 14, ac_lines[ac_line_count]);
            ac_line_count++;
        }
        first = 0; y += 14;
        s = end; while (*s == ' ') s++;
    }
}
static void ac_ok_cancel(struct cp_app *a)
{
    struct du_widget *w = add(a, DU_BUTTON, C_AC_OK, CP_WIDTH - 196, CP_HEIGHT - 110, 84, 24, "OK"); w->def = 1;
    add(a, DU_BUTTON, C_AC_CANCEL, CP_WIDTH - 104, CP_HEIGHT - 110, 84, 24, "Cancel");
}
static void ac_hotkey_block(struct cp_app *a, int y, unsigned flags, const char *hotkey)
{
    ac_check(a, C_AC_HOTKEY, 28, y, 500, hotkey, flags, ACC_HOTKEY);
    ac_check(a, C_AC_CONFIRM, 28, y + 24, 500, "&Give warning message when turning a setting on", flags, ACC_CONFIRM);
    ac_check(a, C_AC_SOUND, 28, y + 48, 500, "&Make a sound when turning a setting on or off", flags, ACC_SOUND);
}
static void ac_build_main(struct cp_app *a)
{
    const struct access_settings *s = &a->cfg.access;
    struct du_widget *w;
    switch (a->ac_tab) {
    case AC_KEYBOARD:
        add(a, DU_GROUP, 2100, 20, 40, 516, 88, "StickyKeys");
        ac_check(a, C_AC_STICKY_ON, 32, 60, 360, "Use S&tickyKeys", s->sticky, ACC_ON);
        ac_note(a, C_AC_NOTE0, 32, 82, 380, "Use StickyKeys if you want to use Shift, Ctrl, or Alt by pressing one key at a time.");
        add(a, DU_BUTTON, C_AC_STICKY_SET, 436, 66, 88, 24, "S&ettings...");
        add(a, DU_GROUP, 2101, 20, 136, 516, 88, "FilterKeys");
        ac_check(a, C_AC_FILTER_ON, 32, 156, 360, "Use &FilterKeys", s->filter, ACC_ON);
        ac_note(a, C_AC_NOTE1, 32, 178, 380, "Use FilterKeys if you want Windows to ignore brief or repeated keystrokes, or slow the repeat rate.");
        add(a, DU_BUTTON, C_AC_FILTER_SET, 436, 162, 88, 24, "Se&ttings...");
        add(a, DU_GROUP, 2102, 20, 232, 516, 88, "ToggleKeys");
        ac_check(a, C_AC_TOGGLE_ON, 32, 252, 360, "Use &ToggleKeys", s->toggle, ACC_ON);
        ac_note(a, C_AC_NOTE2, 32, 274, 380, "Use ToggleKeys if you want to hear tones when pressing Caps Lock, Num Lock, and Scroll Lock.");
        add(a, DU_BUTTON, C_AC_TOGGLE_SET, 436, 258, 88, 24, "Set&tings...");
        break;
    case AC_SOUND:
        add(a, DU_GROUP, 2100, 20, 40, 516, 100, "SoundSentry");
        ac_check(a, C_AC_SENTRY_ON, 32, 60, 360, "Use Sound&Sentry", s->sentry, ACC_ON);
        ac_note(a, C_AC_NOTE0, 32, 84, 390, "Use SoundSentry if you want Windows to use visual warnings when your system makes a sound.");
        add(a, DU_BUTTON, C_AC_SENTRY_SET, 436, 66, 88, 24, "S&ettings...");
        add(a, DU_GROUP, 2101, 20, 150, 516, 100, "ShowSounds");
        ac_check(a, C_AC_SHOW_ON, 32, 170, 400, "Use S&howSounds", s->show_sounds ? ACC_ON : 0, ACC_ON);
        ac_note(a, C_AC_NOTE1, 32, 194, 490, "Use ShowSounds if you want programs to display captions for the speech and sounds they make. "
                "The desktop shows a caption for its own alert sounds.");
        break;
    case AC_DISPLAY:
        add(a, DU_GROUP, 2100, 20, 40, 516, 76, "High Contrast");
        ac_check(a, C_AC_HC_ON, 32, 58, 380, "Use &High Contrast", s->contrast, ACC_ON);
        ac_note(a, C_AC_NOTE0, 32, 80, 390, "Use High Contrast if you want Windows to use colors and fonts designed for easy reading.");
        add(a, DU_BUTTON, C_AC_HC_SET, 436, 62, 88, 24, "Se&ttings...");
        add(a, DU_GROUP, 2101, 20, 124, 516, 116, "Mouse pointer");
        for (unsigned k = 0; k < ACC_POINTERS; k++) a->ac_items[k] = (struct du_item){.text = {acc_pointer_names[k]}, .selected = k == s->pointer};
        w = add(a, DU_LISTVIEW, C_AC_POINTERS, 32, 144, 300, 88, ""); w->items = a->ac_items; w->count = ACC_POINTERS; w->active = s->pointer;
        list_style(w, "Pointer scheme");
        ac_note(a, C_AC_NOTE1, 344, 144, 184, "Large pointers use the bigger drawings; inverted ones swap black and white.");
        add(a, DU_GROUP, 2102, 20, 248, 516, 80, "Magnifier");
        ac_check(a, C_AC_MAG_ON, 32, 266, 380, "Use &Magnifier", s->mag_on ? ACC_ON : 0, ACC_ON);
        ac_note(a, C_AC_NOTE2, 32, 290, 390, "Magnifier enlarges part of the screen in a panel along its top or bottom edge.");
        add(a, DU_BUTTON, C_AC_MAG_SET, 436, 272, 88, 24, "Set&tings...");
        break;
    case AC_MOUSE:
        add(a, DU_GROUP, 2100, 20, 40, 516, 100, "MouseKeys");
        ac_check(a, C_AC_MOUSE_ON, 32, 60, 380, "Use &MouseKeys", s->mouse, ACC_ON);
        ac_note(a, C_AC_NOTE0, 32, 84, 390, "Use MouseKeys if you want to control the pointer with the numeric keypad on your keyboard.");
        add(a, DU_BUTTON, C_AC_MOUSE_SET, 436, 66, 88, 24, "Se&ttings...");
        break;
    default:   /* AC_GENERAL */
        add(a, DU_GROUP, 2100, 20, 40, 516, 100, "Automatic reset");
        ac_check(a, C_AC_TIMEOUT_ON, 32, 60, 500, "Turn off accessibility features after &idle for:", s->timeout, ACC_ATF_ON);
        ac_slider(a, C_AC_TIMEOUT_MIN, 48, 90, 300, 6, 0);
        add(a, DU_LABEL, C_AC_TIMEOUT_V, 360, 90, 120, 17, "");
        add(a, DU_GROUP, 2101, 20, 148, 516, 84, "Notification");
        ac_check(a, C_AC_FEEDBACK, 32, 168, 500, "Give warning when turning a feature off by the time-out", s->timeout, ACC_ATF_FEEDBACK);
        ac_check(a, C_AC_STATUS_ON, 32, 192, 500, "Show &status indicator on screen", s->status ? ACC_ON : 0, ACC_ON);
        add(a, DU_GROUP, 2102, 20, 240, 516, 76, "SerialKey devices");
        w = ac_check(a, C_AC_SERIAL, 32, 260, 500, "Support SerialKey devices", 0, 0); w->disabled = 1;
        ac_note(a, C_AC_NOTE1, 32, 284, 490, "SerialKeys (alternative input devices on a serial port) is not available in CiukiOS.");
        break;
    }
}
static void ac_build_sub(struct cp_app *a)
{
    const struct access_settings *s = &a->cfg.access;
    struct du_widget *w;
    switch (a->ac_view) {
    case AC_V_STICKY:
        ac_hotkey_block(a, 50, s->sticky, "&Use shortcut (Press the Shift key five times to turn StickyKeys on or off)");
        ac_check(a, C_AC_S_LOCK, 28, 120, 500, "Press modifier key t&wice to lock", s->sticky, ACC_SKF_TRISTATE);
        ac_check(a, C_AC_S_TWO, 28, 144, 500, "T&urn StickyKeys off if two keys are pressed at once", s->sticky, ACC_SKF_TWOKEYSOFF);
        ac_check(a, C_AC_S_AUDIBLE, 28, 168, 500, "Make &sounds when modifier key is pressed", s->sticky, ACC_SKF_AUDIBLE);
        ac_check(a, C_AC_INDICATOR, 28, 192, 500, "Show StickyKeys status on &screen", s->sticky, ACC_INDICATOR);
        break;
    case AC_V_FILTER: {
        ac_check(a, C_AC_HOTKEY, 28, 46, 500, "&Use shortcut (Hold the right Shift key for eight seconds to turn FilterKeys on or off)", s->filter, ACC_HOTKEY);
        const unsigned mode = s->filter_bounce ? 2 : 1;
        w = add(a, DU_RADIO, C_AC_F_MODE1, 28, 72, 500, 22, "Ignore quick keystrokes and slow down the repeat rate (SlowKeys and RepeatKeys)");
        w->total = 1; w->value = mode == 1;
        add(a, DU_LABEL, 2110, 48, 98, 150, 14, "Acceptance delay:");
        ac_slider(a, C_AC_F_WAIT, 200, 98, 220, AC_COUNT(ac_wait_ms), ac_nearest(ac_wait_ms, AC_COUNT(ac_wait_ms), s->filter_wait ? s->filter_wait : 1000));
        add(a, DU_LABEL, C_AC_F_WAIT_V, 430, 98, 90, 17, "");
        add(a, DU_LABEL, 2111, 48, 120, 150, 14, "Repeat delay:");
        ac_slider(a, C_AC_F_DELAY, 200, 120, 220, AC_COUNT(ac_repeat_ms), ac_nearest(ac_repeat_ms, AC_COUNT(ac_repeat_ms), s->filter_delay ? s->filter_delay : 1000));
        add(a, DU_LABEL, C_AC_F_DELAY_V, 430, 120, 90, 17, "");
        add(a, DU_LABEL, 2112, 48, 142, 150, 14, "Repeat rate:");
        ac_slider(a, C_AC_F_REPEAT, 200, 142, 220, AC_COUNT(ac_repeat_ms), ac_nearest(ac_repeat_ms, AC_COUNT(ac_repeat_ms), s->filter_repeat ? s->filter_repeat : 500));
        add(a, DU_LABEL, C_AC_F_REPEAT_V, 430, 142, 90, 17, "");
        ac_check(a, C_AC_F_NOREPEAT, 48, 162, 480, "&No keyboard repeat", s->filter, ACC_FKF_NOREPEAT);
        w = add(a, DU_RADIO, C_AC_F_MODE2, 28, 192, 500, 22, "Ignore repeated keystrokes (BounceKeys)");
        w->total = 1; w->value = mode == 2;
        add(a, DU_LABEL, 2113, 48, 218, 150, 14, "Wait before accepting:");
        ac_slider(a, C_AC_F_BOUNCE, 200, 218, 220, AC_COUNT(ac_bounce_ms), ac_nearest(ac_bounce_ms, AC_COUNT(ac_bounce_ms), s->filter_bounce ? s->filter_bounce : 500));
        add(a, DU_LABEL, C_AC_F_BOUNCE_V, 430, 218, 90, 17, "");
        ac_check(a, C_AC_F_CLICK, 28, 242, 500, "&Beep when keys pressed or accepted", s->filter, ACC_FKF_CLICK);
        ac_check(a, C_AC_INDICATOR, 28, 264, 500, "Show FilterKey status on &screen", s->filter, ACC_INDICATOR);
        ac_check(a, C_AC_CONFIRM, 28, 286, 250, "&Give warning message", s->filter, ACC_CONFIRM);
        ac_check(a, C_AC_SOUND, 280, 286, 250, "Make a &sound", s->filter, ACC_SOUND);
        break;
    }
    case AC_V_TOGGLE:
        ac_hotkey_block(a, 50, s->toggle, "&Use shortcut (Hold down Num Lock for five seconds to turn ToggleKeys on or off)");
        break;
    case AC_V_SENTRY: {
        add(a, DU_LABEL, C_AC_NOTE0, 28, 50, 500, 16, "Choose the warning for sounds from programs and windowed DOS programs:");
        static const char *const effects[4] = {"&None", "Flash active &caption bar", "Flash active &window", "Flash &desktop"};
        for (unsigned k = 0; k < 4; k++) {
            w = add(a, DU_RADIO, C_AC_SS0 + (int)k, 40, 76 + (int)k * 28, 400, 22, effects[k]);
            w->total = 2; w->value = s->sentry_effect == k;
        }
        ac_note(a, C_AC_NOTE1, 28, 200, 490, "Full-screen DOS programs are not covered: the desktop does not see their sounds.");
        break;
    }
    case AC_V_CONTRAST:
        ac_hotkey_block(a, 50, s->contrast, "&Use shortcut (Press left Alt+left Shift+Print Screen to turn High Contrast on or off)");
        add(a, DU_LABEL, C_AC_NOTE0, 28, 118, 300, 16, "Choose a high contrast scheme:");
        for (unsigned k = 0; acc_high_contrast_names[k]; k++)
            a->ac_items[k] = (struct du_item){.text = {acc_high_contrast_names[k], ac_scheme_notes[k]}, .selected = !strcmp(s->scheme, acc_high_contrast_names[k])};
        w = add(a, DU_LISTVIEW, C_AC_SCHEMES, 28, 138, 500, 90, ""); w->items = a->ac_items; w->count = 4; w->view = DU_DETAILS;
        w->column_count = 2; w->columns[0] = (struct du_column){"Scheme", 170, 0}; w->columns[1] = (struct du_column){"Colours", 310, 0};
        for (unsigned k = 0; k < 4; k++) if (!strcmp(s->scheme, acc_high_contrast_names[k])) w->active = k;
        ac_note(a, C_AC_NOTE1, 28, 236, 500, "The scheme applies to the desktop, the Control Panel and every program that follows the system theme.");
        break;
    case AC_V_MOUSE: {
        ac_hotkey_block(a, 46, s->mouse, "&Use shortcut (Press left Alt+left Shift+Num Lock to turn MouseKeys on or off)");
        add(a, DU_LABEL, 2120, 28, 120, 230, 16, "Use MouseKeys when Num Lock is:");
        w = add(a, DU_RADIO, C_AC_M_NUM0, 270, 118, 70, 22, "O&n"); w->total = 3; w->value = (s->mouse & ACC_MKF_REPLACENUMBERS) != 0;
        w = add(a, DU_RADIO, C_AC_M_NUM1, 346, 118, 70, 22, "O&ff"); w->total = 3; w->value = !(s->mouse & ACC_MKF_REPLACENUMBERS);
        add(a, DU_LABEL, 2121, 28, 148, 150, 14, "Top speed:");
        ac_slider(a, C_AC_M_SPEED, 180, 148, 230, AC_COUNT(ac_speed_steps), ac_nearest(ac_speed_steps, AC_COUNT(ac_speed_steps), s->mouse_max));
        add(a, DU_LABEL, C_AC_M_SPEED_V, 420, 148, 100, 17, "");
        add(a, DU_LABEL, 2122, 28, 172, 150, 14, "Acceleration:");
        ac_slider(a, C_AC_M_TIME, 180, 172, 230, AC_COUNT(ac_time_ms), ac_nearest(ac_time_ms, AC_COUNT(ac_time_ms), s->mouse_time));
        add(a, DU_LABEL, C_AC_M_TIME_V, 420, 172, 100, 17, "");
        ac_check(a, C_AC_M_MOD, 28, 198, 500, "&Hold down Ctrl to speed up and Shift to slow down", s->mouse, ACC_MKF_MODIFIERS);
        add(a, DU_LABEL, 2123, 48, 224, 130, 14, "Ctrl speed-up:");
        ac_slider(a, C_AC_M_CTRL, 180, 224, 230, AC_COUNT(ac_ctrl_steps), ac_nearest(ac_ctrl_steps, AC_COUNT(ac_ctrl_steps), s->mouse_ctrl));
        add(a, DU_LABEL, C_AC_M_CTRL_V, 420, 224, 100, 17, "");
        ac_check(a, C_AC_INDICATOR, 28, 250, 500, "Show MouseKey status on &screen", s->mouse, ACC_INDICATOR);
        break;
    }
    case AC_V_MAGNIFIER:
        ac_check(a, C_AC_G_ON, 28, 46, 400, "Use &Magnifier", s->mag_on ? ACC_ON : 0, ACC_ON);
        add(a, DU_LABEL, 2130, 28, 78, 150, 14, "Magnification level:");
        ac_slider(a, C_AC_G_LEVEL, 180, 78, 230, ACC_MAG_LEVEL_MAX, s->mag_level - 1);
        add(a, DU_LABEL, C_AC_G_LEVEL_V, 420, 78, 100, 17, "");
        add(a, DU_LABEL, 2131, 28, 106, 150, 14, "Panel height:");
        ac_slider(a, C_AC_G_SIZE, 180, 106, 230, (ACC_MAG_PERCENT_MAX - ACC_MAG_PERCENT_MIN) / 5 + 1, (s->mag_percent - ACC_MAG_PERCENT_MIN) / 5);
        add(a, DU_LABEL, C_AC_G_SIZE_V, 420, 106, 100, 17, "");
        add(a, DU_LABEL, 2132, 28, 136, 400, 14, "Tracking:");
        ac_check(a, C_AC_G_MOUSE, 48, 156, 210, "Follow &mouse cursor", s->mag_follow, ACC_MAG_MOUSE);
        ac_check(a, C_AC_G_FOCUS, 48, 180, 210, "Follow &keyboard focus", s->mag_follow, ACC_MAG_FOCUS);
        ac_check(a, C_AC_G_TEXT, 48, 204, 210, "Follow &text editing", s->mag_follow, ACC_MAG_TEXT);
        ac_check(a, C_AC_G_INVERT, 28, 232, 400, "&Invert colours", s->mag_invert ? ACC_ON : 0, ACC_ON);
        w = add(a, DU_RADIO, C_AC_G_TOP, 270, 156, 240, 22, "Dock at the &top"); w->total = 4; w->value = s->mag_dock == ACC_DOCK_TOP;
        w = add(a, DU_RADIO, C_AC_G_BOTTOM, 270, 180, 240, 22, "Dock at the &bottom"); w->total = 4; w->value = s->mag_dock == ACC_DOCK_BOTTOM;
        ac_note(a, C_AC_NOTE1, 270, 204, 250, "Win+Up and Win+Down change the level, Win+Page Up inverts, Win+Page Down toggles mouse tracking.");
        break;
    default: break;
    }
    ac_ok_cancel(a);
}
static void ac_sync(struct cp_app *a)
{
    const struct access_settings *s = &a->cfg.access;
    char t[40];
    if (a->ac_view == AC_V_FILTER) {
        ac_seconds(a, C_AC_F_WAIT_V, ac_wait_ms[ac_nearest(ac_wait_ms, AC_COUNT(ac_wait_ms), s->filter_wait ? s->filter_wait : 1000)]);
        ac_seconds(a, C_AC_F_DELAY_V, ac_repeat_ms[ac_nearest(ac_repeat_ms, AC_COUNT(ac_repeat_ms), s->filter_delay ? s->filter_delay : 1000)]);
        ac_seconds(a, C_AC_F_REPEAT_V, ac_repeat_ms[ac_nearest(ac_repeat_ms, AC_COUNT(ac_repeat_ms), s->filter_repeat ? s->filter_repeat : 500)]);
        ac_seconds(a, C_AC_F_BOUNCE_V, ac_bounce_ms[ac_nearest(ac_bounce_ms, AC_COUNT(ac_bounce_ms), s->filter_bounce ? s->filter_bounce : 500)]);
        int bounce = s->filter_bounce != 0;
        struct du_widget *w;
        if ((w = du_get(a->ui, C_AC_F_WAIT))) w->disabled = bounce;
        if ((w = du_get(a->ui, C_AC_F_DELAY))) w->disabled = bounce;
        if ((w = du_get(a->ui, C_AC_F_REPEAT))) w->disabled = bounce || (s->filter & ACC_FKF_NOREPEAT);
        if ((w = du_get(a->ui, C_AC_F_NOREPEAT))) w->disabled = 0;
        if ((w = du_get(a->ui, C_AC_F_BOUNCE))) w->disabled = !bounce;
    } else if (a->ac_view == AC_V_MOUSE) {
        snprintf(t, sizeof(t), "%u pixels/s", s->mouse_max); text(a, C_AC_M_SPEED_V, t);
        ac_seconds(a, C_AC_M_TIME_V, s->mouse_time);
        snprintf(t, sizeof(t), "%u times", s->mouse_ctrl); text(a, C_AC_M_CTRL_V, t);
        struct du_widget *w;
        if ((w = du_get(a->ui, C_AC_M_CTRL))) w->disabled = !(s->mouse & ACC_MKF_MODIFIERS);
    } else if (a->ac_view == AC_V_MAGNIFIER) {
        snprintf(t, sizeof(t), "%ux", s->mag_level); text(a, C_AC_G_LEVEL_V, t);
        snprintf(t, sizeof(t), "%u%% of the screen", s->mag_percent); text(a, C_AC_G_SIZE_V, t);
    } else if (a->ac_view == AC_V_MAIN && a->ac_tab == AC_GENERAL) {
        unsigned minutes = s->timeout_ms / 60000u;
        snprintf(t, sizeof(t), "%u minutes", minutes); text(a, C_AC_TIMEOUT_V, t);
        struct du_widget *w;
        if ((w = du_get(a->ui, C_AC_TIMEOUT_MIN))) { w->value = (int)(minutes / 5u ? minutes / 5u - 1 : 0); w->disabled = !(s->timeout & ACC_ATF_ON); }
        if ((w = du_get(a->ui, C_AC_FEEDBACK))) w->disabled = !(s->timeout & ACC_ATF_ON);
    }
}
static void ac_build(struct cp_app *a)
{
    struct du_widget *w = du_get(a->ui, C_TABS);
    ac_line_count = 0;
    if (a->ac_view == AC_V_MAIN) {
        for (unsigned k = 0; k < AC_TABS; k++) a->ac_tabs[k].text[0] = ac_tab_names[k];
        w->id = C_AC_TABS; w->items = a->ac_tabs; w->count = AC_TABS; w->active = (unsigned)a->ac_tab;
        ac_build_main(a);
    } else {
        a->ac_tabs[0].text[0] = ac_view_names[a->ac_view];
        w->id = C_AC_TABS; w->items = a->ac_tabs; w->count = 1; w->active = 0;
        ac_build_sub(a);
    }
    ac_sync(a);
}
static void ac_goto(struct cp_app *a, int view)
{
    if (view != AC_V_MAIN && a->ac_view == AC_V_MAIN) a->ac_backup = a->cfg.access;
    a->ac_view = view;
    build(a);
}
/* The flag word a sub-view's common check boxes edit. */
static uint32_t *ac_flags(struct cp_app *a)
{
    struct access_settings *s = &a->cfg.access;
    switch (a->ac_view) {
    case AC_V_STICKY: return &s->sticky;
    case AC_V_FILTER: return &s->filter;
    case AC_V_TOGGLE: return &s->toggle;
    case AC_V_CONTRAST: return &s->contrast;
    case AC_V_MOUSE: return &s->mouse;
    default: return NULL;
    }
}
static void ac_flag(uint32_t *word, unsigned bit, int on) { if (on) *word |= bit; else *word &= ~bit; }

/* 1 when the event belongs to Accessibility Options. */
static int ac_event(struct cp_app *a, const struct du_event *e)
{
    if (a->page != CP_ACCESS) return 0;
    struct access_settings *s = &a->cfg.access;
    struct du_ui *u = a->ui;
    int change = e->type == DU_EV_CHANGE || e->type == DU_EV_ACTIVATE;
    int on = e->id >= C_AC_STICKY_ON && u && du_get(u, e->id) ? du_get(u, e->id)->value != 0 : 0;
    uint32_t *flags = ac_flags(a);
    switch (e->id) {
    case C_AC_TABS:
        if (e->type == DU_EV_SELECT && a->ac_view == AC_V_MAIN && e->index >= 0 && e->index < AC_TABS && e->index != a->ac_tab) { a->ac_tab = e->index; build(a); }
        return 1;
    case C_AC_OK: if (e->type == DU_EV_ACTIVATE) { ac_save(a); ac_goto(a, AC_V_MAIN); } return 1;
    case C_AC_CANCEL:
        if (e->type == DU_EV_ACTIVATE) { a->cfg.access = a->ac_backup; ac_save(a); ac_goto(a, AC_V_MAIN); }
        return 1;
    /* main page */
    case C_AC_STICKY_ON: if (change) { ac_flag(&s->sticky, ACC_ON, on); ac_save(a); } return 1;
    case C_AC_FILTER_ON: if (change) { ac_flag(&s->filter, ACC_ON, on); ac_save(a); } return 1;
    case C_AC_TOGGLE_ON: if (change) { ac_flag(&s->toggle, ACC_ON, on); ac_save(a); } return 1;
    case C_AC_SENTRY_ON:
        if (change) {
            ac_flag(&s->sentry, ACC_ON, on);
            if (on && !s->sentry_effect) s->sentry_effect = ACC_SS_DISPLAY;   /* switched on with no warning chosen: flash the desktop */
            ac_save(a);
        }
        return 1;
    case C_AC_SHOW_ON: if (change) { s->show_sounds = on; ac_save(a); } return 1;
    case C_AC_HC_ON: if (change) { ac_flag(&s->contrast, ACC_ON, on); ac_save(a); } return 1;
    case C_AC_MAG_ON: if (change) { s->mag_on = on; ac_save(a); } return 1;
    case C_AC_MOUSE_ON: if (change) { ac_flag(&s->mouse, ACC_ON, on); ac_save(a); } return 1;
    case C_AC_STICKY_SET: if (e->type == DU_EV_ACTIVATE) ac_goto(a, AC_V_STICKY); return 1;
    case C_AC_FILTER_SET: if (e->type == DU_EV_ACTIVATE) ac_goto(a, AC_V_FILTER); return 1;
    case C_AC_TOGGLE_SET: if (e->type == DU_EV_ACTIVATE) ac_goto(a, AC_V_TOGGLE); return 1;
    case C_AC_SENTRY_SET: if (e->type == DU_EV_ACTIVATE) ac_goto(a, AC_V_SENTRY); return 1;
    case C_AC_HC_SET: if (e->type == DU_EV_ACTIVATE) ac_goto(a, AC_V_CONTRAST); return 1;
    case C_AC_MOUSE_SET: if (e->type == DU_EV_ACTIVATE) ac_goto(a, AC_V_MOUSE); return 1;
    case C_AC_MAG_SET: if (e->type == DU_EV_ACTIVATE) ac_goto(a, AC_V_MAGNIFIER); return 1;
    case C_AC_POINTERS:
        if (e->type == DU_EV_SELECT && e->index >= 0 && e->index < ACC_POINTERS) {
            s->pointer = (uint32_t)e->index; ac_save(a);
            for (unsigned k = 0; k < ACC_POINTERS; k++) a->ac_items[k].selected = k == s->pointer;
        }
        return 1;
    case C_AC_TIMEOUT_ON: if (change) { ac_flag(&s->timeout, ACC_ATF_ON, on); ac_save(a); ac_sync(a); } return 1;
    case C_AC_TIMEOUT_MIN:
        if (e->type == DU_EV_CHANGE) { s->timeout_ms = (uint32_t)(e->index + 1) * 5u * 60000u; ac_save(a); ac_sync(a); }
        return 1;
    case C_AC_FEEDBACK: if (change) { ac_flag(&s->timeout, ACC_ATF_FEEDBACK, on); ac_save(a); } return 1;
    case C_AC_STATUS_ON: if (change) { s->status = on; ac_save(a); } return 1;
    /* sub-views: the shared check boxes */
    case C_AC_HOTKEY: if (change && flags) { ac_flag(flags, ACC_HOTKEY, on); ac_save(a); } return 1;
    case C_AC_CONFIRM: if (change && flags) { ac_flag(flags, ACC_CONFIRM, on); ac_save(a); } return 1;
    case C_AC_SOUND: if (change && flags) { ac_flag(flags, ACC_SOUND, on); ac_save(a); } return 1;
    case C_AC_INDICATOR: if (change && flags) { ac_flag(flags, ACC_INDICATOR, on); ac_save(a); } return 1;
    case C_AC_S_LOCK: if (change) { ac_flag(&s->sticky, ACC_SKF_TRISTATE, on); ac_save(a); } return 1;
    case C_AC_S_TWO: if (change) { ac_flag(&s->sticky, ACC_SKF_TWOKEYSOFF, on); ac_save(a); } return 1;
    case C_AC_S_AUDIBLE: if (change) { ac_flag(&s->sticky, ACC_SKF_AUDIBLE, on); ac_save(a); } return 1;
    /* FilterKeys */
    case C_AC_F_MODE1: case C_AC_F_MODE2:
        if (e->type == DU_EV_CHANGE && e->index) {
            if (e->id == C_AC_F_MODE2) {   /* BounceKeys excludes the other two (FILTERKEYS: iBounceMSec) */
                if (!s->filter_bounce) s->filter_bounce = 500;
                s->filter_wait = s->filter_delay = s->filter_repeat = 0;
            } else {
                s->filter_bounce = 0;
                if (!s->filter_wait) s->filter_wait = 1000;
                if (!s->filter_delay) s->filter_delay = 1000;
                if (!s->filter_repeat) s->filter_repeat = 500;
            }
            ac_save(a); ac_sync(a);
        }
        return 1;
    case C_AC_F_WAIT: if (e->type == DU_EV_CHANGE && e->index >= 0 && (unsigned)e->index < AC_COUNT(ac_wait_ms)) { s->filter_wait = ac_wait_ms[e->index]; s->filter_bounce = 0; ac_save(a); ac_sync(a); } return 1;
    case C_AC_F_DELAY: if (e->type == DU_EV_CHANGE && e->index >= 0 && (unsigned)e->index < AC_COUNT(ac_repeat_ms)) { s->filter_delay = ac_repeat_ms[e->index]; s->filter_bounce = 0; ac_save(a); ac_sync(a); } return 1;
    case C_AC_F_REPEAT: if (e->type == DU_EV_CHANGE && e->index >= 0 && (unsigned)e->index < AC_COUNT(ac_repeat_ms)) { s->filter_repeat = ac_repeat_ms[e->index]; s->filter_bounce = 0; ac_save(a); ac_sync(a); } return 1;
    case C_AC_F_BOUNCE: if (e->type == DU_EV_CHANGE && e->index >= 0 && (unsigned)e->index < AC_COUNT(ac_bounce_ms)) { s->filter_bounce = ac_bounce_ms[e->index]; s->filter_wait = s->filter_delay = s->filter_repeat = 0; ac_save(a); ac_sync(a); } return 1;
    case C_AC_F_NOREPEAT: if (change) { ac_flag(&s->filter, ACC_FKF_NOREPEAT, on); ac_save(a); ac_sync(a); } return 1;
    case C_AC_F_CLICK: if (change) { ac_flag(&s->filter, ACC_FKF_CLICK, on); ac_save(a); } return 1;
    /* SoundSentry */
    case C_AC_SS0: case C_AC_SS1: case C_AC_SS2: case C_AC_SS3:
        if (e->type == DU_EV_CHANGE && e->index) { s->sentry_effect = (uint32_t)(e->id - C_AC_SS0); ac_save(a); }
        return 1;
    /* High Contrast */
    case C_AC_SCHEMES:
        if (e->type == DU_EV_SELECT && e->index >= 0 && e->index < 4) {
            snprintf(s->scheme, sizeof(s->scheme), "%s", acc_high_contrast_names[e->index]); ac_save(a);
            for (unsigned k = 0; k < 4; k++) a->ac_items[k].selected = k == (unsigned)e->index;
        }
        return 1;
    /* MouseKeys */
    case C_AC_M_NUM0: case C_AC_M_NUM1:
        if (e->type == DU_EV_CHANGE && e->index) { ac_flag(&s->mouse, ACC_MKF_REPLACENUMBERS, e->id == C_AC_M_NUM0); ac_save(a); }
        return 1;
    case C_AC_M_MOD: if (change) { ac_flag(&s->mouse, ACC_MKF_MODIFIERS, on); ac_save(a); ac_sync(a); } return 1;
    case C_AC_M_SPEED: if (e->type == DU_EV_CHANGE && e->index >= 0 && (unsigned)e->index < AC_COUNT(ac_speed_steps)) { s->mouse_max = ac_speed_steps[e->index]; ac_save(a); ac_sync(a); } return 1;
    case C_AC_M_TIME: if (e->type == DU_EV_CHANGE && e->index >= 0 && (unsigned)e->index < AC_COUNT(ac_time_ms)) { s->mouse_time = ac_time_ms[e->index]; ac_save(a); ac_sync(a); } return 1;
    case C_AC_M_CTRL: if (e->type == DU_EV_CHANGE && e->index >= 0 && (unsigned)e->index < AC_COUNT(ac_ctrl_steps)) { s->mouse_ctrl = ac_ctrl_steps[e->index]; ac_save(a); ac_sync(a); } return 1;
    /* Magnifier */
    case C_AC_G_ON: if (change) { s->mag_on = on; ac_save(a); } return 1;
    case C_AC_G_LEVEL: if (e->type == DU_EV_CHANGE && e->index >= 0 && e->index < (int)ACC_MAG_LEVEL_MAX) { s->mag_level = (uint32_t)e->index + 1; ac_save(a); ac_sync(a); } return 1;
    case C_AC_G_SIZE: if (e->type == DU_EV_CHANGE && e->index >= 0 && e->index <= (int)((ACC_MAG_PERCENT_MAX - ACC_MAG_PERCENT_MIN) / 5)) { s->mag_percent = ACC_MAG_PERCENT_MIN + (uint32_t)e->index * 5; ac_save(a); ac_sync(a); } return 1;
    case C_AC_G_MOUSE: if (change) { ac_flag(&s->mag_follow, ACC_MAG_MOUSE, on); ac_save(a); } return 1;
    case C_AC_G_FOCUS: if (change) { ac_flag(&s->mag_follow, ACC_MAG_FOCUS, on); ac_save(a); } return 1;
    case C_AC_G_TEXT: if (change) { ac_flag(&s->mag_follow, ACC_MAG_TEXT, on); ac_save(a); } return 1;
    case C_AC_G_INVERT: if (change) { s->mag_invert = on; ac_save(a); } return 1;
    case C_AC_G_TOP: case C_AC_G_BOTTOM:
        if (e->type == DU_EV_CHANGE && e->index) { s->mag_dock = e->id == C_AC_G_BOTTOM ? ACC_DOCK_BOTTOM : ACC_DOCK_TOP; ac_save(a); }
        return 1;
    default: return 0;
    }
}
/* Once a second: the desktop may have switched a feature with a hot key. */
static void ac_tick(struct cp_app *a)
{
    if (a->page != CP_ACCESS) return;
    struct access_settings before = a->cfg.access;
    ac_refresh_from_file(a);
    if (memcmp(&before, &a->cfg.access, sizeof(before))) build(a);
}
