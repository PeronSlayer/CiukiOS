/* Legacy ATA PIO, one sleepable transaction per channel, never BIOS/retry.
 * Research: T13 ATA/ATAPI-6 d1410r3a, sections 6.20, 7, 8.12-8.15,
 * 8.34-8.35, 8.62-8.63, 9.2, 9.5-9.6, 9.12 and 10.2.2 (table 67):
 * https://www.read.seas.harvard.edu/~kohler/class/04f-aos/ref/hardware/ATA-d1410r3a.pdf
 * The existing sync.c wait queue enqueues/checks atomically; trap.c owns
 * the PIC EOI and spurious IRQ15. Only Status reads acknowledge INTRQ;
 * polling uses Alternate Status. No legacy VM/PIC implementation is copied.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/arch.h>
#include <ciuki/task.h>
#include <ciuki/registry.h>
#include <ciuki/work.h>
#include <ciuki/ata.h>

#define ATA_ERR 0x01u
#define ATA_DRQ 0x08u
#define ATA_DF  0x20u
#define ATA_DRDY 0x40u
#define ATA_BSY 0x80u
#define ATA_NIEN 0x02u
#define ATA_SRST 0x04u
#define ATA_IDENTIFY 0xecu

static struct ata_channel channels[ATA_CHANNELS];
static bool initialized;
static const char *const owners[ATA_CHANNELS] = { "ata0", "ata1" };

static uint8_t native_in8(void *ctx, uint16_t p) { (void)ctx; return inb(p); }
static void native_out8(void *ctx, uint16_t p, uint8_t v) { (void)ctx; outb(p, v); }
static uint16_t native_in16(void *ctx, uint16_t p)
{
    (void)ctx;
    uint16_t v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(p));
    return v;
}
static void native_out16(void *ctx, uint16_t p, uint16_t v)
{
    (void)ctx;
    __asm__ volatile("outw %0, %1" :: "a"(v), "Nd"(p));
}
static uint64_t native_now(void *ctx) { (void)ctx; return deadline_after_ms(0); }
static uint64_t native_deadline(void *ctx, uint64_t d) { (void)ctx; return d; }
static int native_delay(void *ctx, uint32_t us) { (void)ctx; return udelay(us); }
static const struct ata_port_ops native_ops = {
    native_in8, native_out8, native_in16, native_out16, native_now, native_deadline, native_delay,
};

static uint8_t reg_read(struct ata_channel *c, unsigned r)
{
    return c->ops->in8(c->port_ctx, (uint16_t)(c->base + r));
}
static void reg_write(struct ata_channel *c, unsigned r, uint8_t v)
{
    c->ops->out8(c->port_ctx, (uint16_t)(c->base + r), v);
}
static uint8_t alternate(struct ata_channel *c)
{
    return c->ops->in8(c->port_ctx, c->control);
}
static void device_control(struct ata_channel *c, uint8_t v)
{
    c->control_shadow = v;
    c->ops->out8(c->port_ctx, c->control, v);
}
static uint64_t now(struct ata_channel *c) { return c->ops->now_ms(c->port_ctx); }
static bool expired(struct ata_channel *c, uint64_t d) { return now(c) - d < (1ull << 63); }
static int pace(struct ata_channel *c)
{
    /* 9.5/9.6: >=400 ns after Command, >=one PIO cycle after a data
     * block. udelay has microsecond granularity: round UP to 1 us.
     * Table 67's slowest t0 is 600 ns; also pace individual data words.
     * Chipset DIOR-/DIOW-/IORDY timing remains firmware configured. */
    return c->ops->delay_us(c->port_ctx, 1);
}

