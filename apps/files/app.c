#include "../desktop/media_notice.h"
/* SPDX-License-Identifier: MIT
 * Files controller: layout, menus, panes, selection, operations and jobs. */
#include "app.h"
#include "transfer.h"
#include "transfer_ui.h"
#include "../desktop/catalog.h"
#include "../desktop/sounds.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include "../desktop/tz_env.h"

enum {
    C_BACK = 1, C_FORWARD, C_UP, C_CUT, C_COPY, C_PASTE, C_DELETE, C_PROPERTIES, C_VIEWS,
    C_OPEN, C_OPEN_NOTE, C_NEW_FOLDER, C_NEW_TEXT, C_RENAME, C_CLOSE, C_SELECT_ALL, C_INVERT,
    C_ICONS, C_LIST, C_DETAILS, C_SORT_NAME, C_SORT_SIZE, C_SORT_TYPE, C_SORT_MODIFIED, C_REFRESH,
    C_ADDRESS, C_GO_ROOT, C_GO_BIN, C_GO_SYSTEM, C_HELP, C_ABOUT, C_PASTE_INTO, C_TREE_OPEN, C_SAFE_REMOVE,
    W_ADDRESS = 100, W_ADDRESS_LABEL, W_PLACES_HEADER, W_PLACES, W_FOLDERS_HEADER, W_TREE, W_SPLIT,
    W_LIST, W_STATUS, W_RENAME, W_BAND1, W_BAND2, W_BAND3, W_TSEP1, W_TSEP2, W_TSEP3,
    D_DELETE = 200, D_REPLACE, D_ERROR, D_PROPERTIES, D_INFO, D_PROGRESS, D_CANCEL_JOB, D_BIN_PURGE, D_BIN_EMPTY, D_BIN_PROPS, D_BIN_ITEM, D_INSERT,
    C_GO_DOCUMENTS = 300, C_GO_GAMES, C_GO_CONTROL, C_GO_DESKTOP, C_MUSIC, C_GO_RECYCLE, C_RESTORE, C_EMPTY_BIN, C_BIN_PROPS, C_DELETE_NOW,
    T_JOB = 1, T_MEDIA, T_BROWSER, T_DRAG_REFRESH
};
#define PANE_TOP 92
#define STATUS_H 20

enum scan_kind { SCAN_NAVIGATE, SCAN_REFRESH, SCAN_HISTORY, SCAN_TREE, SCAN_MEDIA, SCAN_OPEN };
struct files_scan {
    struct files_app state; /* Owned data only; never a UI or live model. */
    struct fm_vfs source;
    struct files_worker *worker;
    unsigned revision;
    enum scan_kind kind;
    char target[FM_PATH];
    int delta, error, changed, labels_changed, metadata_only, clipboard_valid;
    uint64_t clipboard_key;
    uint16_t index[FM_ENTRIES*2];
    uint8_t seen[FM_ENTRIES], dirty[FM_ENTRIES];
    unsigned listed;
};
struct files_worker {
    struct du_ui *ui;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    pthread_t thread;
    struct files_scan *pending, *result;
    unsigned revision;
    int stopping, busy;
};
static int scan_submit(struct files_app *, enum scan_kind, const char *, int);
static void scan_poll(struct files_app *);
static void scan_stop(struct files_app *);
static void open_item(struct files_app *, unsigned, int);
static void bin_item_properties(struct files_app *, unsigned);
static void drop_free(struct files_app *);
static void bin_properties(struct files_app *);
static void command(struct files_app *, int);
static int scan_cancelled(const struct files_scan *);

static const struct du_menu_item file_template[] = {
    {"&Open", C_OPEN, 0, 0, DU_ITEM_DEFAULT}, {"Open with &CiukNote", C_OPEN_NOTE, 0, 0, 0},
    {NULL, 0, 0, 0, DU_ITEM_SEPARATOR},
    {"New &Folder", C_NEW_FOLDER, 0, 0, 0}, {"New &Text Document", C_NEW_TEXT, 0, 0, 0},
    {NULL, 0, 0, 0, DU_ITEM_SEPARATOR},
    {"&Delete", C_DELETE, DU_KEY_DELETE, 0, 0}, {"Rena&me", C_RENAME, DU_KEY_F2, 0, 0},
    {"P&roperties", C_PROPERTIES, DU_KEY_ENTER, DU_ALT, 0},
    {"Safely &remove", C_SAFE_REMOVE, 0, 0, DU_ITEM_DISABLED}, {"&Close", C_CLOSE, DU_KEY_F4, DU_ALT, 0}};
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
    {"&Documents", C_GO_DOCUMENTS, 0, 0, 0}, {"Des&ktop", C_GO_DESKTOP, 0, 0, 0}, {"&System", C_GO_SYSTEM, 0, 0, 0}, {"&Games", C_GO_GAMES, 0, 0, 0}, {"Control &Panel", C_GO_CONTROL, 0, 0, 0},
    {"&Recycle Bin", C_GO_RECYCLE, 0, 0, 0}};
/* G2: the File menu while the Recycle Bin is shown. */
static const struct du_menu_item bin_template[] = {
    {"R&estore", C_RESTORE, 0, 0, DU_ITEM_DEFAULT}, {"&Empty Recycle Bin", C_EMPTY_BIN, 0, 0, 0},
    {NULL, 0, 0, 0, DU_ITEM_SEPARATOR},
    {"&Delete", C_DELETE, DU_KEY_DELETE, 0, 0}, {"P&roperties", C_PROPERTIES, DU_KEY_ENTER, DU_ALT, 0},
    {NULL, 0, 0, 0, DU_ITEM_SEPARATOR},
    {"&Close", C_CLOSE, DU_KEY_F4, DU_ALT, 0}};
#define BIN_ITEMS 7
static const struct du_menu_item help_template[] = {
    {"&Help Topics", C_HELP, DU_KEY_F1, 0, 0}, {NULL, 0, 0, 0, DU_ITEM_SEPARATOR}, {"&About Files", C_ABOUT, 0, 0, 0}};

static const char *const yes_no[] = {"&Yes", "&No"};

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
    if (when <= 0 || !desk_localtime(&t, &tm)) { snprintf(out, n, "%s", ""); return; }   /* G1: local */
    snprintf(out, n, "%04d-%02d-%02d %02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
}

static const char *reason(int error) { return fm_error_reason(error); }

static void error_box(struct files_app *a, const char *title, const char *what, int error)
{
    char text[DU_DIALOG_TEXT];
    snprintf(text, sizeof(text), "%s\n\n%s", what, reason(error));
    du_message(a->ui, D_ERROR, title, text, DU_DLG_ICON_ERROR, NULL, 0, 0, 0);
}

/* fx-17b: Windows 98 asks for the diskette instead of failing the open. */
static void insert_disk_prompt(struct files_app *a, int drive, const char *path)
{
    static const char *const buttons[] = {"&Retry", "Cancel"};
    char text[160];
    snprintf(a->insert_path, sizeof(a->insert_path), "%s", path); a->insert_drive = drive;
    snprintf(text, sizeof(text), "Please insert a disk into drive %c:", 'A' + drive);
    du_message(a->ui, D_INSERT, "Files", text, DU_DLG_ICON_WARNING, buttons, 2, 0, 1);
}

