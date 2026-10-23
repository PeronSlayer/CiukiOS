/* Adapted packet layout and ring publication from Haiku r1beta5,
 * a1f00118db8b858feb73f64ac4dc3d038b82bf9b:
 * src/add-ons/accelerants/intel_extreme/{engine.cpp,commands.h},
 * headers/private/graphics/intel_extreme/intel_extreme.h.
 * engine.cpp: Copyright 2006-2007, Haiku, Inc. All Rights Reserved.
 * commands.h: Copyright 2006-2008, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 * Authors: Axel Dörfler, axeld@pinc-software.de
 * Private register-header notice: Copyright 2006-2016, Haiku, Inc.
 * All Rights Reserved. Distributed under the terms of the MIT License.
 * Authors: Axel Dörfler; Alexander von Gluck, kallisti5@unixzen.com
 *
 * Breadcrumb sequence adapted from Linux v6.7
 * drivers/gpu/drm/i915/gt/gen2_engine_cs.c (gen2_emit_flush,
 * gen3_emit_breadcrumb; also selected for gen2 by intel_ring_submission.c).
 * Copyright © 2020 Intel Corporation. SPDX-License-Identifier: MIT
 * Full permissions and origins: docs/licenses/intel2d-NOTICES.txt.
 * Ciuki ownership, validation and bounded lifecycle code: GPL-2.0-only.
 */
#include <ciuki/kernel.h>
#include "intel2d.h"

#define MI_FLUSH 0x02000000u
#define MI_STORE_INDEX 0x10800001u
#define XY_FILL 0x54000004u
#define XY_COPY 0x54c00006u
#define RING_END (INTEL_RING_BYTES - 64u)

static const struct intel_chip chips[]={
    /* Amendment 2: Linux v6.7 intel-gtt.c:698-731 proves neither chip's
     * HIC semantics. X.Org's MI_FLUSH is GPU retirement, not publication.
     * Keep these gates ahead of all I/O; see the reservation evidence audit.
     * Enabling them also requires their own TOM/TSEG or TOUD/TSEG decode. */
    {0x3577,0x3575,true,0x01ffffc0u,"i830m_chipset_flush_unproven"},
    {0x2562,0x2560,true,0x01ffffc0u,0},
    {0x3582,0x3580,true,0x01ffffc0u,0},
    {0x2572,0x2570,true,0x01ffffc0u,"i865g_chipset_flush_unproven"},
    {0x2582,0x2580,false,INTEL_IDLE,0}
};
const struct intel_chip *intel_chip_lookup(uint16_t device)
{
    for (unsigned i=0;i<ARRAY_SIZE(chips);i++) if (chips[i].device==device) return &chips[i];
    return 0;
}
/* Linux v6.7 intel-gtt.c:698-731. Caller has completed WBINVD first.
 * A pre-existing flush must finish before we issue ours; never clear HIC. */
