/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef CIUKI_USER32_H
#define CIUKI_USER32_H
#include <ciuki/w32_transport.h>
#include <w32/kernel32.h>
typedef HANDLE HWND;
typedef HANDLE HDC;
typedef HANDLE HICON;
typedef HANDLE HCURSOR;
typedef HANDLE HBRUSH;
typedef HANDLE HRGN;
typedef HANDLE HINSTANCE;
typedef uint32_t UINT;
typedef uintptr_t WPARAM;
typedef intptr_t LPARAM;
typedef intptr_t LRESULT;
typedef uint16_t ATOM;
typedef struct {
  LONG x, y;
} POINT;
typedef struct {
  LONG left, top, right, bottom;
} RECT;
typedef LRESULT(W32API *WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef BOOL(W32API *WNDENUMPROC)(HWND, LPARAM);
typedef LRESULT(W32API *HOOKPROC)(int, WPARAM, LPARAM);
typedef void(W32API *TIMERPROC)(HWND, UINT, uintptr_t, DWORD);
typedef struct {
  HWND hwnd;
  UINT message;
  WPARAM wParam;
  LPARAM lParam;
  DWORD time;
  POINT pt;
} MSG;
typedef struct {
  UINT style;
  WNDPROC lpfnWndProc;
  int cbClsExtra, cbWndExtra;
  HINSTANCE hInstance;
  HICON hIcon;
  HCURSOR hCursor;
  HBRUSH hbrBackground;
  const char *lpszMenuName, *lpszClassName;
} WNDCLASSA;
typedef struct {
  UINT cbSize, style;
  WNDPROC lpfnWndProc;
  int cbClsExtra, cbWndExtra;
  HINSTANCE hInstance;
  HICON hIcon;
  HCURSOR hCursor;
  HBRUSH hbrBackground;
  const char *lpszMenuName, *lpszClassName;
  HICON hIconSm;
} WNDCLASSEXA;
typedef struct {
  HDC hdc;
  BOOL fErase;
  RECT rcPaint;
  BOOL fRestore, fIncUpdate;
  uint8_t rgbReserved[32];
} PAINTSTRUCT;
typedef struct {
  void *lpCreateParams;
  HINSTANCE hInstance;
  HANDLE hMenu;
  HWND hwndParent;
  int cy, cx, y, x;
  LONG style;
  const char *lpszName, *lpszClass;
  DWORD dwExStyle;
} CREATESTRUCTA;
typedef struct {
  uintptr_t dwData;
  DWORD cbData;
  void *lpData;
} COPYDATASTRUCT;
typedef struct {
  BOOL fIcon;
  DWORD xHotspot, yHotspot;
  HANDLE hbmMask, hbmColor;
} ICONINFO;
#define WM_NULL 0u
#define WM_CREATE 1u
#define WM_DESTROY 2u
#define WM_MOVE 3u
#define WM_SIZE 5u
#define WM_ACTIVATE 6u
#define WM_SETFOCUS 7u
#define WM_KILLFOCUS 8u
#define WM_ENABLE 10u
#define WM_SETREDRAW 11u
#define WM_SETTEXT 12u
#define WM_GETTEXT 13u
#define WM_GETTEXTLENGTH 14u
#define WM_PAINT 15u
#define WM_CLOSE 16u
#define WM_QUIT 18u
#define WM_ERASEBKGND 20u
#define WM_SHOWWINDOW 24u
#define WM_ACTIVATEAPP 28u
#define WM_QUERYNEWPALETTE 0x030f
#define WM_PALETTEISCHANGING 0x0310
#define WM_PALETTECHANGED 0x0311
#define WM_SETCURSOR 32u
#define WM_GETMINMAXINFO 36u
#define WM_COPYDATA 74u
#define WM_NCCREATE 129u
#define WM_NCDESTROY 130u
#define WM_NCACTIVATE 134u
#define WM_KEYDOWN 256u
#define WM_KEYUP 257u
#define WM_CHAR 258u
#define WM_SYSKEYDOWN 260u
#define WM_SYSKEYUP 261u
#define WM_SYSCHAR 262u
#define WM_SYSCOMMAND 274u
#define WM_TIMER 275u
#define WM_MOUSEMOVE 512u
#define WM_LBUTTONDOWN 513u
#define WM_LBUTTONUP 514u
#define WM_LBUTTONDBLCLK 515u
#define WM_RBUTTONDOWN 516u
#define WM_RBUTTONUP 517u
#define WM_RBUTTONDBLCLK 518u
#define WM_MBUTTONDOWN 519u
#define WM_MBUTTONUP 520u
#define WM_MBUTTONDBLCLK 521u
#define WM_MOUSEWHEEL 522u
#define WM_CAPTURECHANGED 533u
#define WM_DESTROYCLIPBOARD 775u
#define WM_DRAWCLIPBOARD 776u
#define WM_CHANGECBCHAIN 781u
#define WM_USER 1024u
#define WS_POPUP 0x80000000u
#define WS_CHILD 0x40000000u
#define WS_MINIMIZE 0x20000000u
#define WS_VISIBLE 0x10000000u
#define WS_DISABLED 0x08000000u
#define WS_CLIPSIBLINGS 0x04000000u
#define WS_CLIPCHILDREN 0x02000000u
#define WS_MAXIMIZE 0x01000000u
#define WS_CAPTION 0x00c00000u
#define WS_BORDER 0x00800000u
#define WS_DLGFRAME 0x00400000u
#define WS_THICKFRAME 0x00040000u
#define WS_EX_TOPMOST 8u
#define WS_EX_TOOLWINDOW 128u
#define WS_EX_CLIENTEDGE 512u
#define WS_EX_STATICEDGE 0x20000u
#define PM_REMOVE 1u
#define PM_NOREMOVE 0u
#define QS_KEY W32_QS_KEY
#define QS_MOUSEMOVE W32_QS_MOUSEMOVE
#define QS_MOUSEBUTTON W32_QS_MOUSEBUTTON
#define QS_POSTMESSAGE W32_QS_POSTMESSAGE
#define QS_TIMER W32_QS_TIMER
#define QS_PAINT W32_QS_PAINT
#define QS_SENDMESSAGE W32_QS_SENDMESSAGE
#define QS_HOTKEY W32_QS_HOTKEY
#define QS_INPUT 7u
#define QS_ALLINPUT 255u
#define MWMO_WAITALL 1u
#define MWMO_ALERTABLE 2u
#define MWMO_INPUTAVAILABLE 4u
#define GWL_WNDPROC (-4)
#define GWL_HINSTANCE (-6)
#define GWL_HWNDPARENT (-8)
#define GWL_ID (-12)
#define GWL_STYLE (-16)
#define GWL_EXSTYLE (-20)
#define GWL_USERDATA (-21)
#define SWP_NOSIZE 1u
#define SWP_NOMOVE 2u
#define SWP_NOZORDER 4u
#define SWP_NOACTIVATE 16u
#define SWP_SHOWWINDOW 64u
#define SWP_HIDEWINDOW 128u
#define HWND_TOP ((HWND)0)
#define HWND_BOTTOM ((HWND)(uintptr_t)1)
#define HWND_TOPMOST ((HWND)(intptr_t)-1)
#define HWND_NOTOPMOST ((HWND)(intptr_t)-2)
#define CF_TEXT 1u
#define CF_BITMAP 2u
#define CF_OEMTEXT 7u
#define CF_DIB 8u
#define CF_UNICODETEXT 13u

ATOM W32API RegisterClassA(const WNDCLASSA *);
ATOM W32API RegisterClassExA(const WNDCLASSEXA *);
BOOL W32API UnregisterClassA(const char *, HINSTANCE);
BOOL W32API GetClassInfoA(HINSTANCE, const char *, WNDCLASSA *);
BOOL W32API GetClassInfoExA(HINSTANCE, const char *, WNDCLASSEXA *);
HWND W32API CreateWindowExA(DWORD, const char *, const char *, DWORD, int, int,
                            int, int, HWND, HANDLE, HINSTANCE, void *);
BOOL W32API DestroyWindow(HWND);
BOOL W32API IsWindow(HWND);
BOOL W32API IsChild(HWND, HWND);
BOOL W32API IsWindowVisible(HWND);
BOOL W32API IsWindowEnabled(HWND);
BOOL W32API IsWindowUnicode(HWND);
BOOL W32API IsIconic(HWND);
BOOL W32API IsZoomed(HWND);
BOOL W32API EnableWindow(HWND, BOOL);
BOOL W32API ShowWindow(HWND, int);
BOOL W32API SetWindowPos(HWND, HWND, int, int, int, int, UINT);
BOOL W32API MoveWindow(HWND, int, int, int, int, BOOL);
LONG W32API GetWindowLongA(HWND, int);
LONG W32API SetWindowLongA(HWND, int, LONG);
uintptr_t W32API GetClassLongA(HWND, int);
uintptr_t W32API SetClassLongA(HWND, int, intptr_t);
uint16_t W32API GetWindowWord(HWND, int);
uint16_t W32API SetWindowWord(HWND, int, uint16_t);
int W32API GetWindowTextA(HWND, char *, int);
int W32API GetWindowTextLengthA(HWND);
BOOL W32API SetWindowTextA(HWND, const char *);
int W32API GetClassNameA(HWND, char *, int);
BOOL W32API EnumWindows(WNDENUMPROC, LPARAM);
BOOL W32API EnumChildWindows(HWND, WNDENUMPROC, LPARAM);
HWND W32API FindWindowA(const char *, const char *);
HWND W32API FindWindowExA(HWND, HWND, const char *, const char *);
HWND W32API GetParent(HWND);
HWND W32API SetParent(HWND, HWND);
HWND W32API GetWindow(HWND, UINT);
HWND W32API GetTopWindow(HWND);
HWND W32API GetLastActivePopup(HWND);
BOOL W32API BringWindowToTop(HWND);
BOOL W32API ShowOwnedPopups(HWND, BOOL);
DWORD W32API GetWindowThreadProcessId(HWND, DWORD *);
BOOL W32API SetPropA(HWND, const char *, HANDLE);
HANDLE W32API GetPropA(HWND, const char *);
HANDLE W32API RemovePropA(HWND, const char *);
LRESULT W32API DefWindowProcA(HWND, UINT, WPARAM, LPARAM);
LRESULT W32API CallWindowProcA(WNDPROC, HWND, UINT, WPARAM, LPARAM);
BOOL W32API PostMessageA(HWND, UINT, WPARAM, LPARAM);
BOOL W32API PostThreadMessageA(DWORD, UINT, WPARAM, LPARAM);
void W32API PostQuitMessage(int);
LRESULT W32API SendMessageA(HWND, UINT, WPARAM, LPARAM);
LRESULT W32API SendMessageTimeoutA(HWND, UINT, WPARAM, LPARAM, UINT, UINT,
                                   uintptr_t *);
BOOL W32API SendNotifyMessageA(HWND, UINT, WPARAM, LPARAM);
BOOL W32API ReplyMessage(LRESULT);
BOOL W32API InSendMessage(void);
BOOL W32API PeekMessageA(MSG *, HWND, UINT, UINT, UINT);
BOOL W32API GetMessageA(MSG *, HWND, UINT, UINT);
BOOL W32API TranslateMessage(const MSG *);
LRESULT W32API DispatchMessageA(const MSG *);
DWORD W32API MsgWaitForMultipleObjectsEx(DWORD, const HANDLE *, DWORD, DWORD,
                                         DWORD);
DWORD W32API MsgWaitForMultipleObjects(DWORD, const HANDLE *, BOOL, DWORD,
                                       DWORD);
DWORD W32API GetQueueStatus(UINT);
BOOL W32API WaitMessage(void);
LONG W32API GetMessageTime(void);
DWORD W32API GetMessagePos(void);
uintptr_t W32API SetTimer(HWND, uintptr_t, UINT, TIMERPROC);
BOOL W32API KillTimer(HWND, uintptr_t);
HWND W32API SetCapture(HWND);
BOOL W32API ReleaseCapture(void);
HWND W32API GetCapture(void);
HWND W32API SetFocus(HWND);
HWND W32API GetFocus(void);
HWND W32API SetActiveWindow(HWND);
HWND W32API GetActiveWindow(void);
HWND W32API GetForegroundWindow(void);
BOOL W32API SetForegroundWindow(HWND);
BOOL W32API GetWindowRect(HWND, RECT *);
BOOL W32API GetClientRect(HWND, RECT *);
BOOL W32API AdjustWindowRect(RECT *, DWORD, BOOL);
BOOL W32API AdjustWindowRectEx(RECT *, DWORD, BOOL, DWORD);
BOOL W32API ClientToScreen(HWND, POINT *);
BOOL W32API ScreenToClient(HWND, POINT *);
int W32API MapWindowPoints(HWND, HWND, POINT *, UINT);
HWND W32API WindowFromPoint(POINT);
BOOL W32API ClipCursor(const RECT *);
BOOL W32API GetClipCursor(RECT *);
BOOL W32API SetCursorPos(int, int);
BOOL W32API GetCursorPos(POINT *);
int W32API ShowCursor(BOOL);
HCURSOR W32API SetCursor(HCURSOR);
HCURSOR W32API GetCursor(void);
HCURSOR W32API LoadCursorA(HINSTANCE, const char *);
HCURSOR W32API CreateCursor(HINSTANCE, int, int, int, int, const void *,
                            const void *);
BOOL W32API DestroyCursor(HCURSOR);
HICON W32API LoadIconA(HINSTANCE, const char *);
HICON W32API CreateIconIndirect(const ICONINFO *);
BOOL W32API DestroyIcon(HICON);
BOOL W32API DrawIcon(HDC, int, int, HICON);
BOOL W32API DrawIconEx(HDC, int, int, HICON, int, int, UINT, HBRUSH, UINT);
HANDLE W32API SetWindowsHookExA(int, HOOKPROC, HINSTANCE, DWORD);
BOOL W32API UnhookWindowsHookEx(HANDLE);
LRESULT W32API CallNextHookEx(HANDLE, int, WPARAM, LPARAM);
BOOL W32API CallMsgFilterA(MSG *, int);
uintptr_t W32API GetKeyboardLayout(DWORD);
UINT W32API MapVirtualKeyA(UINT, UINT);
UINT W32API MapVirtualKeyW(UINT, UINT);
UINT W32API MapVirtualKeyExA(UINT, UINT, uintptr_t);
UINT W32API MapVirtualKeyExW(UINT, UINT, uintptr_t);
int16_t W32API VkKeyScanExA(char, uintptr_t);
int W32API ToAsciiEx(UINT, UINT, const uint8_t *, uint16_t *, UINT, uintptr_t);
DWORD W32API OemKeyScan(uint16_t);
int W32API GetKeyNameTextA(LONG, char *, int);
int W32API GetKeyNameTextW(LONG, WCHAR *, int);
int W32API GetKeyboardType(int);
int W32API GetKeyboardLayoutList(int, uintptr_t *);
BOOL W32API GetKeyboardLayoutNameA(char *);
BOOL W32API GetKeyboardLayoutNameW(WCHAR *);
uintptr_t W32API ActivateKeyboardLayout(uintptr_t, UINT);
int16_t W32API VkKeyScanA(char);
int W32API ToAscii(UINT, UINT, const uint8_t *, uint16_t *, UINT);
int16_t W32API GetKeyState(int);
int16_t W32API GetAsyncKeyState(int);
BOOL W32API GetKeyboardState(uint8_t *);
BOOL W32API SetKeyboardState(const uint8_t *);
UINT W32API GetDoubleClickTime(void);
BOOL W32API SetDoubleClickTime(UINT);
BOOL W32API InvalidateRect(HWND, const RECT *, BOOL);
BOOL W32API InvalidateRgn(HWND, HRGN, BOOL);
BOOL W32API ValidateRect(HWND, const RECT *);
BOOL W32API ValidateRgn(HWND, HRGN);
BOOL W32API GetUpdateRect(HWND, RECT *, BOOL);
int W32API GetUpdateRgn(HWND, HRGN, BOOL);
HDC W32API BeginPaint(HWND, PAINTSTRUCT *);
BOOL W32API EndPaint(HWND, const PAINTSTRUCT *);
HDC W32API GetDC(HWND);
HDC W32API GetDCEx(HWND, HRGN, DWORD);
HDC W32API GetWindowDC(HWND);
int W32API ReleaseDC(HWND, HDC);
BOOL W32API UpdateWindow(HWND);
BOOL W32API RedrawWindow(HWND, const RECT *, HRGN, UINT);
BOOL W32API LockWindowUpdate(HWND);
int W32API FillRect(HDC, const RECT *, HBRUSH);
int W32API FrameRect(HDC, const RECT *, HBRUSH);
BOOL W32API ScrollWindow(HWND, int, int, const RECT *, const RECT *);
int W32API ScrollWindowEx(HWND, int, int, const RECT *, const RECT *, HRGN,
                          RECT *, UINT);
BOOL W32API OpenClipboard(HWND);
BOOL W32API CloseClipboard(void);
BOOL W32API EmptyClipboard(void);
HANDLE W32API SetClipboardData(UINT, HANDLE);
HANDLE W32API GetClipboardData(UINT);
BOOL W32API IsClipboardFormatAvailable(UINT);
HWND W32API GetClipboardOwner(void);
HWND W32API SetClipboardViewer(HWND);
BOOL W32API ChangeClipboardChain(HWND, HWND);
int W32API GetSystemMetrics(int);
BOOL W32API SystemParametersInfoA(UINT, UINT, void *, UINT);
DWORD W32API GetSysColor(int);
BOOL W32API SetSysColors(int, const int *, const DWORD *);
BOOL W32API MessageBeep(UINT);
BOOL W32API ExitWindowsEx(UINT, DWORD);
UINT W32API RegisterWindowMessageA(const char *);
BOOL W32API SetRect(RECT *, int, int, int, int);
BOOL W32API SetRectEmpty(RECT *);
BOOL W32API CopyRect(RECT *, const RECT *);
BOOL W32API IsRectEmpty(const RECT *);
BOOL W32API EqualRect(const RECT *, const RECT *);
BOOL W32API IntersectRect(RECT *, const RECT *, const RECT *);
BOOL W32API UnionRect(RECT *, const RECT *, const RECT *);
BOOL W32API OffsetRect(RECT *, int, int);
BOOL W32API InflateRect(RECT *, int, int);
BOOL W32API PtInRect(const RECT *, POINT);
#endif
