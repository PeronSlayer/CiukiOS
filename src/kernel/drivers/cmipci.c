// SPDX-License-Identifier: GPL-2.0-or-later
/* C-Media CMI8338/8738 register algorithms adapted from Linux v6.12,
 * sound/pci/cmipci.c, Copyright (c) 2000 Takashi Iwai <tiwai@suse.de>.
 * Origin and decisions: docs/design/f4-cmipci.md. Ciuki owns all DMA/IRQs.
 */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/audio_backend.h>
#define CMI_FRAMES (AUDIO_HW_SLOTS * AUDIO_QUANTUM)
#define CMI_ENABLE(ch) (0x10000u << (ch))
#define CMI_RESET(ch) (0x40000u << (ch))
#define CMI_BUSY(ch) (4u << (ch))
#define CMI_IRQ(ch) (0x10000u << (ch))
#define CMI_LEGACY 0x0eu
/* C-Media lacks the SB16 I/O gain registers 3fh/40h. */
static const uint8_t cmi_mixer_regs[] = {0x30,0x31,0x32,0x33,0x3c,0x3d,0x3e,0xf0};
static const unsigned cmi_rates[] = {5512,11025,22050,44100,8000,16000,32000,48000};
bool cmipci_match(const struct pci_func *p, enum audio_backend_kind *kind)
{
    if (p->class_code != 4 || p->subclass != 1) return false;
    if (!((p->vendor == 0x13f6 && (p->device == 0x100 || p->device == 0x101 ||
                                 p->device == 0x111 || p->device == 0x112)) ||
          (p->vendor == 0x10b9 && p->device == 0x111))) return false;
    *kind = AUDIO_CMIPCI; return true;
}
static int cmi_rate(unsigned rate)
{
    for (unsigned i=0;i<ARRAY_SIZE(cmi_rates);i++) if (rate==cmi_rates[i]) return (int)i;
    return -EINVAL;
}
static uint8_t cmi_mixer_read(struct audio_dma_device *d, unsigned reg)
{
    uint32_t f=irq_save();
    audio_hw_write(d,0x23,reg,1); uint8_t value=audio_hw_read(d,0x22,1);
    irq_restore(f); return value;
}
static int cmi_mixer_write(struct audio_dma_device *d, unsigned reg, uint8_t value)
{
    uint32_t f=irq_save();
    audio_hw_write(d,0x23,reg,1); audio_hw_write(d,0x22,value,1);
    uint8_t got=audio_hw_read(d,0x22,1);
    irq_restore(f); return got==value ? 0 : -EIO;
}
static int cmi_checked(struct audio_dma_device *d, unsigned reg, uint32_t value, unsigned width, uint32_t mask)
{
    audio_hw_write(d,reg,value,width);
    return ((audio_hw_read(d,reg,width)^value)&mask) ? -EIO : 0;
}
static int cmi_mute(struct audio_dma_device *d, bool mute)
{
    if (d->state.quarantine || !d->touched || !d->cmi.phys) return -EIO;
    /* Master volume is applied once by the common software mixer. */
    int e=cmi_checked(d,0x24,0x80 | (mute ? 0x40 : 0),1,255);
    if (!e) { d->state.master=mute ? 0x8000 : 0; d->state.pcm_volume=0; }
    return e;
}
static int cmi_cursor_channel(struct audio_dma_device *d, unsigned ch, unsigned *frame)
{
    for (unsigned i=0;i<3;i++) {
        unsigned remaining=audio_hw_read(d,0x84+ch*8,2);
        if (remaining<CMI_FRAMES) { *frame=CMI_FRAMES-1-remaining; return 0; }
    }
    return -EIO;
}
static int cmi_cursor(struct audio_dma_device *d, unsigned *frame)
{ return cmi_cursor_channel(d,0,frame); }
static void cmi_gate(struct audio_dma_device *d, unsigned ch)
{
    uint32_t f=irq_save();
    d->serial &= ~CMI_IRQ(ch); audio_hw_write(d,0xc,d->serial,4);
    d->control &= ~CMI_ENABLE(ch); audio_hw_write(d,0,d->control,4);
    irq_restore(f);
}
static void cmi_inhibit(struct audio_dma_device *d)
{
    cmi_gate(d,0);
    if (d->state.quarantine) cmi_gate(d,1);
}
static int cmi_stop_channel(struct audio_dma_device *d, unsigned ch)
{
    cmi_gate(d,ch);
    uint64_t end=audio_hw_deadline(d,500);
    do {
        unsigned a, b;
        uint32_t ctrl=audio_hw_read(d,0,4), status=audio_hw_read(d,0x10,4);
        if (ctrl!=UINT32_MAX && status!=UINT32_MAX && !(ctrl&CMI_ENABLE(ch)) &&
            !(status&CMI_BUSY(ch)) && !cmi_cursor_channel(d,ch,&a)) {
            d->io.sleep(d->io.cookie,1);
            ctrl=audio_hw_read(d,0,4); status=audio_hw_read(d,0x10,4);
            if (ctrl!=UINT32_MAX && status!=UINT32_MAX && !(ctrl&CMI_ENABLE(ch)) &&
                !(status&CMI_BUSY(ch)) && !cmi_cursor_channel(d,ch,&b) && a==b) {
                if (d->state.quarantine) return -EIO;
                /* Channel-local reset, only after idle proof; preserve peer. */
                uint32_t f=irq_save();
                audio_hw_write(d,0,d->control | CMI_RESET(ch),4);
                audio_hw_write(d,0,d->control,4);
                uint32_t got=audio_hw_read(d,0,4);
                irq_restore(f);
                if (got==d->control && !(audio_hw_read(d,0xc,4)&CMI_IRQ(ch))) return 0;
                break;
            }
        }
        d->io.sleep(d->io.cookie,1);
    } while ((int64_t)(d->io.now(d->io.cookie)-end)<0);
    d->reason="dma_stop_unproved"; d->state.quarantine=true; return -EIO;
}
static int cmi_halt(struct audio_dma_device *d)
{
    int e=cmi_stop_channel(d,0);
    if (!e) d->state.running=false;
    return e;
}
static int cmi_program(struct audio_dma_device *d, unsigned ch, unsigned rate, bool proof)
{
    int freq=cmi_rate(rate); if (freq<0) return freq;
    uint32_t f=irq_save();
    unsigned shift=10+ch*3;
    uint32_t fn=audio_hw_read(d,4,4);
    int e=fn==UINT32_MAX ? -EIO : cmi_checked(d,4,(fn&~(7u<<shift)) | (unsigned)freq<<shift,4,0xffff);
    if (!e) {
        uint32_t format=audio_hw_read(d,8,4);
        e=cmi_checked(d,8,(format&~(3u<<(ch*2))) | 3u<<(ch*2),4,15);
    }
    if (!e && ch && d->cmi.model) {
        uint32_t ext=audio_hw_read(d,0x90,4);
        e=cmi_checked(d,0x90,(ext&~0x10000000u) | (rate>44100 ? 0x10000000u : 0),4,0x10000000);
    }
    if (!e) {
        audio_hw_write(d,0x80+ch*8,ch ? d->cmi.phys : d->phys,4);
        audio_hw_write(d,0x84+ch*8,CMI_FRAMES-1,2);
        audio_hw_write(d,0x86+ch*8,AUDIO_QUANTUM-1,2);
        __asm__ volatile("" ::: "memory");
        d->serial &= ~CMI_IRQ(ch); audio_hw_write(d,0xc,d->serial,4);
        if (!proof) d->serial |= CMI_IRQ(ch);
        audio_hw_write(d,0xc,d->serial,4);
        d->control |= CMI_ENABLE(ch);
        e=cmi_checked(d,0,d->control,4,0xfffff);
    }
    irq_restore(f); return e;
}
static int cmi_start(struct audio_dma_device *d, bool proof)
{ return cmi_program(d,0,48000,proof); }
static bool cmi_ack(struct audio_dma_device *d)
{
    uint32_t status=audio_hw_read(d,0x10,4);
    if (status==UINT32_MAX || !(status&0x80000000u)) return false;
    uint32_t mask=(status&3)<<16;
    if (!mask) return false; /* never claim a UART/legacy/foreign IRQ */
    uint32_t f=irq_save();
    audio_hw_write(d,0xc,d->serial&~mask,4);
    audio_hw_write(d,0xc,d->serial,4);
    if (status&2) d->cmi.irqs++;
    irq_restore(f); return true;
}
static int cmi_prepare(struct audio_dma_device *d)
{
    if (!(d->command&1)) { d->reason="firmware_state_unreadable"; return -ENODEV; }
    uint32_t ctrl=audio_hw_read(d,0,4), fn=audio_hw_read(d,4,4);
    uint32_t status=audio_hw_read(d,0x10,4), misc=audio_hw_read(d,0x18,4);
    uint32_t id=audio_hw_read(d,0xc,4), format=audio_hw_read(d,8,4);
    if (ctrl==UINT32_MAX || fn==UINT32_MAX || status==UINT32_MAX || misc==UINT32_MAX ||
        id==UINT32_MAX || format==UINT32_MAX) { d->reason="registers_unreadable"; return -EIO; }
    if ((ctrl&0xf0000) || (status&0xc) || (fn&CMI_LEGACY) || (misc&(0x80000|0xe40))) {
        d->reason="firmware_function_active"; return -EBUSY;
    }
    if (d->pci.device>=0x110) {
        uint32_t signature=id&0xff000000;
        if (signature && signature!=0x04000000 && signature!=0x05000000 && signature!=0x08000000) {
            d->reason="chip_revision_unsupported"; return -ENODEV;
        }
        unsigned old_signature=format&0x1f000000;
        d->cmi.model=signature==0x08000000 ? 55 : (id&0x04000000) ? 39 :
            !old_signature ? 33 : old_signature==0x01000000 ? 37 : 39;
    }
    d->touched=true;
    audio_pci_write(&d->io,&d->pci,4,(d->command|1u)&~4u,2);
    if ((audio_pci_read(&d->io,&d->pci,4)&0xffff)!=((d->command|1u)&~4u)) return -EIO;
    d->cmi.misc=misc; d->cmi.ext=d->cmi.model ? audio_hw_read(d,0x90,4) : 0;
    for (unsigned i=0;i<ARRAY_SIZE(cmi_mixer_regs);i++) d->cmi.mixer[i]=cmi_mixer_read(d,cmi_mixer_regs[i]);
    d->cmi.native[0]=audio_hw_read(d,0x24,1); d->cmi.native[1]=audio_hw_read(d,0x25,1);
    d->cmi.saved=true; d->codec_changed=true;
    d->control=2; d->serial=0;
    int e=cmi_checked(d,0xc,0,4,0xf0000);
    if (!e) e=cmi_checked(d,0,2,4,0xfffff);
    if (!e) e=cmi_checked(d,4,0x10 | (7u<<10) | (7u<<13),4,0xffff);
    /* Stereo analog front DAC only. No SPDIF, modem, multi-channel or FM. */
    if (!e) e=cmi_checked(d,8,0xf,4,0xafffff);
    if (!e) e=cmi_checked(d,0x14,0,4,0xffffffff);
    uint32_t native_misc=misc&~(0xdcfc00e0u | 0x00100000u);
    native_misc |= 0x00800000; /* double-DAC mode, channel A is front */
    bool vx=false;
    for (unsigned i=0;i<pci_count();i++) {
        const struct pci_func *p=pci_get(i);
        if (p->vendor==0x8086 && p->device==0x7030) vx=true;
    }
    if (d->cmi.model && !vx) native_misc |= 0x08000000;
    if (!e) e=cmi_checked(d,0x18,native_misc,4,0xe8fc00e0u);
    if (!e && d->cmi.model) {
        audio_hw_write(d,0x90,0x20,1); audio_hw_write(d,0x91,0x09,1);
    }
    for (unsigned i=0;!e && i<ARRAY_SIZE(cmi_mixer_regs);i++) {
        unsigned reg=cmi_mixer_regs[i];
        uint8_t value=i<4 ? 0xf8 : reg==0x3d ? 0x10 : reg==0x3e ? 8 : 0;
        e=cmi_mixer_write(d,reg,value);
    }
    if (!e) e=cmi_checked(d,0x25,1,1,255); /* no aux monitor; MICGAINZ disables boost */
    if (!e) e=cmi_mute(d,audio.master.mute);
    d->state.rate=48000; /* no external AC'97 ID; PCI identity is authoritative */
    if (e) { d->reason="mixer_prepare"; return e; }
    audio_pci_write(&d->io,&d->pci,4,d->command|5,2);
    return (audio_pci_read(&d->io,&d->pci,4)&0xffff)==(d->command|5u) ? 0 : -EIO;
}
const struct audio_dma_ops cmipci_ops={cmi_prepare,cmi_start,cmi_halt,cmi_cursor,cmi_mute,cmi_ack,cmi_inhibit};

