/* Device control, cancellable copied writes, refill and durable settings.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/audio.h>
#include <ciuki/ac97.h>
#include <ciuki/audio_backend.h>
#include <ciuki/files.h>
#include <ciuki/signal.h>
#include <ciuki/timing.h>
#include <ciuki/thinkpad_ec.h>
#include <ciuki/cpustat.h>
#include "../../../sdk/sysroot-overlay/include/ciuki/audio_preference.h"
#include <ciuki/task.h>
#pragma weak sched_audio_producer_stats /* f1-101: task.c, absent in audio-only hosts */
static struct vfs *settings_vfs;
/* fx-26: poll readiness of /dev/mixer (Windows 98 mixer drivers notify a
 * mixerOpen(CALLBACK_WINDOW) client with MM_MIXM_CONTROL_CHANGE when a
 * control changes; here POLLIN tells the tray to read the master now). The
 * fingerprint covers the whole record, so every writer of audio.master is
 * seen; master_changed wakes poll() at once on the paths that change it. */
extern void net_input_changed(void) __attribute__((weak));
static uint32_t master_fingerprint(const struct ciuki_mixer_master *m)
{
    const uint8_t *p = (const uint8_t *)m; uint32_t h = 2166136261u;
    for (unsigned i = 0; i < sizeof(*m); i++) h = (h ^ p[i]) * 16777619u;
    return h;
}
static void master_changed(void) { if (net_input_changed) net_input_changed(); }
int audio_mixer_poll_events(struct file_description *d)
{
    struct ciuki_mixer_master m = audio.master;
    return !d->observed_valid || d->observed != master_fingerprint(&m) ? POLLIN : 0;
}
static void master_observed(struct file_description *d, const struct ciuki_mixer_master *m)
{ d->observed = master_fingerprint(m); d->observed_valid = true; }
/* fx-5: service nests control (power quiesce, worker error path, refill
 * failure). sync.h orders mutexes by increasing (level, address), so both
 * live in one struct declared in acquisition order. As two statics, the T23
 * trial kernel 9e6e8721 placed control (c06a89b0) below service (c06adb20)
 * and the S5 quiesce panicked `mutex: lock order`. Both are an outer level:
 * they nest the backend's register lock (audio_dma, another object) and the
 * ThinkPad EC lock, whose addresses only the link order put above them. */
static struct {
    struct kmutex service; /* serialize write-side startup and worker */
    struct kmutex control;
} audio_lock = {
    .service = KMUTEX_CLASS("audio.service", KMUTEX_LEVEL_OUTER),
    .control = KMUTEX_CLASS("audio.control", KMUTEX_LEVEL_OUTER),
};
_Static_assert(__builtin_offsetof(__typeof__(audio_lock), service) <
               __builtin_offsetof(__typeof__(audio_lock), control), "audio lock order: service, then control");
static void audio_locks_init(void)
{
    kmutex_init_level(&audio_lock.service, "audio.service", KMUTEX_LEVEL_OUTER);
    kmutex_init_level(&audio_lock.control, "audio.control", KMUTEX_LEVEL_OUTER);
}
static bool power_stopped;
void audio_ec_attach(void)
{
    if (!__atomic_load_n(&audio.initialized, __ATOMIC_ACQUIRE) ||
        !kmutex_lock_until(&audio_lock.control,deadline_after_ms(2000))) return;
    if (!power_stopped) { thinkpad_ec_init(); thinkpad_ec_start(); }
    kmutex_unlock(&audio_lock.control);
}
static struct vfs_table settings_table; /* bounded, never on a kernel stack */
struct audio_cfg { uint32_t magic, sequence, checksum, reserved; struct ciuki_mixer_master master; };
_Static_assert(sizeof(struct audio_cfg) == 48, "durable audio record");
static struct audio_cfg saved[2];
static uint32_t saved_sequence;
static unsigned saved_slot;
/* fx-9b: a volume-key (EC) change applied to the master and not yet in
 * AUDIO.CFG; the next tray save or audio_settings_flush writes it. */
static bool master_pending;
static struct audio_frame quantum[AUDIO_QUANTUM];
/* f4-05: an active DOS VM source keeps the transport running (weak: the
 * native-only host harness does not link audio/vm_source.c). */
extern bool audio_vm_active(void) __attribute__((weak));
extern bool audio_vm_playing(void) __attribute__((weak));
extern unsigned audio_vm_playing_count(void) __attribute__((weak));
extern bool audio_vm_attached(void) __attribute__((weak));
extern void audio_vm_published(void) __attribute__((weak));
extern void audio_vm_reanchor(void) __attribute__((weak));
extern void serial_buffered(bool) __attribute__((weak));
extern bool display_fullscreen_active(void) __attribute__((weak));

