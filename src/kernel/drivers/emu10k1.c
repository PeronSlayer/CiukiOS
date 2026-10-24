// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) by Jaroslav Kysela <perex@perex.cz>
 *                   Lee Revell <rlrevell@joe-job.com>
 *                   James Courtier-Dutton <James@superbug.co.uk>
 *                   Oswald Buddenhagen <oswald.buddenhagen@gmx.de>
 *                   Takashi Iwai <tiwai@suse.de>
 *                   Creative Labs, Inc.
 *
 * Adapted from Linux v6.12 sound/pci/emu10k1/{emu10k1_main,emupcm,
 * emufx,io,memory,irq,voice,emumixer}.c and include/{sound,uapi/sound}/emu10k1.h.
 * Ciuki modifications: bounded private PCM transport, fixed stereo voices,
 * generated routing, copied ADC half-rings, claims/pins and stop quarantine.
 * File origins and retained notices: docs/licenses/emu10k1-NOTICES.txt.
 */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/mm.h>
#include <ciuki/emu10k1.h>

enum {
    E_PTR=0, E_DATA=4, E_IPR=8, E_INTE=12, E_HCFG=0x14,
    E_IOCFG=0x18, E_TIMER=0x1a, E_ACDATA=0x1c, E_ACADDR=0x1e,
    E_CPF=0, E_PTRX=1, E_CVCF=2, E_VTFT=3, E_PSST=6, E_DSL=7,
    E_CCCA=8, E_CCR=9, E_FXRT=0xb, E_MAPA=0xc, E_MAPB=0xd,
    E_DCYSUSV=0x12, E_CD0=0x20, E_PTB=0x40, E_ADCCR=0x42,
    E_ADCBA=0x46, E_MICBS=0x49, E_ADCBS=0x4a, E_FXBS=0x4b,
    E_CLIEL=0x58, E_CLIEH=0x59, E_SOLEL=0x5c, E_SOLEH=0x5d,
    E_TIMER_IPR=0x200, E_ADC_IPR=0xc000, E_PCI_IPR=0x200000,
    E_TIMER_INTE=4, E_ADC_INTE=0x40, E_PCI_INTE=0x800,
    E_CACHE_LOCK=0xc, E_STEREO=0x8000, E_CACHE_AHEAD=61,
    E_CAPTURE_BYTES=8192, E_CAPTURE_FRAMES=2048, E_CAPTURE_HALF=1024,
    E_RING_FRAMES=AUDIO_HW_SLOTS*AUDIO_QUANTUM
};
bool emu10k1_kind(enum audio_backend_kind kind)
{ return kind==AUDIO_EMU10K1 || kind==AUDIO_EMU10K2; }
bool emu10k1_match(const struct pci_func *p, enum audio_backend_kind *kind)
{
    if (p->vendor!=0x1102 || p->class_code!=4 || p->subclass!=1) return false;
    if (p->device==2) { *kind=AUDIO_EMU10K1; return true; }
    if (p->device==4) { *kind=AUDIO_EMU10K2; return true; }
    return false; /* LS/CA0106, Value, CardBus and PCIe are other engines */
}
static unsigned emu_table_pages(const struct audio_dma_device *d)
{ return d->kind==AUDIO_EMU10K1 ? 8 : 4; }
unsigned emu10k1_aux_pages(enum audio_backend_kind kind)
{ return (kind==AUDIO_EMU10K1 ? 8u : 4u)+3; }
uint32_t emu10k1_dma_mask(enum audio_backend_kind kind)
{ return kind==AUDIO_EMU10K1 ? 0x7fffffffu : UINT32_MAX; }
/* Every PTR/DATA pair is atomic against the timer IRQ. Never hold IF clear
 * across a loop, a codec-ready wait or a scheduler call. */
