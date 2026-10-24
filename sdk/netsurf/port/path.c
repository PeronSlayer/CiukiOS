/* SPDX-License-Identifier: MIT
 * fx-24: NetSurf plotter paths (SVG shapes from libsvgtiny) on libnsfb.
 * Upstream's framebuffer front end (frontends/framebuffer/framebuffer.c,
 * framebuffer_plot_path) logs "path unimplemented", so SVG content showed
 * only its text. Curves are flattened in device space, fills use SVG's
 * default non-zero rule (SVG 1.1, 11.3 'fill-rule'), strokes are the union
 * of one quadrilateral per segment plus round joins/caps (all wound the same
 * way, so the non-zero union is exact). Each fill or stroke is rasterised as
 * an 8-bit coverage mask, exact horizontally and SUBSAMPLES rows per pixel
 * vertically, clipped to the current clip rectangle, and composited with
 * nsfb_plot_glyph8 (the same alpha blend that draws text). */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <libnsfb.h>
#include <libnsfb_plot.h>
#include "utils/errors.h"
#include "netsurf/types.h"
#include "netsurf/plot_style.h"
#include "netsurf/plotters.h"
#include "path.h"

#define SUBSAMPLES 4
#define MAX_POINTS (256u*1024u)
#define MAX_AREA (2048u*2048u)
#define JOIN_SIDES 8

struct point { float x, y; };
struct outline {
    struct point *p; size_t n, cap;
    size_t *start; bool *closed; size_t subpaths, scap;
};
struct edge { float x0, y0, x1, dxdy; float y1; int dir; };
static struct ciuki_path_stats stats;
struct ciuki_path_stats ciuki_path_stats(void) { return stats; }

static void outline_free(struct outline *o) { free(o->p); free(o->start); free(o->closed); memset(o, 0, sizeof(*o)); }
static bool push(struct outline *o, float x, float y)
{
    if (!isfinite(x) || !isfinite(y) || o->n >= MAX_POINTS) return false;
    if (o->n == o->cap) {
        size_t cap = o->cap ? o->cap * 2 : 64;
        struct point *p = realloc(o->p, cap * sizeof(*p));
        if (!p) return false;
        o->p = p; o->cap = cap;
    }
    o->p[o->n++] = (struct point){ x, y };
    return true;
}
static bool begin(struct outline *o, float x, float y)
{
    if (o->subpaths == o->scap) {
        size_t cap = o->scap ? o->scap * 2 : 16;
        size_t *s = realloc(o->start, cap * sizeof(*s));
        if (!s) return false;
        o->start = s;
        bool *c = realloc(o->closed, cap * sizeof(*c));
        if (!c) return false;
        o->closed = c; o->scap = cap;
    }
    o->start[o->subpaths] = o->n; o->closed[o->subpaths] = false; o->subpaths++;
    return push(o, x, y);
}
static size_t subpath_end(const struct outline *o, size_t i) { return i + 1 < o->subpaths ? o->start[i + 1] : o->n; }
static struct point map(const float t[6], float x, float y)
{ return (struct point){ t[0] * x + t[2] * y + t[4], t[1] * x + t[3] * y + t[5] }; }
static float distance(struct point a, struct point b) { return hypotf(b.x - a.x, b.y - a.y); }

/* NetSurf path commands to device-space polylines. False: malformed or
 * over the point bound (nothing is drawn). */
static bool flatten(const float *p, unsigned int n, const float t[6], struct outline *o)
{
    float cx = 0, cy = 0, sx = 0, sy = 0;
    bool open = false;
    for (unsigned int i = 0; i < n;) {
        int command = (int)p[i];
        if (command == PLOTTER_PATH_MOVE || command == PLOTTER_PATH_LINE) {
            if (i + 2 >= n) return false;
            float x = p[i + 1], y = p[i + 2];
            struct point d = map(t, x, y);
            if (command == PLOTTER_PATH_MOVE) {
                if (!begin(o, d.x, d.y)) return false;
                sx = x; sy = y;
            } else {
                if (!open) {
                    struct point s = map(t, cx, cy);
                    if (!begin(o, s.x, s.y)) return false;
                    sx = cx; sy = cy;
                }
                if (!push(o, d.x, d.y)) return false;
            }
            cx = x; cy = y; open = true; i += 3;
        } else if (command == PLOTTER_PATH_BEZIER) {
            if (i + 6 >= n) return false;
            if (!open) {
                struct point s = map(t, cx, cy);
                if (!begin(o, s.x, s.y)) return false;
                sx = cx; sy = cy; open = true;
            }
            struct point a = map(t, cx, cy), b = map(t, p[i + 1], p[i + 2]),
                c = map(t, p[i + 3], p[i + 4]), d = map(t, p[i + 5], p[i + 6]);
            float length = distance(a, b) + distance(b, c) + distance(c, d);
            if (!isfinite(length)) return false;
            int steps = (int)ceilf(sqrtf(length * 4.0f));
            if (steps < 2) steps = 2;
            if (steps > 100) steps = 100;
            for (int k = 1; k <= steps; k++) {
                float u = (float)k / steps, v = 1.0f - u;
                float w0 = v * v * v, w1 = 3 * v * v * u, w2 = 3 * v * u * u, w3 = u * u * u;
                if (!push(o, w0 * a.x + w1 * b.x + w2 * c.x + w3 * d.x, w0 * a.y + w1 * b.y + w2 * c.y + w3 * d.y))
                    return false;
            }
            cx = p[i + 5]; cy = p[i + 6]; i += 7;
        } else if (command == PLOTTER_PATH_CLOSE) {
            if (open) o->closed[o->subpaths - 1] = true;
            cx = sx; cy = sy; open = false; i += 1;
        } else {
            return false;
        }
    }
    return true;
}

