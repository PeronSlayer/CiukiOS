/* System Monitor window. SPDX-License-Identifier: MIT */
#include "sysmon_view.h"
#include "../desktop/profile.h" /* sec-user-04: My Documents is per user */
#include "sysui.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Same eight colours as sm_add's palette (monitor.c). */
const uint32_t smv_palette[8] = { 0x0000cc, 0xcc0000, 0x00aa00, 0xaa00aa, 0x00aaaa, 0xaaaa00, 0x808080, 0x000000 };
const char *const smv_palette_names[8] = { "Blue", "Red", "Green", "Purple", "Teal", "Olive", "Gray", "Black" };
static const struct du_column legend_columns[3] = { { "Item", 190, 0 }, { "Value", 150, 1 }, { "Maximum", 150, 1 } };
static const struct du_column add_columns[2] = { { "Category", 120, 0 }, { "Item", 230, 0 } };
static const struct du_column color_columns[1] = { { "Color", 130, 0 } };

static void swatch_fill(uint32_t *px, uint32_t rgb)
{
    for (unsigned y = 0; y < 16; y++) for (unsigned x = 0; x < 16; x++) {
        int edge = x < 2 || y < 2 || x > 13 || y > 13;
        px[y * 16 + x] = !x || !y || x == 15 || y == 15 ? 0u : edge ? 0xff404040u : 0xff000000u | rgb;
    }
}
static void status(struct sysmon_app *a)
{
    char text[320], third[160];
    if (a->message[0]) snprintf(third, sizeof(third), "%s", a->message);
    else if (a->mon.logging) snprintf(third, sizeof(third), "Logging to %s (%u rows)", a->log_path, a->mon.log_rows);
    else third[0] = 0;
    snprintf(text, sizeof(text), "%u item(s)\tUpdate every %u s\t%s", a->mon.count, a->mon.interval_ms / 1000, third);
    sysui_text(a->ui, SMV_STATUS, text);
}
static void set_message(struct sysmon_app *a, const char *m)
{ snprintf(a->message, sizeof(a->message), "%s", m); status(a); }

static void menu_state(struct sysmon_app *a)
{
    int has = a->selected >= 0 && (unsigned)a->selected < a->mon.count;
    a->edit_items[1].flags = a->mon.count < SM_CHART_ITEMS ? 0 : DU_ITEM_DISABLED;
    a->edit_items[2].flags = has ? 0 : DU_ITEM_DISABLED;
    a->edit_items[3].flags = has ? 0 : DU_ITEM_DISABLED;
    a->edit_items[4].flags = has ? 0 : DU_ITEM_DISABLED;
    a->file_items[0].flags = a->mon.logging ? DU_ITEM_DISABLED : 0;
    a->file_items[1].flags = a->mon.logging ? 0 : DU_ITEM_DISABLED;
    for (unsigned i = 0; i < 3; i++) a->view_items[i].flags = (int)i == a->mon.chart ? DU_ITEM_CHECKED : 0;
}
static void legend(struct sysmon_app *a)
{
    struct du_widget *w = du_get(a->ui, SMV_LEGEND);
    for (unsigned i = 0; i < a->mon.count; i++) {
        const struct sm_item *it = &a->mon.items[i];
        char full[96]; sm_format(&a->mon, i, full, sizeof(full));
        size_t skip = strlen(sm_name(it->counter)) + 2;
        snprintf(a->cells[i][0], sizeof(a->cells[i][0]), "%s", sm_name(it->counter));
        snprintf(a->cells[i][1], sizeof(a->cells[i][1]), "%s", strlen(full) > skip ? full + skip : "");
        if (it->scale) snprintf(a->cells[i][2], sizeof(a->cells[i][2]), "%llu (fixed)", (unsigned long long)it->scale);
        else snprintf(a->cells[i][2], sizeof(a->cells[i][2]), "%llu (auto)", (unsigned long long)(it->peak ? it->peak : 1));
        swatch_fill(a->swatch[i], it->color);
        struct du_item *li = &a->legend_items[i];
        memset(li, 0, sizeof(*li));
        for (unsigned c = 0; c < 3; c++) li->text[c] = a->cells[i][c];
        li->icon16 = a->swatch[i];
    }
    w->items = a->legend_items; w->count = a->mon.count;
    if (a->selected >= (int)a->mon.count) a->selected = (int)a->mon.count - 1;
    w->active = a->selected >= 0 ? (unsigned)a->selected : 0; w->selection_active = a->selected >= 0;
    menu_state(a);
}
static void repaint(struct sysmon_app *a)
{ du_invalidate_widget(a->ui, SMV_CHART); du_invalidate_widget(a->ui, SMV_LEGEND); }

