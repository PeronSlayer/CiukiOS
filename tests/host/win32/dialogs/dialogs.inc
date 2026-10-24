/* f5-10C dialog manager, controls, message boxes and installer-style pages.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
static int du_x(int v, int base) { return (v * base * 2 + (v >= 0 ? 4 : -4)) / 8; }
static int du_y(int v, int base) { return (v * base * 2 + (v >= 0 ? 8 : -8)) / 16; }
static RECT child_rect(HWND dlg, HWND c) {
  RECT r;
  GetWindowRect(c, &r);
  POINT o = {0, 0};
  ClientToScreen(dlg, &o);
  OffsetRect(&r, -o.x, -o.y);
  return r;
}
static UINT commands[256], command_count, ctlcolor_static, init_focus_rule;
static HWND init_focus_target;
static intptr_t W32API record_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  (void)lp;
  if (m == WM_INITDIALOG) {
    command_count = 0;
    if (init_focus_rule == 1) {
      init_focus_target = GetDlgItem(h, 102);
      SetFocus(init_focus_target);
      return 0;
    }
    if (GetDlgItem(h, 103))
      CheckRadioButton(h, 103, 105, 103);
    return 1;
  }
  if (m == WM_CTLCOLORSTATIC)
    ctlcolor_static++;
  if (m == WM_COMMAND) {
    if (command_count < 256)
      commands[command_count++] = (UINT)wp;
    UINT id = (UINT)(wp & 0xffff);
    if ((id == IDOK || id == IDCANCEL) && (wp >> 16) == BN_CLICKED) {
      EndDialog(h, id == IDOK ? 42 : IDCANCEL);
      return 1;
    }
  }
  return 0;
}
static BOOL got_command(UINT id, UINT code) {
  for (UINT i = 0; i < command_count; i++)
    if (commands[i] == (id | (code << 16)))
      return 1;
  return 0;
}
static void parse_checks(void) {
  struct u32_template *t = calloc(1, sizeof(*t));
  DWORD n;
  const uint8_t *d = resource(5, 100, &n);
  CHECK(u32_template_parse(d, n, t));
  CHECK(!t->extended && t->count == 12 && t->has_font && t->point == 8);
  CHECK(!strcmp(t->face, "MS Sans Serif") && !strcmp(t->title, "Setup Options"));
  CHECK(!strcmp(t->items[1].klass, "EDIT") && t->items[1].id == 101);
  CHECK(!strcmp(t->items[0].text, "&Name:") && (uint16_t)t->items[0].id == 0xffff);
  CHECK(!strcmp(t->items[5].klass, "BUTTON") && (t->items[5].style & 15) == 9);
  d = resource(5, 101, &n);
  CHECK(u32_template_parse(d, n, t));
  CHECK(t->extended && t->count == 27 && !strcmp(t->face, "MS Shell Dlg") &&
        t->weight == 400 && t->charset == 1 && (t->style & DS_CENTER));
  CHECK(!strcmp(t->items[17].klass, "LISTBOX") &&
        !strcmp(t->items[18].klass, "COMBOBOX") &&
        !strcmp(t->items[21].klass, "SCROLLBAR"));
  d = resource(5, 102, &n);
  CHECK(u32_template_parse(d, n, t) && !t->has_font && t->count == 2);
  /* Every truncation of every template is refused without reading past. */
  static const unsigned ids[] = {100, 101, 102, 103, 110, 111};
  UINT refused = 0;
  for (UINT k = 0; k < 6; k++) {
    d = resource(5, ids[k], &n);
    for (DWORD length = 0; length < n; length++) {
      uint8_t *copy = malloc(length ? length : 1);
      memcpy(copy, d, length);
      int ok = u32_template_parse(copy, length, t);
      free(copy);
      if (ok) {
        /* Trailing alignment padding of the last item is optional. */
        CHECK(length >= n - 3);
      } else
        refused++;
    }
  }
  d = resource(5, 100, &n);
  uint8_t *copy = malloc(n);
  memcpy(copy, d, n);
  w32_put16(copy + 8, 121);
  CHECK(!u32_template_parse(copy, n, t));
  memcpy(copy, d, n);
  for (DWORD i = 0; i + 4 <= n; i++)
    if (w32_u16(copy + i) == 0xffff && w32_u16(copy + i + 2) == 0x0081) {
      w32_put16(copy + i + 2, 0x0086);
      break;
    }
  CHECK(!u32_template_parse(copy, n, t));
  uint32_t seed = 77;
  for (UINT round = 0; round < 2000; round++) {
    memcpy(copy, d, n);
    seed = seed * 1103515245u + 12345u;
    copy[(seed >> 8) % n] ^= (uint8_t)(seed >> 20);
    u32_template_parse(copy, n, t);
  }
  free(copy);
  free(t);
  printf("TEMPLATES truncations refused=%u\n", refused);
}
static void test_templates(void) {
  parse_checks();
  HWND owner = owner_window("Owner", WS_CAPTION | 0x80000u, 600, 420);
  /* Dialog units: the System font and the MS Sans Serif 8 pt plan. */
  CHECK(GetDialogBaseUnits() == (LONG)(8 | (16 << 16)));
  HWND dlg = CreateDialogParamA(FIXTURE, (const char *)(uintptr_t)100, owner,
                                record_proc, 0);
  CHECK(dlg);
  drain(dlg);
  RECT r = {0, 0, 4, 8};
  CHECK(MapDialogRect(dlg, &r) && r.right == 6 && r.bottom == 13);
  RECT hundred = {0, 0, 100, 100};
  MapDialogRect(dlg, &hundred);
  CHECK(hundred.right == 150 && hundred.bottom == 163);
  RECT client;
  GetClientRect(dlg, &client);
  CHECK(client.right == du_x(220, 6) && client.bottom == du_y(140, 13));
  static const struct {
    int id, x, y, cx, cy;
  } layout[] = {{101, 50, 7, 163, 14},   {102, 50, 25, 163, 14},
                {103, 14, 57, 80, 10},   {106, 115, 57, 98, 10},
                {IDOK, 109, 119, 50, 14}, {IDCANCEL, 163, 119, 50, 14}};
  for (UINT i = 0; i < 6; i++) {
    RECT c = child_rect(dlg, GetDlgItem(dlg, layout[i].id));
    CHECK(c.left == du_x(layout[i].x, 6) && c.top == du_y(layout[i].y, 13));
    CHECK(c.right - c.left == du_x(layout[i].cx, 6) &&
          c.bottom - c.top == du_y(layout[i].cy, 13));
  }
  /* Position relative to the owner's client area. */
  RECT window;
  GetWindowRect(dlg, &window);
  POINT o = {0, 0};
  ClientToScreen(owner, &o);
  RECT adjusted = {0, 0, client.right, client.bottom};
  AdjustWindowRectEx(&adjusted, (DWORD)GetWindowLongA(dlg, GWL_STYLE), 0,
                     (DWORD)GetWindowLongA(dlg, GWL_EXSTYLE));
  CHECK(window.left == o.x + adjusted.left && window.top == o.y + adjusted.top);
  /* WM_INITDIALOG returning TRUE focuses the first tab stop, selected. */
  CHECK(GetFocus() == GetDlgItem(dlg, 101));
  CHECK(IsDlgButtonChecked(dlg, 103) == BST_CHECKED);
  CHECK(SendDlgItemMessageA(dlg, IDOK, WM_GETDLGCODE, 0, 0) ==
        (DLGC_BUTTON | DLGC_DEFPUSHBUTTON));
  CHECK(SendDlgItemMessageA(dlg, IDCANCEL, WM_GETDLGCODE, 0, 0) ==
        (DLGC_BUTTON | DLGC_UNDEFPUSHBUTTON));
  CHECK(SendDlgItemMessageA(dlg, 103, WM_GETDLGCODE, 0, 0) ==
        (DLGC_BUTTON | DLGC_RADIOBUTTON));
  CHECK(SendDlgItemMessageA(dlg, 101, WM_GETDLGCODE, 0, 0) ==
        (DLGC_WANTCHARS | DLGC_HASSETSEL | DLGC_WANTARROWS));
  CHECK(SendMessageA(dlg, DM_GETDEFID, 0, 0) == (LRESULT)(IDOK | (DC_HASDEFID << 16)));
  /* Item helpers. */
  CHECK(GetDlgCtrlID(GetDlgItem(dlg, 104)) == 104);
  CHECK(SetDlgItemTextA(dlg, 101, "Ciuki"));
  char text[32];
  CHECK(GetDlgItemTextA(dlg, 101, text, 32) == 5 && !strcmp(text, "Ciuki"));
  CHECK(SetDlgItemInt(dlg, 102, (UINT)-42, 1));
  BOOL ok = 0;
  CHECK((int)GetDlgItemInt(dlg, 102, &ok, 1) == -42 && ok);
  GetDlgItemInt(dlg, 102, &ok, 0);
  CHECK(!ok);
  SetDlgItemTextA(dlg, 102, "4294967296");
  GetDlgItemInt(dlg, 102, &ok, 0);
  CHECK(!ok);
  SetDlgItemTextA(dlg, 102, " 4294967295 ");
  CHECK(GetDlgItemInt(dlg, 102, &ok, 0) == 4294967295u && ok);
  CHECK(CheckRadioButton(dlg, 103, 105, 105));
  CHECK(IsDlgButtonChecked(dlg, 103) == 0 && IsDlgButtonChecked(dlg, 105) == 1);
  CHECK(CheckDlgButton(dlg, 106, BST_CHECKED) && IsDlgButtonChecked(dlg, 106));
  CHECK(!GetDlgItem(dlg, 999) && GetLastError() == 1421);
  /* DWL_* storage and WM_COMMAND/WM_CTLCOLOR routing to the procedure. */
  CHECK(SetWindowLongA(dlg, DWL_USER, 1234) == 0 &&
        GetWindowLongA(dlg, DWL_USER) == 1234);
  SetWindowLongA(dlg, DWL_MSGRESULT, 77);
  CHECK(GetWindowLongA(dlg, DWL_MSGRESULT) == 77);
  command_count = 0;
  SendDlgItemMessageA(dlg, 107, BM_CLICK, 0, 0);
  CHECK(got_command(107, BN_CLICKED) && IsDlgButtonChecked(dlg, 107));
  ctlcolor_static = 0;
  InvalidateRect(dlg, 0, 1);
  drain(dlg);
  CHECK(ctlcolor_static >= 2);
  DestroyWindow(dlg);
  /* WM_INITDIALOG returning FALSE keeps the focus the procedure chose. */
  init_focus_rule = 1;
  dlg = CreateDialogParamA(FIXTURE, (const char *)(uintptr_t)100, owner,
                           record_proc, 0);
  init_focus_rule = 0;
  CHECK(dlg && GetFocus() == init_focus_target);
  DestroyWindow(dlg);
  /* No DS_SETFONT: System font units (8x16). */
  dlg = CreateDialogParamA(FIXTURE, (const char *)(uintptr_t)102, owner,
                           record_proc, 0);
  CHECK(dlg);
  r = (RECT){0, 0, 4, 8};
  CHECK(MapDialogRect(dlg, &r) && r.right == 8 && r.bottom == 16);
  GetClientRect(dlg, &client);
  CHECK(client.right == du_x(160, 8) && client.bottom == du_y(60, 16));
  DestroyWindow(dlg);
  /* DS_CENTER in the work area, at 640x480 and 800x600. */
  for (UINT mode = 0; mode < 2; mode++) {
    u32_screen_w = mode ? 800 : 640;
    u32_screen_h = mode ? 600 : 480;
    dlg = CreateDialogParamA(FIXTURE, (const char *)(uintptr_t)101, owner,
                             record_proc, 0);
    CHECK(dlg);
    GetWindowRect(dlg, &window);
    RECT work;
    SystemParametersInfoA(48, 0, &work, 0);
    int w = window.right - window.left, h = window.bottom - window.top;
    CHECK(window.left == work.left + (work.right - work.left - w) / 2);
    CHECK(window.top == work.top + (work.bottom - work.top - h) / 2);
    DestroyWindow(dlg);
  }
  u32_screen_w = 640;
  u32_screen_h = 480;
  /* Indirect creation, hostile size limit and the explicit-length entry. */
  DWORD n;
  const uint8_t *d = resource(5, 100, &n);
  dlg = CreateDialogIndirectParamA(FIXTURE, (const DLGTEMPLATE *)d, owner,
                                   record_proc, 0);
  CHECK(dlg && GetDlgItem(dlg, 107));
  DestroyWindow(dlg);
  CHECK(!w32_create_dialog(FIXTURE, d, 20, owner, record_proc, 0) &&
        GetLastError() == 1812);
  dlg = w32_create_dialog(FIXTURE, d, n, owner, record_proc, 0);
  CHECK(dlg);
  DestroyWindow(dlg);
  /* DS_CONTROL child page inside a WS_EX_CONTROLPARENT tab order. */
  HWND outer = CreateDialogParamA(FIXTURE, (const char *)(uintptr_t)110, owner,
                                  record_proc, 0);
  CHECK(outer);
  HWND page = CreateDialogParamA(FIXTURE, (const char *)(uintptr_t)103, outer,
                                 record_proc, 0);
  CHECK(page && (GetWindowLongA(page, GWL_STYLE) & WS_CHILD) &&
        (GetWindowLongA(page, GWL_EXSTYLE) & WS_EX_CONTROLPARENT));
  HWND edit = GetDlgItem(page, 301), browse = GetDlgItem(page, 302);
  HWND next = GetNextDlgTabItem(outer, edit, 0);
  CHECK(next == browse);
  HWND at = edit;
  BOOL saw_back = 0;
  for (UINT i = 0; i < 8; i++) {
    at = GetNextDlgTabItem(outer, at, 0);
    saw_back |= GetDlgCtrlID(at) == 3;
  }
  CHECK(saw_back);
  CHECK(GetNextDlgTabItem(outer, browse, 1) == edit);
  DestroyWindow(outer);
  CHECK(!IsWindow(page));
  /* RT_BITMAP resources and .BMP files through LoadBitmap/LoadImage, shown
   * by an SS_BITMAP static. */
  const struct u32_graphics *gr = u32_graphics();
  HANDLE bitmap = LoadBitmapA(FIXTURE, (const char *)(uintptr_t)400);
  CHECK(bitmap);
  uint8_t info[64];
  CHECK(gr->object(U32_PTR(bitmap), sizeof(info), info) &&
        w32_u32(info + 4) == 4 && w32_u32(info + 8) == 4);
  DWORD dibn;
  const uint8_t *dib = resource(2, 400, &dibn);
  char bmp[1024];
  CHECK(host_path("C:\\Program Files\\Sample\\CHECK.BMP", bmp, sizeof(bmp), 1));
  FILE *f = fopen(bmp, "wb");
  uint8_t header[14] = {'B', 'M'};
  w32_put32(header + 2, 14 + dibn);
  w32_put32(header + 10, 14 + 40);
  CHECK(f && fwrite(header, 1, 14, f) == 14 && fwrite(dib, 1, dibn, f) == dibn);
  fclose(f);
  HANDLE file = LoadImageA(0, "C:\\Program Files\\Sample\\CHECK.BMP", 0, 0, 0,
                           0x10);
  CHECK(file && LoadImageA(FIXTURE, (const char *)(uintptr_t)32516, 1, 0, 0, 0) == 0);
  CHECK(LoadImageA(0, (const char *)(uintptr_t)32516, 1, 0, 0, 0));
  HWND picture = CreateWindowExA(0, "STATIC", "", WS_CHILD | WS_VISIBLE | SS_BITMAP,
                                 4, 4, 8, 8, owner, (HANDLE)77, FIXTURE, 0);
  SendMessageA(picture, STM_SETIMAGE, 0, (LPARAM)bitmap);
  drain(0);
  struct u32_window *ow = u32_window(owner);
  POINT so = u32_surface_origin(ow), po = u32_origin(u32_window(picture));
  uint32_t corner = ((uint32_t *)((uint8_t *)ow->surface.pixels +
                                  (po.y - so.y) * ow->surface.pitch))[po.x - so.x];
  uint32_t neighbour = ((uint32_t *)((uint8_t *)ow->surface.pixels +
                                (po.y - so.y) * ow->surface.pitch))[po.x - so.x + 1];
  CHECK((corner & 0xffffff) != (neighbour & 0xffffff));
  gr->delete_object(U32_PTR(bitmap));
  gr->delete_object(U32_PTR(file));
  DestroyWindow(owner);
}

