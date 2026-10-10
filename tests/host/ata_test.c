/* Production ATA/partition views with scripted task-file, IRQ and clock.
 * The real sync/registry services execute against a fake scheduler/PIC.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ciuki/kernel.h>
#include <ciuki/ata.h>
#include <ciuki/blkpart.h>
#include <ciuki/registry.h>
#include <ciuki/task.h>
#include <ciuki/work.h>
#include <ciuki/storage.h>

static unsigned failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

#define CIUKI_CPU_H
static uint32_t flags = 0x200;
static unsigned schedules, yields, masks, unmasks, wakes;
static void (*on_schedule)(void);
static void (*on_yield)(void);
static bool masked[16];
static irq_handler_t handlers[16];
static struct task actor = { .id = 1, .state = T_RUNNING };
static struct task user_actor;
static _Alignas(4) uint8_t user_pages[3][4096];
static unsigned page_count;
static bool survivor_started;
static unsigned probe_ends, probe_passes, probe_errors;
static void *host_page(uint32_t phys) { CHECK(phys >= 1 && phys <= 3); return user_pages[phys - 1]; }
#define P2V(p) host_page(p)
struct task *g_current = &actor;
volatile uint64_t g_ticks;
volatile bool g_need_resched;
bool g_cpu_tsc;
uint64_t g_tsc_per_ms;
struct ciuki_boot_info g_boot;

static uint32_t read_eflags(void) { return flags; }
static uint32_t irq_save(void) { uint32_t f = flags; flags = 0; return f; }
static void irq_restore(uint32_t f) { flags = f; }
static uint8_t inb(uint16_t port) { (void)port; return 0xff; }
static void outb(uint16_t port, uint8_t v) { (void)port; (void)v; }
static void outl(uint16_t port, uint32_t v) { (void)port; (void)v; }
static uint32_t inl(uint16_t port) { (void)port; return UINT32_MAX; }
void klog(const char *fmt, ...) { (void)fmt; }
__attribute__((noreturn)) void panic(const char *fmt, ...)
{
    fprintf(stderr, "unexpected panic: %s\n", fmt);
    exit(2);
}
void task_start(struct task *t)
{
    CHECK(t->state == T_BLOCKED);
    t->state = T_READY;
    if (t == &user_actor) survivor_started = true;
    wakes++;
}
void schedule(void)
{
    CHECK(!flags && g_current->state == T_BLOCKED);
    schedules++;
    if (on_schedule)
        on_schedule();
    if (g_current->state == T_BLOCKED) {
        g_ticks = g_current->wake_tick;
        g_current->state = T_READY;
    }
    CHECK(g_current->state == T_READY);
    g_current->state = T_RUNNING;
}
void task_yield(void) { CHECK(flags & 0x200); yields++; if (on_yield) on_yield(); }
void task_sleep_ms(uint32_t ms)
{
    CHECK(flags & 0x200);
    g_ticks += ms ? ms : 1;
    if (survivor_started) {
        uint32_t *data = (uint32_t *)user_pages[0];
        data[0] += ms;
        data[2] = 0x5eed5eed;
    }
}
void pic_mask(unsigned irq) { CHECK(irq < 16); masked[irq] = true; masks++; }
void pic_unmask(unsigned irq) { CHECK(irq < 16); masked[irq] = false; unmasks++; }
void irq_set_handler(unsigned irq, irq_handler_t h) { CHECK(irq < 16); handlers[irq] = h; }
int kwork_init(void) { return 0; }
bool kwork_queue(kwork_fn fn, void *arg) { (void)fn; (void)arg; return false; }
void kwork_yield(void) { task_yield(); }
struct task *task_create_user(const char *name, enum task_prio prio, uint32_t ip, uint32_t sp,
                              uint32_t a, uint32_t b, uint32_t c)
{
    CHECK(!strcmp(name, "ata-survivor") && prio == P_NORMAL && ip == 0x00401000 && sp == 0xbffffff0);
    CHECK(a == 7 && b == 0x5eed5eed && !c);
    user_actor = (struct task){ .id = 2, .state = T_BLOCKED, .user = true };
    page_count = 0;
    return &user_actor;
}
uint32_t pmm_alloc(void) { CHECK(page_count < 3); return ++page_count; }
void pmm_free(uint32_t p) { CHECK(p >= 1 && p <= 3); }
int as_map(struct aspace *as, uint32_t va, uint32_t phys, uint32_t perms)
{
    (void)as; (void)va; (void)perms;
    CHECK(phys >= 1 && phys <= 3);
    return 0;
}
void task_kill(struct task *t, int code) { CHECK(t == &user_actor); t->state = T_ZOMBIE; t->exit_code = code; }
void task_reap(struct task *t) { CHECK(t == &user_actor && t->state == T_ZOMBIE); survivor_started = false; }
bool task_alive(const struct task *t) { return t->state != T_ZOMBIE && t->state != T_DEAD; }
const uint8_t payload_start[1] = { 0 };
__asm__(".global payload_end\n.set payload_end,payload_start+1");
void rec_emit(const char *name, const char *event, const char *fmt, ...)
{
    char extra[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(extra, sizeof(extra), fmt ? fmt : "", ap);
    va_end(ap);
    size_t length = strlen("CIUKI_TEST v=1 run=12345678 seq=000001 probe= event= ") +
                    strlen(name) + strlen(event) + strlen(extra);
    CHECK(length <= 240);
    if ((!strcmp(name, "ata-fault") && strstr(extra, "accepted=0")) || strstr(extra, "matched=0"))
        probe_errors++;
    if (!strcmp(event, "END")) {
        probe_ends++;
        if (!strcmp(extra, "status=PASS")) probe_passes++;
        else printf("probe %s %s\n", name, extra);
    }
}

#include "../../src/kernel/core/sync.c"
#include "../../src/kernel/lib/stackprot.c"
#include "../../src/kernel/core/registry.c"
#include "../../src/kernel/drivers/ata.c"
#include "../../src/kernel/drivers/blkpart.c"
#include "../../src/kernel/drivers/ata_probe.c"

enum fault { GOOD, ERR, DF, BUSY, NO_DRQ, EXTRA_DRQ, NO_IRQ, BAD_IDENTIFY, BAD_FLUSH, SLOW_FLUSH };
enum kind { ABSENT, DISK, PACKET, NO_SIGNATURE };
struct fake {
    struct ata_channel channel;
    enum kind kind[2];
    enum fault fault;
    uint16_t identify[2][256];
    uint8_t registers[6][2];
    uint8_t selected, status, command, control, head;
    unsigned writes, commands, data_reads, data_writes, words, blocks, irq_acks, delivered, errors;
    unsigned completed_blocks, fail_after;
    uint64_t lba, issued_tick, delayed_irq;
    bool pending, immediate, writing, stalled_clock, fail_delay;
    uint8_t statuses[8];
    unsigned status_count, status_index;
};
static struct fake *running;

static uint16_t sector_word(uint64_t lba, unsigned word)
{
    return (uint16_t)((lba * 257 + word * 17) ^ 0xa53c);
}
static void deliver(struct fake *f)
{
    if (!f->pending || (f->control & 2))
        return;
    f->delivered++;
    uint32_t saved = flags;
    flags = 0;
    ata_channel_irq(&f->channel);
    flags = saved;
}
static void interrupt(struct fake *f)
{
    if (f->fault == NO_IRQ)
        return;
    f->pending = true;
    if (f->immediate)
        deliver(f);
}
static uint8_t fake_in8(void *ctx, uint16_t port)
{
    struct fake *f = ctx;
    unsigned r = port - f->channel.base;
    if (port == f->channel.control || r == 7) {
        if (r == 7 && f->pending) { f->pending = false; f->irq_acks++; }
        if (f->kind[f->selected] == ABSENT)
            return 0;
        if (f->status_index < f->status_count)
            return f->statuses[f->status_index++];
        return f->status;
    }
    if (r == 1) { f->errors++; return f->command ? 4 : 1; }
    if (r == 2 || r == 3)
        return f->command ? f->registers[r][0] : (f->kind[f->selected] == NO_SIGNATURE ? 0 : 1);
    if (r == 4 || r == 5)
        return f->command ? f->registers[r][0] : (f->kind[f->selected] == PACKET ? (r == 4 ? 0x14 : 0xeb) : 0);
    return 0;
}
static void fake_out8(void *ctx, uint16_t port, uint8_t value)
{
    struct fake *f = ctx;
    f->writes++;
    unsigned r = port - f->channel.base;
    if (port == f->channel.control) {
        f->control = value;
        if (value & 4) { f->command = 0; f->status = 0x40; f->pending = false; }
        return;
    }
    if (r == 6) {
        f->selected = (value >> 4) & 1;
        f->head = value;
        f->command = 0;             /* reset signature is preserved per device in this model */
        f->status = 0x40;
        return;
    }
    if (r < 6) {
        f->registers[r][1] = f->registers[r][0];
        f->registers[r][0] = value;
        return;
    }
    CHECK(r == 7);
    f->command = value;
    f->commands++;
    f->issued_tick = g_ticks;
    f->words = 0;
    f->completed_blocks = 0;
    f->writing = value == 0x30 || value == 0x34;
    bool ext = value == 0x24 || value == 0x34;
    f->lba = f->registers[3][0] | (uint64_t)f->registers[4][0] << 8 | (uint64_t)f->registers[5][0] << 16;
    if (ext)
        f->lba |= (uint64_t)f->registers[3][1] << 24 | (uint64_t)f->registers[4][1] << 32 |
                  (uint64_t)f->registers[5][1] << 40;
    else
        f->lba |= (uint64_t)(f->head & 15) << 24;
    f->blocks = value == 0xec ? 1 : f->registers[2][0] + (ext ? (unsigned)f->registers[2][1] << 8 : 0);
    if (!f->blocks)
        f->blocks = 256;
    bool flush = value == 0xe7 || value == 0xea;
    f->status = flush ? 0x40 : 0x48;
    if (f->fault == ERR || (f->fault == BAD_IDENTIFY && value == 0xec) || (f->fault == BAD_FLUSH && flush))
        f->status = 0x41;
    if (f->fault == DF)
        f->status = 0x60;
    if (f->fault == BUSY)
        f->status = 0x89;           /* DRQ/ERR ignored while BSY */
    if (f->fault == NO_DRQ)
        f->status = 0x40;
    if (f->fault == SLOW_FLUSH && flush) {
        f->status = 0x80;
        f->delayed_irq = g_ticks + 45000;
        return;
    }
    if (f->fault != NO_IRQ && (!f->writing || f->fault == ERR || f->fault == DF))
        interrupt(f);
}
static void block_end(struct fake *f)
{
    f->words = 0;
    CHECK(f->blocks > 0);
    f->completed_blocks++;
    if (--f->blocks) {
        f->lba++;
        if (f->fail_after && f->completed_blocks == f->fail_after)
            f->status = 0x41;
        interrupt(f);
    } else {
        f->status = f->fault == EXTRA_DRQ ? 0x48 : 0x40;
        if (f->writing)
            interrupt(f);
    }
}
static uint16_t fake_in16(void *ctx, uint16_t port)
{
    struct fake *f = ctx;
    CHECK(port == f->channel.base && f->status == 0x48 && !f->writing);
    uint16_t v = f->command == 0xec ? f->identify[f->selected][f->words] : sector_word(f->lba, f->words);
    f->data_reads++;
    if (++f->words == 256)
        block_end(f);
    return v;
}
static void fake_out16(void *ctx, uint16_t port, uint16_t value)
{
    struct fake *f = ctx;
    CHECK(port == f->channel.base && f->status == 0x48 && f->writing);
    CHECK(value == sector_word(f->lba, f->words));
    f->data_writes++;
    if (++f->words == 256)
        block_end(f);
}
static uint64_t fake_now(void *ctx) { struct fake *f = ctx; return f->stalled_clock ? 0 : g_ticks; }
static uint64_t fake_deadline(void *ctx, uint64_t d) { (void)ctx; return d; }
static int fake_delay(void *ctx, uint32_t us)
{
    struct fake *f = ctx;
    CHECK(us == 1 || us == 5);
    return f->fail_delay ? -EFAULT : 0;
}
static const struct ata_port_ops fake_ops = {
    fake_in8, fake_out8, fake_in16, fake_out16, fake_now, fake_deadline, fake_delay,
};
static void irq_schedule(void)
{
    struct fake *f = running;
    if (f->delayed_irq) {
        g_ticks = f->delayed_irq;
        f->delayed_irq = 0;
        f->status = 0x40;
        f->pending = true;
    }
    deliver(f);
}
static void quiesce_schedule(void)
{
    CHECK(!ata_channel_idle(&running->channel));
    CHECK(running->channel.quiescing && (running->control & 2));
}
static void stale_schedule(void)
{
    struct ata_channel *c = &running->channel;
    c->irq_generation = c->request_generation - 1;
    c->irq_sequence++;
    kwait_wake_all(&c->wait);
    on_schedule = 0;                /* next scheduler step expires the unmatched wait */
}
static void fresh(struct fake *f, bool ext)
{
    memset(f, 0, sizeof(*f));
    f->kind[0] = DISK;
    f->status = 0x40;
    for (unsigned u = 0; u < 2; u++) {
        uint16_t *id = f->identify[u];
        id[49] = 1u << 9;
        id[60] = 0;
        id[61] = 16;
        id[82] = 1u << 5;
        id[83] = 0x5000 | (ext ? 0x2400 : 0);
        id[85] = 1u << 5;
        id[87] = 0x4000;
        id[100] = 0x1234;
        id[101] = 0x5678;
        id[102] = 0x123;
        for (unsigned i = 27; i < 47; i++) id[i] = 0x2020;
        id[27] = 0x4369; id[28] = 0x756b; id[29] = 0x6920; id[30] = 0x010a;
    }
    CHECK(ata_channel_setup(&f->channel, 0, &fake_ops, f) == 0);
    running = f;
    on_schedule = irq_schedule;
    on_yield = 0;
    flags = 0x200;
    g_current = &actor;
    actor.state = T_RUNNING;
}
static struct blkdev *qualify(struct fake *f, bool ext)
{
    fresh(f, ext);
    CHECK(ata_channel_discover(&f->channel) == 1);
    return &f->channel.devices[0].block;
}

