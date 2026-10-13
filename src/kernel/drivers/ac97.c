/* Intel ICH PCM-out. Sources/decisions: docs/design/f4-acceptance.md.
 * No upstream code imported. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/mm.h>
#include <ciuki/task.h>
#include <ciuki/ac97.h>
#include <ciuki/audio_backend.h>
#include <ciuki/timing.h>

struct ac97_device ac97;
#define AC97_ANALOG_WAIT_MS 5000u
#define AC97_ACTIVATION_MS 7000u
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
static unsigned ac97_offset(struct ac97_device *d, unsigned off)
{
    if (d->sis && off < 0x30 && ((off & 15) == 6 || (off & 15) == 8)) off ^= 14;
    return off;
}
static unsigned ac97_units(struct ac97_device *d) { return d->sis ? 1024 : 512; }
static uint32_t rd(struct ac97_device *d, unsigned off, unsigned width)
{ return d->io.read(d->io.cookie, d->nabm + ac97_offset(d,off), width); }
static void wr(struct ac97_device *d, unsigned off, uint32_t v, unsigned width)
{ d->io.write(d->io.cookie, d->nabm + ac97_offset(d,off), v, width); }
static void stage_log(struct ac97_device *d, const char *stage, const char *result)
{
    /* Cached NAM reads only. Reading CAS for diagnostics would acquire it
     * without issuing the codec transaction that releases it (ICH3 13.2.10). */
    klog("[audio] stage=%s reg26=%04x reg02=%04x reg18=%04x valid=%02x cnt=%08x sta=%08x cas=%02x codec=%08x time=%llu result=%s access=%s reg=%02x value=%04x",
         stage, d->power, d->master, d->pcm_volume, d->codec_valid,
         rd(d, 0x2c, 4), d->global_status, d->last_cas, d->codec_id,
         d->io.now(d->io.cookie), result, d->codec_error ? d->codec_error : "ok",
         d->access_reg, d->access_value);
}
static void stage_next(struct ac97_device *d, const char *stage)
{
    if (d->activation_stage) stage_log(d, d->activation_stage,
        d->analog_warning && !strncmp(d->activation_stage, "codec_power", sizeof("codec_power")) ? "analog_not_ready" : "ok");
    d->activation_stage = stage;
}
static void failure_snapshot(struct ac97_device *d, const char *stage)
{
    if (d->failure.stage) return;
    stage_log(d, stage, "failed");
    struct ac97_failure *f = &d->failure;
    f->stage = stage;
    f->ext_control = d->raw_ext_control; f->rate = d->raw_rate;
    for (unsigned i = 0; i < 3; i++) f->status[i] = rd(d, i * 0x10 + 6, 2);
    f->control = rd(d, 0x1b, 1); f->lvi = rd(d, 0x15, 1);
    f->cursor = (struct ac97_cursor){ .status=f->status[1], .civ=rd(d, 0x14, 1),
        .piv=rd(d, 0x1a, 1), .remaining=rd(d, 0x18, 2) };
}
static bool expired(struct ac97_device *d, uint64_t deadline)
{ return (int64_t)(d->io.now(d->io.cookie) - deadline) >= 0; }
static uint64_t bounded_deadline(struct ac97_device *d, uint32_t ms)
{
    uint64_t deadline = d->io.now(d->io.cookie) + ms;
    return d->activation_deadline && (int64_t)(deadline - d->activation_deadline) > 0 ? d->activation_deadline : deadline;
}
static uint64_t operation_deadline(struct ac97_device *d)
{ return bounded_deadline(d, 500); }
static uint64_t codec_deadline(struct ac97_device *d, uint64_t limit)
{
    uint64_t deadline = d->io.now(d->io.cookie) + 100;
    return (int64_t)(deadline - limit) > 0 ? limit : deadline;
}
int ac97_controller_reset(struct ac97_device *d)
{
    uint64_t limit = bounded_deadline(d, 1000);
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
static int codec_error(struct ac97_device *d, const char *reason, int err)
{
    d->codec_error = reason;
    stage_log(d, d->activation_stage ? d->activation_stage : "codec_access", "access_failed");
    return err;
}
static int cas(struct ac97_device *d, uint16_t reg, uint64_t deadline)
{
    d->access_reg = reg; d->access_value = 0xffff; d->codec_error = 0;
    while (!expired(d, deadline)) {
        uint32_t control = rd(d, 0x2c, 4);
        d->global_status = rd(d, 0x30, 4);
        if (control == UINT32_MAX || d->global_status == UINT32_MAX)
            return codec_error(d, "controller_absent", -EIO);
        if ((control & 0x0e) != 2) return codec_error(d, "link_off_or_reset", -EIO);
        if (!(d->global_status & 0x100)) return codec_error(d, "primary_not_ready", -EIO);
        uint8_t v = d->last_cas = (uint8_t)rd(d, 0x34, 1);
        if (v == 0xff) return codec_error(d, "cas_invalid", -EIO);
        if (!(v & 1)) return 0;
        d->io.sleep(d->io.cookie, 1);
    }
    return codec_error(d, "cas_timeout", -ETIMEDOUT);
}
int ac97_codec_read(struct ac97_device *d, uint16_t reg, uint16_t *out, uint64_t limit)
{
    uint64_t deadline = codec_deadline(d, limit);
    int err = cas(d, reg, deadline);
    if (err) return err;
    *out = d->access_value = (uint16_t)d->io.read(d->io.cookie, d->nam + reg, 2);
    d->global_status = rd(d, 0x30, 4);
    if (d->global_status == UINT32_MAX) return codec_error(d, "controller_absent", -EIO);
    if (d->global_status & (1u << 15)) {
        err = codec_error(d, "read_timeout", -EIO);
        wr(d, 0x30, 1u << 15, 4); /* RCS only; preserve all unrelated W1C bits */
        return err;
    }
    if (!(d->global_status & 0x100)) return codec_error(d, "primary_not_ready", -EIO);
    if (*out == 0xffff) return codec_error(d, "read_invalid", -EIO);
    if (expired(d, deadline)) return codec_error(d, "access_timeout", -ETIMEDOUT);
    /* Diagnostics retain the last successful hardware read, including
     * mismatches and restoration; never report a desired write as readback. */
    if (reg == 0x26) { d->power = *out; d->codec_valid |= 1; }
    else if (reg == 0x02) { d->master = *out; d->codec_valid |= 2; }
    else if (reg == 0x18) { d->pcm_volume = *out; d->codec_valid |= 4; }
    else if (reg == 0x2a) d->raw_ext_control = *out;
    else if (reg == 0x2c) d->raw_rate = *out;
    return 0;
}
int ac97_codec_write(struct ac97_device *d, uint16_t reg, uint16_t value, uint64_t limit)
{
    uint64_t deadline = codec_deadline(d, limit);
    int err = cas(d, reg, deadline);
    if (err) return err;
    d->io.write(d->io.cookie, d->nam + reg, value, 2);
    uint16_t got;
    err = ac97_codec_read(d, reg, &got, deadline);
    return err ? err : got != value ? codec_error(d, "write_mismatch", -EIO) : 0;
}
/* Read-only readiness bits must not participate in power/extended-control
 * write verification. Volume controls get one bounded retry for sticky mute. */
static int codec_update(struct ac97_device *d, uint16_t reg, uint16_t value,
                        uint16_t mask, bool retry, uint64_t limit)
{
    uint64_t deadline = codec_deadline(d, limit);
    for (unsigned attempt = 0; attempt < (retry ? 2u : 1u); attempt++) {
        int err = cas(d, reg, deadline);
        if (err) return err;
        d->io.write(d->io.cookie, d->nam + reg, value, 2);
        uint16_t got;
        err = ac97_codec_read(d, reg, &got, deadline);
        if (err) return err;
        if (!((got ^ value) & mask)) return 0;
        if (!attempt && retry) d->io.sleep(d->io.cookie, 1);
    }
    return codec_error(d, "write_mismatch", -EIO);
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
    uint64_t limit = bounded_deadline(d, 1000);
    d->analog_warning = false;
    stage_next(d, "codec_ready");
    while (!((d->global_status = rd(d, 0x30, 4)) & 0x100)) {
        if (expired(d, limit)) return -ETIMEDOUT;
        d->io.sleep(d->io.cookie, 1);
    }
    if (d->global_status == UINT32_MAX) return -EIO;
    stage_next(d, "codec_identity");
    limit = operation_deadline(d);
    uint16_t id1, id2, ext;
    int err = ac97_codec_read(d, 0x7c, &id1, limit);
    if (!err) err = ac97_codec_read(d, 0x7e, &id2, limit);
    if (!err && (!id1 || !id2)) err = codec_error(d, "identity_invalid", -EIO);
    if (err) return err;
    d->codec_id = (uint32_t)id1 << 16 | id2;
    stage_next(d, "codec_reset");
    /* Register 00h is a reset command on write and capabilities on read,
     * not an ordinary value to verify. Drain the posted write through CAS
     * after 1 ms (Linux uses 100 us), then read capabilities and identity. */
    uint64_t reset_limit = codec_deadline(d, limit);
    err = cas(d, 0, reset_limit);
    if (!err) {
        d->io.write(d->io.cookie, d->nam, 0, 2);
        d->codec_valid = 0;
        d->io.sleep(d->io.cookie, 1);
        err = ac97_codec_read(d, 0, &d->codec_caps, reset_limit);
    }
    if (!err) err = ac97_codec_read(d, 0x7c, &id1, limit);
    if (!err) err = ac97_codec_read(d, 0x7e, &id2, limit);
    if (!err && ((uint32_t)id1 << 16 | id2) != d->codec_id)
        err = codec_error(d, "identity_changed", -EIO);
    if (!err) err = ac97_codec_read(d, 0x26, &d->power, limit);
    /* Digital control/rate snapshots are readable before analog calibration;
     * retain them even if the subsequent power-ready wait expires. */
    if (!err) err = ac97_codec_read(d, 0x28, &ext, limit);
    if (!err) err = ac97_codec_read(d, 0x2a, &d->saved_ext, limit);
    if (!err) err = ac97_codec_read(d, 0x2c, &d->saved_rate, limit);
    if (err) { failure_snapshot(d, d->activation_stage); d->quarantine = true; return err; }
    d->saved_power = d->power;
    /* Power control has RO ready bits; EAPD=0 enables the external amp.
     * No guessed vendor amplifier polarity/registers. */
    d->codec_changed = true;
    stage_next(d, "codec_power");
    err = codec_update(d, 0x26, 0, 0xff00, false, limit);
    limit = bounded_deadline(d, AC97_ANALOG_WAIT_MS);
    while (!err && !expired(d, limit)) {
        err = ac97_codec_read(d, 0x26, &d->power, limit);
        if (err || (d->power & 0xf) == 0xf) break;
        d->io.sleep(d->io.cookie, 1);
    }
    /* Only absent analog-ready bits are advisory. An access error or the
     * caller's shorter overall deadline is still a hard failure. */
    if (!err && d->activation_deadline && expired(d, d->activation_deadline)) err = -ETIMEDOUT;
    d->analog_warning = !err && (d->power & 0xf) != 0xf;
    if (err) { failure_snapshot(d, d->activation_stage); d->quarantine = true; return err; }
    stage_next(d, "codec_snapshot");
    limit = operation_deadline(d);
    if (!err) err = ac97_codec_read(d, 0x02, &d->saved_master, limit);
    if (!err) err = ac97_codec_read(d, 0x18, &d->saved_pcm, limit);
    if (!err && (d->codec_caps & 0x10)) err = ac97_codec_read(d, 0x04, &d->saved_headphone, limit);
    /* PC beep is optional; ffff/absent is not a playback failure. */
    if (!err) {
        int beep = ac97_codec_read(d, 0x0a, &d->saved_beep, limit);
        if (beep && (beep != -EIO || d->saved_beep != 0xffff ||
            !d->codec_error || strncmp(d->codec_error, "read_invalid", sizeof("read_invalid")))) err = beep;
        else { d->beep_present = !beep && d->saved_beep != 0; d->codec_error = 0; }
    }
    d->cirrus_spdif = (d->codec_id & 0xfffffff8u) == 0x43525930;
    if (!err && d->cirrus_spdif) err = ac97_codec_read(d, 0x68, &d->saved_spdif, limit);
    if (err) { failure_snapshot(d, d->activation_stage); d->quarantine = true; return err; }
    stage_next(d, "codec_response");
    /* Two different muted values reject both stale zero and a stuck reset
     * mute value. No DMA/ready admission on identity alone. */
    err = codec_update(d, 0x02, 0x8101, 0x9f1f, true, limit);
    if (!err) err = codec_update(d, 0x02, 0x8000, 0x9f1f, true, limit);
    if (!err && d->analog_warning) stage_log(d, "codec_response", "analog_warning_accepted");
    if (!err) stage_next(d, "codec_rate");
    uint16_t control = d->saved_ext & ~0x000fu; /* VRA/DRA/S/PDIF/VRM */
    if (ext & 1) control |= 1;
    if (!err) err = codec_update(d, 0x2a, control, (ext & 1) ? 0x000e : 0x000f, false, limit);
    if (!err && (ext & 1) && (d->raw_ext_control & 1))
        err = codec_update(d, 0x2c, AUDIO_RATE, 0, false, limit);
    else if (!err) err = ac97_codec_read(d, 0x2c, &d->raw_rate, limit);
    if (!err && (d->raw_ext_control & 1) && d->raw_rate != AUDIO_RATE) {
        /* Optional/RO rate registers may differ. VRA=0 selects fixed 48 kHz;
         * prove that mode rather than pretending a differing rate was accepted. */
        err = codec_update(d, 0x2a, control & ~1u, 0x000f, false, limit);
        if (!err) err = ac97_codec_read(d, 0x2c, &d->raw_rate, limit);
    }
    if (!err) d->rate = AUDIO_RATE;
    if (!err) stage_next(d, "codec_mixer");
    if (!err && d->cirrus_spdif) err = ac97_codec_write(d, 0x68, d->saved_spdif & ~0x8000u, limit);
    if (!err && d->beep_present) err = codec_update(d, 0x0a, 0, 0x801e, true, limit);
    if (!err) err = master_until(d, &audio.master, limit);
    if (err) {
        failure_snapshot(d, d->activation_stage);
        if (ac97_codec_restore(d)) d->quarantine = true;
    }
    return err;
}
int ac97_observe(struct ac97_device *d, struct ac97_cursor *out)
{
    for (unsigned attempt = 0; attempt < 8; attempt++) {
        uint8_t civ = rd(d, 0x14, 1);
        struct ac97_cursor c = { .civ = civ, .status = rd(d, 0x16, 2),
            .remaining = rd(d, 0x18, 2), .piv = rd(d, 0x1a, 1) };
        if (civ == rd(d, 0x14, 1) && c.piv == rd(d, 0x1a, 1)) {
            if (c.civ >= 32 || c.piv >= 32 || c.remaining > ac97_units(d) || c.status == 0xffff) return -EIO;
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
    unsigned delta = 0;
    uint64_t fetched = 0;
    bool coherent = false;
    /* Linux intel8x0 retries zero PICB and a count reloading before CIV
     * advances. Never commit a transitional fetched/retired position. */
    for (unsigned attempt = 0; attempt < 8; attempt++) {
        int err = ac97_observe(d, &c);
        if (err) return err;
        delta = (c.civ - (unsigned)(d->retired % 32)) & 31;
        uint64_t outstanding = d->published - d->retired;
        if (delta >= outstanding || c.piv != ((c.civ + 1) & 31)) return -EIO;
        fetched = (d->retired + delta) * AUDIO_QUANTUM + (ac97_units(d) - c.remaining) / (d->sis ? 4 : 2);
        if (!(c.status & AC97_DCH) && (!c.remaining || fetched < d->generation_fetched)) continue;
        if (fetched < d->generation_fetched) return -EIO;
        coherent = true; break;
    }
    uint64_t now = d->io.now(d->io.cookie);
    if (!coherent) {
        if (!d->cursor_retrying) { d->cursor_retrying = true; d->cursor_retry_since = now; }
        if (now - d->cursor_retry_since > 2 || now - d->last_progress > 100) {
            d->deadline_misses++; return -EIO;
        }
        return -EAGAIN;
    }
    d->cursor_retrying = false;
    if (delta) { d->retired += delta; d->last_progress = now; }
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
    d->bdl[slot].length = (1u << 31) | ac97_units(d);
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
    d->running = true; d->last_progress = d->io.now(d->io.cookie); d->cursor_retrying = false;
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
    failure_snapshot(d, "dma_pause");
    d->quarantine = true; return -EIO;
}
static int channel_reset(struct ac97_device *d, uint64_t limit)
{
    /* Activation has never started DMA and has PCI BME disabled. Runtime
     * callers must first prove DCH and a stable cursor with ac97_pause. */
    if (d->quarantine || (rd(d, 0x1b, 1) & 1)) return -EIO;
    if (expired(d, limit)) {
        failure_snapshot(d, "dma_reset"); d->quarantine = true; return -EIO;
    }
    wr(d, 0x1b, 2, 1); /* RUN proved clear */
    while (rd(d, 0x1b, 1) & 2) {
        if (expired(d, limit)) {
            failure_snapshot(d, "dma_reset"); d->quarantine = true; return -EIO;
        }
        d->io.sleep(d->io.cookie, 1);
    }
    wr(d, 0x16, 0x1c, 2);
    wr(d, 0x10, d->bdl_phys, 4);
    if (rd(d, 0x10, 4) != d->bdl_phys) {
        failure_snapshot(d, "dma_bdbar"); d->quarantine = true; return -EIO;
    }
    d->published = d->retired = d->generation_fetched = 0; d->stopping = false;
    return 0;
}
int ac97_stop(struct ac97_device *d)
{
    uint64_t limit = operation_deadline(d);
    int err = ac97_pause(d);
    return err ? err : channel_reset(d, limit);
}
static void transport_publish(struct ac97_device *d)
{
    /* BDBAR was set after RR. Two complete silence periods also make the
     * prefetched descriptor valid; IRQ enables remain clear during proof. */
    for (unsigned i = 0; i < 2; i++) {
        memset(d->pcm[i], 0, AUDIO_QUANTUM * sizeof(struct audio_frame));
        d->bdl[i].length = (1u << 31) | ac97_units(d);
    }
    __asm__ volatile("" ::: "memory");
    wr(d, 0x15, 1, 1);
}
static int transport_prove(struct ac97_device *d)
{
    uint64_t limit = operation_deadline(d);
    stage_next(d, "dma_start");
    if (expired(d, limit)) { failure_snapshot(d, "dma_start"); return -ETIMEDOUT; }
    d->running = true; /* cleanup must prove stop even if start readback fails */
    wr(d, 0x1b, 1, 1);
    while (!expired(d, limit)) {
        struct ac97_cursor c;
        if (!(rd(d, 0x1b, 1) & 1) || ac97_observe(d, &c) || (c.status & AC97_FIFOE)) {
            failure_snapshot(d, "dma_start"); return -EIO;
        }
        /* Also accept a complete two-buffer fetch if polling missed RUN. */
        if (!(c.status & AC97_DCH) || (c.civ == 1 && !c.remaining &&
            (c.status & (AC97_CELV | AC97_LVBCI)) == (AC97_CELV | AC97_LVBCI))) {
            stage_next(d, "dma_stop");
            int err = ac97_stop(d);
            if (!err) stage_next(d, "dma_proved");
            return err;
        }
        d->io.sleep(d->io.cookie, 1);
    }
    failure_snapshot(d, "dma_start"); return -ETIMEDOUT;
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
static int controller_activate(struct ac97_device *d)
{
    const struct pci_func *p = &d->pci;
    d->activation_deadline = d->io.now(d->io.cookie) + AC97_ACTIVATION_MS;
    d->touched = true;
    d->activation_stage = "pci_decode";
    config_command(p, (p->command | 1) & ~4u); /* no DMA before descriptors */
    int err = (config_read(p, 4) & 0xffff) != ((p->command | 1u) & ~4u) ? -EIO : 0;
    if (!err) { stage_next(d, "controller_reset"); err = ac97_controller_reset(d); }
    if (!err) { stage_next(d, "dma_reset"); err = channel_reset(d, operation_deadline(d)); }
    if (!err) err = ac97_codec_prepare(d);
    if (!err && d->sis) {
        d->saved_sis_gate=rd(d,0x4c,2); d->sis_gate_changed=true;
        wr(d,0x4c,d->saved_sis_gate | 1,2);
        if (!(rd(d,0x4c,2)&1)) err=-EIO;
    }
    if (!err) {
        transport_publish(d);
        stage_next(d, "pci_master");
        config_command(p, p->command | 5);
        if ((config_read(p, 4) & 0xffff) != (p->command | 5u)) err = -EIO;
    }
    if (!err) err = transport_prove(d);
    if (err) failure_snapshot(d, d->activation_stage);
    else stage_log(d, d->activation_stage, "ok");
    return err;
}
static bool supported(const struct pci_func *p)
{
    return p->class_code == 4 && p->subclass == 1 &&
        ((p->vendor == 0x8086 && (p->device == 0x2415 || p->device == 0x2425 || p->device == 0x2445 || p->device == 0x2485 || p->device == 0x24c5)) ||
         (p->vendor == 0x1039 && p->device == 0x7012) ||
         (p->vendor == 0x10de && (p->device == 0x01b1 || p->device == 0x006a)));
}
static bool controller_layout(struct ac97_device *d, uint32_t boot_flags)
{
    const struct pci_func *p=&d->pci;
    d->sis=p->vendor==0x1039 && p->device==0x7012;
    d->nvidia=p->vendor==0x10de;
    bool twin=(boot_flags & CBI_F_SMBIOS_QEMU) && p->vendor==0x8086 && p->device==0x2415;
    d->nam_bytes=twin?1024:256;d->nabm_bytes=twin ? 256 : (d->sis || d->nvidia) ? 128 : 64;
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
    if (!unsafe && d->sis_gate_changed) {
        wr(d,0x4c,d->saved_sis_gate,2);
        if (rd(d,0x4c,2)!=d->saved_sis_gate) unsafe=true;
    }
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
    d->raw_ext_control = d->raw_rate = 0xffff; /* no successful read yet */
    for (unsigned i = 0; i < 12; i++) d->claims[i] = -1;
    d->io = (struct ac97_io){ 0, read_io, write_io, now_io, sleep_io };
    const struct pci_func *p = 0;
    for (unsigned i = 0; i < pci_count(); i++) if (supported(pci_get(i))) { p = pci_get(i); break; }
    if (!p) { audio.reason = "device_absent"; return -ENODEV; }
    d->pci = *p; d->command = p->command;
    if (!controller_layout(d,g_boot.flags)) {
        audio.reason = "invalid_bars"; return -ENODEV;
    }
    if (!(p->command&1) && (p->command&4)) { audio.reason="firmware_state_unreadable"; return -ENODEV; }
    if (!(p->vendor == 0x8086 ? routed(p) : audio_pci_routed(&d->io,p))) { audio.reason = "routing_unqualified"; return -ENODEV; }
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
        if (d->nvidia && ((rd(d,0x7b,1)&1) || !(rd(d,0x76,2)&AC97_DCH))) err=-EBUSY;
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
        err = controller_activate(d);
        if (!err) {
            stage_next(d, "irq_attach");
            d->ready = true; err = irq_chain_add(p->irq_line, ac97_irq, "ac97"); d->irq_attached = !err;
        }
    }
    if (err) {
        if (d->touched) failure_snapshot(d, d->activation_stage);
        audio.reason = d->failure.stage ? d->failure.stage : "activation_failed";
        d->activation_deadline = 0; /* cleanup has its own bounded stop budget */
        ac97_shutdown(); return err;
    }
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
    stage_next(d, "ready");
    stage_log(d, "ready", d->analog_warning ? "analog_warning" : "ok");
    return 0;
}
