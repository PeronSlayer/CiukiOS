/* SPDX-License-Identifier: MIT */
#include "control.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv)
{
    assert(argc == 3); static struct cp_app a; static struct du_ui ui; static uint32_t pixels[CP_WIDTH * CP_HEIGHT];
    a.host.fonts_path = argv[1];
    du_init(&ui, pixels, CP_WIDTH, CP_HEIGHT); ui.focused_window = 1;
    assert(cp_page_index("fonts") == CP_FONTS); assert(!cp_attach(&a, &ui, CP_FONTS));
    struct du_widget *list = du_get(&ui, CP_FONT_LIST);
    assert(list && list->count == 12 && list->column_count == 3 && list->items == a.font_items && a.font_face);
    assert(!strcmp(list->columns[0].title, "Family") && !strcmp(list->columns[1].title, "Style") && !strcmp(list->columns[2].title, "File"));
    for (unsigned i = 0; i < a.font_count; i++) {
        assert(strstr(a.fonts[i].family, "Liberation ") == a.fonts[i].family && a.fonts[i].style[0] && strstr(a.fonts[i].file, ".ttf"));
        struct du_event ev = {.type = DU_EV_SELECT, .id = CP_FONT_LIST, .index = (int)i}; ui.event(&ui, &ev, ui.user);
        du_render(&ui); assert(a.font_selected == i && a.font_face);
        unsigned aa = 0;
        for (int y = 264; y < 283; y++) for (int x = 32; x < CP_WIDTH - 32; x++) {
            uint32_t p = pixels[y * CP_WIDTH + x] & 0xffffff;
            if (p != DU_WINDOW && p != DU_INK) aa++;
        }
        assert(aa > 30);  /* Both styles and all families draw grey coverage. */
    }
    /* The real list keyboard path changes the selected face and invalidates its preview. */
    du_focus(&ui, CP_FONT_LIST);
    struct ciuki_input_event key = {.type = CIUKI_INPUT_KEY, .code = DU_KEY_HOME, .value = 1};
    du_input(&ui, &key); du_render(&ui); assert(a.font_selected == 0);
    struct ciuki_font_cache_stats stats;
    assert(!ciuki_font_cache_info(a.font_face, &stats) && stats.glyphs > 20);
    uint64_t misses = stats.misses;
    ui.dirty = 1; du_render(&ui);
    assert(!ciuki_font_cache_info(a.font_face, &stats) && stats.misses == misses && stats.hits > 0);
    FILE *f = fopen(argv[2], "wb"); assert(f && fwrite(pixels, sizeof(*pixels), CP_WIDTH * CP_HEIGHT, f) == CP_WIDTH * CP_HEIGHT); fclose(f);
    cp_open(&a, CP_HOME); assert(!a.font_face && !a.font_count);
    cp_open(&a, CP_FONTS); assert(a.font_face && a.font_count == 12);
    cp_detach(&a); assert(!a.font_face);
    /* A missing store is a visible empty list, with safe page exit. */
    a.host.fonts_path = "/ciuki-missing-font-store"; cp_open(&a, CP_FONTS); du_render(&ui);
    assert(!a.font_count && !a.font_face && strstr(a.font_status, "unavailable")); cp_detach(&a);
    puts("PASS control fonts: twelve faces, real preview, keyboard selection, cache, lifetime and missing store");
    return 0;
}
