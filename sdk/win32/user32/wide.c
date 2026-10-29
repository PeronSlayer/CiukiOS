/* f5-20B: Unicode forms of the dialog, menu, accelerator, resource, message
 * box and text-drawing APIs, converted through code page 1252 to the ANSI
 * implementations (Microsoft Layer for Unicode model; see unicode.c).
 * Templates (DIALOG, MENU) are Unicode in resources already, so the W and A
 * loaders share one parser; the W dialog functions make the dialog
 * procedure Unicode.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "unicode.h"
#define TEMPLATE_MAX (1u << 20)
static const WCHAR empty[1] = {0};
/* Resource names: integer resources pass through, strings are converted. */
#define NAME_IN(name, out)                                                     \
  char *out = u32_narrow(name);                                                \
  if ((name) && !(out))                                                        \
    return 0;

/* ---- dialogs ------------------------------------------------------------ */
HWND W32API CreateDialogIndirectParamW(HINSTANCE instance,
                                       const DLGTEMPLATE *t, HWND owner,
                                       DLGPROC proc, LPARAM param) {
  U32_GUARD;
  return u32_dialog_from(instance, t, TEMPLATE_MAX, owner, proc, param, 0, 1);
}
HWND W32API CreateDialogParamW(HINSTANCE instance, const WCHAR *name,
                               HWND owner, DLGPROC proc, LPARAM param) {
  U32_GUARD;
  NAME_IN(name, n);
  DWORD size = 0;
  const void *data = u32_resource(instance, n, 5, &size);
  u32_text_free(n);
  if (!data)
    return (HWND)(uintptr_t)u32_fail(1812);
  return u32_dialog_from(instance, data, size, owner, proc, param, 0, 1);
}
intptr_t W32API DialogBoxIndirectParamW(HINSTANCE instance,
                                        const DLGTEMPLATE *t, HWND parent,
                                        DLGPROC proc, LPARAM param) {
  U32_GUARD;
  return u32_dialog_box(instance, t, TEMPLATE_MAX, parent, proc, param, 1);
}
intptr_t W32API DialogBoxParamW(HINSTANCE instance, const WCHAR *name,
                                HWND parent, DLGPROC proc, LPARAM param) {
  U32_GUARD;
  char *n = u32_narrow(name);
  if (name && !n)
    return -1;
  DWORD size = 0;
  const void *data = u32_resource(instance, n, 5, &size);
  u32_text_free(n);
  if (!data) {
    u32_fail(1812);
    return -1;
  }
  return u32_dialog_box(instance, data, size, parent, proc, param, 1);
}
static WPARAM char_ansi(UINT m, WPARAM wp) {
  if (m == WM_CHAR || m == WM_SYSCHAR || m == 0x103u || m == 0x107u)
    return (uint8_t)u32_ansi((unsigned)(wp & 0xffff));
  return wp;
}
BOOL W32API IsDialogMessageW(HWND dlg, MSG *msg) {
  U32_GUARD;
  if (!msg)
    return 0;
  MSG copy = *msg;
  copy.wParam = char_ansi(msg->message, msg->wParam);
  return IsDialogMessageA(dlg, &copy);
}
UINT W32API GetDlgItemTextW(HWND dlg, int id, WCHAR *out, int n) {
  U32_GUARD;
  if (out && n > 0)
    out[0] = 0;
  HWND c = GetDlgItem(dlg, id);
  return c && out && n > 0
             ? (UINT)SendMessageW(c, WM_GETTEXT, (WPARAM)n, (LPARAM)out)
             : 0;
}
BOOL W32API SetDlgItemTextW(HWND dlg, int id, const WCHAR *s) {
  U32_GUARD;
  HWND c = GetDlgItem(dlg, id);
  return c ? (BOOL)SendMessageW(c, WM_SETTEXT, 0, (LPARAM)(s ? s : empty))
           : 0;
}
LRESULT W32API SendDlgItemMessageW(HWND dlg, int id, UINT m, WPARAM wp,
                                   LPARAM lp) {
  U32_GUARD;
  HWND c = GetDlgItem(dlg, id);
  return c ? SendMessageW(c, m, wp, lp) : 0;
}

