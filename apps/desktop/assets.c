/* SPDX-License-Identifier: MIT */
#include "desktop.h"
#include <stdio.h>
int desk_load_portrait(uint32_t *pixels)
{
    FILE *f = fopen("/system/assets/ciuki-portrait.xrgb", "rb");
    if (!f) return -1;
    size_t n = DESK_PORTRAIT_SIZE * DESK_PORTRAIT_SIZE;
    int ok = fread(pixels, 4, n, f) == n && fgetc(f) == EOF && !ferror(f);
    if (fclose(f)) ok = 0;
    return ok ? 0 : -1;
}
