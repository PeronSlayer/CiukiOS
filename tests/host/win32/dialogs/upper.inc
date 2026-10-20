/* f5-20B: the USER and GDI entry points the reused Wine 10.0 DLLs import,
 * over the production USER/GDI in the dialogs harness: the Unicode layer
 * (classes, windows, messages, subclassing, dialogs, menus, accelerators,
 * resources, text), the remaining USER APIs, the DDEML, and the GDI upper
 * set (regions, pens, alpha blending, fonts, Uniscribe subset, metafiles).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../../../../sdk/win32/user32/unicode.h"
/* ---- Unicode USER prototypes (the PE32 export set) ---------------------- */
ATOM RegisterClassW(const WNDCLASSW *);
ATOM RegisterClassExW(const WNDCLASSEXW *);
BOOL UnregisterClassW(const WCHAR *, HINSTANCE);
BOOL GetClassInfoW(HINSTANCE, const WCHAR *, WNDCLASSW *);
BOOL GetClassInfoExW(HINSTANCE, const WCHAR *, WNDCLASSEXW *);
int GetClassNameW(HWND, WCHAR *, int);
uintptr_t GetClassLongW(HWND, int);
int GetWindowTextLengthW(HWND);
int InternalGetWindowText(HWND, WCHAR *, int);
HWND FindWindowW(const WCHAR *, const WCHAR *);
HWND FindWindowExW(HWND, HWND, const WCHAR *, const WCHAR *);
BOOL SetPropW(HWND, const WCHAR *, HANDLE);
HANDLE GetPropW(HWND, const WCHAR *);
HANDLE RemovePropW(HWND, const WCHAR *);
UINT RegisterWindowMessageW(const WCHAR *);
LRESULT SendMessageTimeoutW(HWND, UINT, WPARAM, LPARAM, UINT, UINT, uintptr_t *);
BOOL SendNotifyMessageW(HWND, UINT, WPARAM, LPARAM);
BOOL PostThreadMessageW(DWORD, UINT, WPARAM, LPARAM);
BOOL PeekMessageW(MSG *, HWND, UINT, UINT, UINT);
BOOL GetMessageW(MSG *, HWND, UINT, UINT);
LRESULT DispatchMessageW(const MSG *);
HANDLE SetWindowsHookExW(int, HOOKPROC, HINSTANCE, DWORD);
HWND CreateDialogIndirectParamW(HINSTANCE, const void *, HWND, DLGPROC, LPARAM);
HWND CreateDialogParamW(HINSTANCE, const WCHAR *, HWND, DLGPROC, LPARAM);
intptr_t DialogBoxParamW(HINSTANCE, const WCHAR *, HWND, DLGPROC, LPARAM);
HACCEL LoadAcceleratorsW(HINSTANCE, const WCHAR *);
HANDLE LoadBitmapW(HINSTANCE, const WCHAR *);
HANDLE LoadImageW(HINSTANCE, const WCHAR *, UINT, int, int, UINT);
int MessageBoxW(HWND, const WCHAR *, const WCHAR *, UINT);
intptr_t DialogBoxIndirectParamW(HINSTANCE, const void *, HWND, DLGPROC, LPARAM);
BOOL IsDialogMessageW(HWND, MSG *);
UINT GetDlgItemTextW(HWND, int, WCHAR *, int);
BOOL SetDlgItemTextW(HWND, int, const WCHAR *);
LRESULT SendDlgItemMessageW(HWND, int, UINT, WPARAM, LPARAM);
BOOL AppendMenuW(HMENU, UINT, uintptr_t, const WCHAR *);
BOOL InsertMenuW(HMENU, UINT, UINT, uintptr_t, const WCHAR *);
BOOL ModifyMenuW(HMENU, UINT, UINT, uintptr_t, const WCHAR *);
BOOL InsertMenuItemW(HMENU, UINT, BOOL, const MENUITEMINFOW *);
BOOL SetMenuItemInfoW(HMENU, UINT, BOOL, const MENUITEMINFOW *);
BOOL GetMenuItemInfoW(HMENU, UINT, BOOL, MENUITEMINFOW *);
int GetMenuStringW(HMENU, UINT, WCHAR *, int, UINT);
HMENU LoadMenuW(HINSTANCE, const WCHAR *);
BOOL GetMenuInfo(HMENU, MENUINFO *);
BOOL SetMenuInfo(HMENU, const MENUINFO *);
BOOL GetMenuItemRect(HWND, HMENU, UINT, RECT *);
HACCEL CreateAcceleratorTableW(ACCEL *, int);
int CopyAcceleratorTableW(HACCEL, ACCEL *, int);
int TranslateAcceleratorW(HWND, HACCEL, MSG *);
int MessageBoxIndirectW(const MSGBOXPARAMSW *);
DWORD GetTabbedTextExtentW(HDC, const WCHAR *, int, int, const int *);
int16_t VkKeyScanW(WCHAR);
WCHAR *CharUpperW(WCHAR *);
WCHAR *CharLowerW(WCHAR *);
DWORD CharUpperBuffW(WCHAR *, DWORD);
DWORD CharLowerBuffW(WCHAR *, DWORD);
WCHAR *CharNextW(const WCHAR *);
WCHAR *CharPrevW(const WCHAR *, const WCHAR *);
char *CharUpperA(char *);
char *CharNextA(const char *);
char *CharPrevA(const char *, const char *);
BOOL CharToOemW(const WCHAR *, char *);
BOOL OemToCharW(const char *, WCHAR *);
BOOL CharToOemA(const char *, char *);
BOOL OemToCharA(const char *, char *);
BOOL IsCharAlphaW(WCHAR);
BOOL IsCharAlphaNumericW(WCHAR);
BOOL IsCharUpperW(WCHAR);
BOOL IsCharLowerW(WCHAR);
int wsprintfA(char *, const char *, ...);
int wsprintfW(WCHAR *, const WCHAR *, ...);
UINT DdeInitializeW(DWORD *, void *, DWORD, DWORD);
UINT DdeInitializeA(DWORD *, void *, DWORD, DWORD);
BOOL DdeUninitialize(DWORD);
uintptr_t DdeCreateStringHandleW(DWORD, const WCHAR *, int);
uintptr_t DdeCreateStringHandleA(DWORD, const char *, int);
BOOL DdeFreeStringHandle(DWORD, uintptr_t);
DWORD DdeQueryStringW(DWORD, uintptr_t, WCHAR *, DWORD, int);
DWORD DdeQueryStringA(DWORD, uintptr_t, char *, DWORD, int);
UINT DdeGetLastError(DWORD);
uintptr_t DdeConnect(DWORD, uintptr_t, uintptr_t, void *);
BOOL DdeDisconnect(uintptr_t);
uintptr_t DdeClientTransaction(void *, DWORD, uintptr_t, uintptr_t, UINT, UINT,
                               DWORD, DWORD *);
uintptr_t DdeNameService(DWORD, uintptr_t, uintptr_t, UINT);
uintptr_t DdeCreateDataHandle(DWORD, const void *, DWORD, DWORD, uintptr_t,
                              UINT, UINT);
DWORD DdeGetData(uintptr_t, void *, DWORD, DWORD);
BOOL DdeFreeDataHandle(uintptr_t);
HWND GetDesktopWindow(void);
HWND GetAncestor(HWND, UINT);
HWND ChildWindowFromPoint(HWND, POINT);
HWND ChildWindowFromPointEx(HWND, POINT, UINT);
BOOL GetWindowInfo(HWND, WINDOWINFO *);
int SetWindowRgn(HWND, HANDLE, BOOL);
int GetWindowRgn(HWND, HANDLE);
HANDLE BeginDeferWindowPos(int);
HANDLE DeferWindowPos(HANDLE, HWND, HWND, int, int, int, int, UINT);
BOOL EndDeferWindowPos(HANDLE);
DWORD WaitForInputIdle(HANDLE, DWORD);
BOOL WinHelpW(HWND, const WCHAR *, UINT, uintptr_t);
HANDLE MonitorFromRect(const RECT *, DWORD);
HANDLE MonitorFromPoint(POINT, DWORD);
HANDLE MonitorFromWindow(HWND, DWORD);
BOOL GetMonitorInfoW(HANDLE, void *);
BOOL GetMonitorInfoA(HANDLE, void *);
UINT GetDpiForWindow(HWND);
BOOL IsProcessDPIAware(void);
BOOL GetDpiForMonitorInternal(HANDLE, UINT, UINT *, UINT *);
BOOL SetProcessDpiAwarenessInternal(int);
void NotifyWinEvent(DWORD, HWND, LONG, LONG);
BOOL DragDetect(HWND, POINT);
BOOL TrackMouseEvent(TRACKMOUSEEVENT *);
BOOL InvertRect(HDC, const RECT *);
BOOL SubtractRect(RECT *, const RECT *, const RECT *);
uintptr_t SetSystemTimer(HWND, uintptr_t, UINT, TIMERPROC);
BOOL KillSystemTimer(HWND, uintptr_t);
UINT RegisterClipboardFormatA(const char *);
UINT RegisterClipboardFormatW(const WCHAR *);
int GetClipboardFormatNameA(UINT, char *, int);
int GetClipboardFormatNameW(UINT, WCHAR *, int);
UINT EnumClipboardFormats(UINT);
DWORD GetClipboardSequenceNumber(void);
HICON CopyIcon(HICON);
BOOL GetIconInfo(HICON, ICONINFO *);
HICON CreateIconFromResourceEx(const uint8_t *, DWORD, BOOL, DWORD, int, int,
                               UINT);
HANDLE CopyImage(HANDLE, UINT, int, int, UINT);
UINT PrivateExtractIconsW(const WCHAR *, int, int, int, HICON *, UINT *, UINT,
                          UINT);
UINT PrivateExtractIconExW(const WCHAR *, int, HICON *, HICON *, UINT);
int DlgDirListW(HWND, WCHAR *, int, int, UINT);
int DlgDirListComboBoxW(HWND, WCHAR *, int, int, UINT);
BOOL DrawStateW(HDC, HBRUSH, DRAWSTATEPROC, LPARAM, WPARAM, int, int, int, int,
                UINT);
LONG TabbedTextOutW(HDC, int, int, const WCHAR *, int, int, const int *, int);
/* ---- GDI prototypes (DWORD handles, as across the PE boundary) ---------- */
typedef DWORD GH;
typedef struct {
  DWORD lopnStyle;
  POINT lopnWidth;
  DWORD lopnColor;
} GLOGPEN;
typedef struct {
  uint8_t peRed, peGreen, peBlue, peFlags;
} GPALENTRY;
typedef struct {
  DWORD biSize;
  LONG biWidth, biHeight;
  uint16_t biPlanes, biBitCount;
  DWORD biCompression, biSizeImage;
  LONG biXPelsPerMeter, biYPelsPerMeter;
  DWORD biClrUsed, biClrImportant;
} GBIH;
typedef struct {
  LONG bmType, bmWidth, bmHeight, bmWidthBytes;
  uint16_t bmPlanes, bmBitsPixel;
  void *bmBits;
} GBITMAP;
GH CreateCompatibleDC(GH);
BOOL DeleteDC(GH);
GH SelectObject(GH, GH);
BOOL DeleteObject(GH);
DWORD GetObjectType(GH);
int GetObjectA(GH, int, void *);
GH CreateDIBSection(GH, const void *, unsigned, void **, void *, DWORD);
GH CreateBitmap(int, int, unsigned, unsigned, const void *);
GH CreateSolidBrush(DWORD);
GH CreatePen(int, int, DWORD);
GH CreateRectRgn(int, int, int, int);
int GetRgnBox(GH, RECT *);
BOOL PtInRegion(GH, int, int);
GH CreateRoundRectRgn(int, int, int, int, int, int);
GH ExtCreatePen(DWORD, DWORD, const void *, DWORD, const DWORD *);
BOOL PolyPolyline(GH, const POINT *, const DWORD *, DWORD);
BOOL GdiAlphaBlend(GH, int, int, int, int, GH, int, int, int, int, DWORD);
LONG GetBitmapBits(GH, LONG, void *);
DWORD GetLayout(GH);
GH CreateHalftonePalette(GH);
unsigned GetPaletteEntries(GH, unsigned, unsigned, void *);
void *CreateColorSpaceW(const void *);
BOOL DeleteColorSpace(void *);
int StartDocW(GH, const void *);
int ExtEscape(GH, int, int, const char *, int, char *);
void *GdiConvertToDevmodeW(const uint8_t *);
typedef struct {
  unsigned ciCharset, ciACP;
  DWORD fsUsb[4], fsCsb[2];
} CSI;
BOOL TranslateCharsetInfo(DWORD *, CSI *, DWORD);
unsigned GdiGetCodePage(GH);
LONG GdiGetCharDimensions(GH, void *, LONG *);
BOOL GetCharWidthInfo(GH, int *);
int GetTextCharsetInfo(GH, DWORD *, DWORD);
DWORD GetCharacterPlacementW(GH, const WCHAR *, int, int, void *, DWORD);
BOOL GetTextExtentPoint32W(GH, const WCHAR *, int, LONG *);
BOOL GetCharWidth32W(GH, unsigned, unsigned, int *);
int EnumFontFamiliesW(GH, const WCHAR *, void *, intptr_t);
int EnumFontFamiliesExW(GH, void *, void *, intptr_t, DWORD);
int EnumFontFamiliesA(GH, const char *, void *, intptr_t);
GH CreateFontA(int, int, int, int, int, DWORD, DWORD, DWORD, DWORD, DWORD,
               DWORD, DWORD, DWORD, const char *);
