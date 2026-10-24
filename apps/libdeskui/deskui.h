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
#include "../desktop/theme.h"

/* Colours, fonts and styles come from the current theme (desk_theme,
 * apps/desktop/theme.h): Ciuki Blue (default) or Platinum.
 * These names describe control roles; they are not constants. */
#define DU_FACE       (desk_theme->face)
#define DU_LIGHT      (desk_theme->light)
#define DU_HILITE     (desk_theme->hilite)
#define DU_SHADOW     (desk_theme->shadow)
#define DU_DARK       (desk_theme->dark)
#define DU_WINDOW     (desk_theme->window)
#define DU_INK        (desk_theme->ink)
#define DU_GRAY       (desk_theme->gray)
#define DU_SELECT     (desk_theme->select)
#define DU_SELECT_INK (desk_theme->select_ink)
#define DU_ACCENT     (desk_theme->accent)
#define DU_CREAM      0xe8d7b9u

/* Text: anti-aliased proportional faces (theme.h DESK_FONT_*). du_text and
 * friends take the top of the capitals as y; DU_GLYPH_H is the capital
 * height of the UI face, so (h - DU_GLYPH_H) / 2 centres a line in h rows.
 * DU_CHAR_W is only an average advance for rough sizing. */
#define DU_CHAR_W  7
#define DU_GLYPH_H 9
#define DU_ROW     17   /* list, tree and menu-bar rows */
#define DU_MENU_ITEM_H 19
#define DU_MENU_SEP_H  9
/* Icon slots (16 menus/lists, 22 toolbars, 32 large icons and dialogs). */
#define DU_ICON_SMALL   (desk_theme->icon_small)
#define DU_ICON_TOOLBAR (desk_theme->icon_toolbar)
#define DU_ICON_LARGE   (desk_theme->icon_large)

#define DU_WIDGETS       64
#define DU_DIALOG_ITEMS  10
#define DU_TIMERS        8
#define DU_REGIONS       32
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

struct du_ui;
struct du_rect { int x, y, w, h; };
struct du_canvas { uint32_t *pixels; int width, height; struct du_rect clip; };
/* Local XRGB command batches for client-owned canvases. These never submit
 * arbitrary user pointers to the display service. MONO, INVERT and KEYED
 * share its pixel conventions; inaccessible native targets use this path. */
int du_commands(struct du_canvas *, const struct ciuki_batch_record *, unsigned,
                const void *payload, const uint32_t *source, unsigned source_width, unsigned source_height);
struct du_selection {
    uint64_t ns;
    int x, y;
    unsigned clicks;
    size_t lo, hi;
};
/* UTF-8 byte boundaries; granularity 1 = position, 2 = word, 3 = logical line. */
void du_text_unit(const char *, size_t at, unsigned granularity, size_t *lo, size_t *hi);
void du_select_click(struct du_selection *, const char *, size_t at, int extend,
                     uint64_t ns, int x, int y, size_t *anchor, size_t *cursor);
void du_select_drag(const struct du_selection *, const char *, size_t at, size_t *anchor, size_t *cursor);

/* Bevel styles for raised and recessed controls. */
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
/* NULL canvas measures only; explicit lines and UTF-8 boundaries survive. */
int du_text_wrap(struct du_canvas *, struct du_rect, const char *, uint32_t);
void du_progress_paint(struct du_canvas *, struct du_rect, int value, int total, uint64_t phase_ms);
void du_message_size(const char *, int icon, unsigned buttons, int max_width, int *width, int *height);
const char *du_message_icon_name(int icon);
unsigned du_message_sound(int icon);
void du_text_disabled(struct du_canvas *, int x, int y, const char *);
void du_label(struct du_canvas *, int x, int y, const char *mnemonic, uint32_t, int underline);
/* Straight-alpha AARRGGBB sprite, size x size (at most 64), alpha-blended. */
void du_icon_blit(struct du_canvas *, int x, int y, const uint32_t *argb, int size, enum du_icon_mode);
size_t du_utf8_prev(const char *, size_t);
size_t du_utf8_next(const char *, size_t);
void du_format_key(char *out, size_t size, unsigned key, unsigned modifiers);
/* ui-look: faces, mnemonic labels, theme-drawn faces and focus. */
void du_text_font(struct du_canvas *, int font, int x, int y, const char *, uint32_t);   /* y: capital top */
int du_text_width_font(int font, const char *);
/* Optional scalable grey-AA text, 26.6 em pixels. y is the baseline. Link
 * with -lfont; retain a face for repeated painting. Errors return -1/errno. */
