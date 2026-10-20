/* f5-20B: the Unicode USER entry points the reused Wine 10.0 DLLs call, in
 * the model of the Microsoft Layer for Unicode on Windows 95/98/ME: the ANSI
 * window manager stays authoritative; window procedures, classes and dialog
 * procedures remember whether they take ANSI or Unicode messages, and every
 * message crossing the boundary is translated through the active code page
 * (1252) by u32_convert. A window is Unicode when its procedure is
 * (IsWindowUnicode); a procedure of the other kind is handed out as a
 * 0xFFFFxxxx handle that CallWindowProc recognises, as Windows does.
 * References: Microsoft "Using the Microsoft Layer for Unicode on Windows
 * Me/98/95", "CallWindowProcW", "IsWindowUnicode", "GetWindowLongW" and the
 * per-message pages for the translated messages.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "unicode.h"

/* ---- code page 1252 text ---------------------------------------------- */
UINT u32_wlen(const WCHAR *s) {
  UINT n = 0;
  if (s)
    while (s[n])
      n++;
  return n;
}
UINT u32_to_ansi(char *out, UINT cap, const WCHAR *in, int n) {
  UINT k = 0;
  if (!out || !cap)
    return 0;
  for (; in && (n < 0 ? in[k] != 0 : k < (UINT)n) && k + 1 < cap; k++)
    out[k] = (char)u32_ansi(in[k]);
  out[k] = 0;
  return k;
}
UINT u32_to_wide(WCHAR *out, UINT cap, const char *in, int n) {
  UINT k = 0;
  if (!out || !cap)
    return 0;
  for (; in && (n < 0 ? in[k] != 0 : k < (UINT)n) && k + 1 < cap; k++)
    out[k] = (WCHAR)u32_scalar((uint8_t)in[k]);
  out[k] = 0;
  return k;
}
char *u32_narrow(const WCHAR *w) {
  if (U32_ATOM(w))
    return (char *)(uintptr_t)w;
  UINT n = u32_wlen(w);
  char *p = u32_alloc(n + 1);
  if (p)
    u32_to_ansi(p, n + 1, w, (int)n);
  return p;
}
WCHAR *u32_widen(const char *a) {
  if (U32_ATOM(a))
    return (WCHAR *)(uintptr_t)a;
  UINT n = w32_strlen(a);
  WCHAR *p = u32_alloc((n + 1) * 2);
  if (p)
    u32_to_wide(p, n + 1, a, (int)n);
  return p;
}
void u32_text_free(const void *p) {
  if (!U32_ATOM(p))
    u32_free((void *)(uintptr_t)p);
}
static UINT char_to_wide(UINT c) { return c < 256 ? u32_scalar((uint8_t)c) : c; }
static UINT char_to_ansi(UINT c) { return (UINT)(uint8_t)u32_ansi(c & 0xffff); }

/* ---- procedure handles -------------------------------------------------- */
#define PROCS 256u
static struct {
  WNDPROC proc;
  BOOL wide;
} procs[PROCS];
uintptr_t u32_proc_handle(WNDPROC proc, BOOL proc_wide, BOOL caller_wide) {
  if (!proc || !proc_wide == !caller_wide)
    return (uintptr_t)proc;
  UINT free_slot = PROCS;
  for (UINT i = 0; i < PROCS; i++) {
    if (procs[i].proc == proc && !procs[i].wide == !proc_wide)
      return 0xffff0000u | (i + 1);
    if (!procs[i].proc && free_slot == PROCS)
      free_slot = i;
  }
  if (free_slot == PROCS)
    return (uintptr_t)proc; /* table full: the raw pointer, unconverted */
  procs[free_slot].proc = proc;
  procs[free_slot].wide = !!proc_wide;
  return 0xffff0000u | (free_slot + 1);
}
WNDPROC u32_proc_decode(uintptr_t v, BOOL caller_wide, BOOL *wide) {
  /* Handles travel through LONG (GetWindowLong): compare the low 32 bits. */
  UINT i = (UINT)(v & 0xffffu);
  if (((DWORD)v >> 16) == 0xffffu && i && i <= PROCS && procs[i - 1].proc) {
    *wide = procs[i - 1].wide;
    return procs[i - 1].proc;
  }
  *wide = !!caller_wide;
  return (WNDPROC)v;
}