int ata_channel_setup(struct ata_channel *c, unsigned index, const struct ata_port_ops *ops, void *ctx)
{
    if (!c || index >= ATA_CHANNELS || !ops || !ops->in8 || !ops->out8 || !ops->in16 ||
        !ops->out16 || !ops->now_ms || !ops->deadline_ticks || !ops->delay_us)
        return -FS_EINVAL;
    if ((c == &channels[0] || c == &channels[1]) && (c->native || c->discovered || c->stopped))
        return -FS_EQUARANTINED;    /* exported fake setup cannot rearm a real controller */
    memset(c, 0, sizeof(*c));
    c->ops = ops;
    c->port_ctx = ctx;
    c->base = index ? 0x170 : 0x1f0;
    c->control = index ? 0x376 : 0x3f6;
    c->irq = index ? 15 : 14;
    c->control_shadow = ATA_NIEN;   /* ATA-6: all other implemented bits zero */
    kmutex_init(&c->mutex);
    kwait_init(&c->wait);
    for (unsigned i = 0; i < 3; i++)
        c->claims[i] = -1;
    for (unsigned i = 0; i < ATA_DEVICES; i++) {
        c->devices[i].channel = c;
        c->devices[i].unit = i;
        c->devices[i].block.ctx = &c->devices[i];
        c->devices[i].block.sector_size = BLKDEV_SECTOR_SIZE;
    }
    return 0;
}

void ata_channel_irq(struct ata_channel *c)
{
    /* Always acknowledge the device, including an unarmed late edge. Never
     * transfer data, log, allocate, sleep or send PIC EOI here. */
    uint8_t status = reg_read(c, 7);
    if (!c->active || c->stopped || c->quiescing || !c->request_generation ||
        (c->native && !registry_valid(c->claims[2], c->generations[2])))
        return;
    c->irq_status = status;
    c->irq_generation = c->request_generation;
    c->irq_sequence++;
    kwait_wake_all(&c->wait);
}

static void primary_irq(struct trap_frame *tf) { (void)tf; ata_channel_irq(&channels[0]); }
static void secondary_irq(struct trap_frame *tf) { (void)tf; ata_channel_irq(&channels[1]); }

static int failure(struct ata_device *d, uint64_t start, uint8_t status)
{
    struct ata_channel *c = d->channel;
    d->status = status;
    d->error = reg_read(c, 1);       /* diagnostic only while BSY is set */
    d->elapsed_ms = now(c) - start;
    /* f1-acceptance/storage ownership quarantines the entire controller
     * after an issued failure, including IDENTIFY/FLUSH. This also covers
     * stuck BSY: never select/reset a sibling on an uncertain channel. */
    c->stopped = true;
    for (unsigned i = 0; i < ATA_DEVICES; i++)
        c->devices[i].block.quarantined = true;
    device_control(c, c->control_shadow | ATA_NIEN);
    if (c->native) {
        pic_mask(c->irq);
        for (unsigned i = 0; i < 3; i++)
            if (c->claims[i] >= 0)
                registry_quarantine(c->claims[i], c->generations[i]);
        klog("[ata%u] fault unit=%u command=%02x issued=%u status=%02x error=%02x elapsed_ms=%llu",
             c->irq - 14, d->unit, d->command, d->issued, d->status, d->error, d->elapsed_ms);
    }
    return -FS_EIO;
}

/* Poll only bounded status transitions at reset/selection or within a
 * data phase. BSY makes every other status bit invalid (7.15). Sleep after
 * short sampling so a stuck device cannot monopolize this UP kernel. */
static int poll_status(struct ata_device *d, uint64_t start, uint64_t deadline,
                       uint8_t required, uint8_t forbidden)
{
    struct ata_channel *c = d->channel;
    for (unsigned samples = 0;; samples++) {
        uint8_t s = alternate(c);
        d->status = s;
        if (c->stopped || c->quiescing || expired(c, deadline))
            return failure(d, start, s);
        if (!(s & ATA_BSY)) {
            if (!s || s == 0xff || (s & (ATA_ERR | ATA_DF)))
                return failure(d, start, s);
            if ((s & required) == required && !(s & forbidden))
                return 0;
        }
        if (pace(c))
            return failure(d, start, s);
        if ((samples & 31) == 31)
            task_sleep_ms(1);
    }
}

