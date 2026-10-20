/* f5-10C2 authored guest fixture for the w32-dialogs probe: one DIALOGEX
 * with every standard control class, a menu bar and popup, and a MessageBox,
 * all through the production USER/GDI personality, with the glyphs drawn by
 * the PE-to-native font bridge. Results go to the controller.h block that
 * the kernel reads; failures also go to stderr. The same source runs in the host
 * dialogs harness (W32_DIALOGS_HOST) for the dry-run of the suite case.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/controls.h>
#include <w32/font_stats.h>
#include <w32/user32.h>
#include <ciuki/abi.h>
#include "controller.h"
#include "exercise.h"

#ifndef RDW_INVALIDATE
#define RDW_INVALIDATE 0x0001u
#define RDW_ERASE 0x0004u
#define RDW_ALLCHILDREN 0x0080u
#define RDW_UPDATENOW 0x0100u
#endif
#define RESOURCE(id) ((const char *)(uintptr_t)(id))

static struct w32_dialogs_result *R;
static const struct w32_dialogs_env *E;
static HWND owner, dialog;
static UINT commands[8], command_count, idle_kind, idle_step, idle_disabled;
extern HANDLE W32API FindResourceA(HANDLE, const void *, const void *);
extern HANDLE W32API LoadResource(HANDLE, HANDLE);
extern void *W32API LockResource(HANDLE);
extern DWORD W32API SizeofResource(HANDLE, HANDLE);
/* LOGFONTA ABI, without mixing USER's host pointer handles with GDI's
 * DWORD handles. The PE uses the same 60-byte structure. */
struct exercise_logfont {
  LONG height, width, escapement, orientation, weight;
  uint8_t italic, underline, strike, charset, out, clip, quality, pitch;
  char face[32];
};
extern DWORD W32API CreateFontIndirectA(const struct exercise_logfont *);
extern BOOL W32API DeleteObject(DWORD);
_Static_assert(sizeof(struct exercise_logfont) == 60, "LOGFONTA ABI");
static void stderr_line(const char *text, DWORD bytes) {
#ifdef W32_HOST_MODEL
  extern void dialogs_fixture_stderr(const char *, DWORD);
  dialogs_fixture_stderr(text, bytes);
#else
  /* The desktop spawns this GUI PE with inherited native fd 2. It has no
   * STARTF_USESTDHANDLES/Win32 stderr handle; use the existing native write
   * ABI so diagnostics reach the console/serial log without a CRT. */
  int32_t written;
  __asm__ volatile("pushl %%ebp; xorl %%ebp,%%ebp; int $0x80; popl %%ebp"
                   : "=a"(written)
                   : "a"(CIUKI_SYS_WRITE), "b"(2), "c"(text), "d"(bytes),
                     "S"(0), "D"(0)
                   : "memory", "cc");
  (void)written;
#endif
}
static UINT append(char *out, UINT at, const char *s) {
  while (*s && at < 220)
    out[at++] = *s++;
  return at;
}
static UINT decimal(char *out, UINT at, DWORD value) {
  char digits[10];
  UINT n = 0;
  do {
    digits[n++] = (char)('0' + value % 10);
    value /= 10;
  } while (value);
  while (n && at < 240)
    out[at++] = digits[--n];
  return at;
}
static void diagnostic(const char *step, UINT line, DWORD error) {
  char out[256];
  UINT n = append(out, 0, "dialogs-exercise: step=");
  for (UINT i = 0; step[i] && i < 130; i++) {
    char c = step[i];
    out[n++] = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '_'
                   ? c : '_';
  }
  n = append(out, n, " line=");
  n = decimal(out, n, line);
  n = append(out, n, " last_error=");
  n = decimal(out, n, error);
  out[n++] = '\n';
  stderr_line(out, n);
  SetLastError(error);
}
#define CHECK_STEP(counter, step, e)                                            \
  do {                                                                         \
    if (!(e)) {                                                                \
      DWORD check_error = GetLastError();                                       \
      R->counter++;                                                            \
      if (!R->first_error)                                                     \
        R->first_error = __LINE__;                                             \
      diagnostic(step, __LINE__, check_error);                                  \
    }                                                                          \
  } while (0)
