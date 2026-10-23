/* SPDX-License-Identifier: GPL-2.0-or-later
 * ESS Maestro 1/2/2E PCM, adapted for Ciuki's transport and ownership rules.
 * Origins: Linux v6.12 sound/pci/es1968.c and Linux v2.6.18 sound/oss/maestro.c.
 * Copyright (c) Matze Braun <MatzeBraun@gmx.de>, Takashi Iwai <tiwai@suse.de>.
 * Copyright (c) 1999 Alan Cox <alan.cox@linux.org>.
 * Based heavily on SonicVibes.c, Copyright (C) 1998-1999 Thomas Sailer
 * <sailer@ife.ee.ethz.ch>; heavily modified by Zach Brown <zab@zabbo.net>.
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2, or (at your option) any later version.
 * Distributed WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details (LICENSE and docs/licenses/maestro-NOTICES.txt).
 * Ciuki modifications: bounded indexed transactions, pinned arena, measured
 * clock, copied capture, firmware refusal, full stop proof and quarantine.
 */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/maestro.h>

#define MAESTRO_FRAMES (AUDIO_HW_SLOTS * AUDIO_QUANTUM)
#define MAESTRO_MODE 0x00f0u
static struct {
    struct audio_dma_device *owner;
    uint16_t shadow[32], base[6], config[5], ring[3], record[2], dock[7];
    uint32_t clock, capture_rate, capture_generation;
    uint64_t capture_observation, capture_produced, capture_consumed;
    unsigned capture_frame;
    bool saved, record_saved, dock_saved, e500, capture_running;
} maestro;
static const unsigned maestro_config_regs[] = {0x40,0x50,0x52,0x58,0x60};
static const unsigned maestro_dock_regs[] = {0xc0,0xc3,0xc4,0xc6,0xc8,0xcf,0xd0};

bool maestro_kind(enum audio_backend_kind kind)
{ return kind == AUDIO_MAESTRO1 || kind == AUDIO_MAESTRO2 || kind == AUDIO_MAESTRO2E; }
bool maestro_match(const struct pci_func *p, enum audio_backend_kind *kind)
{
    if (p->class_code != 4 || p->subclass != 1) return false;
    if (p->vendor == 0x1285 && p->device == 0x0100) *kind = AUDIO_MAESTRO1;
    else if (p->vendor == 0x125d && p->device == 0x1968) *kind = AUDIO_MAESTRO2;
    else if (p->vendor == 0x125d && p->device == 0x1978) *kind = AUDIO_MAESTRO2E;
    else return false; /* Maestro3, Solo1 and PT101 are not this engine. */
    return true;
}
/* Caller excludes interrupts over the complete nested index/data access.
 * Eight immediate readbacks bound IRQ work; no sleep or codec transaction. */
