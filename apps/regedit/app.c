/* SPDX-License-Identifier: MIT */
#include "regedit.h"
#include "../desktop/profile.h" /* sec-user-04: My Documents is per user */
#include "../libdeskui/job.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char *const root_names[] = {"System", "Desktop", "Programs", "Users", "Hardware"};
static const char *const type_names[] = {"", "String", "Integer", "Boolean", "Binary"};
static void error(struct re_app *a, const char *what, int e)
{
    char text[512]; snprintf(text, sizeof(text), "%s: %s", what, strerror(e));
    du_message(a->ui, RE_ERROR, "Registry Editor", text, DU_DLG_ICON_ERROR, NULL, 0, 0, 0);
}
static void free_values(struct re_app *a)
{
    for (unsigned i = 0; i < a->value_count; i++) free(a->values[i].text);
    if (a->ui && du_get(a->ui, RE_VALUES)) { du_get(a->ui, RE_VALUES)->items = NULL; du_get(a->ui, RE_VALUES)->count = 0; }
    free(a->values); free(a->value_rows); a->values = NULL; a->value_rows = NULL; a->value_count = 0;
}
static void free_nodes(struct re_app *a)
{
    for (unsigned i = 0; i < a->count; i++) free(a->nodes[i].path);
    free(a->nodes); free(a->tree_rows); free(a->visible); a->nodes = NULL; a->tree_rows = NULL; a->visible = NULL; a->count = 0;
}
static void clear_pending(struct re_app *a)
{
    free(a->pending_key); free(a->pending_text); a->pending_key = a->pending_text = NULL; a->operation = 0;
}
static int add_node(struct re_app *a, unsigned root, unsigned parent, unsigned depth, const char *path)
{
    if (a->count >= REG_KEYS_MAX * 5) { errno = E2BIG; return -1; }
    char *copy = strdup(path); if (!copy) return -1;
    struct re_node *nodes = realloc(a->nodes, (a->count + 1) * sizeof(*nodes)); if (!nodes) { free(copy); return -1; }
    a->nodes = nodes; unsigned index = a->count++;
    a->nodes[index] = (struct re_node){copy, root, depth, parent, depth == 0, 0};
    if (depth) a->nodes[parent].children = 1;
    return (int)index;
}
static int branch(struct re_app *a, unsigned root, unsigned parent, unsigned depth, const char *path)
{
    int index = add_node(a, root, parent, depth, path); if (index < 0) return -1;
    if (!a->roots[root]) return 0;
    for (unsigned i = 0;; i++) {
        char name[REG_NAME_BYTES + 1];
        if (reg_enum_keys(a->roots[root], path, i, name, sizeof(name))) return errno == ENOENT ? 0 : -1;
        size_t n = strlen(path) + strlen(name) + 2; char *child = malloc(n); if (!child) return -1;
        snprintf(child, n, "%s\\%s", path, name);
        int result = branch(a, root, (unsigned)index, depth + 1, child); free(child); if (result) return -1;
    }
}
static void menu_state(struct re_app *a)
{
    int ro = a->selected >= a->count || !a->roots[a->nodes[a->selected].root] || a->nodes[a->selected].root == 4;
    a->file_menu[0].flags = ro ? DU_ITEM_DISABLED : 0;
    for (unsigned i = 0; i < 7; i++) a->edit_menu[i].flags = ro ? DU_ITEM_DISABLED : 0;
    a->edit_menu[5].flags |= a->value_count ? 0 : DU_ITEM_DISABLED;
    if (a->selected < a->count && !a->nodes[a->selected].depth && du_focused(a->ui) != RE_VALUES)
        a->edit_menu[6].flags |= DU_ITEM_DISABLED;
    du_invalidate(a->ui,(struct du_rect){0,0,a->ui->canvas.width,20});
}
static int show_values(struct re_app *a)
{
    free_values(a); a->value_selected = 0;
    struct du_widget *list = du_get(a->ui, RE_VALUES); list->items = NULL; list->count = 0; list->active = 0; list->scroll = 0;
    if (a->selected >= a->count) return -1;
    struct re_node *node = &a->nodes[a->selected]; struct reg_root *r = a->roots[node->root];
    du_set_text(du_get(a->ui, RE_PATH), node->path);
    du_set_text(du_get(a->ui, RE_STATUS), node->root == 4 ? "Hardware is read-only" : "Ready");
    if (!r) { error(a, "Cannot open root", a->errors[node->root]); menu_state(a); return 0; }
    for (unsigned i = 0;; i++) {
        struct reg_value_info info;
        if (reg_enum_values(r, node->path, i, &info)) { if (errno == ENOENT) break; return -1; }
        struct re_value *v = realloc(a->values, (i + 1) * sizeof(*v)); if (!v) return -1; a->values = v;
        size_t size = 0; if (reg_get_text(r, node->path, info.name, NULL, &size)) return -1;
        char *text = malloc(size); if (!text) return -1;
        if (reg_get_text(r, node->path, info.name, text, &size)) { free(text); return -1; }
        a->values[i] = (struct re_value){info, text}; a->value_count++;
    }
    a->value_rows = calloc(a->value_count ? a->value_count : 1, sizeof(*a->value_rows)); if (!a->value_rows) return -1;
    for (unsigned i = 0; i < a->value_count; i++) {
        a->value_rows[i].text[0] = *a->values[i].info.name ? a->values[i].info.name : "(Default)";
        a->value_rows[i].text[1] = type_names[a->values[i].info.type]; a->value_rows[i].text[2] = a->values[i].text;
    }
    list->items = a->value_rows; list->count = a->value_count; menu_state(a); return 0;
}
static int show_tree(struct re_app *a)
{
    free(a->tree_rows); free(a->visible);
    a->tree_rows = calloc(a->count ? a->count : 1, sizeof(*a->tree_rows)); a->visible = calloc(a->count ? a->count : 1, sizeof(*a->visible));
    struct du_widget *w = du_get(a->ui, RE_TREE); w->items = NULL; w->count = 0;
    if (!a->tree_rows || !a->visible) return -1;
    a->visible_count = 0;
    for (unsigned i = 0; i < a->count; i++) {
        int visible = 1; unsigned p = i;
        while (a->nodes[p].depth) { p = a->nodes[p].parent; if (!a->nodes[p].expanded) { visible = 0; break; } }
        if (!visible) continue;
        unsigned j = a->visible_count++; a->visible[j] = i;
        const char *name = strrchr(a->nodes[i].path, '\\'); name = name ? name + 1 : a->nodes[i].path;
        a->tree_rows[j] = (struct du_item){.text = {name}, .depth = (uint8_t)a->nodes[i].depth,
            .expanded = (uint8_t)a->nodes[i].expanded, .children = (uint8_t)a->nodes[i].children, .selected = (uint8_t)(i == a->selected)};
        if (!a->defer_icons) a->tree_rows[j].icon16 = du_icon_argb(a->nodes[i].depth ? "folder" : "applications-system", 16);
        if (i == a->selected) w->active = j;
    }
    w->items = a->tree_rows; w->count = a->visible_count; du_list_ensure(w, w->active); du_invalidate_widget(a->ui,RE_TREE); return 0;
}
int re_select(struct re_app *a, const char *path)
{
    for (unsigned i = 0; i < a->count; i++) if (reg_name_equal(a->nodes[i].path, path)) {
        a->selected = i; unsigned p = i;
        while (a->nodes[p].depth) { p = a->nodes[p].parent; a->nodes[p].expanded = 1; }
        return show_tree(a) || show_values(a) ? -1 : 0;
    }
    errno = ENOENT; return -1;
}
int re_refresh(struct re_app *a)
{
    char *selected = a->selected < a->count ? strdup(a->nodes[a->selected].path) : NULL;
    struct re_node *old = a->nodes; unsigned old_count = a->count, old_selected = a->selected;
    a->nodes = NULL; a->count = 0; a->selected = 0;
    for (unsigned i = 0; i < 5; i++) {
        if (!a->roots[i]) a->roots[i] = reg_open_root_at(root_names[i], a->directory);
        a->errors[i] = a->roots[i] ? 0 : errno;
        a->generations[i] = a->roots[i] ? reg_generation(a->roots[i]) : REG_GENERATION_ERROR;
        if (branch(a, i, 0, 0, root_names[i])) {
            int e = errno;
            for (unsigned j = 0; j < a->count; j++) free(a->nodes[j].path);
            free(a->nodes); a->nodes = old; a->count = old_count; a->selected = old_selected;
            free(selected); errno = e; return -1;
        }
    }
    for (unsigned i = 0; i < a->count; i++) for (unsigned j = 0; j < old_count; j++)
        if (reg_name_equal(a->nodes[i].path, old[j].path)) { a->nodes[i].expanded = old[j].expanded; break; }
    for (unsigned i = 0; i < old_count; i++) free(old[i].path); free(old);
    int result = selected ? re_select(a, selected) : -1; free(selected);
    if (result) { a->selected = 0; result = show_tree(a) || show_values(a) ? -1 : 0; }
    return result;
}
/* Fold substring candidates independently: expansions such as ss/ß work too. */
static int contains(const char *text, const char *needle)
{
    if (!*needle) return 0;
    size_t bytes = strlen(text); char *part = malloc(bytes + 1); if (!part) return 0;
    for (size_t i = 0; i < bytes; i++) {
        if (((unsigned char)text[i] & 0xc0) == 0x80) continue;
        for (size_t j = i + 1; j <= bytes; j++) {
            if (j < bytes && ((unsigned char)text[j] & 0xc0) == 0x80) continue;
            memcpy(part, text + i, j - i); part[j - i] = 0;
            if (reg_name_equal(part, needle)) { free(part); return 1; }
            /* Avoid quadratic scans of large data; a folded scalar expands
             * at most three times, with four bytes per scalar. */
            if (j - i > strlen(needle) * 12 + 4) break;
        }
    }
    free(part); return 0;
}
int re_find(struct re_app *a, const char *text, int next)
{
    if (!text || !*text || strlen(text) >= sizeof(a->search)) { errno = EINVAL; return -1; }
    if (text != a->search) strcpy(a->search, text);
    unsigned start = next ? a->selected + 1 : 0;
    for (unsigned offset = 0; offset < a->count; offset++) {
        unsigned index = (start + offset) % a->count; struct re_node *node = &a->nodes[index];
        if (contains(node->path, text)) return re_select(a, node->path);
        if (!a->roots[node->root]) continue;
        for (unsigned i = 0;; i++) {
            struct reg_value_info v;
            if (reg_enum_values(a->roots[node->root], node->path, i, &v)) break;
            size_t size = 0; if (reg_get_text(a->roots[node->root], node->path, v.name, NULL, &size)) break;
            char *literal = malloc(size); if (!literal) return -1;
            int found = !reg_get_text(a->roots[node->root], node->path, v.name, literal, &size) &&
                (contains(v.name, text) || contains(literal, text)); free(literal);
            if (found) {
                int result = re_select(a, node->path); if (result) return result;
                struct du_widget *w = du_get(a->ui, RE_VALUES); w->active = i; a->value_selected = i;
                du_list_ensure(w, i); du_focus(a->ui, RE_VALUES); return 0;
            }
        }
    }
    errno = ENOENT; return -1;
}
static void confirm(struct re_app *a, const char *action)
{
    static const char *const buttons[] = {"Apply", "Cancel"}; char text[DU_DIALOG_TEXT];
    snprintf(text, sizeof(text), "%s\nKey: %.300s\nName: %.256s\nData/file: %.340s", action,
             a->operation == RE_NEW_KEY ? a->pending_text : a->pending_key,
             a->operation == RE_DELETE && !a->deleting_value ? "(entire branch)" : a->pending_name,
             a->pending_text && a->operation != RE_NEW_KEY ? a->pending_text : "");
    du_message(a->ui, RE_CONFIRM, "Confirm registry change", text, DU_DLG_ICON_WARNING, buttons, 2, 1, 1);
}
static int begin(struct re_app *a, int operation)
{
    clear_pending(a);
    if (a->selected >= a->count) return -1;
    struct re_node *n = &a->nodes[a->selected]; a->pending_root = n->root;
    if ((operation != RE_EXPORT) && (!a->roots[n->root] || n->root == 4)) { error(a, "Read-only root", EROFS); return -1; }
    a->pending_key = strdup(n->path); if (!a->pending_key) return -1;
    a->operation = operation; a->deleting_value = 0; return 0;
}
static void value_input(struct re_app *a, const char *initial)
{
    du_input_box(a->ui, RE_DATA, "Registry value", "Data: quoted string, integer, true/false, or hex: bytes", initial);
}
static void command(struct re_app *a, int id)
{
    if (id == RE_REFRESH) { if (re_refresh(a)) error(a, "Refresh failed", errno); return; }
    if (id == RE_FIND) { du_input_box(a->ui, RE_SEARCH, "Find", "Search keys, value names and data:", a->search); return; }
    if (id == RE_NEXT) { if (re_find(a, a->search, 1)) error(a, "Search", errno); return; }
    if (id == RE_ABOUT) {
        du_message(a->ui, RE_ABOUT, "About Registry Editor", "CiukiOS Registry Editor\nClassic computing, one woof at a time.\nReadable text registry. Code: MIT.", DU_DLG_ICON_INFO, NULL, 0, 0, 0); return;
    }
    if (begin(a, id)) return;
    if (id == RE_NEW_KEY) du_input_box(a->ui, RE_NAME, "New key", "Key name:", "");
    else if (id >= RE_NEW_STRING && id <= RE_NEW_BINARY) {
        a->value_type = id - RE_NEW_STRING + REG_STRING;
        du_input_box(a->ui, RE_NAME, "New value", "Value name (empty for default):", "");
    } else if (id == RE_EDIT) {
        if (a->value_selected >= a->value_count) { clear_pending(a); return; }
        struct re_value *v = &a->values[a->value_selected];
        if (strlen(v->text) >= DU_TEXT_MAX) { error(a, "Value exceeds the edit field; use export/import", E2BIG); clear_pending(a); return; }
        strcpy(a->pending_name, v->info.name); value_input(a, v->text);
    } else if (id == RE_DELETE) {
        a->deleting_value = du_focused(a->ui) == RE_VALUES;
        if (a->deleting_value) {
            if (a->value_selected >= a->value_count) { clear_pending(a); return; }
            strcpy(a->pending_name, a->values[a->value_selected].info.name);
        } else if (!a->nodes[a->selected].depth) { error(a, "Cannot delete a root", EACCES); clear_pending(a); return; }
        confirm(a, a->deleting_value ? "Delete this value?" : "Delete this key and all its subkeys and values?");
    } else if (id == RE_IMPORT || id == RE_EXPORT) {
        static const struct du_file_filter types[] = {{"Registration Files (*.reg)", "*.reg"}, {"All Files (*.*)", "*.*"}};
        struct du_file_options o = {.flags = id == RE_EXPORT ? DU_FILE_SAVE | DU_FILE_OVERWRITE_PROMPT : DU_FILE_FILE_MUST_EXIST,
            .title = id == RE_EXPORT ? "Export branch" : "Import registry file", .folder = profile_home(), .name = "registry.reg",
            .filters = types, .filter_count = 2, .default_ext = "reg"};
        if (du_file_open(a->ui, RE_FILE, &o)) error(a, "File dialog", errno);
    }
}
static void apply(struct re_app *a)
{
    struct reg_root *r = a->roots[a->pending_root]; int result;
    if (a->operation == RE_NEW_KEY) result = reg_create_key(r, a->pending_text);
    else if (a->operation == RE_DELETE) result = reg_delete(r, a->pending_key, a->deleting_value ? a->pending_name : NULL);
    else if (a->operation == RE_IMPORT) result = reg_import(r, a->pending_text);
    else result = reg_set_text(r, a->pending_key, a->pending_name, a->pending_text);
    if (result) { int e = errno; clear_pending(a); error(a, "Change failed", e); return; }
    clear_pending(a); if (re_refresh(a)) error(a, "Refresh failed", errno);
}
void re_event(struct du_ui *u, const struct du_event *e, void *user)
{
    struct re_app *a = user;
    if (e->type == DU_EV_DIALOG) {
        if (e->id == RE_CONFIRM) { if (!e->index && a->operation) apply(a); else clear_pending(a); }
        else if (e->id == RE_SEARCH && !e->index) { if (re_find(a, e->text, 0)) error(a, "Search", errno); }
        else if (e->id == RE_NAME || e->id == RE_DATA || e->id == RE_FILE) {
            if (e->index || !e->text) { clear_pending(a); return; }
            if (e->id == RE_NAME) {
                if (strlen(e->text) > REG_NAME_BYTES) { error(a, "Name too long", E2BIG); clear_pending(a); return; }
                if (a->operation == RE_NEW_KEY) {
                    if (!*e->text || strchr(e->text, '\\')) { error(a, "Enter one key name", EINVAL); clear_pending(a); return; }
                    size_t n = strlen(a->pending_key) + strlen(e->text) + 2; a->pending_text = malloc(n);
                    if (!a->pending_text) { error(a, "New key", ENOMEM); clear_pending(a); return; }
                    snprintf(a->pending_text, n, "%s\\%s", a->pending_key, e->text); confirm(a, "Create this key?");
                } else {
                    strcpy(a->pending_name, e->text);
                    static const char *const initial[] = {"", "\"\"", "0", "false", "hex:"}; value_input(a, initial[a->value_type]);
                }
            } else {
                free(a->pending_text); a->pending_text = strdup(e->text);
                if (!a->pending_text) { error(a, "Input", ENOMEM); clear_pending(a); return; }
                if (e->id == RE_FILE && a->operation == RE_EXPORT) {
                    int result = reg_export(a->roots[a->pending_root], a->pending_key, a->pending_text), err = errno;
                    clear_pending(a); if (result) error(a, "Export failed", err); else du_set_text(du_get(u, RE_STATUS), "Branch exported");
                } else confirm(a, a->operation == RE_IMPORT ? "Merge this file into the selected root?" : "Save this value?");
            }
        }
    } else if (e->type == DU_EV_SELECT && e->id == RE_TREE && e->index >= 0 && (unsigned)e->index < a->visible_count) {
        a->selected = a->visible[e->index]; show_tree(a); if (show_values(a)) error(a, "Read values", errno);
    } else if (e->type == DU_EV_CHANGE && e->id == RE_TREE && e->index >= 0 && (unsigned)e->index < a->visible_count) {
        unsigned i = a->visible[e->index]; a->nodes[i].expanded = !a->nodes[i].expanded; show_tree(a);
    } else if (e->type == DU_EV_SELECT && e->id == RE_VALUES && e->index >= 0 && (unsigned)e->index < a->value_count) {
        a->value_selected = (unsigned)e->index; menu_state(a);
    } else if (e->type == DU_EV_OPEN && e->id == RE_VALUES) command(a, RE_EDIT);
    else if (e->type == DU_EV_FOCUS) menu_state(a);
    else if (e->type == DU_EV_ACTIVATE) command(a, e->id);
    else if (e->type == DU_EV_KEY && e->key == DU_KEY_DELETE) command(a, RE_DELETE);
    else if (e->type == DU_EV_TICK) {
        if (du_dialog_id(u)) return;
        for (unsigned i = 0; i < 5; i++) if (a->roots[i]) {
            uint64_t g = reg_generation(a->roots[i]);
            if (g != a->generations[i]) { if (re_refresh(a)) error(a, "Registry changed", errno); du_invalidate_widget(u,RE_TREE); du_invalidate_widget(u,RE_VALUES); break; }
        }
        return;
    }
    du_invalidate_widget(u,RE_STATUS);
}
static void layout(struct du_ui *u, void *user)
{
    (void)user; int w = u->canvas.width, h = u->canvas.height, left = w / 3;
    du_get(u, RE_PATH)->box = (struct du_rect){6, 23, w - 12, 20};
    du_get(u, RE_TREE)->box = (struct du_rect){6, 47, left - 9, h - 74};
    du_get(u, RE_VALUES)->box = (struct du_rect){left + 2, 47, w - left - 8, h - 74};
    du_get(u, RE_STATUS)->box = (struct du_rect){0, h - 22, w, 22};
    du_get(u, RE_VALUES)->columns[2].width = w - left - 208;
}
static int attach(struct re_app *a, struct du_ui *u, const char *directory, int scan)
{
    a->ui = u; a->directory = directory; u->event = re_event; u->user = a; u->layout = layout;
    struct du_menu_item files[] = {{"&Import...", RE_IMPORT, 0, 0, 0}, {"&Export branch...", RE_EXPORT, 0, 0, 0}, {"&Refresh", RE_REFRESH, DU_KEY_F5, 0, 0}};
    struct du_menu_item edits[] = {{"New &key...", RE_NEW_KEY, 0, 0, 0}, {"New &string...", RE_NEW_STRING, 0, 0, 0},
        {"New &integer...", RE_NEW_INT, 0, 0, 0}, {"New &boolean...", RE_NEW_BOOL, 0, 0, 0}, {"New bi&nary...", RE_NEW_BINARY, 0, 0, 0},
        {"&Edit value...", RE_EDIT, 0, 0, 0}, {"&Delete...", RE_DELETE, 0, 0, 0}, {"", 0, 0, 0, DU_ITEM_SEPARATOR},
        {"&Find...", RE_FIND, 0x21, DU_CTRL, 0}, {"Find ne&xt", RE_NEXT, 0x3d, 0, 0}};
    memcpy(a->file_menu, files, sizeof(files)); memcpy(a->edit_menu, edits, sizeof(edits));
    a->help_menu[0] = (struct du_menu_item){"&About Registry Editor", RE_ABOUT, 0, 0, 0};
    a->menus[0] = (struct du_menu){"&File", a->file_menu, 3}; a->menus[1] = (struct du_menu){"&Edit", a->edit_menu, 10};
    a->menus[2] = (struct du_menu){"&Help", a->help_menu, 1}; u->menus = a->menus; u->menu_count = 3;
    du_add(u, DU_LABEL, RE_PATH, (struct du_rect){0}, ""); du_add(u, DU_TREE, RE_TREE, (struct du_rect){0}, "");
    struct du_widget *v = du_add(u, DU_LISTVIEW, RE_VALUES, (struct du_rect){0}, ""); v->view = DU_DETAILS; v->column_count = 3;
    v->columns[0] = (struct du_column){"Name", 120, 0}; v->columns[1] = (struct du_column){"Type", 80, 0}; v->columns[2].title = "Data";
    du_add(u, DU_STATUS, RE_STATUS, (struct du_rect){0}, ""); layout(u, a);
    if (!scan) { du_set_text(du_get(u,RE_STATUS),"Loading registry..."); return 0; }
    int result = re_refresh(a); du_timer_start(u, RE_POLL, 2000000000ull, 1); return result;
}
struct re_startup { struct du_job job; struct re_app app; struct du_ui ui; int result; };
int re_attach(struct re_app *a, struct du_ui *u, const char *directory)
{ return attach(a,u,directory,1); }
int re_attach_lazy(struct re_app *a, struct du_ui *u, const char *directory)
{
    int r=attach(a,u,directory,0); if (r) return r;
    a->startup=calloc(1,sizeof(*a->startup));
    if (!a->startup) return -ENOMEM;
    a->startup->app.directory=directory;
    a->startup->app.defer_icons=1; /* The process icon cache belongs to the UI thread. */
    du_init(&a->startup->ui,NULL,u->canvas.width,u->canvas.height);
    u->event=NULL; return 0;
}
static void startup_read(void *arg)
{
    struct re_startup *s=arg;
    s->result=re_attach(&s->app,&s->ui,s->app.directory);
}
void re_startup_poll(struct re_app *a)
{
    struct re_startup *s=a->startup; if (!s) return;
    struct du_ui *u=a->ui;
    if (!s->job.started) {
        if (u->dirty || !u->frame_count) return;
        int r=du_job_start_ui(&s->job,startup_read,s,u);
        if (r) { free(s); a->startup=NULL; du_message(u,RE_ERROR,"Registry Editor","The registry worker could not start.",DU_DLG_ICON_ERROR,NULL,0,0,0); }
        return;
    }
    if (!du_job_ready(&s->job)) return;
    du_job_finish(&s->job);
    *a=s->app; a->ui=u; a->startup=NULL; a->defer_icons=0;
    for (unsigned j=0;j<a->visible_count;j++)
        a->tree_rows[j].icon16=du_icon_argb(a->nodes[a->visible[j]].depth ? "folder" : "applications-system",16);
    memcpy(u->widgets,s->ui.widgets,sizeof(u->widgets)); u->count=s->ui.count;
    a->menus[0].items=a->file_menu; a->menus[1].items=a->edit_menu; a->menus[2].items=a->help_menu;
    u->menus=a->menus; u->event=re_event; u->user=a; u->layout=layout;
    layout(u,a); u->dirty=1;
    du_timer_start(u,RE_POLL,2000000000ull,1);
    if (s->result) error(a,"Read registry",EIO);
    free(s);
}
void re_destroy(struct re_app *a)
{
    if (a->startup) {
        struct re_startup *s=a->startup; du_job_finish(&s->job);
        if (s->app.ui) re_destroy(&s->app);
        free(s); a->startup=NULL;
    }
    clear_pending(a); free_values(a); free_nodes(a);
    for (unsigned i = 0; i < 5; i++) { reg_close_root(a->roots[i]); a->roots[i] = NULL; }
    du_get(a->ui, RE_TREE)->items = NULL; du_get(a->ui, RE_VALUES)->items = NULL;
    du_get(a->ui, RE_TREE)->count = du_get(a->ui, RE_VALUES)->count = 0;
    du_timer_stop(a->ui, RE_POLL);
}