int cmipci_capture_start(struct audio_dma_device *d, unsigned rate)
{
    if (d->kind!=AUDIO_CMIPCI || cmi_rate(rate)<0) return -EINVAL;
    kmutex_lock(&d->register_lock);
    int e=0;
    if (d->state.quarantine || (!d->state.ready && !d->proving) || !d->cmi.phys) e=-EIO;
    else if (d->cmi.running) e=-EBUSY;
    else {
        memset(d->cmi.pcm,0,AUDIO_HW_BYTES);
        d->cmi.running=true; d->cmi.failed=false; d->cmi.rate=rate;
        d->cmi.frame=0; d->cmi.produced=d->cmi.consumed=0;
        e=cmi_program(d,1,rate,d->proving);
        d->cmi.observed=d->cmi.progressed=d->io.now(d->io.cookie);
        if (e) { d->cmi.failed=true; cmi_gate(d,1); }
    }
    kmutex_unlock(&d->register_lock); return e;
}
int cmipci_capture_stop(struct audio_dma_device *d)
{
    if (d->kind!=AUDIO_CMIPCI) return -EINVAL;
    if (!d->touched || !d->cmi.phys) return 0;
    kmutex_lock(&d->register_lock);
    int e=cmi_stop_channel(d,1);
    if (!e) d->cmi.running=false;
    kmutex_unlock(&d->register_lock); return e;
}
int cmipci_capture_read(struct audio_dma_device *d, struct audio_frame *out)
{
    if (d->kind!=AUDIO_CMIPCI || !out) return -EINVAL;
    kmutex_lock(&d->register_lock);
    struct cmipci_state *c=&d->cmi;
    int e=-EAGAIN; unsigned frame;
    if (!c->running || c->failed || d->state.quarantine) e=-EIO;
    else {
        uint64_t now=d->io.now(d->io.cookie);
        if (now-c->observed>=CMI_FRAMES*1000u/c->rate || cmi_cursor_channel(d,1,&frame)) e=-EIO;
        else {
            unsigned delta=(frame+CMI_FRAMES-c->frame)%CMI_FRAMES;
            if (delta) c->progressed=now;
            c->produced+=delta;
            c->frame=frame; c->observed=now;
            if (now-c->progressed>100) e=-EIO;
            else if (c->produced-c->consumed>=CMI_FRAMES-AUDIO_QUANTUM) { c->overruns++; e=-EIO; }
            else if (c->produced-c->consumed>=AUDIO_QUANTUM) {
                unsigned at=c->consumed%CMI_FRAMES;
                /* Copy to staging first, recheck DMA before exposing bytes. */
                struct audio_frame staging[AUDIO_QUANTUM];
                memcpy(staging,c->pcm+at,sizeof(staging));
                __asm__ volatile("" ::: "memory");
                unsigned after;
                uint64_t end=d->io.now(d->io.cookie);
                if (end-now>=CMI_FRAMES*1000u/c->rate || cmi_cursor_channel(d,1,&after) ||
                    c->produced+(after+CMI_FRAMES-frame)%CMI_FRAMES-c->consumed>=CMI_FRAMES-AUDIO_QUANTUM) e=-EIO;
                else { memcpy(out,staging,sizeof(staging)); c->consumed+=AUDIO_QUANTUM; e=0; }
            }
        }
        if (e==-EIO) { c->failed=true; cmi_gate(d,1); }
    }
    kmutex_unlock(&d->register_lock); return e;
}
int cmipci_capture_prove(struct audio_dma_device *d)
{
    d->proving=true;
    int e=cmipci_capture_start(d,48000);
    uint64_t end=audio_hw_deadline(d,500);
    bool moved=false;
    while (!e && (int64_t)(d->io.now(d->io.cookie)-end)<0) {
        unsigned frame;
        e=cmi_cursor_channel(d,1,&frame);
        if (e || frame) { moved=!e; break; }
        d->io.sleep(d->io.cookie,1);
    }
    int stop=cmipci_capture_stop(d); d->proving=false;
    return stop ? stop : e ? e : moved ? 0 : -ETIMEDOUT;
}
int cmipci_restore(struct audio_dma_device *d)
{
    if (d->state.quarantine || d->cmi.running || d->state.running) return -EIO;
    if (!d->cmi.saved) return 0;
    int e=cmi_checked(d,4,0,4,0xffff); /* disable internal bus request and all decoders */
    for (unsigned i=0;!e && i<ARRAY_SIZE(cmi_mixer_regs);i++) e=cmi_mixer_write(d,cmi_mixer_regs[i],d->cmi.mixer[i]);
    if (!e) e=cmi_checked(d,0x24,d->cmi.native[0],1,255);
    if (!e) e=cmi_checked(d,0x25,d->cmi.native[1],1,255);
    if (!e && d->cmi.model) e=cmi_checked(d,0x90,d->cmi.ext,4,0xffffffff);
    if (!e) e=cmi_checked(d,0x18,d->cmi.misc,4,~0x01000000u); /* bit24 differs on read */
    if (!e) d->codec_changed=d->cmi.saved=false;
    return e;
}