struct ciuki_font_face;
int du_text_face(struct du_canvas *, struct ciuki_font_face *, unsigned em_size,
                 int x, int y, const char *, uint32_t);
int du_text_ttf(struct du_canvas *, const char *font_path, unsigned em_size,
                int x, int y, const char *, uint32_t);
int du_label_width(const char *mnemonic);                                               /* '&' not counted */
void du_raised(struct du_canvas *, struct du_rect, int pressed, int hot);                /* theme button face */
void du_focus_ring(struct du_canvas *, struct du_rect);                                  /* theme focus cue */
void du_well(struct du_canvas *, struct du_rect, uint32_t fill);                         /* sunken field */
void du_shadow(struct du_canvas *, struct du_rect, int size);                            /* soft drop shadow */
/* Current theme by name (Platinum, Ciuki Blue): 0, or -1 and no
 * change. Marks the UI dirty. Persisting it is the caller's job (the theme
 * key of DESK_SETTINGS_PATH, written with settings.c cfg_format); every
 * client follows the file through desk_theme_poll. */
int du_theme_use(struct du_ui *, const char *name);
const char *du_theme_name(void);

/* System icons: Tango Icon Theme 0.8.90 (Public Domain), one store file
 * (/system/icons/tango.cis, built by apps/desktop/convert_icons.py; format in
 * assets/icons/README.md). du_icon() returns the icon of a Tango name
 * ("folder", or "places/folder") at 16, 22, 32 or 48 pixels, or NULL when the
 * name, size or store is unavailable. Icons are decoded on first use and stay
 * valid for the life of the process. DU_ICON_CIUKI is the identity icon, the
 * approved Ciuki portrait conversion (/system/assets/ciuki-portrait.xrgb)
 * reduced with the store's area filter. Not thread-safe. */
struct du_icon { unsigned size; const uint32_t *argb; };   /* straight-alpha AARRGGBB */
#define DU_ICON_CIUKI "ciuki"
const struct du_icon *du_icon(const char *tango_name, unsigned size);
const uint32_t *du_icon_argb(const char *tango_name, unsigned size);   /* du_icon()->argb or NULL */
void du_icon_draw(struct du_canvas *, int x, int y, const struct du_icon *, enum du_icon_mode);
int du_icon_paths(const char *store, const char *portrait);   /* before the first lookup; NULL keeps the default */
unsigned du_icon_count(void);                                /* names in the store */
const char *du_icon_store_name(unsigned index);
const char *du_icon_notice(void);                            /* the store's Tango licence notice */
/* Read-only launch diagnostics; does not open a store or decode an icon. */
struct du_icon_stats {
    uint64_t store_bytes, portrait_bytes, read_requests;
    uint32_t read_max, decoded, cached, heap_bytes, peak_bytes;
};
void du_icon_snapshot(struct du_icon_stats *);

/* CiukiOS objects -> Tango names (icon_map.c). Keys are "<kind>:<object>":
 * program:<name in /Programs>, menu:, action:, place:, type:folder|executable|unknown,
 * type:.<extension>, status:, dialog:, device:, settings:, category:, emblem:, wm:.
 * An unknown object of a known kind gets that kind's default; a key without
 * a colon is returned unchanged (a Tango name). */
