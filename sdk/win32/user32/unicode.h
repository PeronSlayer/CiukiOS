/* f5-20B private Unicode and upper-API layouts of USER (PE32 layouts of the
 * Windows SDK structures; strings converted through code page 1252).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef U32_UNICODE_H
#define U32_UNICODE_H
#include "classic.h"
typedef struct {
  UINT style;
  WNDPROC lpfnWndProc;
  int cbClsExtra, cbWndExtra;
  HINSTANCE hInstance;
  HICON hIcon;
  HCURSOR hCursor;
  HBRUSH hbrBackground;
  const WCHAR *lpszMenuName, *lpszClassName;
} WNDCLASSW;
typedef struct {
  UINT cbSize, style;
  WNDPROC lpfnWndProc;
  int cbClsExtra, cbWndExtra;
  HINSTANCE hInstance;
  HICON hIcon;
  HCURSOR hCursor;
  HBRUSH hbrBackground;
  const WCHAR *lpszMenuName, *lpszClassName;
  HICON hIconSm;
} WNDCLASSEXW;
typedef struct {
  void *lpCreateParams;
  HINSTANCE hInstance;
  HANDLE hMenu;
  HWND hwndParent;
  int cy, cx, y, x;
  LONG style;
  const WCHAR *lpszName, *lpszClass;
  DWORD dwExStyle;
} CREATESTRUCTW;
typedef struct {
  UINT cbSize, fMask, fType, fState, wID;
  HMENU hSubMenu;
  HANDLE hbmpChecked, hbmpUnchecked;
  uintptr_t dwItemData;
  WCHAR *dwTypeData;
  UINT cch;
  HANDLE hbmpItem;
} MENUITEMINFOW;
typedef void(W32API *MSGBOXCALLBACK)(void *);
typedef struct {
  UINT cbSize;
  HWND hwndOwner;
  HINSTANCE hInstance;
  const char *lpszText, *lpszCaption;
  DWORD dwStyle;
  const char *lpszIcon;
  uintptr_t dwContextHelpId;
  MSGBOXCALLBACK lpfnMsgBoxCallback;
  DWORD dwLanguageId;
} MSGBOXPARAMSA;
typedef struct {
  UINT cbSize;
  HWND hwndOwner;
  HINSTANCE hInstance;
  const WCHAR *lpszText, *lpszCaption;
  DWORD dwStyle;
  const WCHAR *lpszIcon;
  uintptr_t dwContextHelpId;
  MSGBOXCALLBACK lpfnMsgBoxCallback;
  DWORD dwLanguageId;
} MSGBOXPARAMSW;
typedef struct {
  DWORD cbSize;
  RECT rcMonitor, rcWork;
  DWORD dwFlags;
} MONITORINFO;
typedef struct {
  DWORD cbSize;
  RECT rcMonitor, rcWork;
  DWORD dwFlags;
  WCHAR szDevice[32];
} MONITORINFOEXW;
typedef struct {
  DWORD cbSize;
  RECT rcMonitor, rcWork;
  DWORD dwFlags;
  char szDevice[32];
} MONITORINFOEXA;
typedef struct {
  DWORD cbSize;
  RECT rcWindow, rcClient;
  DWORD dwStyle, dwExStyle, dwWindowStatus;
  UINT cxWindowBorders, cyWindowBorders;
  ATOM atomWindowType;
  uint16_t wCreatorVersion;
} WINDOWINFO;
typedef struct {
  DWORD cbSize, dwFlags;
  HWND hwndTrack;
  DWORD dwHoverTime;
} TRACKMOUSEEVENT;
typedef struct {
  DWORD cbSize, fMask, dwStyle;
  UINT cyMax;
  HBRUSH hbrBack;
  DWORD dwContextHelpID;
  uintptr_t dwMenuData;
} MENUINFO;
#define MIM_MAXHEIGHT 1u
#define MIM_BACKGROUND 2u
#define MIM_HELPID 4u
#define MIM_MENUDATA 8u
#define MIM_STYLE 0x10u
#define MIM_APPLYTOSUBMENUS 0x80000000u
/* Unicode entry points used inside USER. */
LRESULT W32API DefWindowProcW(HWND, UINT, WPARAM, LPARAM);
LRESULT W32API CallWindowProcW(WNDPROC, HWND, UINT, WPARAM, LPARAM);
LRESULT W32API SendMessageW(HWND, UINT, WPARAM, LPARAM);
BOOL W32API PostMessageW(HWND, UINT, WPARAM, LPARAM);
HWND W32API CreateWindowExW(DWORD, const WCHAR *, const WCHAR *, DWORD, int,
                            int, int, int, HWND, HANDLE, HINSTANCE, void *);
LONG W32API GetWindowLongW(HWND, int);
LONG W32API SetWindowLongW(HWND, int, LONG);
int W32API GetWindowTextW(HWND, WCHAR *, int);
BOOL W32API SetWindowTextW(HWND, const WCHAR *);
BOOL W32API SystemParametersInfoA(UINT, UINT, void *, UINT);
BOOL W32API SystemParametersInfoW(UINT, UINT, void *, UINT);
int W32API MessageBoxExA(HWND, const char *, const char *, UINT, uint16_t);
int W32API MessageBoxA(HWND, const char *, const char *, UINT);
/* Private menu record exchange (menu.c) for GetMenuInfo/SetMenuInfo and
 * GetMenuItemRect. */
BOOL u32_menu_info(HMENU, MENUINFO *, BOOL set);
BOOL u32_menu_item_rect(HWND, HMENU, UINT, RECT *);
/* Dialog creation with a Unicode dialog procedure (dialog.c). */
HWND u32_dialog_create_ex(HINSTANCE, const struct u32_template *, HWND,
                          DLGPROC, LPARAM, BOOL modal, BOOL wide);
HWND u32_dialog_from(HINSTANCE, const void *, DWORD, HWND, DLGPROC, LPARAM,
                     BOOL modal, BOOL wide);
intptr_t u32_dialog_box(HINSTANCE, const void *, DWORD, HWND, DLGPROC, LPARAM,
                        BOOL wide);
int u32_dialog_long_ex(HWND, int, LONG *, BOOL set, BOOL wide);
LRESULT W32API DefDlgProcW(HWND, UINT, WPARAM, LPARAM);
/* Hook procedures installed with SetWindowsHookExW (hooks.c). */
void u32_hook_unicode(HANDLE);
/* Optional KERNEL32 entry points resolved by name (upper.c). */
void *u32_kernel(const char *);
/* Code-page 1252 character classes shared by the Char* API (text.c). */
UINT u32_upper(UINT), u32_lower(UINT);
BOOL u32_alpha(UINT);
/* Icons (icons.c). */
HICON u32_icon_from_resource(const uint8_t *, DWORD, BOOL icon, BOOL shared);
HICON u32_icon_copy(HICON);
BOOL u32_icon_info(HICON, ICONINFO *);
BOOL u32_is_icon(HANDLE, BOOL *cursor);
#endif
