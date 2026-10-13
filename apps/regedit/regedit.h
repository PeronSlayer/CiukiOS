/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_REGEDIT_H
#define CIUKI_REGEDIT_H
#include "../libdeskui/deskui.h"
#include <ciuki/registry.h>
#define RE_WIDTH 620
#define RE_HEIGHT 380
enum { RE_TREE = 1, RE_VALUES, RE_PATH, RE_STATUS, RE_POLL,
       RE_IMPORT = 20, RE_EXPORT, RE_REFRESH, RE_NEW_KEY, RE_NEW_STRING,
       RE_NEW_INT, RE_NEW_BOOL, RE_NEW_BINARY, RE_EDIT, RE_DELETE, RE_FIND, RE_NEXT,
       RE_ABOUT, RE_NAME = 50, RE_DATA, RE_CONFIRM, RE_FILE, RE_SEARCH, RE_ERROR };
struct re_node { char *path; unsigned root, depth, parent; int expanded, children; };
struct re_value { struct reg_value_info info; char *text; };
struct re_app {
    struct du_ui *ui;
    struct reg_root *roots[5]; int errors[5]; uint64_t generations[5];
    struct re_node *nodes; unsigned count, selected;
    struct du_item *tree_rows; unsigned *visible; unsigned visible_count;
    struct re_value *values; struct du_item *value_rows; unsigned value_count, value_selected;
    struct du_menu menus[3]; struct du_menu_item file_menu[3], edit_menu[10], help_menu[1];
    const char *directory;
    int operation, value_type, deleting_value;
    unsigned pending_root;
    char *pending_key, *pending_text;
    char pending_name[REG_NAME_BYTES + 1], search[DU_TEXT_MAX];
};
int re_attach(struct re_app *, struct du_ui *, const char *directory);
void re_destroy(struct re_app *);
int re_refresh(struct re_app *);
int re_select(struct re_app *, const char *path);
int re_find(struct re_app *, const char *text, int next);
void re_event(struct du_ui *, const struct du_event *, void *);
#endif
