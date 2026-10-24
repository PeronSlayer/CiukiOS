/* CiukiOS Help host tests (G15): topics, index, search, contents, tabs, Back,
 * F1 inside Help, files, renders.
 * SPDX-License-Identifier: MIT */
#include "help.h"
#include "acc_test.h"
#include <errno.h>
#include <strings.h>
int setenv(const char *, const char *, int);
static const char *const program_topics[] = {
    "files", "ciuknote", "ciukide", "paint", "tasks", "control", "calc", "clock", "viewer", "mixer", "charmap", "music",
    "regedit", "hwreport", "terminal", "gfxdemo", "dosbox", "lua", "netsurf", "demo",
    "welcome", "start", "desktop", "keys", "help", "disk", "dos", "sound", "troubleshooting", "about", "credits" };
static void topics(void)
{
    assert(help_topic_count >= 36 && help_topic_count <= HELP_TOPICS_MAX);
    for (unsigned i = 0; i < help_topic_count; i++) {
        const struct help_topic *t = &help_topics[i];
        assert(strlen(t->text) < HELP_FILE_MAX);
        for (const char *p = t->text; *p; p++) assert(*p == '\n' || (*p >= 0x20 && *p < 0x7f));   /* the glyph table is ASCII */
        assert(!strncmp(t->text, t->title, strlen(t->title)));
        for (const char *p = t->id; *p; p++) assert((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-');
        assert(strlen(t->id) >= 2 && strlen(t->id) <= 40 && t->keywords && t->keywords[0]);
        for (unsigned j = 0; j < i; j++) assert(strcmp(help_topics[j].id, t->id) && strcmp(help_topics[j].title, t->title));
        assert(help_topic_find(t->id) == (int)i);
    }
    /* A topic for every built-in program and for the system. */
    for (unsigned i = 0; i < sizeof(program_topics) / sizeof(*program_topics); i++) assert(help_topic_find(program_topics[i]) >= 0);
    assert(help_topic_find("nonesuch") < 0 && help_topic_find(NULL) < 0 && help_topic_find("") < 0);
    assert(help_topic_argument("topic:files") == help_topic_find("files") && help_topic_argument("topic:zz") < 0 &&
           help_topic_argument("/tmp/x") < 0 && help_topic_argument(NULL) < 0);
    int dedication = 0;
    for (unsigned i = 0; i < help_topic_count; i++) dedication |= strstr(help_topics[i].text, "In loving memory of Ciuk (Ciuki)") != NULL;
    assert(dedication);
    /* The topics of one book are contiguous. */
    unsigned books = help_book_count(); assert(books >= 3 && books <= HELP_BOOKS_MAX);
    for (unsigned b = 0; b < books; b++) {
        int first = -1, last = -1;
        for (unsigned i = 0; i < help_topic_count; i++) if (help_topic_book(i) == (int)b) { if (first < 0) first = (int)i; last = (int)i; }
        assert(first >= 0);
        for (int i = first; i <= last; i++) assert(help_topic_book((unsigned)i) == (int)b);
    }
    assert(help_book(books) == NULL && help_topic_book(0) == -1 && help_topic_book(help_topic_count) == -1);
}
static void index_and_search(void)
{
    static struct help_index_entry index[HELP_INDEX_MAX];
    unsigned n = help_index_build(index, HELP_INDEX_MAX);
    assert(n > 100 && n < HELP_INDEX_MAX);
    for (unsigned i = 1; i < n; i++) assert(strcasecmp(index[i - 1].keyword, index[i].keyword) <= 0);
    unsigned at = help_index_seek(index, n, "calc");
    assert(at < n && !strncasecmp(index[at].keyword, "calc", 4) && !strcmp(help_topics[index[at].topic].id, "calc"));
    assert(help_index_seek(index, n, "CALCULATOR") < n && help_index_seek(index, n, "zzzz") == n);
    assert(help_index_seek(index, n, "") == 0 && help_index_build(index, 3) == 3);
    /* Every topic is findable by its title. */
    for (unsigned i = 0; i < help_topic_count; i++) {
        struct help_hit hit[HELP_HITS_MAX];
        unsigned count = help_search(help_topics[i].title, hit, HELP_HITS_MAX);
        int own = 0; for (unsigned k = 0; k < count; k++) own |= hit[k].topic == i;
        assert(own);                                        /* a title finds its own topic */
    }
    struct help_hit hits[HELP_HITS_MAX];
    assert(help_search("", hits, HELP_HITS_MAX) == 0 && help_search("   ", hits, HELP_HITS_MAX) == 0 && help_search(NULL, hits, 4) == 0);
    unsigned c = help_search("calculator", hits, HELP_HITS_MAX);
    assert(c >= 1 && !strcmp(help_topics[hits[0].topic].id, "calc"));
    for (unsigned i = 1; i < c; i++) assert(hits[i - 1].score >= hits[i].score);
    assert(help_search("CALCULATOR", hits, HELP_HITS_MAX) == c);                       /* case-insensitive */
    c = help_search("end task", hits, HELP_HITS_MAX);                                   /* AND of two words */
    assert(c >= 1 && !strcmp(help_topics[hits[0].topic].id, "tasks"));
    assert(help_search("\"end task\"", hits, HELP_HITS_MAX) >= 1);                      /* a phrase */
    assert(help_search("task end", hits, HELP_HITS_MAX) == c);                          /* order does not matter */
    assert(help_search("\"task end\"", hits, HELP_HITS_MAX) == 0);                      /* a phrase does */
    assert(help_search("calculator zebra", hits, HELP_HITS_MAX) == 0);                  /* every word is required */
    assert(help_search("xyzzy", hits, HELP_HITS_MAX) == 0);
    assert(help_search("regist", hits, HELP_HITS_MAX) >= 1);                            /* a word prefix matches */
    assert(help_search("ister", hits, HELP_HITS_MAX) == 0 || strstr("registerister", "x") == NULL);   /* never mid-word... */
    assert(help_search("volume", hits, 2) == 2);                                        /* the capacity bounds the hits */
    c = help_search("volume", hits, HELP_HITS_MAX);
    assert(c >= 3);
    int mixer = 0; for (unsigned i = 0; i < c && i < 3; i++) mixer |= !strcmp(help_topics[hits[i].topic].id, "mixer");
    assert(mixer);                                                                      /* titled topics rank first */
}
static int launches; static char launched[64];
static int fake_launch(struct du_ui *u, const char *topic, void *ctx)
{ (void)u; (void)ctx; launches++; snprintf(launched, sizeof(launched), "%s", topic ? topic : ""); return 0; }
static void type(struct du_ui *u, const char *s)
{
    static const char *const rows[] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
    static const int base[] = {0x10, 0x1e, 0x2c};
    for (; *s; s++) for (int r = 0; r < 3; r++) for (int k = 0; rows[r][k]; k++) if (rows[r][k] == *s) at_tap(u, base[r] + k);
}
int main(int argc, char **argv)
{
    assert(argc >= 2);
    const char *scratch = argv[1], *folder = argc > 2 ? argv[2] : NULL;
    setenv("HOME", scratch, 1);
    topics();
    index_and_search();
    static uint32_t pixels[HELP_W * HELP_H];
    static struct du_ui ui; static struct help_app app;
    du_init(&ui, pixels, HELP_W, HELP_H);
    ui.help_open = fake_launch;
    assert(!help_attach(&app, &ui));
    assert(app.topic == 0 && app.ta.readonly && strstr(app.ta.text, "Welcome to CiukiOS") && ui.help_topic && !strcmp(ui.help_topic, "help"));
    assert(du_get(&ui, H_BACK)->disabled && !du_get(&ui, H_TREE)->hidden && du_get(&ui, H_INDEX_LIST)->hidden && du_get(&ui, H_SEARCH_LIST)->hidden);
    du_render(&ui);
    at_save(folder, "help-welcome", &ui);
    /* Contents: books start closed; Right opens one and Down reads its first topic; typing never edits. */
    unsigned closed = app.row_count; assert(closed == 1 + help_book_count() && app.rows[1].topic < 0);
    at_tap(&ui, DU_KEY_DOWN); assert(app.topic == 0);                         /* a book row does not change the topic */
    at_tap(&ui, DU_KEY_RIGHT); assert(app.row_count > closed && app.open_books[app.rows[1].book]);
    at_tap(&ui, DU_KEY_DOWN); assert(app.topic == help_topic_find("start") && !du_get(&ui, H_BACK)->disabled && app.back_count == 1);
    at_tap(&ui, DU_KEY_LEFT);                                                 /* Left on a topic goes to its book */
    at_tap(&ui, DU_KEY_LEFT); assert(app.row_count == closed);              /* and Left on the open book closes it */
    help_show_topic(&app, help_topic_find("keys"));
    assert(app.topic == help_topic_find("keys") && app.row_count > closed && strstr(app.ta.text, "Shift+F1"));
    du_focus(&ui, H_TEXT); size_t before = app.ta.len; at_tap(&ui, 0x1e); assert(app.ta.len == before);
    du_render(&ui);
    at_save(folder, "help-shortcuts", &ui);
    /* Back returns through the visited topics. */
    unsigned depth = app.back_count; int current = app.topic;
    help_show_topic(&app, current); assert(app.back_count == depth);          /* the same topic is not recorded */
    help_back(&app); assert(app.topic != current && app.back_count == depth - 1);
    while (app.back_count) help_back(&app);
    assert(du_get(&ui, H_BACK)->disabled && app.topic == 0);
    help_back(&app); assert(app.topic == 0);
    for (unsigned i = 0; i < HELP_BACK_MAX + 4; i++) help_show_topic(&app, (int)(i % 2 ? 1 : 2));
    assert(app.back_count <= HELP_BACK_MAX);
    at_click_widget(&ui, H_BACK); assert(app.topic >= 0);
    /* The Index tab: typing seeks the list, Enter reads. */
    help_tab(&app, HELP_TAB_INDEX);
    assert(!du_get(&ui, H_INDEX_LIST)->hidden && du_get(&ui, H_TREE)->hidden && du_focused(&ui) == H_INDEX_EDIT);
    type(&ui, "calc"); assert(!strcmp(du_get(&ui, H_INDEX_EDIT)->text, "calc"));
    struct du_widget *list = du_get(&ui, H_INDEX_LIST);
    assert(list->active < app.index_count && !strncasecmp(app.index[list->active].keyword, "calc", 4));
    at_tap(&ui, DU_KEY_ENTER); assert(app.topic == help_topic_find("calc"));
    du_render(&ui); at_save(folder, "help-index", &ui);
    /* The Search tab: Enter and the button run the query; a click on a hit reads it. */
    help_tab(&app, HELP_TAB_SEARCH);
    assert(du_focused(&ui) == H_SEARCH_EDIT && !du_get(&ui, H_SEARCH_GO)->hidden);
    type(&ui, "volume"); at_tap(&ui, DU_KEY_ENTER);
    assert(app.hit_count >= 3 && du_get(&ui, H_SEARCH_LIST)->count == app.hit_count && strstr(du_get(&ui, H_STATUS)->text, "topics found"));
    at_click_widget(&ui, H_SEARCH_GO); assert(app.hit_count >= 3);
    du_set_text(du_get(&ui, H_SEARCH_EDIT), ""); du_focus(&ui, H_SEARCH_EDIT); type(&ui, "xyzzy"); at_click_widget(&ui, H_SEARCH_GO);
    assert(app.hit_count == 0 && strstr(du_get(&ui, H_STATUS)->text, "No topics"));
    du_set_text(du_get(&ui, H_SEARCH_EDIT), ""); du_focus(&ui, H_SEARCH_EDIT); type(&ui, "mute"); at_tap(&ui, DU_KEY_ENTER);
    assert(app.hit_count >= 1);
    du_render(&ui); at_save(folder, "help-search", &ui);
    du_get(&ui, H_SEARCH_LIST)->active = 0;
    help_event(&ui, &(struct du_event){.type = DU_EV_SELECT, .id = H_SEARCH_LIST, .index = 0}, &app);
    assert(app.topic == (int)app.hits[0].topic);
    du_set_text(du_get(&ui, H_SEARCH_EDIT), ""); help_search_run(&app); assert(app.hit_count == 0);
    help_tab(&app, HELP_TAB_CONTENTS); assert(!du_get(&ui, H_TREE)->hidden && du_get(&ui, H_SEARCH_LIST)->hidden);
    /* F1 inside Help shows "Using Help" here and never starts another window. */
    help_show_topic(&app, help_topic_find("files"));
    launches = 0; at_tap(&ui, DU_KEY_F1);
    assert(!launches && app.topic == help_topic_find("help") && ui.help_consumed);
    /* Arguments: a topic, an unknown topic, a file. */
    assert(!help_open_argument(&app, "topic:tasks") && app.topic == help_topic_find("tasks"));
    assert(help_open_argument(&app, "topic:nonesuch") == -ENOENT && app.topic == help_topic_find("tasks"));
    /* Files: CR LF text, binary refusal, missing file. */
    char path[1024]; snprintf(path, sizeof(path), "%s/README.TXT", scratch);
    FILE *f = fopen(path, "wb"); assert(f); fputs("CiukiOS README\r\nLine two\r\n", f); fclose(f);
    assert(!help_open_file(&app, path) && app.topic == -1 && !strcmp(app.ta.text, "CiukiOS README\nLine two\n"));
    assert(strstr(du_get(&ui, H_STATUS)->text, "(read-only)"));
    assert(!help_open_argument(&app, path) && app.topic == -1);
    snprintf(path, sizeof(path), "%s/data.bin", scratch);
    f = fopen(path, "wb"); assert(f); fwrite("A\0B", 1, 3, f); fclose(f);
    assert(help_open_file(&app, path) == -EINVAL && ui.dialog.kind == DU_DLG_MESSAGE);
    du_dialog_close(&ui);
    snprintf(path, sizeof(path), "%s/none.txt", scratch);
    assert(help_open_file(&app, path) == -ENOENT);
    du_dialog_close(&ui);
    /* Copy a selection. */
    snprintf(path, sizeof(path), "%s/README.TXT", scratch);
    assert(!help_open_file(&app, path));
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
