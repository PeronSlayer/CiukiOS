/* SPDX-License-Identifier: MIT
 * Single UI-thread image ledger, shared by framebuffer bitmaps and libnsgif
 * (including LZW, frame metadata and restore buffers). */
#ifndef CIUKI_NETSURF_IMAGE_MEMORY_H
#define CIUKI_NETSURF_IMAGE_MEMORY_H
#include <stddef.h>
/* fx-14: the decoded-image ledger is an eighth of the memory free when the
 * first image is charged (docmem_query.h), between the plan's 2 MiB decoded
 * image cache and 16 MiB; the 128 MiB baseline model gives 7 MiB. */
#define CIUKI_IMAGE_MIN_BYTES (2u*1024u*1024u)
#define CIUKI_IMAGE_MAX_BYTES (16u*1024u*1024u)
size_t ciuki_image_limit(void);
#define CIUKI_IMAGE_BYTES ciuki_image_limit()
int ciuki_image_reserve(size_t);
void ciuki_image_release(size_t);
size_t ciuki_image_used(void);
void *ciuki_image_malloc(size_t);
void *ciuki_image_calloc(size_t,size_t);
void *ciuki_image_realloc(void *,size_t);
void ciuki_image_free(void *);
#endif
