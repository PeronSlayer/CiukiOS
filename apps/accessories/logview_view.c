/* SPDX-License-Identifier: MIT
 * CiukiOS Log Viewer window: log list, read-only text pane, status bar. */
#include "logview.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIST_W 216
static void status(struct lv_app *a, const char *text) { du_set_text(du_get(a->ui, LV_STATUS), text); du_invalidate_widget(a->ui, LV_STATUS); }

void lv_layout(struct lv_app *a)
{
    struct du_canvas *c = &a->ui->canvas;
    int top = 22, bottom = c->height - 20;
    du_get(a->ui, LV_LIST)->box = (struct du_rect){2, top, LIST_W, bottom - top - 2};
    struct du_widget *t = du_get(a->ui, LV_TEXT);
    t->box = (struct du_rect){LIST_W + 6, top, c->width - LIST_W - 8, bottom - top - 2};
    du_ta_fit(&a->ta, t->box);
    du_get(a->ui, LV_STATUS)->box = (struct du_rect){0, bottom, c->width, 20};
}
static void set_text(struct lv_app *a, const char *text, size_t n)
{
    a->ta.readonly = 0;
    if (du_ta_set_text(&a->ta, text, n)) du_ta_set_text(&a->ta, "", 0);
    a->ta.readonly = 1; a->ta.cursor = a->ta.anchor = 0; a->ta.top = a->ta.left = 0;
    du_invalidate_widget(a->ui, LV_TEXT);
}
/* A log longer than the pane's capacity shows its end, where the newest lines are. */
void lv_show(struct lv_app *a, int index)
{
    if (index < 0 || (unsigned)index >= a->list.count) return;
    const struct lv_entry *e = &a->list.entry[index];
    for (unsigned i = 0; i < a->list.count; i++) a->items[i].selected = (int)i == index;
    struct du_widget *list = du_get(a->ui, LV_LIST);
    list->active = (unsigned)index; du_list_ensure(list, (unsigned)index);
    a->selected = index;
    char *bytes = malloc(LV_TEXT_MAX);
    size_t length = 0; unsigned long total = 0;
    int error = bytes ? lv_read_tail(e->path, bytes, LV_TEXT_MAX, &length, &total) : -ENOMEM;
    if (error) {
        char note[512];
        if (error == -ENOENT)
            snprintf(note, sizeof(note), "There is no %s yet.\n\n%s is created by %s.", e->title, e->path,
                     e->kind == LV_BOOT ? "the system at startup" : e->kind == LV_NETSURF ? "NetSurf when a page fails to load" :
                     e->kind == LV_SETUP ? "Setup" : "the system when a program faults");
        else snprintf(note, sizeof(note), "%s cannot be read: %s", e->path, strerror(-error));
        set_text(a, note, strlen(note));
        snprintf(a->status, sizeof(a->status), "%s (not available)", e->title);
    } else {
        set_text(a, bytes, length);
        if (total > length) snprintf(a->status, sizeof(a->status), "%s (read-only): last %u of %lu bytes", e->title, (unsigned)length, total);
        else snprintf(a->status, sizeof(a->status), "%s (read-only): %lu bytes", e->title, total);
    }
    free(bytes);
    status(a, a->status);
    du_invalidate_widget(a->ui, LV_LIST);
}
void lv_refresh(struct lv_app *a)
{
    int keep = a->selected;
    char path[sizeof(a->list.entry[0].path)];
    path[0] = 0;
    if (keep >= 0 && (unsigned)keep < a->list.count) snprintf(path, sizeof(path), "%s", a->list.entry[keep].path);
    lv_scan(&a->list, lv_root);
    int at = 0;
    for (unsigned i = 0; path[0] && i < a->list.count; i++) if (!strcmp(a->list.entry[i].path, path)) at = (int)i;
    struct du_widget *list = du_get(a->ui, LV_LIST);
    for (unsigned i = 0; i < a->list.count; i++) a->items[i] = (struct du_item){ .text = { a->list.entry[i].title } };
    list->items = a->items; list->count = a->list.count;
    lv_show(a, at);
}

