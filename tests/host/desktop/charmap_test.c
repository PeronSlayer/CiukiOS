/* Character Map host tests. SPDX-License-Identifier: MIT */
#include "charmap.h"
#include "acc_test.h"
int main(int argc, char **argv)
{
    const char *folder = argc > 1 ? argv[1] : NULL;
    static uint32_t pixels[CHARMAP_W * CHARMAP_H];
    static struct du_ui ui; static struct charmap_app app;
    du_init(&ui, pixels, CHARMAP_W, CHARMAP_H);
    charmap_attach(&app, &ui);
    assert(app.selected == 'A' - CHARMAP_FIRST && strstr(du_get(&ui, CM_STATUS)->text, "U+0041"));
    struct du_rect r = charmap_cell('C' - CHARMAP_FIRST);
    at_click(&ui, r.x + 5, r.y + 5); assert(app.selected == 'C' - CHARMAP_FIRST);
    at_click(&ui, r.x + 5, r.y + 5); assert(!strcmp(du_get(&ui, CM_FIELD)->text, "C"));
    at_tap(&ui, DU_KEY_RIGHT); at_tap(&ui, DU_KEY_ENTER);
    at_tap(&ui, 0x0c); at_click_widget(&ui, CM_SELECT);     /* '-' selects it; Select adds */
    assert(!strcmp(du_get(&ui, CM_FIELD)->text, "CD-"));
    at_click_widget(&ui, CM_COPY); assert(!strcmp(du_clipboard_get(), "CD-"));
    at_tap(&ui, DU_KEY_DOWN); assert(app.selected == '-' - CHARMAP_FIRST + CHARMAP_COLS);
    charmap_select(&app, 1000); assert(app.selected == CHARMAP_COUNT - 1);
    charmap_select(&app, 'Q' - CHARMAP_FIRST);
    du_render(&ui);
    at_save(folder, "charmap", &ui);
    puts("charmap: PASS");
    return 0;
}
