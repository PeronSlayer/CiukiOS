/* f4-05 controller (f4:vm-audio): one DOS VM runs VMPLAY.COM through the
 * mailbox at 0000:0600; the evidence is what the kernel mixer rendered for
 * that VM's source (audio/vm_source.c taps and block log) and the final
 * frames published to the AC'97 ring. Digital evidence only: QEMU's WAV
 * capture is not analysed for this probe. Records per
 * docs/design/f4-acceptance.md, section f4-05. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/probe.h>
#include <ciuki/audio.h>
#include <ciuki/audio_vm.h>
#include <ciuki/ac97.h>
#include <ciuki/dosvm.h>
#include <ciuki/dosvm_audio.h>
#include <ciuki/storage.h>
#include <ciuki/timing.h>
#include <ciuki/mm.h>
#include <ciuki/v86.h>
#include <ciuki/task.h>

#define VP "vm-audio"
struct play_mail {
    uint16_t magic, stage, command, irqs, last82, error, version, reset, isrbad;
    uint16_t master, voice, fm, ticks, oplstat, buffer[2], base, irq, dma, hdma;
};
_Static_assert(sizeof(struct play_mail) == 0x28, "VMPLAY mailbox 0600h-0627h");

static int play_mail(struct dosvm *v, struct play_mail *m) { return dosvm_read(v, 0x600, m, sizeof(*m)); }
static int play_command(struct dosvm *v, uint16_t n) { return dosvm_write(v, 0x604, &n, 2); }
static int play_stage(struct dosvm *v, unsigned stage, struct play_mail *m, uint32_t ms)
{
    uint64_t deadline = deadline_after_ms(ms);
    for (;;) {
        struct dosvm_info info; dosvm_info(v, &info);
        if (play_mail(v, m)) return -EFAULT;
        if (m->magic == 0xF405 && m->stage == stage) return 0;
        if (info.state >= DOSVM_STOPPING) return -EIO;
        if (deadline_passed(deadline)) {
            struct dosvm_audio_info a = {0}; dosvm_audio_info(dosvm_key(v), &a);
            rec_emit(VP, "DATA", "case=stage_timeout wanted=%u stage=%u command=%u irqs=%u last82=%02x error=%u isrbad=%u active=%u boundaries=%u services=%u",
                     stage, m->stage, m->command, m->irqs, m->last82, m->error, m->isrbad, a.active, a.boundaries, a.services);
            return -V86_ETIMEDOUT;
        }
        task_sleep_ms(1);
    }
}
static bool play_prompt(struct dosvm *v)
{
    const uint16_t *text = dosvm_text(v, 0);
    for (unsigned i = 0; i < 1997; i++)
        if ((uint8_t)text[i] == 'C' && (uint8_t)text[i + 1] == ':' && (uint8_t)text[i + 2] == '\\')
            for (unsigned j = i + 3; j < 2000 && j < i + 80; j++) if ((uint8_t)text[j] == '>') return true;
    return false;
}
static int play_boot(struct dosvm *v)
{
    int e = dosvm_boot(v);
    if (e) return e;
    uint64_t deadline = deadline_after_ms(30000);
    for (;;) {
        struct dosvm_info info; dosvm_info(v, &info);
        if (info.dos_ready && play_prompt(v)) return 0;
        if (info.state >= DOSVM_STOPPING) return -EIO;
        if (deadline_passed(deadline)) return -V86_ETIMEDOUT;
        task_sleep_ms(1);
    }
}
static int play_type(struct dosvm *v, const char *text)
{
    struct dosvm_key key = dosvm_key(v);
    for (unsigned i = 0; text[i]; i++) if (!dosvm_keyboard(key, (uint8_t)text[i])) return -ENOSPC;
    return dosvm_keyboard(key, 0x1C0D) ? 0 : -ENOSPC;
}
static uint32_t isqrt64(uint64_t x)
{
    uint64_t r = 0, bit = 1ull << 62;
    while (bit > x) bit >>= 2;
    while (bit) {
        if (x >= r + bit) { x -= r + bit; r = (r >> 1) + bit; } else r >>= 1;
        bit >>= 2;
    }
    return (uint32_t)r;
}
static uint32_t rms(const struct audio_vm_tap *t, unsigned c) { return t->frames ? isqrt64(t->sumsq[c] / t->frames) : 0; }
/* Waits until the source has rendered `blocks` logged blocks and its stream
 * has drained, then 150 ms more (the render runs ~56 ms behind the VM). */