static bool add_edge(struct edge **e, size_t *n, size_t *cap, struct point a, struct point b)
{
    if (a.y == b.y) return true;
    if (*n == *cap) {
        size_t next = *cap ? *cap * 2 : 128;
        if (next > 4 * MAX_POINTS) return false;
        struct edge *grown = realloc(*e, next * sizeof(**e));
        if (!grown) return false;
        *e = grown; *cap = next;
    }
    int dir = a.y < b.y ? 1 : -1;
    if (dir < 0) { struct point s = a; a = b; b = s; }
    (*e)[(*n)++] = (struct edge){ a.x, a.y, b.x, (b.x - a.x) / (b.y - a.y), b.y, dir };
    return true;
}
static int by_top(const void *a, const void *b)
{
    float x = ((const struct edge *)a)->y0, y = ((const struct edge *)b)->y0;
    return x < y ? -1 : x > y;
}
static void span(float *acc, int width, float a, float b, float weight)
{
    if (a < 0) a = 0;
    if (b > width) b = (float)width;
    if (b <= a) return;
    int ia = (int)a, ib = (int)b;
    if (ia == ib) { acc[ia] += (b - a) * weight; return; }
    acc[ia] += ((float)(ia + 1) - a) * weight;
    for (int i = ia + 1; i < ib; i++) acc[i] += weight;
    if (ib < width) acc[ib] += (b - (float)ib) * weight;
}
struct crossing { float x; int dir; };

/* Non-zero coverage of the edge set inside the clip, composited in colour. */
static bool raster(nsfb_t *nsfb, struct edge *e, size_t n, nsfb_colour_t colour)
{
    if (!n) return true;
    float x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
    for (size_t i = 0; i < n; i++) {
        float ex0 = e[i].x0, ex1 = e[i].x0 + e[i].dxdy * (e[i].y1 - e[i].y0);
        if (ex0 < x0) x0 = ex0; if (ex1 < x0) x0 = ex1;
        if (ex0 > x1) x1 = ex0; if (ex1 > x1) x1 = ex1;
        if (e[i].y0 < y0) y0 = e[i].y0;
        if (e[i].y1 > y1) y1 = e[i].y1;
    }
    nsfb_bbox_t clip;
    if (!nsfb_plot_get_clip(nsfb, &clip)) return true;
    if (x0 < clip.x0) x0 = (float)clip.x0;
    if (y0 < clip.y0) y0 = (float)clip.y0;
    if (x1 > clip.x1) x1 = (float)clip.x1;
    if (y1 > clip.y1) y1 = (float)clip.y1;
    if (!(x0 < x1 && y0 < y1)) return true;
    int bx0 = (int)floorf(x0), by0 = (int)floorf(y0), bx1 = (int)ceilf(x1), by1 = (int)ceilf(y1);
    int width = bx1 - bx0, height = by1 - by0;
    if (width <= 0 || height <= 0) return true;
    if ((size_t)width * (size_t)height > MAX_AREA) { stats.rejected++; return true; }
    uint8_t *mask = calloc((size_t)width, (size_t)height);
    float *acc = calloc((size_t)width + 1, sizeof(*acc));
    size_t *active = malloc(n * sizeof(*active));
    struct crossing *cross = malloc(n * sizeof(*cross));
    bool ok = mask && acc && active && cross;
    if (ok) {
        qsort(e, n, sizeof(*e), by_top);
        size_t next = 0, live = 0;
        for (int row = 0; row < height; row++) {
            memset(acc, 0, ((size_t)width + 1) * sizeof(*acc));
            bool any = false;
            for (int s = 0; s < SUBSAMPLES; s++) {
                float y = (float)(by0 + row) + ((float)s + 0.5f) / SUBSAMPLES;
                while (next < n && e[next].y0 <= y) active[live++] = next++;
                size_t keep = 0, count = 0;
                for (size_t k = 0; k < live; k++) {
                    struct edge *g = &e[active[k]];
                    if (g->y1 <= y) continue;
                    active[keep++] = active[k];
                    if (g->y0 > y) continue;
                    float x = g->x0 + (y - g->y0) * g->dxdy;
                    size_t at = count++;
                    while (at && cross[at - 1].x > x) { cross[at] = cross[at - 1]; at--; }
                    cross[at] = (struct crossing){ x, g->dir };
                }
                live = keep;
                int winding = 0; float from = 0;
                for (size_t k = 0; k < count; k++) {
                    int before = winding;
                    winding += cross[k].dir;
                    if (!before && winding) from = cross[k].x;
                    else if (before && !winding) {
                        span(acc, width, from - bx0, cross[k].x - bx0, 1.0f / SUBSAMPLES);
                        any = true;
                    }
                }
            }
            if (!any) continue;
            uint8_t *out = mask + (size_t)row * width;
            for (int i = 0; i < width; i++) {
                float v = acc[i] * 255.0f + 0.5f;
                out[i] = v >= 255.0f ? 255 : v <= 0.0f ? 0 : (uint8_t)v;
            }
        }
        nsfb_bbox_t box = { bx0, by0, bx1, by1 };
        nsfb_plot_glyph8(nsfb, &box, mask, width, colour);
    }
    free(cross); free(active); free(acc); free(mask);
    return ok;
}