/* ---- message translation ---------------------------------------------- */
enum { PLAIN, IN_TEXT, OUT_TEXT, CHARACTER, CHARACTER_LOW, CREATE, LINE,
       LIST_TEXT, DIALOG_CODE, RESULT_CHARACTER, ASK_FORMAT };
static BOOL list_strings(HWND h, UINT m) {
  struct u32_window *w = u32_window(h);
  if (!w)
    return 1;
  DWORD s = w->style;
  if (m >= LB_ADDSTRING && m <= 0x1afu)
    return !(s & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE)) ||
           (s & LBS_HASSTRINGS);
  return !(s & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE)) || (s & 0x200u);
}
static int kind(HWND h, UINT m) {
  switch (m) {
  case WM_NCCREATE:
  case WM_CREATE:
    return CREATE;
  case WM_SETTEXT:
  case 0x1au: /* WM_SETTINGCHANGE / WM_WININICHANGE */
  case 0x1bu: /* WM_DEVMODECHANGE */
  case EM_REPLACESEL:
  case LB_DIR:
  case 0x196u: /* LB_ADDFILE */
  case CB_DIR:
    return IN_TEXT;
  case WM_GETTEXT:
    return OUT_TEXT;
  case 0x30cu: /* WM_ASKCBFORMATNAME */
    return ASK_FORMAT;
  case WM_CHAR:
  case 0x103u: /* WM_DEADCHAR */
  case WM_SYSCHAR:
  case 0x107u: /* WM_SYSDEADCHAR */
  case 0x286u: /* WM_IME_CHAR */
  case EM_SETPASSWORDCHAR:
    return CHARACTER;
  case 0x120u: /* WM_MENUCHAR */
  case 0x2fu:  /* WM_CHARTOITEM */
    return CHARACTER_LOW;
  case EM_GETLINE:
    return LINE;
  case EM_GETPASSWORDCHAR:
    return RESULT_CHARACTER;
  case WM_GETDLGCODE:
    return DIALOG_CODE;
  case LB_ADDSTRING:
  case LB_INSERTSTRING:
  case LB_FINDSTRING:
  case LB_FINDSTRINGEXACT:
  case LB_SELECTSTRING:
  case 0x143u: /* CB_ADDSTRING */
  case 0x14au: /* CB_INSERTSTRING */
  case 0x14cu: /* CB_FINDSTRING */
  case 0x14du: /* CB_SELECTSTRING */
  case 0x158u: /* CB_FINDSTRINGEXACT */
    return list_strings(h, m) ? IN_TEXT : PLAIN;
  case LB_GETTEXT:
  case 0x148u: /* CB_GETLBTEXT */
    return list_strings(h, m) ? LIST_TEXT : PLAIN;
  default:
    return PLAIN;
  }
}
static BOOL is_char_message(UINT m) {
  return m == WM_CHAR || m == 0x103u || m == WM_SYSCHAR || m == 0x107u ||
         m == 0x286u;
}
static void *text_copy(BOOL to_wide, LPARAM lp) {
  return to_wide ? (void *)u32_widen((const char *)lp)
                 : (void *)u32_narrow((const WCHAR *)lp);
}
LRESULT u32_convert(BOOL to_wide, u32_caller call, void *context, HWND h,
                    UINT m, WPARAM wp, LPARAM lp) {
  switch (kind(h, m)) {
  case IN_TEXT: {
    if (!lp)
      break;
    void *text = text_copy(to_wide, lp);
    if (!text)
      return m == WM_SETTEXT ? 0 : -2;
    LRESULT r = call(context, h, m, wp, (LPARAM)text);
    u32_text_free(text);
    return r;
  }
  case OUT_TEXT:
  case ASK_FORMAT: {
    if (!lp || !wp)
      break;
    UINT cap = wp > 65536 ? 65536 : (UINT)wp;
    void *buffer = u32_alloc(cap * (to_wide ? 2 : 1));
    if (!buffer)
      return 0;
    LRESULT r = call(context, h, m, cap, (LPARAM)buffer);
    UINT n = to_wide ? u32_to_ansi((char *)lp, cap, buffer, -1)
                     : u32_to_wide((WCHAR *)lp, cap, buffer, -1);
    u32_free(buffer);
    return m == WM_GETTEXT ? (LRESULT)n : r;
  }
  case CHARACTER:
    wp = to_wide ? char_to_wide((UINT)wp) : char_to_ansi((UINT)wp);
    break;
  case CHARACTER_LOW: {
    UINT low = (UINT)(wp & 0xffffu);
    low = to_wide ? char_to_wide(low) : char_to_ansi(low);
    wp = (wp & ~(WPARAM)0xffffu) | low;
    break;
  }
  case RESULT_CHARACTER: {
    LRESULT r = call(context, h, m, wp, lp);
    return to_wide ? (LRESULT)char_to_ansi((UINT)r)
                   : (LRESULT)char_to_wide((UINT)r & 0xff);
  }
  case CREATE: {
    if (!lp)
      break;
    /* CREATESTRUCTA and CREATESTRUCTW share one layout; only the two
     * string pointers change form (atoms and NULL pass through). */
    CREATESTRUCTA copy = *(const CREATESTRUCTA *)lp;
    void *name = copy.lpszName ? text_copy(to_wide, (LPARAM)copy.lpszName) : 0;
    void *klass = text_copy(to_wide, (LPARAM)copy.lpszClass);
    if ((copy.lpszName && !name) || (copy.lpszClass && !klass)) {
      u32_text_free(name);
      u32_text_free(klass);
      return m == WM_CREATE ? -1 : 0;
    }
    copy.lpszName = name;
    copy.lpszClass = klass;
    LRESULT r = call(context, h, m, wp, (LPARAM)&copy);
    u32_text_free(name);
    u32_text_free(klass);
    return r;
  }
  case LINE: {
    if (!lp)
      break;
    UINT cap = *(const uint16_t *)lp;
    if (!cap)
      break;
    uint8_t *buffer = u32_alloc((cap + 1) * 2);
    if (!buffer)
      return 0;
    *(uint16_t *)buffer = (uint16_t)cap;
    LRESULT r = call(context, h, m, wp, (LPARAM)buffer);
    UINT n = r < 0 ? 0 : (UINT)r > cap ? cap : (UINT)r;
    for (UINT i = 0; i < n; i++) {
      if (to_wide)
        ((char *)lp)[i] = (char)char_to_ansi(((WCHAR *)buffer)[i]);
      else
        ((WCHAR *)lp)[i] = (WCHAR)char_to_wide((uint8_t)buffer[i]);
    }
    u32_free(buffer);
    return (LRESULT)n;
  }
  case LIST_TEXT: {
    if (!lp)
      break;
    LRESULT length =
        call(context, h, m == LB_GETTEXT ? LB_GETTEXTLEN : 0x149u, wp, 0);
    if (length < 0)
      return length;
    UINT cap = (UINT)length + 1;
    void *buffer = u32_alloc(cap * 2);
    if (!buffer)
      return -2;
    LRESULT r = call(context, h, m, wp, (LPARAM)buffer);
    if (r >= 0)
      r = to_wide ? (LRESULT)u32_to_ansi((char *)lp, cap, buffer, -1)
                  : (LRESULT)u32_to_wide((WCHAR *)lp, cap, buffer, -1);
    u32_free(buffer);
    return r;
  }
  case DIALOG_CODE: {
    if (!lp)
      break;
    MSG copy = *(const MSG *)lp;
    if (is_char_message(copy.message))
      copy.wParam = to_wide ? char_to_wide((UINT)copy.wParam)
                            : char_to_ansi((UINT)copy.wParam);
    return call(context, h, m, wp, (LPARAM)&copy);
  }
  default:
    break;
  }
  return call(context, h, m, wp, lp);
}
struct proc_context {
  WNDPROC proc;
};
static LRESULT invoke_proc(void *context, HWND h, UINT m, WPARAM wp,
                           LPARAM lp) {
  WNDPROC proc = ((struct proc_context *)context)->proc;
  UINT saved = u32_suspend();
  LRESULT r = proc(h, m, wp, lp);
  u32_resume(saved);
  return r;
}
LRESULT u32_call_proc(WNDPROC proc, BOOL proc_wide, BOOL msg_wide, HWND h,
                      UINT m, WPARAM wp, LPARAM lp) {
  if (!proc)
    return msg_wide ? DefWindowProcW(h, m, wp, lp) : DefWindowProcA(h, m, wp, lp);
  struct proc_context c = {proc};
  if (!proc_wide == !msg_wide)
    return invoke_proc(&c, h, m, wp, lp);
  return u32_convert(!!proc_wide, invoke_proc, &c, h, m, wp, lp);
}
LRESULT u32_invoke_w(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  struct u32_window *w = u32_window(h);
  if (!w)
    return 0;
  if (m == U32_DESTROY)
    return u32_invoke(h, m, wp, lp);
  struct {
    LPARAM lParam;
    WPARAM wParam;
    UINT message;
    HWND hwnd;
  } call = {lp, wp, m, h};
  u32_hooks(4, 0, 0, (LPARAM)&call);
  return u32_call_proc(w->proc, w->unicode, 1, h, m, wp, lp);
}

