/* Intel ICH PCM-out. Sources/decisions: docs/design/f4-acceptance.md.
 * No upstream code imported. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/mm.h>
#include <ciuki/task.h>
#include <ciuki/ac97.h>
#include <ciuki/timing.h>

struct ac97_device ac97;
static uint32_t read_io(void *cookie, uint16_t port, unsigned width)
{
    (void)cookie;
    if (width == 1) return inb(port);
    if (width == 4) return inl(port);
    uint16_t v; __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(port)); return v;
}
static void write_io(void *cookie, uint16_t port, uint32_t v, unsigned width)
{
    (void)cookie;
    if (width == 1) outb(port, (uint8_t)v);
    else if (width == 4) outl(port, v);
    else __asm__ volatile("outw %0,%1" :: "a"((uint16_t)v), "Nd"(port));
}
static uint64_t now_io(void *cookie) { (void)cookie; return g_ticks; }
static void sleep_io(void *cookie, uint32_t ms) { (void)cookie; task_sleep_ms(ms); }
static uint32_t rd(struct ac97_device *d, unsigned off, unsigned width)
{ return d->io.read(d->io.cookie, d->nabm + off, width); }
static void wr(struct ac97_device *d, unsigned off, uint32_t v, unsigned width)
{ d->io.write(d->io.cookie, d->nabm + off, v, width); }
static bool expired(struct ac97_device *d, uint64_t deadline)
{ return (int64_t)(d->io.now(d->io.cookie) - deadline) >= 0; }
static uint64_t operation_deadline(struct ac97_device *d)
{
    uint64_t deadline = d->io.now(d->io.cookie) + 500;
    return d->activation_deadline && (int64_t)(deadline - d->activation_deadline) > 0 ? d->activation_deadline : deadline;
}
static uint64_t codec_deadline(struct ac97_device *d, uint64_t limit)
{
    uint64_t deadline = d->io.now(d->io.cookie) + 100;
    return (int64_t)(deadline - limit) > 0 ? limit : deadline;
}
int ac97_controller_reset(struct ac97_device *d)
{
    uint64_t limit = operation_deadline(d);
    /* Caller proved idle audio engines and excluded a modem dependency.
     * Cold Reset# is active low. 1 ms exceeds the specified 1 us pulse. */
    wr(d, 0x2c, 0, 4);
    if (rd(d, 0x2c, 4) & 2) return -EIO;
    d->io.sleep(d->io.cookie, 1);
    if (expired(d, limit)) return -ETIMEDOUT;
    wr(d, 0x2c, 2, 4); /* stereo, link on; no resume/GPI IRQs */
    if (rd(d, 0x2c, 4) != 2) return -EIO;
    do {
        d->global_status = rd(d, 0x30, 4);
        if (d->global_status == UINT32_MAX) return -EIO;
        if (d->global_status & 0x100) {
            wr(d, 0x30, 1u << 15, 4); /* stale read timeout, isolated W1C */
            return 0;
        }
        d->io.sleep(d->io.cookie, 1);
    } while (!expired(d, limit));
    return -ETIMEDOUT;
}
static int cas(struct ac97_device *d, uint64_t deadline)
{
    while (!expired(d, deadline)) {
        uint8_t v = (uint8_t)rd(d, 0x34, 1);
        if (v == 0xff) return -EIO;
        if (!(v & 1)) return 0;
        d->io.sleep(d->io.cookie, 1);
    }
    return -ETIMEDOUT;
}
int ac97_codec_read(struct ac97_device *d, uint16_t reg, uint16_t *out, uint64_t limit)
{
    uint64_t deadline = codec_deadline(d, limit);
    int err = cas(d, deadline);
    if (err) return err;
    *out = (uint16_t)d->io.read(d->io.cookie, d->nam + reg, 2);
    if (*out == 0xffff || (rd(d, 0x30, 4) & (1u << 15))) return -EIO;
    if (expired(d, deadline)) return -ETIMEDOUT;
    /* Diagnostics retain the last successful hardware read, including
     * mismatches and restoration; never report a desired write as readback. */
    if (reg == 0x26) d->power = *out;
    else if (reg == 0x02) d->master = *out;
    else if (reg == 0x18) d->pcm_volume = *out;
    else if (reg == 0x2c) d->rate = *out;
    return 0;
}
int ac97_codec_write(struct ac97_device *d, uint16_t reg, uint16_t value, uint64_t limit)
{
    uint64_t deadline = codec_deadline(d, limit);
    int err = cas(d, deadline);
    if (err) return err;
    d->io.write(d->io.cookie, d->nam + reg, value, 2);
    uint16_t got;
    err = ac97_codec_read(d, reg, &got, deadline);
    return err ? err : got != value ? -EIO : 0;
}
/* Read-only readiness bits must not participate in power/extended-control
 * write verification. Volume controls get one bounded retry for sticky mute. */
