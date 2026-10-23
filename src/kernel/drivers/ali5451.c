/* SPDX-License-Identifier: GPL-2.0-or-later
 * Adapted from Linux v6.12 sound/pci/ali5451/ali5451.c.
 * Matt Wu <Matt_Wu@acersoftech.com.cn>, Apr 26, 2001.
 * Routines for control of ALi pci audio M5451.
 * Ciuki: bounded PCM/codec transactions, private copied capture and stop proof;
 * no ALSA framework, bridge/PM writes, global reset or legacy passthrough.
 */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/audio_backend.h>

bool ali5451_match(const struct pci_func *p, enum audio_backend_kind *kind)
{
    if (p->vendor != 0x10b9 || p->device != 0x5451 || p->class_code != 4 || p->subclass != 1) return false;
    *kind = AUDIO_ALI5451;
    return true;
}
static int ali5451_timer(struct audio_dma_device *d)
{
    uint32_t first = audio_hw_read(d, 0xc8, 4);
    if (first == UINT32_MAX) return -EIO;
    uint64_t end = audio_hw_deadline(d, 100);
    do {
        uint32_t next = audio_hw_read(d, 0xc8, 4);
        if (next == UINT32_MAX) return -EIO;
        if (next != first) return 0;
        d->io.sleep(d->io.cookie, 1);
    } while ((int64_t)(d->io.now(d->io.cookie) - end) < 0);
    return -ETIMEDOUT;
}
static int ali5451_codec_read(struct audio_dma_device *d, uint16_t reg, uint16_t *value)
{
    if (reg > 0x7e || (reg & 1)) return -EINVAL;
    unsigned port = d->pci.revision == 2 ? 0x40 : 0x44;
    uint64_t saved = audio_voice_codec_begin(d);
    int e = audio_hw_wait(d, port, 0x8000, 0, 100);
    if (!e) e = ali5451_timer(d);
    if (!e) {
        audio_hw_write(d, port, 0x8000 | reg, 4);
        e = ali5451_timer(d);
        if (!e) e = audio_hw_wait(d, port, 0x8000, 0, 100);
    }
    if (!e) {
        uint32_t got = audio_hw_read(d, port, 4);
        if ((got >> 16) == 0xffff || (got & 0x7e) != reg) e = -EIO;
        else *value = got >> 16;
    }
    return audio_voice_codec_end(d, saved, e);
}
static int ali5451_codec_write(struct audio_dma_device *d, uint16_t reg, uint16_t value)
{
    if (reg > 0x7e || (reg & 1)) return -EINVAL;
    uint64_t saved = audio_voice_codec_begin(d);
    int e = audio_hw_wait(d, 0x40, 0x8000, 0, 100);
    if (!e) e = ali5451_timer(d);
    if (!e) {
        audio_hw_write(d, 0x40, (uint32_t)value << 16 | 0x8000 | reg |
                       (d->pci.revision == 2 ? 0x100 : 0), 4);
        e = audio_hw_wait(d, 0x40, 0x8000, 0, 100);
    }
    if (!e) e = audio_voice_codec_verify(d, reg, value);
    return audio_voice_codec_end(d, saved, e);
}
static int ali5451_mute(struct audio_dma_device *d, bool mute)
{ return audio_dma_codec_mute(d, mute, ali5451_codec_write); }
static int ali5451_cursor(struct audio_dma_device *d, unsigned *frame)
{ return audio_voice_cursor(d, 0, frame); }
static void ali5451_inhibit(struct audio_dma_device *d)
{
    uint32_t enabled = audio_hw_read(d, 0xa4, 4);
    audio_hw_write(d, 0xa4, enabled & ~3u, 4);
    audio_hw_write(d, 0x84, 3, 4);
}
static int ali5451_halt(struct audio_dma_device *d)
{ return audio_voice_halt(d, 0x80, 0x84, 0xa4, 0x98, 3, 0); }
static int ali5451_start(struct audio_dma_device *d, bool proof)
{
    if (d->state.quarantine || d->phys > 0x7fffffffu - (AUDIO_HW_BYTES - 1)) return -EIO;
    audio_voice_program(d, 0, d->phys, AUDIO_HW_SLOTS * AUDIO_QUANTUM, false, false);
    audio_voice_program(d, 1, d->phys, 2 * AUDIO_QUANTUM, true, false);
    audio_hw_write(d, 0x98, 3, 4);
    uint32_t enabled = audio_hw_read(d, 0xa4, 4);
    audio_hw_write(d, 0xa4, (enabled & ~3u) | (proof ? 0 : 2), 4);
    __asm__ volatile("" ::: "memory");
    audio_hw_write(d, 0x80, 3, 4);
    uint32_t got = audio_hw_read(d, 0x80, 4);
    return got != UINT32_MAX && (got & 3) == 3 ? 0 : -EIO;
}
static bool ali5451_ack(struct audio_dma_device *d)
{
    uint32_t misc = audio_hw_read(d, 0xb0, 4);
    if (misc == UINT32_MAX || !(misc & 0x20)) return false;
    uint32_t causes = audio_hw_read(d, 0x98, 4);
    if (causes == UINT32_MAX || !(causes & 3)) return false;
    audio_hw_write(d, 0x98, causes & 3, 4);
    return true;
}
static int ali5451_capture_cursor(struct audio_dma_device *d, unsigned *frame)
{ return audio_voice_cursor(d, 31, frame); }
static int ali5451_capture_halt(struct audio_dma_device *d)
{
    int e = audio_voice_halt(d, 0x80, 0x84, 0xa4, 0x98, 1u << 31, 31);
    if (!e) {
        uint32_t v = audio_hw_read(d, 0xd4, 4);
        if (v == UINT32_MAX) return -EIO;
        audio_hw_write(d, 0xd4, v & ~(1u << 31), 4);
    }
    return e;
}
static int ali5451_capture_start(struct audio_dma_device *d)
{
    if (d->state.quarantine || d->capture.phys > 0x7fffffffu - (AUDIO_HW_BYTES - 1)) return -EIO;
    audio_voice_program(d, 31, d->capture.phys, AUDIO_HW_SLOTS * AUDIO_QUANTUM, false, true);
    uint32_t global = audio_hw_read(d, 0xd4, 4);
    if (global == UINT32_MAX) return -EIO;
    audio_hw_write(d, 0xd4, global | 1u << 31, 4);
    __asm__ volatile("" ::: "memory");
    audio_hw_write(d, 0x80, 1u << 31, 4);
    uint32_t got = audio_hw_read(d, 0x80, 4);
    return got != UINT32_MAX && (got & (1u << 31)) ? 0 : -EIO;
}
static const struct audio_capture_ops ali5451_capture_ops = {
    ali5451_capture_start, ali5451_capture_halt, ali5451_capture_cursor
};
static int ali5451_restore(struct audio_dma_device *d)
{
    static const unsigned ports[] = {0xa0, 0xa8, 0xd4};
    for (unsigned i = d->voice_saved_count; i; i--) {
        audio_hw_write(d, ports[i - 1], d->voice_saved[i - 1], 4);
        if (audio_hw_read(d, ports[i - 1], 4) != d->voice_saved[i - 1]) return -EIO;
    }
    return 0;
}
static int ali5451_prepare(struct audio_dma_device *d)
{
    d->voice_codec_read = ali5451_codec_read; d->voice_codec_write = ali5451_codec_write;
    d->capture_ops = &ali5451_capture_ops; d->voice_restore = ali5451_restore;
    uint32_t legacy = audio_pci_read(&d->io, &d->pci, 0x44);
    /* ALSA disables legacy decode with ff00ff00/000800aa. Refuse existing
     * decodes rather than seize firmware MIDI/gameport/PM bridge resources. */
    if (!(d->command & 1) || legacy == UINT32_MAX || (legacy & 0x00ff00ffu) != 0x000800aau) {
        d->reason = "firmware_legacy_unqualified"; return -EBUSY;
    }
    uint32_t link = audio_hw_read(d, 0x48, 4);
    uint32_t global = audio_hw_read(d, 0xd4, 4);
    if (audio_hw_read(d, 0x80, 4) || audio_hw_read(d, 0xa4, 4) ||
        audio_hw_read(d, 0xc4, 1) || (global & 0x00088800u) || (link & ((1u << 25) | 0x20))) {
        d->reason = "firmware_engine_active"; return -EBUSY;
    }
    if (link == UINT32_MAX || !(link & (1u << 24))) {
        d->reason = "codec_link_not_ready"; return -ENODEV;
    }
    static const unsigned ports[] = {0xa0, 0xa8, 0xd4};
    for (unsigned i = 0; i < 3; i++) {
        uint32_t v = audio_hw_read(d, ports[i], 4);
        if (v == UINT32_MAX) return -EIO;
        d->voice_saved[i] = v;
    }
    d->voice_saved_count = 3; d->touched = true;
    audio_pci_write(&d->io, &d->pci, 4, (d->command | 1) & ~4u, 2);
    if ((audio_pci_read(&d->io, &d->pci, 4) & 0xffff) != ((d->command | 1u) & ~4u)) return -EIO;
    audio_hw_write(d, 0xa0, (d->voice_saved[0] & ~0xfc00u) | 0x3000, 4);
    audio_hw_write(d, 0xd4, 1, 4); /* analog PCM; capture enabled only at open */
    audio_hw_write(d, 0xa8, 0, 4);
    int e = audio_dma_codec_prepare(d, ali5451_codec_read, ali5451_codec_write);
    if (e) { d->reason = "codec_prepare"; return e; }
    d->state.rate = AUDIO_RATE;
    audio_pci_write(&d->io, &d->pci, 4, d->command | 5, 2);
    return (audio_pci_read(&d->io, &d->pci, 4) & 0xffff) == (d->command | 5u) ? 0 : -EIO;
}
const struct audio_dma_ops ali5451_ops = {
    ali5451_prepare, ali5451_start, ali5451_halt, ali5451_cursor, ali5451_mute, ali5451_ack, ali5451_inhibit
};
