/* F1 disk evidence. Synthetic fault channels never replace a real channel
 * or acquire real ports/IRQs; rearming models separate fake-device boots.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/task.h>
#include <ciuki/probe.h>
#include <ciuki/ata.h>
#include <ciuki/blkpart.h>
#include <ciuki/sha256.h>

static int verdict(const char *name, bool ok, const char *reason)
{
    rec_emit(name, "END", ok ? "status=PASS" : "status=FAIL reason=%s", reason);
    return ok ? 0 : 1;
}

static struct ata_device *disk(void)
{
    for (unsigned c = 0; c < ATA_CHANNELS; c++)
        for (unsigned u = 0; u < ATA_DEVICES; u++) {
            struct ata_device *d = ata_device_get(c, u);
            if (d)
                return d;
        }
    return 0;
}

int probe_ata(void)
{
    rec_emit("ata", "BEGIN", 0);
    struct ata_device *d = disk();
    if (!d)
        return verdict("ata", false, "no_identified_disk");
    rec_emit("ata", "DATA", "owner=ata%u generation=%u unit=%u model=%s identified=%u",
             d->channel->irq - 14, d->channel->generations[2], d->unit, d->model, d->identified);
    rec_emit("ata", "DATA", "capacity=%llu sector_size=%u lba48=%u cache_state=%u flush=%u bios_calls=0 clock=pit",
             d->block.capacity, d->block.sector_size, d->lba48, d->block.write_cache_state, !!d->block.flush);
    static const uint64_t lbas[] = { 0, 2048 };
    uint8_t data[512], hash[32];
    char hex[65];
    bool ok = true;
    for (unsigned i = 0; i < ARRAY_SIZE(lbas); i++) {
        int e = d->block.read(&d->block, lbas[i], 1, data);
        if (e) {
            rec_emit("ata", "DATA", "lba=%llu result=%d status=%02x error=%02x elapsed_ms=%llu",
                     lbas[i], e, d->status, d->error, d->elapsed_ms);
            ok = false;
            break;
        }
        sha256(data, sizeof(data), hash);
        sha256_hex(hash, hex);
        rec_emit("ata", "DATA", "lba=%llu sha256=%s", lbas[i], hex);
    }
    uint64_t before = ata_command_count();
    int range = d->block.read(&d->block, d->block.capacity, 1, data);
    int zero = d->block.read(&d->block, 0, 0, data);
    int overflow = d->block.read(&d->block, UINT64_MAX, UINT32_MAX, data);
    uint64_t after = ata_command_count();
    rec_emit("ata", "DATA", "range_result=%d zero_result=%d overflow_result=%d boundary_commands=%llu",
             range, zero, overflow, after - before);
    ok = ok && range == -FS_EINVAL && zero == -FS_EINVAL && overflow == -FS_EINVAL && after == before;
    /* Capacity is measured here; the runner compares it with its image,
     * rather than making physical hardware pretend to be the QEMU image. */
    return verdict("ata", ok, "ata_read_or_bounds");
}

enum fault_case { FAKE_ERR, FAKE_DF, FAKE_BSY, FAKE_DRQ, FAKE_MISSING, FAKE_IDENTIFY, FAKE_FLUSH };
struct fault_device {
    struct ata_channel channel;
    enum fault_case fault;
    uint64_t epoch;
    uint8_t status, command, control;
    unsigned selected, word, commands;
    bool armed;
};
static struct fault_device fault_device;