/* ---- keyboard rules of IsDialogMessage in a modal loop -------------------- */
static HWND keyboard_owner;
static void keyboard_script(HWND dlg, UINT why, UINT step) {
  CHECK(why == MSGF_DIALOGBOX);
  CHECK(!IsWindowEnabled(keyboard_owner));
  HWND focus = GetFocus();
  switch (step) {
  case 0:
    CHECK(focus == GetDlgItem(dlg, 101));
    key(VK_TAB);
    break;
  case 1:
    CHECK(focus == GetDlgItem(dlg, 102));
    key(VK_TAB);
    break;
  case 2:
    /* The group's first control (checked radio) receives the tab. */
    CHECK(GetDlgCtrlID(focus) == 103);
    key(VK_DOWN);
    break;
  case 3:
    CHECK(GetDlgCtrlID(focus) == 104 && IsDlgButtonChecked(dlg, 104) &&
          !IsDlgButtonChecked(dlg, 103));
    key(VK_DOWN);
    key(VK_DOWN);
    break;
  case 4:
    CHECK(GetDlgCtrlID(focus) == 103 && IsDlgButtonChecked(dlg, 103));
    key(VK_UP);
    break;
  case 5:
    CHECK(GetDlgCtrlID(focus) == 105);
    key(VK_TAB);
    break;
  case 6:
    CHECK(GetDlgCtrlID(focus) == 106);
    command_count = 0;
    key(VK_SPACE);
    break;
  case 7:
    CHECK(IsDlgButtonChecked(dlg, 106) && got_command(106, BN_CLICKED));
    /* Tab to Cancel: the default style moves to the focused push button. */
    for (HWND h = focus; GetDlgCtrlID(h) != IDCANCEL;) {
      h = GetNextDlgTabItem(dlg, h, 0);
      key(VK_TAB);
      if (GetDlgCtrlID(h) == IDCANCEL)
        break;
    }
    break;
  case 8:
    CHECK(GetDlgCtrlID(focus) == IDCANCEL);
    CHECK((GetWindowLongA(focus, GWL_STYLE) & 15) == BS_DEFPUSHBUTTON);
    CHECK((GetWindowLongA(GetDlgItem(dlg, IDOK), GWL_STYLE) & 15) ==
          BS_PUSHBUTTON);
    key_with(VK_SHIFT, VK_TAB);
    break;
  case 9:
    CHECK(GetDlgCtrlID(focus) == IDOK);
    key_with(VK_SHIFT, VK_TAB);
    break;
  case 10:
    /* Off the push buttons the dialog's default id is restored. */
    CHECK(GetDlgCtrlID(focus) == 107);
    CHECK((GetWindowLongA(GetDlgItem(dlg, IDOK), GWL_STYLE) & 15) ==
          BS_DEFPUSHBUTTON);
    CHECK((GetWindowLongA(GetDlgItem(dlg, IDCANCEL), GWL_STYLE) & 15) ==
          BS_PUSHBUTTON);
    chr('c');
    break;
  case 11:
    /* Plain character mnemonic on a control that wants no characters. */
    CHECK(GetDlgCtrlID(focus) == 104 && IsDlgButtonChecked(dlg, 104));
    syschr('s');
    break;
  case 12:
    CHECK(GetDlgCtrlID(focus) == 106 && !IsDlgButtonChecked(dlg, 106));
    syschr('f');
    break;
  case 13:
    /* A label's mnemonic focuses the next control in tab order. */
    CHECK(focus == GetDlgItem(dlg, 102));
    SendMessageA(focus, EM_SETSEL, 0, -1);
    chr('7');
    break;
  case 14: {
    char text[16];
    GetDlgItemTextA(dlg, 102, text, 16);
    CHECK(!strcmp(text, "7"));
    command_count = 0;
    key(VK_RETURN);
    break;
  }
  default:
    CHECK(!"keyboard script overran");
  }
}
static void escape_script(HWND dlg, UINT why, UINT step) {
  (void)why;
  if (step == 0)
    key(VK_ESCAPE);
  else
    CHECK(!IsWindow(dlg) || !"escape ignored");
}
static void defid_script(HWND dlg, UINT why, UINT step) {
  (void)why;
  if (step == 0) {
    CHECK(SendMessageA(dlg, DM_SETDEFID, IDCANCEL, 0));
    CHECK(SendMessageA(dlg, DM_GETDEFID, 0, 0) ==
          (LRESULT)(IDCANCEL | (DC_HASDEFID << 16)));
    CHECK((GetWindowLongA(GetDlgItem(dlg, IDCANCEL), GWL_STYLE) & 15) ==
          BS_DEFPUSHBUTTON);
    key(VK_RETURN);
  } else
    CHECK(!"enter ignored");
}
static void test_keyboard(void) {
  keyboard_owner = owner_window("Owner", WS_CAPTION | 0x80000u, 600, 420);
  script(keyboard_script);
  intptr_t r = DialogBoxParamA(FIXTURE, (const char *)(uintptr_t)100,
                               keyboard_owner, record_proc, 0);
  CHECK(r == 42 && got_command(IDOK, BN_CLICKED));
  CHECK(IsWindowEnabled(keyboard_owner) && seen(WM_ENTERIDLE));
  script(escape_script);
  r = DialogBoxParamA(FIXTURE, (const char *)(uintptr_t)100, keyboard_owner,
                      record_proc, 0);
  CHECK(r == IDCANCEL);
  script(defid_script);
  r = DialogBoxParamA(FIXTURE, (const char *)(uintptr_t)100, keyboard_owner,
                      record_proc, 0);
  CHECK(r == IDCANCEL);
  /* A missing template fails with -1. */
  CHECK(DialogBoxParamA(FIXTURE, (const char *)(uintptr_t)999, keyboard_owner,
                        record_proc, 0) == -1);
  script(0);
  DestroyWindow(keyboard_owner);
}

