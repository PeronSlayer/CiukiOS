/* SPDX-License-Identifier: MIT
 * Display mode selection and confirmation; all I/O belongs to cp_host.
 * The desktop owns mode switching. f1-23 owns the framebuffer relayout. */
#include "control.h"
#include <stdio.h>
#include <string.h>

#define SECOND 1000000000ull
#define CONFIRM_TIME (15 * SECOND)

const char *cp_adapter_name(uint16_t vendor, uint16_t device, char *out, size_t size)
{
    static const struct { uint16_t vendor, device; const char *name; } adapters[] = {
        {0x5333, 0x8c2e, "S3 SuperSavage IX/C SDR"}, {0x5333, 0x8c2f, "S3 SuperSavage IX/C DDR"},
        {0x5333, 0x8a22, "S3 Savage4"}, {0x5333, 0x8a20, "S3 Savage3D"},
        {0x5333, 0x8a25, "VIA/S3 ProSavage PM133"}, {0x5333, 0x8a26, "VIA/S3 ProSavage KM133"},
        {0x5333, 0x8d01, "VIA/S3 ProSavage PN133"}, {0x5333, 0x8d02, "VIA/S3 ProSavage KN133"},
        {0x5333, 0x8d04, "VIA/S3 ProSavageDDR P4M266"},
        {0x1106, 0x3122, "VIA CLE266 CastleRock Graphics"},
        {0x1106, 0x7205, "VIA KM400/KN400/P4M800 UniChrome"},
        {0x1002, 0x4c4d, "ATI Rage Mobility M/P AGP"}, {0x1002, 0x4c52, "ATI Rage Mobility M/P"},
        {0x1002, 0x4c45, "ATI Rage Mobility M3"}, {0x1002, 0x4742, "ATI 3D Rage Pro"},
        {0x8086, 0x7121, "Intel 810 Graphics"}, {0x8086, 0x7123, "Intel 810 DC-100 Graphics"},
        {0x8086, 0x7125, "Intel 810E Graphics"}, {0x8086, 0x1132, "Intel 815 Graphics"},
        {0x8086, 0x3577, "Intel 830M Graphics"}, {0x8086, 0x2562, "Intel 845G Graphics"},
        {0x8086, 0x2572, "Intel 865G Graphics"}, {0x8086, 0x2582, "Intel 915G Graphics"},
        {0x10de, 0x0020, "NVIDIA RIVA TNT"}, {0x10de, 0x0028, "NVIDIA RIVA TNT2"},
        {0x10de, 0x0110, "NVIDIA GeForce2 MX"}, {0x10de, 0x0150, "NVIDIA GeForce2 GTS"},
        {0x10de, 0x0171, "NVIDIA GeForce4 MX440"}, {0x121a, 0x0003, "3dfx Voodoo Banshee"},
        {0x121a, 0x0005, "3dfx Voodoo3"}, {0x121a, 0x0009, "3dfx Voodoo4 / Voodoo5"}
    };
    for (unsigned i = 0; i < sizeof(adapters) / sizeof(adapters[0]); i++)
        if (adapters[i].vendor == vendor && adapters[i].device == device) return adapters[i].name;
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
    } else if (request == DESK_SETTINGS_DISPLAY || request == DESK_SETTINGS_SOUND_TEST)
        desk_put32(m->data + 12, (uint32_t)value);
}