static uint32_t checksum(const struct audio_cfg *record)
{
    const uint8_t *p = (const uint8_t *)record;
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < sizeof(*record); i++) h = (h ^ (i >= 8 && i < 12 ? 0 : p[i])) * 16777619u;
    return h;
}
static bool cfg_valid(const struct audio_cfg *r)
{
    return r->magic == 0x31445541 && r->sequence && !r->reserved &&
        r->checksum == checksum(r) && r->master.version == 1 && r->master.size == 32 &&
        r->master.left <= 100 && r->master.right <= 100 && r->master.mute <= 1 &&
        !r->master.reserved && !r->master.save_state && !r->master.save_error;
}
void audio_settings_load(struct vfs *v)
{
    settings_vfs = v;
    saved_sequence = 0; saved_slot = 0; master_pending = false;
    audio.master = (struct ciuki_mixer_master){ .version=1, .size=32,
        .left=CIUKI_MASTER_DEFAULT, .right=CIUKI_MASTER_DEFAULT };
    audio.settings_reason = "missing";
    if (!v || !v->volumes[2]) { audio.settings_reason = "storage_absent"; return; }
    int err = vfs_table_init(v, &settings_table, 2);
    if (err) { audio.settings_reason = "read_failed"; return; }
    int fd = vfs_open(&settings_table, "C:\\SYSTEM\\AUDIO.CFG", VFS_READ, VFS_DENY_NONE, 0);
    size_t done = 0;
    memset(saved, 0, sizeof(saved));
    if (fd >= 0) {
        err = vfs_read(&settings_table, fd, saved, sizeof(saved), &done);
        vfs_close(&settings_table, fd);
        audio.settings_reason = err ? "read_failed" : "invalid";
        for (unsigned i = 0; !err && i < 2; i++) if (done >= (i + 1) * sizeof(saved[0]) && cfg_valid(&saved[i]) &&
            (!saved_sequence || saved[i].sequence > saved_sequence)) {
            saved_sequence = saved[i].sequence; saved_slot = i;
            audio.master = saved[i].master; audio.master.save_state = CIUKI_MIXER_SAVED;
            audio.settings_reason = "saved";
        }
    } else if (fd != -FS_ENOENT) audio.settings_reason = "read_failed";
    vfs_table_destroy(&settings_table);
    if (audio_registry_load(v, &audio.master)) audio.settings_reason = "registry";
}
static int save_master(void)
{
    if (!settings_vfs || !settings_vfs->volumes[2]) return -ENODEV;
    if (saved_sequence == UINT32_MAX) return -EOVERFLOW;
    int err = vfs_table_init(settings_vfs, &settings_table, 2);
    if (err) return px_error(err);
    int fd = vfs_open(&settings_table, "C:\\SYSTEM\\AUDIO.CFG", VFS_READ | VFS_WRITE | VFS_CREATE, VFS_DENY_ALL, 0);
    unsigned slot = saved_sequence ? saved_slot ^ 1u : 0;
    struct audio_cfg record = { .magic=0x31445541, .sequence=saved_sequence + 1, .master=audio.master };
    record.master.save_state = 0; record.master.save_error = 0;
    record.checksum = checksum(&record);
    uint64_t position;
    size_t done = 0;
    if (fd < 0) err = fd;
    else {
        err = vfs_seek(&settings_table, fd, slot * sizeof(record), VFS_SEEK_SET, &position);
        if (!err) err = vfs_write(&settings_table, fd, &record, sizeof(record), &done);
        if (!err && done != sizeof(record)) err = -FS_EIO;
        if (!err) err = vfs_commit(&settings_table, fd);
        int closed = vfs_close(&settings_table, fd);
        if (!err) err = closed;
    }
    vfs_table_destroy(&settings_table);
    if (!err) { saved[slot] = record; saved_slot = slot; saved_sequence = record.sequence; }
    return px_error(err);
}
int audio_master_set(struct ciuki_mixer_master *m)
{
    if (m->left > 100 || m->right > 100 || m->mute > 1 || m->save_state || m->save_error) return -EINVAL;
    kmutex_lock(&audio_lock.control);
    /* The EC wrapper also clears delegation on failure. Offline changes
     * still apply and persist; the poller resynchronises from audio.master. */
    (void)thinkpad_ec_master_set(m);
    int err = audio.ready ? audio_backend_master(m) : 0;
    if (err) { audio_fail(-EIO); kmutex_unlock(&audio_lock.control); return -EIO; }
    audio.master = *m;
    err = save_master();
    if (!err) master_pending = false;
    audio.master.save_state = err ? CIUKI_MIXER_UNSAVED : CIUKI_MIXER_SAVED;
    audio.master.save_error = err;
    *m = audio.master;
    kmutex_unlock(&audio_lock.control);
    master_changed();
    return 0;
}
static void master_refresh(bool requested)
{
    kmutex_lock(&audio_lock.control);
    struct ciuki_mixer_master m = audio.master, before = audio.master;
    bool hardware = audio.master_in_hardware;
    int changed = requested ? thinkpad_ec_master_refresh(&m) : thinkpad_ec_master_poll(&m);
    if (changed > 0 || hardware != audio.master_in_hardware) {
        int err = audio.ready ? audio_backend_master(&m) : 0;
        if (err) audio_fail(-EIO);
        else {
            if (changed > 0) {
                /* fx-9b: the key path only applies the change. f4-29 saved
                 * AUDIO.CFG here, on the device-class EC worker under
                 * audio.control, and its vfs_commit is a device-wide cache
                 * barrier (every dirty block of the system disk, write
                 * admission frozen, FLUSH CACHE): 13 s busy at device
                 * priority after each key press in DOOM full screen (T23
                 * 423b803a). SAVED also hid the change from the desktop,
                 * whose normal-priority settings writer persists an
                 * applied-unsaved master in the authoritative registry
                 * preference (f4-18), so key changes were lost at the next
                 * boot. Now applied-unsaved: the desktop writes the registry
                 * once the keys are quiet; AUDIO.CFG follows at the next tray
                 * save or at shutdown (audio_settings_flush). */
                m.save_state = CIUKI_MIXER_UNSAVED; m.save_error = 0;
                master_pending = true;
            }
            audio.master = m;
        }
    }
    bool changed_master = memcmp(&before, &audio.master, sizeof(before)) != 0;
    kmutex_unlock(&audio_lock.control);
    if (changed_master) master_changed(); /* fx-26: the tray reads it on its next turn */
}
/* fx-9b: orderly shutdown (power.c, before the storage flush), in the
 * caller's context (the desktop's call 73, normal priority): a key-applied
 * master reaches AUDIO.CFG once. Never called on the device band. */