/* ---- EDIT ------------------------------------------------------------- */
static UINT edit_changes, edit_maxtext;
static LRESULT W32API edit_parent(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  if (m == WM_COMMAND && (wp >> 16) == EN_CHANGE)
    edit_changes++;
  if (m == WM_COMMAND && (wp >> 16) == EN_MAXTEXT)
    edit_maxtext++;
  return DefWindowProcA(h, m, wp, lp);
}
static HWND make_edit(HWND parent, DWORD style, int id, int w, int h) {
  HWND e = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                           WS_CHILD | WS_VISIBLE | style, 8, 8 + (id % 10) * 30,
                           w, h, parent, (HANDLE)(uintptr_t)id, FIXTURE, 0);
  CHECK(e);
  return e;
}
static void type(HWND e, const char *s) {
  for (; *s; s++)
    SendMessageA(e, WM_CHAR, (uint8_t)*s, 1);
}
static void test_edit(void) {
  WNDCLASSA c = {.lpfnWndProc = edit_parent,
                 .hbrBackground = (HBRUSH)(uintptr_t)(COLOR_BTNFACE + 1),
                 .lpszClassName = "Edit Parent"};
  CHECK(RegisterClassA(&c));
  HWND parent = CreateWindowExA(0, "Edit Parent", "Edits",
                                WS_CAPTION | WS_VISIBLE, 20, 40, 400, 360, 0, 0,
                                FIXTURE, 0);
  CHECK(parent);
  const struct u32_graphics *g = u32_graphics();
  struct u32_logfont lf = {0};
  lf.height = -11;
  strcpy(lf.face, "MS Sans Serif");
  HFONT font = (HFONT)(uintptr_t)g->font_indirect(&lf);
  const struct u32_font_info *f = u32_font_info(font);
  CHECK(f->measured && f->height == 13);
  HWND e = make_edit(parent, ES_AUTOHSCROLL | WS_TABSTOP, 1, 200, 21);
  SendMessageA(e, WM_SETFONT, (WPARAM)font, 0);
  SetFocus(e);
  edit_changes = 0;
  type(e, "Hello");
  char text[256];
  GetWindowTextA(e, text, sizeof(text));
  CHECK(!strcmp(text, "Hello") && edit_changes == 5);
  POINT caret;
  CHECK(GetCaretPos(&caret));
  RECT format;
  SendMessageA(e, EM_GETRECT, 0, (LPARAM)&format);
  CHECK(caret.x == format.left + u32_text_width(f, "Hello", 5));
  CHECK(caret.y == format.top);
  /* Shift+Left selects; typing replaces; one-step undo. */
  uint8_t keys[256] = {0};
  keys[VK_SHIFT] = 0x80;
  SetKeyboardState(keys);
  SendMessageA(e, WM_KEYDOWN, VK_LEFT, 1);
  SendMessageA(e, WM_KEYDOWN, VK_LEFT, 1);
  keys[VK_SHIFT] = 0;
  SetKeyboardState(keys);
  DWORD a = 0, b = 0;
  SendMessageA(e, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
  CHECK(a == 3 && b == 5);
  type(e, "p!");
  GetWindowTextA(e, text, sizeof(text));
  CHECK(!strcmp(text, "Help!"));
  CHECK(SendMessageA(e, EM_CANUNDO, 0, 0));
  SendMessageA(e, EM_UNDO, 0, 0);
  GetWindowTextA(e, text, sizeof(text));
  CHECK(!strcmp(text, "Help"));
  /* Home/End, Delete, Backspace and the clipboard. */
  SendMessageA(e, WM_KEYDOWN, VK_HOME, 1);
  SendMessageA(e, WM_KEYDOWN, VK_DELETE, 1);
  SendMessageA(e, WM_KEYDOWN, VK_END, 1);
  SendMessageA(e, WM_CHAR, VK_BACK, 1);
  GetWindowTextA(e, text, sizeof(text));
  CHECK(!strcmp(text, "el"));
  SendMessageA(e, EM_SETSEL, 0, -1);
  SendMessageA(e, 0x301, 0, 0);
  SendMessageA(e, EM_SETSEL, 2, 2);
  SendMessageA(e, 0x302, 0, 0);
  GetWindowTextA(e, text, sizeof(text));
  CHECK(!strcmp(text, "elel"));
  /* EM_LIMITTEXT and EN_MAXTEXT. */
  SendMessageA(e, EM_LIMITTEXT, 6, 0);
  edit_maxtext = 0;
  SendMessageA(e, EM_SETSEL, 4, 4);
  type(e, "xyz");
  GetWindowTextA(e, text, sizeof(text));
  CHECK(!strcmp(text, "elelxy") && edit_maxtext == 1);
  CHECK(SendMessageA(e, EM_GETLIMITTEXT, 0, 0) == 6);
  /* Password, read-only and number edits. */
  HWND pass = make_edit(parent, ES_PASSWORD | ES_AUTOHSCROLL, 2, 200, 21);
  type(pass, "secret");
  GetWindowTextA(pass, text, sizeof(text));
  CHECK(!strcmp(text, "secret") && SendMessageA(pass, EM_GETPASSWORDCHAR, 0, 0) == '*');
  HWND ro = make_edit(parent, ES_READONLY | ES_AUTOHSCROLL, 3, 200, 21);
  SetWindowTextA(ro, "fixed");
  type(ro, "zz");
  GetWindowTextA(ro, text, sizeof(text));
  CHECK(!strcmp(text, "fixed"));
  SendMessageA(ro, EM_SETREADONLY, 0, 0);
  type(ro, "!");
  HWND number = make_edit(parent, ES_NUMBER | ES_AUTOHSCROLL, 4, 200, 21);
  type(number, "a1b2");
  GetWindowTextA(number, text, sizeof(text));
  CHECK(!strcmp(text, "12"));
  /* Without ES_AUTOHSCROLL a single-line edit refuses text that does not fit. */
  HWND narrow = make_edit(parent, 0, 5, 40, 21);
  SendMessageA(narrow, WM_SETFONT, (WPARAM)font, 0);
  type(narrow, "abcdefghijklmnop");
  CHECK(GetWindowTextLengthA(narrow) < 16 && GetWindowTextLengthA(narrow) > 2);
  /* Multi-line: line arithmetic, CR LF, scrolling. */
  HWND multi = make_edit(parent, ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN |
                                     WS_VSCROLL,
                         6, 200, 60);
  SendMessageA(multi, WM_SETFONT, (WPARAM)font, 0);
  SetWindowTextA(multi, "line one\r\nline two\r\nthird");
  CHECK(SendMessageA(multi, EM_GETLINECOUNT, 0, 0) == 3);
  CHECK(SendMessageA(multi, EM_LINEINDEX, 1, 0) == 10);
  CHECK(SendMessageA(multi, EM_LINELENGTH, 10, 0) == 8);
  CHECK(SendMessageA(multi, EM_LINEFROMCHAR, 21, 0) == 2);
  char line[16];
  w32_put16(line, sizeof(line));
  CHECK(SendMessageA(multi, EM_GETLINE, 2, (LPARAM)line) == 5);
  line[5] = 0;
  CHECK(!strcmp(line, "third"));
  SendMessageA(multi, EM_SETSEL, 26, 26);
  SendMessageA(multi, WM_CHAR, '\r', 1);
  type(multi, "4");
  for (UINT i = 5; i < 12; i++) {
    SendMessageA(multi, WM_CHAR, '\r', 1);
    SendMessageA(multi, WM_CHAR, '0' + i % 10, 1);
  }
  CHECK(SendMessageA(multi, EM_GETLINECOUNT, 0, 0) == 11);
  CHECK(SendMessageA(multi, EM_GETFIRSTVISIBLELINE, 0, 0) > 0);
  SCROLLINFO s = {sizeof(s), SIF_ALL, 0, 0, 0, 0, 0};
  CHECK(GetScrollInfo(multi, SB_VERT, &s) && s.nMax == 10 && s.nPage >= 3);
  RECT mc;
  GetClientRect(multi, &mc);
  CHECK(mc.right == 200 - 4 - 16);
  /* Caret follows focus. */
  SetFocus(multi);
  CHECK(GetCaretPos(&caret));
  SetFocus(e);
  drain(0);
  DestroyWindow(parent);
  g->delete_object(U32_PTR(font));
}

/* ---- LISTBOX / COMBOBOX / SCROLLBAR ------------------------------------ */
static UINT list_notifications[64], list_notification_count, drawitems,
    measured;
static LRESULT W32API list_parent(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  if (m == WM_COMMAND && list_notification_count < 64)
    list_notifications[list_notification_count++] = (UINT)wp;
  if (m == WM_DRAWITEM)
    drawitems++;
  if (m == WM_MEASUREITEM) {
    MEASUREITEMSTRUCT *mi = (MEASUREITEMSTRUCT *)lp;
    mi->itemHeight = 20 + mi->itemID % 3;
    measured++;
    return 1;
  }
  if (m == WM_HSCROLL && list_notification_count < 64)
    list_notifications[list_notification_count++] = 0xa0000u | (UINT)(wp & 0xffff);
  return DefWindowProcA(h, m, wp, lp);
}
static BOOL notified(UINT id, UINT code) {
  for (UINT i = 0; i < list_notification_count; i++)
    if (list_notifications[i] == (id | (code << 16)))
      return 1;
  return 0;
}
static void test_lists(void) {
  WNDCLASSA c = {.lpfnWndProc = list_parent,
                 .hbrBackground = (HBRUSH)(uintptr_t)(COLOR_BTNFACE + 1),
                 .lpszClassName = "List Parent"};
  CHECK(RegisterClassA(&c));
  HWND parent = CreateWindowExA(0, "List Parent", "Lists",
                                WS_CAPTION | WS_VISIBLE, 20, 40, 500, 400, 0, 0,
                                FIXTURE, 0);
  HWND list = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", "",
                              WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY |
                                  LBS_SORT,
                              8, 8, 150, 100, parent, (HANDLE)10, FIXTURE, 0);
  CHECK(list);
  static const char *const fruit[] = {"pear", "apple", "fig", "Banana"};
  for (UINT i = 0; i < 4; i++)
    SendMessageA(list, LB_ADDSTRING, 0, (LPARAM)fruit[i]);
  char text[32];
  SendMessageA(list, LB_GETTEXT, 0, (LPARAM)text);
  CHECK(!strcmp(text, "apple"));
  SendMessageA(list, LB_GETTEXT, 1, (LPARAM)text);
  CHECK(!strcmp(text, "Banana"));
  CHECK(SendMessageA(list, LB_FINDSTRING, (WPARAM)-1, (LPARAM) "f") == 2);
  CHECK(SendMessageA(list, LB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM) "PEAR") == 3);
  CHECK(SendMessageA(list, LB_SELECTSTRING, (WPARAM)-1, (LPARAM) "ba") == 1);
  CHECK(SendMessageA(list, LB_GETCURSEL, 0, 0) == 1);
  list_notification_count = 0;
  SetFocus(list);
  SendMessageA(list, WM_KEYDOWN, VK_DOWN, 1);
  CHECK(SendMessageA(list, LB_GETCURSEL, 0, 0) == 2 &&
        notified(10, LBN_SELCHANGE));
  SendMessageA(list, WM_CHAR, 'p', 1);
  CHECK(SendMessageA(list, LB_GETCURSEL, 0, 0) == 3);
  for (UINT i = 0; i < 30; i++) {
    snprintf(text, sizeof(text), "item %02u", i);
    SendMessageA(list, LB_ADDSTRING, 0, (LPARAM)text);
  }
  SCROLLINFO s = {sizeof(s), SIF_ALL, 0, 0, 0, 0, 0};
  CHECK(GetScrollInfo(list, SB_VERT, &s) && s.nMax == 33);
  RECT client;
  GetClientRect(list, &client);
  CHECK(client.right == 150 - 4 - 16);
  CHECK(s.nPage == (UINT)(client.bottom / SendMessageA(list, LB_GETITEMHEIGHT, 0, 0)));
  SendMessageA(list, WM_VSCROLL, SB_PAGEDOWN, 0);
  CHECK(SendMessageA(list, LB_GETTOPINDEX, 0, 0) == (LRESULT)s.nPage);
  CHECK(SendMessageA(list, LB_DELETESTRING, 0, 0) == 33);
  /* Extended selection with Shift and Ctrl clicks. */
  HWND multi = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", "",
                               WS_CHILD | WS_VISIBLE | LBS_EXTENDEDSEL |
                                   LBS_NOTIFY,
                               170, 8, 150, 120, parent, (HANDLE)11, FIXTURE,
                               0);
  for (UINT i = 0; i < 6; i++) {
    snprintf(text, sizeof(text), "row %u", i);
    SendMessageA(multi, LB_ADDSTRING, 0, (LPARAM)text);
  }
  int h = (int)SendMessageA(multi, LB_GETITEMHEIGHT, 0, 0);
  SendMessageA(multi, WM_LBUTTONDOWN, 1, (LPARAM)((h * 1 + 2) << 16));
  SendMessageA(multi, WM_LBUTTONUP, 0, (LPARAM)((h * 1 + 2) << 16));
  SendMessageA(multi, WM_LBUTTONDOWN, 1 | 4, (LPARAM)((h * 3 + 2) << 16));
  SendMessageA(multi, WM_LBUTTONUP, 0, (LPARAM)((h * 3 + 2) << 16));
  SendMessageA(multi, WM_LBUTTONDOWN, 1 | 8, (LPARAM)((h * 5 + 2) << 16));
  SendMessageA(multi, WM_LBUTTONUP, 0, (LPARAM)((h * 5 + 2) << 16));
  int items[8];
  CHECK(SendMessageA(multi, LB_GETSELCOUNT, 0, 0) == 4);
  CHECK(SendMessageA(multi, LB_GETSELITEMS, 8, (LPARAM)items) == 4 &&
        items[0] == 1 && items[2] == 3 && items[3] == 5);
  CHECK(SendMessageA(multi, LB_SETCURSEL, 0, 0) == LB_ERR);
  /* Owner-drawn fixed and variable lists. */
  drawitems = measured = 0;
  HWND owner = CreateWindowExA(0, "LISTBOX", "",
                               WS_CHILD | WS_VISIBLE | LBS_OWNERDRAWVARIABLE |
                                   LBS_HASSTRINGS,
                               330, 8, 150, 120, parent, (HANDLE)12, FIXTURE, 0);
  for (UINT i = 0; i < 3; i++)
    SendMessageA(owner, LB_ADDSTRING, 0, (LPARAM) "drawn");
  CHECK(measured == 3 && SendMessageA(owner, LB_GETITEMHEIGHT, 1, 0) == 21);
  drain(0);
  CHECK(drawitems >= 3);
  /* Drop-down list combo box: popup list window, keyboard, notifications. */
  HWND combo = CreateWindowExA(0, "COMBOBOX", "",
                               WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST |
                                   WS_VSCROLL,
                               8, 140, 150, 120, parent, (HANDLE)13, FIXTURE, 0);
  CHECK(combo);
  RECT field;
  GetWindowRect(combo, &field);
  CHECK(field.bottom - field.top == u32_font_info(0)->height + 8);
  for (UINT i = 0; i < 5; i++) {
    snprintf(text, sizeof(text), "Choice %u", i);
    CHECK(SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)text) == (LRESULT)i);
  }
  CHECK(SendMessageA(combo, CB_SETCURSEL, 1, 0) == 1);
  list_notification_count = 0;
  SetFocus(combo);
  CHECK(SendMessageA(combo, CB_SHOWDROPDOWN, 1, 0));
  CHECK(SendMessageA(combo, CB_GETDROPPEDSTATE, 0, 0) == 1 &&
        notified(13, CBN_DROPDOWN));
  HWND popup = FindWindowA("ComboLBox", 0);
  CHECK(popup && IsWindowVisible(popup) &&
        !(GetWindowLongA(popup, GWL_STYLE) & WS_CHILD));
  drain(0);
  SendMessageA(combo, WM_KEYDOWN, VK_DOWN, 1);
  SendMessageA(combo, WM_KEYDOWN, VK_RETURN, 1);
  CHECK(!SendMessageA(combo, CB_GETDROPPEDSTATE, 0, 0) && !IsWindow(popup));
  CHECK(SendMessageA(combo, CB_GETCURSEL, 0, 0) == 2);
  CHECK(notified(13, CBN_SELENDOK) && notified(13, CBN_SELCHANGE) &&
        notified(13, CBN_CLOSEUP));
  GetWindowTextA(combo, text, sizeof(text));
  CHECK(!strcmp(text, "Choice 2"));
  SendMessageA(combo, WM_KEYDOWN, VK_DOWN, 1);
  CHECK(SendMessageA(combo, CB_GETCURSEL, 0, 0) == 3);
  /* Drop-down with an edit field and a simple combo box. */
  HWND dropdown = CreateWindowExA(0, "COMBOBOX", "",
                                  WS_CHILD | WS_VISIBLE | CBS_DROPDOWN,
                                  170, 140, 150, 120, parent, (HANDLE)14,
                                  FIXTURE, 0);
  SendMessageA(dropdown, CB_ADDSTRING, 0, (LPARAM) "alpha");
  SendMessageA(dropdown, CB_ADDSTRING, 0, (LPARAM) "beta");
  SendMessageA(dropdown, CB_SETCURSEL, 1, 0);
  GetWindowTextA(dropdown, text, sizeof(text));
  CHECK(!strcmp(text, "beta"));
  SetWindowTextA(dropdown, "gamma");
  GetWindowTextA(dropdown, text, sizeof(text));
  CHECK(!strcmp(text, "gamma") && GetDlgItem(dropdown, 1001));
  HWND simple = CreateWindowExA(0, "COMBOBOX", "",
                                WS_CHILD | WS_VISIBLE | CBS_SIMPLE, 330, 140,
                                150, 100, parent, (HANDLE)15, FIXTURE, 0);
  CHECK(SendMessageA(simple, CB_SHOWDROPDOWN, 1, 0) &&
        !SendMessageA(simple, CB_GETDROPPEDSTATE, 0, 0));
  /* SCROLLBAR control and window scroll bars. */
  HWND bar = CreateWindowExA(0, "SCROLLBAR", "", WS_CHILD | WS_VISIBLE | SBS_HORZ,
                             8, 280, 200, 16, parent, (HANDLE)16, FIXTURE, 0);
  SCROLLINFO info = {sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS, 0, 100, 10,
                     500, 0};
  CHECK(SendMessageA(bar, SBM_SETSCROLLINFO, 1, (LPARAM)&info) == 91);
  CHECK(GetScrollPos(bar, SB_CTL) == 91);
  CHECK(SetScrollPos(bar, SB_CTL, 20, 1) == 91 && GetScrollPos(bar, SB_CTL) == 20);
  list_notification_count = 0;
  SendMessageA(bar, WM_KEYDOWN, VK_RIGHT, 1);
  CHECK(list_notification_count >= 2 && list_notifications[0] == (0xa0000u | SB_LINEDOWN));
  int lo, hi;
  CHECK(GetScrollRange(bar, SB_CTL, &lo, &hi) && lo == 0 && hi == 100);
  RECT before;
  GetClientRect(parent, &before);
  UINT sw = u32_window(parent)->surface.width;
  SCROLLINFO window = {sizeof(window), SIF_RANGE | SIF_PAGE, 0, 999, 100, 0, 0};
  SetScrollInfo(parent, SB_VERT, &window, 1);
  CHECK(GetWindowLongA(parent, GWL_STYLE) & WS_VSCROLL);
  RECT after;
  GetClientRect(parent, &after);
  CHECK(after.right == before.right - 16 &&
        u32_window(parent)->surface.width == sw);
  window.nMax = 50;
  SetScrollInfo(parent, SB_VERT, &window, 1);
  CHECK(!(GetWindowLongA(parent, GWL_STYLE) & WS_VSCROLL));
  window.fMask |= SIF_DISABLENOSCROLL;
  SetScrollInfo(parent, SB_VERT, &window, 1);
  CHECK(GetWindowLongA(parent, GWL_STYLE) & WS_VSCROLL);
  CHECK(EnableScrollBar(parent, SB_VERT, ESB_DISABLE_BOTH));
  CHECK(ShowScrollBar(parent, SB_VERT, 0) &&
        !(GetWindowLongA(parent, GWL_STYLE) & WS_VSCROLL));
  drain(0);
  DestroyWindow(parent);
}