struct du_icon_map { const char *object, *tango; };
extern const struct du_icon_map du_icon_map[];
extern const unsigned du_icon_map_count;
const char *du_icon_name(const char *object);
const char *du_icon_file(const char *filename, int directory, int executable);
const struct du_icon *du_icon_for(const char *object, unsigned size);   /* du_icon(du_icon_name(object), size) */

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
               DU_TABS, DU_GRAPH, DU_GROUP,
               /* f2-32: multi-line editor; widget.data is a struct du_textarea
                * (textarea.h) owned by the application. */
               DU_TEXTAREA,
               /* ui-look: check box and radio button (text is the label, value
                * is the state; radios with the same `total` form one group),
                * and a tool-bar band that draws the strip behind tool buttons. */
               DU_CHECKBOX, DU_RADIO, DU_TOOLBAR,
               /* Application-drawn canvas; pointer events retain capture for
                * either drawing button, including motion outside its box. */
               DU_CANVAS };
#define DU_TAB_H 20

struct du_widget {
    /* fx-19 shell drag and drop on lists and trees: file_drag makes the
     * widget a drag source (DU_EV_DRAG once the pointer leaves the
     * SM_CXDRAG square, key = button 1/2; a right press then shows the
     * context menu on release), file_drag_button/x/y are its press state.
     * marquee enables rubber-band selection from empty space (DU_EV_MARQUEE:
     * index 0 begin, 1 update, 2 end, x/y/key unused; du_list_in_band).
     * drop_hot is the drop-target highlight, item index + 1 (0 none). */
    int file_drag, file_drag_x, file_drag_y, file_drag_button, file_drag_pending;
    int marquee, band, band_x0, band_y0, band_x1, band_y1, drop_hot;
    enum du_kind kind;
    int id;
    struct du_rect box;
    unsigned anchors;                      /* DU_ANCHOR_*; zero keeps top/left */
    uint8_t hidden, disabled, pressed, hot, def, vertical;
    char text[DU_TEXT_MAX];
    size_t cursor, anchor, offset;          /* edit (bytes; offset in cells) */
    struct du_selection selection;
    const uint32_t *icon; int icon_size;    /* buttons, edit prefix icon */
    int accepts_tab;                        /* DU_CANVAS: deliver Tab as DU_EV_KEY */
    int wheel_events;                       /* DU_CANVAS: deliver the wheel as DU_POINTER_WHEEL (G28; off keeps the old behaviour) */
    int dropdown;                           /* toolbutton split arrow width */
    const struct du_item *items;            /* list/tree */
    unsigned count, active;
    enum du_view view;
    struct du_column columns[DU_COLUMNS];
    unsigned column_count;
    int sort, descending, focus_rect, inactive_selection;
    int selection_external, selection_active; /* lists: controller-owned multi-selection, or toolkit active item */
    int scroll, hscroll;                    /* first row/column or pixel offset */
    int value, total, page;                 /* scrollbar, progress, splitter */
    int panels[4];                          /* status panel widths; 0 = rest */
    int drag, drag_offset;                  /* internal pointer capture state */
    const uint8_t *samples;                 /* graph: 0..100, oldest first; total = capacity */
    unsigned sample_count;
    void *data;                             /* f2-32: DU_TEXTAREA state */
    unsigned pointer_cursor;               /* application cursor for this widget; arrow is automatic */
    struct du_rect cursor_box;              /* optional client-coordinate subregion */
};

#define DU_ANCHOR_LEFT   1u
#define DU_ANCHOR_RIGHT  2u
#define DU_ANCHOR_TOP    4u
#define DU_ANCHOR_BOTTOM 8u
#define DU_ANCHOR_GROW_X (DU_ANCHOR_LEFT | DU_ANCHOR_RIGHT)
#define DU_ANCHOR_GROW_Y (DU_ANCHOR_TOP | DU_ANCHOR_BOTTOM)

#define DU_ITEM_DISABLED  1u
#define DU_ITEM_CHECKED   2u
#define DU_ITEM_RADIO     4u
#define DU_ITEM_SEPARATOR 8u
#define DU_ITEM_DEFAULT   16u
struct du_menu_item { const char *label; int id; unsigned key, modifiers, flags; };
struct du_menu { const char *label; struct du_menu_item *items; unsigned count; };

