/* SPDX-License-Identifier: MIT
 * Shared helpers of the CiukiOS accessories; see acc.h. */
#include "acc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int acc_args(int argc, char **argv, const char *test_name, int *channel, int *test, const char **path)
{
    *channel = 3; *test = 0; *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--channel-fd=", 13)) {
            char *end; long n = strtol(argv[i] + 13, &end, 10);
            if (!argv[i][13] || *end || n < 3 || n >= 128) return 2;
            *channel = (int)n;
        } else if (!strncmp(argv[i], "--test=", 7)) {
            if (strcmp(argv[i] + 7, test_name)) return 2;
            *test = 1;
        } else if (argv[i][0] == '-' || *path) return 2;
        else *path = argv[i];
    }
    return 0;
}

void acc_window_size(int pw, int ph, int *w, int *h)
{
    const char *d = getenv("CIUKI_DISPLAY");
    long dw = 640, dh = 480;
    if (d) { char *end; dw = strtol(d, &end, 10); if (*end == 'x') dh = strtol(end + 1, NULL, 10); }
    /* Leave room for the top bar, the taskbar and the window frame. */
    long mw = dw - 16, mh = dh - 96;
    *w = pw < mw ? pw : (int)mw; *h = ph < mh ? ph : (int)mh;
    if (*w < 160) *w = 160;
    if (*h < 120) *h = 120;
}

void acc_pixel(struct du_canvas *c, int x, int y, uint32_t color)
{
    if (du_contains(c->clip, x, y) && x >= 0 && y >= 0 && x < c->width && y < c->height)
        c->pixels[(size_t)y * (size_t)c->width + (size_t)x] = color;
}

void acc_line(struct du_canvas *c, int x0, int y0, int x1, int y1, uint32_t color)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        acc_pixel(c, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void acc_thick_line(struct du_canvas *c, int x0, int y0, int x1, int y1, int width, uint32_t color)
{
    int lo = -(width - 1) / 2, hi = width / 2;
    int steep = abs(y1 - y0) > abs(x1 - x0);
    for (int o = lo; o <= hi; o++) {
        if (steep) acc_line(c, x0 + o, y0, x1 + o, y1, color);
        else acc_line(c, x0, y0 + o, x1, y1 + o, color);
    }
}

void acc_circle(struct du_canvas *c, int cx, int cy, int r, uint32_t color)
{
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        acc_pixel(c, cx + x, cy + y, color); acc_pixel(c, cx + y, cy + x, color);
        acc_pixel(c, cx - y, cy + x, color); acc_pixel(c, cx - x, cy + y, color);
        acc_pixel(c, cx - x, cy - y, color); acc_pixel(c, cx - y, cy - x, color);
        acc_pixel(c, cx + y, cy - x, color); acc_pixel(c, cx + x, cy - y, color);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

void acc_disc(struct du_canvas *c, int cx, int cy, int r, uint32_t color)
{
    for (int y = -r; y <= r; y++) {
        int w = 0;
        while ((w + 1) * (w + 1) + y * y <= r * r) w++;
        du_hline(c, cx - w, cy + y, 2 * w + 1, color);
    }
}

static const short sine[60] = {
    0,107,213,316,416,512,602,685,761,828,887,935,974,1002,1018,1024,1018,1002,974,935,887,828,761,685,602,512,
    416,316,213,107,0,-107,-213,-316,-416,-512,-602,-685,-761,-828,-887,-935,-974,-1002,-1018,-1024,-1018,-1002,
    -974,-935,-887,-828,-761,-685,-602,-512,-416,-316,-213,-107 };
int acc_sin60(int i) { return sine[((i % 60) + 60) % 60]; }
int acc_cos60(int i) { return sine[((i + 15) % 60 + 60) % 60]; }

uint32_t acc_digest(const struct du_canvas *c)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < (size_t)c->width * (size_t)c->height; i++)
        for (int b = 0; b < 4; b++) h = (h ^ ((c->pixels[i] >> (8 * b)) & 0xffu)) * 16777619u;
    return h;
}

const char *acc_basename(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

void acc_title(char *out, size_t cap, const char *label, const char *path)
{
    int n = path ? snprintf(out, cap, "%s - %s", label, acc_basename(path)) : snprintf(out, cap, "%s", label);
    for (int i = 0; i < n && (size_t)i < cap - 1; i++)
        if ((unsigned char)out[i] < 0x20 || (unsigned char)out[i] > 0x7e) out[i] = '?';
}
