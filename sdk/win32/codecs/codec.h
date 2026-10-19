/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef CIUKI_CODEC_H
#define CIUKI_CODEC_H
#include <w32/base.h>
#define CC_OK 0u
#define CC_INVALID 11u
#define CC_NOTSUPPORTED 8u
#define CC_NOTPOSSIBLE 512u
#define CC_BADFORMAT (-2)
#define CC_BADPARAM (-6)
#define CC_BADHANDLE (-8)
#define CC_BADSIZE (-7)
#define CC_ERROR (-100)
#define CC_FOUR(a, b, c, d)                                                    \
  ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) |              \
   ((uint32_t)(d) << 24))
#pragma pack(push, 1)
struct cc_wave {
  uint16_t tag, channels;
  uint32_t rate, average;
  uint16_t align, bits, extra;
};
struct cc_acm_header {
  uint32_t size, status;
  uintptr_t user;
  uint8_t *src;
  uint32_t src_len, src_used;
  uintptr_t src_user;
  uint8_t *dst;
  uint32_t dst_len, dst_used;
  uintptr_t dst_user;
  uint32_t reserved[10];
};
#pragma pack(pop)
struct cc_bitmap {
  uint32_t size;
  int32_t width, height;
  uint16_t planes, bits;
  uint32_t compression, bytes;
  int32_t xppm, yppm;
  uint32_t colors, important;
};
struct cc_decompress {
  uint32_t flags;
  struct cc_bitmap *in;
  const uint8_t *src;
  struct cc_bitmap *out;
  uint8_t *dst;
  uint32_t ckid;
};
struct cc_icinfo {
  uint32_t size, type, handler, flags, version, icm_version;
  WCHAR name[16], description[128], driver[128];
};
struct cc_drvstream {
  uint32_t size;
  struct cc_wave *src, *dst;
  void *filter;
  uintptr_t callback, instance;
  uint32_t open, flags;
  uintptr_t driver, stream;
};
struct cc_drvsize {
  uint32_t size, flags, src, dst;
};
struct cc_suggest {
  uint32_t size, flags;
  struct cc_wave *src;
  uint32_t src_size;
  struct cc_wave *dst;
  uint32_t dst_size;
};
struct cc_book {
  uint8_t bgr[4][3], valid;
};
struct cc_cinepak {
  struct cc_book book[32][2][256];
  uint32_t strips;
};
static inline uint16_t cc_le16(const uint8_t *p) {
  return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static inline uint32_t cc_be16(const uint8_t *p) {
  return ((uint32_t)p[0] << 8) | p[1];
}
static inline uint32_t cc_be24(const uint8_t *p) {
  return ((uint32_t)p[0] << 16) | cc_be16(p + 1);
}
static inline uint32_t cc_be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | cc_be16(p + 2);
}
static inline int cc_clip(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}
static inline void cc_copy(void *to, const void *from, uint32_t n) {
  uint8_t *d = to;
  const uint8_t *s = from;
  while (n--)
    *d++ = *s++;
}
static inline void cc_zero(void *to, uint32_t n) {
  uint8_t *d = to;
  while (n--)
    *d++ = 0;
}
uint32_t cc_wave_validate(const struct cc_wave *, uint32_t *);
uint32_t cc_audio(const struct cc_wave *, const uint8_t *, uint32_t, uint8_t *,
                  uint32_t, uint32_t *, uint32_t *, uint32_t);
uint32_t cc_suggest_format(const struct cc_wave *, struct cc_wave *, uint32_t,
                           uint32_t);
uint32_t cc_stream_size(const struct cc_wave *, uint32_t, uint32_t, uint32_t *);
int cc_video_query(uint32_t, const struct cc_bitmap *,
                   const struct cc_bitmap *);
int cc_video(uint32_t, struct cc_cinepak *, const struct cc_bitmap *,
             const uint8_t *, const struct cc_bitmap *, uint8_t *);
#endif
