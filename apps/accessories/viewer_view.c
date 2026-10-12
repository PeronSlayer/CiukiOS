/* SPDX-License-Identifier: MIT
 * Image Viewer window on libdeskui: toolbar, painted image area, status bar. */
#include "viewer.h"
#include "../libdeskui/recent.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define TOOLBAR_Y 22
#define TOOLBAR_H 26
#define STATUS_H 20
#define AREA_BG 0x3c4b52u

static int fs_read(void *ctx, const char *path, uint8_t **data, size_t *size, size_t limit)
{
    (void)ctx;
    FILE *f = fopen(path, "rb");
    if (!f) return -errno;
    size_t cap = 65536, n = 0; uint8_t *buffer = malloc(cap);
    int err = buffer ? 0 : -ENOMEM;
    while (!err) {
        if (n == cap) {
            if (cap >= limit) { err = -EFBIG; break; }
            cap = cap * 2 > limit ? limit : cap * 2;
            uint8_t *grown = realloc(buffer, cap);
            if (!grown) { err = -ENOMEM; break; }
            buffer = grown;
        }
        size_t got = fread(buffer + n, 1, cap - n, f);
        n += got;
        if (!got) { if (ferror(f)) err = -EIO; break; }
    }
    fclose(f);
    if (err) { free(buffer); return err; }
    *data = buffer; *size = n;
    return 0;
}
static const struct viewer_fs native = { fs_read, du_posix_lister, NULL };
const struct viewer_fs *viewer_native_fs(void) { return &native; }

struct du_rect viewer_area(const struct viewer_app *a)
{
    const struct du_canvas *c = &a->ui->canvas;
    return (struct du_rect){2, TOOLBAR_Y + TOOLBAR_H + 2, c->width - 4, c->height - (TOOLBAR_Y + TOOLBAR_H + 4) - STATUS_H};
}
static int effective_zoom(const struct viewer_app *a)
{
    struct du_rect r = viewer_area(a);
    return a->zoom ? a->zoom : viewer_fit_percent(&a->image, r.w, r.h);
}
static void clamp_scroll(struct viewer_app *a)
{
    struct du_rect r = viewer_area(a); int z = effective_zoom(a);
    int max_x = a->image.width - (int)((int64_t)r.w * 100 / z), max_y = a->image.height - (int)((int64_t)r.h * 100 / z);
    if (a->scroll_x > max_x) a->scroll_x = max_x;
    if (a->scroll_y > max_y) a->scroll_y = max_y;
    if (a->scroll_x < 0) a->scroll_x = 0;
    if (a->scroll_y < 0) a->scroll_y = 0;
}
static void status(struct viewer_app *a)
{
    char text[256];
    if (a->loaded)
        snprintf(text, sizeof(text), "%.80s   %d x %d %s   %d%%%s   %d of %u", acc_basename(a->path), a->image.width, a->image.height,
                 a->image.format, effective_zoom(a), a->zoom ? "" : " (fit)", a->index + 1, a->count);
    else snprintf(text, sizeof(text), "%s", a->error[0] ? a->error : "Open an image with File > Open, or from Files.");
    du_set_text(du_get(a->ui, V_STATUS), text);
    int many = a->count > 1;
    du_get(a->ui, V_PREV)->disabled = du_get(a->ui, V_NEXT)->disabled = (uint8_t)!many;
    a->file_items[1].flags = a->file_items[2].flags = many ? 0 : DU_ITEM_DISABLED;
    a->ui->dirty = 1;
}
void viewer_layout(struct viewer_app *a)
{
    struct du_canvas *c = &a->ui->canvas;
    static const int ids[6] = { V_PREV, V_NEXT, V_OUT, V_IN, V_FIT, V_ACTUAL };
    static const int widths[6] = { 70, 70, 70, 70, 60, 84 };
    int x = 4;
    for (int i = 0; i < 6; i++) { du_get(a->ui, ids[i])->box = (struct du_rect){x, TOOLBAR_Y + 2, widths[i], TOOLBAR_H - 4}; x += widths[i] + 4 + (i == 1 || i == 3) * 8; }
    du_get(a->ui, V_STATUS)->box = (struct du_rect){0, c->height - STATUS_H, c->width, STATUS_H};
}

static void resize_layout(struct du_ui *u, void *user)
{
    (void)u;
    struct viewer_app *a = user;
    viewer_layout(a); clamp_scroll(a); status(a);
}

static int by_name(const void *x, const void *y) { return strcasecmp(x, y); }
static void scan_folder(struct viewer_app *a)
{
    a->count = 0; a->index = 0;
    if (!a->fs->list) return;
    if (!a->entries) a->entries = calloc(DU_FILE_ENTRIES, sizeof(*a->entries));
    unsigned n = 0;
    if (!a->entries || a->fs->list(a->fs->ctx, a->folder, a->entries, DU_FILE_ENTRIES, &n)) n = 0;
    for (unsigned i = 0; i < n && a->count < VIEWER_FOLDER; i++)
        if (!a->entries[i].directory && viewer_is_image_name(a->entries[i].name) && strlen(a->entries[i].name) < sizeof(a->names[0]))
            strcpy(a->names[a->count++], a->entries[i].name);
    qsort(a->names, a->count, sizeof(a->names[0]), by_name);
    const char *base = acc_basename(a->path);
    for (unsigned i = 0; i < a->count; i++) if (!strcmp(a->names[i], base)) a->index = (int)i;
}