/* ---- procedures ---------------------------------------------------------- */
static LRESULT call_default(void *unused, HWND h, UINT m, WPARAM wp,
                            LPARAM lp) {
  (void)unused;
  return DefWindowProcA(h, m, wp, lp);
}
LRESULT W32API DefWindowProcW(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  U32_GUARD;
  return u32_convert(0, call_default, 0, h, m, wp, lp);
}
LRESULT W32API CallWindowProcW(WNDPROC proc, HWND h, UINT m, WPARAM wp,
                               LPARAM lp) {
  U32_GUARD;
  BOOL wide;
  WNDPROC p = u32_proc_decode((uintptr_t)proc, 1, &wide);
  return u32_call_proc(p, wide, 1, h, m, wp, lp);
}
BOOL W32API IsWindowUnicode(HWND h) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  return w && w->unicode;
}

/* ---- classes -------------------------------------------------------------- */
static WCHAR *wide_copy(const WCHAR *s) {
  if (U32_ATOM(s))
    return (WCHAR *)(uintptr_t)s;
  UINT n = u32_wlen(s);
  WCHAR *p = u32_alloc((n + 1) * 2);
  if (p)
    w32_copy(p, s, (n + 1) * 2);
  return p;
}
ATOM W32API RegisterClassExW(const WNDCLASSEXW *c) {
  U32_GUARD;
  if (!c || c->cbSize != sizeof(*c) || !c->lpszClassName ||
      U32_ATOM(c->lpszClassName))
    return u32_fail(87);
  char *name = u32_narrow(c->lpszClassName);
  char *menu = c->lpszMenuName ? u32_narrow(c->lpszMenuName) : 0;
  WCHAR *wmenu = c->lpszMenuName ? wide_copy(c->lpszMenuName) : 0;
  if (!name || (c->lpszMenuName && (!menu || !wmenu))) {
    u32_text_free(name);
    u32_text_free(menu);
    u32_text_free(wmenu);
    return u32_fail(8);
  }
  WNDCLASSEXA a = {sizeof(a),      c->style,         c->lpfnWndProc,
                   c->cbClsExtra,  c->cbWndExtra,    c->hInstance,
                   c->hIcon,       c->hCursor,       c->hbrBackground,
                   menu,           name,             c->hIconSm};
  ATOM atom = RegisterClassExA(&a);
  u32_text_free(name);
  struct u32_class *k = atom ? u32_class((const char *)(uintptr_t)atom, 0) : 0;
  if (!k) {
    u32_text_free(menu);
    u32_text_free(wmenu);
    return 0;
  }
  k->unicode = 1;
  k->wmenu = U32_ATOM(wmenu) ? 0 : wmenu;
  k->amenu = U32_ATOM(menu) ? 0 : menu;
  return atom;
}
ATOM W32API RegisterClassW(const WNDCLASSW *c) {
  U32_GUARD;
  if (!c)
    return u32_fail(87);
  WNDCLASSEXW x = {sizeof(x),       c->style,         c->lpfnWndProc,
                   c->cbClsExtra,   c->cbWndExtra,    c->hInstance,
                   c->hIcon,        c->hCursor,       c->hbrBackground,
                   c->lpszMenuName, c->lpszClassName, 0};
  return RegisterClassExW(&x);
}
BOOL W32API UnregisterClassW(const WCHAR *n, HINSTANCE i) {
  U32_GUARD;
  char *name = u32_narrow(n);
  if (n && !name)
    return u32_fail(8);
  BOOL ok = UnregisterClassA(name, i);
  u32_text_free(name);
  return ok;
}
static struct u32_class *class_named(const WCHAR *n, HINSTANCE i) {
  char *name = u32_narrow(n);
  if (n && !name)
    return 0;
  struct u32_class *c = u32_class(name, i);
  if (!c) {
    u32_controls_register();
    c = u32_class(name, i);
  }
  u32_text_free(name);
  return c;
}
static const WCHAR *class_menu(struct u32_class *c) {
  const char *menu = c->c.lpszMenuName;
  if (U32_ATOM(menu) || c->wmenu)
    return U32_ATOM(menu) ? (const WCHAR *)menu : c->wmenu;
  c->wmenu = u32_widen(menu);
  return c->wmenu;
}
BOOL W32API GetClassInfoExW(HINSTANCE i, const WCHAR *n, WNDCLASSEXW *out) {
  U32_GUARD;
  struct u32_class *c = class_named(n, i);
  if (!c || !out || out->cbSize != sizeof(*out))
    return u32_fail(c ? 87 : 1411);
  WNDCLASSEXW r = {sizeof(r),
                   c->c.style,
                   (WNDPROC)u32_proc_handle(c->c.lpfnWndProc, c->unicode, 1),
                   c->c.cbClsExtra,
                   c->c.cbWndExtra,
                   c->c.hInstance,
                   c->c.hIcon,
                   c->c.hCursor,
                   c->c.hbrBackground,
                   class_menu(c),
                   n,
                   c->c.hIconSm};
  *out = r;
  return c->atom;
}
BOOL W32API GetClassInfoW(HINSTANCE i, const WCHAR *n, WNDCLASSW *out) {
  U32_GUARD;
  if (!out)
    return u32_fail(87);
  WNDCLASSEXW x = {.cbSize = sizeof(x)};
  if (!GetClassInfoExW(i, n, &x))
    return 0;
  *out = (WNDCLASSW){x.style,        x.lpfnWndProc, x.cbClsExtra,
                     x.cbWndExtra,   x.hInstance,   x.hIcon,
                     x.hCursor,      x.hbrBackground, x.lpszMenuName,
                     x.lpszClassName};
  return 1;
}
int W32API GetClassNameW(HWND h, WCHAR *out, int n) {
  U32_GUARD;
  if (!out || n <= 0)
    return 0;
  char name[256];
  int k = GetClassNameA(h, name, sizeof(name));
  if (!k && !name[0])
    return 0;
  return (int)u32_to_wide(out, (UINT)n, name, k);
}
uintptr_t W32API GetClassLongW(HWND h, int index) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (w && index == -24)
    return u32_proc_handle(w->cls->c.lpfnWndProc, w->cls->unicode, 1);
  if (w && index == -8) /* GCL_MENUNAME */
    return (uintptr_t)class_menu(w->cls);
  return GetClassLongA(h, index);
}
uintptr_t W32API SetClassLongW(HWND h, int index, intptr_t value) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (!w || index != -24)
    return SetClassLongA(h, index, value);
  uintptr_t old = GetClassLongW(h, index);
  BOOL wide;
  w->cls->c.lpfnWndProc = u32_proc_decode((uintptr_t)value, 1, &wide);
  w->cls->unicode = wide;
  return old;
}

