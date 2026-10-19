/* Delegated-present transport. SPDX-License-Identifier: MIT */
#ifndef CIUKI_PRESENT_ABI_H
#define CIUKI_PRESENT_ABI_H
#include <ciuki/abi.h>
#include <ciuki/present.h>
#define W32_DISPLAY 0x201u
#define W32_DISPLAY_GRANT 1u
#define W32_DISPLAY_REVOKE 2u
#define W32_DISPLAY_FRAME 3u
#define W32_DISPLAY_PALETTE 4u
#define W32_DISPLAY_GAMMA 5u
/* GRANT/REVOKE: desktop only; pid is the connected Win32 client. GRANT
 * supplies listed mode and source layout; scale=0 chooses largest <=3.
 * FRAME/PALETTE/GAMMA: holder only, with exact token and framebuffer generation.
 * address: frame bytes / count RGB u32 entries / ciuki_present_gamma.
 * PALETTE updates [first, first+count), 1..256 entries. FRAME is whole surface.
 * GRANT returns token/generation/scale. Other operations return result=1.
 * error is native positive errno. No framebuffer mapping or display grant. */
struct ciuki_w32_display_args {
    struct ciuki_w32_header header;
    uint32_t action, pid, token, generation, mode, scale, address;
    struct ciuki_present_surface surface;
    uint32_t first, count, result, error;
};
_Static_assert(sizeof(struct ciuki_w32_display_args)==80,"display wire");
/* Desktop mode-selection helper: exact dimensions/depth, then exact integer
 * multiples, then the smallest enclosing 16/32-bit mode. Returns list index
 * or -ENODEV. It does not switch hardware or confer authority. */
static inline int ciuki_present_choose_mode(const struct ciuki_display_info *info,
                                            const struct ciuki_present_surface *s)
{
    if (!info || !s || !s->width || !s->height || info->mode_count>32 ||
        s->format<1 || s->format>4) return -ENODEV;
    unsigned depth=s->format==CIUKI_PRESENT_INDEX8 ? 8 :
        s->format==CIUKI_PRESENT_XRGB8888 ? 32 : 16;
    int best=-1; unsigned rank=3, area=0;
    for (unsigned i=0;i<info->mode_count;++i) {
        const struct ciuki_display_mode *m=&info->modes[i];
        if (!m->linear || (m->bpp!=16 && m->bpp!=32) || m->width<s->width ||
            m->height<s->height || m->pitch<(uint32_t)m->width*(m->bpp/8)) continue;
        unsigned r=2, a=(unsigned)m->width*m->height;
        if (m->width==s->width && m->height==s->height && m->bpp==depth) r=0;
        else if (m->width%s->width==0 && m->height%s->height==0 &&
            m->width/s->width==m->height/s->height && m->width/s->width<=3) r=1;
        if (best<0 || r<rank || (r==rank && a<area)) {best=(int)i;rank=r;area=a;}
    }
    return best<0 ? -ENODEV : best;
}
#endif
