/* SFF ATA PIO/DMA, one sleepable transaction per channel, never BIOS/retry.
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
#include <ciuki/ata_power.h>
#include <ciuki/timing.h>
#include <ciuki/mm.h>

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
static int command_data(struct ata_device *, uint64_t, uint32_t, uint8_t *, bool);
static const char *const owners[ATA_CHANNELS] = { "ata0", "ata1", "ata2", "ata3", "ata4", "ata5", "ata6", "ata7" };
static struct kmutex addin_mutex;
static bool addin_mutex_ready;
static void ata_lock(struct ata_channel *c)
{
    if(!ata_addin_match(c->pci)) {kmutex_lock(&c->mutex);return;}
    if((uintptr_t)&addin_mutex<(uintptr_t)&c->mutex) {
        kmutex_lock(&addin_mutex);kmutex_lock(&c->mutex);
    } else {kmutex_lock(&c->mutex);kmutex_lock(&addin_mutex);}
}
static void ata_unlock(struct ata_channel *c)
{
    if(!ata_addin_match(c->pci)) {kmutex_unlock(&c->mutex);return;}
    if((uintptr_t)&addin_mutex<(uintptr_t)&c->mutex) {
        kmutex_unlock(&c->mutex);kmutex_unlock(&addin_mutex);
    } else {kmutex_unlock(&addin_mutex);kmutex_unlock(&c->mutex);}
}
int ata_addin_wait(struct ata_channel *c, unsigned ms)
{
    if(c->native) {task_sleep_ms(ms);return 0;}
    for(unsigned i=0;i<ms;i++) if(c->ops->delay_us(c->port_ctx,1000)) return -FS_EIO;
    return 0;
}
const char *ata_channel_owner(unsigned i) { return i<ATA_CHANNELS ? owners[i] : "ata?"; }
bool ata_setup_install_allowed(const struct ata_device *d)
{
    return d && !ata_addin_match(d->channel->pci) && !d->raid_member && !d->block.quarantined;
}
static unsigned mmio_port(struct ata_channel *c, unsigned p)
{
    static const uint8_t offsets[]={0,5,10,15,16,21,26,31};
    return p>=c->base && p<c->base+8u ? c->base+offsets[p-c->base] : p;
}
void proc_launch_dma_read(uint32_t bytes, uint64_t ms, bool bounce);
#pragma weak proc_launch_dma_read

static uint8_t native_in8(void *ctx, uint16_t p) {
    struct ata_channel *c=ctx;
    if(c && c->mmio_phys && !c->mmio && p!=0xcfc) return 0xff;
    return c && c->mmio && p!=0xcfc ? c->mmio[mmio_port(c,p)] : inb(p);
}
static void native_out8(void *ctx, uint16_t p, uint8_t v) {
    struct ata_channel *c=ctx;
    if(c && c->mmio_phys && !c->mmio && p!=0xcfc) return;
    /* Flush posted task-file writes with Alternate Status. Reading back
     * the Command address would read Status and acknowledge INTRQ. */
    if(c && c->mmio && p!=0xcfc) {c->mmio[mmio_port(c,p)]=v;(void)c->mmio[c->control];}
    else outb(p,v);
}
static uint16_t native_in16(void *ctx, uint16_t p)
{
    struct ata_channel *c=ctx;
    if(c && c->mmio_phys && !c->mmio && p!=0xcfc) return 0xffff;
    if(c && c->mmio && p!=0xcfc) return *(volatile uint16_t *)(c->mmio+mmio_port(c,p));
    uint16_t v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(p));
    return v;
}
static void native_out16(void *ctx, uint16_t p, uint16_t v)
{
    struct ata_channel *c=ctx;
    if(c && c->mmio_phys && !c->mmio && p!=0xcfc) return;
    if(c && c->mmio && p!=0xcfc) {*(volatile uint16_t *)(c->mmio+mmio_port(c,p))=v;return;}
    __asm__ volatile("outw %0, %1" :: "a"(v), "Nd"(p));
}
static uint64_t native_now(void *ctx) { (void)ctx; return deadline_after_ms(0); }
static void native_insw(void *ctx, uint16_t port, void *buffer, uint32_t words)
{
    struct ata_channel *c=ctx;
    if(c && c->mmio_phys && !c->mmio) return;
    if(c && c->mmio) {uint8_t *b=buffer;while(words--) {fs_wr16(b,native_in16(ctx,port));b+=2;}return;}
    __asm__ volatile("cld; rep insw" : "+D"(buffer), "+c"(words) : "d"(port) : "memory");
}
static void native_outsw(void *ctx, uint16_t port, const void *buffer, uint32_t words)
{
    struct ata_channel *c=ctx;
    if(c && c->mmio_phys && !c->mmio) return;
    if(c && c->mmio) {const uint8_t *b=buffer;while(words--) {native_out16(ctx,port,fs_rd16(b));b+=2;}return;}
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
    if (r == 6) c->selected_shadow = v;
    c->ops->out8(c->port_ctx, (uint16_t)(c->base + r), v);
}
static uint8_t alternate(struct ata_channel *c)
{
    return c->ops->in8(c->port_ctx, c->control);
}
static void device_control(struct ata_channel *c, uint8_t v)
{
    bool changed = c->control_shadow != v;
    bool reset = (c->control_shadow | v) & ATA_SRST;
    c->control_shadow = v;
    c->ops->out8(c->port_ctx, c->control, v);
    /* pata_via.c via_tf_load: changing IEN may reset Device/Head.
     * SRST has its own selection protocol; don't restore across reset. */
    if (changed && c->chipset.family == ATA_CHIP_VIA && !reset && !c->stopped && !c->quiescing)
        c->ops->out8(c->port_ctx, (uint16_t)(c->base + 6), c->selected_shadow);
}
static uint64_t now(struct ata_channel *c) { return c->ops->now_ms(c->port_ctx); }
/* f1-92: why a command boundary expired, for the fault/quarantine records. */
enum { ATA_EXPIRY_NONE, ATA_EXPIRY_DEADLINE, ATA_EXPIRY_CHECK, ATA_EXPIRY_NO_BM_IRQ };
/* Device deadline only. ATA-6 has no way to withdraw an issued command short
 * of a reset (9.x protocols end only in the device's idle states), so a
 * caller's cancellation or request budget (f3-49b request seam) must not end
 * an issued command: that turned cancelled prefetch/DOS reads into DMA and
 * channel quarantines on the T23 (f1-92). Interruption is honoured only
 * before a command is issued (ata_transfer). The hardware check keeps its
 * own explicit I/O cut-off. */
