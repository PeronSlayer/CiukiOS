/* f3-05 controller (f3:vm-audio-state): per-VM SB16 DSP/mixer, 8237 and OPL3
 * state driven by the DOS-side payload VMAUDIO.COM through the mailbox at
 * 0000:0600, observed through the F4 interface (ciuki/dosvm_audio.h) as its
 * consumer. Records per docs/design/f3-acceptance.md, section f3-05.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/probe.h>
#include <ciuki/dosvm.h>
#include <ciuki/dosvm_audio.h>
#include <ciuki/abi.h>
#include <ciuki/task.h>
#include <ciuki/sync.h>
#include <ciuki/storage.h>

#define AP "vm-audio-state"
struct audio_mail {
    uint16_t magic, stage, command, irqs, last82, error, version, base, irq, dma, hdma;
    uint16_t mix80, mix81, ident, test, speaker, reset, master, compat, def, status;
    uint16_t dmatc, counta, countb, isrbad, sti82, irqsb, opl[6], firqs, freset, buffer[2];
};
_Static_assert(sizeof(struct audio_mail) == 0x4A, "VMAUDIO mailbox 0600h-0649h");

static int audio_mail(struct dosvm *v, struct audio_mail *m) { return dosvm_read(v, 0x600, m, sizeof(*m)); }
static int audio_command(struct dosvm *v, uint16_t n) { return dosvm_write(v, 0x604, &n, 2); }
static int audio_stage(struct dosvm *v, unsigned stage, struct audio_mail *m, uint32_t ms)
{
    uint64_t deadline = deadline_after_ms(ms);
    for (;;) {
        struct dosvm_info info; dosvm_info(v, &info);
        if (audio_mail(v, m)) return -EFAULT;
        if (m->magic == 0xD305 && m->stage == stage) return 0;
        if (info.state >= DOSVM_STOPPING) return -EIO;
        if (deadline_passed(deadline)) {
            struct dosvm_audio_info a = {0}; dosvm_audio_info(dosvm_key(v), &a);
            struct dosvm_device_info d = {0}; dosvm_device_info(dosvm_key(v), &d);
            rec_emit(AP, "DATA", "case=stage_timeout wanted=%u stage=%u command=%u irqs=%u last82=%02x error=%u isrbad=%u",
                     stage, m->stage, m->command, m->irqs, m->last82, m->error, m->isrbad);
            rec_emit(AP, "DATA", "case=stage_audio active=%u waiting=%u paused=%u causes=%u boundaries=%u edges=%u services=%u late=%u faults=%u irr0=%02x isr0=%02x imr0=%02x irq7=%u",
                     a.active, a.waiting, a.paused, a.causes, a.boundaries, a.irq_edges, a.services, a.late,
                     a.dma_faults, d.irr[0], d.isr[0], d.imr[0], d.delivered[7]);
            return -V86_ETIMEDOUT;
        }
        task_sleep_ms(1);
    }
}
static bool audio_prompt(struct dosvm *v)
{
    const uint16_t *text = dosvm_text(v, 0);
    for (unsigned i = 0; i < 1997; i++)
        if ((uint8_t)text[i] == 'C' && (uint8_t)text[i + 1] == ':' && (uint8_t)text[i + 2] == '\\')
            for (unsigned j = i + 3; j < 2000 && j < i + 80; j++) if ((uint8_t)text[j] == '>') return true;
    return false;
}
static int audio_boot(struct dosvm *v)
{
    int e = dosvm_boot(v);
    if (e) return e;
    uint64_t deadline = deadline_after_ms(30000);
    for (;;) {
        struct dosvm_info info; dosvm_info(v, &info);
        if (info.dos_ready && audio_prompt(v)) return 0;
        if (info.state >= DOSVM_STOPPING) return -EIO;
        if (deadline_passed(deadline)) return -V86_ETIMEDOUT;
        task_sleep_ms(1);
    }
}
static int audio_type(struct dosvm *v, const char *text)
{
    struct dosvm_key key = dosvm_key(v);
    for (unsigned i = 0; text[i]; i++) if (!dosvm_keyboard(key, (uint8_t)text[i])) return -ENOSPC;
    return dosvm_keyboard(key, 0x1C0D) ? 0 : -ENOSPC;
}

/* PCM blocks drained through dosvm_audio_fetch and compared frame by frame. */
enum audio_pattern { PATTERN_RAMP, PATTERN_SEQ8, PATTERN_SEQ16 };
struct audio_blocks { uint32_t blocks, fresh, frames, segments, bad_spacing, causes, rate, format, flags; uint64_t last; };
static bool audio_expected(enum audio_pattern p, uint32_t block, uint32_t frame, int16_t l, int16_t r)
{
    if (p == PATTERN_RAMP) { int32_t x = ((int32_t)(frame & 255) - 128) * 256; return l == x && r == x; }
    if (p == PATTERN_SEQ8) { int32_t x = ((int32_t)(0x20 + block) - 128) * 256; return l == x && r == x; }
    int16_t left = (int16_t)(uint16_t)((block << 8) | 0x11);
    return l == left && r == (int16_t)~left;
}
static int16_t audio_pcm[256];
static void audio_drain(struct dosvm_key key, uint32_t token, enum audio_pattern p, uint32_t frames_per_block,
                        uint32_t spacing, struct audio_blocks *out)
{
    memset(out, 0, sizeof(*out));
    bool good = true; uint32_t block_frames = 0;
    for (unsigned guard = 0; guard < 4096; guard++) {
        struct dosvm_audio_block b;
        int n = dosvm_audio_fetch(key, token, &b, audio_pcm, 128);
        if (n < 0) break;
        if (!b.offset) {
            out->segments++;
            if (out->blocks && b.start - out->last != spacing) out->bad_spacing++;
            out->last = b.start; block_frames = 0; good = true;
            out->rate = b.rate; out->format = b.format;
        }
        for (int i = 0; i < n; i++)
            if (!audio_expected(p, out->blocks, b.offset + (uint32_t)i, audio_pcm[2 * i], audio_pcm[2 * i + 1])) good = false;
        block_frames += (uint32_t)n; out->frames += (uint32_t)n;
        if (!b.closed) { if (!n) break; continue; }
        out->flags |= b.flags; out->causes |= b.cause;
        if (b.flags & DOSVM_BLOCK_COMPLETE) {
            if (good && block_frames == frames_per_block && b.flags == DOSVM_BLOCK_COMPLETE) out->fresh++;
            out->blocks++;
        }
    }
}