struct irq_wait { struct ata_channel *channel; uint32_t sequence; gen_t generation; };
static bool irq_ready(void *arg)
{
    struct irq_wait *w = arg;
    return w->channel->stopped || w->channel->quiescing ||
        (w->channel->irq_sequence != w->sequence && w->channel->irq_generation == w->generation);
}

static int wait_irq(struct ata_device *d, uint32_t *sequence, uint64_t start, uint64_t deadline)
{
    struct ata_channel *c = d->channel;
    struct irq_wait w = { c, *sequence, c->request_generation };
    if (expired(c, deadline) || !kwait_wait_until(&c->wait, irq_ready, &w,
                                                c->ops->deadline_ticks(c->port_ctx, deadline)))
        return failure(d, start, alternate(c));
    uint32_t f = irq_save();
    uint8_t s = c->irq_status;
    *sequence = c->irq_sequence;
    irq_restore(f);
    d->status = s;
    if (c->stopped || c->quiescing || expired(c, deadline) ||
        (!(s & ATA_BSY) && (s & (ATA_ERR | ATA_DF))))
        return failure(d, start, s);
    return 0;
}

static int select_device(struct ata_device *d, uint64_t start, uint64_t deadline, uint8_t head)
{
    struct ata_channel *c = d->channel;
    /* The previous selection may be an absent slave or skipped ATAPI.
     * Its non-BSY status is not the requested ATA device's ending status. */
    for (;;) {
        uint8_t s = alternate(c);
        if (s == 0xff || !(s & (ATA_BSY | ATA_DRQ)))
            break;
        if (expired(c, deadline))
            return failure(d, start, s);
        task_sleep_ms(1);
    }
    reg_write(c, 6, (uint8_t)(0xa0 | d->unit << 4 | head));
    if (pace(c))
        return failure(d, start, alternate(c));
    return poll_status(d, start, deadline, ATA_DRDY, ATA_DRQ);
}

static int issue(struct ata_device *d, uint8_t command, uint64_t start, uint32_t *sequence)
{
    struct ata_channel *c = d->channel;
    /* Drain any old source before publishing a fresh generation. The IRQ
     * condition remains latched even if the edge precedes wait enqueue. */
    reg_read(c, 7);
    gen_t generation = gen_alloc();
    if (!generation)
        return failure(d, start, alternate(c));
    uint32_t f = irq_save();
    c->active = d;
    c->request_generation = generation;
    d->generation = generation;
    *sequence = c->irq_sequence;
    d->command = command;
    d->issued = true;
    d->commands++;
    c->commands++;
    reg_write(c, 7, command);
    irq_restore(f);
    if (pace(c))
        return failure(d, start, alternate(c));
    return 0;
}

static void disarm(struct ata_channel *c)
{
    uint32_t f = irq_save();
    c->active = 0;
    c->request_generation = 0;
    irq_restore(f);
    device_control(c, c->control_shadow | ATA_NIEN);
    if (c->native)
        pic_mask(c->irq);
}

static int data_block(struct ata_device *d, uint8_t *buf, bool write, uint64_t start, uint64_t deadline)
{
    struct ata_channel *c = d->channel;
    for (unsigned i = 0; i < 256; i++) {
        if (expired(c, deadline))
            return failure(d, start, alternate(c));
        if (write)
            c->ops->out16(c->port_ctx, c->base, fs_rd16(buf + i * 2));
        else
            fs_wr16(buf + i * 2, c->ops->in16(c->port_ctx, c->base));
        if (pace(c))
            return failure(d, start, alternate(c));
    }
    return 0;
}

static int ata_flush(struct blkdev *b);
static int ata_read(struct blkdev *b, uint64_t lba, uint32_t n, void *buf);
static int ata_write(struct blkdev *b, uint64_t lba, uint32_t n, const void *buf);

