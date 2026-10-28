/* SPDX-License-Identifier: MIT
 * Serial Terminal window: the 80x24 grid, the Call, Transfer and View menus,
 * the Properties dialog and the status bar. */
#include "serial.h"
#include "../desktop/profile.h" /* sec-user-04: My Documents is per user */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CELL_W 7
#define CELL_H 16
#define GRID_X 4
#define GRID_Y 24
#define PUMP_NS 20000000ull

static struct du_rect grid_box(void) { return (struct du_rect){GRID_X, GRID_Y, SCREEN_COLS * CELL_W, SCREEN_ROWS * CELL_H}; }
static void status(struct serial_app *a)
{
    char text[256]; serial_status_line(&a->term, text, sizeof(text));
    struct du_widget *w = du_get(a->ui, SV_STATUS);
    if (strcmp(w->text, text)) { du_set_text(w, text); du_invalidate(a->ui, w->box); }
}
static void sync_menu(struct serial_app *a)
{
    const struct serial_term *t = &a->term; int busy = t->xm_active || t->text != NULL;
    a->call_items[0].flags = t->connected ? DU_ITEM_DISABLED : 0;
    a->call_items[1].flags = t->connected ? 0 : DU_ITEM_DISABLED;
    for (int i = 0; i < 3; i++) a->transfer_items[i].flags = t->connected && !busy ? 0 : DU_ITEM_DISABLED;
    a->transfer_items[3].flags = t->connected && !t->xm_active && !t->text ? 0 : DU_ITEM_DISABLED;
    a->transfer_items[4].flags = a->transfer_items[5].flags = a->transfer_items[3].flags;
    a->transfer_items[3].flags = (a->transfer_items[3].flags & DU_ITEM_DISABLED) | (t->capture ? DU_ITEM_CHECKED : 0);
    for (int i=8;i<14;i++) a->transfer_items[i].flags=t->connected && !busy ? 0 : DU_ITEM_DISABLED;
    a->transfer_items[7].flags = t->xm_active ? 0 : DU_ITEM_DISABLED;
    a->view_items[0].flags = t->local_echo ? DU_ITEM_CHECKED : 0; a->view_items[1].flags = t->ansi ? DU_ITEM_CHECKED : 0;
    a->view_items[2].flags = t->send_lf ? DU_ITEM_CHECKED : 0; a->view_items[3].flags = t->receive_lf ? DU_ITEM_CHECKED : 0;
    a->view_items[5].flags = t->connected ? (t->outputs & SERIAL_DTR ? DU_ITEM_CHECKED : 0) : DU_ITEM_DISABLED;
    a->view_items[6].flags = t->connected ? (t->outputs & SERIAL_RTS ? DU_ITEM_CHECKED : 0) : DU_ITEM_DISABLED;
}
void serial_paint(struct du_ui *u, void *user)
{
    struct serial_app *a = user; struct du_canvas c = u->canvas; struct du_rect box = grid_box();
    c.clip = du_intersection(c.clip, box);
    du_fill(&c, box, 0x000000u);
    const struct screen *s = &a->term.screen; int back = a->term.scroll_back;
    for (int y = 0; y < SCREEN_ROWS; y++) {
        int source = y - back;                    /* negative: scrolled-off lines above the live screen */
        char text[SCREEN_COLS + 1]; const struct screen_cell *cells = NULL;
        if (source >= 0) cells = s->cells[source];
        else if (!screen_history_text(s, (unsigned)(-source - 1), text)) continue;
        for (int x = 0; x < SCREEN_COLS; x++) {
            uint8_t ch = cells ? cells[x].ch : (uint8_t)text[x], attr = cells ? cells[x].attr : 0;
            if (!cells && !text[x]) break;
            if (ch == ' ' && !(attr & SCREEN_REVERSE)) continue;
            uint32_t ink = attr & SCREEN_BOLD ? 0xffffffu : 0xc0c0c0u, paper = 0;
            if (attr & SCREEN_REVERSE) { uint32_t t = ink; ink = paper; paper = t; du_fill(&c, (struct du_rect){box.x + x * CELL_W, box.y + y * CELL_H, CELL_W, CELL_H}, paper); }
            char letter[2] = {(char)ch, 0};
            if (ch != ' ') du_text_font(&c, DESK_FONT_MONO, box.x + x * CELL_W, box.y + y * CELL_H + 2, letter, ink);
        }
    }
    if (!back && a->term.connected && (u->now / 500000000ull) % 2 == 0)   /* a blinking block cursor */
        du_fill(&c, (struct du_rect){box.x + s->col * CELL_W, box.y + s->row * CELL_H + 13, CELL_W, 2}, 0xc0c0c0u);
}
static const struct du_menu_item call_template[] = {
    {"&Connect", SV_CONNECT, 0, 0, 0}, {"&Disconnect", SV_DISCONNECT, 0, 0, DU_ITEM_DISABLED}, {"&Properties...", SV_PROPERTIES, 0, 0, 0},
    {"", 0, 0, 0, DU_ITEM_SEPARATOR}, {"E&xit", SV_EXIT, DU_KEY_F4, DU_ALT, 0} };