enum du_event_type {
    DU_EV_ACTIVATE = 1, DU_EV_SELECT, DU_EV_OPEN, DU_EV_SORT, DU_EV_CONTEXT, DU_EV_CHANGE,
    DU_EV_COMMIT, DU_EV_CANCEL, DU_EV_KEY, DU_EV_TICK, DU_EV_DIALOG, DU_EV_FOCUS,
    DU_EV_POINTER, DU_EV_DRAG, DU_EV_CONSUMER,
    /* fx-19: rubber-band selection phases, and files dropped on the window
     * (x/y client position; du_drop_file reads the paths). */
    DU_EV_MARQUEE, DU_EV_DROPFILES
};
/* fx-19 DU_EV_SELECT .key from a file_drag list press: PENDING on an
 * already selected item without Shift (keep the selection so it can be
 * dragged), RELEASE when that press ends without a drag (apply it now). */
#define DU_SELECT_PENDING 0x1000u
#define DU_SELECT_RELEASE 0x2000u
/* DU_EV_POINTER: index is the phase, key is the captured button (1/2/3,
 * zero on hover), x/y are client coordinates. CANCEL follows focus loss,
 * input resynchronization or resize. The application paints DU_CANVAS.
 * G28: DU_POINTER_WHEEL (only for canvases with wheel_events set) reports the
 * wheel: x is the signed number of detents (positive scrolls up), y the pointer's
 * client y. */
enum du_pointer_phase { DU_POINTER_DOWN, DU_POINTER_MOVE, DU_POINTER_UP, DU_POINTER_CANCEL, DU_POINTER_WHEEL };
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
    struct du_dialog_places *places;        /* owned, live file-dialog Places observer */
    int dragging, drag_x, drag_y, capture;
    int close_hot;                          /* the pointer is over the close button */
    const uint32_t *icon_pixels;            /* optional 32x32 replacement icon */
    struct du_event pending[4]; unsigned pending_count;
    /* Optional custom-controls validation before values are released. Zero
     * closes the dialog; nonzero keeps it open (for example invalid sizes).
     * Called for Cancel too. The callback may read items but must not replace
     * the dialog while it is running. */
    int (*accept)(struct du_ui *, int button, void *);
    void *accept_ctx;
    /* Property-sheet pages reuse the modal controls and their keyboard route. */
    int sheet;
    void (*changed)(struct du_ui *, const struct du_event *, void *);
    void (*paint)(struct du_ui *, void *);
    void *sheet_ctx;
};

struct du_timer { uint64_t deadline, interval; int id, active, repeat; };

struct du_ui {
    void (*sound)(void *, unsigned);
    void *sound_ctx;
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
    struct du_rect invalid; /* dirty=3: union of clipped client invalidations */
    struct du_rect rendered; /* covering bounds, retained for existing clients */
    struct du_rect updates[DU_REGIONS], frames[DU_REGIONS], copied[DU_REGIONS];
    unsigned update_count, frame_count, copied_count;
    unsigned cursor;                       /* application default, used outside text/splitter controls */
    void (*event)(struct du_ui *, const struct du_event *, void *);
    void *user;
    /* Optional application drawing after the widgets, before menus and
     * dialogs (previews, swatches; Control Panel). */
    void (*paint)(struct du_ui *, void *);
    /* Runs after anchors and canvas replacement, preserving widget/model state. */
    void (*layout)(struct du_ui *, void *);
    /* Installed by the window owner before starting workers. Immutable until
     * those workers have joined; service runs only on the UI thread. */
    void (*wake)(void *);
    void *wake_ctx;
    void (*service)(struct du_ui *, void *);
    int wait_fds[4];
    unsigned wait_count;
};
static inline void du_wake(struct du_ui *u)
{ if (u && u->wake) u->wake(u->wake_ctx); }