/* ---- windows --------------------------------------------------------------- */
HWND W32API CreateWindowExW(DWORD ex, const WCHAR *klass, const WCHAR *title,
                            DWORD style, int x, int y, int width, int height,
                            HWND parent, HANDLE menu, HINSTANCE instance,
                            void *param) {
  U32_GUARD;
  char *k = u32_narrow(klass);
  char *t = title ? u32_narrow(title) : 0;
  HWND h = 0;
  if ((klass && !k) || (title && !t))
    u32_fail(8);
  else
    h = CreateWindowExA(ex, k, t, style, x, y, width, height, parent, menu,
                        instance, param);
  u32_text_free(k);
  u32_text_free(t);
  return h;
}
LONG W32API GetWindowLongW(HWND h, int index) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (w && index == GWL_WNDPROC)
    return (LONG)u32_proc_handle(w->proc, w->unicode, 1);
  LONG value = 0;
  if (w && index == DWL_DLGPROC && u32_dialog_long_ex(h, index, &value, 0, 1))
    return value;
  return GetWindowLongA(h, index);
}
LONG W32API SetWindowLongW(HWND h, int index, LONG value) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (w && index == GWL_WNDPROC) {
    LONG old = GetWindowLongW(h, index);
    BOOL wide;
    w->proc = u32_proc_decode((uintptr_t)(DWORD)value, 1, &wide);
    w->unicode = wide;
    return old;
  }
  LONG v = value;
  if (w && index == DWL_DLGPROC && u32_dialog_long_ex(h, index, &v, 1, 1))
    return v;
  return SetWindowLongA(h, index, value);
}
int W32API GetWindowTextW(HWND h, WCHAR *out, int n) {
  U32_GUARD;
  if (!out || n <= 0)
    return 0;
  out[0] = 0;
  if (u32_window(h))
    return (int)SendMessageW(h, WM_GETTEXT, (WPARAM)n, (LPARAM)out);
  char text[256];
  int k = GetWindowTextA(h, text, sizeof(text));
  return (int)u32_to_wide(out, (UINT)n, text, k);
}
int W32API GetWindowTextLengthW(HWND h) {
  U32_GUARD;
  if (u32_window(h))
    return (int)SendMessageW(h, WM_GETTEXTLENGTH, 0, 0);
  return GetWindowTextLengthA(h);
}
int W32API InternalGetWindowText(HWND h, WCHAR *out, int n) {
  U32_GUARD;
  if (!out || n <= 0)
    return 0;
  struct u32_window *w = u32_window(h);
  if (!w) {
    char text[256];
    out[0] = 0;
    if (!IsWindow(h))
      return u32_fail(1400);
    int k = GetWindowTextA(h, text, sizeof(text));
    return (int)u32_to_wide(out, (UINT)n, text, k);
  }
  return (int)u32_to_wide(out, (UINT)n, w->title, -1);
}
BOOL W32API SetWindowTextW(HWND h, const WCHAR *text) {
  U32_GUARD;
  static const WCHAR empty[1] = {0};
  return !!SendMessageW(h, WM_SETTEXT, 0, (LPARAM)(text ? text : empty));
}
HWND W32API FindWindowExW(HWND parent, HWND after, const WCHAR *klass,
                          const WCHAR *title) {
  U32_GUARD;
  char *k = u32_narrow(klass), *t = title ? u32_narrow(title) : 0;
  HWND h = 0;
  if ((klass && !k) || (title && !t))
    u32_fail(8);
  else
    h = FindWindowExA(parent, after, k, t);
  u32_text_free(k);
  u32_text_free(t);
  return h;
}
HWND W32API FindWindowW(const WCHAR *klass, const WCHAR *title) {
  return FindWindowExW(0, 0, klass, title);
}
BOOL W32API SetPropW(HWND h, const WCHAR *name, HANDLE value) {
  U32_GUARD;
  char *n = u32_narrow(name);
  if (name && !n)
    return 0;
  BOOL ok = SetPropA(h, n, value);
  u32_text_free(n);
  return ok;
}
HANDLE W32API GetPropW(HWND h, const WCHAR *name) {
  U32_GUARD;
  char *n = u32_narrow(name);
  if (name && !n)
    return 0;
  HANDLE v = GetPropA(h, n);
  u32_text_free(n);
  return v;
}
HANDLE W32API RemovePropW(HWND h, const WCHAR *name) {
  U32_GUARD;
  char *n = u32_narrow(name);
  if (name && !n)
    return 0;
  HANDLE v = RemovePropA(h, n);
  u32_text_free(n);
  return v;
}
UINT W32API RegisterWindowMessageW(const WCHAR *name) {
  U32_GUARD;
  if (!name || U32_ATOM(name) || u32_wlen(name) > 255)
    return u32_fail(87);
  char n[256];
  u32_to_ansi(n, sizeof(n), name, -1);
  return RegisterWindowMessageA(n);
}

