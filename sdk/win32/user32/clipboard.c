/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"
extern HANDLE W32API GlobalAlloc(DWORD, DWORD);
extern HANDLE W32API GlobalFree(HANDLE);
extern void *W32API GlobalLock(HANDLE);
extern BOOL W32API GlobalUnlock(HANDLE);
extern DWORD W32API GlobalSize(HANDLE);
static HANDLE owned[48], returned[48];
static uint8_t promised[48];
BOOL u32_clip_has_delayed(void) { for (UINT i=0;i<48;i++) if (promised[i]) return 1; return 0; }
static UINT formats[48]={CF_TEXT,CF_OEMTEXT,CF_UNICODETEXT,CF_DIB,CF_BITMAP};
static UINT format_count=5;
static BOOL opened;
static DWORD open_tid;
static void trim_formats(void) {
  for (UINT i=0;i<48;i++) if (owned[i] || returned[i] || promised[i]) return;
  format_count=5;
}
static void clear_returned(void) {
  for (UINT i=0;i<48;i++) if (returned[i]) {
    if (i==4) { if (u32_gdi && u32_gdi->bitmap) u32_gdi->bitmap(returned[i],0,1); }
    else GlobalFree(returned[i]);
    returned[i]=0;
  }
  trim_formats();
}
void u32_clipboard_owned_clear(void) {
  for (UINT i=0;i<48;i++) if (owned[i]) {
    if (i==4) { if (u32_gdi && u32_gdi->bitmap) u32_gdi->bitmap(owned[i],0,1); }
    else if (formats[i]<0x200 || formats[i]>0x2ff) GlobalFree(owned[i]);
    owned[i]=0;
  }
  for (UINT i=0;i<48;i++) promised[i]=0;
  trim_formats();
}
static UINT slot(UINT format) {
  for (UINT i=0;i<format_count;i++) if (formats[i]==format) return i;
  if (format_count==48) return 48;
  formats[format_count]=format; return format_count++;
}
static BOOL call(UINT op, HWND hwnd, UINT format, DWORD flags, const void *data,
                 DWORD bytes, struct w32r_reply *r, void *result, DWORD cap) {
  return u32_clip_call(op, hwnd, format, flags, data, bytes, r, result, cap);
}
BOOL W32API OpenClipboard(HWND h) {
  U32_GUARD;
  struct w32r_reply r;
  if (!call(W32R_CLIP_OPEN, h, 0, 0, 0, 0, &r, 0, 0))
    return 0;
  opened = 1;
  open_tid = u32_tid();
  return 1;
}
BOOL W32API CloseClipboard(void) {
  U32_GUARD;
  if (!opened || open_tid!=u32_tid()) return u32_fail(1418);
  struct w32r_reply r;
  if (!call(W32R_CLIP_CLOSE, 0, 0, 0, 0, 0, &r, 0, 0))
    return 0;
  opened = 0;
  open_tid = 0;
  clear_returned();
  return 1;
}
BOOL W32API EmptyClipboard(void) {
  U32_GUARD;
  if (!opened || open_tid!=u32_tid()) return u32_fail(1418);
  struct w32r_reply r;
  if (!call(W32R_CLIP_EMPTY, 0, 0, 0, 0, 0, &r, 0, 0))
    return 0;
  if (r.disposition)
    SendMessageA((HWND)(uintptr_t)r.disposition, WM_DESTROYCLIPBOARD, 0, 0);
  u32_clipboard_owned_clear(); clear_returned();
  if (r.type)
    SendMessageA((HWND)(uintptr_t)r.type, WM_DRAWCLIPBOARD, 0, 0);
  return 1;
}
HWND W32API GetClipboardOwner(void) {
  U32_GUARD;
  struct w32r_reply r;
  return call(W32R_CLIP_OWNER, 0, 0, 0, 0, 0, &r, 0, 0) ? (HWND)(uintptr_t)r.key
                                                        : 0;
}
HWND W32API SetClipboardViewer(HWND h) {
  U32_GUARD;
  struct w32r_reply r;
  if (!call(W32R_CLIP_VIEWER,h,0,0,0,0,&r,0,0)) return 0;
  SendMessageA(h,WM_DRAWCLIPBOARD,0,0);
  return (HWND)(uintptr_t)r.key;
}
BOOL W32API ChangeClipboardChain(HWND h, HWND next) {
  U32_GUARD;
  struct w32r_reply r;
  if (!call(W32R_CLIP_CHAIN, h, 0, U32_PTR(next), 0, 0, &r, 0, 0))
    return 0;
  if (r.type && r.type != U32_PTR(h))
    SendMessageA((HWND)(uintptr_t)r.type, WM_CHANGECBCHAIN, (WPARAM)h,
                 (LPARAM)next);
  return 1;
}
static BOOL available(UINT f) {
  struct w32r_reply r;
  return call(W32R_CLIP_QUERY, 0, f, 0, 0, 0, &r, 0, 0) && r.disposition;
}
BOOL W32API IsClipboardFormatAvailable(UINT f) {
  U32_GUARD;
  if (available(f))
    return 1;
  if (f == CF_TEXT || f == CF_OEMTEXT || f == CF_UNICODETEXT)
    return available(CF_TEXT) || available(CF_OEMTEXT) ||
           available(CF_UNICODETEXT);
  if (f == CF_BITMAP)
    return available(CF_DIB);
  return 0;
}
HANDLE W32API SetClipboardData(UINT f, HANDLE block) {
  U32_GUARD;
  if ((!opened || open_tid!=u32_tid()) && !u32_clip_rendering()) { u32_fail(1418); return 0; }
  if (!block) {
    struct w32r_reply r;
    if (!opened || open_tid!=u32_tid()) { u32_fail(1418); return 0; }
    if (call(W32R_CLIP_SET,0,f,0,0,0,&r,0,0)) { UINT i=slot(f); if (i<48) promised[i]=1; }
    return 0;
  }
  if (!f || slot(f)==48) {
    u32_fail(120);
    return 0;
  }
  UINT original_format = f;
  void *p = 0;
  DWORD n = 0;
  struct u32_surface bitmap = {0};
  uint8_t *dib = 0;
  if (f == CF_BITMAP) {
    u32_gdi_ensure();
    if (!u32_gdi || !u32_gdi->bitmap || !u32_gdi->bitmap(block, &bitmap, 0)) {
      u32_fail(120);
      return 0;
    }
    if (bitmap.width > 2048 || bitmap.height > 2048 ||
        bitmap.pitch * bitmap.height > DESK_CLIP_ITEM_MAX - 40) {
      u32_gdi->bitmap(block, &bitmap, 1);
      u32_fail(8);
      return 0;
    }
    n = 40 + bitmap.width * bitmap.height * 4;
    dib = HeapAlloc(GetProcessHeap(),8,n);
    if (!dib) {
      u32_gdi->bitmap(block, &bitmap, 1);
      return 0;
    }
    w32_put32(dib, 40);
    w32_put32(dib + 4, bitmap.width);
    w32_put32(dib + 8, -(LONG)bitmap.height);
    w32_put16(dib + 12, 1);
    w32_put16(dib + 14, 32);
    for (UINT y = 0; y < bitmap.height; y++)
      w32_copy(dib + 40 + y * bitmap.width * 4,
               (uint8_t *)bitmap.pixels + y * bitmap.pitch, bitmap.width * 4);
    p = dib;
    f = CF_DIB;
  } else {
    n = GlobalSize(block);
    p = GlobalLock(block);
    if (!p || !n || n > DESK_CLIP_ITEM_MAX) {
      if (p)
        GlobalUnlock(block);
      u32_fail(87);
      return 0;
    }
    if (f == CF_UNICODETEXT && (n & 1)) {
      GlobalUnlock(block);
      u32_fail(87);
      return 0;
    }
  }
  struct w32r_reply r;
  BOOL ok = call(W32R_CLIP_SET, 0, f, 0, p, n, &r, 0, 0);
  if (dib) {
    HeapFree(GetProcessHeap(),0,dib);
    u32_gdi->bitmap(block, &bitmap, 1);
  } else
    GlobalUnlock(block);
  if (!ok)
    return 0;
  UINT index=slot(original_format);
  if (owned[index] && owned[index]!=block) {
    if (index==4) { if (u32_gdi && u32_gdi->bitmap) u32_gdi->bitmap(owned[index],0,1); }
    else if (original_format<0x200 || original_format>0x2ff) GlobalFree(owned[index]);
  }
  for (UINT i=0;i<48;i++) if (returned[i]==block) returned[i]=0;
  owned[index] = block; promised[index]=0;
  if (r.type)
    SendMessageA((HWND)(uintptr_t)r.type, WM_DRAWCLIPBOARD, 0, 0);
  return block;
}
HANDLE W32API GetClipboardData(UINT f) {
  U32_GUARD;
  if (!opened || open_tid!=u32_tid()) {
    u32_fail(1418);
    return 0;
  }
  if (slot(f)==48) { u32_fail(8); return 0; }
  UINT source = f;
  if (!available(source)) {
    if (f == CF_TEXT || f == CF_OEMTEXT || f == CF_UNICODETEXT)
      source = available(CF_UNICODETEXT) ? CF_UNICODETEXT
               : available(CF_TEXT)      ? CF_TEXT
                                         : CF_OEMTEXT;
    else if (f == CF_BITMAP)
      source = CF_DIB;
  }
  extern void *u32_clip_fetch(UINT, DWORD *);
  DWORD bytes=0;
  uint8_t *data=u32_clip_fetch(source,&bytes);
  if (!data) return 0;
  HANDLE h = 0;
  if (f == CF_BITMAP) {
    h = u32_dib_bitmap(data, bytes);
    HeapFree(GetProcessHeap(),0,data);
    if (!h)
      u32_fail(120);
    if (returned[4] && u32_gdi && u32_gdi->bitmap)
      u32_gdi->bitmap(returned[4], 0, 1);
    returned[4] = h;
    return h;
  }
  if (f != source) {
    WCHAR *wide = HeapAlloc(GetProcessHeap(),8,131074);
    if (!wide) {
      HeapFree(GetProcessHeap(),0,data);
      return 0;
    }
    UINT count = 0;
    if (source == CF_UNICODETEXT) {
      for (UINT i = 0; i + 1 < bytes && w32_u16(data + i); i += 2)
        wide[count++] = w32_u16(data + i);
    } else {
      int n = MultiByteToWideChar(source == CF_OEMTEXT ? 437 : 1252, 0,
                                  (char *)data, bytes, wide, 65536);
      if (n > 0) {
        count = n;
        while (count && wide[count - 1] == 0)
          count--;
      }
    }
    wide[count++] = 0;
    HeapFree(GetProcessHeap(),0,data);
    if (f == CF_UNICODETEXT) {
      data = (uint8_t *)wide;
      bytes = count * 2;
    } else {
      data = u32_alloc(count);
      if (!data) {
        HeapFree(GetProcessHeap(),0,wide);
        return 0;
      }
      int n = WideCharToMultiByte(f == CF_OEMTEXT ? 437 : 1252, 0, wide, count,
                                  (char *)data, count, 0, 0);
      HeapFree(GetProcessHeap(),0,wide);
      if (n <= 0) {
        HeapFree(GetProcessHeap(),0,data);
        return 0;
      }
      bytes = n;
    }
  }
  h = GlobalAlloc(2 | 0x40, bytes);
  if (h) {
    void *p = GlobalLock(h);
    if (p) {
      w32_copy(p, data, bytes);
      GlobalUnlock(h);
    } else {
      GlobalFree(h);
      h = 0;
    }
  }
  HeapFree(GetProcessHeap(),0,data);
  UINT i = slot(f);
  if (returned[i])
    GlobalFree(returned[i]);
  returned[i] = h;
  return h;
}
void u32_clipboard_cleanup(DWORD tid) {
  if (opened && (!tid || open_tid == tid)) {
    u32_clip_abandon(open_tid);
    opened = 0; open_tid = 0;
    for (UINT i = 0; i < 48; i++) if (returned[i]) {
      if (i == 4) {
        if (u32_gdi && u32_gdi->bitmap) u32_gdi->bitmap(returned[i], 0, 1);
      } else GlobalFree(returned[i]);
      returned[i] = 0;
    }
  }
  if (!tid) u32_clipboard_owned_clear();
}
/* f5-20B: the formats the clipboard holds, in ascending order, with the
 * ones GetClipboardData synthesises (text forms from each other, CF_BITMAP
 * from CF_DIB). The clipboard must be open on this thread. */
UINT W32API EnumClipboardFormats(UINT format) {
  U32_GUARD;
  extern UINT u32_clip_enum(UINT);
  if (!opened || open_tid!=u32_tid()) return u32_fail(1418);
  return u32_clip_enum(format);
}
DWORD W32API GetClipboardSequenceNumber(void) {
  U32_GUARD;
  struct w32r_reply r;
  return call(W32R_CLIP_QUERY,0,0,0,0,0,&r,0,0) ? r.disposition : 0;
}
