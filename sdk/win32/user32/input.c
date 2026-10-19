/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../../../apps/desktop/keymap.h"
#include "internal.h"
static uint8_t async_keys[256];
UINT u32_pointer_changing;
static UINT double_time = 500;
static HWND click_window;
static UINT click_button;
static DWORD click_time;
static POINT click_pos;
static RECT clip;
static BOOL clipped;
static const uint8_t scan_vk[0x59] = {
    [1] = 27,    [2] = '1',   [3] = '2',   [4] = '3',   [5] = '4',
    [6] = '5',   [7] = '6',   [8] = '7',   [9] = '8',   [10] = '9',
    [11] = '0',  [12] = 0xbd, [13] = 0xbb, [14] = 8,    [15] = 9,
    [16] = 'Q',  [17] = 'W',  [18] = 'E',  [19] = 'R',  [20] = 'T',
    [21] = 'Y',  [22] = 'U',  [23] = 'I',  [24] = 'O',  [25] = 'P',
    [26] = 0xdb, [27] = 0xdd, [28] = 13,   [29] = 0x11, [30] = 'A',
    [31] = 'S',  [32] = 'D',  [33] = 'F',  [34] = 'G',  [35] = 'H',
    [36] = 'J',  [37] = 'K',  [38] = 'L',  [39] = 0xba, [40] = 0xde,
    [41] = 0xc0, [42] = 0x10, [43] = 0xdc, [44] = 'Z',  [45] = 'X',
    [46] = 'C',  [47] = 'V',  [48] = 'B',  [49] = 'N',  [50] = 'M',
    [51] = 0xbc, [52] = 0xbe, [53] = 0xbf, [54] = 0x10, [55] = 0x6a,
    [56] = 0x12, [57] = 32,   [58] = 20,   [59] = 0x70, [60] = 0x71,
    [61] = 0x72, [62] = 0x73, [63] = 0x74, [64] = 0x75, [65] = 0x76,
    [66] = 0x77, [67] = 0x78, [68] = 0x79, [69] = 0x90, [70] = 0x91,
    [71] = 0x67, [72] = 0x68, [73] = 0x69, [74] = 0x6d, [75] = 0x64,
    [76] = 0x65, [77] = 0x66, [78] = 0x6b, [79] = 0x61, [80] = 0x62,
    [81] = 0x63, [82] = 0x60, [83] = 0x6e, [86] = 0xe2, [87] = 0x7a,
    [88] = 0x7b};