static const struct du_menu_item transfer_template[] = {
    {"Send &Text File...", SV_SEND_TEXT, 0, 0, DU_ITEM_DISABLED}, {"Send File (&XMODEM)...", SV_XM_SEND, 0, 0, DU_ITEM_DISABLED},
    {"Send File (XMODEM-&1K)...", SV_XM_SEND_1K, 0, 0, DU_ITEM_DISABLED}, {"&Capture Text...", SV_CAPTURE, 0, 0, DU_ITEM_DISABLED},
    {"&Receive File (XMODEM)...", SV_XM_RECEIVE, 0, 0, DU_ITEM_DISABLED}, {"", 0, 0, 0, DU_ITEM_SEPARATOR},
    {"", 0, 0, 0, DU_ITEM_SEPARATOR}, {"Cancel &Transfer", SV_XM_CANCEL, 0, 0, DU_ITEM_DISABLED},
    {"Send File (YMODEM)...", SV_YM_SEND, 0, 0, DU_ITEM_DISABLED}, {"Receive File (YMODEM)...", SV_YM_RECEIVE, 0, 0, DU_ITEM_DISABLED},
    {"Send File (ZMODEM)...", SV_ZM_SEND, 0, 0, DU_ITEM_DISABLED}, {"Receive File (ZMODEM)...", SV_ZM_RECEIVE, 0, 0, DU_ITEM_DISABLED},
    {"Send File (Kermit)...", SV_KERMIT_SEND, 0, 0, DU_ITEM_DISABLED}, {"Receive File (Kermit)...", SV_KERMIT_RECEIVE, 0, 0, DU_ITEM_DISABLED} };
static const struct du_menu_item view_template[] = {
    {"&Local echo", SV_ECHO, 0, 0, 0}, {"&ANSI terminal", SV_ANSI, 0, 0, DU_ITEM_CHECKED}, {"Send line &feed with return", SV_SEND_LF, 0, 0, 0},
    {"Add line feed to &incoming returns", SV_RECEIVE_LF, 0, 0, 0}, {"C&lear screen", SV_CLEAR, 0, 0, 0}, {"&DTR", SV_DTR, 0, 0, DU_ITEM_DISABLED}, {"&RTS", SV_RTS, 0, 0, DU_ITEM_DISABLED} };
static const struct du_menu_item help_template[] = { {"&About Serial Terminal", SV_ABOUT, 0, 0, 0} };

