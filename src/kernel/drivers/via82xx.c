/* VIA SGD (not Intel NAM/NABM). SPDX-License-Identifier: GPL-2.0-only.
 * References/quirks: docs/design/f4-audio-second-set.md.
 * Revision table adapted from Linux v6.12 sound/pci/via82xx.c,
 * Copyright (c) 2000 Jaroslav Kysela <perex@perex.cz>,
 * Tjeerd.Mulder <Tjeerd.Mulder@fujitsu-siemens.com>, 2002 Takashi Iwai <tiwai@suse.de>.
 * Upstream GPL-2.0-or-later; docs/licenses/linux-ac97-NOTICES.txt. */
#include <ciuki/kernel.h>
#include <ciuki/audio_backend.h>
bool via82xx_match(const struct pci_func *p, enum audio_backend_kind *kind)
{
    if (p->vendor!=0x1106 || p->class_code!=4 || p->subclass!=1) return false;
    if (p->device==0x3058) *kind=AUDIO_VIA686;
    else if (p->device==0x3059 && (p->revision==0x20 || p->revision==0x30 || p->revision==0x40 || p->revision==0x50 || p->revision==0x60)) *kind=AUDIO_VIA8233;
    else return false;
    return true;
}
int via82xx_codec_read(struct audio_dma_device *d, uint16_t reg, uint16_t *value)
{
    int e=audio_hw_wait(d,0x80,1u<<24,0,100); if (e) return e;
    audio_hw_write(d,0x80,1u<<25 | 1u<<23 | (uint32_t)reg<<16,4);
    e=audio_hw_wait(d,0x80,(1u<<24)|(1u<<25),1u<<25,100);
    if (!e) {
        uint32_t v=audio_hw_read(d,0x80,4);
        if (((v>>16)&0x7e)!=reg || (v&0xffff)==0xffff) return -EIO;
        *value=v;
    }
    return e;
}
int via82xx_codec_write(struct audio_dma_device *d, uint16_t reg, uint16_t value)
{
    int e=audio_hw_wait(d,0x80,1u<<24,0,100); if (e) return e;
    audio_hw_write(d,0x80,(uint32_t)reg<<16 | value,4);
    e=audio_hw_wait(d,0x80,1u<<24,0,100);
    uint16_t got;
    if (!e) e=via82xx_codec_read(d,reg,&got);
    const struct ac97_codec_profile *p=ac97_codec_lookup(d->state.codec_id);
    unsigned mask=reg==0x26 ? 0xff00 : reg==0x2a ? 0x000f : reg==2 ? p->master_mask : reg==0x18 ? p->pcm_mask : reg==4 ? p->headphone_mask : 0xffff;
    if (!mask) mask=0x8000; /* absent C-Media control: only the upstream reset mute */
    return e ? e : ((got^value)&mask) ? -EIO : 0;
}
static int via_mute(struct audio_dma_device *d, bool mute)
{ return audio_dma_codec_mute(d,mute,via82xx_codec_write); }
static int via_cursor(struct audio_dma_device *d, unsigned *frame)
{
    unsigned ch=d->channel;
    unsigned status=audio_hw_read(d,ch,1);
    if (status==255) return -EIO;
    if (!(status&0x80)) return -EAGAIN;
    for (unsigned attempt=0;attempt<8;attempt++) {
        uint32_t a=audio_hw_read(d,ch+0xc,4), pointer=audio_hw_read(d,ch+4,4), b=audio_hw_read(d,ch+0xc,4);
        if ((a>>24)!=(b>>24)) continue;
        unsigned index=b>>24, remaining=b&0xffffff;
        if (d->kind==AUDIO_VIA686) {
            if (pointer<d->desc_phys+8 || pointer>d->desc_phys+AUDIO_HW_SLOTS*8 || (pointer-d->desc_phys)%8) return -EIO;
            index=(pointer-d->desc_phys)/8-1;
            if (pointer!=audio_hw_read(d,ch+4,4)) continue;
        }
        if (index>=AUDIO_HW_SLOTS || remaining>1024) return -EIO;
        if (!remaining) continue; /* DMA boundary: retry the coherent snapshot */
        *frame=index*AUDIO_QUANTUM+(1024-remaining)/4;
        return 0;
    }
    return -EAGAIN;
}
static void via_inhibit(struct audio_dma_device *d)
{ audio_hw_write(d,d->channel+1,0x40,1); }
static int via_halt(struct audio_dma_device *d)
{
    via_inhibit(d);
    uint64_t end=audio_hw_deadline(d,500);
    do {
        unsigned status=audio_hw_read(d,d->channel,1);
        uint32_t ptr=audio_hw_read(d,d->channel+4,4), count=audio_hw_read(d,d->channel+0xc,4);
        if (status!=255 && !(status&0x80)) {
            d->io.sleep(d->io.cookie,1);
            if (ptr==audio_hw_read(d,d->channel+4,4) && count==audio_hw_read(d,d->channel+0xc,4) &&
                !(audio_hw_read(d,d->channel,1)&0x80)) {
                /* Reset only after stop has been proved. */
                audio_hw_write(d,d->channel+1,0x49,1); d->io.sleep(d->io.cookie,1);
                audio_hw_write(d,d->channel+1,0,1); audio_hw_write(d,d->channel,3,1);
                d->state.running=false; return 0;
            }
        }
        d->io.sleep(d->io.cookie,1);
    } while ((int64_t)(d->io.now(d->io.cookie)-end)<0);
    d->reason="dma_stop_unproved"; return -EIO;
}
static int via_start(struct audio_dma_device *d, bool proof)
{
    for (unsigned i=0;i<AUDIO_HW_SLOTS;i++)
        d->desc[i]=(struct ac97_bd){d->phys+i*1024,1024u | (i==AUDIO_HW_SLOTS-1 ? 1u<<31 : 1u<<30)};
    __asm__ volatile("" ::: "memory");
    audio_hw_write(d,d->channel+4,d->desc_phys,4);
    if (d->kind==AUDIO_VIA686) audio_hw_write(d,2,proof ? 0xb4 : 0xb7,1);
    else if (d->channel==0x40) {
        audio_hw_write(d,0x42,0xa0,1); /* signed 16-bit, two channels */
        audio_hw_write(d,0x48,0xff000021,4); /* FL/FR slots 3/4 */
    } else {
        audio_hw_write(d,2,0,1); audio_hw_write(d,3,0,1); /* unity DXS attenuation */
        audio_hw_write(d,8,0xff3fffff,4); /* 16-bit stereo, fixed 48k, no stop index */
    }
    audio_hw_write(d,d->channel,3,1);
    audio_hw_write(d,d->channel+1,d->kind==AUDIO_VIA686 ? 0x80 : proof ? 0xa0 : 0xa3,1);
    /* START is write-only/asynchronous; actual progress, not an immediate
     * ACTIVE readback, is the admission gate in audio_dma_prove. */
    return 0;
}
static bool via_ack(struct audio_dma_device *d)
{
    unsigned status=audio_hw_read(d,d->channel,1);
    if (status==255 || !(status&3)) return false;
    audio_hw_write(d,d->channel,status&3,1); return true;
}
static int via_prepare(struct audio_dma_device *d)
{
    d->channel=d->kind==AUDIO_VIA8233 && d->pci.revision==0x40 ? 0x40 : 0;
    for (unsigned i=0;i<pci_count();i++) {
        const struct pci_func *peer=pci_get(i);
        if (peer->vendor==d->pci.vendor && peer->class_code==7 && peer->subclass==3 && (peer->command & 5)) {
            d->reason="aclink_dependency"; return -EBUSY;
        }
    }
    if (d->command&1) {
        for (unsigned i=0;i<8;i++) {
            unsigned ch=i*0x10;
            if (d->kind==AUDIO_VIA686 && ch>0x20) break;
            if (d->kind==AUDIO_VIA8233 && ch==0x50) continue;
            unsigned s=audio_hw_read(d,ch,1);
            if (s==255 || (s&0x80)) { d->reason="firmware_engine_active"; return -EBUSY; }
        }
    }
    d->touched=true;
    audio_pci_write(&d->io,&d->pci,4,d->command | 1,2);
    if (!(audio_pci_read(&d->io,&d->pci,4)&1)) return -EIO;
    uint32_t config=audio_pci_read(&d->io,&d->pci,0x40);
    d->saved_link=(config>>8)&255; d->saved_legacy=(config>>16)&255;
    /* Disable SB/FM aliases so this PCI driver never creates a second owner. */
    d->legacy_changed=true;
    audio_pci_write(&d->io,&d->pci,0x42,d->saved_legacy & ~5u,1);
    d->link_changed=true;
    if (!(config&1)) {
        audio_pci_write(&d->io,&d->pci,0x41,0xe0,1); d->io.sleep(d->io.cookie,1);
        audio_pci_write(&d->io,&d->pci,0x41,0,1); d->io.sleep(d->io.cookie,1);
    }
    audio_pci_write(&d->io,&d->pci,0x41,0xcc,1); /* codec/link/SGD on, VRA allowed, fixed codec48 */
    uint64_t deadline=audio_hw_deadline(d,750);
    while (!(audio_pci_read(&d->io,&d->pci,0x40)&1)) {
        if ((int64_t)(d->io.now(d->io.cookie)-deadline)>=0) { d->reason="codec_link_timeout"; return -ETIMEDOUT; }
        d->io.sleep(d->io.cookie,1);
    }
    int e=via_halt(d);
    if (!e) e=audio_dma_codec_prepare(d,via82xx_codec_read,via82xx_codec_write);
    if (e) { if (!d->reason) d->reason="codec_prepare"; return e; }
    d->state.rate=48000;
    audio_pci_write(&d->io,&d->pci,4,d->command | 5,2); /* VT8233 may ignore BME; engine proof is required */
    return 0;
}
const struct audio_dma_ops via82xx_ops={via_prepare,via_start,via_halt,via_cursor,via_mute,via_ack,via_inhibit};
