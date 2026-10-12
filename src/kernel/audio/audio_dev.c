/* Device control, cancellable copied writes, refill and durable settings.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/audio.h>
#include <ciuki/ac97.h>
#include <ciuki/files.h>
#include <ciuki/signal.h>
#include <ciuki/timing.h>
static struct vfs *settings_vfs;
static struct kmutex control_lock;
static struct vfs_table settings_table; /* bounded, never on a kernel stack */
struct audio_cfg { uint32_t magic, sequence, checksum, reserved; struct ciuki_mixer_master master; };
_Static_assert(sizeof(struct audio_cfg) == 48, "durable audio record");
static struct audio_cfg saved[2];
static uint32_t saved_sequence;
static unsigned saved_slot;
static struct audio_frame quantum[AUDIO_QUANTUM];
/* f4-05: an active DOS VM source keeps the transport running (weak: the
 * native-only host harness does not link audio/vm_source.c). */
extern bool audio_vm_active(void) __attribute__((weak));
extern bool audio_vm_attached(void) __attribute__((weak));

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
    saved_sequence = 0; saved_slot = 0;
    audio.master = (struct ciuki_mixer_master){ .version=1, .size=32, .left=50, .right=50 };
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
    kmutex_lock(&control_lock);
    int err = audio.ready ? ac97_set_mute(&ac97, m->mute != 0) : 0;
    if (err) { audio_fail(-EIO); kmutex_unlock(&control_lock); return -EIO; }
    audio.master = *m;
    err = save_master();
    audio.master.save_state = err ? CIUKI_MIXER_UNSAVED : CIUKI_MIXER_SAVED;
    audio.master.save_error = err;
    *m = audio.master;
    kmutex_unlock(&control_lock);
    return 0;
}
static uint64_t idle_since; /* tick of the first idle service, 0 while a source is active */
void audio_wake(void)
{
    audio.pending = true; kwait_wake_all(&audio.changed);
}
void audio_fail(int error)
{
    audio.error = error; audio.ready = false;
    audio_wake();
    for (unsigned i = 0; i < AUDIO_STREAMS; i++) if (audio.streams[i]) kwait_wake_all(&audio.streams[i]->changed);
}
void audio_service(void)
{
    if (!audio.ready) return;
    uint64_t start = ktime_cycles();
    if (audio.running && audio.last_service_cycles && g_tsc_per_ms) {
        uint64_t gap = (start - audio.last_service_cycles) * 1000 / g_tsc_per_ms;
        if (gap > audio.max_service_gap_us) audio.max_service_gap_us = gap > UINT32_MAX ? UINT32_MAX : (uint32_t)gap;
    }
    audio.last_service_cycles = start;
    bool active = false;
    for (unsigned i = 0; i < AUDIO_STREAMS; i++) if (audio.streams[i] && audio.streams[i]->started) active = true;
    if (audio_vm_active && audio_vm_active()) active = true;
    /* The queue holds only tail silence while the idle tail runs, also in the
     * service where a source arrives (idle_since is cleared at its end). */
    bool tail = audio.running && (!active || idle_since);
    ac97.idle_tail = tail;
    bool control = false; /* a transport stop has its own deadline, not the 1 ms service budget */
    int err = ac97_progress(&ac97);
    if (err && tail) {
        /* Idle tail: an underrun carries no client data. Stop the transport
         * normally instead of failing the backend; a source restarts it. */
        klog("[audio] idle-tail transport stop err=%d published=%llu retired=%llu halts=%llu tail_ms=%llu",
             err, (unsigned long long)ac97.published, (unsigned long long)ac97.retired, (unsigned long long)ac97.dma_halts, (unsigned long long)(idle_since ? g_ticks - idle_since : 0));
        err = ac97_stop(&ac97); audio.running = false; ac97.frame_base = audio.output; idle_since = 0; ac97.idle_tail = false; control = true;
    } else if (!err && tail && ac97.published - ac97.retired <= 1) {
        /* Drained to the period the DMA plays: ac97_publish never rewrites the
         * CIV/PIV slots, so the queue cannot be extended and the refusal
         * failed the backend (QEMU campaign 13, image 22320ccc: the AUDIO_LEAD
         * tail kept while a DOS VM is attached drained between VMPLAY stages
         * 4 and 5). Inside the tail that period is silence: stop as above. */
        klog("[audio] idle-tail transport stop drained published=%llu retired=%llu tail_ms=%llu",
             (unsigned long long)ac97.published, (unsigned long long)ac97.retired, (unsigned long long)(idle_since ? g_ticks - idle_since : 0));
        err = ac97_stop(&ac97); audio.running = false; ac97.frame_base = audio.output; idle_since = 0; ac97.idle_tail = false; control = true;
    } else if (err) {
        struct ac97_cursor c; int oe = ac97_observe(&ac97, &c);
        klog("[audio] progress err=%d observe=%d civ=%u piv=%u remaining=%u status=%04x published=%llu retired=%llu running=%u halts=%llu",
             err, oe, c.civ, c.piv, c.remaining, c.status, (unsigned long long)ac97.published, (unsigned long long)ac97.retired, audio.running, (unsigned long long)ac97.dma_halts);
    }
    audio.fetched = ac97.fetched; audio.played_output = ac97.frame_base + ac97.generation_fetched;
    audio.fifo = ac97.fifo_errors;
    audio.halts = ac97.dma_halts; audio.deadlines = ac97.deadline_misses;
    /* Publish until the lead is restored: one period per service let any
     * service delay erode the lead permanently (QEMU evidence 2026-10-11:
     * published=770 retired=769 → CELV/DCH halt). */
    /* Active sources keep the latency lead, one period per service. The
     * idle tail (silence only) is cushioned deeper in one burst at its
     * start, before the clients' exits delay this worker (QEMU evidence
     * 2026-10-11); the burst is a control operation outside the 1 ms budget.
     * While a DOS VM source is attached the tail stays at AUDIO_LEAD
     * (audio_refill_lead, f4-05). */
    bool tail_start = !active && audio.running && !idle_since;
    if (tail_start) { idle_since = g_ticks; control = true; }
    unsigned limit, published_now = 0;
    unsigned lead = audio_refill_lead(active, tail_start, audio_vm_attached && audio_vm_attached(), &limit);
    while (!err && (active || audio.running) && ac97.published - ac97.retired < lead && published_now++ < limit) {
        if (!audio.running) {
            memset(quantum, 0, sizeof(quantum)); audio.output += AUDIO_QUANTUM;
        } else audio_mix(quantum, AUDIO_QUANTUM);
        err = ac97_publish(&ac97, quantum);
        if (!err && !audio.running && ac97.published == AUDIO_LEAD) {
            err = ac97_start(&ac97); audio.running = !err;
        }
    }
    if (!err && !active && audio.running) {
        /* Client drains have already waited their fetched/tail allowance.
         * Silence still in the queue is not client data. Keep publishing
         * silence for an idle tail before stopping: the QEMU AC97 backend
         * dropped the last ~0.3 s of fetched audio when the channel was reset
         * at once (evidence 2026-10-11), and codecs settle without clicks. */
        if (g_ticks - idle_since >= AUDIO_IDLE_TAIL_MS) {
            err = ac97_stop(&ac97); audio.running = false;
            ac97.frame_base = audio.output; idle_since = 0; ac97.idle_tail = false; control = true;
        }
    } else if (active) {
        idle_since = 0; /* a new source cancels the idle tail */
    }
    if (!control && ktime_elapsed_us(start, 1000)) { audio.deadlines++; ac97.deadline_misses++; }
    uint64_t elapsed = (g_tsc_per_ms ? (ktime_cycles() - start) * 1000 / g_tsc_per_ms : 0);
    /* Transport control (stop, the idle-tail burst) is accounted apart from refill work. */
    uint32_t *peak = control ? &audio.max_control_us : &audio.max_service_us;
    if (elapsed > *peak) *peak = elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
    if (err) {
        klog("[audio] backend failure err=%d published=%llu retired=%llu running=%u active=%u",
             err, (unsigned long long)ac97.published, (unsigned long long)ac97.retired, audio.running, active);
        audio_fail(-EIO); kmutex_lock(&control_lock); ac97_shutdown(); kmutex_unlock(&control_lock);
    }
    for (unsigned i = 0; i < AUDIO_STREAMS; i++) if (audio.streams[i]) kwait_wake_all(&audio.streams[i]->changed);
}
static bool refill_pending(void *arg) { (void)arg; return audio.pending || audio.error; }
static void audio_worker(void *arg)
{
    (void)arg;
    for (;;) {
        if (audio.error) {
            kmutex_lock(&control_lock);
            if (ac97.ready) ac97_shutdown();
            kmutex_unlock(&control_lock);
            task_sleep_ms(1000); continue;
        }
        audio.pending = false;
        audio_service();
        /* A P_DEVICE thread must block between services (f1-18 rule): a 1 ms
         * poll starved ring-3 tasks (F0 preempt, image 7ab10f64). The IRQ and
         * the stream writers wake `changed`; the deadline is only a safety net:
         * short while the first periods are published, under half a period
         * while running, long when idle. */
        unsigned wait_ms = audio.running ? 4 : ac97.published && ac97.published < AUDIO_LEAD ? 1 : 50;
        kwait_wait_until(&audio.changed, refill_pending, 0, deadline_after_ms(wait_ms));
    }
}
void audio_init(struct vfs *v)
{
    if (audio.initialized) return;
    audio.initialized = true;
    kwait_init(&audio.changed); kmutex_init(&control_lock);
    audio_settings_load(v);
    if (g_boot.flags & CBI_F_SAFE_MODE) { audio.reason = "safe_mode"; return; }
    int err = ac97_init();
    if (err) return;
    struct task *worker = task_create_kernel("audio-refill", audio_worker, 0, P_DEVICE);
    if (!worker) {
        audio.reason = "worker_exhausted"; ac97_shutdown(); return;
    }
    task_start(worker); /* lead fix (QEMU 2026-10-11): a created kernel task runs only after task_start */
    audio.commitment_pages += KSTACK_SIZE / PAGE_SIZE;
    audio.ready = true;
}
int audio_device_open(struct file_description *d, uint32_t flags)
{
    if (d->node->kind == PX_AUDIO) return audio_stream_open(&d->audio);
    (void)flags;
    return 0;
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
    case AUDIO_DRAIN: case AUDIO_RESET: return sizeof(struct ciuki_audio_command);
    case MIXER_GET_MASTER: case MIXER_SET_MASTER: return sizeof(struct ciuki_mixer_master);
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
    unsigned reserved = size / 4 - (op >= MIXER_GET_MASTER || op == AUDIO_GET_STATUS ? 1 : 2);
    for (unsigned i = reserved; !err && i < size / 4; i++) if (record[i]) err = -EINVAL;
    if (!err) { err = ua_pin(p->memory, &o->pin, address, size, true); o->pinned = !err; }
    struct audio_stream *s = d->audio;
    if (!err && interrupted()) err = -EINTR;
    if (!err && s && op != AUDIO_GET_STATUS && (audio.error || !audio.ready)) err = -EIO;
    if (!err) switch (op) {
    case AUDIO_GET_CAPS: memcpy(record, &audio.caps, size); break;
    case AUDIO_SET_FORMAT: err = audio_stream_format(s, (struct ciuki_audio_format *)record); break;
    case AUDIO_SET_GAIN:
        if (record[2] > 100 || record[3] > 100) err = -EINVAL;
        else { s->gain[0] = record[2]; s->gain[1] = record[3]; }
        break;
    case AUDIO_GET_STATUS: {
        struct ciuki_audio_status status = { .version=1, .size=112, .queued_frames=s->queued,
            .state=audio.error ? 2 : s->started ? 1 : 0, .output_rate=AUDIO_RATE,
            .generation=audio.caps.generation, .accepted_frames=s->accepted, .consumed_frames=s->consumed,
            .output_frames=audio.output, .fetched_frames=audio.fetched, .source_starvation=s->starvation,
            .fifo_errors=audio.fifo, .dma_halts=audio.halts, .deadline_misses=audio.deadlines,
            .last_error=audio.error, .stream_generation=s->generation,
            .max_service_us=audio.max_service_us, .max_irq_us=audio.max_irq_us,
            .transport_frames=(uint32_t)(ac97.published * AUDIO_QUANTUM - ac97.generation_fetched) };
        memcpy(record, &status, size); break;
    }
    case AUDIO_RESET: audio_stream_reset(s); break;
    case AUDIO_DRAIN: {
        if (s->draining) { err = -EBUSY; break; }
        uint32_t generation = s->generation;
        s->draining = true; s->drain_goal = s->accepted;
        s->drain_latched = !s->queued; s->drain_output = s->last_output;
        audio_wake();
        while (!err && (!s->drain_latched || audio.played_output < s->drain_output)) err = stream_wait(s, generation, true);
        uint64_t limit = deadline_after_ms(audio.caps.drain_ms);
        while (!err && !deadline_passed(limit)) {
            if (interrupted() || s->generation != generation) err = -EINTR;
            else if (audio.error) err = -EIO;
            else task_sleep_ms(1);
        }
        if (s->generation == generation) { s->draining = false; if (!err && !s->queued) s->started = false; }
        break;
    }
    case MIXER_GET_MASTER: memcpy(record, &audio.master, size); break;
    case MIXER_SET_MASTER:
        err = !d->mixer_control ? -EACCES : audio_master_set((struct ciuki_mixer_master *)record); break;
    }
    if (!err) err = copy_to_user(address, record, size);
    operation_end(o); return err;
}