static uint8_t fault_in8(void *ctx, uint16_t port)
{
    struct fault_device *f = ctx;
    unsigned r = port - f->channel.base;
    if (port == f->channel.control || r == 7)
        return f->selected ? 0 : f->status;
    if (r == 1)
        return f->command ? 4 : 1;
    if (r == 2 || r == 3)
        return f->fault == FAKE_MISSING ? 0 : 1;
    return 0;
}
static void fault_out8(void *ctx, uint16_t port, uint8_t value)
{
    struct fault_device *f = ctx;
    unsigned r = port - f->channel.base;
    if (port == f->channel.control) {
        f->control = value;
        if (value & 4) { f->status = 0x40; f->command = 0; }
        return;
    }
    if (r == 6) {
        f->selected = value >> 4 & 1;
        f->status = 0x40;
        return;
    }
    if (r != 7)
        return;
    f->commands++;
    f->command = value;
    f->word = 0;
    f->status = value == 0xe7 || value == 0xea ? 0x40 : 0x48;
    bool inject = f->armed || f->fault == FAKE_IDENTIFY;
    if (inject) {
        if (f->fault == FAKE_ERR || f->fault == FAKE_IDENTIFY || f->fault == FAKE_FLUSH)
            f->status = 0x41;
        else if (f->fault == FAKE_DF)
            f->status = 0x60;
        else if (f->fault == FAKE_BSY)
            f->status = 0x80;
        else if (f->fault == FAKE_DRQ)
            f->status = 0x40;       /* never reaches required DRQ */
    }
    /* Latch even an IRQ that arrives before the request enqueues a wait.
     * The production handler acknowledges through this same boundary. */
    if (!(f->control & 2))
        ata_channel_irq(&f->channel);
}
static uint16_t fault_in16(void *ctx, uint16_t port)
{
    (void)port;
    struct fault_device *f = ctx;
    unsigned w = f->word++;
    uint16_t v = 0;
    if (f->command == 0xec) {
        if (w == 49) v = 1u << 9;
        if (w == 61) v = 16;
        if (w == 82 || w == 85) v = 1u << 5;
        if (w == 83) v = 0x5000;
        if (w == 87) v = 0x4000;
        if (w >= 27 && w < 47) v = 0x4141;
    }
    if (f->word == 256)
        f->status = 0x40;
    return v;
}
static void fault_out16(void *ctx, uint16_t port, uint16_t value)
{
    (void)ctx; (void)port; (void)value; /* no writes used in the fault probe */
}
static uint64_t fault_now(void *ctx)
{
    struct fault_device *f = ctx;
    return (deadline_after_ms(0) - f->epoch) * 1000;
}
static uint64_t fault_deadline(void *ctx, uint64_t d)
{
    uint64_t t = fault_now(ctx), remaining = d - t;
    uint32_t ms = remaining < (1ull << 63) ? (uint32_t)((remaining + 999) / 1000) : 0;
    return deadline_after_ms(ms);
}
static int fault_delay(void *ctx, uint32_t us) { (void)ctx; (void)us; return 0; }
static const struct ata_port_ops fault_ops = {
    fault_in8, fault_out8, fault_in16, fault_out16, fault_now, fault_deadline, fault_delay,
};

/* Reuse the F0 ring-3 survivor payload (id=7), mapped identically to the
 * other F0 probes. Its counter and sentinel establish user progress, not
 * just PIT progress while the kernel task is asleep. */
extern const uint8_t payload_start[], payload_end[];
static struct task *survivor_create(uint32_t *data_phys)
{
    struct task *t = task_create_user("ata-survivor", P_NORMAL, 0x00401000, 0xbffffff0, 7, 0x5eed5eed, 0);
    if (!t)
        return 0;
    uint32_t pages[3] = { pmm_alloc(), pmm_alloc(), pmm_alloc() };
    if (!pages[0] || !pages[1] || !pages[2] || (size_t)(payload_end - payload_start) > PAGE_SIZE) {
        for (unsigned i = 0; i < 3; i++) if (pages[i]) pmm_free(pages[i]);
        task_kill(t, -1); task_reap(t);
        return 0;
    }
    for (unsigned i = 0; i < 3; i++)
        memset(P2V(pages[i]), 0, PAGE_SIZE);
    memcpy(P2V(pages[1]), payload_start, (size_t)(payload_end - payload_start));
    const uint32_t vas[3] = { 0x00400000, 0x00401000, 0xbffff000 };
    const uint32_t permissions[3] = { PTE_U | PTE_W, PTE_U, PTE_U | PTE_W };
    for (unsigned i = 0; i < 3; i++) {
        if (as_map(&t->as, vas[i], pages[i], permissions[i])) {
            for (unsigned j = i; j < 3; j++) pmm_free(pages[j]);
            task_kill(t, -1); task_reap(t);
            return 0;
        }
    }
    *data_phys = pages[0];
    task_start(t);
    return t;
}

