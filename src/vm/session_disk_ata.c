/*
 * Narrow legacy ATA PIO backend for the CiukiOS V86 disk session.
 *
 * Primary references consulted before implementation:
 * Phoenix Enhanced Disk Drive Specification 1.1, FDPT extension fields and
 * LBA/removable/ATAPI option bits:
 * https://wiki.sensi.org/download/doc/ata_edd_11.pdf
 * ATA/ATAPI-5, task-file register and IDENTIFY/PIO command behavior:
 * https://www.seagate.com/support/disc/manuals/ata/d1153r17.pdf
 * ATA/ATAPI-6, PIO state diagrams, status, and Device Control behavior:
 * https://www.read.seas.harvard.edu/~kohler/class/04f-aos/ref/hardware/ATA-d1410r3a.pdf
 *
 * The backend deliberately speaks only one sector at a time, uses the
 * BIOS-reported or firmware-observed legacy channel/device, and never
 * allocates memory. After
 * any issued command failure, callers must treat the result as final rather
 * than retrying through another disk path.
 */
#include <stdint.h>
#include "session_disk_ata.h"

extern uint32_t cvdev_in(uint32_t port, uint32_t size);
extern void cvdev_out(uint32_t port, uint32_t value, uint32_t size);
/* Fixed 512-byte PIO data phase. String I/O also avoids 256 separate VM
 * exits per sector in a virtual machine; status/ownership stays here. */
extern void cvata_pio_read(uint32_t port, uint8_t *buffer);
extern void cvata_pio_write(uint32_t port, const uint8_t *buffer);
extern uint32_t cvata_clock_khz(void);
extern void cvclock_now(uint32_t *low, uint32_t *high);

#define ATA_DATA       0U
#define ATA_ERROR      1U
#define ATA_FEATURES   1U
#define ATA_COUNT      2U
#define ATA_LBA_LOW    3U
#define ATA_LBA_MID    4U
#define ATA_LBA_HIGH   5U
#define ATA_DEVICE     6U
#define ATA_STATUS     7U
#define ATA_COMMAND    7U

#define ATA_CTL_ALT_STATUS 0U
#define ATA_CTL_DEVICE     0U
#define ATA_ST_ERR     0x01U
#define ATA_ST_DRQ     0x08U
#define ATA_ST_DF      0x20U
#define ATA_ST_BSY     0x80U
#define ATA_CTL_NIEN   0x02U
#define ATA_CTL_SRST   0x04U
#define ATA_CTL_HOB    0x80U

#define ATA_IDENTIFY   0xecU
#define ATA_READ       0x20U
#define ATA_WRITE      0x30U
#define ATA_TIMEOUT_MS 30000UL
#define ATA_STALLED_CLOCK_LIMIT 65536UL
#define ATA_SETTLE_READS 4U
#define ATA_NATIVE28_SECTORS 0x10000000UL
#define ATA_NATIVE28_LAST    0x0fffffffUL

#define PIC2_COMMAND 0x00a0U
#define PIC_OCW3_IRR 0x0aU
#define PIC_OCW3_ISR 0x0bU

enum {
    ADP_MAGIC = 0,
    ADP_BYTES = 4,
    ADP_DRIVE = 6,
    ADP_RESERVED = 7,
    ADP_EDD = 8,
    ADP_DPT = 40,
    ADP_GEOMETRY = 56,
    ADP_TAIL_RESERVED = 62,
    EDD_MIN_BYTES = 30,
    EDD_CAPACITY = 16,
    EDD_BYTES_PER_SECTOR = 24,
    DPT_BASE = 0,
    DPT_CONTROL = 2,
    DPT_HEAD = 4,
    DPT_IRQ = 6,
    DPT_OPTIONS = 10,
    DPT_REVISION = 14,
    DPT_CHECKSUM = 15
};

static struct cvata_status g_status;
static uint8_t g_identify[512];
static uint8_t g_bind_sector[512];
/* Native OUTs bypass the V86 observer. Keep the firmware's last observed
 * value separate from our temporary interrupt suppression. */
static uint8_t g_control_saved, g_control_owned, g_command_issued;
static uint32_t g_clock_khz;
static uint32_t g_timeout_low, g_timeout_high;