static int identify(struct ata_device *d)
{
    struct ata_channel *c = d->channel;
    uint8_t data[512];
    uint64_t start = now(c), deadline = start + ATA_COMMAND_MS;
    uint32_t sequence;
    d->issued = false;
    int e = select_device(d, start, deadline, 0);
    if (!e)
        e = issue(d, ATA_IDENTIFY, start, &sequence);
    if (!e)
        e = wait_irq(d, &sequence, start, deadline);
    if (!e)
        e = poll_status(d, start, deadline, ATA_DRQ, 0);
    if (!e)
        e = data_block(d, data, false, start, deadline);
    /* 9.5 HPIOI2:HI0: the final data read ends the command; there is
     * NO extra completion IRQ. Observe post-data BSY/DRQ after pacing. */
    if (!e)
        e = poll_status(d, start, deadline, 0, ATA_DRQ);
    disarm(c);
    if (e)
        return e;
    uint16_t w49 = fs_rd16(data + 49 * 2), w83 = fs_rd16(data + 83 * 2);
    uint16_t w82 = fs_rd16(data + 82 * 2), w85 = fs_rd16(data + 85 * 2);
    uint16_t w87 = fs_rd16(data + 87 * 2), w106 = fs_rd16(data + 106 * 2);
    bool supported_valid = (w83 & 0xc000) == 0x4000;
    bool enabled_valid = (w87 & 0xc000) == 0x4000;
    d->lba48 = supported_valid && (w83 & (1u << 10));
    uint64_t cap = fs_rd32(data + 60 * 2);
    if (d->lba48)
        cap = fs_rd32(data + 100 * 2) | (uint64_t)fs_rd32(data + 102 * 2) << 32;
    /* ATA-6 uses 512-byte sectors. Also reject a later declaration of
     * longer logical sectors: ATA8-ACS, word 106 bit 12 (words 117-118),
     * https://www.t13.org/system/files/Project%20Drafts/2008/D1699r6-ATA8-ACS_2.pdf
     * An asserted bit 12 declares >256 words, even if its length is bad. */
    if (!(w49 & (1u << 9)) || !cap || cap > (d->lba48 ? (1ull << 48) : (1ull << 28)) ||
        (fs_rd16(data) & 0x8000) ||
        ((w106 & 0xc000) == 0x4000 && (w106 & (1u << 12))))
        return failure(d, start, d->status);
    for (unsigned i = 0; i < 40; i++) {
        uint8_t ch = data[54 + (i ^ 1u)];
        d->model[i] = ch >= 33 && ch <= 126 && ch != '=' ? (char)ch : '_';
    }
    unsigned len = 40;
    while (len && d->model[len - 1] == '_')
        len--;
    d->model[len] = 0;
    if (!len)
        memcpy(d->model, "unknown", 8);
    enum blkdev_cache_state cache = BLKDEV_CACHE_UNKNOWN;
    /* 8.15.42/43: honour validity bits before interpreting words 82/85. */
    if (supported_valid && !(w82 & (1u << 5)))
        cache = BLKDEV_CACHE_DISABLED;
    else if (supported_valid && enabled_valid)
        cache = w85 & (1u << 5) ? BLKDEV_CACHE_ENABLED : BLKDEV_CACHE_DISABLED;
    d->flush_ext = d->lba48 && (w83 & (1u << 13));
    d->block.capacity = cap;
    d->block.write_cache_state = cache;
    d->block.read = ata_read;
    d->block.write = ata_write;
    d->block.flush = supported_valid && ((w83 & (1u << 12)) || d->flush_ext) ? ata_flush : 0;
    d->identified = true;
    d->elapsed_ms = now(c) - start;
    return 0;
}

