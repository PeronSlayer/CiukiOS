/* Character Map host tests (G27): fonts, Unicode subsets, Latin-1, Go to Unicode, keystrokes.
 * SPDX-License-Identifier: MIT */
#include "charmap.h"
#include "acc_test.h"
static uint32_t pixels[CHARMAP_W * CHARMAP_H];
static struct du_ui ui; static struct charmap_app app;
static unsigned ink(struct du_rect r)     /* pixels that differ from the cell background: a drawn glyph */
{
    unsigned n = 0; uint32_t bg = pixels[r.y * CHARMAP_W + r.x];
    for (int y = r.y; y < r.y + r.h; y++) for (int x = r.x; x < r.x + r.w; x++) n += pixels[y * CHARMAP_W + x] != bg;
    return n;
}
static unsigned grey(struct du_rect r)    /* the same count, used for TrueType faces and the preview */
{
    return ink(r);
}
static void select_subset(const char *name)
{
    for (unsigned i = 0; i < charmap_subset_count; i++) if (!strcmp(charmap_subsets[i].name, name)) {
        struct du_event e = {.type = DU_EV_SELECT, .id = CM_SUBSETS, .index = (int)i}; charmap_event(&ui, &e, &app); return;
    }
    assert(!"subset");
}
static void select_font(const char *label)
{
    for (unsigned i = 0; i < app.font_count; i++) if (!strcmp(app.fonts[i].label, label)) {
        struct du_event e = {.type = DU_EV_SELECT, .id = CM_FONTS, .index = (int)i}; charmap_event(&ui, &e, &app); return;
    }
    fprintf(stderr, "no font '%s'\n", label); assert(!"font");
}
static void go(const char *hex)
{
    du_set_text(du_get(&ui, CM_GOTO), hex);
    struct du_event e = {.type = DU_EV_COMMIT, .id = CM_GOTO, .text = hex}; charmap_event(&ui, &e, &app);
}
int main(int argc, char **argv)
{
    assert(argc >= 2);
    const char *folder = argc > 2 ? argv[2] : NULL;
    app.fonts_path = argv[1];
    du_init(&ui, pixels, CHARMAP_W, CHARMAP_H);
    charmap_attach(&app, &ui);

    /* Fonts: the built-in face and every installed TrueType face. */
    assert(app.font_count == 13 && !strcmp(app.fonts[0].label, "Inter (CiukiOS UI)") && !app.face && app.font_selected == 0);
    assert(du_get(&ui, CM_FONTS)->count == 13 && du_get(&ui, CM_SUBSETS)->count == charmap_subset_count);
    for (unsigned i = 2; i < app.font_count; i++) assert(strcmp(app.fonts[i - 1].label, app.fonts[i].label) <= 0);   /* sorted after the built-in face */
    assert(app.selected == 'A' && strstr(du_get(&ui, CM_STATUS)->text, "U+0041") && !strcmp(du_get(&ui, CM_KEYSTROKE)->text, "Keystroke: Alt+0065"));

    /* The original behaviour on Basic Latin: click, second click adds, keys, Select and Copy. */
    struct du_rect r = charmap_cell('C' - 0x20);
    at_click(&ui, r.x + 5, r.y + 5); assert(app.selected == 'C');
    at_click(&ui, r.x + 5, r.y + 5); assert(!strcmp(du_get(&ui, CM_FIELD)->text, "C"));
    at_tap(&ui, DU_KEY_RIGHT); at_tap(&ui, DU_KEY_ENTER);
    at_tap(&ui, 0x0c); at_click_widget(&ui, CM_SELECT);     /* '-' selects it; Select adds */
    assert(!strcmp(du_get(&ui, CM_FIELD)->text, "CD-"));
    at_click_widget(&ui, CM_COPY); assert(!strcmp(du_clipboard_get(), "CD-"));
    at_tap(&ui, DU_KEY_DOWN); assert(app.selected == '-' + CHARMAP_COLS);
    charmap_select(&app, 1000); assert(app.selected == 0x7e);   /* clamped to the subset */
    charmap_select(&app, 'Q');
    du_render(&ui);
    at_save(folder, "charmap", &ui);
    assert(ink(charmap_cell('R' - 0x20)) > 8);                  /* the built-in face draws */

    /* Latin-1 Supplement: Italian accented letters in the built-in face, UTF-8 into the field and clipboard. */
    select_subset("Latin-1 Supplement");
    assert(app.subset == 1 && app.top == 0xa0 && app.selected == 0xa0);
    assert(charmap_covered(&app, 0xe8) && charmap_covered(&app, 0xff) && !charmap_covered(&app, 0x100));
    du_set_text(du_get(&ui, CM_FIELD), "");
    const uint32_t italian[] = {0xe0, 0xe8, 0xe9, 0xec, 0xf2, 0xf9};   /* a e e i o u with accents */
    for (unsigned i = 0; i < 6; i++) {
        struct du_rect c = charmap_cell((int)(italian[i] - 0xa0));
        at_click(&ui, c.x + 5, c.y + 5); at_click(&ui, c.x + 5, c.y + 5);          /* select, then add */
        assert(app.selected == italian[i]);
    }
    assert(!strcmp(du_get(&ui, CM_FIELD)->text, "\xc3\xa0\xc3\xa8\xc3\xa9\xc3\xac\xc3\xb2\xc3\xb9"));
    at_click_widget(&ui, CM_COPY); assert(!strcmp(du_clipboard_get(), "\xc3\xa0\xc3\xa8\xc3\xa9\xc3\xac\xc3\xb2\xc3\xb9"));
    assert(!strcmp(du_get(&ui, CM_KEYSTROKE)->text, "Keystroke: Alt+0249") && strstr(du_get(&ui, CM_STATUS)->text, "U+00F9"));
    du_render(&ui);
    assert(ink(charmap_cell(0xe8 - 0xa0)) > 8 && ink(charmap_cell(0xf9 - 0xa0)) > 8);
    at_save(folder, "charmap-latin1", &ui);

    /* A TrueType face: coverage comes from the font, the grid is anti-aliased. */
    select_font("Liberation Serif Regular");
    assert(app.face && app.font_selected != 0);
    assert(charmap_covered(&app, 0xe9) && charmap_covered(&app, 0x20ac) && charmap_covered(&app, 0x416) && !charmap_covered(&app, 0x4e2d));
    du_render(&ui);
    assert(grey(charmap_cell(0xe9 - 0xa0)) > 5);
    struct du_rect preview = {486, 212, 64, 64};
    assert(grey(preview) > 100);                                   /* enlarged anti-aliased preview of U+00E9 */
    select_font("Inter (CiukiOS UI)"); assert(!app.face && app.font_selected == 0);

    /* Subsets: ranges, scrolling, absent glyphs. */
    select_font("Liberation Serif Regular");
    select_subset("Cyrillic");
    assert(app.top == 0x400 && charmap_total_rows(&app) == 15 && du_get(&ui, CM_SCROLL)->total == 15 && du_get(&ui, CM_SCROLL)->page == CHARMAP_ROWS);
    du_render(&ui);
    assert(grey(charmap_cell(0x416 - 0x400)) > 5);
    at_tap(&ui, DU_KEY_PGDN); assert(app.top == 0x400 + 7 * CHARMAP_COLS && app.selected == 0x400 + CHARMAP_COLS * CHARMAP_ROWS);
    at_tap(&ui, DU_KEY_PGDN); at_tap(&ui, DU_KEY_PGDN);
    assert(app.top == 0x400 + 7 * CHARMAP_COLS);                   /* last page: 15 rows, 8 visible */
    at_tap(&ui, DU_KEY_END); assert(app.selected == 0x4ff);
    at_tap(&ui, DU_KEY_HOME); assert(app.selected == 0x400 && app.top == 0x400);
    struct du_event wheel = {.type = DU_EV_CHANGE, .id = CM_SCROLL, .index = 3}; charmap_event(&ui, &wheel, &app);
    assert(app.top == 0x400 + 3 * CHARMAP_COLS);
    charmap_scroll(&app, -10); assert(app.top == 0x400);
    charmap_select(&app, 0x4e0); assert(app.top == 0x400 + 5 * CHARMAP_COLS);   /* the selection scrolls into view */
    select_subset("Greek and Coptic"); assert(app.top == 0x370 && charmap_total_rows(&app) == 8);
    select_subset("Currency Symbols"); assert(charmap_covered(&app, 0x20ac) && charmap_covered(&app, 0x20a4));
    du_render(&ui);
    at_save(folder, "charmap-cyrillic-serif", &ui);

    /* Go to Unicode: switches subset and font-independent navigation. */
    go("e9"); assert(app.subset == 1 && app.selected == 0xe9 && app.top == 0xa0);
    go("0416"); assert(app.subset == 6 && app.selected == 0x416);
    go("2192"); assert(app.subset == 11 && app.selected == 0x2192 && charmap_covered(&app, 0x2192));
    uint32_t before = app.selected;
    go("zz"); assert(app.selected == before && strstr(du_get(&ui, CM_STATUS)->text, "Not in a listed"));
    go("4e2d"); assert(app.selected == before);                    /* no listed subset */
    go("110000"); assert(app.selected == before);
    assert(du_get(&ui, CM_SUBSETS)->active == 11);                 /* the list follows */
    /* Typing a character selects it from any subset. */
    at_tap(&ui, 0x1e); assert(app.selected == 'a' && app.subset == 0);

    /* Keystrokes of the Windows set. */
    char key[16];
    assert(charmap_keystroke(0xe9, key, sizeof(key)) && !strcmp(key, "Alt+0233"));
    assert(charmap_keystroke(0x20ac, key, sizeof(key)) && !strcmp(key, "Alt+0128"));
    assert(charmap_keystroke(' ', key, sizeof(key)) && !strcmp(key, "Alt+0032"));
    assert(!charmap_keystroke(0x416, key, sizeof(key)) && !key[0]);
    /* UTF-8 of every width. */
    char u[5];
    assert(charmap_utf8('A', u) == 1 && charmap_utf8(0xe9, u) == 2 && !strcmp(u, "\xc3\xa9"));
    assert(charmap_utf8(0x20ac, u) == 3 && !strcmp(u, "\xe2\x82\xac"));
    assert(charmap_utf8(0x1f600, u) == 4 && !strcmp(u, "\xf0\x9f\x98\x80"));
    assert(charmap_utf8(0xd800, u) == 3 && !strcmp(u, "\xef\xbf\xbd"));
    /* A missing font file falls back to the built-in face. */
    snprintf(app.fonts[1].file, sizeof(app.fonts[1].file), "missing.ttf");
    assert(charmap_set_font(&app, 1) == -1 && !app.face && app.font_selected == 0);
    charmap_detach(&app);
    puts("charmap: PASS");
    return 0;
}
