/* SPDX-License-Identifier: MIT
 * Read-only enumeration and preview of the installed trusted font store. */
#include "control.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static const char *directory(struct cp_app *a) { return a->host.fonts_path ? a->host.fonts_path : "/System/fonts"; }
void cp_fonts_close(struct cp_app *a)
{
    ciuki_font_close(a->font_face); a->font_face = NULL;
    a->font_count = 0; a->fonts_loaded = 0;
}
static int path(struct cp_app *a, const char *name, char *out, size_t cap)
{
    int n = snprintf(out, cap, "%s/%s", directory(a), name);
    return n < 0 || (size_t)n >= cap ? -1 : 0;
}
static void select_face(struct cp_app *a, unsigned selected)
{
    ciuki_font_close(a->font_face); a->font_face = NULL;
    a->font_selected = selected;
    for (unsigned i = 0; i < a->font_count; i++) a->font_items[i].selected = i == selected;
    char filename[512];
    if (selected < a->font_count && !path(a, a->fonts[selected].file, filename, sizeof(filename)) &&
        !ciuki_font_open(filename, &a->font_face))
        snprintf(a->font_status, sizeof(a->font_status), "%u installed fonts. Preview at 96 dpi.", a->font_count);
    else snprintf(a->font_status, sizeof(a->font_status), "Font preview unavailable: %s", strerror(errno));
}
static int before(const struct cp_font_entry *a, const struct cp_font_entry *b)
{
    int n = strcmp(a->family, b->family);
    if (!n) n = strcmp(a->style, b->style);
    if (!n) n = strcmp(a->file, b->file);
    return n < 0;
}
static void enumerate(struct cp_app *a)
{
    a->fonts_loaded = 1; a->font_count = 0; a->font_selected = 0;
    DIR *d = opendir(directory(a));
    if (!d) { snprintf(a->font_status, sizeof(a->font_status), "Font store unavailable: %s", strerror(errno)); return; }
    struct dirent *entry; unsigned skipped = 0;
    while ((entry = readdir(d))) {
        size_t n = strlen(entry->d_name);
        if (n < 5 || strcasecmp(entry->d_name + n - 4, ".ttf")) continue;
        char filename[512]; struct ciuki_font_face *face;
        if (n >= sizeof(a->fonts[0].file) || a->font_count == CP_FONTS_MAX ||
            path(a, entry->d_name, filename, sizeof(filename)) || ciuki_font_open(filename, &face)) { skipped++; continue; }
        struct ciuki_font_metrics metrics;
        ciuki_font_get_metrics(face, &metrics); ciuki_font_close(face);
        struct cp_font_entry value = {0};
        memcpy(value.family, metrics.family, sizeof(value.family)); memcpy(value.style, metrics.style, sizeof(value.style));
        memcpy(value.file, entry->d_name, n + 1);
        unsigned at = a->font_count++;
        while (at && before(&value, &a->fonts[at - 1])) { a->fonts[at] = a->fonts[at - 1]; at--; }
        a->fonts[at] = value;
    }
    closedir(d);
    for (unsigned i = 0; i < a->font_count; i++)
        a->font_items[i] = (struct du_item){.text = {a->fonts[i].family, a->fonts[i].style, a->fonts[i].file}};
    if (a->font_count) select_face(a, 0);
    else snprintf(a->font_status, sizeof(a->font_status), "No readable TrueType fonts installed.");
    if (skipped) snprintf(a->font_status, sizeof(a->font_status), "%u installed fonts; %u unavailable or beyond the list limit.", a->font_count, skipped);
}
void cp_fonts_build(struct cp_app *a)
{
    if (!a->fonts_loaded) enumerate(a);
    struct du_widget *w = du_add(a->ui, DU_LISTVIEW, CP_FONT_LIST, (struct du_rect){24, 164, CP_WIDTH - 48, 66}, "");
    w->items = a->font_items; w->count = a->font_count; w->view = DU_DETAILS; w->active = (int)a->font_selected;
    w->inactive_selection = 1; w->column_count = 3; w->anchors = DU_ANCHOR_GROW_X;
    w->columns[0] = (struct du_column){"Family", 132, 0}; w->columns[1] = (struct du_column){"Style", 90, 0};
    w->columns[2] = (struct du_column){"File", 268, 0};
}
int cp_fonts_event(struct cp_app *a, const struct du_event *e)
{
    if ((a->page != CP_DISPLAY || a->display_tab != CP_APPEARANCE) || e->id != CP_FONT_LIST) return 0;
    if ((e->type == DU_EV_SELECT || e->type == DU_EV_OPEN) && e->index >= 0 && (unsigned)e->index < a->font_count) {
        select_face(a, (unsigned)e->index); a->ui->dirty = 1;
    }
    return 1;
}
void cp_fonts_paint(struct cp_app *a, struct du_canvas *c)
{
    static const char sample[] = "Classic computing, one woof at a time.";
    du_text_fit(c, 24, 236, c->width - 48, a->font_status, DU_INK);
    struct du_rect box = {24, 258, c->width - 48, 92};
    du_well(c, box, DU_WINDOW);
    struct du_canvas preview = *c;
    preview.clip = du_intersection(c->clip, (struct du_rect){box.x + 4, box.y + 4, box.w - 8, box.h - 8});
    du_text(&preview, 32, 262, "10 pt", DU_INK); du_text(&preview, 32, 305, "14 pt", DU_INK);
    if (a->font_face && (du_text_face(&preview, a->font_face, 853, 32, 286, sample, DU_INK) ||
                         du_text_face(&preview, a->font_face, 1195, 32, 335, sample, DU_INK))) {
        du_text_fit(&preview, 32, 270, box.w - 16, "Font rendering failed.", DU_INK);
    }
}