int audio_settings_flush(void)
{
    if (!__atomic_load_n(&master_pending, __ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&audio.initialized, __ATOMIC_ACQUIRE)) return 0;
    if (!kmutex_lock_until(&audio_lock.control, deadline_after_ms(2000))) return -EBUSY;
    int err = 0;
    if (master_pending) {
        err = save_master();
        if (!err) {
            master_pending = false;
            audio.master.save_state = CIUKI_MIXER_SAVED; audio.master.save_error = 0;
        }
    }
    kmutex_unlock(&audio_lock.control);
    if (!err) master_changed();
    return err;
}
void audio_master_refresh(void) { master_refresh(false); }
void audio_master_refresh_requested(void) { master_refresh(true); }
static uint64_t idle_since; /* tick of the first idle service, 0 while a source is active */
static bool first_playback;
static bool playback_logged;
static uint64_t playback_start, playback_next;
static uint64_t non_silent_end; /* output position after the last nonzero published quantum */
static bool quantum_pending, quantum_mixed; /* retry a refused publication without mixing twice */
static bool cursor_retry; /* service-lock owned; worker must block at most 1 ms */
static uint64_t transport_next;
static unsigned native_source_count(void)
{
    unsigned count = 0;
    for (unsigned i = 0; i < AUDIO_STREAMS; i++) {
        struct audio_stream *s = audio.streams[i];
        if (s && s->started && s->queued) count++;
    }
    return count;
}
static unsigned source_count(void)
{
    return native_source_count() + (audio_vm_playing_count ? audio_vm_playing_count() :
        audio_vm_playing && audio_vm_playing() ? 1 : 0);
}
static void transport_log(const char *reason, const struct audio_transport *b)
{
    char name[16] = "unknown"; uint32_t stretch = 0;
    if (cpu_stretch_snapshot) cpu_stretch_snapshot(name, &stretch);
    name[15] = 0;
    /* Protected disk class, queued UART share: no polled serial or sink wait
     * in the refill path. One record fits the 254-byte payload bound. */
    klog_periodic("[audio] transport %s published=%llu retired=%llu halts=%llu underruns=%llu gap_us=%u lead_periods=%u sources=%u rate=%u task=%s stretch_ms=%u",
        reason, (unsigned long long)b->published, (unsigned long long)b->retired,
        (unsigned long long)b->dma_halts, (unsigned long long)audio.underruns,
        audio.max_service_gap_us, audio.lead_periods, source_count(), audio_output_rate(), name, stretch);
    transport_next = g_ticks + 5000;
}
void audio_diagnostics(struct ciuki_audio_diagnostics *out)
{
    uint32_t f = irq_save();
    struct audio_transport b; audio_backend_snapshot(&b);
    const struct pci_func *pci=audio.backend_pci.vendor ? &audio.backend_pci : &ac97.pci;
    *out = (struct ciuki_audio_diagnostics){ .version=1, .size=sizeof(*out),
        .codec_id=b.codec_id, .controller=(uint32_t)pci->vendor << 16 | pci->device,
        .flags=(audio.ready ? CIUKI_AUDIO_DIAG_READY : 0) | (b.running ? CIUKI_AUDIO_DIAG_RUNNING : 0) |
               (b.quarantine ? CIUKI_AUDIO_DIAG_QUARANTINE : 0) |
               (audio_vm_playing && audio_vm_playing() ? CIUKI_AUDIO_DIAG_VM_ACTIVE : 0),
        .irq_line=b.irq, .civ=b.slot, .picb=b.remaining,
        .fetched_frames=b.fetched, .irqs=b.irqs, .last_error=audio.error ? audio.error : audio.init_error,
        .power=b.power, .master=b.master, .pcm=b.pcm_volume, .rate=b.rate };
    irq_restore(f);
}
static void playback_log(bool failed)
{
    if (!first_playback || playback_logged || !(g_boot.flags & CBI_F_VERBOSE)) return;
    /* A polled UART record can exhaust the DMA lead on physical machines.
     * Keep the counters, but print only after playback stops or fails. */
    if (audio.running && !failed) return;
    uint64_t now = g_ticks;
    if (!failed && now < playback_next) return;
    struct ciuki_audio_diagnostics d;
    audio_diagnostics(&d);
    bool done = failed || now - playback_start >= 1000;
    klog("[audio] play=%s civ=%u picb=%u irqs=%llu fetched=%llu elapsed_ms=%llu",
         failed ? "failed" : done ? "complete" : "running", d.civ, d.picb,
         (unsigned long long)d.irqs, (unsigned long long)d.fetched_frames,
         (unsigned long long)(now - playback_start));
    playback_next = now + 250; playback_logged = done;
}
static void audio_flush_logs(void)
{
    if (audio.running) {
        if (g_ticks >= transport_next) {
            struct audio_transport b; audio_backend_snapshot(&b);
            transport_log("running", &b);
        }
        return;
    }
    playback_log(false);
}
void audio_wake(void)
{
    audio.pending = true;
    if (read_eflags() & 0x200) kwait_wake_all(&audio.changed);
    else kwait_wake_irq(&audio.changed, false);
}
void audio_fail(int error)
{
    audio.error = error; audio.ready = false;
    quantum_pending = false;
    if (serial_buffered) serial_buffered(false);
    audio_wake();
    for (unsigned i = 0; i < AUDIO_STREAMS; i++) if (audio.streams[i]) {
        audio_stream_lead(audio.streams[i]); /* f1-101: no producer boost on a failed device */
        kwait_wake_all(&audio.streams[i]->changed);
    }
}
int audio_power_quiesce(void)
{
    if (!audio.initialized || power_stopped) return 0;
    uint64_t end = deadline_after_ms(2000);
    if (!kmutex_lock_until(&audio_lock.service, end)) return -EBUSY;
    if (!kmutex_lock_until(&audio_lock.control, end)) { kmutex_unlock(&audio_lock.service); return -EBUSY; }
    int stopped = thinkpad_ec_stop();
    if (stopped) { kmutex_unlock(&audio_lock.control); kmutex_unlock(&audio_lock.service); return stopped; }
    audio_fail(-ENODEV);
    audio_backend_shutdown();
    struct audio_transport b; audio_backend_snapshot(&b);
    int e = audio.quarantined || b.quarantine || b.running ? -EIO : 0;
    if (!e) power_stopped = true;
    kmutex_unlock(&audio_lock.control); kmutex_unlock(&audio_lock.service); return e;
}
/* The service start moved later by the firmware time proven since `firmware`. */
static uint64_t service_start(uint64_t start, uint64_t firmware)
{
    uint64_t now = ktime_cycles(), smm = cpu_firmware_cycles ? cpu_firmware_cycles() - firmware : 0;
    return smm < now - start ? start + smm : now;
}
static void audio_service_locked(void)
{
    if (!audio.ready) return;
    struct audio_transport b; audio_backend_snapshot(&b);
    uint64_t start = ktime_cycles();
    /* f2-96: SMM time inside the service (a T23 SMI lasts 2.1 ms, proven
     * by cpustat) is not refill work: the 1 ms yield, the 2 ms deadline and
     * the service maximum measure the work alone. */
    uint64_t firmware = cpu_firmware_cycles ? cpu_firmware_cycles() : 0;
    uint32_t service_gap = 0;
    if (audio.running && audio.last_service_cycles && g_tsc_per_ms) {
        uint64_t gap = (start - audio.last_service_cycles) * 1000 / g_tsc_per_ms;
        service_gap = gap > UINT32_MAX ? UINT32_MAX : (uint32_t)gap;
        if (gap > audio.max_service_gap_us) audio.max_service_gap_us = gap > UINT32_MAX ? UINT32_MAX : (uint32_t)gap;
    }
    audio.last_service_cycles = start;
    bool active = native_source_count() != 0;
    if (audio_vm_active && audio_vm_active()) active = true;
    bool attached = audio_vm_attached && audio_vm_attached();
    unsigned nominal = attached ? AUDIO_VM_LEAD : AUDIO_LEAD;
    if (active && !idle_since && service_gap > nominal * AUDIO_QUANTUM * 1000000u / audio_output_rate()) {
        unsigned gap = service_gap > AUDIO_GAP_BUDGET_US ? service_gap : AUDIO_GAP_BUDGET_US;
        unsigned grown = audio_gap_lead(gap, audio_output_rate());
        if (grown > audio.adaptive_lead) {
            audio.adaptive_lead = grown;
            audio.lead_periods = grown;
            transport_log("late-grow", &b);
        }
    }
    /* An inactive VM can still have nonzero PCM queued. Source state alone
     * does not prove that the DMA lead is silence. */
    bool tail = audio.running && (!active || idle_since);
    audio_backend_idle(tail && b.frame_base + b.retired * AUDIO_QUANTUM >= non_silent_end);
    bool control = false; /* hardware stop has its own bounded deadline */
    bool recovering = false;
    struct audio_transport stop_snapshot;
    bool stop_failed = false;
    const char *error_op = "progress";
    int err = audio_backend_progress();
    cursor_retry = err == -EAGAIN;
    /* Uncertain progress leaves the committed retirement unchanged. It
     * cannot forbid filling already free slots: publish independently
     * checks DMA ownership. Never invent retirement to meet the lead. */
    if (cursor_retry) err = 0;
    audio_backend_snapshot(&b);
    if (b.fifo_errors > audio.fifo && !err) {
        audio.underruns += b.fifo_errors - audio.fifo;
        transport_log("fifo-underrun", &b);
    }
    /* Even the current period must be known silence: DCH proves fetch,
     * not that nonzero data in the codec FIFO has reached the output. */
    bool silent = b.frame_base + b.retired * AUDIO_QUANTUM >= non_silent_end;
    if (err && tail && silent && b.tail_drained) {
        /* Idle tail: an underrun carries no client data. Stop the transport
         * normally instead of failing the backend; a source restarts it. */
        transport_log("idle-halt", &b);
        error_op = "stop";
        stop_snapshot = b;
        err = audio_backend_stop(); stop_failed = err != 0;
        if (!err) {
            audio.running = false;
            audio_backend_rebase(audio.output - (quantum_pending ? AUDIO_QUANTUM : 0));
            idle_since = 0; audio_backend_idle(false);
        }
        control = true;
    } else if (!err && !cursor_retry && tail && silent && b.published - b.retired <= 1) {
        /* Drained to the period the DMA plays: ac97_publish never rewrites the
         * CIV/PIV slots, so the queue cannot be extended and the refusal
         * failed the backend (QEMU campaign 13, image 22320ccc: the AUDIO_LEAD
         * tail kept while a DOS VM is attached drained between VMPLAY stages
         * 4 and 5). Inside the tail that period is silence: stop as above. */
        transport_log("idle-stop", &b);
        error_op = "stop";
        stop_snapshot = b;
        err = audio_backend_stop(); stop_failed = err != 0;
        if (!err) {
            audio.running = false;
            audio_backend_rebase(audio.output - (quantum_pending ? AUDIO_QUANTUM : 0));
            idle_since = 0; audio_backend_idle(false);
        }
        control = true;
    } else if (err && b.tail_drained) {
        /* CIV/LVI/PICB proved that every published period was fetched.
         * Pause must still prove RUN clear and a stable halted cursor before
         * reset or any payload reuse. Prime a new silence lead, preserving
         * an unpublished quantum if publication raced the final fetch. */
        audio.underruns++;
        transport_log("underrun", &b);
        transport_log("recover-stop", &b);
        error_op = "recover_stop";
        stop_snapshot = b;
        err = audio_backend_stop(); stop_failed = err != 0;
        if (!err) {
            audio.running = false;
            audio_backend_rebase(audio.output - (quantum_pending ? AUDIO_QUANTUM : 0));
            if (audio_vm_reanchor) audio_vm_reanchor();
            idle_since = 0; audio_backend_idle(false); recovering = true;
        }
        control = true;
    } else if (err && b.ready && !b.quarantine && b.running && b.slot < 32 &&
               b.prefetch < 32 && b.lvi < 32 && b.status != 0xffff &&
               b.published >= b.retired && b.published - b.retired <= AUDIO_MAX_LEAD) {
        /* A readable runtime error may be reset only after the normal halt
         * proof. Never infer retirement from a damaged/ambiguous cursor. */
        audio.underruns++;
        transport_log("error-halt", &b);
        error_op = "error_stop"; stop_snapshot = b;
        err = audio_backend_stop(); stop_failed = err != 0;
        if (!err) {
            audio.running = false;
            audio_backend_rebase(audio.output - (quantum_pending ? AUDIO_QUANTUM : 0));
            if (audio_vm_reanchor) audio_vm_reanchor();
            idle_since = 0; audio_backend_idle(false); recovering = active || quantum_pending;
        }
        control = true;
    } else if (!err && !cursor_retry && active && silent && b.running &&
               b.published - b.retired > (audio.adaptive_lead ? audio.adaptive_lead : nominal)) {
        /* Cut only proven silent tail on arrival. A paused VM is no reason
         * to make a native writer wait behind the 134 ms silence cushion. */
        transport_log("source-stop", &b);
        error_op = "source_stop"; stop_snapshot = b;
        err = audio_backend_stop(); stop_failed = err != 0;
        if (!err) {
            audio.running = false; audio_backend_rebase(audio.output);
            idle_since = 0; audio_backend_idle(false);
        }
        control = true;
    }
    audio_backend_snapshot(&b);
    audio.fetched = b.fetched; audio.played_output = b.frame_base + b.generation_fetched;
    audio.fifo = b.fifo_errors;
    audio.halts = b.dma_halts; audio.deadlines = b.deadline_misses;
    /* Publish until the lead is restored: one period per service let any
     * service delay erode the lead permanently (QEMU evidence 2026-10-11:
     * published=770 retired=769 → CELV/DCH halt). */
    /* Restore the lead in bounded turns. Deep idle silence is safe even
     * with a paused VM: arrival cuts it only after silence/stop proof. */
    bool tail_start = !active && audio.running && !idle_since;
    if (tail_start) { idle_since = g_ticks; control = true; }
    unsigned limit, published_now = 0;
    unsigned lead = audio_refill_lead(active, tail_start, attached, &limit);
    if (!active) lead = audio_gap_lead(AUDIO_GAP_BUDGET_US, audio_output_rate());
    else if (audio.adaptive_lead > lead) lead = audio.adaptive_lead;
    audio.lead_periods = lead;
    if (active || tail_start) limit = lead;
    /* A recovered inactive source still needs a complete silence lead in
     * this service; otherwise the worker leaves a stopped one-period ring. */
    if (!audio.running) {
        if (!recovering && !active && !quantum_pending && !b.published) return;
        if (!active) lead = nominal;
        audio.lead_periods = lead; limit = lead;
    }
    while (!err && audio.ready && !audio.error && (active || audio.running || recovering || quantum_pending || b.published) &&
           b.published - b.retired < lead && published_now++ < limit) {
        if (!quantum_pending) {
            quantum_mixed = audio.running && active;
            /* No live source means silence, even with a paused VM attached.
             * Do not run an idle VM's resampler/OPL for the deep cushion;
             * its next actual render re-anchors to the VM clock. */
            if (!audio.running || !active) {
                memset(quantum, 0, sizeof(quantum)); audio.output += AUDIO_QUANTUM;
            } else {
                uint64_t mixed_start = ktime_cycles();
                audio_mix(quantum, AUDIO_QUANTUM);
                /* Conservative measured fallback on slower CPUs. A 256-frame
                 * 48 kHz period has a 106 us budget at 2% CPU. VM/preemption
                 * time can also trigger fallback, but never a quality upgrade
                 * during a pressured session. Host -O2 cost is in the audit. */
                if (!audio.linear_resampling && audio_output_rate() == AUDIO_RATE &&
                    g_tsc_per_ms && ktime_elapsed_us(mixed_start, 106)) {
                    for (unsigned i = 0; i < AUDIO_STREAMS; i++)
                        if (audio.streams[i] && audio.streams[i]->rate == 44100) {
                            audio.linear_resampling = true; transport_log("resample-linear", &b); break;
                        }
                }
            }
            quantum_pending = true;
        }
        bool mixed = quantum_mixed;
        error_op = "publish";
        err = audio_backend_publish(quantum);
        if (err == -EAGAIN) { cursor_retry = true; err = 0; break; } /* retain quantum */
        if (!err) quantum_pending = false;
        if (!err && mixed && audio_vm_published) audio_vm_published();
        audio_backend_snapshot(&b);
        if (!err && mixed && !first_playback) {
            first_playback = true;
            playback_start = g_ticks; playback_next = g_ticks;
        }
        if (!err) for (unsigned i = 0; i < AUDIO_QUANTUM; i++) {
            if (quantum[i].left || quantum[i].right) {
                non_silent_end = audio.output; break;
            }
        }
        if (!err && !audio.running && b.published >= lead) {
            if (serial_buffered) serial_buffered(true);
            error_op = "start";
            err = audio_backend_start(); audio.running = !err; audio_backend_snapshot(&b);
            if (!err) { audio.restarts++; transport_log(recovering ? "recover-start" : "start", &b); }
        }
        /* Reserve the second millisecond for one conversion/publication and
         * bookkeeping. If one quantum itself overruns, count it below. */
        if (ktime_elapsed_us(service_start(start, firmware), AUDIO_SERVICE_US / 2)) { audio_wake(); break; }
    }
    if (!err && !cursor_retry && !active && audio.running && !quantum_pending) {
        /* Client drains have already waited their fetched/tail allowance.
         * Silence still in the queue is not client data. Keep publishing
         * silence for an idle tail before stopping: the QEMU AC97 backend
         * dropped the last ~0.3 s of fetched audio when the channel was reset
         * at once (evidence 2026-10-11), and codecs settle without clicks. */
        if (idle_since && g_ticks - idle_since >= AUDIO_IDLE_TAIL_MS && silent) {
            error_op = "stop";
            stop_snapshot = b;
            transport_log("idle-stop", &b);
            err = audio_backend_stop(); stop_failed = err != 0;
            if (!err) {
                audio.running = false;
                audio_backend_rebase(audio.output); idle_since = 0; audio_backend_idle(false);
                audio.adaptive_lead = 0;
            }
            control = true;
        }
    } else if (active) {
        idle_since = 0; /* a new source cancels the idle tail */
    }
    start = service_start(start, firmware);
    if (!control && ktime_elapsed_us(start, AUDIO_SERVICE_US)) { audio.deadlines++; audio_backend_deadline(); }
    uint64_t elapsed = (g_tsc_per_ms ? (ktime_cycles() - start) * 1000 / g_tsc_per_ms : 0);
    /* Transport control (stop, the idle-tail burst) is accounted apart from refill work. */
    uint32_t *peak = control ? &audio.max_control_us : &audio.max_service_us;
    if (elapsed > *peak) *peak = elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
    if (!audio.running && serial_buffered) serial_buffered(false);
    if (err) {
        /* Pause/reset can change both hardware indices and generation
         * counters. Keep the triggering ring snapshot if recovery failed. */
        if (stop_failed) b = stop_snapshot;
        else audio_backend_snapshot(&b);
        transport_log("failed", &b);
        /* At most 227 bytes even with maximum-width counters: klog's
         * 256-byte buffer must retain the complete snapshot in one line. */
        klog("[audio] backend failure err=%d op=%s civ=%u lvi=%u piv=%u picb=%u sr=%04x published=%llu retired=%llu running=%u active=%u gap_us=%u elapsed_ms=%llu",
             err, error_op, b.slot, b.lvi, b.prefetch, b.remaining, b.status,
             (unsigned long long)b.published, (unsigned long long)b.retired, b.running, active,
             audio.max_service_gap_us,
             (unsigned long long)(first_playback ? g_ticks - playback_start : 0));
        playback_logged = first_playback;
        audio_fail(-EIO); kmutex_lock(&audio_lock.control); audio_backend_shutdown(); kmutex_unlock(&audio_lock.control);
    }
    for (unsigned i = 0; i < AUDIO_STREAMS; i++) if (audio.streams[i]) kwait_wake_all(&audio.streams[i]->changed);
}
void audio_service(void)
{
    kmutex_lock(&audio_lock.service);
    if (ledger_critical) ledger_critical(true);
    audio_service_locked();
    if (ledger_critical) ledger_critical(false);
    if (ledger_audio_active) ledger_audio_active(audio.running);
    kmutex_unlock(&audio_lock.service);
}
static void audio_write_service(void)
{
    kmutex_lock(&audio_lock.service);
    /* Observe real DMA, including a halt/inhibit which preceded the worker's
     * next turn. The logical RUN flag alone cannot establish hardware RUN. */
    if (audio.ready && !audio.error && (!audio.running || idle_since || audio_backend_progress())) audio_service_locked();
    kmutex_unlock(&audio_lock.service);
}
static bool refill_pending(void *arg) { (void)arg; return audio.pending || audio.error; }
static void audio_worker_wait(void)
{
    struct audio_transport b; audio_backend_snapshot(&b);
    bool fullscreen=display_fullscreen_active && display_fullscreen_active();
    unsigned wait_ms=cursor_retry ? 1 : audio.running ? (fullscreen ? 2 : 4) :
        b.published && b.published<audio.lead_periods ? 1 : 50;
    /* A ready condition returns without sleeping. Retain pending work and
     * force one real block between services. Aggregate device credit remains
     * scheduler-owned; annotations preserve the existing active wake bounds. */
    if (audio.pending && !audio.error) {
        task_device_deadline(g_current, audio.running ? deadline_after_ms(1) : 0);
        task_sleep_ms(1);
    }
    uint64_t deadline = deadline_after_ms(wait_ms);
    task_device_deadline(g_current, audio.running ? deadline : 0);
    kwait_wait_until(&audio.changed, refill_pending, 0, deadline);
}
static void audio_worker(void *arg)
{
    (void)arg;
    for (;;) {
        if (audio.error) {
            kmutex_lock(&audio_lock.service);
            kmutex_lock(&audio_lock.control);
            struct audio_transport b; audio_backend_snapshot(&b);
            if (b.ready) audio_backend_shutdown();
            kmutex_unlock(&audio_lock.control);
            kmutex_unlock(&audio_lock.service);
            task_sleep_ms(1000); continue;
        }
        audio.pending = false;
        audio_service();
        audio_flush_logs();
        /* A P_DEVICE thread must block between services (f1-18 rule): a 1 ms
         * poll starved ring-3 tasks (F0 preempt, image 7ab10f64). The IRQ and
         * the stream writers wake `changed`; the deadline is only a safety net:
         * short while the first periods are published, under half a period
         * while running, long when idle. */
        audio_worker_wait();
    }
}
/* One 48-sample sine period: exactly 1 kHz at 48 kHz, with short edge ramps.
 * Scratch stays off the kernel stack. The ordinary copied stream/worker owns
 * DMA; the startup task sleeps while that worker refills. */