/* ---- painting: integer only ---- */
static void line(struct du_canvas *c, int x0, int y0, int x1, int y1, uint32_t color, int thick)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y1 - y0 : y0 - y1, sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx - dy;
    for (;;) {
        du_fill(c, (struct du_rect){ x0, y0, 1, 1 }, color);
        if (thick) du_fill(c, (struct du_rect){ x0, y0 + 1, 1, 1 }, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}
void smv_paint(struct du_ui *u, void *user)
{
    struct sysmon_app *a = user; struct du_canvas *c = &u->canvas;
    struct du_rect clip = c->clip, b = du_get(u, SMV_CHART)->box;
    c->clip = du_intersection(clip, b);
    if (c->clip.w <= 0 || c->clip.h <= 0) { c->clip = clip; return; }
    du_fill(c, b, DU_WINDOW); du_edge(c, b, DU_SUNKEN);
    struct du_rect in = { b.x + 3, b.y + 3, b.w - 6, b.h - 6 };
    const struct sm_monitor *m = &a->mon;
    if (m->chart == SM_NUMERIC) {
        for (unsigned i = 0; i < m->count; i++) {
            int y = in.y + 6 + (int)i * 24;
            du_fill(c, (struct du_rect){ in.x + 8, y, 14, 14 }, m->items[i].color);
            du_text(c, in.x + 30, y + 2, a->cells[i][0], DU_INK);
            du_text(c, in.x + in.w / 2, y + 2, a->cells[i][1], DU_INK);
        }
    } else if (m->chart == SM_BAR) {
        int n = (int)m->count; if (n) {
            int slot = in.w / n, bar = slot > 24 ? slot - 12 : slot - 2, base = in.y + in.h - 16, top = in.y + 16;
            for (int i = 0; i < n; i++) {
                const struct sm_item *it = &m->items[i];
                unsigned pct = it->count ? it->history[it->count - 1] : 0;
                int h = (base - top) * (int)pct / 100, x = in.x + i * slot + (slot - bar) / 2;
                du_fill(c, (struct du_rect){ x, base - h, bar, h }, it->color);
                du_outline(c, (struct du_rect){ x, top, bar, base - top }, DU_SHADOW);
                du_text_fit(c, x, base + 3, slot - 2, a->cells[i][0], DU_INK);
            }
        }
    } else {
        for (int g = 1; g < 4; g++) du_hline(c, in.x, in.y + in.h * g / 4, in.w, DU_FACE);
        for (int g = 1; g < 10; g++) du_vline(c, in.x + in.w * g / 10, in.y, in.h, DU_FACE);
        for (unsigned i = 0; i < m->count; i++) {
            const struct sm_item *it = &m->items[i];
            int thick = (int)i == a->selected;
            for (unsigned k = 1; k < it->count; k++) {
                unsigned s0 = SM_HISTORY - it->count + k - 1, s1 = s0 + 1;
                int x0 = in.x + (in.w - 1) * (int)s0 / (SM_HISTORY - 1), x1 = in.x + (in.w - 1) * (int)s1 / (SM_HISTORY - 1);
                int y0 = in.y + in.h - 2 - (in.h - 4) * it->history[k - 1] / 100, y1 = in.y + in.h - 2 - (in.h - 4) * it->history[k] / 100;
                line(c, x0, y0, x1, y1, it->color, thick);
            }
        }
        if (a->selected >= 0 && (unsigned)a->selected < m->count) {
            const struct sm_item *it = &m->items[a->selected];
            char top[32]; snprintf(top, sizeof(top), "%llu", (unsigned long long)(it->scale ? it->scale : it->peak ? it->peak : 1));
            du_text(c, in.x + 3, in.y + 2, top, DU_INK);
        }
    }
    c->clip = clip;
}

/* ---- sampling and logging ---- */
void smv_stop_logging(struct sysmon_app *a)
{
    if (a->mon.logging || a->log_path[0]) a->host.log_close(a->host.ctx);
    a->mon.logging = 0; a->log_path[0] = 0; menu_state(a);
}
static void log_failed(struct sysmon_app *a, const char *why)
{
    smv_stop_logging(a);
    set_message(a, "Logging stopped.");
    sysui_box(a->ui, SMD_MESSAGE, "System Monitor", why, DU_DLG_ICON_WARNING);
}
void smv_sample(struct sysmon_app *a)
{
    struct sm_raw raw;
    int e = a->host.sample(a->host.ctx, &raw);
    if (e) {
        char t[96]; snprintf(t, sizeof(t), "Counters unavailable (error %d).", -e);
        set_message(a, t); return;
    }
    if (a->message[0] && !strncmp(a->message, "Counters unavailable", 20)) a->message[0] = 0;
    a->raw = raw; a->have_raw = 1;
    size_t used = 0, cap = a->log_capacity && a->log_capacity < sizeof(a->log_row) ? a->log_capacity : sizeof(a->log_row);
    int r = sm_sample(&a->mon, &raw, a->log_row, cap, &used);
    if (r == -ENOSPC) log_failed(a, "The log row does not fit the log buffer, so logging was stopped.\nThe charts keep running.");
    else if (a->mon.logging && used && a->host.log_write(a->host.ctx, a->log_row, used))
        log_failed(a, "The log file could not be written, so logging was stopped.\nThe charts keep running.");
    legend(a); repaint(a); status(a);
}
static void start_logging(struct sysmon_app *a, const char *path)
{
    size_t used = 0;
    if (a->host.log_open(a->host.ctx, path)) { sysui_box(a->ui, SMD_MESSAGE, "Start Logging", "The log file could not be created.", DU_DLG_ICON_ERROR); return; }
    snprintf(a->log_path, sizeof(a->log_path), "%s", path);
    if (sm_log_header(&a->mon, a->log_row, sizeof(a->log_row), &used) || a->host.log_write(a->host.ctx, a->log_row, used)) {
        log_failed(a, "The log header could not be written."); return;
    }
    a->message[0] = 0; menu_state(a); status(a);
}

/* ---- dialogs ---- */
static void sheet_text(struct du_ui *u, const char *text)
{ snprintf(u->dialog.text, sizeof(u->dialog.text), "%s", text); du_invalidate(u, u->dialog.box); }
static void add_changed(struct du_ui *u, const struct du_event *e, void *user)
{
    struct sysmon_app *a = user;
    if (e->type == DU_EV_SELECT && e->id == 501) sheet_text(u, "Select the item to add, then click Explain\nfor its definition.");
    else if (e->type == DU_EV_ACTIVATE && e->id == 502) {
        unsigned c = u->dialog.items[4].active; char wrapped[320];
        if (c >= SM_COUNTERS) return;
        sysui_wrap(wrapped, sizeof(wrapped), sm_explain((int)c), 52);
        char text[420]; snprintf(text, sizeof(text), "%s:\n%s", sm_name((int)c), wrapped);
        sheet_text(u, text);
    }
    (void)a;
}
static int add_accept(struct du_ui *u, int button, void *user)
{
    struct sysmon_app *a = user;
    if (button != 0) return 0;
    unsigned c = u->dialog.items[4].active;
    int r = sm_add(&a->mon, (int)c, a->have_raw ? &a->raw : &(struct sm_raw){ 0 });
    if (r == -ENODEV) { sheet_text(u, "This counter is not available on this computer:\nthe system does not report it."); return 1; }
    if (r == -EEXIST) { sheet_text(u, "This item is already on the chart."); return 1; }
    if (r == -ENOSPC) { sheet_text(u, "The chart is full (8 items). Remove an item first."); return 1; }
    if (r) { sheet_text(u, "The item could not be added."); return 1; }
    a->selected = (int)a->mon.count - 1; legend(a); repaint(a); status(a);
    return 0;
}
static const struct du_item add_page[1] = { { .text = { "Item" } } };
static void open_add(struct sysmon_app *a)
{
    struct du_ui *u = a->ui;
    if (a->mon.logging) { sysui_box(u, SMD_MESSAGE, "Add Item", "Stop logging before changing the items on the chart.", DU_DLG_ICON_INFO); return; }
    for (unsigned i = 0; i < SM_COUNTERS; i++) {
        int ok = !a->have_raw || (a->raw.have & (1u << i));
        snprintf(a->add_cells[i][0], sizeof(a->add_cells[i][0]), "%s", sm_category((int)i));
        snprintf(a->add_cells[i][1], sizeof(a->add_cells[i][1]), "%s%s", sm_name((int)i), ok ? "" : " (not available)");
        memset(&a->add_items[i], 0, sizeof(a->add_items[i]));
        a->add_items[i].text[0] = a->add_cells[i][0]; a->add_items[i].text[1] = a->add_cells[i][1]; a->add_items[i].ghost = !ok;
    }
    du_property_sheet(u, SMD_ADD, "Add Item", 440, 330, add_page, 1);
    struct du_dialog *d = &u->dialog;
    d->accept = add_accept; d->accept_ctx = a; d->changed = add_changed; d->sheet_ctx = a;
    d->items[2].disabled = 1;
    struct du_widget *l = du_property_control(u, 4, DU_LISTVIEW, 500, (struct du_rect){ 20, 84, 376, 124 }, "");
    l->view = DU_DETAILS; l->items = a->add_items; l->count = SM_COUNTERS; l->column_count = 2; l->selection_active = 1;
    memcpy(l->columns, add_columns, sizeof(add_columns));
    du_property_control(u, 5, DU_BUTTON, 502, (struct du_rect){ 20, 214, 80, 23 }, "&Explain");
    sheet_text(u, "Select the item to add, then click Explain\nfor its definition.");
}
static int parse_scale(const char *s, uint64_t *out)
{
    uint64_t v = 0;
    if (!*s) return -1;
    for (; *s; s++) { if (*s < '0' || *s > '9' || v > (1ull << 40)) return -1; v = v * 10 + (uint64_t)(*s - '0'); }
    if (!v || v > (1ull << 40)) return -1;
    *out = v; return 0;
}
static int edit_accept(struct du_ui *u, int button, void *user)
{
    struct sysmon_app *a = user;
    if (button != 0) return 0;
    uint64_t scale = 0;
    if (!u->dialog.items[5].value && parse_scale(u->dialog.items[7].text, &scale)) {
        sheet_text(u, "Enter a whole number from 1 for the fixed maximum.");
        return 1;
    }
    unsigned color = u->dialog.items[4].active;
    sm_edit(&a->mon, a->edit_counter, smv_palette[color < 8 ? color : 0], scale);
    legend(a); repaint(a);
    return 0;
}
static const struct du_item edit_page[1] = { { .text = { "Chart item" } } };
static void edit_changed(struct du_ui *u, const struct du_event *e, void *user)
{
    (void)user;
    if (e->type == DU_EV_CHANGE && (e->id == 510 || e->id == 511)) u->dialog.items[7].disabled = e->id == 510;
}
static void open_edit(struct sysmon_app *a)
{
    struct du_ui *u = a->ui;
    if (a->selected < 0 || (unsigned)a->selected >= a->mon.count) return;
    const struct sm_item *it = &a->mon.items[a->selected];
    a->edit_counter = it->counter;
    for (unsigned i = 0; i < 8; i++) {
        memset(&a->color_items[i], 0, sizeof(a->color_items[i]));
        a->color_items[i].text[0] = smv_palette_names[i];
        swatch_fill(a->color_swatch[i], smv_palette[i]); a->color_items[i].icon16 = a->color_swatch[i];
    }
    du_property_sheet(u, SMD_EDIT, "Edit Item", 400, 300, edit_page, 1);
    struct du_dialog *d = &u->dialog;
    d->accept = edit_accept; d->accept_ctx = a; d->changed = edit_changed; d->sheet_ctx = a;
    d->items[2].disabled = 1;
    char text[128]; snprintf(text, sizeof(text), "%s\n\nColor:                                     Scale:", sm_name(it->counter));
    sheet_text(u, text);
    struct du_widget *l = du_property_control(u, 4, DU_LISTVIEW, 520, (struct du_rect){ 20, 84, 150, 140 }, "");
    l->view = DU_DETAILS; l->items = a->color_items; l->count = 8; l->column_count = 1; l->selection_active = 1;
    memcpy(l->columns, color_columns, sizeof(color_columns));
    for (unsigned i = 0; i < 8; i++) if (smv_palette[i] == it->color) l->active = i;
    struct du_widget *r1 = du_property_control(u, 5, DU_RADIO, 510, (struct du_rect){ 190, 84, 180, 22 }, "&Automatic (peak)");
    struct du_widget *r2 = du_property_control(u, 6, DU_RADIO, 511, (struct du_rect){ 190, 110, 180, 22 }, "&Fixed maximum:");
    r1->total = r2->total = 1; r1->value = it->scale == 0; r2->value = it->scale != 0;
    char scale[24]; snprintf(scale, sizeof(scale), "%llu", (unsigned long long)it->scale);
    struct du_widget *ed = du_property_control(u, 7, DU_EDIT, 512, (struct du_rect){ 210, 136, 120, 22 }, it->scale ? scale : "");
    ed->disabled = it->scale == 0;
}

static void open_interval(struct sysmon_app *a, const char *initial)
{
    du_input_box(a->ui, SMD_INTERVAL, "Chart Options", "Update interval in seconds (1 to 300):", initial);
}
static int parse_seconds(const char *s)
{
    long v = 0;
    if (!*s) return -1;
    for (; *s; s++) { if (*s < '0' || *s > '9' || v > 100000) return -1; v = v * 10 + (*s - '0'); }
    return v >= 1 && v <= 300 ? (int)v : -1;
}

static void select_chart(struct sysmon_app *a, int chart)
{ sm_set_chart(&a->mon, chart); menu_state(a); repaint(a); }

void smv_event(struct du_ui *u, const struct du_event *e, void *user)
{
    struct sysmon_app *a = user;
    switch (e->type) {
    case DU_EV_TICK: if (e->id == SMV_TIMER) smv_sample(a); return;
    case DU_EV_SELECT:
        if (e->id == SMV_LEGEND) { a->selected = e->index; menu_state(a); du_invalidate_widget(u, SMV_CHART); }
        return;
    case DU_EV_OPEN: if (e->id == SMV_LEGEND) open_edit(a); return;
    case DU_EV_KEY:
        if (e->key == DU_KEY_DELETE && e->id == SMV_LEGEND && !a->mon.logging) goto remove;
        return;
    case DU_EV_DIALOG:
        if (e->id == SMD_LOG && !e->index && e->text && e->text[0]) start_logging(a, e->text);
        else if (e->id == SMD_INTERVAL && !e->index && e->text) {
            int s = parse_seconds(e->text);
            if (s < 0) {
                a->bad_seconds = 1;
                sysui_box(u, SMD_BAD_INTERVAL, "Chart Options", "Enter a whole number of seconds from 1 to 300.", DU_DLG_ICON_WARNING);
            } else {
                sm_set_interval(&a->mon, (unsigned)s);
                du_timer_start(u, SMV_TIMER, (uint64_t)a->mon.interval_ms * 1000000ull, 1);
                status(a);
            }
        } else if (e->id == SMD_BAD_INTERVAL && a->bad_seconds) { a->bad_seconds = 0; open_interval(a, "5"); }
        return;
    case DU_EV_ACTIVATE: break;
    default: return;
    }
    switch (e->id) {
    case SMM_LOG_START:
        if (du_file_dialog(u, SMD_LOG, 1, "Start Logging", a->folder, "sysmon.csv", NULL, NULL))
            sysui_box(u, SMD_MESSAGE, "Start Logging", "The file dialog could not be opened.", DU_DLG_ICON_ERROR);
        break;
    case SMM_LOG_STOP: smv_stop_logging(a); set_message(a, "Logging stopped."); break;
    case SMM_EXIT: smv_stop_logging(a); a->host.quit(a->host.ctx); break;
    case SMM_ADD: open_add(a); break;
    case SMM_REMOVE:
    remove:
        if (a->mon.logging) { sysui_box(u, SMD_MESSAGE, "Remove Item", "Stop logging before changing the items on the chart.", DU_DLG_ICON_INFO); break; }
        if (a->selected >= 0 && (unsigned)a->selected < a->mon.count) {
            sm_remove(&a->mon, a->mon.items[a->selected].counter);
            if (a->selected >= (int)a->mon.count) a->selected = (int)a->mon.count - 1;
            legend(a); repaint(a); status(a);
        }
        break;
    case SMM_EDIT: open_edit(a); break;
    case SMM_EXPLAIN:
        if (a->selected >= 0 && (unsigned)a->selected < a->mon.count) {
            int c = a->mon.items[a->selected].counter; char wrapped[400];
            sysui_wrap(wrapped, sizeof(wrapped), sm_explain(c), 56);
            snprintf(a->explain, sizeof(a->explain), "%s\n\n%s", sm_category(c), wrapped);
            sysui_box(u, SMD_MESSAGE, sm_name(c), a->explain, DU_DLG_ICON_INFO);
        }
        break;
    case SMM_LINE: select_chart(a, SM_LINE); break;
    case SMM_BAR: select_chart(a, SM_BAR); break;
    case SMM_NUMERIC: select_chart(a, SM_NUMERIC); break;
    case SMM_OPTIONS: { char t[8]; snprintf(t, sizeof(t), "%u", a->mon.interval_ms / 1000); open_interval(a, t); break; }
    default: break;
    }
}

void smv_layout(struct du_ui *u, void *user)
{
    (void)user;
    int w = u->canvas.width, h = u->canvas.height, legend_h = 112;
    du_get(u, SMV_CHART)->box = (struct du_rect){ 6, 26, w - 12, h - 26 - 24 - legend_h - 10 };
    du_get(u, SMV_LEGEND)->box = (struct du_rect){ 6, h - 24 - legend_h - 2, w - 12, legend_h };
    du_get(u, SMV_STATUS)->box = (struct du_rect){ 0, h - 22, w, 22 };
    struct du_widget *l = du_get(u, SMV_LEGEND);
    l->columns[0].width = l->box.w * 34 / 100; l->columns[1].width = l->box.w * 28 / 100;
    l->columns[2].width = l->box.w - l->columns[0].width - l->columns[1].width - 20;
}

int smv_init(struct sysmon_app *a, struct du_ui *u, const struct sysmon_host *host)
{
    if (!a || !u || !host || !host->sample || !host->log_open || !host->log_write || !host->log_close || !host->quit) return -EINVAL;
    memset(a, 0, sizeof(*a));
    a->ui = u; a->host = *host; a->selected = 0; a->log_capacity = sizeof(a->log_row);
    snprintf(a->folder, sizeof(a->folder), "%s", profile_home());
    sm_init(&a->mon);
    a->file_items[0] = (struct du_menu_item){ "Start &Logging...", SMM_LOG_START, 0, 0, 0 };
    a->file_items[1] = (struct du_menu_item){ "S&top Logging", SMM_LOG_STOP, 0, 0, DU_ITEM_DISABLED };
    a->file_items[2] = (struct du_menu_item){ NULL, 0, 0, 0, DU_ITEM_SEPARATOR };
    a->file_items[3] = (struct du_menu_item){ "E&xit", SMM_EXIT, 0, 0, 0 };
    a->edit_items[0] = (struct du_menu_item){ NULL, 0, 0, 0, DU_ITEM_SEPARATOR | DU_ITEM_DISABLED };
    a->edit_items[1] = (struct du_menu_item){ "&Add Item...", SMM_ADD, 0, 0, 0 };
    a->edit_items[2] = (struct du_menu_item){ "&Remove Item", SMM_REMOVE, 0, 0, 0 };
    a->edit_items[3] = (struct du_menu_item){ "&Edit Item...", SMM_EDIT, 0, 0, 0 };
    a->edit_items[4] = (struct du_menu_item){ "E&xplain...", SMM_EXPLAIN, 0, 0, 0 };
    a->view_items[0] = (struct du_menu_item){ "&Line Charts", SMM_LINE, 0x26, DU_CTRL, 0 };
    a->view_items[1] = (struct du_menu_item){ "&Bar Charts", SMM_BAR, 0x30, DU_CTRL, 0 };
    a->view_items[2] = (struct du_menu_item){ "&Numeric Charts", SMM_NUMERIC, 0x31, DU_CTRL, 0 };
    a->options_items[0] = (struct du_menu_item){ "&Chart...", SMM_OPTIONS, 0, 0, 0 };
    a->menus[0] = (struct du_menu){ "&File", a->file_items, 4 };
    a->menus[1] = (struct du_menu){ "&Edit", a->edit_items + 1, 4 };
    a->menus[2] = (struct du_menu){ "&View", a->view_items, 3 };
    a->menus[3] = (struct du_menu){ "&Options", a->options_items, 1 };
    u->menus = a->menus; u->menu_count = 4;
    u->event = smv_event; u->user = a; u->layout = smv_layout; u->paint = smv_paint;
    du_add(u, DU_CANVAS, SMV_CHART, (struct du_rect){ 6, 26, 400, 200 }, "")->disabled = 1;
    struct du_widget *w = du_add(u, DU_LISTVIEW, SMV_LEGEND, (struct du_rect){ 6, 250, 400, 112 }, "");
    w->view = DU_DETAILS; w->column_count = 3; memcpy(w->columns, legend_columns, sizeof(legend_columns));
    w = du_add(u, DU_STATUS, SMV_STATUS, (struct du_rect){ 0, u->canvas.height - 22, u->canvas.width, 22 }, "");
    w->panels[0] = 90; w->panels[1] = 130;
    smv_layout(u, a);
    smv_sample(a);                         /* baseline: rates need two snapshots */
    legend(a);
    du_timer_start(u, SMV_TIMER, (uint64_t)a->mon.interval_ms * 1000000ull, 1);
    du_focus(u, SMV_LEGEND);
    return 0;
}
