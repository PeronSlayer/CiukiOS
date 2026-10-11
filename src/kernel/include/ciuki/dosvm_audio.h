/* f3-05 boundary between the per-VM Sound Blaster 16, OPL3 and 8237 models
 * and their F4 consumer (f4-audio-media-contract.md, "DOS VM audio path").
 * F3 owns guest discovery, the DSP/mixer/DMA/OPL state machines and the VM
 * audio timeline; it never touches physical audio hardware or DMA. F4 reads
 * copied PCM blocks and timestamped register events through these calls.
 * Research and decisions: docs/design/f3-acceptance.md, section f3-05.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_DOSVM_AUDIO_H
#define CIUKI_DOSVM_AUDIO_H
#include <ciuki/dosvm.h>

/* The VM audio timeline counts frames of a 48 kHz clock from VM creation,
 * advanced by dosvm_tick independently of guest IF. Every PCM block and event
 * carries a position on it; F4 maps it to its output cursor. */
#define DOSVM_AUDIO_HZ 48000u

/* PCM segment flags. A DSP block is one or more segments (a pause or a DMA
 * stall closes a segment); COMPLETE marks the block's last segment. */
#define DOSVM_BLOCK_COMPLETE    0x01u /* SB block boundary reached; cause is valid */
#define DOSVM_BLOCK_SILENCE     0x02u /* DSP 80h: silent frames by design */
#define DOSVM_BLOCK_UNSUPPORTED 0x04u /* Creative ADPCM: timing and IRQs kept, silence produced */
#define DOSVM_BLOCK_OVERRUN     0x08u /* frames dropped: queue full (consumer late) */
#define DOSVM_BLOCK_LATE        0x10u /* resumed after the block duration without acknowledgement */
#define DOSVM_BLOCK_STALE       0x20u /* two boundaries unserviced: guest data possibly stale */
#define DOSVM_BLOCK_ABORTED     0x40u /* reset, fault or new command ended the block early */

/* Source format of a segment (before normalisation to signed 16-bit stereo). */
#define DOSVM_FORMAT_16BIT  0x01u
#define DOSVM_FORMAT_SIGNED 0x02u
#define DOSVM_FORMAT_STEREO 0x04u

struct dosvm_audio_block {
    uint64_t start;      /* timeline frame where the segment's first frame begins */
    uint32_t generation; /* VM generation (dosvm_key) */
    uint32_t sequence;   /* DSP block number since VM creation, from 1 */
    uint32_t rate;       /* source frames per second (DSP programmed rate) */
    uint32_t frames;     /* frames in the segment so far; final once closed */
    uint32_t offset;     /* index of the first frame returned by this fetch */
    uint16_t flags;      /* DOSVM_BLOCK_* */
    uint8_t cause;       /* mixer 82h bit latched at the boundary: 1 8-bit, 2 16-bit */
    uint8_t format;      /* DOSVM_FORMAT_* */
    bool closed;         /* no more frames will be appended to this segment */
};

#define DOSVM_EVENT_OPL 1u /* OPL3 register write: reg 000h-1FFh */
#define DOSVM_EVENT_DAC 2u /* DSP 10h direct DAC level (8-bit unsigned) */
#define DOSVM_EVENT_GAP 3u /* events were lost (overflow): resynchronise from the register file */
struct dosvm_audio_event {
    uint64_t clock;      /* timeline frame of the guest write */
    uint32_t generation; /* VM generation */
    uint16_t reg;
    uint8_t value, kind;
};

struct dosvm_audio_info {
    uint64_t clock, dsp_clock;           /* timeline now; DSP nominal position */
    uint64_t frames, discarded_frames, overrun_frames, dropped_frames;
    uint64_t events, discarded_events, lost_events;
    uint32_t base, irq, dma8, dma16;
    uint32_t boundaries, irq_edges, services, late, stale_blocks, forced;
    uint32_t units, dma_faults, dma_wait_frames, unsupported, adpcm_blocks;
    uint32_t restarts, resets, max_service, max_lag, direct_dac, midi_bytes;
    uint32_t pcm_queued, blocks_queued, events_queued;
    uint8_t causes, active, paused, waiting, auto_init, bits, stereo, speaker, highspeed, last_error;
    uint8_t master[2], voice[2], fm[2];  /* CT1745 5-bit levels, 0 (-62 dB) to 31 (0 dB) */
    uint8_t opl_status, opl_new, opl_pairs;
    bool consumer;
    uint32_t opl_period_us[2];           /* last programmed timer periods */
    uint64_t opl_start_us[2], opl_flag_us[2]; /* VM microseconds: start and first flag */
};

/* One consumer per VM. attach returns a token that later calls must present;
 * detach, VM stop or a new attach makes older tokens fail with -EINVAL. */
int dosvm_audio_attach(struct dosvm_key, uint32_t *consumer);
int dosvm_audio_detach(struct dosvm_key, uint32_t consumer);
/* Copies at most max_frames signed 16-bit stereo frames (L,R) of the oldest
 * queued segment and describes them in *block. A call never crosses a segment
 * or block boundary; an open segment may be returned partially and continued
 * by later calls (offset). Returns the frame count, 0 when the oldest segment
 * is open and has no new frames, -EAGAIN when nothing is queued. */
int dosvm_audio_fetch(struct dosvm_key, uint32_t consumer, struct dosvm_audio_block *block,
                      int16_t *stereo, unsigned max_frames);
/* Removes up to max events in write order. A GAP event precedes the first
 * event after an overflow. Returns the count or a negative errno. */
int dosvm_audio_events(struct dosvm_key, uint32_t consumer, struct dosvm_audio_event *out, unsigned max);
bool dosvm_audio_info(struct dosvm_key, struct dosvm_audio_info *out);
/* The authoritative OPL3 register file (index 000h-1FFh) at timeline *clock. */
bool dosvm_audio_opl_registers(struct dosvm_key, uint8_t regs[512], uint64_t *clock);
#endif
