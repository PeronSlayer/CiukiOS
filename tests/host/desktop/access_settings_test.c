/* SPDX-License-Identifier: MIT
 * G25: the access_* keys of desktop.cfg and the high-contrast schemes. */
#include "settings.h"
#include "theme.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* WCAG 2.x relative luminance and contrast ratio (host test: floating point is fine here). */
static double linear(unsigned c)
{ double v = c / 255.0; return v <= 0.03928 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4); }
static double luminance(uint32_t c)
{ return 0.2126 * linear(c >> 16 & 255) + 0.7152 * linear(c >> 8 & 255) + 0.0722 * linear(c & 255); }
/* The contrast ratio times 100. */
static unsigned contrast(uint32_t a, uint32_t b)
{
    double la = luminance(a) + 0.05, lb = luminance(b) + 0.05;
    return (unsigned)((la > lb ? la / lb : lb / la) * 100.0);
}

static void defaults_and_round_trip(void)
{
    struct ciuki_settings s; cfg_defaults(&s);
    assert(s.access.sticky == 0x1fe && s.access.filter == 0x7e && s.access.mouse_max == 80 && s.access.mag_level == 2 && !s.access.pointer);
    char text[CFG_TEXT_MAX]; size_t n = cfg_format(&s, text, sizeof(text)); assert(n);
    /* The theme and the high-contrast line stay together at the top of the file (clients read its first 4 KiB). */
    const char *theme = strstr(text, "\ntheme="), *contrast_line = strstr(text, "\naccess_contrast="), *wall = strstr(text, "\nwallpaper=");
    assert(theme && contrast_line && wall && theme < contrast_line && contrast_line < wall && (size_t)(contrast_line - text) < 200);
    struct ciuki_settings t; cfg_defaults(&t);
    assert(!cfg_parse(&t, text, n));
    assert(!memcmp(&s.access, &t.access, sizeof(s.access)));
    /* Every field survives. */
    s.access.sticky = 0x1ff & ~ACC_SKF_TRISTATE; s.access.filter = 0x7f | ACC_FKF_NOREPEAT; s.access.filter_wait = 2000; s.access.filter_delay = 300;
    s.access.filter_repeat = 1400; s.access.filter_bounce = 700; s.access.toggle = 0x3f; s.access.mouse = 0xff; s.access.mouse_max = 360;
    s.access.mouse_time = 5000; s.access.mouse_ctrl = 20; s.access.sentry = 3; s.access.sentry_effect = ACC_SS_TITLE;
    s.access.contrast = 0x7f; strcpy(s.access.scheme, "High Contrast #2"); s.access.timeout = 3; s.access.timeout_ms = 1800000;
    s.access.show_sounds = 1; s.access.status = 0; s.access.pointer = ACC_POINTER_LARGE_INVERTED;
    s.access.mag_on = 1; s.access.mag_level = 9; s.access.mag_follow = 7; s.access.mag_invert = 1; s.access.mag_dock = ACC_DOCK_BOTTOM; s.access.mag_percent = 50;
    n = cfg_format(&s, text, sizeof(text)); assert(n);
    cfg_defaults(&t); assert(!cfg_parse(&t, text, n));
    assert(!memcmp(&s.access, &t.access, sizeof(s.access)));
    assert(strstr(text, "access_filter=107f,2000,300,1400,700\n") && strstr(text, "access_mouse=ff,360,5000,20\n"));
    assert(strstr(text, "access_magnifier=1,9,7,1,1,50\n") && strstr(text, "access_timeout=3,1800\n") && strstr(text, "access_general=1,0,3\n"));
    assert(!strcmp(cfg_effective_theme(&t), "High Contrast #2"));
    t.access.contrast &= ~ACC_ON;
    assert(!strcmp(cfg_effective_theme(&t), t.theme));
}

