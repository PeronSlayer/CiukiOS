/* Native display-service provider; never called by guest firmware.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_SUPERSAVAGE_DISPLAY_H
#define CIUKI_SUPERSAVAGE_DISPLAY_H
#include <stdint.h>
struct fb_device;
/* Software preflight, before desktop grants/acceleration are changed. */
int supersavage_display_prepare(const struct fb_device *, uint16_t mode);
/* The caller holds the fbdev mode transaction and has stopped the engine. */
int supersavage_display_begin(uint16_t mode);
int supersavage_display_end(void);
#endif