static uint32_t emu_read(struct audio_dma_device *d, unsigned reg, unsigned voice)
{
    uint32_t f=irq_save();
    audio_hw_write(d,E_PTR,(reg<<16)|voice,4);
    uint32_t v=audio_hw_read(d,E_DATA,4); irq_restore(f); return v;
}
static void emu_write(struct audio_dma_device *d, unsigned reg, unsigned voice, uint32_t value)
{
    uint32_t f=irq_save();
    audio_hw_write(d,E_PTR,(reg<<16)|voice,4);
    audio_hw_write(d,E_DATA,value,4); irq_restore(f);
}
static int emu_ready(struct audio_dma_device *d)
{
    uint64_t end=audio_hw_deadline(d,100);
    do {
        unsigned v=audio_hw_read(d,E_ACADDR,1);
        if (v==255) return -EIO;
        if (v&0x80) return 0;
        d->io.sleep(d->io.cookie,1);
    } while ((int64_t)(d->io.now(d->io.cookie)-end)<0);
    return -ETIMEDOUT;
}
static int emu_codec_read(struct audio_dma_device *d, uint16_t reg, uint16_t *value)
{
    int e=emu_ready(d); if (e) return e;
    uint32_t f=irq_save();
    audio_hw_write(d,E_ACADDR,reg,1); *value=audio_hw_read(d,E_ACDATA,2);
    irq_restore(f); return *value==0xffff ? -EIO : 0;
}
int emu10k1_codec_write(struct audio_dma_device *d, uint16_t reg, uint16_t value)
{
    int e=emu_ready(d); if (e) return e;
    uint32_t f=irq_save();
    audio_hw_write(d,E_ACADDR,reg,1); audio_hw_write(d,E_ACDATA,value,2);
    (void)audio_hw_read(d,E_ACDATA,2); /* drain the posted write */
    irq_restore(f);
    uint16_t got; e=emu_codec_read(d,reg,&got);
    uint16_t mask=reg==0x26 ? 0xff00 : 0xffff; /* readiness bits are read-only */
    return e ? e : ((got^value)&mask) ? -EIO : 0;
}
static int emu_busmaster(struct audio_dma_device *d, bool enabled)
{
    uint16_t cmd=(d->command|1u)&~4u;
    if (enabled) cmd|=4;
    audio_pci_write(&d->io,&d->pci,4,cmd,2);
    return (audio_pci_read(&d->io,&d->pci,4)&0xffff)==cmd ? 0 : -EIO;
}
static unsigned emu_debug(const struct audio_dma_device *d)
{ return d->kind==AUDIO_EMU10K1 ? 0x52 : 0x53; }
static uint32_t emu_step(const struct audio_dma_device *d)
{ return d->kind==AUDIO_EMU10K1 ? 0x8000 : 0x20000; }
static unsigned emu_adc_index(const struct audio_dma_device *d)
{ return d->kind==AUDIO_EMU10K1 ? 0x64 : 0x63; }
static uint32_t emu_capture_phys(const struct audio_dma_device *d)
{ return d->desc_phys+(emu_table_pages(d)+1)*PAGE_SIZE; }
static void emu_instruction(struct audio_dma_device *d, unsigned pc, unsigned dst, unsigned src)
{
    bool a=d->kind==AUDIO_EMU10K2;
    unsigned shift=a ? 12 : 10, zero=a ? 0xc0 : 0x40, base=a ? 0x600 : 0x400;
    /* ACC3: R = A + X + Y. No effects, volume table or external firmware. */
    emu_write(d,base+pc*2,0,(zero<<shift)|zero);
    emu_write(d,base+pc*2+1,0,(6u<<(a ? 24 : 20))|(dst<<shift)|src);
}
static int emu_program(struct audio_dma_device *d)
{
    bool a=d->kind==AUDIO_EMU10K2;
    unsigned zero=a ? 0xc0 : 0x40, out=a ? 0x60 : 0x20, pc=0;
    emu_write(d,emu_debug(d),0,emu_step(d));
    /* Zero every physical/FX capture output before assigning the four paths. */
    for (unsigned i=0;i<32;i++) emu_instruction(d,pc++,out+i,zero);
    for (unsigned i=0;i<2;i++) {
        emu_instruction(d,pc++,a ? 0x68+i : 0x20+i,i);
        if (a) emu_instruction(d,pc++,0x70+i,i);
        emu_instruction(d,pc++,a ? 0x76+i : 0x2a+i,(a ? 0x40 : 0x10)+i);
    }
    unsigned instructions=a ? 1024 : 512;
    for (;pc<instructions;pc++) {
        emu_instruction(d,pc,zero,zero);
        if (!(pc&63)) {
            if ((int64_t)(d->io.now(d->io.cookie)-d->activation_deadline)>=0 && d->activation_deadline)
                return -ETIMEDOUT;
            d->io.sleep(d->io.cookie,0);
        }
    }
    /* Read back a complete route pair, so a dead index window never passes. */
    unsigned base=a ? 0x600 : 0x400, shift=a ? 12 : 10;
    unsigned dst=a ? 0x68 : 0x20;
    if (emu_read(d,base+64,0)!=((zero<<shift)|zero) ||
        emu_read(d,base+65,0)!=((6u<<(a ? 24 : 20))|(dst<<shift))) return -EIO;
    return 0;
}
static void emu_inhibit(struct audio_dma_device *d)
{
    audio_hw_write(d,E_INTE,0,4);
    for (unsigned v=0;v<2;v++) {
        emu_write(d,E_DCYSUSV,v,0); emu_write(d,E_PTRX,v,0);
        emu_write(d,E_CPF,v,E_STEREO); emu_write(d,E_VTFT,v,0xffff);
        emu_write(d,E_CVCF,v,0xffff);
    }
    emu_write(d,E_ADCBS,0,0); emu_write(d,E_ADCCR,0,0);
    emu_write(d,emu_debug(d),0,emu_step(d));
    audio_hw_write(d,E_HCFG,E_CACHE_LOCK,4);
    /* BME readback is part of halt's proof, never of IRQ-time release. */
    (void)emu_busmaster(d,false);
    d->emu.capture_running=false;
}
static int emu_halt(struct audio_dma_device *d)
{
    emu_inhibit(d);
    uint64_t end=audio_hw_deadline(d,500);
    uint32_t last=emu_read(d,E_CCCA,0), adc=emu_read(d,emu_adc_index(d),0);
    unsigned stable=0;
    do {
        bool idle=audio_hw_read(d,E_INTE,4)==0 &&
            (audio_hw_read(d,E_HCFG,4)&(E_CACHE_LOCK|1))==E_CACHE_LOCK &&
            !(audio_pci_read(&d->io,&d->pci,4)&4) &&
            emu_read(d,E_ADCBS,0)==0 && emu_read(d,E_ADCCR,0)==0;
        for (unsigned v=0;v<2;v++) idle=idle &&
            (emu_read(d,E_CPF,v)&0xffff0000u)==0 &&
            (emu_read(d,E_PTRX,v)&0xffff0000u)==0 && emu_read(d,E_DCYSUSV,v)==0;
        uint32_t now=emu_read(d,E_CCCA,0), next=emu_read(d,emu_adc_index(d),0);
        if (idle && now!=UINT32_MAX && next!=UINT32_MAX && now==last && next==adc) stable++;
        else stable=0;
        if (stable==3) {
            audio_hw_write(d,E_IPR,E_TIMER_IPR|E_ADC_IPR|E_PCI_IPR,4);
            return 0;
        }
        last=now; adc=next; d->io.sleep(d->io.cookie,1);
    } while ((int64_t)(d->io.now(d->io.cookie)-end)<0);
    d->reason="emu_stop_unproved"; return -ETIMEDOUT;
}
static int emu_mute(struct audio_dma_device *d, bool mute)
{
    if (d->state.quarantine) return -EIO;
    int e=audio_dma_codec_mute(d,mute,emu10k1_codec_write);
    if (!e) {
        uint32_t h=d->control|(mute ? 0u : 1u);
        audio_hw_write(d,E_HCFG,h,4);
        if ((audio_hw_read(d,E_HCFG,4)&1)!=(h&1)) e=-EIO;
    }
    return e;
}
static void emu_input_attach(struct audio_dma_device *d);
static int emu_prepare(struct audio_dma_device *d)
{
    /* Exact original Audigy boards only: do not run an AC97 path on E-MU's
     * FPGA hardware, Audigy2's P16V or a notebook's external SPI/I2C codecs. */
    if (d->kind==AUDIO_EMU10K2 && (d->pci.sub_vendor!=0x1102 || d->pci.revision>=4 ||
        (d->pci.sub_device!=0x0051 && d->pci.sub_device!=0x0052 && d->pci.sub_device!=0x0053))) {
        d->reason=(d->pci.sub_device==0x4001 || d->pci.sub_device==0x4002) ?
            "emu_fpga_firmware_excluded" : "emu_variant_excluded";
        return -ENODEV;
    }
    uint32_t mask=emu10k1_dma_mask(d->kind);
    if (!d->phys || !d->desc_phys || !d->pcm || !d->desc ||
        d->desc_pages!=emu10k1_aux_pages(d->kind) ||
        (d->phys&(PAGE_SIZE-1)) || (d->desc_phys&(PAGE_SIZE-1)) ||
        d->phys>mask-(AUDIO_HW_BYTES-1) || d->desc_phys>mask-(d->desc_pages*PAGE_SIZE-1)) {
        d->reason="emu_dma_address"; return -EINVAL;
    }
    if (audio_hw_read(d,E_INTE,4) || emu_read(d,E_ADCBS,0) ||
        emu_read(d,E_MICBS,0) || emu_read(d,E_FXBS,0)) {
        d->reason="firmware_dma_active"; return -EBUSY;
    }
    for (unsigned v=0;v<64;v++) if ((emu_read(d,E_CPF,v)&0xffff0000u) ||
        (emu_read(d,E_DCYSUSV,v)&0x80)) { d->reason="firmware_voice_active"; return -EBUSY; }
    d->emu.hcfg=audio_hw_read(d,E_HCFG,4);
    d->emu.iocfg=d->kind==AUDIO_EMU10K2 ? audio_hw_read(d,E_IOCFG,2) : 0;
    if (d->emu.hcfg==UINT32_MAX || d->emu.iocfg==0xffff) return -EIO;
    d->emu.config_saved=true; d->touched=true;
    int e=emu_busmaster(d,false); if (e) return e;
    audio_hw_write(d,E_INTE,0,4); audio_hw_write(d,E_HCFG,E_CACHE_LOCK,4);
    emu_write(d,emu_debug(d),0,emu_step(d));
    /* Deterministically initialize all 64 voices, not just the stereo pair.
     * Clearing envelopes/modulation avoids inherited firmware state. */
    static const uint8_t regs[]={0,1,2,3,4,5,6,7,8,9,0xa,0xb,0xe,0xf,
        0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f};
    unsigned shift=d->kind==AUDIO_EMU10K1 ? 1 : 0;
    unsigned entries=emu_table_pages(d)*PAGE_SIZE/4;
    uint32_t silence=d->desc_phys+emu_table_pages(d)*PAGE_SIZE;
    uint32_t *pt=(uint32_t *)d->desc;
    for (unsigned i=0;i<entries;i++) pt[i]=(silence<<shift)|i;
    for (unsigned i=0;i<AUDIO_HW_BYTES/PAGE_SIZE;i++) pt[i]=((d->phys+i*PAGE_SIZE)<<shift)|i;
    __asm__ volatile("" ::: "memory");
    emu_write(d,E_PTB,0,d->desc_phys);
    for (unsigned v=0;v<64;v++) {
        for (unsigned i=0;i<sizeof(regs);i++) emu_write(d,regs[i],v,0);
        emu_write(d,E_MAPA,v,(silence<<shift)|(entries-1));
        emu_write(d,E_MAPB,v,(silence<<shift)|(entries-1));
    }
    emu_write(d,E_CLIEL,0,0); emu_write(d,E_CLIEH,0,0);
    emu_write(d,E_SOLEL,0,0); emu_write(d,E_SOLEH,0,0);
    emu_write(d,E_ADCCR,0,0); emu_write(d,E_ADCBS,0,0);
    emu_write(d,E_MICBS,0,0); emu_write(d,E_FXBS,0,0);
    emu_write(d,0x41,0,0); emu_write(d,0x44,0,0); /* external tank unused */
    if (emu_read(d,E_PTB,0)!=d->desc_phys) return -EIO;
    e=emu_program(d); if (e) return e;
    d->control=4u|0x10u|(d->kind==AUDIO_EMU10K2 ? 0x40000u : 0u);
    if (d->kind==AUDIO_EMU10K2) {
        audio_hw_write(d,E_IOCFG,d->emu.iocfg&~0x44u,2); /* analog jack */
        if ((audio_hw_read(d,E_IOCFG,2)&0x44)!=0) return -EIO;
    }
    e=audio_dma_codec_prepare(d,emu_codec_read,emu10k1_codec_write);
    if (e) return e;
    e=emu_codec_read(d,0x1a,&d->codec_saved[4]);
    if (!e) { d->codec_saved_count=5; e=emu_codec_read(d,0x1c,&d->codec_saved[5]); }
    if (!e) d->codec_saved_count=6;
    if (e) return e;
    d->state.rate=AUDIO_RATE;
    e=emu_mute(d,audio.master.mute);
    if (!e) emu_input_attach(d);
    return e;
}
static int emu_start(struct audio_dma_device *d, bool proof)
{
    if (d->state.quarantine || d->inhibited) return -EIO;
    for (unsigned v=0;v<2;v++) {
        emu_write(d,E_DCYSUSV,v,0); emu_write(d,E_CPF,v,E_STEREO);
        emu_write(d,E_PTRX,v,0xff00); /* send A only, unity */
        emu_write(d,E_PSST,v,0); emu_write(d,E_DSL,v,E_RING_FRAMES);
        emu_write(d,E_CCCA,v,E_CACHE_AHEAD); emu_write(d,0x19,v,0xff00);
        emu_write(d,E_VTFT,v,0xffffffff); emu_write(d,E_CVCF,v,0xffffffff);
        if (d->kind==AUDIO_EMU10K2) {
            emu_write(d,0x7e,v,v); emu_write(d,0x7c,v,0); emu_write(d,0x7d,v,0);
            for (unsigned i=0;i<4;i++) emu_write(d,0x4c+i,v,i ? 0 : 0xffff);
        } else emu_write(d,E_FXRT,v,v<<16);
        for (unsigned i=0;i<3;i++) emu_write(d,E_CD0+i,v,0);
        emu_write(d,E_CCR,v,E_CACHE_AHEAD<<25);
    }
    /* The odd voice's cache read pointer must agree before the master runs. */
    emu_write(d,emu_debug(d),0,0);
    audio_hw_write(d,E_IPR,E_TIMER_IPR|E_PCI_IPR,4);
    audio_hw_write(d,E_TIMER,AUDIO_QUANTUM,2);
    audio_hw_write(d,E_HCFG,d->control|((proof || audio.master.mute) ? 0u : 1u),4);
    int e=emu_busmaster(d,true); if (e) return e;
    emu_write(d,E_PTRX,0,0x4000ff00); emu_write(d,E_CPF,0,0x40008000);
    audio_hw_write(d,E_INTE,proof ? 0u : E_TIMER_INTE|E_PCI_INTE|
        (d->emu.capture_running ? E_ADC_INTE : 0u),4);
    return 0;
}
static int emu_cursor(struct audio_dma_device *d, unsigned *frame)
{
    uint32_t a=emu_read(d,E_CCCA,0), b=emu_read(d,E_CCCA,0);
    if (a==UINT32_MAX || b==UINT32_MAX || !(audio_pci_read(&d->io,&d->pci,4)&4)) return -EIO;
    a&=0xffffff; b&=0xffffff;
    if (a>=E_RING_FRAMES || b>=E_RING_FRAMES) return -EIO;
    if ((b+E_RING_FRAMES-a)%E_RING_FRAMES>64) return -EAGAIN;
    *frame=(b+E_RING_FRAMES-E_CACHE_AHEAD)%E_RING_FRAMES;
    return 0;
}
static bool emu_ack(struct audio_dma_device *d)
{
    uint32_t pending=audio_hw_read(d,E_IPR,4), enabled=audio_hw_read(d,E_INTE,4);
    if (pending==UINT32_MAX || enabled==UINT32_MAX) {
        d->inhibited=true; emu_inhibit(d); return false;
    }
    uint32_t owned=pending&((enabled&E_TIMER_INTE ? E_TIMER_IPR : 0u)|
        (enabled&E_ADC_INTE ? E_ADC_IPR : 0u)|(enabled&E_PCI_INTE ? E_PCI_IPR : 0u));
    if (!owned) return false;
    if (owned&E_PCI_IPR) { d->state.dma_halts++; d->inhibited=true; emu_inhibit(d); }
    audio_hw_write(d,E_IPR,owned,4); return true;
}
int emu10k1_restore(struct audio_dma_device *d)
{
    if (d->state.quarantine) return -EIO;
    if (d->codec_changed) {
        static const uint8_t regs[]={2,0x18,0x26,0x2a,0x1a,0x1c};
        static const uint8_t order[]={5,4,3,1,0,2};
        for (unsigned i=0;i<6;i++) if (order[i]<d->codec_saved_count) {
            unsigned at=order[i]; int e=emu10k1_codec_write(d,regs[at],d->codec_saved[at]); if (e) return e;
        }
        d->codec_changed=false;
    }
    if (d->emu.config_saved) {
        if (d->kind==AUDIO_EMU10K2) {
            audio_hw_write(d,E_IOCFG,d->emu.iocfg,2);
            if (audio_hw_read(d,E_IOCFG,2)!=d->emu.iocfg) return -EIO;
        }
        /* Restore GPIO/format facts, retaining cancelled DMA and mute. */
        uint32_t h=(d->emu.hcfg&~1u)|E_CACHE_LOCK;
        audio_hw_write(d,E_HCFG,h,4);
        if (audio_hw_read(d,E_HCFG,4)!=h) return -EIO;
        d->emu.config_saved=false;
    }
    return 0;
}
static int emu_capture_rate(struct audio_dma_device *d, unsigned rate)
{
    switch (rate) {
    case 48000: return 0; case 44100: return 1; case 32000: return 2;
    case 24000: return 3; case 22050: return 4; case 16000: return 5;
    case 12000: return d->kind==AUDIO_EMU10K2 ? 6 : -EINVAL;
    case 11025: return d->kind==AUDIO_EMU10K2 ? 7 : 6;
    case 8000: return d->kind==AUDIO_EMU10K2 ? 8 : 7;
    default: return -EINVAL;
    }
}
int emu10k1_capture_start(struct audio_dma_device *d, unsigned rate)
{
    if (!emu10k1_kind(d->kind) || !d->state.ready || d->state.quarantine || d->inhibited) return -EIO;
    int code=emu_capture_rate(d,rate); if (code<0) return code;
    kmutex_lock(&d->register_lock);
    if (!d->state.ready || d->state.quarantine || d->inhibited) { kmutex_unlock(&d->register_lock); return -EIO; }
    if (d->emu.capture_running || d->capture.generation || d->capture_reserved) { kmutex_unlock(&d->register_lock); return -EBUSY; }
    int e=emu10k1_codec_write(d,0x1a,0x0404); /* line-in, independent L/R */
    if (!e) e=emu10k1_codec_write(d,0x1c,0); /* ADC unity, unmuted */
    if (!e) {
        emu_write(d,E_ADCBS,0,0); emu_write(d,E_ADCCR,0,0);
        memset(P2V(emu_capture_phys(d)),0,E_CAPTURE_BYTES);
        emu_write(d,E_ADCBA,0,emu_capture_phys(d));
        /* Capture may start with playback stopped. Its AC97-to-ADC routing
         * still needs the DSP clock, while both playback pitches stay zero. */
        emu_write(d,emu_debug(d),0,0);
        __asm__ volatile("" ::: "memory");
        e=emu_busmaster(d,true);
    }
    if (!e) {
        uint32_t f=irq_save();
        audio_hw_write(d,E_IPR,E_ADC_IPR,4);
        emu_write(d,E_ADCCR,0,(d->kind==AUDIO_EMU10K2 ? 0x30u : 0x18u)|(unsigned)code);
        emu_write(d,E_ADCBS,0,0x13); /* documented 8192-byte ADC ring */
        d->emu.capture_running=true; d->emu.capture_failed=false;
        d->emu.capture_frame=0; d->emu.capture_produced=d->emu.capture_consumed=0;
        d->emu.capture_rate=rate; d->emu.capture_tick=d->io.now(d->io.cookie);
        audio_hw_write(d,E_INTE,audio_hw_read(d,E_INTE,4)|E_ADC_INTE|E_PCI_INTE,4);
        irq_restore(f);
    }
    kmutex_unlock(&d->register_lock); return e;
}
int emu10k1_capture_read(struct audio_dma_device *d, struct audio_frame *out, unsigned frames)
{
    if (!out || !frames || frames>E_CAPTURE_HALF) return -EINVAL;
    kmutex_lock(&d->register_lock);
    if (!d->emu.capture_running || d->emu.capture_failed || d->state.quarantine || d->inhibited) {
        kmutex_unlock(&d->register_lock); return -EIO;
    }
    uint64_t now=d->io.now(d->io.cookie);
    uint32_t pos=emu_read(d,emu_adc_index(d),0);
    int e=0;
    if (pos>=E_CAPTURE_BYTES || (pos&3) ||
        now-d->emu.capture_tick>=(E_CAPTURE_FRAMES*1000u)/d->emu.capture_rate) e=-EIO;
    if (!e) {
        unsigned frame=pos/4;
        d->emu.capture_produced+=(frame+E_CAPTURE_FRAMES-d->emu.capture_frame)%E_CAPTURE_FRAMES;
        d->emu.capture_frame=frame; d->emu.capture_tick=now;
        uint64_t completed=d->emu.capture_produced/E_CAPTURE_HALF*E_CAPTURE_HALF;
        if (d->emu.capture_produced-d->emu.capture_consumed>=E_CAPTURE_FRAMES) e=-EIO;
        else if (completed-d->emu.capture_consumed<frames) e=-EAGAIN;
        else {
            unsigned offset=d->emu.capture_consumed%E_CAPTURE_FRAMES;
            unsigned n=E_CAPTURE_HALF-offset%E_CAPTURE_HALF; if (frames>n) frames=n;
            memcpy(out,(struct audio_frame *)P2V(emu_capture_phys(d))+offset,frames*sizeof(*out));
            __asm__ volatile("" ::: "memory");
            uint32_t after=emu_read(d,emu_adc_index(d),0);
            /* If the writer entered the copied half, no bytes are returned. */
            if (after>=E_CAPTURE_BYTES || (after&3) ||
                d->io.now(d->io.cookie)-now>=(E_CAPTURE_HALF*1000u)/d->emu.capture_rate ||
                after/(E_CAPTURE_BYTES/2)==offset/E_CAPTURE_HALF) e=-EIO;
            else { d->emu.capture_consumed+=frames; e=(int)frames; }
        }
    }
    if (e==-EIO) {
        d->emu.capture_failed=true;
        emu_write(d,E_ADCBS,0,0); emu_write(d,E_ADCCR,0,0);
        uint32_t f=irq_save(); audio_hw_write(d,E_INTE,audio_hw_read(d,E_INTE,4)&~E_ADC_INTE,4); irq_restore(f);
    }
    kmutex_unlock(&d->register_lock); return e;
}
int emu10k1_capture_stop(struct audio_dma_device *d)
{
    if (!emu10k1_kind(d->kind) || d->state.quarantine) return -EIO;
    if (d->capture.generation) return -EBUSY;
    /* Whole-transport stop supplies PCI/cursor proof and drains capture too. */
    return audio_dma_stop(d);
}
const struct audio_dma_ops emu10k1_ops={emu_prepare,emu_start,emu_halt,emu_cursor,emu_mute,emu_ack,emu_inhibit};