DWORD gdi32_pixel(GH, int, int);
DWORD GetPixel(GH, int, int);
DWORD SetPixel(GH, int, int, DWORD);
BOOL PatBlt(GH, int, int, int, int, DWORD);
BOOL Rectangle(GH, int, int, int, int);
BOOL Ellipse(GH, int, int, int, int);
BOOL MoveToEx(GH, int, int, POINT *);
BOOL LineTo(GH, int, int);
BOOL Polygon(GH, const POINT *, int);
BOOL TextOutW(GH, int, int, const WCHAR *, int);
int SetMapMode(GH, int);
BOOL SetWindowExtEx(GH, int, int, void *);
BOOL SetViewportExtEx(GH, int, int, void *);
GH GetStockObject(int);
GH CreateMetaFileW(const WCHAR *);
GH CloseMetaFile(GH);
DWORD GetMetaFileBitsEx(GH, DWORD, void *);
GH SetMetaFileBitsEx(DWORD, const void *);
GH CopyMetaFileW(GH, const WCHAR *);
BOOL DeleteMetaFile(GH);
BOOL PlayMetaFile(GH, GH);
GH SetEnhMetaFileBits(DWORD, const void *);
DWORD GetEnhMetaFileBits(GH, DWORD, void *);
DWORD GetEnhMetaFileHeader(GH, DWORD, void *);
GH CopyEnhMetaFileW(GH, const WCHAR *);
BOOL DeleteEnhMetaFile(GH);
BOOL PlayEnhMetaFile(GH, GH, const RECT *);
DWORD GetWinMetaFileBits(GH, DWORD, void *, int, GH);
GH SetWinMetaFileBits(DWORD, const void *, GH, const void *);
int ScriptItemize(const WCHAR *, int, int, const void *, const void *, void *,
                  int *);
int ScriptShape(GH, void **, const WCHAR *, int, int, void *, uint16_t *,
                uint16_t *, void *, int *);
int ScriptPlace(GH, void **, const uint16_t *, int, const void *, void *, int *,
                void *, void *);
int ScriptTextOut(GH, void **, int, int, unsigned, const RECT *, const void *,
                  const WCHAR *, int, const uint16_t *, int, const int *,
                  const int *, const void *);
int ScriptCPtoX(int, BOOL, int, int, const uint16_t *, const void *, const int *,
                const void *, int *);
int ScriptXtoCP(int, int, int, const uint16_t *, const void *, const int *,
                const void *, int *, int *);
int ScriptBreak(const WCHAR *, int, const void *, uint8_t *);
int ScriptLayout(int, const uint8_t *, int *, int *);
int ScriptFreeCache(void **);
int ScriptStringAnalyse(GH, const void *, int, int, int, DWORD, int,
                        const void *, const void *, const int *, const void *,
                        const uint8_t *, void **);
int ScriptStringOut(void *, int, int, unsigned, const RECT *, int, int, BOOL);
int ScriptStringCPtoX(void *, int, BOOL, int *);
int ScriptStringXtoCP(void *, int, int *, int *);
int ScriptStringGetLogicalWidths(void *, int *);
const LONG *ScriptString_pSize(void *);
const int *ScriptString_pcOutChars(void *);
int ScriptStringFree(void **);

static const WCHAR *L(const char *s) {
  static WCHAR pool[16][300];
  static unsigned at;
  WCHAR *w = pool[at++ % 16];
  int n = MultiByteToWideChar(1252, 0, s, -1, w, 300);
  CHECK(n > 0);
  return w;
}
static int wide_is(const WCHAR *w, const char *a) {
  return !memcmp(w, L(a), (strlen(a) + 1) * 2);
}

/* ---- Unicode windows and messages ---------------------------------------- */
static UINT wide_msgs[64], wide_count;
static WCHAR wide_text[64];
static WPARAM wide_char;
static int created_ok;
static LRESULT W32API wide_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  if (wide_count < 64)
    wide_msgs[wide_count++] = m;
  if (m == WM_NCCREATE || m == WM_CREATE) {
    const CREATESTRUCTW *cs = (const CREATESTRUCTW *)lp;
    created_ok += cs->lpszName && wide_is(cs->lpszName, "T\xeftle") &&
                  cs->lpszClass && wide_is(cs->lpszClass, "F5 Wide");
  }
  if (m == WM_SETTEXT && lp)
    memcpy(wide_text, (const WCHAR *)lp,
           (u32_wlen((const WCHAR *)lp) + 1) * 2);
  if (m == WM_CHAR)
    wide_char = wp;
  if (m == WM_USER + 5)
    return IsWindowUnicode(h) ? 0x55 : 0;
  return DefWindowProcW(h, m, wp, lp);
}
static WPARAM ansi_char;
static uintptr_t ansi_previous;
static char ansi_text[64];
static LRESULT W32API ansi_subclass(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  if (m == WM_CHAR)
    ansi_char = wp;
  if (m == WM_SETTEXT && lp)
    snprintf(ansi_text, sizeof(ansi_text), "%s", (const char *)lp);
  return CallWindowProcA((WNDPROC)ansi_previous, h, m, wp, lp);
}
static WPARAM hook_char;
static LRESULT W32API wide_hook(int code, WPARAM wp, LPARAM lp) {
  const MSG *m = (const MSG *)lp;
  if (m && m->message == WM_CHAR)
    hook_char = m->wParam;
  return CallNextHookEx(0, code, wp, lp);
}
static void unicode_windows(void) {
  WNDCLASSW c = {.lpfnWndProc = wide_proc,
                 .hInstance = FIXTURE,
                 .lpszClassName = L("F5 Wide")};
  ATOM atom = RegisterClassW(&c);
  CHECK(atom);
  CHECK(!RegisterClassW(&c) && GetLastError() == 1410);
  WNDCLASSW back;
  CHECK(GetClassInfoW(FIXTURE, L("F5 Wide"), &back) &&
        back.lpfnWndProc == wide_proc && wide_is(back.lpszClassName, "F5 Wide"));
  HWND h = CreateWindowExW(0, L("F5 Wide"), L("T\xeftle"), WS_POPUP, 10, 10,
                           120, 80, 0, 0, FIXTURE, 0);
  CHECK(h && created_ok == 2 && IsWindowUnicode(h));
  WCHAR text[64];
  char a[64];
  CHECK(GetWindowTextW(h, text, 64) == 5 && wide_is(text, "T\xeftle"));
  CHECK(GetWindowTextA(h, a, 64) == 5 && !strcmp(a, "T\xeftle"));
  CHECK(GetWindowTextLengthW(h) == 5);
  CHECK(InternalGetWindowText(h, text, 3) == 2 && wide_is(text, "T\xef"));
  CHECK(GetClassNameW(h, text, 64) == 7 && wide_is(text, "F5 Wide"));
  /* An ANSI sender reaches the Unicode procedure in Unicode form. */
  CHECK(SendMessageA(h, WM_SETTEXT, 0, (LPARAM) "caf\xe9 \x80") &&
        wide_is(wide_text, "caf\xe9 \x80") && wide_text[5] == 0x20ac);
  CHECK(SetWindowTextW(h, L("\xc0" "bc")) && wide_is(wide_text, "\xc0" "bc"));
  CHECK(SendMessageW(h, WM_USER + 5, 0, 0) == 0x55);
  /* A character outside code page 1252 crosses the ANSI core as '?'. */
  WCHAR outside[3] = {0x15c, 'x', 0};
  CHECK(SetWindowTextW(h, outside) && GetWindowTextA(h, a, 64) == 2 &&
        !strcmp(a, "?x"));
  /* Queues hold the ANSI form; GetMessageW/DispatchMessageW translate. */
  HANDLE hook = SetWindowsHookExW(3, wide_hook, 0, GetCurrentThreadId());
  CHECK(hook);
  CHECK(PostMessageW(h, WM_CHAR, 0x20ac, 1));
  MSG msg;
  CHECK(PeekMessageW(&msg, h, WM_CHAR, WM_CHAR, 1) && msg.wParam == 0x20ac &&
        hook_char == 0x20ac);
  CHECK(DispatchMessageW(&msg) == 0 && wide_char == 0x20ac);
  CHECK(PostMessageW(h, WM_CHAR, 0xe9, 1) &&
        PeekMessageA(&msg, h, WM_CHAR, WM_CHAR, 1) && msg.wParam == 0xe9);
  CHECK(UnhookWindowsHookEx(hook));
  wide_char = 0;
  CHECK(SendMessageA(h, WM_CHAR, 0x80, 1) == 0 && wide_char == 0x20ac);
  /* Subclassing with an ANSI procedure: the old one is a handle that
   * CallWindowProcA converts back to Unicode. */
  ansi_previous = (uintptr_t)(DWORD)SetWindowLongA(h, GWL_WNDPROC,
                                                   (LONG)(uintptr_t)ansi_subclass);
  CHECK((ansi_previous >> 16) == 0xffff && !IsWindowUnicode(h));
  CHECK((WNDPROC)(uintptr_t)GetWindowLongA(h, GWL_WNDPROC) == ansi_subclass);
  CHECK(((DWORD)GetWindowLongW(h, GWL_WNDPROC) >> 16) == 0xffff);
  CHECK(SendMessageW(h, WM_CHAR, 0x20ac, 1) == 0 && ansi_char == 0x80 &&
        wide_char == 0x20ac);
  CHECK(SendMessageW(h, WM_SETTEXT, 0, (LPARAM)L("\xe9t\xe9")) &&
        !strcmp(ansi_text, "\xe9t\xe9") && wide_is(wide_text, "\xe9t\xe9"));
  CHECK(CallWindowProcW((WNDPROC)(uintptr_t)(DWORD)GetWindowLongW(h, GWL_WNDPROC),
                        h, WM_USER + 5, 0, 0) == 0);
  /* Restoring the handle makes the window Unicode again. */
  SetWindowLongA(h, GWL_WNDPROC, (LONG)ansi_previous);
  CHECK(IsWindowUnicode(h) &&
        (WNDPROC)(uintptr_t)GetWindowLongW(h, GWL_WNDPROC) == wide_proc);
  /* Properties, messages, lookups. */
  CHECK(SetPropW(h, L("Pr\xf6p"), (HANDLE)7) &&
        GetPropW(h, L("Pr\xf6p")) == (HANDLE)7 &&
        GetPropA(h, "Pr\xf6p") == (HANDLE)7 &&
        RemovePropW(h, L("Pr\xf6p")) == (HANDLE)7);
  CHECK(RegisterWindowMessageW(L("F5 Message")) ==
            RegisterWindowMessageA("F5 Message") &&
        RegisterWindowMessageA("F5 Message") >= 0xc000);
  CHECK(FindWindowW(L("F5 Wide"), 0) == h &&
        FindWindowExW(0, 0, 0, L("\xe9t\xe9")) == h && !FindWindowW(0, outside));
  uintptr_t result = 0;
  CHECK(SendMessageTimeoutW(h, WM_USER + 5, 0, 0, 0, 100, &result) && result == 0x55);
  CHECK(SendNotifyMessageW(h, WM_USER + 6, 0, 0));
  /* Class long and info for the predefined (ANSI) EDIT class. */
  WNDCLASSEXW edit = {.cbSize = sizeof(edit)};
  CHECK(GetClassInfoExW(0, L("Edit"), &edit) &&
        (((uintptr_t)edit.lpfnWndProc) >> 16) == 0xffff);
  HWND e = CreateWindowExW(0, L("EDIT"), L("x"), WS_CHILD | WS_VISIBLE, 0, 0,
                           60, 20, h, 0, FIXTURE, 0);
  CHECK(e && !IsWindowUnicode(e));
  CHECK(CallWindowProcW(edit.lpfnWndProc, e, WM_SETTEXT, 0, (LPARAM)L("\xe0\xe8")) &&
        GetWindowTextW(e, text, 64) == 2 && wide_is(text, "\xe0\xe8"));
  SendMessageW(e, EM_SETSEL, 2, 2);
  SendMessageW(e, EM_REPLACESEL, 0, (LPARAM)L("\x80!"));
  CHECK(GetWindowTextW(e, text, 64) == 4 && text[2] == 0x20ac);
  CHECK(GetWindowTextW(e, text, 3) == 2 && !text[2]);
  WCHAR line[16] = {16};
  CHECK(SendMessageW(e, EM_GETLINE, 0, (LPARAM)line) == 4 && line[2] == 0x20ac);
  HWND list = CreateWindowExA(0, "LISTBOX", "", WS_CHILD | LBS_HASSTRINGS, 0,
                              30, 60, 60, h, (HANDLE)9, FIXTURE, 0);
  CHECK(SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)L("z\xfc")) == 0);
  CHECK(SendMessageW(list, LB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)L("Z\xdc")) == 0);
  CHECK(SendMessageW(list, LB_GETTEXTLEN, 0, 0) == 2 &&
        SendMessageW(list, LB_GETTEXT, 0, (LPARAM)text) == 2 && text[1] == 0xfc);
  CHECK(SendMessageA(list, LB_GETTEXT, 0, (LPARAM)a) == 2 && !strcmp(a, "z\xfc"));
  /* SystemParametersInfoW structures. */
  uint8_t ncm[500];
  memset(ncm, 0, sizeof(ncm));
  w32_put32(ncm, 500);
  CHECK(SystemParametersInfoW(41, 500, ncm, 0) &&
        wide_is((const WCHAR *)(ncm + 24 + 28), "MS Sans Serif"));
  w32_put32(ncm, 340);
  CHECK(!SystemParametersInfoW(41, 340, ncm, 0) && GetLastError() == 87);
  uint8_t lf[92];
  CHECK(SystemParametersInfoW(31, 92, lf, 0) &&
        wide_is((const WCHAR *)(lf + 28), "MS Sans Serif"));
  DestroyWindow(h);
  CHECK(UnregisterClassW(L("F5 Wide"), FIXTURE));
  puts("PASS f5-20B unicode classes windows messages subclassing controls SPI");
}

