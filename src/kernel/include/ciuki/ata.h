/* F1 legacy ATA PIO/DMA and its register/time test boundary.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_ATA_H
#define CIUKI_ATA_H

#include "sync.h"
#include "ata_dma.h"
#include "../../fs/blkdev.h"

#define ATA_CHANNELS 2u
#define ATA_DEVICES 2u
#define ATA_COMMAND_MS 30000u
#define ATA_FLUSH_MS 60000u
#define ATAPI_COMMAND_MS 2000u
#define ATAPI_START_MS 10000u
#define ATAPI_MAX_BYTES 65536u
enum atapi_media { ATAPI_UNKNOWN, ATAPI_READY, ATAPI_NO_MEDIUM, ATAPI_NOT_READY, ATAPI_CHANGED };
/* f5-12B2: READ TOC format 0 (LBA) cached per media_generation. CONTROL
 * bit 2 marks a data track (MMC-3 Table 222); audio_only means no data
 * track at all; first track audio means "ready for CD audio, not
 * mountable" (F4 keeps mounting the first data session only). */
#define ATAPI_TOC_TRACKS 99u
#define ATAPI_TOC_BYTES (4u + 8u * (ATAPI_TOC_TRACKS + 1u))
#define ATAPI_CD_SECTOR 2352u
#define ATAPI_CD_READ_MAX 27u
struct atapi_toc {
    uint32_t generation;
    uint8_t first, last, count;
    bool valid, audio_only, first_audio;
    uint8_t control[ATAPI_TOC_TRACKS], adr[ATAPI_TOC_TRACKS], number[ATAPI_TOC_TRACKS];
    uint32_t start[ATAPI_TOC_TRACKS];
    uint32_t leadout;
};

struct ata_port_ops {
    uint8_t (*in8)(void *ctx, uint16_t port);
    void (*out8)(void *ctx, uint16_t port, uint8_t value);
    uint16_t (*in16)(void *ctx, uint16_t port);
    void (*out16)(void *ctx, uint16_t port, uint16_t value);
    uint64_t (*now_ms)(void *ctx);
    /* Map an absolute boundary-clock deadline to scheduler PIT ticks.
     * Fakes may accelerate time, without changing production timeouts. */
    uint64_t (*deadline_ticks)(void *ctx, uint64_t deadline_ms);
    int (*delay_us)(void *ctx, uint32_t us);
    /* Optional bounded string-I/O bursts; synthetic ports may use word I/O. */
    void (*insw)(void *ctx, uint16_t port, void *buffer, uint32_t words);
    void (*outsw)(void *ctx, uint16_t port, const void *buffer, uint32_t words);
};

struct ata_channel;
struct ata_device {
    struct blkdev block;
    struct ata_channel *channel;
    unsigned unit;
    bool present, atapi, identified, lba48, flush_ext;
    char model[41];                 /* printable, whitespace replaced by '_' */
    char serial[21], firmware[9];   /* f9-02: IDENTIFY words 10-19 and 23-26, trimmed */
    gen_t generation;              /* last issued command, retained for evidence */
    uint64_t commands, elapsed_ms;
    uint8_t status, error, command;
    uint8_t multiple_max, multiple_sectors;
    bool issued;
    bool dma_enabled, dma_quarantined, dma_configuring;
    uint8_t udma_mode;
    int8_t firmware_udma;            /* measured before the discovery SRST */
    uint64_t dma_requests, dma_bytes, dma_bounces, dma_errors;
    /* f1-92: interrupt edges that were not this command's completion, issued
     * commands completed after their request was interrupted, and failed DMA
     * requests repeated once through PIO. Records are bounded per unit. */
    uint32_t early_irqs, interrupted_inflight, dma_retries;
    uint8_t packet_bytes, packet_type, sense_key, asc, ascq;
    const char *ready_stage;
    int ready_error;
    enum atapi_media media;
    uint32_t session_lba;
    uint32_t spinup_generation;
    uint64_t packet_commands, read_blocks, media_writes, max_packet_ms;
    /* f9-01: write-class commands issued to a fixed disk (WRITE, WRITE DMA,
     * emergency write, FLUSH CACHE); Setup mode asserts it stays zero. */
    uint64_t write_commands;
    /* f5-12B2 CD audio: TOC cache, page 2Ah capability bits (fetched once
     * per drive, cd_caps_valid) and the audio-disc readiness flag. */
    struct atapi_toc toc;
    uint8_t cd_caps[12];
    bool cd_caps_valid, audio_only, cd_playing;
    uint64_t cd_audio_sectors, cd_audio_packets, cd_play_commands;
};