static void label(struct cp_app *a, int id, const char *s)
{ struct du_widget *w = du_get(a->ui, id); if (w) du_set_text(w, s); a->ui->dirty = 1; }
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
    char fallback[64];
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
        snprintf(a->adapter_lines[4], 160, "Adapter: %s", cp_adapter_name(d->pci_vendor, d->pci_device, fallback, sizeof(fallback)));
        snprintf(a->adapter_lines[5], 160, "PCI: %04x:%04x", d->pci_vendor, d->pci_device);
        snprintf(a->adapter_lines[6], 160, "VBE OEM: %s", d->oem[0] ? d->oem : "Unavailable");
        snprintf(a->adapter_lines[7], 160, "VBE vendor: %s", d->vendor[0] ? d->vendor : "Unavailable");
        snprintf(a->adapter_lines[8], 160, "VBE product: %s", d->product[0] ? d->product : "Unavailable");
        snprintf(fallback, sizeof(fallback), "Display %u x %u, %u-bit colour", d->width, d->height, d->bpp);
        label(a, CP_SUMMARY, fallback);
    }
    strcpy(a->adapter_lines[9], "Monitor: Plug and Play Monitor (no EDID yet)");
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
    info_lines(a);
    cp_display_sync(a);
    return a->display_ok ? 0 : -1;
}
void cp_display_sync(struct cp_app *a)
{
    struct du_widget *w = du_get(a->ui, CP_AREA);
    if (w) { w->count = a->area_count; w->active = a->area; w->disabled = !a->area_count || a->mode_state; du_list_ensure(w, w->active); }
    w = du_get(a->ui, CP_DEPTH);
    if (w) { w->count = a->depth_count; w->active = a->depth; w->disabled = !a->depth_count || a->mode_state; du_list_ensure(w, w->active); }
    for (unsigned i = 0; i < a->area_count; i++) a->area_items[i].selected = i == a->area;
    for (unsigned i = 0; i < a->depth_count; i++) a->depth_items[i].selected = i == a->depth;
    w = du_get(a->ui, CP_APPLY);
    if (w) w->disabled = a->mode_state || !active_mode(a) || !selected_mode(a) || selected_mode(a) == active_mode(a);
    w = du_get(a->ui, CP_REFRESH); if (w) w->disabled = a->mode_state != CP_MODE_IDLE;
    label(a, CP_MODE_NOTE, !a->display_ok ? "Unavailable." : !active_mode(a) ? "No safe restore." : "15s to confirm.");
}
void cp_adapter_paint(struct cp_app *a, struct du_canvas *c)
{
    for (unsigned i = 0; i < 10; i++) du_text_fit(c, 24, 48 + (int)i * 26, c->width - 48, a->adapter_lines[i], DU_INK);
}
static int request(struct cp_app *a, uint16_t id)
{ return a->host.apply ? a->host.apply(a->host.ctx, DESK_SETTINGS_DISPLAY, id, 0) : -1; }
static void finish(struct cp_app *a, const char *status)
{
    a->mode_state = CP_MODE_IDLE;
    du_timer_stop(a->ui, CP_MODE_TIMER);
    if (du_dialog_id(a->ui) == CP_CONFIRM) du_dialog_close(a->ui);
    cp_display_refresh(a); label(a, 3, status);
}
void cp_display_cancel(struct cp_app *a)
{
    if (a->mode_state == CP_MODE_IDLE || a->mode_state == CP_MODE_REVERT) return;
    if (du_dialog_id(a->ui) == CP_CONFIRM) du_dialog_close(a->ui);
    a->mode_state = CP_MODE_REVERT; a->mode_deadline = a->ui->now + CONFIRM_TIME;
    if (request(a, a->previous_mode)) { finish(a, "The previous mode could not be requested."); return; }
    label(a, 3, "Restoring the previous display mode..."); cp_display_sync(a);
}
static void countdown(struct cp_app *a)
{
    unsigned seconds = a->ui->now >= a->mode_deadline ? 0 : (unsigned)((a->mode_deadline - a->ui->now + SECOND - 1) / SECOND);
    snprintf(a->ui->dialog.text, sizeof(a->ui->dialog.text), "Keep these settings?\nReverting in %u second%s.", seconds, seconds == 1 ? "" : "s");
    a->ui->dirty = 1;
}
void cp_display_result(struct cp_app *a, int result)
{
    if (a->mode_state == CP_MODE_REVERT) {
        if (result || cp_display_refresh(a) || active_mode(a) != a->previous_mode)
            finish(a, "The previous display mode could not be restored.");
        else finish(a, "The previous display mode was restored.");
        return;
    }
    if (a->mode_state != CP_MODE_WAIT) return;
    if (result) { finish(a, "The desktop refused the display mode."); return; }
    if (cp_display_refresh(a) || active_mode(a) != a->pending_mode) { cp_display_cancel(a); return; }
    a->mode_state = CP_MODE_CONFIRM; a->mode_deadline = a->ui->now + CONFIRM_TIME;
    static const char *const buttons[] = {"Keep", "Revert"};
    du_message(a->ui, CP_CONFIRM, "Display Settings", "Keep these settings?\nReverting in 15 seconds.", DU_DLG_ICON_QUESTION, buttons, 2, 1, 1);
    countdown(a); cp_display_sync(a);
}
static int persist(struct cp_app *a)
{
    /* Match the panel's other settings writes: bounded static scratch keeps
     * the i686 client stack below the SDK's 4 KiB frame warning threshold. */
    static struct ciuki_settings cfg; cfg_defaults(&cfg);
    static char buffer[CFG_TEXT_MAX]; size_t n = 0;
    if (a->host.load && !a->host.load(a->host.ctx, buffer, sizeof(buffer), &n)) cfg_parse(&cfg, buffer, n);
    cfg.display_mode = a->pending_mode;
    n = cfg_format(&cfg, buffer, sizeof(buffer));
    if (!n || !a->host.save || a->host.save(a->host.ctx, buffer, n)) return -1;
    a->cfg.display_mode = cfg.display_mode;
    return 0;
}
int cp_display_event(struct cp_app *a, const struct du_event *e)
{
    if (e->type == DU_EV_TICK && e->id == CP_MODE_TIMER) {
        if (a->mode_state && a->ui->now >= a->mode_deadline) {
            if (a->mode_state == CP_MODE_REVERT) finish(a, "No reply while restoring the display mode.");
            else cp_display_cancel(a);
        } else if (a->mode_state == CP_MODE_CONFIRM) countdown(a);
        return 1;
    }
    if (e->id == CP_CONFIRM && e->type == DU_EV_DIALOG) {
        if (a->mode_state != CP_MODE_CONFIRM) return 1;
        if (e->index || a->ui->now >= a->mode_deadline) cp_display_cancel(a);
        else if (cp_display_refresh(a) || active_mode(a) != a->pending_mode || persist(a)) {
            cp_display_cancel(a); label(a, 3, "Settings could not be saved; restoring the previous mode.");
        } else {
            finish(a, "Display settings saved.");
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
        a->pending_mode = selected_mode(a);
        cp_display_refresh(a); /* capture the actual previous mode at Apply, not at panel creation */
        a->previous_mode = active_mode(a);
        int listed = 0;
        for (unsigned i = 0; i < a->display.mode_count; i++)
            if (eligible(&a->display.modes[i]) && a->display.modes[i].id == a->pending_mode) listed = 1;
        if (!listed) { label(a, 3, "The selected display mode is no longer available."); return 1; }
        if (!a->pending_mode || !a->previous_mode || a->pending_mode == a->previous_mode) return 1;
        a->mode_state = CP_MODE_WAIT; a->mode_deadline = a->ui->now + CONFIRM_TIME;
        if (du_timer_start(a->ui, CP_MODE_TIMER, SECOND, 1) || request(a, a->pending_mode)) {
            finish(a, "The display mode could not be requested."); return 1;
        }
        label(a, 3, "Applying the display mode..."); cp_display_sync(a); return 1;
    }
    return 0;
}