/* ---- Unicode dialogs ---------------------------------------------------- */
static UINT wide_dialog_init;
static WCHAR wide_dialog_text[32];
static intptr_t W32API wide_dialog_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  (void)wp;
  if (m == WM_INITDIALOG) {
    wide_dialog_init = (UINT)lp;
    return 1;
  }
  if (m == WM_SETTEXT && lp)
    memcpy(wide_dialog_text, (const WCHAR *)lp,
           (u32_wlen((const WCHAR *)lp) + 1) * 2);
  if (m == WM_COMMAND && (wp & 0xffff) == IDOK) {
    EndDialog(h, 77);
    return 1;
  }
  return 0;
}
static void wide_dialog_script(HWND dlg, UINT why, UINT n) {
  (void)why, (void)n;
  if (dlg)
    PostMessageW(dlg, WM_COMMAND, IDOK, 0);
}
static void unicode_dialogs(void) {
  DWORD size;
  const uint8_t *tmpl = resource(5, 102, &size);
  HWND dlg = CreateDialogIndirectParamW(FIXTURE, tmpl, 0, wide_dialog_proc, 1234);
  CHECK(dlg && wide_dialog_init == 1234 && IsWindowUnicode(dlg));
  CHECK((DLGPROC)(uintptr_t)GetWindowLongW(dlg, DWL_DLGPROC) == wide_dialog_proc);
  CHECK(((DWORD)GetWindowLongA(dlg, DWL_DLGPROC) >> 16) == 0xffff);
  CHECK(SendMessageA(dlg, WM_SETTEXT, 0, (LPARAM) "\xc5ngstr\xf6m") &&
        wide_is(wide_dialog_text, "\xc5ngstr\xf6m"));
  WCHAR text[32];
  CHECK(GetWindowTextW(dlg, text, 32) == 8 && wide_is(text, "\xc5ngstr\xf6m"));
  /* Items through the Unicode helpers. */
  HWND first = GetNextDlgTabItem(dlg, 0, 0);
  int id = first ? GetDlgCtrlID(first) : 0;
  if (id) {
    CHECK(SetDlgItemTextW(dlg, id, L("\xbfS\xed?")) &&
          GetDlgItemTextW(dlg, id, text, 32) == 4 && wide_is(text, "\xbfS\xed?"));
    CHECK(SendDlgItemMessageW(dlg, id, WM_GETTEXTLENGTH, 0, 0) == 4);
  }
  MSG msg = {dlg, WM_CHAR, 0x20ac, 1, 0, {0, 0}};
  CHECK(IsDialogMessageW(dlg, &msg));
  DestroyWindow(dlg);
  HWND owner = owner_window("Wide owner", WS_CAPTION, 300, 200);
  script(wide_dialog_script);
  CHECK(DialogBoxIndirectParamW(FIXTURE, tmpl, owner, wide_dialog_proc, 5) == 77);
  script(0);
  DestroyWindow(owner);
  puts("PASS f5-20B unicode dialogs DefDlgProcW DWL_DLGPROC items modal");
}

/* ---- menus, accelerators, resources -------------------------------------- */
static void unicode_menus(void) {
  HMENU m = CreatePopupMenu();
  CHECK(AppendMenuW(m, MF_STRING, 10, L("&Caf\xe9")));
  CHECK(InsertMenuW(m, 0, MF_BYPOSITION | MF_STRING, 11, L("\x80uro")));
  WCHAR text[32];
  CHECK(GetMenuStringW(m, 10, text, 32, MF_BYCOMMAND) == 5 &&
        wide_is(text, "&Caf\xe9"));
  CHECK(GetMenuStringW(m, 0, text, 32, MF_BYPOSITION) == 4 && text[0] == 0x20ac);
  CHECK(ModifyMenuW(m, 10, MF_BYCOMMAND | MF_STRING, 10, L("Th\xe9")));
  MENUITEMINFOW mi = {.cbSize = sizeof(mi), .fMask = 0x40 /* MIIM_STRING */};
  CHECK(GetMenuItemInfoW(m, 10, 0, &mi) && mi.cch == 3);
  mi.dwTypeData = text;
  mi.cch = 32;
  CHECK(GetMenuItemInfoW(m, 10, 0, &mi) && mi.cch == 3 && wide_is(text, "Th\xe9"));
  mi.cch = 2;
  CHECK(GetMenuItemInfoW(m, 10, 0, &mi) && mi.cch == 1 && text[1] == 0);
  MENUITEMINFOW add = {.cbSize = sizeof(add),
                       .fMask = MIIM_ID | 0x40,
                       .wID = 12,
                       .dwTypeData = (WCHAR *)L("N\xe4u")};
  CHECK(InsertMenuItemW(m, 2, 1, &add));
  CHECK(GetMenuStringW(m, 12, text, 32, MF_BYCOMMAND) == 3 && text[1] == 0xe4);
  add.dwTypeData = (WCHAR *)L("Neu");
  CHECK(SetMenuItemInfoW(m, 12, 0, &add) && GetMenuStringW(m, 12, text, 32, 0) == 3);
  MENUITEMINFOW sep = {.cbSize = sizeof(sep) - sizeof(HANDLE), .fMask = MIIM_TYPE,
                       .fType = MF_SEPARATOR};
  CHECK(InsertMenuItemW(m, 3, 1, &sep));
  /* MENUINFO, also applied to submenus. */
  HMENU bar = CreateMenu(), sub = CreatePopupMenu();
  CHECK(AppendMenuW(bar, MF_POPUP, (uintptr_t)sub, L("&File")) &&
        AppendMenuW(sub, MF_STRING, 1, L("&Open")));
  MENUINFO info = {.cbSize = sizeof(info), .fMask = MIM_MENUDATA | MIM_STYLE | MIM_APPLYTOSUBMENUS,
                   .dwStyle = 0x04000000, .dwMenuData = 0x1234};
  CHECK(SetMenuInfo(bar, &info));
  MENUINFO got = {.cbSize = sizeof(got), .fMask = MIM_MENUDATA | MIM_STYLE};
  CHECK(GetMenuInfo(sub, &got) && got.dwMenuData == 0x1234 && got.dwStyle == 0x04000000);
  got.cbSize = 4;
  CHECK(!GetMenuInfo(sub, &got) && GetLastError() == 87);
  CHECK(!GetMenuInfo((HMENU)(uintptr_t)0x1234, &info) && GetLastError() == 1401);
  /* Menu bar item rectangles. */
  HWND w = CreateWindowExA(0, "F5 Owner", "Menu", WS_CAPTION | WS_VISIBLE, 40,
                           50, 300, 200, 0, bar, FIXTURE, 0);
  CHECK(w);
  drain(0);
  RECT r;
  CHECK(GetMenuItemRect(w, bar, 0, &r) && r.right > r.left && r.bottom > r.top);
  CHECK(!GetMenuItemRect(w, sub, 0, &r) && GetLastError() == 1401);
  CHECK(!GetMenuItemRect(w, bar, 5, &r));
  DestroyWindow(w);
  DestroyMenu(m);
  /* Accelerators: a character entry keeps the code-page character. */
  ACCEL accel[2] = {{0, 0xe9, 501}, {FVIRTKEY | FCONTROL, 'S', 502}};
  HACCEL table = CreateAcceleratorTableW(accel, 2);
  CHECK(table);
  ACCEL out[2];
  CHECK(CopyAcceleratorTableW(table, out, 2) == 2 && out[0].key == 0xe9 &&
        out[1].key == 'S');
  ACCEL euro[1] = {{0, 0x20ac, 503}};
  HACCEL table2 = CreateAcceleratorTableW(euro, 1);
  CHECK(CopyAcceleratorTableA(table2, out, 1) == 1 && out[0].key == 0x80);
  HWND owner = owner_window("Accel", WS_CAPTION, 200, 100);
  owner_last_command = 0;
  MSG msg = {owner, WM_CHAR, 0x20ac, 1, 0, {0, 0}};
  CHECK(TranslateAcceleratorW(owner, table2, &msg) == 1 &&
        owner_last_command == (503 | (1u << 16)));
  DestroyAcceleratorTable(table);
  DestroyAcceleratorTable(table2);
  /* Resources by Unicode name and the menu template loader. */
  CHECK(LoadMenuW(FIXTURE, (const WCHAR *)(uintptr_t)200));
  CHECK(!LoadMenuW(FIXTURE, L("NOPE")));
  HACCEL loaded = LoadAcceleratorsW(FIXTURE, (const WCHAR *)(uintptr_t)300);
  ACCEL four[4];
  CHECK(loaded && CopyAcceleratorTableW(loaded, four, 4) == 4 && four[3].key == 'x' &&
        DestroyAcceleratorTable(loaded));
  CHECK(!LoadAcceleratorsW(FIXTURE, L("NOPE")) && GetLastError() == 1814);
  HANDLE checker = LoadBitmapW(FIXTURE, (const WCHAR *)(uintptr_t)400);
  GBITMAP bm;
  CHECK(checker && GetObjectA((GH)(uintptr_t)checker, sizeof(bm), &bm) && bm.bmWidth == 4);
  DeleteObject((GH)(uintptr_t)checker);
  CHECK(!LoadBitmapW(FIXTURE, L("NOPE")) && GetLastError() == 1814);
  /* LoadImageW from a .BMP file (BITMAPFILEHEADER + the checker DIB). */
  DWORD dib_size;
  const uint8_t *dib = resource(2, 400, &dib_size);
  char path[1100];
  snprintf(path, sizeof(path), "%s/CHECK\xc9.BMP", drive_dir);
  FILE *f = fopen(path, "wb");
  uint8_t header[14] = {'B', 'M'};
  w32_put32(header + 2, 14 + dib_size);
  w32_put32(header + 10, 14 + 40);
  CHECK(f && fwrite(header, 1, 14, f) == 14 && fwrite(dib, 1, dib_size, f) == dib_size);
  fclose(f);
  HANDLE image = LoadImageW(0, L("C:\\CHECK\xc9.BMP"), 0, 0, 0, 0x10);
  CHECK(image && GetObjectA((GH)(uintptr_t)image, sizeof(bm), &bm) && bm.bmHeight == 4);
  DeleteObject((GH)(uintptr_t)image);
  /* Dialogs and message boxes by Unicode resource name / text. */
  HWND made = CreateDialogParamW(FIXTURE, (const WCHAR *)(uintptr_t)102, owner,
                                 wide_dialog_proc, 9);
  CHECK(made && wide_dialog_init == 9 && IsWindowUnicode(made));
  DestroyWindow(made);
  CHECK(!CreateDialogParamW(FIXTURE, L("NOPE"), owner, wide_dialog_proc, 0) &&
        GetLastError() == 1812);
  script(wide_dialog_script);
  CHECK(DialogBoxParamW(FIXTURE, (const WCHAR *)(uintptr_t)102, owner, wide_dialog_proc,
                        3) == 77);
  CHECK(MessageBoxW(owner, L("Caf\xe9 \x80"), L("T\xeftle"), MB_OK) == IDOK);
  MSGBOXPARAMSW mb = {.cbSize = sizeof(mb), .hwndOwner = owner,
                      .lpszText = L("Indirect"), .lpszCaption = L("Box"),
                      .dwStyle = MB_OKCANCEL | 0x80};
  CHECK(MessageBoxIndirectW(&mb) == IDOK);
  script(0);
  /* Message boxes: the parameter block is validated. */
  MSGBOXPARAMSW p = {.cbSize = 4};
  CHECK(!MessageBoxIndirectW(&p) && GetLastError() == 87);
  /* Text drawing in Unicode matches the ANSI forms. */
  HDC dc = GetDC(owner);
  int tabs[1] = {40};
  CHECK(GetTabbedTextExtentW(dc, L("a\tb\xe9"), -1, 1, tabs) ==
            GetTabbedTextExtentA(dc, "a\tb\xe9", -1, 1, tabs) &&
        TabbedTextOutW(dc, 0, 0, L("a\tb"), 3, 1, tabs, 0) ==
            TabbedTextOutA(dc, 0, 0, "a\tb", 3, 1, tabs, 0));
  CHECK(DrawStateW(dc, 0, 0, (LPARAM)L("&Draw"), 0, 2, 2, 0, 0, DST_PREFIXTEXT) &&
        DrawStateW(dc, 0, 0, (LPARAM)L("Text"), 2, 2, 2, 30, 14, DST_TEXT | DSS_DISABLED));
  ReleaseDC(owner, dc);
  CHECK(VkKeyScanW('a') == VkKeyScanA('a') && VkKeyScanW(0x4e2d) == -1);
  DestroyWindow(owner);
  puts("PASS f5-20B unicode menus MENUINFO item rectangles accelerators resources");
}

