/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"
static DWORD colors[29] = {0xc0c0c0, 0x808000, 0x800000, 0x808080, 0xc0c0c0,
                           0xffffff, 0,        0,        0,        0xffffff,
                           0xc0c0c0, 0xc0c0c0, 0x808080, 0x800000, 0xffffff,
                           0xc0c0c0, 0x808080, 0x808080, 0,        0xc0c0c0,
                           0xffffff, 0x000000, 0xdfdfdf, 0x000000, 0xe1ffff,
                           0x000000, 0x000000, 0xd0d0d0, 0x000000};
static DWORD settings[128];
static uint8_t setting_written[128];
DWORD W32API GetSysColor(int n) {
  U32_GUARD;
  return n >= 0 && n < 29 ? colors[n] : 0;
}
BOOL W32API SetSysColors(int n, const int *index, const DWORD *values) {
  U32_GUARD;
  if (n < 0 || (!index && n) || (!values && n))
    return u32_fail(87);
  for (int i = 0; i < n; i++)
    if (index[i] < 0 || index[i] >= 29)
      return u32_fail(87);
  for (int i = 0; i < n; i++)
    colors[index[i]] = values[i] & 0xffffff;
  for (UINT i = 0; i < U32_WINDOWS; i++)
    if (u32_windows[i].handle && !u32_windows[i].parent)
      SendMessageA(u32_windows[i].handle, 21, 0, 0);
  return 1;
}
int W32API GetSystemMetrics(int n) {
  U32_GUARD;
  struct ciuki_display_info d = {.version = 1, .size = sizeof(d)};
  if ((int32_t)u32_backend.gate(CIUKI_SYS_DISPLAY, CIUKI_DISPLAY_QUERY,
                                U32_PTR(&d), 0) == 0 &&
      d.width && d.height) {
    u32_screen_w = d.width;
    u32_screen_h = d.height;
  }
  switch (n) {
  case 0:
  case 16:
  case 59:
  case 61:
    return u32_screen_w;
  case 1:
  case 17:
  case 60:
  case 62:
    return n == 17 ? (int)u32_screen_h - 32 : (int)u32_screen_h;
  case 2:
  case 3:
  case 9:
  case 10:
  case 20:
  case 21:
    return 16;
  case 4:
    return 21;
  case 5:
  case 6:
    return 1;
  case 7:
  case 8:
    return 3;
  case 11:
  case 12:
  case 13:
  case 14:
    return 32;
  case 15:
    return 18;
  case 19:
    return 1;
  case 28:
    return 112;
  case 29:
    return 27;
  case 30:
  case 31:
    return 18;
  case 32:
  case 33:
    return 3;
  case 34:
  case 35:
    return 112;
  case 36:
  case 37:
    return 4;
  case 38:
  case 39:
    return 75;
  case 40:
    return 0;
  case 43:
    return 3;
  case 45:
    return 2;
  case 49:
  case 50:
    return 16;
  case 51:
  case 52:
    return 18;
  case 63:
  case 64:
    return 0;
  case 67:
  case 68:
    return 4;
  case 69:
    return 0;
  case 75:
    return 1;
  case 80:
    return 1;
  default:
    return 0;
  }
}
static void logfont(uint8_t *out, BOOL bold) {
  w32_fill(out, 0, 60);
  w32_put32(out, (DWORD)-11);
  w32_put32(out + 16, bold ? 700 : 400);
  out[23] = 1;
  out[27] = 0x22;
  w32_copy(out + 28, "MS Sans Serif", 14);
}
BOOL W32API SystemParametersInfoA(UINT action, UINT param, void *out,
                                  UINT flags) {
  U32_GUARD;
  (void)flags;
  static const struct {
    UINT get, set, value;
  } values[] = {{5, 6, 1},   {10, 11, 31},    {14, 15, 600}, {16, 17, 0},
                {22, 23, 1}, {38, 37, 0},     {70, 71, 500}, {74, 75, 0},
                {94, 93, 0}, {102, 103, 400}, {104, 105, 3}, {106, 107, 400}};
  for (UINT i = 0; i < sizeof(values) / sizeof(*values); i++) {
    UINT key = values[i].get;
    if (action == key) {
      if (!out)
        return u32_fail(87);
      *(DWORD *)out = setting_written[key] ? settings[key] : values[i].value;
      return 1;
    }
    if (action == values[i].set) {
      settings[key] = param;
      setting_written[key] = 1;
      return 1;
    }
  }
  switch (action) {
  case 48:
    if (!out)
      return u32_fail(87);
    GetSystemMetrics(0);
    *(RECT *)out = (RECT){0, 32, u32_screen_w, u32_screen_h};
    return 1;
  case 2:
    if (!out)
      return u32_fail(87);
    ((int *)out)[0] = 6;
    ((int *)out)[1] = 10;
    ((int *)out)[2] = 1;
    return 1;
  case 13:
  case 24:
    if (param)
      return u32_fail(120);
    if (!out)
      return u32_fail(87);
    *(UINT *)out = 75;
    return 1;
  case 31:
    if (!out)
      return u32_fail(87);
    logfont(out, 0);
    return 1;
  case 41: {
    if (!out || w32_u32(out) != 340)
      return u32_fail(87);
    uint8_t *p = out;
    w32_fill(p + 4, 0, 336);
    w32_put32(p + 4, 1);
    w32_put32(p + 8, 16);
    w32_put32(p + 12, 16);
    w32_put32(p + 16, 18);
    w32_put32(p + 20, 21);
    logfont(p + 24, 1);
    w32_put32(p + 84, 16);
    w32_put32(p + 88, 21);
    logfont(p + 92, 0);
    w32_put32(p + 152, 18);
    w32_put32(p + 156, 18);
    logfont(p + 160, 0);
    logfont(p + 220, 0);
    logfont(p + 280, 0);
    return 1;
  }
  case 43:
    if (!out || w32_u32(out) != 20)
      return u32_fail(87);
    w32_put32((uint8_t *)out + 4, 160);
    w32_put32((uint8_t *)out + 8, 0);
    w32_put32((uint8_t *)out + 12, 0);
    w32_put32((uint8_t *)out + 16, 8);
    return 1;
  case 45:
    if (!out || w32_u32(out) != 76)
      return u32_fail(87);
    w32_put32((uint8_t *)out + 4, 75);
    w32_put32((uint8_t *)out + 8, 75);
    w32_put32((uint8_t *)out + 12, 1);
    logfont((uint8_t *)out + 16, 0);
    return 1;
  case 72:
    if (!out || w32_u32(out) != 8)
      return u32_fail(87);
    w32_put32((uint8_t *)out + 4, 1);
    return 1;
  case 32:
    return SetDoubleClickTime(param);
  default:
    return u32_fail(120);
  }
}
BOOL W32API MessageBeep(UINT type) {
  U32_GUARD;
  (void)type;
  static BOOL once;
  if (!once) {
    once = 1;
    w32_diag("stub", "user32.dll", "MessageBeep");
  }
  return u32_fail(120);
}
BOOL W32API ExitWindowsEx(UINT flags, DWORD reserved) {
  U32_GUARD;
  (void)reserved;
  if (flags & ~0x1fu)
    return u32_fail(87);
  w32_diag("end-session", "user32.dll", "ExitWindowsEx");
  struct u32_window *w = u32_root(u32_window(u32_foreground));
  if (!w)
    for (UINT i = 0; i < U32_WINDOWS; i++)
      if (u32_windows[i].handle && !u32_windows[i].parent) {
        w = &u32_windows[i];
        break;
      }
  if (w) {
    uint8_t b[4];
    desk_put32(b, flags);
    u32_wire(w, DESK_V3_RESTART, b, 4, -1);
  }
  return 1;
}
