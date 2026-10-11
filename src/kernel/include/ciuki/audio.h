/* Native copied streams and integer mixer. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_AUDIO_H
#define CIUKI_AUDIO_H
#include <ciuki/abi.h>
#include <ciuki/sync.h>
#define AUDIO_STREAMS 8u
#define AUDIO_RING 2048u
#define AUDIO_QUANTUM 256u
#define AUDIO_LEAD 8u
#define AUDIO_RATE 48000u
struct audio_frame { int16_t left, right; };
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
    uint32_t commitment_pages, max_service_us, max_irq_us, max_service_gap_us;
    int error;
    bool initialized, ready, running, pending, quarantined;
    const char *reason, *settings_reason;
};
extern struct audio_state audio;
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
void audio_settings_load(struct vfs *);
#endif