static void maestro_index_write(struct audio_dma_device *d, unsigned reg, uint16_t value)
{
    audio_hw_write(d,2,reg,2); audio_hw_write(d,0,value,2);
    maestro.shadow[reg] = value;
}
static uint16_t maestro_index_read(struct audio_dma_device *d, unsigned reg)
{
    if (0x64003fu & (1u << reg)) {
        audio_hw_write(d,2,reg,2); return audio_hw_read(d,0,2);
    }
    return maestro.shadow[reg];
}
static int maestro_apu(struct audio_dma_device *d, unsigned voice, unsigned reg, uint16_t *value, bool write)
{
    uint32_t flags = irq_save();
    uint16_t index = voice * 16 + reg;
    maestro_index_write(d,1,index);
    unsigned attempt;
    for (attempt=0; attempt<8; attempt++) if (maestro_index_read(d,1)==index) break;
    if (attempt==8) { irq_restore(flags); return -EIO; }
    if (write) {
        for (attempt=0; attempt<8; attempt++) {
            maestro_index_write(d,0,*value);
            if (maestro_index_read(d,0)==*value) break;
        }
        if (attempt==8) { irq_restore(flags); return -EIO; }
    } else *value = maestro_index_read(d,0);
    irq_restore(flags); return 0;
}
static int maestro_set(struct audio_dma_device *d, unsigned voice, unsigned reg, uint16_t value)
{ return maestro_apu(d,voice,reg,&value,true); }
static int maestro_get(struct audio_dma_device *d, unsigned voice, unsigned reg, uint16_t *value)
{ return maestro_apu(d,voice,reg,value,false); }
static int maestro_wave(struct audio_dma_device *d, unsigned reg, uint16_t value)
{
    uint32_t flags=irq_save();
    audio_hw_write(d,0x10,reg,2); audio_hw_write(d,0x12,value,2);
    uint16_t got=audio_hw_read(d,0x12,2);
    irq_restore(flags); return got==value ? 0 : -EIO;
}
static int maestro_codec_wait(struct audio_dma_device *d)
{
    uint64_t end=audio_hw_deadline(d,100);
    do {
        unsigned status=audio_hw_read(d,0x30,1);
        if (status==255) return -EIO;
        if (!(status&1)) return 0;
        d->io.sleep(d->io.cookie,1);
    } while ((int64_t)(d->io.now(d->io.cookie)-end)<0);
    return -ETIMEDOUT;
}
static int maestro_codec_read(struct audio_dma_device *d, uint16_t reg, uint16_t *value)
{
    int e=maestro_codec_wait(d);
    if (!e) { audio_hw_write(d,0x30,reg|0x80,1); e=maestro_codec_wait(d); }
    if (!e) { *value=audio_hw_read(d,0x32,2); if (*value==0xffff) e=-EIO; }
    return e;
}
static int maestro_codec_write(struct audio_dma_device *d, uint16_t reg, uint16_t value)
{
    int e=maestro_codec_wait(d);
    if (!e) {
        audio_hw_write(d,0x32,value,2); audio_hw_write(d,0x30,reg,1);
        e=maestro_codec_wait(d);
    }
    uint16_t got;
    if (!e) e=maestro_codec_read(d,reg,&got);
    unsigned mask=reg==0x26 ? 0xff00 : reg==2 || reg==0x18 ? 0x9f1f : 0xffff;
    return e ? e : ((got^value)&mask) ? -EIO : 0;
}
static int maestro_mute(struct audio_dma_device *d, bool mute)
{ return audio_dma_codec_mute(d,mute,maestro_codec_write); }
static int maestro_pci_word(struct audio_dma_device *d, unsigned reg, uint16_t value)
{
    audio_pci_write(&d->io,&d->pci,reg,value,2);
    return ((audio_pci_read(&d->io,&d->pci,reg)>>((reg&2)*8))&0xffff)==value ? 0 : -EIO;
}
static void maestro_timer(struct audio_dma_device *d, bool enable)
{
    uint32_t flags=irq_save();
    /* 50 MHz / 2^(5+9) / (3+1) = 763 Hz, below the donor's 800 Hz cap. */
    if (enable) maestro_index_write(d,6,0x9000 | (5<<5) | 3);
    maestro_index_write(d,0x11,enable ? 1 : 0);
    maestro_index_write(d,0x17,enable ? 1 : 0);
    irq_restore(flags);
    audio_hw_write(d,0x18,enable ? 4 : 0,2);
}
static int maestro_frequency(struct audio_dma_device *d, unsigned voice, uint32_t step)
{
    uint16_t value;
    int e=maestro_get(d,voice,2,&value);
    if (!e) e=maestro_set(d,voice,2,(value&255) | ((step&255)<<8) | 0x10);
    if (!e) e=maestro_set(d,voice,3,step>>8);
    return e;
}
static int maestro_voice(struct audio_dma_device *d, unsigned voice, unsigned offset,
                         unsigned size, unsigned route, bool playback)
{
    uint32_t address=d->phys+offset;
    int e=maestro_wave(d,voice<<3,((address-0x10)&0xfff8) | (playback ? 2 : 0));
    uint32_t pa=(offset>>1) | 0x400000;
    if (playback) { if (voice&1) pa|=0x800000; pa>>=1; }
    maestro.base[voice]=pa;
    for (unsigned reg=0; !e && reg<16; reg++) e=maestro_set(d,voice,reg,0);
    if (!e) e=maestro_set(d,voice,4,((pa>>16)&255)<<8);
    if (!e) e=maestro_set(d,voice,5,pa);
    if (!e) e=maestro_set(d,voice,6,pa+size);
    if (!e) e=maestro_set(d,voice,7,size);
    if (!e) e=maestro_set(d,voice,8,playback ? 0 : 0xf0);
    if (!e) e=maestro_set(d,voice,9,playback ? 0xd000 : 0);
    if (!e) e=maestro_set(d,voice,10,playback ? 0x8f00 | (voice ? 0 : 0x10) : 0x8f08);
    if (!e) e=maestro_set(d,voice,11,route);
    if (!e && !playback) e=maestro_set(d,voice,2,8);
    if (!e) e=maestro_set(d,voice,0,0x400f); /* DMA configured, mode OFF */
    return e;
}
static int maestro_mode(struct audio_dma_device *d, unsigned voice, unsigned mode)
{
    uint16_t value;
    int e=maestro_get(d,voice,0,&value);
    if (!e) e=maestro_set(d,voice,0,(value&~MAESTRO_MODE)|(mode<<4));
    return e;
}
static int maestro_position(struct audio_dma_device *d, unsigned voice, unsigned *frame)
{
    uint16_t value;
    int e=maestro_get(d,voice,5,&value);
    if (e) return e;
    unsigned delta=(uint16_t)(value-maestro.base[voice]);
    if (delta>=MAESTRO_FRAMES) return -EIO;
    *frame=delta; return 0;
}
static int maestro_cursor(struct audio_dma_device *d, unsigned *frame)
{
    uint32_t flags=irq_save(); unsigned left=0,right=0;
    int e=maestro_position(d,0,&left);
    if (!e) e=maestro_position(d,1,&right);
    irq_restore(flags);
    unsigned diff=(left+MAESTRO_FRAMES-right)%MAESTRO_FRAMES;
    if (!e && diff>1 && diff<MAESTRO_FRAMES-1) e=-EAGAIN;
    if (!e) *frame=(diff==1 ? right : left)&~1u; /* retire both channels; mask jitter bit */
    return e;
}
static void maestro_inhibit(struct audio_dma_device *d)
{
    maestro_timer(d,false);
    for (unsigned voice=0; voice<6; voice++) (void)maestro_mode(d,voice,0);
    /* Immediate gates are also used by IRQ exhaustion, never a reset. */
    audio_hw_write(d,0x14,0,2);
    audio_pci_write(&d->io,&d->pci,4,d->command&~4u,2);
}
static int maestro_halt(struct audio_dma_device *d)
{
    maestro.capture_running=false; maestro.capture_generation=0;
    maestro_inhibit(d);
    uint64_t end=audio_hw_deadline(d,500);
    do {
        bool idle=true; uint16_t positions[6];
        for (unsigned voice=0; voice<6; voice++) {
            uint16_t mode=0xffff;
            if (maestro_get(d,voice,0,&mode) || (mode&MAESTRO_MODE) ||
                maestro_get(d,voice,5,&positions[voice])) idle=false;
        }
        if (audio_pci_read(&d->io,&d->pci,4)&4 || audio_hw_read(d,0x14,2)!=0 || audio_hw_read(d,0x18,2)!=0) idle=false;
        d->io.sleep(d->io.cookie,2); /* drain posted transactions/cache before release */
        if (idle) for (unsigned voice=0; voice<6; voice++) {
            uint16_t got=0xffff,mode=0xffff;
            if (maestro_get(d,voice,5,&got) || got!=positions[voice] ||
                maestro_get(d,voice,0,&mode) || (mode&MAESTRO_MODE)) idle=false;
        }
        if (idle) {
            audio_hw_write(d,4,1,2); audio_hw_write(d,0x1a,4,1);
            d->state.running=false; return 0;
        }
    } while ((int64_t)(d->io.now(d->io.cookie)-end)<0);
    d->reason="dma_stop_unproved"; return -EIO;
}
static int maestro_start(struct audio_dma_device *d, bool proof)
{
    if (d->state.quarantine || d->inhibited) return -EIO;
    int e=maestro_wave(d,0x1fc,d->phys>>12);
    for (unsigned voice=0; !e && voice<2; voice++) {
        e=maestro_voice(d,voice,MAESTRO_PLAY_OFFSET,MAESTRO_FRAMES,0,true);
        if (!e) e=maestro_frequency(d,voice,(uint32_t)(((uint64_t)d->state.rate<<16)/maestro.clock));
    }
    if (!e) e=maestro_pci_word(d,4,d->command|5);
    if (!e) {
        audio_hw_write(d,0x14,0xa1a0,2); /* donor WC configuration, no SG/test */
        audio_hw_write(d,4,1,2); audio_hw_write(d,0x1a,4,1);
        uint32_t flags=irq_save();
        e=maestro_mode(d,0,2);
        if (!e) e=maestro_mode(d,1,2);
        irq_restore(flags);
        maestro_timer(d,!proof);
    }
    if (e) { d->inhibited=true; maestro_inhibit(d); }
    return e;
}
static bool maestro_ack(struct audio_dma_device *d)
{
    unsigned status=audio_hw_read(d,0x1a,1);
    if (status==255 || !(status&4)) return false;
    audio_hw_write(d,4,1,2); audio_hw_write(d,0x1a,4,1);
    return true; /* leave unrelated ASSP/MIDI/volume causes untouched */
}
static int maestro_measure_clock(struct audio_dma_device *d)
{
    maestro.clock=48000; d->state.rate=48000;
    int e=maestro_start(d,true);
    uint64_t begin=d->io.now(d->io.cookie);
    d->io.sleep(d->io.cookie,50);
    uint64_t elapsed=d->io.now(d->io.cookie)-begin;
    unsigned frame=0;
    if (!e) e=maestro_cursor(d,&frame);
    int stop=maestro_halt(d);
    if (stop) { d->state.quarantine=true; return stop; }
    if (e) return e;
    if (elapsed<50 || elapsed>100 || !frame) return -ETIMEDOUT;
    maestro.clock=frame*1000/(unsigned)elapsed;
    if (maestro.clock<40000 || maestro.clock>50000) return -EIO;
    return 0;
}
static int maestro_prepare(struct audio_dma_device *d)
{
    if (d->state.quarantine) return -EIO;
    if ((d->phys&(MAESTRO_ARENA_BYTES-1)) || !d->phys || d->phys>0x10000000u-MAESTRO_ARENA_BYTES ||
        d->dma_pages*4096u!=MAESTRO_ARENA_BYTES) { d->reason="dma_window"; return -EINVAL; }
    if (!(d->command&1)) { d->reason="firmware_state_unreadable"; return -ENODEV; }
    memset(&maestro,0,sizeof(maestro)); maestro.owner=d;
    maestro.e500=d->kind==AUDIO_MAESTRO2E && d->pci.sub_vendor==0x0e11 && d->pci.sub_device==0xb112;
    /* Register selection writes are protected by the claimed BAR. Refusal
     * leaves firmware modes/BME/GPIO/IRQ controls unchanged. */
    for (unsigned voice=0; voice<64; voice++) {
        uint16_t mode;
        if (maestro_get(d,voice,0,&mode) || mode==0xffff || (mode&MAESTRO_MODE)) {
            d->reason="firmware_engine_active"; return -EBUSY;
        }
    }
    if (audio_hw_read(d,0xa4,1)&1 || audio_hw_read(d,0xa6,1)&9 ||
        audio_hw_read(d,0x38,2)&0x20 || audio_hw_read(d,0x18,2)&~0x104u) {
        d->reason="firmware_link_dependency"; return -EBUSY;
    }
    for (unsigned i=0;i<5;i++) maestro.config[i]=audio_pci_read(&d->io,&d->pci,maestro_config_regs[i])>>((maestro_config_regs[i]&2)*8);
    for (unsigned i=0;i<3;i++) maestro.ring[i]=audio_hw_read(d,0x34+i*2,2);
    if (maestro.e500) for (unsigned i=0;i<7;i++) maestro.dock[i]=audio_hw_read(d,maestro_dock_regs[i],1);
    maestro.saved=true; maestro.dock_saved=maestro.e500;
    d->touched=true;
    int e=maestro_pci_word(d,4,d->command&~4u);
    if (!e) e=maestro_halt(d);
    if (e) { d->state.quarantine=true; return e; }
    e=maestro_pci_word(d,0x40,(maestro.config[0]|0x8000)&~0x401fu);
    if (!e) e=maestro_pci_word(d,0x50,(maestro.config[1]&~0x6f22u)|0xc0);
    if (!e) e=maestro_pci_word(d,0x52,(maestro.config[2]&~0xc12fu)|0x10);
    if (!e) e=maestro_pci_word(d,0x60,maestro.config[4]&~1u);
    if (!e && maestro.e500) e=maestro_pci_word(d,0x58,(maestro.config[3]|0xc)&~0x800u);
    if (e) return e;
    audio_hw_write(d,0x34,0xc090,2); audio_hw_write(d,0x36,0x3000,2);
    audio_hw_write(d,0x38,maestro.ring[2]&~0x10u,2);
    if (audio_hw_read(d,0x34,2)!=0xc090 || audio_hw_read(d,0x36,2)!=0x3000) return -EIO;
    /* Firmware GPIO is deliberately preserved: no reset pulse or NEC quirk. */
    for (unsigned i=0;i<32 && !e;i++) e=maestro_wave(d,0x1d0+i,0);
    uint32_t flags=irq_save();
    maestro_index_write(d,2,0);
    maestro_index_write(d,8,0xb004); maestro_index_write(d,9,0x001b);
    maestro_index_write(d,10,0x8000); maestro_index_write(d,11,0x3f37);
    maestro_index_write(d,12,0x8598); maestro_index_write(d,13,0x7632);
    irq_restore(flags);
    if (maestro.e500) for (unsigned i=0;i<7;i++) {
        unsigned value=i==0 ? maestro.dock[0]|0x20 : i<5 ? 0xff : 0x3f;
        audio_hw_write(d,maestro_dock_regs[i],value,1);
        if (audio_hw_read(d,maestro_dock_regs[i],1)!=value) e=-EIO;
    }
    if (!e) e=audio_dma_codec_prepare(d,maestro_codec_read,maestro_codec_write);
    /* Silent calibration must remain muted regardless of the saved setting. */
    if (!e) e=maestro_mute(d,true);
    if (!e) e=maestro_measure_clock(d);
    if (!e) e=maestro_mute(d,audio.master.mute);
    if (e) { if (!d->reason) d->reason="codec_or_clock"; return e; }
    klog("[audio] maestro chip=%04x:%04x subsystem=%04x:%04x clock=%u eapd=%u ready=%x board=%s analog=unqualified",
         d->pci.vendor,d->pci.device,d->pci.sub_vendor,d->pci.sub_device,maestro.clock,
         (d->state.power>>15)&1,d->state.power&15,maestro.e500 ? "e500" : "generic");
    return 0;
}
int maestro_restore(struct audio_dma_device *d)
{
    if (d->state.quarantine) return -EIO;
    if (d->codec_changed) {
        static const unsigned regs[]={2,0x18,0x26,0x2a};
        static const unsigned order[]={3,1,0,2};
        if (maestro.record_saved) for (unsigned i=0;i<2;i++) {
            int e=maestro_codec_write(d,0x1a+i*2,maestro.record[i]); if (e) return e;
        }
        for (unsigned i=0;i<4;i++) {
            unsigned at=order[i]; int e=maestro_codec_write(d,regs[at],d->codec_saved[at]); if (e) return e;
        }
        d->codec_changed=false;
    }
    if (maestro.dock_saved) for (unsigned i=0;i<7;i++) {
        audio_hw_write(d,maestro_dock_regs[i],maestro.dock[i],1);
        if (audio_hw_read(d,maestro_dock_regs[i],1)!=maestro.dock[i]) return -EIO;
    }
    if (maestro.saved) {
        for (unsigned i=0;i<3;i++) audio_hw_write(d,0x34+i*2,maestro.ring[i],2);
        for (unsigned i=0;i<5;i++) {
            int e=maestro_pci_word(d,maestro_config_regs[i],maestro.config[i]); if (e) return e;
        }
    }
    return 0;
}
const struct audio_dma_ops maestro_ops={maestro_prepare,maestro_start,maestro_halt,maestro_cursor,maestro_mute,maestro_ack,maestro_inhibit};