static void test_identify(void)
{
    struct fake f;
    struct blkdev *b = qualify(&f, false);
    CHECK(b->capacity == 1048576 && b->write_cache_state == BLKDEV_CACHE_ENABLED && b->flush);
    CHECK(!f.channel.devices[0].lba48 && !strcmp(f.channel.devices[0].model, "Ciuki"));
    CHECK(f.delivered == 1 && f.irq_acks == 1 && f.data_reads == 256);
    b = qualify(&f, true);
    CHECK(b->capacity == 0x12356781234ull && f.channel.devices[0].lba48 && f.channel.devices[0].flush_ext);
    CHECK(b->flush(b) == 0 && f.command == 0xea);
    fresh(&f, false);
    f.identify[0][85] = 0;
    CHECK(ata_channel_discover(&f.channel) == 1);
    CHECK(f.channel.devices[0].block.write_cache_state == BLKDEV_CACHE_DISABLED);
    fresh(&f, false);
    f.identify[0][82] = 0;
    f.identify[0][85] = 0;
    CHECK(ata_channel_discover(&f.channel) == 1);
    CHECK(f.channel.devices[0].block.write_cache_state == BLKDEV_CACHE_DISABLED);
    fresh(&f, false);
    f.identify[0][87] = 0;
    f.identify[0][83] &= ~0x1000;
    CHECK(ata_channel_discover(&f.channel) == 1);
    CHECK(f.channel.devices[0].block.write_cache_state == BLKDEV_CACHE_UNKNOWN && !f.channel.devices[0].block.flush);
    fresh(&f, false);
    f.identify[0][83] = 0;
    CHECK(ata_channel_discover(&f.channel) == 1);
    CHECK(f.channel.devices[0].block.write_cache_state == BLKDEV_CACHE_UNKNOWN);
    fresh(&f, false);
    f.kind[1] = PACKET;
    CHECK(ata_channel_discover(&f.channel) == 1 && f.channel.devices[1].atapi && f.commands == 1);
    fresh(&f, false);
    f.kind[0] = ABSENT; f.kind[1] = DISK;
    CHECK(ata_channel_discover(&f.channel) == 1 && f.channel.devices[1].identified);
    fresh(&f, false);
    f.kind[1] = DISK;
    CHECK(ata_channel_discover(&f.channel) == 2 && f.commands == 2);
    for (unsigned i = 0; i < 3; i++) {
        fresh(&f, true);
        if (!i) f.identify[0][49] = 0;
        if (i == 1) f.identify[0][103] = 1;
        if (i == 2) { f.identify[0][106] = 0x5000; f.identify[0][117] = 2048; }
        CHECK(ata_channel_discover(&f.channel) == -FS_EIO && f.channel.stopped);
    }
    printf("ata detection/IDENTIFY (LBA28/48, cache, ATAPI, master/slave, format rejection): PASS\n");
}