static void play_rendered(struct dosvm_key key, uint32_t blocks, struct audio_vm_stats *st)
{
    uint64_t deadline = deadline_after_ms(4000);
    while (!deadline_passed(deadline)) {
        if (!audio_vm_stats(key, st)) break;
        if (st->log_count >= blocks) break;
        task_sleep_ms(5);
    }
    task_sleep_ms(150);
    audio_vm_stats(key, st);
}
static struct audio_vm_stats stats, before;
static struct audio_vm_tap ring_tap;

/* The PCM checks: the logged blocks against the payload's pattern. */
static void block_check(const struct audio_vm_stats *st, bool wide, uint32_t count, uint32_t frames,
                        uint32_t *fresh, uint32_t *repeats, uint32_t *gaps)
{
    *fresh = *repeats = *gaps = 0;
    for (uint32_t i = 0; i < count && i < st->log_count && i < AUDIO_VM_LOG; i++) {
        const struct audio_vm_block_log *b = &st->log[i];
        int16_t l = wide ? (int16_t)(uint16_t)((i << 8) | 0x11) : (int16_t)(((int)(0x20 + i) - 128) * 256);
        int16_t r = wide ? (int16_t)~l : l;
        if (b->first[0] == l && b->first[1] == r && b->frames == frames && !(b->flags & ~DOSVM_BLOCK_COMPLETE)) (*fresh)++;
        if (i && b->first[0] == st->log[i - 1].first[0] && b->first[1] == st->log[i - 1].first[1]) (*repeats)++;
        if (i && b->sequence != st->log[i - 1].sequence + 1) (*gaps)++;
    }
}