static int16_t boot_sample(unsigned frame)
{
    static const int16_t quarter[13] = {0,1069,2120,3135,4096,4987,5793,6499,7094,7568,7913,8122,8192};
    unsigned phase = frame % 48, index = phase % 12;
    int sample = quarter[(phase / 12) & 1 ? 12 - index : index];
    if (phase >= 24) sample = -sample;
    unsigned edge = frame < AUDIO_RATE - 1 - frame ? frame : AUDIO_RATE - 1 - frame;
    if (edge < 96) sample = sample * (int)edge / 96;
    return sample;
}
static struct audio_frame boot_pcm[AUDIO_QUANTUM];
int audio_check_duration(uint32_t seconds, uint32_t *frames)
{
    if (frames) *frames = 0;
    if (!seconds || seconds > 10) return -EINVAL;
    if (!audio.ready) return -ENODEV;
    /* The boot tone uses the same configured master as every UI sound. */
    int err = 0;
    struct audio_stream *s = 0;
    err = audio_stream_open(&s);
    uint64_t limit = deadline_after_ms(seconds * 1000 + 2000);
    unsigned total = seconds * AUDIO_RATE;
    uint64_t underruns = audio.underruns;
    struct audio_transport before; audio_backend_snapshot(&before);
    unsigned lead_min = AUDIO_MAX_LEAD, lead_max = 0;
    unsigned sent = 0;
    if (s) { s->draining = true; s->drain_goal = total; }
    while (!err && sent < total) {
        if (deadline_passed(limit)) { err = -ETIMEDOUT; break; }
        unsigned count = total - sent;
        if (count > AUDIO_QUANTUM) count = AUDIO_QUANTUM;
        for (unsigned i = 0; i < count; i++) {
            int16_t sample = boot_sample((sent + i) % AUDIO_RATE);
            boot_pcm[i] = (struct audio_frame){sample, sample};
        }
        int wrote = audio_stream_enqueue(s, boot_pcm, count * sizeof(*boot_pcm));
        if (wrote > 0) sent += (unsigned)wrote / sizeof(*boot_pcm);
        else if (wrote != -EAGAIN) err = wrote;
        if (audio.lead_periods < lead_min) lead_min = audio.lead_periods;
        if (audio.lead_periods > lead_max) lead_max = audio.lead_periods;
        if (!err && sent < total) task_sleep_ms(1);
    }
    while (!err && (!s->drain_latched || audio.played_output < s->drain_output)) {
        if (audio.error) err = audio.error;
        else if (deadline_passed(limit)) err = -ETIMEDOUT;
        else task_sleep_ms(1);
    }
    if (!err) task_sleep_ms(audio.caps.drain_ms);
    if (s) audio_stream_close(s);
    playback_log(err != 0);
    if (seconds == 1) klog("[audio] boot_tone=1khz frames=%u result=%d restore=0", sent, err);
    else {
        struct audio_transport after; audio_backend_snapshot(&after);
        uint64_t irqs = after.irqs - before.irqs;
        uint64_t elapsed = (seconds * 1000 + 2000) - (limit - g_ticks);
        uint64_t missed = audio.underruns - underruns;
        unsigned expected = seconds * audio_output_rate() / AUDIO_QUANTUM;
        if (!err && (missed || irqs < expected/2 || irqs > expected*2+2 ||
                     after.fifo_errors != before.fifo_errors)) err = -EIO;
        klog("[audio] transport check10s frames=%u underruns=%llu irqs=%llu irq_mean_us=%u lead_min=%u lead_max=%u error=%d",
            sent, (unsigned long long)missed, (unsigned long long)irqs,
            irqs ? (unsigned)(elapsed * 1000 / irqs) : 0, lead_min, lead_max, err);
    }
    if (frames) *frames = sent;
    return err;
}
int audio_check_tone(uint32_t *frames) { return audio_check_duration(1, frames); }
static void audio_boot_tone(void)
{
    if (audio_boot_tone_enabled(g_boot.flags) && !(g_boot.flags & CBI_F_HW_CHECK))
        (void)audio_check_tone(0);
}
static void audio_init_log(int err)
{
    if (audio.backend_name && strncmp(audio.backend_name,"ac97",sizeof("ac97"))) {
        klog("[audio] backend=%s error=%d reason=%s",audio.backend_name,err,audio.reason); return;
    }
    klog("[audio] controller=%04x:%04x codec=%08x ready=%08x power=%04x master=%04x pcm=%04x rate=%u irq=%u error=%d reason=%s",
         ac97.pci.vendor, ac97.pci.device, ac97.codec_id, ac97.global_status,
         ac97.power, ac97.master, ac97.pcm_volume, ac97.rate, ac97.pci.irq_line, err, audio.reason);
    if (err && ac97.failure.stage) {
        const struct ac97_failure *f = &ac97.failure;
        klog("[audio] failure stage=%s ext2a=%04x rate2c=%04x pisr=%04x posr=%04x mcsr=%04x pocr=%02x civ=%u lvi=%u piv=%u picb=%u",
             f->stage, f->ext_control, f->rate, f->status[0], f->status[1], f->status[2],
             f->control, f->cursor.civ, f->lvi, f->cursor.piv, f->cursor.remaining);
    }
}
void audio_init(struct vfs *v)
{
    if (audio.initialized) return;
    kwait_init(&audio.changed); audio_locks_init();
    /* fx-3: audio_ec_attach (acpi-power task) runs once the inventory has
     * settled and may race this init; both serialize on audio_lock.control,
     * and the locks exist before initialized is published. */
    __atomic_store_n(&audio.initialized, true, __ATOMIC_RELEASE);
    audio_settings_load(v);
    kmutex_lock(&audio_lock.control); thinkpad_ec_init(); kmutex_unlock(&audio_lock.control);
    if (g_boot.flags & CBI_F_SAFE_MODE) { audio.reason = "safe_mode"; return; }
    int err = audio_backend_init();
    audio.init_error = err;
    audio_init_log(err);
    if (err) return;
    struct task *worker = task_create_kernel("audio-refill", audio_worker, 0, P_DEVICE);
    if (!worker) {
        audio.reason = "worker_exhausted"; audio.error = -ENOMEM; audio_backend_shutdown(); return;
    }
    /* f1-90: the DMA refill has its own bounded device budget; other device
     * tasks' consumption never demotes it (R3). */
    task_set_audio_class(worker);
    task_start(worker); /* lead fix (QEMU 2026-10-11): a created kernel task runs only after task_start */
    audio.commitment_pages += KSTACK_SIZE / PAGE_SIZE;
    audio.ready = true;
    kmutex_lock(&audio_lock.control); thinkpad_ec_start(); kmutex_unlock(&audio_lock.control);
    audio_boot_tone();
}
int audio_device_open(struct file_description *d, uint32_t flags)
{
    if (d->node->kind == PX_AUDIO) {
        if ((flags&O_ACCMODE)==O_RDONLY) return -EACCES;
        return audio_stream_open(&d->audio);
    }
    (void)flags;
    return 0;
}
/* A decoder explicitly names its own thread, not every thread sharing the
 * process fd table. One 64 KiB source-read budget expires after 20 ms and
 * is valid only while the live writable stream has refill demand. Stream
 * generations prevent close/reset/fd reuse from reviving a stale token. */
