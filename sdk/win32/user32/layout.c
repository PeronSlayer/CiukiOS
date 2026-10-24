/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../../../apps/desktop/input_text.h"
#include "internal.h"
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
UINT u32_scan_vk(UINT layout, UINT code) {
  if (code == 0x146)
    return 3; /* Ctrl+Break */
  if (code == 0x54)
    return 0x2c; /* Alt+SysRq */
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
  if (layout) {
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

UINT u32_dik(UINT code) {
  if (code == 0x200)
    return 0xc5;
  return code < 0x80 ? code
                     : (code >= 0x100 && code < 0x180 ? (code & 127) | 128 : 0);
}
DWORD u32_key_lparam(UINT code, BOOL down, BOOL previous, BOOL alt) {
  UINT scan = code == 0x200 ? 0x45 : code & 255;
  return 1u | (scan << 16) | ((code & 0x100) || code == 0x45 ? 1u << 24 : 0) |
         (alt ? 1u << 29 : 0) | (previous || !down ? 1u << 30 : 0) |
         (!down ? 1u << 31 : 0);
}
void u32_key_state(uint8_t *keys, UINT key, UINT code, BOOL down, BOOL repeat) {
  if (key == 16 || key == 17 || key == 18) {
    UINT left = key == 16 ? 160 : key == 17 ? 162 : 164;
    UINT side = left + (key == 16 ? code == 0x36 : !!(code & 0x100));
    keys[side] = down ? 128 : 0;
    keys[key] = (keys[left] | keys[left + 1]) & 128;
  } else {
    if (down && !repeat && !(keys[key] & 128) &&
        (key == 20 || key == 144 || key == 145))
      keys[key] ^= 1;
    keys[key] = (keys[key] & 1) | (down ? 128 : 0);
  }
}
static UINT scan_code(UINT code) {
  return (code & 0xff00) == 0xe000 ? 0x100 | (code & 255) : code;
}
static int layout_id(uintptr_t h) {
  return h == 0x04100410 ? 1 : h == 0x04090409 ? 0 : -1;
}
uintptr_t W32API GetKeyboardLayout(DWORD tid) {
  U32_GUARD;
  u32_settings();
  (void)tid;
  return u32_layout ? 0x04100410 : 0x04090409;
}
int W32API GetKeyboardLayoutList(int count, uintptr_t *list) {
  U32_GUARD;
  if (count < 0)
    return u32_fail(87), 0;
  if (!count)
    return 2;
  if (!list)
    return u32_fail(87), 0;
  list[0] = GetKeyboardLayout(0);
  if (count > 1)
    list[1] = list[0] == 0x04090409 ? 0x04100410 : 0x04090409;
  return count > 1 ? 2 : 1;
}
BOOL W32API GetKeyboardLayoutNameA(char *out) {
  U32_GUARD;
  if (!out)
    return u32_fail(87);
  w32_copy(out, GetKeyboardLayout(0) == 0x04100410 ? "00000410" : "00000409",
           9);
  return 1;
}
BOOL W32API GetKeyboardLayoutNameW(WCHAR *out) {
  char name[9];
  if (!out || !GetKeyboardLayoutNameA(name))
    return 0;
  for (UINT i = 0; i < 9; i++)
    out[i] = (uint8_t)name[i];
  return 1;
}
uintptr_t W32API ActivateKeyboardLayout(uintptr_t h, UINT flags) {
  U32_GUARD;
  (void)flags;
  uintptr_t old = GetKeyboardLayout(0);
  if (h == 0 || h == 1)
    h = old == 0x04090409 ? 0x04100410 : 0x04090409;
  int id = layout_id(h);
  if (id < 0)
    return u32_fail(87), 0;
  u32_layout =
      id; /* Recorded until a subsequent desktop configuration refresh. */
  return old;
}
int W32API GetKeyboardType(int type) {
  return type == 0 ? 4 : type == 1 ? 0 : type == 2 ? 12 : 0;
}
UINT W32API MapVirtualKeyExA(UINT code, UINT type, uintptr_t h) {
  U32_GUARD;
  int layout = layout_id(h);
  if (layout < 0)
    return 0;
  if (type == 1 || type == 3) {
    UINT scan = scan_code(code), key = u32_scan_vk(layout, scan);
    if (type == 3 && key >= 16 && key <= 18)
      return key == 16   ? (scan == 0x36 ? 161 : 160)
             : key == 17 ? (scan & 0x100 ? 163 : 162)
                         : (scan & 0x100 ? 165 : 164);
    return key;
  }
  if (type == 0) {
    if (code >= 160 && code <= 165) {
      static const UINT sides[] = {0x2a, 0x36, 0x1d, 0x1d, 0x38, 0x38};
      return sides[code - 160];
    }
    if (code == 0x13)
      return 0x45;
    for (UINT scan = 1; scan < 0x59; scan++)
      if (u32_scan_vk(layout, scan) == code)
        return scan;
    for (UINT scan = 0x100; scan < 0x160; scan++)
      if (u32_scan_vk(layout, scan) == code)
        return scan & 255;
    return 0;
  }
  if (type == 2) {
    if (code >= 0x60 && code <= 0x69)
      return '0' + code - 0x60;
    if (code == 8 || code == 9 || code == 13 || code == 27)
      return code;
    UINT scan = MapVirtualKeyExA(code, 0, h);
    unsigned c = ciuki_keymap_char(layout, scan, 0);
    return c >= 'a' && c <= 'z' ? c - 32 : c;
  }
  return 0;
}
UINT W32API MapVirtualKeyA(UINT code, UINT type) {
  return MapVirtualKeyExA(code, type, GetKeyboardLayout(0));
}
UINT W32API MapVirtualKeyW(UINT code, UINT type) {
  return MapVirtualKeyA(code, type);
}
UINT W32API MapVirtualKeyExW(UINT code, UINT type, uintptr_t h) {
  return MapVirtualKeyExA(code, type, h);
}
int16_t W32API VkKeyScanExA(char c, uintptr_t h) {
  U32_GUARD;
  int layout = layout_id(h);
  if (layout < 0)
    return -1;
  unsigned cp = u32_scalar((uint8_t)c);
  if (!cp)
    return -1;
  if (cp == 8 || cp == 9 || cp == 13 || cp == 27)
    return cp;
  for (UINT mods = 0; mods < 8; mods++) {
    if ((mods & 6) && (mods & 6) != 6)
      continue;
    unsigned flags = (mods & 1 ? CIUKI_KEYMAP_SHIFT : 0) |
                     ((mods & 6) == 6 ? CIUKI_KEYMAP_ALTGR : 0);
    for (UINT scan = 1; scan <= 0x56; scan++)
      if (ciuki_keymap_char(layout, scan, flags) == cp)
        return u32_scan_vk(layout, scan) | (mods << 8);
  }
  return -1;
}
int16_t W32API VkKeyScanA(char c) {
  return VkKeyScanExA(c, GetKeyboardLayout(0));
}
int W32API ToAsciiEx(UINT v, UINT scan, const uint8_t *keys, uint16_t *out,
                     UINT flags, uintptr_t h) {
  U32_GUARD;
  (void)flags;
  int layout = layout_id(h);
  if (!keys || !out || layout < 0 || scan & 0x8000)
    return 0;
  unsigned c = 0;
  if (v >= 0x60 && v <= 0x69)
    c = '0' + v - 0x60;
  else if (v == 0x6a)
    c = '*';
  else if (v == 0x6b)
    c = '+';
  else if (v == 0x6d)
    c = '-';
  else if (v == 0x6e)
    c = '.';
  else if (v == 0x6f)
    c = '/';
  else if (v == 8 || v == 9 || v == 13 || v == 27)
    c = v;
  else {
    if (v >= 0x21 && v <= 0x2e)
      return 0;
    if (!scan)
      scan = MapVirtualKeyExA(v, 0, h);
    unsigned mods = (keys[16] & 128 ? CIUKI_KEYMAP_SHIFT : 0) |
                    (keys[20] & 1 ? CIUKI_KEYMAP_CAPS : 0) |
                    ((keys[165] & 128) || ((keys[17] & 128) && (keys[18] & 128))
                         ? CIUKI_KEYMAP_ALTGR
                         : 0);
    c = ciuki_keymap_char(layout, scan & 255, mods);
    if ((keys[17] & 128) && !(mods & CIUKI_KEYMAP_ALTGR)) {
      if (c >= 'a' && c <= 'z')
        c -= 32;
      if (c >= '@' && c <= '_')
        c &= 31;
      else
        return 0;
      *out = (uint16_t)c;
      return 1;
    }
  }
  if (!c)
    return 0;
  struct u32_thread *thread=u32_thread();
  unsigned chars[2], pending=thread ? thread->dead_accent : 0;
  unsigned count=ciuki_input_compose(&pending,c,layout==1 && (scan&255)==0x0d && (keys[16]&128),chars);
  if(thread)thread->dead_accent=pending;
  if(!count) {*out=(uint16_t)u32_ansi(c);return -1;}
  *out=(uint16_t)u32_ansi(chars[0]);
  if(count==2)*out|=(uint16_t)((uint8_t)u32_ansi(chars[1]))<<8;
  return (int)count;
}
int W32API ToAscii(UINT v, UINT scan, const uint8_t *keys, uint16_t *out,
                   UINT flags) {
  return ToAsciiEx(v, scan, keys, out, flags, GetKeyboardLayout(0));
}
DWORD W32API OemKeyScan(uint16_t ch) {
  if (ch > 255)
    return 0xffffffffu;
  char byte = (char)ch;
  WCHAR cp;
  if (!MultiByteToWideChar(437, 0, &byte, 1, &cp, 1))
    return 0xffffffffu;
  int ansi = u32_ansi(cp);
  if (u32_scalar((uint8_t)ansi) != cp)
    return 0xffffffffu;
  int16_t key = VkKeyScanA((char)ansi);
  if (key == -1 || ((key >> 8) & 6) == 6)
    return 0xffffffffu;
  return MapVirtualKeyA(key & 255, 0) | ((DWORD)(uint16_t)key >> 8 << 16);
}
int W32API GetKeyNameTextA(LONG lp, char *out, int count) {
  U32_GUARD;
  if (!out || count <= 0)
    return 0;
  UINT scan = ((DWORD)lp >> 16) & 255, ext = ((DWORD)lp >> 24) & 1;
  const char *name = 0;
  char tmp[16] = {0};
  UINT key = scan == 0x45 ? (ext ? 144 : 19)
                          : u32_scan_vk(u32_layout, scan | (ext ? 0x100 : 0));
  if ((DWORD)lp & (1u << 25)) {
    if (key == 16)
      name = "Shift";
    if (key == 17)
      name = "Ctrl";
    if (key == 18)
      name = "Alt";
  }
  if (!name)
    switch (key) {
    case 8:
      name = "Backspace";
      break;
    case 9:
      name = "Tab";
      break;
    case 13:
      name = ext ? "Num Enter" : "Enter";
      break;
    case 16:
      name = scan == 0x36 ? "Right Shift" : "Left Shift";
      break;
    case 17:
      name = ext ? "Right Ctrl" : "Left Ctrl";
      break;
    case 18:
      name = ext ? "Right Alt" : "Left Alt";
      break;
    case 19:
      name = "Pause";
      break;
    case 20:
      name = "Caps Lock";
      break;
    case 27:
      name = "Esc";
      break;
    case 32:
      name = "Space";
      break;
    case 33:
      name = "Page Up";
      break;
    case 34:
      name = "Page Down";
      break;
    case 35:
      name = "End";
      break;
    case 36:
      name = "Home";
      break;
    case 37:
      name = "Left";
      break;
    case 38:
      name = "Up";
      break;
    case 39:
      name = "Right";
      break;
    case 40:
      name = "Down";
      break;
    case 44:
      name = "Print Screen";
      break;
    case 45:
      name = "Insert";
      break;
    case 46:
      name = "Delete";
      break;
    case 144:
      name = "Num Lock";
      break;
    case 145:
      name = "Scroll Lock";
      break;
    case 3:
      name = "Break";
      break;
    default:
      break;
    }
  if (!name) {
    if (key >= 0x70 && key <= 0x7b) {
      tmp[0] = 'F';
      UINT n = key - 0x6f;
      tmp[1] = n < 10 ? '0' + n : '1';
      if (n >= 10)
        tmp[2] = '0' + n % 10;
    } else if (key >= 0x60 && key <= 0x69) {
      w32_copy(tmp, "Num ", 4);
      tmp[4] = '0' + key - 0x60;
    } else {
      unsigned c = ciuki_keymap_char(u32_layout, scan, 0);
      if (c >= 'a' && c <= 'z')
        c -= 32;
      tmp[0] = (char)u32_ansi(c);
    }
    name = tmp;
  }
  UINT n = w32_strlen(name);
  if (n >= (UINT)count)
    n = count - 1;
  w32_copy(out, name, n);
  out[n] = 0;
  return n;
}
int W32API GetKeyNameTextW(LONG lp, WCHAR *out, int count) {
  char name[64];
  if (!out || count <= 0)
    return 0;
  int n = GetKeyNameTextA(lp, name, 64);
  if (n >= count)
    n = count - 1;
  for (int i = 0; i < n; i++)
    out[i] = u32_scalar((uint8_t)name[i]);
  out[n] = 0;
  return n;
}