#define CHECK(counter, e) CHECK_STEP(counter, #e, e)

static UINT read16(const uint8_t *p) { return p[0] | ((UINT)p[1] << 8); }
static DWORD read32(const uint8_t *p) {
  return read16(p) | ((DWORD)read16(p + 2) << 16);
}
static void resource_diagnostic(const uint8_t *p, DWORD size) {
  char out[256];
  UINT n = append(out, 0, "dialogs-exercise: resource_size=");
  n = decimal(out, n, size);
  n = append(out, n, " dlgver=");
  n = decimal(out, n, size >= 2 ? read16(p) : 0);
  n = append(out, n, " signature=");
  n = decimal(out, n, size >= 4 ? read16(p + 2) : 0);
  out[n++] = '\n';
  DWORD error = GetLastError();
  stderr_line(out, n);
  SetLastError(error);
}

/* Read only the extended header/font fields we diagnose. USER still owns
 * parsing and creating the complete compiled template. */
static BOOL skip_name(const uint8_t *p, DWORD size, DWORD *at) {
  if (*at > size || size - *at < 2)
    return 0;
  UINT c = read16(p + *at);
  *at += 2;
  if (c == 0xffff) {
    if (size - *at < 2)
      return 0;
    *at += 2;
    return 1;
  }
  while (c) {
    if (size - *at < 2)
      return 0;
    c = read16(p + *at);
    *at += 2;
  }
  return 1;
}
static BOOL template_font(const uint8_t *p, DWORD size,
                           struct exercise_logfont *lf) {
  DWORD at = 26;
  if (size < at || read16(p) != 1 || read16(p + 2) != 0xffff ||
      !(read32(p + 12) & DS_SETFONT))
    return 0;
  for (UINT i = 0; i < 3; i++)
    if (!skip_name(p, size, &at))
      return 0;
  if (size - at < 6)
    return 0;
  lf->height = -((int)read16(p + at) * 96 + 36) / 72;
  lf->weight = read16(p + at + 2);
  lf->italic = p[at + 4];
  lf->charset = p[at + 5];
  at += 6;
  for (UINT i = 0; i < sizeof(lf->face); i++) {
    if (size - at < 2)
      return 0;
    UINT c = read16(p + at);
    at += 2;
    if (c > 127)
      return 0; /* This authored fixture names an ASCII face. */
    lf->face[i] = (char)c;
    if (!c)
      return 1;
  }
  return 0;
}

static HWND target(void) {
  HWND f = GetFocus();
  return f ? f : GetActiveWindow();
}
static void post_key(UINT vk, BOOL down) {
  PostMessageA(target(), down ? WM_KEYDOWN : WM_KEYUP, vk,
               (LPARAM)(down ? 1u : 0xc0000001u));
}
static void key(UINT vk) {
  post_key(vk, 1);
  post_key(vk, 0);
}
static void shift_tab(void) {
  post_key(VK_SHIFT, 1);
  key(VK_TAB);
  post_key(VK_SHIFT, 0);
}
static void sys_char(UINT c) { PostMessageA(target(), WM_SYSCHAR, c, 1); }
static void character(UINT c) { PostMessageA(target(), WM_CHAR, c, 1); }

static intptr_t W32API dialog_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  (void)h, (void)lp;
  if (m == WM_INITDIALOG)
    return 1;
  if (m == WM_COMMAND && !(wp >> 16) && command_count < 8)
    commands[command_count++] = (UINT)(wp & 0xffff);
  return 0;
}
/* Modal loops (MessageBox, TrackPopupMenu) send WM_ENTERIDLE to the owner
 * when their queue is empty: the scripted keys come from here. */