/* ---- text: Char*, OEM, wsprintf ------------------------------------------- */
static void text_functions(void) {
  WCHAR w[32];
  memcpy(w, L("caf\xe9 \xff \x9a"), 20);
  w[7] = 0x3b1; /* Greek alpha */
  w[8] = 0x101; /* a macron */
  w[9] = 0;
  CHECK(CharUpperW(w) == w && w[3] == 0xc9 && w[5] == 0x178 && w[7] == 0x391 &&
        w[8] == 0x100);
  memcpy(w, L("caf\xe9"), 10);
  CHECK(CharUpperBuffW(w, 2) == 2 && w[0] == 'C' && w[2] == 'f');
  CHECK(CharLowerBuffW(w, 4) == 4 && wide_is(w, "caf\xe9"));
  CHECK((uintptr_t)CharUpperW((WCHAR *)(uintptr_t)0xe9) == 0xc9);
  CHECK((uintptr_t)CharLowerW((WCHAR *)(uintptr_t)0x391) == 0x3b1);
  CHECK((uintptr_t)CharUpperA((char *)(uintptr_t)0x9a) == 0x8a);
  char a[32] = "x\xe9y";
  CHECK(CharNextA(a) == a + 1 && CharNextA(a + 3) == a + 3 && CharPrevA(a, a) == a &&
        CharPrevA(a, a + 2) == a + 1);
  CHECK(CharNextW(w) == w + 1 && CharPrevW(w, w + 1) == w);
  CHECK(IsCharAlphaW(0xe9) && IsCharAlphaW(0x3b1) && !IsCharAlphaW('1') &&
        IsCharAlphaNumericW('1') && IsCharUpperW(0xc9) && IsCharLowerW(0x17e) &&
        !IsCharUpperW(0x17e) && !IsCharAlphaW(0x4e2d));
  /* OEM 437 <-> ANSI 1252. */
  CHECK(CharToOemA("\xe9\xc7\x80", a) && !strcmp(a, "\x82\x80?"));
  CHECK(OemToCharA("\x82\x81\xb3", a) && !strcmp(a, "\xe9\xfc?"));
  CHECK(CharToOemW(L("\xe9"), a) && !strcmp(a, "\x82"));
  CHECK(OemToCharW("\xe1\xb3", w) && w[0] == 0xdf && w[1] == 0x2502 && !w[2]);
  /* wsprintf: types, sizes, flags, width and precision. */
  char out[1100];
  static const char expect1[] =
      "-42|4000000000|beef|BEEF|0xff|00042|7   |005|Z|str|   ab|%";
  CHECK(wsprintfA(out, "%d|%u|%x|%X|%#x|%05d|%-4d|%.3d|%c|%s|%5.2s|%%", -42,
                  4000000000u, 0xbeef, 0xbeef, 255, 42, 7, 5, 'Z', "str",
                  "abc") == (int)strlen(expect1) &&
        !strcmp(out, expect1));
  static const char expect2[] = "w\xe9|l|h|\x80|(null)";
  CHECK(wsprintfA(out, "%S|%ls|%hs|%C|%s", L("w\xe9"), L("l"), "h", (int)0x20ac,
                  (char *)0) == (int)strlen(expect2) &&
        !strcmp(out, expect2));
  CHECK(wsprintfA(out, "%I64d|%I64u|%p", (int64_t)-1234567890123ll,
                  (uint64_t)18446744073709551615ull, (void *)(uintptr_t)0x1234) > 0 &&
        !strncmp(out, "-1234567890123|18446744073709551615|", 36));
  CHECK(wsprintfA(out, "[%hc|%lc|%Ix]", 0, 0, 0x1fu) == 6 && !strcmp(out, "[||1f]"));
  WCHAR wout[64];
  CHECK(wsprintfW(wout, L("%s=%d %S"), L("\x80"), 5, "\xe9") == 5 &&
        wout[0] == 0x20ac && wout[4] == 0xe9);
  char big[2000];
  memset(big, 'q', sizeof(big) - 1);
  big[sizeof(big) - 1] = 0;
  CHECK(wsprintfA(out, "%s", big) == 1023 && strlen(out) == 1023);
  puts("PASS f5-20B text Char* IsChar* OEM wsprintf");
}

/* ---- DDEML --------------------------------------------------------------- */
static void *W32API dde_callback(UINT a, UINT b, void *c, uintptr_t d,
                                 uintptr_t e, void *f, uintptr_t g, uintptr_t h) {
  (void)a, (void)b, (void)c, (void)d, (void)e, (void)f, (void)g, (void)h;
  return 0;
}
static void ddeml(void) {
  DWORD id = 0, other = 0;
  CHECK(DdeInitializeW(&id, dde_callback, 0, 0) == 0 && id);
  CHECK(DdeInitializeA(&other, dde_callback, 0x10 /* APPCMD_CLIENTONLY */, 0) == 0 &&
        other && other != id);
  CHECK(DdeInitializeW(&id, dde_callback, 0x20, 0) == 0); /* reinitialise */
  DWORD bad = 0;
  CHECK(DdeInitializeW(&bad, 0, 0, 0) == 0x4006 && DdeInitializeW(0, dde_callback, 0, 0) == 0x4006);
  uintptr_t s1 = DdeCreateStringHandleW(id, L("Progman"), 1200),
            s2 = DdeCreateStringHandleA(other, "PROGMAN", 1004);
  CHECK(s1 && s1 == s2); /* case-insensitive, shared like atoms */
  WCHAR w[16];
  char a[16];
  CHECK(DdeQueryStringW(id, s1, 0, 0, 1200) == 7 &&
        DdeQueryStringW(id, s1, w, 16, 1200) == 7 && wide_is(w, "Progman"));
  CHECK(DdeQueryStringA(other, s1, a, 4, 1004) == 3 && !strcmp(a, "Pro"));
  CHECK(!DdeQueryStringW(id, s1, w, 16, 1004) && DdeGetLastError(id) == 0x4006 &&
        DdeGetLastError(id) == 0);
  uintptr_t topic = DdeCreateStringHandleW(id, L("Progman"), 1200);
  CHECK(!DdeConnect(id, s1, topic, 0) && DdeGetLastError(id) == 0x400a);
  CHECK(!DdeDisconnect(0x1234) && !DdeClientTransaction(0, 0, 0x1234, 0, 1, 0x4050, 1000, 0));
  CHECK(DdeNameService(id, s1, 0, 1) && DdeNameService(id, 0, 0, 2));
  CHECK(!DdeNameService(other, s1, 0, 1) && DdeGetLastError(other) == 0x4004);
  uintptr_t data = DdeCreateDataHandle(id, "xxhello", 6, 2, 0, 1, 0);
  CHECK(data && DdeGetData(data, 0, 0, 0) == 6);
  char got[8] = {0};
  CHECK(DdeGetData(data, got, 8, 1) == 5 && !memcmp(got, "ello\0", 5));
  CHECK(DdeFreeDataHandle(data) && !DdeFreeDataHandle(data));
  CHECK(DdeFreeStringHandle(id, s1) && DdeFreeStringHandle(other, s2) &&
        DdeFreeStringHandle(id, topic) && !DdeFreeStringHandle(id, topic) &&
        DdeGetLastError(id) == 0x4006);
  CHECK(DdeUninitialize(id) && DdeUninitialize(other) && !DdeUninitialize(id));
  CHECK(DdeGetLastError(id) == 0x4003);
  puts("PASS f5-20B DDEML instances string handles data handles no-partner connect");
}