/* Core */
void du_init(struct du_ui *, uint32_t *pixels, int width, int height);
void du_resize(struct du_ui *, uint32_t *pixels, int width, int height);
struct du_widget *du_add(struct du_ui *, enum du_kind, int id, struct du_rect, const char *text);
struct du_widget *du_get(struct du_ui *, int id);
int du_hit(const struct du_ui *, int x, int y);
unsigned du_cursor_at(const struct du_ui *);
void du_cursor(struct du_ui *, unsigned shape);
void du_focus(struct du_ui *, int id);
int du_focused(const struct du_ui *);
void du_set_text(struct du_widget *, const char *);
void du_render(struct du_ui *);
/* Union clipped repaint regions; a full dirty=1 invalidation takes precedence. */
void du_invalidate(struct du_ui *, struct du_rect);
void du_invalidate_widget(struct du_ui *, int id);
/* Retain unchanged pixels; repaint only exposed strips. Pending paint or
 * overlays cause a bounded repaint instead of copying stale pixels. */
void du_scroll(struct du_ui *, struct du_rect, int dx, int dy);
void du_input(struct du_ui *, const struct ciuki_input_event *);
void du_window_focus(struct du_ui *, int focused);
void du_release(struct du_ui *);
unsigned du_key_char(const struct du_ui *, unsigned code);
void du_post(struct du_ui *, enum du_event_type, int id, int index);

/* Lists and trees */
int du_list_hit(const struct du_widget *, int x, int y);
struct du_rect du_list_label(const struct du_widget *, unsigned index);
struct du_rect du_list_body(const struct du_widget *);
/* Full item cell clipped to the visible viewport; empty for an offscreen item. */
struct du_rect du_list_row(const struct du_widget *, unsigned index);
void du_list_ensure(struct du_widget *, unsigned index);
int du_list_page(const struct du_widget *);
int du_list_target(const struct du_widget *, unsigned code);
/* fx-19: item i intersects the rubber band (icon and label bounds), and the
 * band itself in client coordinates (zero size when none). */
int du_list_in_band(const struct du_widget *, unsigned index);
struct du_rect du_list_band(const struct du_widget *);
unsigned du_list_band_hits(const struct du_widget *, uint8_t *hits);  /* one byte per item; returns the count */
/* fx-19 drag auto-scroll: scroll one step towards (x, y) when it lies in the
 * edge zone of the list body; returns 1 when the view scrolled. */
int du_list_autoscroll(struct du_ui *, struct du_widget *, int x, int y);
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
/* OK=0, Cancel=1, Apply=2; accept may keep the sheet open after Apply/error.
 * Control slots 4..9 are caller-owned, boxes relative to the sheet body.
 * Page labels and callbacks remain valid until the sheet closes. */
void du_property_sheet(struct du_ui *, int id, const char *title, int width, int height,
                       const struct du_item *pages, unsigned count);
struct du_widget *du_property_control(struct du_ui *, unsigned slot, enum du_kind, int id,
                                     struct du_rect, const char *text);
void du_dialog_icon(struct du_ui *, const uint32_t *argb32);
void du_dialog_close(struct du_ui *);
int du_dialog_id(const struct du_ui *);
struct du_rect du_dialog_button(const struct du_ui *, unsigned index);

/* Timers (monotonic nanoseconds) and the shared text clipboard. */
/* DU_EV_TICK preserves dirty state; callbacks invalidate changed content. */
int du_timer_start(struct du_ui *, int id, uint64_t interval_ns, int repeat);
void du_timer_stop(struct du_ui *, int id);
void du_tick(struct du_ui *, uint64_t now);
uint64_t du_next_deadline(const struct du_ui *);
void du_clipboard_set(const char *);
const char *du_clipboard_get(void);
#define DU_CLIPBOARD_MAX (1024u * 1024u)
int du_clipboard_set_n(const char *, size_t); /* failure leaves the previous copy intact */
int du_clipboard_path(const char *); /* optional host/provider path, NULL restores the default */

