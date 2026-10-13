/* Firmware-assigned ISA SB16, DSP4 signed stereo auto-init DMA5.
 * SPDX-License-Identifier: GPL-2.0-only; docs/design/f4-audio-second-set.md. */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/audio_backend.h>
static uint8_t sb_mix_read(struct audio_dma_device *d, uint8_t reg)
{
    uint32_t f=irq_save(); audio_hw_write(d,4,reg,1);
    uint8_t v=audio_hw_read(d,5,1); irq_restore(f); return v;
}
static int sb_mix_write(struct audio_dma_device *d, uint8_t reg, uint8_t v)
{
    uint32_t f=irq_save(); audio_hw_write(d,4,reg,1); audio_hw_write(d,5,v,1);
    uint8_t got=audio_hw_read(d,5,1); irq_restore(f); return got==v ? 0 : -EIO;
}
static int sb_send(struct audio_dma_device *d, uint8_t command)
{
    uint64_t end=audio_hw_deadline(d,100);
    do {
        uint8_t v=audio_hw_read(d,0xc,1);
        if (v==0xff) return -EIO;
        if (!(v&0x80)) { audio_hw_write(d,0xc,command,1); return 0; }
        d->io.sleep(d->io.cookie,1);
    } while ((int64_t)(d->io.now(d->io.cookie)-end)<0);
    return -ETIMEDOUT;
}
static int sb_receive(struct audio_dma_device *d, uint8_t *value)
{
    uint64_t end=audio_hw_deadline(d,100);
    do {
        unsigned v=audio_hw_read(d,0xe,1);
        if (v==0xff) return -EIO;
        if (v&0x80) { *value=audio_hw_read(d,0xa,1); return 0; }
        d->io.sleep(d->io.cookie,1);
    } while ((int64_t)(d->io.now(d->io.cookie)-end)<0);
    return -ETIMEDOUT;
}
static int sb_reset(struct audio_dma_device *d)
{
    audio_hw_write(d,6,1,1); d->io.sleep(d->io.cookie,1); audio_hw_write(d,6,0,1);
    uint8_t v; int e=sb_receive(d,&v); return e ? e : v==0xaa ? 0 : -EIO;
}
static void sb_inhibit(struct audio_dma_device *d)
{ d->io.write(d->io.cookie,0xd4,5,1); /* mask only DMA5 */ }
static int sb_count(struct audio_dma_device *d, unsigned *words)
{
    /* Clear the shared flip-flop and take two agreeing full reads, bounded.
     * No status read (would clear every channel's terminal-count bits). */
    uint32_t f=irq_save(); unsigned previous=0x10000;
    for (unsigned i=0;i<8;i++) {
        d->io.write(d->io.cookie,0xd8,0,1);
        unsigned lo=d->io.read(d->io.cookie,0xc6,1), hi=d->io.read(d->io.cookie,0xc6,1);
        unsigned v=lo | hi<<8;
        if (v==previous && v<AUDIO_HW_BYTES/2) { *words=v+1; irq_restore(f); return 0; }
        previous=v;
    }
    irq_restore(f); return -EIO;
}
static int sb_cursor(struct audio_dma_device *d, unsigned *frame)
{
    unsigned words; int e=sb_count(d,&words);
    if (!e) *frame=(AUDIO_HW_BYTES/2-words)/2; /* stereo: two words/frame */
    return e;
}
static int sb_halt(struct audio_dma_device *d)
{
    sb_inhibit(d); /* mask is the DMA safety boundary, independent of DSP */
    int e=sb_send(d,0xd5); /* pause 16-bit */
    if (!e) e=sb_reset(d); /* cancels auto-init and proves DSP response */
    unsigned a,b;
    if (!e) e=sb_count(d,&a);
    if (!e) { d->io.sleep(d->io.cookie,1); e=sb_count(d,&b); if (!e && a!=b) e=-EIO; }
    if (!e) { (void)audio_hw_read(d,0xf,1); d->state.running=false; }
    else d->reason="dma_stop_unproved";
    return e;
}
static int sb_mute(struct audio_dma_device *d, bool mute)
{
    /* CT1745 attenuation at unity, master software gain. Zero is the
     * hardware path's maximum attenuation; DSP speaker-off also silences. */
    int e=sb_send(d,mute ? 0xd3 : 0xd1);
    for (unsigned i=0; !e && i<4; i++) e=sb_mix_write(d,0x30+i,mute ? 0 : 0xf8);
    if (!e) { d->state.master=mute ? 0x8000 : 0; d->state.pcm_volume=mute ? 0x8000 : 0; }
    return e;
}
int sb16_restore(struct audio_dma_device *d)
{
    int e=0;
    for (unsigned i=0; !e && i<6; i++) e=sb_mix_write(d,0x30+i,d->saved_mixer[i]);
    d->codec_changed=e!=0; return e;
}
static int sb_prepare(struct audio_dma_device *d)
{
    if (!d->phys || d->phys>=0x1000000 || (d->phys & 0x7fff) ||
        d->phys>0x1000000u-AUDIO_HW_BYTES || (d->phys & 0x1ffff)+AUDIO_HW_BYTES>0x20000) return -EINVAL;
    if (sb_mix_read(d,0x80)!=2 || sb_mix_read(d,0x81)!=0x22 || (sb_mix_read(d,0x82)&7)) {
        d->reason="isa_resources_unqualified"; return -ENODEV;
    }
    d->touched=true; sb_inhibit(d);
    int e=sb_reset(d);
    if (!e) e=sb_send(d,0xe1);
    if (!e) e=sb_receive(d,&d->dsp_major);
    if (!e) e=sb_receive(d,&d->dsp_minor);
    if (!e && d->dsp_major!=4) e=-ENODEV;
    if (e) { d->reason="dsp_identity"; return e; }
    for (unsigned i=0;i<6;i++) d->saved_mixer[i]=sb_mix_read(d,0x30+i);
    d->codec_changed=true;
    d->state.codec_id=(uint32_t)d->dsp_major<<8 | d->dsp_minor;
    d->state.rate=44100;
    e=sb_mix_write(d,0x34,0); if (!e) e=sb_mix_write(d,0x35,0); /* no physical MIDI */
    if (!e) e=sb_mute(d,audio.master.mute);
    /* Establish a readable DMA count for rollback even before the proof. */
    uint32_t f=irq_save(); d->io.write(d->io.cookie,0xd8,0,1);
    d->io.write(d->io.cookie,0xc6,0xff,1); d->io.write(d->io.cookie,0xc6,0x3f,1); irq_restore(f);
    return e;
}
static int sb_start(struct audio_dma_device *d, bool proof)
{
    (void)proof; sb_inhibit(d);
    uint32_t flags=irq_save();
    d->io.write(d->io.cookie,0xd8,0,1);
    d->io.write(d->io.cookie,0xd6,0x59,1); /* single, auto-init, memory->I/O, channel1 (DMA5) */
    d->io.write(d->io.cookie,0x8b,(d->phys>>16)&0xfe,1);
    d->io.write(d->io.cookie,0xc4,(d->phys>>1)&255,1);
    d->io.write(d->io.cookie,0xc4,(d->phys>>9)&255,1);
    unsigned count=AUDIO_HW_BYTES/2-1;
    d->io.write(d->io.cookie,0xc6,count&255,1); d->io.write(d->io.cookie,0xc6,count>>8,1);
    irq_restore(flags);
    int e=sb_mute(d,audio.master.mute); if (!e) e=sb_send(d,0x41); if (!e) e=sb_send(d,d->state.rate>>8); if (!e) e=sb_send(d,d->state.rate&255);
    /* DSP block counts words too: 256 stereo frames == 512 words. */
    if (!e) e=sb_send(d,0xb6); if (!e) e=sb_send(d,0x30);
    if (!e) e=sb_send(d,0xff); if (!e) e=sb_send(d,1);
    if (!e) d->io.write(d->io.cookie,0xd4,1,1);
    return e;
}
static bool sb_ack(struct audio_dma_device *d)
{
    if (!(sb_mix_read(d,0x82)&2)) return false;
    (void)audio_hw_read(d,0xf,1); return true;
}
const struct audio_dma_ops sb16_ops={sb_prepare,sb_start,sb_halt,sb_cursor,sb_mute,sb_ack,sb_inhibit};
