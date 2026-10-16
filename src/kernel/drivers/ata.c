/* Legacy ATA PIO/DMA, one sleepable transaction per channel, never BIOS/retry.
 * Research: T13 ATA/ATAPI-6 d1410r3a, sections 6.20, 7, 8.12-8.15,
 * 8.34-8.35, 8.62-8.63, 9.2, 9.5-9.6, 9.12 and 10.2.2 (table 67):
 * https://www.read.seas.harvard.edu/~kohler/class/04f-aos/ref/hardware/ATA-d1410r3a.pdf
 * The existing sync.c wait queue enqueues/checks atomically; trap.c owns
 * the PIC EOI and spurious IRQ15. Only Status reads acknowledge INTRQ;
 * polling uses Alternate Status. No legacy VM/PIC implementation is copied.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/hardware_check.h>
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
static int ata_emergency_write(struct blkdev *,uint64_t,const void *);
static bool initialized;
static const char *const owners[ATA_CHANNELS] = { "ata0", "ata1" };
void proc_launch_dma_read(uint32_t bytes, uint64_t ms, bool bounce);
#pragma weak proc_launch_dma_read

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
static void native_insw(void *ctx, uint16_t port, void *buffer, uint32_t words)
{
    (void)ctx;
    __asm__ volatile("cld; rep insw" : "+D"(buffer), "+c"(words) : "d"(port) : "memory");
}
static void native_outsw(void *ctx, uint16_t port, const void *buffer, uint32_t words)
{
    (void)ctx;
    __asm__ volatile("cld; rep outsw" : "+S"(buffer), "+c"(words) : "d"(port) : "memory");
}
static uint64_t native_deadline(void *ctx, uint64_t d) { (void)ctx; return d; }
static int native_delay(void *ctx, uint32_t us) { (void)ctx; return udelay(us); }
static const struct ata_port_ops native_ops = {
    native_in8, native_out8, native_in16, native_out16, native_now, native_deadline, native_delay,
    native_insw, native_outsw,
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
static bool expired(struct ata_channel *c, uint64_t d) {
    return (hardware_check_io_cancelled && hardware_check_io_cancelled()) || now(c) - d < (1ull << 63);
}
static int pace(struct ata_channel *c)
{
    /* 9.5/9.6: >=400 ns after Command, >=one PIO cycle after a data
     * block. udelay has microsecond granularity: round UP to 1 us.
     * IDENTIFY/ATAPI retain their original individual-word pacing.
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
    c->dma.claim = -1;
    c->control_shadow = ATA_NIEN;   /* ATA-6: all other implemented bits zero */
    kmutex_init(&c->mutex);
    kwait_init(&c->wait);
    for (unsigned i = 0; i < 3; i++)
        c->claims[i] = -1;
    for (unsigned i = 0; i < ATA_DEVICES; i++) {
        c->devices[i].channel = c;
        c->devices[i].unit = i;
        c->devices[i].firmware_udma = -1;
        c->devices[i].block.ctx = &c->devices[i];
        c->devices[i].block.sector_size = BLKDEV_SECTOR_SIZE;
    }
    return 0;
}

