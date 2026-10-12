/* SPDX-License-Identifier: MIT
 * Files controller: layout, menus, panes, selection, operations and jobs. */
#include "app.h"
#include "../desktop/catalog.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    C_BACK = 1, C_FORWARD, C_UP, C_CUT, C_COPY, C_PASTE, C_DELETE, C_PROPERTIES, C_VIEWS,
    C_OPEN, C_OPEN_NOTE, C_NEW_FOLDER, C_NEW_TEXT, C_RENAME, C_CLOSE, C_SELECT_ALL, C_INVERT,
    C_ICONS, C_LIST, C_DETAILS, C_SORT_NAME, C_SORT_SIZE, C_SORT_TYPE, C_SORT_MODIFIED, C_REFRESH,
    C_ADDRESS, C_GO_ROOT, C_GO_BIN, C_GO_SYSTEM, C_HELP, C_ABOUT, C_PASTE_INTO, C_TREE_OPEN,
    W_ADDRESS = 100, W_ADDRESS_LABEL, W_PLACES_HEADER, W_PLACES, W_FOLDERS_HEADER, W_TREE, W_SPLIT,
    W_LIST, W_STATUS, W_RENAME, W_BAND1, W_BAND2, W_BAND3, W_TSEP1, W_TSEP2, W_TSEP3,
    D_DELETE = 200, D_REPLACE, D_ERROR, D_PROPERTIES, D_INFO, D_PROGRESS,
    C_GO_DOCUMENTS = 300, C_GO_GAMES, C_GO_CONTROL,
    T_JOB = 1
};
#define PANE_TOP 92
#define STATUS_H 20

static const struct du_menu_item file_template[] = {
    {"&Open", C_OPEN, 0, 0, DU_ITEM_DEFAULT}, {"Open with &CiukNote", C_OPEN_NOTE, 0, 0, 0},
    {NULL, 0, 0, 0, DU_ITEM_SEPARATOR},
    {"New &Folder", C_NEW_FOLDER, 0, 0, 0}, {"New &Text Document", C_NEW_TEXT, 0, 0, 0},
    {NULL, 0, 0, 0, DU_ITEM_SEPARATOR},
    {"&Delete", C_DELETE, DU_KEY_DELETE, 0, 0}, {"Rena&me", C_RENAME, DU_KEY_F2, 0, 0},
    {"P&roperties", C_PROPERTIES, DU_KEY_ENTER, DU_ALT, 0},
    {NULL, 0, 0, 0, DU_ITEM_SEPARATOR}, {"&Close", C_CLOSE, DU_KEY_F4, DU_ALT, 0}};
static const struct du_menu_item edit_template[] = {
    {"Cu&t", C_CUT, 0x2d, DU_CTRL, 0}, {"&Copy", C_COPY, 0x2e, DU_CTRL, 0}, {"&Paste", C_PASTE, 0x2f, DU_CTRL, 0},
    {NULL, 0, 0, 0, DU_ITEM_SEPARATOR},
    {"Select &All", C_SELECT_ALL, 0x1e, DU_CTRL, 0}, {"&Invert Selection", C_INVERT, 0, 0, 0}};
static const struct du_menu_item view_template[] = {
    {"Lar&ge Icons", C_ICONS, 0, 0, DU_ITEM_RADIO}, {"&List", C_LIST, 0, 0, DU_ITEM_RADIO},
    {"&Details", C_DETAILS, 0, 0, DU_ITEM_RADIO}, {NULL, 0, 0, 0, DU_ITEM_SEPARATOR},
    {"Sort by &Name", C_SORT_NAME, 0, 0, DU_ITEM_RADIO}, {"Sort by &Size", C_SORT_SIZE, 0, 0, DU_ITEM_RADIO},
    {"Sort by &Type", C_SORT_TYPE, 0, 0, DU_ITEM_RADIO}, {"Sort by &Modified", C_SORT_MODIFIED, 0, 0, DU_ITEM_RADIO},
    {NULL, 0, 0, 0, DU_ITEM_SEPARATOR}, {"&Refresh", C_REFRESH, DU_KEY_F5, 0, 0}};
static const struct du_menu_item go_template[] = {
    {"&Back", C_BACK, DU_KEY_LEFT, DU_ALT, 0}, {"&Forward", C_FORWARD, DU_KEY_RIGHT, DU_ALT, 0},
    {"&Up One Level", C_UP, DU_KEY_UP, DU_ALT, 0}, {NULL, 0, 0, 0, DU_ITEM_SEPARATOR},
    {"&Address Bar", C_ADDRESS, 0x20, DU_ALT, 0}, {NULL, 0, 0, 0, DU_ITEM_SEPARATOR},
    {"&Computer", C_GO_ROOT, 0, 0, 0}, {"&Programs", C_GO_BIN, 0, 0, 0},
    {"&Documents", C_GO_DOCUMENTS, 0, 0, 0}, {"&System", C_GO_SYSTEM, 0, 0, 0}, {"&Games", C_GO_GAMES, 0, 0, 0}, {"Control &Panel", C_GO_CONTROL, 0, 0, 0}};
static const struct du_menu_item help_template[] = {
    {"&Help Topics", C_HELP, DU_KEY_F1, 0, 0}, {NULL, 0, 0, 0, DU_ITEM_SEPARATOR}, {"&About Files", C_ABOUT, 0, 0, 0}};

static const char *const yes_no[] = {"&Yes", "&No"};
static const char *const replace_buttons[] = {"&Yes", "Yes to &All", "&No", "Cancel"};

/* ---- formatting ---- */
static void group(char *out, size_t n, uint64_t v)
{
    char digits[32]; int len = snprintf(digits, sizeof(digits), "%llu", (unsigned long long)v), k = 0;
    size_t at = 0;
    for (int i = 0; i < len && at + 2 < n; i++) {
        out[at++] = digits[i];
        k = len - i - 1;
        if (k && k % 3 == 0) out[at++] = ',';
    }
    out[at] = 0;
}

void files_size_text(char *out, size_t n, uint64_t bytes)
{
    static const char *const units[] = {"KB", "MB", "GB", "TB"};
    if (bytes < 1024) { snprintf(out, n, "%llu bytes", (unsigned long long)bytes); return; }
    uint64_t scaled = bytes; unsigned u = 0;
    while (scaled >= 1024 * 1024 && u < 3) { scaled /= 1024; u++; }
    uint64_t hundredths = scaled * 100 / 1024;
    if (hundredths < 1000) snprintf(out, n, "%llu.%02llu %s", (unsigned long long)(hundredths / 100), (unsigned long long)(hundredths % 100), units[u]);
    else if (hundredths < 10000) snprintf(out, n, "%llu.%llu %s", (unsigned long long)(hundredths / 100), (unsigned long long)(hundredths % 100 / 10), units[u]);
    else snprintf(out, n, "%llu %s", (unsigned long long)(hundredths / 100), units[u]);
}

static void kb_text(char *out, size_t n, uint64_t bytes)
{
    char g[32]; group(g, sizeof(g), (bytes + 1023) / 1024);
    snprintf(out, n, "%s KB", g);
}

