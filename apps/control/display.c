/* SPDX-License-Identifier: MIT
 * Display mode selection and confirmation; all I/O belongs to cp_host.
 * The desktop owns mode switching. f1-23 owns the framebuffer relayout. */
#include "control.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>

#define SECOND 1000000000ull
#define CONFIRM_TIME (15 * SECOND)

const char *cp_adapter_name(uint16_t vendor, uint16_t device, char *out, size_t size)
{
    snprintf(out, size, "Unknown adapter (%04x:%04x)", (unsigned)vendor, (unsigned)device);
    return out;
}

void cp_settings_message(struct ciuki_message *m, uint32_t serial, unsigned request, int value, int flag)
{
    desk_message(m, DESK_SETTINGS, 24);
    m->data[0] = DESK_SHELL_VERSION;
    desk_put32(m->data + 4, serial); desk_put32(m->data + 8, request);
    if (request == DESK_SETTINGS_MASTER) {
        desk_put32(m->data + 12, (uint32_t)value); desk_put32(m->data + 16, (uint32_t)value);
        desk_put32(m->data + 20, (uint32_t)(flag != 0));
    } else if (request == DESK_SETTINGS_DISPLAY || request == DESK_SETTINGS_DISPLAY_PREVIEW || request == DESK_SETTINGS_SOUND_TEST || request == DESK_SETTINGS_SAVER_PREVIEW)
        desk_put32(m->data + 12, (uint32_t)value);
}