void ata_channel_irq(struct ata_channel *c)
{
    /* Always acknowledge the device, including an unarmed late edge. Never
     * transfer data, log, allocate, sleep or send PIC EOI here. */
    if (c->dma.armed) ata_dma_irq(c);
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

static int failure(struct ata_device *d, uint64_t start, uint8_t status);
static int dma_failure(struct ata_device *d, uint64_t start, uint8_t status)
{
    struct ata_channel *c = d->channel;
    bool unstarted = c->dma.armed && d->issued && !c->dma.started && !(status & ATA_ERR);
    uint8_t bm = c->dma.irq_status | c->dma.ops->in8(c->dma.ctx, c->dma.base + 2);
    bool idle = ata_dma_stop(c);
    if (!idle) ata_dma_revoke(c);
    c->dma.armed = false;
    d->dma_enabled = false; d->dma_quarantined = true; d->dma_configuring = false;
    d->dma_errors++;
    d->status = status; d->error = reg_read(c, 1); d->elapsed_ms = now(c) - start;
    uint8_t ending = alternate(c);
    bool fallback = idle && !unstarted && !c->quiescing && (ending & ATA_DRDY) && ending != 0xff &&
                    !(ending & (ATA_BSY | ATA_DRQ | ATA_DF));
    klog("[ata%u] dma quarantine unit=%u mode=udma%u generation=%u command=%02x issued=%u bm=%02x status=%02x error=%02x elapsed_ms=%llu fallback=%s",
         c->irq - 14, d->unit, d->udma_mode, d->generation, d->command, d->issued, bm,
         status, d->error, d->elapsed_ms, fallback ? "pio" : "blocked");
    /* A failed command is never replayed. Only a later independent request
     * may use PIO, after proving both engines idle. Busy/DRQ/DF or missing
     * devices retain the old whole-channel quarantine and pinned memory. */
    if (!fallback) return failure(d, start, ending);
    return -FS_EIO;
}
static int failure(struct ata_device *d, uint64_t start, uint8_t status)
{
    struct ata_channel *c = d->channel;
    if (c->dma.armed || d->dma_configuring) return dma_failure(d, start, status);
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
        if (c->dma.claim >= 0) registry_quarantine(c->dma.claim, c->dma.generation);
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
    uint64_t check = hardware_check_io_deadline ? hardware_check_io_deadline() : 0;
    if (check && check < deadline) deadline = check;
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
    /* ERR from a completed failed DMA command persists until the next
     * command. It is not the next PIO command's ending status. */
    if (d->dma_quarantined) {
        uint8_t s = alternate(c);
        if ((s & ATA_DRDY) && !(s & (ATA_BSY | ATA_DRQ | ATA_DF)) && s != 0xff) return 0;
    }
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
    c->dma.armed = false;
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

/* ATAPI accepts a non-busy selection without ATA's DRDY requirement. */
static int packet_select(struct ata_device *d, uint64_t start, uint64_t deadline)
{
    struct ata_channel *c = d->channel;
    while (alternate(c) & (ATA_BSY | ATA_DRQ)) {
        if (expired(c, deadline)) return failure(d, start, alternate(c));
        task_sleep_ms(1);
    }
    reg_write(c, 6, (uint8_t)(0xa0 | d->unit << 4));
    if (pace(c)) return failure(d, start, alternate(c));
    while (alternate(c) & ATA_BSY) {
        if (expired(c, deadline)) return failure(d, start, alternate(c));
        task_sleep_ms(1);
    }
    return 0;
}
static void media_changed(struct ata_device *d, enum atapi_media state)
{
    if (d->block.media_generation == UINT32_MAX) {
        failure(d, now(d->channel), alternate(d->channel));
        return;
    }
    ++d->block.media_generation;
    d->block.capacity = 0;
    d->media = state;
}
/* Returns CHECK CONDITION separately so REQUEST SENSE remains under the
 * same owner mutex. No unrelated command may intervene and clear sense. */
static int packet_raw(struct ata_device *d, const uint8_t *packet, unsigned length,
                      void *buffer, uint32_t capacity, uint32_t *done, uint32_t limit)
{
    struct ata_channel *c = d->channel;
    uint64_t start = now(c), deadline = start + limit;
    uint32_t sequence;
    *done = 0;
    if (c->stopped || c->quiescing || d->block.quarantined) return -FS_EQUARANTINED;
    device_control(c, c->control_shadow | ATA_NIEN);
    d->issued = false;
    int e = packet_select(d, start, deadline);
    if (e) return e;
    reg_write(c, 1, 0); /* PIO, no overlap */
    reg_write(c, 2, 0); reg_write(c, 3, 0);
    uint32_t phase_limit = capacity ? (capacity > 0xfffe ? 0xfffe : (capacity + 1) & ~1u) : 2048;
    reg_write(c, 4, (uint8_t)phase_limit); reg_write(c, 5, (uint8_t)(phase_limit >> 8));
    e = issue(d, 0xa0, start, &sequence);
    bool sent = false, odd = false;
    unsigned phases = 0;
    while (!e) {
        uint8_t status = alternate(c);
        d->status = status;
        /* SFF-8020i 5.2: DRDY is not required for PACKET. A zero
         * completion status is valid when CoD/I/O report completion. */
        if (expired(c, deadline) || c->stopped || c->quiescing || status == 0xff || (status & ATA_DF)) {
            e = failure(d, start, status); break;
        }
        if (status & ATA_BSY) { task_sleep_ms(1); continue; }
        uint8_t reason = reg_read(c, 2) & 7;
        if (status & ATA_ERR) {
            if ((status & ATA_DRQ) || reason != 3 || !sent) e = failure(d, start, status);
            else { d->error = reg_read(c, 1); e = -FS_EAGAIN; }
            break;
        }
        if (!(status & ATA_DRQ)) {
            if (!sent || reason != 3) e = failure(d, start, status);
            break;
        }
        if (++phases > ATAPI_MAX_BYTES + 1) { e = failure(d, start, status); break; }
        reg_read(c, 7); /* acknowledge optional packet/data interrupt */
        if (!sent) {
            if (reason != 1) { e = failure(d, start, status); break; }
            for (unsigned i = 0; i < d->packet_bytes; i += 2) {
                uint16_t word = i < length ? packet[i] : 0;
                if (i + 1 < length) word |= (uint16_t)packet[i + 1] << 8;
                c->ops->out16(c->port_ctx, c->base, word);
                if (pace(c) || expired(c, deadline)) { e = failure(d, start, alternate(c)); break; }
            }
            sent = true; d->packet_commands++;
        } else {
            uint32_t bytes = reg_read(c, 4) | (uint32_t)reg_read(c, 5) << 8;
            if (reason != 2 || !bytes || bytes > phase_limit || bytes > capacity - *done || odd) {
                e = failure(d, start, status); break;
            }
            uint8_t *out = (uint8_t *)buffer + *done;
            for (uint32_t i = 0; i < bytes; i += 2) {
                if (expired(c, deadline)) { e = failure(d, start, alternate(c)); break; }
                uint16_t word = c->ops->in16(c->port_ctx, c->base);
                out[i] = (uint8_t)word;
                if (i + 1 < bytes) out[i + 1] = (uint8_t)(word >> 8);
                if (pace(c)) { e = failure(d, start, alternate(c)); break; }
                if ((i & 127)==126) task_yield();
            }
            *done += bytes; odd = !!(bytes & 1);
        }
        if (!e && pace(c)) e = failure(d, start, alternate(c));
        task_yield();
    }
    reg_read(c, 7);
    disarm(c);
    d->elapsed_ms = now(c) - start;
    if (d->elapsed_ms > d->max_packet_ms) d->max_packet_ms = d->elapsed_ms;
    return e;
}
static int packet_checked(struct ata_device *d, const uint8_t *p, unsigned length,
                          void *buf, uint32_t cap, uint32_t *done, uint32_t deadline)
{
    uint64_t start=now(d->channel);
    int e = packet_raw(d, p, length, buf, cap, done, deadline);
    if (e != -FS_EAGAIN) { if (e) *done=0; return e; }
    *done=0;
    uint64_t elapsed=now(d->channel)-start;
    if (elapsed>=deadline) return failure(d,start,d->status);
    uint8_t request[12] = { 3, 0, 0, 0, 18 }, sense[18]; uint32_t n;
    e = packet_raw(d, request, sizeof(request), sense, sizeof(sense), &n, (uint32_t)(deadline-elapsed));
    if (e || n < 14 || (sense[0] & 0x7e) != 0x70 || sense[7] < 6)
        return failure(d, now(d->channel), d->status);
    bool new_sense = d->sense_key != (sense[2] & 15) || d->asc != sense[12] || d->ascq != sense[13];
    d->sense_key = sense[2] & 15; d->asc = sense[12]; d->ascq = sense[13];
    if (d->sense_key == 6) media_changed(d, ATAPI_CHANGED);
    else if (d->sense_key == 2) {
        enum atapi_media m = d->asc == 0x3a ? ATAPI_NO_MEDIUM : ATAPI_NOT_READY;
        if (d->media != m) media_changed(d, m);
    }
    if (new_sense || d->sense_key == 6)
        klog("[optical] media channel=%u unit=%u stage=sense opcode=%02x key=%02x asc=%02x ascq=%02x generation=%u result=%d",
             d->channel->irq - 14, d->unit, p[0], d->sense_key, d->asc, d->ascq,
             d->block.media_generation, -FS_EIO);
    d->elapsed_ms=now(d->channel)-start;
    if (d->elapsed_ms>d->max_packet_ms) d->max_packet_ms=d->elapsed_ms;
    return -FS_EIO; /* a failed data request is never retried */
}
int atapi_packet(struct ata_device *d, const uint8_t *p, unsigned length,
                 void *buf, uint32_t cap, uint32_t *done)
{
    if (!d || !d->identified || !d->atapi || !p || !length || length > d->packet_bytes ||
        !done || cap > ATAPI_MAX_BYTES || (cap && !buf)) return -FS_EINVAL;
    switch (p[0]) {
    case 0: case 3: case 0x12: case 0x25: case 0x28: case 0x43: case 0x1b: case 0x1e: break;
    default: return -FS_EOPNOTSUPP;
    }
    kmutex_lock(&d->channel->mutex);
    int e = packet_checked(d, p, length, buf, cap, done,
                          p[0] == 0x1b && (length < 2 || !(p[1] & 1)) ? ATAPI_START_MS : ATAPI_COMMAND_MS);
    if (!e) d->sense_key = d->asc = d->ascq = d->error = 0;
    kmutex_unlock(&d->channel->mutex);
    return e;
}
static uint32_t packet_be32(const uint8_t *p)
{ return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static int ready_result(struct ata_device *d, const char *stage, int e, uint32_t bytes)
{
    if (!d) return e;
    if (d->ready_stage != stage || d->ready_error != e)
        klog("[optical] media channel=%u unit=%u stage=%s result=%d bytes=%u status=%02x error=%02x key=%02x asc=%02x ascq=%02x generation=%u sectors=%llu session=%u quarantined=%u",
             d->channel->irq - 14, d->unit, stage, e, bytes, d->status, d->error,
             d->sense_key, d->asc, d->ascq, d->block.media_generation,
             d->block.capacity, d->session_lba, d->block.quarantined);
    d->ready_stage = stage; d->ready_error = e;
    return e;
}
static int atapi_read(struct blkdev *b, uint64_t lba, uint32_t count, void *buf)
{
    int e = blkdev_range(b, lba, count);
    if (e || !buf || lba > UINT32_MAX || count > ATAPI_MAX_BYTES / 2048) return e ? e : -FS_EINVAL;
    struct ata_device *d = b->ctx; uint32_t gen = b->media_generation, done;
    uint8_t p[12] = { 0x28, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
        (uint8_t)(lba >> 8), (uint8_t)lba, 0, (uint8_t)(count >> 8), (uint8_t)count };
    e = atapi_packet(d, p, sizeof(p), buf, count * 2048, &done);
    if (!e && (done != count * 2048 || gen != b->media_generation)) e = -FS_EIO;
    if (!e) d->read_blocks += count;
    return e;
}
int atapi_ready(struct ata_device *d)
{
    uint8_t p[12] = { 0 }; uint32_t n;
    if (!d || !d->identified || !d->atapi) return -FS_EINVAL;
    /* SFF-8020i 10.6: reset/change attention may reject a readiness
     * command. Consume up to three queued attentions in one bounded TUR
     * transaction. READ(10) is never retried. */
    int e = -FS_EIO;
    kmutex_lock(&d->channel->mutex);
    uint64_t start = now(d->channel);
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        uint64_t elapsed = now(d->channel) - start;
        if (elapsed >= ATAPI_COMMAND_MS) break;
        e = packet_checked(d, p, sizeof(p), 0, 0, &n, (uint32_t)(ATAPI_COMMAND_MS - elapsed));
        if (!e || d->block.quarantined || d->sense_key != 6) break;
    }
    if (!e) d->sense_key = d->asc = d->ascq = d->error = 0;
    kmutex_unlock(&d->channel->mutex);
    if (e) {
        ready_result(d, "tur", e, 0);
        /* SFF-8020i 10.8.26: 02/04/02 requests initialization. Start
         * without loading/closing the tray (LoEj=0), once per generation.
         * IMMED keeps spin-up asynchronous; later TUR polls prove ready. */
        if (!d->block.quarantined && d->sense_key == 2 && d->asc == 4 && d->ascq == 2 &&
            d->spinup_generation != d->block.media_generation) {
            d->spinup_generation = d->block.media_generation;
            uint8_t spin[12] = { 0x1b, 1, 0, 0, 1 };
            int spin_error = atapi_packet(d, spin, sizeof(spin), 0, 0, &n);
            klog("[optical] media channel=%u unit=%u stage=spinup generation=%u result=%d",
                 d->channel->irq - 14, d->unit, d->block.media_generation, spin_error);
            if (spin_error) return ready_result(d, "spinup", spin_error, n);
        }
        return e;
    }
    if (d->media == ATAPI_READY && d->block.capacity) return 0;
    uint8_t inquiry[36]; memset(p,0,sizeof(p)); p[0]=0x12; p[4]=sizeof(inquiry);
    e=atapi_packet(d,p,sizeof(p),inquiry,sizeof(inquiry),&n);
    if (!e && (n<36 || (inquiry[0]&31)!=5)) e = -FS_EOPNOTSUPP;
    ready_result(d, "inquiry", e, n); if (e) return e;
    memset(p,0,sizeof(p)); p[0] = 0x25; uint8_t capacity[8];
    e = atapi_packet(d, p, sizeof(p), capacity, sizeof(capacity), &n);
    if (!e && (n != 8 || packet_be32(capacity + 4) != 2048 || packet_be32(capacity) == UINT32_MAX)) e = -FS_EOPNOTSUPP;
    ready_result(d, "capacity", e, n); if (e) return e;
    d->block.capacity = (uint64_t)packet_be32(capacity) + 1;
    /* First data track, then last-session information. QEMU/SFF encode
     * format 1 in byte 9, unlike later MMC's additional byte-2 field. */
    memset(p, 0, sizeof(p)); p[0] = 0x43; p[8] = 20;
    uint8_t toc[20]; e = atapi_packet(d, p, sizeof(p), toc, sizeof(toc), &n);
    if (!e && (n < 12 || !(toc[5] & 4))) e = -FS_EOPNOTSUPP;
    ready_result(d, "toc", e, n); if (e) return e;
    d->session_lba = packet_be32(toc + 8);
    p[8] = 12; p[9] = 0x40;
    e = atapi_packet(d, p, sizeof(p), toc, 12, &n);
    if (!e && (n != 12 || packet_be32(toc + 8) >= d->block.capacity)) e = -FS_EIO;
    ready_result(d, "session", e, n); if (e) return e;
    d->session_lba = packet_be32(toc + 8);
    d->media = ATAPI_READY;
    ready_result(d, "ready", 0, n);
    return 0;
}
int atapi_eject(struct ata_device *d)
{
    uint8_t p[12] = { 0x1e }; uint32_t n;
    int e = atapi_packet(d, p, sizeof(p), 0, 0, &n);
    if (!e) { p[0] = 0x1b; p[4] = 2; e = atapi_packet(d, p, sizeof(p), 0, 0, &n); }
    if (!e) media_changed(d, ATAPI_NO_MEDIUM);
    return e;
}
int atapi_start(struct ata_device *d)
{
    uint8_t p[12] = { 0x1b, 0, 0, 0, 3 }; uint32_t n;
    return atapi_packet(d, p, sizeof(p), 0, 0, &n);
}
static int atapi_check_block(struct blkdev *b) { return atapi_ready(b->ctx); }
static int identify_packet(struct ata_device *d)
{
    struct ata_channel *c = d->channel; uint8_t data[512];
    uint64_t start = now(c), deadline = start + ATAPI_COMMAND_MS; uint32_t sequence;
    d->issued = false;
    unsigned stage = 0;
    int e = packet_select(d, start, deadline); stage = 1;
    if (!e) { e = issue(d, 0xa1, start, &sequence); stage = 2; }
    if (!e) { e = poll_status(d, start, deadline, ATA_DRQ, 0); stage = 3; }
    if (!e) { e = data_block(d, data, false, start, deadline); stage = 4; }
    if (!e) { e = poll_status(d, start, deadline, 0, ATA_DRQ); stage = 5; }
    disarm(c);
    if (e) {
        /* Lead diagnostics (QEMU evidence of 2026-10-11): which stage failed. */
        klog("[ata%u] atapi identify unit=%u stage=%u error=%d status=%02x error_reg=%02x", c->irq - 14, d->unit, stage, e, alternate(c), reg_read(c, 1));
        return e;
    }
    uint16_t config = fs_rd16(data);
    d->packet_type = (config >> 8) & 31;
    if ((config & 0xc000) != 0x8000 || (config & 3) > 1) return -FS_EOPNOTSUPP;
    d->packet_bytes = (config & 3) ? 16 : 12;
    if (d->packet_type != 5) return -FS_EOPNOTSUPP;
    for (unsigned i = 0; i < 40; i++) {
        uint8_t ch = data[54 + (i ^ 1)]; d->model[i] = ch >= 33 && ch <= 126 && ch != '=' ? (char)ch : '_';
    }
    d->model[40] = 0;
    d->identified = true; d->block.read = atapi_read; d->block.write = 0; d->block.flush = 0;
    d->block.sector_size = 2048; d->block.media_generation = 1; d->block.media_check = atapi_check_block;
    klog("[ata%u] unit=%u kind=atapi type=%u packet_bytes=%u", c->irq - 14, d->unit, d->packet_type, d->packet_bytes);
    return 0;
}

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
    d->block.emergency_write=ata_emergency_write;
    d->block.flush = supported_valid && ((w83 & (1u << 12)) || d->flush_ext) ? ata_flush : 0;
    /* 8.15.22/28: word 47 advertises support independently of word 59's
     * bit 8, which validates only the current (possibly disabled) setting. */
    unsigned maximum = fs_rd16(data + 47 * 2) & 255;
    uint16_t current = fs_rd16(data + 59 * 2);
    if (maximum && maximum <= 128 && !(maximum & (maximum - 1))) {
        d->multiple_max = (uint8_t)maximum;
        d->multiple_sectors = (current & 0x100) && (current & 255) <= maximum ?
                              (uint8_t)current : 0;
        /* 8.49/9.4: explicitly establish the block size after reset. An
         * issued SET failure follows the usual quarantine/no-retry rule. */
        start = now(c); deadline = start + ATA_COMMAND_MS;
        e = select_device(d, start, deadline, 0);
        if (!e) {
            device_control(c, (uint8_t)(c->control_shadow & ~ATA_NIEN));
            reg_write(c, 2, (uint8_t)maximum);
            if (c->native) pic_unmask(c->irq);
            e = issue(d, 0xc6, start, &sequence);
        }
        if (!e) e = wait_irq(d, &sequence, start, deadline);
        if (!e) e = poll_status(d, start, deadline, 0, ATA_DRQ);
        disarm(c);
        if (e) { d->multiple_sectors = 0; return e; }
        d->multiple_sectors = (uint8_t)maximum;
    }
    d->identified = true;
    int mode = c->dma.ops ? ata_dma_restore_mode(d->firmware_udma, data) : -1;
    if (mode >= 0) {
        d->udma_mode = (uint8_t)mode;
        start = now(c); deadline = start + ATA_COMMAND_MS;
        e = select_device(d, start, deadline, 0);
        if (!e) {
            d->dma_configuring = true;
            d->issued = false;
            device_control(c, (uint8_t)(c->control_shadow & ~ATA_NIEN));
            reg_write(c, 1, 3); reg_write(c, 2, (uint8_t)(0x40 | mode));
            if (c->native) pic_unmask(c->irq);
            e = issue(d, 0xef, start, &sequence);
        }
        if (!e) e = wait_irq(d, &sequence, start, deadline);
        if (!e) e = poll_status(d, start, deadline, 0, ATA_DRQ);
        d->dma_configuring = false;
        disarm(c);
        if (c->stopped) return e;
        d->dma_enabled = !e;
        if (!e) klog("[ata%u] dma mode=udma%u unit=%u bm=%04x cable80=%u buffer_bytes=%u",
                      c->irq - 14, d->udma_mode, d->unit, c->dma.base,
                      (fs_rd16(data + 93 * 2) & 0xe000) == 0x6000, ATA_DMA_BYTES);
    }
    d->elapsed_ms = now(c) - start;
    return 0;
}

/* ATA-6 6.6.1 permits SRST to revert to MWDMA. Measure the firmware's
 * selected UDMA BEFORE resetting, so SET FEATURES can restore exactly that
 * timing without guessing at vendor registers or accelerating the channel.
 * Firmware leaves each channel idle. Probe with nIEN and bounded polling;
 * an IDENTIFY ABRT with the ATAPI signature is the expected packet case. */
static int firmware_mode(struct ata_device *d)
{
    struct ata_channel *c = d->channel;
    uint8_t data[512]; uint32_t sequence;
    uint64_t start = now(c), deadline = start + ATA_COMMAND_MS;
    d->issued = false;
    uint8_t status;
    /* An unselected/absent unit may read FFh. It must not be interpreted
     * as a real device asserting BSY/DRQ, before or after selection. */
    while ((status = alternate(c)) != 0xff && (status & (ATA_BSY | ATA_DRQ))) {
        if (expired(c, deadline)) return failure(d, start, status);
        task_sleep_ms(1);
    }
    reg_write(c, 6, (uint8_t)(0xa0 | d->unit << 4));
    if (pace(c)) return failure(d, start, alternate(c));
    for (;;) {
        status = alternate(c);
        if (!status || status == 0xff) return 0;
        if (!(status & ATA_BSY)) break;
        if (expired(c, deadline)) return failure(d, start, status);
        task_sleep_ms(1);
    }
    if (reg_read(c, 4) == 0x14 && reg_read(c, 5) == 0xeb) return 0;
    int e = issue(d, ATA_IDENTIFY, start, &sequence);
    while (!e) {
        status = alternate(c);
        if (expired(c, deadline)) { e = failure(d, start, status); break; }
        if (status & ATA_BSY) { task_sleep_ms(1); continue; }
        if ((status & ATA_ERR) && !(status & (ATA_DF | ATA_DRQ)) &&
            reg_read(c, 4) == 0x14 && reg_read(c, 5) == 0xeb) break;
        e = poll_status(d, start, deadline, ATA_DRQ, 0);
        if (!e) e = data_block(d, data, false, start, deadline);
        if (!e) e = poll_status(d, start, deadline, 0, ATA_DRQ);
        if (!e) d->firmware_udma = (int8_t)ata_dma_mode(data);
        break;
    }
    reg_read(c, 7); disarm(c);
    return e;
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
    int e = 0;
    if (c->dma.ops) {
        device_control(c, c->control_shadow | ATA_NIEN);
        for (unsigned i = 0; i < ATA_DEVICES && !e; i++) e = firmware_mode(&c->devices[i]);
        if (e) { kmutex_unlock(&c->mutex); return e; }
    }
    /* 9.2: SRST >=5 us, then >=2 ms before inspecting reset signature.
     * Discovery is the only reset; failures never rearm native channels. */
    device_control(c, c->control_shadow | ATA_NIEN | ATA_SRST);
    e = c->ops->delay_us(c->port_ctx, 5);
    device_control(c, (uint8_t)(c->control_shadow & ~ATA_SRST));
    task_sleep_ms(2);
    uint64_t start = now(c), deadline = start + ATA_COMMAND_MS;
    int found = 0;
    /* Pass 1: read every unit's reset signature before any command on the
     * channel. Task-file writes reach both devices (QEMU ide/core.c writes
     * the Sector Count of both drives; libata-sff classifies dev0 and dev1
     * right after the soft reset): the SET FEATURES that programs the
     * master's UDMA mode left 0x45 in the slave's Sector Count and the old
     * single pass then skipped a present slave (gate evidence 2026-10-16). */
    struct { bool present; uint8_t status, count, low, mid, high; } sig[ATA_DEVICES] = {{0}};
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
        if (s == 0xff)
            continue;
        /* ATA/ATAPI-6 9.12: a PACKET device keeps DRDY clear after reset but
         * presents its signature; only an absent device reads status 0 with
         * no signature (QEMU ide-cd evidence of 2026-10-11, lead fix). */
        if (!s && !(reg_read(c, 4) == 0x14 && reg_read(c, 5) == 0xeb))
            continue;
        while (s & ATA_BSY) {
            if (expired(c, deadline)) { e = failure(d, start, s); break; }
            task_sleep_ms(1);
            s = alternate(c);
        }
        if (e)
            break;
        /* 9.12: SC=1/LBA Low=1, ATA mid/high=00/00; ATAPI=14/EB. */
        sig[i].present = true; sig[i].status = s;
        sig[i].count = reg_read(c, 2); sig[i].low = reg_read(c, 3);
        sig[i].mid = reg_read(c, 4); sig[i].high = reg_read(c, 5);
        /* One line per selected unit so an absent slave can be told from a
         * signature mismatch in the boot log (gate evidence of 2026-10-16). */
        klog("[ata%u] unit=%u status=%02x signature=%02x%02x%02x%02x", c->irq - 14, i, s,
             sig[i].count, sig[i].low, sig[i].mid, sig[i].high);
    }
    /* Pass 2: classify and identify. Commands may now change task files. */
    for (unsigned i = 0; i < ATA_DEVICES && !e; i++) {
        struct ata_device *d = &c->devices[i];
        if (!sig[i].present || sig[i].count != 1 || sig[i].low != 1)
            continue;
        reg_write(c, 6, (uint8_t)(0xa0 | i << 4));
        if (pace(c)) { e = failure(d, start, alternate(c)); break; }
        if (sig[i].mid == 0x14 && sig[i].high == 0xeb) {
            d->present = d->atapi = true;
            e = identify_packet(d);
            if (e == -FS_EOPNOTSUPP) { e = 0; continue; }
            if (!e) found++;
            continue;
        }
        if (sig[i].mid || sig[i].high)
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

/* 9.5/9.6: one DRQ/IRQ per multiple block, possibly a short final block.
 * Firmware owns bus cycle/IORDY timing (10.2.2). Software pacing belongs
 * after the block, not after each word. Small bursts bound cooperative work
 * even when a device advertises a 64 KiB multiple block. IF stays enabled. */
static int transfer_block(struct ata_device *d, uint8_t *buf, unsigned sectors,
                          bool write, uint64_t start, uint64_t deadline)
{
    struct ata_channel *c = d->channel;
    for (unsigned off = 0, bytes = sectors * 512; off < bytes;) {
        if (c->stopped || c->quiescing || expired(c, deadline))
            return failure(d, start, alternate(c));
        unsigned take = bytes - off;
        if (take > 2048) take = 2048;
        if (write && c->ops->outsw)
            c->ops->outsw(c->port_ctx, c->base, buf + off, take / 2);
        else if (!write && c->ops->insw)
            c->ops->insw(c->port_ctx, c->base, buf + off, take / 2);
        else for (unsigned i = 0; i < take; i += 2) {
            if (write) c->ops->out16(c->port_ctx, c->base, fs_rd16(buf + off + i));
            else fs_wr16(buf + off + i, c->ops->in16(c->port_ctx, c->base));
        }
        off += take;
        if (off < bytes) task_yield();
    }
    if (pace(c) || expired(c, deadline)) return failure(d, start, alternate(c));
    return 0;
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
    bool dma = d->dma_enabled;
    if (dma) {
        e = ata_dma_prepare(c, buf, n * 512, write);
        if (e) return dma_failure(d, start, alternate(c));
    }
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
    bool multiple = d->multiple_sectors != 0;
    uint8_t command = write ? (d->lba48 ? 0x34 : 0x30) : (d->lba48 ? 0x24 : 0x20);
    if (multiple) command = write ? (d->lba48 ? 0x39 : 0xc5) : (d->lba48 ? 0x29 : 0xc4);
    if (dma) command = write ? (d->lba48 ? 0x35 : 0xca) : (d->lba48 ? 0x25 : 0xc8);
    e = issue(d, command, start, &sequence);
    if (dma) {
        /* An immediate device abort can precede Start. Do not restart a
         * master already stopped by that interrupt or a failure path. */
        uint32_t f = irq_save();
        if (!e && c->irq_sequence == sequence) ata_dma_start(c);
        irq_restore(f);
        if (!e) e = wait_irq(d, &sequence, start, deadline);
        uint8_t s = alternate(c), bm = c->dma.irq_status;
        if (!e && (!c->dma.started || (bm & (ATA_BM_ACTIVE | ATA_BM_ERROR | ATA_BM_IRQ)) != ATA_BM_IRQ || !(s & ATA_DRDY) ||
                   (s & (ATA_BSY | ATA_DRQ | ATA_ERR | ATA_DF)))) e = dma_failure(d, start, s);
        if (!ata_dma_stop(c) && !e) e = dma_failure(d, start, s);
        ata_dma_finish(c, buf, write, !e);
        disarm(c);
        d->elapsed_ms = now(c) - start;
        if (!e) {
            d->dma_requests++; d->dma_bytes += n * 512;
            if (c->dma.bounced) d->dma_bounces++;
            if (!write && proc_launch_dma_read) proc_launch_dma_read(n * 512, d->elapsed_ms, c->dma.bounced);
        }
        return e;
    }
    for (unsigned i = 0; i < n && !e;) {
        unsigned sectors = multiple ? d->multiple_sectors : 1;
        if (sectors > n - i) sectors = n - i;
        /* 9.6: first WRITE DRQ is NOT accompanied by INTRQ. All READ
         * blocks and subsequent WRITE blocks require an IRQ. */
        if (!write || i)
            e = wait_irq(d, &sequence, start, deadline);
        if (!e)
            e = poll_status(d, start, deadline, ATA_DRQ, 0);
        if (!e)
            e = transfer_block(d, buf + i * 512, sectors, write, start, deadline);
        i += sectors;
        if (!e && i < n)
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
    if (hardware_check_io_cancelled && hardware_check_io_cancelled()) return -FS_EAGAIN;
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
    if (hardware_check_io_cancelled && hardware_check_io_cancelled()) return -FS_EAGAIN;
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

/* Emergency recorder: no mutex, scheduler, DMA, logging or command replay.
 * Panic holds IF clear. Refuse a channel whose former owner/phase is unknown.
 * A finite port-poll budget also works when timer interrupts have stopped. */
static int emergency_poll(struct ata_channel *c, uint8_t required) {
    for (unsigned i=0;i<200000;i++) {
        uint8_t status=alternate(c);
        if (!(status&ATA_BSY)) {
            if (!status || status==0xff || (status&(ATA_ERR|ATA_DF))) return -FS_EIO;
            if ((status&ATA_DRDY) && (status&ATA_DRQ)==required) return 0;
        }
    }
    return -FS_EIO;
}
static int ata_emergency_write(struct blkdev *b,uint64_t lba,const void *data) {
    struct ata_device *d=b->ctx;struct ata_channel *c=d->channel;
    if ((read_eflags()&0x200) || !data || blkdev_range(b,lba,1) || !d->identified || d->atapi ||
        c->stopped || c->quiescing || c->active || c->mutex.owner || c->dma.armed)
        return -FS_EBUSY;
    if (c->dma.ops && ((c->dma.ops->in8(c->dma.ctx,c->dma.base)&1) ||
        (c->dma.ops->in8(c->dma.ctx,c->dma.base+2)&ATA_BM_ACTIVE))) return -FS_EBUSY;
    uint8_t status=alternate(c);
    /* Discovery can leave an absent slave selected (status zero). An idle
     * channel may select the known target; a busy/data/fault phase may not. */
    if (status==0xff || (status&(ATA_BSY|ATA_DRQ|ATA_DF))) return -FS_EBUSY;
    uint8_t control=c->control_shadow;
    device_control(c,control|ATA_NIEN);
    reg_write(c,6,(uint8_t)(0xe0|(d->unit<<4)|(d->lba48 ? 0 : (lba>>24)&15)));
    for (unsigned i=0;i<4;i++) (void)alternate(c);
    int e=emergency_poll(c,0);
    if (!e) {
        if (d->lba48) {
            reg_write(c,1,0);reg_write(c,2,0);reg_write(c,3,(uint8_t)(lba>>24));
            reg_write(c,4,(uint8_t)(lba>>32));reg_write(c,5,(uint8_t)(lba>>40));
        }
        reg_write(c,1,0);reg_write(c,2,1);reg_write(c,3,(uint8_t)lba);
        reg_write(c,4,(uint8_t)(lba>>8));reg_write(c,5,(uint8_t)(lba>>16));
        reg_write(c,7,d->lba48 ? 0x34 : 0x30);
        for (unsigned i=0;i<4;i++) (void)alternate(c);
        e=emergency_poll(c,ATA_DRQ);
        if (!e) {
            for (unsigned i=0;i<256;i++) c->ops->out16(c->port_ctx,c->base,fs_rd16((const uint8_t *)data+i*2));
            for (unsigned i=0;i<4;i++) (void)alternate(c);
            e=emergency_poll(c,0);
        }
        if (!e && b->write_cache_state!=BLKDEV_CACHE_DISABLED) {
            reg_write(c,7,d->flush_ext ? 0xea : 0xe7);
            for (unsigned i=0;i<4;i++) (void)alternate(c);
            e=emergency_poll(c,0);
        }
    }
    /* An uncertain command is terminal here. Panic will halt immediately. */
    (void)reg_read(c,7);device_control(c,control);
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
    bool master_idle = ata_dma_stop(c);
    if (!master_idle) ata_dma_revoke(c);
    if (c->active || c->mutex.owner)
        return false;
    /* No queued driver callbacks exist. An uncertain busy device/master
     * cannot supply a release proof; quarantine retains its claims. */
    bool idle = master_idle && !c->stopped && !(alternate(c) & (ATA_BSY | ATA_DRQ));
    if (idle && c->native)
        irq_set_handler(c->irq, 0); /* no callback may outlive a released edge claim */
    return idle;
}

bool ata_registry_idle(int handle, gen_t generation)
{
    for (unsigned i = 0; i < ATA_CHANNELS; i++) {
        struct ata_channel *c = &channels[i];
        if (c->native && c->dma.claim == handle && gen_matches(c->dma.generation, generation))
            return ata_channel_idle(c);
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
        ata_dma_attach(c, i);
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
            if (!c->stopped && c->dma.claim >= 0 && registry_activate(c->dma.claim, c->dma.generation))
                failure(&c->devices[0], now(c), alternate(c));
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

const struct ata_device *ata_inventory_get(unsigned channel, unsigned unit)
{
    if (!initialized || channel >= ATA_CHANNELS || unit >= ATA_DEVICES) return 0;
    const struct ata_device *d = &channels[channel].devices[unit];
    return d->identified ? d : 0;
}

uint64_t ata_command_count(void)
{
    uint64_t n = 0;
    for (unsigned i = 0; i < ATA_CHANNELS; i++)
        n += channels[i].commands;
    return n;
}
