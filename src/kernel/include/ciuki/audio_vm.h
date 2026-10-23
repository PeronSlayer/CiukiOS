/* f4-05 DOS VM audio sources in the kernel mixer: one consumer per VM of the
 * f3-05 interface (ciuki/dosvm_audio.h), rendered by the f4-01 refill worker.
 * Directive docs/directives/f4-05-vm-audio-output.md; research and decisions
 * docs/design/f4-acceptance.md, section f4-05. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_AUDIO_VM_H
#define CIUKI_AUDIO_VM_H
#include <ciuki/dosvm.h>
#include <ciuki/audio.h>

/* Eight periods plus one VM tick and one source quantum: 49.0 ms nominal
 * at 48 kHz (2352 frames), within the F4-32 Amendment 1 <=50 ms target.
 * Adaptive transport lead can exceed this target; never read future DMA. */
#define AUDIO_VM_LATENCY (AUDIO_VM_LEAD * AUDIO_QUANTUM + 304u)
/* A quantum is rendered at least one VM tick plus one quantum behind the VM
 * clock, so f3-05 has produced its frames. */
#define AUDIO_VM_FLOOR (AUDIO_QUANTUM + 48u)
#define AUDIO_VM_HOLD_BAND (4u * AUDIO_QUANTUM)
#define AUDIO_VM_SKIP_BAND (6u * AUDIO_QUANTUM)
/* F4-35: one source's cumulative late-arrival shift, 20 ms at 48 kHz.
 * Separate from the physical transport lead and stop/recovery latency. */
#define AUDIO_VM_LATE_BUDGET (AUDIO_RATE / 50u)
#define AUDIO_VM_LOG 64u
#define AUDIO_VM_HEAD 4096u /* OPL frames digested from the first audible one */

enum { AUDIO_VM_FREE, AUDIO_VM_PENDING, AUDIO_VM_ACTIVE };

/* Digital evidence of one path: frames with any non-zero channel, their
 * FNV-1a digest (int16 L,R little endian), sums of squares, peaks and
 * negative-to-non-negative crossings per channel. */
struct audio_vm_tap {
    uint64_t frames, sumsq[2];
    uint32_t digest, peak[2], crossings[2];
    int32_t last[2];
};
/* One fully consumed DSP block: sequence, frames rendered or dropped, the
 * first source frame and the OR of its segment flags (DOSVM_BLOCK_*, plus
 * AUDIO_VM_BLOCK_DROPPED when some frames arrived too late). */
#define AUDIO_VM_BLOCK_DROPPED 0x8000u
struct audio_vm_block_log {
    uint32_t sequence, frames;
    uint32_t dropped; /* source frames actually discarded in this block */
    int16_t first[2];
    uint16_t flags, reserved;
};
struct audio_vm_stats {
    uint32_t epoch;
    uint8_t state, muted, gain[2];
    uint64_t rendered;       /* output frames rendered for this source */
    uint64_t pcm_frames;     /* source frames consumed into the stream */
    uint64_t late_frames;    /* source frames dropped: their timeline position had passed */
    /* Source-frame histogram before late recovery: <1, [1,5), [5,20], >20 ms.
     * Snapshots describe the most recent actual drop, in 48 kHz frames. */
    uint64_t late_hist[4];
    uint32_t drop_runs, drop_last, drop_max;
    int32_t drop_lag, drop_lag_min, drop_lead, drop_lead_min, drop_lead_max;
    uint32_t delay_frames; /* common PCM/OPL timeline shift, bounded per source */
    uint64_t starved;        /* output frames silent while the DSP was active without data */
    uint64_t hold_frames, skipped_frames;
    uint32_t blocks, segments, streams, holds, skips, reanchors, seq_gaps;
    uint32_t late_blocks, stale_blocks, overrun_blocks, unsupported_blocks;
    uint32_t opl_events, late_events, opl_resyncs, dac_events, foreign_events, revoked;
    int32_t lead, lead_min, lead_max; /* timeline-to-output latency in frames at each quantum */
    int32_t lag, lag_min;             /* VM clock minus the next rendered timeline frame */
    uint32_t opl_head_digest, opl_head_frames;
    struct audio_vm_tap pcm, opl;     /* after CT1745 and source gains, before the master */
    uint32_t log_count;
    struct audio_vm_block_log log[AUDIO_VM_LOG];
};

/* Called by dosvm_create once the VM is complete: -ENODEV without a ready
 * backend (f3-05 keeps discarding). dosvm_destroy revokes before
 * dosvm_audio_close. Thread context. */
int audio_vm_attach(struct dosvm_key key);
void audio_vm_detach(struct dosvm_key key);
/* From the vm-clock task (fx-9; thread context): wakes an idle refill
 * worker once when a VM has queued audio. */
void audio_vm_kick(void);
/* Per-source gain 0-100 and mute (default 100/100, unmuted). */
int audio_vm_set_gain(struct dosvm_key key, unsigned left, unsigned right, bool mute);
bool audio_vm_stats(struct dosvm_key key, struct audio_vm_stats *out);
/* Clears the source's taps, head digest and block log, and the ring tap. */
void audio_vm_tap_reset(struct dosvm_key key);
/* The final published frames (after the master gain and saturation) while
 * any VM source is attached. */
void audio_vm_ring(struct audio_vm_tap *out);
/* Refill worker hooks (weak references in mixer.c and audio_dev.c). */
void audio_vm_mix(int64_t (*sums)[2], unsigned frames);
void audio_vm_tap(const struct audio_frame *out, unsigned frames);
bool audio_vm_active(void);
bool audio_vm_playing(void); /* read-only snapshot for mixer diagnostics */
unsigned audio_vm_playing_count(void);
/* Any VM source attached (refill policy, audio_refill_lead). */
bool audio_vm_attached(void);
/* Successful physical publication, and a small coherent diagnostic snapshot.
 * No printing in the refill worker. */
struct audio_vm_diagnostic {
    uint64_t published, pcm, opl, starved, fifo, halts;
    uint64_t level_at;
    uint32_t pcm_peak, opl_peak, mix_peak, ring_peak;
    uint32_t rate;
    uint32_t refill_us, refill_gap_us;
    uint64_t late_hist[4], late_frames;
    uint32_t drop_runs, drop_last, drop_max, delay_frames;
    int32_t lag, lag_min, lead, lead_min, lead_max;
    int32_t drop_lag, drop_lag_min, drop_lead, drop_lead_min, drop_lead_max;
    int error;
    uint8_t gain[2], master[2];
    bool ready, attached, running, muted, master_muted, master_in_codec;
};
void audio_vm_published(void);
/* Refill worker, after proven stop/reset: keep the next unrendered VM
 * frame through the new generation's silence prime and any retained quantum. */
void audio_vm_reanchor(void);
void audio_vm_diagnostic(struct dosvm_key, struct audio_vm_diagnostic *);
#endif
