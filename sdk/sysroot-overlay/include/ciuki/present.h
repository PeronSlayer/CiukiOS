/* Shared F5 surface and delegated-present records. SPDX-License-Identifier: MIT */
#ifndef CIUKI_PRESENT_H
#define CIUKI_PRESENT_H
#include <stdint.h>
#define CIUKI_PRESENT_XRGB8888 1u
#define CIUKI_PRESENT_RGB565 2u
#define CIUKI_PRESENT_XRGB1555 3u
#define CIUKI_PRESENT_INDEX8 4u
/* Little endian, positive pitch. No user pointer or physical address here. */
struct ciuki_present_surface { uint32_t width, height, pitch, size, format; };
struct ciuki_present_gamma { uint16_t red[256], green[256], blue[256]; };
#endif
