/* Guest VBE modes implemented by native display providers.
 * Timings: VESA DMT 640x480@60 and 800x600@60; programming follows
 * The mode-driver implementation documents its upstream timing sources.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_DOS_VIDEO_MODES_H
#define CIUKI_DOS_VIDEO_MODES_H
#include <stdint.h>
struct dos_video_mode {
    uint16_t number, width, height, pitch;
    uint8_t bpp;
    uint32_t clock_khz;
};
#define DOS_VBE_BYTES (1024u * 1024u)
/* A VM-local physical token, never a host BAR or firmware pointer. */
#define DOS_VBE_PHYS 0xE0000000u
static inline const struct dos_video_mode *dos_video_mode(uint16_t number)
{
    static const struct dos_video_mode modes[] = {
        {0x101,640,480,640,8,25175}, {0x103,800,600,800,8,40000},
        {0x111,640,480,1280,16,25175}, {0x114,800,600,1600,16,40000}
    };
    for (unsigned i=0;i<sizeof(modes)/sizeof(modes[0]);i++)
        if (modes[i].number==number) return &modes[i];
    return 0;
}
static inline uint32_t dos_video_frame_us(const struct dos_video_mode *m)
{
    unsigned pixels=m->width==640 ? 800u*525u : 1056u*628u;
    return pixels*1000u/m->clock_khz;
}
#endif