int viewer_open(struct viewer_app *a, const char *path)
{
    if (!path || strlen(path) >= sizeof(a->path)) return -ENAMETOOLONG;
    uint8_t *data = NULL; size_t size = 0;
    int err = a->fs->read(a->fs->ctx, path, &data, &size, VIEWER_MAX_FILE);
    struct viewer_image image;
    if (!err) err = viewer_decode(data, size, path, &image);
    free(data);
    int same_folder = !strncmp(a->path, path, strlen(a->folder)) && a->folder[0];
    snprintf(a->path, sizeof(a->path), "%s", path);
    char *slash = strrchr(a->path, '/');
    char folder[VIEWER_PATH];
    if (slash == a->path) strcpy(folder, "/");
    else if (slash) snprintf(folder, sizeof(folder), "%.*s", (int)(slash - a->path), a->path);
    else strcpy(folder, ".");
    if (!same_folder || strcmp(folder, a->folder)) { snprintf(a->folder, sizeof(a->folder), "%s", folder); scan_folder(a); }
    else for (unsigned i = 0; i < a->count; i++) if (!strcmp(a->names[i], acc_basename(a->path))) a->index = (int)i;
    viewer_free(&a->image); a->loaded = 0; a->zoom = 0; a->scroll_x = a->scroll_y = 0;
    if (err) {
        snprintf(a->error, sizeof(a->error), "%.80s: %s", acc_basename(path),
                 err == -EINVAL ? "not a supported image (PNG, JPEG, BMP, GIF)" : err == -EFBIG ? "the image is too large" :
                 err == -ENOMEM ? "not enough memory" : err == -ENOENT ? "the file was not found" : "the file could not be read");
    } else { a->image = image; a->loaded = 1; a->error[0] = 0; du_recent_add(path); }
    status(a);
    return err;
}

static void step(struct viewer_app *a, int direction)
{
    if (a->count < 2) return;
    int i = (a->index + direction + (int)a->count) % (int)a->count;
    char path[VIEWER_PATH];
    snprintf(path, sizeof(path), "%s%s%s", a->folder, strcmp(a->folder, "/") ? "/" : "", a->names[i]);
    viewer_open(a, path);
}
static void zoom(struct viewer_app *a, int z)
{
    if (!a->loaded) return;
    struct du_rect r = viewer_area(a);
    int old = effective_zoom(a);
    /* Keep the centre of the view in place. */
    int cx = a->scroll_x + (int)((int64_t)r.w * 50 / old), cy = a->scroll_y + (int)((int64_t)r.h * 50 / old);
    a->zoom = z;
    int nz = effective_zoom(a);
    a->scroll_x = cx - (int)((int64_t)r.w * 50 / nz); a->scroll_y = cy - (int)((int64_t)r.h * 50 / nz);
    clamp_scroll(a); status(a);
}

void viewer_paint(struct du_ui *u, void *user)
{
    struct viewer_app *a = user; struct du_canvas *c = &u->canvas;
    struct du_rect area = viewer_area(a), frame = {area.x - 2, area.y - 2, area.w + 4, area.h + 4};
    du_edge(c, frame, DU_SUNKEN);
    du_fill(c, area, AREA_BG);
    c->clip = area;
    if (a->loaded) viewer_blit(c, area, &a->image, effective_zoom(a), a->scroll_x, a->scroll_y);
    else {
        const char *line1 = a->error[0] ? "The image cannot be shown." : "CiukiOS Image Viewer";
        const char *line2 = a->error[0] ? a->error : "PNG, JPEG, BMP and GIF pictures";
        du_text(c, area.x + (area.w - du_text_width(line1)) / 2, area.y + area.h / 2 - 12, line1, 0xffffffu);
        du_text(c, area.x + (area.w - du_text_width(line2)) / 2, area.y + area.h / 2 + 4, line2, 0xdbe5e7u);
    }
    c->clip = (struct du_rect){0, 0, c->width, c->height};
}

static const struct du_menu_item file_template[] = {
    {"&Open...", V_M_OPEN, 0x18, DU_CTRL, 0}, {"&Previous Image", V_M_PREV, DU_KEY_PGUP, 0, DU_ITEM_DISABLED},
    {"&Next Image", V_M_NEXT, DU_KEY_PGDN, 0, DU_ITEM_DISABLED}, {"", 0, 0, 0, DU_ITEM_SEPARATOR}, {"E&xit", V_M_EXIT, 0, 0, 0} };
static const struct du_menu_item view_template[] = {
    {"Zoom &In", V_M_IN, 0, 0, 0}, {"Zoom &Out", V_M_OUT, 0, 0, 0}, {"", 0, 0, 0, DU_ITEM_SEPARATOR},
    {"&Best Fit", V_M_FIT, 0, 0, 0}, {"&Actual Size", V_M_ACTUAL, 0, 0, 0}, {"", 0, 0, 0, DU_ITEM_SEPARATOR} };