/* ---- menus -------------------------------------------------------------- */
static BOOL menu_text(UINT flags) {
  return !(flags & (MF_BITMAP | MF_OWNERDRAW | MF_SEPARATOR));
}
BOOL W32API AppendMenuW(HMENU h, UINT flags, uintptr_t id, const WCHAR *data) {
  U32_GUARD;
  char *s = menu_text(flags) && data ? u32_narrow(data) : (char *)data;
  if (menu_text(flags) && data && !s)
    return 0;
  BOOL ok = AppendMenuA(h, flags, id, s);
  if (menu_text(flags))
    u32_text_free(s);
  return ok;
}
BOOL W32API InsertMenuW(HMENU h, UINT at, UINT flags, uintptr_t id,
                        const WCHAR *data) {
  U32_GUARD;
  char *s = menu_text(flags) && data ? u32_narrow(data) : (char *)data;
  if (menu_text(flags) && data && !s)
    return 0;
  BOOL ok = InsertMenuA(h, at, flags, id, s);
  if (menu_text(flags))
    u32_text_free(s);
  return ok;
}
BOOL W32API ModifyMenuW(HMENU h, UINT at, UINT flags, uintptr_t id,
                        const WCHAR *data) {
  U32_GUARD;
  char *s = menu_text(flags) && data ? u32_narrow(data) : (char *)data;
  if (menu_text(flags) && data && !s)
    return 0;
  BOOL ok = ModifyMenuA(h, at, flags, id, s);
  if (menu_text(flags))
    u32_text_free(s);
  return ok;
}
/* MENUITEMINFOA and MENUITEMINFOW share one layout; dwTypeData is a string
 * for MIIM_STRING, or for MIIM_TYPE with a string type. */
static BOOL item_text(const MENUITEMINFOW *i) {
  return (i->fMask & 0x40u) ||
         ((i->fMask & MIIM_TYPE) && menu_text(i->fType));
}
static BOOL item_in(const MENUITEMINFOW *i, MENUITEMINFOA *a, char **text) {
  if (!i || (i->cbSize != sizeof(*i) && i->cbSize != sizeof(*i) - sizeof(HANDLE)))
    return u32_fail(87);
  w32_copy(a, i, sizeof(*a));
  *text = 0;
  if (item_text(i) && i->dwTypeData) {
    *text = u32_narrow(i->dwTypeData);
    if (!*text)
      return 0;
    a->dwTypeData = *text;
  }
  return 1;
}
BOOL W32API InsertMenuItemW(HMENU h, UINT at, BOOL by_position,
                            const MENUITEMINFOW *i) {
  U32_GUARD;
  MENUITEMINFOA a;
  char *text;
  if (!item_in(i, &a, &text))
    return 0;
  BOOL ok = InsertMenuItemA(h, at, by_position, &a);
  u32_text_free(text);
  return ok;
}
BOOL W32API SetMenuItemInfoW(HMENU h, UINT at, BOOL by_position,
                             const MENUITEMINFOW *i) {
  U32_GUARD;
  MENUITEMINFOA a;
  char *text;
  if (!item_in(i, &a, &text))
    return 0;
  BOOL ok = SetMenuItemInfoA(h, at, by_position, &a);
  u32_text_free(text);
  return ok;
}
BOOL W32API GetMenuItemInfoW(HMENU h, UINT at, BOOL by_position,
                             MENUITEMINFOW *i) {
  U32_GUARD;
  if (!i || (i->cbSize != sizeof(*i) && i->cbSize != sizeof(*i) - sizeof(HANDLE)))
    return u32_fail(87);
  MENUITEMINFOA a;
  w32_copy(&a, i, sizeof(a));
  char text[256];
  BOOL wants = (i->fMask & (MIIM_TYPE | 0x40u)) != 0;
  if (wants) {
    a.dwTypeData = text;
    a.cch = sizeof(text);
  }
  if (!GetMenuItemInfoA(h, at, by_position, &a))
    return 0;
  WCHAR *out = i->dwTypeData;
  UINT cap = i->cch;
  w32_copy(i, &a, i->cbSize);
  if (wants && a.dwTypeData == text) {
    /* The ANSI call copied a string (cch < 256) or left the buffer alone. */
    i->dwTypeData = out;
    if (a.cch < sizeof(text))
      i->cch = out && cap ? u32_to_wide(out, cap, text, (int)a.cch) : a.cch;
    else
      i->cch = cap;
  }
  return 1;
}
int W32API GetMenuStringW(HMENU h, UINT at, WCHAR *out, int n, UINT flags) {
  U32_GUARD;
  char text[256];
  int k = GetMenuStringA(h, at, text, sizeof(text), flags);
  if (!out)
    return k;
  if (n <= 0)
    return 0;
  return (int)u32_to_wide(out, (UINT)n, text, k);
}
HMENU W32API LoadMenuW(HINSTANCE instance, const WCHAR *name) {
  U32_GUARD;
  NAME_IN(name, n);
  HMENU m = LoadMenuA(instance, n);
  u32_text_free(n);
  return m;
}
HMENU W32API LoadMenuIndirectW(const void *data) {
  return LoadMenuIndirectA(data);
}