static int codec_update(struct ac97_device *d, uint16_t reg, uint16_t value,
                        uint16_t mask, bool retry, uint64_t limit)
{
    uint64_t deadline = codec_deadline(d, limit);
    for (unsigned attempt = 0; attempt < (retry ? 2u : 1u); attempt++) {
        int err = cas(d, deadline);
        if (err) return err;
        d->io.write(d->io.cookie, d->nam + reg, value, 2);
        uint16_t got;
        err = ac97_codec_read(d, reg, &got, deadline);
        if (err) return err;
        if (!((got ^ value) & mask)) return 0;
        if (!attempt && retry) d->io.sleep(d->io.cookie, 1);
    }
    return -EIO;
}
static unsigned attenuation(unsigned percent)
{
    /* Rounded 10000 * 10^(-1.5*n/20), the mandatory 5-bit AC'97 scale. */
    static const uint16_t gain[32] = {10000,8414,7079,5957,5012,4217,3548,2985,
        2512,2113,1778,1496,1259,1059,891,750,631,531,447,376,316,266,224,
        188,158,133,112,94,79,67,56,47};
    unsigned target = percent * 100, best = 0, distance = UINT32_MAX;
    for (unsigned n = 0; n < 32; n++) {
        unsigned diff = gain[n] > target ? gain[n] - target : target - gain[n];
        if (diff < distance) { best = n; distance = diff; }
    }
    return best;
}
static int master_until(struct ac97_device *d, const struct ciuki_mixer_master *m, uint64_t limit)
{
    uint16_t mute = (m->mute || (!m->left && !m->right)) ? 0x8000 : 0;
    uint16_t volume = mute;
    if (!d->software_master) volume |= attenuation(m->left) << 8 | attenuation(m->right);
    int err = codec_update(d, 0x18, d->pcm_unity | mute, 0x9f1f, true, limit);
    if (!err) err = codec_update(d, 0x02, volume, 0x9f1f, true, limit);
    if (!err && (d->codec_caps & 0x10)) err = codec_update(d, 0x04, volume, 0x9f1f, true, limit);
    return err;
}
int ac97_set_master(struct ac97_device *d, const struct ciuki_mixer_master *m)
{
    if (m->left > 100 || m->right > 100 || m->mute > 1) return -EINVAL;
    return master_until(d, m, operation_deadline(d));
}
int ac97_set_mute(struct ac97_device *d, bool mute)
{
    struct ciuki_mixer_master m = audio.master; m.mute = mute;
    return ac97_set_master(d, &m);
}
int ac97_codec_restore(struct ac97_device *d)
{
    if (!d->codec_changed) return 0;
    uint64_t limit = operation_deadline(d);
    int err = codec_update(d, 0x18, d->saved_pcm, 0x9f1f, true, limit);
    if (!err) err = codec_update(d, 0x02, d->saved_master, 0x9f1f, true, limit);
    if (!err && (d->codec_caps & 0x10)) err = codec_update(d, 0x04, d->saved_headphone, 0x9f1f, true, limit);
    if (!err && d->beep_present) err = codec_update(d, 0x0a, d->saved_beep, 0x801e, true, limit);
    if (!err && d->cirrus_spdif) err = ac97_codec_write(d, 0x68, d->saved_spdif, limit);
    if (!err) err = codec_update(d, 0x2a, d->saved_ext, 0x000f, false, limit);
    if (!err && (d->saved_ext & 1)) err = ac97_codec_write(d, 0x2c, d->saved_rate, limit);
    if (!err) err = codec_update(d, 0x26, d->saved_power & 0xff00, 0xff00, false, limit);
    if (!err) d->codec_changed = false;
    return err;
}
int ac97_codec_prepare(struct ac97_device *d)
{
    uint64_t limit = operation_deadline(d);
    while (!((d->global_status = rd(d, 0x30, 4)) & 0x100)) {
        if (expired(d, limit)) return -ETIMEDOUT;
        d->io.sleep(d->io.cookie, 1);
    }
    if (d->global_status == UINT32_MAX) return -EIO;
    uint16_t id1, id2, ext;
    int err = ac97_codec_read(d, 0x7c, &id1, limit);
    if (!err) err = ac97_codec_read(d, 0x7e, &id2, limit);
    if (!err && (!id1 || !id2)) err = -EIO;
    if (err) return err;
    d->codec_id = (uint32_t)id1 << 16 | id2;
    err = ac97_codec_read(d, 0, &d->codec_caps, limit);
    if (!err) err = ac97_codec_read(d, 0x26, &d->power, limit);
    if (err) return err;
    d->saved_power = d->power;
    /* Power control has RO ready bits; EAPD=0 enables the external amp.
     * No guessed vendor amplifier polarity/registers. */
    d->codec_changed = true;
    err = codec_update(d, 0x26, 0, 0xff00, false, limit);
    while (!err) {
        err = ac97_codec_read(d, 0x26, &d->power, limit);
        if (err || (d->power & 0xf) == 0xf) break;
        if (expired(d, limit)) { err = -ETIMEDOUT; break; }
        d->io.sleep(d->io.cookie, 1);
    }
    /* Mixer snapshots are meaningful only once analog sections are ready.
     * A power failure cannot safely restore an incomplete mixer snapshot. */
    if (err) { d->quarantine = true; return err; }
    if (!err) err = ac97_codec_read(d, 0x28, &ext, limit);
    if (!err) err = ac97_codec_read(d, 0x02, &d->saved_master, limit);
    if (!err) err = ac97_codec_read(d, 0x18, &d->saved_pcm, limit);
    if (!err && (d->codec_caps & 0x10)) err = ac97_codec_read(d, 0x04, &d->saved_headphone, limit);
    if (!err) err = ac97_codec_read(d, 0x2a, &d->saved_ext, limit);
    if (!err) err = ac97_codec_read(d, 0x2c, &d->saved_rate, limit);
    /* PC beep is optional; ffff/absent is not a playback failure. */
    if (!err) {
        int beep = ac97_codec_read(d, 0x0a, &d->saved_beep, limit);
        if (beep && (beep != -EIO || d->saved_beep != 0xffff)) err = beep;
        else if (beep && (rd(d, 0x30, 4) & (1u << 15))) wr(d, 0x30, 1u << 15, 4);
        else d->beep_present = !beep && d->saved_beep != 0;
    }
    d->cirrus_spdif = (d->codec_id & 0xfffffff8u) == 0x43525930;
    if (!err && d->cirrus_spdif) err = ac97_codec_read(d, 0x68, &d->saved_spdif, limit);
    if (err) { d->quarantine = true; return err; }
    uint16_t control = d->saved_ext & ~0x000fu; /* VRA/DRA/S/PDIF/VRM */
    if (ext & 1) control |= 1;
    err = codec_update(d, 0x2a, control, 0x000f, false, limit);
    if (!err && (ext & 1)) err = ac97_codec_write(d, 0x2c, AUDIO_RATE, limit);
    if (!err) err = ac97_codec_read(d, 0x2c, &d->rate, limit);
    if (!err && d->rate != AUDIO_RATE) err = -EIO;
    if (!err && d->cirrus_spdif) err = ac97_codec_write(d, 0x68, d->saved_spdif & ~0x8000u, limit);
    if (!err && d->beep_present) err = codec_update(d, 0x0a, 0, 0x801e, true, limit);
    if (!err) err = master_until(d, &audio.master, limit);
    if (err && ac97_codec_restore(d)) d->quarantine = true;
    return err;
}
int ac97_observe(struct ac97_device *d, struct ac97_cursor *out)
{
    for (unsigned attempt = 0; attempt < 8; attempt++) {
        uint8_t civ = rd(d, 0x14, 1);
        struct ac97_cursor c = { .civ = civ, .status = rd(d, 0x16, 2),
            .remaining = rd(d, 0x18, 2), .piv = rd(d, 0x1a, 1) };
        if (civ == rd(d, 0x14, 1) && c.piv == rd(d, 0x1a, 1)) {
            if (c.civ >= 32 || c.piv >= 32 || c.remaining > 512 || c.status == 0xffff) return -EIO;
            d->cursor = c; *out = c; return 0;
        }
    }
    return -EIO;
}
int ac97_progress(struct ac97_device *d)
{
    d->tail_drained = false;
    if (!d->running) return 0;
    struct ac97_cursor c;
    int err = ac97_observe(d, &c);
    if (err) return err;
    unsigned delta = (c.civ - (unsigned)(d->retired % 32)) & 31;
    uint64_t outstanding = d->published - d->retired;
    if (delta >= outstanding || c.piv != ((c.civ + 1) & 31)) return -EIO;
    if (delta) { d->retired += delta; d->last_progress = d->io.now(d->io.cookie); }
    uint64_t fetched = d->retired * AUDIO_QUANTUM + (512 - c.remaining) / 2;
    if (fetched < d->generation_fetched) return -EIO;
    d->fetched += fetched - d->generation_fetched;
    d->generation_fetched = fetched;
    if (c.status & AC97_FIFOE) { d->fifo_errors++; wr(d, 0x16, AC97_FIFOE, 2); }
    if (c.status & AC97_DCH) {
        d->tail_drained = !(c.status & AC97_FIFOE) && !c.remaining && d->retired + 1 == d->published;
        if (!d->observed_halt && !d->idle_tail) d->dma_halts++; /* an idle-tail underrun carries no client data */
        d->observed_halt = true;
        return -EIO; /* no replay/restart of potentially committed PCM */
    }
    d->observed_halt = false;
    if (d->io.now(d->io.cookie) - d->last_progress > 100) {
        d->deadline_misses++; return -EIO;
    }
    return 0;
}
int ac97_publish(struct ac97_device *d, const struct audio_frame *pcm)
{
    if (d->quarantine || d->stopping || d->published - d->retired >= 31) return -EAGAIN; /* ring full; the service owns the lead policy */
    unsigned slot = d->published % 32;
    if (d->running) {
        struct ac97_cursor c;
        if (ac97_observe(d, &c)) return -EIO;
        if (slot == c.civ || slot == c.piv) return -EIO;
    }
    memcpy(d->pcm[slot], pcm, AUDIO_QUANTUM * sizeof(*pcm));
    d->bdl[slot].length = (1u << 31) | 512;
    /* x86 coherent PCI DMA: compiler release barrier orders payload and BDL
     * stores before the strongly ordered I/O publication. */
    __asm__ volatile("" ::: "memory");
    wr(d, 0x15, slot, 1);
    d->published++;
    return 0;
}
int ac97_start(struct ac97_device *d)
{
    if (!d->ready || d->quarantine || d->published - d->retired != AUDIO_LEAD) return -EIO;
    wr(d, 0x1b, 0x1d, 1);
    d->running = true; d->last_progress = d->io.now(d->io.cookie);
    return 0;
}
int ac97_pause(struct ac97_device *d)
{
    uint64_t limit = operation_deadline(d);
    d->stopping = true;
    wr(d, 0x1b, 0, 1);
    while (!expired(d, limit)) {
        if (!(rd(d, 0x1b, 1) & 1) && (rd(d, 0x16, 2) & AC97_DCH)) {
            struct ac97_cursor a, b;
            if (!ac97_observe(d, &a) && !ac97_observe(d, &b) &&
                a.civ == b.civ && a.piv == b.piv && a.remaining == b.remaining) {
                d->running = false; return 0;
            }
        }
        d->io.sleep(d->io.cookie, 1);
    }
    d->quarantine = true; return -EIO;
}
int ac97_stop(struct ac97_device *d)
{
    uint64_t limit = operation_deadline(d);
    int err = ac97_pause(d);
    if (err) return err;
    wr(d, 0x1b, 2, 1); /* RUN proved clear */
    while (rd(d, 0x1b, 1) & 2) {
        if (expired(d, limit)) { d->quarantine = true; return -EIO; }
        d->io.sleep(d->io.cookie, 1);
    }
    wr(d, 0x16, 0x1c, 2);
    wr(d, 0x10, d->bdl_phys, 4);
    d->published = d->retired = d->generation_fetched = 0; d->stopping = false;
    return 0;
}
static uint32_t config_read(const struct pci_func *p, unsigned off)
{
    uint32_t f = irq_save();
    ac97.io.write(ac97.io.cookie, 0xcf8, 0x80000000 | (uint32_t)p->bus << 16 | (uint32_t)p->dev << 11 | (uint32_t)p->fn << 8 | (off & 0xfc), 4);
    uint32_t v = ac97.io.read(ac97.io.cookie, 0xcfc, 4); irq_restore(f); return v;
}
static void config_command(const struct pci_func *p, uint16_t value)
{
    uint32_t f = irq_save();
    ac97.io.write(ac97.io.cookie, 0xcf8, 0x80000000 | (uint32_t)p->bus << 16 | (uint32_t)p->dev << 11 | (uint32_t)p->fn << 8 | 4, 4);
    /* Word write never echoes PCI Status W1C bits. */
    ac97.io.write(ac97.io.cookie, 0xcfc, value, 2);
    irq_restore(f);
}
static bool supported(const struct pci_func *p)
{
    return p->vendor == 0x8086 && p->class_code == 4 && p->subclass == 1 &&
        (p->device == 0x2415 || p->device == 0x2425 || p->device == 0x2445 || p->device == 0x2485 || p->device == 0x24c5);
}
static bool controller_layout(struct ac97_device *d, uint32_t boot_flags)
{
    const struct pci_func *p=&d->pci;
    bool twin=(boot_flags & CBI_F_SMBIOS_QEMU) && p->device==0x2415;
    d->nam_bytes=twin?1024:256;d->nabm_bytes=twin?256:64;
    d->pcm_unity=twin?0:0x0808;
    d->software_master=twin;
    uint32_t nam=p->bar[0]&~(uint32_t)(d->nam_bytes-1);
    uint32_t nabm=p->bar[1]&~(uint32_t)(d->nabm_bytes-1);
    if ((p->header_type & 0x7f) || !(p->bar[0]&1) || !(p->bar[1]&1) ||
        (p->bar[0] & (d->nam_bytes-2u)) || (p->bar[1] & (d->nabm_bytes-2u)) ||
        !nam || !nabm || nam>0x10000u-d->nam_bytes || nabm>0x10000u-d->nabm_bytes) return false;
    d->nam=nam;d->nabm=nabm;return true;
}
static bool routed(const struct pci_func *p)
{
    if (p->bus || !p->irq_pin || p->irq_pin > 4 ||
        (p->irq_line != 3 && p->irq_line != 4 && p->irq_line != 5 && p->irq_line != 7 &&
         p->irq_line != 9 && p->irq_line != 10 && p->irq_line != 11 && p->irq_line != 12 && p->irq_line != 14 && p->irq_line != 15)) return false;
    unsigned pirq = 1; /* integrated ICH audio PIRQB */
    for (unsigned i = 0; i < pci_count(); i++) {
        const struct pci_func *bridge = pci_get(i);
        if (bridge->vendor != 0x8086 || bridge->bus || bridge->class_code != 6 || bridge->subclass != 1) continue;
        bool piix = bridge->device == 0x7000 || bridge->device == 0x7110;
        bool ich = bridge->device == 0x2410 || bridge->device == 0x2420 || bridge->device == 0x2440 ||
                   bridge->device == 0x244c || bridge->device == 0x2480 || bridge->device == 0x248c || bridge->device == 0x24c0 || bridge->device == 0x24cc;
        if (piix && (g_boot.flags & CBI_F_SMBIOS_QEMU)) pirq = (p->dev + p->irq_pin - 2) & 3;
        else if (!ich || p->dev != 31 || p->fn != 5 || p->irq_pin != 2) continue;
        unsigned route = (config_read(bridge, 0x60) >> (pirq * 8)) & 0xff;
        unsigned elcr = inb(0x4d0 + p->irq_line / 8);
        return !(route & 0x80) && (route & 15) == p->irq_line && (elcr & (1u << (p->irq_line % 8)));
    }
    return false;
}
static bool modem_idle(struct ac97_device *d, const struct pci_func *peer)
{
    /* A PCI modem function is not proof of an active AC-link client. The
     * Intel peer shares GLOB_CNT; cold reset is safe only with no running
     * modem channel. These checks are read-only, before hardware takeover. */
    if (peer->vendor != 0x8086 || peer->device != d->pci.device + 1u ||
        peer->subclass != 3 || (peer->header_type & 0x7f)) return false;
    if (!(peer->command & 5)) return true; /* decode and DMA both disabled */
    uint32_t base = peer->bar[1] & ~0x7fu;
    if (!(peer->command & 1) || !(peer->bar[1] & 1) || (peer->bar[1] & 0x7e) ||
        !base || base > 0xff80) return false;
    for (unsigned pass = 0; pass < 2; pass++) for (unsigned off = 0; off <= 0x10; off += 0x10) {
        uint32_t control = d->io.read(d->io.cookie, base + off + 0xb, 1);
        uint32_t status = d->io.read(d->io.cookie, base + off + 6, 2);
        if (control == 0xff || status == 0xffff || (control & 1) || !(status & AC97_DCH) ||
            (status & 0x1c)) return false;
    }
    return true;
}
static bool ac97_irq(unsigned irq, const char *owner)
{
    (void)irq; (void)owner;
    if (!ac97.ready || ac97.stopping) return false;
    uint64_t start = ktime_cycles();
    uint16_t status = rd(&ac97, 0x16, 2);
    unsigned causes = status & 0x1c;
    if (!causes) return false;
    ac97.irqs++;
    ac97.irq_cursor = (struct ac97_cursor){ .status=status, .civ=rd(&ac97, 0x14, 1),
        .piv=rd(&ac97, 0x1a, 1), .remaining=rd(&ac97, 0x18, 2) };
    if (causes & AC97_FIFOE) ac97.fifo_errors++;
    if ((status & AC97_DCH) && !ac97.observed_halt) { if (!ac97.idle_tail) ac97.dma_halts++; ac97.observed_halt = true; }
    wr(&ac97, 0x16, causes, 2);
    audio_wake();
    uint64_t us = g_tsc_per_ms ? (ktime_cycles() - start) * 1000 / g_tsc_per_ms : 0;
    if (us > audio.max_irq_us) audio.max_irq_us = (uint32_t)us;
    return true;
}
static bool idle_proof(int handle, gen_t generation)
{ (void)handle; (void)generation; return !ac97.running && !ac97.irq_attached && !ac97.quarantine; }
void ac97_shutdown(void)
{
    struct ac97_device *d = &ac97;
    d->ready = false;
    bool unsafe = d->quarantine;
    if (d->touched) {
        /* A prior quarantine permits RUN clear, never reset/republication. */
        if (d->quarantine ? ac97_pause(d) : ac97_stop(d)) unsafe = true;
    }
    if (d->irq_attached) {
        if (irq_chain_remove(d->pci.irq_line, ac97_irq, "ac97")) unsafe = true;
        else d->irq_attached = false;
    }
    if (!unsafe && d->codec_changed && ac97_codec_restore(d)) unsafe = true;
    if (!unsafe && d->touched) {
        config_command(&d->pci, d->command);
        if ((config_read(&d->pci, 4) & 0xffff) != d->command) unsafe = true;
    }
    d->quarantine = unsafe;
    for (unsigned i = 0; i < 12; i++) if (d->claims[i] >= 0) {
        if (unsafe) registry_quarantine(d->claims[i], d->claim_generation[i]);
        else {
            const struct resource *r = registry_get(d->claims[i]);
            int err = !r || r->generation != d->claim_generation[i] ? -EINVAL : 0;
            /* An admission refusal may precede the activation loop. These
             * claims have issued no hardware work, but release still needs
             * the registry's ACTIVE -> QUIESCING proof. */
            if (!err && r->state == RS_CLAIMED) err = registry_activate(d->claims[i], d->claim_generation[i]);
            if (!err) err = registry_quiesce(d->claims[i], d->claim_generation[i], idle_proof);
            if (!err) err = registry_release(d->claims[i], d->claim_generation[i]);
            if (err) {
                unsafe = d->quarantine = true;
                registry_quarantine(d->claims[i], d->claim_generation[i]);
            } else d->claims[i] = -1;
        }
    }
    if (!unsafe) {
        if (d->bdl_phys) { pmm_free(d->bdl_phys); audio.commitment_pages--; d->bdl_phys = 0; }
        for (unsigned i = 0; i < 8; i++) if (d->payload_pages[i]) {
            pmm_free(d->payload_pages[i]); audio.commitment_pages--; d->payload_pages[i] = 0;
        }
    } else { audio.quarantined = true; audio.reason = "stop_unproved"; }
}
int ac97_init(void)
{
    struct ac97_device *d = &ac97;
    memset(d, 0, sizeof(*d));
    for (unsigned i = 0; i < 12; i++) d->claims[i] = -1;
    d->io = (struct ac97_io){ 0, read_io, write_io, now_io, sleep_io };
    const struct pci_func *p = 0;
    for (unsigned i = 0; i < pci_count(); i++) if (supported(pci_get(i))) { p = pci_get(i); break; }
    if (!p) { audio.reason = "device_absent"; return -ENODEV; }
    d->pci = *p; d->command = p->command;
    if (!controller_layout(d,g_boot.flags)) {
        audio.reason = "invalid_bars"; return -ENODEV;
    }
    if (!routed(p)) { audio.reason = "routing_unqualified"; return -ENODEV; }
    for (unsigned i = 0; i < pci_count(); i++) {
        const struct pci_func *peer = pci_get(i);
        if (peer->bus == p->bus && peer->dev == p->dev && peer->fn == 6 && peer->class_code == 7 && !modem_idle(d, peer)) {
            audio.reason = "aclink_dependency"; return -ENODEV;
        }
    }
    uint32_t starts[4] = { d->nam, d->nabm, p->irq_line, 0 };
    uint32_t ends[4] = { d->nam + d->nam_bytes, d->nabm + d->nabm_bytes, p->irq_line + 1u, 0 };
    enum res_type types[4] = { RES_PORT, RES_PORT, RES_IRQ, RES_MMIO };
    int err = 0;
    for (unsigned i = 0; i < 3; i++) {
        d->claims[i] = registry_claim(types[i], starts[i], ends[i], "ac97", i == 2);
        if (d->claims[i] < 0) { err = d->claims[i]; d->claims[i] = -1; break; }
        d->claim_generation[i] = registry_get(d->claims[i])->generation;
    }
    if (!err && (p->command & 1)) {
        /* Read-only idle checks before the first write; existing engines or
         * firmware ownership fail without reset or takeover. */
        for (unsigned off = 0; off <= 0x20; off += 0x10)
            if ((rd(d, off + 0xb, 1) & 1) || !(rd(d, off + 6, 2) & AC97_DCH)) err = -EBUSY;
    }
    if (!err) {
        d->bdl_phys = pmm_alloc_zone(ZONE_DMA);
        if (!d->bdl_phys) err = -ENOMEM;
        else { d->bdl = P2V(d->bdl_phys); memset(d->bdl, 0, PAGE_SIZE); audio.commitment_pages++; }
    }
    for (unsigned i = 0; !err && i < 8; i++) {
        d->payload_pages[i] = pmm_alloc_zone(ZONE_DMA);
        if (!d->payload_pages[i]) { err = -ENOMEM; break; }
        memset(P2V(d->payload_pages[i]), 0, PAGE_SIZE); audio.commitment_pages++;
        for (unsigned j = 0; j < 4; j++) {
            unsigned slot = i * 4 + j;
            d->pcm[slot] = (struct audio_frame *)((uint8_t *)P2V(d->payload_pages[i]) + j * 1024);
            d->bdl[slot].address = d->payload_pages[i] + j * 1024;
        }
    }
    if (!err) {
        d->claims[3] = registry_claim(RES_MMIO, d->bdl_phys, d->bdl_phys + PAGE_SIZE, "ac97-dma", false);
        if (d->claims[3] < 0) { err = d->claims[3]; d->claims[3] = -1; }
        else d->claim_generation[3] = registry_get(d->claims[3])->generation;
    }
    for (unsigned i = 0; !err && i < 8; i++) {
        unsigned at = i + 4;
        d->claims[at] = registry_claim(RES_MMIO, d->payload_pages[i], d->payload_pages[i] + PAGE_SIZE, "ac97-dma", false);
        if (d->claims[at] < 0) { err = d->claims[at]; d->claims[at] = -1; }
        else d->claim_generation[at] = registry_get(d->claims[at])->generation;
    }
    for (unsigned i = 0; !err && i < 12; i++) err = registry_activate(d->claims[i], d->claim_generation[i]);
    if (!err) {
        d->activation_deadline = d->io.now(d->io.cookie) + 500;
        d->touched = true;
        config_command(p, (p->command | 1) & ~4u); /* decode; no DMA before descriptors */
        if ((config_read(p, 4) & 0xffff) != ((p->command | 1u) & ~4u)) err = -EIO;
        if (!err) err = ac97_controller_reset(d);
        if (!err) err = ac97_stop(d); /* BDBAR must follow cold reset */
        if (!err) err = ac97_codec_prepare(d);
        if (!err) {
            config_command(p, p->command | 5);
            if ((config_read(p, 4) & 0xffff) != (p->command | 5u)) err = -EIO;
        }
        if (!err) { d->ready = true; err = irq_chain_add(p->irq_line, ac97_irq, "ac97"); d->irq_attached = !err; }
    }
    if (err) { audio.reason = "activation_failed"; ac97_shutdown(); return err; }
    d->generation = gen_alloc();
    if (!d->generation) { audio.reason = "generation_exhausted"; ac97_shutdown(); return -ENOMEM; }
    d->activation_deadline = 0;
    audio.caps = (struct ciuki_audio_caps){ .version=1, .size=80, .formats=3,
        .min_rate=8000, .max_rate=48000, .output_rate=48000, .max_streams=8,
        .ring_frames=2048, .period_frames=256, .published_periods=8, .backend=1,
        .generation=d->generation, .vendor=p->vendor, .device=p->device,
        .subsystem=(uint32_t)p->sub_vendor | (uint32_t)p->sub_device << 16,
        .revision=p->revision, .codec_id=d->codec_id, .drain_ms=20 };
    audio.reason = "ready";
    return 0;
}