static bool fill_edges(const struct outline *o, struct edge **e, size_t *n, size_t *cap)
{
    for (size_t s = 0; s < o->subpaths; s++) {
        size_t first = o->start[s], last = subpath_end(o, s);
        if (last - first < 3) continue;
        for (size_t i = first; i < last; i++)
            if (!add_edge(e, n, cap, o->p[i], o->p[i + 1 < last ? i + 1 : first])) return false;
    }
    return true;
}
/* Polygon with positive orientation (screen coordinates), as edges. */
static bool polygon(struct edge **e, size_t *n, size_t *cap, const struct point *q, unsigned count)
{
    float area = 0;
    for (unsigned i = 0; i < count; i++) {
        struct point a = q[i], b = q[(i + 1) % count];
        area += a.x * b.y - b.x * a.y;
    }
    for (unsigned i = 0; i < count; i++) {
        unsigned j = (i + 1) % count;
        if (!(area >= 0 ? add_edge(e, n, cap, q[i], q[j]) : add_edge(e, n, cap, q[j], q[i]))) return false;
    }
    return true;
}
static bool stroke_edges(const struct outline *o, float width, struct edge **e, size_t *n, size_t *cap)
{
    float h = width / 2;
    for (size_t s = 0; s < o->subpaths; s++) {
        size_t first = o->start[s], last = subpath_end(o, s);
        size_t segments = last - first - 1 + (o->closed[s] && last - first > 2);
        for (size_t k = 0; k < segments; k++) {
            struct point a = o->p[first + k], b = o->p[first + (k + 1) % (last - first)];
            float length = distance(a, b);
            if (length < 1e-6f) continue;
            float nx = -(b.y - a.y) / length * h, ny = (b.x - a.x) / length * h;
            struct point quad[4] = { { a.x + nx, a.y + ny }, { b.x + nx, b.y + ny }, { b.x - nx, b.y - ny }, { a.x - nx, a.y - ny } };
            if (!polygon(e, n, cap, quad, 4)) return false;
        }
        /* Round joins and caps; below 1.5 px they are inside the quads' AA. */
        if (width < 1.5f) continue;
        for (size_t i = first; i < last; i++) {
            struct point join[JOIN_SIDES];
            for (unsigned j = 0; j < JOIN_SIDES; j++) {
                float angle = (float)j * 6.2831853f / JOIN_SIDES;
                join[j] = (struct point){ o->p[i].x + h * cosf(angle), o->p[i].y + h * sinf(angle) };
            }
            if (!polygon(e, n, cap, join, JOIN_SIDES)) return false;
        }
    }
    return true;
}

nserror ciuki_plot_path(nsfb_t *nsfb, const plot_style_t *style, const float *p, unsigned int n,
                        const float transform[6])
{
    stats.paths++;
    if (!nsfb || !style || !p || !n || !transform) return NSERROR_OK;
    bool fill = style->fill_colour != NS_TRANSPARENT, stroke = style->stroke_colour != NS_TRANSPARENT;
    if (!fill && !stroke) return NSERROR_OK;
    struct outline o = { 0 };
    nserror result = NSERROR_OK;
    if (!flatten(p, n, transform, &o)) {
        stats.rejected++;
        outline_free(&o);
        return NSERROR_OK;  /* malformed or oversized path: draw nothing, like upstream */
    }
    struct edge *e = NULL; size_t count = 0, cap = 0;
    if (fill) {
        if (!fill_edges(&o, &e, &count, &cap) || !raster(nsfb, e, count, style->fill_colour)) result = NSERROR_NOMEM;
        else stats.fills++;
    }
    if (stroke && result == NSERROR_OK) {
        float scale = sqrtf(fabsf(transform[0] * transform[3] - transform[1] * transform[2]));
        float width = plot_style_fixed_to_float(style->stroke_width) * scale;
        if (!(width >= 1.0f)) width = 1.0f;
        count = 0;
        if (!stroke_edges(&o, width, &e, &count, &cap) || !raster(nsfb, e, count, style->stroke_colour)) result = NSERROR_NOMEM;
        else stats.strokes++;
    }
    free(e);
    outline_free(&o);
    return result;
}
