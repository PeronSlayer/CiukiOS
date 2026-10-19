/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "codec.h"
struct stream {
  uint32_t token;
  uint8_t format[1046];
};
static struct stream streams[32];
static uint32_t serial;
static struct stream *lookup(uintptr_t h) {
  uint32_t i;
  for (i = 0; i < 32; i++)
    if (streams[i].token && streams[i].token == h)
      return &streams[i];
  return 0;
}
uint32_t W32API acmGetVersion(void) { return 0x04000000; }
uint32_t W32API acmFormatSuggest(uintptr_t driver, const struct cc_wave *src,
                                 struct cc_wave *dst, uint32_t bytes,
                                 uint32_t flags) {
  if (driver && driver != 1 && driver != 2 && driver != 17)
    return 5;
  return cc_suggest_format(src, dst, bytes, flags);
}
uint32_t W32API acmStreamOpen(uintptr_t *out, uintptr_t driver,
                              const struct cc_wave *src,
                              const struct cc_wave *dst, void *filter,
                              uintptr_t callback, uintptr_t instance,
                              uint32_t flags) {
  uint32_t samples, i;
  (void)instance;
  if (out)
    *out = 0;
  if (!src || !dst || (!out && !(flags & 1)) || filter || callback ||
      flags & ~1u)
    return CC_INVALID;
  if (driver && driver != src->tag)
    return 5;
  if (cc_wave_validate(src, &samples) || dst->tag != 1 ||
      dst->bits != (src->tag == 1 ? src->bits : 16) ||
      dst->channels != src->channels || dst->rate != src->rate ||
      dst->align != dst->channels * (dst->bits / 8))
    return CC_NOTPOSSIBLE;
  if (flags & 1)
    return 0;
  for (i = 0; i < 32; i++)
    if (!streams[i].token) {
      struct stream *s = &streams[i];
      uint32_t bytes = 18 + src->extra;
      if (bytes > sizeof(s->format))
        return CC_NOTPOSSIBLE;
      cc_copy(s->format, src, bytes);
      serial = (serial + 1) & 0x3ffffffu;
      if (!serial)
        serial = 1;
      s->token = (serial << 5) | i;
      *out = s->token;
      return 0;
    }
  return 7;
}
uint32_t W32API acmStreamClose(uintptr_t h, uint32_t flags) {
  struct stream *s = lookup(h);
  if (!s)
    return 5;
  if (flags)
    return CC_INVALID;
  s->token = 0;
  return 0;
}
uint32_t W32API acmStreamSize(uintptr_t h, uint32_t bytes, uint32_t *out,
                              uint32_t flags) {
  struct stream *s = lookup(h);
  if (!s)
    return 5;
  return cc_stream_size((const struct cc_wave *)s->format, bytes, flags, out);
}
uint32_t W32API acmStreamReset(uintptr_t h, uint32_t flags) {
  if (!lookup(h))
    return 5;
  return flags ? CC_INVALID : 0;
}
uint32_t W32API acmStreamPrepareHeader(uintptr_t h, struct cc_acm_header *b,
                                       uint32_t flags) {
  if (!lookup(h))
    return 5;
  if (!b || b->size < sizeof(*b) || flags || (!b->src && b->src_len) ||
      (!b->dst && b->dst_len))
    return CC_INVALID;
  if (b->status & 0x100000)
    return 33;
  b->status |= 0x20000;
  b->src_used = b->dst_used = 0;
  b->reserved[0] = (uint32_t)h;
  b->reserved[1] = b->src_len;
  b->reserved[2] = b->dst_len;
  return 0;
}
uint32_t W32API acmStreamUnprepareHeader(uintptr_t h, struct cc_acm_header *b,
                                         uint32_t flags) {
  if (!lookup(h))
    return 5;
  if (!b || b->size < sizeof(*b) || flags)
    return CC_INVALID;
  if (b->status & 0x100000)
    return 33;
  if (b->reserved[0] != (uint32_t)h)
    return CC_INVALID;
  b->status &= ~0x20000u;
  b->reserved[0] = 0;
  return 0;
}
uint32_t W32API acmStreamConvert(uintptr_t h, struct cc_acm_header *b,
                                 uint32_t flags) {
  struct stream *s = lookup(h);
  uint32_t result;
  if (!s)
    return 5;
  if (!b || b->size < sizeof(*b))
    return CC_INVALID;
  if (!(b->status & 0x20000))
    return 514;
  if (b->reserved[0] != (uint32_t)h || b->src_len > b->reserved[1] ||
      b->dst_len > b->reserved[2])
    return CC_INVALID;
  b->status &= ~0x10000u;
  result = cc_audio((const struct cc_wave *)s->format, b->src, b->src_len,
                    b->dst, b->dst_len, &b->src_used, &b->dst_used, flags);
  if (!result)
    b->status |= 0x10000;
  return result;
}
uint32_t W32API acmDriverOpen(uintptr_t *out, uintptr_t id, uint32_t flags) {
  if (!out || flags)
    return CC_INVALID;
  *out = 0;
  if (id != 1 && id != 17 && id != 2)
    return 5;
  *out = id;
  return 0;
}
uint32_t W32API acmDriverClose(uintptr_t h, uint32_t flags) {
  if (h != 1 && h != 17 && h != 2)
    return 5;
  return flags ? CC_INVALID : 0;
}
uint32_t W32API acmDriverID(uintptr_t h, uintptr_t *out, uint32_t flags) {
  if (!out || flags)
    return CC_INVALID;
  if (h != 1 && h != 17 && h != 2)
    return 5;
  *out = h;
  return 0;
}
uint32_t W32API acmDriverEnum(int(W32API *callback)(uintptr_t, uintptr_t,
                                                    uint32_t),
                              uintptr_t user, uint32_t flags) {
  if (!callback || flags & ~0x40000000u)
    return CC_INVALID;
  if (callback(17, user, 1) && callback(2, user, 1))
    callback(1, user, 2);
  return 0;
}
intptr_t cc_acm_driver(uint32_t tag, uint32_t message, uintptr_t p1,
                       uintptr_t p2) {
  struct cc_drvstream *s = (void *)p1;
  uint32_t samples;
  if (message == 1 || message == 2 || message == 4 || message == 5 ||
      message == 6 || message == 7 || message == 8)
    return 1;
  if (message == 3)
    return tag;
  if (message == 0x604c) {
    if (!s || s->size < sizeof(*s) || !s->src || s->src->tag != tag ||
        !s->dst || s->filter || s->open & ~1u)
      return CC_INVALID;
    return cc_wave_validate(s->src, &samples) || s->dst->tag != 1 ||
                   s->dst->bits != (tag == 1 ? s->src->bits : 16) ||
                   s->dst->channels != s->src->channels ||
                   s->dst->rate != s->src->rate ||
                   s->dst->align != s->src->channels * (s->dst->bits / 8)
               ? CC_NOTPOSSIBLE
               : 0;
  }
  if (message == 0x604d || message == 0x6050 || message == 0x6051 ||
      message == 0x6052)
    return 0;
  if (message == 0x601b) {
    struct cc_suggest *f = (void *)p1;
    if (!f || f->size < sizeof(*f) || !f->src || f->src->tag != tag ||
        f->src_size < 18u + f->src->extra)
      return CC_INVALID;
    return cc_suggest_format(f->src, f->dst, f->dst_size, f->flags);
  }
  if (message == 0x604e) {
    struct cc_drvsize *size = (void *)p2;
    if (!s || !s->src || s->src->tag != tag || !size ||
        size->size < sizeof(*size))
      return CC_INVALID;
    return cc_stream_size(s->src, size->flags ? size->dst : size->src,
                          size->flags, size->flags ? &size->src : &size->dst);
  }
  if (message == 0x604f) {
    struct cc_acm_header *b = (void *)p2;
    uint32_t flags;
    if (!s || !s->src || s->src->tag != tag || !b ||
        b->size < offsetof(struct cc_acm_header, reserved) + 4)
      return CC_INVALID;
    /* ACMDRVSTREAMHEADER has fdwConvert immediately after dwDstUser. */
    cc_copy(&flags,
            (const uint8_t *)b + offsetof(struct cc_acm_header, reserved), 4);
    return cc_audio(s->src, b->src, b->src_len, b->dst, b->dst_len,
                    &b->src_used, &b->dst_used, flags);
  }
  return CC_NOTSUPPORTED;
}
intptr_t W32API acmDriverMessage(uintptr_t h, uint32_t message, uintptr_t p1,
                                 uintptr_t p2) {
  if (h != 1 && h != 17 && h != 2)
    return 5;
  return cc_acm_driver((uint32_t)h, message, p1, p2);
}
#if !defined(CC_AUDIO_DRIVER) && !defined(CC_VIDEO_DRIVER)
#define CC_AUDIO_DRIVER 1
#endif
#ifdef CC_AUDIO_DRIVER
intptr_t W32API DriverProc(uintptr_t id, uintptr_t driver, uint32_t message,
                           uintptr_t p1, uintptr_t p2) {
  (void)id;
  (void)driver;
  return cc_acm_driver(CC_AUDIO_DRIVER, message, p1, p2);
}
#endif
uint32_t W32API acmMetrics(uintptr_t driver, uint32_t metric, uint32_t *out) {
  if (!out)
    return CC_INVALID;
  if (driver && driver != 1 && driver != 2 && driver != 17)
    return 5;
  switch (metric) {
  case 1:
    *out = driver ? 1 : 3;
    return 0;
  case 2:
    *out = driver ? (driver == 1 ? 0 : 1) : 2;
    return 0;
  case 3:
    *out = driver ? (driver == 1 ? 1 : 0) : 1;
    return 0;
  case 4:
  case 5:
  case 6:
  case 20:
  case 21:
  case 22:
  case 23:
  case 24:
    *out = 0;
    return 0;
  case 50:
    *out = driver == 1 ? 18 : 50;
    return 0;
  case 51:
    *out = 0;
    return 0;
  case 100:
    if (!driver)
      return CC_INVALID;
    *out = driver == 1 ? 2 : 1;
    return 0;
  case 101:
    if (!driver)
      return CC_INVALID;
    *out = driver == 17 ? 1 : (driver == 2 ? 2 : 3);
    return 0;
  default:
    return CC_NOTSUPPORTED;
  }
}