static void fill(uint8_t *buf, uint64_t lba, unsigned n)
{
    for (unsigned s = 0; s < n; s++)
        for (unsigned w = 0; w < 256; w++)
            fs_wr16(buf + s * 512 + w * 2, sector_word(lba + s, w));
}
static void test_data(void)
{
    struct fake f;
    uint8_t *buf = malloc(513 * 512 + 1), *expected = malloc(513 * 512);
    CHECK(buf && expected);
    static const unsigned counts[] = { 1, 2, 256, 513 };
    for (unsigned ext = 0; ext < 2; ext++) {
        for (unsigned i = 0; i < 4; i++) {
            struct blkdev *b = qualify(&f, ext != 0);
            uint64_t lba = ext ? 0x123456789ull : 0x81234;
            unsigned n = counts[i], commands = f.commands, irqs = f.delivered;
            fill(expected, lba, n);
            memset(buf, 0, n * 512 + 1);
            CHECK(b->read(b, lba, n, buf + 1) == 0);
            CHECK(!memcmp(buf + 1, expected, n * 512) && buf[0] == 0);
            CHECK(f.commands - commands == (n + 255) / 256 && f.delivered - irqs == n);
            CHECK(!f.channel.active && !f.channel.mutex.owner);
            CHECK(f.command == (ext ? 0x24 : 0x20));
            commands = f.commands; irqs = f.delivered;
            CHECK(b->write(b, lba, n, buf + 1) == 0);
            CHECK(f.commands - commands == (n + 255) / 256 && f.delivered - irqs == n);
            CHECK(f.command == (ext ? 0x34 : 0x30) && f.data_writes == n * 256);
            CHECK(b->flush(b) == 0 && f.command == (ext ? 0xea : 0xe7));
            CHECK(f.irq_acks == f.delivered && (f.control & 2));
        }
    }
    struct blkdev *b = qualify(&f, false);
    unsigned commands = f.commands, writes = f.writes;
    CHECK(b->read(b, 0, 0, buf) == -FS_EINVAL);
    CHECK(b->read(b, b->capacity, 1, buf) == -FS_EINVAL);
    CHECK(b->read(b, b->capacity - 1, 2, buf) == -FS_EINVAL);
    CHECK(b->read(b, UINT64_MAX, UINT32_MAX, buf) == -FS_EINVAL);
    CHECK(b->read(b, 0, 1, 0) == -FS_EINVAL);
    CHECK(f.commands == commands && f.writes == writes);
    CHECK(b->read(b, b->capacity - 1, 1, buf) == 0);
    fresh(&f, false);
    f.identify[0][61] = 0x1000; /* maximum LBA28 capacity, exercise Device nibble */
    CHECK(ata_channel_discover(&f.channel) == 1);
    b = &f.channel.devices[0].block;
    fill(expected, 0x0ffffffe, 2);
    CHECK(b->read(b, 0x0ffffffe, 2, buf) == 0 && !memcmp(buf, expected, 1024));
    CHECK(b->write(b, 0x0ffffffe, 2, expected) == 0);
    b = qualify(&f, false);
    f.immediate = true;
    CHECK(b->read(b, 0, 2, buf) == 0); /* IRQ before wait enqueue */
    CHECK(b->write(b, 0, 2, buf) == 0);
    free(buf); free(expected);
    printf("ata PIO (1/2/256 sectors, split 513, IRQ order, unaligned data, bounds): PASS\n");
}