/* VMAUDIO.COM OPL script, as written (register, value). Writes only. */
static const uint16_t opl_regs[] = {0x004, 0x004, 0x002, 0x004, 0x004, 0x004, 0x0C0, 0x105, 0x104, 0x1C0,
    0x0BD, 0x1B0, 0x0A0, 0x040, 0x020, 0x023, 0x060, 0x063, 0x080, 0x083, 0x0A1, 0x0B1, 0x002, 0x003,
    0x004, 0x004, 0x004, 0x004, 0x0B1, 0x105};
static const uint8_t opl_values[] = {0x60, 0x80, 0xFF, 0x21, 0x60, 0x80, 0x31, 0x01, 0x3F, 0xF1,
    0x3F, 0x31, 0x98, 0x10, 0x01, 0x01, 0xF0, 0xF0, 0x77, 0x77, 0x98, 0x31, 0x00, 0x00,
    0x03, 0x43, 0x80, 0x00, 0x11, 0x00};
_Static_assert(sizeof(opl_regs) / sizeof(opl_regs[0]) == sizeof(opl_values), "OPL script");
static struct dosvm_audio_event audio_events[64];
static uint8_t audio_regs[512];

static int probe_f3_audio_state(void)
{
    rec_emit(AP, "BEGIN", "version=1");
    struct dosvm *v = 0, *w = 0; uint32_t token = 0; int error = 0;
    struct audio_mail m = {0};
    struct dosvm_audio_info a = {0}, b = {0};
    struct dosvm_device_info d0 = {0}, d1 = {0};
    struct audio_blocks blocks;
    struct dosvm_ledger baseline, final;
    dosvm_snapshot(&baseline);
    const struct dosvm_config config = {.commitment = DOSVM_LOW_CAP, .probe = AP};
    if ((error = storage_enable_write(storage_get(), 2))) goto cleanup;
    if ((error = dosvm_create(&config, &w)) || (error = audio_boot(w)) ||
        (error = dosvm_create(&config, &v)) || (error = audio_boot(v))) goto cleanup;
    struct dosvm_key key = dosvm_key(v);
    if ((error = dosvm_audio_attach(key, &token))) goto cleanup;
    if ((error = audio_type(v, "\\VMAUDIO.COM"))) goto cleanup; /* the shell starts in C:\APPS */

    /* 1. Detection: reset/version/ident, speaker latch, BLASTER-style A/I/D/H, mixer. */
    if ((error = audio_stage(v, 1, &m, 20000))) goto cleanup;
    bool ok = m.reset == 0xAA && m.version == 0x0405 && m.ident == 0xAA && m.test == 0x5A && m.speaker == 0xFF00 &&
              m.base == 0x220 && m.irq == 7 && m.dma == 1 && m.hdma == 5 && !m.error;
    rec_emit(AP, "DATA", "case=detect ok=%u reset=%02x version=%04x ident=%02x test=%02x speaker=%04x base=%03x irq=%u dma=%u hdma=%u error=%u",
             ok, m.reset, m.version, m.ident, m.test, m.speaker, m.base, m.irq, m.dma, m.hdma, m.error);
    bool mixer = m.mix80 == 0x04 && m.mix81 == 0x22 && m.status == 0x20 && m.master == 0xA8 && m.compat == 0xA5 && m.def == 0xC0;
    rec_emit(AP, "DATA", "case=mixer ok=%u irq_bits=%02x dma_bits=%02x status=%02x master=%02x compat=%02x default=%02x",
             mixer, m.mix80, m.mix81, m.status, m.master, m.compat, m.def);
    if (!ok || !mixer) { error = -EFAULT; goto cleanup; }

    /* 2. 8-bit single cycle: 256-byte ramp at 8000 Hz (32 ms), one IRQ, TC. */
    dosvm_audio_info(key, &b);
    uint64_t start = g_ticks;
    if ((error = audio_command(v, 2)) || (error = audio_stage(v, 2, &m, 10000))) goto cleanup;
    uint64_t elapsed = g_ticks - start;
    dosvm_audio_info(key, &a);
    audio_drain(key, token, PATTERN_RAMP, 256, 0, &blocks);
    ok = m.irqs == 1 && (m.last82 & 1) && (m.dmatc & 2) && blocks.blocks == 1 && blocks.fresh == 1 &&
         blocks.rate == 8000 && blocks.causes == 1 && elapsed >= 32 && a.boundaries == b.boundaries + 1;
    rec_emit(AP, "DATA", "case=dma8_single ok=%u irqs=%u status=%02x tc=%02x blocks=%u fresh=%u frames=%u rate=%u cause=%u host_ms=%llu",
             ok, m.irqs, m.last82, m.dmatc, blocks.blocks, blocks.fresh, blocks.frames, blocks.rate, blocks.causes, elapsed);
    if (!ok) { error = -EFAULT; goto cleanup; }

    /* 3. 64-unit auto-init double buffer, 32 blocks: fresh, serviced, nominal spacing. */
    dosvm_audio_info(key, &b);
    if ((error = audio_command(v, 3)) || (error = audio_stage(v, 3, &m, 10000))) goto cleanup;
    task_sleep_ms(2);
    dosvm_audio_info(key, &a);
    audio_drain(key, token, PATTERN_SEQ8, 64, 384, &blocks); /* 64 units at 8000 Hz = 384 timeline frames */
    ok = m.irqs == 32 && !m.isrbad && blocks.blocks == 32 && blocks.fresh == 32 && !blocks.bad_spacing &&
         a.services - b.services == 32 && a.boundaries - b.boundaries == 32 && !a.stale_blocks && !a.active;
    rec_emit(AP, "DATA", "case=double_buffer ok=%u irqs=%u blocks=%u fresh=%u services=%u boundaries=%u late=%u stale=%u bad_spacing=%u isr_bad=%u max_service=%u",
             ok, m.irqs, blocks.blocks, blocks.fresh, a.services - b.services, a.boundaries - b.boundaries,
             a.late - b.late, a.stale_blocks, blocks.bad_spacing, m.isrbad, a.max_service);
    if (!ok) { error = -EFAULT; goto cleanup; }

    /* 4-5. Pause (D0h) holds DMA and the block timeline; continue (D4h); exit (DAh). */
    if ((error = audio_command(v, 4)) || (error = audio_stage(v, 4, &m, 10000))) goto cleanup;
    dosvm_audio_info(key, &b); task_sleep_ms(50); dosvm_audio_info(key, &a);
    ok = a.paused && a.active && a.units == b.units && a.boundaries == b.boundaries && m.irqs == 2;
    rec_emit(AP, "DATA", "case=pause ok=%u paused=%u units=%u boundaries=%u count=%u irqs=%u",
             ok, a.paused, a.units - b.units, a.boundaries - b.boundaries, m.counta, m.irqs);
    if (!ok) { error = -EFAULT; goto cleanup; }
    if ((error = audio_command(v, 5)) || (error = audio_stage(v, 5, &m, 10000))) goto cleanup;
    task_sleep_ms(2);
    dosvm_audio_info(key, &a);
    audio_drain(key, token, PATTERN_SEQ8, 80, 0, &blocks); /* content not checked: unrefilled ring */
    ok = m.counta == m.countb && m.irqs == 5 && !m.isrbad && !a.active && blocks.blocks == 5;
    rec_emit(AP, "DATA", "case=continue ok=%u count_a=%u count_b=%u irqs=%u active=%u blocks=%u segments=%u",
             ok, m.counta, m.countb, m.irqs, a.active, blocks.blocks, blocks.segments);
    if (!ok) { error = -EFAULT; goto cleanup; }

    /* 6. 16-bit signed stereo auto-init on the high DMA, 2xFh acknowledgement. */
    if ((error = audio_command(v, 6)) || (error = audio_stage(v, 6, &m, 10000))) goto cleanup;
    task_sleep_ms(2);
    dosvm_audio_info(key, &a);
    audio_drain(key, token, PATTERN_SEQ16, 64, 256, &blocks); /* 64 frames at 12000 Hz = 256 */
    ok = m.irqs == 8 && !m.isrbad && (m.last82 & 2) && blocks.blocks == 8 && blocks.fresh == 8 && !blocks.bad_spacing &&
         blocks.causes == 2 && blocks.rate == 12000 &&
         blocks.format == (DOSVM_FORMAT_16BIT | DOSVM_FORMAT_SIGNED | DOSVM_FORMAT_STEREO) && !a.active;
    rec_emit(AP, "DATA", "case=dma16_stereo ok=%u irqs=%u blocks=%u fresh=%u cause=%u format=%u rate=%u bad_spacing=%u isr_bad=%u",
             ok, m.irqs, blocks.blocks, blocks.fresh, blocks.causes, blocks.format, blocks.rate, blocks.bad_spacing, m.isrbad);
    if (!ok) { error = -EFAULT; goto cleanup; }

    /* 7-8. Virtual IF clear for 100 ms: timeline and blocks continue, one
     * pending request, no delivery; STI delivers exactly that request. */
    if ((error = audio_command(v, 7)) || (error = audio_stage(v, 7, &m, 10000))) goto cleanup;
    dosvm_audio_info(key, &b); dosvm_device_info(key, &d0);
    uint64_t hold = g_ticks;
    task_sleep_ms(100);
    dosvm_audio_info(key, &a); dosvm_device_info(key, &d1);
    hold = g_ticks - hold;
    uint32_t expect = (uint32_t)(hold / 10);
    struct audio_mail held; audio_mail(v, &held);
    ok = a.boundaries - b.boundaries + 1 >= expect && a.boundaries - b.boundaries <= expect + 1 &&
         a.irq_edges - b.irq_edges <= 1 && (d1.irr[0] & 0x80) && d1.delivered[7] == d0.delivered[7] &&
         a.causes == 1 && held.irqs == 1 && a.clock - a.dsp_clock <= 480;
    rec_emit(AP, "DATA", "case=if_clear ok=%u host_ms=%llu boundaries=%u expected=%u edges=%u pending=%u delivered=%u causes=%u guest_irqs=%u lag=%llu",
             ok, hold, a.boundaries - b.boundaries, expect, a.irq_edges - b.irq_edges, !!(d1.irr[0] & 0x80),
             d1.delivered[7] - d0.delivered[7], a.causes, held.irqs, a.clock - a.dsp_clock);
    if (!ok) { error = -EFAULT; goto cleanup; }
    if ((error = audio_command(v, 8)) || (error = audio_stage(v, 8, &m, 10000))) goto cleanup;
    ok = m.irqsb == 1 && (m.sti82 & 1) && m.irqs == 3 && !m.isrbad;
    rec_emit(AP, "DATA", "case=if_restore ok=%u irqs_before=%u status=%02x irqs=%u", ok, m.irqsb, m.sti82, m.irqs);
    if (!ok) { error = -EFAULT; goto cleanup; }
    task_sleep_ms(2);
    audio_drain(key, token, PATTERN_SEQ8, 80, 0, &blocks); /* discard the IF-clear blocks */

    /* 9. OPL3: detection, NEW/pairs/rhythm/array addressing, SB16 mirrors,
     * timers on the VM clock and the timestamped event queue. */
    while (dosvm_audio_events(key, token, audio_events, 64) > 0) {}
    dosvm_audio_info(key, &b);
    if ((error = audio_command(v, 9)) || (error = audio_stage(v, 9, &m, 10000))) goto cleanup;
    dosvm_audio_info(key, &a);
    int n = dosvm_audio_events(key, token, audio_events, 64);
    unsigned match = 0, monotonic = 1, generation = 1, window = 1;
    for (int i = 0; i < n; i++) {
        if (i < (int)ARRAY_SIZE(opl_regs) && audio_events[i].kind == DOSVM_EVENT_OPL &&
            audio_events[i].reg == opl_regs[i] && audio_events[i].value == opl_values[i]) match++;
        if (i && audio_events[i].clock < audio_events[i - 1].clock) monotonic = 0;
        if (audio_events[i].generation != key.generation) generation = 0;
        if (audio_events[i].clock < b.clock || audio_events[i].clock > a.clock + DOSVM_AUDIO_HZ / 1000) window = 0;
    }
    ok = n == (int)ARRAY_SIZE(opl_regs) && match == ARRAY_SIZE(opl_regs) && monotonic && generation && window &&
         (m.opl[0] & 0xE7) == 0 && (m.opl[1] & 0xE0) == 0xC0 && (m.opl[2] & 0xC0) == 0xC0 && m.opl[3] == 0xE0 &&
         m.opl[4] == 0xA0 && m.opl[5] == 0 && !m.error;
    rec_emit(AP, "DATA", "case=opl ok=%u s1=%02x s2=%02x t1=%02x t2=%02x s3=%02x s4=%02x events=%d match=%u monotonic=%u generation=%u window=%u",
             ok, m.opl[0], m.opl[1], m.opl[2], m.opl[3], m.opl[4], m.opl[5], n, match, monotonic, generation, window);
    if (!ok) { error = -EFAULT; goto cleanup; }
    uint64_t clock = 0;
    dosvm_audio_opl_registers(key, audio_regs, &clock);
    uint32_t t1 = (uint32_t)(a.opl_flag_us[0] - a.opl_start_us[0]), t2 = (uint32_t)(a.opl_flag_us[1] - a.opl_start_us[1]);
    ok = !audio_regs[0x105] && audio_regs[0x104] == 0x3F && audio_regs[0xBD] == 0x3F && audio_regs[0x1C0] == 0xF1 &&
         audio_regs[0xC0] == 0x31 && audio_regs[0x1B0] == 0x31 && audio_regs[0xA0] == 0x98 && audio_regs[0x40] == 0x10 &&
         audio_regs[0xB1] == 0x11 && t1 > 20400 && t1 <= 20480 && t2 > 81600 && t2 <= 81920;
    rec_emit(AP, "DATA", "case=opl_state ok=%u new=%u pairs=%02x rhythm=%02x r1c0=%02x rc0=%02x t1_us=%u t2_us=%u",
             ok, audio_regs[0x105], audio_regs[0x104], audio_regs[0xBD], audio_regs[0x1C0], audio_regs[0xC0], t1, t2);
    if (!ok) { error = -EFAULT; goto cleanup; }

    /* 10. DMA page outside the VM: the transfer fails locally. */
    dosvm_audio_info(key, &b);
    if ((error = audio_command(v, 10)) || (error = audio_stage(v, 10, &m, 10000))) goto cleanup;
    dosvm_audio_info(key, &a);
    ok = a.dma_faults == b.dma_faults + 1 && a.last_error == 1 && a.boundaries == b.boundaries && !m.firqs &&
         m.freset == 0xAA && dosvm_valid(key);
    rec_emit(AP, "DATA", "case=dma_fault ok=%u faults=%u error=%u boundaries=%u irqs=%u reset=%02x alive=%u",
             ok, a.dma_faults - b.dma_faults, a.last_error, a.boundaries - b.boundaries, m.firqs, m.freset, dosvm_valid(key));
    if (!ok) { error = -EFAULT; goto cleanup; }

    /* 11. Payload restores the IVT/PIC and returns to DOS; the shell exits the VM. */
    if ((error = audio_command(v, 11)) || (error = audio_stage(v, 11, &m, 10000))) goto cleanup;
    uint64_t until = deadline_after_ms(10000);
    while (!audio_prompt(v) && !deadline_passed(until)) task_sleep_ms(1);
    if ((error = dosvm_audio_detach(key, token))) goto cleanup;
    token = 0;
    if ((error = audio_type(v, "EXIT"))) goto cleanup;
    struct dosvm_info info;
    until = deadline_after_ms(10000);
    do { dosvm_info(v, &info); if (info.state >= DOSVM_STOPPING) break; task_sleep_ms(1); } while (!deadline_passed(until));
    ok = info.state >= DOSVM_STOPPING && info.reason == DOSVM_EXIT;
    rec_emit(AP, "DATA", "case=exit ok=%u state=%u reason=%u", ok, info.state, info.reason);
    if (!ok) { error = -EFAULT; goto cleanup; }

    /* Locality: the witness VM's devices never saw any of it. */
    struct dosvm_audio_info wi = {0};
    bool witness = dosvm_valid(dosvm_key(w)) && dosvm_audio_info(dosvm_key(w), &wi) &&
                   dosvm_audio_opl_registers(dosvm_key(w), audio_regs, &clock);
    unsigned regs = 0;
    for (unsigned i = 0; i < 512; i++) regs += audio_regs[i] != 0;
    ok = witness && !wi.frames && !wi.events && !wi.boundaries && !wi.causes && !wi.dma_faults && !regs && wi.irq == 7;
    rec_emit(AP, "DATA", "case=locality ok=%u witness=%u frames=%llu events=%llu boundaries=%u causes=%u regs=%u",
             ok, witness, wi.frames, wi.events, wi.boundaries, wi.causes, regs);
    if (!ok) error = -EFAULT;
cleanup:
    if (token && v) dosvm_audio_detach(dosvm_key(v), token);
    if (v) { int e = dosvm_destroy(dosvm_key(v), error != 0); if (!error) error = e; }
    if (w) { int e = dosvm_destroy(dosvm_key(w), true); if (!error) error = e; }
    dosvm_snapshot(&final);
    bool restored = !memcmp(&baseline, &final, sizeof(final));
    rec_emit(AP, "DATA", "case=cleanup restored=%u vms=%u tasks=%u pages=%u handles=%u devices=%u",
             restored, final.vms, final.tasks, final.pages, final.handles, final.devices);
    if (!restored && !error) error = -ENOMEM;
    rec_emit(AP, "END", "status=%s error=%d", error ? "FAIL" : "PASS", error);
    return error;
}
CIUKI_F3_PROBE("vm-audio-state", probe_f3_audio_state);
