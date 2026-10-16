/* Native display-service provider; never called by guest firmware.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_SUPERSAVAGE_DISPLAY_H
#define CIUKI_SUPERSAVAGE_DISPLAY_H
#include <stdint.h>
#include <stdbool.h>
struct fb_device;
bool supersavage_display_supported(const struct fb_device *, uint16_t mode);
/* Software preflight, before desktop grants/acceleration are changed. */
int supersavage_display_prepare(const struct fb_device *, uint16_t mode);
/* The caller holds the fbdev mode transaction and has stopped the engine. */
int supersavage_display_begin(uint16_t mode);
int supersavage_display_end(void);
/* Final desktop commit/fallback result, after end has revoked CPU aliases. */
void supersavage_display_complete(bool recovered);
/* CPU-only packed writes under the caller's quiesced framebuffer lease. */
int supersavage_display_write(uint32_t offset, const void *, uint32_t bytes);
#endif