static void test_faults(void)
{
    struct fake f;
    uint8_t buf[512];
    static const enum fault faults[] = { ERR, DF, BUSY, NO_DRQ, EXTRA_DRQ, NO_IRQ, BAD_FLUSH };
    for (unsigned i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
        struct blkdev *b = qualify(&f, false);
        f.fault = faults[i];
        uint64_t start = g_ticks;
        int e = f.fault == BAD_FLUSH ? b->flush(b) : b->read(b, 0, 1, buf);
        CHECK(e == -FS_EIO && b->quarantined && f.channel.stopped);
        CHECK(f.channel.devices[1].block.quarantined);
        CHECK(g_ticks - start <= (f.fault == BAD_FLUSH ? ATA_FLUSH_MS : ATA_COMMAND_MS));
        CHECK(!f.channel.active && !f.channel.mutex.owner && (f.control & 2));
        unsigned n = f.commands, w = f.writes;
        CHECK(b->read(b, 0, 1, buf) == -FS_EQUARANTINED);
        CHECK(b->write(b, 0, 1, buf) == -FS_EQUARANTINED);
        CHECK(b->flush(b) == -FS_EQUARANTINED);
        CHECK(ata_channel_discover(&f.channel) == -FS_EQUARANTINED);
        CHECK(f.commands == n && f.writes == w);
        CHECK(!ata_channel_idle(&f.channel));
    }
    fresh(&f, false);
    f.kind[0] = NO_SIGNATURE;
    CHECK(ata_channel_discover(&f.channel) == 0 && !f.commands);
    fresh(&f, false);
    f.fault = BAD_IDENTIFY;
    CHECK(ata_channel_discover(&f.channel) == -FS_EIO && f.channel.devices[0].block.quarantined);
    CHECK(f.commands == 1 && ata_channel_discover(&f.channel) == -FS_EQUARANTINED && f.commands == 1);
    struct blkdev *b = qualify(&f, false);
    f.fault = SLOW_FLUSH;
    uint64_t start = g_ticks;
    CHECK(b->flush(b) == 0 && g_ticks - start == 45000 && !b->quarantined);
    b = qualify(&f, false);
    /* IRQ with BSY+ERR+DRQ must not treat the invalid low bits as error.
     * The following Alternate Status transition produces a valid DRQ. */
    f.statuses[0] = 0x40; f.statuses[1] = 0x40; f.statuses[2] = 0x40;
    f.statuses[3] = 0x89; f.statuses[4] = 0x48; f.status_count = 5;
    CHECK(b->read(b, 0, 1, buf) == 0 && !b->quarantined);
    b = qualify(&f, false);
    f.fail_delay = true;
    CHECK(b->read(b, 0, 1, buf) == -FS_EIO && b->quarantined);
    b = qualify(&f, false);
    f.fault = SLOW_FLUSH;
    f.delayed_irq = 0;
    /* The same late-completion path is checked against an expired data
     * deadline directly; late IRQs must not turn an expired request into PASS. */
    uint64_t deadline = g_ticks + 3;
    f.channel.active = &f.channel.devices[0];
    f.channel.request_generation = gen_alloc();
    uint32_t sequence = f.channel.irq_sequence;
    device_control(&f.channel, 0);
    f.delayed_irq = g_ticks + 4;
    CHECK(wait_irq(&f.channel.devices[0], &sequence, g_ticks, deadline) == -FS_EIO);
    disarm(&f.channel);
    b = qualify(&f, false);
    f.fault = NO_IRQ;
    fill(buf, 0, 1);
    CHECK(b->write(b, 0, 1, buf) == -FS_EIO && f.data_writes == 256 && b->quarantined);
    for (unsigned write = 0; write < 2; write++) {
        uint8_t pair[1024];
        b = qualify(&f, false);
        f.fail_after = 1;
        fill(pair, 0, 2);
        CHECK((write ? b->write(b, 0, 2, pair) : b->read(b, 0, 2, pair)) == -FS_EIO);
        CHECK((write ? f.data_writes : f.data_reads - 256) == 256 && b->quarantined);
    }
    b = qualify(&f, false);
    f.fault = NO_IRQ;
    on_schedule = quiesce_schedule;
    CHECK(b->read(b, 0, 1, buf) == -FS_EIO && b->quarantined && f.commands == 2);
    b = qualify(&f, false);
    f.fault = NO_IRQ;
    on_schedule = stale_schedule;
    CHECK(b->read(b, 0, 1, buf) == -FS_EIO && f.data_reads == 256);
    printf("ata failures/deadlines/quarantine (ERR, DF, BSY/DRQ, IRQ, IDENTIFY, FLUSH): PASS\n");
}

