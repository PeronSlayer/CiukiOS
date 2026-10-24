/* SPDX-License-Identifier: MIT
 * WordPad picture decoding and encoding (G28b): stb_image v2.30 and stb_image_write (third_party/stb, MIT or Public
 * Domain) for PNG, JPEG, BMP and GIF. Pixels are 0x00RRGGBB, row-major, top-down; transparency is composited on white. */
#ifndef CIUKI_WORDPAD_IMAGE_H
#define CIUKI_WORDPAD_IMAGE_H
#include "rich.h"
#define WP_IMAGE_SIDE 8192
#define WP_IMAGE_PIXELS (4096u * 4096u)   /* largest picture accepted */
#define WP_IMAGE_KEEP (4u * 1024u * 1024u)  /* decoded pixels kept for display and printing: a larger picture is reduced by a whole factor */
/* The pixels of a picture larger than WP_IMAGE_KEEP are reduced by the smallest whole factor that fits; the caller keeps the natural
 * size from the file's header. Decodes a picture of the given kind (WP_PIC_PNG, WP_PIC_JPEG, WP_PIC_DIB) or, with kind 0, the bytes of an image file
 * (PNG, JPEG, BMP, GIF). 0, -EINVAL (not a decodable image), -ENOTSUP (a metafile), -EFBIG (too large), -ENOMEM. The pixels are malloc'd. */
int wp_image_decode(unsigned kind, const uint8_t *data, size_t len, uint32_t **px, unsigned *w, unsigned *h);
/* A PNG file of the pixels (malloc'd). */
int wp_image_png(const uint32_t *px, unsigned w, unsigned h, uint8_t **out, size_t *len);
#endif