/* ---- messages --------------------------------------------------------------- */
static LRESULT send_a(void *unused, HWND h, UINT m, WPARAM wp, LPARAM lp) {
  (void)unused;
  return SendMessageA(h, m, wp, lp);
}
static BOOL local(HWND h) {
  struct u32_window *w = u32_window(h);
  return w && w->tid == u32_tid();
}
LRESULT W32API SendMessageW(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  U32_GUARD;
  if (h == (HWND)(uintptr_t)0xffff) {
    for (UINT i = 0; i < U32_WINDOWS; i++)
      if (u32_windows[i].handle && !u32_windows[i].parent)
        SendMessageW(u32_windows[i].handle, m, wp, lp);
    return 1;
  }
  if (local(h))
    return u32_invoke_w(h, m, wp, lp);
  return u32_convert(0, send_a, 0, h, m, wp, lp);
}
struct timeout_context {
  UINT flags, ms;
  uintptr_t result;
  LRESULT ok;
};
static LRESULT send_timeout_a(void *context, HWND h, UINT m, WPARAM wp,
                              LPARAM lp) {
  struct timeout_context *c = context;
  c->result = 0;
  c->ok = SendMessageTimeoutA(h, m, wp, lp, c->flags, c->ms, &c->result);
  return (LRESULT)c->result;
}
LRESULT W32API SendMessageTimeoutW(HWND h, UINT m, WPARAM wp, LPARAM lp,
                                   UINT flags, UINT ms, uintptr_t *result) {
  U32_GUARD;
  if (flags & ~0x2bu)
    return u32_fail(87);
  if (local(h)) {
    LRESULT r = u32_invoke_w(h, m, wp, lp);
    if (result)
      *result = (uintptr_t)r;
    return 1;
  }
  struct timeout_context c = {flags, ms, 0, 0};
  LRESULT r = u32_convert(0, send_timeout_a, &c, h, m, wp, lp);
  if (c.ok && result)
    *result = (uintptr_t)r;
  return c.ok;
}
static LRESULT notify_a(void *unused, HWND h, UINT m, WPARAM wp, LPARAM lp) {
  (void)unused;
  return SendNotifyMessageA(h, m, wp, lp);
}
BOOL W32API SendNotifyMessageW(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  U32_GUARD;
  if (local(h)) {
    u32_invoke_w(h, m, wp, lp);
    return 1;
  }
  return !!u32_convert(0, notify_a, 0, h, m, wp, lp);
}
/* The queues hold the ANSI form; character messages are translated when
 * posted and when retrieved. */
