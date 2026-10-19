/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "codec.h"
#ifndef W32_HOST_MODEL
__declspec(dllimport)
#endif
void *W32API GetProcessHeap(void);
#ifndef W32_HOST_MODEL
__declspec(dllimport)
#endif
void *W32API HeapAlloc(void *, uint32_t, uint32_t);
#ifndef W32_HOST_MODEL
__declspec(dllimport)
#endif
int W32API HeapFree(void *, uint32_t, void *);
struct video {
  uint32_t token, kind, begun;
  struct cc_cinepak *ctx;
};
static struct video videos[16];
static uint32_t serial;
static const uint32_t kinds[3] = {CC_FOUR('m', 'r', 'l', 'e'),
                                  CC_FOUR('m', 's', 'v', 'c'),
                                  CC_FOUR('c', 'v', 'i', 'd')};
static struct video *find(uintptr_t h) {
  uint32_t i;
  for (i = 0; i < 16; i++)
    if (videos[i].token && videos[i].token == h)
      return &videos[i];
  return 0;
}
static void wide(WCHAR *out, const char *s, uint32_t cap) {
  uint32_t i;
  for (i = 0; i + 1 < cap && s[i]; i++)
    out[i] = (uint8_t)s[i];
  out[i] = 0;
}
static int info(uint32_t kind, struct cc_icinfo *out, uint32_t bytes) {
  if (!out || bytes < sizeof(*out))
    return 0;
  cc_zero(out, sizeof(*out));
  out->size = sizeof(*out);
  out->type = CC_FOUR('v', 'i', 'd', 'c');
  out->handler = kind;
  out->flags = 0; /* Decode-only: do not advertise compression/quality. */
  out->version = 0x10000;
  out->icm_version = 0x104;
  wide(out->name,
       kind == kinds[0]   ? "MS RLE"
       : kind == kinds[1] ? "MS Video 1"
                          : "Cinepak",
       16);
  wide(out->description, "CiukiOS bounded video decoder", 128);
  wide(out->driver,
       kind == kinds[0]   ? "msrle32.dll"
       : kind == kinds[1] ? "msvidc32.dll"
                          : "iccvid.dll",
       128);
  return sizeof(*out);
}
uint32_t W32API VideoForWindowsVersion(void) { return 0x01040000; }
int W32API ICInfo(uint32_t type, uint32_t handler, struct cc_icinfo *out) {
  uint32_t i, kind = handler | 0x20202020u;
  if (!out)
    return 0;
  cc_zero(out, sizeof(*out));
  out->size = sizeof(*out);
  out->type = type;
  out->handler = handler;
  out->icm_version = 0x104;
  if (type && type != CC_FOUR('v', 'i', 'd', 'c'))
    return 0;
  if (handler < 3)
    kind = kinds[handler];
  for (i = 0; i < 3; i++)
    if (kind == kinds[i])
      break;
  if (i == 3)
    return 0;
  out->type = CC_FOUR('v', 'i', 'd', 'c');
  out->handler = kind;
  wide(out->driver,
       kind == kinds[0]   ? "msrle32.dll"
       : kind == kinds[1] ? "msvidc32.dll"
                          : "iccvid.dll",
       128);
  return 1;
}
uintptr_t W32API ICOpen(uint32_t type, uint32_t handler, uint32_t mode) {
  uint32_t i, kind = handler | 0x20202020u;
  if (type != CC_FOUR('v', 'i', 'd', 'c') || (mode != 2 && mode != 4))
    return 0;
  if (kind == CC_FOUR('c', 'r', 'a', 'm') ||
      kind == CC_FOUR('w', 'h', 'a', 'm'))
    kind = kinds[1];
  for (i = 0; i < 3; i++)
    if (kind == kinds[i])
      break;
  if (i == 3)
    return 0;
  for (i = 0; i < 16; i++)
    if (!videos[i].token) {
      struct video *v = &videos[i];
      serial = (serial + 1) & 0x7ffffffu;
      if (!serial)
        serial = 1;
      v->token = (serial << 4) | i;
      v->kind = kind;
      v->begun = 0;
      v->ctx = 0;
      return v->token;
    }
  return 0;
}
int32_t W32API ICClose(uintptr_t h) {
  struct video *v = find(h);
  if (!v)
    return CC_BADHANDLE;
  if (v->ctx)
    HeapFree(GetProcessHeap(), 0, v->ctx);
  cc_zero(v, sizeof(*v));
  return 0;
}
intptr_t W32API ICSendMessage(uintptr_t h, uint32_t message, uintptr_t p1,
                              uintptr_t p2) {
  struct video *v = find(h);
  int result;
  if (!v)
    return CC_BADHANDLE;
  if (message == 0x5002)
    return info(v->kind, (void *)p1, (uint32_t)p2);
  if (message == 0x400b)
    return cc_video_query(v->kind, (void *)p1, (void *)p2);
  if (message == 0x400a) {
    struct cc_bitmap *in = (void *)p1, *out = (void *)p2;
    if (cc_video_query(v->kind, in, 0))
      return CC_BADFORMAT;
    if (!out)
      return 40;
    cc_copy(out, in, 40);
    out->bits = v->kind == kinds[2] ? 24 : (v->kind == kinds[0] ? 8 : in->bits);
    out->compression = 0;
    out->size = 40;
    out->bytes = (((uint32_t)out->width * out->bits + 31) / 32) * 4 *
                 (uint32_t)(out->height < 0 ? -out->height : out->height);
    return 0;
  }
  if (message == 0x400c) {
    result = cc_video_query(v->kind, (void *)p1, (void *)p2);
    if (result)
      return result;
    if (!p2)
      return CC_BADPARAM;
    if (v->kind == kinds[2]) {
      if (!v->ctx)
        v->ctx = HeapAlloc(GetProcessHeap(), 8, sizeof(*v->ctx));
      else
        cc_zero(v->ctx, sizeof(*v->ctx));
      if (!v->ctx)
        return -3;
    }
    v->begun = 1;
    return 0;
  }
  if (message == 0x400e) {
    if (v->ctx) {
      HeapFree(GetProcessHeap(), 0, v->ctx);
      v->ctx = 0;
    }
    v->begun = 0;
    return 0;
  }
  if (message == 0x400d) {
    struct cc_decompress *d = (void *)p1;
    if (!v->begun || !d || p2 < sizeof(*d) || d->flags & ~0xf8000000u)
      return CC_BADPARAM;
    if (d->flags & 0x10000000)
      return 0;
    return cc_video(v->kind, v->ctx, d->in, d->src, d->out, d->dst);
  }
  return -1;
}
int32_t W32API ICGetInfo(uintptr_t h, struct cc_icinfo *out, uint32_t bytes) {
  return (int32_t)ICSendMessage(h, 0x5002, (uintptr_t)out, bytes);
}
int32_t ICDecompress(uintptr_t h, uint32_t flags, struct cc_bitmap *in,
                     const void *src, struct cc_bitmap *out, void *dst) {
  struct cc_decompress d = {flags, in, src, out, dst, 0};
  return (int32_t)ICSendMessage(h, 0x400d, (uintptr_t)&d, sizeof(d));
}
uintptr_t W32API ICLocate(uint32_t type, uint32_t handler, struct cc_bitmap *in,
                          struct cc_bitmap *out, uint32_t mode) {
  uint32_t i;
  uintptr_t h;
  if (handler) {
    h = ICOpen(type, handler, mode);
    if (h && ICSendMessage(h, 0x400b, (uintptr_t)in, (uintptr_t)out)) {
      ICClose(h);
      h = 0;
    }
    return h;
  }
  for (i = 0; i < 3; i++) {
    h = ICLocate(type, kinds[i], in, out, mode);
    if (h)
      return h;
  }
  return 0;
}
#ifdef CC_VIDEO_DRIVER
intptr_t W32API DriverProc(uintptr_t id, uintptr_t driver, uint32_t message,
                           uintptr_t p1, uintptr_t p2) {
  (void)driver;
  if (message == 1 || message == 2 || message == 5 || message == 6 ||
      message == 7 || message == 8)
    return 1;
  if (message == 3)
    return (intptr_t)ICOpen(CC_FOUR('v', 'i', 'd', 'c'), CC_VIDEO_DRIVER, 2);
  if (message == 4)
    return ICClose(id) == 0;
  return ICSendMessage(id, message, p1, p2);
}
#endif
