/* Private classic-look boundary of USER: GDI entry points (handles on the
 * wire are DWORDs), font metrics for layout, frames, caret and the
 * dialog/menu/control hooks shared by the f5-10C modules.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef U32_CLASSIC_H
#define U32_CLASSIC_H
#include "internal.h"
#include <w32/controls.h>
extern HANDLE W32API GlobalAlloc(DWORD, DWORD);
extern void *W32API GlobalLock(HANDLE);
extern BOOL W32API GlobalUnlock(HANDLE);
typedef struct {
  LONG cx, cy;
} U32SIZE;
/* PE32 TEXTMETRICA and LOGFONTA layouts, independent of gdi32.h. */
struct u32_textmetric {
  LONG height, ascent, descent, internal, external, average, maximum, weight,
      overhang, aspect_x, aspect_y;
  uint8_t first, last, fallback, brk, italic, underlined, struck, pitch,
      charset;
};
struct u32_logfont {
  LONG height, width, escapement, orientation, weight;
  uint8_t italic, underline, strike, charset, out, clip, quality, pitch;
  char face[32];
};
struct u32_graphics {
  DWORD(W32API *brush)(DWORD);
  BOOL(W32API *delete_object)(DWORD);
  DWORD(W32API *select)(DWORD, DWORD);
  BOOL(W32API *patblt)(DWORD, int, int, int, int, DWORD);
  DWORD(W32API *text_color)(DWORD, DWORD);
  DWORD(W32API *bk_color)(DWORD, DWORD);
  int(W32API *bk_mode)(DWORD, int);
  int(W32API *draw_text)(DWORD, const void *, int, RECT *, unsigned, int,
                         void *);
  BOOL(W32API *text_out)(DWORD, int, int, const char *, int);
  DWORD(W32API *font_indirect)(const struct u32_logfont *);
  BOOL(W32API *metrics)(DWORD, struct u32_textmetric *);
  BOOL(W32API *extent)(DWORD, const char *, int, U32SIZE *);
  BOOL(W32API *char_widths)(DWORD, unsigned, unsigned, int *);
  DWORD(W32API *memory_dc)(DWORD);
  BOOL(W32API *delete_dc)(DWORD);
  DWORD(W32API *stock)(int);
  int(W32API *object)(DWORD, int, void *);
  BOOL(W32API *bitblt)(DWORD, int, int, int, int, DWORD, int, int, DWORD);
  DWORD(W32API *pen)(int, int, DWORD);
  BOOL(W32API *move_to)(DWORD, int, int, POINT *);
  BOOL(W32API *line_to)(DWORD, int, int);
  DWORD(W32API *create_bitmap)(int, int, UINT, UINT, const void *);
  DWORD(W32API *pattern_brush)(DWORD);
  DWORD(W32API *get_text_color)(DWORD);
  DWORD(W32API *get_bk_color)(DWORD);
  BOOL(W32API *fill_rect)(DWORD, const RECT *, DWORD);
  DWORD(W32API *dib_bitmap)(DWORD, const void *, DWORD, const void *,
                            const void *, UINT);
};
const struct u32_graphics *u32_graphics(void);
int u32_stricmp(const char *, const char *);
int64_t u32_div64(int64_t, int64_t);
void u32_strcpy(char *, UINT, const char *);
/* Font layout data; falls back to the Windows 98 dialog references
 * (8x16 system font, 6x13 MS Sans Serif 8 pt) when GDI has no faces. */
struct u32_font_info {
  HFONT font;
  int height, ascent, descent, average, external, base_x, base_y, measured;
  int widths[256];
};
const struct u32_font_info *u32_font_info(HFONT);
int u32_text_width(const struct u32_font_info *, const char *, int);
void u32_color_rect(HDC, RECT, DWORD);
void u32_label(HDC, RECT, const char *, HFONT, DWORD, UINT);
int u32_label_height(HDC, RECT, const char *, HFONT, UINT);
void u32_mnemonic_text(HDC, RECT, const char *, HFONT, DWORD, UINT, BOOL);
HBRUSH u32_sys_brush(int);
/* Surface DCs over a top-level surface or a child's window rectangle. */
HDC u32_band_dc(struct u32_window *, RECT, struct u32_window **);
void u32_band_release(struct u32_window *, HDC);
/* Non-client bands owned by the personality (menu bar, scroll bars). */
void u32_nonclient(struct u32_window *, int *top, int *right, int *bottom);
POINT u32_surface_origin(struct u32_window *);
void u32_surface_size(struct u32_window *, UINT *, UINT *);
void u32_paint_frame(HWND);
UINT u32_hit_test(HWND, POINT);
LRESULT u32_nc_mouse(HWND, UINT, WPARAM, LPARAM);
void u32_scroll_paint(struct u32_window *, HDC, int, RECT);
void u32_scroll_track(HWND, int, POINT);
LRESULT u32_scrollbar_message(HWND, UINT, WPARAM, LPARAM, BOOL *);
HBRUSH u32_dither_brush(void);
/* Modules. */
void u32_controls_register(void);
LRESULT W32API u32_control_proc(HWND, UINT, WPARAM, LPARAM);
LRESULT W32API u32_dialog_proc(HWND, UINT, WPARAM, LPARAM);
void u32_control_cleanup(HWND);
void u32_caret_paint(HWND, HDC);
void u32_caret_destroyed(HWND);
const void *u32_resource(HINSTANCE, const char *, unsigned, DWORD *);
int u32_dialog_long(HWND, int, LONG *, BOOL);
void u32_dialog_destroyed(HWND);
void u32_dialog_focus(HWND, HWND);
HWND u32_dialog_of(HWND);
int u32_menu_height(struct u32_window *);
void u32_menu_destroy_window(HWND);
void u32_menu_paint(struct u32_window *);
LRESULT u32_menu_command(HWND, WPARAM, LPARAM);
LRESULT W32API u32_menu_window_proc(HWND, UINT, WPARAM, LPARAM);
LRESULT u32_menu_key(HWND, UINT, WPARAM, LPARAM);
HWND u32_menu_owner_tracking(void);
UINT u32_mnemonic(const char *);
/* Parsed DIALOG/DIALOGEX template (all strings converted to CP1252). */
#define U32_TEMPLATE_ITEMS 120
struct u32_template_item {
  DWORD help, ex, style, id;
  int x, y, cx, cy;
  char klass[64];
  char text[256];
  UINT text_ordinal;
  const uint8_t *data;
  UINT data_bytes;
};
struct u32_template {
  BOOL extended, has_font, italic;
  DWORD help, ex, style;
  int x, y, cx, cy, point, weight;
  UINT count, menu_ordinal;
  uint8_t charset;
  char menu[64], klass[64], title[256], face[32];
  struct u32_template_item items[U32_TEMPLATE_ITEMS];
};
BOOL u32_template_parse(const void *, DWORD, struct u32_template *);
HWND u32_dialog_create(HINSTANCE, const struct u32_template *, HWND, DLGPROC,
                       LPARAM, BOOL);
intptr_t u32_dialog_run(HWND, HWND);
int u32_muldiv(int, int, int);
#define U32_DIALOG_EXTRA 30
#define U32_MENU_CLASS "#32768"
#define U32_DIALOG_CLASS "#32770"
#endif
