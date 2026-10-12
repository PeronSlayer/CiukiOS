/* SPDX-License-Identifier: MIT
 * CiukiOS Help window: topic list, read-only text pane, status bar. */
#include "help.h"
#include "../libdeskui/recent.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIST_W 172
static void status(struct help_app *a, const char *text) { du_set_text(du_get(a->ui, H_STATUS), text); a->ui->dirty = 1; }

void help_layout(struct help_app *a)
{
    struct du_canvas *c = &a->ui->canvas;
    int top = 22, bottom = c->height - 20;
    du_get(a->ui, H_TOPICS)->box = (struct du_rect){2, top, LIST_W, bottom - top - 2};
    struct du_widget *t = du_get(a->ui, H_TEXT);
    t->box = (struct du_rect){LIST_W + 6, top, c->width - LIST_W - 8, bottom - top - 2};
    du_ta_fit(&a->ta, t->box);
    du_get(a->ui, H_STATUS)->box = (struct du_rect){0, bottom, c->width, 20};
}
int help_load_text(struct help_app *a, const char *data, size_t n, const char *title)
{
    if (n > HELP_FILE_MAX) return -EFBIG;
    for (size_t i = 0; i < n; i++) if (!data[i]) return -EINVAL;    /* binary files are not text */
    /* CR LF and lone CR become LF for the text area. */
    char *copy = malloc(n + 1);
    if (!copy) return -ENOMEM;
    size_t m = 0;
    for (size_t i = 0; i < n; i++) {
        if (data[i] == '\r') { copy[m++] = '\n'; if (i + 1 < n && data[i + 1] == '\n') i++; }
        else copy[m++] = data[i];
    }
    a->ta.readonly = 0;
    int err = du_ta_set_text(&a->ta, copy, m);
    a->ta.readonly = 1; a->ta.cursor = a->ta.anchor = 0; a->ta.top = a->ta.left = 0;
    free(copy);
    if (err) return -EFBIG;
    if (title) status(a, title);
    a->ui->dirty = 1;
    return 0;
}
void help_show_topic(struct help_app *a, int topic)
{
    if (topic < 0 || (unsigned)topic >= help_topic_count) return;
    for (unsigned i = 0; i < help_topic_count; i++) a->items[i].selected = (int)i == topic;
    struct du_widget *list = du_get(a->ui, H_TOPICS);
    list->active = (unsigned)topic; du_list_ensure(list, (unsigned)topic);
    a->topic = topic;
    help_load_text(a, help_topics[topic].text, strlen(help_topics[topic].text), help_topics[topic].title);
}
int help_open_file(struct help_app *a, const char *path)
{
    FILE *f = fopen(path, "rb");
    int err = f ? 0 : -errno;
    char *data = err ? NULL : malloc(HELP_FILE_MAX + 2);
    size_t n = 0;
    if (!err && !data) err = -ENOMEM;
    if (!err) { n = fread(data, 1, HELP_FILE_MAX + 1, f); if (ferror(f)) err = -EIO; }
    if (f) fclose(f);
    if (!err) {
        char title[200]; snprintf(title, sizeof(title), "%.180s (read-only)", path);
        err = help_load_text(a, data, n, title);
    }
    free(data);
    if (err) {
        char text[300];
        snprintf(text, sizeof(text), "The file cannot be shown:\n%.200s\n%s", path,
                 err == -EFBIG ? "It is larger than 60 KB." : err == -EINVAL ? "It is not a text file." :
                 err == -ENOENT ? "The file was not found." : "It could not be read.");
        du_message(a->ui, H_D_INFO, "CiukiOS Help", text, DU_DLG_ICON_WARNING, NULL, 0, 0, 0);
        return err;
    }
    snprintf(a->path, sizeof(a->path), "%s", path);
    for (unsigned i = 0; i < help_topic_count; i++) a->items[i].selected = 0;
    a->topic = -1;
    du_recent_add(path);
    du_focus(a->ui, H_TEXT);
    return 0;
}