static int probe_vm_audio(void)
{
    rec_emit(VP, "BEGIN", "probe_version=1 build_id=%s", CIUKI_BUILD_ID);
    rec_emit(VP, "DATA", "case=memory usable_kib=%u free_pages=%u audio_pages=%u vm_source_bytes=%u",
             pmm_total_usable() * 4, pmm_free_count(), audio.commitment_pages, (unsigned)sizeof(struct audio_vm_stats));
    if (!audio.ready) {
        const char *reason = audio.reason ? audio.reason : "device_absent";
        if (audio.error || audio.quarantined || !strncmp(reason, "activation_failed", sizeof("activation_failed")) ||
            !strncmp(reason, "generation_exhausted", sizeof("generation_exhausted")) ||
            !strncmp(reason, "worker_exhausted", sizeof("worker_exhausted"))) {
            rec_emit(VP, "DATA", "case=backend status=failed reason=%s", reason);
            rec_emit(VP, "END", "status=FAIL reason=%s", reason); return 1;
        }
        rec_emit(VP, "READY", "case=backend status=not_run reason=%s", reason);
        rec_emit(VP, "ERROR", "status=not_run reason=%s", reason); return 0;
    }
    rec_emit(VP, "DATA", "case=identity vendor=%04x device=%04x codec=%08x generation=%u",
             audio.caps.vendor, audio.caps.device, audio.caps.codec_id, audio.caps.generation);
    rec_emit(VP, "DATA", "case=transport accepted_rate=%u period_frames=%u published_periods=%u latency_frames=%u master_left=%u master_right=%u mute=%u",
             AUDIO_RATE, AUDIO_QUANTUM, AUDIO_LEAD, AUDIO_VM_LATENCY, audio.master.left, audio.master.right, audio.master.mute);
    struct dosvm *v = 0; int error = 0;
    struct play_mail m = {0};
    struct dosvm_audio_info a = {0}, b = {0};
    struct dosvm_ledger baseline, final;
    unsigned pages = audio.commitment_pages;
    uint32_t fresh, repeats, gaps;
    bool ok;
    dosvm_snapshot(&baseline);
    const struct dosvm_config config = {.commitment = DOSVM_LOW_CAP, .probe = VP};
    if ((error = storage_enable_write(storage_get(), 2))) goto cleanup;
    if ((error = dosvm_create(&config, &v)) || (error = play_boot(v))) goto cleanup;
    struct dosvm_key key = dosvm_key(v);
    ok = audio_vm_stats(key, &stats) && stats.state != AUDIO_VM_FREE;
    rec_emit(VP, "DATA", "case=attach ok=%u state=%u epoch=%u", ok, stats.state, stats.epoch);
    if (!ok) { error = -ENODEV; goto cleanup; }
    if ((error = play_type(v, "\\VMPLAY.COM"))) goto cleanup; /* the shell starts in C:\APPS */

    /* 1. Detection, CT1745 master/voice/FM at level 31 (F8h). */
    if ((error = play_stage(v, 1, &m, 20000))) goto cleanup;
    ok = m.reset == 0xAA && m.version == 0x0405 && m.master == 0xF8 && m.voice == 0xF8 && m.fm == 0xF8 && !m.error;
    rec_emit(VP, "DATA", "case=detect ok=%u reset=%02x version=%04x master=%02x voice=%02x fm=%02x base=%03x irq=%u dma=%u hdma=%u error=%u",
             ok, m.reset, m.version, m.master, m.voice, m.fm, m.base, m.irq, m.dma, m.hdma, m.error);
    if (!ok) { error = -EFAULT; goto cleanup; }

    /* 2. The f4-05 regression: 32 fresh 64-unit blocks at 8000 Hz, rendered in sequence. */
    audio_vm_tap_reset(key); audio_vm_stats(key, &before); dosvm_audio_info(key, &b);
    if ((error = play_command(v, 2)) || (error = play_stage(v, 2, &m, 10000))) goto cleanup;
    play_rendered(key, 32, &stats); dosvm_audio_info(key, &a); audio_vm_ring(&ring_tap);
    block_check(&stats, false, 32, 64, &fresh, &repeats, &gaps);
    ok = m.irqs == 32 && !m.isrbad && stats.log_count == 32 && fresh == 32 && !repeats && !gaps &&
         a.services - b.services == 32 && a.boundaries - b.boundaries == 32 && stats.pcm.frames == 12288 &&
         stats.pcm.peak[0] == 24576 && stats.pcm.peak[1] == 24576 && stats.late_frames == before.late_frames;
    rec_emit(VP, "DATA", "case=pcm8 ok=%u irqs=%u blocks=%u fresh=%u repeats=%u seq_gaps=%u services=%u boundaries=%u frames=%llu peak_l=%u peak_r=%u",
             ok, m.irqs, stats.log_count, fresh, repeats, gaps, a.services - b.services, a.boundaries - b.boundaries,
             stats.pcm.frames, stats.pcm.peak[0], stats.pcm.peak[1]);
    rec_emit(VP, "DATA", "case=pcm8_digest digest=%08x ring_frames=%llu ring_digest=%08x late=%llu starved=%llu pcm_frames=%llu",
             stats.pcm.digest, ring_tap.frames, ring_tap.digest, stats.late_frames - before.late_frames,
             stats.starved - before.starved, stats.pcm_frames - before.pcm_frames);
    if (!ok) { error = -EFAULT; goto cleanup; }

    /* 3. 16-bit signed stereo at 12000 Hz, voice at -12 dB: 8 blocks of 64 frames. */
    audio_vm_tap_reset(key); audio_vm_stats(key, &before);
    if ((error = play_command(v, 3)) || (error = play_stage(v, 3, &m, 10000))) goto cleanup;
    play_rendered(key, 8, &stats); audio_vm_ring(&ring_tap);
    block_check(&stats, true, 8, 64, &fresh, &repeats, &gaps);
    /* -12 dB = Q16 16462: 0711h -> 454, NOT 0711h (-1810) -> -455 (arithmetic shift). */
    ok = m.irqs == 8 && !m.isrbad && stats.log_count == 8 && fresh == 8 && !repeats && !gaps &&
         stats.pcm.frames == 2048 && stats.pcm.peak[0] == 454 && stats.pcm.peak[1] == 455 &&
         stats.late_frames == before.late_frames;
    rec_emit(VP, "DATA", "case=pcm16 ok=%u irqs=%u blocks=%u fresh=%u repeats=%u seq_gaps=%u frames=%llu peak_l=%u peak_r=%u digest=%08x ring_digest=%08x",
             ok, m.irqs, stats.log_count, fresh, repeats, gaps, stats.pcm.frames, stats.pcm.peak[0], stats.pcm.peak[1],
             stats.pcm.digest, ring_tap.digest);
    if (!ok) { error = -EFAULT; goto cleanup; }

    /* 4-5. OPL2 440 Hz on both outputs; OPL3 880 Hz on output A (left) only. */
    for (unsigned stage = 4; stage <= 5; stage++) {
        audio_vm_tap_reset(key); audio_vm_stats(key, &before);
        if ((error = play_command(v, (uint16_t)stage)) || (error = play_stage(v, stage, &m, 10000))) goto cleanup;
        task_sleep_ms(250);
        audio_vm_stats(key, &stats); audio_vm_ring(&ring_tap);
        const struct audio_vm_tap *t = &stats.opl;
        uint32_t hz10 = t->frames ? (uint32_t)((uint64_t)t->crossings[0] * 480000u / t->frames) : 0;
        uint32_t want = stage == 4 ? 4400 : 8800;
        ok = t->frames >= 24000 && hz10 + 40 >= want && hz10 <= want + 40 && !stats.pcm.frames &&
             stats.opl_head_frames == AUDIO_VM_HEAD && stats.opl_events > before.opl_events &&
             (stage == 4 ? t->sumsq[0] == t->sumsq[1] && t->crossings[1] == t->crossings[0] : !t->sumsq[1] && !t->peak[1]);
        rec_emit(VP, "DATA", "case=%s ok=%u frames=%llu hz10=%u crossings_l=%u crossings_r=%u rms_l=%u rms_r=%u peak_l=%u peak_r=%u ticks=%u",
                 stage == 4 ? "opl2" : "opl3", ok, t->frames, hz10, t->crossings[0], t->crossings[1],
                 rms(t, 0), rms(t, 1), t->peak[0], t->peak[1], m.ticks);
        rec_emit(VP, "DATA", "case=%s_digest head_digest=%08x head_frames=%u events=%u late_events=%u ring_frames=%llu",
                 stage == 4 ? "opl2" : "opl3", stats.opl_head_digest, stats.opl_head_frames,
                 stats.opl_events - before.opl_events, stats.late_events - before.late_events, ring_tap.frames);
        if (!ok) { error = -EFAULT; goto cleanup; }
    }

    /* 6. Restore and exit; pacing and transport evidence over the whole run. */
    if ((error = play_command(v, 6)) || (error = play_stage(v, 6, &m, 10000))) goto cleanup;
    uint64_t until = deadline_after_ms(10000);
    while (!play_prompt(v) && !deadline_passed(until)) task_sleep_ms(1);
    audio_vm_stats(key, &stats);
    rec_emit(VP, "DATA", "case=pacing lead=%d lead_min=%d lead_max=%d lag_min=%d holds=%u skips=%u reanchors=%u late=%llu starved=%llu",
             stats.lead, stats.lead_min, stats.lead_max, stats.lag_min, stats.holds, stats.skips, stats.reanchors,
             stats.late_frames, stats.starved);
    rec_emit(VP, "DATA", "case=accounting rendered=%llu streams=%u segments=%u blocks=%u late_blocks=%u stale=%u overrun=%u unsupported=%u revoked=%u resyncs=%u",
             stats.rendered, stats.streams, stats.segments, stats.blocks, stats.late_blocks, stats.stale_blocks,
             stats.overrun_blocks, stats.unsupported_blocks, stats.revoked, stats.opl_resyncs);
    if ((error = play_type(v, "EXIT"))) goto cleanup;
    struct dosvm_info info;
    until = deadline_after_ms(10000);
    do { dosvm_info(v, &info); if (info.state >= DOSVM_STOPPING) break; task_sleep_ms(1); } while (!deadline_passed(until));
    ok = info.state >= DOSVM_STOPPING && info.reason == DOSVM_EXIT;
    rec_emit(VP, "DATA", "case=exit ok=%u state=%u reason=%u", ok, info.state, info.reason);
    if (!ok) error = -EFAULT;
cleanup:
    {
        struct dosvm_key key0 = v ? dosvm_key(v) : (struct dosvm_key){0, 0};
        if (v) { int e = dosvm_destroy(key0, error != 0); if (!error) error = e; }
        bool detached = !v || (audio_vm_stats(key0, &stats) && stats.state == AUDIO_VM_FREE);
        dosvm_snapshot(&final);
        bool restored = !memcmp(&baseline, &final, sizeof(final));
        rec_emit(VP, "DATA", "case=clock fifo=%llu halts=%llu deadlines=%llu max_service_us=%u max_irq_us=%u quarantine=%u",
                 audio.fifo, audio.halts, audio.deadlines, audio.max_service_us, audio.max_irq_us, audio.quarantined);
        rec_emit(VP, "DATA", "case=cleanup restored=%u detached=%u vms=%u pages=%u devices=%u before_pages=%u after_pages=%u",
                 restored, detached, final.vms, final.pages, final.devices, pages, audio.commitment_pages);
        if ((!restored || !detached || pages != audio.commitment_pages) && !error) error = -ENOMEM;
    }
    rec_emit(VP, "END", "status=%s error=%d", error ? "FAIL" : "PASS", error);
    return error ? 1 : 0;
}
CIUKI_F4_PROBE("vm-audio", probe_vm_audio);