int ata_channel_discover(struct ata_channel *c)
{
    if (!c || !c->ops)
        return -FS_EINVAL;
    kmutex_lock(&c->mutex);
    if (c->discovered || c->stopped || c->quiescing) {
        int e = c->stopped || c->quiescing ? -FS_EQUARANTINED : 0;
        kmutex_unlock(&c->mutex);
        return e;
    }
    c->discovered = true;
    /* 9.2: SRST >=5 us, then >=2 ms before inspecting reset signature.
     * Discovery is the only reset; failures never rearm native channels. */
    device_control(c, c->control_shadow | ATA_NIEN | ATA_SRST);
    int e = c->ops->delay_us(c->port_ctx, 5);
    device_control(c, (uint8_t)(c->control_shadow & ~ATA_SRST));
    task_sleep_ms(2);
    uint64_t start = now(c), deadline = start + ATA_COMMAND_MS;
    int found = 0;
    for (unsigned i = 0; i < ATA_DEVICES && !e; i++) {
        struct ata_device *d = &c->devices[i];
        /* Do not select another device while reset is still busy. */
        for (uint8_t s = alternate(c); s & ATA_BSY && s != 0xff; s = alternate(c)) {
            if (expired(c, deadline)) {
                e = failure(d, start, s);
                break;
            }
            task_sleep_ms(1);
        }
        if (e)
            break;
        reg_write(c, 6, (uint8_t)(0xa0 | i << 4));
        if (pace(c)) { e = failure(d, start, alternate(c)); break; }
        uint8_t s = alternate(c);
        if (!s || s == 0xff)
            continue;
        while (s & ATA_BSY) {
            if (expired(c, deadline)) { e = failure(d, start, s); break; }
            task_sleep_ms(1);
            s = alternate(c);
        }
        if (e)
            break;
        /* 9.12: SC=1/LBA Low=1, ATA mid/high=00/00; ATAPI=14/EB. */
        if (reg_read(c, 2) != 1 || reg_read(c, 3) != 1)
            continue;
        uint8_t mid = reg_read(c, 4), high = reg_read(c, 5);
        if (mid == 0x14 && high == 0xeb) {
            d->present = d->atapi = true;
            if (c->native)
                klog("[ata%u] unit=%u kind=atapi skipped=F4", c->irq - 14, i);
            continue;
        }
        if (mid || high)
            continue;
        d->present = true;
        /* Edge handler is published before native discovery; nIEN is
         * cleared only for an ATA IDENTIFY, never for skipped ATAPI. */
        device_control(c, (uint8_t)(c->control_shadow & ~ATA_NIEN));
        if (c->native)
            pic_unmask(c->irq);
        e = identify(d);
        device_control(c, c->control_shadow | ATA_NIEN);
        if (c->native)
            pic_mask(c->irq);
        if (!e)
            found++;
    }
    if (e && !c->stopped)
        e = failure(&c->devices[0], start, alternate(c));
    kmutex_unlock(&c->mutex);
    return e ? e : found;
}