/* Public ADC uses its own common pinned 32768-byte ring (ADCBS=1bh),
 * distinct from the old diagnostic half-ring. Polling needs no ADC IRQ.
 * No playback pitch, timer, DSP program, HCFG or BME changes on input stop. */
static int emu_input_cursor(struct audio_dma_device *d,unsigned *frame)
{
    uint32_t pos=emu_read(d,emu_adc_index(d),0);
    if (pos>=AUDIO_HW_BYTES || (pos&3)) return -EIO;
    *frame=pos/sizeof(struct audio_frame); return 0;
}
static int emu_input_start(struct audio_dma_device *d)
{
    if (d->emu.capture_running) return -EBUSY;
    if (emu_read(d,E_ADCBS,0) || emu_read(d,E_ADCCR,0)) return -EBUSY;
    emu_write(d,E_ADCBA,0,d->capture.phys);
    emu_write(d,emu_debug(d),0,0); /* ADC's existing AC97-to-DSP route */
    int e=emu_busmaster(d,true);
    if (e) return e;
    uint32_t f=irq_save();
    audio_hw_write(d,E_INTE,audio_hw_read(d,E_INTE,4)&~E_ADC_INTE,4);
    audio_hw_write(d,E_IPR,E_ADC_IPR,4);
    emu_write(d,E_ADCCR,0,d->kind==AUDIO_EMU10K2 ? 0x30 : 0x18);
    __asm__ volatile("" ::: "memory");
    emu_write(d,E_ADCBS,0,0x1b);
    bool valid=emu_read(d,E_ADCBA,0)==d->capture.phys && emu_read(d,E_ADCBS,0)==0x1b &&
        emu_read(d,E_ADCCR,0)==(d->kind==AUDIO_EMU10K2 ? 0x30u : 0x18u);
    irq_restore(f); return valid ? 0 : -EIO;
}
static int emu_input_halt(struct audio_dma_device *d)
{
    uint32_t f=irq_save();
    audio_hw_write(d,E_INTE,audio_hw_read(d,E_INTE,4)&~E_ADC_INTE,4);
    emu_write(d,E_ADCBS,0,0); emu_write(d,E_ADCCR,0,0);
    irq_restore(f);
    uint64_t end=audio_hw_deadline(d,500);
    unsigned last=UINT32_MAX,stable=0;
    do {
        unsigned now;
        bool idle=emu_read(d,E_ADCBS,0)==0 && emu_read(d,E_ADCCR,0)==0 &&
            !(audio_hw_read(d,E_INTE,4)&E_ADC_INTE) && !emu_input_cursor(d,&now);
        if (idle && now==last) stable++; else stable=0;
        if (stable==3) { audio_hw_write(d,E_IPR,E_ADC_IPR,4); return 0; }
        last=idle ? now : UINT32_MAX; d->io.sleep(d->io.cookie,1);
    } while ((int64_t)(d->io.now(d->io.cookie)-end)<0);
    return -EIO;
}
static void emu_input_attach(struct audio_dma_device *d)
{
    static const struct audio_capture_ops input={emu_input_start,emu_input_halt,emu_input_cursor};
    d->capture_ops=&input;
    d->voice_codec_read=emu_codec_read; d->voice_codec_write=emu10k1_codec_write;
}
