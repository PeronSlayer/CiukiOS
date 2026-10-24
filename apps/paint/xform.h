/* SPDX-License-Identifier: MIT
 * Image > Stretch/Skew (xform.c). */
#ifndef CIUKI_PAINT_XFORM_H
#define CIUKI_PAINT_XFORM_H
#include "io.h"
#define PAINT_STRETCH_MIN 1
#define PAINT_STRETCH_MAX 500
#define PAINT_SKEW_MAX 89
/* Builds a new picture (dst->pixels is malloc'ed) from src: stretch by sx and sy percent, then skew
 * horizontally and vertically by degrees; the uncovered corners take `background`.
 * 0, -EINVAL (out of range), -EFBIG (larger than PAINT_MAX_AXIS / PAINT_MAX_PIXELS) or -ENOMEM. */
int paint_stretch_skew(const struct paint_image *src, struct paint_image *dst, int sx, int sy, int skew_h, int skew_v, uint8_t background);
#endif