static void test_views(void)
{
    struct fake f;
    struct blkdev *b = qualify(&f, false);
    struct partition p = { .start = 2048, .count = 100, .type = 0x0c };
    struct blkpart v;
    CHECK(blkpart_init(&v, b, &p) == 0);
    struct blkdev *d = blkpart_device(&v);
    uint8_t buf[512], expected[512];
    fill(expected, 2051, 1);
    CHECK(d->read(d, 3, 1, buf) == 0 && !memcmp(buf, expected, 512));
    CHECK(d->write(d, 3, 1, expected) == 0 && d->flush(d) == 0);
    unsigned writes = f.writes;
    CHECK(d->read(d, 100, 1, buf) == -FS_EINVAL && d->read(d, 99, 2, buf) == -FS_EINVAL);
    CHECK(d->read(d, UINT64_MAX, 1, buf) == -FS_EINVAL && f.writes == writes);
    f.fault = ERR;
    CHECK(d->read(d, 0, 1, buf) == -FS_EIO && d->quarantined && b->quarantined);
    CHECK(d->read(d, 0, 1, buf) == -FS_EQUARANTINED);
    b = qualify(&f, false);
    CHECK(blkpart_init(&v, b, &p) == 0);
    b->quarantined = true;
    CHECK(blkpart_device(&v)->quarantined && v.block.flush(&v.block) == -FS_EQUARANTINED);
    p.start = UINT64_MAX; p.count = 2;
    CHECK(blkpart_init(&v, b, &p) == -FS_EINVAL);
    b = qualify(&f, false);
    CHECK(ata_channel_idle(&f.channel) && f.channel.quiescing && (f.control & 2));
    writes = f.writes;
    CHECK(b->read(b, 0, 1, buf) == -FS_EQUARANTINED && b->flush(b) == -FS_EQUARANTINED);
    CHECK(f.writes == writes);
    p.start = 2048; p.count = 0;
    CHECK(blkpart_init(&v, b, &p) == -FS_EINVAL);
    printf("blkpart offset/bounds/cache/flush/quarantine forwarding: PASS\n");
}

