/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef U32_INTERNAL_H
#define U32_INTERNAL_H
#include "../../../apps/libdeskui/v3.h"
#include <w32/server.h>
#include <w32/user_surface.h>
#define U32_WINDOWS 128
#define U32_CLASSES 64
#define U32_THREADS 64
#define U32_TIMERS 128
#define U32_RECTS 64
#define U32_DESTROY 0x7fffffffu /* private, same-process sent request */
#define U32_PTR(x) ((DWORD)(uintptr_t)(x))
struct u32_region {
  RECT r[U32_RECTS];
  UINT n;
};
struct u32_class {
  ATOM atom;
  WNDCLASSEXA c;
  char name[256];
  uint8_t *extra;
  UINT refs;
  BOOL unicode;   /* f5-20B: class procedure registered with RegisterClass(Ex)W */
  WCHAR *wmenu;   /* f5-20B: lpszMenuName as given to RegisterClass(Ex)W */
  char *amenu;    /* f5-20B: its code-page copy, owned by the class */
};
struct u32_prop {
  struct u32_prop *next;
  char *name;
  HANDLE value;
  ATOM atom;
};
struct u32_window {
  HWND handle, parent, owner;
  DWORD tid, pid, style, exstyle, id;
  WNDPROC proc;
  HINSTANCE instance;
  intptr_t userdata;
  struct u32_class *cls;
  uint8_t *extra;
  char title[256];
  RECT rect;
  UINT z;
  uint32_t wire, generation, serial, ping;
  struct u32_surface surface;
  struct u32_region update, damage;
  struct u32_prop *props;
  BOOL destroying, creating, redraw, erase, internal_paint;
  UINT dcs;
  struct dd_lease lease;
  DWORD lease_w, lease_h, lease_bpp;
  BOOL lease_wanted;
  BOOL destroy_request; /* U32_DESTROY is honoured only when USER sent it */
  /* f5-10C classic state: control/dialog records, menus, scroll bars. */
  void *control, *dialog;
  HANDLE menu, system_menu;
  BOOL unicode; /* f5-20B: the window procedure takes Unicode messages */
  HANDLE rgn;   /* f5-20B: SetWindowRgn region, owned by USER (window coordinates) */
  struct u32_scroll_state {
    int minimum, maximum, position, track;
    UINT page, disabled;
  } scroll[3];
};
struct u32_send_frame {
  struct u32_send_frame *prev;
  DWORD token;
  void *data;
  DWORD bytes;
  BOOL replied;
};
struct u32_pending_event {
  struct u32_pending_event *next;
  struct desk_packet packet;
};
struct u32_thread {
  DWORD session_poll;
  BOOL session_active;
  DWORD id;
  HWND focus, active;
  MSG last;
  uint8_t keys[256];
  struct u32_send_frame *send;
  struct u32_pending_event *events, *events_tail;
  void *hook_frame;
  UINT event_count;
  MSG text[64];
  UINT text_count;
  HWND key_window, translate_window;
  DWORD key_time, translate_time, key_lparam;
};
struct u32_timer {
  DWORD tid;
  HWND hwnd;
  uintptr_t id;
  DWORD period, due;
  TIMERPROC proc;
  BOOL system; /* f5-20B: SetSystemTimer, delivered as WM_SYSTIMER */
};
struct u32_dc {
  HDC dc;
  HWND hwnd, root;
  BOOL paint;
  RECT painted;
};
struct u32_backend {
  int32_t (*call)(void *, DWORD);
  DWORD (*gate)(DWORD, DWORD, DWORD, DWORD);
  DWORD (*rpc)(const void *, DWORD, void *, DWORD, DWORD *);
  DWORD (*tid)(void), (*pid)(void), (*ticks)(void);
  BOOL (*surface)(UINT, UINT, struct u32_surface *);
  void (*drop_surface)(struct u32_surface *);
  void (*pause)(DWORD);
};
extern struct u32_backend u32_backend;
extern struct u32_window u32_windows[U32_WINDOWS];
extern struct u32_class u32_classes[U32_CLASSES];
extern struct u32_timer u32_timers[U32_TIMERS];
extern const struct u32_gdi *u32_gdi;
extern struct du3_connection u32_desktop;
extern HWND u32_capture, u32_foreground;
extern POINT u32_pointer;
extern UINT u32_screen_w, u32_screen_h, u32_layout;
void u32_lock(void);
void u32_unlock(int *);
UINT u32_suspend(void);
void u32_resume(UINT);
#define U32_GUARD                                                              \
  int u32_guard __attribute__((cleanup(u32_unlock))) = (u32_lock(), 0)