/* ---- message boxes ---------------------------------------------------------- */
static HWND box_owner;
static UINT box_kind;
static void box_script(HWND dlg, UINT why, UINT step) {
  CHECK(why == MSGF_DIALOGBOX && dlg);
  if (box_kind == 0) {
    CHECK(step == 0);
    CHECK(GetDlgItem(dlg, IDYES) && GetDlgItem(dlg, IDNO) &&
          GetDlgItem(dlg, IDCANCEL) && GetDlgItem(dlg, 0x14));
    CHECK(GetFocus() == GetDlgItem(dlg, IDNO));
    CHECK((GetWindowLongA(GetDlgItem(dlg, IDNO), GWL_STYLE) & 15) ==
          BS_DEFPUSHBUTTON);
    CHECK(!IsWindowEnabled(box_owner));
    char caption[64];
    GetWindowTextA(dlg, caption, sizeof(caption));
    CHECK(!strcmp(caption, "Question"));
    key(VK_RETURN);
  } else if (box_kind == 1) {
    CHECK(step == 0);
    key(VK_ESCAPE);
  } else if (box_kind == 2) {
    if (step == 0)
      key(VK_ESCAPE);
    else if (step == 1) {
      CHECK(IsWindow(dlg));
      chr('y');
    } else
      CHECK(!"mnemonic ignored");
  } else if (box_kind == 3) {
    CHECK(step == 0);
    chr('r');
  } else if (box_kind == 5) {
    /* G15 MB_HELP: the Help button and F1 send WM_HELP to the owner. */
    CHECK(step == 0);
    HWND help = GetDlgItem(dlg, IDHELP);
    CHECK(help && GetDlgItem(dlg, IDOK) && GetDlgItem(dlg, IDCANCEL));
    owner_help_count = 0;
    SendMessageA(dlg, WM_COMMAND, IDHELP | (BN_CLICKED << 16), (LPARAM)help);
    CHECK(IsWindow(dlg) && owner_help_count == 1 && owner_help.iContextType == HELPINFO_WINDOW &&
          owner_help.iCtrlId == IDHELP && owner_help.hItemHandle == help && owner_help.cbSize == sizeof(HELPINFO));
    SendMessageA(GetFocus(), WM_KEYDOWN, VK_F1, 0);
    CHECK(IsWindow(dlg) && owner_help_count == 2);
    key(VK_ESCAPE);
  } else if (box_kind == 4) {
    CHECK(step == 0);
    HWND label = GetDlgItem(dlg, 0xffff);
    CHECK(GetWindowTextLengthA(label) > 300);
    RECT r;
    GetWindowRect(dlg, &r);
    CHECK(r.right - r.left <= (LONG)u32_screen_w * 5 / 8 + 40);
    char caption[16];
    GetWindowTextA(dlg, caption, sizeof(caption));
    CHECK(!strcmp(caption, "Error"));
    CHECK(GetWindowLongA(dlg, GWL_EXSTYLE) & WS_EX_TOPMOST);
    key(VK_ESCAPE);
  }
}
/* An ownerless task-modal box sends no WM_ENTERIDLE; a message for another
 * window, dispatched by the box's loop, drives it instead. */