static void label(struct cp_app *a, int id, const char *s)
{ struct du_widget *w=du_get(a->ui,id); if (w && strcmp(w->text,s)) { du_set_text(w,s); du_invalidate(a->ui,w->box); } }
static int eligible(const struct ciuki_display_mode *m)
{
    return m->id && m->linear && m->width >= 640 && m->width <= 1600 && m->height >= 480 && m->height <= 1200 &&
           (m->bpp == 16 || m->bpp == 24 || m->bpp == 32) && m->pitch >= (uint32_t)m->width * (m->bpp / 8);
}
static uint16_t active_mode(const struct cp_app *a)
{
    if (!a->display_ok) return 0;
    for (unsigned i = 0; i < a->display.mode_count; i++) {
        const struct ciuki_display_mode *m = &a->display.modes[i];
        if (eligible(m) && m->current && m->width == a->display.width && m->height == a->display.height && m->bpp == a->display.bpp)
            return m->id;
    }
    return 0;
}
static uint16_t selected_mode(const struct cp_app *a)
{
    if (!a->display_ok || a->area >= a->area_count || a->depth >= a->depth_count) return 0;
    uint16_t id = 0;
    for (unsigned i = 0; i < a->display.mode_count; i++) {
        const struct ciuki_display_mode *m = &a->display.modes[i];
        if (eligible(m) && m->width == a->area_w[a->area] && m->height == a->area_h[a->area] && m->bpp == a->depths[a->depth]) {
            if (m->current) return m->id;
            if (!id) id = m->id;
        }
    }
    return id;
}
static void depths_build(struct cp_app *a, unsigned preferred)
{
    static const char *const labels[] = {"16-bit High Colour", "24-bit True Colour", "32-bit True Colour"};
    static const uint16_t bits[] = {16, 24, 32};
    a->depth_count = 0; a->depth = 0;
    if (a->area >= a->area_count) return;
    for (unsigned k = 0; k < 3; k++) for (unsigned i = 0; i < a->display.mode_count; i++) {
        const struct ciuki_display_mode *m = &a->display.modes[i];
        if (!eligible(m) || m->width != a->area_w[a->area] || m->height != a->area_h[a->area] || m->bpp != bits[k]) continue;
        unsigned n = a->depth_count++;
        a->depths[n] = bits[k]; a->depth_items[n] = (struct du_item){0}; a->depth_items[n].text[0] = labels[k];
        if (bits[k] == preferred) a->depth = n;
        break;
    }
}
static void info_lines(struct cp_app *a)
{
    char fallback[64], adapter[128], resolved[128];
    memset(a->adapter_lines, 0, sizeof(a->adapter_lines));
    if (!a->display_ok) {
        strcpy(a->adapter_lines[0], "Display information is unavailable.");
        label(a, CP_SUMMARY, "Display information unavailable");
    } else {
        const struct ciuki_display_info *d = &a->display;
        snprintf(a->adapter_lines[0], 160, "Screen: %u x %u, %u-bit colour", d->width, d->height, d->bpp);
        snprintf(a->adapter_lines[1], 160, "Pitch: %u bytes per scanline", d->pitch);
        snprintf(a->adapter_lines[2], 160, "VBE: %x.%02x   Video memory: %u KiB", (unsigned)(d->vbe_version >> 8),
                 (unsigned)(d->vbe_version & 255), (unsigned)(d->vram_bytes / 1024));
        const char *name=cp_adapter_name(d->pci_vendor,d->pci_device,adapter,sizeof(adapter));
        if (a->host.hwid_name && !a->host.hwid_name(a->host.ctx,d->pci_vendor,d->pci_device,resolved,sizeof(resolved)) && resolved[0]) name=resolved;
        snprintf(a->adapter_lines[4],160,"Adapter: %s",name);
        snprintf(a->adapter_lines[5], 160, "PCI: %04x:%04x", d->pci_vendor, d->pci_device);
        snprintf(a->adapter_lines[6], 160, "VBE OEM: %s", d->oem[0] ? d->oem : "Unavailable");
        snprintf(a->adapter_lines[7], 160, "VBE vendor: %s", d->vendor[0] ? d->vendor : "Unavailable");
        snprintf(a->adapter_lines[8], 160, "VBE product: %s", d->product[0] ? d->product : "Unavailable");
        snprintf(fallback, sizeof(fallback), "Display %u x %u, %u-bit colour", d->width, d->height, d->bpp);
        label(a, CP_SUMMARY, fallback);
    }
    strcpy(a->adapter_lines[9], "Monitor: Plug and Play Monitor (no EDID yet)");
}
void cp_engine_refresh(struct cp_app *a)
{
    struct ciuki_display_engine_info engine={0};
    int available=a->host.engine_info && !a->host.engine_info(a->host.ctx,&engine);
    /* Identity/readiness come from the stable accelerator device record.
     * The negotiated query adds per-operation counters. */
    a->devices.version=CIUKI_DEVICES_VERSION; a->devices.size=sizeof(a->devices);
    if (a->host.devices && !a->host.devices(a->host.ctx,&a->devices) && cp_snapshot_valid(&a->devices)) {
        for (unsigned i=0;i<a->devices.count;i++) {
            const struct ciuki_device_record *record=&a->devices.devices[i];
            if (record->id!=CIUKI_DEVICE_ACCELERATOR_ID) continue;
            snprintf(engine.display_name,sizeof(engine.display_name),"%s",record->driver_display_name);
            snprintf(engine.reason,sizeof(engine.reason),"%s",record->reason);
            engine.ready=record->accel_ready; engine.native_used=record->native_used;
            engine.quarantined=record->state==CIUKI_DEVICE_QUARANTINED;
            available=1; break;
        }
    }
    if (available) {
        uint64_t native=0, software=0;
        for (unsigned k=0;k<CIUKI_ENGINE_OPERATIONS;k++) { native+=engine.native[k]; software+=engine.software[k]; }
        snprintf(a->engine_line,sizeof(a->engine_line),"%s: %s (%s); engine %llu, software %llu",
            engine.display_name,engine.quarantined ? "quarantined" : engine.ready ? "active" : "software",
            engine.reason,(unsigned long long)native,(unsigned long long)software);
    } else {
        struct ciuki_accel_info legacy={0};
        if (!a->host.engine || a->host.engine(a->host.ctx,&legacy))
            snprintf(a->engine_line,sizeof(a->engine_line),"2D engine: status unavailable");
        else snprintf(a->engine_line,sizeof(a->engine_line),"2D engine: %s; capabilities 0x%X",
            legacy.quarantined ? "quarantined" : legacy.capabilities ? "available" : "software rendering",legacy.capabilities);
    }
    label(a,974,a->engine_line);
}
int cp_display_refresh(struct cp_app *a)
{
    struct ciuki_display_info d = {.version = CIUKI_DISPLAY_VERSION, .size = sizeof(d)};
    a->display_ok = a->host.display && !a->host.display(a->host.ctx, &d) &&
        d.version == CIUKI_DISPLAY_VERSION && d.size == sizeof(d) && d.mode_count <= 32;
    a->area_count = a->depth_count = a->area = a->depth = 0;
    if (a->display_ok) {
        d.oem[63] = d.vendor[63] = d.product[63] = 0;
        a->display = d;
        a->host.screen_w = d.width; a->host.screen_h = d.height;
        for (unsigned i = 0; i < d.mode_count; i++) {
            const struct ciuki_display_mode *m = &d.modes[i];
            if (!eligible(m)) continue;
            unsigned k = 0;
            while (k < a->area_count && (a->area_w[k] < m->width || (a->area_w[k] == m->width && a->area_h[k] < m->height))) k++;
            if (k < a->area_count && a->area_w[k] == m->width && a->area_h[k] == m->height) continue;
            for (unsigned j = a->area_count; j > k; j--) { a->area_w[j] = a->area_w[j-1]; a->area_h[j] = a->area_h[j-1]; }
            a->area_w[k] = m->width; a->area_h[k] = m->height; a->area_count++;
        }
        for (unsigned i = 0; i < a->area_count; i++) {
            snprintf(a->area_labels[i], 32, "%u x %u", a->area_w[i], a->area_h[i]);
            a->area_items[i] = (struct du_item){0}; a->area_items[i].text[0] = a->area_labels[i];
            if (a->area_w[i] == d.width && a->area_h[i] == d.height) a->area = i;
        }
        depths_build(a, d.bpp); a->preview_valid = 0;
    } else memset(&a->display, 0, sizeof(a->display));
    cp_engine_refresh(a);
    info_lines(a);
    label(a,972,a->adapter_lines[4]); label(a,973,a->adapter_lines[2]);
    cp_display_sync(a);
    return a->display_ok ? 0 : -1;
}
void cp_display_sync(struct cp_app *a)
{
    const int ids[]={CP_AREA,CP_DEPTH,CP_APPLY,CP_OK,CP_DISPLAY_TABS_ID,CP_REFRESH};
    struct { unsigned active,count; int disabled; } old[6];
    for (unsigned i=0;i<6;i++) { struct du_widget *v=du_get(a->ui,ids[i]); old[i].active=v?v->active:0; old[i].count=v?v->count:0; old[i].disabled=v?v->disabled:0; }
    struct du_widget *w=du_get(a->ui,CP_AREA);
    if (w) { w->count = a->area_count; w->active = a->area; w->disabled = !a->area_count || a->mode_state; du_list_ensure(w, w->active); }
    w = du_get(a->ui, CP_DEPTH);
    if (w) { w->count = a->depth_count; w->active = a->depth; w->disabled = !a->depth_count || a->mode_state; du_list_ensure(w, w->active); }
    for (unsigned i = 0; i < a->area_count; i++) a->area_items[i].selected = i == a->area;
    for (unsigned i = 0; i < a->depth_count; i++) a->depth_items[i].selected = i == a->depth;
    w = du_get(a->ui, CP_APPLY);
    if (w) w->disabled = a->mode_state || a->display_write ||
        (!a->display_dirty && (!active_mode(a) || !selected_mode(a) || selected_mode(a)==active_mode(a)));
    w=du_get(a->ui,CP_OK); if (w) w->disabled=a->mode_state || a->display_write;
    w=du_get(a->ui,CP_DISPLAY_TABS_ID); if (w) w->disabled=a->mode_state || a->display_write;
    w = du_get(a->ui, CP_REFRESH); if (w) w->disabled = a->mode_state != CP_MODE_IDLE;
    for (unsigned i=0;i<6;i++) { struct du_widget *v=du_get(a->ui,ids[i]); if (v && (v->active!=old[i].active || v->count!=old[i].count || v->disabled!=old[i].disabled)) du_invalidate_widget(a->ui,v->id); }
    label(a, CP_MODE_NOTE, !a->display_ok ? "Unavailable." : !active_mode(a) ? "No safe restore." : "15s to confirm.");
}
void cp_adapter_paint(struct cp_app *a, struct du_canvas *c)
{
    for (unsigned i = 0; i < 10; i++) du_text_fit(c, 24, 48 + (int)i * 26, c->width - 48, a->adapter_lines[i], DU_INK);
}
static int request(struct cp_app *a, uint16_t id)
{ return a->host.apply ? a->host.apply(a->host.ctx, DESK_SETTINGS_DISPLAY_PREVIEW, id, 0) : -1; }
static void finish(struct cp_app *a, const char *status)
{
    a->mode_state = CP_MODE_IDLE;
    a->mode_retry=0;
    du_timer_stop(a->ui, CP_MODE_TIMER);
    if (du_dialog_id(a->ui) == CP_CONFIRM) du_dialog_close(a->ui);
    cp_display_refresh(a); label(a, 3, status);
}
static void restore_failed(struct cp_app *a, const char *status)
{
    a->display_accept=0;
    if (a->display_leaving) {
        a->display_preview=1; a->display_leaving=0;
        a->close_pending=a->navigation_pending=0;
    }
    finish(a,status);
}
void cp_display_cancel(struct cp_app *a)
{
    if (a->mode_state == CP_MODE_IDLE || a->mode_state == CP_MODE_REVERT) return;
    if (du_dialog_id(a->ui) == CP_CONFIRM) du_dialog_close(a->ui);
    a->mode_state = CP_MODE_REVERT; a->mode_deadline = a->ui->now + CONFIRM_TIME;
    a->mode_retry=0;
    a->display_accept=0;
    if (request(a, a->previous_mode)) { restore_failed(a, "The previous mode could not be requested. Try Cancel again."); return; }
    label(a, 3, "Restoring the previous display mode..."); cp_display_sync(a);
}
static void countdown(struct cp_app *a)
{
    unsigned seconds = a->ui->now >= a->mode_deadline ? 0 : (unsigned)((a->mode_deadline - a->ui->now + SECOND - 1) / SECOND);
    snprintf(a->ui->dialog.text, sizeof(a->ui->dialog.text), "Keep these settings?\nReverting in %u second%s.", seconds, seconds == 1 ? "" : "s");
    du_invalidate(a->ui,a->ui->dialog.box);
}
void cp_display_result(struct cp_app *a, int result)
{
    if (result==-EBUSY && (a->mode_state==CP_MODE_WAIT || a->mode_state==CP_MODE_REVERT) && a->ui->now<a->mode_deadline) {
        a->mode_retry=1; a->mode_retry_ns=a->ui->now+SECOND;
        label(a,3,"Display is finishing a frame; waiting to retry..."); return;
    }
    if (a->mode_state == CP_MODE_REVERT) {
        if (result || cp_display_refresh(a) || active_mode(a) != a->previous_mode)
            restore_failed(a, "The previous display mode could not be restored. Try Cancel again.");
        else finish(a, "The previous display mode was restored.");
        return;
    }
    if (a->mode_state != CP_MODE_WAIT) return;
    if (result) { a->display_accept=0; finish(a, "The desktop refused the display mode."); return; }
    if (cp_display_refresh(a) || active_mode(a) != a->pending_mode) { cp_display_cancel(a); return; }
    a->mode_state = CP_MODE_CONFIRM; a->mode_deadline = a->ui->now + CONFIRM_TIME;
    static const char *const buttons[] = {"Keep", "Revert"};
    du_message(a->ui, CP_CONFIRM, "Display Settings", "Keep these settings?\nReverting in 15 seconds.", DU_DLG_ICON_QUESTION, buttons, 2, 1, 1);
    countdown(a); cp_display_sync(a);
}
void cp_display_saved(struct cp_app *a, int error)
{
    if (a->mode_state!=CP_MODE_SAVE) return;
    if (error) {
        a->display_accept=0;
        char text[256]; if (error<0) error=-error;
        snprintf(text,sizeof(text),"Restoring the previous mode: %s (%d)",strerror(error),error);
        cp_display_cancel(a); label(a,3,text);
    } else {
        finish(a,"Display settings saved.");
        if (a->display_accept) cp_display_accept(a);
        if (!a->host.apply || a->host.apply(a->host.ctx,DESK_SETTINGS_RELOAD,0,0))
            label(a,3,"Saved. Desktop reload could not be requested.");
    }
}
static int persist(struct cp_app *a)
{
    a->cfg.display_mode = a->pending_mode;
    return cp_display_store(a);
}
int cp_display_event(struct cp_app *a, const struct du_event *e)
{
    if (e->type == DU_EV_TICK && e->id == CP_MODE_TIMER) {
        if (a->mode_state==CP_MODE_SAVE) return 1; /* Keep was confirmed; durable completion owns the reply. */
        if (a->mode_retry && a->ui->now>=a->mode_retry_ns && a->ui->now<a->mode_deadline) {
            a->mode_retry=0;
            if (request(a,a->mode_state==CP_MODE_REVERT ? a->previous_mode : a->pending_mode))
                restore_failed(a,"The display retry could not be requested.");
            return 1;
        }
        if (a->mode_state && a->ui->now >= a->mode_deadline) {
            if (a->mode_state == CP_MODE_REVERT) restore_failed(a, "No reply while restoring the display mode. Try Cancel again.");
            else cp_display_cancel(a);
        } else if (a->mode_state == CP_MODE_CONFIRM) countdown(a);
        return 1;
    }
    if (e->id == CP_CONFIRM && e->type == DU_EV_DIALOG) {
        if (a->mode_state != CP_MODE_CONFIRM) return 1;
        if (e->index || a->ui->now >= a->mode_deadline) { a->display_accept=0; cp_display_cancel(a); }
        else if (cp_display_refresh(a) || active_mode(a) != a->pending_mode || persist(a)) {
            a->display_accept=0; cp_display_cancel(a); label(a, 3, "Settings could not be saved; restoring the previous mode.");
        } else if (a->host.save_async) {
            a->mode_state=CP_MODE_SAVE;
            label(a,3,"Saving display settings..."); cp_display_sync(a);
        } else {
            finish(a, "Display settings saved.");
            if (a->display_accept) cp_display_accept(a);
            if (!a->host.apply || a->host.apply(a->host.ctx, DESK_SETTINGS_RELOAD, 0, 0)) label(a, 3, "Saved. Desktop reload could not be requested.");
        }
        return 1;
    }
    if (e->id == CP_REFRESH && e->type == DU_EV_ACTIVATE && !a->mode_state) {
        cp_display_refresh(a); return 1;
    }
    if (a->mode_state) return e->id >= CP_AREA_LABEL && e->id <= CP_REFRESH;
    if (e->id == CP_AREA && e->type == DU_EV_SELECT && e->index >= 0 && (unsigned)e->index < a->area_count) {
        unsigned bits = a->depth < a->depth_count ? a->depths[a->depth] : a->display.bpp;
        a->area = (unsigned)e->index; depths_build(a, bits); cp_display_sync(a); return 1;
    }
    if (e->id == CP_DEPTH && e->type == DU_EV_SELECT && e->index >= 0 && (unsigned)e->index < a->depth_count) {
        a->depth = (unsigned)e->index; cp_display_sync(a); return 1;
    }
    if (e->id == CP_APPLY && e->type == DU_EV_ACTIVATE) {
        if (a->display_write) return 1;
        a->pending_mode = selected_mode(a);
        if (!active_mode(a) || !a->pending_mode || a->pending_mode==active_mode(a)) {
            if (a->display_dirty && cp_display_store(a)) a->display_accept=0;
            cp_display_sync(a); return 1;
        }
        cp_display_refresh(a); /* capture the actual previous mode at Apply, not at panel creation */
        a->previous_mode = active_mode(a);
        int listed = 0;
        for (unsigned i = 0; i < a->display.mode_count; i++)
            if (eligible(&a->display.modes[i]) && a->display.modes[i].id == a->pending_mode) listed = 1;
        if (!listed) { a->display_accept=0; label(a, 3, "The selected display mode is no longer available."); return 1; }
        if (!a->pending_mode || !a->previous_mode || a->pending_mode == a->previous_mode) return 1;
        a->mode_state = CP_MODE_WAIT; a->mode_deadline = a->ui->now + CONFIRM_TIME;
        if (du_timer_start(a->ui, CP_MODE_TIMER, SECOND, 1) || request(a, a->pending_mode)) {
            a->display_accept=0; finish(a, "The display mode could not be requested."); return 1;
        }
        label(a, 3, "Applying the display mode..."); cp_display_sync(a); return 1;
    }
    return 0;
}