static void time_text(char *out, size_t n, int64_t when)
{
    time_t t = (time_t)when; struct tm tm;
    if (when <= 0 || !gmtime_r(&t, &tm)) { snprintf(out, n, "%s", ""); return; }
    snprintf(out, n, "%04d-%02d-%02d %02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
}

static const char *reason(int error)
{
    switch (error < 0 ? -error : error) {
    case EEXIST: return "A file or folder with the name you specified already exists.\nSpecify a different name.";
    case EINVAL: return "The name or destination is not valid. A file name cannot\ncontain any of the following characters: \\ / : * ? \" < > |";
    case ENOENT: return "The file or folder cannot be found. It may have been moved or deleted.";
    case EROFS: return "The disk is read-only.";
    case ENOSPC: return "There is not enough free disk space.";
    case EACCES: case EPERM: return "Access is denied. The file may be read-only.";
    case ENOTEMPTY: return "The folder is not empty.";
    case EBUSY: return "The file or folder is in use.";
    case ENAMETOOLONG: return "The name is too long.";
    case ENOTDIR: return "The path does not name a folder.";
    case EISDIR: return "A folder has that name.";
    case EIO: return "A disk error occurred (I/O error).";
    case ENOMEM: return "There is not enough memory.";
    case ENOEXEC: return "The file is not a CiukiOS program.";
    default: return "The operation failed.";
    }
}

static void error_box(struct files_app *a, const char *title, const char *what, int error)
{
    char text[DU_DIALOG_TEXT];
    snprintf(text, sizeof(text), "%s\n\n%s", what, reason(error));
    du_message(a->ui, D_ERROR, title, text, DU_DLG_ICON_ERROR, NULL, 0, 0, 0);
}

static void checkpoint(struct files_app *a, int stage)
{
    if (!a->test || stage != a->stage + 1) return;
    a->stage = stage;
    if (a->checkpoint) a->checkpoint(a, stage);
}

/* ---- icons: Tango store (libdeskui du_icon, icon_map.c) ---- */
static const uint32_t *icon_of(const struct fm_entry *e, unsigned size)
{ return du_icon_argb(du_icon_file(e->name, e->meta.type == FM_DIR, e->meta.exec != 0), size); }
static const uint32_t *icon16(const struct fm_entry *e) { return icon_of(e, 16); }
static const uint32_t *icon32(const struct fm_entry *e) { return icon_of(e, 32); }
static const uint32_t *named(const char *object, unsigned size) { return du_icon_argb(du_icon_name(object), size); }

/* Deterministic box-filter size conversion of the approved portrait array. */
static void shrink(const uint32_t *src, uint32_t *dst, int size)
{
    int block = 256 / size;
    for (int y = 0; y < size; y++) for (int x = 0; x < size; x++) {
        uint32_t r = 0, g = 0, b = 0;
        for (int yy = 0; yy < block; yy++) for (int xx = 0; xx < block; xx++) {
            uint32_t p = src[(y * block + yy) * 256 + x * block + xx];
            r += (p >> 16) & 255; g += (p >> 8) & 255; b += p & 255;
        }
        unsigned n = (unsigned)(block * block);
        dst[y * size + x] = 0xff000000u | ((r + n / 2) / n) << 16 | ((g + n / 2) / n) << 8 | (b + n / 2) / n;
    }
}

/* ---- widgets and layout ---- */
static struct du_widget *W(struct files_app *a, int id) { return du_get(a->ui, id); }

static const struct { int id; const char *label; const char *icon; int width; } toolbar[] = {
    {C_BACK, "Back", "action:back", 46}, {C_FORWARD, "Forward", "action:forward", 54}, {C_UP, "Up", "action:up", 40},
    {C_CUT, "Cut", "action:cut", 40}, {C_COPY, "Copy", "action:copy", 40}, {C_PASTE, "Paste", "action:paste", 44},
    {C_DELETE, "Delete", "action:delete", 48}, {C_PROPERTIES, "Properties", "action:properties", 70},
    {C_VIEWS, "Views", "action:views", 56}};

static struct du_rect rename_box(struct du_widget *list, unsigned index)
{
    du_list_ensure(list, index);
    struct du_rect label = du_list_label(list, index), body = du_list_body(list);
    int width = label.w + 30; if (width < 110) width = 110;
    int x = label.x - 1;
    if (list->view == DU_ICONS) { width = 96; x = label.x + label.w / 2 - width / 2; }
    if (x + width > body.x + body.w) width = body.x + body.w - x;
    if (x < body.x) x = body.x;
    return (struct du_rect){x, label.y - 2, width, 19};
}

void files_layout(struct files_app *a)
{
    a->width = a->ui->canvas.width; a->height = a->ui->canvas.height;
    int w = a->width, h = a->height;
    if (a->split < 120) a->split = 120;
    if (a->split > w - 260) a->split = w - 260;
    W(a, W_BAND1)->box = (struct du_rect){0, 20, w, 2};
    int x = 4;
    for (unsigned i = 0; i < sizeof(toolbar) / sizeof(toolbar[0]); i++) {
        W(a, toolbar[i].id)->box = (struct du_rect){x, 23, toolbar[i].width, 39};
        x += toolbar[i].width + 1;
        int sep = toolbar[i].id == C_UP ? W_TSEP1 : toolbar[i].id == C_PASTE ? W_TSEP2 : toolbar[i].id == C_PROPERTIES ? W_TSEP3 : 0;
        if (sep) { W(a, sep)->box = (struct du_rect){x + 2, 25, 2, 35}; x += 7; }
    }
    W(a, W_BAND2)->box = (struct du_rect){0, 63, w, 2};
    W(a, W_ADDRESS_LABEL)->box = (struct du_rect){6, 66, 48, 22};
    W(a, W_ADDRESS)->box = (struct du_rect){56, 66, w - 60, 22};
    W(a, W_BAND3)->box = (struct du_rect){0, 89, w, 2};
    int bottom = h - STATUS_H - 1, places_h = (int)a->place_count * DU_ROW + 6;
    W(a, W_PLACES_HEADER)->box = (struct du_rect){0, PANE_TOP, a->split, 18};
    W(a, W_PLACES)->box = (struct du_rect){0, PANE_TOP + 18, a->split, places_h};
    int fy = PANE_TOP + 18 + places_h + 2;
    W(a, W_FOLDERS_HEADER)->box = (struct du_rect){0, fy, a->split, 18};
    W(a, W_TREE)->box = (struct du_rect){0, fy + 18, a->split, bottom - fy - 18};
    W(a, W_SPLIT)->box = (struct du_rect){a->split, PANE_TOP, 4, bottom - PANE_TOP};
    struct du_widget *list = W(a, W_LIST);
    list->box = (struct du_rect){a->split + 4, PANE_TOP, w - a->split - 4, bottom - PANE_TOP};
    int name = list->box.w - 4 - 16 - 70 - 132 - 108;
    if (name < 120) name = 120;
    list->columns[0] = (struct du_column){"Name", name, 0};
    list->columns[1] = (struct du_column){"Size", 70, 1};
    list->columns[2] = (struct du_column){"Type", 132, 0};
    list->columns[3] = (struct du_column){"Modified", 108, 0};
    list->column_count = 4;
    struct du_widget *status = W(a, W_STATUS);
    status->box = (struct du_rect){0, h - STATUS_H, w, STATUS_H};
    status->panels[0] = w - 250; status->panels[1] = 100; status->panels[2] = 0;
    if (a->rename_index >= 0) {
        W(a, W_RENAME)->box = rename_box(list, (unsigned)a->rename_index);
    }
    a->ui->dirty = 1;
}

static void resize_layout(struct du_ui *u, void *user)
{ (void)u; files_layout(user); }

/* ---- places and folder tree ---- */
struct names { char (*v)[FM_NAME]; unsigned count, capacity; const char *parent; };
static int collect_dir(const char *name, const struct fm_meta *meta, void *ctx)
{
    struct names *n = ctx;
    if (meta->type != FM_DIR || !strcmp(name, ".") || !strcmp(name, "..") ||
        (n->parent && fm_hidden(n->parent, name)) || n->count >= 512) return 0;
    if (n->count == n->capacity) {
        unsigned cap = n->capacity ? n->capacity * 2 : 16;
        void *p = realloc(n->v, cap * sizeof(*n->v));
        if (!p) return -ENOMEM;
        n->v = p; n->capacity = cap;
    }
    snprintf(n->v[n->count++], FM_NAME, "%s", name);
    return 0;
}
static unsigned fold(unsigned c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int name_order(const char *x, const char *y)
{
    while (*x && fold((unsigned char)*x) == fold((unsigned char)*y)) { x++; y++; }
    int r = (int)fold((unsigned char)*x) - (int)fold((unsigned char)*y);
    return r;
}
static void sort_names(struct names *n)
{
    for (unsigned i = 1; i < n->count; i++) {
        char key[FM_NAME]; strcpy(key, n->v[i]); unsigned j = i;
        while (j > 0 && name_order(n->v[j - 1], key) > 0) { strcpy(n->v[j], n->v[j - 1]); j--; }
        strcpy(n->v[j], key);
    }
}

static void places_build(struct files_app *a)
{
    static const char *const names[] = {"Computer", "Programs", "Documents", "System", "Games", "Control Panel"};
    static const char *const paths[] = {FM_COMPUTER, "/Programs", "/Documents", "/system", "/Games", "control:"};
    static const char *const icons[] = {"place:computer", "place:applications", "place:documents", "place:system", "place:folder", "menu:control-panel"};
    a->place_count = 0;
    for (unsigned i = 0; i < 6; i++) {
        struct fm_meta meta;
        if (i == 4 && (a->model.fs.stat_path(a->model.fs.ctx, paths[i], &meta) || meta.type != FM_DIR)) continue;
        struct files_place *p = &a->places[a->place_count++];
        snprintf(p->name, sizeof(p->name), "%s", names[i]); snprintf(p->path, sizeof(p->path), "%s", paths[i]);
    }
    for (unsigned i = 0; i < a->place_count; i++) {
        struct du_item *it = &a->place_items[i];
        memset(it, 0, sizeof(*it));
        it->text[0] = a->places[i].name;
        it->icon16 = i == 0 && a->ciuki ? a->ciuki16 : named(!strcmp(a->places[i].path, "control:") ? "menu:control-panel" : icons[i], 16);
    }
    struct du_widget *w = W(a, W_PLACES);
    w->items = a->place_items; w->count = a->place_count;
}

static int in_set(char set[][FM_PATH], unsigned count, const char *path)
{ for (unsigned i = 0; i < count; i++) if (fm_same_path(set[i], path)) return 1; return 0; }
static void set_add(char set[][FM_PATH], unsigned *count, const char *path)
{
    if (in_set(set, *count, path)) return;
    if (*count == FILES_CACHE) { memmove(set[0], set[1], (FILES_CACHE - 1) * FM_PATH); (*count)--; }
    snprintf(set[(*count)++], FM_PATH, "%s", path);
}
static void set_remove(char set[][FM_PATH], unsigned *count, const char *path)
{
    for (unsigned i = 0; i < *count; i++) if (fm_same_path(set[i], path)) {
        memmove(set[i], set[i + 1], (*count - i - 1) * FM_PATH); (*count)--; return;
    }
}
static int expanded(struct files_app *a, const char *path)
{
    if (in_set(a->collapsed, a->collapsed_count, path)) return 0;
    return fm_beneath(a->model.path, path) || in_set(a->expanded, a->expanded_count, path);
}

static void tree_add(struct files_app *a, unsigned parent)
{
    struct files_node *p = &a->tree[parent];
    struct names n = {.parent = p->path};
    if (a->model.fs.list(a->model.fs.ctx, p->path, collect_dir, &n)) { free(n.v); p->children = 0; return; }
    sort_names(&n);
    p->children = n.count > 0;
    if (!n.count) set_add(a->leaf, &a->leaf_count, p->path); else set_remove(a->leaf, &a->leaf_count, p->path);
    for (unsigned i = 0; i < n.count && a->tree_count < FILES_TREE; i++) {
        unsigned index = a->tree_count++;
        struct files_node *c = &a->tree[index];
        snprintf(c->name, sizeof(c->name), "%s", n.v[i]);
        if (fm_join(c->path, a->tree[parent].path, n.v[i])) { a->tree_count--; continue; }
        c->depth = a->tree[parent].depth + 1;
        c->expanded = (uint8_t)expanded(a, c->path);
        c->children = !in_set(a->leaf, a->leaf_count, c->path);
        if (c->expanded) tree_add(a, index);
    }
    free(n.v);
}

static int tree_drive(const char *name, const struct fm_meta *meta, void *ctx)
{
    struct files_app *a = ctx;
    (void)meta;
    if (a->tree_count == FILES_TREE) return 0;
    unsigned index = a->tree_count++;
    struct files_node *n = &a->tree[index];
    snprintf(n->name, sizeof(n->name), "%s", name);
    fm_posix_path(n->path, name); n->depth = 1;
    n->expanded = (uint8_t)expanded(a, n->path); n->children = 1;
    if (n->expanded) tree_add(a, index);
    return 0;
}

static void tree_build(struct files_app *a)
{
    a->tree_count = 1;
    struct files_node *root = &a->tree[0];
    snprintf(root->name, sizeof(root->name), "Computer");
    strcpy(root->path, FM_COMPUTER); root->depth = 0;
    root->expanded = (uint8_t)!in_set(a->collapsed, a->collapsed_count, FM_COMPUTER); root->children = 1;
    if (root->expanded) fm_volumes(&a->model.fs, tree_drive, a);
    a->tree_dirty = 0;
}

static void tree_items(struct files_app *a)
{
    struct du_widget *w = W(a, W_TREE);
    int current = -1;
    for (unsigned i = 0; i < a->tree_count; i++) {
        struct files_node *n = &a->tree[i];
        struct du_item *it = &a->tree_items[i];
        memset(it, 0, sizeof(*it));
        int here = fm_same_path(n->path, a->model.path);
        if (here) current = (int)i;
        it->text[0] = n->name;
        it->icon16 = !i ? (a->ciuki ? a->ciuki16 : named("place:computer", 16)) :
                     named(n->depth == 1 ? "place:volume" : here ? "type:folder-open" : "type:folder", 16);
        it->selected = (uint8_t)here; it->depth = (uint8_t)(n->depth > 255 ? 255 : n->depth);
        it->expanded = n->expanded; it->children = n->children;
    }
    w->items = a->tree_items; w->count = a->tree_count;
    if (current >= 0) { w->active = (unsigned)current; du_list_ensure(w, w->active); }
    else if (w->active >= w->count) w->active = 0;
}

/* ---- model to widgets ---- */
static int in_clipboard(const struct files_app *a, const char *name)
{
    if (!a->model.cut) return 0;
    char path[FM_PATH];
    if (fm_join(path, a->model.path, name)) return 0;
    for (unsigned i = 0; i < a->model.clipboard_count; i++) if (fm_same_path(a->model.clipboard[i], path)) return 1;
    return 0;
}

static void set_flag(struct du_menu_item *items, unsigned n, int id, unsigned flag, int on)
{ for (unsigned i = 0; i < n; i++) if (items[i].id == id) items[i].flags = on ? items[i].flags | flag : items[i].flags & ~flag; }

void files_update(struct files_app *a)
{
    struct fm_model *m = &a->model;
    int computer = !m->path[0];
    char display[FM_PATH];
    if (fm_display_path(display, m->path)) strcpy(display, "(path too long)");
    if (m->count > a->capacity) {
        unsigned cap = m->count + 32;
        struct du_item *rows = realloc(a->rows, cap * sizeof(*rows));
        if (rows) a->rows = rows;
        struct files_row *texts = realloc(a->texts, cap * sizeof(*texts));
        if (texts) a->texts = texts;
        if (rows && texts) a->capacity = cap;
    }
    unsigned count = m->count < a->capacity ? m->count : a->capacity;
    uint64_t folder_bytes = 0;
    for (unsigned i = 0; i < count; i++) {
        struct fm_entry *e = &m->entries[i];
        struct files_row *t = &a->texts[i];
        if (e->meta.type == FM_DIR) t->size[0] = 0; else { kb_text(t->size, sizeof(t->size), e->meta.size); folder_bytes += e->meta.size; }
        char buffer[40]; snprintf(t->type, sizeof(t->type), "%s", computer ? "Disk Drive" : fm_type_name(e, buffer, sizeof(buffer)));
        time_text(t->modified, sizeof(t->modified), computer ? 0 : e->meta.modified);
        a->rows[i] = (struct du_item){{e->name, t->size, t->type, t->modified},
                                      computer ? named("place:volume", 16) : icon16(e), computer ? named("place:volume", 32) : icon32(e),
                                      (uint8_t)e->selected, (uint8_t)in_clipboard(a, e->name), 0, 0, 0};
    }
    struct du_widget *list = W(a, W_LIST);
    list->items = a->rows; list->count = count;
    list->active = m->active < count ? m->active : 0;
    list->sort = (int)m->sort; list->descending = m->descending;
    du_set_text(list, "This folder is empty.");
    struct du_widget *address = W(a, W_ADDRESS);
    if (du_focused(a->ui) != W_ADDRESS) du_set_text(address, display);
    address->icon = computer ? (a->ciuki ? a->ciuki16 : named("place:computer", 16)) : named("type:folder", 16);
    uint64_t bytes = 0; unsigned selected = fm_selected(m, &bytes), selected_files = 0;
    for (unsigned i = 0; i < count; i++) selected_files += m->entries[i].selected && m->entries[i].meta.type == FM_FILE;
    char size[48] = "", left[200];
    /* Only the selection's size: a folder total would tie the status bar
     * (and the test digests) to unrelated file sizes such as the kernel's. */
    if (selected_files) files_size_text(size, sizeof(size), bytes);
    (void)folder_bytes;
    if (a->note[0]) snprintf(left, sizeof(left), "%s", a->note);
    else if (selected) snprintf(left, sizeof(left), "%u object(s) selected", selected);
    else snprintf(left, sizeof(left), "%u object(s)", count);
    snprintf(W(a, W_STATUS)->text, DU_TEXT_MAX, "%s\t%s\t%.*s", left, size, DU_TEXT_MAX - 252, display);
    int root = computer;
    W(a, C_BACK)->disabled = !m->history_at;
    W(a, C_FORWARD)->disabled = m->history_at + 1 >= m->history_count;
    W(a, C_UP)->disabled = (uint8_t)root;
    W(a, C_CUT)->disabled = W(a, C_COPY)->disabled = W(a, C_DELETE)->disabled = computer || !selected;
    W(a, C_PASTE)->disabled = computer || !m->clipboard_count;
    W(a, C_PROPERTIES)->disabled = (uint8_t)computer;
    set_flag(a->file_items, 11, C_OPEN, DU_ITEM_DISABLED, !selected);
    set_flag(a->file_items, 11, C_OPEN_NOTE, DU_ITEM_DISABLED, computer || !selected);
    set_flag(a->file_items, 11, C_DELETE, DU_ITEM_DISABLED, computer || !selected);
    set_flag(a->file_items, 11, C_RENAME, DU_ITEM_DISABLED, computer || selected != 1);
    set_flag(a->file_items, 11, C_NEW_FOLDER, DU_ITEM_DISABLED, computer);
    set_flag(a->file_items, 11, C_NEW_TEXT, DU_ITEM_DISABLED, computer);
    set_flag(a->file_items, 11, C_PROPERTIES, DU_ITEM_DISABLED, computer);
    set_flag(a->edit_items, 6, C_CUT, DU_ITEM_DISABLED, computer || !selected);
    set_flag(a->edit_items, 6, C_COPY, DU_ITEM_DISABLED, computer || !selected);
    set_flag(a->edit_items, 6, C_PASTE, DU_ITEM_DISABLED, computer || !m->clipboard_count);
    set_flag(a->go_items, 12, C_BACK, DU_ITEM_DISABLED, !m->history_at);
    set_flag(a->go_items, 12, C_FORWARD, DU_ITEM_DISABLED, m->history_at + 1 >= m->history_count);
    set_flag(a->go_items, 12, C_UP, DU_ITEM_DISABLED, root);
    set_flag(a->go_items, 12, C_GO_GAMES, DU_ITEM_DISABLED, a->place_count < 6);
    set_flag(a->view_items, 10, C_ICONS, DU_ITEM_CHECKED, list->view == DU_ICONS);
    set_flag(a->view_items, 10, C_LIST, DU_ITEM_CHECKED, list->view == DU_LIST);
    set_flag(a->view_items, 10, C_DETAILS, DU_ITEM_CHECKED, list->view == DU_DETAILS);
    for (unsigned c = 0; c < 4; c++) set_flag(a->view_items, 10, C_SORT_NAME + (int)c, DU_ITEM_CHECKED, m->sort == c);
    struct du_widget *places = W(a, W_PLACES);
    int place = -1;
    for (unsigned i = 0; i < a->place_count; i++) {
        a->place_items[i].selected = (uint8_t)fm_same_path(a->places[i].path, m->path);
        if (a->place_items[i].selected) place = (int)i;
    }
    if (place >= 0) places->active = (unsigned)place;
    if (a->tree_dirty) tree_build(a);
    tree_items(a);
    a->ui->dirty = 1;
}

/* ---- inline rename ---- */
static void rename_hide(struct files_app *a)
{
    W(a, W_RENAME)->hidden = 1; a->rename_index = -1; a->rename_new = 0;
    du_focus(a->ui, W_LIST); a->ui->dirty = 1;
}

static void rename_start(struct files_app *a, unsigned index, int is_new)
{
    if (index >= a->model.count) return;
    struct du_widget *list = W(a, W_LIST), *edit = W(a, W_RENAME);
    list->active = index;
    edit->box = rename_box(list, index);
    edit->def = 1; edit->hidden = 0;
    du_set_text(edit, a->model.entries[index].name);
    edit->anchor = 0;
    snprintf(a->rename_original, sizeof(a->rename_original), "%s", a->model.entries[index].name);
    a->rename_index = (int)index; a->rename_new = is_new;
    du_focus(a->ui, W_RENAME);
    a->ui->dirty = 1;
}

static void rename_commit(struct files_app *a)
{
    if (a->rename_index < 0) return;
    char name[FM_NAME]; snprintf(name, sizeof(name), "%s", W(a, W_RENAME)->text);
    unsigned index = (unsigned)a->rename_index;
    char original[FM_NAME]; strcpy(original, a->rename_original);
    rename_hide(a);
    if (index >= a->model.count || strcmp(a->model.entries[index].name, original)) { files_update(a); return; }
    if (strcmp(name, original)) {
        int r = fm_rename(&a->model, index, name);
        if (r) {
            char what[FM_NAME + 40]; snprintf(what, sizeof(what), "Cannot rename %s:", original);
            error_box(a, "Error Renaming File or Folder", what, r);
        } else { a->tree_dirty = 1; checkpoint(a, 3); }
    }
    files_update(a);
}

/* ---- navigation ---- */
int files_navigate(struct files_app *a, const char *path)
{
    if (a->rename_index >= 0) rename_hide(a);
    int r = fm_navigate(&a->model, path);
    if (r) {
        char what[FM_PATH + 64]; snprintf(what, sizeof(what), "Cannot open '%s'.", path);
        error_box(a, "Files", what, r);
        files_update(a);
        return r;
    }
    struct du_widget *list = W(a, W_LIST);
    list->scroll = list->hscroll = 0;
    a->note[0] = 0; a->tree_dirty = 1;
    files_update(a);   /* list items point into the new model entries */
    du_list_ensure(list, list->active);
    if (fm_same_path(a->model.path, "/system")) checkpoint(a, 1);
    return 0;
}

static void after_history(struct files_app *a, int r)
{
    if (a->rename_index >= 0) rename_hide(a);
    if (r && r != -ENOENT) error_box(a, "Files", "Cannot open the folder.", r);
    struct du_widget *list = W(a, W_LIST);
    list->scroll = list->hscroll = 0;
    a->note[0] = 0; a->tree_dirty = 1;
    files_update(a);
    du_list_ensure(list, list->active);
}

/* ---- opening ---- */
void files_launch_failed(struct files_app *a, int error)
{
    error_box(a, "Files", "Cannot start the application:", error > 0 ? -error : error);
}
static int spawn(struct files_app *a, const char *program, char *const argv[])
{
    if (!a->spawn) return -ENOSYS;
    return a->spawn(a, program, argv);
}

static void open_item(struct files_app *a, unsigned index, int with_note)
{
    if (index >= a->model.count) return;
    struct fm_entry *e = &a->model.entries[index];
    char path[FM_PATH];
    if (!a->model.path[0] && with_note) return;
    if (fm_entry_path(&a->model, index, path)) return;
    if (e->meta.type == FM_DIR && !with_note) { files_navigate(a, path); return; }
    enum fm_kind kind = fm_kind(e);
    if (e->meta.exec && !with_note) {
        char *argv[] = {(char *)e->name, NULL};
        int r = spawn(a, path, argv);
        if (r < 0) { char what[FM_NAME + 40]; snprintf(what, sizeof(what), "Cannot run %s:", e->name); error_box(a, "Files", what, r); }
        else { snprintf(a->note, sizeof(a->note), "Started %.120s", e->name); files_update(a); }
        return;
    }
    if (with_note || kind == FM_KIND_TEXT || kind == FM_KIND_SCRIPT) {
        struct fm_meta meta;
        if (a->model.fs.stat_path(a->model.fs.ctx, "/Programs/ciuknote", &meta) || meta.type != FM_FILE) {
            du_message(a->ui, D_INFO, "Files", "CiukNote is not installed.\nText files open in /Programs/ciuknote once it is available.",
                       DU_DLG_ICON_INFO, NULL, 0, 0, 0);
            return;
        }
        char *argv[] = {"ciuknote", path, NULL};
        int r = spawn(a, "/Programs/ciuknote", argv);
        if (r < 0) error_box(a, "Files", "Cannot start CiukNote:", r);
        return;
    }
    const struct desk_catalog_entry *program = desk_catalog_file(path);
    if (kind == FM_KIND_IMAGE || program) {
        struct fm_meta meta;
        char executable[80];
        if (program) snprintf(executable, sizeof(executable), "/Programs/%s", program->name);
        if (program && !a->model.fs.stat_path(a->model.fs.ctx, executable, &meta) && meta.type == FM_FILE) {
            char *argv[] = {(char *)program->name, path, NULL};
            int r = spawn(a, executable, argv);
            if (r < 0) error_box(a, "Files", "Cannot start the image application:", r);
            return;
        }
    }
    char text[FM_NAME + 160], type[40];
    snprintf(text, sizeof(text), "Files cannot open '%s'.\nNo program is registered for files of type\n'%s'.",
             e->name, fm_type_name(e, type, sizeof(type)));
    du_message(a->ui, D_INFO, "Open With", text, DU_DLG_ICON_INFO, NULL, 0, 0, 0);
}

/* ---- jobs ---- */
static const char *verb(enum fm_job_kind kind)
{ return kind == FM_COPY ? "copy" : kind == FM_MOVE ? "move" : kind == FM_DELETE ? "delete" : "read"; }

static void progress(struct files_app *a)
{
    struct fm_job *j = &a->job;
    const char *source = j->depth ? j->frames[j->depth - 1].source : j->next ? j->sources[j->next - 1] : j->sources[0];
    char parent[FM_PATH], text[DU_DIALOG_TEXT];
    fm_parent(parent, source);
    if (j->kind == FM_PROPERTIES) snprintf(text, sizeof(text), "Counting: %.200s\n%llu files, %llu folders", fm_basename(source),
                                           (unsigned long long)j->files, (unsigned long long)j->folders);
    else if (j->kind == FM_DELETE) snprintf(text, sizeof(text), "Deleting: %.200s\nFrom: %.300s", fm_basename(source), parent);
    else snprintf(text, sizeof(text), "%s: %.200s\nFrom: %.300s\nTo: %.300s", j->kind == FM_COPY ? "Copying" : "Moving",
                  fm_basename(source), parent, j->destination);
    const char *title = j->kind == FM_COPY ? "Copying..." : j->kind == FM_MOVE ? "Moving..." : j->kind == FM_DELETE ? "Deleting..." : "Properties";
    du_progress(a->ui, D_PROGRESS, title, text, j->completed, j->count);
}

static int start_job(struct files_app *a, enum fm_job_kind kind, char *const *paths, unsigned n, const char *destination)
{
    if (a->job_active) return -EBUSY;
    fm_job_destroy(&a->job);
    int r = fm_job_start(&a->job, &a->model.fs, kind, paths, n, destination);
    if (r) {
        char what[160]; snprintf(what, sizeof(what), "Cannot %s the selected items:", verb(kind));
        error_box(a, "Files", what, r);
        return r;
    }
    a->job_active = 1;
    du_timer_start(a->ui, T_JOB, 2000000u, 1);
    progress(a);
    return 0;
}

static void properties_dialog(struct files_app *a)
{
    struct fm_job *j = &a->job;
    char text[DU_DIALOG_TEXT], size[48], g[32], contains[96] = "", type[48], when[24] = "", parent[FM_PATH];
    struct fm_meta meta = {0};
    const char *path = j->sources[0];
    a->model.fs.stat_path(a->model.fs.ctx, path, &meta);
    files_size_text(size, sizeof(size), j->bytes); group(g, sizeof(g), j->bytes);
    fm_parent(parent, path);
    if (j->count == 1 && fm_drive_root(path)) strcpy(parent, path);
    fm_display_path(parent, parent);
    const uint32_t *icon = named("type:folder", 32);
    if (j->count == 1) {
        struct fm_entry e; memset(&e, 0, sizeof(e)); snprintf(e.name, sizeof(e.name), "%s", fm_basename(path)); e.meta = meta;
        char buffer[40]; snprintf(type, sizeof(type), "%s", fm_type_name(&e, buffer, sizeof(buffer)));
        icon = !strcmp(path, "/") && a->ciuki ? a->ciuki32 : icon32(&e);
        time_text(when, sizeof(when), meta.modified);
    } else snprintf(type, sizeof(type), "Multiple types");
    if (j->folders || meta.type == FM_DIR || j->count > 1)
        snprintf(contains, sizeof(contains), "\nContains:\t%llu Files, %llu Folders", (unsigned long long)j->files, (unsigned long long)j->folders);
    char name[FM_NAME + 32];
    if (j->count == 1) snprintf(name, sizeof(name), "%s", fm_drive_root(path) ? parent : fm_basename(path));
    else snprintf(name, sizeof(name), "%u items", j->count);
    snprintf(text, sizeof(text), "%.200s\n---\nType:\t%s\nLocation:\t%.200s\nSize:\t%s (%s bytes)%s\n---\nModified:\t%s\nAttributes:\t%s",
             name, type, parent, size, g, contains,
             when[0] ? when : "-", meta.mode & 0200 ? "(none)" : "Read-only");
    char title[FM_NAME + 32];
    snprintf(title, sizeof(title), "%.60s Properties", j->count == 1 ? (fm_drive_root(path) ? parent : fm_basename(path)) : "Selection");
    du_message(a->ui, D_PROPERTIES, title, text, DU_DLG_ICON_INFO, NULL, 0, 0, 0);
    du_dialog_icon(a->ui, icon);
}

static void job_finished(struct files_app *a)
{
    struct fm_job *j = &a->job;
    a->job_active = 0;
    du_timer_stop(a->ui, T_JOB);
    if (du_dialog_id(a->ui) == D_PROGRESS || du_dialog_id(a->ui) == D_REPLACE) du_dialog_close(a->ui);
    if (a->close_pending) { if (a->close) a->close(a); return; }
    enum fm_job_kind kind = j->kind;
    if (j->state == FM_FAILED) {
        const char *source = j->depth ? j->frames[j->depth - 1].source : j->next ? j->sources[j->next - 1] : j->sources[0];
        char what[FM_NAME + 40], title[64];
        snprintf(what, sizeof(what), "Cannot %s %.200s:", verb(kind), fm_basename(source));
        snprintf(title, sizeof(title), "Error %s File or Folder", kind == FM_COPY ? "Copying" : kind == FM_MOVE ? "Moving" :
                 kind == FM_DELETE ? "Deleting" : "Reading");
        error_box(a, title, what, -j->error);
    }
    if (kind == FM_PROPERTIES) {
        if (j->state == FM_DONE) properties_dialog(a);
        files_update(a);
        return;
    }
    fm_refresh(&a->model);
    if ((kind == FM_COPY || kind == FM_MOVE) && j->created_count && fm_same_path(j->destination, a->model.path)) {
        fm_select_none(&a->model);
        for (unsigned i = 0; i < j->created_count; i++) fm_select_name(&a->model, j->created[i], 1);
    }
    if (kind == FM_MOVE && j->state == FM_DONE && !j->skipped && a->model.cut) fm_clipboard_clear(&a->model);
    if (j->state == FM_CANCELLED) snprintf(a->note, sizeof(a->note), "Cancelled; %u item(s) completed", j->completed);
    else if (j->state == FM_DONE)
        snprintf(a->note, sizeof(a->note), "%u item(s) %s%s%s", j->completed - j->skipped,
                 kind == FM_COPY ? "copied" : kind == FM_MOVE ? "moved" : "deleted",
                 j->skipped ? ", some skipped" : "", kind == FM_COPY && j->metadata_warnings ? " (dates not kept)" : "");
    a->tree_dirty = 1;
    files_update(a);
    du_list_ensure(W(a, W_LIST), W(a, W_LIST)->active);
    if (j->state == FM_DONE && !j->skipped && kind == FM_COPY && j->completed) checkpoint(a, 4);
    if (j->state == FM_DONE && kind == FM_DELETE && j->completed) checkpoint(a, 5);
}

static void replace_prompt(struct files_app *a)
{
    struct fm_job *j = &a->job;
    struct fm_frame *f = &j->frames[j->depth - 1];
    char text[DU_DIALOG_TEXT], a1[48], a2[48], t1[24], t2[24];
    const char *name = fm_basename(f->target);
    if (j->prompt_dir)
        snprintf(text, sizeof(text), "This folder already contains a folder named '%.120s'.\n\n"
                 "If files in the existing folder have the same names as files in\n"
                 "the folder you are copying, you will be asked before each one\n"
                 "is replaced. Do you still want to copy or move the folder?", name);
    else {
        files_size_text(a1, sizeof(a1), f->existing.size); files_size_text(a2, sizeof(a2), f->meta.size);
        time_text(t1, sizeof(t1), f->existing.modified); time_text(t2, sizeof(t2), f->meta.modified);
        snprintf(text, sizeof(text), "This folder already contains a file named '%.120s'.\n\n"
                 "Would you like to replace the existing file\n\t%s, modified %s\n\nwith this one?\n\t%s, modified %s",
                 name, a1, t1[0] ? t1 : "-", a2, t2[0] ? t2 : "-");
    }
    du_message(a->ui, D_REPLACE, j->prompt_dir ? "Confirm Folder Replace" : "Confirm File Replace", text,
               DU_DLG_ICON_QUESTION, replace_buttons, 4, 0, 3);
}

static void job_tick(struct files_app *a)
{
    if (!a->job_active) return;
    struct fm_job *j = &a->job;
    for (unsigned n = 0; n < 24 && j->state == FM_RUNNING; n++) fm_job_step(j);
    if (j->state == FM_REPLACE) { if (du_dialog_id(a->ui) != D_REPLACE) replace_prompt(a); return; }
    if (j->state == FM_RUNNING) { progress(a); return; }
    job_finished(a);
}

/* ---- commands ---- */
static void paste(struct files_app *a, const char *destination)
{
    if (!a->model.clipboard_count) return;
    start_job(a, a->model.cut ? FM_MOVE : FM_COPY, a->model.clipboard, a->model.clipboard_count, destination);
}

static int selection_paths(struct files_app *a, char ***out, unsigned *count)
{
    unsigned n = fm_selected(&a->model, NULL), k = 0;
    if (!n) return -ENOENT;
    char **paths = calloc(n, sizeof(*paths));
    if (!paths) return -ENOMEM;
    for (unsigned i = 0; i < a->model.count; i++) if (a->model.entries[i].selected) {
        paths[k] = malloc(FM_PATH);
        if (!paths[k] || fm_join(paths[k], a->model.path, a->model.entries[i].name)) {
            for (unsigned x = 0; x <= k && x < n; x++) free(paths[x]);
            free(paths); return -ENOMEM;
        }
        k++;
    }
    *out = paths; *count = n;
    return 0;
}

static void run_selection_job(struct files_app *a, enum fm_job_kind kind)
{
    char **paths; unsigned n;
    if (selection_paths(a, &paths, &n)) return;
    start_job(a, kind, paths, n, NULL);
    for (unsigned i = 0; i < n; i++) free(paths[i]);
    free(paths);
}

static void confirm_delete(struct files_app *a)
{
    unsigned n = fm_selected(&a->model, NULL);
    if (!n) return;
    char text[FM_NAME + 160];
    const struct fm_entry *e = &a->model.entries[a->model.active < a->model.count && a->model.entries[a->model.active].selected ? a->model.active : 0];
    if (n == 1) for (unsigned i = 0; i < a->model.count; i++) if (a->model.entries[i].selected) e = &a->model.entries[i];
    if (n == 1 && e->meta.type == FM_DIR)
        snprintf(text, sizeof(text), "Are you sure you want to permanently remove the folder\n'%.120s' and all its contents?", e->name);
    else if (n == 1) snprintf(text, sizeof(text), "Are you sure you want to permanently delete '%.120s'?", e->name);
    else snprintf(text, sizeof(text), "Are you sure you want to permanently delete these %u items?", n);
    du_message(a->ui, D_DELETE, n > 1 ? "Confirm Multiple File Delete" : e->meta.type == FM_DIR ? "Confirm Folder Delete" : "Confirm File Delete",
               text, DU_DLG_ICON_WARNING, yes_no, 2, 0, 1);
}

static void properties(struct files_app *a)
{
    if (fm_selected(&a->model, NULL)) { run_selection_job(a, FM_PROPERTIES); return; }
    char *paths[] = {a->model.path};
    start_job(a, FM_PROPERTIES, paths, 1, NULL);
}

static void set_view(struct files_app *a, enum du_view view)
{
    struct du_widget *list = W(a, W_LIST);
    list->view = view; list->scroll = list->hscroll = 0;
    du_list_ensure(list, list->active);
    files_update(a);
}

static void create(struct files_app *a, int folder)
{
    char name[FM_NAME];
    int r = fm_unique(&a->model, folder ? "New Folder" : "New Text Document", folder ? "" : ".txt", name);
    if (!r) r = fm_create(&a->model, name, folder);
    if (r) { error_box(a, folder ? "Error Creating Folder" : "Error Creating File", folder ? "Cannot create the folder:" : "Cannot create the file:", r); return; }
    if (folder) { char path[FM_PATH]; if (!fm_join(path, a->model.path, name)) set_add(a->leaf, &a->leaf_count, path); }
    a->note[0] = 0; a->tree_dirty = 1;
    files_update(a);
    rename_start(a, a->model.active, 1);
    if (folder) checkpoint(a, 2);
}

static void popup_add(struct files_app *a, const char *label, int id, unsigned flags)
{
    if (!a->model.path[0] && (id == C_OPEN_NOTE || id == C_CUT || id == C_COPY || id == C_PASTE ||
        id == C_PASTE_INTO || id == C_DELETE || id == C_RENAME || id == C_PROPERTIES || id == C_NEW_FOLDER || id == C_NEW_TEXT))
        flags |= DU_ITEM_DISABLED;
    if (a->popup_count < sizeof(a->popup) / sizeof(a->popup[0]))
        a->popup[a->popup_count++] = (struct du_menu_item){label, id, 0, 0, flags};
}

static void context(struct files_app *a, const struct du_event *e)
{
    a->popup_count = 0;
    struct fm_model *m = &a->model;
    if (e->id == W_LIST && e->index >= 0 && (unsigned)e->index < m->count) {
        if (!m->entries[e->index].selected) fm_select(m, (unsigned)e->index, 0); else fm_focus(m, (unsigned)e->index);
        const struct fm_entry *it = &m->entries[e->index];
        unsigned n = fm_selected(m, NULL);
        enum fm_kind kind = fm_kind(it);
        popup_add(a, "&Open", C_OPEN, DU_ITEM_DEFAULT);
        if (kind != FM_KIND_FOLDER) popup_add(a, "Open with &CiukNote", C_OPEN_NOTE, 0);
        popup_add(a, NULL, 0, DU_ITEM_SEPARATOR);
        popup_add(a, "Cu&t", C_CUT, 0); popup_add(a, "&Copy", C_COPY, 0);
        if (kind == FM_KIND_FOLDER && n == 1) popup_add(a, "&Paste", C_PASTE_INTO, m->clipboard_count ? 0 : DU_ITEM_DISABLED);
        popup_add(a, NULL, 0, DU_ITEM_SEPARATOR);
        popup_add(a, "&Delete", C_DELETE, 0); popup_add(a, "Rena&me", C_RENAME, n == 1 ? 0 : DU_ITEM_DISABLED);
        popup_add(a, NULL, 0, DU_ITEM_SEPARATOR);
        popup_add(a, "P&roperties", C_PROPERTIES, 0);
        files_update(a);
    } else if (e->id == W_LIST) {
        if (!(e->modifiers & (DU_CTRL | DU_SHIFT))) fm_select_none(m);
        struct du_widget *list = W(a, W_LIST);
        popup_add(a, "Lar&ge Icons", C_ICONS, DU_ITEM_RADIO | (list->view == DU_ICONS ? DU_ITEM_CHECKED : 0));
        popup_add(a, "&List", C_LIST, DU_ITEM_RADIO | (list->view == DU_LIST ? DU_ITEM_CHECKED : 0));
        popup_add(a, "&Details", C_DETAILS, DU_ITEM_RADIO | (list->view == DU_DETAILS ? DU_ITEM_CHECKED : 0));
        popup_add(a, NULL, 0, DU_ITEM_SEPARATOR);
        popup_add(a, "Sort by &Name", C_SORT_NAME, DU_ITEM_RADIO | (m->sort == 0 ? DU_ITEM_CHECKED : 0));
        popup_add(a, "Sort by &Size", C_SORT_SIZE, DU_ITEM_RADIO | (m->sort == 1 ? DU_ITEM_CHECKED : 0));
        popup_add(a, "Sort by &Type", C_SORT_TYPE, DU_ITEM_RADIO | (m->sort == 2 ? DU_ITEM_CHECKED : 0));
        popup_add(a, "Sort by &Modified", C_SORT_MODIFIED, DU_ITEM_RADIO | (m->sort == 3 ? DU_ITEM_CHECKED : 0));
        popup_add(a, NULL, 0, DU_ITEM_SEPARATOR);
        popup_add(a, "Refres&h", C_REFRESH, 0);
        popup_add(a, NULL, 0, DU_ITEM_SEPARATOR);
        popup_add(a, "&Paste", C_PASTE, m->clipboard_count ? 0 : DU_ITEM_DISABLED);
        popup_add(a, NULL, 0, DU_ITEM_SEPARATOR);
        popup_add(a, "New &Folder", C_NEW_FOLDER, 0); popup_add(a, "New &Text Document", C_NEW_TEXT, 0);
        popup_add(a, NULL, 0, DU_ITEM_SEPARATOR);
        popup_add(a, "P&roperties", C_PROPERTIES, 0);
        files_update(a);
    } else if ((e->id == W_TREE || e->id == W_PLACES) && e->index >= 0) {
        const char *path = e->id == W_TREE ? a->tree[e->index].path : a->places[e->index].path;
        if (!fm_same_path(path, a->model.path)) files_navigate(a, path);
        popup_add(a, "&Open", C_TREE_OPEN, DU_ITEM_DEFAULT);
        popup_add(a, NULL, 0, DU_ITEM_SEPARATOR);
        popup_add(a, "P&roperties", C_PROPERTIES, 0);
        fm_select_none(&a->model); files_update(a);
    } else return;
    du_popup(a->ui, a->popup, a->popup_count, e->x, e->y, e->id);
}

static void help(struct files_app *a)
{
    du_message(a->ui, D_INFO, "Files Help",
               "Enter\tOpen the selection\nBackspace\tUp one level\nAlt+Left/Right\tBack and Forward\n"
               "Alt+D, F4\tAddress bar\nF2\tRename\nF5\tRefresh\nDel\tDelete permanently\nAlt+Enter\tProperties\n"
               "Ctrl+C/X/V\tCopy, Cut, Paste\nCtrl+A\tSelect all\n---\nType the first letters of a name to select it.\n"
               "Copies cannot keep dates and attributes in this release.", DU_DLG_ICON_INFO, NULL, 0, 0, 0);
}

static void about(struct files_app *a)
{
    du_message(a->ui, D_INFO, "About Files",
               "CiukiOS Files\nClassic computing, one woof at a time.\n---\nDedicated to Ciuki.\nTango 0.8.90 icons: Public Domain.\n"
               "Ciuki portrait: separate project asset.\nlibdeskui and Files: MIT licence.", DU_DLG_ICON_INFO, NULL, 0, 0, 0);
    if (a->ciuki) du_dialog_icon(a->ui, a->ciuki32);
}

static void open_control(struct files_app *a)
{
    char *argv[] = {"control", "--panel=home", NULL};
    int r = a->spawn ? a->spawn(a, "/Programs/control", argv) : -ENOSYS;
    if (r < 0) error_box(a, "Control Panel", "Cannot open Control Panel:", r);
}

static void command(struct files_app *a, int id)
{
    struct fm_model *m = &a->model;
    m->find_length = 0;
    if (a->rename_index >= 0 && id != C_RENAME) rename_commit(a);
    if (du_dialog_id(a->ui)) return;
    if (!m->path[0] && (id == C_OPEN_NOTE || id == C_CUT || id == C_COPY || id == C_PASTE ||
        id == C_PASTE_INTO || id == C_DELETE || id == C_RENAME || id == C_PROPERTIES || id == C_NEW_FOLDER || id == C_NEW_TEXT)) return;
    switch (id) {
    case C_BACK: after_history(a, fm_history(m, -1)); break;
    case C_FORWARD: after_history(a, fm_history(m, 1)); break;
    case C_UP: after_history(a, fm_up(m)); if (fm_same_path(m->path, "/system")) checkpoint(a, 1); break;
    case C_GO_CONTROL: open_control(a); break;
    case C_GO_ROOT: files_navigate(a, FM_COMPUTER); break;
    case C_GO_BIN: files_navigate(a, "/Programs"); break;
    case C_GO_DOCUMENTS: files_navigate(a, "/Documents"); break;
    case C_GO_GAMES: if (a->place_count == 6) files_navigate(a, "/Games"); break;
    case C_GO_SYSTEM: files_navigate(a, "/system"); break;
    case C_TREE_OPEN: files_update(a); break;
    case C_ADDRESS: { struct du_widget *e = W(a, W_ADDRESS); du_focus(a->ui, W_ADDRESS); e->anchor = 0; e->cursor = strlen(e->text); break; }
    case C_CUT: case C_COPY: if (!fm_clipboard(m, id == C_CUT)) { a->note[0] = 0; files_update(a); } break;
    case C_PASTE: paste(a, m->path); break;
    case C_PASTE_INTO: {
        char p[FM_PATH];
        if (m->active < m->count && m->entries[m->active].meta.type == FM_DIR && !fm_join(p, m->path, m->entries[m->active].name)) paste(a, p);
        break;
    }
    case C_DELETE: confirm_delete(a); break;
    case C_PROPERTIES: properties(a); break;
    case C_VIEWS: { struct du_widget *l = W(a, W_LIST); set_view(a, l->view == DU_ICONS ? DU_LIST : l->view == DU_LIST ? DU_DETAILS : DU_ICONS); break; }
    case C_ICONS: set_view(a, DU_ICONS); break;
    case C_LIST: set_view(a, DU_LIST); break;
    case C_DETAILS: set_view(a, DU_DETAILS); break;
    case C_SORT_NAME: case C_SORT_SIZE: case C_SORT_TYPE: case C_SORT_MODIFIED:
        fm_sort(m, (unsigned)(id - C_SORT_NAME), 0); files_update(a); break;
    case C_REFRESH: {
        int r = fm_refresh(m);
        if (r) error_box(a, "Files", "Cannot refresh the folder:", r);
        places_build(a); files_layout(a); a->tree_dirty = 1; a->note[0] = 0; files_update(a); break;
    }
    case C_OPEN: if (fm_selected(m, NULL)) open_item(a, m->active < m->count && m->entries[m->active].selected ? m->active : 0, 0); break;
    case C_OPEN_NOTE: if (fm_selected(m, NULL)) open_item(a, m->active, 1); break;
    case C_NEW_FOLDER: create(a, 1); break;
    case C_NEW_TEXT: create(a, 0); break;
    case C_RENAME: if (fm_selected(m, NULL) == 1 && m->active < m->count && m->entries[m->active].selected) rename_start(a, m->active, 0); break;
    case C_SELECT_ALL: fm_select_all(m, 0); a->note[0] = 0; files_update(a); du_focus(a->ui, W_LIST); break;
    case C_INVERT: fm_select_all(m, 1); a->note[0] = 0; files_update(a); break;
    case C_CLOSE: if (files_close_request(a) && a->close) a->close(a); break;   /* a job is modal: idle here */
    case C_HELP: help(a); break;
    case C_ABOUT: about(a); break;
    default: break;
    }
}

static void dialog_result(struct files_app *a, const struct du_event *e)
{
    switch (e->id) {
    case D_DELETE: if (e->index == 0) run_selection_job(a, FM_DELETE); break;
    case D_REPLACE:
        fm_job_answer(&a->job, e->index == 0 ? FM_YES : e->index == 1 ? FM_YES_ALL : e->index == 2 ? FM_NO : FM_CANCEL);
        if (a->job_active) progress(a);
        break;
    case D_PROGRESS: fm_job_cancel(&a->job); if (a->job_active) progress(a); break;
    default: break;
    }
}

/* The shell terminates a client two seconds after its close request, so a
 * confirmation could not be answered: cancel the job (its cleanup removes
 * the unfinished copy, never a source) and close once it has stopped. */
int files_close_request(struct files_app *a)
{
    if (!a->job_active) return 1;
    a->close_pending = 1;
    fm_job_cancel(&a->job);
    return 0;
}

static void list_select(struct files_app *a, const struct du_event *e)
{
    struct fm_model *m = &a->model;
    a->note[0] = 0;
    if (e->index < 0) {
        if (!(e->modifiers & (DU_CTRL | DU_SHIFT))) fm_select_none(m);
        files_update(a); return;
    }
    unsigned i = (unsigned)e->index, shift = (e->modifiers & DU_SHIFT) != 0, ctrl = (e->modifiers & DU_CTRL) != 0;
    if (e->keyboard) {
        m->find_length = 0;
        if (e->key == DU_KEY_SPACE && ctrl) fm_select(m, i, 2);
        else if (shift) fm_select(m, i, ctrl ? 3 : 1);
        else if (ctrl) fm_focus(m, i);
        else fm_select(m, i, 0);
    } else fm_select(m, i, (shift ? 1u : 0u) | (ctrl ? 2u : 0u));
    files_update(a);
}

static void key(struct files_app *a, const struct du_event *e)
{
    int focus = du_focused(a->ui);
    if (e->key == DU_KEY_F4 && !(e->modifiers & (DU_CTRL | DU_ALT | DU_SHIFT))) { command(a, C_ADDRESS); return; }
    if (focus != W_LIST && focus != W_TREE && focus != W_PLACES) return;
    if (e->key == DU_KEY_BACKSPACE && !(e->modifiers & (DU_CTRL | DU_ALT))) { command(a, C_UP); return; }
    if (focus == W_LIST && e->ch > 32 && e->ch < 127) {
        uint64_t now = a->ui->now;
        if (fm_find(&a->model, e->ch, now) >= 0) {
            a->note[0] = 0;
            struct du_widget *list = W(a, W_LIST);
            list->active = a->model.active; du_list_ensure(list, list->active);
            files_update(a);
        }
        return;
    }
    if (!e->ch) a->model.find_length = 0;
}

static void toggle_node(struct files_app *a, int index)
{
    if (index < 0 || (unsigned)index >= a->tree_count) return;
    struct files_node *n = &a->tree[index];
    if (n->expanded) {
        set_remove(a->expanded, &a->expanded_count, n->path); set_add(a->collapsed, &a->collapsed_count, n->path);
        if (fm_beneath(a->model.path, n->path) && !fm_same_path(a->model.path, n->path)) { char p[FM_PATH]; strcpy(p, n->path); files_navigate(a, p); }
    } else { set_remove(a->collapsed, &a->collapsed_count, n->path); set_add(a->expanded, &a->expanded_count, n->path); }
    a->tree_dirty = 1;
    files_update(a);
}

void files_event(struct du_ui *u, const struct du_event *e, void *arg)
{
    struct files_app *a = arg;
    (void)u;
    if (a->rename_index >= 0 && e->id != W_RENAME &&
        (e->type == DU_EV_SELECT || e->type == DU_EV_OPEN || e->type == DU_EV_SORT || e->type == DU_EV_CONTEXT))
        rename_commit(a);
    if (du_dialog_id(a->ui) && e->type != DU_EV_TICK && e->type != DU_EV_DIALOG && e->type != DU_EV_FOCUS) return;
    switch (e->type) {
    case DU_EV_TICK: if (e->id == T_JOB) job_tick(a); break;
    case DU_EV_DIALOG: dialog_result(a, e); break;
    case DU_EV_COMMIT:
        if (e->id == W_RENAME) rename_commit(a);
        else if (e->id == W_ADDRESS) { char p[FM_PATH]; snprintf(p, sizeof(p), "%s", W(a, W_ADDRESS)->text); du_focus(a->ui, W_LIST); files_navigate(a, p); }
        break;
    case DU_EV_CANCEL:
        if (e->id == W_RENAME) { rename_hide(a); files_update(a); }
        else if (e->id == W_ADDRESS) { du_focus(a->ui, W_LIST); files_update(a); }
        break;
    case DU_EV_CHANGE:
        if (e->id == W_SPLIT) { a->split = e->index; files_layout(a); files_update(a); }
        else if (e->id == W_TREE) toggle_node(a, e->index);
        else if (e->id == C_VIEWS && e->index == 1) {
            struct du_widget *b = W(a, C_VIEWS), *list = W(a, W_LIST);
            a->popup_count = 0;
            popup_add(a, "Lar&ge Icons", C_ICONS, DU_ITEM_RADIO | (list->view == DU_ICONS ? DU_ITEM_CHECKED : 0));
            popup_add(a, "&List", C_LIST, DU_ITEM_RADIO | (list->view == DU_LIST ? DU_ITEM_CHECKED : 0));
            popup_add(a, "&Details", C_DETAILS, DU_ITEM_RADIO | (list->view == DU_DETAILS ? DU_ITEM_CHECKED : 0));
            du_popup(a->ui, a->popup, a->popup_count, b->box.x, b->box.y + b->box.h, C_VIEWS);
        }
        break;
    case DU_EV_SELECT:
        if (e->id == W_LIST) list_select(a, e);
        else if (e->id == W_TREE && e->index >= 0 && (unsigned)e->index < a->tree_count) {
            if (!fm_same_path(a->tree[e->index].path, a->model.path)) { char p[FM_PATH]; strcpy(p, a->tree[e->index].path); files_navigate(a, p); }
            du_focus(a->ui, W_TREE);
        } else if (e->id == W_PLACES && e->index >= 0 && (unsigned)e->index < a->place_count) {
            if (!strcmp(a->places[e->index].path, "control:")) open_control(a);
            else if (!fm_same_path(a->places[e->index].path, a->model.path)) files_navigate(a, a->places[e->index].path);
        }
        break;
    case DU_EV_OPEN:
        if (e->id == W_LIST && e->index >= 0) open_item(a, (unsigned)e->index, 0);
        else if (e->id == W_TREE) toggle_node(a, e->index);
        break;
    case DU_EV_SORT: {
        unsigned column = (unsigned)e->index;
        fm_sort(&a->model, column, a->model.sort == column ? !a->model.descending : 0);
        files_update(a);
        break;
    }
    case DU_EV_CONTEXT: context(a, e); break;
    case DU_EV_ACTIVATE: command(a, e->id); break;
    case DU_EV_KEY: key(a, e); break;
    default: break;
    }
}

static void copy_menu(struct du_menu_item *dst, const struct du_menu_item *src, unsigned n) { memcpy(dst, src, n * sizeof(*src)); }

int files_setup(struct files_app *a, struct du_ui *u, const struct fm_vfs *fs, const char *path)
{
    a->ui = u; u->event = files_event; u->user = a; u->layout = resize_layout;
    a->width = u->canvas.width; a->height = u->canvas.height;
    a->rename_index = -1; a->split = a->width >= 700 ? 180 : 150;
    fm_init(&a->model, fs);
    a->tree = calloc(FILES_TREE, sizeof(*a->tree));
    a->tree_items = calloc(FILES_TREE, sizeof(*a->tree_items));
    if (!a->tree || !a->tree_items) return -ENOMEM;
    copy_menu(a->file_items, file_template, 11); copy_menu(a->edit_items, edit_template, 6);
    copy_menu(a->view_items, view_template, 10); copy_menu(a->go_items, go_template, 12); copy_menu(a->help_items, help_template, 3);
    a->menus[0] = (struct du_menu){"&File", a->file_items, 11};
    a->menus[1] = (struct du_menu){"&Edit", a->edit_items, 6};
    a->menus[2] = (struct du_menu){"&View", a->view_items, 10};
    a->menus[3] = (struct du_menu){"&Go", a->go_items, 12};
    a->menus[4] = (struct du_menu){"&Help", a->help_items, 3};
    u->menus = a->menus; u->menu_count = 5;
    struct du_rect zero = {0, 0, 0, 0};
    du_add(u, DU_SEPARATOR, W_BAND1, zero, "");
    for (unsigned i = 0; i < sizeof(toolbar) / sizeof(toolbar[0]); i++) {
        struct du_widget *b = du_add(u, DU_TOOLBUTTON, toolbar[i].id, zero, toolbar[i].label);
        b->icon = named(toolbar[i].icon, 22); b->icon_size = 22;
        if (toolbar[i].id == C_VIEWS) b->dropdown = 12;
    }
    du_add(u, DU_SEPARATOR, W_TSEP1, zero, ""); du_add(u, DU_SEPARATOR, W_TSEP2, zero, ""); du_add(u, DU_SEPARATOR, W_TSEP3, zero, "");
    du_add(u, DU_SEPARATOR, W_BAND2, zero, "");
    du_add(u, DU_LABEL, W_ADDRESS_LABEL, zero, "A&ddress");
    du_add(u, DU_EDIT, W_ADDRESS, zero, "Computer")->icon = named("place:computer", 16);
    du_add(u, DU_SEPARATOR, W_BAND3, zero, "");
    du_add(u, DU_HEADER, W_PLACES_HEADER, zero, "Places");
    struct du_widget *places = du_add(u, DU_TREE, W_PLACES, zero, "");
    places->focus_rect = 1;
    du_add(u, DU_HEADER, W_FOLDERS_HEADER, zero, "Folders");
    du_add(u, DU_TREE, W_TREE, zero, "");
    du_add(u, DU_SPLITTER, W_SPLIT, zero, "");
    struct du_widget *list = du_add(u, DU_LISTVIEW, W_LIST, zero, "");
    list->view = DU_DETAILS; list->inactive_selection = 1;
    du_add(u, DU_STATUS, W_STATUS, zero, "");
    struct du_widget *rename = du_add(u, DU_EDIT, W_RENAME, zero, "");
    rename->hidden = 1; rename->def = 1;
    if (a->portrait) {
        uint32_t *pixels = malloc(256 * 256 * 4);
        if (pixels && !a->portrait(pixels)) { shrink(pixels, a->ciuki16, 16); shrink(pixels, a->ciuki32, 32); a->ciuki = 1; }
        free(pixels);
    }
    places_build(a);
    files_layout(a);
    du_focus(u, W_LIST);
    if (files_navigate(a, path ? path : FM_COMPUTER)) {
        du_dialog_close(u);
        if (files_navigate(a, FM_COMPUTER)) return -EIO;
    }
    return 0;
}

void files_destroy(struct files_app *a)
{
    if (a->job_active) { fm_job_cancel(&a->job); fm_job_step(&a->job); }
    fm_job_destroy(&a->job);
    fm_destroy(&a->model);
    free(a->rows); free(a->texts); free(a->tree); free(a->tree_items);
    a->rows = NULL; a->texts = NULL; a->tree = NULL; a->tree_items = NULL;
}