static int command_data(struct ata_device *d, uint64_t lba, uint32_t n, uint8_t *buf, bool write)
{
    struct ata_channel *c = d->channel;
    if (c->stopped || c->quiescing)
        return -FS_EQUARANTINED;
    uint64_t start = now(c), deadline = start + ATA_COMMAND_MS;
    uint32_t sequence;
    d->issued = false;
    uint8_t head = (uint8_t)(0x40 | (d->lba48 ? 0 : (lba >> 24) & 15));
    int e = select_device(d, start, deadline, head);
    if (e)
        return e;
    device_control(c, (uint8_t)(c->control_shadow & ~ATA_NIEN));
    if (c->native)
        pic_unmask(c->irq);
    /* 6.20/8.35/8.63: LBA48 high-order task file first, then low-order.
     * 256 encodes as 00/01 for EXT and 00 (meaning 256) for LBA28. */
    if (d->lba48) {
        reg_write(c, 1, 0);
        reg_write(c, 2, (uint8_t)(n >> 8));
        reg_write(c, 3, (uint8_t)(lba >> 24));
        reg_write(c, 4, (uint8_t)(lba >> 32));
        reg_write(c, 5, (uint8_t)(lba >> 40));
    }
    reg_write(c, 1, 0);
    reg_write(c, 2, (uint8_t)n);
    reg_write(c, 3, (uint8_t)lba);
    reg_write(c, 4, (uint8_t)(lba >> 8));
    reg_write(c, 5, (uint8_t)(lba >> 16));
    e = issue(d, write ? (d->lba48 ? 0x34 : 0x30) : (d->lba48 ? 0x24 : 0x20), start, &sequence);
    for (unsigned i = 0; i < n && !e; i++) {
        /* 9.6: first WRITE DRQ is NOT accompanied by INTRQ. All READ
         * blocks and subsequent WRITE blocks require an IRQ. */
        if (!write || i)
            e = wait_irq(d, &sequence, start, deadline);
        if (!e)
            e = poll_status(d, start, deadline, ATA_DRQ, 0);
        if (!e)
            e = data_block(d, buf + i * 512, write, start, deadline);
        if (!e && i + 1 < n)
            task_yield();
    }
    if (!e && write)
        e = wait_irq(d, &sequence, start, deadline);
    if (!e)
        e = poll_status(d, start, deadline, 0, ATA_DRQ);
    disarm(c);
    d->elapsed_ms = now(c) - start;
    return e;
}

static int ata_transfer(struct blkdev *b, uint64_t lba, uint32_t n, void *buf, bool write)
{
    int e = blkdev_range(b, lba, n);
    size_t bytes = (size_t)n * BLKDEV_SECTOR_SIZE;
    if (e || !buf || bytes / BLKDEV_SECTOR_SIZE != n)
        return e ? e : -FS_EINVAL;
    struct ata_device *d = b->ctx;
    struct ata_channel *c = d->channel;
    kmutex_lock(&c->mutex);
    /* Recheck after sleeping behind a failed request. No port write on
     * invalid/quarantined requests, including requests queued on a sibling. */
    e = blkdev_range(b, lba, n);
    if (!e && (!d->identified || c->stopped || c->quiescing))
        e = -FS_EQUARANTINED;
    uint8_t *p = buf;
    while (!e && n) {
        uint32_t chunk = n > 256 ? 256 : n;
        e = command_data(d, lba, chunk, p, write);
        if (e)
            break;
        lba += chunk;
        n -= chunk;
        p += chunk * BLKDEV_SECTOR_SIZE;
        if (n)
            task_yield();
    }
    kmutex_unlock(&c->mutex);
    return e;
}

static int ata_read(struct blkdev *b, uint64_t lba, uint32_t n, void *buf)
{
    return ata_transfer(b, lba, n, buf, false);
}
static int ata_write(struct blkdev *b, uint64_t lba, uint32_t n, const void *buf)
{
    return ata_transfer(b, lba, n, (void *)buf, true);
}
static int ata_flush(struct blkdev *b)
{
    struct ata_device *d = b->ctx;
    struct ata_channel *c = d->channel;
    kmutex_lock(&c->mutex);
    int e = b->quarantined || c->stopped || c->quiescing ? -FS_EQUARANTINED : 0;
    if (!e) {
        /* 8.12.8 permits >30 s; f1-acceptance sets a 60 s flush limit. */
        uint64_t start = now(c), deadline = start + ATA_FLUSH_MS;
        uint32_t sequence;
        d->issued = false;
        e = select_device(d, start, deadline, 0);
        if (!e) {
            device_control(c, (uint8_t)(c->control_shadow & ~ATA_NIEN));
            if (c->native)
                pic_unmask(c->irq);
            e = issue(d, d->flush_ext ? 0xea : 0xe7, start, &sequence);
        }
        if (!e)
            e = wait_irq(d, &sequence, start, deadline);
        if (!e)
            e = poll_status(d, start, deadline, 0, ATA_DRQ);
        disarm(c);
        d->elapsed_ms = now(c) - start;
    }
    kmutex_unlock(&c->mutex);
    return e;
}

