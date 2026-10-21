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
static inline unsigned audio_refill_lead(bool active, bool tail_start, bool vm_attached, unsigned *limit)
{
    unsigned lead = active ? (vm_attached ? AUDIO_VM_LEAD : AUDIO_LEAD) : AUDIO_TAIL_LEAD;
    *limit = active || tail_start ? lead : 1u;
    return lead;
}
struct audio_stream {
    struct audio_frame *ring[2];
    uint32_t pages[2], head, queued, phase, rate, format, channels;
    uint32_t gain[2], generation;
    uint64_t accepted, consumed, discarded, starvation, last_output, drain_goal, drain_output;
    bool started, configured, draining, drain_latched;
    struct audio_frame previous;
    bool have_previous;
    struct kwait changed;
};
struct audio_state {
    struct audio_stream *streams[AUDIO_STREAMS];
    struct ciuki_mixer_master master;
    struct ciuki_audio_caps caps;
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
void audio_stream_reset(struct audio_stream *);
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
void audio_master_refresh(void); /* EC worker: apply without storage I/O */
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
