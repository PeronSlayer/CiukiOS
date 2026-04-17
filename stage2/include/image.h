#ifndef STAGE2_IMAGE_H
#define STAGE2_IMAGE_H

#include "types.h"

typedef struct image_info {
    u32 width;
    u32 height;
    u32 bpp;        /* source bpp: 24 or 32 */
} image_info_t;

/*
 * Decode a Windows BMP (BITMAPINFOHEADER, BI_RGB, 24bpp or 32bpp,
 * top-down or bottom-up) into a 32bpp 0x00RRGGBB pixel buffer owned
 * by the decoder (static scratch). Returns NULL on error.
 *
 * The returned pointer is valid until the next image_bmp_decode call.
 */
const u32 *image_bmp_decode(const void *data, u32 size, image_info_t *out_info);

#endif /* STAGE2_IMAGE_H */