void u32_gdi_ensure(void);
void u32_set_backend(const struct u32_backend *);
void *u32_alloc(DWORD);
void u32_free(void *);
BOOL u32_fail(DWORD);
DWORD u32_tid(void);
DWORD u32_pid(void);
DWORD u32_ticks(void);
struct u32_thread *u32_thread(void);
struct u32_thread *u32_thread_for(DWORD);
struct u32_window *u32_window(HWND);
struct u32_window *u32_root(struct u32_window *);
struct u32_class *u32_class(const char *, HINSTANCE);
BOOL u32_directory(UINT, HWND, DWORD, DWORD, DWORD, const char *, const void *,
                   DWORD, struct w32r_reply *, void *, DWORD);
BOOL u32_directory_window(HWND, struct w32r_window *, char *);
BOOL u32_sync_window(struct u32_window *);
LRESULT u32_invoke(HWND, UINT, WPARAM, LPARAM);
LRESULT u32_hooks(int, int, WPARAM, LPARAM);
BOOL u32_queue(DWORD, HWND, UINT, WPARAM, LPARAM, UINT, DWORD, POINT);
BOOL u32_message(struct ciuki_w32_message_args *);
void u32_pump(void);
void u32_flush(void);
BOOL u32_connect(void);
BOOL u32_wire(struct u32_window *, UINT, const void *, UINT, int);
BOOL u32_create_wire(struct u32_window *);
BOOL u32_place(struct u32_window *);
BOOL u32_order_wire(struct u32_window *, HWND, HWND, UINT);
void u32_restack(struct u32_window *, HWND);
/* SetWindowPos hWndInsertAfter sentinels never name a window. */
#define U32_ORDER_SENTINEL(h) ((h) == HWND_TOP || (h) == HWND_BOTTOM || (h) == HWND_TOPMOST || (h) == HWND_NOTOPMOST)
enum { U32_TEARDOWN, U32_NORMAL, U32_VICTIM };
extern BOOL u32_exiting;
BOOL u32_destroy(struct u32_window *, UINT);
HANDLE u32_dib_bitmap(const void *, DWORD);
void u32_palette_deliver(void);
HWND u32_dc_window(HDC);
void u32_hooks_cleanup(DWORD);
void u32_desktop_cleanup(DWORD);
void u32_clipboard_cleanup(DWORD);
void u32_icons_cleanup(void);
void u32_display_event(struct u32_window *, const struct desk_packet *);
void u32_display_cache(void);
BOOL u32_resize(struct u32_window *);
void u32_clipboard_owned_clear(void);
void u32_event(const struct desk_packet *);
void u32_input(struct u32_window *, const struct ciuki_input_event *);
void u32_clear_input(void);
void u32_clear_thread_input(void);
UINT u32_scan_vk(UINT, UINT);
UINT u32_dik(UINT);
DWORD u32_key_lparam(UINT, BOOL, BOOL, BOOL);
void u32_key_state(uint8_t *, UINT, UINT, BOOL, BOOL);
void u32_input_notify(HWND, const struct ciuki_input_event *, UINT);
void u32_input_reset(void);
void u32_input_focus(void);
BOOL u32_input_pointer(HWND, BOOL);
extern HWND u32_input_exclusive;
void u32_settings(void);
extern UINT u32_pointer_changing;
void u32_activate(struct u32_window *, BOOL, BOOL);
POINT u32_origin(struct u32_window *);
void u32_frame(DWORD, DWORD, int *, int *);
UINT u32_style(struct u32_window *);
void u32_region_add(struct u32_region *, RECT);
void u32_region_sub(struct u32_region *, RECT);
void u32_damage(struct u32_window *, RECT);
void u32_mark(void);
BOOL u32_rect_intersect(RECT *, RECT, RECT);
BOOL u32_pointer_message(UINT);
int u32_ansi(unsigned);
unsigned u32_scalar(uint8_t);
/* f5-10C classic hooks (classic.c, scroll.c, dialog.c, menu.c). */
void u32_controls_register(void);
int u32_dialog_long(HWND, int, LONG *, BOOL);
void u32_classic_destroy(HWND);
LRESULT u32_classic_default(HWND, UINT, WPARAM, LPARAM, BOOL *);
void u32_nonclient(struct u32_window *, int *, int *, int *);
POINT u32_surface_origin(struct u32_window *);
void u32_surface_size(struct u32_window *, UINT *, UINT *);
void u32_paint_frame(HWND);
void u32_caret_paint(HWND, HDC);
UINT u32_hit_test(HWND, POINT);
void u32_invalidate_children(struct u32_window *, RECT, BOOL);
HANDLE W32API LoadMenuA(HINSTANCE, const char *);
/* f5-20B Unicode layer (unicode.c), Microsoft Layer for Unicode model: the
 * ANSI core stays authoritative and Unicode callers are converted through
 * the active code page (1252). u32_convert translates one message between
 * its ANSI and Unicode forms around `call`; to_wide means the message is in
 * ANSI form and `call` expects Unicode. */
