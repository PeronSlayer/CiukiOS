/* Origin: X.Org xf86-video-nv commit
 * 0b6619fec39fada995646fb95bb25e72d3c9a491, src/nv_hw.c
 * NVLoadStateExt: pre-NV40 objects and graph setup, adapted to private I/O.
 * PIO channel setup additionally uses the separately licensed pio_table.h.
 * Firmware owns scanout, PLLs and panel state. No DMA command-fetch object. */
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
#include <ciuki/kernel.h>
#include "nv2d.h"
#include "pio_table.h"
#define W(r,v) s->io.write(s->io.ctx,(r),(v))
#define R(r) s->io.read(s->io.ctx,(r))
int nv2d_setup(struct nv2d *s)
{
    if (s->dma) {
        int e=nv2d_dma_setup(s);
        if (e) return e;
        goto methods;
    }
    /* Reject a firmware DMA channel instead of stealing it or resetting a
     * potentially outstanding bus transaction. No write on this refusal. */
    if (R(0x3220) || R(NV2D_GRAPH_BUSY)) return -EBUSY;
    s->touched=true; s->cpu_safe=false;
    uint32_t enabled=R(0x200);
    if (enabled==UINT32_MAX) return -EIO;
    W(0x200,enabled & ~0x00001100u); W(0x200,enabled | 0x00001100u);
    W(0x2100,0xffffffff); W(0x2140,0);
    /* Disable tiling only after rejecting active firmware DMA. Preserve
     * pitch/scanout registers and every clock/panel register. */
    if (s->architecture>=0x10) for (unsigned i=0;i<8;i++) {
        W(0x100240+i*16,0); W(0x100244+i*16,s->vram_bytes-1);
    }
       W(0x710000 + 4*0x0000, 0x80000010);
       W(0x710000 + 4*0x0001, 0x80011201);
       W(0x710000 + 4*0x0002, 0x80000011);
       W(0x710000 + 4*0x0003, 0x80011202);
       W(0x710000 + 4*0x0004, 0x80000012);
       W(0x710000 + 4*0x0005, 0x80011203);
       W(0x710000 + 4*0x0006, 0x80000013);
       W(0x710000 + 4*0x0007, 0x80011204);
       W(0x710000 + 4*0x0008, 0x80000014);
       W(0x710000 + 4*0x0009, 0x80011205);
       W(0x710000 + 4*0x000A, 0x80000015);
       W(0x710000 + 4*0x000B, 0x80011206);
       W(0x710000 + 4*0x000C, 0x80000016);
       W(0x710000 + 4*0x000D, 0x80011207);
       W(0x710000 + 4*0x000E, 0x80000017);
       W(0x710000 + 4*0x000F, 0x80011208);
       W(0x710000 + 4*0x0800, 0x00003000);
       W(0x710000 + 4*0x0801, s->vram_bytes - 1);
       W(0x710000 + 4*0x0802, 0x00000002);
       W(0x710000 + 4*0x0803, 0x00000002);
       if(s->architecture >= 0x10)
          W(0x710000 + 4*0x0804, 0x01008062);
       else
          W(0x710000 + 4*0x0804, 0x01008042);
       W(0x710000 + 4*0x0805, 0x00000000);
       W(0x710000 + 4*0x0806, 0x12001200);
       W(0x710000 + 4*0x0807, 0x00000000);
       W(0x710000 + 4*0x0808, 0x01008043);
       W(0x710000 + 4*0x0809, 0x00000000);
       W(0x710000 + 4*0x080A, 0x00000000);
       W(0x710000 + 4*0x080B, 0x00000000);
       W(0x710000 + 4*0x080C, 0x01008044);
       W(0x710000 + 4*0x080D, 0x00000002);
       W(0x710000 + 4*0x080E, 0x00000000);
       W(0x710000 + 4*0x080F, 0x00000000);
       W(0x710000 + 4*0x0810, 0x01008019);
       W(0x710000 + 4*0x0811, 0x00000000);
       W(0x710000 + 4*0x0812, 0x00000000);
       W(0x710000 + 4*0x0813, 0x00000000);
       W(0x710000 + 4*0x0814, 0x0100A05C);
       W(0x710000 + 4*0x0815, 0x00000000);
       W(0x710000 + 4*0x0816, 0x00000000);
       W(0x710000 + 4*0x0817, 0x00000000);
       /* No vblank methods are exposed by this synchronous port. */
       W(0x710000 + 4*0x0818, 0x0100805F);
       W(0x710000 + 4*0x0819, 0x00000000);
       W(0x710000 + 4*0x081A, 0x12001200);
       W(0x710000 + 4*0x081B, 0x00000000);
       W(0x710000 + 4*0x081C, 0x0100804A);
       W(0x710000 + 4*0x081D, 0x00000002);
       W(0x710000 + 4*0x081E, 0x00000000);
       W(0x710000 + 4*0x081F, 0x00000000);
       W(0x710000 + 4*0x0820, 0x01018077);
       W(0x710000 + 4*0x0821, 0x00000000);
       W(0x710000 + 4*0x0822, 0x12001200);
       W(0x710000 + 4*0x0823, 0x00000000);

    if (s->architecture == 4) {
       W(0x400000+4*(0x0080/4), 0x000001FF);
       W(0x400000+4*(0x0080/4), 0x1230C000);
       W(0x400000+4*(0x0084/4), 0x72111101);
       W(0x400000+4*(0x0088/4), 0x11D5F071);
       W(0x400000+4*(0x008C/4), 0x0004FF31);
       W(0x400000+4*(0x008C/4), 0x4004FF31);

       W(0x400000+4*(0x0140/4), 0x00000000);
       W(0x400000+4*(0x0100/4), 0xFFFFFFFF);
       W(0x400000+4*(0x0170/4), 0x10010100);
       W(0x400000+4*(0x0710/4), 0xFFFFFFFF);
       W(0x400000+4*(0x0720/4), 0x00000001);

       W(0x400000+4*(0x0810/4), 0x00000000);
       W(0x400000+4*(0x0608/4), 0xFFFFFFFF);
    } else {
       W(0x400000+4*(0x0080/4), 0xFFFFFFFF);
       W(0x400000+4*(0x0080/4), 0x00000000);

       W(0x400000+4*(0x0140/4), 0x00000000);
       W(0x400000+4*(0x0100/4), 0xFFFFFFFF);
       W(0x400000+4*(0x0144/4), 0x10010100);
       W(0x400000+4*(0x0714/4), 0xFFFFFFFF);
       W(0x400000+4*(0x0720/4), 0x00000001);
       W(0x400000+4*(0x0710/4),R(0x400000+4*(0x0710/4)) & (0x0007ff00));
       W(0x400000+4*(0x0710/4),R(0x400000+4*(0x0710/4)) | (0x00020100));

        if (s->architecture == 0x10) {
           W(0x400000+4*(0x0084/4), 0x00118700);
           W(0x400000+4*(0x0088/4), 0x24E00810);
           W(0x400000+4*(0x008C/4), 0x55DE0030);

           for(unsigned i = 0; i < 32; i++)
             W(0x400000+4*((0x0B00/4) + i), R(0x100000+4*((0x0240/4) + i)));

           W(0x400000+4*(0x640/4), 0);
           W(0x400000+4*(0x644/4), 0);
           W(0x400000+4*(0x684/4), s->vram_bytes - 1);
           W(0x400000+4*(0x688/4), s->vram_bytes - 1);

           W(0x400000+4*(0x0810/4), 0x00000000);
           W(0x400000+4*(0x0608/4), 0xFFFFFFFF);
        } else {
              W(0x400000+4*(0x0084/4), 0x00118700);
              W(0x400000+4*(0x008C/4), 0xF20E0431);
              W(0x400000+4*(0x0090/4), 0x00000000);
              W(0x400000+4*(0x009C/4), 0x00000040);

              if((s->device & 0x0ff0) >= 0x0250) {
                 W(0x400000+4*(0x0890/4), 0x00080000);
                 W(0x400000+4*(0x0610/4), 0x304B1FB6);
                 W(0x400000+4*(0x0B80/4), 0x18B82880);
                 W(0x400000+4*(0x0B84/4), 0x44000000);
                 W(0x400000+4*(0x0098/4), 0x40000080);
                 W(0x400000+4*(0x0B88/4), 0x000000ff);
              } else {
                 W(0x400000+4*(0x0880/4), 0x00080000);
                 W(0x400000+4*(0x0094/4), 0x00000005);
                 W(0x400000+4*(0x0B80/4), 0x45CAA208);
                 W(0x400000+4*(0x0B84/4), 0x24000000);
                 W(0x400000+4*(0x0098/4), 0x00000040);
                 W(0x400000+4*(0x0750/4), 0x00E00038);
                 W(0x400000+4*(0x0754/4), 0x00000030);
                 W(0x400000+4*(0x0750/4), 0x00E10038);
                 W(0x400000+4*(0x0754/4), 0x00000030);
              }
            for (unsigned i=0;i<32;i++) {
                W(0x400900+4*i,R(0x100240+4*i));
                W(0x406900+4*i,R(0x100240+4*i));
            }
            W(0x4009a4,R(0x100200)); W(0x4009a8,R(0x100204));
            W(0x400820,0); W(0x400824,0);
            W(0x400864,s->vram_bytes-1); W(0x400868,s->vram_bytes-1);
            W(0x400b20,0); W(0x400b04,0xffffffff);

        }
    }

    W(0x40053c,0); W(0x400540,0); W(0x400544,0x7fff); W(0x400548,0x7fff);
    for (unsigned i=0;i<ARRAY_SIZE(pio_fifo);i++)
        W(0x2000+4*pio_fifo[i][0],pio_fifo[i][1]);
    s->io.drain(s->io.ctx);
    uint32_t free=R(NV2D_FREE);
    if (free==UINT32_MAX || !(free&0xffff) || (free&3)) return -EIO;
    s->empty_bytes=free&0xffff;
methods:
    for (unsigned i=0;i<8;i++) {
        uint32_t handle=0x80000010+i;
        int e=nv2d_emit(s,i*0x2000,1,&handle);
        if (e) return e;
    }
    uint32_t format=s->fb.bpp==32 ? 6 : s->fb.green_size==6 ? 4 : 2;
    uint32_t surface[]={format,s->fb.pitch|(s->fb.pitch<<16),0,0};
    uint32_t packed=s->fb.bpp==16 ? 1 : 3;
    uint32_t clip[]={0,s->fb.width|(s->fb.height<<16)};
    int e=nv2d_emit(s,0x300,4,surface);
    if (!e) e=nv2d_emit(s,0x4300,1,&packed);
    if (!e) e=nv2d_emit(s,0xc300,1,&packed);
    if (!e) e=nv2d_emit(s,0x8300,1,&packed);
    if (!e) e=nv2d_emit(s,0x6300,2,clip);
    if (!e) e=nv2d_idle(s);
    return e;
}
#undef W
#undef R
