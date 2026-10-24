/* SPDX-License-Identifier: MIT
 * Forced into the decoder library only, before any allocator declarations. */
#ifndef CIUKI_WEBP_MEMORY_H
#define CIUKI_WEBP_MEMORY_H
#include <stdlib.h>
#include "sdk/netsurf/port/image_memory.h"
#define malloc ciuki_image_malloc
#define calloc ciuki_image_calloc
#define realloc ciuki_image_realloc
#define free ciuki_image_free
#endif