int intel_hic_flush(const struct intel_io *io, struct intel_wait *b,
                    int (*pause)(void *), void *pause_arg)
{
    for (unsigned phase=0;phase<2;phase++) {
        while (io->read(io->ctx,INTEL_HIC)&(1u<<31)) {
            uint64_t now=io->now(io->ctx);
            if ((int64_t)(now-b->deadline)>=0 || ++b->polls>=INTEL_POLL_LIMIT)
                return -ETIMEDOUT;
            if (!io->valid(io->ctx)) return -EIO;
            if (pause && now-b->slice>=1000) {
                int e=pause(pause_arg);
                if (e) return e;
                b->slice=io->now(io->ctx);
                if (!io->valid(io->ctx)) return -EIO;
                if ((int64_t)(b->slice-b->deadline)>=0) return -ETIMEDOUT;
            }
            io->relax(io->ctx);
        }
        if (!phase) {
            if (!io->valid(io->ctx)) return -EIO;
            if ((int64_t)(io->now(io->ctx)-b->deadline)>=0) return -ETIMEDOUT;
            io->write(io->ctx,INTEL_HIC,io->read(io->ctx,INTEL_HIC)|(1u<<31));
        }
    }
    return 0;
}
static uint32_t rd(struct intel_engine *s, uint32_t reg) { return s->io.read(s->io.ctx,reg); }
static void wr(struct intel_engine *s, uint32_t reg, uint32_t value)
{ s->io.write(s->io.ctx,reg,value); }
static bool valid(struct intel_engine *s)
{
    if (!s->io.valid(s->io.ctx) || rd(s,INTEL_PGTBL)!=s->pgtbl) return false;
    if (s->mapped) for (unsigned i=0;i<INTEL_DMA_PAGES;i++)
        if (s->io.pte(s->io.ctx,s->first+i)!=(s->physical+i*4096u+1u)) return false;
    if (s->touched && (rd(s,INTEL_START)!=s->gpu ||
        rd(s,INTEL_HWS)!=s->physical+INTEL_RING_BYTES ||
        rd(s,INTEL_CTL)!=((INTEL_RING_BYTES-4096u)|1u))) return false;
    return true;
}
bool intel_mode_valid(const struct fb_device *d)
{
    if (!d || !d->present || !d->mapped || !d->width || !d->height ||
        d->width>32767 || d->height>32767 || !d->pitch || d->pitch>32767 ||
        (d->pitch&3) || (d->bpp!=16 && d->bpp!=32) ||
        (uint64_t)d->width*(d->bpp/8)>d->pitch ||
        (uint64_t)d->pitch*d->height>d->size) return false;
    return d->bpp==16 ? d->red_size==5 && d->red_pos==11 &&
        d->green_size==6 && d->green_pos==5 && d->blue_size==5 && !d->blue_pos :
        d->red_size==8 && d->red_pos==16 && d->green_size==8 && d->green_pos==8 &&
        d->blue_size==8 && !d->blue_pos;
}
void intel_engine_quarantine(struct intel_engine *s, const char *why)
{
    if (!s->quarantined) s->reason=why;
    s->quarantined=true; s->active=false;
    /* Quarantine never manufactures a CPU-safety proof. */
}
static int fault(struct intel_engine *s, int error, const char *why)
{ s->cpu_safe=false; intel_engine_quarantine(s,why); return error; }

static struct intel_wait budget(struct intel_engine *s)
{
    uint64_t now=s->io.now(s->io.ctx);
    return (struct intel_wait){now+INTEL_WAIT_US,now,0};
}
static int checkpoint(struct intel_engine *s, struct intel_wait *b)
{
    uint64_t now=s->io.now(s->io.ctx);
    if ((int64_t)(now-b->deadline)>=0 || ++b->polls>=INTEL_POLL_LIMIT)
        return -ETIMEDOUT;
    if (s->pause && now-b->slice>=1000) {
        int e=s->pause(s->pause_arg);
        if (e) return e;
        b->slice=s->io.now(s->io.ctx);
        if (!valid(s)) return -EIO;
        if ((int64_t)(b->slice-b->deadline)>=0) return -ETIMEDOUT;
    }
    s->io.relax(s->io.ctx);
    return 0;
}
static bool chip_idle(struct intel_engine *s)
{
    if (s->chip->gen2) return !(rd(s,INTEL_HIC)&(1u<<31)) &&
        (rd(s,INTEL_INSTDONE)&s->chip->idle_mask)==s->chip->idle_mask;
    return (rd(s,INTEL_MODE)&s->chip->idle_mask)==s->chip->idle_mask;
}
static bool empty_idle(struct intel_engine *s)
{
    uint32_t head=rd(s,INTEL_HEAD), tail=rd(s,INTEL_TAIL);
    return !(head&3u) && !(tail&~0x001ffff8u) &&
        (head&0x001ffffcu)<INTEL_RING_BYTES && tail==s->tail &&
        (head&0x001ffffcu)==tail && chip_idle(s);
}
static int completion(struct intel_engine *s, struct intel_wait *b)
{
    s->waits++;
    for (;;) {
        if (!valid(s)) return fault(s,-EIO,"ownership_changed");
        bool seen=!s->pending || s->io.status(s->io.ctx,
            (volatile uint32_t *)(s->memory+INTEL_RING_BYTES/4+INTEL_SEQ))==s->seq;
        if (seen && empty_idle(s)) {
            s->pending=false; s->cpu_safe=true; return 0;
        }
        int e=checkpoint(s,b);
        if (e) return fault(s,e,"completion_timeout");
    }
}
/* Linux v6.7 gen2_emit_flush: full write flush, twelve store/flush pairs,
 * full write flush. No user-controlled addresses or interrupts. */