static const struct du_menu_item help_template[] = { {"&About Image Viewer", V_M_ABOUT, 0, 0, 0} };

int viewer_attach(struct viewer_app *a, struct du_ui *u, const struct viewer_fs *fs)
{
    a->ui = u; a->fs = fs ? fs : &native; u->event = viewer_event; u->user = a; u->paint = viewer_paint; u->layout = resize_layout;
    memcpy(a->file_items, file_template, sizeof(file_template));
    memcpy(a->view_items, view_template, sizeof(view_template));
    memcpy(a->help_items, help_template, sizeof(help_template));
    a->menus[0] = (struct du_menu){"&File", a->file_items, 5};
    a->menus[1] = (struct du_menu){"&View", a->view_items, 5};
    a->menus[2] = (struct du_menu){"&Help", a->help_items, 1};
    u->menus = a->menus; u->menu_count = 3;
    struct du_rect zero = {0, 0, 0, 0};
    du_add(u, DU_BUTTON, V_PREV, zero, "< &Prev"); du_add(u, DU_BUTTON, V_NEXT, zero, "&Next >");
    du_add(u, DU_BUTTON, V_OUT, zero, "Zoom -"); du_add(u, DU_BUTTON, V_IN, zero, "Zoom +");
    du_add(u, DU_BUTTON, V_FIT, zero, "&Fit"); du_add(u, DU_BUTTON, V_ACTUAL, zero, "&Actual");
    du_add(u, DU_STATUS, V_STATUS, zero, "");
    viewer_layout(a); status(a);
    u->focus = -1;
    return 0;
}
void viewer_detach(struct viewer_app *a) { viewer_free(&a->image); free(a->entries); a->entries = NULL; }

void viewer_event(struct du_ui *u, const struct du_event *e, void *user)
{
    struct viewer_app *a = user;
    int id = e->id;
    switch (e->type) {
    case DU_EV_DIALOG:
        if (id == V_D_OPEN && !e->index && e->text && e->text[0]) viewer_open(a, e->text);
        break;
    case DU_EV_ACTIVATE:
        switch (id) {
        case V_M_OPEN: du_file_dialog(u, V_D_OPEN, 0, "Open", a->folder[0] ? a->folder : "/", "", a->fs->list, a->fs->ctx); break;
        case V_M_PREV: case V_PREV: step(a, -1); break;
        case V_M_NEXT: case V_NEXT: step(a, 1); break;
        case V_M_IN: case V_IN: zoom(a, viewer_zoom_step(effective_zoom(a), 1)); break;
        case V_M_OUT: case V_OUT: zoom(a, viewer_zoom_step(effective_zoom(a), -1)); break;
        case V_M_FIT: case V_FIT: zoom(a, 0); break;
        case V_M_ACTUAL: case V_ACTUAL: zoom(a, 100); break;
        case V_M_EXIT: if (a->quit) a->quit(a->host); break;
        case V_M_ABOUT:
            du_message(u, V_D_INFO, "About Image Viewer", "CiukiOS Image Viewer - A modern Retro OS\n"
                       "PNG, JPEG, BMP and GIF by stb_image v2.30\n(MIT or Public Domain). Viewer code: MIT.", DU_DLG_ICON_INFO, NULL, 0, 0, 0);
            break;
        default: break;
        }
        if (id >= V_PREV) u->focus = -1;
        break;
    case DU_EV_KEY: {
        struct du_rect r = viewer_area(a); int z = effective_zoom(a);
        int big = a->loaded && ((int64_t)a->image.width * z / 100 > r.w || (int64_t)a->image.height * z / 100 > r.h);
        int pan = (int)((int64_t)32 * 100 / z);
        if (e->ch == '+' || e->ch == '=' || e->key == 0x4e) zoom(a, viewer_zoom_step(z, 1));
        else if (e->ch == '-' || e->key == 0x4a) zoom(a, viewer_zoom_step(z, -1));
        else if (e->ch == 'f' || e->ch == 'F') zoom(a, 0);
        else if (e->ch == '1') zoom(a, 100);
        else if (e->key == DU_KEY_PGUP || (!big && e->key == DU_KEY_LEFT)) step(a, -1);
        else if (e->key == DU_KEY_PGDN || e->ch == ' ' || (!big && e->key == DU_KEY_RIGHT)) step(a, 1);
        else if (big && (e->key == DU_KEY_LEFT || e->key == DU_KEY_RIGHT || e->key == DU_KEY_UP || e->key == DU_KEY_DOWN)) {
            a->scroll_x += e->key == DU_KEY_LEFT ? -pan : e->key == DU_KEY_RIGHT ? pan : 0;
            a->scroll_y += e->key == DU_KEY_UP ? -pan : e->key == DU_KEY_DOWN ? pan : 0;
            clamp_scroll(a);
        } else if (e->key == DU_KEY_HOME) { a->scroll_x = a->scroll_y = 0; }
        else return;
        break;
    }
    default: return;
    }
    u->dirty = 1;
}
