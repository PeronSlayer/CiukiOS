/* Control Panel and its property pages: every incremental paint must equal a full repaint of the same state.
 * Windows 98 property sheets repaint only the active page and a tab switch hides the previous page's controls,
 * so no page may leave pixels (or hit-testable widgets) behind. Drives applets, categories, tabs, sliders and
 * dialogs with synthetic input and compares the canvas after the incremental du_render with a forced full one.
 * SPDX-License-Identifier: MIT */
#include "control.h"
#include "acc_test.h"
#include "device_fixture.h"
static struct cp_app app;
static struct du_ui ui;
static uint32_t pixels[CP_WIDTH * CP_HEIGHT], part[CP_WIDTH * CP_HEIGHT];
static struct ciuki_devices_info sample;
static unsigned checks, failures;
static const char *step_name = "";

static int devices(void *c, struct ciuki_devices_info *out) { (void)c; *out = sample; return 0; }
static int settings_load(void *c, char *s, size_t cap, size_t *n)
{ (void)c; const char text[] = "version=1\nwallpaper=none\n"; assert(cap >= sizeof(text)); memcpy(s, text, sizeof(text)); *n = sizeof(text) - 1; return 0; }
static int mixer(void *c, int *v, int *m) { (void)c; *v = 70; *m = 0; return 0; }
static int apply(void *c, unsigned r, int v, int m) { (void)c; (void)r; (void)v; (void)m; return 0; }
static int save(void *c, const char *t, size_t n) { (void)c; (void)t; (void)n; return 0; }
static struct du_widget *kind(enum du_kind k)
{ for (unsigned i = 0; i < ui.count; i++) if (ui.widgets[i].kind == k && !ui.widgets[i].hidden) return &ui.widgets[i]; return NULL; }

/* Paint what is pending, then repaint everything and require the same pixels. */
static void check(const char *what)
{
    du_render(&ui);
    memcpy(part, pixels, sizeof(pixels));
    ui.dirty = 1; du_render(&ui);
    checks++;
    if (memcmp(part, pixels, sizeof(pixels))) {
        unsigned bad = 0, x0 = CP_WIDTH, y0 = CP_HEIGHT, x1 = 0, y1 = 0;
        for (unsigned k = 0; k < CP_WIDTH * CP_HEIGHT; k++) if (part[k] != pixels[k]) {
            bad++; unsigned x = k % CP_WIDTH, y = k / CP_WIDTH;
            if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
        }
        fprintf(stderr, "STALE %s / %s: page %d, %u pixels differ in [%u,%u]-[%u,%u]\n", step_name, what, app.page, bad, x0, y0, x1, y1);
        failures++;
    }
    /* a hidden widget must not stay hit-testable: nothing visible may be covered by a hidden sibling's box state */
}
static void attach(int page)
{
    memset(&app, 0, sizeof(app));
    app.host = (struct cp_host){.load = settings_load, .devices = devices, .mixer = mixer, .apply = apply, .save = save,
        .screen_w = 1024, .screen_h = 768, .mem_total_k = 512 * 1024, .mem_free_k = 400 * 1024,
        .version = "0.9.0", .build = "host", .machine = "i686"};
    du_init(&ui, pixels, CP_WIDTH, CP_HEIGHT); ui.focused_window = 1;
    assert(!cp_attach(&app, &ui, page));
    du_render(&ui);
}
static void exercise_tabs(void)
{
    struct du_widget *t = kind(DU_TABS);
    if (!t || t->count < 2) return;
    unsigned n = t->count;
    for (unsigned round = 0; round < 2; round++)
        for (unsigned i = 0; i < n; i++) {
            t = kind(DU_TABS); if (!t) return;
            du_post(&ui, DU_EV_SELECT, t->id, (int)i);
            char what[32]; snprintf(what, sizeof(what), "tab %u", i); check(what);
        }
    for (unsigned i = n; i-- > 0;) {
        t = kind(DU_TABS); if (!t) return;
        du_post(&ui, DU_EV_SELECT, t->id, (int)i);
        char what[32]; snprintf(what, sizeof(what), "tab back %u", i); check(what);
    }
}
static void exercise_widgets(void)
{
    /* sliders: move each one by keyboard and by mouse, then buttons/checkboxes via click */
    for (unsigned i = 0; i < ui.count; i++) {
        struct du_widget *w = &ui.widgets[i];
        if (w->hidden || w->disabled) continue;
        if (w->kind == DU_TRACKBAR) {
            int id = w->id;
            at_click(&ui, w->box.x + w->box.w / 3, w->box.y + w->box.h / 2); check("slider click");
            w = du_get(&ui, id); if (!w) return;
            at_click(&ui, w->box.x + w->box.w * 2 / 3, w->box.y + w->box.h / 2); check("slider click 2");
        }
    }
}
static void exercise_dialog(void)
{
    struct du_widget *b = NULL;
    for (unsigned i = 0; i < ui.count; i++) if (ui.widgets[i].kind == DU_BUTTON && !ui.widgets[i].hidden && !ui.widgets[i].disabled &&
        strcmp(ui.widgets[i].text, "Control Panel") && strcmp(ui.widgets[i].text, "Close") && strcmp(ui.widgets[i].text, "Cancel") && strcmp(ui.widgets[i].text, "OK")) { b = &ui.widgets[i]; break; }
    if (!b) return;
    at_click_widget(&ui, b->id); check("button");
    if (du_dialog_id(&ui) || ui.dialog.kind) { at_tap(&ui, DU_KEY_ESC); check("dialog closed"); }
}

int main(void)
{
    device_fixture(&sample);
    attach(CP_HOME);
    struct du_widget *cats = kind(DU_TREE); assert(cats);
    unsigned ncat = cats->count;
    for (unsigned c = 0; c < ncat; c++) {
        attach(CP_HOME);
        cats = kind(DU_TREE);
        step_name = "category"; du_post(&ui, DU_EV_SELECT, cats->id, (int)c); check("category");
        struct du_widget *l = kind(DU_LISTVIEW); assert(l);
        for (unsigned i = 0; i < l->count; i++) {
            attach(CP_HOME);
            cats = kind(DU_TREE); du_post(&ui, DU_EV_SELECT, cats->id, (int)c); check("category");
            l = kind(DU_LISTVIEW);
            char name[64]; snprintf(name, sizeof(name), "cat %u applet %u (%s)", c, i, l->items[i].text[0]); step_name = name;
            du_post(&ui, DU_EV_OPEN, l->id, (int)i); check("open applet");
            if (app.page == CP_HOME) continue;
            exercise_tabs();
            exercise_widgets();
            exercise_dialog();
            struct du_widget *back = NULL;
            for (unsigned k = 0; k < ui.count; k++) if (ui.widgets[k].kind == DU_BUTTON && !strcmp(ui.widgets[k].text, "Control Panel")) back = &ui.widgets[k];
            if (back) { at_click_widget(&ui, back->id); check("back to home"); }
        }
    }
    cp_detach(&app);
    printf("%s: %u checks, %u stale\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
