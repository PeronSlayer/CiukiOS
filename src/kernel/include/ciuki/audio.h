/* Native copied streams and integer mixer. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_AUDIO_H
#define CIUKI_AUDIO_H
#include <ciuki/abi.h>
#include <ciuki/boot_info.h>
#include <ciuki/sync.h>
#include <ciuki/registry.h>
#define AUDIO_STREAMS 8u
#define AUDIO_RING 2048u
#define AUDIO_QUANTUM 256u
#define AUDIO_LEAD 8u
/* F4-32 Amendment 1: a single 1024-frame capacity callback drains a
 * four-period frontier before the worker runs. Eight periods retain 1024
 * frames of margin; lead evidence has intact PCM at 8, startup halts at 4.
 * 8 * 256 + 304 source frames = 2352 / 48000 = 49.0 ms, <=50 ms nominal.
 * Also covers one missed 10 ms service + one period + one period margin
 * (480 + 256 + 256 = 992 frames); ICH3 needs 3 * 256 = 768 frames.
 * A 10 ms QEMU timer does not bound callback capacity. Measured gaps still
 * grow the session lead (below), exceeding the nominal latency target. */
#define AUDIO_VM_LEAD 8u
#define AUDIO_RATE 48000u
struct audio_frame { int16_t left, right; };
/* T23 observed gap: 112933 us; display stretch: 134 ms. Two periods of
 * margin require 28 periods at 48 kHz. See validation/f4-31-audio-audit.md.
 * Deep silence is cut on source arrival, after stop proof; active lateness
 * grows the session lead and exposes the resulting latency in status/logs. */
#define AUDIO_GAP_BUDGET_US 134000u
#define AUDIO_MAX_LEAD 30u
#define AUDIO_SERVICE_US 2000u
#define AUDIO_TAIL_LEAD 28u
#define AUDIO_IDLE_TAIL_MS 500u /* silence published after the last source before the transport stops */
static inline unsigned audio_gap_lead(unsigned gap_us, unsigned rate)
{
    /* Bound first so the product fits 32 bits at every supported rate. */
    if (gap_us > 150000u) return AUDIO_MAX_LEAD;
    unsigned periods = (gap_us * (rate / 100u) + AUDIO_QUANTUM * 10000u - 1) /
        (AUDIO_QUANTUM * 10000u) + 2u;
    return periods > AUDIO_MAX_LEAD ? AUDIO_MAX_LEAD : periods;
}
/* 2026-10-24 lead decision (docs/design/f4-audio-media-contract.md): a lead
 * grown by a late service or an underrun survives the transport's idle stop.
 * The next session starts with it for AUDIO_LEAD_HOLD_MS after the stop; then
 * it decays by AUDIO_LEAD_DECAY_STEP periods every AUDIO_LEAD_DECAY_MS until
 * it is back at the nominal lead (28 -> 8 periods: 80 s), so the first sound
 * after a long idle keeps the f4-31/f4-32 nominal latency. Returns the kept
 * lead, or 0 for "nominal". 32-bit constant divisions only (i686 helpers). */
#define AUDIO_LEAD_HOLD_MS 30000u
#define AUDIO_LEAD_DECAY_MS 5000u
#define AUDIO_LEAD_DECAY_STEP 2u
static inline unsigned audio_held_lead(unsigned lead, unsigned nominal, uint32_t idle_ms)
{
    if (lead <= nominal) return 0;
    if (idle_ms <= AUDIO_LEAD_HOLD_MS) return lead;
    uint32_t steps = (idle_ms - AUDIO_LEAD_HOLD_MS + AUDIO_LEAD_DECAY_MS - 1u) / AUDIO_LEAD_DECAY_MS;
    uint32_t excess = lead - nominal;
    if (steps >= (excess + AUDIO_LEAD_DECAY_STEP - 1u) / AUDIO_LEAD_DECAY_STEP) return 0;
    return lead - steps * AUDIO_LEAD_DECAY_STEP;
}
static inline unsigned audio_refill_lead(bool active, bool tail_start, bool vm_attached, unsigned *limit)
{
    unsigned lead = active ? (vm_attached ? AUDIO_VM_LEAD : AUDIO_LEAD) : AUDIO_TAIL_LEAD;
    *limit = active || tail_start ? lead : 1u;
    return lead;
}
/* f1-101 (R3): the producer lead. The mixer, not the application, decides
 * when a native stream's producers need CPU: its lead (source frames still
 * queued in the mixer, in output frames) is behind once it falls more than
 * one period below the target lead (AUDIO_PRODUCER_TARGET, the f4-36 refill
 * mark one period under the ring, so AUDIO_PRODUCER_BEHIND equals the 3/4
 * ring mark of AUDIO_REFILL_READ demand at 48 kHz), and refilled once it is
 * back at the target. Rates are at most AUDIO_RATE, so every stream's ring
 * holds at least 2048 output frames and can reach the target. The lead stays
 * below 2^32 in 32-bit arithmetic: 2048 * 48000. */