static LRESULT W32API owner_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  if (m == WM_ENTERIDLE) {
    UINT step = idle_step++;
    if (step > 8)
      return 0;
    if (idle_kind == 1 && wp == MSGF_DIALOGBOX && step == 0) {
      idle_disabled = !IsWindowEnabled(owner) && IsWindow((HWND)lp);
      key(VK_RETURN);
    } else if (idle_kind == 2 && wp == MSGF_MENU)
      key(step == 0 ? VK_DOWN : VK_RETURN);
    return 0;
  }
  return DefWindowProcA(h, m, wp, lp);
}

static const struct {
  int id;
  const char *class_name;
} expected[] = {{709, "Static"},   {701, "Edit"},     {702, "Button"},
                {703, "Button"},   {704, "Button"},   {705, "ListBox"},
                {706, "ComboBox"}, {707, "Edit"},     {708, "ScrollBar"},
                {710, "Button"},   {711, "Static"},   {IDOK, "Button"},
                {IDCANCEL, "Button"}};
#define CONTROLS (sizeof(expected) / sizeof(*expected))
static HWND windows[CONTROLS + 2];

static int same(const char *a, const char *b) {
  for (; *a || *b; a++, b++) {
    int x = *a >= 'a' && *a <= 'z' ? *a - 32 : *a;
    int y = *b >= 'a' && *b <= 'z' ? *b - 32 : *b;
    if (x != y)
      return 0;
  }
  return 1;
}
static int focus_is(int id) { return GetFocus() == GetDlgItem(dialog, id); }
static void step(int ok) {
  if (ok)
    R->steps++;
  else
    CHECK(keyboard_errors, 0);
}