bool ata_channel_idle(struct ata_channel *c)
{
    if (!c)
        return false;
    c->quiescing = true;            /* rejects even submissions queued on the mutex */
    device_control(c, c->control_shadow | ATA_NIEN);
    if (c->native)
        pic_mask(c->irq);
    kwait_wake_all(&c->wait);        /* an issued waiter aborts, never issues another command */
    if (c->active || c->mutex.owner)
        return false;
    /* No queued driver callbacks or DMA exist. An uncertain busy device
     * cannot supply a release proof; quarantine retains its claims. */
    bool idle = !c->stopped && !(alternate(c) & ATA_BSY);
    if (idle && c->native)
        irq_set_handler(c->irq, 0); /* no callback may outlive a released edge claim */
    return idle;
}

bool ata_registry_idle(int handle, gen_t generation)
{
    for (unsigned i = 0; i < ATA_CHANNELS; i++) {
        struct ata_channel *c = &channels[i];
        for (unsigned j = 0; j < 3; j++)
            if (c->native && c->claims[j] == handle && gen_matches(c->generations[j], generation))
                return ata_channel_idle(c);
    }
    return false;
}

static int claim_channel(struct ata_channel *c, unsigned index)
{
    const enum res_type types[3] = { RES_PORT, RES_PORT, RES_IRQ };
    uint32_t starts[3] = { c->base, c->control, c->irq };
    uint32_t ends[3] = { c->base + 8u, c->control + 1u, c->irq + 1u };
    for (unsigned i = 0; i < 3; i++) {
        int h = registry_claim(types[i], starts[i], ends[i], owners[index], false);
        if (h < 0) {
            /* Registry has no CLAIMED -> RELEASED rollback interface.
             * Retain/quarantine partial claims; conflict causes no I/O. */
            for (unsigned j = 0; j < i; j++)
                registry_quarantine(c->claims[j], c->generations[j]);
            return h;
        }
        c->claims[i] = h;
        c->generations[i] = registry_get((unsigned)h)->generation;
    }
    return 0;
}

int ata_init(void)
{
    if (initialized)
        return 0;
    if (!(read_eflags() & 0x200) || !g_current)
        return -FS_EINVAL;
    int e = kwork_init();
    if (e)
        return e;
    initialized = true;
    for (unsigned i = 0; i < ATA_CHANNELS; i++) {
        struct ata_channel *c = &channels[i];
        ata_channel_setup(c, i, &native_ops, 0);
        if (claim_channel(c, i)) {
            c->stopped = true;
            continue;
        }
        c->native = true;
        pic_mask(c->irq);
        irq_set_handler(c->irq, i ? secondary_irq : primary_irq);
        int found = ata_channel_discover(c);
        if (found > 0) {
            for (unsigned j = 0; j < 3; j++)
                if (registry_activate(c->claims[j], c->generations[j])) {
                    failure(&c->devices[0], now(c), alternate(c));
                    break;
                }
        }
        /* IDENTIFY completed before activation. IRQ source is off while
         * idle; each request enables it only after serialized selection. */
        klog("[ata%u] identified=%d quarantined=%u", i, found > 0 ? found : 0, c->stopped);
    }
    return 0;
}

struct ata_device *ata_device_get(unsigned channel, unsigned unit)
{
    if (!initialized || channel >= ATA_CHANNELS || unit >= ATA_DEVICES)
        return 0;
    struct ata_device *d = &channels[channel].devices[unit];
    return d->identified && !d->block.quarantined && !d->channel->quiescing ? d : 0;
}

uint64_t ata_command_count(void)
{
    uint64_t n = 0;
    for (unsigned i = 0; i < ATA_CHANNELS; i++)
        n += channels[i].commands;
    return n;
}
