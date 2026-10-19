/* f5-10C menus, tracking, accelerators and the system menu.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
static UINT menu_selects, init_popups;
static void menu_script_popup(HWND popup, UINT why, UINT step) {
  CHECK(why == MSGF_MENU);
  (void)popup;
  switch (step) {
  case 0:
    key(VK_DOWN);
    break;
  case 1:
    key(VK_DOWN);
    key(VK_DOWN);
    break;
  case 2:
    key(VK_RIGHT);
    break;
  case 3:
    key(VK_RETURN);
    break;
  default:
    CHECK(!"popup script overran");
  }
}
static void menu_script_mnemonic(HWND popup, UINT why, UINT step) {
  (void)popup, (void)why;
  CHECK(step == 0);
  chr('x');
}
static void menu_script_escape(HWND popup, UINT why, UINT step) {
  (void)popup, (void)why;
  CHECK(step == 0);
  key(VK_ESCAPE);
}
static HWND menu_owner;
static void menu_script_bar(HWND popup, UINT why, UINT step) {
  (void)popup;
  CHECK(why == MSGF_MENU);
  switch (step) {
  case 0:
    key(VK_RIGHT);
    break;
  case 1:
    key(VK_DOWN);
    break;
  case 2:
    key(VK_RETURN);
    break;
  default:
    CHECK(!"bar script overran");
  }
}
static void menu_script_alt(HWND popup, UINT why, UINT step) {
  (void)popup, (void)why;
  switch (step) {
  case 0:
    key(VK_ESCAPE);
    break;
  case 1:
    key(VK_ESCAPE);
    break;
  default:
    CHECK(!"alt script overran");
  }
}
static void menu_script_mouse(HWND popup, UINT why, UINT step) {
  (void)why;
  if (step == 0) {
    CHECK(popup && IsWindow(popup));
    RECT r;
    GetWindowRect(popup, &r);
    u32_pointer = (POINT){r.left + 30, r.top + 3 + 9};
    PostMessageA(popup, WM_LBUTTONUP, 0, 0);
  } else
    CHECK(!"mouse script overran");
}
static void menu_script_system(HWND popup, UINT why, UINT step) {
  (void)popup, (void)why;
  switch (step) {
  case 0:
    key(VK_UP);
    break;
  case 1:
    key(VK_RETURN);
    break;
  default:
    CHECK(!"system script overran");
  }
}
static LRESULT W32API menu_owner_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  if (m == WM_MENUSELECT)
    menu_selects++;
  if (m == WM_INITMENUPOPUP)
    init_popups++;
  return owner_proc(h, m, wp, lp);
}
static void test_menus(void) {
  HMENU bar = LoadMenuA(FIXTURE, (const char *)(uintptr_t)200);
  CHECK(bar && IsMenu(bar) && GetMenuItemCount(bar) == 3);
  HMENU file = GetSubMenu(bar, 0);
  CHECK(file && GetMenuItemCount(file) == 5);
  CHECK(GetMenuItemID(file, 0) == 1001 && GetMenuItemID(file, 3) == (UINT)-1);
  CHECK(GetMenuState(file, 1002, MF_BYCOMMAND) & MF_GRAYED);
  CHECK(GetMenuState(file, 2, MF_BYPOSITION) & MF_SEPARATOR);
  CHECK((GetMenuState(file, 3, MF_BYPOSITION) >> 8) == 1);
  CHECK(GetMenuState(bar, 1004, MF_BYCOMMAND) & MF_CHECKED);
  char text[64];
  CHECK(GetMenuStringA(bar, 1001, text, 64, MF_BYCOMMAND) == 15 &&
        !strcmp(text, "&Open...\tCtrl+O"));
  MENUITEMINFOA info = {.cbSize = sizeof(info),
                        .fMask = MIIM_TYPE | MIIM_ID | MIIM_STATE};
  info.dwTypeData = text;
  info.cch = sizeof(text);
  CHECK(GetMenuItemInfoA(file, 4, 1, &info) && info.wID == 1003 &&
        !strcmp(text, "E&xit"));
  CHECK(CheckMenuItem(bar, 1005, MF_CHECKED) == 0 &&
        CheckMenuItem(bar, 1005, MF_UNCHECKED) == MF_CHECKED);
  CHECK(EnableMenuItem(bar, 1002, MF_ENABLED) == MF_GRAYED &&
        EnableMenuItem(bar, 1002, MF_GRAYED) == MF_ENABLED);
  CHECK(EnableMenuItem(bar, 4242, 0) == (UINT)-1);
  HMENU ex = LoadMenuA(FIXTURE, (const char *)(uintptr_t)201);
  CHECK(ex);
  HMENU game = GetSubMenu(ex, 0);
  CHECK(GetMenuItemCount(game) == 4);
  CHECK((GetMenuState(game, 1102, 0) & (MF_CHECKED | MFT_RADIOCHECK)) ==
        MF_CHECKED);
  CHECK(CheckMenuRadioItem(game, 1102, 1103, 1103, MF_BYCOMMAND));
  CHECK(!(GetMenuState(game, 1102, 0) & MF_CHECKED) &&
        (GetMenuState(game, 1103, 0) & MF_CHECKED));
  CHECK(AppendMenuA(game, MF_STRING, 1104, "&Custom..."));
  CHECK(InsertMenuA(game, 0, MF_BYPOSITION | MF_STRING, 1100, "&Resume"));
  CHECK(GetMenuItemID(game, 0) == 1100 && GetMenuItemCount(game) == 6);
  CHECK(ModifyMenuA(game, 1100, MF_BYCOMMAND | MF_STRING, 1100, "R&esume"));
  CHECK(RemoveMenu(game, 1100, MF_BYCOMMAND) && GetMenuItemCount(game) == 5);
  CHECK(DeleteMenu(ex, 0, MF_BYPOSITION) && !IsMenu(game));
  CHECK(DestroyMenu(ex) && !IsMenu(ex));
  /* The menu bar lives in the top-level surface above the client area. */
  WNDCLASSA c = {.style = 8,
                 .lpfnWndProc = menu_owner_proc,
                 .hbrBackground = (HBRUSH)(uintptr_t)(COLOR_WINDOW + 1),
                 .lpszClassName = "Menu Owner"};
  CHECK(RegisterClassA(&c));
  menu_owner = CreateWindowExA(0, "Menu Owner", "Menus",
                               WS_CAPTION | 0x80000u | WS_VISIBLE, 20, 40, 400,
                               300, 0, 0, FIXTURE, 0);
  CHECK(menu_owner);
  RECT before;
  GetClientRect(menu_owner, &before);
  POINT origin_before = {0, 0};
  ClientToScreen(menu_owner, &origin_before);
  CHECK(SetMenu(menu_owner, bar) && GetMenu(menu_owner) == bar);
  drain(0);
  RECT after;
  GetClientRect(menu_owner, &after);
  int strip = GetSystemMetrics(15);
  CHECK(after.bottom == before.bottom - strip && after.right == before.right);
  POINT origin_after = {0, 0};
  ClientToScreen(menu_owner, &origin_after);
  CHECK(origin_after.y == origin_before.y + strip);
  struct u32_window *w = u32_window(menu_owner);
  CHECK(w->surface.height == (UINT)(after.bottom + strip));
  uint32_t menu_pixel = ((uint32_t *)w->surface.pixels)[2 * w->surface.width + 1];
  CHECK((menu_pixel & 0xffffff) == rgb(GetSysColor(COLOR_MENU)));
  CHECK(u32_hit_test(menu_owner, (POINT){origin_after.x + 5,
                                         origin_after.y - strip / 2}) ==
        HTMENU);
  /* TrackPopupMenu: keyboard into a submenu, returned command. */
  script(menu_script_popup);
  menu_selects = init_popups = 0;
  UINT cmd = (UINT)TrackPopupMenu(file, TPM_RETURNCMD, 100, 100, 0, menu_owner,
                                  0);
  CHECK(cmd == 1010 && menu_selects >= 3 && init_popups == 2);
  CHECK(seen(WM_ENTERMENULOOP) && seen(WM_EXITMENULOOP) && seen(WM_INITMENU));
  script(menu_script_mnemonic);
  CHECK(TrackPopupMenu(file, TPM_RETURNCMD, 100, 100, 0, menu_owner, 0) == 1003);
  script(menu_script_escape);
  CHECK(TrackPopupMenu(file, TPM_RETURNCMD, 100, 100, 0, menu_owner, 0) == 0);
  /* Without TPM_RETURNCMD the command is posted to the owner. */
  script(menu_script_mnemonic);
  owner_last_command = 0;
  CHECK(TrackPopupMenu(file, 0, 100, 100, 0, menu_owner, 0));
  drain(0);
  CHECK(owner_last_command == 1003);
  /* Mouse release over the first item. */
  script(menu_script_mouse);
  CHECK(TrackPopupMenu(file, TPM_RETURNCMD, 120, 120, 0, menu_owner, 0) == 1001);
  /* Menu bar from the keyboard: Alt, Right, Down, Enter. */
  script(menu_script_bar);
  owner_last_command = 0;
  SetFocus(menu_owner);
  post_key(menu_owner, VK_MENU, 1, 1);
  post_key(menu_owner, VK_MENU, 0, 1);
  drain(0);
  CHECK(owner_last_command == 1004);
  /* Alt+F opens File; Escape returns to the bar, Escape leaves. */
  script(menu_script_alt);
  owner_last_command = 0;
  PostMessageA(menu_owner, WM_SYSCHAR, 'f', 1);
  drain(0);
  CHECK(owner_last_command == 0 && idle_count == 2);
  /* Accelerators. */
  HACCEL accel = LoadAcceleratorsA(FIXTURE, (const char *)(uintptr_t)300);
  CHECK(accel && CopyAcceleratorTableA(accel, 0, 0) == 4);
  ACCEL entries[4];
  CHECK(CopyAcceleratorTableA(accel, entries, 4) == 4 &&
        entries[0].cmd == 1001 && (entries[0].fVirt & (FVIRTKEY | FCONTROL)) ==
                                      (FVIRTKEY | FCONTROL));
  uint8_t keys[256] = {0};
  keys[VK_CONTROL] = 0x80;
  SetKeyboardState(keys);
  MSG msg = {menu_owner, WM_KEYDOWN, 'O', 1, 0, {0, 0}};
  owner_last_command = 0;
  CHECK(TranslateAcceleratorA(menu_owner, accel, &msg) &&
        owner_last_command == (1001 | (1u << 16)));
  msg.wParam = 'S';
  owner_last_command = 0;
  CHECK(TranslateAcceleratorA(menu_owner, accel, &msg) && owner_last_command == 0);
  keys[VK_CONTROL] = 0;
  SetKeyboardState(keys);
  msg.wParam = 'O';
  CHECK(!TranslateAcceleratorA(menu_owner, accel, &msg));
  msg = (MSG){menu_owner, WM_KEYDOWN, 0x70, 1, 0, {0, 0}};
  CHECK(TranslateAcceleratorA(menu_owner, accel, &msg) &&
        owner_last_command == (1006 | (1u << 16)));
  msg = (MSG){menu_owner, WM_SYSCHAR, 'x', 1, 0, {0, 0}};
  CHECK(TranslateAcceleratorA(menu_owner, accel, &msg) &&
        owner_last_command == (1003 | (1u << 16)));
  ACCEL custom[1] = {{FVIRTKEY, VK_F4, 77}};
  HACCEL created = CreateAcceleratorTableA(custom, 1);
  CHECK(created && DestroyAcceleratorTable(created) &&
        !DestroyAcceleratorTable(created));
  CHECK(DestroyAcceleratorTable(accel));
  /* System menu subset and Alt+Space. */
  HMENU system = GetSystemMenu(menu_owner, 0);
  CHECK(system && GetMenuItemCount(system) == 7 &&
        GetMenuItemID(system, 6) == SC_CLOSE);
  script(menu_script_system);
  owner_last_command = 0;
  PostMessageA(menu_owner, WM_SYSCHAR, ' ', 1);
  drain(0);
  CHECK((owner_last_command & 0xfff0) == SC_CLOSE && IsWindow(menu_owner));
  CHECK(!GetSystemMenu(menu_owner, 1) && !IsMenu(system));
  /* WM_CONTEXTMENU from a right click propagates to the parent. */
  HWND child = CreateWindowExA(0, "STATIC", "x", WS_CHILD | WS_VISIBLE, 10, 10,
                               40, 20, menu_owner, (HANDLE)9, FIXTURE, 0);
  owner_contextmenu = 0;
  SendMessageA(child, WM_RBUTTONUP, 0, (LPARAM)(5 | (5 << 16)));
  CHECK(owner_contextmenu == 1);
  /* Window menus are destroyed with their window. */
  script(0);
  DestroyWindow(menu_owner);
  CHECK(!IsMenu(bar));
}
static void popup_frame_script(HWND popup, UINT why, UINT step) {
  (void)popup, (void)why;
  switch (step) {
  case 0:
    key(VK_DOWN);
    key(VK_DOWN);
    break;
  case 1:
    key(VK_RIGHT);
    break;
  case 2:
    frame(frame_name);
    key(VK_ESCAPE);
    break;
  case 3:
    key(VK_ESCAPE);
    break;
  case 4:
    key(VK_ESCAPE);
    break;
  default:
    CHECK(!"popup frame script overran");
  }
}
static void menus_frames(HWND owner, const char *suffix) {
  HMENU bar = LoadMenuA(FIXTURE, (const char *)(uintptr_t)200);
  CHECK(bar && SetMenu(owner, bar));
  drain(0);
  snprintf(frame_name, sizeof(frame_name), "menu-bar-%s", suffix);
  frame(frame_name);
  snprintf(frame_name, sizeof(frame_name), "menu-open-submenu-%s", suffix);
  script(popup_frame_script);
  PostMessageA(owner, WM_SYSCHAR, 'f', 1);
  drain(0);
  CHECK(idle_count >= 3);
  SetMenu(owner, 0);
  DestroyMenu(bar);
  drain(0);
}
