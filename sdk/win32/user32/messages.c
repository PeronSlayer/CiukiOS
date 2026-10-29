/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"
#include <w32/controls.h>
BOOL u32_message(struct ciuki_w32_message_args *a) {
  a->header = K32_HEADER(W32_MESSAGE, *a);
  a->result = a->error = 0;
  UINT saved = u32_suspend();
  int32_t status = u32_backend.call(a, sizeof(*a));
  u32_resume(saved);
  return status ? u32_fail(87) : a->error ? u32_fail(a->error) : 1;
}
BOOL u32_pointer_message(UINT m) {
  switch (m) {
  case U32_DESTROY:
  case WM_CREATE:
  case WM_NCCREATE:
  case WM_GETMINMAXINFO:
  case WM_SETTEXT:
  case WM_GETTEXT:
  case WM_COPYDATA:
  case WM_PAINTCLIPBOARD:
  case WM_SIZECLIPBOARD:
  case WM_ASKCBFORMATNAME:
  case 0x2b:
  case 0x2c:
  case 0x46:
  case 0x47:
  case 0x83:
    return 1;
  default:
    return 0;
  }
}
BOOL u32_queue(DWORD tid, HWND h, UINT m, WPARAM wp, LPARAM lp, UINT bits,
               DWORD time, POINT pt) {
  struct ciuki_w32_message_args a = {
      .action = W32_MSG_POST,
      .tid = tid,
      .bits = bits,
      .message = {U32_PTR(h), m, (DWORD)wp, (DWORD)lp, time, pt.x, pt.y}};
  return u32_message(&a);
}
BOOL W32API PostThreadMessageA(DWORD tid, UINT m, WPARAM wp, LPARAM lp) {
  U32_GUARD;
  if (u32_pointer_message(m))
    return u32_fail(87);
  return u32_queue(tid, 0, m, wp, lp, QS_POSTMESSAGE, u32_ticks(), u32_pointer);
}
BOOL W32API PostMessageA(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  U32_GUARD;
  LRESULT dde;
  if (u32_progman_message(h,m,wp,lp,&dde)) return 1;
  if (u32_pointer_message(m))
    return u32_fail(87);
  if (h == (HWND)(uintptr_t)0xffff) {
    BOOL ok = 1;
    for (UINT i = 0; i < U32_WINDOWS; i++)
      if (u32_windows[i].handle && !u32_windows[i].parent)
        ok &= PostMessageA(u32_windows[i].handle, m, wp, lp);
    return ok;
  }
  DWORD tid = h ? GetWindowThreadProcessId(h, 0) : u32_tid();
  if (!tid)
    return u32_fail(1400);
  return u32_queue(tid, h, m, wp, lp, QS_POSTMESSAGE, u32_ticks(), u32_pointer);
}
void W32API PostQuitMessage(int code) {
  U32_GUARD;
  struct ciuki_w32_message_args a = {.action = W32_MSG_QUIT,
                                     .message = {.wparam = code}};
  u32_message(&a);
}
LRESULT W32API CallWindowProcA(WNDPROC proc, HWND h, UINT m, WPARAM wp,
                               LPARAM lp) {
  U32_GUARD;
  /* f5-20B: a 0xFFFFxxxx procedure handle names a Unicode procedure. */
  BOOL wide;
  WNDPROC p = u32_proc_decode((uintptr_t)proc, 0, &wide);
  return u32_call_proc(p, wide, 0, h, m, wp, lp);
}
LRESULT u32_invoke(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  struct u32_window *w = u32_window(h);
  if (!w)
    return 0;
  if (m == U32_DESTROY) {
    /* Private, system-range message: an application cannot forge it. */
    if (!w->destroy_request)
      return 0;
    w->destroy_request = 0;
    return u32_destroy(w, U32_NORMAL);
  }
  WNDPROC proc = w->proc;
  struct {
    LPARAM lParam;
    WPARAM wParam;
    UINT message;
    HWND hwnd;
  } call = {lp, wp, m, h};
  u32_hooks(4, 0, 0, (LPARAM)&call);
  return u32_call_proc(proc, w->unicode, 0, h, m, wp, lp);
}
BOOL u32_sent(void) {
  u32_session_pump();
  struct u32_thread *t = u32_thread();
  if (!t)
    return 0;
  struct ciuki_w32_message_args a = {.action = W32_MSG_GET,
                                     .bits = QS_SENDMESSAGE};
  /* First query discovers payload length without removing a short-buffer send.
   */
  if (!u32_message(&a) && GetLastError() != 122)
    return 0;
  if (a.error == 122) {
    a.buffer = 0;
    void *data = u32_alloc(a.bytes);
    if (!data)
      return 0;
    a.buffer = U32_PTR(data);
    a.error = a.result = 0;
    if (!u32_message(&a)) {
      u32_free(data);
      return 0;
    }
  }
  if (!a.result || !a.token)
    return 0;
  void *data = (void *)(uintptr_t)a.buffer;
  struct u32_send_frame frame = {t->send, a.token, data, a.bytes, 0};
  t->send = &frame;
  LPARAM lp = a.message.lparam;
  COPYDATASTRUCT copy;
  if (a.message.message == WM_COPYDATA && a.bytes >= 8) {
    copy.dwData = w32_u32(data);
    copy.cbData = w32_u32((uint8_t *)data + 4);
    copy.lpData = (uint8_t *)data + 8;
    if (copy.cbData > a.bytes - 8) {
      ReplyMessage(0);
      goto done;
    }
    lp = (LPARAM)&copy;
  }
  LRESULT result = u32_invoke((HWND)(uintptr_t)a.message.window,
                              a.message.message, a.message.wparam, lp);
  if (!frame.replied)
    ReplyMessage(result);
done:
  t->send = frame.prev;
  u32_free(data);
  return 1;
}
BOOL W32API InSendMessage(void) {
  U32_GUARD;
  struct u32_thread *t = u32_thread();
  return t && t->send;
}
BOOL W32API ReplyMessage(LRESULT result) {
  U32_GUARD;
  struct u32_thread *t = u32_thread();
  if (!t || !t->send || t->send->replied)
    return 0;
  struct u32_send_frame *f = t->send;
  struct ciuki_w32_message_args a = {.action = W32_MSG_REPLY,
                                     .token = f->token,
                                     .buffer = U32_PTR(f->data),
                                     .bytes = f->bytes,
                                     .message = {.lparam = (DWORD)result}};
  if (!u32_message(&a))
    return 0;
  f->replied = 1;
  return 1;
}
static LRESULT send(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT flags,
                    UINT timeout, uintptr_t *output, BOOL notify) {
  LRESULT display_result=0;
  int display=notify ? 0 : u32_clip_display_send(h,m,wp,lp,&display_result);
  if (display) {
    if (output) *output=display_result; return display>0;
  }
  DWORD pid = 0, tid = GetWindowThreadProcessId(h, &pid);
  if (!tid)
    return u32_fail(1400);
  if (tid == u32_tid()) {
    LRESULT r = u32_invoke(h, m, wp, lp);
    if (output)
      *output = r;
    return 1;
  }
  if ((pid != u32_pid() && u32_pointer_message(m) && m != WM_COPYDATA &&
       m != WM_GETTEXT && m != WM_SETTEXT) ||
      (notify && u32_pointer_message(m)))
    return u32_fail(87);
  uint8_t *buffer = 0;
  DWORD bytes = 0;
  if (m == WM_SETTEXT) {
    if (!lp)
      return u32_fail(87);
    bytes = w32_strlen((char *)lp) + 1;
    if (bytes > 65536)
      return u32_fail(87);
    buffer = u32_alloc(bytes);
    if (buffer)
      w32_copy(buffer, (void *)lp, bytes);
  } else if (m == WM_GETTEXT) {
    if (!lp || !wp || wp > 65536)
      return u32_fail(87);
    bytes = wp;
    buffer = u32_alloc(bytes);
  } else if (m == WM_COPYDATA) {
    const COPYDATASTRUCT *c = (void *)lp;
    if (!c || c->cbData > 65528 || (c->cbData && !c->lpData))
      return u32_fail(87);
    bytes = c->cbData + 8;
    buffer = u32_alloc(bytes);
    if (buffer) {
      w32_put32(buffer, c->dwData);
      w32_put32(buffer + 4, c->cbData);
      if (c->cbData)
        w32_copy(buffer + 8, c->lpData, c->cbData);
    }
  }
  if (bytes && !buffer)
    return 0;
  struct ciuki_w32_message_args a = {
      .action = notify ? W32_MSG_NOTIFY : W32_MSG_START,
      .tid = tid,
      .buffer = U32_PTR(buffer),
      .bytes = bytes,
      .message = {U32_PTR(h), m, (DWORD)wp, (DWORD)lp, u32_ticks(),
                  u32_pointer.x, u32_pointer.y}};
  if (!u32_message(&a)) {
    u32_free(buffer);
    return 0;
  }
  if (notify) {
    u32_free(buffer);
    return 1;
  }
  DWORD start = u32_ticks();
  while (a.token) {
    if (!(flags & 1))
      while (u32_sent()) {
      }
    a.action = W32_MSG_POLL_REPLY;
    if (!u32_message(&a)) {
      u32_free(buffer);
      return 0;
    }
    if (!a.token)
      break;
    if (timeout != W32_INFINITE && (DWORD)(u32_ticks() - start) >= timeout) {
      struct ciuki_w32_message_args cancel = {.action = W32_MSG_CANCEL,
                                              .token = a.token};
      u32_message(&cancel);
      u32_free(buffer);
      return u32_fail(1460);
    }
    UINT saved = u32_suspend();
    u32_backend.pause(1);
    u32_resume(saved);
  }
  if (m == WM_GETTEXT && bytes) {
    DWORD copied = a.result < bytes ? a.result : bytes - 1;
    w32_copy((void *)lp, buffer, copied + 1);
  }
  if (output)
    *output = (int32_t)a.result;
  u32_free(buffer);
  return 1;
}
LRESULT W32API SendMessageTimeoutA(HWND h, UINT m, WPARAM wp, LPARAM lp,
                                   UINT flags, UINT ms, uintptr_t *r) {
  U32_GUARD;
  LRESULT dde;
  if(u32_progman_message(h,m,wp,lp,&dde)){if(r)*r=dde;return 1;}
  if (flags & ~0x2bu)
    return u32_fail(87);
  return send(h, m, wp, lp, flags, ms, r, 0);
}
LRESULT W32API SendMessageA(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  U32_GUARD;
  LRESULT dde;
  if (u32_progman_message(h,m,wp,lp,&dde)) return dde;
  if (h == (HWND)(uintptr_t)0xffff || h == (HWND)(intptr_t)-1) {
    for (UINT i = 0; i < U32_WINDOWS; i++)
      if (u32_windows[i].handle && !u32_windows[i].parent)
        SendMessageA(u32_windows[i].handle, m, wp, lp);
    return 1;
  }
  uintptr_t r = 0;
  send(h, m, wp, lp, 0, W32_INFINITE, &r, 0);
  return (LRESULT)r;
}
BOOL W32API SendNotifyMessageA(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  U32_GUARD;
  return !!send(h, m, wp, lp, 0, 0, 0, 1);
}
static BOOL filter(HWND h, UINT m, HWND wanted, UINT lo, UINT hi) {
  return m == WM_QUIT ||
         ((!wanted || (wanted == (HWND)(intptr_t)-1
                           ? !h
                           : h == wanted || IsChild(wanted, h))) &&
          ((!lo && !hi) || (m >= lo && m <= hi)));
}
void u32_mark(void) {
  UINT bits = 0;
  DWORD tid = u32_tid(), now = u32_ticks();
  for (UINT i = 0; i < U32_WINDOWS; i++) {
    struct u32_window *w = &u32_windows[i];
    if (w->handle && w->tid == tid && w->redraw &&
        (w->update.n || w->internal_paint) && !IsIconic(w->handle) &&
        u32_paintable(w))
      bits |= QS_PAINT;
  }
  for (UINT i = 0; i < U32_TIMERS; i++)
    if (u32_timers[i].tid == tid && (int32_t)(now - u32_timers[i].due) >= 0)
      bits |= QS_TIMER;
  struct ciuki_w32_message_args a = {
      .action = W32_MSG_MARK,
      .bits = bits,
      .message = {.wparam = QS_PAINT | QS_TIMER}};
  u32_message(&a);
  /* A different GUI thread can invalidate our windows while we are blocked.
   * Publish its paint readiness now; the owner still synthesizes WM_PAINT. */
  DWORD marked[U32_WINDOWS];UINT count=0;
  for (UINT i=0;i<U32_WINDOWS;i++) {
    struct u32_window *w=&u32_windows[i];
    if (!w->handle || w->tid==tid || !w->redraw ||
        (!w->update.n && !w->internal_paint) || IsIconic(w->handle) || !u32_paintable(w)) continue;
    UINT j=0;while(j<count && marked[j]!=w->tid)j++;
    if(j<count)continue;
    marked[count++]=w->tid;
    struct ciuki_w32_message_args paint={.action=W32_MSG_MARK,.tid=w->tid,
        .bits=QS_PAINT,.message={.wparam=QS_PAINT}};
    u32_message(&paint);
  }
}
static void msg_from(MSG *m, const struct ciuki_w32_message *n) {
  *m = (MSG){(HWND)(uintptr_t)n->window, n->message, n->wparam,
             (int32_t)n->lparam,         n->time,    {n->x, n->y}};
}
BOOL W32API PeekMessageA(MSG *out, HWND h, UINT lo, UINT hi, UINT flags) {
  U32_GUARD;
  if (!out || lo > hi || (h && h != (HWND)(intptr_t)-1 && !IsWindow(h)))
    return u32_fail(1400);
  struct u32_thread *t = u32_thread();
  if (!t)
    return 0;
  UINT mask = flags >> 16;
  if (!mask)
    mask = QS_ALLINPUT;
  if (mask & QS_SENDMESSAGE)
    while (u32_sent()) {
    }
  u32_pump();
  u32_mark();
  DWORD windows[U32_WINDOWS], count = 0;
  if (h && h != (HWND)(intptr_t)-1)
    for (UINT i = 0; i < U32_WINDOWS; i++)
      if (u32_windows[i].handle &&
          (u32_windows[i].handle == h || IsChild(h, u32_windows[i].handle)))
        windows[count++] = U32_PTR(u32_windows[i].handle);
  struct ciuki_w32_message_args a = {
      .action = W32_MSG_SELECT,
      .bits = mask & ~QS_SENDMESSAGE,
      .token = flags & PM_REMOVE ? 1 : 0,
      .message = {.window = U32_PTR(h), .message = lo, .wparam = hi},
      .buffer = U32_PTR(windows),
      .bytes = count * 4};
  if (mask & (QS_POSTMESSAGE | QS_INPUT | QS_HOTKEY)) {
    if (!u32_message(&a))
      return 0;
    if (a.result) {
      msg_from(out, &a.message);
      goto found;
    }
  }
  if (mask & QS_SENDMESSAGE)
    while (u32_sent()) {
    }
  for (UINT i = 0; i < U32_WINDOWS; i++) {
    struct u32_window *w = &u32_windows[i];
    if ((mask & QS_PAINT) && w->handle && w->tid == t->id && w->redraw &&
        (w->update.n || w->internal_paint) && !IsIconic(w->handle) &&
        u32_paintable(w) && filter(w->handle, WM_PAINT, h, lo, hi)) {
      /* Parents paint before their children (f5-10C classic painting). */
      for (struct u32_window *p = u32_window(w->parent); p;
           p = u32_window(p->parent))
        if (p->tid == t->id && p->redraw && (p->update.n || p->internal_paint) &&
            filter(p->handle, WM_PAINT, h, lo, hi))
          w = p;
      *out = (MSG){w->handle, WM_PAINT, 0, 0, u32_ticks(), u32_pointer};
      if (!w->update.n && (flags & 1))
        w->internal_paint = 0;
      goto found;
    }
  }
  for (UINT i = 0; i < U32_TIMERS; i++) {
    struct u32_timer *timer = &u32_timers[i];
    if ((mask & QS_TIMER) && timer->tid == t->id &&
        (int32_t)(u32_ticks() - timer->due) >= 0 &&
        filter(timer->hwnd, timer->system ? 0x118u : WM_TIMER, h, lo, hi)) {
      *out = (MSG){timer->hwnd,
                   timer->system ? 0x118u : WM_TIMER, /* WM_SYSTIMER */
                   timer->id,
                   (LPARAM)timer->proc,
                   u32_ticks(),
                   u32_pointer};
      if (flags & PM_REMOVE)
        timer->due = u32_ticks() + timer->period;
      goto found;
    }
  }
  if (!(flags & 2u /* PM_NOYIELD */)) {
    struct ciuki_w32_message_args idle = {.action = W32_MSG_INPUT_IDLE, .tid = t->id};
    u32_message(&idle);
  }
  return 0;
found:
  t->last = *out;
  if ((flags & PM_REMOVE) &&
      (out->message == WM_KEYDOWN || out->message == WM_SYSKEYDOWN ||
       out->message == WM_KEYUP || out->message == WM_SYSKEYUP)) {
    UINT vk = out->wParam & 255;
    BOOL down = out->message == WM_KEYDOWN || out->message == WM_SYSKEYDOWN;
    UINT code=((DWORD)out->lParam>>16)&255;
    if ((DWORD)out->lParam&(1u<<24)) code|=0x100;
    u32_key_state(t->keys,vk,code,down,!!((DWORD)out->lParam&(1u<<30)));

  }
  u32_hooks(3, 0, flags & 1, (LPARAM)out);
  u32_mark();
  return 1;
}
BOOL W32API GetMessageA(MSG *out, HWND h, UINT lo, UINT hi) {
  U32_GUARD;
  if (!out || lo > hi || (h && h != (HWND)(intptr_t)-1 && !IsWindow(h))) {
    u32_fail(1400);
    return -1;
  }
  struct ciuki_w32_system_args idle = {.action = W32_S_LAUNCH_IDLE};
  idle.header = (struct ciuki_w32_header){1, sizeof(idle), W32_SYSTEM, 0};
  u32_backend.call(&idle, sizeof(idle));
  while (!PeekMessageA(out, h, lo, hi, 1)) {
    DWORD r = MsgWaitForMultipleObjectsEx(0, 0, W32_INFINITE, QS_ALLINPUT,
                                          MWMO_INPUTAVAILABLE);
    if (r == W32_WAIT_FAILED)
      return -1;
  }
  return out->message != WM_QUIT;
}
BOOL W32API TranslateMessage(const MSG *m) {
  U32_GUARD;
  if (!m)
    return 0;
  if (m->message != WM_KEYDOWN && m->message != WM_SYSKEYDOWN)
    return m->message == WM_KEYUP || m->message == WM_SYSKEYUP;
  u32_pump();
  struct u32_thread *t = u32_thread();
  if (!t)
    return 0;
  BOOL translated = 0;
  for (UINT i = 0; i < t->text_count;) {
    MSG text = t->text[i];
    if (text.hwnd == m->hwnd && text.time == m->time) {
      u32_queue(t->id, m->hwnd, text.message, text.wParam, text.lParam,
                QS_POSTMESSAGE, u32_ticks(), text.pt);
      for (UINT j = i + 1; j < t->text_count; j++)
        t->text[j - 1] = t->text[j];
      t->text_count--;
      translated = 1;
    } else
      i++;
  }
  if (!translated) {
    t->translate_window = m->hwnd;
    t->translate_time = m->time;
  }
  return 1;
}
LRESULT W32API DispatchMessageA(const MSG *m) {
  U32_GUARD;
  if (!m)
    return 0;
  if ((m->message == WM_TIMER || m->message == 0x118u) && m->lParam) {
    UINT saved = u32_suspend();
    ((TIMERPROC)m->lParam)(m->hwnd, m->message, m->wParam, u32_ticks());
    u32_resume(saved);
    return 0;
  }
  if (!m->hwnd || m->message == WM_QUIT)
    return 0;
  LRESULT r = u32_invoke(m->hwnd, m->message, m->wParam, m->lParam);
  if (m->message == WM_CLOSE ||
      (m->message == WM_SYSCOMMAND && (m->wParam & 0xfff0u) == SC_CLOSE)) {
    /* G3: the desktop's close was handled and the window lives on (the
     * program's "Save changes?" was cancelled, or it ignored SC_CLOSE):
     * decline, so the desktop ends its "Closing..." state. A destroyed
     * window sent CLOSE instead. */
    struct u32_window *w = u32_window(m->hwnd);
    if (w && w->desktop_close && !w->destroying) {
      static const uint8_t declined[4];
      w->desktop_close = 0;
      u32_wire(w, DESK_CLOSE_ACK, declined, sizeof(declined), -1);
    }
  }
  u32_flush();
  return r;
}
DWORD W32API MsgWaitForMultipleObjectsEx(DWORD n, const HANDLE *handles,
                                         DWORD ms, DWORD mask, DWORD flags) {
  U32_GUARD;
  if (n > 63 || (n && !handles) || (mask & ~QS_ALLINPUT) || (flags & ~7u)) {
    u32_fail(87);
    return W32_WAIT_FAILED;
  }
  DWORD values[63];
  for (UINT i = 0; i < n; i++)
    values[i] = U32_PTR(handles[i]);
  DWORD start = u32_ticks();
  for (;;) {
    u32_pump();
    u32_mark();
    DWORD elapsed = u32_ticks() - start;
    DWORD slice = ms == W32_INFINITE ? W32_INFINITE : elapsed >= ms ? 0 : ms - elapsed;
    for (UINT i = 0; i < U32_TIMERS; i++)
      if ((mask & QS_TIMER) && u32_timers[i].tid == u32_tid()) {
        int32_t left = (int32_t)(u32_timers[i].due - u32_ticks());
        if (left > 0 && (DWORD)left < slice)
          slice = left;
      }
    struct ciuki_w32_wait_args a = {
        .header = K32_HEADER(W32_WAIT, a),
        .handles = U32_PTR(values),
        .count = n,
        .all = !!(flags & MWMO_WAITALL),
        .alertable = !!(flags & MWMO_ALERTABLE),
        .timeout = slice,
        .wake_mask = mask,
        .reserved = (flags & MWMO_INPUTAVAILABLE ? 0 : W32_WAIT_NEW_INPUT) |
                    (u32_desktop.disconnected ? W32_WAIT_SESSION : W32_WAIT_SERVICES)};
    UINT saved = u32_suspend();
    int32_t status = u32_backend.call(&a, sizeof(a));
    u32_resume(saved);
    if (status || a.error) {
      u32_fail(a.error ? a.error : 87);
      return W32_WAIT_FAILED;
    }
    if (a.result == W32_WAIT_SERVICE)
      continue;
    if (a.result != W32_WAIT_TIMEOUT)
      return a.result;
    if (ms != W32_INFINITE && (DWORD)(u32_ticks() - start) >= ms)
      return W32_WAIT_TIMEOUT;
  }
}
DWORD W32API MsgWaitForMultipleObjects(DWORD n, const HANDLE *h, BOOL all,
                                       DWORD ms, DWORD mask) {
  U32_GUARD;
  return MsgWaitForMultipleObjectsEx(n, h, ms, mask, all ? MWMO_WAITALL : 0);
}
DWORD W32API GetQueueStatus(UINT mask) {
  U32_GUARD;
  if (!mask)
    return 0;
  u32_pump();
  u32_mark();
  struct ciuki_w32_message_args a = {.action = W32_MSG_STATUS, .bits = mask};
  if (mask & ~QS_ALLINPUT || !u32_message(&a))
    return 0;
  return ((a.result & mask) << 16) | (a.message.wparam & mask);
}
BOOL W32API WaitMessage(void) {
  U32_GUARD;
  return MsgWaitForMultipleObjectsEx(0, 0, W32_INFINITE, QS_ALLINPUT, 0) !=
         W32_WAIT_FAILED;
}
LONG W32API GetMessageTime(void) {
  U32_GUARD;
  struct u32_thread *t = u32_thread();
  return t ? (LONG)t->last.time : 0;
}
DWORD W32API GetMessagePos(void) {
  U32_GUARD;
  struct u32_thread *t = u32_thread();
  return t ? (uint16_t)t->last.pt.x | ((uint32_t)(uint16_t)t->last.pt.y << 16)
           : 0;
}
/* f5-20B: application timers and system timers (SetSystemTimer, delivered
 * as WM_SYSTIMER) live in one table with separate identifier spaces. */