/* ---- window tree, positioning, monitors, DPI, mouse, timers -------------- */
static UINT system_ticks;
static void W32API system_timer(HWND h, UINT m, uintptr_t id, DWORD t) {
  (void)h, (void)id, (void)t;
  if (m == 0x118)
    system_ticks++;
}
static void upper_windows(void) {
  HWND desktop = GetDesktopWindow();
  CHECK(desktop && IsWindow(desktop) && IsWindowVisible(desktop));
  RECT r;
  CHECK(GetWindowRect(desktop, &r) && r.right == (LONG)GetSystemMetrics(0) &&
        GetClientRect(desktop, &r) && r.bottom == (LONG)GetSystemMetrics(1));
  char name[16];
  CHECK(GetClassNameA(desktop, name, 16) == 6 && !strcmp(name, "#32769"));
  HDC screen = GetDC(desktop), null_dc = GetDC(0);
  CHECK(screen && null_dc && ReleaseDC(desktop, screen) && ReleaseDC(0, null_dc));
  HWND top = owner_window("Top", WS_CAPTION, 300, 200);
  HWND owned = CreateWindowExA(0, "F5 Owner", "Owned", WS_POPUP, 0, 0, 50, 50,
                               desktop, 0, FIXTURE, 0);
  CHECK(owned && !GetParent(owned));
  HWND popup = CreateWindowExA(0, "F5 Owner", "Popup", WS_POPUP, 0, 0, 50, 50,
                               top, 0, FIXTURE, 0);
  HWND child = CreateWindowExA(0, "BUTTON", "c", WS_CHILD | WS_VISIBLE, 10, 10,
                               40, 20, top, (HANDLE)7, FIXTURE, 0);
  HWND grandchild = CreateWindowExA(0, "STATIC", "g", WS_CHILD | WS_VISIBLE, 1,
                                    1, 5, 5, child, 0, FIXTURE, 0);
  CHECK(GetAncestor(child, 1) == top && GetAncestor(top, 1) == desktop &&
        GetAncestor(grandchild, 2) == top && GetAncestor(popup, 3) == top &&
        GetAncestor(top, 3) == top && !GetAncestor(desktop, 1));
  CHECK(!GetAncestor(child, 9) && GetLastError() == 87);
  CHECK(ChildWindowFromPoint(top, (POINT){15, 15}) == child &&
        ChildWindowFromPoint(top, (POINT){100, 100}) == top &&
        !ChildWindowFromPoint(top, (POINT){-5, 5}));
  ShowWindow(child, 0);
  CHECK(ChildWindowFromPointEx(top, (POINT){15, 15}, 1) == top &&
        ChildWindowFromPoint(top, (POINT){15, 15}) == child);
  ShowWindow(child, 5);
  WINDOWINFO wi = {.cbSize = sizeof(wi)};
  CHECK(GetWindowInfo(top, &wi) && wi.dwStyle == (DWORD)GetWindowLongA(top, GWL_STYLE) &&
        wi.rcClient.left > wi.rcWindow.left && wi.wCreatorVersion == 0x400 &&
        wi.atomWindowType);
  wi.cbSize = 8;
  CHECK(!GetWindowInfo(top, &wi) && GetLastError() == 87);
  /* Deferred positioning moves both windows; a bad window frees it all. */
  HANDLE dwp = BeginDeferWindowPos(2);
  CHECK(dwp && (dwp = DeferWindowPos(dwp, popup, 0, 30, 40, 60, 70, 0x14)) &&
        (dwp = DeferWindowPos(dwp, owned, 0, 5, 6, 0, 0, 0x15)) &&
        EndDeferWindowPos(dwp));
  CHECK(GetWindowRect(popup, &r) && r.left == 30 && r.top == 40 &&
        r.right == 90 && GetWindowRect(owned, &r) && r.left == 5);
  dwp = BeginDeferWindowPos(1);
  CHECK(!DeferWindowPos(dwp, (HWND)(uintptr_t)0x5555, 0, 0, 0, 0, 0, 0) &&
        GetLastError() == 1400 && !EndDeferWindowPos(dwp));
  /* Regions are kept and returned. */
  GH region = CreateRectRgn(0, 0, 20, 10);
  CHECK(!GetWindowRgn(top, (HANDLE)(uintptr_t)region));
  CHECK(SetWindowRgn(top, (HANDLE)(uintptr_t)CreateRoundRectRgn(0, 0, 40, 40, 10, 10), 1));
  CHECK(GetWindowRgn(top, (HANDLE)(uintptr_t)region) == 3 &&
        GetRgnBox(region, &r) && r.right == 40 && !PtInRegion(region, 0, 0));
  CHECK(SetWindowRgn(top, 0, 0) && !GetWindowRgn(top, (HANDLE)(uintptr_t)region));
  DeleteObject(region);
  /* Monitors and DPI: one 96-DPI monitor. */
  HANDLE monitor = MonitorFromWindow(top, 2);
  RECT far = {5000, 5000, 5100, 5100};
  CHECK(monitor && MonitorFromRect(&far, 2) == monitor && !MonitorFromRect(&far, 0) &&
        MonitorFromPoint((POINT){1, 1}, 0) == monitor &&
        !MonitorFromPoint((POINT){-9, 1}, 0));
  MONITORINFOEXW mw = {.cbSize = sizeof(mw)};
  CHECK(GetMonitorInfoW(monitor, &mw) && mw.dwFlags == 1 &&
        mw.rcMonitor.right == (LONG)GetSystemMetrics(0) &&
        wide_is(mw.szDevice, "\\\\.\\DISPLAY1"));
  MONITORINFO ma = {.cbSize = sizeof(ma)};
  CHECK(GetMonitorInfoA(monitor, &ma) && ma.rcWork.bottom <= ma.rcMonitor.bottom);
  ma.cbSize = 12;
  CHECK(!GetMonitorInfoA(monitor, &ma) && !GetMonitorInfoW((HANDLE)7, &mw));
  CHECK(GetDpiForWindow(top) == 96 && !GetDpiForWindow((HWND)(uintptr_t)0x5555) &&
        !IsProcessDPIAware());
  UINT dx, dy;
  CHECK(!GetDpiForMonitorInternal(monitor, 0, &dx, &dy) && GetLastError() == 120 &&
        !SetProcessDpiAwarenessInternal(2) && GetLastError() == 120);
  NotifyWinEvent(0x8000, top, 0, 0);
  /* System timers come as WM_SYSTIMER; their ids are separate. */
  CHECK(SetSystemTimer(top, 5, 10, system_timer) == 5 && SetTimer(top, 5, 10, 0));
  CHECK(KillTimer(top, 5) && !KillTimer(top, 5));
  ui_kernel_pause(20);
  MSG msg;
  for (UINT guard = 0; guard < 20 && !system_ticks; guard++)
    if (PeekMessageA(&msg, top, 0, 0, 1))
      DispatchMessageA(&msg);
  CHECK(system_ticks == 1 && KillSystemTimer(top, 5) && !KillSystemTimer(top, 5));
  /* TrackMouseEvent: leave at once when outside; hover and leave by timer. */
  TRACKMOUSEEVENT t = {.cbSize = sizeof(t), .dwFlags = 2, .hwndTrack = top};
  u32_pointer = (POINT){-50, -50};
  CHECK(TrackMouseEvent(&t));
  CHECK(PeekMessageA(&msg, top, 0x2a3, 0x2a3, 1));
  RECT wr;
  GetWindowRect(top, &wr);
  u32_pointer = (POINT){wr.left + 50, wr.top + 60};
  t.dwFlags = 3;
  t.dwHoverTime = 100;
  CHECK(TrackMouseEvent(&t));
  TRACKMOUSEEVENT q = {.cbSize = sizeof(q), .dwFlags = 0x40000000};
  CHECK(TrackMouseEvent(&q) && q.hwndTrack == top && (q.dwFlags & 3) == 3 &&
        q.dwHoverTime == 100);
  BOOL hovered = 0, left = 0;
  for (UINT step = 0; step < 40 && !(hovered && left); step++) {
    ui_kernel_pause(60);
    if (step == 5)
      u32_pointer = (POINT){-50, -50};
    while (PeekMessageA(&msg, 0, 0, 0, 1)) {
      hovered |= msg.message == 0x2a1;
      left |= msg.message == 0x2a3;
      DispatchMessageA(&msg);
    }
  }
  CHECK(hovered && left);
  q.dwFlags = 0x40000000;
  CHECK(TrackMouseEvent(&q) && !q.hwndTrack);
  t.cbSize = 4;
  CHECK(!TrackMouseEvent(&t) && GetLastError() == 87);
  /* DragDetect: a release inside the drag rectangle is no drag. */
  PostMessageA(top, WM_LBUTTONUP, 0, 0);
  CHECK(!DragDetect(top, (POINT){wr.left + 5, wr.top + 30}));
  MSG move = {top, WM_MOUSEMOVE, 0, 0, 0, {wr.left + 40, wr.top + 30}};
  u32_queue(GetCurrentThreadId(), top, WM_MOUSEMOVE, 0, 0, 0x2, 0, move.pt);
  CHECK(DragDetect(top, (POINT){wr.left + 5, wr.top + 30}));
  /* Rectangles. */
  RECT a = {0, 0, 10, 10}, b = {0, 0, 10, 4}, out;
  CHECK(SubtractRect(&out, &a, &b) && out.top == 4 && out.bottom == 10);
  b = (RECT){2, 2, 5, 5};
  CHECK(SubtractRect(&out, &a, &b) && EqualRect(&out, &a));
  b = (RECT){-1, -1, 11, 11};
  CHECK(!SubtractRect(&out, &a, &b) && IsRectEmpty(&out));
  HDC dc = GetDC(top);
  DWORD before = GetPixel((GH)(uintptr_t)dc, 2, 2);
  RECT inv = {0, 0, 5, 5};
  CHECK(InvertRect(dc, &inv) &&
        GetPixel((GH)(uintptr_t)dc, 2, 2) == (~before & 0xffffff));
  ReleaseDC(top, dc);
  /* Process idle and help. */
  CHECK(!WaitForInputIdle((HANDLE)(uintptr_t)0x44, 10));
  CHECK(WinHelpW(top, L("x.hlp"), 2, 0) && !WinHelpW(top, L("x.hlp"), 1, 0) &&
        GetLastError() == 2);
  DestroyWindow(child);
  DestroyWindow(popup);
  DestroyWindow(owned);
  DestroyWindow(top);
  puts("PASS f5-20B desktop ancestors hit testing window info deferred positions regions monitors DPI timers mouse rectangles");
}