typedef LRESULT (*u32_caller)(void *, HWND, UINT, WPARAM, LPARAM);
LRESULT u32_convert(BOOL to_wide, u32_caller call, void *context, HWND h,
                    UINT m, WPARAM wp, LPARAM lp);
LRESULT u32_call_proc(WNDPROC proc, BOOL proc_wide, BOOL msg_wide, HWND h,
                      UINT m, WPARAM wp, LPARAM lp);
LRESULT u32_invoke_w(HWND, UINT, WPARAM, LPARAM);
uintptr_t u32_proc_handle(WNDPROC proc, BOOL proc_wide, BOOL caller_wide);
WNDPROC u32_proc_decode(uintptr_t value, BOOL caller_wide, BOOL *wide);
UINT u32_wlen(const WCHAR *);
char *u32_narrow(const WCHAR *);         /* NULL and atoms pass through */
WCHAR *u32_widen(const char *);          /* NULL and atoms pass through */
void u32_text_free(const void *);        /* frees u32_narrow/u32_widen results */
UINT u32_to_ansi(char *out, UINT cap, const WCHAR *in, int n);
UINT u32_to_wide(WCHAR *out, UINT cap, const char *in, int n);
#define U32_ATOM(p) ((uintptr_t)(p) < 0x10000u)
/* f5-20B: GetDesktopWindow. The desktop is drawn by the native desktop, so
 * this handle names the screen: its rectangle is the screen, its DC is a
 * measurement DC and windows created with it as parent are top-level. */
#define U32_DESKTOP ((HWND)(uintptr_t)0x00010010u)
void u32_window_region_free(struct u32_window *);
BOOL W32API IsMenu(HANDLE);
DWORD u32_progman_rpc(UINT,DWORD,const char *,const char *,struct w32r_reply *,void *,DWORD);
void u32_progman_ansi(char *);
BOOL u32_progman_message(HWND,UINT,WPARAM,LPARAM,LRESULT *);
void u32_progman_cleanup(DWORD);
void u32_session_pump(void);
UINT u32_utf8(char *, UINT, const char *);
#endif
