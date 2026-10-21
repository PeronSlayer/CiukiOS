/* SPDX-License-Identifier: MIT
 * Single UI-thread image ledger, shared by framebuffer bitmaps and libnsgif
 * (including LZW, frame metadata and restore buffers). */
#ifndef CIUKI_NETSURF_IMAGE_MEMORY_H
#define CIUKI_NETSURF_IMAGE_MEMORY_H
#include <stddef.h>
#define CIUKI_IMAGE_BYTES (2u*1024u*1024u)
int ciuki_image_reserve(size_t);
void ciuki_image_release(size_t);
size_t ciuki_image_used(void);
void *ciuki_image_malloc(size_t);
void *ciuki_image_calloc(size_t,size_t);
void *ciuki_image_realloc(void *,size_t);
void ciuki_image_free(void *);
#endif
