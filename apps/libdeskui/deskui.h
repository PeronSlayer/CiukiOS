/* SPDX-License-Identifier: MIT
 * libdeskui: integer-only widget toolkit for CiukiOS desktop clients (f2-31).
 *
 * One client window per desktop channel (protocol v2). Everything, dialogs
 * included, is drawn inside that window's surface. Rendering is a pure
 * function of the widget state: equal state gives equal pixels. */
#ifndef CIUKI_DESKUI_H
#define CIUKI_DESKUI_H
#include <stdint.h>
#include <stddef.h>
#include "../desktop/protocol.h"

/* CiukiOS identity colours on a Windows 98 class layout. FACE equals the
 * compositor's window frame and taskbar; SELECT equals its focused title. */
#define DU_FACE       0xb8c9ccu
#define DU_LIGHT      0xdbe5e7u
#define DU_HILITE     0xffffffu
#define DU_SHADOW     0x6c8489u
#define DU_DARK       0x14262eu
#define DU_WINDOW     0xffffffu
#define DU_INK        0x10212au
#define DU_GRAY       0x6c8489u
#define DU_SELECT     0x213c4cu
#define DU_SELECT_INK 0xffffffu
#define DU_CREAM      0xe8d7b9u

/* The compositor's own glyph table (5x7, apps/desktop/compositor.c), drawn
 * at its scale and advance so client text matches window titles. */
#define DU_CHAR_W  6
#define DU_GLYPH_H 7
#define DU_ROW     17   /* list, tree and menu rows */

#define DU_WIDGETS       48
#define DU_DIALOG_ITEMS  10
#define DU_TIMERS        8
#define DU_TEXT_MAX      1040
#define DU_COLUMNS       8   /* f2-33: Task Manager Processes uses six */
#define DU_DIALOG_TEXT   1024
#define DU_FILE_ENTRIES  512

#define DU_SHIFT 1u
#define DU_CTRL  2u
#define DU_ALT   4u

/* Set-1 scan codes used by the toolkit and its applications. */
enum {
    DU_KEY_ESC = 0x01, DU_KEY_BACKSPACE = 0x0e, DU_KEY_TAB = 0x0f, DU_KEY_ENTER = 0x1c,
    DU_KEY_LCTRL = 0x1d, DU_KEY_LSHIFT = 0x2a, DU_KEY_RSHIFT = 0x36, DU_KEY_LALT = 0x38,
    DU_KEY_SPACE = 0x39, DU_KEY_CAPS = 0x3a, DU_KEY_F1 = 0x3b, DU_KEY_F2 = 0x3c,
    DU_KEY_F4 = 0x3e, DU_KEY_F5 = 0x3f, DU_KEY_F10 = 0x44,
    DU_KEY_RCTRL = 0x11d, DU_KEY_RALT = 0x138, DU_KEY_HOME = 0x147, DU_KEY_UP = 0x148,
    DU_KEY_PGUP = 0x149, DU_KEY_LEFT = 0x14b, DU_KEY_RIGHT = 0x14d, DU_KEY_END = 0x14f,
    DU_KEY_DOWN = 0x150, DU_KEY_PGDN = 0x151, DU_KEY_INSERT = 0x152, DU_KEY_DELETE = 0x153,
    DU_KEY_MENU = 0x15d
};

struct du_rect { int x, y, w, h; };
struct du_canvas { uint32_t *pixels; int width, height; struct du_rect clip; };

/* Bevel styles (Windows 98 classic edges). */
enum du_edge { DU_RAISED, DU_SUNKEN, DU_PRESSED, DU_RAISED_THIN, DU_SUNKEN_THIN, DU_ETCHED, DU_BUTTON_EDGE };
/* Icon blending modes. */
enum du_icon_mode { DU_ICON_NORMAL, DU_ICON_SELECTED, DU_ICON_GHOST, DU_ICON_DISABLED };

int du_contains(struct du_rect, int x, int y);
struct du_rect du_intersection(struct du_rect, struct du_rect);
void du_fill(struct du_canvas *, struct du_rect, uint32_t);
void du_hline(struct du_canvas *, int x, int y, int w, uint32_t);
void du_vline(struct du_canvas *, int x, int y, int h, uint32_t);
void du_outline(struct du_canvas *, struct du_rect, uint32_t);
void du_edge(struct du_canvas *, struct du_rect, enum du_edge);
void du_dotted(struct du_canvas *, struct du_rect);
void du_checker(struct du_canvas *, struct du_rect, uint32_t, uint32_t);
void du_triangle(struct du_canvas *, int cx, int cy, int dir, int size, uint32_t);
size_t du_cells(const char *);
int du_text_width(const char *);
void du_text(struct du_canvas *, int x, int y, const char *, uint32_t);
void du_text_n(struct du_canvas *, int x, int y, const char *, size_t cells, uint32_t);
void du_text_fit(struct du_canvas *, int x, int y, int width, const char *, uint32_t);
void du_text_disabled(struct du_canvas *, int x, int y, const char *);
void du_label(struct du_canvas *, int x, int y, const char *mnemonic, uint32_t, int underline);
void du_icon(struct du_canvas *, int x, int y, const uint32_t *argb, int size, enum du_icon_mode);
size_t du_utf8_prev(const char *, size_t);
size_t du_utf8_next(const char *, size_t);
void du_format_key(char *out, size_t size, unsigned key, unsigned modifiers);

