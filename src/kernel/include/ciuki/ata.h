/* F1 legacy ATA PIO and its register/time test boundary.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_ATA_H
#define CIUKI_ATA_H

#include "sync.h"
#include "../../fs/blkdev.h"

#define ATA_CHANNELS 2u
#define ATA_DEVICES 2u
#define ATA_COMMAND_MS 30000u
#define ATA_FLUSH_MS 60000u
#define ATAPI_COMMAND_MS 2000u
#define ATAPI_START_MS 10000u
#define ATAPI_MAX_BYTES 65536u
enum atapi_media { ATAPI_UNKNOWN, ATAPI_READY, ATAPI_NO_MEDIUM, ATAPI_NOT_READY, ATAPI_CHANGED };

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
};

struct ata_channel;
struct ata_device {
    struct blkdev block;
    struct ata_channel *channel;
    unsigned unit;
    bool present, atapi, identified, lba48, flush_ext;
    char model[41];                 /* printable, whitespace replaced by '_' */
    gen_t generation;              /* last issued command, retained for evidence */
    uint64_t commands, elapsed_ms;
    uint8_t status, error, command;
    bool issued;
    uint8_t packet_bytes, packet_type, sense_key, asc, ascq;
    enum atapi_media media;
    uint32_t session_lba;
    uint64_t packet_commands, read_blocks, media_writes, max_packet_ms;
};

/* Pinned until reboot/quiescence; no allocation or work in the IRQ handler.
 * Synthetic channels have native=false: no PIC, registry or physical I/O.
 * Setup/rearming is exclusively for a fresh synthetic device or boot. */
struct ata_channel {
    const struct ata_port_ops *ops;
    void *port_ctx;
    uint16_t base, control;
    unsigned irq;
    struct kmutex mutex;
    struct kwait wait;
    struct ata_device devices[ATA_DEVICES];
    struct ata_device *active;
    gen_t request_generation;
    volatile gen_t irq_generation;
    volatile uint32_t irq_sequence;
    volatile uint8_t irq_status;
    uint64_t commands;
    int claims[3];
    gen_t generations[3];
    uint8_t control_shadow;
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

int probe_ata(void);
int probe_ata_fault(void);
int probe_partition(void);

#endif