void serial_attach(struct serial_app *a, struct du_ui *u)
{
    a->ui = u; u->event = serial_event; u->user = a; u->paint = serial_paint;
    memcpy(a->call_items, call_template, sizeof(call_template)); memcpy(a->transfer_items, transfer_template, sizeof(transfer_template));
    memcpy(a->view_items, view_template, sizeof(view_template)); memcpy(a->help_items, help_template, sizeof(help_template));
    a->menus[0] = (struct du_menu){"&Call", a->call_items, 5}; a->menus[1] = (struct du_menu){"&Transfer", a->transfer_items, 14};
    a->menus[2] = (struct du_menu){"&View", a->view_items, 7}; a->menus[3] = (struct du_menu){"&Help", a->help_items, 1};
    u->menus = a->menus; u->menu_count = 4;
    du_add(u, DU_CANVAS, SV_CANVAS, grid_box(), "")->accepts_tab = 1;
    du_add(u, DU_STATUS, SV_STATUS, (struct du_rect){0, u->canvas.height - 20, u->canvas.width, 20}, "");
    if (!a->folder) a->folder = profile_home();
    sync_menu(a); status(a); du_focus(u, SV_CANVAS); u->dirty = 1;
}
void serial_tick(struct serial_app *a, unsigned elapsed_ms)
{
    struct serial_term *t = &a->term; uint64_t before = t->received + t->transmitted; unsigned bells = t->screen.bells;   /* the bell redraws; no sound API yet */
    int had_transfer = t->xm_active || t->text != NULL;
    serial_pump(t, elapsed_ms);
    if (t->received + t->transmitted != before || had_transfer || bells != t->screen.bells) du_invalidate(a->ui, grid_box());
    if (had_transfer != (t->xm_active || t->text != NULL)) sync_menu(a);
    status(a);
}
static void properties(struct serial_app *a)
{
    char value[3][16]; struct serial_line *l = &a->term.line;
    snprintf(value[0], sizeof(value[0]), "%u", a->term.port); snprintf(value[1], sizeof(value[1]), "%u", l->baud); serial_format_text(l, value[2], sizeof(value[2]));
    static const char *const buttons[] = {"OK", "Cancel"};
    du_message(a->ui, SV_D_PROPS, "Connection Properties", "", 0, buttons, 2, 0, 1);
    struct du_dialog *d = &a->ui->dialog; int w = 360, h = 190;
    d->box = (struct du_rect){(a->ui->canvas.width - w) / 2, (a->ui->canvas.height - h) / 2, w, h};
    d->items[0].box = (struct du_rect){d->box.x + w - 174, d->box.y + h - 34, 75, 23};
    d->items[1].box = (struct du_rect){d->box.x + w - 93, d->box.y + h - 34, 75, 23};
    static const char *const names[3] = {"&Port (COM 1-8):", "&Bits per second:", "&Format (8N1, 7E1...):"};
    for (unsigned i = 0; i < 3; i++) {
        struct du_widget *label = &d->items[2 + i * 2], *edit = &d->items[3 + i * 2];
        memset(label, 0, sizeof(*label)); label->kind = DU_LABEL; label->id = -8000 - (int)(2 + i * 2); label->total = 1;
        label->box = (struct du_rect){d->box.x + 14, d->box.y + 36 + (int)i * 34, 160, 22}; du_set_text(label, names[i]);
        memset(edit, 0, sizeof(*edit)); edit->kind = DU_EDIT; edit->id = -8000 - (int)(3 + i * 2); edit->total = 1;
        edit->box = (struct du_rect){d->box.x + 184, d->box.y + 34 + (int)i * 34, 100, 22}; du_set_text(edit, value[i]);
    }
    d->count = 8; d->focus = 3;
}
static int apply_properties(struct serial_app *a)
{
    struct du_dialog *d = &a->ui->dialog; char *end;
    long port = strtol(d->items[3].text, &end, 10); if (*end || port < 1 || port > SERIAL_PORTS) return -EINVAL;
    long baud = strtol(d->items[5].text, &end, 10); if (*end || baud < 0 || !serial_baud_valid((unsigned)baud)) return -EINVAL;
    struct serial_line line = {(unsigned)baud, 8, 0, 1};
    if (serial_parse_format(d->items[7].text, &line)) return -EINVAL;
    int r = serial_set_line(&a->term, &line);
    if (!r && (unsigned)port != a->term.port) r = serial_set_port(&a->term, (unsigned)port);
    return r;
}
static void file_dialog(struct serial_app *a, int id, int save, const char *title, const char *name)
{
    if (du_file_dialog(a->ui, id, save, title, a->folder, name, du_posix_lister, NULL)) du_message(a->ui, SV_D_INFO, "Serial Terminal", "Not enough memory.", DU_DLG_ICON_WARNING, NULL, 0, 0, 0);
}
static void report(struct serial_app *a, int r)
{
    if (!r) return;
    if (!a->term.message[0]) snprintf(a->term.message, sizeof(a->term.message), "%s", strerror(-r));   /* the model already explained a connection failure */
    du_message(a->ui, SV_D_INFO, "Serial Terminal", a->term.message, DU_DLG_ICON_WARNING, NULL, 0, 0, 0);
}
void serial_event(struct du_ui *u, const struct du_event *e, void *user)
{
    struct serial_app *a = user; struct serial_term *t = &a->term;
    switch (e->type) {
    case DU_EV_TICK: if (e->id == SV_TIMER) serial_tick(a, 20); return;
    case DU_EV_KEY:
        if (e->key == DU_KEY_PGUP || e->key == DU_KEY_PGDN) {
            t->scroll_back += e->key == DU_KEY_PGUP ? SCREEN_ROWS / 2 : -SCREEN_ROWS / 2;
            if (t->scroll_back < 0) t->scroll_back = 0;
            if (t->scroll_back > (int)t->screen.history_count) t->scroll_back = (int)t->screen.history_count;
            du_invalidate(u, grid_box()); return;
        }
        if (!t->connected) return;
        if (serial_key(t, e->key, e->ch, u->modifiers)) { serial_pump(t, 0); du_invalidate(u, grid_box()); status(a); }
        return;
    case DU_EV_DIALOG:
        if (e->index != 0) { a->xm_kind = 0; return; }
        t->message[0] = 0;
        if (e->id == SV_D_PROPS) { int r = apply_properties(a); if (r) { report(a, r == -EINVAL ? -EINVAL : r); } sync_menu(a); status(a); return; }
        if (e->id == SV_D_TEXT && e->text) report(a, serial_send_text_file(t, e->text));
        else if (e->id == SV_D_CAPTURE && e->text) report(a, serial_capture_start(t, e->text));
        else if ((e->id == SV_D_SEND || e->id == SV_D_RECEIVE) && e->text && a->xm_kind >= 10)
            report(a, serial_transfer_start(t,e->text,a->xm_kind-10,e->id==SV_D_SEND));
        else if (e->id == SV_D_SEND && e->text) report(a, serial_xmodem_send(t, e->text, a->xm_kind == 2));
        else if (e->id == SV_D_RECEIVE && e->text) report(a, serial_xmodem_receive(t, e->text, 1));
        sync_menu(a); status(a); return;
    case DU_EV_ACTIVATE:
        t->message[0] = 0;
        switch (e->id) {
        case SV_CONNECT: report(a, serial_connect(t)); break;
        case SV_DISCONNECT: serial_disconnect(t); break;
        case SV_PROPERTIES: properties(a); break;
        case SV_EXIT: if (a->quit) a->quit(a->host); break;
        case SV_SEND_TEXT: file_dialog(a, SV_D_TEXT, 0, "Send Text File", ""); break;
        case SV_CAPTURE: if (t->capture) serial_capture_stop(t); else file_dialog(a, SV_D_CAPTURE, 1, "Capture Text", "capture.txt"); break;
        case SV_YM_SEND: case SV_YM_RECEIVE: case SV_ZM_SEND: case SV_ZM_RECEIVE: case SV_KERMIT_SEND: case SV_KERMIT_RECEIVE: {
            int sending=(e->id-SV_YM_SEND)%2==0;
            int protocol=(e->id-SV_YM_SEND)/2+1;a->xm_kind=10+protocol;
            file_dialog(a,sending ? SV_D_SEND : SV_D_RECEIVE,!sending,sending ? "Send File" : "Receive File",sending ? "" : "received.bin");break;
        }
        case SV_XM_SEND: case SV_XM_SEND_1K: a->xm_kind = e->id == SV_XM_SEND ? 1 : 2; file_dialog(a, SV_D_SEND, 0, "Send File", ""); break;
        case SV_XM_RECEIVE: a->xm_kind = 3; file_dialog(a, SV_D_RECEIVE, 1, "Receive File", "received.bin"); break;
        case SV_XM_CANCEL: serial_xmodem_cancel(t); break;
        case SV_ECHO: t->local_echo = !t->local_echo; break;
        case SV_ANSI: t->ansi = !t->ansi; t->screen.ansi = t->ansi; break;
        case SV_SEND_LF: t->send_lf = !t->send_lf; break;
        case SV_RECEIVE_LF: t->receive_lf = !t->receive_lf; break;
        case SV_CLEAR: serial_clear(t); du_invalidate(u, grid_box()); break;
        case SV_DTR: report(a, serial_set_modem(t, t->outputs ^ SERIAL_DTR)); break;
        case SV_RTS: report(a, serial_set_modem(t, t->outputs ^ SERIAL_RTS)); break;
        case SV_ABOUT:
            du_message(u, SV_D_INFO, "About Serial Terminal", "CiukiOS Serial Terminal - Classic computing, one woof at a time.\nNative COM ports, TTY/ANSI, text capture, X/Y/ZMODEM, Kermit.\nCode: MIT.", DU_DLG_ICON_INFO, NULL, 0, 0, 0);
            break;
        default: return;
        }
        sync_menu(a); status(a); du_invalidate(u, grid_box());
        return;
    default: return;
    }
}