/* ---- accelerators -------------------------------------------------------- */
/* A character (non-FVIRTKEY) accelerator stores the code-page character in
 * the ANSI table and the Unicode character in the W view of it. */
HACCEL W32API CreateAcceleratorTableW(ACCEL *entries, int n) {
  U32_GUARD;
  if (!entries || n <= 0 || n > 512)
    return (HACCEL)(uintptr_t)u32_fail(87);
  ACCEL copy[512];
  for (int i = 0; i < n; i++) {
    copy[i] = entries[i];
    if (!(copy[i].fVirt & FVIRTKEY))
      copy[i].key = (uint16_t)(uint8_t)u32_ansi(copy[i].key);
  }
  return CreateAcceleratorTableA(copy, n);
}
int W32API CopyAcceleratorTableW(HACCEL h, ACCEL *out, int n) {
  U32_GUARD;
  int k = CopyAcceleratorTableA(h, out, n);
  for (int i = 0; out && i < k; i++)
    if (!(out[i].fVirt & FVIRTKEY) && out[i].key < 256)
      out[i].key = (uint16_t)u32_scalar((uint8_t)out[i].key);
  return k;
}
HACCEL W32API LoadAcceleratorsW(HINSTANCE instance, const WCHAR *name) {
  U32_GUARD;
  NAME_IN(name, n);
  HACCEL h = LoadAcceleratorsA(instance, n);
  u32_text_free(n);
  return h;
}
int W32API TranslateAcceleratorW(HWND h, HACCEL table, MSG *msg) {
  U32_GUARD;
  if (!msg)
    return u32_fail(1403);
  MSG copy = *msg;
  copy.wParam = char_ansi(msg->message, msg->wParam);
  return TranslateAcceleratorA(h, table, &copy);
}

/* ---- resources ----------------------------------------------------------- */
HANDLE W32API LoadBitmapW(HINSTANCE instance, const WCHAR *name) {
  U32_GUARD;
  NAME_IN(name, n);
  HANDLE h = LoadBitmapA(instance, n);
  u32_text_free(n);
  return h;
}
HCURSOR W32API LoadCursorW(HINSTANCE instance, const WCHAR *name) {
  U32_GUARD;
  NAME_IN(name, n);
  HCURSOR h = LoadCursorA(instance, n);
  u32_text_free(n);
  return h;
}
HICON W32API LoadIconW(HINSTANCE instance, const WCHAR *name) {
  U32_GUARD;
  NAME_IN(name, n);
  HICON h = LoadIconA(instance, n);
  u32_text_free(n);
  return h;
}
HANDLE W32API LoadImageW(HINSTANCE instance, const WCHAR *name, UINT type,
                         int cx, int cy, UINT flags) {
  U32_GUARD;
  NAME_IN(name, n);
  HANDLE h = LoadImageA(instance, n, type, cx, cy, flags);
  u32_text_free(n);
  return h;
}

