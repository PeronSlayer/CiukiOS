/* Native audio evidence controller: production processes, fds and transport.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/audio.h>
#include <ciuki/ac97.h>
#include <ciuki/process.h>
#include <ciuki/probe.h>
#include <ciuki/timing.h>
__asm__(".pushsection .rodata.audio_payload,\"a\"\n.balign 4\n"
        ".global f4_payload_start,f4_payload_end\nf4_payload_start:\n"
        ".incbin \"" CIUKI_AUDIO_PAYLOAD_BIN "\"\nf4_payload_end:\n.popsection\n");
extern const uint8_t f4_payload_start[], f4_payload_end[];
#define RESULT (CIUKI_IMAGE_BASE + CIUKI_PAGE_SIZE)
struct tone_result { uint32_t mode, ready, start, done, error, fd, frames, count; struct ciuki_audio_status status; };
static int tone_read(void *cookie, uint32_t offset, void *out, uint32_t bytes)
{
    (void)cookie;
    uint32_t size = f4_payload_end - f4_payload_start;
    if (offset > size || bytes > size - offset) return -EIO;
    memcpy(out, f4_payload_start + offset, bytes); return 0;
}
static int tone_spawn(unsigned channel, struct process **out)
{
    struct proc_strings *strings = proc_strings_new();
    if (!strings) return -ENOMEM;
    int err = proc_strings_add(strings, "audio-tone", 11, false);
    struct ciuki_file file = { .bytes=f4_payload_end - f4_payload_start, .read=tone_read };
    if (!err) err = proc_spawn_file(proc_supervisor(), &file, strings, 0, 0, 0, 0, out);
    proc_strings_free(strings);
    if (!err) err = ua_write((*out)->memory, RESULT, &channel, sizeof(channel));
    return err;
}
static int probe_audio_native(void)
{
    const char *name = "audio-native";
    rec_emit(name, "BEGIN", "probe_version=1 build_id=%s", CIUKI_BUILD_ID);
    unsigned reserved = 0;
    for (unsigned i = 0; i < g_boot.e820_count; i++) {
        const struct ciuki_e820 *e = &g_boot.e820[i];
        if (e->type != CBI_E820_RAM) continue;
        uint64_t first = (e->base + PAGE_SIZE - 1) / PAGE_SIZE;
        uint64_t last = (e->base + e->length) / PAGE_SIZE;
        if (last > PMM_MAX_PAGES) last = PMM_MAX_PAGES;
        for (uint64_t page = first; page < last; page++) if (pmm_is_reserved((uint32_t)page * PAGE_SIZE)) reserved++;
    }
    rec_emit(name, "DATA", "case=memory usable_kib=%u free_pages=%u reserved_pages=%u audio_pages=%u",
        pmm_total_usable() * 4, pmm_free_count(), reserved, audio.commitment_pages);
    if (!audio.ready) {
        const char *reason=audio.reason ? audio.reason : "device_absent";
        if (audio.error || audio.quarantined || !strncmp(reason,"activation_failed",sizeof("activation_failed")) ||
            !strncmp(reason,"generation_exhausted",sizeof("generation_exhausted")) ||
            !strncmp(reason,"worker_exhausted",sizeof("worker_exhausted"))) {
            rec_emit(name,"DATA","case=backend status=failed reason=%s",reason);
            rec_emit(name,"END","status=FAIL reason=%s",reason);return 1;
        }
        rec_emit(name, "READY", "case=backend status=not_run reason=%s", audio.reason ? audio.reason : "device_absent");
        rec_emit(name, "ERROR", "status=not_run reason=%s", audio.reason ? audio.reason : "device_absent"); return 0;
    }
    rec_emit(name, "DATA", "case=identity vendor=%04x device=%04x subsystem=%08x revision=%u codec=%08x generation=%u irq=%u",
        audio.caps.vendor, audio.caps.device, audio.caps.subsystem, audio.caps.revision, audio.caps.codec_id, audio.caps.generation, ac97.pci.irq_line);
    rec_emit(name,"DATA","case=codec_paths pcm_unity=%04x nam_bytes=%u nabm_bytes=%u",
        ac97.pcm_unity,ac97.nam_bytes,ac97.nabm_bytes);
    rec_emit(name, "DATA", "case=transport accepted_rate=%u period_frames=%u published_periods=%u lead_us=42667 drain_ms=%u master_left=%u master_right=%u mute=%u",
        AUDIO_RATE, AUDIO_QUANTUM, AUDIO_LEAD, audio.caps.drain_ms, audio.master.left, audio.master.right, audio.master.mute);
    struct process *p[2] = {0}; struct tone_result r[2] = {0};
    unsigned baseline = audio.commitment_pages;
    int err = tone_spawn(0, &p[0]); if (!err) err = tone_spawn(1, &p[1]);
    uint64_t deadline = deadline_after_ms(10000);
    bool ready = false;
    while (!err && !deadline_passed(deadline)) {
        for (unsigned i = 0; i < 2; i++) {
            if (p[i]->state != PROC_LIVE || ua_read(p[i]->memory, &r[i], RESULT, sizeof(r[i]))) { err = -EIO; break; }
            if (r[i].error) err = -EIO;
        }
        if (r[0].ready && r[1].ready) { ready = true; break; }
        task_sleep_ms(1);
    }
    if (!ready) err = -EIO;
    uint32_t start = 1;
    for (unsigned i = 0; !err && i < 2; i++) err = ua_write(p[i]->memory, RESULT + 8, &start, sizeof(start));
    deadline = deadline_after_ms(15000);
    bool done = false;
    while (!err && !deadline_passed(deadline)) {
        for (unsigned i = 0; i < 2; i++) {
            if (p[i]->state != PROC_LIVE || ua_read(p[i]->memory, &r[i], RESULT, sizeof(r[i])) || r[i].error) { err = -EIO; break; }
        }
        if (r[0].done && r[1].done) { done = true; break; }
        task_sleep_ms(1);
    }
    if (!done) err = -EIO;
    for (unsigned i = 0; i < 2; i++) {
        struct ciuki_audio_status *s = &r[i].status;
        rec_emit(name, "DATA", "case=source index=%u pid=%u accepted=%llu consumed=%llu queued=%u starvation=%llu error=%u",
            i, p[i] ? p[i]->pid : 0, s->accepted_frames, s->consumed_frames, s->queued_frames, s->source_starvation, r[i].error);
        if (s->accepted_frames != 192000 || s->consumed_frames != 192000 || s->queued_frames || s->source_starvation) err = -EIO;
    }
    for (unsigned i = 0; i < 2; i++) if (p[i]) proc_stop(p[i], 0, 0);
    for (unsigned tries = 0; tries < 1000; tries++) {
        proc_collect();
        bool live = false;
        for (unsigned i = 0; i < 2; i++) if (p[i] && p[i]->state != PROC_ZOMBIE) live = true;
        if (!live) break;
        task_sleep_ms(1);
    }
    for (unsigned i = 0; i < 2; i++) if (p[i]) {
        if (p[i]->state == PROC_ZOMBIE) proc_reap(proc_supervisor(), p[i]); else err = -EIO;
    }
    rec_emit(name, "DATA", "case=clock output=%llu fetched=%llu fifo=%llu halts=%llu deadlines=%llu quarantine=%u",
        audio.output, audio.fetched, audio.fifo, audio.halts, audio.deadlines, audio.quarantined);
    rec_emit(name, "DATA", "case=service max_service_us=%u max_irq_us=%u max_service_gap_us=%u",
        audio.max_service_us, audio.max_irq_us, audio.max_service_gap_us);
    rec_emit(name, "DATA", "case=cleanup before_pages=%u after_pages=%u", baseline, audio.commitment_pages);
    if (audio.fifo || audio.halts || audio.deadlines || audio.quarantined || baseline != audio.commitment_pages) err = -EIO;
    rec_emit(name, "END", "status=%s", err ? "FAIL" : "PASS");
    return err ? 1 : 0;
}
CIUKI_F4_PROBE("audio-native", probe_audio_native);