/* ---- clipboard formats, icons, files ---------------------------------------- */
static uint8_t *icon_image(UINT size, UINT bpp, DWORD *bytes) {
  UINT colors = bpp <= 8 ? 1u << bpp : 0,
       pitch = ((size * bpp + 31) / 32) * 4, mask = ((size + 31) / 32) * 4;
  *bytes = 40 + colors * 4 + pitch * size + mask * size;
  uint8_t *p = calloc(1, *bytes);
  w32_put32(p, 40);
  w32_put32(p + 4, size);
  w32_put32(p + 8, size * 2);
  w32_put16(p + 12, 1);
  w32_put16(p + 14, (uint16_t)bpp);
  for (UINT i = 0; i < colors; i++)
    w32_put32(p + 40 + i * 4, i == 1 ? 0xff0000u : 0);
  uint8_t *bits = p + 40 + colors * 4;
  for (UINT y = 0; y < size; y++)
    for (UINT x = 0; x < size; x++) {
      uint8_t *row = bits + y * pitch; /* bottom-up */
      if (bpp == 4)
        row[x / 2] |= (uint8_t)(1u << (x & 1 ? 0 : 4));
      else if (bpp == 32)
        w32_put32(row + x * 4, 0x0000ff00u);
    }
  /* Mask: the left half transparent. */
  for (UINT y = 0; y < size; y++)
    for (UINT x = 0; x < size / 2; x++)
      bits[pitch * size + y * mask + x / 8] |= (uint8_t)(128 >> (x & 7));
  return p;
}
static void upper_resources(void) {
  /* Registered clipboard formats share the window-message atoms. */
  UINT f = RegisterClipboardFormatW(L("F5 Format \xe9"));
  CHECK(f >= 0xc000 && RegisterClipboardFormatA("f5 format \xe9") == f &&
        RegisterWindowMessageA("F5 Format \xe9") == f);
  char name[32];
  WCHAR wname[32];
  CHECK(GetClipboardFormatNameA(f, name, 32) == 11 && !strcmp(name, "F5 Format \xe9"));
  CHECK(GetClipboardFormatNameW(f, wname, 32) == 11 && wname[10] == 0xe9);
  CHECK(GetClipboardFormatNameA(f, name, 4) == 3 && !strcmp(name, "F5 "));
  CHECK(!GetClipboardFormatNameA(1, name, 32) && !RegisterClipboardFormatA(""));
  HWND owner = owner_window("Clip", WS_CAPTION, 100, 100);
  CHECK(!EnumClipboardFormats(0) && GetLastError() == 1418);
  DWORD sequence = GetClipboardSequenceNumber();
  CHECK(OpenClipboard(owner) && EmptyClipboard());
  HANDLE text = GlobalAlloc(2, 6);
  memcpy(GlobalLock(text), "hello", 6);
  GlobalUnlock(text);
  CHECK(SetClipboardData(CF_TEXT, text));
  CHECK(GetClipboardSequenceNumber() == sequence + 2);
  CHECK(EnumClipboardFormats(0) == CF_TEXT && EnumClipboardFormats(CF_TEXT) == 7 &&
        EnumClipboardFormats(7) == 13 && !EnumClipboardFormats(13) && !GetLastError());
  CHECK(CloseClipboard());
  DestroyWindow(owner);
  /* Icons: resource images of other sizes, copies, ICONINFO. */
  DWORD n;
  uint8_t *small = icon_image(16, 4, &n);
  HICON icon = CreateIconFromResourceEx(small, n, 1, 0x30000, 0, 0, 0);
  CHECK(icon && !CreateIconFromResourceEx(small, n, 1, 0x40000, 0, 0, 0) &&
        !CreateIconFromResourceEx(small, 20, 1, 0x30000, 0, 0, 0));
  free(small);
  HICON copy = CopyIcon(icon);
  CHECK(copy && copy != icon && DestroyIcon(icon));
  ICONINFO info;
  CHECK(GetIconInfo(copy, &info) && info.fIcon && info.xHotspot == 16);
  GBITMAP bm;
  CHECK(GetObjectA((GH)(uintptr_t)info.hbmColor, sizeof(bm), &bm) &&
        bm.bmWidth == 32 && bm.bmBitsPixel == 32 &&
        GetObjectA((GH)(uintptr_t)info.hbmMask, sizeof(bm), &bm) &&
        bm.bmBitsPixel == 1);
  /* The left half (transparent) has the AND bit set, the right half not. */
  uint8_t mask_bits[128];
  CHECK(GetBitmapBits((GH)(uintptr_t)info.hbmMask, sizeof(mask_bits), mask_bits) == 128 &&
        mask_bits[0] == 0xff && mask_bits[3] == 0);
  HICON again = CopyImage(copy, 1, 0, 0, 8 /* LR_COPYDELETEORG */);
  CHECK(again && !DestroyIcon(copy) && DestroyIcon(again));
  HANDLE bitmap = (HANDLE)(uintptr_t)CreateBitmap(4, 3, 1, 32, 0);
  HANDLE bigger = CopyImage(bitmap, 0, 8, 6, 0);
  CHECK(bigger && GetObjectA((GH)(uintptr_t)bigger, sizeof(bm), &bm) &&
        bm.bmWidth == 8 && bm.bmHeight == 6);
  DeleteObject((GH)(uintptr_t)info.hbmColor);
  DeleteObject((GH)(uintptr_t)info.hbmMask);
  DeleteObject((GH)(uintptr_t)bigger);
  DeleteObject((GH)(uintptr_t)bitmap);
  /* PrivateExtractIcons: an .ICO file and a PE image's icon groups. */
  uint8_t *image = icon_image(32, 32, &n);
  char path[1100];
  snprintf(path, sizeof(path), "%s/TEST.ICO", drive_dir);
  FILE *ico = fopen(path, "wb");
  CHECK(ico);
  uint8_t dir[22] = {0, 0, 1, 0, 1, 0, 32, 32, 0, 0, 1, 0, 32, 0};
  w32_put32(dir + 14, n);
  w32_put32(dir + 18, 22);
  CHECK(fwrite(dir, 1, 22, ico) == 22 && fwrite(image, 1, n, ico) == n);
  fclose(ico);
  HICON icons[4] = {0};
  UINT ids[4];
  CHECK(PrivateExtractIconsW(L("C:\\TEST.ICO"), 0, 32, 32, 0, 0, 0, 0) == 1);
  CHECK(PrivateExtractIconsW(L("C:\\TEST.ICO"), 0, 32, 32, icons, ids, 1, 0) == 1 &&
        icons[0] && DestroyIcon(icons[0]));
  CHECK(PrivateExtractIconsW(L("C:\\NONE.ICO"), 0, 32, 32, icons, ids, 1, 0) ==
        0xffffffffu);
  icon_module_image = image;
  icon_module_bytes = n;
  CHECK(PrivateExtractIconsW(L("C:\\ICONS.EXE"), 0, 32, 32, 0, 0, 0, 0) == 2);
  CHECK(PrivateExtractIconsW(L("C:\\ICONS.EXE"), 1, 32, 32, icons, ids, 2, 0) == 1 &&
        ids[0] == 7 && DestroyIcon(icons[0]));
  CHECK(PrivateExtractIconsW(L("C:\\ICONS.EXE"), -5, 32, 32, icons, ids, 1, 0) == 1 &&
        ids[0] == 5 && DestroyIcon(icons[0]));
  HICON large[2], small_icons[2];
  CHECK(PrivateExtractIconExW(L("C:\\ICONS.EXE"), -1, 0, 0, 0) == 2);
  CHECK(PrivateExtractIconExW(L("C:\\ICONS.EXE"), 0, large, small_icons, 2) == 2 &&
        DestroyIcon(large[0]) && DestroyIcon(small_icons[1]));
  DestroyIcon(large[1]);
  DestroyIcon(small_icons[0]);
  icon_module_image = 0;
  free(image);
  /* DlgDirList over LB_DIR/CB_DIR. */
  snprintf(path, sizeof(path), "%s/DIRTEST", drive_dir);
  CHECK(!mkdir(path, 0755));
  snprintf(path, sizeof(path), "%s/DIRTEST/A.TXT", drive_dir);
  fclose(fopen(path, "w"));
  snprintf(path, sizeof(path), "%s/DIRTEST/B.DOC", drive_dir);
  fclose(fopen(path, "w"));
  snprintf(path, sizeof(path), "%s/DIRTEST/SUB", drive_dir);
  CHECK(!mkdir(path, 0755));
  DWORD tsize;
  const uint8_t *tmpl = resource(5, 103, &tsize);
  (void)tmpl;
  HWND host = owner_window("Dir", WS_CAPTION, 300, 200);
  HWND list = CreateWindowExA(0, "LISTBOX", "", WS_CHILD | LBS_SORT | LBS_HASSTRINGS,
                              0, 0, 100, 100, host, (HANDLE)20, FIXTURE, 0);
  HWND combo = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | 3 | CBS_SORT, 0, 0,
                               100, 100, host, (HANDLE)21, FIXTURE, 0);
  HWND label = CreateWindowExA(0, "STATIC", "", WS_CHILD, 0, 0, 100, 20, host,
                               (HANDLE)22, FIXTURE, 0);
  CHECK(list && combo && label);
  WCHAR spec[64];
  memcpy(spec, L("C:\\DIRTEST\\*.TXT"), 34);
  CHECK(DlgDirListW(host, spec, 20, 22, 0x10) && wide_is(spec, "*.TXT"));
  WCHAR item[32];
  CHECK(SendMessageW(list, LB_GETCOUNT, 0, 0) == 1 &&
        SendMessageW(list, LB_GETTEXT, 0, (LPARAM)item) && wide_is(item, "A.TXT"));
  CHECK(GetWindowTextA(label, name, 32) && !strcmp(name, "C:\\DIRTEST"));
  memcpy(spec, L("*.*"), 8);
  CHECK(DlgDirListW(host, spec, 20, 0, 0x10) &&
        SendMessageW(list, LB_GETCOUNT, 0, 0) == 3 &&
        SendMessageW(list, LB_GETTEXT, 2, (LPARAM)item) && wide_is(item, "[SUB]"));
  CHECK(DlgDirListComboBoxW(host, spec, 21, 0, 0) &&
        SendMessageA(combo, CB_GETCOUNT, 0, 0) == 2);
  memcpy(spec, L("C:\\NOWHERE\\*.*"), 32);
  CHECK(!DlgDirListW(host, spec, 20, 0, 0) && GetLastError() == 3);
  DestroyWindow(host);
  puts("PASS f5-20B clipboard formats icons extraction DlgDirList");
}

