/* MessageBox(Ex): a dialog built by the dialog manager with the message
 * font, stock icons, button sets, default button and owner disabling.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "classic.h"
#define ICON_ID 0x14
#define TEXT_ID 0xffff
#define MARGIN 12
#define BUTTON_W 75
#define BUTTON_H 23
#define BUTTON_GAP 6
struct box {
  UINT type, buttons[4], count;
  BOOL cancel;
  const char *text;
};
static const char *button_text(UINT id) {
  switch (id) {
  case IDOK:
    return "OK";
  case IDCANCEL:
    return "Cancel";
  case IDABORT:
    return "&Abort";
  case IDRETRY:
    return "&Retry";
  case IDIGNORE:
    return "&Ignore";
  case IDYES:
    return "&Yes";
  case IDNO:
    return "&No";
  case IDHELP:
    return "Help";
  default:
    return "";
  }
}
static UINT buttons_for(UINT type, UINT *out, BOOL *cancel) {
  static const UINT sets[6][3] = {{IDOK, 0, 0},
                                  {IDOK, IDCANCEL, 0},
                                  {IDABORT, IDRETRY, IDIGNORE},
                                  {IDYES, IDNO, IDCANCEL},
                                  {IDYES, IDNO, 0},
                                  {IDRETRY, IDCANCEL, 0}};
  UINT set = type & MB_TYPEMASK, n = 0;
  if (set > 5)
    set = 0;
  for (UINT i = 0; i < 3 && sets[set][i]; i++)
    out[n++] = sets[set][i];
  if (type & MB_HELP)
    out[n++] = IDHELP;
  *cancel = set == 0 || set == 1 || set == 3 || set == 5;
  return n;
}
static int icon_for(UINT type) {
  switch (type & MB_ICONMASK) {
  case MB_ICONHAND:
    return 32513;
  case MB_ICONQUESTION:
    return 32514;
  case MB_ICONEXCLAMATION:
    return 32515;
  case MB_ICONASTERISK:
    return 32516;
  default:
    return 0;
  }
}
/* Wrapped text extent with the dialog's font. */
static RECT measure(HWND dlg, const char *text, int width) {
  RECT r = {0, 0, width, 0};
  const struct u32_graphics *g = u32_graphics();
  HFONT font = (HFONT)SendMessageA(dlg, WM_GETFONT, 0, 0);
  if (g && g->memory_dc) {
    DWORD dc = g->memory_dc(0);
    if (dc) {
      DWORD old = font ? g->select(dc, U32_PTR(font)) : 0;
      g->draw_text(dc, text, -1, &r, 0x400u | 0x10u | 0x40u | 0x800u, 0, 0);
      if (old)
        g->select(dc, old);
      g->delete_dc(dc);
      return r;
    }
  }
  const struct u32_font_info *f = u32_font_info(font);
  int lines = 1, x = 0, widest = 0;
  for (const char *p = text; *p; p++) {
    if (*p == '\n' || x + f->widths[(uint8_t)*p] > width) {
      lines++;
      x = 0;
      if (*p == '\n')
        continue;
    }
    x += f->widths[(uint8_t)*p];
    if (x > widest)
      widest = x;
  }
  r.right = widest;
  r.bottom = lines * f->height;
  return r;
}
static void layout(HWND dlg, struct box *b) {
  int screen = GetSystemMetrics(0);
  HWND icon = GetDlgItem(dlg, ICON_ID), text = GetDlgItem(dlg, TEXT_ID);
  int icon_w = icon ? 32 : 0, text_x = MARGIN + (icon ? icon_w + MARGIN : 0);
  int limit = screen * 5 / 8 - text_x - MARGIN;
  if (limit < 100)
    limit = 100;
  RECT t = measure(dlg, b->text, limit);
  int text_h = t.bottom - t.top, text_w = t.right - t.left;
  int content_h = text_h > icon_w ? text_h : icon_w;
  int text_y = MARGIN + (icon && text_h < icon_w ? (icon_w - text_h) / 2 : 0);
  int row = (int)b->count * BUTTON_W + ((int)b->count - 1) * BUTTON_GAP;
  int width = text_x + text_w + MARGIN;
  if (width < row + 2 * MARGIN)
    width = row + 2 * MARGIN;
  char caption[256];
  GetWindowTextA(dlg, caption, sizeof(caption));
  const struct u32_font_info *cf = u32_font_info(0);
  int caption_w = u32_text_width(cf, caption, (int)w32_strlen(caption)) + 48;
  if (width < caption_w)
    width = caption_w;
  int buttons_y = MARGIN + content_h + MARGIN;
  int height = buttons_y + BUTTON_H + MARGIN;
  if (icon)
    MoveWindow(icon, MARGIN, MARGIN, 32, 32, 0);
  if (text)
    MoveWindow(text, text_x, text_y, text_w + 1, text_h, 0);
  int x = (width - row) / 2;
  for (UINT i = 0; i < b->count; i++) {
    HWND button = GetDlgItem(dlg, (int)b->buttons[i]);
    if (button)
      MoveWindow(button, x, buttons_y, BUTTON_W, BUTTON_H, 0);
    x += BUTTON_W + BUTTON_GAP;
  }
  RECT window = {0, 0, width, height};
  AdjustWindowRectEx(&window, (DWORD)GetWindowLongA(dlg, GWL_STYLE), 0,
                     (DWORD)GetWindowLongA(dlg, GWL_EXSTYLE));
  int ww = window.right - window.left, wh = window.bottom - window.top;
  RECT area = {0, 0, screen, GetSystemMetrics(1)};
  SystemParametersInfoA(48, 0, &area, 0);
  HWND owner = GetWindow(dlg, 4);
  RECT center = area;
  if (owner && IsWindowVisible(owner))
    GetWindowRect(owner, &center);
  int left = (center.left + center.right - ww) / 2,
      top = (center.top + center.bottom - wh) / 2;
  if (left + ww > area.right)
    left = area.right - ww;
  if (top + wh > area.bottom)
    top = area.bottom - wh;
  if (left < area.left)
    left = area.left;
  if (top < area.top)
    top = area.top;
  MoveWindow(dlg, left, top, ww, wh, 0);
}
static intptr_t W32API box_proc(HWND dlg, UINT m, WPARAM wp, LPARAM lp) {
  struct box *b = (struct box *)GetPropA(dlg, "CiukiMessageBox");
  if (m == WM_INITDIALOG) {
    b = (struct box *)lp;
    SetPropA(dlg, "CiukiMessageBox", (HANDLE)b);
    HWND label = GetDlgItem(dlg, TEXT_ID);
    if (label && w32_strlen(b->text) > 255)
      SetWindowTextA(label, b->text);
    layout(dlg, b);
    UINT index = (b->type & MB_DEFMASK) >> 8;
    if (index >= b->count)
      index = 0;
    HWND def = GetDlgItem(dlg, (int)b->buttons[index]);
    SendMessageA(dlg, DM_SETDEFID, b->buttons[index], 0);
    if (def)
      u32_dialog_focus(dlg, def);
    if (b->type & MB_SETFOREGROUND)
      SetForegroundWindow(dlg);
    return 0;
  }
  if (m == WM_HELP && b && (b->type & MB_HELP)) {
    /* F1 in a message box with a Help button behaves like the button. */
    HWND owner = GetWindow(dlg, 4);
    if (owner)
      SendMessageA(owner, WM_HELP, 0, lp);
    return 1;
  }
  if (m == WM_COMMAND && b) {
    UINT id = (UINT)(wp & 0xffff);
    if (id == IDHELP) {
      /* G15 (MB_HELP): the Help button sends WM_HELP to the owner window. */
      HWND owner = GetWindow(dlg, 4);
      POINT p;
      GetCursorPos(&p);
      if (owner)
        u32_help_send(owner, GetDlgItem(dlg, IDHELP), IDHELP, p);
      return 1;
    }
    if (id == IDCANCEL && !GetDlgItem(dlg, IDCANCEL)) {
      if (!b->cancel)
        return 1;
      id = b->buttons[0];
    }
    for (UINT i = 0; i < b->count; i++)
      if (b->buttons[i] == id) {
        RemovePropA(dlg, "CiukiMessageBox");
        EndDialog(dlg, (intptr_t)id);
        return 1;
      }
    return 1;
  }
  return 0;
}
int W32API MessageBoxExA(HWND owner, const char *text, const char *caption,
                         UINT type, uint16_t language) {
  U32_GUARD;
  (void)language;
  struct box b = {0};
  b.type = type;
  b.text = text ? text : "";
  b.count = buttons_for(type, b.buttons, &b.cancel);
  struct u32_template *t = u32_alloc(sizeof(*t));
  if (!t)
    return u32_fail(8);
  w32_fill(t, 0, sizeof(*t));
  t->style = WS_POPUP | WS_CAPTION | 0x80000u | DS_MODALFRAME | DS_SETFONT |
             WS_VISIBLE;
  t->ex = (type & (MB_SYSTEMMODAL | MB_TOPMOST)) ? WS_EX_TOPMOST : 0;
  t->cx = t->cy = 40;
  t->has_font = 1;
  t->point = 8;
  u32_strcpy(t->face, sizeof(t->face), "MS Sans Serif");
  u32_strcpy(t->title, sizeof(t->title), caption ? caption : "Error");
  UINT n = 0;
  int icon = icon_for(type);
  if (icon) {
    struct u32_template_item *it = &t->items[n++];
    u32_strcpy(it->klass, sizeof(it->klass), "STATIC");
    it->style = WS_CHILD | WS_VISIBLE | SS_ICON;
    it->id = ICON_ID;
    it->text_ordinal = (UINT)icon;
    it->cx = it->cy = 21;
  }
  struct u32_template_item *label = &t->items[n++];
  u32_strcpy(label->klass, sizeof(label->klass), "STATIC");
  label->style = WS_CHILD | WS_VISIBLE | WS_GROUP | SS_NOPREFIX |
                 (type & MB_RIGHT ? SS_RIGHT : SS_LEFT);
  label->id = TEXT_ID;
  u32_strcpy(label->text, sizeof(label->text), b.text);
  UINT index = (type & MB_DEFMASK) >> 8;
  if (index >= b.count)
    index = 0;
  for (UINT i = 0; i < b.count; i++) {
    struct u32_template_item *it = &t->items[n++];
    u32_strcpy(it->klass, sizeof(it->klass), "BUTTON");
    it->style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | (i ? 0 : WS_GROUP) |
                (i == index ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON);
    it->id = b.buttons[i];
    u32_strcpy(it->text, sizeof(it->text), button_text(b.buttons[i]));
    it->cx = 50;
    it->cy = 14;
  }
  t->count = n;
  /* Long texts exceed the 255-byte template field; WM_INITDIALOG sets the
   * label from the caller's buffer before the layout. */
  HWND top = owner;
  while (top && (GetWindowLongA(top, GWL_STYLE) & WS_CHILD))
    top = GetParent(top);
  HWND disabled[U32_WINDOWS];
  UINT disabled_count = 0;
  if ((type & MB_MODEMASK) == MB_TASKMODAL && !top) {
    for (UINT i = 0; i < U32_WINDOWS; i++) {
      struct u32_window *w = &u32_windows[i];
      if (w->handle && !(w->style & WS_CHILD) && w->tid == u32_tid() &&
          IsWindowEnabled(w->handle) && IsWindowVisible(w->handle)) {
        EnableWindow(w->handle, 0);
        disabled[disabled_count++] = w->handle;
      }
    }
  }
  MessageBeep(type & MB_ICONMASK);
  HWND dlg = u32_dialog_create(0, t, top, box_proc, (LPARAM)&b, 1);
  u32_free(t);
  if (!dlg) {
    for (UINT i = 0; i < disabled_count; i++)
      if (IsWindow(disabled[i]))
        EnableWindow(disabled[i], 1);
    return 0;
  }
  intptr_t result = u32_dialog_run(dlg, top);
  for (UINT i = 0; i < disabled_count; i++)
    if (IsWindow(disabled[i]))
      EnableWindow(disabled[i], 1);
  return result > 0 ? (int)result : 0;
}
int W32API MessageBoxA(HWND owner, const char *text, const char *caption,
                       UINT type) {
  return MessageBoxExA(owner, text, caption, type, 0);
}