/* Desktop window glue: handshake, surface, back buffer, damage, events. */
struct du_window {
    struct du_completion *completion;
    int channel, surface, configured, closing, closed, error;
    int close_reply_pending; /* disposition+1, retried if the client queue is full */
    int present_pending; /* optional completion-paced client freezes surface writes */
    int manual_damage; /* application combines ordinary damage with its frame request */
    /* Explicit presenters opt in to the last render regions. The normal
     * du_window_turn path always presents those regions. */
    int present_rendered;
    uint32_t server_pid, ping_serial;
    struct ciuki_surface_info info;
    uint32_t *pixels, *back;
    /* At most one replacement. After sending its attachment, retain the
     * previous surface until CONFIGURE(COMMITTED) confirms the swap. */
    int old_surface, replacement, swap_sent, resizable;
    unsigned cursor_sent;
    int accept_files;        /* fx-19: DragAcceptFiles state sent to the desktop */
    struct ciuki_surface_info old_info;
    uint32_t *old_pixels, *old_back;
    uint32_t configure_serial, request_serial, request_width, request_height, last_serial;
    struct ciuki_message tx[16]; unsigned head, count;
    struct du_rect damage; int damaged;
    struct du_rect damages[DU_REGIONS]; unsigned damage_count;
    struct du_ui *ui;
    int (*close_request)(struct du_window *, void *);
    void *user;
    /* f2-33: v2 messages the glue does not consume (WINDOW_INFO, TASK_RESULT). */
    void (*message)(struct du_window *, const struct desk_packet *, void *);
    /* G3 end of session (WM_QUERYENDSESSION, protocol.h QUERY_END_SESSION).
     * query_end_session, when set, answers the query: DU_SESSION_AGREE,
     * DU_SESSION_VETO, or DU_SESSION_ASKING after showing a prompt (then
     * du_window_session_reply). When NULL, the glue asks close_request: a
     * nonzero result agrees; zero means the program shows its save prompt,
     * and the program's later du_window_close() agrees (the window stays
     * open until END_SESSION) while du_window_close_reply(w, 0) vetoes.
     * END_SESSION(1) closes the window without asking again. */
    int (*query_end_session)(struct du_window *, uint32_t flags, void *);
    int session;                 /* DU_SESSION_* state, internal */
    uint32_t session_serial, session_flags;
    int session_reply_pending;   /* answer+1, retried if the queue is full */
};
enum { DU_SESSION_VETO = 0, DU_SESSION_AGREE = 1, DU_SESSION_ASKING = 2 };
int du_window_open(struct du_window *, struct du_ui *, int channel, int width, int height, const char *title);
int du_window_open_resizable(struct du_window *, struct du_ui *, int channel, int width, int height,
                             const char *title, int min_width, int min_height);
int du_window_turn(struct du_window *, uint64_t now);
void du_window_present(struct du_window *);
int du_window_ping(struct du_window *, uint32_t serial);
int du_window_queue(struct du_window *, struct ciuki_message *);  /* f2-33: any v2 request */
int du_window_move(struct du_window *, int x, int y);
int du_window_set_title(struct du_window *, const char *title);
void du_window_close(struct du_window *);
/* Reply to CLOSE_REQUEST: later=1 (the program asks the user; no deadline
 * while it answers PINGs), later=0 declines it (the user cancelled). A
 * close_request callback returning zero automatically queues later=1. While
 * an end-of-session query waits for the user, later=0 vetoes the session. */
void du_window_close_reply(struct du_window *, int later);
/* G3: the answer to a query that returned DU_SESSION_ASKING (1 agree, 0 veto). */
void du_window_session_reply(struct du_window *, int agree);
/* fx-19 DragAcceptFiles: on=1 makes the window a WM_DROPFILES-style drop
 * target; dropped files arrive as DU_EV_DROPFILES. du_drop_file copies the
 * index-th dropped path (POSIX) and returns the number of dropped items, or
 * -errno; out is empty past the last item. */
int du_window_accept_files(struct du_window *, int on);
int du_drop_file(unsigned index, char *out, size_t size);
int du_drop_entry(unsigned index, char *out, size_t size, int *folder);   /* also reports a folder item */
int du_drop_source(const char *path);   /* host tests: read dropped files from another data object */
void du_window_destroy(struct du_window *);
int du_run(struct du_window *, void (*idle)(void *), void *);
/* Absolute monotonic deadline; UINT64_MAX waits indefinitely. Includes
 * input, pending output, worker completions and application descriptors. */
int du_window_wait(struct du_window *, uint64_t deadline);
uint64_t du_now(void);
#endif
