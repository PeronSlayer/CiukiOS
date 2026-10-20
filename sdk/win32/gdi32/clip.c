/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"
int gdi_visible(void *opaque, int x, int y) {
  struct dc *d = opaque;
  struct w32_dib *s = gdi_target(d);
  return s && x >= 0 && y >= 0 && (DWORD)x < s->width && (DWORD)y < s->height &&
         (!d->s.has_clip || gdi_region_point(&d->s.clip, x, y)) &&
         (!d->has_visible || gdi_region_point(&d->visible, x, y)) &&
         (!d->has_shape || gdi_region_point(&d->shape, x, y));
}
BOOL W32API w32_gdi_set_visible_region(HDC h, HRGN region) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  struct object *r = region ? gdi_object(region, GDI_REGION) : 0;
  if (!d || (region && !r))
    return 0;
  if (r && !gdi_region_copy(&d->visible, &r->u.region))
    return 0;
  if (!r)
    gdi_region_free(&d->visible);
  d->has_visible = region != 0;
  return 1;
}
BOOL W32API w32_gdi_window_shape(HDC h, HRGN region) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  struct object *r = region ? gdi_object(region, GDI_REGION) : 0;
  if (!d || (region && !r))
    return 0;
  if (r && !gdi_region_copy(&d->shape, &r->u.region))
    return 0;
  if (!r)
    gdi_region_free(&d->shape);
  d->has_shape = region != 0;
  return 1;
}
static int select_device_region(struct dc *d, const struct region *r,
                                int mode) {
  if (!r) {
    gdi_region_free(&d->s.clip);
    d->s.has_clip = 0;
    return SIMPLEREGION;
  }
  if (mode == RGN_COPY) {
    if (!gdi_region_copy(&d->s.clip, r))
      return ERROR;
  } else {
    struct w32_dib *s = gdi_target(d);
    RECT bounds = {0, 0, (LONG)s->width, (LONG)s->height};
    struct region all = {&bounds, 1, 0};
    if (!gdi_region_combine(&d->s.clip, d->s.has_clip ? &d->s.clip : &all, r,
                            mode))
      return ERROR;
  }
  d->s.has_clip = 1;
  return gdi_region_type(&d->s.clip);
}
int W32API ExtSelectClipRgn(HDC h, HRGN region, int mode) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  struct object *r = region ? gdi_object(region, GDI_REGION) : 0;
  if (!d || mode < 1 || mode > 5 || (region && !r) || (!r && mode != RGN_COPY))
    return ERROR;
  if (!r)
    return select_device_region(d, 0, mode);
  struct region translated = {0};
  if (!gdi_region_copy(&translated, &r->u.region) ||
      !gdi_region_shift(&translated, d->origin.x, d->origin.y)) {
    gdi_region_free(&translated);
    return ERROR;
  }
  int result = select_device_region(d, &translated, mode);
  gdi_region_free(&translated);
  return result;
}
int W32API SelectClipRgn(HDC h, HRGN r) {
  return ExtSelectClipRgn(h, r, RGN_COPY);
}
static int clip_rect(HDC h, int l, int t, int r, int b, int mode) {
  struct dc *d = gdi_dc(h);
  if (!d)
    return ERROR;
  RECT q = gdi_rect(d, (RECT){l, t, r, b});
  struct region region = {&q, 1, 0};
  return select_device_region(d, &region, mode);
}
/* f5-20B: on a metafile DC the clipping rectangle is recorded. */
static int record_clip(HDC h, int l, int t, int r, int b, unsigned function) {
  struct dc *d = gdi_dc(h);
  if (!d || !d->record)
    return 0;
  int16_t v[4] = {(int16_t)b, (int16_t)r, (int16_t)t, (int16_t)l};
  return gdi_record(d, function, v, 4);
}
int W32API IntersectClipRect(HDC h, int l, int t, int r, int b) {
  GDI_GUARD;
  if (record_clip(h, l, t, r, b, 0x0416u))
    return SIMPLEREGION;
  return clip_rect(h, l, t, r, b, RGN_AND);
}
int W32API ExcludeClipRect(HDC h, int l, int t, int r, int b) {
  GDI_GUARD;
  if (record_clip(h, l, t, r, b, 0x0415u))
    return SIMPLEREGION;
  return clip_rect(h, l, t, r, b, RGN_DIFF);
}
int W32API OffsetClipRgn(HDC h, int x, int y) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  if (!d)
    return ERROR;
  if (!d->s.has_clip)
    return SIMPLEREGION;
  POINT p[2] = {{0, 0}, {x, y}};
  if (!gdi_transform(d, p, 2, 0))
    return ERROR;
  int dx = p[1].x - p[0].x, dy = p[1].y - p[0].y;
  RECT box = gdi_region_box(&d->s.clip);
  if ((int64_t)box.left + dx < -GDI_LIMIT ||
      (int64_t)box.right + dx > GDI_LIMIT ||
      (int64_t)box.top + dy < -GDI_LIMIT ||
      (int64_t)box.bottom + dy > GDI_LIMIT)
    return ERROR;
  for (unsigned i = 0; i < d->s.clip.count; i++) {
    RECT *q = &d->s.clip.rects[i];
    q->left += dx;
    q->right += dx;
    q->top += dy;
    q->bottom += dy;
  }
  return gdi_region_type(&d->s.clip);
}
static int effective(struct dc *d, struct region *out) {
  struct w32_dib *s = gdi_target(d);
  if (!s)
    return 0;
  if (!gdi_region_add(out, (RECT){0, 0, (LONG)s->width, (LONG)s->height}))
    return 0;
  if (d->s.has_clip && !gdi_region_combine(out, out, &d->s.clip, RGN_AND))
    return 0;
  if (d->has_visible && !gdi_region_combine(out, out, &d->visible, RGN_AND))
    return 0;
  if (d->has_shape && !gdi_region_combine(out, out, &d->shape, RGN_AND))
    return 0;
  return 1;
}
int W32API GetClipBox(HDC h, RECT *box) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  struct region out = {0};
  if (!d || !box)
    return ERROR;
  if (!effective(d, &out)) {
    gdi_region_free(&out);
    return ERROR;
  }
  *box = gdi_region_box(&out);
  int type = gdi_region_type(&out);
  gdi_region_free(&out);
  POINT p[2] = {{box->left, box->top}, {box->right, box->bottom}};
  if (!gdi_transform(d, p, 2, 1))
    return ERROR;
  *box = (RECT){
      p[0].x < p[1].x ? p[0].x : p[1].x, p[0].y < p[1].y ? p[0].y : p[1].y,
      p[0].x > p[1].x ? p[0].x : p[1].x, p[0].y > p[1].y ? p[0].y : p[1].y};
  return type;
}
int W32API GetClipRgn(HDC h, HRGN region) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  struct object *r = gdi_object(region, GDI_REGION);
  if (!d || !r)
    return -1;
  if (!d->s.has_clip)
    return 0;
  return gdi_region_copy(&r->u.region, &d->s.clip) &&
                 gdi_region_shift(&r->u.region, -d->origin.x, -d->origin.y)
             ? 1
             : -1;
}
BOOL W32API PtVisible(HDC h, int x, int y) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  POINT p = {x, y};
  return d && gdi_transform(d, &p, 1, 0) && gdi_visible(d, p.x, p.y);
}
BOOL W32API RectVisible(HDC h, const RECT *r) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  if (!d || !r)
    return 0;
  struct region clip = {0}, out = {0};
  RECT q = gdi_rect(d, *r);
  struct region in = {&q, 1, 0};
  int good = effective(d, &clip) &&
             gdi_region_combine(&out, &clip, &in, RGN_AND) && out.count;
  gdi_region_free(&clip);
  gdi_region_free(&out);
  return good;
}
static int paint_region(HDC h, HRGN region, HBRUSH brush, int invert, int fw,
                        int fh) {
  struct dc *d = gdi_dc(h);
  struct object *r = gdi_object(region, GDI_REGION);
  if (!d || !r)
    return 0;
  if (!fw && !fh) {
    for (unsigned i = 0; i < r->u.region.count; i++)
      if (!gdi_fill(d, gdi_rect(d, r->u.region.rects[i]), brush, invert))
        return 0;
    return 1;
  }
  if (fw < 0 || fh < 0 || fw > 8192 || fh > 8192)
    return 0;
  /* Erosion by a rectangular brush: a point survives only if its entire
   * neighbourhood is contained, including concave/diagonal boundaries. */
  for (unsigned i = 0; i < r->u.region.count; i++) {
    RECT q = r->u.region.rects[i];
    for (int y = q.top; y < q.bottom; y++)
      for (int x = q.left; x < q.right; x++) {
        int boundary = 0;
        for (int yy = y - fh; yy <= y + fh && !boundary; yy++) {
          int cursor = x - fw;
          for (unsigned j = 0; j < r->u.region.count && cursor <= x + fw; j++) {
            RECT a = r->u.region.rects[j];
            if (yy >= a.top && yy < a.bottom && cursor >= a.left &&
                cursor < a.right)
              cursor = a.right;
          }
          if (cursor <= x + fw)
            boundary = 1;
        }
        if (boundary &&
            !gdi_fill(d, gdi_rect(d, (RECT){x, y, x + 1, y + 1}), brush, 0))
          return 0;
      }
  }
  return 1;
}
BOOL W32API FillRgn(HDC h, HRGN r, HBRUSH b) {
  GDI_GUARD;
  return paint_region(h, r, b, 0, 0, 0);
}
BOOL W32API FrameRgn(HDC h, HRGN r, HBRUSH b, int w, int height) {
  GDI_GUARD;
  return paint_region(h, r, b, 0, w, height);
}
BOOL W32API InvertRgn(HDC h, HRGN r) {
  GDI_GUARD;
  return paint_region(h, r, 0, 1, 0, 0);
}
BOOL W32API PaintRgn(HDC h, HRGN r) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  return d && paint_region(h, r, d->s.brush, 0, 0, 0);
}