/* Display preview ownership and readiness, f2-88c. The app.c paint/event glue
 * calls these functions; no other applet's scheduling policy changes. */
static const struct desk_theme *chosen(const struct cp_app *a)
{ const struct desk_theme *t = desk_theme_named(a->cfg.theme); return t ? t : desk_theme; }
static uint32_t desktop_colour(const struct cp_app *a, const struct desk_theme *t)
{ return a->cfg.desktop_color == CFG_DESKTOP_COLOR ? t->desktop : a->cfg.desktop_color; }
void cp_preview_request(struct cp_app *a, struct cp_preview_req *r, int have_image)
{
    uint32_t sh = a->host.screen_h ? a->host.screen_h : 768, bar = sh ? (32u * 144u) / sh : 6;
    const struct desk_theme *t = chosen(a);
    memset(r, 0, sizeof(*r));
    snprintf(r->path, sizeof(r->path), "%s", a->cfg.wallpaper);
    r->mode = a->cfg.wallpaper_mode; r->colour = desktop_colour(a, t); r->bar = bar;
    r->font = t->font_large; r->label = t->desktop_label; r->aa = t->text_aa; r->portrait = a->host.portrait;
    if (have_image) r->image = a->image;
}
static int preview_request_same(const struct cp_app *a, const struct cp_preview_req *r)
{
    struct cp_preview_req now; cp_preview_request((struct cp_app *)a, &now, 0);
    return !strncmp(now.path, r->path, sizeof(now.path)) && now.mode == r->mode && now.colour == r->colour &&
           now.bar == r->bar && now.font == r->font && now.label == r->label && now.aa == r->aa &&
           now.portrait == r->portrait;
}