bool audio_refill_reader(struct process *p,uint32_t bytes)
{
    struct task *t=g_current;
    if (!t || !bytes || bytes>t->audio_refill_bytes ||
        deadline_passed(t->audio_refill_expiry) || !audio.ready || audio.error) return false;
    for (unsigned fd=0;fd<CIUKI_OPEN_MAX;fd++) {
        struct file_description *d=file_fd(p,(int32_t)fd);
        if (!d || d->node->kind!=PX_AUDIO || (d->flags&O_ACCMODE)==O_RDONLY) continue;
        struct audio_stream *s=d->audio;
        if (s && s->generation==t->audio_refill_generation && s->started && !s->draining &&
            s->queued<=AUDIO_RING*3/4) {
            t->audio_refill_bytes-=bytes; return true;
        }
    }
    return false;
}
void audio_device_close(struct file_description *d) { if (d->audio) { audio_stream_close(d->audio); d->audio = 0; } }
int audio_mixer_delegate(struct process *supervisor, struct process *trusted)
{
    if (supervisor != proc_supervisor() || !trusted || !supervisor->cwd || !trusted->fds) return -EACCES;
    int fd = file_open(trusted, "/dev/mixer", O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0) return fd;
    struct file_description *d = file_fd(trusted, fd);
    d->mixer_control = true; d->flags = O_RDWR; d->object.inheritable = false;
    return fd;
}
struct audio_operation {
    struct process *process;
    struct file_description *d;
    struct ua_pin pin;
    bool pinned;
    _Alignas(uint32_t) uint8_t copied[1024];
};
static void audio_operation_free(void *arg)
{
    struct audio_operation *o = arg;
    if (o->pinned) ua_unpin(o->process->memory, &o->pin);
    o->d->object.release(&o->d->object); kfree(o);
}
static bool interrupted(void)
{
    struct proc_thread *t = proc_thread_for(g_current);
    return t && (t->process->state == PROC_STOPPING || proc_signal_caught(t));
}
struct stream_wait { struct audio_stream *s; uint32_t generation; bool drain; };
static bool stream_ready(void *arg)
{
    struct stream_wait *w = arg;
    return audio.error || interrupted() || w->s->generation != w->generation ||
        (w->drain ? w->s->drain_latched && audio.played_output >= w->s->drain_output : w->s->queued < AUDIO_RING);
}
static int stream_wait(struct audio_stream *s, uint32_t generation, bool drain)
{
    struct stream_wait w = { s, generation, drain };
    kwait_wait_until(&s->changed, stream_ready, &w, deadline_after_ms(1000));
    if (audio.error || !audio.ready) return -EIO;
    return interrupted() || s->generation != generation ? -EINTR : 0;
}
static struct audio_operation *operation_begin(struct file_description *d)
{
    struct audio_operation *o = kzalloc(sizeof(*o));
    if (!o) return 0;
    o->process = proc_current(); o->d = d; d->object.retain(&d->object);
    struct proc_thread *t = proc_thread_for(g_current);
    if (t) { t->operation = o; t->cleanup = audio_operation_free; }
    return o;
}
static void operation_end(struct audio_operation *o)
{
    struct proc_thread *t = proc_thread_for(g_current);
    if (t) { t->operation = 0; t->cleanup = 0; }
    audio_operation_free(o);
}
int audio_write_user(struct file_description *d, uint32_t address, uint32_t bytes)
{
    struct audio_stream *s = d->audio;
    unsigned frame = s->channels * (s->format == CIUKI_AUDIO_U8 ? 1 : 2);
    if (bytes > CIUKI_IO_MAX || bytes % frame) return -EINVAL;
    if (!bytes) return 0;
    struct audio_operation *o = operation_begin(d);
    if (!o) return -ENOMEM;
    /* f1-101: the writing thread is this stream's producer (writer role). */
    audio_stream_producer(s, g_current, SCHED_PRODUCER_WRITER);
    int err = ua_pin(o->process->memory, &o->pin, address, bytes, false);
    o->pinned = !err;
    unsigned done = 0; uint32_t generation = s->generation;
    while (!err && done < bytes) {
        if (audio.error || !audio.ready) { err = -EIO; break; }
        if (interrupted() || s->generation != generation) { err = -EINTR; break; }
        if (s->queued == AUDIO_RING) {
            if (d->flags & O_NONBLOCK) { err = -EAGAIN; break; }
            err = stream_wait(s, generation, false); continue;
        }
        unsigned take = bytes - done;
        if (take > sizeof(o->copied)) take = sizeof(o->copied);
        if (take > (AUDIO_RING - s->queued) * frame) take = (AUDIO_RING - s->queued) * frame;
        err = copy_from_user(o->copied, address + done, take);
        if (err) break;
        int wrote = audio_stream_enqueue(s, o->copied, take);
        if (wrote < 0) { err = wrote; break; }
        done += wrote;
        /* RUN is established before a successful native write returns,
         * including writes made after the VM drained/stopped the transport.
         * The worker and this kick share the service mutex. */
        {
            audio_write_service();
            uint64_t limit = deadline_after_ms(20);
            while (!audio.running && !audio.error && !deadline_passed(limit)) {
                task_sleep_ms(1); audio_service();
            }
            if (!audio.running || audio.error) { err = audio.error ? audio.error : -ETIMEDOUT; done = 0; break; }
        }
        if (d->flags & O_NONBLOCK) break;
    }
    operation_end(o);
    return done ? (int)done : err;
}
static uint32_t record_size(uint32_t op)
{
    switch (op) {
    case AUDIO_GET_CAPS: return sizeof(struct ciuki_audio_caps);
    case AUDIO_SET_FORMAT: return sizeof(struct ciuki_audio_format);
    case AUDIO_GET_STATUS: return sizeof(struct ciuki_audio_status);
    case AUDIO_SET_GAIN: return sizeof(struct ciuki_audio_gain);
    case AUDIO_DRAIN: case AUDIO_RESET: case AUDIO_REFILL_READ: return sizeof(struct ciuki_audio_command);
    case AUDIO_GET_PRODUCER: return sizeof(struct ciuki_audio_producer);
    case MIXER_GET_MASTER: case MIXER_SET_MASTER: case MIXER_REFRESH_MASTER: return sizeof(struct ciuki_mixer_master);
    case MIXER_GET_DIAGNOSTICS: return sizeof(struct ciuki_audio_diagnostics);
    default: return 0;
    }
}
int audio_devctl(struct process *p, int32_t fd, uint32_t op, uint32_t address, uint32_t bytes)
{
    if (!file_fd_object(p, fd)) return -EBADF;
    struct file_description *d = file_fd(p, fd);
    if (!d || (d->node->kind != PX_AUDIO && d->node->kind != PX_MIXER)) return -ENOTTY;
    unsigned size = record_size(op);
    if (!size || (d->node->kind == PX_AUDIO ? op >= MIXER_GET_MASTER : op < MIXER_GET_MASTER)) return -ENOTTY;
    if (bytes != size) return -EINVAL;
    struct audio_operation *o = operation_begin(d);
    if (!o) return -ENOMEM;
    uint32_t *record = (uint32_t *)o->copied;
    int err = copy_from_user(record, address, size);
    if (!err && record[0] != 1) err = -EOPNOTSUPP;
    if (!err && record[1] != size) err = -EINVAL;
    bool observation = op == AUDIO_GET_STATUS || op == AUDIO_GET_PRODUCER; /* output-only records */
    unsigned reserved = size / 4 - (op >= MIXER_GET_MASTER ? 1 : observation ? 0 : 2);
    for (unsigned i = reserved; !err && i < size / 4; i++) if (record[i]) err = -EINVAL;
    if (!err) { err = ua_pin(p->memory, &o->pin, address, size, true); o->pinned = !err; }
    struct audio_stream *s = d->audio;
    if (!err && interrupted()) err = -EINTR;
    if (!err && s && !observation && (audio.error || !audio.ready)) err = -EIO;
    if (!err) switch (op) {
    case AUDIO_GET_CAPS: memcpy(record, &audio.caps, size); break;
    case AUDIO_REFILL_READ:
        if ((d->flags&O_ACCMODE)==O_RDONLY) { err=-EACCES; break; }
        if (!g_current) { err=-EINVAL; break; }
        g_current->audio_refill_bytes=0;
        /* f1-101: the declared reader is this stream's producer (reader role);
         * the mixer's lead, never this call, decides when it is boosted. */
        audio_stream_producer(s, g_current, SCHED_PRODUCER_READER);
        if (s && s->started && !s->draining && s->queued<=AUDIO_RING*3/4) {
            g_current->audio_refill_generation=s->generation;
            g_current->audio_refill_expiry=deadline_after_ms(20);
            g_current->audio_refill_bytes=65536;
        }
        break;
    case AUDIO_SET_FORMAT: err = audio_stream_format(s, (struct ciuki_audio_format *)record); break;
    case AUDIO_SET_GAIN:
        if (record[2] > 100 || record[3] > 100) err = -EINVAL;
        else { s->gain[0] = record[2]; s->gain[1] = record[3]; }
        break;
    case AUDIO_GET_STATUS: {
        struct audio_transport b; audio_backend_snapshot(&b);
        struct ciuki_audio_status status = { .version=1, .size=112, .queued_frames=s->queued,
            .state=audio.error ? 2 : s->started ? 1 : 0, .output_rate=audio_output_rate(),
            .generation=audio.caps.generation, .accepted_frames=s->accepted, .consumed_frames=s->consumed,
            .output_frames=audio.output, .fetched_frames=audio.fetched, .source_starvation=s->starvation,
            .fifo_errors=audio.fifo, .dma_halts=audio.halts, .deadline_misses=audio.deadlines,
            .last_error=audio.error, .stream_generation=s->generation,
            .max_service_us=audio.max_service_us, .max_irq_us=audio.max_irq_us,
            .transport_frames=(uint32_t)(b.published * AUDIO_QUANTUM - b.generation_fetched) };
        uint64_t queued = ((uint64_t)s->queued * audio_output_rate() + s->rate - 1) / s->rate;
        /* Once the source queue is empty, later mixed sources/idle silence
         * are not this stream's latency. Retain its own last contribution. */
        uint64_t latency = queued ? queued + status.transport_frames :
            s->last_output > audio.played_output ? s->last_output - audio.played_output : 0;
        if (s->accepted) latency += (uint64_t)audio.caps.drain_ms * audio_output_rate() / 1000;
        status.latency_frames = latency > UINT32_MAX ? UINT32_MAX : (uint32_t)latency;
        memcpy(record, &status, size); break;
    }
    case AUDIO_GET_PRODUCER: {
        struct sched_producer_stats stats = {0};
        if (sched_audio_producer_stats) sched_audio_producer_stats(s, &stats);
        struct ciuki_audio_producer producer = { .version=1, .size=sizeof(producer),
            .flags=(stats.flags & SCHED_PRODUCER_BEHIND ? CIUKI_AUDIO_PRODUCER_BEHIND : 0) |
                   (stats.flags & SCHED_PRODUCER_BOOSTED ? CIUKI_AUDIO_PRODUCER_BOOSTED : 0) |
                   (stats.flags & SCHED_PRODUCER_OVER ? CIUKI_AUDIO_PRODUCER_OVER_BUDGET : 0),
            .boosts=stats.boosts, .over_budget=stats.over_budget, .producers=stats.producers,
            .boost_us=stats.cycles_per_ms ? stats.boost_cycles * 1000 / stats.cycles_per_ms : 0 };
        memcpy(record, &producer, size); break;
    }
    case AUDIO_RESET: audio_stream_reset(s); break;
    case AUDIO_DRAIN: {
        if (s->draining) { err = -EBUSY; break; }
        uint32_t generation = s->generation;
        s->draining = true; s->drain_goal = s->accepted;
        s->drain_latched = !s->queued; s->drain_output = s->last_output;
        audio_stream_lead(s); /* f1-101: a draining stream needs no producer */
        audio_wake();
        while (!err && (!s->drain_latched || audio.played_output < s->drain_output)) err = stream_wait(s, generation, true);
        uint64_t limit = deadline_after_ms(audio.caps.drain_ms);
        while (!err && !deadline_passed(limit)) {
            if (interrupted() || s->generation != generation) err = -EINTR;
            else if (audio.error) err = -EIO;
            else task_sleep_ms(1);
        }
        if (s->generation == generation) {
            s->draining = false; if (!err && !s->queued) s->started = false;
            audio_stream_lead(s);
        }
        break;
    }
    case MIXER_GET_MASTER: memcpy(record, &audio.master, size); master_observed(d, (struct ciuki_mixer_master *)record); break;
    case MIXER_REFRESH_MASTER:
        /* f1-100: an explicit user action (volume popup) asks the EC worker
         * for one hardware read; no periodic EC traffic. The reply is the
         * current master; the refreshed one follows in later reads. */
        if (!d->mixer_control) { err = -EACCES; break; }
        thinkpad_ec_refresh_request();
        memcpy(record, &audio.master, size); master_observed(d, (struct ciuki_mixer_master *)record); break;
    case MIXER_GET_DIAGNOSTICS: audio_diagnostics((struct ciuki_audio_diagnostics *)record); break;
    case MIXER_SET_MASTER:
        err = !d->mixer_control ? -EACCES : audio_master_set((struct ciuki_mixer_master *)record);
        if (!err) master_observed(d, (struct ciuki_mixer_master *)record);
        break;
    }
    if (!err) err = copy_to_user(address, record, size);
    operation_end(o); return err;
}