static void test_claims(void)
{
    struct fake f;
    fresh(&f, false);
    registry_init();
    CHECK(claim_channel(&f.channel, 0) == 0);
    for (unsigned i = 0; i < 3; i++) {
        const struct resource *r = registry_get((unsigned)f.channel.claims[i]);
        CHECK(r && r->state == RS_CLAIMED && !strcmp(r->owner, "ata0") && r->generation);
    }
    f.channel.native = true;
    CHECK(ata_channel_discover(&f.channel) == 1);
    for (unsigned i = 0; i < 3; i++) CHECK(registry_activate(f.channel.claims[i], f.channel.generations[i]) == 0);
    f.fault = BUSY;
    uint8_t buf[512];
    CHECK(f.channel.devices[0].block.read(&f.channel.devices[0].block, 0, 1, buf) == -FS_EIO);
    CHECK(masked[14] && masks && unmasks);
    for (unsigned i = 0; i < 3; i++)
        CHECK(registry_get((unsigned)f.channel.claims[i])->state == RS_QUARANTINED);
    fresh(&f, false);
    unsigned writes = f.writes;
    CHECK(claim_channel(&f.channel, 0) < 0 && f.writes == writes);
    printf("ata registry claims/generations/conflict/no-write/controller quarantine: PASS\n");
}