/* The playback-only public ABI stays frozen. These copied kernel calls are
 * generation checked, and never expose guest/user addresses to the engine. */
int maestro_capture_start(struct audio_dma_device *d, unsigned rate, unsigned source, uint32_t *token)
{
    if (!token || rate<4000 || rate>47999 || (source!=0 && source!=4)) return -EINVAL;
    kmutex_lock(&d->register_lock);
    if (maestro.owner!=d || !d->state.ready || d->state.quarantine || d->inhibited || !d->state.running || maestro.capture_running) {
        kmutex_unlock(&d->register_lock); return -EBUSY;
    }
    uint32_t generation=gen_alloc();
    if (!generation) { kmutex_unlock(&d->register_lock); return -ENOMEM; }
    int e=0;
    if (!maestro.record_saved) {
        e=maestro_codec_read(d,0x1a,&maestro.record[0]);
        if (!e) e=maestro_codec_read(d,0x1c,&maestro.record[1]);
        if (!e) maestro.record_saved=true;
    }
    if (!e) e=maestro_codec_write(d,0x1a,source|(source<<8));
    if (!e) e=maestro_codec_write(d,0x1c,0); /* unity capture, monitor stays untouched */
    memset((uint8_t *)d->pcm-MAESTRO_PLAY_OFFSET+MAESTRO_MIX_OFFSET,0,1024);
    memset((uint8_t *)d->pcm-MAESTRO_PLAY_OFFSET+MAESTRO_CAPTURE_OFFSET,0,AUDIO_HW_BYTES);
    uint32_t step=(uint32_t)(((uint64_t)rate<<16)/maestro.clock);
    if (step==0x10000) step--; /* SRC cannot run at the unity encoding */
    for (unsigned voice=2;!e && voice<6;voice++) {
        bool input=voice>=4;
        unsigned offset=input ? MAESTRO_MIX_OFFSET+(voice-4)*512 : MAESTRO_CAPTURE_OFFSET+(voice-2)*MAESTRO_FRAMES*2;
        e=maestro_voice(d,voice,offset,input ? 256 : MAESTRO_FRAMES,input ? 0x14+voice-4 : voice+2,false);
        if (!e) e=maestro_frequency(d,voice,input ? 0x10000 : step);
    }
    uint32_t flags=irq_save();
    for (unsigned voice=2;!e && voice<6;voice++) e=maestro_mode(d,voice,voice>=4 ? 9 : 11);
    if (!e) {
        maestro.capture_rate=(uint32_t)(((uint64_t)step*maestro.clock)>>16);
        maestro.capture_generation=generation; maestro.capture_running=true;
        maestro.capture_produced=maestro.capture_consumed=maestro.capture_frame=0;
        maestro.capture_observation=d->io.now(d->io.cookie); *token=generation;
    }
    irq_restore(flags);
    if (e) { d->inhibited=true; maestro_inhibit(d); }
    kmutex_unlock(&d->register_lock); return e;
}
int maestro_capture_read(struct audio_dma_device *d, uint32_t token, struct audio_frame *output, unsigned count)
{
    if (!output || !count || count>AUDIO_QUANTUM) return -EINVAL;
    kmutex_lock(&d->register_lock);
    int e=0;
    if (maestro.owner!=d || !token || token!=maestro.capture_generation || !maestro.capture_running ||
        d->state.quarantine || d->inhibited) {
        kmutex_unlock(&d->register_lock); return -EIO;
    }
    unsigned left=0,right=0;
    uint64_t now=d->io.now(d->io.cookie);
    if (!e && now-maestro.capture_observation >= MAESTRO_FRAMES*1000/maestro.capture_rate) e=-EIO;
    uint32_t flags=irq_save();
    if (!e) e=maestro_position(d,2,&left);
    if (!e) e=maestro_position(d,3,&right);
    unsigned diff=(left+MAESTRO_FRAMES-right)%MAESTRO_FRAMES;
    if (!e && diff>1 && diff<MAESTRO_FRAMES-1) e=-EAGAIN;
    irq_restore(flags);
    if (!e) {
        left=(diff==1 ? right : left)&~1u;
        maestro.capture_produced+=(left+MAESTRO_FRAMES-maestro.capture_frame)%MAESTRO_FRAMES;
        maestro.capture_frame=left; maestro.capture_observation=now;
        uint64_t available=maestro.capture_produced-maestro.capture_consumed;
        if (available>=MAESTRO_FRAMES-AUDIO_QUANTUM) e=-EIO; /* copy guard against DMA lapping */
        else {
            if (count>available) count=available;
            const int16_t *samples=(const int16_t *)((const uint8_t *)d->pcm-MAESTRO_PLAY_OFFSET+MAESTRO_CAPTURE_OFFSET);
            __asm__ volatile("" ::: "memory");
            for (unsigned i=0;i<count;i++) {
                unsigned at=(unsigned)(maestro.capture_consumed+i)%MAESTRO_FRAMES;
                output[i]=(struct audio_frame){samples[at],samples[at+MAESTRO_FRAMES]};
            }
            /* A preemption during the copy may consume the guard. Never
             * return potentially overwritten samples as valid input. */
            uint64_t copy_ms=d->io.now(d->io.cookie)-now;
            if (copy_ms >= (MAESTRO_FRAMES-(unsigned)available)*1000/maestro.capture_rate) e=-EIO;
            else maestro.capture_consumed+=count;
        }
    }
    if (e && e!=-EAGAIN) { d->inhibited=true; maestro_inhibit(d); }
    kmutex_unlock(&d->register_lock); return e ? e : (int)count;
}
int maestro_capture_stop(struct audio_dma_device *d, uint32_t token)
{
    kmutex_lock(&d->register_lock);
    if (!token || maestro.owner!=d || token!=maestro.capture_generation) {
        kmutex_unlock(&d->register_lock); return -EIO;
    }
    maestro.capture_running=false; maestro.capture_generation=0;
    for (unsigned voice=2;voice<6;voice++) (void)maestro_mode(d,voice,0);
    uint64_t end=audio_hw_deadline(d,500);
    do {
        bool idle=true; uint16_t positions[4];
        for (unsigned voice=2;voice<6;voice++) {
            uint16_t mode=0xffff;
            if (maestro_get(d,voice,0,&mode) || (mode&MAESTRO_MODE) ||
                maestro_get(d,voice,5,&positions[voice-2])) idle=false;
        }
        d->io.sleep(d->io.cookie,2);
        if (idle) for (unsigned voice=2;voice<6;voice++) {
            uint16_t got=0xffff,mode=0xffff;
            if (maestro_get(d,voice,5,&got) || got!=positions[voice-2] ||
                maestro_get(d,voice,0,&mode) || (mode&MAESTRO_MODE)) idle=false;
        }
        if (idle) { kmutex_unlock(&d->register_lock); return 0; }
    } while ((int64_t)(d->io.now(d->io.cookie)-end)<0);
    d->reason="capture_stop_unproved"; d->state.quarantine=true;
    d->inhibited=true; maestro_inhibit(d);
    kmutex_unlock(&d->register_lock); return -EIO;
}
