/* Per-VM Sound Blaster 16 DSP/mixer, OPL3 and 8237A state (f3-05).
 * No physical I/O or DMA. Research: docs/design/f3-acceptance.md, f3-05.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_DOSVM_AUDIO_STATE_H
#define CIUKI_DOSVM_AUDIO_STATE_H
#include <ciuki/dosvm_audio.h>

#define DOSVM_AUDIO_PER_MS (DOSVM_AUDIO_HZ / 1000u)
/* DSP work per tick call; the remainder stays as backlog for later ticks. */
#define DOSVM_AUDIO_SLICE (64u * DOSVM_AUDIO_PER_MS)
/* Backlog beyond one second is dropped from device time and counted. */
#define DOSVM_AUDIO_BACKLOG (1000u * DOSVM_AUDIO_PER_MS)
#define DOSVM_PCM_PAGES 4u
#define DOSVM_PCM_FRAMES (DOSVM_PCM_PAGES * PAGE_SIZE / 4u)   /* 4096 stereo frames */
#define DOSVM_EVENT_PAGES 2u
#define DOSVM_EVENTS (DOSVM_EVENT_PAGES * PAGE_SIZE / sizeof(struct dosvm_audio_event))
#define DOSVM_AUDIO_PAGES (DOSVM_PCM_PAGES + DOSVM_EVENT_PAGES)
#define DOSVM_SEGMENTS 64u
/* Factory SB16 setting, matching CIUKIDOS's BLASTER=A220 I7 D1 H5 T6. */
#define DOSVM_SB_BASE 0x220u
#define DOSVM_SB_IRQ 7u
#define DOSVM_SB_DMA8 1u
#define DOSVM_SB_DMA16 5u
#define DOSVM_OPL_BASE 0x388u
#define DOSVM_SB_RATE_MIN 5000u  /* Creative guide, command 41h: 5000-45000 Hz */
#define DOSVM_SB_RATE_MAX 45000u

enum dosvm_audio_error { DOSVM_AUDIO_OK, DOSVM_AUDIO_DMA_FAULT, DOSVM_AUDIO_DMA_MODE };
enum dosvm_sb_kind { DOSVM_SB_PCM, DOSVM_SB_SILENCE, DOSVM_SB_ADPCM };
enum dosvm_audio_record_kind { DOSVM_AUDIO_RESET=1, DOSVM_AUDIO_VERSION,
    DOSVM_AUDIO_PLAY, DOSVM_AUDIO_DMA };

struct dosvm_dma_channel {
    uint16_t base_address, base_count, address, count;
    uint8_t mode;
    bool masked;
};
struct dosvm_dma {
    struct dosvm_dma_channel ch[8];
    uint8_t page[16]; /* 80h-8Fh; channel pages plus plain storage */
    uint8_t flipflop[2], status[2], command[2], request[2], temporary[2];
    uint32_t faults;
};

struct dosvm_sb_transfer {
    uint32_t units, left, rate, threshold; /* units per block; units to the boundary; frames/s; phase per unit */
    uint8_t format, kind, channel, samples; /* ADPCM: samples per byte */
    bool active, auto_init, exit_pending, paused, highspeed, waiting, reference;
    bool late, stale;
};
struct dosvm_sb16 {
    uint16_t base;
    uint8_t irq, dma8, dma16;
    uint8_t command, in[3], in_count, in_need;
    uint8_t out[64], out_head, out_count, last;
    uint8_t mixer_index, mixer[256], test, causes, last_error;
    bool reset_high, speaker, uart, rate_from_tc, next_valid, half_valid, carry_valid;
    int16_t half;
    uint8_t carry;
    uint32_t rate, block_units, phase, unserviced, sequence;
    uint64_t pos, wait_until, boundary_at, block_start;
    struct dosvm_sb_transfer x, next;
    uint32_t boundaries, irq_edges, services, late, stale_blocks, forced, units;
    uint32_t dma_wait_frames, unsupported, adpcm_blocks, restarts, resets;
    uint32_t max_service, max_lag, direct_dac, midi_bytes;
    uint64_t dropped_frames;
};

struct dosvm_opl_timer {
    uint64_t start, trigger, started_us, flag_us;
    uint32_t period;
    bool enabled, masked, overflow;
};
struct dosvm_opl {
    uint16_t address;
    uint8_t regs[512];
    struct dosvm_opl_timer timer[2];
    uint32_t io_us; /* virtual ISA I/O time inside the current millisecond */
};

struct dosvm_audio_queue {
    int16_t *pcm[DOSVM_PCM_PAGES];
    struct dosvm_audio_event *events[DOSVM_EVENT_PAGES];
    uint32_t pages[DOSVM_AUDIO_PAGES];
    unsigned page_count;
    unsigned pcm_head, pcm_count, event_head, event_count, seg_head, seg_count;
    struct dosvm_audio_block seg[DOSVM_SEGMENTS]; /* offset = frames already fetched */
    uint32_t consumer;
    bool open, gap;
    uint64_t frames, discarded_frames, overrun_frames;
    uint64_t event_writes, discarded_events, lost_events;
};

struct dosvm_audio {
    uint64_t clock, ms;
    struct dosvm_dma dma;
    struct dosvm_sb16 sb;
    struct dosvm_opl opl;
    struct dosvm_audio_queue q;
    /* Bounded first discovery/programming records; produced without I/O,
     * emitted by the VM worker outside guest interrupt chains. */
    struct {
        struct { uint32_t a, b, c; uint8_t kind; } event[16];
        uint8_t head, count, used[5];
        uint32_t dropped;
        bool attachment, environment, summary_pending;
        int attach_error;
        uint64_t next_log, next_summary, last_summary, published;
    } diag;
};

struct dosvm;
int dosvm_audio_init(struct dosvm *);
void dosvm_audio_close(struct dosvm *);
void dosvm_audio_advance(struct dosvm *, uint64_t elapsed_ms);
int dosvm_audio_port(struct dosvm *, uint16_t, unsigned, bool, uint32_t *, void *);
void dosvm_audio_log(struct dosvm *);
void dosvm_audio_record(struct dosvm *, unsigned kind, uint32_t, uint32_t, uint32_t);
/* 8237A */
void dosvm_dma_init(struct dosvm *);
uint8_t dosvm_dma_io(struct dosvm *, uint16_t port, bool write, uint8_t value);
int dosvm_dma_read(struct dosvm *, unsigned channel, uint8_t *buffer, unsigned transfers, bool *terminal);
/* OPL3 */
void dosvm_opl_init(struct dosvm *);
uint8_t dosvm_opl_io(struct dosvm *, unsigned port, bool write, uint8_t value);
uint8_t dosvm_opl_status(struct dosvm *);
void dosvm_audio_event(struct dosvm *, uint8_t kind, uint16_t reg, uint8_t value);
struct dosvm_audio_event *dosvm_event_slot(struct dosvm_audio_queue *, unsigned index);
uint64_t dosvm_audio_now(const struct dosvm *);
#endif
