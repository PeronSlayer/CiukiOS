/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Reference algorithm notices: Copyright 2001 Dr. Tim Ferguson;
 * Portions Copyright 2003 Mike McCormack for CodeWeavers;
 * Copyright (C) 2003 the ffmpeg project; Portions Copyright (C) 2004
 * Mike McCormack for CodeWeavers; Copyright 2002-2003 Michael Günnewig.
 * Copyright 2026 CiukiOS contributors (bounded implementation).
 * See third_party/wine/COPYING.LIB and SOURCE-OFFER.txt.
 * Bounded RLE, Video 1 and Cinepak. See milestone C source references.
 */
#include "codec.h"
#define CVID CC_FOUR('c', 'v', 'i', 'd')
#define MSVC CC_FOUR('m', 's', 'v', 'c')
#define RLE CC_FOUR('m', 'r', 'l', 'e')
static uint32_t height(const struct cc_bitmap *b) {
  return b->height < 0 ? (uint32_t)-b->height : (uint32_t)b->height;
}
static uint32_t stride(const struct cc_bitmap *b) {
  return (((uint32_t)b->width * b->bits + 31) / 32) * 4;
}
int cc_video_query(uint32_t kind, const struct cc_bitmap *in,
                   const struct cc_bitmap *out) {
  if (!in || in->size < 40 || in->planes != 1 || in->width <= 0 ||
      in->width > 2048 || !in->height || in->height < -2048 ||
      in->height > 2048)
    return CC_BADFORMAT;
  if (kind == RLE) {
    if (!((in->compression == 1 && in->bits == 8) ||
          (in->compression == 2 && in->bits == 4)) ||
        in->height < 0)
      return CC_BADFORMAT;
  } else if (kind == MSVC) {
    if ((in->compression | 0x20202020u) != MSVC &&
        (in->compression | 0x20202020u) != CC_FOUR('c', 'r', 'a', 'm') &&
        (in->compression | 0x20202020u) != CC_FOUR('w', 'h', 'a', 'm'))
      return CC_BADFORMAT;
    if (in->bits != 8 && in->bits != 16)
      return CC_BADFORMAT;
  } else if (kind != CVID || (in->compression | 0x20202020u) != CVID)
    return CC_BADFORMAT;
  if (kind != RLE && ((in->width & 3) || (height(in) & 3)))
    return CC_BADFORMAT;
  if (!out)
    return 0;
  if (out->size < 40 || out->planes != 1 || out->width != in->width ||
      out->height == (-2147483647 - 1) || height(out) != height(in) ||
      out->compression ||
      out->bits != (kind == CVID ? 24 : (kind == RLE ? 8 : in->bits)))
    return CC_BADFORMAT;
  if (out->bytes && out->bytes < stride(out) * height(out))
    return CC_BADSIZE;
  return 0;
}
static void pixel(uint8_t *out, const struct cc_bitmap *b, uint32_t x,
                  uint32_t y, uint32_t v, int from_top) {
  uint32_t row = (from_top ? (b->height < 0 ? y : height(b) - 1 - y)
                           : (b->height > 0 ? y : height(b) - 1 - y));
  uint8_t *d = out + row * stride(b) + x * (b->bits / 8);
  *d = (uint8_t)v;
  if (b->bits >= 16)
    d[1] = (uint8_t)(v >> 8);
  if (b->bits == 24)
    d[2] = (uint8_t)(v >> 16);
}
static int rle(const struct cc_bitmap *in, const uint8_t *s,
               const struct cc_bitmap *out, uint8_t *d) {
  uint32_t p = 0, x = 0, y = 0, w = (uint32_t)in->width, h = height(in),
           n = in->bytes;
  while (p < n) {
    uint32_t count, code, k, bytes;
    if (n - p < 2)
      return CC_BADSIZE;
    count = s[p++];
    code = s[p++];
    if (!count) {
      if (code == 0) {
        if (y >= h)
          return CC_BADFORMAT;
        x = 0;
        y++;
        continue;
      }
      /* fx-hlrun: the end-of-bitmap code ends the frame; biSizeImage may
       * be larger (comctl32's animation control passes the stream format,
       * whose size is the largest frame). Wine 10.0 msrle32.c
       * MSRLE32_DecompressRLE4/RLE8 stop at it without a size check. */
      if (code == 1)
        return 0;
      if (code == 2) {
        if (n - p < 2)
          return CC_BADSIZE;
        if (s[p] > w - x || y >= h || s[p + 1] >= h - y)
          return CC_BADFORMAT;
        x += s[p++];
        y += s[p++];
        continue;
      }
      count = code;
      bytes = in->bits == 8 ? count : (count + 1) / 2;
      if (bytes + (bytes & 1) > n - p)
        return CC_BADSIZE;
    } else
      bytes = 0;
    if (y >= h || count > w - x)
      return CC_BADFORMAT;
    for (k = 0; k < count; k++) {
      uint32_t v =
          bytes ? (in->bits == 8 ? s[p + k]
                                 : (s[p + k / 2] >> ((k & 1) ? 0 : 4)) & 15)
                : (in->bits == 8 ? code : (code >> ((k & 1) ? 0 : 4)) & 15);
      pixel(d, out, x++, y, v, 0);
    }
    p += bytes + (bytes & 1);
  }
  return CC_BADSIZE;
}
static int video1(const struct cc_bitmap *in, const uint8_t *s,
                  const struct cc_bitmap *out, uint8_t *d) {
  uint32_t block = 0, blocks = (uint32_t)in->width / 4 * height(in) / 4, p = 0,
           n = in->bytes;
  while (block < blocks) {
    uint32_t a, b, flags, colors[8], count = 0, x, y, k;
    if (n - p < 2)
      return CC_BADSIZE;
    a = s[p++];
    b = s[p++];
    flags = a + (b << 8);
    if ((b & 0xfc) == 0x84) {
      count = (b - 0x84) * 256 + a;
      if (!count || count > blocks - block)
        return CC_BADFORMAT;
      block += count;
      continue;
    }
    if (b < 0x80) {
      count = 2;
      if (n - p < (in->bits == 8 ? 2u : 4u))
        return CC_BADSIZE;
      for (k = 0; k < 2; k++) {
        colors[k] = in->bits == 8 ? s[p++] : cc_le16(s + p);
        if (in->bits == 16)
          p += 2;
      }
      if (in->bits == 16 && colors[0] & 0x8000)
        count = 8;
    } else if (in->bits == 8 && b >= 0x90)
      count = 8;
    if (count == 8) {
      k = (b < 0x80 ? 2 : 0);
      if (n - p < (8 - k) * (in->bits / 8))
        return CC_BADSIZE;
      for (; k < 8; k++) {
        colors[k] = in->bits == 8 ? s[p++] : cc_le16(s + p);
        if (in->bits == 16)
          p += 2;
      }
    }
    if (!count)
      colors[0] = in->bits == 8 ? a : flags;
    for (y = 0; y < 4; y++)
      for (x = 0; x < 4; x++) {
        uint32_t ci = count ? ((flags & 1) ^ 1) : 0;
        if (count == 8)
          ci += (y & 2) * 2 + (x & 2);
        pixel(d, out, (block % ((uint32_t)in->width / 4)) * 4 + x,
              (block / ((uint32_t)in->width / 4)) * 4 + y,
              colors[ci] & (in->bits == 16 ? 0x7fff : 255), 0);
        flags >>= 1;
      }
    block++;
  }
  /* Optional documented end marker, never arbitrary overlong data. */
  return p == n || (n - p == 2 && s[p] == 0 && s[p + 1] == 0) ? 0 : CC_BADSIZE;
}
static void book_read(struct cc_book *b, const uint8_t *p, int gray) {
  int u = gray ? 0 : (int8_t)p[4], v = gray ? 0 : (int8_t)p[5];
  uint32_t j;
  /* Arithmetic floor for signed chroma, independent of host right shifts. */
  int half = u + 1;
  half = half >= 0 ? half / 2 : -((-half + 1) / 2);
  for (j = 0; j < 4; j++) {
    b->bgr[j][0] = (uint8_t)cc_clip(p[j] + 2 * u, 0, 255);
    b->bgr[j][1] = (uint8_t)cc_clip(p[j] - half - v, 0, 255);
    b->bgr[j][2] = (uint8_t)cc_clip(p[j] + 2 * v, 0, 255);
  }
  b->valid = 1;
}
struct bits {
  const uint8_t *s;
  uint32_t p, n, word, left;
};
static int bit(struct bits *b) {
  int result;
  if (!b->left) {
    if (b->n - b->p < 4)
      return -1;
    b->word = cc_be32(b->s + b->p);
    b->p += 4;
    b->left = 32;
  }
  result = (int)(b->word >> 31);
  b->word <<= 1;
  b->left--;
  return result;
}
static int cinepak(struct cc_cinepak *ctx, const struct cc_bitmap *in,
                   const uint8_t *s, const struct cc_bitmap *out, uint8_t *d) {
  uint32_t n = in->bytes, p = 10, strips, i, last_y = 0;
  if (!ctx || n < 10 || cc_be24(s + 1) != n ||
      cc_be16(s + 4) != (uint32_t)in->width || cc_be16(s + 6) != height(in))
    return CC_BADSIZE;
  strips = cc_be16(s + 8);
  if (!strips || strips > 32 || (s[0] & ~1u))
    return CC_BADFORMAT;
  for (i = 0; i < strips; i++) {
    uint32_t end, y0, x0, y1, x1, b = 0, total, stripid;
    if (n - p < 12)
      return CC_BADSIZE;
    stripid = cc_be16(s + p);
    end = cc_be16(s + p + 2);
    y0 = cc_be16(s + p + 4);
    x0 = cc_be16(s + p + 6);
    y1 = cc_be16(s + p + 8);
    x1 = cc_be16(s + p + 10);
    if (end < 12 || end > n - p || (stripid != 0x1000 && stripid != 0x1100))
      return CC_BADSIZE;
    end += p;
    p += 12;
    /* AVI strips use relative y0=0 and a strip height in y1. */
    if (y0 == 0) {
      y0 = last_y;
      y1 += last_y;
    }
    if (x0 || x1 != (uint32_t)in->width || y0 != last_y || y1 <= y0 ||
        y1 > height(in) || ((y1 - y0) & 3))
      return CC_BADFORMAT;
    if (i && !(s[0] & 1))
      cc_copy(ctx->book[i], ctx->book[i - 1], sizeof(ctx->book[i]));
    total = (x1 / 4) * ((y1 - y0) / 4);
    while (p < end) {
      uint32_t id, size, e;
      if (end - p < 4)
        return CC_BADSIZE;
      id = cc_be16(s + p);
      size = cc_be16(s + p + 2);
      if (size < 4 || size > end - p)
        return CC_BADSIZE;
      e = p + size;
      p += 4;
      if (id >= 0x2000 && id <= 0x2700 && !(id & 255)) {
        uint32_t index = 0, mode = (id >> 8) & 1, gray = (id & 0x400) != 0,
                 unit = gray ? 4 : 6, which = (id & 0x200) ? 1 : 0;
        if (!mode) {
          if ((e - p) % unit || (e - p) / unit > 256)
            return CC_BADFORMAT;
          while (p < e) {
            book_read(&ctx->book[i][which][index++], s + p, gray);
            p += unit;
          }
        } else {
          while (p < e) {
            uint32_t flags, k;
            if (e - p < 4 || index >= 256)
              return CC_BADSIZE;
            flags = cc_be32(s + p);
            p += 4;
            for (k = 0; k < 32; k++, index++) {
              if (flags & (0x80000000u >> k)) {
                if (e - p < unit)
                  return CC_BADSIZE;
                book_read(&ctx->book[i][which][index], s + p, gray);
                p += unit;
              }
            }
          }
        }
      } else if (id == 0x3000 || id == 0x3100 || id == 0x3200) {
        struct bits bits = {s, p, e, 0, 0};
        while (b < total) {
          int v4 = 0, changed = 1;
          uint32_t ids[4], j, x, y;
          if (id == 0x3100) {
            changed = bit(&bits);
            if (changed < 0)
              return CC_BADSIZE;
          }
          if (changed && id != 0x3200) {
            v4 = bit(&bits);
            if (v4 < 0)
              return CC_BADSIZE;
          }
          if (changed) {
            if (bits.n - bits.p < (v4 ? 4u : 1u))
              return CC_BADSIZE;
            for (j = 0; j < (v4 ? 4u : 1u); j++) {
              ids[j] = s[bits.p++];
              if (!ctx->book[i][v4 ? 0 : 1][ids[j]].valid)
                return CC_BADFORMAT;
            }
            for (y = 0; y < 4; y++)
              for (x = 0; x < 4; x++) {
                struct cc_book *cb =
                    &ctx->book[i][v4 ? 0 : 1]
                              [ids[v4 ? (y / 2) * 2 + x / 2 : 0]];
                uint32_t k = v4 ? (y & 1) * 2 + (x & 1) : (y / 2) * 2 + x / 2;
                uint8_t *rgb = cb->bgr[k];
                pixel(d, out, (b % (x1 / 4)) * 4 + x,
                      y0 + (b / (x1 / 4)) * 4 + y,
                      (uint32_t)rgb[0] | ((uint32_t)rgb[1] << 8) |
                          ((uint32_t)rgb[2] << 16),
                      1);
              }
          }
          b++;
        }
        if (bits.p != e)
          return CC_BADSIZE;
        p = e;
      } else
        return CC_BADFORMAT;
      if (p != e)
        return CC_BADSIZE;
    }
    if (b != total)
      return CC_BADSIZE;
    last_y = y1;
  }
  ctx->strips = strips;
  return p == n && last_y == height(in) ? 0 : CC_BADSIZE;
}
int cc_video(uint32_t kind, struct cc_cinepak *ctx, const struct cc_bitmap *in,
             const uint8_t *src, const struct cc_bitmap *out, uint8_t *dst) {
  int result = cc_video_query(kind, in, out);
  if (result)
    return result;
  if (!out || !dst || !src || !in->bytes || in->bytes > 16u * 1024u * 1024u)
    return CC_BADPARAM;
  if (kind == RLE)
    return rle(in, src, out, dst);
  if (kind == MSVC)
    return video1(in, src, out, dst);
  return cinepak(ctx, in, src, out, dst);
}
