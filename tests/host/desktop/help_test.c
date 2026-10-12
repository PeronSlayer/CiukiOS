/* CiukiOS Help host tests: topics, read-only text, files, renders.
 * SPDX-License-Identifier: MIT */
#include "help.h"
#include "acc_test.h"
#include <errno.h>
int setenv(const char *, const char *, int);
int main(int argc, char **argv)
{
    assert(argc >= 2);
    const char *scratch = argv[1], *folder = argc > 2 ? argv[2] : NULL;
    setenv("HOME", scratch, 1);
    assert(help_topic_count >= 8);
    for (unsigned i = 0; i < help_topic_count; i++) {
        assert(strlen(help_topics[i].text) < HELP_FILE_MAX);
        for (const char *p = help_topics[i].text; *p; p++) assert(*p == '\n' || (*p >= 0x20 && *p < 0x7f));   /* the glyph table is ASCII */
        assert(!strncmp(help_topics[i].text, help_topics[i].title, strlen(help_topics[i].title)));
    }
    int dedication = 0;
    for (unsigned i = 0; i < help_topic_count; i++) dedication |= strstr(help_topics[i].text, "In loving memory of Ciuk (Ciuki)") != NULL;
    assert(dedication);
    static uint32_t pixels[HELP_W * HELP_H];
    static struct du_ui ui; static struct help_app app;
    du_init(&ui, pixels, HELP_W, HELP_H);
    assert(!help_attach(&app, &ui));
    assert(app.topic == 0 && app.ta.readonly && strstr(app.ta.text, "Welcome to CiukiOS"));
    du_render(&ui);
    at_save(folder, "help-welcome", &ui);
    /* Keyboard: Down selects the next topic; typing never edits. */
    at_tap(&ui, DU_KEY_DOWN); assert(app.topic == 1);
    at_tap(&ui, DU_KEY_END); assert(app.topic == (int)help_topic_count - 1);
    du_focus(&ui, H_TEXT); size_t before = app.ta.len; at_tap(&ui, 0x1e); assert(app.ta.len == before);
    help_show_topic(&app, 3);
    du_render(&ui);
    at_save(folder, "help-shortcuts", &ui);
    /* Files: CR LF text, binary refusal, missing file. */
    char path[1024]; snprintf(path, sizeof(path), "%s/README.TXT", scratch);
    FILE *f = fopen(path, "wb"); assert(f); fputs("CiukiOS README\r\nLine two\r\n", f); fclose(f);
    assert(!help_open_file(&app, path) && app.topic == -1 && !strcmp(app.ta.text, "CiukiOS README\nLine two\n"));
    assert(strstr(du_get(&ui, H_STATUS)->text, "(read-only)"));
    snprintf(path, sizeof(path), "%s/data.bin", scratch);
    f = fopen(path, "wb"); assert(f); fwrite("A\0B", 1, 3, f); fclose(f);
    assert(help_open_file(&app, path) == -EINVAL && ui.dialog.kind == DU_DLG_MESSAGE);
    du_dialog_close(&ui);
    snprintf(path, sizeof(path), "%s/none.txt", scratch);
    assert(help_open_file(&app, path) == -ENOENT);
    du_dialog_close(&ui);
    /* Copy a selection. */
    du_ta_select(&app.ta, 0, 7);
    help_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = H_M_COPY}, &app);
    assert(!strcmp(du_clipboard_get(), "CiukiOS"));
    uint32_t *large = calloc(760 * 520 + 1, 4); assert(large);
    large[760 * 520] = 0xdeadbeef;
    size_t cursor = app.ta.cursor, anchor = app.ta.anchor;
    du_resize(&ui, large, 760, 520);
    assert(app.ta.cursor == cursor && app.ta.anchor == anchor && app.ta.readonly);
    assert(!strcmp(app.ta.text, "CiukiOS README\nLine two\n"));
    assert(du_get(&ui, H_STATUS)->box.y == 500 && du_get(&ui, H_STATUS)->box.h == 20);
    du_render(&ui); assert(large[760 * 520] == 0xdeadbeef);
    du_resize(&ui, pixels, HELP_W, HELP_H); free(large);
    help_detach(&app);
    puts("help: PASS");
    return 0;
}
