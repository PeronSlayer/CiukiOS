#ifndef CIUKI_SESSION_DISK_ATA_H
#define CIUKI_SESSION_DISK_ATA_H

#include <stdint.h>

enum {
    CVATA_OK = 0,
    CVATA_UNBOUND = 1,
    CVATA_E_PACKET = 2,
    CVATA_E_UNSUPPORTED = 3,
    CVATA_E_BUSY = 4,
    CVATA_E_NO_DEVICE = 5,
    CVATA_E_ATA = 6,
    CVATA_E_TIMEOUT = 7,
    CVATA_E_QUARANTINED = 8,
    CVATA_E_RANGE = 9,
    CVATA_E_ARGUMENT = 10
};

enum {
    CVATA_STATE_UNBOUND = 0,
    CVATA_STATE_PREPARED = 1,
    CVATA_STATE_BOUND = 2,
    CVATA_STATE_QUARANTINED = 3
};

/* Stable, 32-bit fields: the VM assembly adapter reads this record directly. */
struct cvata_status {
    uint32_t ready;
    uint32_t state;
    uint32_t error;
    uint32_t command_base;
    uint32_t control_base;
    uint32_t device_head;
    uint32_t irq;
    uint32_t control_shadow_known;
    uint32_t control_shadow;
    uint32_t read_count;
    uint32_t write_count;
    uint32_t last_status;
    uint32_t last_error;
    uint32_t last_lba;
    uint32_t capacity_sectors;
    uint32_t logical_heads;
    uint32_t sectors_per_track;
    uint32_t cylinders;
};
typedef char cvata_status_size_must_be_72[(sizeof(struct cvata_status) == 72U) ? 1 : -1];

/* The adapter serializes these calls with BIOS disk I/O and runs native
 * transactions with physical IF clear. Buffers are validated/mapped by the
 * adapter; no function allocates memory or invokes DOS/firmware.
 * packet is the bounded ADP1 record documented by the VM session ABI.
 * DPTE revisions 10h/11h are recognized with the same required checksum,
 * legacy channel/device and live LBA option; binding still proves the
 * native ATA identity and all 512 BIOS-provided LBA0 bytes. */
uint32_t cvata_prepare(const uint8_t *packet);
/* When EDD does not expose a usable DPTE, observe the serialized VM0 BIOS
 * reset/read instead of probing guessed devices. These routines do no port
 * I/O. BEGIN needs the real EDD capacity/sector size and AH08 geometry;
 * IO receives actual firmware byte OUTs to legacy task/control registers;
 * END writes only packet[40..55] with the proven mapping, leaves the raw
 * EDD record intact, and prepares it for the existing IDENTIFY/LBA0 bind.
 * The adapter calls END only after its requested BIOS LBA0 read succeeded. */
uint32_t cvata_discovery_begin(const uint8_t *packet);
void cvata_discovery_io(uint32_t port, uint32_t value, uint32_t width);
uint32_t cvata_discovery_end(uint8_t *packet);
/* Abort the passive observation; never erase a quarantined/owned channel. */
void cvata_discovery_cancel(void);
/* Call for every VM OUT to the selected channel control port. */
void cvata_observe_control(uint32_t value, uint32_t width);
/* IDENTIFY and compare native LBA0 to the BIOS-provided 512-byte sector. */
uint32_t cvata_bind(const uint8_t *bios_sector);
/* Read or write exactly one 512-byte sector. write is 0 for read, 1 for write.
 * An issued-command error is final, never permission for BIOS fallback.
 * Quarantine retains interrupt suppression until reboot; prepare cannot
 * erase it and a second transfer does not issue another command. */
uint32_t cvata_transfer(uint32_t write, uint32_t lba, uint8_t *buffer);
const struct cvata_status *cvata_get_status(void);

#endif
