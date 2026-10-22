/* SPDX-License-Identifier: MIT
 * Native CiukPaint. One editor per process/window, as in the legacy app.
 * Host harnesses attach the same implementation to a libdeskui surface. */
#ifndef CIUKI_PAINT_H
#define CIUKI_PAINT_H
#include "../libdeskui/deskui.h"
#include "io.h"
#define PAINT_W 640
#define PAINT_H 480
enum paint_tool { T_SELECT, T_ERASER, T_FILL, T_PICK, T_ZOOM, T_PENCIL, T_BRUSH, T_AIR,
                  T_TEXT, T_LINE, T_CURVE, T_RECT, T_POLY, T_ELLIPSE, T_RRECT, NTOOLS };
enum paint_command {
    M_NEW = 1, M_OPEN, M_SAVE, M_SAVEAS, M_EXIT, M_UNDO, M_REDO, M_CUT, M_COPY, M_PASTE,
    M_CLEARSEL, M_SELALL, M_TOOLBOX, M_COLORBOX, M_STATUS, M_ZOOMIN, M_ZOOMOUT, M_Z1, M_Z2,
    M_Z4, M_Z6, M_Z8, M_GRID, M_FONT, M_FLIP, M_INVERT, M_ATTR, M_CLEARIMG, M_OPAQUE,
    M_HELP, M_ABOUT
};
enum { P_CANVAS = 100, P_HSCROLL, P_VSCROLL, P_STATUS, P_TOOL = 200, P_SWATCH = 300, P_OPTIONS = 400 };
struct paint_state {
    int width, height, tool, zoom, scroll_x, scroll_y, dirty, can_undo, can_redo;
    int selection, floating, text, operation, selection_x, selection_y, selection_w, selection_h;
    int free_select;
    size_t text_cursor, text_anchor, text_bytes;
    uint8_t foreground, background;
    struct du_rect view, options, colors;
};
int paint_attach(struct du_ui *);
void paint_detach(void);
void paint_callbacks(void (*quit)(void *), void (*title)(void *, const char *), void *host);
void paint_command(int);
int paint_open_begin(const char *);
int paint_open_poll(void); /* 0 pending, 1 installed, negative error */
int paint_open(const char *);
int paint_save(const char *, enum paint_format);
int paint_request_close(void); /* 1: unsaved prompt pending; 0: safe to close */
void paint_snapshot(struct paint_state *);
uint8_t paint_pixel(int x, int y);
uint32_t paint_rgb(uint8_t index);
const char *paint_path(void);

/* Checked codecs, shared by the app and host round-trip tests. */
int paint_read_image(const char *, const uint8_t *palette, struct paint_image *);
int paint_write_image(const char *, enum paint_format, const struct paint_image *, const uint8_t *palette);
#endif
