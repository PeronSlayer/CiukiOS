/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "unicode.h"
struct hook {
  DWORD id, tid;
  int kind;
  HOOKPROC proc;
  BOOL unicode; /* f5-20B: installed by SetWindowsHookExW */
};
struct hook_frame {
  struct hook_frame *prev;
  DWORD tid;
  int kind;
  DWORD limit;
  int code;
  WPARAM wp;
  LPARAM lp;
};
static struct hook hooks[32];

static DWORD serial;
/* fx-hlrun: whether this thread has a hook of KIND (WH_CBT: a hook saw a
 * window CreateWindowEx then could not finish). */
BOOL u32_hook_set(int kind) {
  DWORD tid = u32_tid();
  for (UINT i = 0; i < 32; i++)
    if (hooks[i].proc && hooks[i].kind == kind && hooks[i].tid == tid)
      return 1;
  return 0;
}
void u32_hooks_cleanup(DWORD tid) {
  for (UINT i = 0; i < 32; i++)
    if (!tid || hooks[i].tid == tid) w32_fill(&hooks[i], 0, sizeof(hooks[i]));
}
/* Thread hooks: WH_MSGFILTER (-1), WH_KEYBOARD (2), WH_GETMESSAGE (3),
 * WH_CALLWNDPROC (4), WH_CBT (5) and WH_MOUSE (7). fx-hlrun: WH_CBT, which
 * every MFC program installs (AfxHookWindowCreate subclasses each window it
 * creates in HCBT_CREATEWND; Half-Life's launcher is one), is called as
 * Windows 98 USER does (Microsoft "CBTProc callback function"; Wine 10.0
 * dlls/win32u: window.c HCBT_CREATEWND/HCBT_DESTROYWND/HCBT_MINMAX,
 * input.c HCBT_ACTIVATE/HCBT_SETFOCUS, defwnd.c HCBT_SYSCOMMAND,
 * message.c HCBT_KEYSKIPPED/HCBT_CLICKSKIPPED). The desktop owns interactive
 * move/size, so HCBT_MOVESIZE has no USER-side loop to come from; HCBT_QS
 * (WM_QUEUESYNC for computer-based training players) is never generated. */