static const struct du_menu_item file_template[] = {
    {"&Refresh", LV_M_REFRESH, DU_KEY_F5, 0, 0}, {"", 0, 0, 0, DU_ITEM_SEPARATOR}, {"E&xit", LV_M_EXIT, 0, 0, 0} };
static const struct du_menu_item edit_template[] = {
    {"&Copy", LV_M_COPY, 0x2e, DU_CTRL, 0}, {"Select &All", LV_M_SELALL, 0x1e, DU_CTRL, 0} };
static const struct du_menu_item help_template[] = { {"&About Log Viewer", LV_M_ABOUT, 0, 0, 0} };

static void resize_layout(struct du_ui *u, void *user) { (void)u; lv_layout(user); }

int lv_attach(struct lv_app *a, struct du_ui *u)
{
    a->ui = u; u->event = lv_event; u->user = a; u->layout = resize_layout; a->selected = -1;
    if (du_ta_init(&a->ta, LV_TEXT_MAX)) return -ENOMEM;
    a->ta.wrap = 0; a->ta.readonly = 1;
    memcpy(a->file_items, file_template, sizeof(file_template));
    memcpy(a->edit_items, edit_template, sizeof(edit_template));
    memcpy(a->help_items, help_template, sizeof(help_template));
    a->menus[0] = (struct du_menu){"&File", a->file_items, 3};
    a->menus[1] = (struct du_menu){"&Edit", a->edit_items, 2};
    a->menus[2] = (struct du_menu){"&Help", a->help_items, 1};
    u->menus = a->menus; u->menu_count = 3;
    struct du_rect zero = {0, 0, 0, 0};
    struct du_widget *list = du_add(u, DU_LISTVIEW, LV_LIST, zero, "");
    list->view = DU_DETAILS; list->inactive_selection = 1;
    list->columns[0] = (struct du_column){"Logs", LIST_W - 20, 0}; list->column_count = 1;
    struct du_widget *text = du_add(u, DU_TEXTAREA, LV_TEXT, zero, "");
    text->data = &a->ta;
    du_add(u, DU_STATUS, LV_STATUS, zero, "");
    lv_layout(a);
    lv_refresh(a);
    du_focus(u, LV_LIST);
    return 0;
}
void lv_detach(struct lv_app *a) { du_ta_free(&a->ta); }

void lv_event(struct du_ui *u, const struct du_event *e, void *user)
{
    struct lv_app *a = user;
    switch (e->type) {
    case DU_EV_SELECT: case DU_EV_OPEN:
        if (e->id == LV_LIST && e->index >= 0) lv_show(a, e->index);
        break;
    case DU_EV_ACTIVATE:
        if (e->id == LV_M_REFRESH) lv_refresh(a);
        else if (e->id == LV_M_EXIT) { if (a->quit) a->quit(a->host); }
        else if (e->id == LV_M_COPY) {
            size_t lo, hi; du_ta_selection(&a->ta, &lo, &hi);
            if (hi > lo) {
                char *copy = malloc(hi - lo + 1);
                if (copy) { memcpy(copy, a->ta.text + lo, hi - lo); copy[hi - lo] = 0; du_clipboard_set(copy); free(copy); }
            }
        } else if (e->id == LV_M_SELALL) { du_ta_select_all(&a->ta); du_focus(u, LV_TEXT); }
        else if (e->id == LV_M_ABOUT)
            du_message(u, LV_D_INFO, "About Log Viewer", "CiukiOS Log Viewer - Classic computing, one woof at a time.\nBoot, NetSurf, Setup and crash logs, read-only.\nCode: MIT.",
                       DU_DLG_ICON_INFO, NULL, 0, 0, 0);
        break;
    default: return;
    }
    du_invalidate_widget(u, LV_TEXT);
}
