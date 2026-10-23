/*
 * Copyright (c) 2000,2001 Søren Schmidt <sos@FreeBSD.org>
 * All rights reserved.
 *
 * dmraid modifications:
 * Copyright (C) 2004,2005  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer,
 *    without modification, immediately at the beginning of the file.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR `AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/* Promise/HighPoint native-resource adapter. Linux v6.12 donor algorithms;
 * origins and original notices: docs/licenses/ata-addin-NOTICES.txt.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/task.h>
#include <ciuki/ata.h>

static bool pdc_new_id(unsigned id)
{
    return id == 0x4d68 || id == 0x4d69 || id == 0x6269 || id == 0x1275 ||
           id == 0x5275 || id == 0x7275;
}
bool ata_addin_match(const struct pci_func *p)
{
    if (!p) return false;
    if (p->vendor == 0x105a) return pdc_new_id(p->device) || p->device == 0x4d33 ||
        p->device == 0x4d38 || p->device == 0x0d30 || p->device == 0x4d30;
    return p->vendor == 0x1103 && (p->device == 4 || p->device == 5 || p->device == 8 || p->device == 9);
}
unsigned ata_addin_channels(const struct pci_func *p)
{
    return p && p->vendor == 0x1103 && p->device == 4 && p->revision < 3 ? 1 : 2;
}
int ata_addin_resources(const struct pci_func *p, unsigned ch, bool routed, struct ata_resources *r)
{
    if (!ata_addin_match(p) || !r || ch >= ata_addin_channels(p) || p->class_code != 1 ||
        (p->subclass != 1 && p->subclass != 4 && p->subclass != 0x80) || (p->header_type & 0x7f))
        return -FS_EINVAL;
    if(p->vendor==0x1103 && p->device==4 && p->revision>6) return -FS_EOPNOTSUPP;
    if (!routed || (p->command & 0x400) || !p->irq_pin || p->irq_pin > 4 ||
        p->irq_line < 3 || p->irq_line > 15 || p->irq_line == 8 || p->irq_line == 13)
        return -FS_EOPNOTSUPP;
    struct ata_resources v = {.irq=p->irq_line, .native_irq=true};
    if (p->vendor == 0x105a && pdc_new_id(p->device)) {
        uint32_t raw=p->bar[5], b=raw&~15u;
        if (!(p->command & 2) || (raw&7) || !b || (b & 0x3fff) || b > 0xffffbfffu) return -FS_EINVAL;
        v.mmio = b;
        v.base = ch ? 0x15c0 : 0x17c0;
        v.control = (uint16_t)(v.base + 0x81a);
        v.bm = (uint16_t)(0x1000 + 8 * ch);
    } else {
        if (!(p->command & 1)) return -FS_EINVAL;
        unsigned span = p->vendor == 0x1103 ? 256 : 64;
        unsigned bm = p->bar[4] & ~3u;
        if (!(p->bar[4] & 1) || !bm || (bm & (span - 1)) || bm > 0x10000u - span) return -FS_EINVAL;
        unsigned start[5] = {0}, size[5] = {8,4,8,4,span};
        for (unsigned n = 0; n < ata_addin_channels(p); n++) {
            uint32_t b=p->bar[2*n], ctl=p->bar[2*n+1];
            if (!(b&1) || !(ctl&1) || !(b&~3u) || !(ctl&~3u) || (b&6) || (ctl&2) ||
                (b&~3u)>0xfff8 || (ctl&~3u)>0xfffc) return -FS_EINVAL;
            start[2*n]=b&~3u; start[2*n+1]=ctl&~3u;
        }
        if (ata_addin_channels(p) == 1) size[2]=size[3]=0;
        start[4]=bm;
        for (unsigned i=0;i<5;i++) for(unsigned j=i+1;j<5;j++)
            if(size[i] && size[j] && start[i]<start[j]+size[j] && start[j]<start[i]+size[i]) return -FS_EINVAL;
        v.base=(uint16_t)start[2*ch]; v.control=(uint16_t)(start[2*ch+1]+2); v.bm=(uint16_t)(bm+8*ch);
    }
    *r=v; return 0;
}
void ata_addin_detect(struct ata_chipset *s)
{
    if (!ata_addin_match(s->pci)) return;
    const struct pci_func *p=s->pci;
    if (p->vendor==0x105a) {
        s->family=pdc_new_id(p->device) ? ATA_CHIP_PDC_NEW : ATA_CHIP_PDC_OLD;
        s->maximum=p->device==0x4d33 ? 2 : p->device==0x4d38 ? 4 :
            s->family==ATA_CHIP_PDC_NEW && p->device!=0x4d68 ? 6 : 5;
        s->no_lba48_dma=p->device==0x4d33;
        if (p->device==0x0d30) for (unsigned i=0;i<pci_count();i++) {
            const struct pci_func *b=pci_get(i);
            if(b->vendor==0x8086 && (b->device==0x0960 || b->device==0x0962) && b->class_code==6) {
                unsigned sec=s->ops->read(s->ctx,b,0x19), end=s->ops->read(s->ctx,b,0x1a);
                if(p->bus>=sec && p->bus<=end) {s->maximum=-1;s->reason="promise_i960_raid";return;}
            }
        }
        if(s->family==ATA_CHIP_PDC_OLD) s->cable80=p->device!=0x4d33 && !(ata_chip_read(s,0x50,2)&(1u<<(10+s->channel)));
    } else {
        if(p->device==4 && p->revision<3) {s->family=ATA_CHIP_HPT36;s->maximum=4;}
        else if(p->device==4 && p->revision<=5) {s->family=ATA_CHIP_HPT37;s->maximum=p->revision<5 ? 5 : 6;}
        else if(p->device==4 && p->revision==6) {s->family=ATA_CHIP_HPTN;s->maximum=6;}
        else if(p->device==5) {s->family=p->revision<2 ? ATA_CHIP_HPT37 : ATA_CHIP_HPTN;s->maximum=6;}
        else if(p->device==8) {s->family=ATA_CHIP_HPT37;s->maximum=5;}
        else if(p->device==9) {s->family=ATA_CHIP_HPTN;s->maximum=6;}
        else {s->reason="highpoint_unknown_revision";return;}
    }
    s->reason="addin_not_prepared";
}
static uint32_t addin_native_read(void *ctx, unsigned reg, unsigned bytes)
{
    struct ata_channel *c=ctx;
    if(c->mmio_phys && !c->mmio) return UINT32_MAX;
    if(c->mmio) {
        if(bytes==4) return *(volatile uint32_t *)(c->mmio+reg);
        if(bytes==2) return *(volatile uint16_t *)(c->mmio+reg);
        return c->mmio[reg];
    }
    uint16_t port=(uint16_t)(c->addin_base+reg);
    if(bytes==4) return inl(port);
    if(bytes==2) return c->ops->in16(c->port_ctx,port);
    return inb(port);
}
static void addin_native_write(void *ctx, unsigned reg, unsigned bytes, uint32_t v)
{
    struct ata_channel *c=ctx;
    if(c->mmio_phys && !c->mmio) return;
    if(c->mmio) {
        if(bytes==4) *(volatile uint32_t *)(c->mmio+reg)=v;
        else if(bytes==2) *(volatile uint16_t *)(c->mmio+reg)=(uint16_t)v;
        else c->mmio[reg]=(uint8_t)v;
    } else {
        uint16_t port=(uint16_t)(c->addin_base+reg);
        if(bytes==4) outl(port,v);
        else if(bytes==2) c->ops->out16(c->port_ctx,port,(uint16_t)v);
        else outb(port,(uint8_t)v);
    }
}
const struct ata_addin_ops ata_addin_native_ops={addin_native_read,addin_native_write};
uint32_t ata_addin_read(struct ata_channel *c, unsigned reg, unsigned bytes)
{
    return c->addin_ops->read(c->addin_ctx,reg,bytes);
}
int ata_addin_write(struct ata_channel *c, unsigned reg, unsigned bytes, uint32_t v)
{
    c->addin_ops->write(c->addin_ctx,reg,bytes,v);
    return ata_addin_read(c,reg,bytes)==v ? 0 : -FS_EIO;
}
int ata_addin_cfg(struct ata_chipset *s, unsigned reg, unsigned bytes, uint32_t v)
{
    if(s->ops==&ata_native_config && bytes==4 && !(reg&3)) {
        uint32_t f=irq_save();
        outl(0xcf8,0x80000000u | (uint32_t)s->pci->bus<<16 | s->pci->dev<<11 | s->pci->fn<<8 | reg);
        outl(0xcfc,v); irq_restore(f);
    } else for(unsigned i=0;i<bytes;i++) s->ops->write(s->ctx,s->pci,reg+i,(uint8_t)(v>>(8*i)));
    /* DPLL lock is read-only and may change independently. */
    uint32_t mask=reg==0x5b && bytes==1 ? 0x7fu : bytes==4 ? UINT32_MAX : (1u<<(8*bytes))-1;
    return (ata_chip_read(s,reg,bytes)&mask)==(v&mask) ? 0 : -FS_EIO;
}
bool ata_addin_idle(struct ata_channel *c)
{
    if(!c || !c->chipset.pci || c->mutex.owner!=g_current || !g_current || c->active ||
        c->dma.armed || c->dma.started || c->stopped || c->quiescing || !c->dma.ops) return false;
    unsigned base=c->dma.base-8*c->chipset.channel;
    for(unsigned i=0;i<ata_addin_channels(c->pci);i++) {
        struct ata_resources r;
        if(ata_addin_resources(c->pci,i,true,&r)) return false;
        uint8_t status=c->ops->in8(c->port_ctx,i==c->chipset.channel ? c->control : r.control);
        if(status!=0xff && (status&0x88)) return false;
        if((c->dma.ops->in8(c->dma.ctx,(uint16_t)(base+8*i))&1) ||
            (c->dma.ops->in8(c->dma.ctx,(uint16_t)(base+8*i+2))&ATA_BM_ACTIVE)) return false;
    }
    return true;
}
bool ata_addin_irq_pending(struct ata_channel *c)
{
    if(c->chipset.family==ATA_CHIP_PDC_OLD && c->addin_ops)
        return !!(ata_addin_read(c,0x1d,1)&(c->chipset.channel ? 0x40 : 4));
    return !!(c->dma.ops->in8(c->dma.ctx,c->dma.base+2)&ATA_BM_IRQ);
}
int ata_addin_prepare(struct ata_channel *c)
{
    if(c->chipset.family<ATA_CHIP_PDC_OLD) return 0;
    if(!c->addin_ops || !ata_addin_idle(c) || c->modes_published) return -FS_EBUSY;
    if(c->chipset.prepared && c->chipset.maximum<0) return 0;
    int e=c->chipset.maximum<0 ? -FS_EOPNOTSUPP : c->pci->vendor==0x105a ?
        ata_promise_prepare(c) : ata_highpoint_prepare(c);
    if(!e) {c->chipset.prepared=true;if(c->chipset.maximum>=0)c->chipset.reason="addin_ready";}
    return e;
}
int ata_addin_dma_begin(struct ata_device *d, uint32_t bytes, bool write)
{
    struct ata_channel *c=d->channel;
    if(c->chipset.family!=ATA_CHIP_PDC_OLD || c->pci->device==0x4d33) return 0;
    unsigned mask=c->chipset.channel ? 8 : 2;
    uint32_t flags=irq_save();
    uint8_t clk=(uint8_t)ata_addin_read(c,0x11,1);
    int e=ata_addin_write(c,0x11,1,(clk&~mask)|(d->udma_mode>2 ? mask : 0));
    irq_restore(flags);
    if(!e && d->lba48) e=ata_addin_write(c,0x20+4*c->chipset.channel,4,
        (bytes/2)|(write ? 0x06000000u : 0x05000000u));
    return e;
}
void ata_addin_dma_stopping(struct ata_channel *c)
{
    if(c->chipset.family!=ATA_CHIP_HPT37 && c->chipset.family!=ATA_CHIP_HPTN) return;
    struct ata_chipset *s=&c->chipset;
    if((s->pci->device!=4 || s->pci->revision>=5) && (ata_chip_read(s,0x6a,1)&(1u<<s->channel))) {
        unsigned reg=0x50+4*s->channel;
        /* Stop proof is still required by the common transport; no reset retry. */
        if(ata_addin_cfg(s,reg,1,ata_chip_read(s,reg,1)|0x30)) {
            c->fault_cause="highpoint_stop_readback";c->addin_fault=true;
        }
    }
}
bool ata_addin_dma_end(struct ata_channel *c)
{
    if(c->chipset.family!=ATA_CHIP_PDC_OLD || c->pci->device==0x4d33) return true;
    if((c->dma.ops->in8(c->dma.ctx,c->dma.base)&1) ||
        (c->dma.ops->in8(c->dma.ctx,c->dma.base+2)&ATA_BM_ACTIVE)) return false;
    unsigned mask=c->chipset.channel ? 8 : 2;
    if(ata_addin_write(c,0x20+4*c->chipset.channel,4,0)) return false;
    uint32_t flags=irq_save();
    int e=ata_addin_write(c,0x11,1,ata_addin_read(c,0x11,1)&~mask);
    irq_restore(flags);
    return !e;
}