HANDLE W32API SetWindowsHookExA(int kind, HOOKPROC proc, HINSTANCE module,
                                DWORD tid) {
  U32_GUARD;
  (void)module;
  if (!proc || tid != u32_tid() ||
      (kind != 2 && kind != 7 && kind != 3 && kind != 4 && kind != 5 && kind != -1)) {
    u32_fail(120);
    return 0;
  }
  for (UINT i = 0; i < 32; i++)
    if (!hooks[i].id) {
      hooks[i] = (struct hook){++serial, tid, kind, proc, 0};
      return (HANDLE)(uintptr_t)serial;
    }
  u32_fail(8);
  return 0;
}
void u32_hook_unicode(HANDLE h) {
  for (UINT i = 0; i < 32; i++)
    if (hooks[i].id == U32_PTR(h))
      hooks[i].unicode = 1;
}
BOOL W32API UnhookWindowsHookEx(HANDLE h) {
  U32_GUARD;
  for (UINT i = 0; i < 32; i++)
    if (hooks[i].id == U32_PTR(h) && hooks[i].tid == u32_tid()) {
      w32_fill(&hooks[i], 0, sizeof(hooks[i]));
      return 1;
    }
  return u32_fail(1404);
}
static LRESULT next(struct hook_frame *f, int code, WPARAM wp, LPARAM lp) {
  int best = -1;
  for (int i = 0; i < 32; i++)
    if (hooks[i].id && hooks[i].id < f->limit && hooks[i].tid == f->tid &&
        hooks[i].kind == f->kind && (best < 0 || hooks[i].id > hooks[best].id))
      best = i;
  if (best < 0)
    return 0;
  DWORD before = f->limit;
  f->limit = hooks[best].id;
  HOOKPROC proc = hooks[best].proc;
  /* f5-20B: a Unicode hook sees the character of a WH_GETMESSAGE or message
   * filter MSG (WH_CALLWNDPROC: CWPSTRUCT) in Unicode form; changes made by
   * a WH_GETMESSAGE hook are carried back in ANSI form. */
  MSG *msg = 0, copy;
  struct cwp {
    LPARAM lParam;
    WPARAM wParam;
    UINT message;
    HWND hwnd;
  } *cwp = 0, cwp_copy;
  /* A Unicode WH_CBT hook sees HCBT_CREATEWND's CREATESTRUCT with Unicode
   * strings (atoms pass through); only hwndInsertAfter is copied back, as
   * in Wine's call_hook_AtoW. */
  struct cbt_create {
    CREATESTRUCTA *cs;
    HWND after;
  } *cbt = 0, cbt_copy;
  CREATESTRUCTA cs_copy;
  void *cbt_name = 0, *cbt_class = 0;
  if (hooks[best].unicode && lp && f->kind == 5 && code == 3) {
    cbt = (struct cbt_create *)lp;
    cs_copy = *cbt->cs;
    cbt_name = cs_copy.lpszName ? u32_widen(cs_copy.lpszName) : 0;
    cbt_class = cs_copy.lpszClass ? u32_widen(cs_copy.lpszClass) : 0;
    cs_copy.lpszName = cbt_name;
    cs_copy.lpszClass = cbt_class;
    cbt_copy = (struct cbt_create){&cs_copy, cbt->after};
    lp = (LPARAM)&cbt_copy;
  } else if (hooks[best].unicode && lp && (f->kind == 3 || f->kind == -1)) {
    msg = (MSG *)lp;
    copy = *msg;
    if (copy.message == WM_CHAR || copy.message == WM_SYSCHAR ||
        copy.message == 0x103u || copy.message == 0x107u)
      copy.wParam = copy.wParam < 256 ? u32_scalar((uint8_t)copy.wParam)
                                      : copy.wParam;
    lp = (LPARAM)&copy;
  } else if (hooks[best].unicode && lp && f->kind == 4) {
    cwp = (struct cwp *)lp;
    cwp_copy = *cwp;
    if (cwp_copy.message == WM_CHAR || cwp_copy.message == WM_SYSCHAR)
      cwp_copy.wParam = cwp_copy.wParam < 256
                            ? u32_scalar((uint8_t)cwp_copy.wParam)
                            : cwp_copy.wParam;
    lp = (LPARAM)&cwp_copy;
  }
  UINT saved = u32_suspend();
  LRESULT r = proc(code, wp, lp);
  u32_resume(saved);
  if (cbt) {
    cbt->after = cbt_copy.after;
    u32_text_free(cbt_name);
    u32_text_free(cbt_class);
  }
  if (msg) {
    if (copy.message == WM_CHAR || copy.message == WM_SYSCHAR ||
        copy.message == 0x103u || copy.message == 0x107u)
      copy.wParam = (uint8_t)u32_ansi((unsigned)(copy.wParam & 0xffff));
    if (f->kind == 3)
      *msg = copy;
  }
  f->limit = before;
  return r;
}
LRESULT u32_hooks(int kind, int code, WPARAM wp, LPARAM lp) {
  struct u32_thread *t = u32_thread();
  if (!t)
    return 0;
  struct hook_frame f = {t->hook_frame, u32_tid(), kind, UINT32_MAX,
                         code,          wp,        lp};
  t->hook_frame = &f;
  LRESULT r = next(&f, code, wp, lp);
  t->hook_frame = f.prev;
  return r;
}
LRESULT W32API CallNextHookEx(HANDLE ignored, int code, WPARAM wp, LPARAM lp) {
  U32_GUARD;
  (void)ignored;
  struct u32_thread *t = u32_thread();
  for (struct hook_frame *f = t ? t->hook_frame : 0; f; f = f->prev)
    if (f->tid == u32_tid())
      return next(f, code, wp, lp);
  return 0;
}
BOOL W32API CallMsgFilterA(MSG *msg, int code) {
  U32_GUARD;
  return msg && !!u32_hooks(-1, code, 0, (LPARAM)msg);
}