static LRESULT W32API box_helper(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  if (m == WM_USER + 77) {
    CHECK(!IsWindowEnabled(box_owner));
    key(VK_RETURN);
    return 0;
  }
  return DefWindowProcA(h, m, wp, lp);
}
static void test_msgbox(void) {
  box_owner = owner_window("Owner", WS_CAPTION | 0x80000u, 600, 420);
  script(box_script);
  box_kind = 0;
  CHECK(MessageBoxA(box_owner, "Save changes before closing?", "Question",
                    MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2) == IDNO);
  CHECK(IsWindowEnabled(box_owner));
  box_kind = 1;
  idle_count = 0;
  CHECK(MessageBoxA(box_owner, "Done.", "Setup", MB_OK | MB_ICONASTERISK) ==
        IDOK);
  box_kind = 2;
  idle_count = 0;
  CHECK(MessageBoxA(box_owner, "Overwrite?", "Setup", MB_YESNO) == IDYES);
  box_kind = 3;
  idle_count = 0;
  CHECK(MessageBoxA(box_owner, "Disk error.", "Setup",
                    MB_ABORTRETRYIGNORE | MB_ICONHAND) == IDRETRY);
  box_kind = 4;
  idle_count = 0;
  char text[600];
  for (UINT i = 0; i < 599; i++)
    text[i] = i % 7 == 6 ? ' ' : 'a' + i % 26;
  text[599] = 0;
  CHECK(MessageBoxExA(box_owner, text, 0,
                      MB_OKCANCEL | MB_ICONEXCLAMATION | MB_SYSTEMMODAL,
                      0) == IDCANCEL);
  /* G15: without MB_HELP, F1 in a box reaches nobody. */
  box_kind = 5;
  idle_count = 0;
  CHECK(MessageBoxA(box_owner, "Help me.", "Setup", MB_OKCANCEL | MB_HELP) == IDCANCEL);
  CHECK(owner_help_count == 2);
  /* Task-modal without an owner disables the thread's top-level windows. */
  HWND other = owner_window("Second", WS_CAPTION, 200, 100);
  box_owner = other;
  WNDCLASSA hc = {.lpfnWndProc = box_helper, .lpszClassName = "Box Helper"};
  CHECK(RegisterClassA(&hc));
  HWND helper = CreateWindowExA(0, "Box Helper", "", 0, 0, 0, 10, 10, 0, 0,
                                FIXTURE, 0);
  CHECK(helper && PostMessageA(helper, WM_USER + 77, 0, 0));
  CHECK(MessageBoxA(0, "Task modal", "Setup", MB_OK | MB_TASKMODAL) == IDOK);
  DestroyWindow(helper);
  CHECK(IsWindowEnabled(other));
  script(0);
  DestroyWindow(other);
}