static const struct du_menu_item file_template[] = {
    {"&Open...", H_M_OPEN, 0x18, DU_CTRL, 0}, {"", 0, 0, 0, DU_ITEM_SEPARATOR}, {"E&xit", H_M_EXIT, 0, 0, 0} };
static const struct du_menu_item edit_template[] = {
    {"&Copy", H_M_COPY, 0x2e, DU_CTRL, 0}, {"Select &All", H_M_SELALL, 0x1e, DU_CTRL, 0} };
static const struct du_menu_item help_template[] = { {"&About CiukiOS Help", H_M_ABOUT, 0, 0, 0} };

static void resize_layout(struct du_ui *u, void *user)
{ (void)u; help_layout(user); }

int help_attach(struct help_app *a, struct du_ui *u)
{
    a->ui = u; u->event = help_event; u->user = a; u->layout = resize_layout;
    if (du_ta_init(&a->ta, HELP_FILE_MAX)) return -ENOMEM;
    a->ta.wrap = 1; a->ta.readonly = 1;
    memcpy(a->file_items, file_template, sizeof(file_template));
    memcpy(a->edit_items, edit_template, sizeof(edit_template));
    memcpy(a->help_items, help_template, sizeof(help_template));
    a->menus[0] = (struct du_menu){"&File", a->file_items, 3};
    a->menus[1] = (struct du_menu){"&Edit", a->edit_items, 2};
    a->menus[2] = (struct du_menu){"&Help", a->help_items, 1};
    u->menus = a->menus; u->menu_count = 3;
    struct du_rect zero = {0, 0, 0, 0};
    struct du_widget *list = du_add(u, DU_LISTVIEW, H_TOPICS, zero, "");
    for (unsigned i = 0; i < help_topic_count; i++) a->items[i] = (struct du_item){ .text = { help_topics[i].title } };
    list->items = a->items; list->count = help_topic_count; list->view = DU_DETAILS; list->inactive_selection = 1;
    list->columns[0] = (struct du_column){"Contents", LIST_W - 20, 0}; list->column_count = 1;
    struct du_widget *text = du_add(u, DU_TEXTAREA, H_TEXT, zero, "");
    text->data = &a->ta;
    du_add(u, DU_STATUS, H_STATUS, zero, "");
    help_layout(a);
    help_show_topic(a, 0);
    du_focus(u, H_TOPICS);
    return 0;
}
void help_detach(struct help_app *a) { du_ta_free(&a->ta); }

void help_event(struct du_ui *u, const struct du_event *e, void *user)
{
    struct help_app *a = user;
    switch (e->type) {
    case DU_EV_SELECT: case DU_EV_OPEN:
        if (e->id == H_TOPICS && e->index >= 0) help_show_topic(a, e->index);
        break;
    case DU_EV_DIALOG:
        if (e->id == H_D_OPEN && !e->index && e->text && e->text[0]) help_open_file(a, e->text);
        break;
    case DU_EV_ACTIVATE:
        if (e->id == H_M_OPEN) du_file_dialog(u, H_D_OPEN, 0, "Open", "/", "", du_posix_lister, NULL);
        else if (e->id == H_M_EXIT) { if (a->quit) a->quit(a->host); }
        else if (e->id == H_M_COPY) {
            size_t lo, hi; du_ta_selection(&a->ta, &lo, &hi);
            if (hi > lo) {
                char *copy = malloc(hi - lo + 1);
                if (copy) { memcpy(copy, a->ta.text + lo, hi - lo); copy[hi - lo] = 0; du_clipboard_set(copy); free(copy); }
            }
        } else if (e->id == H_M_SELALL) { du_ta_select_all(&a->ta); du_focus(u, H_TEXT); }
        else if (e->id == H_M_ABOUT)
            du_message(u, H_D_INFO, "About CiukiOS Help", "CiukiOS Help - Classic computing, one woof at a time.\nTopics and a read-only text viewer.\nCode: MIT.",
                       DU_DLG_ICON_INFO, NULL, 0, 0, 0);
        break;
    default: return;
    }
    u->dirty = 1;
}