static bool expired(struct ata_channel *c, uint64_t d) {
    if (hardware_check_io_cancelled && hardware_check_io_cancelled()) { c->expiry = ATA_EXPIRY_CHECK; return true; }
    if (now(c) - d < (1ull << 63)) { c->expiry = ATA_EXPIRY_DEADLINE; return true; }
    return false;
}
static bool request_interrupted(void) { return blk_request_interrupted && blk_request_interrupted(); }
/* Bounded per-unit records: the first four, then each power of two. */
static bool record_due(uint32_t count) { return count <= 4 || !(count & (count - 1)); }
static const char *fault_cause(struct ata_device *d, uint8_t status, uint8_t bm, bool dma)
{
    struct ata_channel *c = d->channel;
    uint8_t expiry = c->expiry;
    c->expiry = ATA_EXPIRY_NONE;
    if (c->quiescing) return "quiesce";
    if (expiry == ATA_EXPIRY_CHECK) return "check_cancel";
    if (expiry == ATA_EXPIRY_NO_BM_IRQ) return "bm_no_interrupt";
    if (expiry == ATA_EXPIRY_DEADLINE) return "timeout";
    if (dma && !d->issued) return "dma_prepare";
    if (status == 0xff || !status) return "no_device";
    if (status & (ATA_ERR | ATA_DF)) return "device_error";
    if (dma && (bm & ATA_BM_ERROR)) return "bm_error";
    if (dma && !c->dma.started) return "dma_not_started";
    if (dma && (bm & ATA_BM_ACTIVE)) return "bm_active";
    if (dma && !(bm & ATA_BM_IRQ)) return "bm_no_interrupt";
    return "protocol";
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
    for(unsigned i=0;i<ATA_CHANNELS;i++)
        if(c==&channels[i] && (c->native || c->discovered || c->stopped)) return -FS_EQUARANTINED;
    if(!addin_mutex_ready) {kmutex_init(&addin_mutex);addin_mutex_ready=true;}
    memset(c, 0, sizeof(*c));
    c->ops = ops;
    c->index = index;
    c->selected_shadow = 0xa0;
    c->port_ctx = ctx;
    c->base = index ? 0x170 : 0x1f0;
    c->control = index ? 0x376 : 0x3f6;
    c->irq = index ? 15 : 14;
    c->dma.claim = -1;
    c->addin_claim = -1;
    c->peer_claims[0]=c->peer_claims[1]=-1;
    c->control_shadow = ATA_NIEN;   /* ATA-6: all other implemented bits zero */
    kmutex_init(&c->mutex);
    blk_queue_init(&c->requests,owners[index],BLKREQ_COMMAND_BYTES);
    kwait_init(&c->wait);
    for (unsigned i = 0; i < 3; i++)
        c->claims[i] = -1;
    for (unsigned i = 0; i < ATA_DEVICES; i++) {
        c->devices[i].channel = c;
        c->devices[i].unit = i;
        c->devices[i].firmware_udma = -1;
        c->devices[i].block.ctx = &c->devices[i];
        c->devices[i].block.stats_unit = i;
        c->devices[i].block.sector_size = BLKDEV_SECTOR_SIZE;
        c->devices[i].block.queue = &c->requests;
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
    if (status && status != 0xff && !(status & ATA_BSY)) kwait_wake_irq(&c->wait, true);
    else kwait_wake_irq(&c->wait, false);
}

static void primary_irq(struct trap_frame *tf) { (void)tf; ata_channel_irq(&channels[0]); }
static void secondary_irq(struct trap_frame *tf) { (void)tf; ata_channel_irq(&channels[1]); }

static bool ata_pci_interrupt(unsigned irq, const char *owner)
{
    for (unsigned i = 0; i < ATA_CHANNELS; i++) {
        struct ata_channel *c = &channels[i];
        if (!c->native_irq || c->irq != irq || strncmp(owner, owners[i], 5) || !c->dma.ops) continue;
        uint8_t s = c->dma.ops->in8(c->dma.ctx, (uint16_t)(c->dma.base + 2));
        if (!ata_addin_irq_pending(c)) return false;
        bool armed = c->dma.armed;
        ata_channel_irq(c);
        if (!armed) c->dma.ops->out8(c->dma.ctx, (uint16_t)(c->dma.base + 2), (s & 0x60) | ATA_BM_IRQ);
        return true;
    }
    return false;
}

static int failure(struct ata_device *d, uint64_t start, uint8_t status);
static int dma_failure(struct ata_device *d, uint64_t start, uint8_t status)
{
    struct ata_channel *c = d->channel;
    bool unstarted = c->dma.armed && d->issued && !c->dma.started && !(status & ATA_ERR);
    uint8_t bm = c->dma.irq_status | c->dma.ops->in8(c->dma.ctx, c->dma.base + 2);
    const char *cause = fault_cause(d, status, bm, true);
    bool idle = ata_dma_stop(c);
    if (!idle) ata_dma_revoke(c);
    c->dma.armed = false;
    d->dma_enabled = false; d->dma_quarantined = true; d->dma_configuring = false;
    d->dma_errors++;
    d->status = status; d->error = reg_read(c, 1); d->elapsed_ms = now(c) - start;
    uint8_t ending = alternate(c);
    bool fallback = idle && !unstarted && !c->quiescing && (ending & ATA_DRDY) && ending != 0xff &&
                    !(ending & (ATA_BSY | ATA_DRQ | ATA_DF));
    klog("[ata%u] dma quarantine unit=%u mode=udma%u generation=%u command=%02x issued=%u bm=%02x status=%02x error=%02x elapsed_ms=%llu fallback=%s cause=%s",
         c->index, d->unit, d->udma_mode, d->generation, d->command, d->issued, bm,
         status, d->error, d->elapsed_ms, fallback ? "pio" : "blocked", cause);
    if (fallback) klog("[ata%u] dma fallback unit=%u generation=%u command=%02x bm=%02x status=%02x mode=pio",
         c->index, d->unit, d->generation, d->command, bm, ending);
    /* The failed DMA command itself is never resumed. With both engines
     * proven idle, ata_transfer repeats the same request once through PIO
     * (f1-92) before any error reaches the volume. Busy/DRQ/DF or missing
     * devices retain the old whole-channel quarantine and pinned memory. */
    if (!fallback) { c->fault_cause = cause; return failure(d, start, ending); }
    return -FS_EIO;
}
static int failure(struct ata_device *d, uint64_t start, uint8_t status)
{
    struct ata_channel *c = d->channel;
    if (c->dma.armed || d->dma_configuring) return dma_failure(d, start, status);
    const char *cause = c->fault_cause ? c->fault_cause : fault_cause(d, status, 0, false);
    c->fault_cause = 0;
    d->status = status;
    d->error = reg_read(c, 1);       /* diagnostic only while BSY is set */
    d->elapsed_ms = now(c) - start;
    /* f1-acceptance/storage ownership quarantines the entire controller
     * after an issued failure, including IDENTIFY/FLUSH. This also covers
     * stuck BSY: never select/reset a sibling on an uncertain channel. */
    c->stopped = true;
    for (unsigned i = 0; i < ATA_DEVICES; i++)
        c->devices[i].block.quarantined = true;
    if(c->native && ata_addin_match(c->pci)) for(unsigned j=0;j<ATA_CHANNELS;j++) {
        struct ata_channel *peer=&channels[j];
        if(peer!=c && peer->pci==c->pci) {
            peer->stopped=true;
            for(unsigned u=0;u<ATA_DEVICES;u++) peer->devices[u].block.quarantined=true;
        }
    }
    device_control(c, c->control_shadow | ATA_NIEN);
    if (c->native) {
        if (!c->native_irq) pic_mask(c->irq);
        for (unsigned i = 0; i < 3; i++)
            if (c->claims[i] >= 0)
                registry_quarantine(c->claims[i], c->generations[i]);
        if (c->dma.claim >= 0) registry_quarantine(c->dma.claim, c->dma.generation);
        if (c->addin_claim >= 0) registry_quarantine(c->addin_claim, c->addin_generation);
        for(unsigned i=0;i<2;i++) if(c->peer_claims[i]>=0) registry_quarantine(c->peer_claims[i],c->peer_generations[i]);
        klog("[ata%u] fault unit=%u command=%02x issued=%u status=%02x error=%02x elapsed_ms=%llu cause=%s",
             c->index, d->unit, d->command, d->issued, d->status, d->error, d->elapsed_ms, cause);
    }
    return -FS_EIO;
}

static uint32_t read_signature(struct ata_channel *c)
{
    uint32_t value = reg_read(c, 2);
    value |= (uint32_t)reg_read(c, 3) << 8;
    value |= (uint32_t)reg_read(c, 4) << 16;
    return value | (uint32_t)reg_read(c, 5) << 24;
}
static bool packet_signature(uint32_t value)
{
    /* libata ata_dev_classify: some PACKET units only publish Mid/High. */
    return (value >> 16) == 0xeb14;
}
static void log_signature(struct ata_device *d, uint8_t status, uint32_t value)
{
    klog("[ata%u] unit=%u status=%02x signature=%02x%02x%02x%02x",
         d->channel->index, d->unit, status, value & 255,
         (value >> 8) & 255, (value >> 16) & 255, value >> 24);
}

/* ATA-6 8.15.5.2, 9.12 / QEMU cmd_identify: EC on a PACKET unit restores its
 * signature and aborts. This is classification, never a failed I/O retry.
 * Call only for IDENTIFY, after checking the deadline and clearing BSY. */
static bool packet_identify_abort(struct ata_device *d, uint8_t s)
{
    struct ata_channel *c = d->channel;
    if (!(s & ATA_ERR) || (s & (ATA_BSY | ATA_DRQ | ATA_DF)) || reg_read(c, 1) != 0x04)
        return false;
    uint32_t value = read_signature(c);
    log_signature(d, s, value);
    return packet_signature(value);
}
/* G11: a non-data command the device rejects (ATA-6 6.2: ERR with only ABRT
 * in the Error register, not BSY/DRQ/DF) is a refusal of that command, never
 * a channel fault. Only for optional power-management commands. */
#define ATA_CLASSIFY_PACKET 1
#define ATA_CLASSIFY_ABORT 2
static bool command_abort(struct ata_device *d, uint8_t s)
{
    return (s & ATA_ERR) && !(s & (ATA_BSY | ATA_DRQ | ATA_DF)) && reg_read(d->channel, 1) == 0x04;
}

/* Poll only bounded status transitions at reset/selection or within a
 * data phase. BSY makes every other status bit invalid (7.15). Sleep after
 * short sampling so a stuck device cannot monopolize this UP kernel. */
static int poll_status_result(struct ata_device *d, uint64_t start, uint64_t deadline,
                              uint8_t required, uint8_t forbidden, int classify)
{
    struct ata_channel *c = d->channel;
    for (unsigned samples = 0;; samples++) {
        uint8_t s = alternate(c);
        d->status = s;
        if (c->stopped || c->quiescing || expired(c, deadline))
            return failure(d, start, s);
        if (!(s & ATA_BSY)) {
            if (classify == ATA_CLASSIFY_PACKET && packet_identify_abort(d, s)) return 1;
            if (classify == ATA_CLASSIFY_ABORT && command_abort(d, s)) return 1;
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
static int poll_status(struct ata_device *d, uint64_t start, uint64_t deadline,
                       uint8_t required, uint8_t forbidden)
{
    return poll_status_result(d, start, deadline, required, forbidden, false);
}

struct irq_wait { struct ata_channel *channel; uint32_t sequence; gen_t generation; };
static bool irq_ready(void *arg)
{
    struct irq_wait *w = arg;
    /* f1-92: no request-interruption term. An issued command waits for its
     * device interrupt or its own device deadline, never for a caller. */
    return w->channel->stopped || w->channel->quiescing ||
        (w->channel->irq_sequence != w->sequence && w->channel->irq_generation == w->generation);
}

static int wait_irq_until(struct ata_device *d, uint32_t *sequence, uint64_t start,
                          uint64_t deadline, int classify, uint8_t timeout_cause)
{
    uint64_t check = hardware_check_io_deadline ? hardware_check_io_deadline() : 0;
    if (check && check < deadline) deadline = check;
    struct ata_channel *c = d->channel;
    struct irq_wait w = { c, *sequence, c->request_generation };
    if (expired(c, deadline) || !kwait_wait_until(&c->wait, irq_ready, &w,
                                                c->ops->deadline_ticks(c->port_ctx, deadline))) {
        if (c->expiry != ATA_EXPIRY_CHECK) c->expiry = timeout_cause;
        return failure(d, start, alternate(c));
    }
    uint32_t f = irq_save();
    uint8_t s = c->irq_status;
    *sequence = c->irq_sequence;
    irq_restore(f);
    d->status = s;
    if (c->stopped || c->quiescing || expired(c, deadline)) {
        if (c->expiry == ATA_EXPIRY_DEADLINE) c->expiry = timeout_cause;
        return failure(d, start, s);
    }
    if (classify == ATA_CLASSIFY_PACKET && packet_identify_abort(d, s)) return 1;
    if (classify == ATA_CLASSIFY_ABORT && !(s & ATA_BSY) && command_abort(d, s)) return 1;
    if (!(s & ATA_BSY) && (s & (ATA_ERR | ATA_DF)))
        return failure(d, start, s);
    return 0;
}
static int wait_irq_result(struct ata_device *d, uint32_t *sequence, uint64_t start,
                           uint64_t deadline, int classify)
{
    return wait_irq_until(d, sequence, start, deadline, classify, ATA_EXPIRY_DEADLINE);
}
static int wait_irq(struct ata_device *d, uint32_t *sequence, uint64_t start, uint64_t deadline)
{
    return wait_irq_result(d, sequence, start, deadline, false);
}
/* f1-92: an interrupt edge that was not the awaited event (early, stale or a
 * late edge of an earlier command). Thread context only; bounded records. */
static void early_irq(struct ata_device *d, const char *phase, uint8_t status, uint8_t bm)
{
    struct ata_channel *c = d->channel;
    if (d->early_irqs != UINT32_MAX) d->early_irqs++;
    if (record_due(d->early_irqs))
        klog("[ata%u] early_irq unit=%u phase=%s command=%02x generation=%u status=%02x bm=%02x count=%u",
             c->index, d->unit, phase, d->command, d->generation, status, bm, d->early_irqs);
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
    if (c->native_irq && !c->dma.armed && c->dma.ops) {
        uint8_t bm = c->dma.ops->in8(c->dma.ctx, (uint16_t)(c->dma.base + 2));
        c->dma.ops->out8(c->dma.ctx, (uint16_t)(c->dma.base + 2), (bm & 0x60) | ATA_BM_IRQ);
    }
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
    /* ATA-6 7.8.6: nIEN gates the device INTRQ. The registered PIC line
     * stays open between requests; late edges only acknowledge Status. */
    device_control(c, c->control_shadow | ATA_NIEN);
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
    d->toc.valid = false; d->audio_only = false; d->cd_playing = false;
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
    if (!e && *done) d->block.stats.pio++;
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
             d->channel->index, d->unit, p[0], d->sense_key, d->asc, d->ascq,
             d->block.media_generation, -FS_EIO);
    d->elapsed_ms=now(d->channel)-start;
    if (d->elapsed_ms>d->max_packet_ms) d->max_packet_ms=d->elapsed_ms;
    return -FS_EIO; /* a failed data request is never retried */
}
static int packet_admit(struct ata_device *d, const uint8_t *p, unsigned length,
                 void *buf, uint32_t cap, uint32_t *done, bool check_generation, uint32_t generation)
{
    if (!d || !d->identified || !d->atapi || !p || !length || length > d->packet_bytes ||
        !done || cap > ATAPI_MAX_BYTES || (cap && !buf)) return -FS_EINVAL;
    switch (p[0]) {
    case 0: case 3: case 0x12: case 0x25: case 0x28: case 0x43: case 0x1b: case 0x1e: break;
    /* f5-12B2 CD audio (optical_cd.c): MODE SENSE(10), READ CD, READ
     * SUB-CHANNEL, PLAY AUDIO MSF, PAUSE/RESUME, STOP PLAY/SCAN. Still
     * device-to-host only; the audio play commands carry no data. */
    case 0x5a: case 0xbe: case 0x42: case 0x47: case 0x4b: case 0x4e: break;
    default: return -FS_EOPNOTSUPP;
    }
    kmutex_lock(&d->channel->mutex);
    if (p[0] == 0x1b && length > 4 && (p[4] & 3) == 2 && d->cd_playing) {
        *done = 0;
        kmutex_unlock(&d->channel->mutex);
        return -FS_EBUSY;
    }
    if (check_generation && (generation != d->block.media_generation || d->media != ATAPI_READY)) {
        *done = 0;
        kmutex_unlock(&d->channel->mutex);
        return -FS_EAGAIN;
    }
    int e = packet_checked(d, p, length, buf, cap, done,
                          p[0] == 0x1b && (length < 2 || !(p[1] & 1)) ? ATAPI_START_MS : ATAPI_COMMAND_MS);
    if (!e) {
        d->sense_key = d->asc = d->ascq = d->error = 0;
        if (p[0] == 0x47) d->cd_playing = true;
        else if (p[0] == 0x4e) d->cd_playing = false;
        else if (p[0] == 0x42 && *done >= 2) {
            uint8_t status = ((const uint8_t *)buf)[1];
            d->cd_playing = status == 0x11 || status == 0x12;
        }
    }
    kmutex_unlock(&d->channel->mutex);
    return e;
}
int atapi_packet(struct ata_device *d, const uint8_t *p, unsigned length,
                 void *buf, uint32_t cap, uint32_t *done)
{ return packet_admit(d, p, length, buf, cap, done, false, 0); }
int atapi_packet_generation(struct ata_device *d, const uint8_t *p, unsigned length,
                 void *buf, uint32_t cap, uint32_t *done, uint32_t generation)
{ return packet_admit(d, p, length, buf, cap, done, true, generation); }
static uint32_t packet_be32(const uint8_t *p)
{ return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static int ready_result(struct ata_device *d, const char *stage, int e, uint32_t bytes)
{
    if (!d) return e;
    if (d->ready_stage != stage || d->ready_error != e)
        klog("[optical] media channel=%u unit=%u stage=%s result=%d bytes=%u status=%02x error=%02x key=%02x asc=%02x ascq=%02x generation=%u sectors=%llu session=%u quarantined=%u",
             d->channel->index, d->unit, stage, e, bytes, d->status, d->error,
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
                 d->channel->index, d->unit, d->block.media_generation, spin_error);
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
    /* f5-12B2: some drives refuse READ CAPACITY on a CD-DA disc (ILLEGAL
     * REQUEST / MEDIUM ERROR) or report a block length other than 2048.
     * Only those answers allow the TOC to decide; a missing medium, a
     * transport failure or quarantine still ends the readiness attempt
     * here, exactly as in F4. */
    bool capacity_unknown = e && !d->block.quarantined && d->media != ATAPI_NO_MEDIUM &&
        (e == -FS_EOPNOTSUPP || (e == -FS_EIO && (d->sense_key == 3 || d->sense_key == 5)));
    uint32_t capacity_bytes = n;
    if (!capacity_unknown) { ready_result(d, "capacity", e, n); if (e) return e; }
    int capacity_error = e;
    if (!e) d->block.capacity = (uint64_t)packet_be32(capacity) + 1;
    /* The whole TOC (format 0, LBA; cached for CD audio), then last-session
     * information. QEMU/SFF encode format 1 in byte 9, unlike later MMC's
     * additional byte-2 field. */
    e = atapi_cd_toc(d, true);
    if (e || !capacity_error)
        ready_result(d, "toc", e, d->toc.valid ? 4u + 8u * (d->toc.count + 1u) : 0);
    if (e) return capacity_error ? ready_result(d, "capacity", capacity_error, capacity_bytes) : e;
    d->audio_only = d->toc.audio_only;
    if (d->toc.first_audio) {
        /* f5-12B2 (lead decision 4): an audio first track is ready for the
         * CD-audio interface and never mounted; no file system is probed.
         * F4 keeps mounting only a disc whose first track is data. The
         * "capacity" of a disc whose READ CAPACITY failed is its lead-out
         * LBA, so the repeat-poll fast path above still applies. */
        if (capacity_error) d->block.capacity = d->toc.leadout ? d->toc.leadout : 1;
        d->session_lba = 0; d->media = ATAPI_READY;
        ready_result(d, "audio", 0, d->toc.count);
        return 0;
    }
    /* A data disc whose capacity is unusable keeps the F4 refusal. */
    if (capacity_error) return ready_result(d, "capacity", capacity_error, capacity_bytes);
    d->session_lba = d->toc.start[0];
    uint8_t toc[12];
    memset(p, 0, sizeof(p)); p[0] = 0x43; p[8] = 12; p[9] = 0x40;
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
    if(ata_addin_match(d->channel->pci)) return -FS_EOPNOTSUPP;
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
        klog("[ata%u] atapi identify unit=%u stage=%u error=%d status=%02x error_reg=%02x", c->index, d->unit, stage, e, alternate(c), reg_read(c, 1));
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
    d->identified = true; d->block.request_read = atapi_read; d->block.read = blkdev_read; d->block.write = 0; d->block.flush = 0;
    d->block.sector_size = 2048; d->block.media_generation = 1; d->block.media_check = atapi_check_block;
    klog("[ata%u] unit=%u kind=atapi type=%u packet_bytes=%u mode=pio reason=atapi_pio",
         c->index, d->unit, d->packet_type, d->packet_bytes);
    return 0;
}

/* A separate frame keeps the IDENTIFY buffer out of its caller's frame.
 * Verify both the selected transfer mode and the disk that accepted it. */
__attribute__((noinline)) static int verify_udma(struct ata_device *d, int mode, const uint8_t before[512])
{
    struct ata_channel *c = d->channel;
    uint8_t data[512]; uint32_t sequence;
    uint64_t start = now(c), deadline = start + ATA_COMMAND_MS;
    int e = select_device(d, start, deadline, 0);
    if (!e) device_control(c, (uint8_t)(c->control_shadow & ~ATA_NIEN));
    if (!e) e = issue(d, ATA_IDENTIFY, start, &sequence);
    if (!e) e = wait_irq(d, &sequence, start, deadline);
    if (!e) e = poll_status(d, start, deadline, ATA_DRQ, 0);
    if (!e) e = data_block(d, data, false, start, deadline);
    if (!e) e = poll_status(d, start, deadline, 0, ATA_DRQ);
    disarm(c);
    if (e) return e;
    if (ata_dma_mode(data) != mode || memcmp(data + 20, before + 20, 20) ||
        memcmp(data + 46, before + 46, 48) || memcmp(data + 120, before + 120, 4) ||
        memcmp(data + 200, before + 200, 8)) {
        c->fault_cause = "mode_verify";
        return failure(d, start, alternate(c));
    }
    return 0;
}

static void identity_text(char *out, const uint8_t *words, unsigned chars)
{
    unsigned n = 0;
    for (unsigned i = 0; i < chars; i++) {
        uint8_t ch = words[i ^ 1u];
        if (!n && ch == ' ') continue;
        out[n++] = ch >= 32 && ch <= 126 && ch != '=' ? (char)(ch == ' ' ? '_' : ch) : '_';
    }
    while (n && out[n - 1] == '_') n--;
    out[n] = 0;
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
        e = wait_irq_result(d, &sequence, start, deadline, ATA_CLASSIFY_PACKET);
    if (!e)
        e = poll_status_result(d, start, deadline, ATA_DRQ, 0, ATA_CLASSIFY_PACKET);
    if (e == 1) {
        /* Retire EC's IRQ/generation and restore nIEN before issuing A1.
         * All reset signatures have already been captured by pass 1. */
        disarm(c);
        d->atapi = true;
        klog("[ata%u] unit=%u identify=packet_signature", c->index, d->unit);
        return identify_packet(d);
    }
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
    /* f9-02 disk identity (ATA/ATAPI-6 d1410r3a 8.15 IDENTIFY DEVICE: serial
     * number words 10-19, firmware revision words 23-26; ASCII, two characters
     * per word, the first in bits 15:8). Trim the padding; data only, no I/O. */
    identity_text(d->serial, data + 20, 20);
    identity_text(d->firmware, data + 46, 8);
    enum blkdev_cache_state cache = BLKDEV_CACHE_UNKNOWN;
    /* 8.15.42/43: honour validity bits before interpreting words 82/85. */
    if (supported_valid && !(w82 & (1u << 5)))
        cache = BLKDEV_CACHE_DISABLED;
    else if (supported_valid && enabled_valid)
        cache = w85 & (1u << 5) ? BLKDEV_CACHE_ENABLED : BLKDEV_CACHE_DISABLED;
    d->flush_ext = d->lba48 && (w83 & (1u << 13));
    /* G11: 8.15.38 word 82 bit 3, Power Management feature set (IDLE E3h). */
    d->power_mgmt = supported_valid && (w82 & (1u << 3));
    d->block.capacity = cap;
    d->block.write_cache_state = cache;
    d->block.request_read = ata_read; d->block.read = blkdev_read;
    d->block.request_write = ata_write; d->block.write = blkdev_write;
    d->block.emergency_write=ata_emergency_write;
    d->block.request_flush = supported_valid && ((w83 & (1u << 12)) || d->flush_ext) ? ata_flush : 0;
    d->block.flush = d->block.request_flush ? blkdev_flush : 0;
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
            e = issue(d, 0xc6, start, &sequence);
        }
        if (!e) e = wait_irq(d, &sequence, start, deadline);
        if (!e) e = poll_status(d, start, deadline, 0, ATA_DRQ);
        disarm(c);
        if (e) { d->multiple_sectors = 0; return e; }
        d->multiple_sectors = (uint8_t)maximum;
    }
    d->identified = true;
    struct ata_timing_plan plan;
    bool negotiated = c->chipset.pci != 0;
    int mode = -1;
    if (c->dma.ops) mode = negotiated ? ata_chipset_plan(&c->chipset, d->unit, data, &plan) :
                                       ata_dma_restore_mode(d->firmware_udma, data);
    if (mode >= 0 && d->lba48 && c->chipset.no_lba48_dma) {
        /* This transport always uses EXT commands on LBA48 disks. ALi C2-C4
         * cannot DMA those commands, even for low addresses. */
        mode = -1; c->chipset.reason = c->chipset.family==ATA_CHIP_PDC_OLD ? "promise_ext_pio" : "ali_lba48_pio";
    }
    if (mode >= 0 && negotiated) {
        e = ata_chipset_program(c, &plan);
        if (e) {
            c->fault_cause = "chipset_program";
            return failure(d, now(c), alternate(c));
        }
    }
    if (mode >= 0) {
        d->udma_mode = (uint8_t)mode;
        start = now(c); deadline = start + ATA_COMMAND_MS;
        e = select_device(d, start, deadline, 0);
        if (!e) {
            d->dma_configuring = !negotiated;
            d->issued = false;
            device_control(c, (uint8_t)(c->control_shadow & ~ATA_NIEN));
            reg_write(c, 1, 3); reg_write(c, 2, (uint8_t)(0x40 | mode));
            e = issue(d, 0xef, start, &sequence);
        }
        if (!e) e = wait_irq(d, &sequence, start, deadline);
        if (!e) e = poll_status(d, start, deadline, 0, ATA_DRQ);
        d->dma_configuring = false;
        disarm(c);
        if (c->stopped) return e;
        if(!e && negotiated && ata_addin_post_mode(d)) {
            c->fault_cause="promise_autotiming_readback";
            return failure(d,now(c),alternate(c));
        }
        if (!e && negotiated) e = verify_udma(d, mode, data);
        if (c->stopped) return e;
        d->dma_enabled = !e;
        if (!e) klog("[ata%u] dma mode=udma%u unit=%u bm=%04x cable80=%u buffer_bytes=%u",
                      c->index, d->udma_mode, d->unit, c->dma.base,
                      (!negotiated || c->chipset.cable80) && (fs_rd16(data + 93 * 2) & 0xe000) == 0x6000, ATA_DMA_BYTES);
    }
    klog("[ata%u] chipset family=%u pci=%04x:%04x revision=%02x native=%u unit=%u mode=%s reason=%s clock_mhz=%u cable80=%u",
         c->index, c->chipset.family, c->pci ? c->pci->vendor : 0, c->pci ? c->pci->device : 0,
         c->pci ? c->pci->revision : 0, c->native_irq, d->unit,
         d->dma_enabled ? "udma" : "pio", d->dma_enabled ? (negotiated ? "negotiated" : "firmware_restore") :
         !c->dma.ops ? "bm_unavailable" : negotiated ? c->chipset.reason : "firmware_mode_unavailable",
         c->chipset.clock_mhz,c->chipset.cable80);
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
        if (status == 0xff) return 0;
        if (!(status & ATA_BSY)) break;
        if (expired(c, deadline)) return failure(d, start, status);
        task_sleep_ms(1);
    }
    uint32_t value = read_signature(c);
    log_signature(d, status, value);
    if (packet_signature(value) || !status) return 0;
    int e = issue(d, ATA_IDENTIFY, start, &sequence);
    while (!e) {
        status = alternate(c);
        if (expired(c, deadline)) { e = failure(d, start, status); break; }
        if (status & ATA_BSY) { task_sleep_ms(1); continue; }
        if (packet_identify_abort(d, status)) break;
        /* The abort may arrive between the status sample above and the
         * DRQ poll. Classify it there too, before generic containment. */
        e = poll_status_result(d, start, deadline, ATA_DRQ, 0, ATA_CLASSIFY_PACKET);
        if (e == 1) { e = 0; break; }
        if (!e) e = data_block(d, data, false, start, deadline);
        if (!e) e = poll_status(d, start, deadline, 0, ATA_DRQ);
        if (!e) d->firmware_udma = (int8_t)ata_dma_mode(data);
        break;
    }
    reg_read(c, 7); disarm(c);
    return e;
}

struct reset_signature { uint32_t value; uint8_t status; bool present; };

static int read_reset_signature(struct ata_device *d, struct reset_signature *sig)
{
    struct ata_channel *c = d->channel;
    uint64_t start = now(c), deadline = start + ATA_COMMAND_MS;
    /* Wait for the previous selection before switching, then wait for the
     * newly selected unit. DRDY is deliberately not required for PACKET. */
    for (;;) {
        uint8_t s = alternate(c);
        if (expired(c, deadline)) return failure(d, start, s);
        if (s == 0xff || !(s & ATA_BSY)) break;
        task_sleep_ms(1);
    }
    reg_write(c, 6, (uint8_t)(0xa0 | d->unit << 4));
    if (pace(c)) return failure(d, start, alternate(c));
    bool sampled = false;
    uint32_t previous = 0;
    uint8_t previous_status = 0;
    for (;;) {
        uint8_t s = alternate(c);
        if (expired(c, deadline) || c->stopped || c->quiescing)
            return failure(d, start, s);
        if (s == 0xff) return 0;
        if (!(s & ATA_BSY)) {
            uint32_t value = read_signature(c);
            /* Bracket the task file with BSY-clear status. PACKET's
             * Mid/High pair is decisive even while SC/Low or DRDY/DSC
             * change; other signatures still need identical snapshots. */
            uint8_t ending = alternate(c);
            if (!sampled || value != previous || s != previous_status)
                log_signature(d, s, value);
            bool packet = packet_signature(value);
            if (ending != 0xff && !(ending & ATA_BSY) && (ending == s || packet)) {
                if (sampled && (packet ? packet_signature(previous) :
                                          value == previous && s == previous_status)) {
                    sig->value = value; sig->status = s;
                    sig->present = s || packet;
                    return 0;
                }
                sampled = true; previous = value; previous_status = s;
            } else sampled = false;
        } else sampled = false;
        task_sleep_ms(1);
    }
}

static int ata_raid_screen(struct ata_device *d)
{
    uint64_t locations[54]; uint8_t data[512];
    unsigned count=ata_raid_locations(d->block.capacity,locations,ARRAY_SIZE(locations));
    bool dma=d->dma_enabled;d->dma_enabled=false;
    int e=count ? 0 : -FS_EIO;
    for(unsigned i=0;i<count && !e;i++) {
        e=command_data(d,locations[i],1,data,false);
        if(e) break;
        d->raid_reason=ata_raid_signature(data);
        if(d->raid_reason) {d->raid_member=true;break;}
    }
    d->raid_checked=!e;
    d->dma_enabled=dma && !e && !d->raid_member;
    if(e || d->raid_member) {
        d->block.quarantined=true;
        d->block.write=0;d->block.request_write=0;d->block.flush=0;
        d->block.request_flush=0;d->block.emergency_write=0;
        if(e) d->raid_reason="raid_metadata_unreadable";
    }
    klog("[ata%u] unit=%u raid=%s reason=%s setup_install=unqualified",
        d->channel->index,d->unit,e ? "unknown" : d->raid_member ? "member" : "plain",
        d->raid_reason ? d->raid_reason : "metadata_absent");
    return e;
}
int ata_channel_discover(struct ata_channel *c)
{
    if (!c || !c->ops)
        return -FS_EINVAL;
    ata_lock(c);
    if (c->discovered || c->stopped || c->quiescing) {
        int e = c->stopped || c->quiescing ? -FS_EQUARANTINED : 0;
        ata_unlock(c);
        return e;
    }
    c->discovered = true;
    int e = 0;
    bool addin=ata_addin_match(c->pci);
    if(addin) {
        for(unsigned i=0;c->native && i<c->index;i++) if(channels[i].pci==c->pci) {
            struct ata_chipset *p=&channels[i].chipset;
            if(channels[i].stopped) {e=-FS_EQUARANTINED;break;}
            if(p->prepared) {
                c->chipset.prepared=true;c->chipset.layout=p->layout;
                c->chipset.clock_mhz=p->clock_mhz;c->chipset.maximum=p->maximum;
                c->chipset.pci_mhz=p->pci_mhz;
            }
        }
        if(c->native && c->pci->vendor==0x1103 && c->pci->device==8 && (c->pci->fn&1))
            for(unsigned i=0;i<c->index;i++) {
                const struct ata_channel *p=&channels[i];
                if(p->pci && p->pci->bus==c->pci->bus && p->pci->dev==c->pci->dev &&
                    p->pci->fn+1==c->pci->fn && !p->stopped && p->chipset.prepared)
                    c->chipset.pci_mhz=p->chipset.pci_mhz;
            }
        if(!e) e=ata_addin_prepare(c);
        if(e) {
            c->fault_cause="addin_prepare";
            klog("[ata%u] mode=pio unavailable=1 reason=%s clock_mhz=%u",c->index,c->chipset.reason,c->chipset.clock_mhz);
            failure(&c->devices[0],now(c),alternate(c));
            ata_unlock(c);return e;
        }
    }
    if (c->dma.ops) {
        device_control(c, c->control_shadow | ATA_NIEN);
        for (unsigned i = 0; i < ATA_DEVICES && !e; i++) e = firmware_mode(&c->devices[i]);
        if (e) { ata_unlock(c); return e; }
    }
    /* 9.2: SRST >=5 us, then >=2 ms before inspecting reset signature.
     * Discovery is the only reset; failures never rearm native channels. */
    device_control(c, c->control_shadow | ATA_NIEN | ATA_SRST);
    e = c->ops->delay_us(c->port_ctx, 5);
    device_control(c, (uint8_t)(c->control_shadow & ~ATA_SRST));
    task_sleep_ms(2);
    uint64_t start = now(c);
    int found = 0;
    /* Pass 1: read every unit's reset signature before any command on the
     * channel. Task-file writes reach both devices (QEMU ide/core.c writes
     * the Sector Count of both drives; libata-sff classifies dev0 and dev1
     * right after the soft reset): the SET FEATURES that programs the
     * master's UDMA mode left 0x45 in the slave's Sector Count and the old
     * single pass then skipped a present slave (gate evidence 2026-10-16). */
    struct reset_signature sig[ATA_DEVICES] = {{0}};
    for (unsigned i = 0; i < ATA_DEVICES && !e; i++) {
        struct ata_device *d = &c->devices[i];
        d->issued = false;
        e = read_reset_signature(d, &sig[i]);
    }
    /* Pass 2: classify and identify. Commands may now change task files. */
    for (unsigned i = 0; i < ATA_DEVICES && !e; i++) {
        struct ata_device *d = &c->devices[i];
        if (!sig[i].present)
            continue;
        reg_write(c, 6, (uint8_t)(0xa0 | i << 4));
        if (pace(c)) { e = failure(d, start, alternate(c)); break; }
        if (packet_signature(sig[i].value)) {
            if(addin) {klog("[ata%u] unit=%u reason=addin_plain_disk_only",c->index,i);continue;}
            d->present = d->atapi = true;
            e = identify_packet(d);
            if (e == -FS_EOPNOTSUPP) { e = 0; continue; }
            if (!e) found++;
            continue;
        }
        if (sig[i].value != 0x00000101u)
            continue;
        d->present = true;
        /* Edge handler is published before native discovery; nIEN is
         * cleared only for an ATA IDENTIFY, never for skipped ATAPI. */
        device_control(c, (uint8_t)(c->control_shadow & ~ATA_NIEN));
        e = identify(d);
        device_control(c, c->control_shadow | ATA_NIEN);
        if (e == -FS_EOPNOTSUPP && d->atapi) { e = 0; continue; }
        if (!e)
            found++;
    }
    /* No member or unexamined disk reaches a request worker or a mount. */
    for(unsigned i=0;addin && i<ATA_DEVICES && !e;i++) {
        struct ata_device *d=&c->devices[i];
        if(!d->identified || d->atapi) continue;
        e=ata_raid_screen(d);
        if(d->raid_member) found--;
    }
    if (e && !c->stopped)
        e = failure(&c->devices[0], start, alternate(c));
    ata_unlock(c);
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

/* f1-92: an edge counted between the DMA command and Start. ATA-6 9.7: a DMA
 * data command interrupts once, after all of its data or on an abort with
 * ERR; with the master never started no data can have moved, so an edge
 * without ERR/DF from a present drive (BSY, DRQ/DMARQ or a stale DRDY) and a
 * master without Active/Error is not this command's end. The ISR already
 * acknowledged the drive and cleared the master's Interrupt bit (W1C):
 * consume the edge, start the master and keep the command's deadline.
 * Anything else stays a DMA failure with the existing classification. */
static int dma_early(struct ata_device *d, uint32_t *sequence, uint64_t start)
{
    struct ata_channel *c = d->channel;
    uint32_t f = irq_save();
    uint8_t snapshot = c->irq_status, bm = c->dma.irq_status;
    irq_restore(f);
    uint8_t s = alternate(c);
    bm |= c->dma.ops->in8(c->dma.ctx, c->dma.base + 2);
    uint8_t fault = (uint8_t)((s | snapshot) & (ATA_ERR | ATA_DF));
    if (fault || s == 0xff || !s || snapshot == 0xff || (bm & (ATA_BM_ACTIVE | ATA_BM_ERROR))) {
        d->status = fault && !(s & fault) ? snapshot : s;
        return dma_failure(d, start, d->status);
    }
    early_irq(d, "before_start", snapshot, bm);
    f = irq_save();
    *sequence = c->irq_sequence;
    c->dma.irq_status = 0;
    ata_dma_start(c);
    irq_restore(f);
    return 0;
}
/* f1-92: after Start, return at the bus-master completion (Interrupt or
 * Error captured by the ISR). A stray edge left the master running (see
 * ata_dma_irq): while the drive shows BSY or DRQ the transfer is live, wait
 * on with the same deadline (ATA-6 9.7 HDMA1/HDMA2). If the drive already
 * reports the end of the command without a latched Interrupt, the real edge
 * is either in flight or missing: wait a bounded grace, then fail as
 * bm_no_interrupt (the timeout path stops the master and proves idle). */
#define ATA_DMA_IRQ_GRACE_MS 10u
static int dma_wait(struct ata_device *d, uint32_t *sequence, uint64_t start, uint64_t deadline)
{
    struct ata_channel *c = d->channel;
    uint64_t limit = deadline;
    uint8_t cause = ATA_EXPIRY_DEADLINE;
    for (;;) {
        int e = wait_irq_until(d, sequence, start, limit, false, cause);
        if (e) return e;
        uint32_t f = irq_save();
        uint8_t bm = c->dma.irq_status, snapshot = c->irq_status;
        irq_restore(f);
        if (bm & (ATA_BM_IRQ | ATA_BM_ERROR)) return 0;
        early_irq(d, "dma_active", snapshot, bm);
        uint8_t s = alternate(c);
        if (s != 0xff && (s & (ATA_BSY | ATA_DRQ)) && !(s & (ATA_ERR | ATA_DF))) {
            limit = deadline; cause = ATA_EXPIRY_DEADLINE;
            continue;
        }
        uint64_t grace = now(c) + ATA_DMA_IRQ_GRACE_MS;
        if (grace < limit) { limit = grace; cause = ATA_EXPIRY_NO_BM_IRQ; }
    }
}

static int command_data(struct ata_device *d, uint64_t lba, uint32_t n, uint8_t *buf, bool write)
{
    struct ata_channel *c = d->channel;
    if (c->stopped || c->quiescing)
        return -FS_EQUARANTINED;
    uint64_t start = now(c), deadline = start + ATA_COMMAND_MS;
    uint64_t first_start = d->dma_enabled && !c->dma_completion_logged && g_tsc_per_ms ? ktime_cycles() : 0;
    uint32_t sequence;
    d->issued = false;
    uint8_t head = (uint8_t)(0x40 | (d->lba48 ? 0 : (lba >> 24) & 15));
    int e = select_device(d, start, deadline, head);
    if (e)
        return e;
    bool dma = d->dma_enabled && !(d->lba48 && c->chipset.no_lba48_dma);
    if (dma) {
        e = ata_dma_prepare(c, buf, n * 512, write);
        if (e) return dma_failure(d, start, alternate(c));
        if(ata_addin_dma_begin(d,n*512,write)) {
            c->fault_cause="addin_dma_clock";
            /* A failed clock change cannot authorize the generic PIO retry. */
            (void)ata_dma_stop(c);c->dma.armed=false;
            ata_dma_finish(c,buf,write,false);
            return failure(d,start,alternate(c));
        }
    }
    device_control(c, (uint8_t)(c->control_shadow & ~ATA_NIEN));
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
    if (write) d->write_commands++; /* f9-01 diagnostic: every issued write command */
    e = issue(d, command, start, &sequence);
    if (dma) {
        /* SFF/Intel bus master 3.1: command, then Start. An edge counted in
         * between is classified by dma_early (f1-92) before any Start. */
        bool early = false;
        if (!e) {
            uint32_t f = irq_save();
            early = c->irq_sequence != sequence;
            if (!early) ata_dma_start(c);
            irq_restore(f);
        }
        if (!e && early) e = dma_early(d, &sequence, start);
        if (!e) e = dma_wait(d, &sequence, start, deadline);
        uint8_t s = alternate(c), bm = c->dma.irq_status;
        if (!e && (!c->dma.started || (bm & (ATA_BM_ACTIVE | ATA_BM_ERROR | ATA_BM_IRQ)) != ATA_BM_IRQ || !(s & ATA_DRDY) ||
                   (s & (ATA_BSY | ATA_DRQ | ATA_ERR | ATA_DF)))) e = dma_failure(d, start, s);
        if (!ata_dma_stop(c) && !e) e = dma_failure(d, start, s);
        ata_dma_finish(c, buf, write, !e);
        disarm(c);
        d->elapsed_ms = now(c) - start;
        if (!e) {
            d->block.stats.dma++;
            if (!c->dma_completion_logged) {
                c->dma_completion_logged = true;
                uint64_t us = first_start ? (ktime_cycles()-first_start)*1000/g_tsc_per_ms : d->elapsed_ms*1000;
                klog("[ata%u] dma first-completion unit=%u elapsed_us=%u",
                    c->index, d->unit, us>UINT32_MAX ? UINT32_MAX : (uint32_t)us);
            }
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
        if (!write || i) {
            e = wait_irq(d, &sequence, start, deadline);
            /* f1-92, ATA-6 9.5/9.6: the drive asserts INTRQ only with BSY=0
             * (DRQ for a block, or the error/end status). A BSY snapshot is
             * an early or stale edge; the DRQ poll below waits for the real
             * block, which is the data phase (status 58h), never a fault. */
            if (!e && (c->irq_status & ATA_BSY)) early_irq(d, "pio", c->irq_status, 0);
        }
        if (!e)
            e = poll_status(d, start, deadline, ATA_DRQ, 0);
        if (!e)
            e = transfer_block(d, buf + i * 512, sectors, write, start, deadline);
        i += sectors;
        if (!e && i < n)
            task_yield();
    }
    if (!e && write) {
        e = wait_irq(d, &sequence, start, deadline);
        if (!e && (c->irq_status & ATA_BSY)) early_irq(d, "pio", c->irq_status, 0);
    }
    if (!e)
        e = poll_status(d, start, deadline, 0, ATA_DRQ);
    disarm(c);
    d->elapsed_ms = now(c) - start;
    if (!e) d->block.stats.pio++;
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
    ata_lock(c);
    bool addin=ata_addin_match(c->pci);
    /* Recheck after sleeping behind a failed request. No port write on
     * invalid/quarantined requests, including requests queued on a sibling. */
    e = blkdev_range(b, lba, n);
    if (!e && (!d->identified || c->stopped || c->quiescing))
        e = -FS_EQUARANTINED;
    if(!e && addin && (!d->raid_checked || d->raid_member)) e=-FS_EQUARANTINED;
    uint8_t *p = buf;
    while (!e && n) {
        /* f1-92: a cancelled or out-of-budget request stops here, between
         * commands, with no port access; an issued command always ends on
         * the device's own terms. */
        if (request_interrupted()) { e = -FS_EAGAIN; break; }
        uint32_t chunk = n > BLKDEV_READ_SECTORS ? BLKDEV_READ_SECTORS : n;
        bool dma = d->dma_enabled;
        uint64_t started = now(c);
        e = command_data(d, lba, chunk, p, write);
        if (e == -FS_EIO && dma && d->dma_quarantined && !d->dma_enabled &&
            !c->stopped && !c->quiescing && !b->quarantined) {
            /* f1-92 fallback policy: dma_failure proved drive and master idle
             * (fallback=pio). Repeat this same request once through PIO with
             * the same buffer and LBAs (sector writes are idempotent) before
             * any error reaches the volume; only a failed PIO repeat ends in
             * the channel quarantine and a read-only volume. */
            if (d->dma_retries != UINT32_MAX) d->dma_retries++;
            gen_t failed = d->generation;
            e = command_data(d, lba, chunk, p, write);
            klog("[ata%u] dma retry unit=%u generation=%u command=%02x mode=pio result=%d count=%u",
                 c->index, d->unit, failed, d->command, e, d->dma_retries);
        }
        if (d->issued && request_interrupted()) {
            if (d->interrupted_inflight != UINT32_MAX) d->interrupted_inflight++;
            if (record_due(d->interrupted_inflight))
                klog("[ata%u] interrupted_inflight unit=%u command=%02x result=%d elapsed_ms=%llu count=%u",
                     c->index, d->unit, d->command, e, now(c) - started, d->interrupted_inflight);
        }
        if (e)
            break;
        lba += chunk;
        n -= chunk;
        p += chunk * BLKDEV_SECTOR_SIZE;
        if (n)
            task_yield();
    }
    ata_unlock(c);
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
    ata_lock(c);
    bool addin=ata_addin_match(c->pci);
    int e = b->quarantined || c->stopped || c->quiescing ? -FS_EQUARANTINED : 0;
    if(!e && addin && (!d->raid_checked || d->raid_member)) e=-FS_EQUARANTINED;
    if (!e) {
        /* 8.12.8 permits >30 s; f1-acceptance sets a 60 s flush limit. */
        uint64_t start = now(c), deadline = start + ATA_FLUSH_MS;
        uint32_t sequence;
        d->issued = false;
        e = select_device(d, start, deadline, 0);
        if (!e) {
            device_control(c, (uint8_t)(c->control_shadow & ~ATA_NIEN));
            d->write_commands++;
            e = issue(d, d->flush_ext ? 0xea : 0xe7, start, &sequence);
        }
        if (!e)
            e = wait_irq(d, &sequence, start, deadline);
        if (!e)
            e = poll_status(d, start, deadline, 0, ATA_DRQ);
        disarm(c);
        d->elapsed_ms = now(c) - start;
    }
    ata_unlock(c);
    return e;
}

/* G11 (docs/design/power-policy.md): ATA/ATAPI-6 8.14 IDLE (E3h) with the
 * standby timer in the Count register, a non-data command. Only on a fixed
 * disk whose IDENTIFY word 82 bit 3 advertises the Power Management feature
 * set. An ABRT is that disk's refusal (recorded, the channel stays in
 * service); any other failure keeps the whole-channel quarantine rule. The
 * drive returns to Active by itself at the next media access. */
int ata_standby_timer(struct ata_device *d, uint8_t count)
{
    struct ata_channel *c = d->channel;
    if (!d->identified || d->atapi || !d->power_mgmt) return -FS_EOPNOTSUPP;
    if (hardware_check_io_cancelled && hardware_check_io_cancelled()) return -FS_EAGAIN;
    ata_lock(c);
    bool addin = ata_addin_match(c->pci);
    int e = d->block.quarantined || c->stopped || c->quiescing ? -FS_EQUARANTINED : 0;
    if (!e && addin && (!d->raid_checked || d->raid_member)) e = -FS_EQUARANTINED;
    if (!e) {
        uint64_t start = now(c), deadline = start + ATA_COMMAND_MS;
        uint32_t sequence;
        d->issued = false;
        e = select_device(d, start, deadline, 0);
        if (!e) {
            device_control(c, (uint8_t)(c->control_shadow & ~ATA_NIEN));
            reg_write(c, 2, count);
            e = issue(d, 0xe3, start, &sequence);
        }
        if (!e) e = wait_irq_result(d, &sequence, start, deadline, ATA_CLASSIFY_ABORT);
        if (!e) e = poll_status_result(d, start, deadline, 0, ATA_DRQ, ATA_CLASSIFY_ABORT);
        disarm(c);
        d->elapsed_ms = now(c) - start;
        if (e == 1) { d->error = 0x04; e = -FS_EOPNOTSUPP; }
    }
    d->standby_refused = e == -FS_EOPNOTSUPP;
    if (!e) d->standby_count = count;
    ata_unlock(c);
    return e;
}
int ata_standby_timer_all(uint32_t seconds, struct ata_standby_result *out)
{
    uint32_t applied;
    uint8_t count = ata_standby_count(seconds, &applied);
    *out = (struct ata_standby_result){0, 0, applied, 0};
    unsigned fixed = 0;
    for (unsigned i = 0; i < ATA_CHANNELS; i++) for (unsigned u = 0; u < ATA_DEVICES; u++) {
        struct ata_channel *c = &channels[i];
        struct ata_device *d = &c->devices[u];
        if (!c->ops || !d->identified || d->atapi) continue;
        fixed++;
        int e = ata_standby_timer(d, count);
        if (!e) out->disks++;
        else { out->refused++; if (!out->error) out->error = e; }
        klog("[ata%u] standby_timer unit=%u count=%u seconds=%u result=%d", c->index, u, count, applied, e);
    }
    return (int)fixed;
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
    if (ata_addin_match(c->pci) || (read_eflags()&0x200) || !data || blkdev_range(b,lba,1) || !d->identified || d->atapi ||
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
        d->write_commands++;
        reg_write(c,7,d->lba48 ? 0x34 : 0x30);
        for (unsigned i=0;i<4;i++) (void)alternate(c);
        e=emergency_poll(c,ATA_DRQ);
        if (!e) {
            for (unsigned i=0;i<256;i++) c->ops->out16(c->port_ctx,c->base,fs_rd16((const uint8_t *)data+i*2));
            for (unsigned i=0;i<4;i++) (void)alternate(c);
            e=emergency_poll(c,0);
        }
        if (!e && b->write_cache_state!=BLKDEV_CACHE_DISABLED) {
            d->write_commands++;
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
    kwait_wake_all(&c->wait);        /* an issued waiter aborts, never issues another command */
    bool master_idle = ata_dma_stop(c);
    if (!master_idle) ata_dma_revoke(c);
    /* No queued driver callbacks exist. An uncertain busy device/master
     * cannot supply a release proof; quarantine retains its claims. */
    bool idle = !c->active && !c->mutex.owner && master_idle && !c->stopped &&
                !(alternate(c) & (ATA_BSY | ATA_DRQ));
    if (c->native) {
        if (c->native_irq) {
            if (idle && c->native_irq_attached) {
                if (irq_chain_remove(c->irq, ata_pci_interrupt, owners[c->index])) idle = false;
                else c->native_irq_attached = false;
            }
        } else {
            if (idle) irq_set_handler(c->irq, 0); /* no callback outlives a released claim */
            else pic_mask(c->irq);  /* failed idle proof: quarantine containment */
        }
    }
    return idle;
}

bool ata_registry_idle(int handle, gen_t generation)
{
    bool matched=false, idle=true;
    for (unsigned i = 0; i < ATA_CHANNELS; i++) {
        struct ata_channel *c = &channels[i];
        if(!c->native) continue;
        bool owns=(c->dma.claim==handle && gen_matches(c->dma.generation,generation)) ||
            (c->addin_claim==handle && gen_matches(c->addin_generation,generation));
        for (unsigned j = 0; j < 3; j++)
            if(c->claims[j]==handle && gen_matches(c->generations[j],generation)) owns=true;
        if(owns) {
            matched=true;
            if(!(c->quiescing && c->mmio_phys && !c->mmio) && !ata_channel_idle(c)) idle=false;
        }
    }
    if(matched && idle) for(unsigned i=0;i<ATA_CHANNELS;i++) {
        struct ata_channel *c=&channels[i];
        if(c->native && c->mmio && c->addin_claim==handle && gen_matches(c->addin_generation,generation)) {
            volatile uint8_t *mapped=c->mmio;
            vmm_revoke_mmio((void *)mapped,0x4000);
            for(unsigned j=0;j<ATA_CHANNELS;j++) if(channels[j].mmio==mapped) channels[j].mmio=0;
        }
    }
    return matched && idle;
}

static int claim_channel(struct ata_channel *c, unsigned index)
{
    const enum res_type types[3] = { RES_PORT, RES_PORT, RES_IRQ };
    uint32_t starts[3] = { c->base, c->control, c->irq };
    uint32_t ends[3] = { c->base + 8u, c->control + 1u, c->irq + 1u };
    if (c->native_irq) { starts[1] -= 2; ends[1] = starts[1] + 4; }
    for (unsigned i = 0; i < 3; i++) {
        if(c->mmio_phys && i<2) {
            c->claims[i]=c->addin_claim;c->generations[i]=c->addin_generation;continue;
        }
        if(ata_addin_match(c->pci) && i<2) {
            if(c->claims[i]<0) return -FS_EINVAL;
            continue; /* Both channels were claimed before any clock mutation. */
        }
        int h = registry_claim(types[i], starts[i], ends[i], owners[index], i == 2 && c->native_irq);
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

static int ata_bind_resources(struct ata_channel *c, unsigned index)
{
    const struct pci_func *selected = 0;
    unsigned local=index;
    if(index>=2) {
        unsigned slot=2;
        for(unsigned j=0;j<pci_count();j++) {
            const struct pci_func *p=pci_get(j);
            if(!ata_addin_match(p)) continue;
            unsigned n=ata_addin_channels(p);
            if(index>=slot && index<slot+n) {
                if(slot+n>ATA_CHANNELS) return -FS_EOPNOTSUPP;
                selected=p;local=index-slot;break;
            }
            slot+=n;
        }
        if(!selected) return -FS_EOPNOTSUPP;
    }
    if(index<2)
    for (unsigned j = 0; j < pci_count(); j++) {
        const struct pci_func *p = pci_get(j);
        if(ata_addin_match(p)) continue;
        if (p->class_code != 1 || p->subclass != 1 || (p->header_type & 0x7f)) continue;
        if (selected) return -FS_EBUSY; /* never probe an ambiguous legacy alias */
        selected = p;
    }
    if (!selected) return 0; /* actual ISA compatibility controller */
    struct ata_resources r;
    bool native_irq = ata_addin_match(selected) || !!(selected->prog_if & (1u << (2 * local)));
    int e = ata_pci_resources(selected, local, native_irq && pci_irq_routed(selected), &r);
    if (e) return e;
    c->pci = selected; c->native_irq = r.native_irq;
    c->base = r.base; c->control = r.control; c->irq = r.irq;
    c->mmio_phys=r.mmio;
    c->addin_base=(uint16_t)(r.bm-8*local);
    c->chipset.channel=local;
    return 0;
}

static int addin_claim(struct ata_channel *c)
{
    if(!ata_addin_match(c->pci)) return 0;
    for(unsigned i=0;i<c->index;i++) if(channels[i].pci==c->pci && channels[i].addin_claim>=0) {
        struct ata_channel *peer=&channels[i];
        if(peer->stopped) return -FS_EQUARANTINED;
        c->addin_claim=peer->addin_claim;c->addin_generation=peer->addin_generation;
        for(unsigned k=0;k<2;k++) {c->claims[k]=peer->peer_claims[k];c->generations[k]=peer->peer_generations[k];}
        c->mmio=peer->mmio; return 0;
    }
    unsigned base=c->mmio_phys ? c->mmio_phys : c->addin_base;
    unsigned size=c->mmio_phys ? 0x4000 : c->pci->vendor==0x1103 ? 256 : 64;
    int h=registry_claim(c->mmio_phys ? RES_MMIO : RES_PORT,base,base+size,owners[c->index],false);
    if(h<0) return h;
    c->addin_claim=h;c->addin_generation=registry_get((unsigned)h)->generation;
    if(!c->mmio_phys) {
        int error=0;
        for(unsigned ch=0;ch<ata_addin_channels(c->pci) && !error;ch++) {
            struct ata_resources r;
            error=ata_addin_resources(c->pci,ch,true,&r);
            for(unsigned k=0;k<2 && !error;k++) {
                unsigned start=k ? r.control-2u : r.base, size=k ? 4 : 8;
                int handle=registry_claim(RES_PORT,start,start+size,owners[c->index+ch],false);
                if(handle<0) {error=handle;break;}
                gen_t gen=registry_get((unsigned)handle)->generation;
                if(ch) {c->peer_claims[k]=handle;c->peer_generations[k]=gen;}
                else {c->claims[k]=handle;c->generations[k]=gen;}
            }
        }
        if(error) {
            for(unsigned k=0;k<2;k++) {
                if(c->claims[k]>=0) registry_quarantine(c->claims[k],c->generations[k]);
                if(c->peer_claims[k]>=0) registry_quarantine(c->peer_claims[k],c->peer_generations[k]);
            }
            registry_quarantine(h,c->addin_generation);return error;
        }
    }
    if(c->mmio_phys) {
        c->mmio=vmm_map_mmio(c->mmio_phys,0x4000,true);
        if(!c->mmio) {registry_quarantine(h,c->addin_generation);return -FS_ENOMEM;}
    }
    return 0;
}
static int activate_once(int h, gen_t generation)
{
    const struct resource *r=h>=0 ? registry_get((unsigned)h) : 0;
    return r && r->generation==generation && r->state==RS_ACTIVE ? 0 : registry_activate(h,generation);
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
    unsigned slots=2;
    for(unsigned j=0;j<pci_count();j++) {
        const struct pci_func *p=pci_get(j);
        if(!ata_addin_match(p)) continue;
        slots+=ata_addin_channels(p);
        if(slots>ATA_CHANNELS) klog("[ata] pci=%04x:%04x bus=%u device=%u function=%u reason=channel_capacity limit=%u",
            p->vendor,p->device,p->bus,p->dev,p->fn,ATA_CHANNELS);
    }
    int found[ATA_CHANNELS] = {0};
    for (unsigned i = 0; i < ATA_CHANNELS; i++) {
        struct ata_channel *c = &channels[i];
        ata_channel_setup(c, i, &native_ops, 0);
        c->port_ctx=c;
        e = ata_bind_resources(c, i);
        if(!e) e=addin_claim(c);
        if (!e) e = claim_channel(c, i);
        if (e) {
            klog("[ata%u] mode=pio unavailable=1 reason=resources error=%d", i, e);
            c->stopped = true;
            continue;
        }
        unsigned local=c->chipset.channel;
        if (c->pci) ata_chipset_init(&c->chipset, c->pci, local, &ata_native_config, 0);
        if(ata_addin_match(c->pci)) {c->addin_ops=&ata_addin_native_ops;c->addin_ctx=c;}
        ata_dma_attach(c, i);
        c->native = true;
        if (c->native_irq && !c->dma.ops) {
            failure(&c->devices[0], now(c), 0);
            continue;
        }
        /* Suppress inherited device INTRQ before opening the registered
         * line once. Requests thereafter gate only ATA-6 nIEN (7.8.6). */
        if (!c->stopped) {
            device_control(c, c->control_shadow | ATA_NIEN);
            if (c->native_irq) {
                if (registry_activate(c->claims[2], c->generations[2]) ||
                    irq_chain_add(c->irq, ata_pci_interrupt, owners[i])) {
                    failure(&c->devices[0], now(c), 0);
                    continue;
                }
                c->native_irq_attached = true;
            } else {
                irq_set_handler(c->irq, i ? secondary_irq : primary_irq);
                pic_unmask(c->irq);
            }
        }
        found[i] = ata_channel_discover(c);
    }
    /* No request worker or block device is published until ALL boot-only
     * timing transactions are finished, including shared PCI fields. */
    for (unsigned i = 0; i < ATA_CHANNELS; i++) channels[i].modes_published = true;
    for (unsigned i = 0; i < ATA_CHANNELS; i++) {
        struct ata_channel *c = &channels[i];
        if (found[i] > 0 && !c->stopped) {
            if (blk_queue_start(&c->requests)) {
                klog("[storage] queue name=%s worker=unavailable",owners[i]);
                c->stopped=true;
                for (unsigned unit=0;unit<ATA_DEVICES;unit++) c->devices[unit].block.quarantined=true;
                continue;
            }
            for (unsigned j = 0; j < (c->native_irq ? 2u : 3u); j++)
                if (activate_once(c->claims[j], c->generations[j])) {
                    failure(&c->devices[0], now(c), alternate(c));
                    break;
                }
            if (!c->stopped && c->dma.claim >= 0 && activate_once(c->dma.claim, c->dma.generation))
                failure(&c->devices[0], now(c), alternate(c));
        }
        /* IDENTIFY completed before activation. IRQ source is off while
         * idle; each request enables it only after serialized selection. */
        klog("[ata%u] identified=%d quarantined=%u", i, found[i] > 0 ? found[i] : 0, c->stopped);
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