#define AUDIO_PRODUCER_TARGET (AUDIO_RING - AUDIO_QUANTUM)
#define AUDIO_PRODUCER_BEHIND (AUDIO_PRODUCER_TARGET - AUDIO_QUANTUM)
static inline bool audio_producer_behind(bool behind, unsigned lead, unsigned mark)
{
    return behind ? lead < AUDIO_PRODUCER_TARGET : lead < mark;
}
struct audio_stream {
    struct audio_midi *midi;
    struct audio_frame *ring[2];
    uint32_t pages[2], head, queued, phase, rate, format, channels;
    uint32_t gain[2], generation, source;
    uint64_t accepted, consumed, discarded, starvation, last_output, drain_goal, drain_output;
    bool started, configured, draining, drain_latched;
    bool producer_behind; /* f1-101: last lead state reported to the scheduler */
    struct audio_frame previous;
    bool have_previous;
    struct kwait changed;
};
struct audio_state {
    struct audio_stream *streams[AUDIO_STREAMS];
    struct ciuki_mixer_master master;
    struct ciuki_audio_caps caps;
    struct ciuki_mixer_line lines[6];
    bool lines_initialized;
    unsigned record_source, record_gain;
    struct kwait changed;
    uint64_t output, fetched, played_output, fifo, halts, deadlines, underruns, restarts;
    uint64_t last_service_cycles;
    uint32_t commitment_pages, max_service_us, max_irq_us, max_service_gap_us, max_control_us;
    uint32_t lead_periods, adaptive_lead;
    int error, init_error;
    bool initialized, ready, running, pending, quarantined, master_in_codec, master_in_hardware;
    const char *reason, *settings_reason, *backend_name;
    struct pci_func backend_pci;
    bool backend_touched, linear_resampling;
};
extern struct audio_state audio;
static inline unsigned audio_output_rate(void) { return audio.caps.output_rate ? audio.caps.output_rate : AUDIO_RATE; }
struct file_description;
struct process;
struct vfs;
void audio_init(struct vfs *vfs);
static inline struct audio_frame *audio_frame_at(struct audio_stream *s, unsigned index)
{ return &s->ring[index / 1024][index % 1024]; }
int audio_stream_open(struct audio_stream **out);
void audio_stream_close(struct audio_stream *s);
int audio_stream_format(struct audio_stream *, struct ciuki_audio_format *);
int audio_stream_enqueue(struct audio_stream *, const void *, uint32_t bytes);
int audio_midi_write(struct audio_stream *, struct ciuki_midi_data *);
void audio_midi_close(struct audio_stream *);
bool audio_midi_active(struct audio_stream *);
void audio_midi_mix(struct audio_stream *, int64_t (*)[2], unsigned);
void audio_stream_reset(struct audio_stream *);
/* f1-101: report the stream's lead to the scheduler (on a state change) and
 * register the calling thread as one of its producers (task.h roles). */
void audio_stream_lead(struct audio_stream *);
struct task;
void audio_stream_producer(struct audio_stream *, struct task *, unsigned role);
/* One quantum, no blocking/allocation/codec/file I/O. */
void audio_mix(struct audio_frame *out, unsigned frames);
void audio_service(void);
void audio_fail(int error);
void audio_wake(void);
int audio_device_open(struct file_description *, uint32_t flags);
void audio_device_close(struct file_description *);
int audio_write_user(struct file_description *, uint32_t address, uint32_t bytes);
bool audio_refill_reader(struct process *, uint32_t bytes);
int audio_devctl(struct process *, int32_t fd, uint32_t op, uint32_t address, uint32_t bytes);
int audio_mixer_delegate(struct process *supervisor, struct process *trusted);
int audio_master_set(struct ciuki_mixer_master *);
void audio_lines_load(struct vfs *);
int audio_lines_restore(void);
int audio_line_control(struct ciuki_mixer_line *, bool set);
static inline unsigned audio_source_gain(unsigned source, unsigned channel)
{
    if (!audio.lines_initialized || source == CIUKI_AUDIO_MIXED) return 100;
    if (!source) source = CIUKI_MIXER_WAVE;
    if (source > CIUKI_MIXER_CD) return 100;
    const struct ciuki_mixer_line *l = &audio.lines[source];
    return l->mute ? 0 : channel ? l->right : l->left;
}
void audio_master_refresh(void); /* EC worker: apply without storage I/O (fx-9b) */
int audio_settings_flush(void);   /* fx-9b: shutdown, before the storage flush */
void audio_master_refresh_requested(void); /* f1-100: EC worker, user-requested read */
void audio_settings_load(struct vfs *);
bool audio_registry_load(struct vfs *, struct ciuki_mixer_master *);
void audio_diagnostics(struct ciuki_audio_diagnostics *);
int audio_check_tone(uint32_t *frames);
int audio_check_duration(uint32_t seconds, uint32_t *frames);
static inline bool audio_boot_tone_enabled(uint32_t flags)
{
    return (flags & CBI_F_VERBOSE) && !(flags & (CBI_F_SAFE_MODE | CBI_F_TEST_REQUEST));
}
#endif
