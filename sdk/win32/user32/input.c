/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../../../apps/desktop/input_text.h"
#include "internal.h"
static uint8_t async_keys[256], lock_keys[256], key_vks[0x201];
UINT u32_pointer_changing;

static HWND click_window;
static UINT click_button;
static DWORD click_time;
static POINT click_pos;
static RECT clip;
static BOOL clipped;
HWND u32_input_exclusive;
static HWND relative_window;
static BOOL pointer_request(HWND, BOOL);
int16_t W32API GetKeyState(int key) {
  U32_GUARD;
  struct u32_thread *t = u32_thread();
  if (!t || key < 0 || key > 255)
    return 0;
  return (t->keys[key] & 128 ? 0x8000 : 0) | (t->keys[key] & 1);
}
int16_t W32API GetAsyncKeyState(int key) {
  U32_GUARD;
  if (key < 0 || key > 255)
    return 0;
  uint8_t v = async_keys[key];
  async_keys[key] &= 128;
  return (v & 128 ? 0x8000 : 0) | (v & 1);
}
BOOL W32API GetKeyboardState(uint8_t *keys) {
  U32_GUARD;
  struct u32_thread *t = u32_thread();
  if (!t || !keys)
    return u32_fail(87);
  w32_copy(keys, t->keys, 256);
  return 1;
}
BOOL W32API SetKeyboardState(const uint8_t *keys) {
  U32_GUARD;
  struct u32_thread *t = u32_thread();
  if (!t || !keys)
    return u32_fail(87);
  w32_copy(t->keys, keys, 256);
  return 1;
}
void u32_clear_input(void) {
  w32_fill(async_keys, 0, 256);
  w32_fill(lock_keys, 0, 256);
  w32_fill(key_vks, 0, sizeof(key_vks));
  HWND captured = u32_capture;
  BOOL had_clip = clipped;
  u32_capture = 0;
  clipped = 0;
  u32_clear_thread_input();
  u32_input_reset();
  if (captured || had_clip)
    pointer_request(u32_foreground ? u32_foreground : captured, 0);
  if (captured)
    SendMessageA(captured, WM_CAPTURECHANGED, 0, 0);
  click_window = 0;
}
UINT W32API GetDoubleClickTime(void) {
  U32_GUARD;
  u32_input_refresh();return u32_input_value(DESK_INPUT_DOUBLE_CLICK);
}
BOOL W32API SetDoubleClickTime(UINT t) {
  U32_GUARD;
  return u32_input_setting(DESK_INPUT_DOUBLE_CLICK,t ? (t>5000 ? 5000 : t) : 500,0);
}
void u32_input(struct u32_window *root, const struct ciuki_input_event *e) {
  u32_settings();
  struct u32_thread *t = u32_thread();
  if (!root || !t)
    return;
  HWND h = t->focus ? t->focus : root->handle;
  DWORD time = (DWORD)(e->monotonic_ns / 1000000);
  UINT m = 0, mask = QS_KEY;
  WPARAM wp = 0;
  LPARAM lp = 0;
  if (e->type == CIUKI_INPUT_RESYNC) {
    u32_clear_input();
    return;
  }
  if (e->type != CIUKI_INPUT_MOTION)
    u32_input_notify(root->handle, e, 0);
  if (e->type == CIUKI_INPUT_KEY) {
    UINT key = u32_scan_vk(u32_layout, e->code);
    if (e->code >= 0x47 && e->code <= 0x53 && key >= 0x60 && key <= 0x6e &&
        key != 0x6b && key != 0x6d &&
        (!(lock_keys[144] & 1) != !!(async_keys[16] & 128))) {
      static const uint8_t nav[] = {36, 38, 33, 0,  37, 12, 39,
                                    0,  35, 40, 34, 45, 46};
      key = nav[e->code - 0x47];
    }
    if (e->code >= 0 && e->code <= 0x200) {
      if (key_vks[e->code])
        key = key_vks[e->code];
      if (e->value)
        key_vks[e->code] = key;
      else
        key_vks[e->code] = 0;
    }
    if (!key)
      return;
    UINT physical = key;
    if (key >= 16 && key <= 18)
      physical = (key == 16   ? 160
                  : key == 17 ? 162
                              : 164) +
                 (key == 16 ? e->code == 0x36 : !!(e->code & 0x100));
    BOOL down = !!e->value,
         previous = e->value == 2 || !!(async_keys[physical] & 128),
         alt = !!(async_keys[18] & 128);
    m = alt || key == 18 || key == 0x79 ? (down ? WM_SYSKEYDOWN : WM_SYSKEYUP)
                                        : (down ? WM_KEYDOWN : WM_KEYUP);
    lp = u32_key_lparam(e->code, down, previous, alt);
    wp = key;
    u32_key_state(lock_keys, key, e->code, down, e->value == 2);
    if (key >= 16 && key <= 18) {
      UINT left = key == 16 ? 160 : key == 17 ? 162 : 164;
      UINT side = left + (key == 16 ? e->code == 0x36 : !!(e->code & 0x100));
      async_keys[side] =
          (down ? 128 : 0) | (down && !previous ? 1 : async_keys[side] & 1);
      async_keys[key] = ((async_keys[left] | async_keys[left + 1]) & 128) |
                        (down && !previous ? 1 : async_keys[key] & 1);
    } else
      async_keys[key] =
          (down ? 128 : 0) | (down && !previous ? 1 : async_keys[key] & 1);
    if (key == 0x2c && down)
      return; /* Windows delivers Snapshot on release. */
    if (u32_hooks(2, 0, key, lp))
      return;
    if (down) {
      t->key_scan=(UINT)e->code;
      t->key_window = h;
      t->key_time = time ? time : u32_ticks();
      t->key_lparam = (DWORD)lp;
    }
  } else if (e->type == CIUKI_INPUT_TEXT) {
    BOOL system=(async_keys[18]&128) && !(async_keys[165]&128);
    m=system ? WM_SYSCHAR : WM_CHAR;
    unsigned chars[2];
    unsigned count=ciuki_input_compose(&t->dead_accent,(unsigned)e->code,
        u32_layout==1 && t->key_scan==0x0d && (async_keys[16]&128),chars);
    if(!count) {chars[0]=(unsigned)e->code;count=1;m=system ? 0x107 : 0x103;}
    lp = t->key_lparam ? t->key_lparam : 1;
    HWND target = t->key_window ? t->key_window : h;
    BOOL immediate=!t->key_window || (t->translate_window==target && t->translate_time==t->key_time);
    for(unsigned k=0;k<count;k++) {
      wp=u32_ansi(chars[k]);
      if(immediate)u32_queue(GetWindowThreadProcessId(target,0),target,m,wp,lp,QS_POSTMESSAGE,time,u32_pointer);
      else if(t->text_count<64)t->text[t->text_count++]=(MSG){target,m,wp,lp,t->key_time,u32_pointer};
      else u32_fail(1816);
    }
    if(immediate)t->translate_window=0;
    return;
  } else {
    POINT p = u32_pointer;
    if (e->type == CIUKI_INPUT_MOTION) {
      POINT so = u32_surface_origin(root);
      p = (POINT){e->value + so.x, e->value2 + so.y};
      u32_pointer = p;
      u32_input_notify(root->handle, e, 0);
    }
    h = u32_capture ? u32_capture : WindowFromPoint(p);
    if (!h)
      h = root->handle;
    POINT client = p;
    ScreenToClient(h, &client);
    lp = (uint16_t)client.x | ((uint32_t)(uint16_t)client.y << 16);
    /* f5-10C: points over a personality-owned band (menu bar, border,
     * scroll bar) become WM_NC* messages with the hit-test code. */
    UINT hit = u32_capture ? 1 : u32_hit_test(h, p);
    mask = e->type == CIUKI_INPUT_MOTION ? QS_MOUSEMOVE : QS_MOUSEBUTTON;
    wp = (async_keys[1] & 128 ? 1 : 0) | (async_keys[2] & 128 ? 2 : 0) |
         (async_keys[16] & 128 ? 4 : 0) | (async_keys[17] & 128 ? 8 : 0) |
         (async_keys[4] & 128 ? 16 : 0);
    if (e->type == CIUKI_INPUT_MOTION)
      m = WM_MOUSEMOVE;
    else if (e->type == CIUKI_INPUT_WHEEL) {
      m = WM_MOUSEWHEEL;
      int64_t delta = (int64_t)e->value * 120;
      if (delta > 32760)
        delta = 32760;
      if (delta < -32760)
        delta = -32760;
      wp |= (uint32_t)(uint16_t)delta << 16;
      lp = (uint16_t)p.x | ((uint32_t)(uint16_t)p.y << 16);
    } else if (e->type == CIUKI_INPUT_BUTTON) {
      UINT button = e->code == 1   ? 1
                    : e->code == 2 ? 2
                                   : 4,
           flag = button == 1   ? 1
                  : button == 2 ? 2
                                : 16;
      async_keys[button] = e->value ? 129 : 0;
      if (e->value)
        wp |= flag;
      else
        wp &= ~flag;
      m = button == 1   ? (e->value ? WM_LBUTTONDOWN : WM_LBUTTONUP)
          : button == 2 ? (e->value ? WM_RBUTTONDOWN : WM_RBUTTONUP)
                        : (e->value ? WM_MBUTTONDOWN : WM_MBUTTONUP);
      if (e->value) {
        struct u32_window *w = u32_window(h);
        int dx = p.x - click_pos.x, dy = p.y - click_pos.y;
        if (w && (w->cls->c.style & 8) && h == click_window &&
            click_button == button &&
            (DWORD)(time - click_time) <= GetDoubleClickTime() && dx >= -2 && dx <= 2 &&
            dy >= -2 && dy <= 2) {
          m++;
          m++;
          click_window = 0;
        } else {
          click_window = h;
          click_button = button;
          click_time = time;
          click_pos = p;
        }
      }
    }
    if (hit > 1 && m >= WM_MOUSEMOVE && m <= 0x209u) {
      m = m - WM_MOUSEMOVE + 0xa0u;
      wp = hit;
      lp = (uint16_t)p.x | ((uint32_t)(uint16_t)p.y << 16);
    }
    struct {
      POINT pt;
      HWND hwnd;
      UINT hit;
      uintptr_t extra;
    } mouse = {p, h, hit ? hit : 1, 0};
    if (u32_hooks(7, 0, m, (LPARAM)&mouse))
      return;
  }
  if (m) {
    DWORD tid = GetWindowThreadProcessId(h, 0);
    u32_queue(tid, h, m, wp, lp, mask, time, u32_pointer);
  }
}
HWND W32API GetFocus(void) {
  U32_GUARD;
  struct u32_thread *t = u32_thread();
  return t ? t->focus : 0;
}
HWND W32API GetActiveWindow(void) {
  U32_GUARD;
  struct u32_thread *t = u32_thread();
  return t ? t->active : 0;
}
HWND W32API GetForegroundWindow(void) {
  U32_GUARD;
  return u32_foreground;
}
HWND W32API SetFocus(HWND h) {
  U32_GUARD;
  struct u32_thread *t = u32_thread();
  if (!t)
    return 0;
  if (h && (!IsWindowEnabled(h) || GetWindowThreadProcessId(h, 0) != t->id)) {
    u32_fail(87);
    return 0;
  }
  HWND old = t->focus;
  if (old == h)
    return old;
  t->dead_accent=0;
  t->focus = h;
  if (old)
    SendMessageA(old, WM_KILLFOCUS, (WPARAM)h, 0);
  if (t->focus == h && h)
    SendMessageA(h, WM_SETFOCUS, (WPARAM)old, 0);
  return old;
}
HWND W32API SetActiveWindow(HWND h) {
  U32_GUARD;
  struct u32_thread *t = u32_thread();
  struct u32_window *w = u32_window(h);
  if (!t)
    return 0;
  if (h && (!w || w->tid != t->id || w->parent)) {
    u32_fail(87);
    return 0;
  }
  HWND old = t->active;
  if (!h && old) {
    struct u32_window *previous = u32_window(old);
    if (previous) {
      uint8_t off[8] = {0};
      if (!u32_wire(previous, DESK_V3_ACTIVATE, off, 8, -1))
        return 0;
    }
    if (t->active == old) {
      t->active = 0;
      SendMessageA(old, WM_NCACTIVATE, 0, 0);
      SendMessageA(old, WM_ACTIVATE, 0, 0);
    }
    if (u32_foreground == old)
      u32_foreground = 0;
  }
  if (h && w) {
    uint8_t b[8];
    desk_put32(b, 1);
    desk_put32(b + 4, 1);
    if (!u32_wire(w, DESK_V3_ACTIVATE, b, 8, -1))
      return 0;
  }
  return old;
}
BOOL W32API SetForegroundWindow(HWND h) {
  U32_GUARD;
  struct u32_window *w = u32_root(u32_window(h));
  if (!w)
    return u32_fail(1400);
  uint8_t b[8];
  desk_put32(b, 1);
  desk_put32(b + 4, 1);
  return u32_wire(w, DESK_V3_ACTIVATE, b, 8, -1);
}
void u32_activate(struct u32_window *w, BOOL focused, BOOL active) {
  HWND h = w->handle;
  struct u32_thread *t = u32_thread();
  if (!t)
    return;
  HWND old = t->active;
  BOOL was_app = !!u32_foreground;
  if (focused) {
    t->active = h;
    u32_foreground = h;
  } else {
    if (t->active == h)
      t->active = 0;
    if (!active && u32_foreground == h)
      u32_foreground = 0;
  }
  u32_input_focus();
  if (old != t->active) {
    if (old)
      SendMessageA(old, WM_NCACTIVATE, 0, (LPARAM)h),
          SendMessageA(old, WM_ACTIVATE, 0, (LPARAM)h);
    if (focused)
      SendMessageA(h, WM_NCACTIVATE, 1, (LPARAM)old),
          SendMessageA(h, WM_ACTIVATE, 1, (LPARAM)old);
  }
  if (was_app != active)
    for (UINT i = 0; i < U32_WINDOWS; i++)
      if (u32_windows[i].handle && !u32_windows[i].parent)
        SendNotifyMessageA(u32_windows[i].handle, WM_ACTIVATEAPP, active,
                           w->tid);
  if (focused) {
    /* WM_QUERYNEWPALETTE: the window is about to receive the keyboard focus
     * and may realise its logical palette in the foreground. */
    if (old != h)
      SendMessageA(h, WM_QUERYNEWPALETTE, 0, 0);
    if (!t->focus || u32_root(u32_window(t->focus)) != w)
      SetFocus(h);
  } else if (t->focus && u32_root(u32_window(t->focus)) == w) {
    SetFocus(0);
    u32_clear_input();
  }
}
HWND W32API GetCapture(void) {
  U32_GUARD;
  return u32_capture;
}
static BOOL pointer_request(HWND h, BOOL capture) {
  if (u32_input_exclusive)
    h = u32_input_exclusive;
  else if (!h)
    h = relative_window;
  struct u32_window *w = u32_root(u32_window(h));
  if (!w)
    return h ? 0 : 1;
  uint8_t b[20];
  if (u32_input_exclusive) {
    RECT client;
    GetClientRect(h, &client);
    desk_put32(b, DESK_V3_CAPTURE | DESK_V3_CLIP | DESK_V3_RELATIVE |
                      DESK_V3_HIDDEN);
    desk_put32(b + 4, 0);
    desk_put32(b + 8, 0);
    desk_put32(b + 12, client.right);
    desk_put32(b + 16, client.bottom);
    u32_pointer_changing++;
    BOOL ok = u32_wire(w, DESK_V3_POINTER, b, 20, -1);
    u32_pointer_changing--;
    return ok;
  }
  /* V3 requires capture or clip with RELATIVE. A nonexclusive reader uses
   * the ordinary screen bounds when no ordinary capture/clip exists. */
  BOOL screen_clip = relative_window && !capture && !clipped;
  BOOL use_clip = clipped || screen_clip;
  RECT bounds = clipped ? clip : (RECT){0, 0, u32_screen_w, u32_screen_h};
  desk_put32(b, (capture ? DESK_V3_CAPTURE : 0) |
                    (use_clip ? DESK_V3_CLIP : 0) |
                    (relative_window ? DESK_V3_RELATIVE : 0));
  POINT origin = u32_surface_origin(w);
  desk_put32(b + 4, use_clip ? bounds.left - origin.x : 0);
  desk_put32(b + 8, use_clip ? bounds.top - origin.y : 0);
  desk_put32(b + 12, use_clip ? bounds.right - bounds.left : 0);
  desk_put32(b + 16, use_clip ? bounds.bottom - bounds.top : 0);
  u32_pointer_changing++;
  BOOL ok = u32_wire(w, DESK_V3_POINTER, b, 20, -1);
  u32_pointer_changing--;
  return ok;
}
BOOL W32API CiukiUserInputPointer(HWND hwnd, UINT flags) {
  U32_GUARD;
  if (flags & ~3u || (flags && !u32_window(hwnd)))
    return 0;
  HWND old = relative_window, exclusive = u32_input_exclusive;
  if (!flags && (!u32_foreground || (!old && !exclusive))) {
    relative_window = 0;
    u32_input_exclusive = 0;
    return 1;
  }
  relative_window = flags & 1 ? hwnd : 0;
  u32_input_exclusive = flags & 2 ? hwnd : 0;
  HWND target = u32_input_exclusive ? u32_input_exclusive
                : u32_capture       ? u32_capture
                : u32_foreground    ? u32_foreground
                                    : old;
  if (!pointer_request(target, !!u32_capture)) {
    relative_window = old;
    u32_input_exclusive = exclusive;
    return 0;
  }
  return 1;
}
HWND W32API SetCapture(HWND h) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (!w) {
    u32_fail(1400);
    return 0;
  }
  HWND old = u32_capture;
  if (old == h)
    return old;
  if (!pointer_request(h, 1))
    return 0;
  u32_capture = h;
  if (old && old != h)
    SendMessageA(old, WM_CAPTURECHANGED, 0, (LPARAM)h);
  return old;
}
BOOL W32API ReleaseCapture(void) {
  U32_GUARD;
  HWND old = u32_capture;
  if (old) {
    if (!pointer_request(old, 0))
      return 0;
    u32_capture = 0;
    SendMessageA(old, WM_CAPTURECHANGED, 0, 0);
  }
  return 1;
}
BOOL W32API ClipCursor(const RECT *r) {
  U32_GUARD;
  if (r && IsRectEmpty(r))
    return u32_fail(87);
  RECT old = clip;
  BOOL prev = clipped;
  clipped = !!r;
  if (r)
    clip = *r;
  if (!pointer_request(u32_foreground, !!u32_capture)) {
    clip = old;
    clipped = prev;
    return 0;
  }
  return 1;
}
BOOL W32API GetClipCursor(RECT *r) {
  U32_GUARD;
  if (!r)
    return u32_fail(87);
  if (u32_input_exclusive) {
    GetClientRect(u32_input_exclusive, r);
    POINT origin = {0, 0};
    ClientToScreen(u32_input_exclusive, &origin);
    OffsetRect(r, origin.x, origin.y);
  } else
    *r = clipped ? clip : (RECT){0, 0, u32_screen_w, u32_screen_h};
  return 1;
}
BOOL W32API SetCursorPos(int x, int y) {
  U32_GUARD;
  struct u32_window *w = u32_window(u32_foreground);
  if (!w)
    return u32_fail(5);
  RECT bounds;
  GetClipCursor(&bounds);
  if (x < bounds.left)
    x = bounds.left;
  if (y < bounds.top)
    y = bounds.top;
  if (x >= bounds.right)
    x = bounds.right - 1;
  if (y >= bounds.bottom)
    y = bounds.bottom - 1;
  POINT p = {x, y};
  ScreenToClient(w->handle, &p);
  uint8_t b[8];
  desk_put32(b, p.x);
  desk_put32(b + 4, p.y);
  if (!u32_wire(w, DESK_V3_WARP, b, 8, -1))
    return 0;
  u32_pointer = (POINT){x, y};
  return 1;
}
BOOL W32API GetCursorPos(POINT *p) {
  U32_GUARD;
  if (!p)
    return u32_fail(87);
  *p = u32_pointer;
  return 1;
}
HWND W32API WindowFromPoint(POINT p) {
  U32_GUARD;
  HWND best = 0;
  UINT z = 0;
  for (UINT i = 0; i < U32_WINDOWS; i++) {
    struct u32_window *w = &u32_windows[i];
    RECT r;
    if (w->handle && IsWindowVisible(w->handle) && IsWindowEnabled(w->handle) &&
        GetWindowRect(w->handle, &r) && PtInRect(&r, p)) {
      UINT rank = w->z + (w->exstyle & WS_EX_TOPMOST ? 0x40000000 : 0) +
                  (w->parent ? 0x20000000 : 0);
      if (!best || rank >= z)
        best = w->handle, z = rank;
    }
  }
  return best;
}
