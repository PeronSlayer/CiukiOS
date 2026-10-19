/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"
struct hook {
  DWORD id, tid;
  int kind;
  HOOKPROC proc;
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
void u32_hooks_cleanup(DWORD tid) {
  for (UINT i = 0; i < 32; i++)
    if (!tid || hooks[i].tid == tid) w32_fill(&hooks[i], 0, sizeof(hooks[i]));
}
HANDLE W32API SetWindowsHookExA(int kind, HOOKPROC proc, HINSTANCE module,
                                DWORD tid) {
  U32_GUARD;
  (void)module;
  if (!proc || tid != u32_tid() ||
      (kind != 2 && kind != 7 && kind != 3 && kind != 4 && kind != -1)) {
    u32_fail(120);
    return 0;
  }
  for (UINT i = 0; i < 32; i++)
    if (!hooks[i].id) {
      hooks[i] = (struct hook){++serial, tid, kind, proc};
      return (HANDLE)(uintptr_t)serial;
    }
  u32_fail(8);
  return 0;
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
  UINT saved = u32_suspend();
  LRESULT r = proc(code, wp, lp);
  u32_resume(saved);
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
