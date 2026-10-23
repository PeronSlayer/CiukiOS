/* Origin: X.Org xf86-video-nv commit
 * 0b6619fec39fada995646fb95bb25e72d3c9a491, src/nv_hw.c NVLoadStateExt.
 * NV30/NV40 local-VRAM objects and engine setup; native bounded transport.
 * Firmware mode, routing, clocks and panels remain untouched. */
/*
 * Copyright (c) 1993-2003 NVIDIA, Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "nv2d.h"
#include <ciuki/kernel.h>
#define W(r, v) s->io.write(s->io.ctx, (r), (v))
#define R(r) s->io.read(s->io.ctx, (r))
int nv2d_dma_setup(struct nv2d *s)
{
    if (R(0x3220) || R(NV2D_GRAPH_BUSY))
        return -EBUSY;
    uint32_t enabled = R(0x200);
    if (enabled == UINT32_MAX)
        return -EIO;
    s->touched = true;
    s->cpu_safe = false;
    W(0x200, enabled & ~0x1100u);
    W(0x200, enabled | 0x1100u);
    W(0x3200, 0);
    W(0x3250, 0);
    W(0x3220, 0);
    W(0x2500, 0);
    /* RAMHT and object storage belong solely to our stopped private channel. */
    for (unsigned n = 0; n < 0x400; n++)
        W(0x710000 + 4 * n, 0);
    for (unsigned n = 0x800; n < 0x850; n++)
        W(0x710000 + 4 * n, 0);
    for (unsigned n = 0; n < NV2D_RING_WORDS; n++)
        s->ring[n] = 0;
    s->current = s->put = 0;
    s->io.drain(s->io.ctx);
    (void)s->ring[0];
    W(NV2D_PUT, 0);
    W(NV2D_GET, 0);
    if (R(NV2D_PUT) || R(NV2D_GET))
        return -EIO;
    int i, j;
    if ((s->architecture < 0x40) || ((s->device & 0xfff0) == 0x0040)) {
        for (i = 0; i < 8; i++) {
            W(0x100000 + 4 * ((0x0240 + (i * 0x10)) / 4), 0);
            W(0x100000 + 4 * ((0x0244 + (i * 0x10)) / 4), s->vram_bytes - 1);
        }
    } else {
        int regions = 12;

        for (i = 0; i < regions; i++) {
            W(0x100000 + 4 * ((0x0600 + (i * 0x10)) / 4), 0);
            W(0x100000 + 4 * ((0x0604 + (i * 0x10)) / 4), s->vram_bytes - 1);
        }
    }

    /* RAMHT handles and context layout from NVLoadStateExt. NV40 uses
     * eight-word records (two instance units), NV30 four-word records. */
    const uint32_t nv30[8][4] = {
        {0x01008062, 0, 0x12001200, 0}, {0x01008043, 0, 0, 0},
        {0x01008044, 2, 0, 0},          {0x01008019, 0, 0, 0},
        {0x0100a05c, 0, 0, 0},          {0x0100809f, 0, 0x12001200, 0},
        {0x0100804a, 2, 0, 0},          {0x01018077, 0, 0x12001200, 0}};
    const uint32_t nv40[8][8] = {{0x02080062, 0, 0x1200, 0x1200, 0, 0, 0, 0},
                                 {0x02080043, 0, 0, 0, 0, 0, 0, 0},
                                 {0x02080044, 0x02000000, 0, 0, 0, 0, 0, 0},
                                 {0x02080019, 0, 0, 0, 0, 0, 0, 0},
                                 {0x020a005c, 0, 0, 0, 0, 0, 0, 0},
                                 {0x0208009f, 0, 0x1200, 0x1200, 0, 0, 0, 0},
                                 {0x0208004a, 0x02000000, 0, 0, 0, 0, 0, 0},
                                 {0x02080077, 0, 0x1200, 0x1200, 0, 0, 0, 0}};
    bool nv40_chip = s->architecture >= 0x40;
    unsigned stride = nv40_chip ? 8 : 4;
    for (unsigned n = 0; n < 8; n++) {
        W(0x710000 + 8 * n, 0x80000010 + n);
        W(0x710004 + 8 * n, nv40_chip ? 0x00101202 + 2 * n : 0x80011201 + n);
        for (unsigned k = 0; k < stride; k++)
            W(0x712000 + 4 * (stride * (n + 1) + k),
              nv40_chip ? nv40[n][k] : nv30[n][k]);
    }
    W(0x712000, 0x3000);
    W(0x712004, s->vram_bytes - 1);
    W(0x712008, 2);
    if (!nv40_chip)
        W(0x71200c, 2);
    uint32_t fetch = nv40_chip ? 0x712130 : 0x712090;
    W(fetch, 0x3002);
    W(fetch + 4, 0x7fff);
    W(fetch + 8, s->ring_offset | 2);
    if (!nv40_chip)
        W(fetch + 12, 2);

    W(0x400080, 0xFFFFFFFF);
    W(0x400080, 0x00000000);

    W(0x400140, 0x00000000);
    W(0x400100, 0xFFFFFFFF);
    W(0x400144, 0x10010100);
    W(0x400714, 0xFFFFFFFF);
    W(0x400720, 0x00000001);
    W(0x400710, R(0x400710) & (0x0007ff00));
    W(0x400710, R(0x400710) | (0x00020100));

    if (s->architecture >= 0x40) {
        W(0x400084, 0x401287c0);
        W(0x40008c, 0x60de8051);
        W(0x400090, 0x00008000);
        W(0x400610, 0x00be3c5f);
        W(0x400bc4, R(0x400bc4) | (0x00008000));

        j = R(0x001540) & 0xff;
        if (j) {
            for (i = 0; !(j & 1); j >>= 1, i++)
                ;
            W(0x405000, i);
        }

        if ((s->device & 0xfff0) == 0x0040) {
            W(0x4009b0, 0x83280fff);
            W(0x4009b4, 0x000000a0);
        } else {
            W(0x400820, 0x83280eff);
            W(0x400824, 0x000000a0);
        }

        switch (s->device & 0xfff0) {
        case 0x0040:
        case 0x0210:
            W(0x4009b8, 0x0078e366);
            W(0x4009bc, 0x0000014c);
            W(0x10033c, R(0x10033c) & (0xffff7fff));
            break;
        case 0x00C0:
            W(0x400828, 0x007596ff);
            W(0x40082c, 0x00000108);
            break;
        case 0x0160:
            W(0x001700, R(0x10020c));
            W(0x001704, 0);
            W(0x001708, 0);
            W(0x00170c, R(0x10020c));
            W(0x400860, 0);
            W(0x400864, 0);
            W(0x680608, R(0x680608) | (0x00100000));
            break;
        case 0x0140:
            W(0x400828, 0x0072cb77);
            W(0x40082c, 0x00000108);
            break;
        case 0x0220:
            W(0x400860, 0);
            W(0x400864, 0);
            W(0x680608, R(0x680608) | (0x00100000));
            break;
        default:
            break;
        };

        W(0x400b38, 0x2ffff800);
        W(0x400b3c, 0x00006000);
        W(0x40032c, 0x01000000);
        W(0x400220, 0x00001200);
    } else if (s->architecture == 0x30) {
        W(0x400084, 0x40108700);
        W(0x400890, 0x00140000);
        W(0x40008c, 0xf00e0431);
        W(0x400090, 0x00008000);
        W(0x400610, 0xf04b1f36);
        W(0x400b80, 0x1002d888);
        W(0x400b88, 0x62ff007f);
    }

    if ((s->architecture < 0x40) || ((s->device & 0xfff0) == 0x0040)) {
        for (i = 0; i < 32; i++) {
            W(0x400900 + 4 * i, R(0x100240 + 4 * i));
            W(0x406900 + 4 * i, R(0x100240 + 4 * i));
        }
    } else {

        for (i = 0; i < 48; i++) {
            W(0x400900 + 4 * i, R(0x100600 + 4 * i));
            if (((s->device & 0xfff0) != 0x0160) &&
                ((s->device & 0xfff0) != 0x0220)) {
                W(0x406900 + 4 * i, R(0x100600 + 4 * i));
            }
        }
    }

    if (s->architecture >= 0x40) {
        if ((s->device & 0xfff0) == 0x0040) {
            W(0x4009a4, R(0x100200));
            W(0x4009a8, R(0x100204));
            W(0x4069a4, R(0x100200));
            W(0x4069a8, R(0x100204));

            W(0x400820, 0);
            W(0x400824, 0);
            W(0x400864, s->vram_bytes - 1);
            W(0x400868, s->vram_bytes - 1);
        } else {

            W(0x4009f0, R(0x100200));
            W(0x4009f4, R(0x100204));

            W(0x4069f0, R(0x100200));
            W(0x4069f4, R(0x100204));

            W(0x400840, 0);
            W(0x400844, 0);
            W(0x4008a0, s->vram_bytes - 1);
            W(0x4008a4, s->vram_bytes - 1);
        }
    } else {
        W(0x4009a4, R(0x100200));
        W(0x4009a8, R(0x100204));
        W(0x400750, 0x00EA0000);
        W(0x400754, R(0x100200));
        W(0x400750, 0x00EA0004);
        W(0x400754, R(0x100204));

        W(0x400820, 0);
        W(0x400824, 0);
        W(0x400864, s->vram_bytes - 1);
        W(0x400868, s->vram_bytes - 1);
    }

    W(0x400b20, 0x00000000);
    W(0x400b04, 0xFFFFFFFF);

    W(0x40053c, 0);
    W(0x400540, 0);
    W(0x400544, 0x00007FFF);
    W(0x400548, 0x00007FFF);

    W(0x002500, 0x00000000);
    W(0x002504, 0x00000001);
    W(0x003200, 0x00000000);
    W(0x003250, 0x00000000);
    if (s->architecture >= 0x40)
        W(0x003204, 0x00010000);
    else
        W(0x003204, 0x00000100);
    W(0x003240, 0x00000000);
    W(0x003244, 0x00000000);
    if (s->architecture >= 0x40)
        W(0x00322c, 0x00001213);
    else
        W(0x00322c, 0x00001209);
    W(0x003000, 0x00000000);
    W(0x003050, 0x00000000);
    W(0x002210, 0x03000100);
    W(0x002214, 0x00000110);
    W(0x002218, 0x00000112);
    W(0x00250c, 0x0000FFFF);
    W(0x003258, 0x0000FFFF);
    W(0x002140, 0x00000000);
    W(0x002100, 0xFFFFFFFF);
    W(0x003054, 0x00000001);
    W(0x003230, 0x00000000);
    W(0x003280, 0x00000000);
    W(0x003224, 0x000F0078);
    W(0x003220, 0x00000001);
    W(0x003200, 0x00000001);
    W(0x003250, 0x00000001);
    W(0x003254, 0x00000001);
    W(0x002500, 0x00000001);

    s->ring_ready = true;
    /* Skip area is immutable NOP padding. Bindings are emitted by the common
     * setup and the first PUT includes the padding, just as NVResetGraphics. */
    s->current = NV2D_SKIPS;
    return 0;
}
#undef W
#undef R
