/* AudioPCI DAC2: AK4531 / ES1371 SRC + AC'97. No imported code.
 * Sources: docs/design/f4-audio-second-set.md. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/audio_backend.h>
#define ES_DAC2_ENABLE (1u << 5)
#define ES_P2_IRQ (1u << 9)
#define ES_SRC_DISABLE (1u << 22)
#define ES_SRC_MASK (15u << 19)
bool es137x_match(const struct pci_func *p, enum audio_backend_kind *kind)
{
    if (p->class_code != 4 || p->subclass != 1 || p->vendor != 0x1274) return false;
    if (p->device == 0x5000) *kind = AUDIO_ES1370;
    else if (p->device == 0x1371 || p->device == 0x5880) *kind = AUDIO_ES1371;
    else return false;
    return true;
}
static int es_src_wait(struct audio_dma_device *d)
{ return audio_hw_wait(d, 0x10, 1u << 23, 0, 100); }
static int es_src_write(struct audio_dma_device *d, unsigned reg, unsigned data)
{
    int e = es_src_wait(d); if (e) return e;
    uint32_t state = audio_hw_read(d, 0x10, 4) & ES_SRC_MASK;
    audio_hw_write(d, 0x10, state | reg << 25 | 1u << 24 | (data & 0xffff), 4);
    return es_src_wait(d);
}
static int es_window_wait(struct audio_dma_device *d, uint32_t value)
{
    uint64_t end=audio_hw_deadline(d,100);
    unsigned polls=0;
    do {
        uint32_t state=audio_hw_read(d,0x10,4);
        if (state==UINT32_MAX) return -EIO;
        if ((state&0x00870000)==value) return 0;
        if (++polls%128==0) d->io.sleep(d->io.cookie,1);
    } while ((int64_t)(d->io.now(d->io.cookie)-end)<0);
    return -ETIMEDOUT;
}
static int es_codec_access(struct audio_dma_device *d, uint16_t reg, uint16_t *v, bool read)
{
    int e = audio_hw_wait(d, 0x14, 1u << 30, 0, 100);
    if (!e) e = es_src_wait(d);
    if (e) return e;
    uint32_t saved = audio_hw_read(d, 0x10, 4) & ES_SRC_MASK;
    audio_hw_write(d, 0x10, saved | 0x10000, 4);
    e = es_window_wait(d,0);
    if (!e) e = es_window_wait(d,0x10000);
    if (!e) audio_hw_write(d, 0x14, (uint32_t)reg << 16 | (read ? 1u << 23 : *v), 4);
    if (!e) e = es_src_wait(d);
    audio_hw_write(d, 0x10, saved, 4); /* always close the safe window */
    if (!e) e = audio_hw_wait(d, 0x14, (1u << 30) | (read ? 1u << 31 : 0), read ? 1u << 31 : 0, 100);
    if (!e && read) { *v = audio_hw_read(d, 0x14, 4); if (*v == 0xffff) e = -EIO; }
    return e;
}
int es137x_codec_read(struct audio_dma_device *d, uint16_t reg, uint16_t *v)
{ return es_codec_access(d,reg,v,true); }
int es137x_codec_write(struct audio_dma_device *d, uint16_t reg, uint16_t value)
{
    int e = es_codec_access(d,reg,&value,false);
    uint16_t got;
    if (!e) e = es137x_codec_read(d,reg,&got);
    unsigned mask = reg == 0x26 ? 0xff00 : reg == 2 ? 0x9f1f : reg == 0x18 ? 0x9f1f : 0xffff;
    return e ? e : ((got ^ value) & mask) ? -EIO : 0;
}
static int es_ak_write(struct audio_dma_device *d, unsigned reg, unsigned v)
{
    int e = audio_hw_wait(d, 4, 0x700, 0, 100);
    if (e) return e;
    audio_hw_write(d, 0x10, reg << 8 | v, 2);
    e = audio_hw_wait(d, 4, 0x700, 0, 100);
    if (!e) d->ak[reg] = v;
    return e;
}
static int es_mute(struct audio_dma_device *d, bool mute)
{
    if (d->kind == AUDIO_ES1371) return audio_dma_codec_mute(d,mute,es137x_codec_write);
    int e = es_ak_write(d,0,mute ? 0x80 : 0);
    if (!e) e = es_ak_write(d,1,mute ? 0x80 : 0);
    if (!e) { d->state.master = mute ? 0x8000 : 0; d->state.pcm_volume = 0x0606; }
    return e;
}
static int es_cursor(struct audio_dma_device *d, unsigned *frame)
{
    uint32_t flags = irq_save();
    audio_hw_write(d, 0xc, 0xc, 4);
    uint32_t v = audio_hw_read(d, 0x3c, 4);
    irq_restore(flags);
    if (v == UINT32_MAX || (v & 0xffff) != AUDIO_HW_SLOTS * AUDIO_QUANTUM - 1) return -EIO;
    *frame = v >> 16;
    return *frame >= AUDIO_HW_SLOTS * AUDIO_QUANTUM ? -EIO : 0;
}
static void es_inhibit(struct audio_dma_device *d)
{ audio_hw_write(d, 0, d->control & ~ES_DAC2_ENABLE, 4); }
static int es_halt(struct audio_dma_device *d)
{
    es_inhibit(d);
    audio_hw_write(d,0x20,d->serial & ~ES_P2_IRQ,4);
    uint64_t end = audio_hw_deadline(d,500);
    do {
        uint32_t a=audio_hw_read(d,0x3c,4);
        if (!(audio_hw_read(d,0,4) & ES_DAC2_ENABLE) && a!=UINT32_MAX) {
            d->io.sleep(d->io.cookie,1);
            uint32_t b=audio_hw_read(d,0x3c,4);
            if (a==b) { d->state.running=false; return 0; }
        }
        d->io.sleep(d->io.cookie,1);
    } while ((int64_t)(d->io.now(d->io.cookie) - end) < 0);
    d->reason="dma_stop_unproved"; return -EIO;
}
static int es_start(struct audio_dma_device *d, bool proof)
{
    uint32_t flags=irq_save();
    audio_hw_write(d,0xc,0xc,4);
    audio_hw_write(d,0x38,d->phys,4);
    audio_hw_write(d,0x3c,AUDIO_HW_SLOTS * AUDIO_QUANTUM - 1,4);
    audio_hw_write(d,0x28,AUDIO_QUANTUM - 1,4); /* stereo frame count, not words */
    audio_hw_write(d,0x20,d->serial | (proof ? 0 : ES_P2_IRQ),4);
    audio_hw_write(d,0,d->control | ES_DAC2_ENABLE,4);
    unsigned got=audio_hw_read(d,0,4);
    irq_restore(flags);
    return (got & ES_DAC2_ENABLE) ? 0 : -EIO;
}
static bool es_ack(struct audio_dma_device *d)
{
    uint32_t status=audio_hw_read(d,4,4);
    if (status == UINT32_MAX || !(status & 2)) return false;
    audio_hw_write(d,0x20,d->serial & ~ES_P2_IRQ,4);
    audio_hw_write(d,0x20,d->serial | ES_P2_IRQ,4);
    return true;
}
static int es_prepare(struct audio_dma_device *d)
{
    if ((d->command & 1) && (audio_hw_read(d,0,4) & 0x7c)) { d->reason="firmware_engine_active"; return -EBUSY; }
    d->touched=true;
    audio_pci_write(&d->io,&d->pci,4,(d->command | 1) & ~4u,2);
    if ((audio_pci_read(&d->io,&d->pci,4)&0xffff) != ((d->command|1u)&~4u)) return -EIO;
    d->control = d->kind == AUDIO_ES1370 ? (30u << 16) | 0x103 : 0;
    d->serial = (2u << 19) | 12;
    audio_hw_write(d,0,d->control,4); audio_hw_write(d,0x20,d->serial,4);
    int e=0;
    if (d->kind == AUDIO_ES1370) {
        /* Allocate phantom address inside the same pinned ring, distinct from
         * DAC2; Linux programs it for the ES1370 bus-master erratum. */
        audio_hw_write(d,0xc,0xd,4); audio_hw_write(d,0x38,d->phys,4); audio_hw_write(d,0x3c,0,4);
        e=es_ak_write(d,0x16,2); d->io.sleep(d->io.cookie,1);
        if (!e) e=es_ak_write(d,0x16,3);
        if (!e) e=es_ak_write(d,0x17,0);
        /* Mute external analog inputs; voice is the sole output source. */
        for (unsigned i=0; !e && i<26; i++) {
            if (i==0x16 || i==0x17) continue;
            unsigned v=i<2 ? 0x80 : i<4 ? 6 : i<16 ? 0x80 : i==0x11 ? 0x0c : 0;
            e=es_ak_write(d,i,v);
        }
        d->codec_changed=true; d->state.codec_id=0x414b4531; d->state.rate=44100;
        if (!e) e=es_mute(d,audio.master.mute);
    } else {
        if ((d->pci.device==0x5880 && d->pci.revision>=2 && d->pci.revision<=4) ||
            (d->pci.device==0x1371 && (d->pci.revision==7 || d->pci.revision==8))) {
            audio_hw_write(d,4,1u << 29,4); d->io.sleep(d->io.cookie,20);
        }
        audio_hw_write(d,0x18,0,4); /* legacy traps disabled */
        audio_hw_write(d,0,d->control | 1u << 14,4); d->io.sleep(d->io.cookie,1); audio_hw_write(d,0,d->control,4);
        e=es_src_wait(d);
        if (!e) audio_hw_write(d,0x10,ES_SRC_DISABLE,4);
        for (unsigned i=0; !e && i<128; i++) e=es_src_write(d,i,0);
        for (unsigned ch=0x70; !e && ch<=0x78; ch+=4) {
            e=es_src_write(d,ch,16u << 4);
            if (!e) e=es_src_write(d,ch+1,16u << 10);
            if (!e) e=es_src_write(d,ch+3,0);
        }
        /* DAC2 48k: freq=(48000<<15)/3000=0x80000; int=0x4000, frac=0. */
        for (unsigned i=0x6c; !e && i<=0x7f; i++)
            if (i==0x6c || i==0x6d || i>=0x7c) e=es_src_write(d,i,1u << 12);
        if (!e) audio_hw_write(d,0x10,0,4);
        d->io.sleep(d->io.cookie,750);
        if (!e) e=audio_dma_codec_prepare(d,es137x_codec_read,es137x_codec_write);
        d->state.rate=48000;
    }
    if (e) { d->reason="codec_prepare"; return e; }
    audio_pci_write(&d->io,&d->pci,4,d->command | 5,2);
    return (audio_pci_read(&d->io,&d->pci,4)&0xffff)==(d->command|5u) ? 0 : -EIO;
}
const struct audio_dma_ops es137x_ops={es_prepare,es_start,es_halt,es_cursor,es_mute,es_ack,es_inhibit};