static unsigned gen2_flush(uint32_t *p)
{
    unsigned n=0;
    p[n++]=MI_FLUSH;
    for (unsigned i=0;i<12;i++) {
        p[n++]=MI_STORE_INDEX; p[n++]=INTEL_SCRATCH*4u; p[n++]=0;
        p[n++]=MI_FLUSH|4u; /* MI_NO_WRITE_FLUSH */
    }
    p[n++]=MI_FLUSH;
    return n;
}
static int publish(struct intel_engine *s, const void *p, unsigned bytes,
                   struct intel_wait *b)
{
    int e=s->io.publish(s->io.ctx,p,bytes,b);
    if (e) return fault(s,e,e==-ETIMEDOUT ? "chipset_flush_timeout" : "chipset_flush_failed");
    if (!valid(s)) return fault(s,-EIO,"ownership_changed");
    if ((int64_t)(s->io.now(s->io.ctx)-b->deadline)>=0)
        return fault(s,-ETIMEDOUT,"submission_deadline");
    return 0;
}
/* Reserve all words before any tail write; never overwrite the 8-byte gap.
 * The last cacheline stays NOOP, including when the packet wraps. */
static int submit(struct intel_engine *s, const uint32_t *words, unsigned n,
                  struct intel_wait *b)
{
    uint32_t packet[192];
    if (!s->active || s->quarantined) return -EIO;
    unsigned overhead=s->chip->gen2 ? 124u : 74u;
    if (n>ARRAY_SIZE(packet)-overhead || (n&1)) return -EINVAL;
    if (!valid(s)) return fault(s,-EIO,"ownership_changed");
    if ((int64_t)(s->io.now(s->io.ctx)-b->deadline)>=0)
        return fault(s,-ETIMEDOUT,"submission_deadline");
    if (n) memcpy(packet,words,n*4);
    unsigned used=n;
    if (s->chip->gen2) used+=gen2_flush(packet+used);
    packet[used++]=MI_FLUSH;
    uint32_t sequence=s->seq+1u;
    if (!sequence) sequence=1; /* synchronous: no older request can alias */
    for (unsigned i=0;i<24;i++) {
        packet[used++]=MI_STORE_INDEX;
        packet[used++]=(i<16 ? INTEL_SCRATCH : INTEL_SEQ)*4u;
        packet[used++]=sequence;
    }
    packet[used++]=0; /* polling replaces donor's MI_USER_INTERRUPT */
    unsigned bytes=used*4, pad=s->tail+bytes>RING_END ? INTEL_RING_BYTES-s->tail : 0;
    for (;;) {
        uint32_t raw=rd(s,INTEL_HEAD), head=raw&0x001ffffcu;
        if ((raw&3u) || head>=INTEL_RING_BYTES || rd(s,INTEL_TAIL)!=s->tail)
            return fault(s,-EIO,"ring_pointer_invalid");
        unsigned free=(head+INTEL_RING_BYTES-s->tail-8u)%INTEL_RING_BYTES;
        if (free>=pad+bytes) break;
        int e=checkpoint(s,b);
        if (e) return fault(s,e,"ring_space_timeout");
    }
    if (!valid(s)) return fault(s,-EIO,"ownership_changed");
    if (!s->active || s->quarantined) return -EIO;
    if (pad) {
        memset((uint8_t *)s->memory+s->tail,0,pad);
        int e=publish(s,(uint8_t *)s->memory+s->tail,pad,b);
        if (e) return e;
    }
    unsigned tail=pad ? 0 : s->tail;
    memcpy((uint8_t *)s->memory+tail,packet,bytes);
    int e=publish(s,(uint8_t *)s->memory+tail,bytes,b);
    if (e) return e;
    if (!s->active || s->quarantined) return -EIO;
    s->seq=sequence; s->pending=true; s->cpu_safe=false;
    s->tail=tail+bytes;
    wr(s,INTEL_TAIL,s->tail);
    if (rd(s,INTEL_TAIL)!=s->tail) return fault(s,-EIO,"tail_readback_failed");
    return completion(s,b);
}
static int sync_engine(void *ctx)
{
    struct intel_engine *s=ctx;
    if (!s->active || s->quarantined) return s->cpu_safe ? 0 : -EIO;
    struct intel_wait b=budget(s);
    return submit(s,0,0,&b);
}
static bool rect_valid(struct intel_engine *s, const struct fb_rect *r)
{
    return r && r->x>=0 && r->y>=0 && r->width>0 && r->height>0 &&
        (uint64_t)(uint32_t)r->x+(uint32_t)r->width<=s->fb.width &&
        (uint64_t)(uint32_t)r->y+(uint32_t)r->height<=s->fb.height;
}
static uint32_t command(struct intel_engine *s, uint32_t opcode)
{ return opcode | (s->fb.bpp==32 ? 0x00300000u : 0); }
static uint32_t control(struct intel_engine *s, unsigned rop, unsigned pitch)
{ return (s->fb.bpp==32 ? 3u : 1u)<<24 | rop<<16 | pitch; }
static int fill(void *ctx, const struct fb_rect *r, uint32_t color)
{
    struct intel_engine *s=ctx;
    if (!s->active || s->quarantined) return -EIO;
    if (!rect_valid(s,r)) { s->fallbacks++; return -EOPNOTSUPP; }
    uint32_t p[]={command(s,XY_FILL),control(s,0xf0,s->fb.pitch),
        (uint32_t)r->y<<16 | (uint32_t)r->x,
        (uint32_t)(r->y+r->height)<<16 | (uint32_t)(r->x+r->width),0,color};
    struct intel_wait b=budget(s);
    int e=submit(s,p,ARRAY_SIZE(p),&b);
    if (!e) s->operations++;
    return e;
}
static void copy_packet(struct intel_engine *s, uint32_t p[8], uint32_t dest,
                        unsigned dpitch, uint32_t source, unsigned spitch, unsigned width)
{
    /* Address origins rather than negative pitch/direction control. */
    p[0]=command(s,XY_COPY); p[1]=control(s,0xcc,dpitch);
    p[2]=0; p[3]=1u<<16 | width; p[4]=dest;
    p[5]=0; p[6]=spitch; p[7]=source;
}
static int copy(void *ctx, const struct fb_rect *r, int32_t dx, int32_t dy)
{
    struct intel_engine *s=ctx;
    if (!s->active || s->quarantined) return -EIO;
    struct fb_rect dst={dx,dy,r ? r->width : 0,r ? r->height : 0};
    if (!rect_valid(s,r) || !rect_valid(s,&dst)) { s->fallbacks++; return -EOPNOTSUPP; }
    struct intel_wait b=budget(s);
    unsigned bytes=s->fb.bpp/8;
    unsigned scratch_pitch=((unsigned)r->width*bytes+3u)&~3u;
    /* The row's address origins must meet the donor's dword alignment. */
    if (((unsigned)r->x*bytes | (unsigned)dx*bytes)&3u) {
        s->fallbacks++; return -EOPNOTSUPP;
    }
    for (int row=0;row<r->height;row++) {
        int y=dy>r->y ? r->height-1-row : row;
        uint32_t p[66];
        uint32_t scratch=s->gpu+INTEL_RING_BYTES+4096u;
        copy_packet(s,p,scratch,scratch_pitch,
            (uint32_t)(r->y+y)*s->fb.pitch+(uint32_t)r->x*bytes,s->fb.pitch,(unsigned)r->width);
        unsigned n=8;
        if (s->chip->gen2) n+=gen2_flush(p+n);
        else { p[n++]=MI_FLUSH; p[n++]=0; }
        copy_packet(s,p+n,(uint32_t)(dy+y)*s->fb.pitch+(uint32_t)dx*bytes,
            s->fb.pitch,scratch,scratch_pitch,(unsigned)r->width);
        int e=submit(s,p,n+8,&b);
        /* A submitted overlapping copy requires reconstruction, never replay. */
        if (e) return -EAGAIN;
        if (row+1<r->height) {
            e=checkpoint(s,&b);
            if (e) { fault(s,e,"copy_deadline"); return -EAGAIN; }
        }
    }
    s->operations++; return 0;
}
static bool cpu_ready(void *ctx) { return ((struct intel_engine *)ctx)->cpu_safe; }
static unsigned caps(void *ctx)
{ struct intel_engine *s=ctx; return s->active && !s->quarantined ? 1u : 0; }
static void set_pause(void *ctx, int (*pause)(void *), void *arg)
{ struct intel_engine *s=ctx; s->pause=pause; s->pause_arg=arg; }
static int stop(void *ctx) { return intel_engine_stop(ctx); }