/* List and tree content belongs to the application; the toolkit only reads it. */
struct du_item {
    const char *text[DU_COLUMNS];
    const uint32_t *icon16, *icon32;
    uint8_t selected, ghost, depth, expanded, children;
};
struct du_column { const char *title; int width, right; };
enum du_view { DU_DETAILS, DU_ICONS, DU_LIST };

enum du_kind { DU_LABEL, DU_BUTTON, DU_TOOLBUTTON, DU_EDIT, DU_LISTVIEW, DU_TREE,
               DU_SCROLLBAR, DU_STATUS, DU_SPLITTER, DU_PROGRESS, DU_HEADER, DU_SEPARATOR,
               /* f2-33: tab control (items[i].text[0] are the labels, active is the
                * selected tab; the box spans strip and page), history graph and
                * labelled group frame. */
               DU_TABS, DU_GRAPH, DU_GROUP };
#define DU_TAB_H 20

struct du_widget {
    enum du_kind kind;
    int id;
    struct du_rect box;
    uint8_t hidden, disabled, pressed, hot, def, vertical;
    char text[DU_TEXT_MAX];
    size_t cursor, anchor, offset;          /* edit (bytes; offset in cells) */
    const uint32_t *icon; int icon_size;    /* buttons, edit prefix icon */
    int dropdown;                           /* toolbutton split arrow width */
    const struct du_item *items;            /* list/tree */
    unsigned count, active;
    enum du_view view;
    struct du_column columns[DU_COLUMNS];
    unsigned column_count;
    int sort, descending, focus_rect, inactive_selection;
    int scroll, hscroll;                    /* first row/column or pixel offset */
    int value, total, page;                 /* scrollbar, progress, splitter */
    int panels[4];                          /* status panel widths; 0 = rest */
    int drag, drag_offset;                  /* internal pointer capture state */
    const uint8_t *samples;                 /* graph: 0..100, oldest first; total = capacity */
    unsigned sample_count;
};

#define DU_ITEM_DISABLED  1u
#define DU_ITEM_CHECKED   2u
#define DU_ITEM_RADIO     4u
#define DU_ITEM_SEPARATOR 8u
#define DU_ITEM_DEFAULT   16u
struct du_menu_item { const char *label; int id; unsigned key, modifiers, flags; };
struct du_menu { const char *label; struct du_menu_item *items; unsigned count; };

enum du_event_type {
    DU_EV_ACTIVATE = 1, DU_EV_SELECT, DU_EV_OPEN, DU_EV_SORT, DU_EV_CONTEXT, DU_EV_CHANGE,
    DU_EV_COMMIT, DU_EV_CANCEL, DU_EV_KEY, DU_EV_TICK, DU_EV_DIALOG, DU_EV_FOCUS
};
struct du_event {
    enum du_event_type type;
    int id, index, x, y;
    unsigned modifiers, key, ch, keyboard;
    const char *text;
};

enum du_dialog_kind { DU_DLG_NONE, DU_DLG_MESSAGE, DU_DLG_INPUT, DU_DLG_OPEN, DU_DLG_SAVE, DU_DLG_PROGRESS };
enum du_dialog_icon { DU_DLG_ICON_NONE, DU_DLG_ICON_INFO, DU_DLG_ICON_WARNING, DU_DLG_ICON_ERROR, DU_DLG_ICON_QUESTION };

/* Directory entries for the open/save dialog, from a replaceable lister. */
struct du_file_entry { char name[256]; int directory; uint64_t size; };
typedef int (*du_lister)(void *ctx, const char *path, struct du_file_entry *out, unsigned capacity, unsigned *count);
int du_posix_lister(void *ctx, const char *path, struct du_file_entry *out, unsigned capacity, unsigned *count);

struct du_dialog {
    enum du_dialog_kind kind;
    int id, icon, cancel, focus, result;
    struct du_rect box;
    char title[96], text[DU_DIALOG_TEXT], folder[DU_TEXT_MAX];
    struct du_widget items[DU_DIALOG_ITEMS];
    unsigned count, buttons;
    unsigned progress, progress_total;
    du_lister lister; void *lister_ctx;
    struct du_file_entry *entries; struct du_item *rows; unsigned entry_count;
    int dragging, drag_x, drag_y, capture;
    const uint32_t *icon_pixels;            /* optional 32x32 replacement icon */
    struct du_event pending[4]; unsigned pending_count;
};

struct du_timer { uint64_t deadline, interval; int id, active, repeat; };

