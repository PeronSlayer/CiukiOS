/* SPDX-License-Identifier: MIT
 * Shared helpers of the CiukiOS accessories (Calculator, Clock, Image Viewer,
 * Help, Volume Control, Character Map): command-line parsing, window sizing,
 * integer drawing on a libdeskui canvas and a surface digest. Drawing uses
 * integers only, like the toolkit. */
#ifndef CIUKI_ACCESSORIES_ACC_H
#define CIUKI_ACCESSORIES_ACC_H
#include "../libdeskui/deskui.h"
#include <stddef.h>
#include <stdint.h>

/* The launcher catalogue (names, labels, Tango icon names) is apps/desktop/catalog.h. */

/* argv: [--channel-fd=N] [--test=<name>] [path]. Returns 0, or 2 on a bad
 * argument (the process exit status, as the other desktop clients). */
int acc_args(int argc, char **argv, const char *test_name, int *channel, int *test, const char **path);
int acc_set_wallpaper(struct du_window *, const char *path);
/* Window size from CIUKI_DISPLAY=<w>x<h>, clamped to the given preferred size. */
void acc_window_size(int preferred_w, int preferred_h, int *w, int *h);

/* Drawing (clipped to the canvas clip). */
void acc_pixel(struct du_canvas *, int x, int y, uint32_t);
void acc_line(struct du_canvas *, int x0, int y0, int x1, int y1, uint32_t);
void acc_thick_line(struct du_canvas *, int x0, int y0, int x1, int y1, int width, uint32_t);
void acc_circle(struct du_canvas *, int cx, int cy, int r, uint32_t);
void acc_disc(struct du_canvas *, int cx, int cy, int r, uint32_t);
/* sin/cos of i*6 degrees (i in 0..59, clockwise from 12 o'clock), scaled by 1024. */
int acc_sin60(int i);
int acc_cos60(int i);
/* FNV-1a over the canvas pixels (the test digests of the other apps). */
uint32_t acc_digest(const struct du_canvas *);
/* Short program name in the title: "Label - name" with non-ASCII as '?'. */
void acc_title(char *out, size_t cap, const char *label, const char *path);
const char *acc_basename(const char *path);
#endif