int cp_system_thumbnail(const char *path, char *name, size_t size)
{
    static const char *const names[] = {"Ciuk1", "Ciuk2", "Ciuk3"};
    /* FAT paths are case insensitive; accept their normal case variants, but
     * not another user file with the same basename or an arbitrary prefix. */
    for (unsigned i = 0; i < 3; i++) {
        char full[64]; snprintf(full, sizeof(full), "/system/wallpapers/%s.png", names[i]);
        unsigned k = 0;
        while (full[k] && path[k]) {
            unsigned a = (unsigned char)full[k], b = (unsigned char)path[k];
            if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
            if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
            if (a != b) break;
            k++;
        }
        if (!full[k] && !path[k]) { snprintf(name, size, "%s-192x144.raw", names[i]); return 1; }
    }
    return 0;
}
void cp_preview_cancel(struct cp_app *a)
{
    du_timer_stop(a->ui, 700); /* deferred first paint, never a completion timer */
    a->preview_pending = 0; a->preview_valid = 0;
    if (a->decode_wait && !a->decode_cancelled) {
        a->decode_cancelled = 1;
        if (a->host.decode_cancel) a->host.decode_cancel(a->host.ctx);
    }
}
void cp_preview_ready(struct cp_app *a)
{
    struct cp_preview_done done = {.preview = a->preview};
    if (!a->decode_wait || !a->host.decode_poll || a->host.decode_poll(a->host.ctx, &done) <= 0) return;
    int cancelled = a->decode_cancelled || done.error == -ECANCELED;
    a->decode_wait = a->decode_cancelled = 0;
    a->decode_ms = done.decode_ms; a->render_ms = done.render_ms; a->decode_done++;
    int current = !cancelled && a->page == CP_DISPLAY && a->display_tab == CP_BACKGROUND &&
                  !strncmp(done.req.path, a->cfg.wallpaper, sizeof(done.req.path));
    if (done.image.pixels && current) {
        desk_image_free(&a->image); a->image = done.image;
        snprintf(a->image_path, sizeof(a->image_path), "%s", done.req.path);
    } else {
        if (!cancelled && !done.image.pixels) snprintf(a->decode_failed, sizeof(a->decode_failed), "%s", done.req.path);
        desk_image_free(&done.image);
    }
    a->preview_pending = 2;
    a->preview_valid = done.rendered && current && a->image.pixels && preview_request_same(a, &done.req);
    if (!cancelled && current && !done.image.pixels) {
        for (size_t i = 0; i < 192 * 144; i++) a->preview[i] = desktop_colour(a, chosen(a));
        a->preview_valid = 1;
    }
    if (a->page == CP_DISPLAY && a->display_tab == CP_BACKGROUND)
        du_invalidate(a->ui,(struct du_rect){24,44,208,188});
}
int cp_display_wait_ms(uint64_t now, uint64_t deadline)
{
    /* Round up: sub-ms or overdue timers must not turn poll into a busy loop.
     * The caller has already dispatched due timers in du_window_turn(). */
    if (deadline == UINT64_MAX) return -1;
    if (deadline <= now) return 1;
    uint64_t left = deadline - now;
    return left >= 2147483647ull * 1000000ull ? 2147483647 : (int)((left + 999999ull) / 1000000ull);
}
