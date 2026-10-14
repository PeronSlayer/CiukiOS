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
#define AUDIO_RATE 48000u
struct audio_frame { int16_t left, right; };
/* Refill lead policy of audio_service (audio_dev.c): returns the periods kept
 * published and sets *limit to the most one service publishes. Active
 * sources restore AUDIO_LEAD in a bounded burst after a late service; the idle tail (silence
 * only) is cushioned to AUDIO_TAIL_LEAD in one burst at its start (QEMU
 * evidence 2026-10-11: the worker is not scheduled for ~130 ms while native
 * clients exit), except while a DOS VM source is attached: a VM plays on its
 * own timeline and cannot wait behind ~150 ms of queued silence. Its next
 * transfer would stop the refill until the cushion drains to AUDIO_LEAD, and
 * the source would then skip that timeline and drop the PCM as late (f4-05,
 * QEMU campaign 12: VMPLAY stage 3 started inside the tail). The tail keeps
 * AUDIO_LEAD then; an underrun ends it as a normal transport stop. */
#define AUDIO_TAIL_LEAD 28u     /* silence periods kept ahead during the idle tail (BDL holds 31) */
#define AUDIO_IDLE_TAIL_MS 500u /* silence published after the last source before the transport stops */
static inline unsigned audio_refill_lead(bool active, bool tail_start, bool vm_attached, unsigned *limit)
{
    unsigned tail = vm_attached ? AUDIO_LEAD : AUDIO_TAIL_LEAD;
    *limit = active ? AUDIO_LEAD : tail_start ? tail : 1u;
    return active ? AUDIO_LEAD : tail;
}
struct audio_stream {
    struct audio_frame *ring[2];
    uint32_t pages[2], head, queued, phase, rate, format, channels;
    uint32_t gain[2], generation;
    uint64_t accepted, consumed, discarded, starvation, last_output, drain_goal, drain_output;
    bool started, configured, draining, drain_latched;
    struct kwait changed;
};
struct audio_state {
    struct audio_stream *streams[AUDIO_STREAMS];
    struct ciuki_mixer_master master;
    struct ciuki_audio_caps caps;
    struct kwait changed;
    uint64_t output, fetched, played_output, fifo, halts, deadlines;
    uint64_t last_service_cycles;
    uint32_t commitment_pages, max_service_us, max_irq_us, max_service_gap_us, max_control_us;
    int error, init_error;
    bool initialized, ready, running, pending, quarantined, master_in_codec, master_in_hardware;
    const char *reason, *settings_reason, *backend_name;
    struct pci_func backend_pci;
    bool backend_touched;
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
int audio_devctl(struct process *, int32_t fd, uint32_t op, uint32_t address, uint32_t bytes);
int audio_mixer_delegate(struct process *supervisor, struct process *trusted);
int audio_master_set(struct ciuki_mixer_master *);
void audio_master_refresh(void); /* EC worker: apply without storage I/O */
void audio_settings_load(struct vfs *);
bool audio_registry_load(struct vfs *, struct ciuki_mixer_master *);
void audio_diagnostics(struct ciuki_audio_diagnostics *);
static inline bool audio_boot_tone_enabled(uint32_t flags)
{
    return (flags & CBI_F_VERBOSE) && !(flags & (CBI_F_SAFE_MODE | CBI_F_TEST_REQUEST));
}
#endif