/* ---- installer-style pages (authored; not NSIS) --------------------------- */
static HWND pages[3];
static UINT page_at;
static intptr_t W32API page_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  (void)lp;
  if (m == WM_INITDIALOG)
    return 0;
  if (m == WM_COMMAND && (wp & 0xffff) == 1034) {
    HWND outer = GetParent(h);
    EnableWindow(GetDlgItem(outer, IDOK),
                 IsDlgButtonChecked(h, 1034) == BST_CHECKED);
    return 1;
  }
  return 0;
}
static void show_page(HWND outer, UINT at) {
  for (UINT i = 0; i < 3; i++)
    if (pages[i])
      ShowWindow(pages[i], i == at ? 5 : 0);
  page_at = at;
  static const char *const titles[][2] = {
      {"License Agreement", "Please review the license terms."},
      {"Choose Install Location", "Choose the folder in which to install Ciuki Sample."},
      {"Installing", "Install actions are stubbed in this fixture (f5-15)."}};
  SetDlgItemTextA(outer, 1037, titles[at][0]);
  SetDlgItemTextA(outer, 1038, titles[at][1]);
  SetDlgItemTextA(outer, IDOK, at == 2 ? "&Finish" : at == 1 ? "&Install" : "&Next >");
  EnableWindow(GetDlgItem(outer, 3), at > 0);
  EnableWindow(GetDlgItem(outer, IDOK),
               at != 0 || IsDlgButtonChecked(pages[0], 1034) == BST_CHECKED);
  HWND first = GetNextDlgTabItem(pages[at], 0, 0);
  u32_dialog_focus(outer, first ? first : GetDlgItem(outer, IDOK));
}
static intptr_t W32API outer_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  (void)lp;
  if (m == WM_INITDIALOG) {
    RECT r = {15, 35, 0, 0};
    MapDialogRect(h, &r);
    pages[0] = CreateDialogParamA(FIXTURE, (const char *)(uintptr_t)111, h,
                                  page_proc, 0);
    pages[1] = CreateDialogParamA(FIXTURE, (const char *)(uintptr_t)103, h,
                                  page_proc, 0);
    pages[2] = 0;
    for (UINT i = 0; i < 2; i++)
      SetWindowPos(pages[i], 0, r.left, r.top, 0, 0, 0x15);
    SetDlgItemTextA(pages[0], 1000,
                    "CIUKI SAMPLE LICENSE\r\n\r\nThis fixture authors an "
                    "installer-style page sequence for the dialog manager. "
                    "It is not the NSIS installer of the census.");
    SetDlgItemTextA(pages[1], 301, "C:\\Program Files\\Ciuki Sample");
    show_page(h, 0);
    return 0;
  }
  if (m == WM_COMMAND && (wp >> 16) == BN_CLICKED) {
    UINT id = (UINT)(wp & 0xffff);
    if (id == IDCANCEL) {
      EndDialog(h, IDCANCEL);
      return 1;
    }
    if (id == 3 && page_at > 0) {
      show_page(h, page_at - 1);
      return 1;
    }
    if (id == IDOK) {
      if (page_at == 1) {
        RECT r = {15, 35, 236, 90};
        MapDialogRect(h, &r);
        pages[2] = CreateWindowExA(0, "STATIC", "Extracting files (stubbed).",
                                   WS_CHILD, r.left, r.top, r.right, r.bottom,
                                   h, (HANDLE)(uintptr_t)1100, FIXTURE, 0);
        if (pages[2])
          SendMessageA(pages[2], WM_SETFONT,
                       SendMessageA(h, WM_GETFONT, 0, 0), 0);
        show_page(h, 2);
        return 1;
      }
      if (page_at == 2) {
        EndDialog(h, IDOK);
        return 1;
      }
      show_page(h, page_at + 1);
      return 1;
    }
  }
  return 0;
}
static char chosen_folder[260];
static void installer_script(HWND dlg, UINT why, UINT step) {
  (void)why;
  switch (step) {
  case 0:
    CHECK(page_at == 0 && !IsWindowEnabled(GetDlgItem(dlg, IDOK)));
    CHECK(IsChild(dlg, GetFocus()));
    key(VK_TAB);
    break;
  case 1:
    CHECK(GetDlgCtrlID(GetFocus()) == 1034);
    key(VK_SPACE);
    break;
  case 2:
    CHECK(IsWindowEnabled(GetDlgItem(dlg, IDOK)));
    key(VK_RETURN);
    break;
  case 3:
    CHECK(page_at == 1 && GetFocus() == GetDlgItem(pages[1], 301));
    chr('X');
    break;
  case 4:
    GetDlgItemTextA(pages[1], 301, chosen_folder, sizeof(chosen_folder));
    CHECK(!strcmp(chosen_folder, "X"));
    syschr('b');
    break;
  case 5:
    CHECK(page_at == 0);
    key(VK_RETURN);
    break;
  case 6:
    CHECK(page_at == 1);
    key(VK_RETURN);
    break;
  case 7:
    CHECK(page_at == 2);
    key(VK_RETURN);
    break;
  default:
    CHECK(!"installer script overran");
  }
}
static void test_installer(void) {
  HWND owner = owner_window("Owner", WS_CAPTION | 0x80000u, 600, 420);
  script(installer_script);
  CHECK(DialogBoxParamA(FIXTURE, (const char *)(uintptr_t)110, owner,
                        outer_proc, 0) == IDOK);
  printf("INSTALLER pages license -> directory (%s) -> back -> finish\n",
         chosen_folder);
  script(0);
  DestroyWindow(owner);
}