int probe_ata_fault(void)
{
    rec_emit("ata-fault", "BEGIN", 0);
    uint64_t real_before = ata_command_count();
    static const char *const names[] = { "err", "df", "bsy_stuck", "drq_stuck", "missing", "identify", "flush" };
    bool ok = true;
    uint8_t buf[512];
    for (unsigned i = 0; i < ARRAY_SIZE(names); i++) {
        struct fault_device *f = &fault_device;
        memset(f, 0, sizeof(*f));
        f->fault = (enum fault_case)i;
        f->epoch = deadline_after_ms(0);
        ata_channel_setup(&f->channel, 0, &fault_ops, f);
        int found = ata_channel_discover(&f->channel);
        struct ata_device *d = &f->channel.devices[0];
        int result = found;
        uint64_t start = fault_now(f);
        unsigned before = f->commands;
        if (found == 1) {
            f->armed = true;
            result = i == FAKE_FLUSH ? d->block.flush(&d->block) : d->block.read(&d->block, 0, 1, buf);
        }
        unsigned issued = f->commands - before;
        if (i == FAKE_IDENTIFY)
            issued = f->commands;
        uint64_t elapsed = i == FAKE_IDENTIFY ? d->elapsed_ms : fault_now(f) - start;
        unsigned commands = f->commands;
        int next = d->identified ? d->block.read(&d->block, 0, 1, buf) : ata_channel_discover(&f->channel);
        unsigned further = f->commands - commands;
        bool missing = i == FAKE_MISSING;
        uint32_t limit = i == FAKE_FLUSH ? ATA_FLUSH_MS : ATA_COMMAND_MS;
        bool pass = missing ? found == 0 && !commands && !further :
            result == -FS_EIO && d->block.quarantined && !further && next == -FS_EQUARANTINED && elapsed <= limit;
        rec_emit("ata-fault", "DATA", "case=%s owner=fake-ata0 generation=%u result=%d issued=%u status=%02x error=%02x quarantined=%u",
                 names[i], d->generation, result, issued, d->status, d->error, d->block.quarantined);
        rec_emit("ata-fault", "DATA", "case=%s elapsed_ms=%llu deadline_ms=%u next_result=%d further_commands=%u accepted=%u clock=scaled_pit",
                 names[i], elapsed, limit, next, further, pass);
        ok = ok && pass;
    }
    uint32_t data_phys = 0;
    struct task *survivor = survivor_create(&data_phys);
    uint32_t progress = 0, samples = 0;
    bool alive = false;
    if (survivor) {
        volatile uint32_t *data = P2V(data_phys);
        task_sleep_ms(10);
        uint32_t count = data[0];
        uint64_t start = deadline_after_ms(0);
        task_sleep_ms(150);
        samples = (uint32_t)(deadline_after_ms(0) - start);
        progress = data[0] - count;
        alive = task_alive(survivor) && data[1] == 0 && data[2] == 0x5eed5eed;
        task_kill(survivor, -1);
        task_reap(survivor);
    }
    uint64_t real_commands = ata_command_count() - real_before;
    rec_emit("ata-fault", "DATA", "real_commands=%llu bios_calls=0 survivor_samples=%u survivor_progress=%u survivor_alive=%u",
             real_commands, samples, progress, alive);
    ok = ok && !real_commands && samples >= 100 && progress && alive;
    return verdict("ata-fault", ok, "fault_containment");
}

struct partition_fixture {
    struct blkdev block;
    uint8_t sectors[3][512];
    unsigned reads, outside;
};
static struct partition_fixture fixture;
static struct partition_table fixture_table;

static int fixture_read(struct blkdev *d, uint64_t lba, uint32_t n, void *buf)
{
    struct partition_fixture *f = d->ctx;
    if (blkdev_range(d, lba, n) || n != 1) {
        f->outside++;
        return -FS_EIO;
    }
    unsigned sector = lba == 0 ? 0 : lba == 1000 ? 1 : lba == 1200 ? 2 : 3;
    f->reads++;
    if (sector == 3) { f->outside++; return -FS_EIO; }
    memcpy(buf, f->sectors[sector], 512);
    return 0;
}
static void entry(uint8_t *sector, unsigned i, uint8_t type, uint32_t start, uint32_t length)
{
    uint8_t *p = sector + 446 + i * 16;
    memset(p, 0, 16);
    p[4] = type;
    fs_wr32(p + 8, start);
    fs_wr32(p + 12, length);
}
static void fixture_init(void)
{
    memset(&fixture, 0, sizeof(fixture));
    fixture.block = (struct blkdev){ .read = fixture_read, .capacity = 10000,
        .sector_size = 512, .ctx = &fixture };
    for (unsigned i = 0; i < 3; i++)
        fs_wr16(fixture.sectors[i] + 510, 0xaa55);
    entry(fixture.sectors[0], 0, 0x0c, 100, 200);
    entry(fixture.sectors[0], 1, 0x0f, 1000, 2000);
    entry(fixture.sectors[1], 0, 0x0b, 1, 100);
    entry(fixture.sectors[1], 1, 0x0f, 200, 1000);
    entry(fixture.sectors[2], 0, 0x0c, 1, 100);
}

