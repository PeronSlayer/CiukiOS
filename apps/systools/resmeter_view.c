/* Resource Meter window. SPDX-License-Identifier: MIT */
#include "resmeter_view.h"
#include "sysui.h"
#include <errno.h>
#include <string.h>

uint32_t rmv_color(enum rm_level l)
{ return l == RM_OK ? 0x2e9e3au : l == RM_LOW ? 0xe0a000u : 0xcc2222u; }
enum rm_level rmv_level_of(unsigned p)
{ return p < RM_CRITICAL_PCT ? RM_CRITICAL : p < RM_LOW_PCT ? RM_LOW : RM_OK; }

static const char *const names[3] = { "System:", "User:", "GDI:" };

void rmv_update(struct resmeter_app *a, const struct rm_desktop *in, int windows_known)
{
    struct rm_sample s; struct rm_state st;
    rm_from_desktop(&s, in);
    if (!windows_known) { s.user_cap = 1; s.user_used = 0; }   /* placeholder; never shown */
    rm_compute(&st, &s);
    a->user_known = windows_known;
    if (!windows_known) { st.user_free = 0; st.system_free = st.gdi_free; st.level = rmv_level_of(st.gdi_free); }
    a->state = st;
    a->percent[0] = st.system_free; a->percent[1] = st.user_free; a->percent[2] = st.gdi_free;
    for (unsigned i = 0; i < 3; i++) a->level[i] = rmv_level_of(a->percent[i]);
    snprintf(a->text[0], sizeof(a->text[0]), "%u%%", st.system_free);
    if (windows_known) snprintf(a->text[1], sizeof(a->text[1]), "%u%%", st.user_free); else snprintf(a->text[1], sizeof(a->text[1]), "n/a");
    snprintf(a->text[2], sizeof(a->text[2]), "%u%%", st.gdi_free);
    for (unsigned i = 0; i < 3; i++) sysui_text(a->ui, RMV_SYSTEM_TEXT + (int)i, a->text[i]);
    sysui_text(a->ui, RMV_SUMMARY, st.level == RM_OK ? "Resources are at a healthy level." :
               st.level == RM_LOW ? "Resources are low. Close some programs." :
               "Resources are critically low!");
    for (int id = RMV_SYSTEM_BAR; id <= RMV_GDI_BAR; id++) du_invalidate_widget(a->ui, id);
}

void rmv_paint(struct du_ui *u, void *user)
{
    struct resmeter_app *a = user; struct du_canvas *c = &u->canvas;
    struct du_rect clip = c->clip;
    for (unsigned i = 0; i < 3; i++) {
        struct du_rect b = du_get(u, RMV_SYSTEM_BAR + (int)i)->box;
        c->clip = du_intersection(clip, b);
        if (c->clip.w <= 0 || c->clip.h <= 0) continue;
        du_fill(c, b, DU_WINDOW); du_edge(c, b, DU_SUNKEN);
        int inner = b.w - 4, fill = (int)((int64_t)inner * (a->user_known || i != 1 ? a->percent[i] : 0) / 100);
        if (fill > 0) du_fill(c, (struct du_rect){ b.x + 2, b.y + 2, fill, b.h - 4 }, rmv_color(a->level[i]));
    }
    c->clip = clip;
}

void rmv_event(struct du_ui *u, const struct du_event *e, void *user)
{
    struct resmeter_app *a = user; (void)u;
    if (e->type == DU_EV_TICK && e->id == RMV_TIMER) a->host.refresh(a->host.ctx);
    else if (e->type == DU_EV_ACTIVATE && e->id == RMV_CLOSE) a->host.quit(a->host.ctx);
}

int rmv_init(struct resmeter_app *a, struct du_ui *u, const struct resmeter_host *host)
{
    if (!a || !u || !host || !host->refresh || !host->quit) return -EINVAL;
    memset(a, 0, sizeof(*a)); a->ui = u; a->host = *host;
    u->event = rmv_event; u->user = a; u->paint = rmv_paint;
    du_add(u, DU_LABEL, 20, (struct du_rect){ 12, 8, 296, 16 }, "Percentage of free system resources:");
    for (int i = 0; i < 3; i++) {
        int y = 32 + i * 28;
        du_add(u, DU_LABEL, 30 + i, (struct du_rect){ 12, y, 70, 20 }, names[i]);
        du_add(u, DU_CANVAS, RMV_SYSTEM_BAR + i, (struct du_rect){ 88, y, 170, 20 }, "")->disabled = 1;
        du_add(u, DU_LABEL, RMV_SYSTEM_TEXT + i, (struct du_rect){ 266, y, 44, 20 }, "");
    }
    du_add(u, DU_LABEL, RMV_SUMMARY, (struct du_rect){ 12, 120, 296, 16 }, "");
    struct du_widget *b = du_add(u, DU_BUTTON, RMV_CLOSE, (struct du_rect){ 232, 146, 76, 24 }, "&Close");
    b->def = 1;
    du_focus(u, RMV_CLOSE);
    du_timer_start(u, RMV_TIMER, RMV_INTERVAL_NS, 1);
    return 0;
}