/* ---- pinned frames ---------------------------------------------------------- */
static char frame_name[64];
static UINT capture_key = VK_ESCAPE;
static void capture_script(HWND dlg, UINT why, UINT step) {
  (void)dlg, (void)why;
  if (step == 0) {
    frame(frame_name);
    key(capture_key);
  } else if (step > 3)
    CHECK(!"capture did not close");
}
static void gallery_fill(HWND dlg) {
  SendDlgItemMessageA(dlg, 204, BM_SETCHECK, BST_CHECKED, 0);
  SendDlgItemMessageA(dlg, 205, BM_SETCHECK, BST_INDETERMINATE, 0);
  SendDlgItemMessageA(dlg, 206, BM_SETCHECK, BST_CHECKED, 0);
  SetDlgItemTextA(dlg, 214, "Editable");
  SetDlgItemTextA(dlg, 215, "secret");
  SetDlgItemTextA(dlg, 216, "Read-only");
  SetDlgItemTextA(dlg, 217, "Multi-line edit\r\nsecond line\r\nthird line\r\nfourth");
  static const char *const rows[] = {"Alpha", "Bravo", "Charlie", "Delta",
                                     "Echo", "Foxtrot"};
  for (UINT i = 0; i < 6; i++) {
    SendDlgItemMessageA(dlg, 218, LB_ADDSTRING, 0, (LPARAM)rows[i]);
    SendDlgItemMessageA(dlg, 219, CB_ADDSTRING, 0, (LPARAM)rows[i]);
    SendDlgItemMessageA(dlg, 220, CB_ADDSTRING, 0, (LPARAM)rows[i]);
    SendDlgItemMessageA(dlg, 221, CB_ADDSTRING, 0, (LPARAM)rows[i]);
  }
  SendDlgItemMessageA(dlg, 218, LB_SETCURSEL, 2, 0);
  SendDlgItemMessageA(dlg, 219, CB_SETCURSEL, 1, 0);
  SendDlgItemMessageA(dlg, 220, CB_SETCURSEL, 3, 0);
  SCROLLINFO s = {sizeof(s), SIF_ALL, 0, 100, 20, 30, 0};
  SendDlgItemMessageA(dlg, 222, SBM_SETSCROLLINFO, 1, (LPARAM)&s);
  SendDlgItemMessageA(dlg, 223, SBM_SETSCROLLINFO, 1, (LPARAM)&s);
  SendDlgItemMessageA(dlg, 225, STM_SETICON,
                      (WPARAM)LoadIconA(0, (const char *)(uintptr_t)32516), 0);
}
static intptr_t W32API gallery_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  if (m == WM_INITDIALOG) {
    gallery_fill(h);
    if (lp == 1) {
      HWND e = GetDlgItem(h, 214);
      u32_dialog_focus(h, e);
      SendMessageA(e, EM_SETSEL, 2, 6);
      return 0;
    }
    return 1;
  }
  return record_proc(h, m, wp, lp);
}
static void dropped_script(HWND dlg, UINT why, UINT step) {
  (void)why;
  if (step == 0) {
    SendDlgItemMessageA(dlg, 219, CB_SHOWDROPDOWN, 1, 0);
    return;
  }
  if (step == 1) {
    frame(frame_name);
    SendDlgItemMessageA(dlg, 219, CB_SHOWDROPDOWN, 0, 0);
    key(VK_ESCAPE);
    return;
  }
  if (step > 4)
    CHECK(!"dropped frame did not close");
}
static void frames_at(UINT width, UINT height) {
  u32_screen_w = width;
  u32_screen_h = height;
  char suffix[16];
  snprintf(suffix, sizeof(suffix), "%ux%u", width, height);
  HWND owner = owner_window("Ciuki Sample", WS_CAPTION | 0x80000u | 0x40000u,
                            (int)width - 64, (int)height - 96);
  script(capture_script);
  snprintf(frame_name, sizeof(frame_name), "dialog-classic-%s", suffix);
  DialogBoxParamA(FIXTURE, (const char *)(uintptr_t)100, owner, record_proc, 0);
  snprintf(frame_name, sizeof(frame_name), "dialog-system-font-%s", suffix);
  idle_count = 0;
  DialogBoxParamA(FIXTURE, (const char *)(uintptr_t)102, owner, record_proc, 0);
  snprintf(frame_name, sizeof(frame_name), "controls-gallery-%s", suffix);
  idle_count = 0;
  DialogBoxParamA(FIXTURE, (const char *)(uintptr_t)101, owner, gallery_proc, 0);
  snprintf(frame_name, sizeof(frame_name), "controls-edit-focus-%s", suffix);
  idle_count = 0;
  DialogBoxParamA(FIXTURE, (const char *)(uintptr_t)101, owner, gallery_proc, 1);
  script(dropped_script);
  snprintf(frame_name, sizeof(frame_name), "combo-dropped-%s", suffix);
  DialogBoxParamA(FIXTURE, (const char *)(uintptr_t)101, owner, gallery_proc, 0);
  script(capture_script);
  static const struct {
    const char *name, *text, *caption;
    UINT type;
  } boxes[] = {
      {"msgbox-ok-information", "Setup has finished installing Ciuki Sample.",
       "Ciuki Sample Setup", MB_OK | MB_ICONASTERISK},
      {"msgbox-okcancel-exclamation",
       "The destination folder already exists.\nDo you want to continue?",
       "Warning", MB_OKCANCEL | MB_ICONEXCLAMATION},
      {"msgbox-yesnocancel-question", "Save changes to Untitled?", "CiukNote",
       MB_YESNOCANCEL | MB_ICONQUESTION},
      {"msgbox-abortretryignore-hand",
       "Cannot read from drive E:.\nThe disc may be missing or unreadable.",
       "Error", MB_ABORTRETRYIGNORE | MB_ICONHAND | MB_DEFBUTTON2}};
  capture_key = VK_RETURN;
  for (UINT i = 0; i < 4; i++) {
    snprintf(frame_name, sizeof(frame_name), "%s-%s", boxes[i].name, suffix);
    idle_count = 0;
    MessageBoxA(owner, boxes[i].text, boxes[i].caption, boxes[i].type);
  }
  capture_key = VK_ESCAPE;
  menus_frames(owner, suffix);
  script(0);
  DestroyWindow(owner);
}
static void test_frames(void) {
  frames_at(640, 480);
  frames_at(800, 600);
  CHECK(frame_count >= 20);
}

/* ---- G15: WM_HELP, F1 and What's This? ----
 * Windows 98: F1 sends WM_HELP (HELPINFO_WINDOW, the control, its help id,
 * the pointer) to the focused control and DefWindowProc passes it to the
 * parent; Shift+F1 is SC_CONTEXTHELP: the help arrow until the next click,
 * which sends WM_HELP for the window under the pointer and is not delivered;
 * Esc ends the mode. */
