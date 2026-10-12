/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_PAINT_IO_H
#define CIUKI_PAINT_IO_H
#include <stdint.h>
#define PAINT_COLORS 144
#define PAINT_MAX_AXIS 2048
#define PAINT_MAX_PIXELS (2048u * 1200u)
#define PAINT_PATH_MAX 1040
enum paint_format { PAINT_BMP8, PAINT_BMP24, PAINT_PNG };
struct paint_image { int w, h; uint8_t *pixels; };
int paint_read_image(const char *, const uint8_t *palette, struct paint_image *);
int paint_write_image(const char *, enum paint_format, const struct paint_image *, const uint8_t *palette);
#endif
