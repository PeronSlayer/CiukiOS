/*
 * SiS hardware cursor handling
 * Definitions
 *
 * Copyright (C) 2001-2005 by Thomas Winischhofer, Vienna, Austria.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1) Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2) Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3) The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Author:   Thomas Winischhofer <thomas@winischhofer.net>
 *
 * Idea based on code by Can-Ru Yeou, SiS Inc.
 *
 */

/* Origin: X.Org xf86-video-sis commit
 * 0f50f8c3db2b9f1c9d4ecab8ad278e9db6418a92, src/sis_cursor.h and
 * sis_cursor.c. CiukiOS adaptation: CRT1, 64x64 monochrome storage,
 * legacy 32x32 coverage/image input; 315 requires coordinate refreshes. */
#include <ciuki/kernel.h>
#include "sis.h"
#define CURSOR_ENABLE 0x40000000u
static uint32_t readreg(struct sis_engine *s,unsigned n)
{ return s->io.read(s->io.ctx,0x8500+n*4,32); }
static void writereg(struct sis_engine *s,unsigned n,uint32_t v)
{ s->io.write(s->io.ctx,0x8500+n*4,32,v); }
static bool control(struct sis_engine *s,uint32_t value)
{
    writereg(s,0,value);
    if (s->generation==SIS_315) {
        writereg(s,3,s->cursor_regs[3]); writereg(s,4,s->cursor_regs[4]);
    }
    uint32_t actual=readreg(s,0);
    if (actual==UINT32_MAX || actual!=value) return false;
    s->cursor_regs[0]=value;
    s->cursor_enabled=(value&CURSOR_ENABLE)!=0;
    return true;
}
bool sis_cursor_hide(struct sis_engine *s)
{
    uint32_t value=readreg(s,0);
    if (value==UINT32_MAX) return false;
    if (s->generation==SIS_315) {
        s->cursor_regs[3]=readreg(s,3); s->cursor_regs[4]=readreg(s,4);
        if (s->cursor_regs[3]==UINT32_MAX || s->cursor_regs[4]==UINT32_MAX) return false;
    }
    return control(s,value&~CURSOR_ENABLE);
}
static int fault(struct sis_engine *s)
{ s->cpu_safe=false; sis_quarantine(s,"cursor_readback_failed"); return -EIO; }
int sis_cursor_show(void *ctx,bool show)
{
    struct sis_engine *s=ctx;
    if (!s->cursor_ok || !s->cursor) return -EOPNOTSUPP;
    if (!s->active || s->quarantined) return -EIO;
    if (show && !s->cursor_loaded) return -EINVAL;
    uint32_t value=s->cursor_regs[0]&0x0fffffffu;
    if (show) value|=CURSOR_ENABLE;
    return control(s,value) ? 0 : fault(s);
}
static uint32_t colour(struct sis_engine *s,uint32_t value)
{
    if (s->fb.bpp==32) return value&0xffffff;
    unsigned r=(value>>11)&31, g=(value>>5)&63, b=value&31;
    return ((r*255+15)/31<<16)|((g*255+31)/63<<8)|(b*255+15)/31;
}
int sis_cursor_set(void *ctx,const struct accel_cursor *c)
{
    struct sis_engine *s=ctx;
    if (!c || !s->cursor_ok || !s->cursor) return -EOPNOTSUPP;
    if (!s->active || s->quarantined) return -EIO;
    int e=sis_sync(s); if (e) return e;
    if (!sis_cursor_hide(s)) return fault(s);
    s->cursor_loaded=false;
    /* Hardware: MSB first, 64 AND bits followed by 64 XOR/image bits
     * per row. Legacy input mask means coverage, unlike part C AND rows. */
    for (unsigned y=0;y<64;y++) {
        uint32_t mask=y<32 ? c->mask[y] : 0;
        uint32_t image=y<32 ? c->image[y]&mask : 0;
        for (unsigned b=0;b<8;b++) {
            s->cursor[y*16+b]=b<4 ? (uint8_t)(~mask>>(24-b*8)) : 0xff;
            s->cursor[y*16+8+b]=b<4 ? (uint8_t)(image>>(24-b*8)) : 0;
        }
    }
    s->io.drain(s->io.ctx);
    s->cursor_regs[1]=colour(s,c->background); s->cursor_regs[2]=colour(s,c->foreground);
    writereg(s,1,s->cursor_regs[1]); writereg(s,2,s->cursor_regs[2]);
    uint32_t address=s->cursor_offset>>10;
    uint32_t mask=s->generation==SIS_300 ? 0xf0ff0000u : 0xf0f00000u;
    uint32_t value=(s->cursor_regs[0]&mask&0x0fffffffu)|address;
    if (!control(s,value) || readreg(s,1)!=s->cursor_regs[1] || readreg(s,2)!=s->cursor_regs[2]) return fault(s);
    s->cursor_loaded=true;
    return 0;
}
int sis_cursor_move(void *ctx,int32_t x,int32_t y)
{
    struct sis_engine *s=ctx;
    if (!s->cursor_ok || !s->cursor) return -EOPNOTSUPP;
    if (!s->active || s->quarantined) return -EIO;
    if (x < -63 || y < -63 || x>2047 || y>2047) return -EOPNOTSUPP;
    uint32_t px=x<0 ? (uint32_t)(-x)<<16 : (uint32_t)x;
    uint32_t py=y<0 ? (uint32_t)(-y)<<16 : (uint32_t)y;
    writereg(s,3,px); writereg(s,4,py);
    if (readreg(s,3)!=px || readreg(s,4)!=py) return fault(s);
    s->cursor_regs[3]=px; s->cursor_regs[4]=py;
    return 0;
}