/* A firmware-issued command, never sector contents, establishes the route
 * from BIOS DL to a legacy ATA channel/device when DPTE is unavailable. */
struct ata_discovery {
    uint8_t packet[64];
    uint8_t task[2][5]; /* count, LBA low/mid/high, device */
    uint8_t seen[2], control[2], control_known[2];
    uint8_t active, rejected, channel, head;
    uint32_t reads;
};
static struct ata_discovery g_discovery;

struct ata_deadline {
    uint32_t start_low, start_high, previous_low, previous_high;
    uint32_t stagnant;
};

static void deadline_begin(struct ata_deadline *d)
{
    cvclock_now(&d->start_low, &d->start_high);
    d->previous_low = d->start_low;
    d->previous_high = d->start_high;
    d->stagnant = 0U;
}

static int deadline_expired(struct ata_deadline *d)
{
    uint32_t low, high, elapsed_low, elapsed_high;
    cvclock_now(&low, &high);
    /* The boot-calibrated TSC continues with IRQs disabled. A poll count is
     * not an elapsed-time limit. Retain a separate frozen-clock escape. */
    if (low == d->previous_low && high == d->previous_high) ++d->stagnant;
    else d->stagnant = 0U;
    d->previous_low = low;
    d->previous_high = high;
    elapsed_low = low - d->start_low;
    elapsed_high = high - d->start_high - (low < d->start_low);
    return elapsed_high > g_timeout_high ||
           (elapsed_high == g_timeout_high && elapsed_low >= g_timeout_low) ||
           d->stagnant >= ATA_STALLED_CLOCK_LIMIT;
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t set_error(uint32_t error)
{
    g_status.error = error;
    return error;
}

static uint8_t ata_in8(uint32_t port)
{
    return (uint8_t)cvdev_in(port, 1U);
}

static void ata_out8(uint32_t port, uint8_t value)
{
    cvdev_out(port, value, 1U);
}

static uint8_t ata_status(void)
{
    uint8_t s = ata_in8(g_status.command_base + ATA_STATUS);
    g_status.last_status = s;
    return s;
}

static uint8_t ata_alt_status(void)
{
    uint8_t s = ata_in8(g_status.control_base + ATA_CTL_ALT_STATUS);
    g_status.last_status = s;
    return s;
}

static void settle_400ns(void)
{
    uint32_t i;
    for (i = 0; i < ATA_SETTLE_READS; ++i)
        (void)ata_alt_status();
}

static uint32_t poll_drq(uint8_t *last)
{
    struct ata_deadline deadline;
    uint8_t s = 0U;
    deadline_begin(&deadline);
    do {
        s = ata_alt_status();
        if (s == 0U || s == 0xffU) {
            *last = s;
            return CVATA_E_NO_DEVICE;
        }
        if ((s & ATA_ST_BSY) != 0U) continue;
        if ((s & (ATA_ST_ERR | ATA_ST_DF)) != 0U) {
            *last = s;
            g_status.last_error = ata_in8(g_status.command_base + ATA_ERROR);
            return CVATA_E_ATA;
        }
        if ((s & ATA_ST_DRQ) != 0U) {
            *last = s;
            return CVATA_OK;
        }
    } while (!deadline_expired(&deadline));
    *last = s;
    if ((s & (ATA_ST_BSY | ATA_ST_DRQ)) != 0U) {
        g_status.state = CVATA_STATE_QUARANTINED;
        g_status.ready = 0U;
        return CVATA_E_QUARANTINED;
    }
    return CVATA_E_TIMEOUT;
}

static uint32_t poll_complete(uint8_t *last)
{
    struct ata_deadline deadline;
    uint8_t s = 0U;
    deadline_begin(&deadline);
    do {
        s = ata_alt_status();
        if (s == 0U || s == 0xffU) {
            *last = s;
            return CVATA_E_NO_DEVICE;
        }
        if ((s & ATA_ST_BSY) != 0U) continue;
        if ((s & (ATA_ST_ERR | ATA_ST_DF)) != 0U) {
            *last = s;
            g_status.last_error = ata_in8(g_status.command_base + ATA_ERROR);
            return CVATA_E_ATA;
        }
        if ((s & ATA_ST_DRQ) == 0U) {
            *last = s;
            return CVATA_OK;
        }
    } while (!deadline_expired(&deadline));
    *last = s;
    if ((s & (ATA_ST_BSY | ATA_ST_DRQ)) != 0U) {
        g_status.state = CVATA_STATE_QUARANTINED;
        g_status.ready = 0U;
        return CVATA_E_QUARANTINED;
    }
    return CVATA_E_TIMEOUT;
}

static uint32_t pic_channel_available(void)
{
    uint8_t irr, isr, bit = (uint8_t)(1U << (g_status.irq - 8U));
    ata_out8(PIC2_COMMAND, PIC_OCW3_IRR);
    irr = ata_in8(PIC2_COMMAND);
    ata_out8(PIC2_COMMAND, PIC_OCW3_ISR);
    isr = ata_in8(PIC2_COMMAND);
    ata_out8(PIC2_COMMAND, PIC_OCW3_IRR);
    if (((irr | isr) & bit) != 0U) return CVATA_E_BUSY;
    return CVATA_OK;
}

static uint32_t control_known(void)
{
    return g_status.control_shadow_known != 0U &&
           (g_status.control_shadow & (ATA_CTL_SRST | ATA_CTL_HOB)) == 0U;
}

static uint32_t enable_nien(void)
{
    if (g_control_owned) return CVATA_OK;
    if (!control_known()) return CVATA_E_UNSUPPORTED;
    g_control_saved = (uint8_t)g_status.control_shadow;
    g_control_owned = 1U;
    g_command_issued = 0U;
    ata_out8(g_status.control_base + ATA_CTL_DEVICE,
             (uint8_t)(g_control_saved | ATA_CTL_NIEN));
    return CVATA_OK;
}

/* Every exit after suppressing INTRQ goes through this function. A completed
 * command is acknowledged using Status, not Alternate Status. Never restore
 * interrupts or permit BIOS fallback with an unresolved command on the bus.
 * A selection failure before issuing a command may restore the original
 * control value without consuming someone else's pending device interrupt. */
static uint32_t transaction_finish(uint32_t error)
{
    uint8_t status;
    if (!g_control_owned) return set_error(error);
    if (g_command_issued) {
        status = ata_status();
        if (status == 0U || status == 0xffU ||
            (status & (ATA_ST_BSY | ATA_ST_DRQ)) != 0U ||
            g_status.state == CVATA_STATE_QUARANTINED) {
            g_status.state = CVATA_STATE_QUARANTINED;
            g_status.ready = 0U;
            return set_error(CVATA_E_QUARANTINED);
        }
        if ((status & (ATA_ST_ERR | ATA_ST_DF)) != 0U) {
            g_status.last_error = ata_in8(g_status.command_base + ATA_ERROR);
            if (error == CVATA_OK) error = CVATA_E_ATA;
        }
    }
    ata_out8(g_status.control_base + ATA_CTL_DEVICE, g_control_saved);
    g_control_owned = 0U;
    g_command_issued = 0U;
    return set_error(error);
}

static uint32_t select_device(uint32_t lba, uint8_t *last)
{
    uint8_t s = ata_alt_status();
    uint8_t devhead;
    uint32_t error;
    struct ata_deadline deadline;
    *last = s;
    /* An absent previously selected slave may report zero. The DPTE names
     * the device to select; verify its presence after the selection delay. */
    if (s == 0xffU) return CVATA_E_NO_DEVICE;
    if ((s & (ATA_ST_BSY | ATA_ST_DRQ)) != 0U) return CVATA_E_BUSY;
    if (!control_known()) return CVATA_E_UNSUPPORTED;
    error = enable_nien();
    if (error != CVATA_OK) return error;
    devhead = (uint8_t)((g_status.device_head & 0x10U) | 0x40U |
                        (g_status.device_head & 0xe0U) |
                        ((lba >> 24) & 0x0fU));
    ata_out8(g_status.command_base + ATA_DEVICE, devhead);
    settle_400ns();
    deadline_begin(&deadline);
    do {
        s = ata_alt_status();
        *last = s;
        if (s == 0U || s == 0xffU) return CVATA_E_NO_DEVICE;
        if ((s & (ATA_ST_BSY | ATA_ST_DRQ)) != 0U) continue;
        /* ERR/DF describe the preceding command. A new command clears ERR;
         * do not make a recoverable command error permanently disable I/O. */
        return CVATA_OK;
    } while (!deadline_expired(&deadline));
    return CVATA_E_BUSY;
}

static void identify_read(uint8_t words[512])
{
    cvata_pio_read(g_status.command_base + ATA_DATA, words);
}

static uint32_t issue_identify(uint8_t *last)
{
    uint32_t error;
    error = select_device(0U, last);
    if (error != CVATA_OK) return error;
    ata_out8(g_status.command_base + ATA_FEATURES, 0U);
    ata_out8(g_status.command_base + ATA_COUNT, 0U);
    ata_out8(g_status.command_base + ATA_LBA_LOW, 0U);
    ata_out8(g_status.command_base + ATA_LBA_MID, 0U);
    ata_out8(g_status.command_base + ATA_LBA_HIGH, 0U);
    g_command_issued = 1U;
    ata_out8(g_status.command_base + ATA_COMMAND, ATA_IDENTIFY);
    settle_400ns();
    error = poll_drq(last);
    if (error != CVATA_OK) return error;
    identify_read(g_identify);
    settle_400ns();
    error = poll_complete(last);
    if (error != CVATA_OK) return error;
    *last = ata_status();
    if ((*last & (ATA_ST_BSY | ATA_ST_DRQ | ATA_ST_ERR | ATA_ST_DF)) != 0U) {
        if ((*last & (ATA_ST_ERR | ATA_ST_DF)) != 0U)
            g_status.last_error = ata_in8(g_status.command_base + ATA_ERROR);
        return CVATA_E_ATA;
    }
    return CVATA_OK;
}

static uint32_t identify_capacity(uint32_t *capacity)
{
    uint16_t word49 = get16(g_identify + 49U * 2U);
    uint16_t word106 = get16(g_identify + 106U * 2U);
    uint32_t sectors = (uint32_t)get16(g_identify + 60U * 2U) |
                       ((uint32_t)get16(g_identify + 61U * 2U) << 16);
    uint32_t logical_words;
    if ((get16(g_identify) & 0x8080U) != 0U ||
        (word49 & (1U << 9)) == 0U || sectors == 0U)
        return CVATA_E_UNSUPPORTED;
    if ((word106 & 0xc000U) == 0x4000U && (word106 & 0x1000U) != 0U) {
        logical_words = (uint32_t)get16(g_identify + 117U * 2U) |
                        ((uint32_t)get16(g_identify + 118U * 2U) << 16);
        if (logical_words != 256U) return CVATA_E_UNSUPPORTED;
    }
    if (sectors > ATA_NATIVE28_SECTORS) sectors = ATA_NATIVE28_SECTORS;
    *capacity = sectors;
    return CVATA_OK;
}

static void reset_status(void)
{
    g_status.ready = 0U;
    g_status.state = CVATA_STATE_UNBOUND;
    g_status.error = CVATA_OK;
    g_status.command_base = 0U;
    g_status.control_base = 0U;
    g_status.device_head = 0U;
    g_status.irq = 0U;
    g_status.read_count = 0U;
    g_status.write_count = 0U;
    g_status.last_status = 0U;
    g_status.last_error = 0U;
    g_status.last_lba = 0U;
    g_status.capacity_sectors = 0U;
    g_status.logical_heads = 0U;
    g_status.sectors_per_track = 0U;
    g_status.cylinders = 0U;
    /* A new packet may select a different channel: never reuse a stale
     * Device Control value. The adapter observes the channel after prepare. */
    g_status.control_shadow_known = 0U;
    g_status.control_shadow = 0U;
}

static void discovery_clear(void)
{
    uint32_t i;
    uint8_t *p = (uint8_t *)&g_discovery;
    for (i = 0U; i < sizeof g_discovery; ++i) p[i] = 0U;
}

static uint32_t validate_parameters(const uint8_t *packet, uint32_t min_edd)
{
    uint32_t capacity, edd_bytes, heads, spt, cylinders;
    if (packet == 0) return set_error(CVATA_E_ARGUMENT);
    if (packet[0] != 'A' || packet[1] != 'D' || packet[2] != 'P' ||
        packet[3] != '1' || get16(packet + ADP_BYTES) != 64U ||
        packet[ADP_DRIVE] < 0x80U || packet[ADP_RESERVED] != 0U)
        return set_error(CVATA_E_PACKET);
    if (packet[38] != 0U || packet[39] != 0U ||
        packet[ADP_TAIL_RESERVED] != 0U || packet[63] != 0U)
        return set_error(CVATA_E_PACKET);

    edd_bytes = get16(packet + ADP_EDD);
    capacity = get32(packet + ADP_EDD + EDD_CAPACITY);
    if (edd_bytes < min_edd || get32(packet + ADP_EDD + EDD_CAPACITY + 4U) != 0U ||
        capacity == 0U || get16(packet + ADP_EDD + EDD_BYTES_PER_SECTOR) != 512U ||
        (get16(packet + ADP_EDD + 2U) & 0x0004U) != 0U)
        return set_error(CVATA_E_UNSUPPORTED);
    if (capacity > ATA_NATIVE28_SECTORS) capacity = ATA_NATIVE28_SECTORS;
    heads = get16(packet + ADP_GEOMETRY);
    spt = get16(packet + ADP_GEOMETRY + 2U);
    cylinders = get16(packet + ADP_GEOMETRY + 4U);
    if (heads == 0U || heads > 256U || spt == 0U || spt > 63U ||
        cylinders == 0U || cylinders > 1024U)
        return set_error(CVATA_E_UNSUPPORTED);

    g_clock_khz = cvata_clock_khz();
    if (g_clock_khz == 0U) return set_error(CVATA_E_UNSUPPORTED);
    /* 32x16 -> 64, without Watcom's external 64-bit arithmetic helpers. */
    {
        uint32_t lower = (g_clock_khz & 0xffffU) * ATA_TIMEOUT_MS;
        uint32_t upper = (g_clock_khz >> 16) * ATA_TIMEOUT_MS;
        g_timeout_low = lower + (upper << 16);
        g_timeout_high = (upper >> 16) + (g_timeout_low < lower);
    }
    g_status.capacity_sectors = capacity;
    g_status.logical_heads = heads;
    g_status.sectors_per_track = spt;
    g_status.cylinders = cylinders;
    return CVATA_OK;
}

static uint32_t prepare_packet(const uint8_t *packet, uint32_t min_edd)
{
    uint32_t i, sum = 0U, cmd, ctl, irq, options, revision, head, error;
    if (g_control_owned || g_status.state == CVATA_STATE_QUARANTINED)
        return set_error(CVATA_E_QUARANTINED);
    reset_status();
    error = validate_parameters(packet, min_edd);
    if (error != CVATA_OK) return error;
    for (i = 0U; i < 16U; ++i) sum = (sum + packet[ADP_DPT + i]) & 0xffU;
    if (sum != 0U) return set_error(CVATA_E_PACKET);

    cmd = get16(packet + ADP_DPT + DPT_BASE);
    ctl = get16(packet + ADP_DPT + DPT_CONTROL);
    head = packet[ADP_DPT + DPT_HEAD];
    irq = packet[ADP_DPT + DPT_IRQ];
    options = get16(packet + ADP_DPT + DPT_OPTIONS);
    revision = packet[ADP_DPT + DPT_REVISION];
    if (!((cmd == 0x1f0U && ctl == 0x3f6U && irq == 14U) ||
          (cmd == 0x170U && ctl == 0x376U && irq == 15U)))
        return set_error(CVATA_E_UNSUPPORTED);
    if ((head & 0xefU) != 0xa0U && (head & 0xefU) != 0xe0U)
        return set_error(CVATA_E_UNSUPPORTED);
    /* This is the DPTE structure revision, not the EDD installation-check
     * version. Accept the bounded legacy 10h layout found in the official
     * Lenovo T23 BIOS and the documented EDD 1.1 11h layout. Neither a ROM
     * template nor the revision supplies the required live LBA option:
     * https://download.lenovo.com/ibmdl/pub/pc/pccbbs/mobiles/1auj20us.exe
     * https://www.t10.org/ftp/t10/document.95/95-153r0.pdf */
    if ((options & 0x0010U) == 0U || (options & 0x0060U) != 0U ||
        (revision != 0x10U && revision != 0x11U))
        return set_error(CVATA_E_UNSUPPORTED);
    if (packet[ADP_DRIVE] < 0x80U) return set_error(CVATA_E_UNSUPPORTED);

    g_status.command_base = cmd;
    g_status.control_base = ctl;
    g_status.device_head = head;
    g_status.irq = irq;
    g_status.state = CVATA_STATE_PREPARED;
    return set_error(CVATA_OK);
}

uint32_t cvata_prepare(const uint8_t *packet)
{
    discovery_clear();
    return prepare_packet(packet, EDD_MIN_BYTES);
}

uint32_t cvata_discovery_begin(const uint8_t *packet)
{
    uint32_t i, error;
    if (g_control_owned || g_status.state == CVATA_STATE_QUARANTINED)
        return set_error(CVATA_E_QUARANTINED);
    if (g_status.state != CVATA_STATE_UNBOUND) return set_error(CVATA_E_BUSY);
    discovery_clear();
    reset_status();
    error = validate_parameters(packet, 26U);
    if (error != CVATA_OK) return error;
    for (i = 0U; i < 64U; ++i) g_discovery.packet[i] = packet[i];
    g_discovery.active = 1U;
    return set_error(CVATA_OK);
}

void cvata_discovery_cancel(void)
{
    uint32_t active = g_discovery.active;
    discovery_clear();
    if (active && !g_control_owned && g_status.state == CVATA_STATE_UNBOUND)
        reset_status();
}

void cvata_discovery_io(uint32_t port, uint32_t value, uint32_t width)
{
    uint32_t channel, reg;
    uint8_t v = (uint8_t)value;
    if (!g_discovery.active || g_discovery.rejected) return;
    if (port >= 0x1f2U && port <= 0x1f7U) {
        channel = 0U; reg = port - 0x1f0U;
    } else if (port >= 0x172U && port <= 0x177U) {
        channel = 1U; reg = port - 0x170U;
    } else if (port == 0x3f6U || port == 0x376U) {
        channel = port == 0x3f6U ? 0U : 1U; reg = 8U;
    } else return;
    if (width != 1U) { g_discovery.rejected = 1U; return; }
    if (reg == 8U) {
        g_discovery.control[channel] = v;
        g_discovery.control_known[channel] =
            (v & (ATA_CTL_SRST | ATA_CTL_HOB)) == 0U;
        if ((v & ATA_CTL_SRST) != 0U) {
            g_discovery.seen[channel] = 0U;
            if (g_discovery.reads != 0U) g_discovery.rejected = 1U;
        }
        return;
    }
    if (reg < ATA_COMMAND) {
        /* Setup may address both devices/channels during the BIOS reset,
         * but no later output may change the established read's route. */
        if (g_discovery.reads != 0U &&
            (channel != g_discovery.channel ||
             (reg == ATA_DEVICE && (v & 0x10U) != (g_discovery.head & 0x10U)))) {
            g_discovery.rejected = 1U; return;
        }
        g_discovery.task[channel][reg - ATA_COUNT] = v;
        g_discovery.seen[channel] |= (uint8_t)(1U << (reg - ATA_COUNT));
        return;
    }
    /* Initialization commands are not identity evidence. Fresh task-file
     * writes are mandatory after every such command and every read. */
    if (v == 0xc6U || v == 0x91U || v == 0xefU ||
        v == ATA_IDENTIFY || v == 0x08U) {
        g_discovery.seen[0] = g_discovery.seen[1] = 0U;
        if (g_discovery.reads != 0U) g_discovery.rejected = 1U;
        return;
    }
    /* ATA/ATAPI-6 READ SECTORS, READ MULTIPLE and READ DMA share this
     * 28-bit task-file tuple. DMA is only observed, never implemented here.
     * Write, CHS, queued and extended commands cannot establish a mapping. */
    if ((v != ATA_READ && v != 0xc4U && v != 0xc8U) ||
        g_discovery.seen[channel] != 0x1fU ||
        g_discovery.task[channel][0] != 1U ||
        g_discovery.task[channel][1] != 0U ||
        g_discovery.task[channel][2] != 0U ||
        g_discovery.task[channel][3] != 0U ||
        (g_discovery.task[channel][4] != 0xe0U &&
         g_discovery.task[channel][4] != 0xf0U) ||
        !g_discovery.control_known[channel] ||
        (g_discovery.reads != 0U &&
         (channel != g_discovery.channel ||
          g_discovery.task[channel][4] != g_discovery.head))) {
        g_discovery.rejected = 1U; return;
    }
    g_discovery.channel = (uint8_t)channel;
    g_discovery.head = g_discovery.task[channel][4];
    ++g_discovery.reads;
    g_discovery.seen[channel] = 0U;
}

uint32_t cvata_discovery_end(uint8_t *packet)
{
    uint32_t i, channel, sum = 0U, error;
    uint8_t control;
    if (g_control_owned || g_status.state == CVATA_STATE_QUARANTINED)
        return set_error(CVATA_E_QUARANTINED);
    if (packet == 0) return set_error(CVATA_E_ARGUMENT);
    if (!g_discovery.active) return set_error(CVATA_UNBOUND);
    g_discovery.active = 0U;
    channel = g_discovery.channel;
    if (g_discovery.rejected || g_discovery.reads == 0U ||
        !g_discovery.control_known[channel])
        return set_error(CVATA_E_UNSUPPORTED);
    for (i = 0U; i < 64U; ++i) {
        if (i >= ADP_DPT && i < ADP_DPT + 16U) continue;
        if (packet[i] != g_discovery.packet[i]) return set_error(CVATA_E_PACKET);
    }
    /* These are internal ADP1 mapping bytes, not a claimed firmware DPTE.
     * Raw EDD length and pointer remain exactly as returned by the BIOS. */
    for (i = 0U; i < 16U; ++i) packet[ADP_DPT + i] = 0U;
    packet[ADP_DPT + DPT_BASE] = channel == 0U ? 0xf0U : 0x70U;
    packet[ADP_DPT + DPT_BASE + 1U] = 1U;
    packet[ADP_DPT + DPT_CONTROL] = 0x76U;
    if (channel == 0U) packet[ADP_DPT + DPT_CONTROL] = 0xf6U;
    packet[ADP_DPT + DPT_CONTROL + 1U] = 3U;
    packet[ADP_DPT + DPT_HEAD] = g_discovery.head;
    packet[ADP_DPT + DPT_IRQ] = channel == 0U ? 14U : 15U;
    packet[ADP_DPT + DPT_OPTIONS] = 0x10U;
    packet[ADP_DPT + DPT_REVISION] = 0x11U;
    for (i = 0U; i < 16U; ++i) sum += packet[ADP_DPT + i];
    packet[ADP_DPT + DPT_CHECKSUM] = (uint8_t)(0U - sum);
    control = g_discovery.control[channel];
    error = prepare_packet(packet, 26U);
    if (error == CVATA_OK) cvata_observe_control(control, 1U);
    return error;
}

void cvata_observe_control(uint32_t value, uint32_t width)
{
    if (width != 1U || (value & (ATA_CTL_SRST | ATA_CTL_HOB)) != 0U) {
        g_status.control_shadow_known = 0U;
        return;
    }
    g_status.control_shadow = value & 0xffU;
    g_status.control_shadow_known = 1U;
}

uint32_t cvata_bind(const uint8_t *bios_sector)
{
    uint8_t last = 0U;
    uint32_t error, native_capacity;
    uint32_t i;
    if (bios_sector == 0) return set_error(CVATA_E_ARGUMENT);
    if (g_status.state == CVATA_STATE_QUARANTINED)
        return set_error(CVATA_E_QUARANTINED);
    if (g_status.state != CVATA_STATE_PREPARED)
        return set_error(CVATA_UNBOUND);
    if (!control_known()) return set_error(CVATA_E_UNSUPPORTED);
    error = pic_channel_available();
    if (error != CVATA_OK) return set_error(error);
    last = ata_alt_status();
    g_status.last_status = last;
    if (last == 0xffU) return set_error(CVATA_E_NO_DEVICE);
    if ((last & (ATA_ST_BSY | ATA_ST_DRQ)) != 0U) return set_error(CVATA_E_BUSY);
    error = issue_identify(&last);
    if (error != CVATA_OK) return transaction_finish(error);
    error = identify_capacity(&native_capacity);
    if (error != CVATA_OK) return transaction_finish(error);
    if (native_capacity < g_status.capacity_sectors)
        return transaction_finish(CVATA_E_UNSUPPORTED);
    /* issue_identify consumed all 256 words and acknowledged completion. */
    error = select_device(0U, &last);
    if (error != CVATA_OK) return transaction_finish(error);
    ata_out8(g_status.command_base + ATA_COUNT, 1U);
    ata_out8(g_status.command_base + ATA_LBA_LOW, 0U);
    ata_out8(g_status.command_base + ATA_LBA_MID, 0U);
    ata_out8(g_status.command_base + ATA_LBA_HIGH, 0U);
    g_command_issued = 1U;
    ata_out8(g_status.command_base + ATA_COMMAND, ATA_READ);
    settle_400ns();
    error = poll_drq(&last);
    if (error != CVATA_OK) return transaction_finish(error);
    cvata_pio_read(g_status.command_base + ATA_DATA, g_bind_sector);
    settle_400ns();
    error = poll_complete(&last);
    if (error != CVATA_OK) return transaction_finish(error);
    last = ata_status();
    if ((last & (ATA_ST_BSY | ATA_ST_DRQ | ATA_ST_ERR | ATA_ST_DF)) != 0U)
        return transaction_finish(CVATA_E_ATA);
    for (i = 0U; i < 512U; ++i)
        if (g_bind_sector[i] != bios_sector[i]) return transaction_finish(CVATA_E_UNSUPPORTED);
    error = transaction_finish(CVATA_OK);
    if (error != CVATA_OK) return error;
    g_status.state = CVATA_STATE_BOUND;
    g_status.ready = 1U;
    g_status.error = CVATA_OK;
    g_status.last_lba = 0U;
    return CVATA_OK;
}

uint32_t cvata_transfer(uint32_t write, uint32_t lba, uint8_t *buffer)
{
    uint32_t error;
    uint8_t last = 0U;
    uint8_t status;
    if (buffer == 0 || write > 1U) return set_error(CVATA_E_ARGUMENT);
    if (g_status.state == CVATA_STATE_QUARANTINED)
        return set_error(CVATA_E_QUARANTINED);
    if (g_status.state != CVATA_STATE_BOUND || g_status.ready == 0U)
        return set_error(CVATA_UNBOUND);
    if (lba > ATA_NATIVE28_LAST || lba >= g_status.capacity_sectors)
        return set_error(CVATA_E_RANGE);
    error = pic_channel_available();
    if (error != CVATA_OK) return transaction_finish(error);
    error = select_device(lba, &last);
    if (error != CVATA_OK) return transaction_finish(error);
    ata_out8(g_status.command_base + ATA_COUNT, 1U);
    ata_out8(g_status.command_base + ATA_LBA_LOW, (uint8_t)lba);
    ata_out8(g_status.command_base + ATA_LBA_MID, (uint8_t)(lba >> 8));
    ata_out8(g_status.command_base + ATA_LBA_HIGH, (uint8_t)(lba >> 16));
    g_status.last_lba = lba;
    g_command_issued = 1U;
    if (write != 0U) {
        ata_out8(g_status.command_base + ATA_COMMAND, ATA_WRITE);
        settle_400ns();
        error = poll_drq(&last);
        if (error != CVATA_OK) return transaction_finish(error);
        cvata_pio_write(g_status.command_base + ATA_DATA, buffer);
    } else {
        ata_out8(g_status.command_base + ATA_COMMAND, ATA_READ);
        settle_400ns();
        error = poll_drq(&last);
        if (error != CVATA_OK) return transaction_finish(error);
        cvata_pio_read(g_status.command_base + ATA_DATA, buffer);
    }
    settle_400ns();
    error = poll_complete(&last);
    if (error != CVATA_OK) return transaction_finish(error);
    status = ata_status();
    if ((status & (ATA_ST_BSY | ATA_ST_DRQ | ATA_ST_ERR | ATA_ST_DF)) != 0U) {
        if ((status & (ATA_ST_ERR | ATA_ST_DF)) != 0U)
            g_status.last_error = ata_in8(g_status.command_base + ATA_ERROR);
        return transaction_finish(CVATA_E_ATA);
    }
    error = transaction_finish(CVATA_OK);
    if (error != CVATA_OK) return error;
    if (write != 0U) ++g_status.write_count;
    else ++g_status.read_count;
    return set_error(CVATA_OK);
}

const struct cvata_status *cvata_get_status(void)
{
    return &g_status;
}