static WPARAM posted(UINT m, WPARAM wp, BOOL to_wide) {
  if (is_char_message(m) || m == EM_SETPASSWORDCHAR)
    return to_wide ? char_to_wide((UINT)wp) : char_to_ansi((UINT)wp);
  if (m == 0x120u || m == 0x2fu) {
    UINT low = (UINT)(wp & 0xffffu);
    return (wp & ~(WPARAM)0xffffu) |
           (to_wide ? char_to_wide(low) : char_to_ansi(low));
  }
  return wp;
}
BOOL W32API PostMessageW(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  U32_GUARD;
  return PostMessageA(h, m, posted(m, wp, 0), lp);
}
BOOL W32API PostThreadMessageW(DWORD tid, UINT m, WPARAM wp, LPARAM lp) {
  U32_GUARD;
  return PostThreadMessageA(tid, m, posted(m, wp, 0), lp);
}
BOOL W32API PeekMessageW(MSG *out, HWND h, UINT lo, UINT hi, UINT flags) {
  U32_GUARD;
  BOOL r = PeekMessageA(out, h, lo, hi, flags);
  if (r)
    out->wParam = posted(out->message, out->wParam, 1);
  return r;
}
BOOL W32API GetMessageW(MSG *out, HWND h, UINT lo, UINT hi) {
  U32_GUARD;
  BOOL r = GetMessageA(out, h, lo, hi);
  if (r && r != (BOOL)-1)
    out->wParam = posted(out->message, out->wParam, 1);
  return r;
}
LRESULT W32API DispatchMessageW(const MSG *m) {
  U32_GUARD;
  if (!m)
    return 0;
  if (((m->message == WM_TIMER || m->message == 0x118u) && m->lParam) || !m->hwnd ||
      m->message == WM_QUIT)
    return DispatchMessageA(m);
  if (!local(m->hwnd)) {
    MSG copy = *m;
    copy.wParam = posted(m->message, m->wParam, 0);
    return DispatchMessageA(&copy);
  }
  LRESULT r = u32_invoke_w(m->hwnd, m->message, m->wParam, m->lParam);
  u32_flush();
  return r;
}
BOOL W32API CallMsgFilterW(MSG *msg, int code) {
  U32_GUARD;
  if (!msg)
    return 0;
  MSG copy = *msg;
  copy.wParam = posted(msg->message, msg->wParam, 0);
  return CallMsgFilterA(&copy, code);
}
HANDLE W32API SetWindowsHookExW(int kind, HOOKPROC proc, HINSTANCE module,
                                DWORD tid) {
  U32_GUARD;
  HANDLE h = SetWindowsHookExA(kind, proc, module, tid);
  if (h)
    u32_hook_unicode(h);
  return h;
}