static void raw_input(HWND h, UINT type, UINT code, UINT value, UINT value2) {
  struct u32_window *root = u32_root(u32_window(h));
  UINT data[10] = {0, 0, 0, type, 0, 0, code, value, value2, 0};
  event(root, type == CIUKI_INPUT_MOTION ? DESK_MOTION : type == CIUKI_INPUT_BUTTON ? DESK_BUTTON : DESK_KEY, data, 10);
}
static void pump_all(void) {
  MSG m;
  for (UINT i = 0; i < 400 && PeekMessageA(&m, 0, 0, 0, 1); i++) {
    TranslateMessage(&m);
    DispatchMessageA(&m);
  }
}
static void raw_key(HWND h, UINT scan, UINT down) {
  raw_input(h, CIUKI_INPUT_KEY, scan, down, 0);
  pump_all();
}
static void raw_click(HWND owner, HWND target) {
  RECT r;
  GetWindowRect(target, &r);
  POINT so = u32_surface_origin(u32_window(owner));
  raw_input(owner, CIUKI_INPUT_MOTION, 0, (UINT)(r.left + 4 - so.x), (UINT)(r.top + 4 - so.y));
  pump_all();
  raw_input(owner, CIUKI_INPUT_BUTTON, 1, 1, 0);
  pump_all();
  raw_input(owner, CIUKI_INPUT_BUTTON, 1, 0, 0);
  pump_all();
}
static void test_help(void) {
  HWND owner = owner_window("Help owner", WS_CAPTION | 0x80000u, 300, 200);
  UINT activate[2] = {1, 1};
  event(u32_window(owner), DESK_V3_ACTIVATE, activate, 2);
  pump_all();
  HWND edit = CreateWindowExA(0, "EDIT", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 10, 10, 120, 22,
                              owner, (HANDLE)(uintptr_t)41, FIXTURE, 0);
  HWND button = CreateWindowExA(0, "BUTTON", "OK", WS_CHILD | WS_VISIBLE, 10, 50, 80, 24,
                                owner, (HANDLE)(uintptr_t)42, FIXTURE, 0);
  CHECK(edit && button);
  pump_all();
  /* Window help ids: SetWindowContextHelpId / GetWindowContextHelpId. */
  CHECK(SetWindowContextHelpId(edit, 7001) && GetWindowContextHelpId(edit) == 7001 &&
        GetWindowContextHelpId(button) == 0);
  CHECK(!SetWindowContextHelpId(0, 1) && GetLastError() == 1400 && GetWindowContextHelpId(0) == 0);
  CHECK(!u32_help_mode());
  /* F1 with the edit focused: HELPINFO_WINDOW for the edit, delivered to its parent. */
  SetFocus(edit);
  owner_help_count = 0;
  raw_key(owner, 0x3b, 1);
  raw_key(owner, 0x3b, 0);
  POINT cursor;
  GetCursorPos(&cursor);
  CHECK(owner_help_count == 1 && owner_help.cbSize == sizeof(HELPINFO) && owner_help.iContextType == HELPINFO_WINDOW &&
        owner_help.iCtrlId == 41 && owner_help.hItemHandle == edit && owner_help.dwContextId == 7001 &&
        owner_help.MousePos.x == cursor.x && owner_help.MousePos.y == cursor.y);
  /* The button has no help id and reports its own id. */
  SetFocus(button);
  raw_key(owner, 0x3b, 1);
  raw_key(owner, 0x3b, 0);
  CHECK(owner_help_count == 2 && owner_help.iCtrlId == 42 && owner_help.hItemHandle == button && owner_help.dwContextId == 0);
  /* F1 with Ctrl or Alt is not help. */
  raw_key(owner, 0x1d, 1);
  raw_key(owner, 0x3b, 1);
  raw_key(owner, 0x3b, 0);
  raw_key(owner, 0x1d, 0);
  raw_key(owner, 0x38, 1);
  raw_key(owner, 0x3b, 1);
  raw_key(owner, 0x3b, 0);
  raw_key(owner, 0x38, 0);
  CHECK(owner_help_count == 2 && !u32_help_mode());
  /* A window nobody answers ignores WM_HELP (there is no WinHelp engine); a child passes it up. */
  WNDCLASSA plain = {.lpfnWndProc = DefWindowProcA, .lpszClassName = "Help plain"};
  CHECK(RegisterClassA(&plain));
  HWND top = CreateWindowExA(0, "Help plain", "p", 0, 0, 0, 10, 10, 0, 0, FIXTURE, 0);
  HWND leaf = CreateWindowExA(0, "Help plain", "c", WS_CHILD, 0, 0, 5, 5, top, (HANDLE)(uintptr_t)3, FIXTURE, 0);
  CHECK(top && leaf);
  HELPINFO hi = {sizeof(hi), HELPINFO_WINDOW, 3, leaf, 0, {0, 0}};
  CHECK(SendMessageA(leaf, WM_HELP, 0, (LPARAM)&hi) == 0 && SendMessageA(top, WM_HELP, 0, (LPARAM)&hi) == 0);
  DestroyWindow(top);
  /* Shift+F1: the help arrow until the click. */
  HCURSOR before = GetCursor();
  SetFocus(edit);
  raw_key(owner, 0x2a, 1);
  raw_key(owner, 0x3b, 1);
  raw_key(owner, 0x3b, 0);
  raw_key(owner, 0x2a, 0);
  CHECK(u32_help_mode() && GetCursor() == (HCURSOR)(uintptr_t)32651u && owner_help_count == 2);
  /* The click is the help request for the button under it, not a click. */
  owner_last_command = 0;
  HWND focus_before = GetFocus();
  raw_click(owner, button);
  CHECK(!u32_help_mode());
  CHECK(GetCursor() == before);
  CHECK(owner_help_count == 3);
  CHECK(owner_help.hItemHandle == button && owner_help.iCtrlId == 42);
  CHECK(owner_last_command == 0 && GetFocus() == focus_before);
  /* Esc ends the mode without delivering the key. */
  raw_key(owner, 0x2a, 1);
  raw_key(owner, 0x3b, 1);
  raw_key(owner, 0x3b, 0);
  raw_key(owner, 0x2a, 0);
  CHECK(u32_help_mode());
  raw_key(owner, 0x01, 1);
  MSG m;
  CHECK(!u32_help_mode() && GetCursor() == before && !PeekMessageA(&m, edit, WM_KEYDOWN, WM_KEYDOWN, 0) &&
        owner_help_count == 3);
  raw_key(owner, 0x01, 0);
  /* The mode ends after one click, a normal click works again, and the edit gets the id of its own help. */
  SendMessageA(owner, WM_SYSCOMMAND, SC_CONTEXTHELP, 0);
  CHECK(u32_help_mode());
  raw_click(owner, edit);
  CHECK(!u32_help_mode() && owner_help_count == 4 && owner_help.hItemHandle == edit && owner_help.dwContextId == 7001);
  raw_click(owner, edit);
  CHECK(owner_help_count == 4 && GetFocus() == edit);
  /* WS_EX_CONTEXTHELP puts the "?" caption button on the window (v3 style bit), also for a
   * template with DS_CONTEXTHELP; the desktop's HELP message starts What's This?. */
  UINT before_create = outgoing_count;
  HWND helper = CreateWindowExA(WS_EX_CONTEXTHELP, "Help plain", "dialog", WS_CAPTION | WS_VISIBLE, 0, 0, 200, 100, 0, 0, FIXTURE, 0);
  CHECK(helper && (u32_window(helper)->exstyle & WS_EX_CONTEXTHELP));
  UINT created_style = 0xffffffffu;
  for (UINT i = before_create; i < outgoing_count; i++)
    if (outgoing[i].data[1] == DESK_CREATE_WINDOW && desk_get32(outgoing[i].data + 8) == u32_window(helper)->wire)
      created_style = desk_get32(outgoing[i].data + 16);
  CHECK(created_style != 0xffffffffu && (created_style & DESK_V3_CONTEXTHELP));
  CHECK(!(u32_style(u32_window(owner)) & DESK_V3_CONTEXTHELP) && (u32_style(u32_window(helper)) & DESK_V3_CONTEXTHELP));
  CHECK(!u32_help_mode());
  event(u32_window(helper), DESK_V3_HELP, 0, 0);
  pump_all();
  CHECK(u32_help_mode());
  raw_key(helper, 0x01, 1);
  raw_key(helper, 0x01, 0);
  CHECK(!u32_help_mode());
  DestroyWindow(helper);
  #pragma pack(push, 2)
  static const struct { DWORD style, ex; uint16_t cdit; int16_t x, y, cx, cy; uint16_t menu, cls, title; } tpl =
      {WS_POPUP | WS_CAPTION | DS_CONTEXTHELP, 0, 0, 0, 0, 100, 50, 0, 0, 0};
  #pragma pack(pop)
  HWND dlg = CreateDialogIndirectParamA(FIXTURE, (const DLGTEMPLATE *)&tpl, 0, 0, 0);
  CHECK(dlg && (GetWindowLongA(dlg, GWL_EXSTYLE) & WS_EX_CONTEXTHELP));
  DestroyWindow(dlg);
  DestroyWindow(owner);
}