/* ---- GDI upper set ------------------------------------------------------- */
struct gdi_surface {
  GH dc, bitmap;
  uint32_t *pixels;
};
static struct gdi_surface gdi_surface(int w, int h) {
  struct gdi_surface s = {0, 0, 0};
  GBIH bi = {40, w, -h, 1, 32, 0, 0, 0, 0, 0, 0};
  s.dc = CreateCompatibleDC(0);
  s.bitmap = CreateDIBSection(s.dc, &bi, 0, (void **)&s.pixels, 0, 0);
  CHECK(s.dc && s.bitmap && s.pixels);
  SelectObject(s.dc, s.bitmap);
  return s;
}
static void gdi_free_surface(struct gdi_surface s) {
  DeleteDC(s.dc);
  DeleteObject(s.bitmap);
}
static UINT families_w, families_a;
static int W32API family_w(const void *lf, const void *tm, DWORD type, intptr_t p) {
  (void)tm, (void)type, (void)p;
  const WCHAR *face = (const WCHAR *)((const uint8_t *)lf + 28);
  families_w++;
  return !(p == 1 && face[0] == 'A');
}
static int W32API family_a(const void *lf, const void *tm, DWORD type, intptr_t p) {
  (void)lf, (void)tm, (void)type, (void)p;
  families_a++;
  return 1;
}
static void gdi_upper(void) {
  RECT r;
  GH round = CreateRoundRectRgn(0, 0, 40, 30, 12, 12);
  CHECK(round && GetRgnBox(round, &r) == 3 && r.right == 40 && r.bottom == 30 &&
        !PtInRegion(round, 0, 0) && PtInRegion(round, 20, 15) &&
        PtInRegion(round, 20, 0) && !PtInRegion(round, 39, 29));
  DeleteObject(round);
  struct {
    DWORD style, color;
    uintptr_t hatch;
  } brush = {0, 0x0000ff, 0};
  GH pen = ExtCreatePen(0x10000 /* PS_GEOMETRIC */, 3, &brush, 0, 0);
  GLOGPEN lp;
  CHECK(pen && GetObjectA(pen, sizeof(lp), &lp) && lp.lopnWidth.x == 3 &&
        lp.lopnColor == 0xff);
  DeleteObject(pen);
  CHECK(!ExtCreatePen(0, 2, &brush, 0, 0) && !ExtCreatePen(0x30000, 1, &brush, 0, 0));
  DWORD dashes[2] = {2, 2};
  CHECK((pen = ExtCreatePen(7, 1, &brush, 2, dashes)));
  DeleteObject(pen);
  struct gdi_surface s = gdi_surface(32, 32), t = gdi_surface(8, 8);
  PatBlt(s.dc, 0, 0, 32, 32, 0x00ff0062 /* WHITENESS */);
  POINT points[5] = {{1, 1}, {10, 1}, {1, 5}, {1, 8}, {10, 8}};
  DWORD counts[2] = {2, 3};
  CHECK(PolyPolyline(s.dc, points, counts, 2) && s.pixels[1 * 32 + 5] == 0 &&
        s.pixels[8 * 32 + 5] == 0 && s.pixels[4 * 32 + 5] == 0xffffff);
  counts[1] = 1;
  CHECK(!PolyPolyline(s.dc, points, counts, 2));
  /* Alpha blending: constant alpha and premultiplied per-pixel alpha. */
  for (int i = 0; i < 64; i++)
    t.pixels[i] = 0x00000000;
  PatBlt(s.dc, 0, 0, 32, 32, 0x00ff0062);
  CHECK(GdiAlphaBlend(s.dc, 0, 0, 8, 8, t.dc, 0, 0, 8, 8, 0x00800000));
  CHECK((s.pixels[0] & 0xff) == 127);
  for (int i = 0; i < 64; i++)
    t.pixels[i] = i < 32 ? 0x80400000u : 0; /* premultiplied: red 0x40, alpha 0x80 */
  PatBlt(s.dc, 0, 0, 32, 32, 0x00ff0062);
  CHECK(GdiAlphaBlend(s.dc, 0, 0, 16, 16, t.dc, 0, 0, 8, 8, 0x01ff0000));
  CHECK(((s.pixels[0] >> 16) & 0xff) == 0x40 + 127 && (s.pixels[0] & 0xff) == 127 &&
        s.pixels[31 * 32 + 0] == 0xffffff);
  CHECK(!GdiAlphaBlend(s.dc, 0, 0, 8, 8, t.dc, 0, 0, 9, 8, 0x00ff0000));
  CHECK(!GdiAlphaBlend(s.dc, 0, 0, 8, 8, t.dc, 0, 0, 8, 8, 0x00ff0001));
  /* Device-dependent bits are WORD aligned and top-down. */
  /* CreateBitmap rows are WORD aligned: 3 x 24 bits = 9 bytes + 1. */
  uint8_t px[20] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 10, 11, 12, 13, 14, 15, 16, 17, 18, 0};
  GH b24 = CreateBitmap(3, 2, 1, 24, px);
  CHECK(b24);
  uint8_t ddb[32];
  CHECK(GetBitmapBits(b24, 0, 0) == 20 && GetBitmapBits(b24, 32, ddb) == 20 &&
        ddb[0] == 1 && ddb[10] == 10 && GetBitmapBits(b24, 15, ddb) == 10);
  DeleteObject(b24);
  CHECK(GetLayout(s.dc) == 0 && GetLayout(0x1234) == 0xffffffffu);
  GH halftone = CreateHalftonePalette(s.dc);
  GPALENTRY pe[256];
  CHECK(halftone && GetPaletteEntries(halftone, 0, 256, pe) == 256 &&
        pe[10].peRed == 0 && pe[10 + 215].peBlue == 255);
  DeleteObject(halftone);
  uint8_t space[588];
  memset(space, 0, sizeof(space));
  w32_put32(space, 0x50534f43);
  w32_put32(space + 4, 0x400);
  w32_put32(space + 8, 588);
  void *cs = CreateColorSpaceW(space);
  CHECK(cs && DeleteColorSpace(cs) && !DeleteColorSpace(cs));
  w32_put32(space, 0);
  CHECK(!CreateColorSpaceW(space));
  uint8_t docinfo[20] = {20};
  CHECK(StartDocW(s.dc, docinfo) == -1);
  DWORD query = 8;
  CHECK(ExtEscape(s.dc, 8, 4, (const char *)&query, 0, 0) == 1);
  query = 15;
  CHECK(ExtEscape(s.dc, 8, 4, (const char *)&query, 0, 0) == 0 &&
        ExtEscape(s.dc, 0x1000, 0, 0, 0, 0) == 0 && ExtEscape(0x1234, 8, 0, 0, 0, 0) == -1);
  /* DEVMODEA (156) to DEVMODEW (220). */
  uint8_t dma[156 + 4];
  memset(dma, 0, sizeof(dma));
  memcpy(dma, "Printer \xe9", 9);
  w32_put16(dma + 36, 156);
  w32_put16(dma + 38, 4);
  w32_put32(dma + 40, 0x12345678);
  memcpy(dma + 70, "Letter", 6);
  w32_put32(dma + 104, 24);
  memcpy(dma + 156, "XTRA", 4);
  uint8_t *dmw = GdiConvertToDevmodeW(dma);
  CHECK(dmw && w32_u16(dmw + 100) == 220 && w32_u16(dmw + 16) == 0xe9 &&
        w32_u32(dmw + 72) == 0x12345678 && w32_u16(dmw + 102) == 'L' &&
        w32_u32(dmw + 168) == 24 && !memcmp(dmw + 220, "XTRA", 4));
  HeapFree((HANDLE)1, 0, dmw);
  w32_put16(dma + 36, 20);
  CHECK(!GdiConvertToDevmodeW(dma));
  /* Character sets. */
  CSI csi;
  CHECK(TranslateCharsetInfo((DWORD *)(uintptr_t)0, &csi, 1) && csi.ciACP == 1252 &&
        csi.fsCsb[0] == 1);
  CHECK(TranslateCharsetInfo((DWORD *)(uintptr_t)1251, &csi, 2) && csi.ciCharset == 204);
  DWORD sig = 0x4 | 0x8;
  CHECK(TranslateCharsetInfo(&sig, &csi, 3) && csi.ciCharset == 204);
  CHECK(!TranslateCharsetInfo((DWORD *)(uintptr_t)77, &csi, 1));
  GH font = CreateFontA(-11, 0, 0, 0, 400, 0, 0, 0, 0, 0, 0, 0, 0, "MS Sans Serif");
  GH old = SelectObject(s.dc, font);
  CHECK(GdiGetCodePage(s.dc) == 1252);
  LONG height = 0;
  CHECK(GdiGetCharDimensions(s.dc, 0, &height) == 6 && height == 13);
  int widths[3];
  CHECK(GetCharWidthInfo(s.dc, widths) && widths[2] == 0);
  DWORD fs[6];
  CHECK(GetTextCharsetInfo(s.dc, fs, 0) == 0 && (fs[4] & 1));
  struct {
    DWORD size;
    WCHAR *out;
    unsigned *order;
    int *dx, *caret;
    char *klass;
    WCHAR *glyphs;
    unsigned n;
    int fit;
  } gcp;
  WCHAR outs[8], glyphs[8];
  unsigned order[8];
  int dx[8], caret[8];
  memset(&gcp, 0, sizeof(gcp));
  gcp.size = sizeof(gcp);
  gcp.out = outs;
  gcp.order = order;
  gcp.dx = dx;
  gcp.caret = caret;
  gcp.glyphs = glyphs;
  gcp.n = 8;
  LONG extent[2];
  CHECK(GetTextExtentPoint32W(s.dc, L("Wide"), 4, extent));
  DWORD placed = GetCharacterPlacementW(s.dc, L("Wide"), 4, 0, &gcp, 0);
  CHECK((LONG)(placed & 0xffff) == extent[0] && gcp.n == 4 && order[3] == 3 &&
        caret[1] == dx[0] && glyphs[0] == 'W' && outs[3] == 'e');
  gcp.n = 8;
  placed = GetCharacterPlacementW(s.dc, L("Wide"), 4, dx[0] + dx[1], &gcp, 0x00100000);
  CHECK(gcp.fit == 2 && (LONG)(placed & 0xffff) == dx[0] + dx[1]);
  /* Unicode font enumeration matches the ANSI one. */
  CHECK(EnumFontFamiliesW(s.dc, 0, (void *)family_w, 0) &&
        EnumFontFamiliesA(s.dc, 0, (void *)family_a, 0) &&
        families_w == families_a && families_w > 0);
  uint8_t lfw[92];
  memset(lfw, 0, sizeof(lfw));
  lfw[23] = 1;
  memcpy(lfw + 28, L("Arial"), 12);
  families_w = 0;
  CHECK(!EnumFontFamiliesExW(s.dc, lfw, (void *)family_w, 1, 0) && families_w == 1);
  SelectObject(s.dc, old);
  DeleteObject(font);
  gdi_free_surface(t);
  gdi_free_surface(s);
  puts("PASS f5-20B GDI regions pens polylines alpha DDB bits layout palette colour space printing escapes DEVMODE charsets measurement enumeration");
}

/* ---- Uniscribe subset --------------------------------------------------- */
static void uniscribe(void) {
  struct gdi_surface s = gdi_surface(200, 40);
  GH font = CreateFontA(-11, 0, 0, 0, 400, 0, 0, 0, 0, 0, 0, 0, 0, "MS Sans Serif");
  SelectObject(s.dc, font);
  PatBlt(s.dc, 0, 0, 200, 40, 0x00ff0062);
  const WCHAR *text = L("Hello world");
  uint8_t items[3 * 8];
  int count = 0;
  CHECK(ScriptItemize(text, 11, 3, 0, 0, items, &count) == 0 && count == 1 &&
        w32_u32(items + 8) == 11);
  CHECK(ScriptItemize(text, 11, 1, 0, 0, items, &count) == (int)0x80070057);
  void *cache = 0;
  uint16_t glyphs[16], clusters[16];
  uint16_t va[16];
  uint8_t analysis[4] = {0, 0, 0, 0};
  int n = 0;
  CHECK(ScriptShape(0, &cache, text, 11, 16, analysis, glyphs, clusters, va, &n) ==
        (int)0x8000000a);
  CHECK(ScriptShape(s.dc, &cache, text, 11, 16, analysis, glyphs, clusters, va, &n) == 0 &&
        n == 11 && glyphs[0] == 'H' && clusters[10] == 10 && (analysis[1] & 0x80) &&
        cache);
  CHECK(ScriptShape(s.dc, &cache, text, 11, 5, analysis, glyphs, clusters, va, &n) ==
        (int)0x8007000e);
  int advance[16], total = 0;
  int abc[3];
  CHECK(ScriptPlace(0, &cache, glyphs, 11, va, analysis, advance, 0, abc) == 0);
  for (int i = 0; i < 11; i++) {
    int w;
    CHECK(GetCharWidth32W(s.dc, text[i], text[i], &w) && w == advance[i]);
    total += advance[i];
  }
  CHECK(abc[0] + abc[1] + abc[2] == total);
  int x = 0, cp = 0, trailing = 0;
  CHECK(ScriptCPtoX(3, 0, 11, 11, clusters, va, advance, analysis, &x) == 0 &&
        x == advance[0] + advance[1] + advance[2]);
  CHECK(ScriptCPtoX(3, 1, 11, 11, clusters, va, advance, analysis, &x) == 0 &&
        x == advance[0] + advance[1] + advance[2] + advance[3]);
  CHECK(ScriptXtoCP(advance[0] + 1, 11, 11, clusters, va, advance, analysis, &cp,
                    &trailing) == 0 && cp == 1 && !trailing);
  CHECK(ScriptXtoCP(-3, 11, 11, clusters, va, advance, analysis, &cp, &trailing) == 0 &&
        cp == -1 && trailing == 1);
  CHECK(ScriptXtoCP(total + 5, 11, 11, clusters, va, advance, analysis, &cp,
                    &trailing) == 0 && cp == 11 && !trailing);
  uint8_t attrs[11];
  CHECK(ScriptBreak(text, 11, analysis, attrs) == 0 && (attrs[5] & 2) &&
        (attrs[6] & 1) && (attrs[6] & 8) && (attrs[0] & 8) && !(attrs[1] & 8));
  uint8_t levels[4] = {0, 1, 1, 0};
  int v2l[4], l2v[4];
  CHECK(ScriptLayout(4, levels, v2l, l2v) == 0 && v2l[1] == 2 && v2l[2] == 1 &&
        l2v[2] == 1 && v2l[3] == 3);
  CHECK(ScriptTextOut(s.dc, &cache, 2, 2, 0, 0, analysis, 0, 0, glyphs, n, advance,
                      0, 0) == 0);
  BOOL inked = 0;
  for (int i = 0; i < 200 * 40; i++)
    inked |= s.pixels[i] != 0xffffff;
  CHECK(inked);
  CHECK(ScriptFreeCache(&cache) == 0 && !cache);
  /* ScriptString: clipping, tabs, password, selection drawing. */
  void *ssa = 0;
  CHECK(ScriptStringAnalyse(s.dc, text, 11, 16, -1, 0x4 | 0x40, advance[0] * 3, 0, 0,
                            0, 0, 0, &ssa) == 0 && ssa);
  CHECK(*ScriptString_pcOutChars(ssa) >= 3 && *ScriptString_pcOutChars(ssa) < 11 &&
        ScriptString_pSize(ssa)[0] <= advance[0] * 3 && ScriptString_pSize(ssa)[1] > 0);
  int logical[11];
  CHECK(ScriptStringGetLogicalWidths(ssa, logical) == 0 && logical[0] == advance[0] &&
        logical[*ScriptString_pcOutChars(ssa)] == 0);
  CHECK(ScriptStringCPtoX(ssa, 1, 0, &x) == 0 && x == advance[0]);
  CHECK(ScriptStringXtoCP(ssa, advance[0] + 1, &cp, &trailing) == 0 && cp == 1);
  CHECK(ScriptStringFree(&ssa) == 0 && !ssa);
  CHECK(ScriptStringAnalyse(s.dc, "a\tb", 3, 8, 0, 0x1, 0, 0, 0, 0, 0, 0, &ssa) == 0);
  int star;
  CHECK(ScriptStringGetLogicalWidths(ssa, logical) == 0 &&
        GetCharWidth32W(s.dc, '*', '*', &star) && logical[1] == star);
  CHECK(ScriptStringFree(&ssa) == 0);
  CHECK(ScriptStringAnalyse(s.dc, "a\tb", 3, 8, 0, 0x2, 0, 0, 0, 0, 0, 0, &ssa) == 0);
  CHECK(ScriptStringGetLogicalWidths(ssa, logical) == 0 && logical[1] > logical[0]);
  PatBlt(s.dc, 0, 0, 200, 40, 0x00ff0062);
  CHECK(ScriptStringOut(ssa, 0, 0, 0, 0, 1, 2, 0) == 0);
  BOOL highlighted = 0;
  for (int i = 0; i < 200 * 40; i++)
    highlighted |= s.pixels[i] == 0x000080;
  CHECK(highlighted);
  CHECK(ScriptStringFree(&ssa) == 0);
  CHECK(ScriptStringAnalyse(0, text, 11, 16, -1, 0x80, 0, 0, 0, 0, 0, 0, &ssa) ==
        (int)0x80070057);
  CHECK(ScriptStringAnalyse(0, text, 11, 16, -1, 0x40, 0, 0, 0, 0, 0, 0, &ssa) == 0 &&
        ScriptStringOut(ssa, 0, 0, 0, 0, 0, 0, 0) == (int)0x80070057);
  ScriptStringFree(&ssa);
  DeleteObject(font);
  gdi_free_surface(s);
  puts("PASS f5-20B Uniscribe items shaping placement hit testing breaks layout text-out strings");
}