/* ---- SystemParametersInfoW ---------------------------------------------- */
/* LOGFONTA (60 bytes) and LOGFONTW (92) share the first 28 bytes. */
static void font_to_wide(uint8_t *w, const uint8_t *a) {
  w32_copy(w, a, 28);
  u32_to_wide((WCHAR *)(w + 28), 32, (const char *)(a + 28), 31);
}
static void font_to_ansi(uint8_t *a, const uint8_t *w) {
  w32_copy(a, w, 28);
  u32_to_ansi((char *)(a + 28), 32, (const WCHAR *)(w + 28), 31);
}
/* NONCLIENTMETRICS: cbSize, five ints, font, two ints, font, two ints and
 * three fonts (A 340 bytes, W 500); ICONMETRICS: cbSize, three ints, font
 * (A 76, W 108). */
static void metrics_to_wide(uint8_t *w, const uint8_t *a) {
  w32_copy(w + 4, a + 4, 20);
  font_to_wide(w + 24, a + 24);
  w32_copy(w + 116, a + 84, 8);
  font_to_wide(w + 124, a + 92);
  w32_copy(w + 216, a + 152, 8);
  for (UINT i = 0; i < 3; i++)
    font_to_wide(w + 224 + i * 92, a + 160 + i * 60);
}
static void metrics_to_ansi(uint8_t *a, const uint8_t *w) {
  w32_put32(a, 340);
  w32_copy(a + 4, w + 4, 20);
  font_to_ansi(a + 24, w + 24);
  w32_copy(a + 84, w + 116, 8);
  font_to_ansi(a + 92, w + 124);
  w32_copy(a + 152, w + 216, 8);
  for (UINT i = 0; i < 3; i++)
    font_to_ansi(a + 160 + i * 60, w + 224 + i * 92);
}
BOOL W32API SystemParametersInfoW(UINT action, UINT param, void *value,
                                  UINT flags) {
  U32_GUARD;
  uint8_t a[340], *w = value;
  switch (action) {
  case 31: /* SPI_GETICONTITLELOGFONT */
    if (!w)
      return u32_fail(87);
    if (!SystemParametersInfoA(action, 60, a, flags))
      return 0;
    font_to_wide(w, a);
    return 1;
  case 34: /* SPI_SETICONTITLELOGFONT */
    if (!w)
      return SystemParametersInfoA(action, param, 0, flags);
    font_to_ansi(a, w);
    return SystemParametersInfoA(action, 60, a, flags);
  case 41: /* SPI_GETNONCLIENTMETRICS */
    if (!w || w32_u32(w) != 500)
      return u32_fail(87);
    w32_put32(a, 340);
    if (!SystemParametersInfoA(action, 340, a, flags))
      return 0;
    metrics_to_wide(w, a);
    return 1;
  case 42: /* SPI_SETNONCLIENTMETRICS */
    if (!w || w32_u32(w) != 500)
      return u32_fail(87);
    metrics_to_ansi(a, w);
    return SystemParametersInfoA(action, 340, a, flags);
  case 45: /* SPI_GETICONMETRICS */
    if (!w || w32_u32(w) != 108)
      return u32_fail(87);
    w32_put32(a, 76);
    if (!SystemParametersInfoA(action, 76, a, flags))
      return 0;
    w32_copy(w + 4, a + 4, 12);
    font_to_wide(w + 16, a + 16);
    return 1;
  case 46: /* SPI_SETICONMETRICS */
    if (!w || w32_u32(w) != 108)
      return u32_fail(87);
    w32_put32(a, 76);
    w32_copy(a + 4, w + 4, 12);
    font_to_ansi(a + 16, w + 16);
    return SystemParametersInfoA(action, 76, a, flags);
  case 20: { /* SPI_SETDESKWALLPAPER: pvParam string */
    char *s = w ? u32_narrow((const WCHAR *)w) : 0;
    if (w && !s)
      return 0;
    BOOL ok = SystemParametersInfoA(action, param, s, flags);
    u32_text_free(s);
    return ok;
  }
  case 115: { /* SPI_GETDESKWALLPAPER: uiParam characters */
    if (!w || !param)
      return u32_fail(87);
    char *s = u32_alloc(param);
    if (!s)
      return 0;
    BOOL ok = SystemParametersInfoA(action, param, s, flags);
    if (ok)
      u32_to_wide((WCHAR *)w, param, s, -1);
    u32_free(s);
    return ok;
  }
  default:
    return SystemParametersInfoA(action, param, value, flags);
  }
}
