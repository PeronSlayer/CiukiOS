/*
 * Copyright 2000 ATI Technologies Inc., Markham, Ontario, and
 *                VA Linux Systems Inc., Fremont, California.
 *
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation on the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NON-INFRINGEMENT.  IN NO EVENT SHALL ATI, VA LINUX SYSTEMS AND/OR
 * THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/* Origin: X.Org ATI xf86-video-ati-6.14.6, src/radeon_cursor.c,
 * legacy hide/show and locked position sequence. Adapted for the legacy
 * Ciuki coverage/selector cursor, not the donor ARGB payload.
 * Monochrome plane layout: QEMU v9.2.0 hw/display/ati.c, ati_cursor_define.
 * Copyright (c) 2019 BALATON Zoltan; GPL version 2 or later.
 * Ciuki adaptation: SPDX-License-Identifier: GPL-2.0-only
 */
#include <ciuki/kernel.h>
#include "engine.h"

int radeon_cursor_hide(struct radeon_engine *s)
{
    if (!s->touched) return 0;
    uint32_t crtc=s->io.read(s->io.ctx, CRTC_GEN_CNTL);
    if (crtc == UINT32_MAX) return -EIO;
    s->io.write(s->io.ctx, CRTC_GEN_CNTL, crtc & ~CRTC_CUR_EN);
    s->cursor_hidden=!(s->io.read(s->io.ctx, CRTC_GEN_CNTL) & CRTC_CUR_EN);
    return s->cursor_hidden ? 0 : -EIO;
}
static int cursor_fault(struct radeon_engine *s)
{
    s->cpu_safe=false;
    radeon_engine_quarantine(s, "cursor_readback_failed");
    return -EIO;
}
static bool cursor_ready(struct radeon_engine *s)
{ return s->active && !s->quarantined && s->cursor; }
int radeon_cursor_show(void *ctx, bool show)
{
    struct radeon_engine *s=ctx;
    if (!cursor_ready(s)) return -EOPNOTSUPP;
    if (show && !s->cursor_loaded) return -EINVAL;
    uint32_t crtc=s->io.read(s->io.ctx, CRTC_GEN_CNTL);
    if (crtc == UINT32_MAX) return cursor_fault(s);
    /* Monochrome mode 0, preserving the firmware's pixel/scanout settings. */
    uint32_t next=(crtc & ~(CRTC_CUR_EN | 0x00700000u)) | (show ? CRTC_CUR_EN : 0u);
    s->io.write(s->io.ctx, CRTC_GEN_CNTL, next);
    uint32_t check=s->io.read(s->io.ctx, CRTC_GEN_CNTL);
    if ((check & (CRTC_CUR_EN | 0x00700000u)) != (next & (CRTC_CUR_EN | 0x00700000u)))
        return cursor_fault(s);
    s->cursor_hidden=!show;
    return 0;
}
static uint32_t rgb(const struct fb_device *d, uint32_t colour)
{
    uint32_t out=0;
    unsigned sizes[3]={d->blue_size,d->green_size,d->red_size};
    unsigned positions[3]={d->blue_pos,d->green_pos,d->red_pos};
    for (unsigned i=0; i<3; i++) {
        unsigned mask=(1u<<sizes[i])-1u, v=(colour>>positions[i]) & mask;
        out |= ((v*255u+mask/2u)/mask) << (i*8u);
    }
    return out;
}
int radeon_cursor_move(void *ctx, int32_t x, int32_t y)
{
    struct radeon_engine *s=ctx;
    if (!cursor_ready(s)) return -EOPNOTSUPP;
    /* QEMU v9.2.0 explicitly lacks correct negative-origin cropping.
     * Refuse before a register write; the desktop retains cursor ownership. */
    if (x<0 || y<0) return -EOPNOTSUPP;
    if (x>8191 || y>4095) return -EINVAL;
    if (!s->cursor_loaded) return -EINVAL;
    s->io.write(s->io.ctx, CUR_HORZ_VERT_OFF, CUR_LOCK);
    s->io.write(s->io.ctx, CUR_HORZ_VERT_POSN, CUR_LOCK | ((uint32_t)x<<16) | (uint32_t)y);
    s->io.write(s->io.ctx, CUR_OFFSET, s->cursor_offset);
    if ((s->io.read(s->io.ctx, CUR_HORZ_VERT_OFF) & ~CUR_LOCK) ||
        (s->io.read(s->io.ctx, CUR_HORZ_VERT_POSN) & ~CUR_LOCK) != (((uint32_t)x<<16) | (uint32_t)y) ||
        s->io.read(s->io.ctx, CUR_OFFSET) != s->cursor_offset) return cursor_fault(s);
    return 0;
}
int radeon_cursor_set(void *ctx, const struct accel_cursor *c)
{
    struct radeon_engine *s=ctx;
    if (!c) return -EINVAL;
    if (!cursor_ready(s)) return -EOPNOTSUPP;
    int e=radeon_engine_idle(s);
    if (e) return e;
    if (radeon_cursor_hide(s)) return cursor_fault(s);
    s->cursor_loaded=false;
    /* 64 rows, eight AND bytes followed by eight XOR bytes. Leftmost
     * bit is MSB, matching QEMU's model and the legacy register format.
     * Every pixel outside Ciuki's 32x32 is transparent. */
    for (unsigned y=0; y<64; y++) for (unsigned b=0; b<8; b++) {
        uint8_t coverage=0, image=0;
        if (y<32 && b<4) {
            coverage=(uint8_t)(c->mask[y] >> (24u-b*8u));
            image=(uint8_t)(c->image[y] >> (24u-b*8u)) & coverage;
        }
        ((volatile uint8_t *)s->cursor)[y*16+b]=(uint8_t)~coverage;
        ((volatile uint8_t *)s->cursor)[y*16+8+b]=image;
    }
    s->io.drain(s->io.ctx);
    uint32_t bg=rgb(&s->fb,c->background), fg=rgb(&s->fb,c->foreground);
    s->io.write(s->io.ctx, CUR_CLR0, bg);
    s->io.write(s->io.ctx, CUR_CLR1, fg);
    s->io.write(s->io.ctx, CUR_OFFSET, s->cursor_offset);
    if (s->io.read(s->io.ctx, CUR_CLR0) != bg || s->io.read(s->io.ctx, CUR_CLR1) != fg ||
        s->io.read(s->io.ctx, CUR_OFFSET) != s->cursor_offset) return cursor_fault(s);
    s->cursor_loaded=true;
    return radeon_cursor_move(s,0,0);
}
