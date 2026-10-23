/* SPDX-License-Identifier: GPL-2.0-or-later
 * Adapted from Linux v6.12 sound/pci/trident/trident_main.c and trident.h.
 * Maintained by Jaroslav Kysela <perex@perex.cz>
 * Originated by audio@tridentmicro.com, Fri Feb 19 15:55:28 MST 1999.
 * Ciuki: separate DX/NX PCM, bounded codec IO, no reset of firmware peers,
 * direct-address DMA, capture and stop proof. See f1-120-audio-engines.md.
 */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/audio_backend.h>

bool trident_match(const struct pci_func *p, enum audio_backend_kind *kind)
{
    if (p->vendor != 0x1023 || p->class_code != 4 || p->subclass != 1) return false;
    if (p->device == 0x2000) *kind = AUDIO_TRIDENT_DX;
    else if (p->device == 0x2001) *kind = AUDIO_TRIDENT_NX;
    else return false;
    return true;
}
static int trident_codec_read(struct audio_dma_device *d, uint16_t reg, uint16_t *value)
{
    if (reg > 0x7e || (reg & 1)) return -EINVAL;
    bool nx = d->kind == AUDIO_TRIDENT_NX;
    unsigned port = nx ? 0x48 : 0x44, busy = nx ? 0xc00 : 0x8000;
    uint64_t saved = audio_voice_codec_begin(d);
    int e = audio_hw_wait(d, port, busy, 0, 100);
    if (!e) {
        audio_hw_write(d, port, reg | (nx ? 0x800 : 0x8000), 4);
        e = audio_hw_wait(d, port, busy, 0, 100);
    }
    if (!e) {
        uint32_t got = audio_hw_read(d, port, 4);
        if ((got >> 16) == 0xffff || (got & 0x7e) != reg) e = -EIO;
        else *value = got >> 16;
    }
    return audio_voice_codec_end(d, saved, e);
}
static int trident_codec_write(struct audio_dma_device *d, uint16_t reg, uint16_t value)
{
    if (reg > 0x7e || (reg & 1)) return -EINVAL;
    bool nx = d->kind == AUDIO_TRIDENT_NX;
    unsigned port = nx ? 0x44 : 0x40, busy = nx ? 0x800 : 0x8000;
    uint64_t saved = audio_voice_codec_begin(d);
    int e = audio_hw_wait(d, port, busy, 0, 100);
    if (!e) {
        audio_hw_write(d, port, (uint32_t)value << 16 | busy | reg, 4);
        e = audio_hw_wait(d, port, busy, 0, 100);
    }
    if (!e) e = audio_voice_codec_verify(d, reg, value);
    return audio_voice_codec_end(d, saved, e);
}
static int trident_mute(struct audio_dma_device *d, bool mute)
{ return audio_dma_codec_mute(d, mute, trident_codec_write); }
static int trident_cursor(struct audio_dma_device *d, unsigned *frame)
{ return audio_voice_cursor(d, 32, frame); }
static void trident_inhibit(struct audio_dma_device *d)
{
    audio_hw_write(d, 0xdc, 0, 4);
    audio_hw_write(d, 0xb8, 3, 4);
}
static int trident_halt(struct audio_dma_device *d)
{ return audio_voice_halt(d, 0xb4, 0xb8, 0xdc, 0xd8, 3, 32); }
static int trident_start(struct audio_dma_device *d, bool proof)
{
    if (d->state.quarantine || d->phys > 0x3fffffffu - (AUDIO_HW_BYTES - 1)) return -EIO;
    audio_voice_program(d, 32, d->phys, AUDIO_HW_SLOTS * AUDIO_QUANTUM, false, false);
    audio_voice_program(d, 33, d->phys, 2 * AUDIO_QUANTUM, true, false);
    audio_hw_write(d, 0xd8, 3, 4);
    audio_hw_write(d, 0xdc, proof ? 0 : 2, 4);
    __asm__ volatile("" ::: "memory");
    audio_hw_write(d, 0xb4, 3, 4);
    uint32_t got = audio_hw_read(d, 0xb4, 4);
    return got != UINT32_MAX && (got & 3) == 3 ? 0 : -EIO;
}
static bool trident_ack(struct audio_dma_device *d)
{
    uint32_t misc = audio_hw_read(d, 0xb0, 4);
    if (misc == UINT32_MAX || !(misc & 0x20)) return false;
    uint32_t causes = audio_hw_read(d, 0xd8, 4);
    if (causes == UINT32_MAX || !(causes & 3)) return false;
    audio_hw_write(d, 0xd8, causes & 3, 4);
    return true;
}
static int trident_capture_cursor(struct audio_dma_device *d, unsigned *frame)
{
    uint32_t count = audio_hw_read(d, 0xc0, 2);
    if (count == 0xffff || count >= AUDIO_HW_BYTES / 2) return -EIO;
    /* SBBL/SBCL counts remaining 16-bit samples minus one, not frames. */
    *frame = (AUDIO_HW_BYTES / 2 - 1 - count) / 2;
    return 0;
}
static int trident_capture_halt(struct audio_dma_device *d)
{
    audio_hw_write(d, 0xc4, 0, 1);
    uint64_t end = audio_hw_deadline(d, 500);
    do {
        uint32_t ctrl = audio_hw_read(d, 0xc4, 1), a = audio_hw_read(d, 0xc0, 2);
        uint32_t timer = audio_hw_read(d, 0xc8, 4);
        d->io.sleep(d->io.cookie, 2);
        uint32_t after = audio_hw_read(d, 0xc8, 4);
        if (!ctrl && !audio_hw_read(d, 0xc4, 1) && a != 0xffff &&
            a == audio_hw_read(d, 0xc0, 2) && timer != UINT32_MAX && after != UINT32_MAX && timer != after)
            return 0;
        d->io.sleep(d->io.cookie, 1);
    } while ((int64_t)(d->io.now(d->io.cookie) - end) < 0);
    return -EIO;
}
static int trident_capture_start(struct audio_dma_device *d)
{
    if (d->state.quarantine || d->capture.phys > 0x3fffffffu - (AUDIO_HW_BYTES - 1)) return -EIO;
    audio_hw_write(d, 0xf, 0, 1);
    audio_hw_write(d, 0xb, 0x54, 1);
    audio_hw_write(d, 0, d->capture.phys, 4);
    audio_hw_write(d, 6, (AUDIO_HW_BYTES - 1) >> 16, 1);
    audio_hw_write(d, 4, (AUDIO_HW_BYTES - 1) & 0xffff, 2);
    audio_hw_write(d, 0xac, 0x1000, 2);
    audio_hw_write(d, 0xc0, 0x3fff3fff, 4);
    __asm__ volatile("" ::: "memory");
    audio_hw_write(d, 0xc4, 0xf9, 1); /* S16 stereo, cyclic capture */
    return audio_hw_read(d, 0xc4, 1) == 0xf9 ? 0 : -EIO;
}
static const struct audio_capture_ops trident_capture_ops = {
    trident_capture_start, trident_capture_halt, trident_capture_cursor
};
static int trident_restore(struct audio_dma_device *d)
{
    static const unsigned ports[] = {0xa0, 0xa8, 0x48, 0x6c};
    for (unsigned i = d->voice_saved_count; i; i--) {
        unsigned at = i - 1, port = ports[at];
        if (at == 2 && d->kind == AUDIO_TRIDENT_NX) port = 0x40;
        audio_hw_write(d, port, d->voice_saved[at], 4);
        if (audio_hw_read(d, port, 4) != d->voice_saved[at]) return -EIO;
    }
    return 0;
}
static int trident_prepare(struct audio_dma_device *d)
{
    bool nx = d->kind == AUDIO_TRIDENT_NX;
    d->voice_codec_read = trident_codec_read; d->voice_codec_write = trident_codec_write;
    d->capture_ops = &trident_capture_ops; d->voice_restore = trident_restore;
    unsigned ctrl = nx ? 0x40 : 0x48;
    /* Claim does not authorize resetting firmware, modem, MIDI or joystick. */
    uint32_t cfg = audio_pci_read(&d->io, &d->pci, 0x40);
    uint32_t legacy = audio_pci_read(&d->io, &d->pci, 0x44);
    if (!(d->command & 1) || cfg == UINT32_MAX || legacy == UINT32_MAX || cfg || (legacy & 0xffff)) {
        d->reason = "firmware_legacy_active"; return -EBUSY;
    }
    uint32_t link = audio_hw_read(d, ctrl, 4);
    if (audio_hw_read(d, 0x80, 4) || audio_hw_read(d, 0xb4, 4) || audio_hw_read(d, 0xc4, 1) ||
        audio_hw_read(d, 0x9c, 4) || audio_hw_read(d, 0xdc, 4) ||
        (nx && ((link & 0x70) || (audio_hw_read(d, 0x27, 1) & 0x28)))) {
        d->reason = "firmware_engine_active"; return -EBUSY;
    }
    if (link == UINT32_MAX || !(link & (nx ? 8 : 0x10))) {
        d->reason = "codec_link_not_ready"; return -ENODEV;
    }
    static const unsigned ports[] = {0xa0, 0xa8, 0x48, 0x6c};
    unsigned count = nx ? 4 : 3;
    for (unsigned i = 0; i < count; i++) {
        unsigned port = i == 2 ? ctrl : ports[i];
        uint32_t v = audio_hw_read(d, port, 4);
        if (v == UINT32_MAX) return -EIO;
        d->voice_saved[i] = v;
    }
    d->voice_saved_count = count;
    d->touched = true;
    audio_pci_write(&d->io, &d->pci, 4, (d->command | 1) & ~4u, 2);
    if ((audio_pci_read(&d->io, &d->pci, 4) & 0xffff) != ((d->command | 1u) & ~4u)) return -EIO;
    if (nx) {
        audio_hw_write(d, 0x6c, 0, 4); /* no hardware TLB */
        audio_hw_write(d, 0xb0, 0x1000, 4); /* no physical SB IRQ */
    }
    audio_hw_write(d, ctrl, nx ? 6 : 0x4a, 4); /* PCM out and ADC slots; no reset */
    audio_hw_write(d, 0xa8, 0, 4);
    audio_hw_write(d, 0xa0, (d->voice_saved[0] & ~0xfc00u) | 0x3000, 4);
    int e = audio_dma_codec_prepare(d, trident_codec_read, trident_codec_write);
    if (e) { d->reason = "codec_prepare"; return e; }
    d->state.rate = AUDIO_RATE;
    audio_pci_write(&d->io, &d->pci, 4, d->command | 5, 2);
    return (audio_pci_read(&d->io, &d->pci, 4) & 0xffff) == (d->command | 5u) ? 0 : -EIO;
}
const struct audio_dma_ops trident_ops = {
    trident_prepare, trident_start, trident_halt, trident_cursor, trident_mute, trident_ack, trident_inhibit
};