static UINT vk(UINT code) {
  if (code == 0x200)
    return 0x13;
  if (code & 0x100) {
    switch (code & 255) {
    case 0x1c:
      return 13;
    case 0x1d:
      return 0x11;
    case 0x35:
      return 0x6f;
    case 0x37:
      return 0x2c;
    case 0x38:
      return 0x12;
    case 0x47:
      return 0x24;
    case 0x48:
      return 0x26;
    case 0x49:
      return 0x21;
    case 0x4b:
      return 0x25;
    case 0x4d:
      return 0x27;
    case 0x4f:
      return 0x23;
    case 0x50:
      return 0x28;
    case 0x51:
      return 0x22;
    case 0x52:
      return 0x2d;
    case 0x53:
      return 0x2e;
    case 0x5b:
      return 0x5b;
    case 0x5c:
      return 0x5c;
    case 0x5d:
      return 0x5d;
    default:
      return 0;
    }
  }
  if (u32_layout) {
    switch (code) {
    case 0x0c:
      return 0xdb;
    case 0x0d:
      return 0xdd;
    case 0x1a:
      return 0xba;
    case 0x1b:
      return 0xbb;
    case 0x27:
      return 0xc0;
    case 0x28:
      return 0xde;
    case 0x29:
      return 0xbf;
    case 0x2b:
      return 0xdc;
    case 0x35:
      return 0xbd;
    default:
      break;
    }
  }
  return code < sizeof(scan_vk) ? scan_vk[code] : 0;
}
uintptr_t W32API GetKeyboardLayout(DWORD tid) {
  U32_GUARD;
  u32_settings();
  (void)tid;
  return u32_layout ? 0x04100410 : 0x04090409;
}
UINT W32API MapVirtualKeyA(UINT code, UINT type) {
  U32_GUARD;
  u32_settings();
  if (type == 3) {
    UINT key = vk(code);
    return key == 16   ? (code == 0x36 ? 161 : 160)
           : key == 17 ? (code & 0x100 ? 163 : 162)
           : key == 18 ? (code & 0x100 ? 165 : 164)
                       : key;
  }
  if (type == 1)
    return vk(code);
  if (type == 0) {
    for (UINT s = 1; s < 0x59; s++)
      if (vk(s) == code)
        return s;
    for (UINT s = 0x100; s < 0x160; s++)
      if (vk(s) == code)
        return s & 255;
    return 0;
  }
  if (type == 2) {
    UINT scan = MapVirtualKeyA(code, 0);
    unsigned c = ciuki_keymap_char(u32_layout, scan, 0);
    if (c >= 'a' && c <= 'z')
      c -= 32;
    return c;
  }
  return 0;
}
int16_t W32API VkKeyScanA(char c) {
  U32_GUARD;
  unsigned cp = u32_scalar((uint8_t)c);
  for (UINT mods = 0; mods < 8; mods++) {
    unsigned flags = (mods & 1 ? CIUKI_KEYMAP_SHIFT : 0) |
                     ((mods & 6) == 6 ? CIUKI_KEYMAP_ALTGR : 0);
    if ((mods & 6) && ((mods & 6) != 6))
      continue;
    for (UINT s = 1; s <= 0x56; s++)
      if (ciuki_keymap_char(u32_layout, s, flags) == cp)
        return vk(s) | (mods << 8);
  }
  return -1;
}
int W32API ToAscii(UINT v, UINT scan, const uint8_t *keys, uint16_t *out,
                   UINT flags) {
  U32_GUARD;
  (void)flags;
  if (!keys || !out)
    return 0;
  if (!scan)
    scan = MapVirtualKeyA(v, 0);
  unsigned mods = (keys[16] & 128 ? CIUKI_KEYMAP_SHIFT : 0) |
                  (keys[20] & 1 ? CIUKI_KEYMAP_CAPS : 0) |
                  ((keys[0xa5] & 128) || ((keys[17] & 128) && (keys[18] & 128))
                       ? CIUKI_KEYMAP_ALTGR
                       : 0);
  unsigned c = ciuki_keymap_char(u32_layout, scan & 255, mods);
  if (!c)
    return 0;
  *out = u32_ansi(c);
  return 1;
}
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
  struct u32_thread *t = u32_thread();
  if (t) {
    w32_fill(t->keys, 0, 256);
    t->text_count = 0;
    t->key_window = t->translate_window = 0;
    t->key_lparam = 0;
  }
  if (u32_capture)
    ReleaseCapture();
  clipped = 0;
  click_window = 0;
}
UINT W32API GetDoubleClickTime(void) {
  U32_GUARD;
  return double_time;
}
BOOL W32API SetDoubleClickTime(UINT t) {
  U32_GUARD;
  double_time = t ? (t > 5000 ? 5000 : t) : 500;
  return 1;
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
  if (e->type == CIUKI_INPUT_KEY) {
    UINT key = vk(e->code);
    if (!key)
      return;
    BOOL down = !!e->value,
         previous = e->value == 2 || !!(async_keys[key] & 128),
         alt = !!(async_keys[18] & 128);
    m = alt || key == 18 ? (down ? WM_SYSKEYDOWN : WM_SYSKEYUP)
                         : (down ? WM_KEYDOWN : WM_KEYUP);
    lp = 1 | (((UINT)e->code & 255) << 16) |
         ((e->code & 0x100) ? 1u << 24 : 0) | (alt ? 1u << 29 : 0) |
         (previous ? 1u << 30 : 0) | (!down ? (1u << 30) | (1u << 31) : 0);
    wp = key;
    async_keys[key] =
        (down ? 128 : 0) | (down && !previous ? 1 : async_keys[key] & 1);
    if (u32_hooks(2, 0, key, lp))
      return;
    if (down) {
      t->key_window = h;
      t->key_time = time ? time : u32_ticks();
      t->key_lparam = (DWORD)lp;
    }
  } else if (e->type == CIUKI_INPUT_TEXT) {
    m = async_keys[18] & 128 ? WM_SYSCHAR : WM_CHAR;
    wp = u32_ansi(e->code);
    lp = t->key_lparam ? t->key_lparam : 1;
    HWND target = t->key_window ? t->key_window : h;
    if (!t->key_window ||
        (t->translate_window == target && t->translate_time == t->key_time)) {
      u32_queue(GetWindowThreadProcessId(target, 0), target, m, wp, lp,
                QS_POSTMESSAGE, time, u32_pointer);
      t->translate_window = 0;
    } else if (t->text_count < 64)
      t->text[t->text_count++] =
          (MSG){target, m, wp, lp, t->key_time, u32_pointer};
    else
      u32_fail(1816);
    return;
  } else {
    POINT p = u32_pointer;
    if (e->type == CIUKI_INPUT_MOTION) {
      p = (POINT){e->value, e->value2};
      ClientToScreen(root->handle, &p);
      u32_pointer = p;
    }
    h = u32_capture ? u32_capture : WindowFromPoint(p);
    if (!h)
      h = root->handle;
    POINT client = p;
    ScreenToClient(h, &client);
    lp = (uint16_t)client.x | ((uint32_t)(uint16_t)client.y << 16);
    mask = e->type == CIUKI_INPUT_MOTION ? QS_MOUSEMOVE : QS_MOUSEBUTTON;
    wp = (async_keys[1] & 128 ? 1 : 0) | (async_keys[2] & 128 ? 2 : 0) |
         (async_keys[16] & 128 ? 4 : 0) | (async_keys[17] & 128 ? 8 : 0) |
         (async_keys[4] & 128 ? 16 : 0);
    if (e->type == CIUKI_INPUT_MOTION)
      m = WM_MOUSEMOVE;
    else if (e->type == CIUKI_INPUT_WHEEL) {
      m = WM_MOUSEWHEEL;
      wp |= (uint32_t)(uint16_t)(e->value * 120) << 16;
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
            (DWORD)(time - click_time) <= double_time && dx >= -2 && dx <= 2 &&
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
    struct {
      POINT pt;
      HWND hwnd;
      UINT hit;
      uintptr_t extra;
    } mouse = {p, h, 1, 0};
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
  struct u32_window *w = u32_root(u32_window(h));
  if (!w)
    return 0;
  uint8_t b[20];
  desk_put32(b, (capture ? DESK_V3_CAPTURE : 0) | (clipped ? DESK_V3_CLIP : 0));
  POINT origin = u32_origin(w);
  desk_put32(b + 4, clipped ? clip.left - origin.x : 0);
  desk_put32(b + 8, clipped ? clip.top - origin.y : 0);
  desk_put32(b + 12, clipped ? clip.right - clip.left : 0);
  desk_put32(b + 16, clipped ? clip.bottom - clip.top : 0);
  u32_pointer_changing++;
  BOOL ok = u32_wire(w, DESK_V3_POINTER, b, 20, -1);
  u32_pointer_changing--;
  return ok;
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
  *r = clipped ? clip : (RECT){0, 0, u32_screen_w, u32_screen_h};
  return 1;
}
BOOL W32API SetCursorPos(int x, int y) {
  U32_GUARD;
  struct u32_window *w = u32_window(u32_foreground);
  if (!w)
    return u32_fail(5);
  RECT bounds = clipped ? clip : (RECT){0, 0, u32_screen_w, u32_screen_h};
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