/* ---- message boxes -------------------------------------------------------- */
int W32API MessageBoxExW(HWND owner, const WCHAR *text, const WCHAR *caption,
                         UINT type, uint16_t language) {
  U32_GUARD;
  char *t = text ? u32_narrow(text) : 0, *c = caption ? u32_narrow(caption) : 0;
  int r = 0;
  if ((text && !t) || (caption && !c))
    u32_fail(8);
  else
    r = MessageBoxExA(owner, t, c, type, language);
  u32_text_free(t);
  u32_text_free(c);
  return r;
}
int W32API MessageBoxW(HWND owner, const WCHAR *text, const WCHAR *caption,
                       UINT type) {
  return MessageBoxExW(owner, text, caption, type, 0);
}
/* MessageBoxIndirect: the Windows 98 parameter block. MB_USERICON (0x80)
 * names an icon resource of hInstance; the box shows the standard layout
 * without it (no custom-icon slot, recorded as limited). The help callback
 * is not invoked: CiukiOS has no WinHelp (MB_HELP sends WM_HELP to the
 * owner as before). */
int W32API MessageBoxIndirectA(const MSGBOXPARAMSA *p) {
  U32_GUARD;
  if (!p || p->cbSize != sizeof(*p))
    return u32_fail(87);
  UINT type = p->dwStyle & ~0x80u;
  return MessageBoxExA(p->hwndOwner, p->lpszText, p->lpszCaption, type,
                       (uint16_t)p->dwLanguageId);
}
int W32API MessageBoxIndirectW(const MSGBOXPARAMSW *p) {
  U32_GUARD;
  if (!p || p->cbSize != sizeof(*p))
    return u32_fail(87);
  UINT type = p->dwStyle & ~0x80u;
  return MessageBoxExW(p->hwndOwner, p->lpszText, p->lpszCaption, type,
                       (uint16_t)p->dwLanguageId);
}

/* ---- text drawing ------------------------------------------------------- */
BOOL W32API DrawStateW(HDC dc, HBRUSH brush, DRAWSTATEPROC proc, LPARAM data,
                       WPARAM n, int x, int y, int cx, int cy, UINT flags) {
  U32_GUARD;
  UINT type = flags & 15;
  if ((type != DST_TEXT && type != DST_PREFIXTEXT) || !data)
    return DrawStateA(dc, brush, proc, data, n, x, y, cx, cy, flags);
  const WCHAR *s = (const WCHAR *)data;
  UINT len = n ? (UINT)n : u32_wlen(s);
  char *a = u32_alloc(len + 1);
  if (!a)
    return 0;
  u32_to_ansi(a, len + 1, s, (int)len);
  BOOL ok = DrawStateA(dc, brush, proc, (LPARAM)a, len, x, y, cx, cy, flags);
  u32_free(a);
  return ok;
}
static char *counted(const WCHAR *s, int *n) {
  if (!s)
    return 0;
  if (*n < 0)
    *n = (int)u32_wlen(s);
  char *a = u32_alloc((UINT)*n + 1);
  if (a)
    u32_to_ansi(a, (UINT)*n + 1, s, *n);
  return a;
}
DWORD W32API GetTabbedTextExtentW(HDC dc, const WCHAR *s, int n, int count,
                                  const int *tabs) {
  U32_GUARD;
  char *a = counted(s, &n);
  if (s && !a)
    return 0;
  DWORD r = GetTabbedTextExtentA(dc, a, n, count, tabs);
  u32_free(a);
  return r;
}
LONG W32API TabbedTextOutW(HDC dc, int x, int y, const WCHAR *s, int n,
                           int count, const int *tabs, int origin) {
  U32_GUARD;
  char *a = counted(s, &n);
  if (s && !a)
    return 0;
  LONG r = TabbedTextOutA(dc, x, y, a, n, count, tabs, origin);
  u32_free(a);
  return r;
}
typedef BOOL(W32API *GRAYSTRINGPROC)(HDC, LPARAM, int);
BOOL W32API GrayStringA(HDC, HBRUSH, GRAYSTRINGPROC, LPARAM, int, int, int,
                        int, int);