/* fx-17b: A:/B: drives the controller found, listed with or without a disk. */
static int floppy_drive(const struct files_app *a, int d)
{ return d>=0 && d<2 && a->media.type[d]==CIUKI_MEDIA_FLOPPY; }
static uint32_t floppy_slots(const struct files_app *a)
{ return (floppy_drive(a,0) ? 1u : 0u) | (floppy_drive(a,1) ? 2u : 0u); }
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
    list->file_drag=1; list->marquee=1;
    W(a, W_TREE)->file_drag=1;
    list->box = (struct du_rect){a->split + 4, PANE_TOP, w - a->split - 4, bottom - PANE_TOP};
    if (fm_is_recycle(a->model.path)) {
        /* G2: Windows 98's Recycle Bin columns. */
        int bin_name = list->box.w - 4 - 16 - 120 - 98 - 84 - 58 - 98;
        if (bin_name < 120) bin_name = 120;
        list->columns[0] = (struct du_column){"Name", bin_name, 0};
        list->columns[1] = (struct du_column){"Original Location", 120, 0};
        list->columns[2] = (struct du_column){"Date Deleted", 98, 0};
        list->columns[3] = (struct du_column){"Type", 84, 0};
        list->columns[4] = (struct du_column){"Size", 58, 1};
        list->columns[5] = (struct du_column){"Modified", 98, 0};
        list->column_count = 6;
    } else {
        int name = list->box.w - 4 - 16 - 70 - 132 - 108;
        if (name < 120) name = 120;
        list->columns[0] = (struct du_column){"Name", name, 0};
        list->columns[1] = (struct du_column){"Size", 70, 1};
        list->columns[2] = (struct du_column){"Type", 132, 0};
        list->columns[3] = (struct du_column){"Modified", 108, 0};
        list->column_count = 4;
    }
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
    static const char *const names[] = {"Computer", "Programs", "Documents", "System", "Games", "Control Panel", "Desktop"};
    static const char *const paths[] = {FM_COMPUTER, "/Programs", "/Documents", "/system", "/Games", "control:", "/Documents/Desktop"};
    static const char *const icons[] = {"place:computer", "place:applications", "place:documents", "place:system", "place:folder", "menu:control-panel", "place:desktop"};
    a->place_count = 0;
    for (unsigned i = 0; i < 7; i++) {
        if (i == 4 && !a->games_available) continue;
        struct files_place *p = &a->places[a->place_count++];
        snprintf(p->name, sizeof(p->name), "%s", names[i]); snprintf(p->path, sizeof(p->path), "%s", paths[i]);
    }
    for (unsigned i = 0; i < a->place_count; i++) {
        struct du_item *it = &a->place_items[i];
        memset(it, 0, sizeof(*it));
        it->text[0] = a->places[i].name;
        it->icon16 = i == 0 && a->ciuki ? a->ciuki16 : named(!strcmp(a->places[i].path, "control:") ? "menu:control-panel" : !strcmp(a->places[i].path,"/Documents/Desktop") ? "place:desktop" : !strcmp(a->places[i].path,"/Games") ? "place:folder" : icons[i], 16);
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
    int error = 0;
    if (a->reading && fm_same_path(p->path, a->model.path)) {
        /* Reuse the browser's directory snapshot instead of reading a slow
         * current directory a second time for the tree. */
        for (unsigned i = 0; i < a->model.count && !error; i++)
            error = collect_dir(a->model.entries[i].name, &a->model.entries[i].meta, &n);
    } else error = a->model.fs.list(a->model.fs.ctx, p->path, FM_LIST_NAMES_ONLY, collect_dir, &n);
    if (error) { free(n.v); p->children = 0; return; }
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
    if (root->expanded) fm_volumes_slots(&a->model.fs, a->model.drive_slots, tree_drive, a);
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
                     named(n->depth == 1 ? (n->name[0]>='D' && n->name[0]<='Z' && a->media.type[n->name[0]-'A']==CIUKI_MEDIA_OPTICAL ? "place:optical" :
                                            floppy_drive(a,n->name[0]-'A') ? "place:floppy" : "place:volume") : here ? "type:folder-open" : "type:folder", 16);
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

static int media_drive(const char *path)
{
    if (!path || strncmp(path,"/mnt/",5) || path[5]<'a' || path[5]>'z' || path[5]=='c' || (path[6] && path[6]!='/')) return -1;
    return path[5]-'a';
}
/* Content reads are restricted to arrival/generation or label-source changes.
 * Failures keep the previous successful cache and are retried by media ticks. */
static int labels_refresh(struct files_app *a, const struct files_scan *s)
{
    int changed=0;
    for (unsigned d=3;d<26;d++) {
        if (scan_cancelled(s)) break;
        struct files_drive_label *label=&a->drive_labels[d];
        if (!(a->media.removable_mask & (1u<<d))) {
            changed|=label->valid; memset(label,0,sizeof(*label)); continue;
        }
        struct files_label_key key={0};
        if (a->media_label_key && a->media_label_key(d,&key)) continue;
        int same=label->valid && label->generation==a->media.generation[d] &&
            !strcmp(label->key.volume,key.volume) && label->key.size==key.size &&
            label->key.ino==key.ino && label->key.modified==key.modified && label->key.inf_status==key.inf_status;
        if (same || !a->media_label) continue;
        char text[33]={0};
        if (a->media_label(d,text)) continue;
        text[32]=0;
        memcpy(label->text,text,sizeof(text)); label->key=key;
        label->generation=a->media.generation[d]; label->valid=1; changed=1;
    }
    return changed;
}
static const char *drive_label(const struct files_app *a,int drive)
{
    if (drive<3 || drive>=26 || !(a->media.removable_mask & (1u<<drive)) ||
        !a->drive_labels[drive].valid || a->drive_labels[drive].generation!=a->media.generation[drive]) return "";
    return a->drive_labels[drive].text;
}
static const char *removal_label(struct files_app *a,int drive)
{
    if (drive<3) return "Safely &remove";
    const char *name=drive_label(a,drive);
    snprintf(a->removal_label,sizeof(a->removal_label),"%s %.32s%s%c:",
        drive>=3 && a->media.type[drive]==CIUKI_MEDIA_OPTICAL ? "&Eject disc" : "Safely &remove",
        name,name[0] ? " - " : "",drive>=3 ? 'A'+drive : '?');
    return a->removal_label;
}
static int removal_target(struct files_app *a)
{
    char path[FM_PATH];
    const char *p=a->model.path;
    if (!*p) {
        if (fm_selected(&a->model,NULL)!=1 || a->model.active>=a->model.count ||
            !a->model.entries[a->model.active].selected || fm_entry_path(&a->model,a->model.active,path)) return -1;
        p=path;
    }
    int d=media_drive(p);
    return d>=3 && a->media_remove && (a->media.removable_mask & (1u<<d)) ? d : -1;
}
/* G2: the drives whose volumes are fixed disks (they keep a Recycle Bin);
 * only known once the media state has been read. */
static uint32_t files_fixed(const struct files_app *a)
{ return a->media.version ? ~a->media.removable_mask & 0x03fffff8u : 0; }
static const int bin_header_of_key[6] = {0, 4, 3, 5, 1, 2};   /* sort key -> Details column */
static const unsigned bin_key_of_header[6] = {0, 4, 5, 2, 1, 3};
void files_update(struct files_app *a)
{
    struct fm_model *m = &a->model;
    int computer = !(a->loading ? a->loading_path : m->path)[0];
    m->fixed = files_fixed(a);
    int bin = !a->loading && fm_is_recycle(m->path);
    char display[FM_PATH];
    if (fm_display_path(display, a->loading ? a->loading_path : m->path)) strcpy(display, "(path too long)");
    if (m->count > a->capacity) {
        unsigned cap = m->count + 32;
        struct du_item *rows = realloc(a->rows, cap * sizeof(*rows));
        if (rows) a->rows = rows;
        struct files_row *texts = realloc(a->texts, cap * sizeof(*texts));
        if (texts) a->texts = texts;
        if (rows && texts) a->capacity = cap;
    }
    if (bin && m->count > a->bin_row_capacity) {
        unsigned cap = m->count + 32;
        struct files_bin_row *bins = realloc(a->bin_rows, cap * sizeof(*bins));
        if (bins) { a->bin_rows = bins; a->bin_row_capacity = cap; }
    }
    if (bin && !a->bin_rows) bin = 0;
    unsigned count = a->loading ? 0 : m->count < a->capacity ? m->count : a->capacity;
    uint64_t folder_bytes = 0;
    for (unsigned i = 0; i < count; i++) {
        struct fm_entry *e = &m->entries[i];
        struct files_row *t = &a->texts[i];
        if (e->meta.type == FM_DIR) t->size[0] = 0; else { kb_text(t->size, sizeof(t->size), e->meta.size); folder_bytes += e->meta.size; }
        int drive=computer && e->name[0]>='A' && e->name[0]<='Z' && e->name[0]!='C' ? e->name[0]-'A' : -1;
        int floppy=floppy_drive(a,drive);
        int removable=drive>=3 && (a->media.removable_mask & (1u<<drive));
        int optical=removable && a->media.type[drive]==CIUKI_MEDIA_OPTICAL;
        const char *drive_icon=floppy ? "place:floppy" : optical ? "place:optical" : removable ? "place:removable" : "place:volume";
        char buffer[40]; snprintf(t->type, sizeof(t->type), "%s", computer ? (floppy ? "3.5-Inch Floppy Disk" : optical ? "CD-ROM Drive" : removable ? "Removable Drive" : "Disk Drive") : fm_type_name(e, buffer, sizeof(buffer)));
        time_text(t->modified, sizeof(t->modified), computer ? 0 : e->meta.modified);
        const char *volume_label=drive_label(a,drive);
        if (volume_label[0]) snprintf(t->label,sizeof(t->label),"%s (%s)",volume_label,e->name);
        if (bin && e->bin && i < a->bin_row_capacity) {
            /* G2: the original name, location and deletion time; sizes also for folders. */
            struct files_bin_row *br = &a->bin_rows[i]; const struct rb_item *item = rb_entry_item(m, e);
            rb_item_name(item, br->name); rb_item_location(item, br->where);
            if (!br->where[0]) snprintf(br->where, sizeof(br->where), "(unknown)");
            time_text(br->deleted, sizeof(br->deleted), item->deleted);
            kb_text(t->size, sizeof(t->size), e->meta.size);
            a->rows[i] = (struct du_item){{br->name, br->where, br->deleted, t->type, t->size, t->modified},
                                          icon16(e), icon32(e), (uint8_t)e->selected, 0, 0, 0, 0};
            continue;
        }
        a->rows[i] = (struct du_item){{volume_label[0] ? t->label : e->name, t->size, t->type, t->modified},
                                      computer ? named(drive_icon, 16) : icon16(e), computer ? named(drive_icon, 32) : icon32(e),
                                      (uint8_t)e->selected, (uint8_t)in_clipboard(a, e->name), 0, 0, 0};
    }
    struct du_widget *list = W(a, W_LIST);
    list->items = a->rows; list->count = count;
    list->active = m->active < count ? m->active : 0;
    list->sort = bin ? bin_header_of_key[m->sort < 6 ? m->sort : 0] : (int)m->sort; list->descending = m->descending;
    du_set_text(list, a->loading ? "Loading..." : bin ? "The Recycle Bin is empty." : "This folder is empty.");
    struct du_widget *address = W(a, W_ADDRESS);
    if (du_focused(a->ui) != W_ADDRESS) du_set_text(address, display);
    address->icon = computer ? (a->ciuki ? a->ciuki16 : named("place:computer", 16)) : bin ? named(count ? "place:trash-full" : "place:trash", 16) : named("type:folder", 16);
    uint64_t bytes = 0; unsigned selected = a->loading ? 0 : fm_selected(m, &bytes), selected_files = 0;
    for (unsigned i = 0; i < count; i++) selected_files += m->entries[i].selected && m->entries[i].meta.type == FM_FILE;
    char size[48] = "", left[200];
    /* Only the selection's size: a folder total would tie the status bar
     * (and the test digests) to unrelated file sizes such as the kernel's. */
    if (selected_files) files_size_text(size, sizeof(size), bytes);
    (void)folder_bytes;
    if (a->loading) snprintf(left, sizeof(left), "Loading...");
    else if (a->note[0]) snprintf(left, sizeof(left), "%s", a->note);
    else if (selected == 1 && computer && m->active < count && m->entries[m->active].selected && a->media_space) {
        /* fx-17b: Windows 98 My Computer shows the selected drive's free
         * space and capacity; "calculating..." during a background count. */
        const char *name = m->entries[m->active].name; struct ciuki_media_space space;
        int d = name[0] >= 'A' && name[0] <= 'Z' && name[1] == ':' ? name[0] - 'A' : -1;
        if (d >= 0 && !a->media_space((unsigned)d, &space)) {
            char cap[48], free_text[48];
            uint64_t cluster = space.cluster_bytes;
            files_size_text(cap, sizeof(cap), (uint64_t)space.total_clusters * cluster);
            if (space.flags & CIUKI_MEDIA_SPACE_COUNTING) snprintf(free_text, sizeof(free_text), "calculating...");
            else files_size_text(free_text, sizeof(free_text), (uint64_t)space.free_clusters * cluster);
            snprintf(left, sizeof(left), "Free Space: %s, Capacity: %s", free_text, cap);
        } else snprintf(left, sizeof(left), "%u object(s) selected", selected);
    }
    else if (selected) snprintf(left, sizeof(left), "%u object(s) selected", selected);
    else snprintf(left, sizeof(left), "%u object(s)", count);
    snprintf(W(a, W_STATUS)->text, DU_TEXT_MAX, "%s\t%s\t%.*s", left, size, DU_TEXT_MAX - 252, display);
    int root = computer;
    W(a, C_BACK)->disabled = !m->history_at;
    W(a, C_FORWARD)->disabled = m->history_at + 1 >= m->history_count;
    W(a, C_UP)->disabled = (uint8_t)root;
    W(a, C_CUT)->disabled = W(a, C_COPY)->disabled = computer || bin || !selected;
    W(a, C_DELETE)->disabled = computer || !selected;
    W(a, C_PASTE)->disabled = computer || bin || !m->clipboard_count;
    W(a, C_PROPERTIES)->disabled = (uint8_t)(a->loading || computer);
    /* G2: the File menu is the Recycle Bin's while it is shown. */
    if (bin != a->bin_menu) {
        a->bin_menu = bin;
        a->menus[0].items = bin ? a->bin_items : a->file_items; a->menus[0].count = bin ? BIN_ITEMS : 11;
    }
    if (bin) {
        set_flag(a->bin_items, BIN_ITEMS, C_RESTORE, DU_ITEM_DISABLED, !selected);
        set_flag(a->bin_items, BIN_ITEMS, C_EMPTY_BIN, DU_ITEM_DISABLED, !count);
        set_flag(a->bin_items, BIN_ITEMS, C_DELETE, DU_ITEM_DISABLED, !selected);
    }
    int target=removal_target(a);
    set_flag(a->file_items, 11, C_SAFE_REMOVE, DU_ITEM_DISABLED, target<3);
    for (unsigned i=0;i<11;i++) if (a->file_items[i].id==C_SAFE_REMOVE)
        a->file_items[i].label=removal_label(a,target);
    set_flag(a->file_items, 11, C_OPEN, DU_ITEM_DISABLED, !selected);
    set_flag(a->file_items, 11, C_OPEN_NOTE, DU_ITEM_DISABLED, computer || !selected);
    set_flag(a->file_items, 11, C_DELETE, DU_ITEM_DISABLED, computer || !selected);
    set_flag(a->file_items, 11, C_RENAME, DU_ITEM_DISABLED, computer || selected != 1);
    set_flag(a->file_items, 11, C_NEW_FOLDER, DU_ITEM_DISABLED, a->loading || computer);
    set_flag(a->file_items, 11, C_NEW_TEXT, DU_ITEM_DISABLED, a->loading || computer);
    set_flag(a->file_items, 11, C_PROPERTIES, DU_ITEM_DISABLED, a->loading || computer);
    set_flag(a->edit_items, 6, C_CUT, DU_ITEM_DISABLED, computer || bin || !selected);
    set_flag(a->edit_items, 6, C_COPY, DU_ITEM_DISABLED, computer || bin || !selected);
    set_flag(a->edit_items, 6, C_PASTE, DU_ITEM_DISABLED, computer || bin || !m->clipboard_count);
    set_flag(a->go_items, 14, C_BACK, DU_ITEM_DISABLED, !m->history_at);
    set_flag(a->go_items, 14, C_FORWARD, DU_ITEM_DISABLED, m->history_at + 1 >= m->history_count);
    set_flag(a->go_items, 14, C_UP, DU_ITEM_DISABLED, root);
    set_flag(a->go_items, 14, C_GO_GAMES, DU_ITEM_DISABLED, !a->games_available);
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
    if (a->tree_dirty && !a->loading && a->worker && !a->job_active)
        scan_submit(a, SCAN_TREE, NULL, 0);
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
/* The mailbox lock protects pointer ownership only. All storage calls run
 * outside it, and cancelled scans release their directory stream via list(). */
static int scan_cancelled(const struct files_scan *s)
{
    return s->revision != __atomic_load_n(&s->worker->revision, __ATOMIC_ACQUIRE);
}
struct scan_emit { struct files_scan *scan; fm_emit emit; void *arg; };
static int scan_emit(const char *name, const struct fm_meta *meta, void *arg)
{
    struct scan_emit *e = arg;
    return scan_cancelled(e->scan) ? -ECANCELED : e->emit(name, meta, e->arg);
}
static int scan_list(void *ctx, const char *path, unsigned flags, fm_emit emit, void *arg)
{
    struct files_scan *s = ctx;
    if (scan_cancelled(s)) return -ECANCELED;
    struct scan_emit e = {s, emit, arg};
    int r = s->source.list(s->source.ctx, path, flags, scan_emit, &e);
    return scan_cancelled(s) ? -ECANCELED : r;
}
static int scan_stat(void *ctx, const char *path, struct fm_meta *meta)
{
    struct files_scan *s = ctx;
    if (scan_cancelled(s)) return -ECANCELED;
    int r = s->source.stat_path(s->source.ctx, path, meta);
    return scan_cancelled(s) ? -ECANCELED : r;
}
/* G2: the Recycle Bin reads INFO2 and may take the legacy Trash in, so the
 * worker's VFS also forwards file I/O; only reads observe cancellation. */
static int scan_open(void *ctx, const char *path, int make)
{ struct files_scan *s = ctx; if (!make && scan_cancelled(s)) return -ECANCELED; return s->source.open(s->source.ctx, path, make); }
static int scan_read(void *ctx, int fd, void *buffer, unsigned n)
{ struct files_scan *s = ctx; return s->source.read(s->source.ctx, fd, buffer, n); }
static int scan_write(void *ctx, int fd, const void *buffer, unsigned n)
{ struct files_scan *s = ctx; return s->source.write(s->source.ctx, fd, buffer, n); }
static int scan_sync(void *ctx, int fd) { struct files_scan *s = ctx; return s->source.sync(s->source.ctx, fd); }
static int scan_close(void *ctx, int fd) { struct files_scan *s = ctx; return s->source.close(s->source.ctx, fd); }
static int scan_mkdir(void *ctx, const char *path) { struct files_scan *s = ctx; return s->source.mkdir(s->source.ctx, path); }
static int scan_rename(void *ctx, const char *from, const char *to) { struct files_scan *s = ctx; return s->source.rename(s->source.ctx, from, to); }
static int scan_unlink(void *ctx, const char *path) { struct files_scan *s = ctx; return s->source.unlink(s->source.ctx, path); }
static int scan_rmdir(void *ctx, const char *path) { struct files_scan *s = ctx; return s->source.rmdir(s->source.ctx, path); }
static void scan_free(struct files_scan *s)
{
    if (!s) return;
    fm_destroy(&s->state.model);
    free(s->state.tree);
    free(s);
}
static int scan_sniff(struct files_scan *s)
{
    if (scan_cancelled(s)) return -ECANCELED;
    int fd = s->source.open(s->source.ctx, s->target, 0);
    if (fd < 0) return fd;
    unsigned char h[20]; unsigned used = 0; int error = 0;
    while (used < sizeof(h) && !scan_cancelled(s)) {
        int n = s->source.read(s->source.ctx, fd, h + used, sizeof(h) - used);
        if (n == -EINTR) continue;
        if (n < 0) { error = n; break; }
        if (!n) break;
        if ((unsigned)n > sizeof(h) - used) { error = -EIO; break; }
        used += (unsigned)n;
    }
    int closed = s->source.close(s->source.ctx, fd);
    if (!error) error = closed;
    if (scan_cancelled(s)) return -ECANCELED;
    if (error) return error;
    s->changed = used == sizeof(h) && !memcmp(h, "\177ELF\1\1\1", 7) && h[16] == 2 && h[18] == 3;
    return 0;
}
static unsigned scan_bucket(const char *name)
{
    uint32_t hash=2166136261u;
    for (const unsigned char *p=(const unsigned char *)name;*p;p++) hash=(hash^*p)*16777619u;
    return hash%(FM_ENTRIES*2);
}
static int scan_entry(const char *name, const struct fm_meta *meta, void *arg)
{
    struct files_scan *s=arg; struct fm_model *m=&s->state.model;
    if (!strcmp(name,".") || !strcmp(name,"..") || fm_hidden(m->path,name)) return 0;
    if (!*name || strlen(name)>=FM_NAME || strchr(name,'/')) return -EIO;
    if (s->listed++>=FM_ENTRIES) return -E2BIG;
    unsigned slot=scan_bucket(name);
    while (s->index[slot] && strcmp(m->entries[s->index[slot]-1].name,name)) slot=(slot+1)%(FM_ENTRIES*2);
    if (s->index[slot]) {
        unsigned i=s->index[slot]-1; struct fm_entry *e=&m->entries[i];
        if (s->seen[i]) return -EIO;
        s->seen[i]=1;
        if (e->meta.type!=meta->type) s->metadata_only=0;
        if (e->meta.type!=meta->type || e->meta.size!=meta->size || e->meta.modified!=meta->modified || e->meta.ino!=meta->ino || e->meta.dev!=meta->dev) {
            uint32_t exec=e->meta.exec; e->meta=*meta; e->meta.exec=exec; s->dirty[i]=1; s->changed=1;
        }
    } else {
        s->metadata_only=0; s->changed=1;
        if (m->count>=FM_ENTRIES) return -E2BIG;
        if (m->count==m->capacity) {
            unsigned cap=m->capacity+32; if (cap>FM_ENTRIES) cap=FM_ENTRIES;
            struct fm_entry *p=realloc(m->entries,cap*sizeof(*p)); if (!p) return -ENOMEM;
            m->entries=p; m->capacity=cap;
        }
        unsigned i=m->count++; m->entries[i]=(struct fm_entry){.meta=*meta};
        snprintf(m->entries[i].name,sizeof(m->entries[i].name),"%s",name); s->seen[i]=1;
        s->index[slot]=(uint16_t)(i+1);
    }
    return 0;
}
static int scan_listing(struct files_scan *s)
{
    struct fm_model *m=&s->state.model;
    s->metadata_only=1;
    for (unsigned i=0;i<m->count;i++) {
        unsigned slot=scan_bucket(m->entries[i].name);
        while (s->index[slot]) slot=(slot+1)%(FM_ENTRIES*2);
        s->index[slot]=(uint16_t)(i+1);
    }
    int r=scan_list(s,m->path,FM_LIST_NAMES_ONLY,scan_entry,s); if (r) return r;
    unsigned count=0;
    for (unsigned i=0;i<m->count;i++) if (s->seen[i]) m->entries[count++]=m->entries[i];
    if (count!=m->count) { s->metadata_only=0; s->changed=1; m->count=count; }
    if (!s->metadata_only) fm_sort(m,m->sort,m->descending);
    return 0;
}
static int metadata_apply(struct files_app *a, struct files_scan *s)
{
    struct fm_model *m=&a->model, *n=&s->state.model;
    if (!s->metadata_only || s->labels_changed || memcmp(&a->media,&s->state.media,sizeof(a->media)) || m->count!=n->count || !m->path[0]) return 0;
    for (unsigned i=0;i<m->count;i++) if (strcmp(m->entries[i].name,n->entries[i].name)) return 0;
    int reorder=0, status=0;
    for (unsigned i=0;i<m->count;i++) if (s->dirty[i]) {
        struct fm_entry *e=&m->entries[i];
        reorder|=(m->sort==1 && e->meta.size!=n->entries[i].meta.size) || (m->sort==3 && e->meta.modified!=n->entries[i].meta.modified);
        status|=e->selected && e->meta.size!=n->entries[i].meta.size;
        e->meta=n->entries[i].meta;
        if (i<a->capacity) {
            if (e->meta.type==FM_DIR) a->texts[i].size[0]=0; else kb_text(a->texts[i].size,sizeof(a->texts[i].size),e->meta.size);
            time_text(a->texts[i].modified,sizeof(a->texts[i].modified),e->meta.modified);
            if (W(a,W_LIST)->view==DU_DETAILS) du_invalidate(a->ui,du_list_row(W(a,W_LIST),i));
        }
    }
    if (reorder) {
        char active[FM_NAME]="", anchor[FM_NAME]="";
        if (m->active<m->count) strcpy(active,m->entries[m->active].name);
        if (m->anchor<m->count) strcpy(anchor,m->entries[m->anchor].name);
        fm_sort(m,m->sort,m->descending);
        for (unsigned i=0;i<m->count;i++) { if (!strcmp(active,m->entries[i].name)) m->active=i; if (!strcmp(anchor,m->entries[i].name)) m->anchor=i; }
        int dirty=a->ui->dirty; files_update(a); a->ui->dirty=dirty;
        du_invalidate(a->ui,W(a,W_LIST)->box);
    }
    if (status && !reorder) {
        struct du_widget *bar=W(a,W_STATUS);
        const char *tab=strchr(bar->text,'\t'), *tail=tab ? strchr(tab+1,'\t') : NULL;
        if (tab && tail) {
            uint64_t bytes=0; fm_selected(m,&bytes); char size[48], text[DU_TEXT_MAX];
            files_size_text(size,sizeof(size),bytes);
            snprintf(text,sizeof(text),"%.*s\t%s%s",(int)(tab-bar->text),bar->text,size,tail);
            du_set_text(bar,text); du_invalidate(a->ui,bar->box);
        }
    }
    return 1;
}
static uint64_t clipboard_key(const struct fm_model *m)
{
    uint64_t key=1469598103934665603ull;
    key=(key^(unsigned)m->cut)*1099511628211ull;
    for (unsigned i=0;i<m->clipboard_count;i++) {
        for (const unsigned char *p=(const unsigned char *)m->clipboard[i];*p;p++) key=(key^*p)*1099511628211ull;
        key=(key^255u)*1099511628211ull;
    }
    return key;
}
static void *scan_run(void *arg)
{
    struct files_worker *w = arg;
    for (;;) {
        pthread_mutex_lock(&w->lock);
        while (!w->pending && !w->stopping) pthread_cond_wait(&w->changed, &w->lock);
        if (w->stopping) { pthread_mutex_unlock(&w->lock); break; }
        struct files_scan *s = w->pending; w->pending = NULL; w->busy = 1;
        pthread_mutex_unlock(&w->lock);
        struct files_app *a = &s->state;
        if (s->kind==SCAN_MEDIA && a->poll_clipboard && !scan_cancelled(s)) {
            struct fm_vfs adapter=a->model.fs; a->model.fs=s->source;
            s->clipboard_valid=!fm_clipboard_load(&a->model); a->model.fs=adapter;
        }
        if (s->kind == SCAN_OPEN) s->error = scan_sniff(s);
        else {
            if (s->kind == SCAN_MEDIA || s->kind == SCAN_REFRESH ||
                (s->kind == SCAN_NAVIGATE && !a->model.history_count)) {
                struct ciuki_media_state state = a->media;
                if (a->media_read && !scan_cancelled(s) && !a->media_read(&state)) {
                    int d = media_drive(a->model.path);
                    int expired = d >= 0 && (a->media.removable_mask & (1u << d)) &&
                        (!(state.removable_mask & (1u << d)) || a->media.generation[d] != state.generation[d]);
                    s->changed = memcmp(&state, &a->media, sizeof(state)) != 0;
                    a->media = state;
                    if (expired && s->kind == SCAN_MEDIA) {
                        s->kind = SCAN_NAVIGATE; strcpy(s->target, FM_COMPUTER);
                    }
                }
                if (!scan_cancelled(s)) s->labels_changed = labels_refresh(a, s);
            }
            a->model.drive_slots = floppy_slots(a);
            if (s->kind == SCAN_NAVIGATE) {
                s->error = fm_navigate(&a->model, s->target);
                /* fx-17b: Windows 98 reads a diskette when its drive is opened;
                 * there is no insertion event. Read it once, then retry. */
                int d = media_drive(s->target);
                if (s->error && floppy_drive(a, d) && a->media_load && !scan_cancelled(s) && !a->media_load((unsigned)d)) {
                    struct ciuki_media_state state = a->media;
                    if (a->media_read && !a->media_read(&state)) { s->changed = 1; a->media = state; }
                    s->error = fm_navigate(&a->model, s->target);
                }
            }
            else if (s->kind == SCAN_HISTORY) s->error = fm_history(&a->model, s->delta);
            else if (s->kind==SCAN_MEDIA && a->model.path[0] && !fm_is_recycle(a->model.path)) {
                int topology=s->changed || s->labels_changed;
                s->error=scan_listing(s);
                if (topology) s->metadata_only=0;
            }
            else if (s->kind == SCAN_REFRESH || (s->kind == SCAN_MEDIA && s->changed)) s->error = fm_refresh(&a->model);
            if (!s->error && !scan_cancelled(s) && (s->kind != SCAN_MEDIA || (s->changed && !s->metadata_only))) {
                a->reading = s->kind == SCAN_NAVIGATE || s->kind == SCAN_HISTORY || s->kind == SCAN_REFRESH || (s->kind == SCAN_MEDIA && s->changed);
                struct fm_meta meta;
                a->games_available = !scan_stat(s, "/Games", &meta) && meta.type == FM_DIR;
                tree_build(a);
            }
        }
        pthread_mutex_lock(&w->lock);
        struct files_scan *old = w->result;
        if (scan_cancelled(s) || w->stopping) { w->busy = 0; pthread_mutex_unlock(&w->lock); scan_free(s); }
        else { w->result = s; w->busy = 0; pthread_mutex_unlock(&w->lock); scan_free(old); du_wake(w->ui); }
    }
    return NULL;
}
static int scan_start(struct files_app *a)
{
    struct files_worker *w = calloc(1, sizeof(*w));
    if (!w) return -ENOMEM;
    int e = pthread_mutex_init(&w->lock, NULL);
    if (e) { free(w); return -e; }
    e = pthread_cond_init(&w->changed, NULL);
    if (e) { pthread_mutex_destroy(&w->lock); free(w); return -e; }
    w->ui=a->ui;
    e = pthread_create(&w->thread, NULL, scan_run, w);
    if (e) { pthread_cond_destroy(&w->changed); pthread_mutex_destroy(&w->lock); free(w); return -e; }
    a->worker = w;
    return 0;
}
static int scan_submit(struct files_app *a, enum scan_kind kind, const char *path, int delta)
{
    struct files_worker *w = a->worker;
    if (!w) return -EIO;
    if (kind==SCAN_MEDIA) {
        if (a->media_polled && a->ui->now-a->media_poll_ns<1000000000ull) return 0;
        a->media_polled=1; a->media_poll_ns=a->ui->now;
    }
    struct files_scan *s = calloc(1, sizeof(*s));
    if (!s) return -ENOMEM;
    s->state.tree = calloc(FILES_TREE, sizeof(*s->state.tree));
    if (!s->state.tree) { free(s); return -ENOMEM; }
    struct files_app *b = &s->state;
    memcpy(b->tree, a->tree, FILES_TREE * sizeof(*b->tree)); b->tree_count = a->tree_count;
    b->games_available = a->games_available;
    fm_init(&b->model, &a->model.fs);
    memcpy(b->model.path, a->model.path, FM_PATH);
    memcpy(b->model.history, a->model.history, sizeof(b->model.history));
    b->model.history_count = a->model.history_count; b->model.history_at = a->model.history_at;
    b->model.sort = a->model.sort; b->model.descending = a->model.descending;
    memcpy(b->expanded, a->expanded, sizeof(b->expanded)); b->expanded_count = a->expanded_count;
    memcpy(b->collapsed, a->collapsed, sizeof(b->collapsed)); b->collapsed_count = a->collapsed_count;
    memcpy(b->leaf, a->leaf, sizeof(b->leaf)); b->leaf_count = a->leaf_count;
    b->media = a->media; memcpy(b->drive_labels, a->drive_labels, sizeof(b->drive_labels));
    b->media_read = a->media_read; b->media_label_key = a->media_label_key; b->media_label = a->media_label;
    b->media_load = a->media_load; b->model.drive_slots = a->model.drive_slots;
    if (kind==SCAN_MEDIA && a->model.count) {
        b->model.entries=malloc(a->model.count*sizeof(*b->model.entries));
        if (!b->model.entries) { scan_free(s); return -ENOMEM; }
        memcpy(b->model.entries,a->model.entries,a->model.count*sizeof(*b->model.entries));
        b->model.count=b->model.capacity=a->model.count;
    }
    b->poll_clipboard=a->poll_clipboard; s->clipboard_key=clipboard_key(&a->model);
    s->kind = kind; s->delta = delta; s->worker = w; s->source = a->model.fs;
    /* Only read callbacks are used through this cancellation adapter. */
    b->model.fs = (struct fm_vfs){.ctx=s, .list=scan_list, .stat_path=scan_stat, .open=scan_open, .read=scan_read, .write=scan_write,
        .sync=scan_sync, .close=scan_close, .mkdir=scan_mkdir, .rename=scan_rename, .unlink=scan_unlink, .rmdir=scan_rmdir};
    b->model.fixed = a->model.fixed;
    if (path) snprintf(s->target, sizeof(s->target), "%s", path);
    pthread_mutex_lock(&w->lock);
    s->revision = __atomic_add_fetch(&w->revision, 1, __ATOMIC_RELEASE);
    struct files_scan *old = w->pending, *result = w->result;
    w->pending = s; w->result = NULL;
    pthread_cond_signal(&w->changed);
    pthread_mutex_unlock(&w->lock);
    scan_free(old); scan_free(result);
    a->tree_dirty = 0;
    a->reading = 1;
    if (kind == SCAN_NAVIGATE || kind == SCAN_HISTORY || kind == SCAN_REFRESH) {
        a->loading = 1;
        snprintf(a->loading_path, sizeof(a->loading_path), "%s", kind == SCAN_REFRESH ? a->model.path : s->target);
        if (kind == SCAN_HISTORY) {
            int at = (int)a->model.history_at + delta;
            if (at >= 0 && (unsigned)at < a->model.history_count) strcpy(a->loading_path, a->model.history[at]);
        }
        files_update(a);
    }
    return 0;
}
static void scan_poll(struct files_app *a)
{
    struct files_worker *w = a->worker;
    if (!w) return;
    pthread_mutex_lock(&w->lock);
    struct files_scan *s = w->result; w->result = NULL;
    pthread_mutex_unlock(&w->lock);
    if (!s) return;
    if (scan_cancelled(s)) { scan_free(s); return; }
    struct files_app *b = &s->state;
    a->reading = 0;
    du_timer_stop(a->ui, T_BROWSER);
    if (s->clipboard_valid && s->clipboard_key==clipboard_key(&a->model) && clipboard_key(&a->model)!=clipboard_key(&b->model)) {
        fm_clipboard_clear(&a->model);
        a->model.clipboard=b->model.clipboard; a->model.clipboard_count=b->model.clipboard_count; a->model.cut=b->model.cut;
        b->model.clipboard=NULL; b->model.clipboard_count=0;
        files_update(a);
    }
    if (s->kind == SCAN_MEDIA && !s->error && !s->changed && !s->labels_changed) {
        scan_free(s); return; /* An unchanged poll must not repaint the window. */
    }
    if (s->kind==SCAN_MEDIA && !s->error && metadata_apply(a,s)) { scan_free(s); return; }
    int tree_changed = a->tree_dirty;
    if (s->kind == SCAN_OPEN) {
        unsigned index = a->model.count;
        for (unsigned i = 0; i < a->model.count; i++) {
            char path[FM_PATH];
            if (!fm_entry_path(&a->model, i, path) && !strcmp(path, s->target)) { index = i; break; }
        }
        if (s->error) error_box(a, "Files", "Cannot read the file:", s->error);
        else if (index < a->model.count) {
            a->model.entries[index].meta.exec = (uint32_t)s->changed;
            /* The negative argument marks an already classified open. */
            open_item(a, index, -1);
        }
        scan_free(s); return;
    }
    a->loading = 0;
    if (s->error) {
        int floppy = (s->kind == SCAN_NAVIGATE || s->kind == SCAN_HISTORY) ? media_drive(s->target) : -1;
        a->media = b->media; a->model.drive_slots = floppy_slots(a);
        if (floppy_drive(a, floppy) && s->kind == SCAN_NAVIGATE) insert_disk_prompt(a, floppy, s->target);
        else if (s->error != -ENOENT || s->kind != SCAN_HISTORY) error_box(a, "Files", "Cannot open the folder.", s->error);
    } else {
        if (s->kind != SCAN_TREE && (s->kind != SCAN_MEDIA || (s->changed && !s->metadata_only))) {
            struct fm_model *m = &a->model, *n = &b->model;
            int refresh = s->kind == SCAN_REFRESH || s->kind == SCAN_MEDIA;
            char active[FM_NAME] = "";
            if (refresh && m->active < m->count) strcpy(active, m->entries[m->active].name);
            if (refresh) for (unsigned i = 0; i < n->count; i++) for (unsigned j = 0; j < m->count; j++)
                if (!strcmp(n->entries[i].name, m->entries[j].name)) n->entries[i].selected = m->entries[j].selected;
            free(m->entries); m->entries = n->entries; n->entries = NULL;
            free(m->bin); m->bin = n->bin; n->bin = NULL; m->bin_count = n->bin_count; m->fixed = n->fixed;
            m->count = n->count; m->capacity = n->capacity; m->active = n->active; m->anchor = n->anchor;
            memcpy(m->path, n->path, FM_PATH); memcpy(m->history, n->history, sizeof(m->history));
            m->history_count = n->history_count; m->history_at = n->history_at;
            m->find_length = 0; m->find[0] = 0;
            if (refresh) for (unsigned i = 0; i < m->count; i++) if (!strcmp(active, m->entries[i].name)) m->active = m->anchor = i;
            fm_sort(m, m->sort, m->descending);
            if (!refresh) W(a, W_LIST)->scroll = W(a, W_LIST)->hscroll = 0;
            if (a->select_created) {
                fm_select_none(m);
                for (unsigned i = 0; i < a->job.created_count; i++) fm_select_name(m, a->job.created[i], 1);
            }
        }
        a->media = b->media; memcpy(a->drive_labels, b->drive_labels, sizeof(a->drive_labels));
        a->model.drive_slots = floppy_slots(a);
        a->games_available = b->games_available;
        memcpy(a->tree, b->tree, FILES_TREE * sizeof(*a->tree)); a->tree_count = b->tree_count;
        memcpy(a->leaf, b->leaf, sizeof(a->leaf)); a->leaf_count = b->leaf_count;
        places_build(a); files_layout(a);
    }
    a->tree_dirty = tree_changed; a->select_created = 0;
    files_update(a);
    du_list_ensure(W(a, W_LIST), W(a, W_LIST)->active);
    int stage = a->refresh_checkpoint; a->refresh_checkpoint = 0;
    if (!s->error && fm_same_path(a->model.path, "/system")) checkpoint(a, 1);
    if (!s->error && stage) checkpoint(a, stage);
    if (!s->error && a->show_bin_properties && fm_is_recycle(a->model.path)) { a->show_bin_properties = 0; bin_properties(a); }
    int pending = s->error ? 0 : a->pending_command;
    a->pending_command = 0;
    scan_free(s);
    if (pending) command(a, pending);
}
static void scan_stop(struct files_app *a)
{
    struct files_worker *w = a->worker;
    if (!w) return;
    pthread_mutex_lock(&w->lock);
    w->stopping = 1; __atomic_add_fetch(&w->revision, 1, __ATOMIC_RELEASE);
    pthread_cond_signal(&w->changed);
    pthread_mutex_unlock(&w->lock);
    pthread_join(w->thread, NULL);
    scan_free(w->pending); scan_free(w->result);
    pthread_cond_destroy(&w->changed); pthread_mutex_destroy(&w->lock);
    free(w); a->worker = NULL;
}
int files_navigate(struct files_app *a, const char *path)
{
    char target[FM_PATH]; int r = fm_posix_path(target, path);
    if (r) { error_box(a, "Files", "Cannot open the folder.", r); return r; }
    if (a->rename_index >= 0) rename_hide(a);
    a->note[0] = 0; a->select_created = a->refresh_checkpoint = a->pending_command = 0;
    r = scan_submit(a, SCAN_NAVIGATE, target, 0);
    if (r) error_box(a, "Files", "Cannot open the folder.", r);
    return r;
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
    if (a->loading || index >= a->model.count) return;
    int classified = with_note < 0;
    if (classified) with_note = 0;
    struct fm_entry *e = &a->model.entries[index];
    char path[FM_PATH];
    if (!a->model.path[0] && with_note) return;
    if (e->bin) { bin_item_properties(a, index); return; }   /* G2: an item in the bin opens its properties */
    if (fm_entry_path(&a->model, index, path)) return;
    if (e->meta.type == FM_DIR && !with_note) { files_navigate(a, path); return; }
    if (!with_note && !classified && e->meta.type == FM_FILE && !e->meta.exec && !strchr(e->name, '.')) {
        int r = scan_submit(a, SCAN_OPEN, path, (int)index);
        if (r) error_box(a, "Files", "Cannot open the file.", r);
        return;
    }
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
            if (r < 0) error_box(a, "Files", "Cannot start the application:", r);
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

static void transfer_notify(void *ctx) { du_wake(ctx); }
static int transfer_start(struct files_app *a)
{
    if (a->transfer) return 0;
    int r=fm_transfer_create(&a->transfer,&a->job);
    if (!r) { a->transfer->notify=transfer_notify; a->transfer->notify_ctx=a->ui; }
    return r;
}
static void transfer_command(struct files_app *a, int cancel, int answer)
{ if (a->job_active) fm_transfer_command(a->transfer,cancel,answer); }
static void transfer_stop(struct files_app *a)
{ fm_transfer_destroy(a->transfer); a->transfer=NULL; }
static void progress(struct files_app *a)
{
    struct files_transfer *w=a->transfer;
    if (!w) return;
    pthread_mutex_lock(&w->lock);
    w->ui_view=w->view; pthread_mutex_unlock(&w->lock);
    fm_progress_dialog(a->ui,D_PROGRESS,a->job.kind,a->job.destination,&w->ui_view,w->started_at);
}
static int job_begin(struct files_app *a)
{
    if (a->job_active || a->loading) return -EBUSY;
    int r=transfer_start(a);
    if (r) { error_box(a,"Files","Cannot start the file worker:",r); return r; }
    fm_job_destroy(&a->job);
    return 0;
}
static void job_launch(struct files_app *a)
{
    a->job.chunk=FM_CHUNK;
    a->job_active=1;
    fm_transfer_reset(a->transfer);
    a->job_ui_ns=job_now();
    du_timer_start(a->ui,T_JOB,1000000000ull,0); /* visible elapsed time, not result polling */
    progress(a);
    fm_transfer_submit(a->transfer);
}
static int start_job(struct files_app *a, enum fm_job_kind kind, char *const *paths, unsigned n, const char *destination)
{
    int r=job_begin(a);
    if (r) return r;
    r=fm_job_start(&a->job,&a->model.fs,kind,paths,n,destination);
    if (r) { error_box(a,"Files","Cannot start the selected operation:",r); return r; }
    job_launch(a);
    return 0;
}
/* G2: the Recycle Bin's jobs run on the same worker as every other transfer. */
enum { BIN_DELETE = 1, BIN_PURGE, BIN_RESTORE, BIN_EMPTY };
static int start_bin_job(struct files_app *a, int op, char *const *paths, unsigned n, unsigned flags)
{
    int r=job_begin(a);
    if (r) return r;
    switch (op) {
    case BIN_DELETE: r=rb_delete_job(&a->job,&a->model.fs,paths,n,&a->policy,a->model.fixed,flags); break;
    case BIN_PURGE: r=rb_purge_job(&a->job,&a->model.fs,paths,n); break;
    case BIN_RESTORE: r=rb_restore_job(&a->job,&a->model.fs,paths,n); break;
    default: r=rb_empty_job(&a->job,&a->model.fs,a->model.fixed); break;
    }
    if (r==-ENOENT && op==BIN_EMPTY) {
        du_message(a->ui,D_INFO,"Recycle Bin","The Recycle Bin is already empty.",DU_DLG_ICON_INFO,NULL,0,0,0);
        return r;
    }
    if (r) { error_box(a,"Files",op==BIN_RESTORE ? "Cannot restore the selected items:" : "Cannot start the selected operation:",r); return r; }
    a->bin_op=op;
    job_launch(a);
    return 0;
}

static int fold_drive(char c) { return (c >= 'A' && c <= 'Z' ? c - 'A' : c - 'a'); }
static void properties_dialog(struct files_app *a)
{
    struct fm_job *j = &a->job;
    /* text and parent are static: UI thread only, the dialog copies them, and
     * on the stack they exceeded the 4 KiB frame limit of the i686 build. */
    static char text[DU_DIALOG_TEXT], parent[FM_PATH];
    char size[48], g[32], contains[96] = "", type[48], when[24] = "";
    struct fm_meta meta = j->property_meta;
    const char *path = j->sources[0];
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
    /* fx-17b: a drive's Properties show its capacity and free space, and
     * "calculating" while a removable FAT32 volume's free count (FSInfo
     * unknown) is still being computed in the background. */
    struct ciuki_media_space space;
    int drive = fm_drive_root(path) ? (strcmp(path, "/") ? fold_drive(path[5]) : 2) : -1;
    if (j->count == 1 && drive >= 0 && a->media_space && !a->media_space((unsigned)drive, &space)) {
        char cap[48], freebuf[48], used[48], line[200];
        uint64_t cluster = space.cluster_bytes, total = (uint64_t)space.total_clusters * cluster;
        uint64_t free_bytes = (uint64_t)space.free_clusters * cluster;
        files_size_text(cap, sizeof(cap), total);
        if (space.flags & CIUKI_MEDIA_SPACE_COUNTING) { snprintf(freebuf, sizeof(freebuf), "calculating..."); snprintf(used, sizeof(used), "calculating..."); }
        else { files_size_text(freebuf, sizeof(freebuf), free_bytes); files_size_text(used, sizeof(used), total > free_bytes ? total - free_bytes : 0); }
        snprintf(line, sizeof(line), "\n---\nUsed space:\t%s\nFree space:\t%s\nCapacity:\t%s", used, freebuf, cap);
        size_t n = strlen(text); snprintf(text + n, sizeof(text) - n, "%s", line);
    }
    char title[FM_NAME + 32];
    snprintf(title, sizeof(title), "%.60s Properties", j->count == 1 ? (fm_drive_root(path) ? parent : fm_basename(path)) : "Selection");
    du_message(a->ui, D_PROPERTIES, title, text, DU_DLG_ICON_INFO, NULL, 0, 0, 0);
    du_dialog_icon(a->ui, icon);
}

/* Not inlined into job_tick: its name and report buffers would push job_tick
 * past the 4 KiB frame limit of the i686 build. */
__attribute__((noinline)) static void job_finished(struct files_app *a)
{
    struct fm_job *j = &a->job;
    a->job_active = 0;
    du_timer_stop(a->ui, T_JOB);
    if (du_dialog_id(a->ui) == D_PROGRESS || du_dialog_id(a->ui) == D_REPLACE) du_dialog_close(a->ui);
    if (a->close_pending) { if (a->close) a->close(a); return; }
    enum fm_job_kind kind = j->kind;
    /* Static: job_finished runs once per job on the UI thread and du_message
     * copies the text; on the stack it pushed job_tick over the 4 KiB frame limit. */
    static char report[DU_DIALOG_TEXT];
    char report_title[64]; int report_icon = 0;
    if (kind != FM_PROPERTIES && fm_job_report(j, report_title, sizeof(report_title), report, sizeof(report), &report_icon))
        du_message(a->ui, D_ERROR, report_title, report, report_icon, NULL, 0, 0, 0);
    else if (j->state == FM_FAILED) {
        const char *source = j->depth ? j->frames[j->depth - 1].source : j->next ? j->sources[j->next - 1] : j->sources[0];
        char what[FM_NAME + 40];
        snprintf(what, sizeof(what), "Cannot %s %.200s:", verb(kind), fm_basename(source));
        error_box(a, "Error Reading File or Folder", what, -j->error);
    }
    if (kind == FM_PROPERTIES) {
        if (j->state == FM_DONE) properties_dialog(a);
        files_update(a);
        return;
    }
    a->select_created = (kind == FM_COPY || kind == FM_MOVE) && j->created_count && fm_same_path(j->destination, a->model.path);
    scan_submit(a, SCAN_REFRESH, NULL, 0);
    if (kind == FM_MOVE && !j->bin && j->state == FM_DONE && !j->skipped && !j->kept && !j->kept_folders && a->model.cut) fm_clipboard_consume(&a->model);
    if (j->bin && j->state == FM_DONE && j->bin->mode == RB_PURGE && a->bin_op == BIN_EMPTY && a->ui->sound) a->ui->sound(a->ui->sound_ctx, SOUND_EMPTY_RECYCLE);
    if (j->state == FM_CANCELLED) snprintf(a->note, sizeof(a->note), "Cancelled; %u item(s) completed", j->completed);
    else if (j->state == FM_DONE && j->bin)
        snprintf(a->note, sizeof(a->note), "%u item(s) %s%s", j->completed - j->skipped,
                 j->bin->mode == RB_RECYCLE ? (j->bin->recycled == j->completed - j->skipped ? "sent to the Recycle Bin" : j->bin->recycled ? "deleted, some sent to the Recycle Bin" : "deleted") :
                 j->bin->mode == RB_RESTORE ? "restored" : a->bin_op == BIN_EMPTY ? "deleted, the Recycle Bin is empty" : "deleted",
                 j->skipped ? ", some skipped" : "");
    else if (j->state == FM_DONE)
        snprintf(a->note, sizeof(a->note), "%u item(s) %s%s%s", j->completed - j->skipped,
                 kind == FM_COPY ? "copied" : kind == FM_MOVE ? (j->kept || j->kept_folders ? "copied; originals kept" : "moved") : "deleted",
                 j->skipped ? ", some skipped" : "", kind == FM_COPY && j->metadata_warnings ? " (dates not kept)" : "");
    files_update(a);
    du_list_ensure(W(a, W_LIST), W(a, W_LIST)->active);
    if (j->state == FM_DONE && !j->skipped && kind == FM_COPY && j->completed) a->refresh_checkpoint = 4;
    if (j->state == FM_DONE && kind == FM_DELETE && j->completed) a->refresh_checkpoint = 5;
}

static void replace_prompt(struct files_app *a)
{
    struct files_transfer *w=a->transfer;
    pthread_mutex_lock(&w->lock); w->ui_view=w->view; pthread_mutex_unlock(&w->lock);
    fm_replace_dialog(a->ui,D_REPLACE,&w->ui_view);
}

static void job_tick(struct files_app *a)
{
    if (!a->job_active) return;
    struct files_transfer *w=a->transfer;
    pthread_mutex_lock(&w->lock);
    enum fm_job_state state=w->view.state; int finished=w->finished;
    uint64_t bytes=w->view.bytes; unsigned items=w->view.items, publication=w->publication;
    pthread_mutex_unlock(&w->lock);
    uint64_t now=job_now();
    if (publication==a->job_publication && !finished && now-a->job_ui_ns<1000000000ull) return;
    a->job_publication=publication; a->job_ui_ns=now;
    if (!finished && state==FM_RUNNING) du_timer_start(a->ui,T_JOB,1000000000ull,0);
    if (!finished && a->job.kind!=FM_PROPERTIES && now-w->progress_logged>=5000000000ull) {
        job_record("[files] job=%s progress items=%u bytes=%llu",verb(a->job.kind),items,(unsigned long long)bytes);
        w->progress_logged=now;
    }
    if (du_dialog_id(a->ui)==D_CANCEL_JOB) return;
    if (finished) { job_finished(a); return; }
    if (state==FM_REPLACE) { if (du_dialog_id(a->ui)!=D_REPLACE) replace_prompt(a); return; }
    progress(a);
}

/* A completion or an ordinary UI event services both mailboxes. No retry
 * timer is armed; the short mailbox locks never contain VFS operations. */
static void files_service(struct du_ui *u,void *ctx)
{
    (void)u; struct files_app *a=ctx;
    scan_poll(a); job_tick(a);
}

/* ---- commands ---- */
static void paste(struct files_app *a, const char *destination)
{
    if (fm_clipboard_load(&a->model)) return;
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
        if (!paths[k] || fm_entry_path(&a->model, i, paths[k])) {
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

/* G2: Delete sends the selection to the Recycle Bin; Shift+Delete, drives
 * set to "do not move files" and removable media delete for good. */
static void run_delete(struct files_app *a)
{
    char **paths; unsigned n;
    if (selection_paths(a, &paths, &n)) return;
    start_bin_job(a, BIN_DELETE, paths, n, (unsigned)a->delete_flags);
    for (unsigned i = 0; i < n; i++) free(paths[i]);
    free(paths);
}
static void selected_label(struct files_app *a, char name[FM_NAME], int *folder)
{
    const struct fm_model *m = &a->model;
    const struct fm_entry *e = NULL;
    for (unsigned i = 0; i < m->count && !e; i++) if (m->entries[i].selected) e = &m->entries[i];
    name[0] = 0; *folder = 0;
    if (!e) return;
    const struct rb_item *item = rb_entry_item(m, e);
    if (item) rb_item_name(item, name); else snprintf(name, FM_NAME, "%s", e->name);
    *folder = e->meta.type == FM_DIR;
}
static void confirm_delete(struct files_app *a, unsigned flags)
{
    unsigned n = fm_selected(&a->model, NULL);
    if (!n) return;
    if (rb_policy_load(&a->model.fs, &a->policy)) rb_policy_default(&a->policy);
    a->model.fixed = files_fixed(a);
    char **paths; unsigned count;
    if (selection_paths(a, &paths, &count)) return;
    int recycle = rb_classify(&a->policy, a->model.fixed, flags, paths, count) != 0;
    for (unsigned i = 0; i < count; i++) free(paths[i]);
    free(paths);
    a->delete_flags = (int)flags;
    if (!a->policy.confirm) { run_delete(a); return; }
    char title[64], text[FM_NAME + 200], name[FM_NAME]; int folder;
    selected_label(a, name, &folder);
    rb_delete_prompt(n, n == 1 && folder, name, recycle, title, sizeof(title), text, sizeof(text));
    du_message(a->ui, D_DELETE, title, text, recycle ? DU_DLG_ICON_QUESTION : DU_DLG_ICON_WARNING, yes_no, 2, 0, 1);
}
static void run_bin_selection(struct files_app *a, int op)
{
    char **paths; unsigned n;
    if (selection_paths(a, &paths, &n)) return;
    start_bin_job(a, op, paths, n, 0);
    for (unsigned i = 0; i < n; i++) free(paths[i]);
    free(paths);
}
/* Delete inside the bin removes the items for good. */
static void confirm_purge(struct files_app *a)
{
    unsigned n = fm_selected(&a->model, NULL);
    if (!n) return;
    if (rb_policy_load(&a->model.fs, &a->policy)) rb_policy_default(&a->policy);
    if (!a->policy.confirm) { run_bin_selection(a, BIN_PURGE); return; }
    char title[64], text[FM_NAME + 200], name[FM_NAME]; int folder;
    selected_label(a, name, &folder);
    rb_delete_prompt(n, n == 1 && folder, name, 0, title, sizeof(title), text, sizeof(text));
    du_message(a->ui, D_BIN_PURGE, title, text, DU_DLG_ICON_WARNING, yes_no, 2, 0, 1);
}
static void confirm_empty(struct files_app *a)
{
    if (!a->model.count) {
        du_message(a->ui, D_INFO, "Recycle Bin", "The Recycle Bin is already empty.", DU_DLG_ICON_INFO, NULL, 0, 0, 0);
        return;
    }
    if (rb_policy_load(&a->model.fs, &a->policy)) rb_policy_default(&a->policy);
    if (!a->policy.confirm) { start_bin_job(a, BIN_EMPTY, NULL, 0, 0); return; }
    char title[64], text[FM_NAME + 200]; unsigned n = a->model.count;
    const struct rb_item *only = n == 1 ? rb_entry_item(&a->model, &a->model.entries[0]) : NULL;
    char name[FM_NAME] = "";
    if (only) rb_item_name(only, name);
    rb_delete_prompt(n, only && only->directory, name, 0, title, sizeof(title), text, sizeof(text));
    du_message(a->ui, D_BIN_EMPTY, title, text, DU_DLG_ICON_WARNING, yes_no, 2, 0, 1);
}

/* ---- G2: Recycle Bin properties ---- */
/* Item properties: where it came from, when it was deleted. */
static void bin_item_properties(struct files_app *a, unsigned index)
{
    struct fm_model *m = &a->model;
    if (index >= m->count) return;
    const struct fm_entry *e = &m->entries[index]; const struct rb_item *item = rb_entry_item(m, e);
    if (!item) return;
    char name[FM_NAME], where[FM_PATH], size[48], g[32], kind_buffer[48], deleted[24], modified[24], text[DU_DIALOG_TEXT];
    rb_item_name(item, name); rb_item_location(item, where);
    files_size_text(size, sizeof(size), item->size); group(g, sizeof(g), item->size);
    const char *type = fm_type_name(e, kind_buffer, sizeof(kind_buffer));
    time_text(deleted, sizeof(deleted), item->deleted); time_text(modified, sizeof(modified), e->meta.modified);
    snprintf(text, sizeof(text), "%.200s\n---\nType:\t%s\nOriginal location:\t%.200s\nDeleted:\t%s\nSize:\t%s (%s bytes)\nModified:\t%s",
             name, type, where[0] ? where : "(unknown)", deleted[0] ? deleted : "-", size, g, modified[0] ? modified : "-");
    char title[FM_NAME + 32]; snprintf(title, sizeof(title), "%.60s Properties", name);
    du_message(a->ui, D_BIN_ITEM, title, text, DU_DLG_ICON_INFO, NULL, 0, 0, 0);
    du_dialog_icon(a->ui, icon32(e));
}
/* Recycle Bin Properties: one setting for all drives or one per drive (the
 * Drive button walks Global, C:, D:, ...), "do not move files", the maximum
 * size as a percentage, and the delete confirmation. */
enum { BP_OK, BP_CANCEL, BP_DRIVE, BP_SCOPE, BP_ONE, BP_EACH, BP_NUKE, BP_SIZE, BP_PERCENT, BP_CONFIRM };
static char bin_scope_drive(struct files_app *a)
{
    char drives[26]; unsigned n = rb_drives(&a->model.fs, a->model.fixed, drives);
    return a->bin_scope > 0 && (unsigned)a->bin_scope <= n ? drives[a->bin_scope - 1] : 0;
}
static void bin_scope_show(struct files_app *a)
{
    struct du_dialog *d = &a->ui->dialog; struct rb_policy *p = &a->bin_edit;
    char drive = bin_scope_drive(a); int i = drive ? drive - 'C' + 2 : -1;
    char text[200], size[48];
    d->items[BP_ONE].hidden = d->items[BP_EACH].hidden = drive != 0;
    d->items[BP_ONE].value = !p->independent; d->items[BP_EACH].value = p->independent;
    snprintf(text, sizeof(text), drive ? "Settings for drive %c:" : "Global settings (used for every drive unless configured independently)", drive);
    du_set_text(&d->items[BP_SCOPE], text);
    d->items[BP_NUKE].value = drive ? p->drive_nuke[i] : p->nuke;
    unsigned percent = drive ? p->drive_percent[i] : p->percent;
    snprintf(text, sizeof(text), "%u", percent); du_set_text(&d->items[BP_PERCENT], text);
    struct rb_policy shown = *p; shown.independent = 1; shown.drive_percent[drive ? i : 2] = percent;
    files_size_text(size, sizeof(size), rb_limit(&shown, drive ? drive : 'C'));
    snprintf(text, sizeof(text), "&Maximum size of the Recycle Bin, percent of %s (%s):", drive ? "the drive" : "each drive", size);
    du_set_text(&d->items[BP_SIZE], text);
    d->items[BP_CONFIRM].value = p->confirm;
    d->items[BP_DRIVE].disabled = 0;
}
/* Reads the controls into bin_edit; returns 0, or 1 when the size is not 1-100. */
static int bin_scope_store(struct files_app *a)
{
    struct du_dialog *d = &a->ui->dialog; struct rb_policy *p = &a->bin_edit;
    char drive = bin_scope_drive(a); int i = drive ? drive - 'C' + 2 : -1;
    if (!drive) p->independent = d->items[BP_EACH].value != 0;
    char *end; long percent = strtol(d->items[BP_PERCENT].text, &end, 10);
    int bad = !d->items[BP_PERCENT].text[0] || *end || percent < 1 || percent > 100;
    if (!bad) { if (drive) p->drive_percent[i] = (unsigned)percent; else p->percent = (unsigned)percent; }
    if (drive) p->drive_nuke[i] = d->items[BP_NUKE].value != 0; else p->nuke = d->items[BP_NUKE].value != 0;
    p->confirm = d->items[BP_CONFIRM].value != 0;
    return bad;
}
static int bin_accept(struct du_ui *u, int button, void *arg)
{
    struct files_app *a = arg; struct du_dialog *d = &u->dialog;
    if (button == BP_CANCEL) return 0;
    int bad = bin_scope_store(a);
    if (bad) { du_set_text(&d->items[BP_SIZE], "The maximum size must be a percentage from 1 to 100:"); d->focus = BP_PERCENT; return 1; }
    if (button == BP_DRIVE) {
        char drives[26]; unsigned n = rb_drives(&a->model.fs, a->model.fixed, drives);
        if (!a->bin_scope && !a->bin_edit.independent) {
            du_set_text(&d->items[BP_SCOPE], "Select \"Configure drives independently\" to set each drive separately.");
            return 1;
        }
        a->bin_scope = (int)((unsigned)(a->bin_scope + 1) % (n + 1));
        bin_scope_show(a); return 1;
    }
    return 0;
}
static struct du_widget *bin_control(struct files_app *a, unsigned index, enum du_kind kind, const char *label, int x, int y, int w, int h)
{
    struct du_dialog *d = &a->ui->dialog; struct du_widget *it = &d->items[index];
    memset(it, 0, sizeof(*it)); it->kind = kind; it->id = -8000 - (int)index; it->total = 1; it->vertical = 1; it->sort = -1; it->focus_rect = 1;
    it->box = (struct du_rect){d->box.x + x, d->box.y + y, w, h}; du_set_text(it, label);
    if (index >= d->count) d->count = index + 1;
    return it;
}
static void bin_properties(struct files_app *a)
{
    if (rb_policy_load(&a->model.fs, &a->bin_edit)) rb_policy_default(&a->bin_edit);
    a->model.fixed = files_fixed(a); a->bin_scope = 0;
    static const char *const buttons[] = {"OK", "Cancel", "&Drive"};
    du_message(a->ui, D_BIN_PROPS, "Recycle Bin Properties", "", 0, buttons, 3, 0, 1);
    struct du_dialog *d = &a->ui->dialog;
    int w = a->width - 8 < 520 ? a->width - 8 : 520, h = 270;
    if (h > a->height - 8) h = a->height - 8;
    d->box = (struct du_rect){(a->width - w) / 2, (a->height - h) / 2, w, h};
    int by = d->box.y + h - 34;
    d->items[BP_OK].box = (struct du_rect){d->box.x + w - 262, by, 75, 23};
    d->items[BP_CANCEL].box = (struct du_rect){d->box.x + w - 181, by, 75, 23};
    d->items[BP_DRIVE].box = (struct du_rect){d->box.x + w - 100, by, 84, 23};
    bin_control(a, BP_SCOPE, DU_LABEL, "", 14, 38, w - 28, 34);
    bin_control(a, BP_ONE, DU_RADIO, "Use &one setting for all drives", 20, 78, w - 40, 20);
    bin_control(a, BP_EACH, DU_RADIO, "Configure drives &independently", 20, 100, w - 40, 20);
    bin_control(a, BP_NUKE, DU_CHECKBOX, "Do not &move files to the Recycle Bin. Remove files immediately when deleted.", 20, 130, w - 40, 36);
    bin_control(a, BP_SIZE, DU_LABEL, "", 14, 172, w - 28, 34);
    bin_control(a, BP_PERCENT, DU_EDIT, "10", 20, 206, 70, 22);
    bin_control(a, BP_CONFIRM, DU_CHECKBOX, "Display &delete confirmation dialog", 110, 207, w - 130, 20);
    d->focus = BP_NUKE; d->accept = bin_accept; d->accept_ctx = a;
    bin_scope_show(a);
}

static void properties(struct files_app *a)
{
    if (fm_is_recycle(a->model.path)) {
        if (fm_selected(&a->model, NULL)) bin_item_properties(a, a->model.active < a->model.count && a->model.entries[a->model.active].selected ? a->model.active : 0);
        else bin_properties(a);
        return;
    }
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
    if (!a->loading && e->id == W_LIST && e->index >= 0 && (unsigned)e->index < m->count) {
        if (!m->entries[e->index].selected) fm_select(m, (unsigned)e->index, 0); else fm_focus(m, (unsigned)e->index);
        const struct fm_entry *it = &m->entries[e->index];
        unsigned n = fm_selected(m, NULL);
        enum fm_kind kind = fm_kind(it);
        if (it->bin) {   /* G2: Restore, Delete, Properties on an item of the Recycle Bin */
            popup_add(a, "R&estore", C_RESTORE, DU_ITEM_DEFAULT);
            popup_add(a, NULL, 0, DU_ITEM_SEPARATOR);
            popup_add(a, "&Delete", C_DELETE, 0);
            popup_add(a, NULL, 0, DU_ITEM_SEPARATOR);
            popup_add(a, "P&roperties", C_PROPERTIES, 0);
            files_update(a);
            du_popup(a->ui, a->popup, a->popup_count, e->x, e->y, e->id);
            return;
        }
        popup_add(a, "&Open", C_OPEN, DU_ITEM_DEFAULT);
        int target=removal_target(a);
        if (target>=3) popup_add(a, removal_label(a,target), C_SAFE_REMOVE, 0);
        if (kind != FM_KIND_FOLDER) popup_add(a, "Open with &CiukNote", C_OPEN_NOTE, 0);
        if (n == 1 && m->path[0] && (kind == FM_KIND_FOLDER || kind == FM_KIND_AUDIO))
            popup_add(a, "Play in &Music Player", C_MUSIC, 0);
        popup_add(a, NULL, 0, DU_ITEM_SEPARATOR);
        popup_add(a, "Cu&t", C_CUT, 0); popup_add(a, "&Copy", C_COPY, 0);
        if (kind == FM_KIND_FOLDER && n == 1) popup_add(a, "&Paste", C_PASTE_INTO, m->clipboard_count ? 0 : DU_ITEM_DISABLED);
        popup_add(a, NULL, 0, DU_ITEM_SEPARATOR);
        popup_add(a, "&Delete", C_DELETE, 0); popup_add(a, "Rena&me", C_RENAME, n == 1 ? 0 : DU_ITEM_DISABLED);
        popup_add(a, NULL, 0, DU_ITEM_SEPARATOR);
        popup_add(a, "P&roperties", C_PROPERTIES, 0);
        files_update(a);
    } else if (e->id == W_LIST && fm_is_recycle(m->path)) {
        if (!(e->modifiers & (DU_CTRL | DU_SHIFT))) fm_select_none(m);
        struct du_widget *list = W(a, W_LIST);
        popup_add(a, "&Empty Recycle Bin", C_EMPTY_BIN, m->count ? 0 : DU_ITEM_DISABLED);
        popup_add(a, NULL, 0, DU_ITEM_SEPARATOR);
        popup_add(a, "Lar&ge Icons", C_ICONS, DU_ITEM_RADIO | (list->view == DU_ICONS ? DU_ITEM_CHECKED : 0));
        popup_add(a, "&List", C_LIST, DU_ITEM_RADIO | (list->view == DU_LIST ? DU_ITEM_CHECKED : 0));
        popup_add(a, "&Details", C_DETAILS, DU_ITEM_RADIO | (list->view == DU_DETAILS ? DU_ITEM_CHECKED : 0));
        popup_add(a, NULL, 0, DU_ITEM_SEPARATOR);
        popup_add(a, "Refres&h", C_REFRESH, 0);
        popup_add(a, NULL, 0, DU_ITEM_SEPARATOR);
        popup_add(a, "P&roperties", C_BIN_PROPS, 0);
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
        int target=removal_target(a);
        if (target>=3) popup_add(a, removal_label(a,target), C_SAFE_REMOVE, 0);
        popup_add(a, NULL, 0, DU_ITEM_SEPARATOR);
        popup_add(a, "P&roperties", C_PROPERTIES, 0);
        fm_select_none(&a->model); files_update(a);
    } else return;
    du_popup(a->ui, a->popup, a->popup_count, e->x, e->y, e->id);
}

static void help(struct files_app *a)
{
    if (!du_help(a->ui, NULL)) return;   /* G15: the Files topic in CiukiOS Help; the box below when Help cannot start */
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
    /* Enter and Ctrl+V may arrive in the same input batch. Preserve Paste
     * for the accepted destination, then use the ordinary fm_job path. A new
     * navigation clears it; no operation can target the hidden old rows. */
    if (a->loading && id == C_PASTE) { a->pending_command = id; return; }
    if (a->loading && (id == C_OPEN || id == C_OPEN_NOTE || id == C_CUT || id == C_COPY || id == C_PASTE ||
        id == C_PASTE_INTO || id == C_DELETE || id == C_RENAME || id == C_PROPERTIES || id == C_NEW_FOLDER || id == C_NEW_TEXT || id == C_SAFE_REMOVE || id == C_MUSIC)) return;
    if (!m->path[0] && (id == C_OPEN_NOTE || id == C_CUT || id == C_COPY || id == C_PASTE ||
        id == C_PASTE_INTO || id == C_DELETE || id == C_DELETE_NOW || id == C_RENAME || id == C_PROPERTIES || id == C_NEW_FOLDER || id == C_NEW_TEXT ||
        id == C_RESTORE || id == C_EMPTY_BIN || id == C_BIN_PROPS)) return;
    int in_bin = fm_is_recycle(m->path);
    /* G2: the bin keeps its items as stored; nothing is created, renamed, cut or pasted there. */
    if (in_bin && (id == C_OPEN_NOTE || id == C_CUT || id == C_COPY || id == C_PASTE || id == C_PASTE_INTO || id == C_RENAME ||
                   id == C_NEW_FOLDER || id == C_NEW_TEXT || id == C_MUSIC || id == C_SAFE_REMOVE)) return;
    if (!in_bin && (id == C_RESTORE || id == C_EMPTY_BIN || id == C_BIN_PROPS)) return;
    switch (id) {
    case C_SAFE_REMOVE: {
        int d=removal_target(a);
        if (d<3) break;
        int optical=a->media.type[d]==CIUKI_MEDIA_OPTICAL;
        int r=a->media_remove((unsigned)d,a->media.generation[d]);
        if (r==-EBUSY) {
            struct ciuki_media_blockers b; char text[1024];
            int have=a->media_blockers && !a->media_blockers((unsigned)d,a->media.generation[d],&b);
            desk_media_blocker_text(text,sizeof(text),have ? &b : NULL,desk_media_dialog_body(a->width,a->height));
            static const char *const buttons[]={"&OK"};
            du_message(a->ui,D_INFO,"Drive is in use",text,DU_DLG_ICON_WARNING,buttons,1,0,0);
        } else if (r) error_box(a,"Safely remove","The drive could not be safely removed:",r);
        else { files_navigate(a,FM_COMPUTER); snprintf(a->note,sizeof(a->note),"%s",optical ? "The disc has been ejected." : "It is safe to unplug the USB device."); files_update(a); }
        break;
    }
    case C_BACK: case C_FORWARD:
        if ((id == C_BACK && m->history_at) || (id == C_FORWARD && m->history_at + 1 < m->history_count))
            scan_submit(a, SCAN_HISTORY, NULL, id == C_BACK ? -1 : 1);
        break;
    case C_UP: {
        char path[FM_PATH];
        snprintf(path, sizeof(path), "%s", a->loading ? a->loading_path : m->path);
        if (path[0]) { if (fm_drive_root(path)) path[0] = 0; else fm_parent(path, path); files_navigate(a, path); }
        break;
    }
    case C_GO_CONTROL: open_control(a); break;
    case C_GO_ROOT: files_navigate(a, FM_COMPUTER); break;
    case C_GO_RECYCLE: files_navigate(a, FM_RECYCLE); break;
    case C_RESTORE: if (fm_selected(m, NULL)) run_bin_selection(a, BIN_RESTORE); break;
    case C_EMPTY_BIN: confirm_empty(a); break;
    case C_BIN_PROPS: bin_properties(a); break;
    case C_GO_BIN: files_navigate(a, "/Programs"); break;
    case C_GO_DESKTOP: files_navigate(a,"/Documents/Desktop"); break;
    case C_GO_DOCUMENTS: files_navigate(a, "/Documents"); break;
    case C_GO_GAMES: files_navigate(a, "/Games"); break;
    case C_GO_SYSTEM: files_navigate(a, "/system"); break;
    case C_TREE_OPEN: files_update(a); break;
    case C_ADDRESS: { struct du_widget *e = W(a, W_ADDRESS); du_focus(a->ui, W_ADDRESS); e->anchor = 0; e->cursor = strlen(e->text); break; }
    case C_CUT: case C_COPY: if (!fm_clipboard(m, id == C_CUT) && !fm_clipboard_save(m)) { a->note[0] = 0; files_update(a); } break;
    case C_PASTE: paste(a, m->path); break;
    case C_PASTE_INTO: {
        char p[FM_PATH];
        if (m->active < m->count && m->entries[m->active].meta.type == FM_DIR && !fm_join(p, m->path, m->entries[m->active].name)) paste(a, p);
        break;
    }
    case C_DELETE: if (in_bin) confirm_purge(a); else confirm_delete(a, 0); break;
    case C_DELETE_NOW: if (in_bin) confirm_purge(a); else confirm_delete(a, RB_PERMANENT); break;   /* Shift+Delete */
    case C_PROPERTIES: properties(a); break;
    case C_VIEWS: { struct du_widget *l = W(a, W_LIST); set_view(a, l->view == DU_ICONS ? DU_LIST : l->view == DU_LIST ? DU_DETAILS : DU_ICONS); break; }
    case C_ICONS: set_view(a, DU_ICONS); break;
    case C_LIST: set_view(a, DU_LIST); break;
    case C_DETAILS: set_view(a, DU_DETAILS); break;
    case C_SORT_NAME: case C_SORT_SIZE: case C_SORT_TYPE: case C_SORT_MODIFIED:
        fm_sort(m, (unsigned)(id - C_SORT_NAME), 0); files_update(a); break;
    case C_REFRESH: {
        if (a->loading) { char path[FM_PATH]; strcpy(path, a->loading_path); files_navigate(a, path); break; }
        for (unsigned d=3;d<26;d++) a->drive_labels[d].valid=0;
        a->note[0] = 0;
        int r = scan_submit(a, SCAN_REFRESH, NULL, 0);
        if (r) error_box(a, "Files", "Cannot refresh the folder:", r);
        break;
    }
    case C_OPEN: if (fm_selected(m, NULL)) open_item(a, m->active < m->count && m->entries[m->active].selected ? m->active : 0, 0); break;
    case C_MUSIC: {
        char path[FM_PATH];
        if (m->active < m->count && !fm_entry_path(m, m->active, path)) {
            char *argv[] = {"music", path, NULL};
            int r = spawn(a, "/Programs/music", argv);
            if (r < 0) error_box(a, "Files", "Cannot start Music Player:", r);
        }
        break;
    }
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
    case D_DELETE:
        if (a->drop_count) { if (e->index == 0) start_bin_job(a, BIN_DELETE, a->drop_paths, a->drop_count, 0); drop_free(a); }
        else if (e->index == 0) run_delete(a);
        break;
    case D_BIN_PURGE: if (e->index == 0) run_bin_selection(a, BIN_PURGE); break;
    case D_BIN_PROPS:
        if (e->index == 0) {
            int r = rb_policy_save(&a->model.fs, &a->bin_edit);
            if (r) error_box(a, "Recycle Bin", "The Recycle Bin settings could not be saved:", r);
            else { snprintf(a->note, sizeof(a->note), "Recycle Bin settings saved"); files_update(a); }
        }
        break;
    case D_BIN_EMPTY: if (e->index == 0) start_bin_job(a, BIN_EMPTY, NULL, 0, 0); break;
    case D_REPLACE:
        transfer_command(a, 0, e->index == 0 ? FM_YES : e->index == 1 ? FM_YES_ALL : e->index == 2 ? FM_NO : FM_CANCEL);
        if (a->job_active) progress(a);
        break;
    case D_PROGRESS: transfer_command(a, 1, -1); if (a->job_active) progress(a); break;
    case D_INSERT: if (e->index == 0) { char p[FM_PATH]; snprintf(p, sizeof(p), "%s", a->insert_path); files_navigate(a, p); } break;
    case D_CANCEL_JOB:
        if (e->index==0) { a->close_pending=1; transfer_command(a,1,-1); }
        else if (a->close_declined) a->close_declined(a);
        if (a->job_active) job_tick(a);
        break;
    default: break;
    }
}

/* The toolkit acknowledges the shell request while confirmation/cleanup waits. */
int files_close_request(struct files_app *a)
{
    if (!a->job_active) return 1;
    if (du_dialog_id(a->ui)!=D_CANCEL_JOB && !a->close_pending)
        du_message(a->ui,D_CANCEL_JOB,"Cancel Operation?","Cancel the current operation and close Files?",DU_DLG_ICON_QUESTION,yes_no,2,1,1);
    return 0;
}

static void list_select(struct files_app *a, const struct du_event *e)
{
    if (a->loading) return;
    struct fm_model *m = &a->model;
    a->note[0] = 0;
    if (e->index < 0) {
        if (!(e->modifiers & (DU_CTRL | DU_SHIFT))) fm_select_none(m);
        files_update(a); return;
    }
    unsigned i = (unsigned)e->index, shift = (e->modifiers & DU_SHIFT) != 0, ctrl = (e->modifiers & DU_CTRL) != 0;
    /* fx-19: a press on a selected item only moves the focus, so the whole
     * selection can be dragged; the click applies at release (Explorer). */
    if (!e->keyboard && e->key == DU_SELECT_PENDING) { fm_focus(m, i); files_update(a); return; }
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
    /* G2: Shift+Delete bypasses the Recycle Bin (the plain Delete is a menu accelerator). */
    if (e->key == DU_KEY_DELETE && (e->modifiers & (DU_CTRL | DU_ALT | DU_SHIFT)) == DU_SHIFT && (focus == W_LIST || focus == W_TREE || focus == W_PLACES)) {
        command(a, C_DELETE_NOW); return;
    }
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

/* ---- fx-19 drag and drop and rubber-band selection ---- */
static void marquee(struct files_app *a, const struct du_event *e)
{
    struct fm_model *m = &a->model; struct du_widget *list = W(a, W_LIST);
    if (a->loading || !m->count || list->count != m->count) return;
    if (m->count > a->band_capacity) {
        uint8_t *o = realloc(a->band_original, m->count), *h = o ? realloc(a->band_hits, m->count) : NULL;
        if (o) a->band_original = o;
        if (!o || !h) return;
        a->band_hits = h; a->band_capacity = m->count;
    }
    if (e->index == 0) {
        /* Without modifiers the press already cleared the selection. Ctrl
         * toggles the items the band covers, Shift adds them (Windows 98). */
        a->band_modifiers = e->modifiers & (DU_CTRL | DU_SHIFT);
        for (unsigned i = 0; i < m->count; i++) a->band_original[i] = (uint8_t)(m->entries[i].selected != 0);
        a->note[0] = 0; return;
    }
    du_list_band_hits(list, a->band_hits);
    int changed = 0, first = -1;
    for (unsigned i = 0; i < m->count; i++) {
        unsigned sel = a->band_modifiers & DU_CTRL ? a->band_original[i] ^ a->band_hits[i] : a->band_original[i] | a->band_hits[i];
        if (a->band_hits[i] && first < 0) first = (int)i;
        if (m->entries[i].selected != sel) { m->entries[i].selected = sel; changed = 1; }
    }
    if (first >= 0 && e->index == 2) { m->active = (unsigned)first; if (!(a->band_modifiers & DU_SHIFT)) m->anchor = (unsigned)first; }
    if (changed || e->index == 2) files_update(a);
}

static int drag_source_paths(struct files_app *a, const struct du_event *e, char ***paths, char **types, unsigned *count)
{
    if (e->id == W_TREE) {
        if (e->index < 0 || (unsigned)e->index >= a->tree_count || fm_drive_root(a->tree[e->index].path)) return -EPERM;
        char **p = calloc(1, sizeof(*p)); char *t = malloc(1);
        if (!p || !t || !(p[0] = malloc(FM_PATH))) { free(p ? p[0] : NULL); free(p); free(t); return -ENOMEM; }
        snprintf(p[0], FM_PATH, "%s", a->tree[e->index].path); t[0] = 'D';
        *paths = p; *types = t; *count = 1; return 0;
    }
    if (!a->model.path[0]) return -EPERM;   /* drives in Computer: link only, unsupported */
    int r = selection_paths(a, paths, count);
    if (r) return r;
    char *t = malloc(*count);
    if (!t) { fm_drag_free(*paths, NULL, *count); return -ENOMEM; }
    unsigned k = 0;
    for (unsigned i = 0; i < a->model.count && k < *count; i++) if (a->model.entries[i].selected)
        t[k++] = a->model.entries[i].meta.type == FM_DIR ? 'D' : 'F';
    *types = t; return 0;
}
static void drag_start(struct files_app *a, const struct du_event *e)
{
    if ((e->id != W_LIST && e->id != W_TREE) || !a->drag_begin || a->loading || a->rename_index >= 0) return;
    if (e->id == W_LIST && fm_is_recycle(a->model.path)) return;   /* G2: items leave the bin through Restore */
    char **paths; char *types; unsigned count;
    if (drag_source_paths(a, e, &paths, &types, &count)) return;
    int r = fm_drag_save(&a->model.fs, paths, types, count);
    fm_drag_free(paths, types, count);
    if (r) { error_box(a, "Files", "Cannot start dragging the selection:", r); return; }
    if (!a->drag_begin(a, e->key == 2 ? 2 : 1, count)) a->note[0] = 0;
}

static void drag_highlight(struct files_app *a, int widget, int index)
{
    int ids[] = {W_LIST, W_TREE, W_PLACES};
    for (unsigned k = 0; k < 3; k++) {
        struct du_widget *w = W(a, ids[k]);
        int hot = ids[k] == widget && index >= 0 ? index + 1 : 0;
        if (w->drop_hot != hot) { w->drop_hot = hot; du_invalidate(a->ui, w->box); }
    }
    a->drag_widget = widget; a->drag_index = index;
}
static int drag_data(struct files_app *a, uint32_t serial)
{
    if (serial == a->drag_serial && a->drag_count) return 0;
    fm_drag_free(a->drag_paths, a->drag_types, a->drag_count);
    a->drag_paths = NULL; a->drag_types = NULL; a->drag_count = 0; a->drag_serial = serial;
    a->drag_hover_node = -1; a->drag_hover_ns = 0;
    return fm_drag_load(&a->model.fs, &a->drag_paths, &a->drag_types, &a->drag_count);
}
static int read_only_volume(const struct files_app *a, int volume)
{ return volume >= 'D' && volume <= 'Z' && a->media.type[volume - 'A'] == CIUKI_MEDIA_OPTICAL; }
/* Destination folder under (x, y), with its highlighted item; 0 when the
 * point is a valid drop location. */
static int drag_destination(struct files_app *a, int x, int y, char destination[FM_PATH], int *widget, int *index)
{
    *widget = 0; *index = -1;
    if (a->loading || a->job_active || du_dialog_id(a->ui) || a->rename_index >= 0) return -EBUSY;
    struct du_widget *list = W(a, W_LIST), *tree = W(a, W_TREE), *places = W(a, W_PLACES);
    if (du_contains(du_list_body(list), x, y)) {
        int i = du_list_hit(list, x, y);
        if (fm_is_recycle(a->model.path)) { *widget = W_LIST; snprintf(destination, FM_PATH, "%s", FM_RECYCLE); return 0; }   /* G2: dropping on the bin deletes */
        if (i >= 0 && (unsigned)i < a->model.count && (a->model.entries[i].meta.type == FM_DIR || !a->model.path[0])) {
            *widget = W_LIST; *index = i;
            return fm_entry_path(&a->model, (unsigned)i, destination);
        }
        if (!a->model.path[0]) return -EPERM;
        *widget = W_LIST;
        snprintf(destination, FM_PATH, "%s", a->model.path); return 0;
    }
    if (du_contains(du_list_body(tree), x, y)) {
        int i = du_list_hit(tree, x, y);
        if (i < 0 || (unsigned)i >= a->tree_count) return -ENOENT;
        *widget = W_TREE; *index = i;
        snprintf(destination, FM_PATH, "%s", a->tree[i].path); return 0;
    }
    if (du_contains(du_list_body(places), x, y)) {
        int i = du_list_hit(places, x, y);
        if (i < 0 || (unsigned)i >= a->place_count || a->places[i].path[0] != '/') return -ENOENT;
        *widget = W_PLACES; *index = i;
        snprintf(destination, FM_PATH, "%s", a->places[i].path); return 0;
    }
    return -ENOENT;
}
static unsigned drag_evaluate(struct files_app *a, int x, int y, unsigned keys, char destination[FM_PATH], unsigned *allowed)
{
    int widget, index; *allowed = 0;
    if (!a->drag_count || drag_destination(a, x, y, destination, &widget, &index)) { drag_highlight(a, 0, -1); return FM_EFFECT_NONE; }
    int source = fm_volume(a->drag_paths[0]), target = fm_volume(destination);
    if (fm_is_recycle(destination)) {
        /* G2: the Recycle Bin window takes a move, never a copy; a CD cannot be deleted from; the bin's own items are in it already. */
        *allowed = read_only_volume(a, source) ? 0 : FM_EFFECT_MOVE;
        for (unsigned i = 0; i < a->drag_count && *allowed; i++) {
            char dir[FM_PATH];
            if (!rb_dir(fm_drive_letter(a->drag_paths[i]), dir) && fm_beneath(a->drag_paths[i], dir)) *allowed = 0;
        }
        unsigned effect = *allowed && !(keys & (FM_KEY_CTRL | FM_KEY_ALT)) ? FM_EFFECT_MOVE : FM_EFFECT_NONE;
        drag_highlight(a, 0, -1);
        return effect;
    }
    /* Copy and move only: CiukiOS has no native file shortcuts (link). A CD
     * source cannot be moved; a CD target accepts nothing. */
    *allowed = read_only_volume(a, target) ? 0 : read_only_volume(a, source) ? FM_EFFECT_COPY : FM_EFFECT_COPY | FM_EFFECT_MOVE;
    for (unsigned i = 0; i < a->drag_count && *allowed; i++) if (a->drag_types[i] == 'D' && fm_beneath(destination, a->drag_paths[i])) *allowed = 0;
    unsigned effect = fm_drop_effect(source, target, keys, *allowed);
    if (effect && fm_drop_check(a->drag_paths, a->drag_count, destination, effect)) effect = FM_EFFECT_NONE;
    /* Only a folder that will receive the items is highlighted. */
    drag_highlight(a, effect ? widget : 0, effect ? index : -1);
    return effect;
}
/* G2: items dropped on the Recycle Bin window are deleted like Delete would. */
static void drop_free(struct files_app *a)
{
    for (unsigned i = 0; a->drop_paths && i < a->drop_count; i++) free(a->drop_paths[i]);
    free(a->drop_paths); a->drop_paths = NULL; a->drop_count = 0;
}
static void drop_delete(struct files_app *a)
{
    drop_free(a);
    a->drop_paths = calloc(a->drag_count, sizeof(*a->drop_paths));
    if (!a->drop_paths) return;
    for (unsigned i = 0; i < a->drag_count; i++) {
        a->drop_paths[i] = strdup(a->drag_paths[i]);
        if (!a->drop_paths[i]) { a->drop_count = i; drop_free(a); return; }
    }
    a->drop_count = a->drag_count;
    if (rb_policy_load(&a->model.fs, &a->policy)) rb_policy_default(&a->policy);
    a->model.fixed = files_fixed(a); a->delete_flags = 0;
    if (!a->policy.confirm) {
        start_bin_job(a, BIN_DELETE, a->drop_paths, a->drop_count, 0); drop_free(a); return;
    }
    char title[64], text[FM_NAME + 200];
    int recycle = rb_classify(&a->policy, a->model.fixed, 0, a->drop_paths, a->drop_count) != 0;
    rb_delete_prompt(a->drop_count, a->drop_count == 1 && a->drag_types[0] == 'D', fm_basename(a->drop_paths[0]), recycle, title, sizeof(title), text, sizeof(text));
    du_message(a->ui, D_DELETE, title, text, recycle ? DU_DLG_ICON_QUESTION : DU_DLG_ICON_WARNING, yes_no, 2, 0, 1);
}
void files_drag_over(struct files_app *a, uint32_t serial, int phase, int x, int y, unsigned keys)
{
    if (phase == 1) {
        drag_highlight(a, 0, -1); a->drag_hover_node = -1;
        if (a->drag_status) a->drag_status(a, serial, FM_EFFECT_NONE, 0);
        return;
    }
    int r = drag_data(a, serial);
    char destination[FM_PATH]; unsigned allowed = 0, effect = FM_EFFECT_NONE;
    if (!r) {
        /* DragOver auto-scroll and the Explorer folder tree's hover expansion. */
        struct du_widget *list = W(a, W_LIST), *tree = W(a, W_TREE);
        if (du_contains(list->box, x, y)) du_list_autoscroll(a->ui, list, x, y);
        else if (du_contains(tree->box, x, y)) du_list_autoscroll(a->ui, tree, x, y);
        effect = drag_evaluate(a, x, y, keys, destination, &allowed);
        if (a->drag_widget == W_TREE && a->drag_index >= 0) {
            if (a->drag_hover_node != a->drag_index) { a->drag_hover_node = a->drag_index; a->drag_hover_ns = a->ui->now; }
            else if (a->ui->now - a->drag_hover_ns >= 1000000000ull && (unsigned)a->drag_index < a->tree_count &&
                     !a->tree[a->drag_index].expanded && a->tree[a->drag_index].children) {
                toggle_node(a, a->drag_index); a->drag_hover_ns = a->ui->now;
            }
        } else a->drag_hover_node = -1;
    } else drag_highlight(a, 0, -1);
    a->drag_effect = effect; a->drag_allowed = allowed;
    if (a->drag_status) a->drag_status(a, serial, effect, allowed);
}
void files_drag_drop(struct files_app *a, uint32_t serial, int x, int y, unsigned effect)
{
    char destination[FM_PATH]; unsigned allowed = 0;
    int r = drag_data(a, serial);
    unsigned keys = effect == FM_EFFECT_MOVE ? FM_KEY_SHIFT : effect == FM_EFFECT_COPY ? FM_KEY_CTRL : FM_KEY_ALT;
    unsigned chosen = r ? FM_EFFECT_NONE : drag_evaluate(a, x, y, keys, destination, &allowed);
    drag_highlight(a, 0, -1);
    if (r) { error_box(a, "Files", "Cannot read the dragged items:", r); return; }
    if (chosen != effect || (effect != FM_EFFECT_COPY && effect != FM_EFFECT_MOVE)) return;
    if (fm_is_recycle(destination)) { drop_delete(a); return; }
    job_record("[files] drop effect=%s items=%u", effect == FM_EFFECT_MOVE ? "move" : "copy", a->drag_count);
    start_job(a, effect == FM_EFFECT_MOVE ? FM_MOVE : FM_COPY, a->drag_paths, a->drag_count, destination);
}
static void drag_refresh(struct files_app *a)
{
    if (a->ui->now >= a->drag_refresh_until) { du_timer_stop(a->ui, T_DRAG_REFRESH); return; }
    if (a->loading || a->job_active || du_dialog_id(a->ui) || W(a, W_LIST)->band || W(a, W_LIST)->drag) return;
    struct files_worker *w = a->worker;
    if (w && !pthread_mutex_trylock(&w->lock)) {
        int idle = !w->busy && !w->pending && !w->result;
        pthread_mutex_unlock(&w->lock);
        if (idle) scan_submit(a, SCAN_REFRESH, NULL, 0);
    }
}
void files_drag_end(struct files_app *a, unsigned effect)
{
    /* A move performed by another window empties this folder later: watch
     * it briefly, as Explorer's change notification would refresh it. */
    if (effect == FM_EFFECT_MOVE) {
        a->drag_refresh_until = a->ui->now + 10000000000ull;
        du_timer_start(a->ui, T_DRAG_REFRESH, 500000000u, 1);
    }
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
    case DU_EV_DRAG: drag_start(a, e); break;
    case DU_EV_MARQUEE: if (e->id == W_LIST) marquee(a, e); break;
    case DU_EV_TICK:
        if (e->id == T_JOB) job_tick(a);
        else if (e->id == T_DRAG_REFRESH) drag_refresh(a);
        else if (e->id == T_BROWSER) scan_poll(a);
        else if (e->id == T_MEDIA && a->media_read && !a->loading && !a->job_active && a->rename_index<0 && !du_dialog_id(a->ui)) {
            struct files_worker *w = a->worker;
            if (w && !pthread_mutex_trylock(&w->lock)) {
                int idle = !w->busy && !w->pending && !w->result;
                pthread_mutex_unlock(&w->lock);
                if (idle) scan_submit(a, SCAN_MEDIA, NULL, 0);
            }
        }
        break;
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
        if (fm_is_recycle(a->model.path)) column = column < 6 ? bin_key_of_header[column] : 0;   /* G2: the bin's Details order differs */
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
    copy_menu(a->file_items, file_template, 11); copy_menu(a->bin_items, bin_template, BIN_ITEMS); copy_menu(a->edit_items, edit_template, 6);
    copy_menu(a->view_items, view_template, 10); copy_menu(a->go_items, go_template, 14); copy_menu(a->help_items, help_template, 3);
    a->menus[0] = (struct du_menu){"&File", a->file_items, 11};
    a->menus[1] = (struct du_menu){"&Edit", a->edit_items, 6};
    a->menus[2] = (struct du_menu){"&View", a->view_items, 10};
    a->menus[3] = (struct du_menu){"&Go", a->go_items, 14};
    a->menus[4] = (struct du_menu){"&Help", a->help_items, 3};
    u->menus = a->menus; u->menu_count = 5;
    struct du_rect zero = {0, 0, 0, 0};
    du_add(u, DU_SEPARATOR, W_BAND1, zero, "");
    for (unsigned i = 0; i < sizeof(toolbar) / sizeof(toolbar[0]); i++) {
        struct du_widget *b = du_add(u, DU_TOOLBUTTON, toolbar[i].id, zero, toolbar[i].label);
        b->icon = named(toolbar[i].icon, 22); b->icon_size = 22;
        b->tip = toolbar[i].label;   /* G15: tooltip after the pointer rests on the button */
        if (toolbar[i].id == C_VIEWS) b->dropdown = 12;
    }
    du_add(u, DU_SEPARATOR, W_TSEP1, zero, ""); du_add(u, DU_SEPARATOR, W_TSEP2, zero, ""); du_add(u, DU_SEPARATOR, W_TSEP3, zero, "");
    du_add(u, DU_SEPARATOR, W_BAND2, zero, "");
    du_add(u, DU_LABEL, W_ADDRESS_LABEL, zero, "A&ddress");
    du_add(u, DU_EDIT, W_ADDRESS, zero, "Computer")->icon = named("place:computer", 16);
    du_add(u, DU_SEPARATOR, W_BAND3, zero, "");
    du_add(u, DU_HEADER, W_PLACES_HEADER, zero, "Places");
    struct du_widget *places = du_add(u, DU_TREE, W_PLACES, zero, "");
    places->selection_external = 1;
    places->focus_rect = 1;
    du_add(u, DU_HEADER, W_FOLDERS_HEADER, zero, "Folders");
    du_add(u, DU_TREE, W_TREE, zero, "")->selection_external = 1;
    du_add(u, DU_SPLITTER, W_SPLIT, zero, "");
    struct du_widget *list = du_add(u, DU_LISTVIEW, W_LIST, zero, "");
    list->selection_external = 1;
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
    u->service=files_service;
    int worker_error = scan_start(a);
    if (worker_error) return worker_error;
    if (files_navigate(a, path ? path : FM_COMPUTER)) {
        du_dialog_close(u);
        if (files_navigate(a, FM_COMPUTER)) return -EIO;
    }
    if (a->media_read && !a->test) du_timer_start(u,T_MEDIA,1000000000u,1);
    return 0;
}

void files_destroy(struct files_app *a)
{
    a->ui->service=NULL;
    du_timer_stop(a->ui,T_DRAG_REFRESH);
    fm_drag_free(a->drag_paths,a->drag_types,a->drag_count); a->drag_paths=NULL; a->drag_types=NULL; a->drag_count=0;
    for (unsigned i=0;a->drop_paths && i<a->drop_count;i++) free(a->drop_paths[i]);
    free(a->drop_paths); a->drop_paths=NULL; a->drop_count=0; free(a->bin_rows); a->bin_rows=NULL;
    free(a->band_original); free(a->band_hits); a->band_original=a->band_hits=NULL; a->band_capacity=0;
    du_timer_stop(a->ui,T_MEDIA);
    du_timer_stop(a->ui,T_BROWSER);
    scan_stop(a);
    du_timer_stop(a->ui,T_JOB);
    transfer_stop(a);
    fm_job_destroy(&a->job);
    fm_destroy(&a->model);
    free(a->rows); free(a->texts); free(a->tree); free(a->tree_items);
    a->rows = NULL; a->texts = NULL; a->tree = NULL; a->tree_items = NULL;
}

void files_drop(struct files_app *a, int x, int y, int copy)
{
    if (a->loading || a->job_active || du_dialog_id(a->ui) || a->rename_index>=0) return;
    char destination[FM_PATH]; snprintf(destination,sizeof(destination),"%s",a->model.path);
    int index=du_list_hit(W(a,W_LIST),x,y);
    if (index>=0 && (unsigned)index<a->model.count && a->model.entries[index].meta.type==FM_DIR)
        fm_entry_path(&a->model,(unsigned)index,destination);
    else {
        index=du_list_hit(W(a,W_TREE),x,y);
        if (index>=0 && (unsigned)index<a->tree_count) snprintf(destination,sizeof(destination),"%s",a->tree[index].path);
        else if (!du_contains(du_list_body(W(a,W_LIST)),x,y)) return;
    }
    int r=fm_clipboard_load(&a->model);
    if (r) { error_box(a,"Paste","Cannot read the file clipboard:",r); return; }
    if (a->model.clipboard_count) start_job(a,copy ? FM_COPY : FM_MOVE,a->model.clipboard,a->model.clipboard_count,destination);
}
