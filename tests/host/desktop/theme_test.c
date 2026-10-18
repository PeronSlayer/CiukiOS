/* SPDX-License-Identifier: MIT
 * Default/persisted choices, settings polling and Blue readability samples. */
#include "theme.h"
#include "settings.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void settings_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb"); assert(f);
    assert(fwrite(text, 1, strlen(text), f) == strlen(text) && !fclose(f));
}
static void pair(const char *role, uint32_t ink, uint32_t face)
{ printf("PAIR %s %06x %06x\n", role, ink, face); }

int main(int argc, char **argv)
{
    assert(argc == 2);
    assert(!strcmp(desk_theme->name, "Ciuki Blue"));
    assert(desk_theme_count() == 2 && CFG_THEMES == 2);
    assert(!desk_theme_named("Classic 98") && cfg_theme_index("Classic 98") == -1);
    struct ciuki_settings s, restored;
    cfg_defaults(&s); assert(!strcmp(s.theme, "Ciuki Blue"));
    const char invalid[] = "theme=unknown\n";
    assert(cfg_parse(&s, invalid, sizeof(invalid) - 1) == 1 && !strcmp(s.theme, "Ciuki Blue"));
    char text[CFG_TEXT_MAX], name[32];
    for (int i = 0; i < desk_theme_count(); i++) {
        cfg_defaults(&s);
        snprintf(s.theme, sizeof(s.theme), "%s", desk_theme_at(i)->name);
        size_t n = cfg_format(&s, text, sizeof(text)); assert(n);
        cfg_defaults(&restored); assert(!cfg_parse(&restored, text, n));
        assert(!strcmp(restored.theme, s.theme));
        assert(!desk_settings_theme(text, n, name, sizeof(name)) && !strcmp(name, s.theme));
    }
    /* Missing file, choice, throttle, invalid key, removed theme after a
     * valid entry, repeated reload, and deletion. */
    assert(!desk_theme_use("Platinum"));
    assert(desk_theme_poll(argv[1], 0) && !strcmp(desk_theme->name, "Ciuki Blue"));
    settings_file(argv[1], "theme=Platinum\n");
    assert(!desk_theme_poll(argv[1], 1000000000ull));
    assert(desk_theme_poll(argv[1], 2000000000ull) && !strcmp(desk_theme->name, "Platinum"));
    settings_file(argv[1], invalid);
    assert(desk_theme_poll(argv[1], 4000000000ull) && !strcmp(desk_theme->name, "Ciuki Blue"));
    settings_file(argv[1], "theme=Platinum\r\ntheme=Classic 98\ntheme=unknown\n");
    assert(!desk_theme_use("Platinum"));
    assert(desk_theme_poll(argv[1], 6000000000ull) && !strcmp(desk_theme->name, "Ciuki Blue"));
    const char removed[] = "theme=Platinum\ntheme=Classic 98\n";
    cfg_defaults(&s);
    assert(!cfg_parse(&s, removed, sizeof(removed)-1) && !strcmp(s.theme, "Ciuki Blue"));
    assert(!desk_theme_poll(argv[1], 8000000000ull));
    assert(!remove(argv[1]));
    assert(!desk_theme_poll(argv[1], 10000000000ull) && !strcmp(desk_theme->name, "Ciuki Blue"));

    const struct desk_theme *t = desk_theme;
    pair("dialog", t->ink, t->face); pair("window", t->ink, t->window);
    pair("disabled", t->gray, t->face);
    pair("topbar", t->topbar_text, t->topbar);
    pair("taskbar_tray", t->taskbar_text, t->taskbar);
    pair("active_task", t->task_active_text, t->task_active);
    pair("start", t->start_text, t->start); pair("start_open", t->start_text, t->start_open);
    for (unsigned a = 0; a < 256; a++) {
        pair("active_caption", t->title_text, desk_mix(t->title[1], t->title[0], a));
        pair("inactive_caption", t->title_text_inactive, desk_mix(t->title_inactive[1], t->title_inactive[0], a));
        pair("menu_selection", t->select_ink, desk_mix(0xffffffu, t->select, a * 50 / 255));
    }
    uint32_t pixels[240 * 60];
    struct desk_paint p = {pixels, 240, 60, 0, 0, 240, 60};
    for (int pressed = 0; pressed < 2; pressed++) for (int hot = 0; hot < 2; hot++) {
        memset(pixels, 0, sizeof(pixels));
        desk_paint_raised(&p, 0, 0, 80, 24, pressed, hot);
        for (int y = 3; y < 21; y++) pair("button", t->ink, pixels[y * 240 + 40]);
    }
    for (int active = 0; active < 2; active++) for (int alert = 0; alert < 2; alert++) {
        struct desk_frame_style frame = {.title = "", .active = active, .alert = alert};
        memset(pixels, 0, sizeof(pixels)); desk_paint_frame(&p, 0, 0, 240, 60, &frame);
        assert(pixels[10 * 240 + 20] != pixels[10 * 240 + 210]);
        assert(pixels[10 * 240 + 20] == pixels[15 * 240 + 20]);
        for (int x = 4; x < 236; x++)
            pair(alert ? "warning_caption" : "rendered_caption",
                 active ? t->title_text : t->title_text_inactive, pixels[10 * 240 + x]);
    }
    puts("PASS Blue default, persisted choices, poll fallback and horizontal captions");
    return 0;
}