int probe_partition(void)
{
    rec_emit("partition", "BEGIN", 0);
    struct ata_device *d = disk();
    if (!d)
        return verdict("partition", false, "no_identified_disk");
    int e = partition_scan(&d->block, &fixture_table);
    bool ok = !e && fixture_table.count && fixture_table.entries[0].start == 2048 &&
        (fixture_table.entries[0].type == 0x0b || fixture_table.entries[0].type == 0x0c);
    rec_emit("partition", "DATA", "source=real owner=ata%u generation=%u result=%d count=%u walk_count=%u clock=pit",
             d->channel->irq - 14, d->channel->generations[2], e, fixture_table.count, fixture_table.ebr_reads);
    for (unsigned i = 0; i < fixture_table.count; i++) {
        const struct partition *p = &fixture_table.entries[i];
        rec_emit("partition", "DATA", "source=real partition=%u type=%02x start=%llu length=%llu logical=%u",
                 i + 1, p->type, p->start, p->count, p->logical);
    }
    static const char *const names[] = { "primary_extended", "loop", "overflow", "overlap", "protective_gpt", "signature", "out_of_range" };
    static const int expected[] = { 0, -FS_ELOOP, -FS_EINVAL, -FS_EINVAL, -FS_EOPNOTSUPP, -FS_EINVAL, -FS_EINVAL };
    for (unsigned i = 0; i < ARRAY_SIZE(names); i++) {
        fixture_init();
        if (i == 1) entry(fixture.sectors[2], 1, 0x0f, 200, 1000);
        if (i == 2) entry(fixture.sectors[0], 0, 0x0c, UINT32_MAX - 100, 200);
        if (i == 3) entry(fixture.sectors[0], 2, 0x0c, 150, 200);
        if (i == 4) entry(fixture.sectors[0], 0, 0xee, 1, 9999);
        if (i == 5) fs_wr16(fixture.sectors[0] + 510, 0);
        if (i == 6) entry(fixture.sectors[0], 0, 0x0c, 9900, 200);
        uint8_t hash[32]; char hex[65];
        sha256(fixture.sectors, sizeof(fixture.sectors), hash);
        sha256_hex(hash, hex);
        e = partition_scan(&fixture.block, &fixture_table);
        bool pass = e == expected[i] && !fixture.outside;
        if (!i)
            pass = pass && fixture_table.count == 3 && fixture_table.ebr_reads == 2 &&
                fixture_table.entries[0].start == 100 && fixture_table.entries[0].count == 200 &&
                fixture_table.entries[1].start == 1001 && fixture_table.entries[1].count == 100 &&
                fixture_table.entries[2].start == 1201 && fixture_table.entries[2].count == 100;
        else
            pass = pass && fixture_table.count == 0;
        rec_emit("partition", "DATA", "fixture=%s owner=fixture generation=0 sha256=%s", names[i], hex);
        rec_emit("partition", "DATA", "fixture=%s result=%d expected=%d walk_count=%u reads=%u outside=%u accepted=%u matched=%u",
                 names[i], e, expected[i], fixture_table.ebr_reads, fixture.reads, fixture.outside, !e, pass);
        ok = ok && pass;
    }
    return verdict("partition", ok, "partition_validation");
}

#ifdef CIUKI_F1_PROBE
CIUKI_F1_PROBE("ata", probe_ata);
CIUKI_F1_PROBE("ata-fault", probe_ata_fault);
CIUKI_F1_PROBE("partition", probe_partition);
#endif