static void test_probe_fixtures(void)
{
    /* Exercise the production fake-time fault probe and its records on the
     * host. The survivor scheduler is simulated here; ring-3 execution is
     * deliberately left to the lead's QEMU evidence. */
    CHECK(probe_ata_fault() == 0 && probe_ends == 1 && probe_passes == 1 && !probe_errors);
    for (unsigned i = 0; i < 5; i++) {
        fixture_init();
        if (i == 1) entry(fixture.sectors[2], 1, 0x0f, 200, 1000);
        if (i == 2) entry(fixture.sectors[0], 0, 0x0c, UINT32_MAX - 100, 200);
        if (i == 3) entry(fixture.sectors[0], 2, 0x0c, 150, 200);
        if (i == 4) entry(fixture.sectors[0], 0, 0xee, 1, 9999);
        const int expected[] = { 0, -FS_ELOOP, -FS_EINVAL, -FS_EINVAL, -FS_EOPNOTSUPP };
        CHECK(partition_scan(&fixture.block, &fixture_table) == expected[i]);
        CHECK(!fixture.outside && fixture_table.ebr_reads <= 2);
        CHECK(fixture_table.count == (i ? 0u : 3u));
    }
    /* Present a canonical-LBA MBR through a memory block device, then run
     * all seven production partition fixtures and validate record lengths. */
    fixture_init();
    memset(fixture.sectors[0] + 446, 0, 64);
    entry(fixture.sectors[0], 0, 0x0c, 2048, 1000);
    memset(channels, 0, sizeof(channels));
    channels[0].irq = 14;
    channels[0].devices[0].identified = true;
    channels[0].devices[0].channel = &channels[0];
    channels[0].devices[0].block = fixture.block;
    initialized = true;
    CHECK(probe_partition() == 0 && probe_ends == 2 && probe_passes == 2 && !probe_errors);
    channels[0].native = true;
    CHECK(ata_channel_setup(&channels[0], 0, &fake_ops, 0) == -FS_EQUARANTINED);
    initialized = false;
    printf("ata-fault production probe/time boundary/record bounds; partition fixtures: PASS\n");
}

static void cache_inject(void *ctx, bool flush) { ((struct fake *)ctx)->fault = flush ? BAD_FLUSH : ERR; }
static void test_storage_cache_faults(void)
{
    struct block_cache c;
    CHECK(!cache_init(&c,8192));
    for (unsigned i=0;i<2;i++) {
        struct fake f; struct blkdev *d=qualify(&f,false);
        CHECK(!storage_cache_fault(&c,d,cache_inject,&f,!!i));
        unsigned before=f.commands; uint8_t bytes[512];
        CHECK(d->quarantined && d->read(d,0,1,bytes)==-FS_EQUARANTINED && before==f.commands);
    }
    cache_destroy(&c);
    puts("storage cache/ATA: PASS (register-boundary write/flush faults, delayed EIO, sticky unmount, quarantine/no further command)");
}
int main(void)
{
    test_identify(); test_data(); test_faults(); test_views(); test_claims(); test_probe_fixtures();
    test_storage_cache_faults();
    printf("ata/blkpart host tests: %s (waits=%u wakes=%u yields=%u)\n",
           failures ? "FAIL" : "PASS", schedules, wakes, yields);
    return failures ? 1 : 0;
}
