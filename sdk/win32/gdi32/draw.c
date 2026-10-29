/* Scalar primitives, cosmetic strokes and 8x8 brushes.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"
HPEN W32API CreatePen(int style, int width, COLORREF color) {
  GDI_GUARD;
  if (style < 0 || style > PS_INSIDEFRAME || width < -256 || width > 256)
    return 0;
  HPEN h;
  struct object *o = gdi_new(GDI_PEN, &h);
  if (!o)
    return 0;
  o->u.pen = (LOGPEN){(DWORD)style, {width < 0 ? -width : width, 0}, color};
  return h;
}
HPEN W32API CreatePenIndirect(const LOGPEN *p) {
  return p ? CreatePen((int)p->lopnStyle, p->lopnWidth.x, p->lopnColor) : 0;
}
HBRUSH W32API CreateSolidBrush(COLORREF color) {
  GDI_GUARD;
  HBRUSH h;
  struct object *o = gdi_new(GDI_BRUSH, &h);
  if (!o)
    return 0;
  o->u.brush.log = (LOGBRUSH){BS_SOLID, color, 0};
  return h;
}
HBRUSH W32API CreateHatchBrush(int hatch, COLORREF color) {
  GDI_GUARD;
  if (hatch < 0 || hatch > 5)
    return 0;
  HBRUSH h = CreateSolidBrush(color);
  struct object *o = gdi_object(h, GDI_BRUSH);
  if (o) {
    o->u.brush.log.lbStyle = BS_HATCHED;
    o->u.brush.log.lbHatch = (uintptr_t)hatch;
  }
  return h;
}
HBRUSH W32API CreatePatternBrush(HBITMAP bitmap) {
  GDI_GUARD;
  struct object *b = gdi_object(bitmap, GDI_BITMAP);
  if (!b)
    return 0;
  HBRUSH h = CreateSolidBrush(0);
  struct object *o = gdi_object(h, GDI_BRUSH);
  if (!o)
    return 0;
  struct brush *p = &o->u.brush;
  p->log.lbStyle = BS_PATTERN;
  p->pattern = (struct w32_dib){.bits = p->pixels,
                                .width = 8,
                                .height = 8,
                                .pitch = 32,
                                .size = 256,
                                .bpp = 32,
                                .top_down = 1,
                                .masks = {0xff0000, 0xff00, 0xff}};
  if (b->u.bitmap.mono) {
    p->pattern.bpp = 1;
    p->pattern.pitch = 1;
    p->pattern.size = 8;
    p->pattern.color_count = 2;
    p->pattern.colors[1] = 0xffffff;
  }
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 8; x++) {
      struct w32_dib *s = &b->u.bitmap.dib;
      DWORD c = p->pattern.bpp == 1
                    ? w32_dib_raw(s, x % (int)s->width, y % (int)s->height)
                    : w32_dib_rgb(s, x % (int)s->width, y % (int)s->height);
      w32_dib_put(&p->pattern, x, y, c);
    }
  return h;
}
HBRUSH W32API CreateBrushIndirect(const LOGBRUSH *b) {
  if (!b)
    return 0;
  if (b->lbStyle == BS_SOLID)
    return CreateSolidBrush(b->lbColor);
  if (b->lbStyle == BS_NULL) {
    HBRUSH h = CreateSolidBrush(0);
    GDI_GUARD;
    struct object *o = gdi_object(h, GDI_BRUSH);
    if (o)
      o->u.brush.log.lbStyle = BS_NULL;
    return h;
  }
  if (b->lbStyle == BS_HATCHED)
    return CreateHatchBrush((int)b->lbHatch, b->lbColor);
  if (b->lbStyle == BS_PATTERN)
    return CreatePatternBrush((HBITMAP)b->lbHatch);
  return 0;
}
int gdi_brush_pixel(struct dc *d, HBRUSH handle, int x, int y, DWORD *color) {
  struct object *o = gdi_object(handle, GDI_BRUSH);
  if (!o)
    return 0;
  struct brush *b = &o->u.brush;
  if (b->log.lbStyle == BS_NULL)
    return 0;
  *color = handle == GetStockObject(DC_BRUSH) ? d->s.dcbrush : b->log.lbColor;
  int xx = (int)(((int64_t)x - d->s.brush_org.x - d->origin.x) % 8),
      yy = (int)(((int64_t)y - d->s.brush_org.y - d->origin.y) % 8);
  if (xx < 0)
    xx += 8;
  if (yy < 0)
    yy += 8;
  if (b->log.lbStyle == BS_PATTERN) {
    if (b->pattern.bpp == 1)
      *color = w32_dib_raw(&b->pattern, xx, yy) ? d->s.bk : d->s.text;
    else
      *color = gdi_colorref(w32_dib_rgb(&b->pattern, xx, yy));
  }
  if (b->log.lbStyle == BS_HATCHED) {
    unsigned h = (unsigned)b->log.lbHatch;
    int set = h == 0   ? yy == 0
              : h == 1 ? xx == 0
              : h == 2 ? ((xx + yy) & 7) == 0
              : h == 3 ? xx == yy
              : h == 4 ? (xx == 0 || yy == 0)
                       : (((xx + yy) & 7) == 0 || xx == yy);
    if (!set) {
      if (d->s.bk_mode == TRANSPARENT)
        return 0;
      *color = d->s.bk;
    }
  }
  return 1;
}
void gdi_brush_pattern(struct dc *d, struct w32_dib *pattern, void *storage) {
  *pattern = *gdi_target(d);
  pattern->bits = storage;
  pattern->width = pattern->height = 8;
  pattern->top_down = 1;
  pattern->pitch = pattern->bpp;
  pattern->size = pattern->pitch * 8;
  w32_fill(storage, 0, 256);
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 8; x++) {
      DWORD c = 0;
      gdi_brush_pixel(d, d->s.brush, x + d->s.brush_org.x + d->origin.x,
                      y + d->s.brush_org.y + d->origin.y, &c);
      w32_dib_put(pattern, x, y, gdi_encode_color(d, c));
    }
}
static void paint(struct dc *d, int x, int y, COLORREF color, int invert,
                  int pen) {
  if (!gdi_visible(d, x, y))
    return;
  struct w32_dib *s = gdi_target(d);
  DWORD dst = w32_dib_raw(s, x, y);
  DWORD src = gdi_encode_color(d, color),
        value = invert ? ~dst
                : pen
                    ? w32_gdi_rop3((unsigned)(d->s.rop2 - 1) * 17, 0, src, dst)
                    : src;
  w32_dib_put(s, x, y, value);
}
int gdi_fill(struct dc *d, RECT r, HBRUSH b, int invert) {
  struct w32_dib *s = gdi_target(d);
  if (!s || (!invert && !gdi_object(b, GDI_BRUSH)))
    return 0;
  if (r.left < 0)
    r.left = 0;
  if (r.top < 0)
    r.top = 0;
  if (r.right > (LONG)s->width)
    r.right = (LONG)s->width;
  if (r.bottom > (LONG)s->height)
    r.bottom = (LONG)s->height;
  for (int y = r.top; y < r.bottom; y++)
    for (int x = r.left; x < r.right; x++) {
      DWORD c = 0;
      if (invert || gdi_brush_pixel(d, b, x, y, &c))
        paint(d, x, y, c, invert, 0);
    }
  gdi_damage(d, r);
  return 1;
}
int W32API FillRect(HDC h, const RECT *r, HBRUSH b) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  return d && r && gdi_fill(d, gdi_rect(d, *r), b, 0);
}
int W32API FrameRect(HDC h, const RECT *r, HBRUSH b) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  if (!d || !r)
    return 0;
  RECT q = gdi_rect(d, *r);
  if (q.left >= q.right || q.top >= q.bottom)
    return 1;
  return gdi_fill(d, (RECT){q.left, q.top, q.right, q.top + 1}, b, 0) &&
         (q.bottom - q.top <= 1 ||
          gdi_fill(d, (RECT){q.left, q.bottom - 1, q.right, q.bottom}, b, 0)) &&
         gdi_fill(d, (RECT){q.left, q.top + 1, q.left + 1, q.bottom - 1}, b,
                  0) &&
         (q.right - q.left <= 1 ||
          gdi_fill(d, (RECT){q.right - 1, q.top + 1, q.right, q.bottom - 1}, b,
                   0));
}
BOOL W32API InvertRect(HDC h, const RECT *r) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  return d && r && gdi_fill(d, gdi_rect(d, *r), 0, 1);
}
COLORREF W32API GetPixel(HDC h, int x, int y) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  POINT p = {x, y};
  if (!d || !gdi_transform(d, &p, 1, 0) || !gdi_visible(d, p.x, p.y))
    return CLR_INVALID;
  return gdi_colorref(w32_dib_rgb(gdi_target(d), p.x, p.y));
}
COLORREF W32API SetPixel(HDC h, int x, int y, COLORREF c) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  if (d && d->record) { /* f5-20B metafile DC */
    int16_t v[4] = {(int16_t)(c & 0xffff), (int16_t)(c >> 16), (int16_t)y,
                    (int16_t)x};
    gdi_record(d, 0x041fu, v, 4);
    return c;
  }
  POINT p = {x, y};
  if (!d || !gdi_transform(d, &p, 1, 0) || !gdi_visible(d, p.x, p.y))
    return CLR_INVALID;
  paint(d, p.x, p.y, c, 0, 0);
  gdi_damage(d, (RECT){p.x, p.y, p.x + 1, p.y + 1});
  return gdi_colorref(w32_dib_rgb(gdi_target(d), p.x, p.y));
}
BOOL W32API SetPixelV(HDC h, int x, int y, COLORREF c) {
  return SetPixel(h, x, y, c) != CLR_INVALID;
}
BOOL W32API MoveToEx(HDC h, int x, int y, POINT *old) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  if (!d || x < -GDI_LIMIT || x > GDI_LIMIT || y < -GDI_LIMIT || y > GDI_LIMIT)
    return 0;
  if (d->record) {
    int16_t v[2] = {(int16_t)y, (int16_t)x};
    gdi_record(d, 0x0214u, v, 2);
  }
  if (old)
    *old = d->s.current;
  d->s.current = (POINT){x, y};
  if (d->path && d->path->open)
    d->path->fresh = 1;
  return 1;
}
BOOL W32API GetCurrentPositionEx(HDC h, POINT *p) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  if (!d || !p)
    return 0;
  *p = d->s.current;
  return 1;
}
static int dash(unsigned style, unsigned step) {
  static const uint8_t lengths[5] = {1, 6, 2, 8, 10};
  static const uint16_t patterns[5] = {1, 7, 1, 0x17, 0x57};
  return style > 4 || ((patterns[style] >> (step % lengths[style])) & 1);
}
static int stroke(struct dc *d, const POINT *points, unsigned count,
                  int close) {
  struct object *o = gdi_object(d->s.pen, GDI_PEN);
  struct w32_dib *s = gdi_target(d);
  if (!o || !s || count < 2 || count > 8192)
    return 0;
  LOGPEN pen = o->u.pen;
  if (pen.lopnStyle == PS_NULL)
    return 1;
  int width = pen.lopnWidth.x ? pen.lopnWidth.x : 1;
  int radius = width / 2;
  RECT box = {GDI_LIMIT, GDI_LIMIT, -GDI_LIMIT, -GDI_LIMIT};
  for (unsigned i = 0; i < count; i++) {
    POINT p = points[i];
    if (p.x < -GDI_LIMIT || p.x > GDI_LIMIT || p.y < -GDI_LIMIT ||
        p.y > GDI_LIMIT)
      return 0;
    if (p.x - radius < box.left)
      box.left = p.x - radius;
    if (p.y - radius < box.top)
      box.top = p.y - radius;
    if (p.x - radius + width > box.right)
      box.right = p.x - radius + width;
    if (p.y - radius + width > box.bottom)
      box.bottom = p.y - radius + width;
  }
  if (box.left < 0)
    box.left = 0;
  if (box.top < 0)
    box.top = 0;
  if (box.right > (LONG)s->width)
    box.right = (LONG)s->width;
  if (box.bottom > (LONG)s->height)
    box.bottom = (LONG)s->height;
  if (box.right <= box.left || box.bottom <= box.top)
    return 1;
  unsigned pitch = (unsigned)(box.right - box.left),
           bytes = pitch * (unsigned)(box.bottom - box.top);
  uint8_t *mask = gdi_alloc(bytes);
  if (!mask)
    return 0;
  unsigned step = 0;
  for (unsigned i = 0; i < count - 1 + (unsigned)close; i++) {
    POINT a = points[i % count], b = points[(i + 1) % count];
    int dx = b.x - a.x, dy = b.y - a.y;
    int xs = dx < 0 ? -1 : 1, ys = dy < 0 ? -1 : 1;
    if (dx < 0)
      dx = -dx;
    if (dy < 0)
      dy = -dy;
    int error = dx - dy;
    while (a.x != b.x || a.y != b.y) {
      if (dash(pen.lopnStyle, step))
        for (int yy = a.y - radius; yy < a.y - radius + width; yy++)
          for (int xx = a.x - radius; xx < a.x - radius + width; xx++) {
            if (xx >= box.left && xx < box.right && yy >= box.top &&
                yy < box.bottom)
              mask[(unsigned)(yy - box.top) * pitch +
                   (unsigned)(xx - box.left)] = 1;
          }
      step++;
      int twice = 2 * error;
      if (twice > -dy) {
        error -= dy;
        a.x += xs;
      }
      if (twice < dx) {
        error += dx;
        a.y += ys;
      }
    }
  }
  if (close && !step)
    for (int yy = points[0].y - radius; yy < points[0].y - radius + width; yy++)
      for (int xx = points[0].x - radius; xx < points[0].x - radius + width;
           xx++)
        if (xx >= box.left && xx < box.right && yy >= box.top &&
            yy < box.bottom)
          mask[(unsigned)(yy - box.top) * pitch + (unsigned)(xx - box.left)] =
              1;
  COLORREF color =
      d->s.pen == GetStockObject(DC_PEN) ? d->s.dcpen : pen.lopnColor;
  for (int yy = box.top; yy < box.bottom; yy++)
    for (int xx = box.left; xx < box.right; xx++)
      if (mask[(unsigned)(yy - box.top) * pitch + (unsigned)(xx - box.left)])
        paint(d, xx, yy, color, 0, 1);
  gdi_free(mask, bytes);
  gdi_damage(d, box);
  return 1;
}
BOOL W32API LineTo(HDC h, int x, int y) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  if (!d)
    return 0;
  if (d->path && d->path->open) {
    POINT p = {x, y};
    if (!gdi_path_poly(h, &p, 1, 1, 0)) return 0;
    d->s.current = p;
    return 1;
  }
  if (d->record) { /* f5-20B metafile DC */
    int16_t v[2] = {(int16_t)y, (int16_t)x};
    d->s.current = (POINT){x, y};
    return gdi_record(d, 0x0213u, v, 2);
  }
  POINT p[2] = {d->s.current, {x, y}};
  if (!gdi_transform(d, p, 2, 0) || !stroke(d, p, 2, 0))
    return 0;
  d->s.current = (POINT){x, y};
  return 1;
}
BOOL W32API Polyline(HDC h, const POINT *p, int count) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  if (!d || !p || count < 2 || count > 8192)
    return 0;
  if (d->path && d->path->open)
    return gdi_path_poly(h,p,(unsigned)count,0,0);
  if (d->record)
    return gdi_record_poly(d, 0x0325u, p, 0, (unsigned)count);
  POINT *copy = gdi_alloc((DWORD)count * sizeof(POINT));
  if (!copy)
    return 0;
  w32_copy(copy, p, (DWORD)count * sizeof(POINT));
  int good =
      gdi_transform(d, copy, count, 0) && stroke(d, copy, (unsigned)count, 0);
  gdi_free(copy, (DWORD)count * sizeof(POINT));
  return good;
}
BOOL W32API PolyBezier(HDC h, const POINT *p, DWORD count) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  if (!d || !p || count < 4 || count > 766 || (count - 1) % 3)
    return 0;
  if (d->path && d->path->open)
    return gdi_path_bezier(h,p,count,0);
  for (unsigned i = 0; i < count; i++)
    if (p[i].x < -GDI_LIMIT || p[i].x > GDI_LIMIT || p[i].y < -GDI_LIMIT ||
        p[i].y > GDI_LIMIT)
      return 0;
  unsigned n = (count - 1) / 3 * 32 + 1;
  POINT *curve = gdi_alloc(n * sizeof(POINT));
  if (!curve)
    return 0;
  unsigned j = 0;
  curve[j++] = p[0];
  for (unsigned i = 0; i + 3 < count; i += 3)
    for (int t = 1; t <= 32; t++) {
      int64_t a = 32 - t, b = t, aa = a * a, bb = b * b;
      curve[j++] =
          (POINT){(LONG)((aa * a * p[i].x + 3 * aa * b * p[i + 1].x +
                          3 * a * bb * p[i + 2].x + bb * b * p[i + 3].x) /
                         32768),
                  (LONG)((aa * a * p[i].y + 3 * aa * b * p[i + 1].y +
                          3 * a * bb * p[i + 2].y + bb * b * p[i + 3].y) /
                         32768)};
    }
  int good = gdi_transform(d, curve, (int)n, 0) && stroke(d, curve, n, 0);
  gdi_free(curve, n * sizeof(POINT));
  return good;
}
BOOL W32API PolyPolygon(HDC h, const POINT *p, const int *counts,
                        int polygons) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  if (!d)
    return 0;
  if (d->path && d->path->open) {
    if (!p || !counts || polygons <= 0 || polygons > 8192) return 0;
    unsigned base=0;
    for(int i=0;i<polygons;i++) {
      if(counts[i]<2 || counts[i]>8192 || base>8192u-(unsigned)counts[i]) return 0;
      base+=(unsigned)counts[i];
    }
    unsigned old=d->path->count; base=0;
    for(int i=0;i<polygons;i++) {
      if(!gdi_path_poly(h,p+base,(unsigned)counts[i],0,1)) {
        d->path->count=old;return 0;
      }
      base+=(unsigned)counts[i];
    }
    return 1;
  }
  if (d->record && p && counts && polygons > 0) /* f5-20B metafile DC */
    return polygons == 1
               ? gdi_record_poly(d, 0x0324u, p, 0, (unsigned)counts[0])
               : gdi_record_poly(d, 0x0538u, p, counts, (unsigned)polygons);
  HRGN region = CreatePolyPolygonRgn(p, counts, polygons, d->s.fill);
  if (!region)
    return 0;
  int good = FillRgn(h, region, d->s.brush);
  unsigned base = 0;
  for (int k = 0; k < polygons && good; k++) {
    unsigned n = (unsigned)counts[k];
    POINT *q = gdi_alloc(n * sizeof(POINT));
    if (!q) {
      good = 0;
      break;
    }
    w32_copy(q, p + base, n * sizeof(POINT));
    good = gdi_transform(d, q, (int)n, 0) && stroke(d, q, n, 1);
    gdi_free(q, n * sizeof(POINT));
    base += n;
  }
  DeleteObject(region);
  return good;
}
BOOL W32API Polygon(HDC h, const POINT *p, int count) {
  return PolyPolygon(h, p, &count, 1);
}
BOOL W32API Rectangle(HDC h, int l, int t, int r, int b) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  if (!d)
    return 0;
  if (d->path && d->path->open) {
    if(l>=r || t>=b) return 1;
    if(l< -GDI_LIMIT || r>GDI_LIMIT || t< -GDI_LIMIT || b>GDI_LIMIT) return 0;
    POINT p[4]={{l,t},{r-1,t},{r-1,b-1},{l,b-1}};
    return gdi_path_poly(h,p,4,0,1);
  }
  if (d->record) {
    int16_t v[4] = {(int16_t)b, (int16_t)r, (int16_t)t, (int16_t)l};
    return gdi_record(d, 0x041bu, v, 4);
  }
  RECT q = gdi_rect(d, (RECT){l, t, r, b});
  if (q.left >= q.right || q.top >= q.bottom)
    return 1;
  if (!gdi_fill(d, q, d->s.brush, 0))
    return 0;
  struct object *pen = gdi_object(d->s.pen, GDI_PEN);
  int inset = pen && pen->u.pen.lopnStyle == PS_INSIDEFRAME
                  ? pen->u.pen.lopnWidth.x / 2
                  : 0;
  int inside = pen && pen->u.pen.lopnStyle == PS_INSIDEFRAME;
  int ix = inset, iy = inset;
  if (ix > (q.right - q.left - 1) / 2)
    ix = (q.right - q.left - 1) / 2;
  if (iy > (q.bottom - q.top - 1) / 2)
    iy = (q.bottom - q.top - 1) / 2;
  POINT p[4] = {{q.left + ix, q.top + iy},
                {q.right - 1 - ix, q.top + iy},
                {q.right - 1 - ix, q.bottom - 1 - iy},
                {q.left + ix, q.bottom - 1 - iy}};
  if (!inside)
    return stroke(d, p, 4, 1);
  struct region prior = d->s.clip, clip = {0}, bounds = {&q, 1, 0};
  int had_clip = d->s.has_clip;
  if (!gdi_region_combine(&clip, had_clip ? &prior : &bounds, &bounds, RGN_AND))
    return 0;
  d->s.clip = clip;
  d->s.has_clip = 1;
  int good = stroke(d, p, 4, 1);
  d->s.clip = prior;
  d->s.has_clip = had_clip;
  gdi_region_free(&clip);
  return good;
}
static int shape(HDC h, int l, int t, int r, int b, int rw, int rh) {
  struct dc *d = gdi_dc(h);
  if (!d)
    return 0;
  RECT q = gdi_rect(d, (RECT){l, t, r, b});
  int w = q.right - q.left, height = q.bottom - q.top;
  if (w <= 0 || height <= 0)
    return 1;
  if (rw <= 0 || rh <= 0)
    return Rectangle(h, l, t, r, b);
  if (rw > w)
    rw = w;
  if (rh > height)
    rh = height;
  HRGN region = CreateRectRgn(0, 0, 0, 0);
  struct object *o = gdi_object(region, GDI_REGION);
  if (!o)
    return 0;
  int good = 1;
  for (int y = q.top; y < q.bottom; y++) {
    int inset = 0;
    int64_t yy = 0;
    if (2 * (y - q.top) + 1 < rh)
      yy = 2 * (int64_t)(y - q.top) + 1 - rh;
    else if (2 * (q.bottom - y) - 1 < rh)
      yy = 2 * (int64_t)(q.bottom - y) - 1 - rh;
    if (yy)
      while (inset < rw / 2) {
        int64_t xx = 2 * (int64_t)inset + 1 - rw;
        if (xx * xx * 65536 / ((int64_t)rw * rw) +
                yy * yy * 65536 / ((int64_t)rh * rh) <=
            65536)
          break;
        inset++;
      }
    if (!gdi_region_add(&o->u.region,
                        (RECT){q.left + inset, y, q.right - inset, y + 1})) {
      good = 0;
      break;
    }
  }
  /* Region here is already in device units. */
  if (good)
    for (unsigned i = 0; i < o->u.region.count; i++)
      gdi_fill(d, o->u.region.rects[i], d->s.brush, 0);
  struct object *pen = gdi_object(d->s.pen, GDI_PEN);
  if (good && pen && pen->u.pen.lopnStyle != PS_NULL) {
    int width = pen->u.pen.lopnWidth.x ? pen->u.pen.lopnWidth.x : 1;
    COLORREF color =
        d->s.pen == GetStockObject(DC_PEN) ? d->s.dcpen : pen->u.pen.lopnColor;
    for (unsigned i = 0; i < o->u.region.count; i++) {
      RECT band = o->u.region.rects[i];
      int y = band.top;
      for (int x = band.left; x < band.right; x++)
        if (!gdi_region_point(&o->u.region, x - width, y) ||
            !gdi_region_point(&o->u.region, x + width, y) ||
            !gdi_region_point(&o->u.region, x, y - width) ||
            !gdi_region_point(&o->u.region, x, y + width)) {
          if (dash(pen->u.pen.lopnStyle, (unsigned)(x + y)))
            paint(d, x, y, color, 0, 1);
        }
    }
  }
  DeleteObject(region);
  gdi_damage(d, q);
  return good;
}
BOOL W32API Ellipse(HDC h, int l, int t, int r, int b) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  if (!d)
    return 0;
  if(d->path && d->path->open) {
    if(l>=r || t>=b) return 1;
    if(l< -GDI_LIMIT || r>GDI_LIMIT || t< -GDI_LIMIT || b>GDI_LIMIT) return 0;
    return gdi_path_shape(h,l,t,r,b,r-l,b-t);
  }
  if (d->record) {
    int16_t v[4] = {(int16_t)b, (int16_t)r, (int16_t)t, (int16_t)l};
    return gdi_record(d, 0x0418u, v, 4);
  }
  RECT q = gdi_rect(d, (RECT){l, t, r, b});
  return shape(h, l, t, r, b, q.right - q.left, q.bottom - q.top);
}
BOOL W32API RoundRect(HDC h, int l, int t, int r, int b, int w, int height) {
  GDI_GUARD;
  if (w < -GDI_LIMIT || w > GDI_LIMIT || height < -GDI_LIMIT ||
      height > GDI_LIMIT)
    return 0;
  struct dc *d = gdi_dc(h);
  if(d && d->path && d->path->open)
    return gdi_path_shape(h,l,t,r,b,w<0 ? -w : w,height<0 ? -height : height);
  if (d && d->record) {
    int16_t v[6] = {(int16_t)height, (int16_t)w, (int16_t)b,
                    (int16_t)r,      (int16_t)t, (int16_t)l};
    return gdi_record(d, 0x061cu, v, 6);
  }
  return shape(h, l, t, r, b, w < 0 ? -w : w, height < 0 ? -height : height);
}
static int angle_half(int64_t x, int64_t y) {
  return y > 0 || (!y && x >= 0) ? 0 : 1;
}
static int angle_before(int64_t ax, int64_t ay, int64_t bx, int64_t by) {
  int a = angle_half(ax, ay), b = angle_half(bx, by);
  return a != b ? a < b : ax * by - ay * bx > 0;
}
BOOL gdi_arc(HDC h, int l, int t, int r, int b, int sx, int sy, int ex,
                int ey, int mode) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  if (!d || l >= r || t >= b || l < -GDI_LIMIT || r > GDI_LIMIT ||
      t < -GDI_LIMIT || b > GDI_LIMIT || sx < -GDI_LIMIT || sx > GDI_LIMIT ||
      sy < -GDI_LIMIT || sy > GDI_LIMIT || ex < -GDI_LIMIT || ex > GDI_LIMIT ||
      ey < -GDI_LIMIT || ey > GDI_LIMIT)
    return 0;
  POINT all[128], out[130];
  int64_t vx[128], vy[128];
  unsigned n = 0;
  for (int quadrant = 0; quadrant < 4; quadrant++)
    for (int i = 0; i < 32; i++) {
      int64_t den = 1024 + i * i, a = (1024 - i * i) * 65536 / den,
              c = 64 * (int64_t)i * 65536 / den, x, y;
      if (quadrant == 0) {
        x = a;
        y = c;
      } else if (quadrant == 1) {
        x = -c;
        y = a;
      } else if (quadrant == 2) {
        x = -a;
        y = -c;
      } else {
        x = c;
        y = -a;
      }
      vx[n] = x;
      vy[n] = y;
      all[n++] = (POINT){(LONG)((l + r) / 2 + x * (r - l - 1) / 131072),
                         (LONG)((t + b) / 2 - y * (b - t - 1) / 131072)};
    }
  int64_t ax = (2 * (int64_t)sx - l - r) * (b - t),
          ay = (t + b - 2 * (int64_t)sy) * (r - l);
  int64_t bx = (2 * (int64_t)ex - l - r) * (b - t),
          by = (t + b - 2 * (int64_t)ey) * (r - l);
  /* Normalize direction before cross products; no trig/FPU instructions. */
  int64_t scale = ax < 0 ? -ax : ax, temp = ay < 0 ? -ay : ay;
  if (temp > scale)
    scale = temp;
  if (scale) {
    ax = ax * 65536 / scale;
    ay = ay * 65536 / scale;
  }
  scale = bx < 0 ? -bx : bx;
  temp = by < 0 ? -by : by;
  if (temp > scale)
    scale = temp;
  if (scale) {
    bx = bx * 65536 / scale;
    by = by * 65536 / scale;
  }
  unsigned first = 0, last = 0;
  while (first < 128 && angle_before(vx[first], vy[first], ax, ay))
    first++;
  while (last < 128 && angle_before(vx[last], vy[last], bx, by))
    last++;
  first %= 128;
  last %= 128;
  unsigned count = (last + 128 - first) % 128;
  if (!count)
    count = 128;
  for (unsigned i = 0; i <= count; i++)
    out[i] = all[(first + i) % 128];
  if (d->s.arc_direction == 2) {
    count = 128-count;
    if (!count) count=128;
    for (unsigned i=0;i<=count;i++) out[i]=all[(first+128-i)%128];
  }
  if (mode == 1) {
    return PolylineTo(h,out,count+1);
  }
  if (mode == 3) out[++count]=(POINT){(l+r)/2,(t+b)/2};
  if (d->path && d->path->open)
    return gdi_path_poly(h,out,count+1,0,mode>=2);
  if(d->record) return mode>=2 ? Polygon(h,out,(int)count+1) : Polyline(h,out,(int)count+1);
  if (mode >= 2) return Polygon(h,out,(int)count+1);
  return gdi_transform(d, out, (int)count + 1, 0) &&
         stroke(d, out, count + 1, 0);
}
BOOL W32API Arc(HDC h,int l,int t,int r,int b,int sx,int sy,int ex,int ey) {
  return gdi_arc(h,l,t,r,b,sx,sy,ex,ey,0);
}
BOOL W32API ExtFloodFill(HDC h, int x, int y, COLORREF boundary,
                         unsigned mode) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  POINT p = {x, y};
  if (!d || mode > 1 || !gdi_transform(d, &p, 1, 0) ||
      !gdi_visible(d, p.x, p.y))
    return 0;
  struct w32_dib *s = gdi_target(d);
  DWORD color = gdi_resolve_color(d, boundary), seed = w32_dib_rgb(s, p.x, p.y);
  if ((!mode && seed == color) || (mode && seed != color))
    return 0;
  unsigned area = s->width * s->height;
  uint8_t *visited = gdi_alloc(area);
  DWORD *queue = gdi_alloc(area * sizeof(DWORD));
  if (!visited || !queue) {
    gdi_free(visited, area);
    gdi_free(queue, area * sizeof(DWORD));
    return 0;
  }
  unsigned read = 0, write = 1;
  queue[0] = (DWORD)p.y * s->width + (DWORD)p.x;
  visited[queue[0]] = 1;
  while (read < write) {
    DWORD index = queue[read++];
    int px = (int)(index % s->width), py = (int)(index / s->width);
    DWORD c = w32_dib_rgb(s, px, py);
    if (!gdi_visible(d, px, py) || (!mode ? c == color : c != color))
      continue;
    DWORD brush;
    if (gdi_brush_pixel(d, d->s.brush, px, py, &brush))
      paint(d, px, py, brush, 0, 0);
    int dx[4] = {-1, 1, 0, 0}, dy[4] = {0, 0, -1, 1};
    for (unsigned k = 0; k < 4; k++) {
      int xx = px + dx[k], yy = py + dy[k];
      if (xx < 0 || yy < 0 || (DWORD)xx >= s->width || (DWORD)yy >= s->height)
        continue;
      unsigned next = (unsigned)yy * s->width + (unsigned)xx;
      if (!visited[next]) {
        visited[next] = 1;
        queue[write++] = next;
      }
    }
  }
  gdi_free(queue, area * sizeof(DWORD));
  gdi_free(visited, area);
  gdi_damage(d, (RECT){0, 0, (LONG)s->width, (LONG)s->height});
  return 1;
}
BOOL W32API w32_gdi_draw_icon(HDC h, int x, int y, int w, int height,
                              HBITMAP color, HBITMAP mask) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  struct object *c = color ? gdi_object(color, GDI_BITMAP) : 0,
                *m = mask ? gdi_object(mask, GDI_BITMAP) : 0;
  if (!d || (!c && !m) || (mask && (!m || m->u.bitmap.dib.bpp != 1)) ||
      w <= 0 || height <= 0 || (int64_t)x + w < -GDI_LIMIT ||
      (int64_t)x + w > GDI_LIMIT || (int64_t)y + height < -GDI_LIMIT ||
      (int64_t)y + height > GDI_LIMIT)
    return 0;
  if (d->record) { /* f5-20B: AND mask, then the colour image (SRCINVERT) */
    if (m)
      gdi_record_bitmap(d, x, y, w, height, &m->u.bitmap.dib, 0, 0,
                        (int)m->u.bitmap.dib.width,
                        (int)(c ? m->u.bitmap.dib.height
                                : m->u.bitmap.dib.height / 2),
                        0x008800c6u);
    if (c)
      gdi_record_bitmap(d, x, y, w, height, &c->u.bitmap.dib, 0, 0,
                        (int)c->u.bitmap.dib.width,
                        (int)c->u.bitmap.dib.height, 0x00660046u);
    return 1;
  }
  struct w32_dib *src = c ? &c->u.bitmap.dib : &m->u.bitmap.dib,
                 *dst = gdi_target(d);
  RECT r = gdi_rect(d, (RECT){x, y, x + w, y + height});
  int dw = r.right - r.left, dh = r.bottom - r.top;
  if (!dw || !dh)
    return 1;
  for (int yy = r.top; yy < r.bottom; yy++)
    for (int xx = r.left; xx < r.right; xx++)
      if (gdi_visible(d, xx, yy)) {
        int sx = (int)((int64_t)(xx - r.left) * src->width / dw),
            sy = (int)((int64_t)(yy - r.top) *
                       (c ? src->height : src->height / 2) / dh);
        DWORD a = 0;
        if (m) {
          struct w32_dib *bits = &m->u.bitmap.dib;
          if ((unsigned)sx >= bits->width || (unsigned)sy >= bits->height)
            continue;
          a = w32_dib_raw(bits, sx, sy) ? 0xffffffffu : 0;
        }
        DWORD rgb =
            c ? w32_dib_rgb(src, sx, sy)
              : (w32_dib_raw(src, sx, sy + (int)src->height / 2) ? 0xffffff
                                                                 : 0);
        w32_dib_put(dst, xx, yy,
                    (w32_dib_raw(dst, xx, yy) & a) ^ w32_dib_encode(dst, rgb));
      }
  gdi_damage(d, r);
  return 1;
}