struct du_ui {
    struct du_canvas canvas;
    struct du_widget widgets[DU_WIDGETS];
    unsigned count;
    int focus, capture, hover;
    struct du_menu *menus; unsigned menu_count;
    int menu, menu_item, menu_keyboard, menu_armed, menu_track;
    const struct du_menu_item *popup; unsigned popup_count; int popup_x, popup_y, popup_owner;
    struct du_dialog dialog;
    struct du_timer timers[DU_TIMERS];
    int mouse_x, mouse_y, focused_window;
    unsigned modifiers, buttons, caps;
    uint8_t keys[0x201];
    uint64_t now, last_click_ns; int last_click_x, last_click_y, last_click_widget, last_click_index;
    int dirty;
    void (*event)(struct du_ui *, const struct du_event *, void *);
    void *user;
};

/* Core */
void du_init(struct du_ui *, uint32_t *pixels, int width, int height);
struct du_widget *du_add(struct du_ui *, enum du_kind, int id, struct du_rect, const char *text);
struct du_widget *du_get(struct du_ui *, int id);
int du_hit(const struct du_ui *, int x, int y);
void du_focus(struct du_ui *, int id);
int du_focused(const struct du_ui *);
void du_set_text(struct du_widget *, const char *);
void du_render(struct du_ui *);
void du_input(struct du_ui *, const struct ciuki_input_event *);
void du_window_focus(struct du_ui *, int focused);
void du_release(struct du_ui *);
unsigned du_key_char(const struct du_ui *, unsigned code);
void du_post(struct du_ui *, enum du_event_type, int id, int index);

/* Lists and trees */
int du_list_hit(const struct du_widget *, int x, int y);
struct du_rect du_list_label(const struct du_widget *, unsigned index);
struct du_rect du_list_body(const struct du_widget *);
void du_list_ensure(struct du_widget *, unsigned index);
int du_list_page(const struct du_widget *);
int du_list_target(const struct du_widget *, unsigned code);
int du_header_hit(const struct du_widget *, int x, int y);
int du_tree_toggle_hit(const struct du_widget *, int x, int y);
int du_tabs_hit(const struct du_widget *, int x, int y);          /* f2-33: tab index or -1 */

/* Menus */
void du_menu_open(struct du_ui *, int menu, int keyboard);
void du_menu_close(struct du_ui *);
void du_popup(struct du_ui *, const struct du_menu_item *, unsigned, int x, int y, int owner);
struct du_rect du_menu_title(const struct du_ui *, unsigned index);
struct du_rect du_menu_box(const struct du_ui *, const struct du_menu_item *, unsigned, int x, int y);
int du_menu_item_at(const struct du_menu_item *, unsigned, struct du_rect box, int y);

/* Dialogs: modal panels inside the window. DU_EV_DIALOG reports the
 * pressed button index (cancel index for Esc) and the edit text. */
void du_message(struct du_ui *, int id, const char *title, const char *text, int icon,
                const char *const *buttons, unsigned count, int def, int cancel);
void du_input_box(struct du_ui *, int id, const char *title, const char *prompt, const char *initial);
int du_file_dialog(struct du_ui *, int id, int save, const char *title, const char *folder,
                   const char *name, du_lister, void *ctx);
void du_progress(struct du_ui *, int id, const char *title, const char *text, unsigned value, unsigned total);
void du_dialog_icon(struct du_ui *, const uint32_t *argb32);
void du_dialog_close(struct du_ui *);
int du_dialog_id(const struct du_ui *);
struct du_rect du_dialog_button(const struct du_ui *, unsigned index);

/* Timers (monotonic nanoseconds) and the per-process text clipboard. */
int du_timer_start(struct du_ui *, int id, uint64_t interval_ns, int repeat);
void du_timer_stop(struct du_ui *, int id);
void du_tick(struct du_ui *, uint64_t now);
uint64_t du_next_deadline(const struct du_ui *);
void du_clipboard_set(const char *);
const char *du_clipboard_get(void);

/* Desktop window glue: handshake, surface, back buffer, damage, events. */
struct du_window {
    int channel, surface, configured, closing, closed, error;
    uint32_t server_pid, ping_serial;
    struct ciuki_surface_info info;
    uint32_t *pixels, *back;
    struct ciuki_message tx[16]; unsigned head, count;
    struct du_rect damage; int damaged;
    struct du_ui *ui;
    int (*close_request)(struct du_window *, void *);
    void *user;
    /* f2-33: v2 messages the glue does not consume (WINDOW_INFO, TASK_RESULT). */
    void (*message)(struct du_window *, const struct desk_packet *, void *);
};
int du_window_open(struct du_window *, struct du_ui *, int channel, int width, int height, const char *title);
int du_window_turn(struct du_window *, uint64_t now);
void du_window_present(struct du_window *);
int du_window_ping(struct du_window *, uint32_t serial);
int du_window_queue(struct du_window *, struct ciuki_message *);  /* f2-33: any v2 request */
int du_window_move(struct du_window *, int x, int y);
void du_window_close(struct du_window *);
void du_window_destroy(struct du_window *);
int du_run(struct du_window *, void (*idle)(void *), void *);
uint64_t du_now(void);
#endif