/* ---- metafiles --------------------------------------------------------------- */
static void put32(uint8_t *p, DWORD v) { w32_put32(p, v); }
/* An EMF of the 100x50 reference-pixel frame: a filled rectangle and text. */
static uint8_t *authored_emf(DWORD *size) {
  static uint8_t e[512];
  memset(e, 0, sizeof(e));
  DWORD at = 0;
  /* Header: 1024x768 px over 270x203 mm: 100x50 px ~ 2637x1322 (.01 mm). */
  put32(e, 1);
  put32(e + 4, 88);
  put32(e + 16, 99);
  put32(e + 20, 49);
  put32(e + 32, 100 * 27000 / 1024);
  put32(e + 36, 50 * 20300 / 768);
  put32(e + 40, 0x464d4520);
  put32(e + 44, 0x10000);
  put32(e + 56, 3);
  put32(e + 72, 1024);
  put32(e + 76, 768);
  put32(e + 80, 270);
  put32(e + 84, 203);
  at = 88;
  /* EMR_CREATEBRUSHINDIRECT ih=1 solid blue; EMR_SELECTOBJECT 1 */
  DWORD rec1[] = {39, 24, 1, 0, 0xff0000, 0};
  memcpy(e + at, rec1, sizeof(rec1));
  at += sizeof(rec1);
  DWORD rec2[] = {37, 12, 1};
  memcpy(e + at, rec2, sizeof(rec2));
  at += sizeof(rec2);
  /* EMR_SELECTOBJECT NULL_PEN (stock 8) */
  DWORD rec3[] = {37, 12, 0x80000008};
  memcpy(e + at, rec3, sizeof(rec3));
  at += sizeof(rec3);
  /* EMR_RECTANGLE 10,10,60,40 */
  DWORD rec4[] = {43, 24, 10, 10, 60, 40};
  memcpy(e + at, rec4, sizeof(rec4));
  at += sizeof(rec4);
  /* EMR_EOF */
  DWORD eof[] = {14, 20, 0, 16, 20};
  memcpy(e + at, eof, sizeof(eof));
  at += sizeof(eof);
  put32(e + 48, at);
  put32(e + 52, 5 + 1);
  *size = at;
  return e;
}
static void metafiles(void) {
  /* Recording: state, objects, shapes, text and a comment. */
  GH mdc = CreateMetaFileW(0);
  CHECK(mdc && GetObjectType(mdc) == 4);
  GH pen = CreatePen(0, 1, 0x0000ff), brush = CreateSolidBrush(0x00ff00);
  GH font = CreateFontA(-11, 0, 0, 0, 700, 0, 0, 0, 0, 0, 0, 0, 0, "Arial");
  CHECK(SelectObject(mdc, pen) && SelectObject(mdc, brush) && SelectObject(mdc, font));
  CHECK(ExtEscape(mdc, 15, 5, "ICON", 0, 0) == 1);
  CHECK(SetMapMode(mdc, 8) && SetWindowExtEx(mdc, 100, 50, 0));
  CHECK(Rectangle(mdc, 10, 10, 60, 40) && Ellipse(mdc, 0, 0, 20, 20) &&
        MoveToEx(mdc, 0, 45, 0) && LineTo(mdc, 99, 45));
  POINT tri[3] = {{70, 5}, {95, 5}, {82, 30}};
  CHECK(Polygon(mdc, tri, 3) && TextOutW(mdc, 5, 0, L("Hi\xe9"), 3));
  CHECK(SetPixel(mdc, 1, 1, 0x123456) == 0x123456);
  GH hmf = CloseMetaFile(mdc);
  CHECK(hmf && GetObjectType(hmf) == 9 && GetObjectType(mdc) == 0);
  DWORD size = GetMetaFileBitsEx(hmf, 0, 0);
  uint8_t *bits = calloc(1, size);
  CHECK(size > 18 && GetMetaFileBitsEx(hmf, size, bits) == size &&
        w32_u16(bits) == 1 && w32_u16(bits + 2) == 9 && w32_u16(bits + 4) == 0x300 &&
        w32_u32(bits + 6) * 2 == size && w32_u16(bits + 10) == 3);
  CHECK(!GetMetaFileBitsEx(hmf, 4, bits) && GetLastError() == 122);
  /* Record functions in order. */
  static const unsigned expected[] = {0x02fa, 0x012d, 0x02fc, 0x012d, 0x02fb,
                                      0x012d, 0x0626, 0x0103, 0x020c, 0x041b,
                                      0x0418, 0x0214, 0x0213, 0x0324, 0x0a32,
                                      0x041f, 0x0000};
  DWORD at = 18;
  for (unsigned i = 0; i < sizeof(expected) / sizeof(*expected); i++) {
    CHECK(at + 6 <= size && w32_u16(bits + at + 4) == expected[i]);
    at += w32_u32(bits + at) * 2;
  }
  /* Playback onto a 100x50 pixel surface (MM_ANISOTROPIC 1:1). */
  struct gdi_surface s = gdi_surface(100, 50);
  PatBlt(s.dc, 0, 0, 100, 50, 0x00ff0062);
  SetMapMode(s.dc, 8);
  SetViewportExtEx(s.dc, 100, 50, 0);
  CHECK(PlayMetaFile(s.dc, hmf));
  CHECK(s.pixels[25 * 100 + 40] == 0x00ff00 && s.pixels[10 * 100 + 30] == 0xff0000 &&
        s.pixels[45 * 100 + 50] == 0xff0000);
  /* Copies, bits round trip and files. */
  GH copy = CopyMetaFileW(hmf, 0), again = SetMetaFileBitsEx(size, bits);
  CHECK(copy && again && GetMetaFileBitsEx(copy, 0, 0) == size);
  char path[1100];
  snprintf(path, sizeof(path), "%s/PIC.WMF", drive_dir);
  GH file = CopyMetaFileW(hmf, L("C:\\PIC.WMF"));
  FILE *f = fopen(path, "rb");
  CHECK(file && f);
  fclose(f);
  CHECK(DeleteMetaFile(copy) && DeleteMetaFile(again) && DeleteMetaFile(file) &&
        !DeleteMetaFile(file) && !SetMetaFileBitsEx(10, bits));
  /* Without an embedded EMF, SetWinMetaFileBits converts the records. */
  struct {
    LONG mm, x, y;
    uint32_t h;
  } pict = {8, 2646, 1323, 0};
  GH converted = SetWinMetaFileBits(size, bits, 0, &pict);
  CHECK(converted && GetObjectType(converted) == 13);
  PatBlt(s.dc, 0, 0, 100, 50, 0x00ff0062);
  SetMapMode(s.dc, 1);
  RECT whole = {0, 0, 100, 50};
  CHECK(PlayEnhMetaFile(s.dc, converted, &whole));
  /* Objects are not converted: the rectangle uses the default pen. */
  CHECK(s.pixels[10 * 100 + 30] == 0 && s.pixels[45 * 100 + 50] == 0);
  CHECK(DeleteEnhMetaFile(converted) && DeleteMetaFile(hmf));
  free(bits);
  /* Enhanced metafiles: bits, header, playback scaled to a rectangle. */
  DWORD esize;
  uint8_t *emf = authored_emf(&esize);
  GH hemf = SetEnhMetaFileBits(esize, emf);
  CHECK(hemf && GetEnhMetaFileBits(hemf, 0, 0) == esize);
  uint8_t header[100];
  CHECK(GetEnhMetaFileHeader(hemf, 0, 0) == 88 &&
        GetEnhMetaFileHeader(hemf, sizeof(header), header) == 88 &&
        w32_u32(header + 40) == 0x464d4520);
  PatBlt(s.dc, 0, 0, 100, 50, 0x00ff0062);
  CHECK(PlayEnhMetaFile(s.dc, hemf, &whole));
  CHECK(s.pixels[25 * 100 + 30] == 0x0000ff && s.pixels[5 * 100 + 5] == 0xffffff);
  RECT half = {0, 0, 50, 25};
  PatBlt(s.dc, 0, 0, 100, 50, 0x00ff0062);
  CHECK(PlayEnhMetaFile(s.dc, hemf, &half) && s.pixels[12 * 100 + 15] == 0x0000ff &&
        s.pixels[30 * 100 + 40] == 0xffffff);
  CHECK(!PlayEnhMetaFile(s.dc, hemf, 0) && GetLastError() == 87);
  GH ecopy = CopyEnhMetaFileW(hemf, 0);
  CHECK(ecopy && GetEnhMetaFileBits(ecopy, 0, 0) == esize);
  CHECK(!SetEnhMetaFileBits(40, emf));
  /* EMF -> WMF keeps the EMF (MFCOMMENT 'WMFC'); back -> identical bits. */
  DWORD wsize = GetWinMetaFileBits(hemf, 0, 0, 8, 0);
  uint8_t *wmf = calloc(1, wsize);
  CHECK(wsize > esize && GetWinMetaFileBits(hemf, wsize, wmf, 8, 0) == wsize &&
        w32_u16(wmf + 18 + 4) == 0x0626 && w32_u32(wmf + 18 + 10) == 0x43464d57);
  GH restored = SetWinMetaFileBits(wsize, wmf, 0, 0);
  uint8_t *round_trip = calloc(1, esize);
  CHECK(restored && GetEnhMetaFileBits(restored, esize, round_trip) == esize &&
        !memcmp(round_trip, emf, esize));
  GH wmf_handle = SetMetaFileBitsEx(wsize, wmf);
  PatBlt(s.dc, 0, 0, 100, 50, 0x00ff0062);
  SetMapMode(s.dc, 8);
  SetViewportExtEx(s.dc, 100, 50, 0);
  CHECK(wmf_handle && PlayMetaFile(s.dc, wmf_handle));
  CHECK(!GetWinMetaFileBits(hemf, 0, 0, 0, 0) && GetLastError() == 87);
  CHECK(DeleteMetaFile(wmf_handle) && DeleteEnhMetaFile(restored) &&
        DeleteEnhMetaFile(ecopy) && DeleteEnhMetaFile(hemf) && !DeleteEnhMetaFile(hemf));
  free(wmf);
  free(round_trip);
  DeleteObject(pen);
  DeleteObject(brush);
  DeleteObject(font);
  /* DrawIcon on a metafile DC records the mask and the image. */
  GH icon_dc = CreateMetaFileW(0);
  CHECK(DrawIcon((HDC)(uintptr_t)icon_dc, 0, 0, LoadIconA(0, (const char *)(uintptr_t)32512)));
  GH icon_mf = CloseMetaFile(icon_dc);
  size = GetMetaFileBitsEx(icon_mf, 0, 0);
  bits = calloc(1, size);
  CHECK(GetMetaFileBitsEx(icon_mf, size, bits) == size && w32_u16(bits + 18 + 4) == 0x0f43);
  free(bits);
  DeleteMetaFile(icon_mf);
  gdi_free_surface(s);
  puts("PASS f5-20B metafiles recording playback bits copies files EMF headers scaling WMF/EMF conversion embedded round trip icons");
}

static void test_upper_user(void) {
  DestroyWindow(owner_window("Init", WS_CAPTION, 60, 40));
  unicode_windows();
  unicode_dialogs();
  unicode_menus();
  text_functions();
  ddeml();
  upper_windows();
  upper_resources();
}
static void test_upper_gdi(void) {
  DestroyWindow(owner_window("Init", WS_CAPTION, 60, 40));
  gdi_upper();
  uniscribe();
  metafiles();
}