BOOL W32API GrayStringW(HDC dc, HBRUSH brush, GRAYSTRINGPROC proc, LPARAM data,
                        int n, int x, int y, int cx, int cy) {
  U32_GUARD;
  if (proc || !data)
    return GrayStringA(dc, brush, proc, data, n, x, y, cx, cy);
  if (!n)
    n = -1;
  char *a = counted((const WCHAR *)data, &n);
  if (!a)
    return 0;
  BOOL ok = GrayStringA(dc, brush, 0, (LPARAM)a, n, x, y, cx, cy);
  u32_free(a);
  return ok;
}

/* ---- keyboard ----------------------------------------------------------- */
int16_t W32API VkKeyScanExW(WCHAR c, uintptr_t layout) {
  U32_GUARD;
  int a = u32_ansi(c);
  if (a == '?' && c != '?')
    return -1;
  return VkKeyScanExA((char)a, layout);
}
int16_t W32API VkKeyScanW(WCHAR c) {
  U32_GUARD;
  int a = u32_ansi(c);
  if (a == '?' && c != '?')
    return -1;
  return VkKeyScanA((char)a);
}

/* ---- DlgDirList ---------------------------------------------------------- */
/* The path part of the specification becomes the current directory (as on
 * Windows), the list is filled with LB_DIR/CB_DIR from the remaining file
 * specification, the static control shows the current directory, and the
 * caller's buffer is left holding the file specification. */
static int dir_list(HWND dlg, char *spec, int list, int path, UINT type,
                    BOOL combo) {
  BOOL(W32API * set_dir)(const char *) =
      (BOOL(W32API *)(const char *))u32_kernel("SetCurrentDirectoryA");
  DWORD(W32API * get_dir)(DWORD, char *) =
      (DWORD(W32API *)(DWORD, char *))u32_kernel("GetCurrentDirectoryA");
  char pattern[260] = "*.*";
  if (spec && *spec) {
    UINT n = w32_strlen(spec), cut = n;
    while (cut && spec[cut - 1] != '\\' && spec[cut - 1] != '/' &&
           spec[cut - 1] != ':')
      cut--;
    if (cut) {
      char dir[260];
      if (cut >= sizeof(dir))
        return u32_fail(206);
      w32_copy(dir, spec, cut);
      /* Keep "C:\" and ":" forms; drop a trailing separator otherwise. */
      dir[cut] = 0;
      if (cut > 1 && (dir[cut - 1] == '\\' || dir[cut - 1] == '/') &&
          dir[cut - 2] != ':')
        dir[cut - 1] = 0;
      if (!set_dir || !set_dir(dir))
        return u32_fail(3);
    }
    if (spec[cut])
      u32_strcpy(pattern, sizeof(pattern), spec + cut);
    u32_strcpy(spec, n + 1, pattern);
  }
  if (list) {
    HWND c = GetDlgItem(dlg, list);
    if (!c)
      return 0;
    SendMessageA(c, combo ? 0x14bu /* CB_RESETCONTENT */ : LB_RESETCONTENT, 0,
                 0);
    if ((int)SendMessageA(c, combo ? CB_DIR : LB_DIR, type & ~0x2000u,
                          (LPARAM)pattern) == -2)
      return 0;
  }
  if (path) {
    char current[260] = "";
    if (get_dir)
      get_dir(sizeof(current), current);
    SetDlgItemTextA(dlg, path, current);
  }
  return 1;
}
/* fx-hlrun (MFC's CWnd::DlgDirSelect imports them): the selected item of a
 * DlgDirList list box or combo box. "[name]" is a directory and comes back
 * as "name\\", "[-x-]" a drive and comes back as "x:", both returning TRUE;
 * a file name returns FALSE, with "." appended when it has no extension.
 * Wine 10.0 dlls/user32/dialog.c DIALOG_DlgDirSelect. */