int intel_reserve(struct intel_engine *s, const struct fb_device *fb, const struct intel_io *io,
                  uint32_t *memory, uint32_t physical, unsigned entries, uint16_t device)
{
    const struct intel_chip *chip=intel_chip_lookup(device);
    if (!chip || chip->refusal) return -ENODEV;
    if (!s || s->quarantined || s->mapped || !intel_mode_valid(fb) || !io ||
        !io->read || !io->write || !io->pte || !io->set_pte || !io->publish ||
        !io->status || !io->now || !io->relax || !io->valid || !memory ||
        !physical || (physical&4095u) || physical>UINT32_MAX-INTEL_DMA_BYTES ||
        entries<INTEL_DMA_PAGES || entries>65536) return -EINVAL;
    memset(s,0,sizeof(*s)); s->chip=chip; s->io=*io; s->fb=*fb; s->memory=memory;
    s->physical=physical; s->entries=entries; s->cpu_safe=true;
    s->first=entries-INTEL_DMA_PAGES; s->gpu=s->first*4096u;
    if (fb->size>s->gpu || !io->valid(io->ctx)) return -ENODEV;
    s->pgtbl=rd(s,INTEL_PGTBL);
    if (!(s->pgtbl&1u)) return -ENODEV;
    for (unsigned i=0;i<entries;i++) {
        uint32_t p=io->pte(io->ctx,i), address=p&~4095u;
        if ((p&1u) && address>=physical && address<physical+INTEL_DMA_BYTES)
            return -EBUSY;
    }
    /* Invalid PTEs only. A repeated valid address is not a scratch identity. */
    for (unsigned i=s->first;i<entries;i++) {
        uint32_t p=io->pte(io->ctx,i);
        if (p&1u) return -ENOSPC;
        s->saved_ptes[i-s->first]=p;
    }
    s->ops=(struct accel_ops){.ctx=s,.sync=sync_engine,.copy_rect=copy,.fill_rect=fill,
        .stop=stop,.capabilities=caps,.cpu_ready=cpu_ready,.set_pause=set_pause};
    return 0;
}
int intel_engine_start(struct intel_engine *s)
{
    if (!s || s->quarantined || s->active || s->mapped || !valid(s)) return -ENODEV;
    for (unsigned i=0;i<4;i++) s->saved_ring[i]=rd(s,INTEL_TAIL+i*4);
    s->saved_hws=rd(s,INTEL_HWS);
    /* No adoption, drain or reset of a foreign engine. */
    if ((s->saved_ring[3]&1u) || !chip_idle(s) ||
        s->saved_ring[1] || s->saved_ring[0]) return -EBUSY;
    for (unsigned i=0;i<INTEL_DMA_PAGES;i++)
        if (s->io.pte(s->io.ctx,s->first+i)!=s->saved_ptes[i]) return -EBUSY;
    memset(s->memory,0,INTEL_DMA_BYTES);
    struct intel_wait b=budget(s);
    int e=publish(s,s->memory,INTEL_DMA_BYTES,&b);
    if (e) return e;
    s->mapped=true;
    for (unsigned i=0;i<INTEL_DMA_PAGES;i++)
        s->io.set_pte(s->io.ctx,s->first+i,s->physical+i*4096u+1u);
    e=publish(s,0,0,&b);
    if (e) return e;
    if (!valid(s)) return fault(s,-EIO,"gtt_readback_failed");
    wr(s,INTEL_HWS,s->physical+INTEL_RING_BYTES);
    wr(s,INTEL_TAIL,0); wr(s,INTEL_HEAD,0); wr(s,INTEL_START,s->gpu);
    s->touched=true; s->cpu_safe=false;
    if (rd(s,INTEL_HWS)!=s->physical+INTEL_RING_BYTES || rd(s,INTEL_START)!=s->gpu ||
        rd(s,INTEL_TAIL) || rd(s,INTEL_HEAD) || (rd(s,INTEL_CTL)&1u))
        return fault(s,-EIO,"ring_prepare_failed");
    wr(s,INTEL_CTL,(INTEL_RING_BYTES-4096u)|1u);
    if (!valid(s)) return fault(s,-EIO,"ring_setup_failed");
    s->active=true;
    return submit(s,0,0,&b);
}
int intel_engine_stop(struct intel_engine *s)
{
    if (!s || !s->mapped) return 0;
    struct intel_wait b=budget(s);
    /* Even a partial GTT setup may be rolled back only while the original
     * disabled engine and its display ownership are still observable. */
    if (!s->touched && (!s->io.valid(s->io.ctx) || rd(s,INTEL_PGTBL)!=s->pgtbl ||
        rd(s,INTEL_CTL)!=s->saved_ring[3] || (rd(s,INTEL_CTL)&1u) ||
        rd(s,INTEL_HEAD)!=s->saved_ring[1] || rd(s,INTEL_TAIL)!=s->saved_ring[0] ||
        !chip_idle(s))) return fault(s,-EIO,"setup_rollback_unsafe");
    if (s->touched) {
        int e;
        if (!s->quarantined) e=submit(s,0,0,&b);
        else e=completion(s,&b);
        if (e) return e; /* Everything stays pinned; no blind disable/reset. */
        wr(s,INTEL_CTL,0);
        if ((rd(s,INTEL_CTL)&1u) || !chip_idle(s))
            return fault(s,-EIO,"ring_disable_failed");
        wr(s,INTEL_HWS,s->saved_hws);
        wr(s,INTEL_START,s->saved_ring[2]);
        wr(s,INTEL_HEAD,s->saved_ring[1]); wr(s,INTEL_TAIL,s->saved_ring[0]);
        wr(s,INTEL_CTL,s->saved_ring[3]);
        for (unsigned i=0;i<4;i++) if (rd(s,INTEL_TAIL+i*4)!=s->saved_ring[i])
            return fault(s,-EIO,"ring_restore_failed");
        if (rd(s,INTEL_HWS)!=s->saved_hws) return fault(s,-EIO,"hws_restore_failed");
        s->touched=false;
    }
    for (unsigned i=0;i<INTEL_DMA_PAGES;i++)
        s->io.set_pte(s->io.ctx,s->first+i,s->saved_ptes[i]);
    int e=s->io.publish(s->io.ctx,0,0,&b);
    if (e) return fault(s,e,e==-ETIMEDOUT ? "chipset_flush_timeout" : "chipset_flush_failed");
    if (!s->io.valid(s->io.ctx)) return fault(s,-EIO,"ownership_changed");
    for (unsigned i=0;i<INTEL_DMA_PAGES;i++)
        if (s->io.pte(s->io.ctx,s->first+i)!=s->saved_ptes[i])
            return fault(s,-EIO,"gtt_restore_failed");
    s->mapped=false; s->active=false; s->stopped=true; s->cpu_safe=true;
    s->pause=0; s->pause_arg=0; return 0;
}
