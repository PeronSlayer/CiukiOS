/* Canonical horizontal bands; all region operations are transactional.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"
void gdi_region_free(struct region *r) {
  gdi_free(r->rects, r->capacity * sizeof(RECT));
  *r = (struct region){0};
}
int gdi_region_add(struct region *r, RECT q) {
  if (q.left >= q.right || q.top >= q.bottom)
    return 1;
  if (q.left < -GDI_LIMIT || q.right > GDI_LIMIT || q.top < -GDI_LIMIT ||
      q.bottom > GDI_LIMIT)
    return 0;
  if (r->count == r->capacity) {
    unsigned cap = r->capacity ? r->capacity * 2 : 16;
    if (cap > GDI_MAX_RECTS)
      return 0;
    RECT *p = gdi_alloc(cap * sizeof(RECT));
    if (!p)
      return 0;
    if (r->count)
      w32_copy(p, r->rects, r->count * sizeof(RECT));
    gdi_free(r->rects, r->capacity * sizeof(RECT));
    r->rects = p;
    r->capacity = cap;
  }
  r->rects[r->count++] = q;
  return 1;
}
int gdi_region_copy(struct region *out, const struct region *r) {
  struct region n = {0};
  if (r->count) {
    n.rects = gdi_alloc(r->count * sizeof(RECT));
    if (!n.rects)
      return 0;
    n.count = n.capacity = r->count;
    w32_copy(n.rects, r->rects, n.count * sizeof(RECT));
  }
  gdi_region_free(out);
  *out = n;
  return 1;
}
int gdi_region_point(const struct region *r, int x, int y) {
  for (unsigned i = 0; i < r->count; i++) {
    RECT q = r->rects[i];
    if (q.top > y)
      break;
    if (x >= q.left && x < q.right && y >= q.top && y < q.bottom)
      return 1;
  }
  return 0;
}
RECT gdi_region_box(const struct region *r) {
  RECT q = {0};
  if (!r->count)
    return q;
  q = r->rects[0];
  for (unsigned i = 1; i < r->count; i++) {
    RECT a = r->rects[i];
    if (a.left < q.left)
      q.left = a.left;
    if (a.top < q.top)
      q.top = a.top;
    if (a.right > q.right)
      q.right = a.right;
    if (a.bottom > q.bottom)
      q.bottom = a.bottom;
  }
  return q;
}
int gdi_region_type(const struct region *r) {
  return !r->count ? NULLREGION : r->count == 1 ? SIMPLEREGION : COMPLEXREGION;
}
static void sort(int *v, unsigned n) {
  /* Shell sort has bounded stack and avoids quadratic insertion on tall
   * polygon regions with interleaved bands. */
  for (unsigned gap = n / 2; gap; gap /= 2)
    for (unsigned i = gap; i < n; i++) {
      int x = v[i];
      unsigned j = i;
      while (j >= gap && v[j - gap] > x) {
        v[j] = v[j - gap];
        j -= gap;
      }
      v[j] = x;
    }
}
static unsigned unique(int *v, unsigned n) {
  sort(v, n);
  unsigned m = 0;
  for (unsigned i = 0; i < n; i++)
    if (!m || v[i] != v[m - 1])
      v[m++] = v[i];
  return m;
}
static int boolean(int a, int b, int mode) {
  return mode == RGN_AND    ? a && b
         : mode == RGN_OR   ? a || b
         : mode == RGN_XOR  ? a != b
         : mode == RGN_DIFF ? a && !b
                            : a;
}
/* Merge an entire band only when all its intervals match the preceding band. */
static void coalesce(struct region *r, unsigned *previous, unsigned start) {
  unsigned prev = *previous, n = r->count - start;
  if (prev < start && start - prev == n && n &&
      r->rects[prev].bottom == r->rects[start].top) {
    unsigned i = 0;
    for (; i < n; i++)
      if (r->rects[prev + i].left != r->rects[start + i].left ||
          r->rects[prev + i].right != r->rects[start + i].right)
        break;
    if (i == n) {
      for (i = 0; i < n; i++)
        r->rects[prev + i].bottom = r->rects[start + i].bottom;
      r->count = start;
      return;
    }
  }
  *previous = start;
}
int gdi_region_combine(struct region *out, const struct region *a,
                       const struct region *b, int mode) {
  if (mode < 1 || mode > 5 || !a || (mode != RGN_COPY && !b))
    return 0;
  if (mode == RGN_COPY)
    return gdi_region_copy(out, a);
  unsigned count = a->count + b->count;
  if (!count) {
    gdi_region_free(out);
    return 1;
  }
  int *ys = gdi_alloc(count * 2 * sizeof(int)),
      *xs = gdi_alloc(count * 2 * sizeof(int));
  struct region n = {0};
  int good = 0;
  if (!ys || !xs)
    goto done;
  unsigned yn = 0;
  for (unsigned k = 0; k < 2; k++) {
    const struct region *r = k ? b : a;
    for (unsigned i = 0; i < r->count; i++) {
      ys[yn++] = r->rects[i].top;
      ys[yn++] = r->rects[i].bottom;
    }
  }
  yn = unique(ys, yn);
  unsigned previous = 0;
  for (unsigned yi = 0; yi + 1 < yn; yi++) {
    unsigned xn = 0;
    for (unsigned k = 0; k < 2; k++) {
      const struct region *r = k ? b : a;
      for (unsigned i = 0; i < r->count; i++) {
        RECT q = r->rects[i];
        if (q.top <= ys[yi] && q.bottom > ys[yi]) {
          xs[xn++] = q.left;
          xs[xn++] = q.right;
        }
      }
    }
    xn = unique(xs, xn);
    int open = 0, left = 0;
    unsigned start = n.count;
    for (unsigned xi = 0; xi < xn; xi++) {
      int set =
          xi + 1 < xn && boolean(gdi_region_point(a, xs[xi], ys[yi]),
                                 gdi_region_point(b, xs[xi], ys[yi]), mode);
      if (set && !open) {
        open = 1;
        left = xs[xi];
      }
      if (!set && open) {
        if (!gdi_region_add(&n, (RECT){left, ys[yi], xs[xi], ys[yi + 1]}))
          goto done;
        open = 0;
      }
    }
    coalesce(&n, &previous, start);
  }
  gdi_region_free(out);
  *out = n;
  n = (struct region){0};
  good = 1;
done:
  gdi_free(xs, count * 2 * sizeof(int));
  gdi_free(ys, count * 2 * sizeof(int));
  gdi_region_free(&n);
  return good;
}
HRGN W32API CreateRectRgn(int l, int t, int r, int b) {
  GDI_GUARD;
  HGDIOBJ h;
  struct object *o = gdi_new(GDI_REGION, &h);
  if (!o)
    return 0;
  if (l > r) {
    int v = l;
    l = r;
    r = v;
  }
  if (t > b) {
    int v = t;
    t = b;
    b = v;
  }
  if (!gdi_region_add(&o->u.region, (RECT){l, t, r, b})) {
    DeleteObject(h);
    return 0;
  }
  return h;
}
HRGN W32API CreateRectRgnIndirect(const RECT *r) {
  return r ? CreateRectRgn(r->left, r->top, r->right, r->bottom) : 0;
}
BOOL W32API SetRectRgn(HRGN h, int l, int t, int r, int b) {
  GDI_GUARD;
  struct object *o = gdi_object(h, GDI_REGION);
  if (!o)
    return 0;
  struct region n = {0};
  if (l > r) {
    int v = l;
    l = r;
    r = v;
  }
  if (t > b) {
    int v = t;
    t = b;
    b = v;
  }
  if (!gdi_region_add(&n, (RECT){l, t, r, b}))
    return 0;
  gdi_region_free(&o->u.region);
  o->u.region = n;
  return 1;
}
int W32API CombineRgn(HRGN dst, HRGN ah, HRGN bh, int mode) {
  GDI_GUARD;
  struct object *d = gdi_object(dst, GDI_REGION),
                *a = gdi_object(ah, GDI_REGION),
                *b = gdi_object(bh, GDI_REGION);
  if (!d || !a ||
      !gdi_region_combine(&d->u.region, &a->u.region, b ? &b->u.region : 0,
                          mode))
    return ERROR;
  return gdi_region_type(&d->u.region);
}
int gdi_region_shift(struct region *region, int dx, int dy) {
  RECT box = gdi_region_box(region);
  if ((int64_t)box.left + dx < -GDI_LIMIT ||
      (int64_t)box.right + dx > GDI_LIMIT ||
      (int64_t)box.top + dy < -GDI_LIMIT ||
      (int64_t)box.bottom + dy > GDI_LIMIT)
    return 0;
  for (unsigned i = 0; i < region->count; i++) {
    RECT *q = &region->rects[i];
    q->left += dx;
    q->right += dx;
    q->top += dy;
    q->bottom += dy;
  }
  return 1;
}
int W32API OffsetRgn(HRGN h, int dx, int dy) {
  GDI_GUARD;
  struct object *o = gdi_object(h, GDI_REGION);
  return o && gdi_region_shift(&o->u.region, dx, dy)
             ? gdi_region_type(&o->u.region)
             : ERROR;
}
BOOL W32API PtInRegion(HRGN h, int x, int y) {
  GDI_GUARD;
  struct object *o = gdi_object(h, GDI_REGION);
  return o && gdi_region_point(&o->u.region, x, y);
}
BOOL W32API RectInRegion(HRGN h, const RECT *r) {
  GDI_GUARD;
  struct object *o = gdi_object(h, GDI_REGION);
  if (!o || !r)
    return 0;
  for (unsigned i = 0; i < o->u.region.count; i++) {
    RECT q = o->u.region.rects[i];
    if (q.left < r->right && q.right > r->left && q.top < r->bottom &&
        q.bottom > r->top)
      return 1;
  }
  return 0;
}
BOOL W32API EqualRgn(HRGN ah, HRGN bh) {
  GDI_GUARD;
  struct object *a = gdi_object(ah, GDI_REGION),
                *b = gdi_object(bh, GDI_REGION);
  struct region n = {0};
  if (!a || !b || !gdi_region_combine(&n, &a->u.region, &b->u.region, RGN_XOR))
    return 0;
  int equal = !n.count;
  gdi_region_free(&n);
  return equal;
}
int W32API GetRgnBox(HRGN h, RECT *out) {
  GDI_GUARD;
  struct object *o = gdi_object(h, GDI_REGION);
  if (!o || !out)
    return ERROR;
  *out = gdi_region_box(&o->u.region);
  return gdi_region_type(&o->u.region);
}
HRGN W32API CreateEllipticRgn(int l, int t, int r, int b) {
  GDI_GUARD;
  HRGN h = CreateRectRgn(0, 0, 0, 0);
  struct object *o = gdi_object(h, GDI_REGION);
  if (!o)
    return 0;
  if (l > r) {
    int v = l;
    l = r;
    r = v;
  }
  if (t > b) {
    int v = t;
    t = b;
    b = v;
  }
  if (l < -GDI_LIMIT || t < -GDI_LIMIT || r > GDI_LIMIT || b > GDI_LIMIT)
    goto fail;
  int64_t w = r - l, height = b - t;
  unsigned previous = 0;
  /* Dividing each square first keeps the ellipse test within signed 64-bit. */
  for (int y = t; y < b; y++) {
    int64_t yy = 2 * (int64_t)y + 1 - t - b;
    int left = l, right = r;
    while (left < right) {
      int64_t xx = 2 * (int64_t)left + 1 - l - r;
      if ((xx * xx * 65536 / (w * w)) + (yy * yy * 65536 / (height * height)) <=
          65536)
        break;
      left++;
    }
    right = r - (left - l);
    unsigned start = o->u.region.count;
    if (!gdi_region_add(&o->u.region, (RECT){left, y, right, y + 1}))
      goto fail;
    coalesce(&o->u.region, &previous, start);
  }
  return h;
fail:
  DeleteObject(h);
  return 0;
}
HRGN W32API CreateEllipticRgnIndirect(const RECT *r) {
  return r ? CreateEllipticRgn(r->left, r->top, r->right, r->bottom) : 0;
}
struct crossing {
  int64_t x;
  int winding;
};
static int ceil_pixel(int64_t v) {
  v -= 32768;
  return (int)(v >= 0 ? (v + 65535) / 65536 : v / 65536);
}
HRGN W32API CreatePolyPolygonRgn(const POINT *p, const int *counts,
                                 int polygons, int mode) {
  GDI_GUARD;
  if (!p || !counts || polygons <= 0 || polygons > 4096 ||
      (mode != ALTERNATE && mode != WINDING))
    return 0;
  unsigned total = 0;
  int top = GDI_LIMIT, bottom = -GDI_LIMIT;
  for (int k = 0; k < polygons; k++) {
    if (counts[k] < 2 || counts[k] > 8192 || total + (unsigned)counts[k] > 8192)
      return 0;
    total += (unsigned)counts[k];
  }
  for (unsigned i = 0; i < total; i++) {
    if (p[i].x < -GDI_LIMIT || p[i].x > GDI_LIMIT || p[i].y < -GDI_LIMIT ||
        p[i].y > GDI_LIMIT)
      return 0;
    if (p[i].y < top)
      top = p[i].y;
    if (p[i].y > bottom)
      bottom = p[i].y;
  }
  struct crossing *v = gdi_alloc(total * sizeof(*v));
  if (!v)
    return 0;
  HRGN h = CreateRectRgn(0, 0, 0, 0);
  struct object *o = gdi_object(h, GDI_REGION);
  if (!o) {
    gdi_free(v, total * sizeof(*v));
    return 0;
  }
  unsigned previous = 0;
  for (int y = top; y < bottom; y++) {
    unsigned n = 0, base = 0;
    for (int k = 0; k < polygons; k++) {
      for (int i = 0; i < counts[k]; i++) {
        POINT a = p[base + (unsigned)i],
              b = p[base + (unsigned)((i + 1) % counts[k])];
        int direction = a.y < b.y ? 1 : -1;
        if (a.y > b.y) {
          POINT temp = a;
          a = b;
          b = temp;
        }
        if (y < a.y || y >= b.y)
          continue;
        v[n++] = (struct crossing){
            (int64_t)a.x * 65536 + ((2 * (int64_t)y + 1 - 2 * (int64_t)a.y) *
                                    (b.x - a.x) * 32768) /
                                       (b.y - a.y),
            direction};
      }
      base += (unsigned)counts[k];
    }
    for (unsigned gap = n / 2; gap; gap /= 2)
      for (unsigned i = gap; i < n; i++) {
        struct crossing c = v[i];
        unsigned j = i;
        while (j >= gap && v[j - gap].x > c.x) {
          v[j] = v[j - gap];
          j -= gap;
        }
        v[j] = c;
      }
    int winding = 0, left = 0;
    unsigned start = o->u.region.count;
    for (unsigned i = 0; i < n;) {
      int old = mode == WINDING ? winding != 0 : (winding & 1);
      int64_t x = v[i].x;
      do {
        winding += mode == WINDING ? v[i].winding : 1;
        i++;
      } while (i < n && v[i].x == x);
      int set = mode == WINDING ? winding != 0 : (winding & 1);
      if (!old && set)
        left = ceil_pixel(x);
      if (old && !set &&
          !gdi_region_add(&o->u.region,
                          (RECT){left, y, ceil_pixel(x), y + 1})) {
        DeleteObject(h);
        h = 0;
        goto done;
      }
    }
    coalesce(&o->u.region, &previous, start);
  }
done:
  gdi_free(v, total * sizeof(*v));
  return h;
}
HRGN W32API CreatePolygonRgn(const POINT *p, int n, int mode) {
  return CreatePolyPolygonRgn(p, &n, 1, mode);
}
DWORD W32API GetRegionData(HRGN h, DWORD bytes, RGNDATA *data) {
  GDI_GUARD;
  struct object *o = gdi_object(h, GDI_REGION);
  if (!o)
    return 0;
  DWORD size = sizeof(RGNDATAHEADER) + o->u.region.count * sizeof(RECT);
  if (!data)
    return size;
  if (bytes < size)
    return 0;
  data->rdh = (RGNDATAHEADER){sizeof(RGNDATAHEADER), 1, o->u.region.count,
                              o->u.region.count * sizeof(RECT),
                              gdi_region_box(&o->u.region)};
  if (o->u.region.count)
    w32_copy(data->Buffer, o->u.region.rects, o->u.region.count * sizeof(RECT));
  return size;
}
/* Decode the public XFORM's IEEE binary32 fields into signed 16.16 without
 * executing floating point instructions in this i686 engine. */
