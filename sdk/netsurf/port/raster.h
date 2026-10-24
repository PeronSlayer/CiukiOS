/* Private PNG/JPEG decode API. SPDX-License-Identifier: MIT */
#ifndef CIUKI_NETSURF_RASTER_H
#define CIUKI_NETSURF_RASTER_H
#include <stddef.h>
unsigned char *ciuki_raster_decode(const unsigned char *,size_t,int *,int *);
#endif