static BOOL dir_select(HWND dlg, char *out, int len, int id, BOOL combo) {
  HWND c = GetDlgItem(dlg, id);
  if (!c)
    return 0;
  LRESULT item = SendMessageA(c, combo ? 0x147u /* CB_GETCURSEL */ : 0x188u /* LB_GETCURSEL */, 0, 0);
  if (item < 0)
    return 0;
  LRESULT size = SendMessageA(c, combo ? 0x149u /* CB_GETLBTEXTLEN */ : 0x18au /* LB_GETTEXTLEN */, (WPARAM)item, 0);
  if (size < 0 || size > 4096)
    return 0;
  char *b = u32_alloc((DWORD)size + 3);
  if (!b)
    return 0;
  w32_fill(b, 0, (DWORD)size + 3);
  SendMessageA(c, combo ? 0x148u /* CB_GETLBTEXT */ : 0x189u /* LB_GETTEXT */, (WPARAM)item, (LPARAM)b);
  BOOL directory = b[0] == '[';
  char *p = b;
  if (directory && b[1] == '-') {
    b[3] = ':';
    b[4] = 0;
    p = b + 2;
  } else if (directory) {
    UINT n = w32_strlen(b);
    if (n)
      b[n - 1] = '\\';
    p = b + 1;
  } else {
    BOOL dot = 0;
    for (char *q = b; *q; q++)
      dot |= *q == '.';
    if (!dot) {
      UINT n = w32_strlen(b);
      b[n] = '.';
      b[n + 1] = 0;
    }
  }
  if (out && len > 0)
    u32_strcpy(out, (UINT)len, p);
  u32_free(b);
  return directory;
}
BOOL W32API DlgDirSelectExA(HWND dlg, char *out, int len, int id) {
  U32_GUARD;
  return dir_select(dlg, out, len, id, 0);
}
BOOL W32API DlgDirSelectComboBoxExA(HWND dlg, char *out, int len, int id) {
  U32_GUARD;
  return dir_select(dlg, out, len, id, 1);
}
static BOOL dir_select_w(HWND dlg, WCHAR *out, int len, int id, BOOL combo) {
  char a[264];
  BOOL r = dir_select(dlg, a, sizeof(a), id, combo);
  if (out && len > 0)
    u32_to_wide(out, (UINT)len, a, -1);
  return r;
}
BOOL W32API DlgDirSelectExW(HWND dlg, WCHAR *out, int len, int id) {
  U32_GUARD;
  return dir_select_w(dlg, out, len, id, 0);
}
BOOL W32API DlgDirSelectComboBoxExW(HWND dlg, WCHAR *out, int len, int id) {
  U32_GUARD;
  return dir_select_w(dlg, out, len, id, 1);
}
int W32API DlgDirListA(HWND dlg, char *spec, int list, int path, UINT type) {
  U32_GUARD;
  return dir_list(dlg, spec, list, path, type, 0);
}
int W32API DlgDirListComboBoxA(HWND dlg, char *spec, int list, int path,
                               UINT type) {
  U32_GUARD;
  return dir_list(dlg, spec, list, path, type, 1);
}
static int dir_list_w(HWND dlg, WCHAR *spec, int list, int path, UINT type,
                      BOOL combo) {
  char *a = spec ? u32_narrow(spec) : 0;
  if (spec && !a)
    return 0;
  int r = dir_list(dlg, a, list, path, type, combo);
  if (r && spec)
    u32_to_wide(spec, u32_wlen(spec) + 1, a, -1);
  u32_text_free(a);
  return r;
}
int W32API DlgDirListW(HWND dlg, WCHAR *spec, int list, int path, UINT type) {
  U32_GUARD;
  return dir_list_w(dlg, spec, list, path, type, 0);
}
int W32API DlgDirListComboBoxW(HWND dlg, WCHAR *spec, int list, int path,
                               UINT type) {
  U32_GUARD;
  return dir_list_w(dlg, spec, list, path, type, 1);
}