static void rejected_values(void)
{
    static const char *const bad[] = {
        "access_sticky=zz\n", "access_sticky=\n", "access_sticky=1ff,2\n", "access_sticky=123456789\n",
        "access_filter=7e,1000,1000,500\n", "access_filter=7e,1000,1000,500,20001\n", "access_filter=7e,-1,1000,500,0\n",
        "access_mouse=7e,9,3000,8\n", "access_mouse=7e,361,3000,8\n", "access_mouse=7e,80,999,8\n", "access_mouse=7e,80,3000,0\n",
        "access_sentry=2,4\n", "access_sentry=2\n", "access_contrast=7e,Nope\n", "access_contrast=7e\n",
        "access_timeout=3,59\n", "access_timeout=3,3601\n", "access_general=2,1,0\n", "access_general=0,0,4\n",
        "access_magnifier=1,0,7,0,0,25\n", "access_magnifier=1,10,7,0,0,25\n", "access_magnifier=1,2,8,0,0,25\n",
        "access_magnifier=1,2,7,0,2,25\n", "access_magnifier=1,2,7,0,0,9\n", "access_magnifier=1,2,7,0,0,51\n",
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        struct ciuki_settings s, ref; cfg_defaults(&s); cfg_defaults(&ref);
        assert(cfg_parse(&s, bad[i], strlen(bad[i])) == 1);
        assert(!memcmp(&s.access, &ref.access, sizeof(ref.access)));   /* the default stays */
    }
    /* Unknown access_ keys are kept like any other unknown key. */
    struct ciuki_settings s; cfg_defaults(&s);
    const char *text = "access_future=1\n";
    assert(!cfg_parse(&s, text, strlen(text)) && strstr(s.extra, "access_future=1"));
    char out[CFG_TEXT_MAX]; assert(cfg_format(&s, out, sizeof(out)) && strstr(out, "access_future=1\n"));
    /* Upper-case hex, CRLF lines and the last line wins. */
    cfg_defaults(&s);
    text = "access_sticky=1FF\r\naccess_sticky=105\r\n";
    assert(!cfg_parse(&s, text, strlen(text)) && s.access.sticky == 0x105);
    /* Win32 state bits (latched, locked) are not stored. */
    text = "access_sticky=ffffffff\n";
    cfg_defaults(&s);
    assert(!cfg_parse(&s, text, strlen(text)) && s.access.sticky == 0x1ff);   /* a DWORD: the state bits are masked off */
    text = "access_sticky=1000001ff\n";
    cfg_defaults(&s); assert(cfg_parse(&s, text, strlen(text)) == 1);   /* more than eight digits is refused */
    text = "access_sticky=ffff01ff\n";
    cfg_defaults(&s); assert(!cfg_parse(&s, text, strlen(text)) && s.access.sticky == 0x1ff);
}

static void themes(void)
{
    assert(desk_theme_count() == 2 && CFG_THEMES == 2);   /* the schemes are not selectable themes */
    for (unsigned i = 0; acc_high_contrast_names[i]; i++) {
        const struct desk_theme *t = desk_theme_named(acc_high_contrast_names[i]);
        assert(t && !strcmp(t->name, acc_high_contrast_names[i]));
        /* "a high degree of contrast": body text 7:1, selection and captions 4.5:1, disabled text 3:1 (WCAG AAA/AA). */
        assert(contrast(t->ink, t->window) >= 700);
        assert(contrast(t->select_ink, t->select) >= 450);
        assert(contrast(t->title_text, t->title[0]) >= 450);
        assert(contrast(t->gray, t->window) >= 300);
        assert(contrast(t->topbar_text, t->topbar) >= 700 && contrast(t->taskbar_text, t->taskbar) >= 700);
        assert(contrast(t->frame, t->window) >= 700);
        assert(t->radius == 0 && t->shadow_size == 0 && t->title_style == DESK_TITLE_FLAT);
    }
    /* A scheme is selected by name, and only the high-contrast line can name it. */
    assert(!desk_theme_use("High Contrast White") && !strcmp(desk_theme->name, "High Contrast White"));
    assert(!desk_theme_use("ciuki blue"));
    struct ciuki_settings s; cfg_defaults(&s);
    const char *line = "theme=High Contrast Black\n";
    assert(cfg_parse(&s, line, strlen(line)) == 1 && !strcmp(s.theme, DESK_THEME_DEFAULT));
    char name[32];
    const char *file = "theme=Platinum\naccess_contrast=7f,High Contrast White\n";
    assert(!desk_settings_theme(file, strlen(file), name, sizeof(name)) && !strcmp(name, "High Contrast White"));
    file = "theme=Platinum\naccess_contrast=7e,High Contrast White\n";   /* off */
    assert(!desk_settings_theme(file, strlen(file), name, sizeof(name)) && !strcmp(name, "Platinum"));
    file = "access_contrast=7f,High Contrast #1\naccess_contrast=7e,High Contrast #1\ntheme=Platinum\n";   /* the last valid line wins */
    assert(!desk_settings_theme(file, strlen(file), name, sizeof(name)) && !strcmp(name, "Platinum"));
    file = "access_contrast=7F,High Contrast Black\r\n";
    assert(!desk_settings_theme(file, strlen(file), name, sizeof(name)) && !strcmp(name, "High Contrast Black"));
    file = "access_contrast=7f,Bogus\n";   /* an unknown scheme is ignored */
    assert(desk_settings_theme(file, strlen(file), name, sizeof(name)) == -1);
    file = "access_contrast=1,\n";
    assert(desk_settings_theme(file, strlen(file), name, sizeof(name)) == -1);
    file = "access_contrast=zz1,High Contrast Black\n";
    assert(desk_settings_theme(file, strlen(file), name, sizeof(name)) == -1);
    /* What cfg_format writes is what the theme engine reads. */
    cfg_defaults(&s); s.access.contrast |= ACC_ON; strcpy(s.access.scheme, "High Contrast #2");
    char text[CFG_TEXT_MAX]; size_t n = cfg_format(&s, text, sizeof(text));
    assert(n && !desk_settings_theme(text, n, name, sizeof(name)) && !strcmp(name, "High Contrast #2"));
}

int main(void)
{
    defaults_and_round_trip();
    rejected_values();
    themes();
    puts("PASS g25 settings: access_* keys round trip, rejections, Win32 state bits, high-contrast schemes and their contrast");
    return 0;
}