static void units(void) {
  LONG base = GetDialogBaseUnits();
  R->system_x = (uint32_t)(base & 0xffff);
  R->system_y = (uint32_t)((DWORD)base >> 16);
  RECT r = {0, 0, 4, 8};
  CHECK(units_errors, MapDialogRect(dialog, &r));
  R->dialog_x = (uint32_t)r.right;
  R->dialog_y = (uint32_t)r.bottom;
  CHECK(units_errors, R->system_x == 8 && R->system_y == 16);
  CHECK(units_errors, R->dialog_x == 6 && R->dialog_y == 13);
}
static void controls(void) {
  const char *seen[CONTROLS];
  UINT distinct = 0;
  for (UINT i = 0; i < CONTROLS; i++) {
    HWND c = GetDlgItem(dialog, expected[i].id);
    char name[32] = {0};
    windows[2 + i] = c;
    if (c && GetClassNameA(c, name, sizeof(name)) &&
        same(name, expected[i].class_name)) {
      R->controls++;
      UINT k = 0;
      while (k < distinct && !same(seen[k], expected[i].class_name))
        k++;
      if (k == distinct)
        seen[distinct++] = expected[i].class_name;
    } else
      CHECK(controls_errors, 0);
  }
  R->classes = distinct;
  CHECK(controls_errors, distinct == 6 && R->controls == CONTROLS);
  static const char *const items[] = {"Liberation Sans", "Liberation Serif",
                                      "Liberation Mono"};
  for (UINT i = 0; i < 3; i++) {
    CHECK(controls_errors,
          SendDlgItemMessageA(dialog, 705, LB_ADDSTRING, 0,
                              (LPARAM)(uintptr_t)items[i]) == (LRESULT)i);
    CHECK(controls_errors,
          SendDlgItemMessageA(dialog, 706, CB_ADDSTRING, 0,
                              (LPARAM)(uintptr_t)items[i]) == (LRESULT)i);
  }
  SendDlgItemMessageA(dialog, 705, LB_SETCURSEL, 0, 0);
  SendDlgItemMessageA(dialog, 706, CB_SETCURSEL, 0, 0);
  CHECK(controls_errors,
        SetDlgItemTextA(dialog, 707, "Classic computing,\r\none woof at a time."));
  char text[64] = {0};
  CHECK(controls_errors, GetDlgItemTextA(dialog, 707, text, sizeof(text)) == 39);
  /* Paint every control: the text goes through the font bridge. */
  RedrawWindow(dialog, 0, 0,
               RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
  E->pump(dialog, 1);
}
static void keyboard(void) {
  E->pump(dialog, 0);
  CHECK(keyboard_errors, focus_is(701));
  key(VK_TAB), E->pump(dialog, 0), step(focus_is(702));
  key(VK_SPACE), E->pump(dialog, 0), step(IsDlgButtonChecked(dialog, 702) == 1);
  key(VK_TAB), E->pump(dialog, 0), step(focus_is(703));
  key(VK_DOWN), E->pump(dialog, 0);
  step(focus_is(704) && IsDlgButtonChecked(dialog, 704) == 1 &&
       IsDlgButtonChecked(dialog, 703) == 0);
  key(VK_UP), E->pump(dialog, 0);
  step(focus_is(703) && IsDlgButtonChecked(dialog, 703) == 1);
  key(VK_TAB), E->pump(dialog, 0), step(focus_is(705));
  key(VK_DOWN), E->pump(dialog, 0);
  step(SendDlgItemMessageA(dialog, 705, LB_GETCURSEL, 0, 0) == 1);
  key(VK_TAB), E->pump(dialog, 0), step(focus_is(706));
  key(VK_DOWN), E->pump(dialog, 0);
  step(SendDlgItemMessageA(dialog, 706, CB_GETCURSEL, 0, 0) == 1);
  key(VK_TAB), E->pump(dialog, 0), step(focus_is(IDOK));
  key(VK_TAB), E->pump(dialog, 0), step(focus_is(IDCANCEL));
  key(VK_TAB), E->pump(dialog, 0), step(focus_is(701));
  shift_tab(), E->pump(dialog, 0), step(focus_is(IDCANCEL));
  shift_tab(), E->pump(dialog, 0), step(focus_is(IDOK));
  /* Mnemonics: a check box is clicked, a label focuses the next control. */
  sys_char('c'), E->pump(dialog, 0);
  step(focus_is(702) && IsDlgButtonChecked(dialog, 702) == 0);
  command_count = 0;
  key(VK_RETURN), E->pump(dialog, 0);
  step(command_count == 1 && commands[0] == IDOK);
  key(VK_ESCAPE), E->pump(dialog, 0);
  step(command_count == 2 && commands[1] == IDCANCEL);
  sys_char('n'), E->pump(dialog, 0), step(focus_is(701));
  static const char word[] = "Ciuki";
  for (UINT i = 0; word[i]; i++)
    character((UINT)(unsigned char)word[i]);
  E->pump(dialog, 0);
  char text[16] = {0};
  GetDlgItemTextA(dialog, 701, text, sizeof(text));
  step(same(text, word) && text[0] == 'C' && text[1] == 'i');
}
static HMENU bar, file;
static void menus(void) {
  bar = LoadMenuA(E->instance, RESOURCE(601));
  CHECK(menu_errors, bar && IsMenu(bar) && GetMenuItemCount(bar) == 2);
  file = bar ? GetSubMenu(bar, 0) : 0;
  CHECK(menu_errors, file && GetMenuItemCount(file) == 4);
  if (!file)
    return;
  R->menu_items = (uint32_t)GetMenuItemCount(file);
  CHECK(menu_errors, GetMenuState(file, 802, MF_BYCOMMAND) & MF_GRAYED);
  CHECK(menu_errors, CheckMenuItem(bar, 804, MF_UNCHECKED) == MF_CHECKED);
  CHECK(menu_errors, EnableMenuItem(bar, 802, MF_ENABLED) == MF_GRAYED);
  CHECK(menu_errors, SetMenu(owner, bar) && GetMenu(owner) == bar);
  CHECK(menu_errors, DrawMenuBar(owner));
  E->pump(0, 1);
  idle_kind = 2, idle_step = 0;
  UINT command = (UINT)TrackPopupMenu(file, TPM_RETURNCMD, 40, 40, 0, owner, 0);
  idle_kind = 0;
  CHECK(menu_errors, command == 801);
}
static void message_box(void) {
  idle_kind = 1, idle_step = 0, idle_disabled = 0;
  R->msgbox_result = (uint32_t)MessageBoxA(
      owner, "Classic computing, one woof at a time.", "CiukiOS",
      MB_OKCANCEL | 0x40u /* MB_ICONINFORMATION */);
  idle_kind = 0;
  CHECK(msgbox_errors, R->msgbox_result == IDOK);
  CHECK(msgbox_errors, idle_disabled && IsWindowEnabled(owner));
}
static void fonts(void) {
  struct w32_gdi_font_stats f = {0};
  struct w32_gdi_bridge_stats b = {0};
  w32_gdi_font_statistics(&f);
  BOOL service = w32_gdi_bridge_statistics(&b);
  R->total_peak = f.total_peak, R->glyph_peak = f.glyph_peak;
  R->face_peak = f.face_peak, R->hits = f.hits, R->misses = f.misses;
  R->evictions = f.evictions;
  R->bridge_glyphs = b.glyphs, R->bridge_refusals = b.refusals;
  R->bridge_errors = b.errors;
  R->service_peak = b.service_face_peak, R->service_budget = b.service_budget;
  R->service_glyphs = b.service_glyphs;
  CHECK(font_errors, service && b.glyphs && b.service_glyphs && f.hits);
  CHECK(font_errors, !b.refusals && !b.errors);
  CHECK(font_errors, f.total_peak && f.total_peak <= 2u * 1024u * 1024u);
  CHECK(font_errors, b.service_face_peak &&
                         b.service_face_peak <= b.service_budget);
}
static void teardown(void) {
  CHECK(teardown_errors, SetMenu(owner, 0));
  CHECK(teardown_errors, DestroyMenu(bar));
  CHECK(teardown_errors, DestroyWindow(dialog));
  CHECK(teardown_errors, DestroyWindow(owner));
  E->pump(0, 0);
  for (UINT i = 0; i < CONTROLS + 2; i++)
    R->live_windows += windows[i] && IsWindow(windows[i]);
  R->live_menus = (uint32_t)(!!IsMenu(bar) + !!IsMenu(file));
  CHECK(teardown_errors, !R->live_windows && !R->live_menus);
}

void w32_dialogs_run(const struct w32_dialogs_env *env,
                     struct w32_dialogs_result *result) {
  R = result, E = env;
  WNDCLASSA c = {.style = 8,
                 .lpfnWndProc = owner_proc,
                 .hInstance = env->instance,
                 .hbrBackground = (HBRUSH)(uintptr_t)(COLOR_WINDOW + 1),
                 .lpszClassName = "CiukiDialogsOwner"};
  CHECK(errors, RegisterClassA(&c));
  owner = CreateWindowExA(0, "CiukiDialogsOwner", "Ciuki dialogs",
                          0x00cf0000u /* WS_OVERLAPPEDWINDOW */ | WS_VISIBLE, 16, 16, 560, 400, 0,
                          0, env->instance, 0);
  CHECK(errors, owner);
  SetLastError(0);
  HANDLE resource = FindResourceA(env->instance, RESOURCE(600), RESOURCE(5));
  CHECK_STEP(errors, "FindResourceA", resource);
  HANDLE loaded = resource ? LoadResource(env->instance, resource) : 0;
  if (resource)
    CHECK_STEP(errors, "LoadResource", loaded);
  const uint8_t *bytes = loaded ? LockResource(loaded) : 0;
  if (loaded)
    CHECK_STEP(errors, "LockResource", bytes);
  DWORD size = resource ? SizeofResource(env->instance, resource) : 0;
  if (resource)
    CHECK_STEP(errors, "SizeofResource", size >= 26);
  if (bytes)
    resource_diagnostic(bytes, size);
  BOOL signature = bytes && size >= 26 && read16(bytes) == 1 &&
                   read16(bytes + 2) == 0xffff;
  if (bytes && size >= 26)
    CHECK_STEP(errors, "DLGTEMPLATEEX_signature", signature);
  if (signature) {
    struct exercise_logfont lf = {0};
    BOOL named = template_font(bytes, size, &lf);
    CHECK_STEP(errors, "DLGTEMPLATEEX_font", named);
    if (named) {
      SetLastError(0);
      DWORD font = CreateFontIndirectA(&lf);
      DWORD error = GetLastError();
      CHECK_STEP(errors, "CreateFontIndirectA", font);
      if (font) {
        diagnostic("CreateFontIndirectA", __LINE__, error);
        CHECK_STEP(errors, "DeleteObject_font", DeleteObject(font));
      }
      struct w32_gdi_bridge_stats bridge;
      CHECK_STEP(errors, "font_bridge", w32_gdi_bridge_statistics(&bridge));
    }
  }
  dialog = owner && signature
               ? CreateDialogIndirectParamA(env->instance,
                    (const DLGTEMPLATE *)bytes, owner, dialog_proc, 0) : 0;
  if (owner && signature)
    CHECK_STEP(errors, "CreateDialogIndirectParamA", dialog);
  windows[0] = owner, windows[1] = dialog;
  if (owner && dialog) {
    env->pump(dialog, 1);
    units();
    controls();
    keyboard();
    menus();
    message_box();
    fonts();
  }
  teardown();
  R->errors += R->units_errors + R->controls_errors + R->keyboard_errors +
               R->menu_errors + R->msgbox_errors + R->font_errors;
}

#ifndef W32_DIALOGS_HOST
/* Guest entry: no CRT. The kernel reads the block, then sets release. */
void *W32API GetModuleHandleA(const char *);
W32_NORETURN void W32API ExitProcess(DWORD);
static void pe_pump(HWND dlg, int settle) {
  /* settle: let the desktop answer placement (CONFIGURE) and paint. */
  for (UINT round = 0; round < (settle ? 40u : 2u); round++) {
    MSG m;
    for (UINT guard = 0; guard < 4000 && PeekMessageA(&m, 0, 0, 0, PM_REMOVE);
         guard++)
      if (!dlg || !IsWindow(dlg) || !IsDialogMessageA(dlg, &m)) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
      }
    Sleep(settle ? 10 : 1);
  }
}
void W32API entry(void) {
  struct w32_dialogs_result *r = VirtualAlloc(
      (void *)(uintptr_t)W32_DIALOGS_RESULT_VA, 4096, 0x3000u, 4u);
  if (!r)
    ExitProcess(2);
  r->magic = W32_DIALOGS_MAGIC;
  r->version = W32_DIALOGS_VERSION;
  /* The desktop starts w32srv just before this process: wait (5 s at most)
   * until it answers, font engine included. */
  struct w32_gdi_bridge_stats ready;
  for (UINT i = 0; i < 500 && !w32_gdi_bridge_statistics(&ready); i++)
    Sleep(10);
  const struct w32_dialogs_env env = {GetModuleHandleA(0), pe_pump};
  w32_dialogs_run(&env, r);
  __atomic_store_n(&r->done, 1u, __ATOMIC_RELEASE);
  for (UINT i = 0; i < 3000 && !__atomic_load_n(&r->release, __ATOMIC_ACQUIRE);
       i++)
    Sleep(10);
  ExitProcess(r->errors ? 1 : 0);
}
#endif
