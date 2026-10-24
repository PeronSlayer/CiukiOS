/* System Information window. Windows 98 MSInfo layout (Resource Kit ch. 27): categories on the left,
 * the items of the selected category on the right, a Tools menu and a status bar.
 * SPDX-License-Identifier: MIT */
#include "sysinfo_view.h"
#include "sysui.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static const struct du_column list_columns[2] = { { "Item", 230, 0 }, { "Value", 300, 0 } };
static const struct { int id; const char *program, *label; } tools[5] = {
    { SIM_HWREPORT, "hwreport", "Hardware Report" }, { SIM_SYSMON, "sysmon", "System Monitor" },
    { SIM_RESMETER, "resmeter", "Resource Meter" }, { SIM_TASKS, "tasks", "Task Manager" },
    { SIM_CONTROL, "control", "Control Panel" } };

/* Hash of the labels from the root to the node: a category keeps its expanded state across refreshes. */
static uint32_t path_hash(const struct si_tree *t, int node)
{
    int chain[16]; unsigned n = 0;
    for (int i = node; i >= 0 && n < 16; i = t->n[i].parent) chain[n++] = i;
    uint32_t h = 2166136261u;
    while (n--) {
        for (const char *s = t->n[chain[n]].label; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
        h = (h ^ '/') * 16777619u;
    }
    return h;
}
static int is_open(const struct sysinfo_app *a, int node)
{
    uint32_t h = path_hash(a->tree, node);
    for (unsigned i = 0; i < a->open_count; i++) if (a->open_hash[i] == h) return 1;
    return 0;
}
static void set_open(struct sysinfo_app *a, int node, int open)
{
    uint32_t h = path_hash(a->tree, node);
    for (unsigned i = 0; i < a->open_count; i++)
        if (a->open_hash[i] == h) {
            if (!open) a->open_hash[i] = a->open_hash[--a->open_count];
            return;
        }
    if (open && a->open_count < SI_NODES) a->open_hash[a->open_count++] = h;
}
static int is_ancestor(const struct si_tree *t, int ancestor, int node)
{
    for (int i = t->n[node].parent; i >= 0; i = t->n[i].parent) if (i == ancestor) return 1;
    return 0;
}
static int is_conflict_node(const struct si_tree *t, int i)
{ return !strncmp(t->n[i].value, "Conflict:", 9); }
static int in_category(const struct si_tree *t, int i, const char *label)
{
    int c = si_find(t, label);
    return c >= 0 && t->n[i].parent == c;
}
static const uint32_t *row_icon(const struct sysinfo_app *a, int i)
{
    const struct si_tree *t = a->tree; const struct si_node *n = &t->n[i];
    if (is_conflict_node(t, i)) return a->warning_icon;
    if (in_category(t, i, "Problem Devices") || !strncmp(n->value, "Stopped (problem)", 17)) return a->error_icon;
    if (!strncmp(n->value, "IRQ shared:", 11)) return a->info_icon;
    if (i == 0 && n->kind == SI_CATEGORY && n->parent < 0 && a->root_icon) return a->root_icon; /* System Information: the computer icon */
    return n->kind == SI_CATEGORY ? a->folder_icon : NULL;
}

static void status(struct sysinfo_app *a)
{
    char text[256];
    snprintf(text, sizeof(text), "%u device(s)\t%u conflict(s)\t%s",
             a->devices ? a->devices->count : 0, a->tree->conflicts, a->message);
    sysui_text(a->ui, SIV_STATUS, text);
}
static void set_message(struct sysinfo_app *a, const char *text)
{ snprintf(a->message, sizeof(a->message), "%s", text); status(a); }

/* Tree rows: categories whose ancestors are all expanded. */
static void rebuild_tree(struct sysinfo_app *a)
{
    const struct si_tree *t = a->tree; uint8_t shown[SI_NODES], has_child[SI_NODES];
    memset(has_child, 0, sizeof(has_child));
    for (unsigned i = 0; i < t->count; i++)
        if (t->n[i].kind == SI_CATEGORY && t->n[i].parent >= 0) has_child[t->n[i].parent] = 1;
    a->visible_count = 0;
    struct du_widget *w = du_get(a->ui, SIV_TREE);
    for (unsigned i = 0; i < t->count; i++) {
        const struct si_node *n = &t->n[i];
        int p = n->parent;
        shown[i] = n->kind == SI_CATEGORY && (p < 0 || (shown[p] && is_open(a, p)));
        if (!shown[i]) continue;
        struct du_item *it = &a->tree_items[a->visible_count];
        memset(it, 0, sizeof(*it));
        it->text[0] = n->label; it->depth = (uint8_t)n->depth; it->children = has_child[i];
        it->expanded = has_child[i] && is_open(a, (int)i);
        it->icon16 = row_icon(a, (int)i);
        if ((int)i == a->selected) w->active = a->visible_count;
        a->visible[a->visible_count++] = (int)i;
    }
    w->items = a->tree_items; w->count = a->visible_count; w->selection_active = 1;
    du_invalidate_widget(a->ui, SIV_TREE);
}

/* List rows: the children of the selected node. A sub-category shows its own summary or item count. */
static void rebuild_list(struct sysinfo_app *a)
{
    const struct si_tree *t = a->tree;
    struct du_widget *w = du_get(a->ui, SIV_LIST);
    a->list_count = 0;
    for (unsigned i = 0; i < t->count && a->list_count < SI_NODES; i++) {
        const struct si_node *n = &t->n[i];
        if (n->parent != a->selected) continue;
        unsigned row = a->list_count++;
        struct du_item *it = &a->list_items[row];
        memset(it, 0, sizeof(*it));
        it->text[0] = n->label;
        if (n->value[0]) it->text[1] = n->value;
        else {
            snprintf(a->summary[row], sizeof(a->summary[row]), "%u item(s)", si_children(t, (int)i));
            it->text[1] = a->summary[row];
        }
        it->icon16 = row_icon(a, (int)i);
        a->list_node[row] = (int)i;
    }
    w->items = a->list_items; w->count = a->list_count; w->active = 0; w->scroll = 0; w->selection_active = 0;
    du_invalidate_widget(a->ui, SIV_LIST);
}
static void rebuild(struct sysinfo_app *a)
{ rebuild_tree(a); rebuild_list(a); status(a); }

int siv_node_at(const struct sysinfo_app *a, unsigned row)
{ return row < a->visible_count ? a->visible[row] : -1; }

void siv_refresh(struct sysinfo_app *a)
{
    uint32_t keep = a->tree->count ? path_hash(a->tree, a->selected) : 0;
    memset(a->devices, 0, sizeof(*a->devices));
    memset(&a->input, 0, sizeof(a->input));
    a->input.devices = a->devices;
    int e = a->host.collect(a->host.ctx, a->devices, &a->input);
    a->input.devices = a->devices;
    if (e) {
        memset(a->devices, 0, sizeof(*a->devices));
        char text[96]; snprintf(text, sizeof(text), "Hardware inventory unavailable (error %d).", -e);
        snprintf(a->message, sizeof(a->message), "%s", text);
    } else a->message[0] = 0;
    si_build(a->tree, &a->input);
    a->selected = 0;
    if (keep) for (unsigned i = 0; i < a->tree->count; i++)
        if (a->tree->n[i].kind == SI_CATEGORY && path_hash(a->tree, (int)i) == keep) { a->selected = (int)i; break; }
    if (a->tree->overflow && !e) snprintf(a->message, sizeof(a->message), "Some entries are not shown (list is full).");
    rebuild(a);
}

static void launch(struct sysinfo_app *a, int id)
{
    for (unsigned i = 0; i < sizeof(tools) / sizeof(tools[0]); i++) {
        if (tools[i].id != id) continue;
        int r = a->host.launch(a->host.ctx, tools[i].program);
        if (r < 0) {
            char text[160];
            snprintf(text, sizeof(text), "%s could not be started (error %d).", tools[i].label, -r);
            sysui_box(a->ui, SID_MESSAGE, "System Information", text, DU_DLG_ICON_ERROR);
        } else { a->launch_serial = (uint32_t)r; snprintf(a->launch_label, sizeof(a->launch_label), "%s", tools[i].label); }
        return;
    }
}
void siv_launch_result(struct sysinfo_app *a, uint32_t serial, int result)
{
    if (!serial || serial != a->launch_serial) return;
    a->launch_serial = 0;
    if (result < 0) {
        char text[160];
        snprintf(text, sizeof(text), "%s could not be started (error %d).", a->launch_label, -result);
        sysui_box(a->ui, SID_MESSAGE, "System Information", text, DU_DLG_ICON_ERROR);
    }
}

static void save_text(struct sysinfo_app *a, const char *path)
{
    size_t cap = (size_t)a->tree->count * 200 + 64;
    char *text = malloc(cap);
    int e = 0;
    if (!text) e = ENOMEM;
    else {
        size_t n = si_export(a->tree, text, cap);
        e = n ? -a->host.write_file(a->host.ctx, path, text, n) : ENOSPC;
        free(text);
    }
    if (e) {
        char box[200]; snprintf(box, sizeof(box), "The information could not be saved to\n%s\n(error %d).", path, e);
        sysui_box(a->ui, SID_MESSAGE, "Save As Text File", box, DU_DLG_ICON_ERROR);
    } else {
        char msg[96]; snprintf(msg, sizeof(msg), "Saved %s", path);
        set_message(a, msg);
    }
}

/* Double click or Enter on a sub-category: go into it in the tree. */
static void open_row(struct sysinfo_app *a, int row)
{
    if (row < 0 || (unsigned)row >= a->list_count) return;
    int node = a->list_node[row];
    if (a->tree->n[node].kind != SI_CATEGORY) return;
    for (int p = a->tree->n[node].parent; p >= 0; p = a->tree->n[p].parent) set_open(a, p, 1);
    a->selected = node;
    rebuild(a);
}

void siv_event(struct du_ui *u, const struct du_event *e, void *user)
{
    struct sysinfo_app *a = user;
    switch (e->type) {
    case DU_EV_SELECT:
        if (e->id == SIV_TREE && e->index >= 0 && (unsigned)e->index < a->visible_count) {
            a->selected = a->visible[e->index]; rebuild_list(a); status(a);
        }
        return;
    case DU_EV_CHANGE:    /* expand or collapse (toggle box, Left/Right, +/-) */
        if (e->id == SIV_TREE && e->index >= 0 && (unsigned)e->index < a->visible_count) {
            int node = a->visible[e->index];
            int open = !is_open(a, node);
            set_open(a, node, open);
            /* Collapsing a category that holds the selection selects the category. */
            if (!open && a->selected != node && is_ancestor(a->tree, node, a->selected)) a->selected = node;
            rebuild(a);
        }
        return;
    case DU_EV_OPEN:
        if (e->id == SIV_LIST) open_row(a, e->index);
        return;
    case DU_EV_DIALOG:
        if (e->id == SID_SAVE && !e->index && e->text && e->text[0]) save_text(a, e->text);
        return;
    case DU_EV_ACTIVATE: break;
    default: return;
    }
    switch (e->id) {
    case SIM_SAVE:
        if (du_file_dialog(u, SID_SAVE, 1, "Save As Text File", a->folder, "sysinfo.txt", NULL, NULL))
            sysui_box(u, SID_MESSAGE, "Save As Text File", "The file dialog could not be opened.", DU_DLG_ICON_ERROR);
        break;
    case SIM_EXIT: if (a->host.quit) a->host.quit(a->host.ctx); break;
    case SIM_REFRESH: siv_refresh(a); break;
    case SIM_HWREPORT: case SIM_SYSMON: case SIM_RESMETER: case SIM_TASKS: case SIM_CONTROL: launch(a, e->id); break;
    default: break;
    }
}

void siv_layout(struct du_ui *u, void *user)
{
    (void)user;
    int w = u->canvas.width, h = u->canvas.height, left = w / 3 < 170 ? 170 : w / 3 > 230 ? 230 : w / 3;
    du_get(u, SIV_TREE)->box = (struct du_rect){4, 24, left, h - 24 - 24 - 4};
    du_get(u, SIV_LIST)->box = (struct du_rect){left + 8, 24, w - left - 12, h - 24 - 24 - 4};
    du_get(u, SIV_STATUS)->box = (struct du_rect){0, h - 22, w, 22};
    struct du_widget *list = du_get(u, SIV_LIST);
    list->columns[0].width = list->box.w * 52 / 100; /* "CPU family/model/stepping" must fit */
    list->columns[1].width = list->box.w - list->columns[0].width;
}

int siv_init(struct sysinfo_app *a, struct du_ui *u, const struct sysinfo_host *host,
             struct ciuki_devices_info *devices, struct si_tree *tree)
{
    if (!a || !u || !host || !host->collect || !host->launch || !host->write_file || !devices || !tree) return -EINVAL;
    memset(a, 0, sizeof(*a));
    a->ui = u; a->host = *host; a->devices = devices; a->tree = tree;
    snprintf(a->folder, sizeof(a->folder), "/Documents");
    a->root_icon = du_icon_argb(du_icon_name("place:computer"), 16);
    a->folder_icon = du_icon_argb(du_icon_name("type:folder"), 16);
    a->warning_icon = du_icon_argb(du_icon_name("status:warning"), 16);
    a->error_icon = du_icon_argb(du_icon_name("status:error"), 16);
    a->info_icon = du_icon_argb(du_icon_name("status:info"), 16);
    a->file_items[0] = (struct du_menu_item){ "&Save As Text File...", SIM_SAVE, 0, 0, 0 };
    a->file_items[1] = (struct du_menu_item){ NULL, 0, 0, 0, DU_ITEM_SEPARATOR };
    a->file_items[2] = (struct du_menu_item){ "E&xit", SIM_EXIT, 0, 0, 0 };
    a->view_items[0] = (struct du_menu_item){ "&Refresh", SIM_REFRESH, DU_KEY_F5, 0, 0 };
    for (unsigned i = 0; i < 5; i++) {
        static char labels[5][32];
        snprintf(labels[i], sizeof(labels[i]), "&%s", tools[i].label);
        a->tools_items[i] = (struct du_menu_item){ labels[i], tools[i].id, 0, 0, 0 };
    }
    a->menus[0] = (struct du_menu){ "&File", a->file_items, 3 };
    a->menus[1] = (struct du_menu){ "&View", a->view_items, 1 };
    a->menus[2] = (struct du_menu){ "&Tools", a->tools_items, 5 };
    u->menus = a->menus; u->menu_count = 3;
    u->event = siv_event; u->user = a; u->layout = siv_layout;
    struct du_widget *w = du_add(u, DU_TREE, SIV_TREE, (struct du_rect){4, 24, 200, 400}, "");
    w = du_add(u, DU_LISTVIEW, SIV_LIST, (struct du_rect){212, 24, 424, 400}, "");
    w->view = DU_DETAILS; w->column_count = 2;
    memcpy(w->columns, list_columns, sizeof(list_columns));
    w = du_add(u, DU_STATUS, SIV_STATUS, (struct du_rect){0, u->canvas.height - 22, u->canvas.width, 22}, "");
    w->panels[0] = 130; w->panels[1] = 130;
    siv_layout(u, a);
    siv_refresh(a);
    du_focus(u, SIV_TREE);
    return 0;
}