/* dmraid pdc/hpt37x/hpt45x format constants, not array assembly code.
 * A signature alone is enough to refuse writes, including corrupt metadata. */
const char *ata_raid_signature(const uint8_t data[512])
{
    if(!memcmp(data,"Promise Technology, Inc.",23) || !memcmp(data,"FreeBSD ATA driver RAID ",24)) return "promise_raid_member";
    uint32_t old=fs_rd32(data+32), recent=fs_rd32(data);
    if(old==0x5a7816f0 || old==0x5a7816fd || recent==0x5a7816f3 || recent==0x5a7816fd)
        return "highpoint_raid_member";
    return 0;
}
unsigned ata_raid_locations(uint64_t capacity, uint64_t *lbas, unsigned size)
{
    static const unsigned offsets[]={63,255,256,16,399,591,675,735,911,974,991,3087};
    unsigned count=0;
    if(!lbas || size<54 || capacity<=11) return 0;
    lbas[count++]=9;lbas[count++]=capacity-11;
    for(unsigned i=0;i<ARRAY_SIZE(offsets);i++) for(unsigned copy=0;copy<4;copy++) {
        unsigned advance=14*copy;
        if(offsets[i]<=advance || capacity<offsets[i]) continue;
        lbas[count++]=capacity-offsets[i]+advance;
    }
    for(unsigned copy=0;copy<4;copy++) {
        uint64_t lba=268435377ull+14*copy;
        if(lba<capacity) lbas[count++]=lba;
    }
    return count;
}