uintptr_t u32_set_timer(HWND h, uintptr_t id, UINT ms, TIMERPROC proc,
                        BOOL system) {
  if (h && GetWindowThreadProcessId(h, 0) != u32_tid())
    return u32_fail(5);
  static uintptr_t next = 1;
  struct u32_timer *free_timer = 0;
  for (UINT i = 0; i < U32_TIMERS; i++) {
    struct u32_timer *t = &u32_timers[i];
    if (!t->tid && !free_timer)
      free_timer = t;
    if (t->tid == u32_tid() && t->hwnd == h && id && t->id == id &&
        !t->system == !system) {
      free_timer = t;
      break;
    }
  }
  if (!free_timer)
    return u32_fail(8);
  if (!h && !id)
    id = next++;
  if (!id && !h)
    return u32_fail(8);
  if (ms < 10)
    ms = 10;
  if (ms > 0x7fffffff)
    ms = 0x7fffffff;
  *free_timer = (struct u32_timer){u32_tid(), h,    id, ms, u32_ticks() + ms,
                                   proc,      !!system};
  u32_mark();
  return id;
}
BOOL u32_kill_timer(HWND h, uintptr_t id, BOOL system) {
  for (UINT i = 0; i < U32_TIMERS; i++) {
    struct u32_timer *t = &u32_timers[i];
    if (t->tid == u32_tid() && t->hwnd == h && t->id == id &&
        !t->system == !system) {
      w32_fill(t, 0, sizeof(*t));
      u32_mark();
      return 1;
    }
  }
  return 0;
}
uintptr_t W32API SetTimer(HWND h, uintptr_t id, UINT ms, TIMERPROC proc) {
  U32_GUARD;
  return u32_set_timer(h, id, ms, proc, 0);
}
BOOL W32API KillTimer(HWND h, uintptr_t id) {
  U32_GUARD;
  return u32_kill_timer(h, id, 0);
}
UINT W32API RegisterWindowMessageA(const char *name) {
  U32_GUARD;
  if (!name || w32_strlen(name) > 255)
    return u32_fail(87);
  struct w32r_reply r;
  return u32_directory(W32R_ATOM_ADD, 0, 0, 0, 0, name, 0, 0, &r, 0, 0) ? r.key
                                                                        : 0;
}