/* Pinned until reboot/quiescence; no allocation or work in the IRQ handler.
 * Synthetic channels have native=false: no PIC, registry or physical I/O.
 * Setup/rearming is exclusively for a fresh synthetic device or boot. */
struct ata_channel {
    bool dma_completion_logged; /* first successful DMA on this channel/boot */
    const struct ata_port_ops *ops;
    void *port_ctx;
    uint16_t base, control;
    unsigned irq;
    struct kmutex mutex;
    struct blk_queue requests;
    struct kwait wait;
    struct ata_device devices[ATA_DEVICES];
    struct ata_device *active;
    struct ata_dma dma;
    gen_t request_generation;
    volatile gen_t irq_generation;
    volatile uint32_t irq_sequence;
    volatile uint8_t irq_status;
    uint64_t commands;
    int claims[3];
    gen_t generations[3];
    uint8_t control_shadow;
    uint8_t expiry;                 /* f1-92: why the last expired()/wait ended */
    const char *fault_cause;        /* f1-92: cause carried from DMA to channel fault */
    bool native, stopped, discovered, quiescing;
};

int ata_channel_setup(struct ata_channel *channel, unsigned index,
                      const struct ata_port_ops *ops, void *ctx);
int ata_channel_discover(struct ata_channel *channel);
void ata_channel_irq(struct ata_channel *channel);
bool ata_channel_idle(struct ata_channel *channel);
bool ata_registry_idle(int handle, gen_t generation); /* registry_quiesce proof */
int ata_init(void);                 /* thread context, after kwork_init */
struct ata_device *ata_device_get(unsigned channel, unsigned unit);
/* Inventory includes identified quarantined units; no I/O or recovery. */
const struct ata_device *ata_inventory_get(unsigned channel, unsigned unit);
uint64_t ata_command_count(void);   /* real devices only */
/* Read-only packet boundary. Only the required read/control opcodes are
 * admitted, data is device-to-host, CHECK CONDITION is classified by sense.
 * A media condition returns EIO without poisoning the ATA peer. */
int atapi_packet(struct ata_device *, const uint8_t *packet, unsigned packet_length,
                 void *buffer, uint32_t capacity, uint32_t *done);
int atapi_ready(struct ata_device *); /* new transaction, never retry a read */
int atapi_eject(struct ata_device *);
int atapi_start(struct ata_device *);
/* f5-12B2 CD-audio packet paths (src/kernel/drivers/optical_cd.c), MMC-3
 * r10g / SFF-8020i r2.6; each takes the channel mutex through atapi_packet
 * and is refused before any packet when the request is malformed. */
int atapi_cd_toc(struct ata_device *, bool refresh);        /* READ TOC 43h format 0, LBA */
/* Recheck generation under the F4 channel mutex, after waiting for a peer. */
int atapi_packet_generation(struct ata_device *, const uint8_t *, unsigned,
                            void *, uint32_t, uint32_t *, uint32_t generation);
int atapi_cd_caps(struct ata_device *);                     /* MODE SENSE(10) 5Ah page 2Ah */
int atapi_cd_read_check(struct ata_device *, uint32_t generation, uint32_t lba, uint32_t sectors);
int atapi_cd_read_audio(struct ata_device *, uint32_t generation, uint32_t lba,
                        uint32_t sectors, void *buffer);    /* READ CD BEh, CD-DA, user data */
int atapi_cd_play(struct ata_device *, uint32_t generation, uint32_t start_lba, uint32_t end_lba); /* 47h */
int atapi_cd_pause(struct ata_device *, uint32_t generation, bool resume);                        /* 4Bh */
int atapi_cd_stop(struct ata_device *, uint32_t generation);                                      /* 4Eh */
int atapi_cd_position(struct ata_device *, uint32_t generation, uint8_t *audio_status,
                      uint8_t *track, uint8_t *index, uint32_t *absolute, uint32_t *relative);     /* 42h */
int atapi_cd_load(struct ata_device *);                     /* START STOP UNIT LoEj=1 Start=1 */
int atapi_cd_range_check(const struct atapi_toc *, uint32_t lba, uint32_t sectors); /* audio only */
void atapi_lba_to_msf(uint32_t lba, uint8_t msf[3]);
uint32_t atapi_msf_to_lba(const uint8_t msf[3]);

int probe_ata(void);
int probe_ata_fault(void);
int probe_partition(void);

#endif