static int fixed_float(DWORD raw, int64_t *out) {
  unsigned exponent = (raw >> 23) & 255;
  uint64_t mantissa = (raw & 0x7fffff) | (exponent ? 0x800000 : 0);
  if (exponent == 255)
    return 0;
  int shift = (int)exponent - 127 - 23 + 16;
  if (!exponent)
    shift = -126 - 23 + 16;
  if (shift > 15)
    return 0;
  uint64_t value = shift >= 0     ? mantissa << (unsigned)shift
                   : shift <= -64 ? 0
                                  : mantissa >> (unsigned)-shift;
  if (value > (uint64_t)GDI_LIMIT * 65536)
    return 0;
  *out = (raw >> 31) ? -(int64_t)value : (int64_t)value;
  return 1;
}
HRGN W32API ExtCreateRegion(const void *transform, DWORD bytes,
                            const RGNDATA *data) {
  GDI_GUARD;
  if (!data || bytes < sizeof(RGNDATAHEADER) ||
      data->rdh.dwSize != sizeof(RGNDATAHEADER) || data->rdh.iType != 1 ||
      data->rdh.nCount > GDI_MAX_RECTS ||
      data->rdh.nRgnSize != data->rdh.nCount * sizeof(RECT) ||
      data->rdh.nCount > (bytes - sizeof(RGNDATAHEADER)) / sizeof(RECT))
    return 0;
  int64_t matrix[6] = {65536, 0, 0, 65536, 0, 0};
  if (transform)
    for (unsigned i = 0; i < 6; i++)
      if (!fixed_float(w32_u32((const uint8_t *)transform + i * 4), &matrix[i]))
        return 0;
  HRGN h = CreateRectRgn(0, 0, 0, 0);
  struct object *o = gdi_object(h, GDI_REGION);
  if (!o)
    return 0;
  for (unsigned i = 0; i < data->rdh.nCount; i++) {
    RECT q;
    w32_copy(&q, data->Buffer + i * sizeof(RECT), sizeof(q));
    struct region a = {&q, 1, 0};
    if (q.left >= q.right || q.top >= q.bottom || q.left < -GDI_LIMIT ||
        q.right > GDI_LIMIT || q.top < -GDI_LIMIT || q.bottom > GDI_LIMIT) {
      DeleteObject(h);
      return 0;
    }
    if (!transform) {
      if (!gdi_region_combine(&o->u.region, &o->u.region, &a, RGN_OR)) {
        DeleteObject(h);
        return 0;
      }
    } else {
      POINT points[4] = {{q.left, q.top},
                         {q.right, q.top},
                         {q.right, q.bottom},
                         {q.left, q.bottom}};
      for (unsigned j = 0; j < 4; j++) {
        int64_t x =
            (matrix[0] * points[j].x + matrix[2] * points[j].y + matrix[4]) /
            65536;
        int64_t y =
            (matrix[1] * points[j].x + matrix[3] * points[j].y + matrix[5]) /
            65536;
        if (x < -GDI_LIMIT || x > GDI_LIMIT || y < -GDI_LIMIT ||
            y > GDI_LIMIT) {
          DeleteObject(h);
          return 0;
        }
        points[j] = (POINT){(LONG)x, (LONG)y};
      }
      HRGN polygon = CreatePolygonRgn(points, 4, WINDING);
      if (!polygon || !CombineRgn(h, h, polygon, RGN_OR)) {
        if (polygon)
          DeleteObject(polygon);
        DeleteObject(h);
        return 0;
      }
      DeleteObject(polygon);
    }
  }
  return h;
}
