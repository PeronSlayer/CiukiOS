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
#define KERNEL_VBASE 0xc0000000u
static uint32_t flags = 0x200;
static unsigned schedules, yields, masks, unmasks, wakes;
static void (*on_schedule)(void);
static void (*on_yield)(void);
static bool masked[16];
static irq_handler_t handlers[16];
static struct task actor = { .id = 1, .state = T_RUNNING };
static struct task user_actor;
static _Alignas(4) uint8_t user_pages[3][4096];
static _Alignas(4096) uint8_t native_dma_table[4096], native_dma_bounce[ATA_DMA_BYTES];
static bool native_dma_ports, native_dma_config_fail;
static uint8_t native_dma_status[2], native_dma_command[2];
static uint16_t native_dma_pci_command;
static uint32_t native_dma_config_address;
static unsigned native_dma_allocs, native_dma_frees, native_dma_alloc_fail, native_dma_config_writes;
static unsigned page_count;
static unsigned dma_pin_live, dma_pin_sequence, dma_pin_fail;
/* f5-12B2: the CD-audio devctl bounce run (media_cd.c), 16 pages at 0x600000. */
#define CD_BOUNCE_PHYS 0x600000u
#define CD_BOUNCE_PAGES 16u
static _Alignas(4096) uint8_t cd_bounce_memory[CD_BOUNCE_PAGES * 4096];
static bool cd_bounce_mode, cd_bounce_fail;
static unsigned cd_bounce_allocs, cd_bounce_frees, cd_bounce_pages_out;
int mm_pin(struct mm_pin *pin) {
    CHECK(flags&0x200);
    if (dma_pin_fail && !--dma_pin_fail) return -ENOMEM;
    CHECK(pin && pin->pages && !pin->ticket);
    pin->ticket=++dma_pin_sequence; dma_pin_live++; return 0;
}
int mm_unpin(struct mm_pin *pin,uint32_t generation) {
    CHECK((flags&0x200) && pin && pin->ticket && pin->generation==generation && dma_pin_live);
    pin->ticket=0; dma_pin_live--; return 0;
}
static bool survivor_started;
static unsigned probe_ends, probe_passes, probe_errors;
static void *host_page(uint32_t phys) {
    if (phys == 0x400000) return native_dma_table;
    if (phys == 0x500000) return native_dma_bounce;
    if (phys >= CD_BOUNCE_PHYS && phys < CD_BOUNCE_PHYS + sizeof(cd_bounce_memory)) return cd_bounce_memory + (phys - CD_BOUNCE_PHYS);
    CHECK(phys >= 1 && phys <= 3); return user_pages[phys - 1];
}
#define P2V(p) host_page(p)
struct task *g_current = &actor;
volatile uint64_t g_ticks;
volatile bool g_need_resched;
bool g_cpu_tsc;
uint64_t g_tsc_per_ms;
#include <ciuki/init.h>
const struct activation_entry *drivers_mount_get(unsigned drive) { (void)drive; return 0; }
struct ciuki_boot_info g_boot;

static uint32_t read_eflags(void) { return flags; }
static uint32_t irq_save(void) { uint32_t f = flags; flags = 0; return f; }
static void irq_restore(uint32_t f) { flags = f; }
static uint8_t inb(uint16_t port) {
    if (native_dma_ports && (port == 0xc000 || port == 0xc008)) return native_dma_command[(port - 0xc000) / 8];
    if (native_dma_ports && (port == 0xc002 || port == 0xc00a)) return native_dma_status[(port - 0xc002) / 8];
    return 0xff;
}
static void outb(uint16_t port, uint8_t v) {
    if (native_dma_ports && (port == 0xc000 || port == 0xc008)) {
        unsigned channel = (port - 0xc000) / 8;
        native_dma_command[channel] = v;
        if (!(v & 1)) native_dma_status[channel] &= ~1u;
    }
}
static void outl(uint16_t port, uint32_t v) { if (port == 0xcf8) native_dma_config_address = v; }
static uint32_t inl(uint16_t port) { (void)port; return UINT32_MAX; }
static uint16_t cpu_inw(uint16_t port) { (void)port; return UINT16_MAX; }
static bool capture_logs;
static char captured_logs[4096];
void klog(const char *fmt, ...)
{
    if (!capture_logs) return;
    size_t used = strlen(captured_logs);
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(captured_logs + used, sizeof(captured_logs) - used, fmt, ap);
    va_end(ap);
    CHECK(n >= 0 && (size_t)n + 1 < sizeof(captured_logs) - used);
    if (n >= 0 && (size_t)n + 1 < sizeof(captured_logs) - used)
        strcat(captured_logs, "\n");
}
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
    CHECK((flags & 0x200) && g_current->state == T_BLOCKED);
    uint32_t saved = irq_save();
    schedules++;
    if (on_schedule)
        on_schedule();
    irq_restore(saved);
    ktimer_tick(g_ticks);
    while (ktimer_run_one()) ktimer_tick(g_ticks);
    saved = irq_save();
    if (g_current->state == T_BLOCKED) {
        g_ticks = g_current->wake_tick;
        g_current->state = T_READY;
    }
    CHECK(g_current->state == T_READY);
    g_current->state = T_RUNNING;
    irq_restore(saved);
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
uint32_t pmm_alloc_contig(uint32_t pages) {
    if (cd_bounce_mode) {
        CHECK(pages >= 1 && pages <= CD_BOUNCE_PAGES && !cd_bounce_pages_out);
        if (cd_bounce_fail) return 0;
        cd_bounce_allocs++; cd_bounce_pages_out = pages;
        return CD_BOUNCE_PHYS;
    }
    if (!native_dma_ports || ++native_dma_allocs == native_dma_alloc_fail) return 0;
    CHECK(pages == 1 || pages == ATA_DMA_BYTES / 4096);
    return pages == 1 ? 0x400000 : 0x500000;
}
bool pmm_is_reserved(uint32_t phys) { return phys < 0x100000; }
static uint32_t dma_mapping_base, dma_mapping_phys;
static bool dma_mapping_fragmented, dma_mapping_readonly, dma_mapping_uncached;
uint32_t vmm_kernel_pte(uint32_t va)
{
    if (va < dma_mapping_base || va - dma_mapping_base >= ATA_DMA_BYTES + 4096) return 0;
    unsigned page = (va - dma_mapping_base) / 4096;
    return (dma_mapping_phys + page * 4096 + (dma_mapping_fragmented && page ? 4096 : 0)) |
           1u | (dma_mapping_readonly ? 0 : 2u) | (dma_mapping_uncached ? 0x10u : 0);
}
void pmm_free(uint32_t p) {
    if (p >= CD_BOUNCE_PHYS && p < CD_BOUNCE_PHYS + sizeof(cd_bounce_memory)) {
        CHECK(cd_bounce_pages_out && !((p - CD_BOUNCE_PHYS) % 4096));
        if (!--cd_bounce_pages_out) cd_bounce_frees++;
        return;
    }
    if (p == 0x400000 || (p >= 0x500000 && p < 0x500000 + ATA_DMA_BYTES)) { native_dma_frees++; return; }
    CHECK(p >= 1 && p <= 3);
}
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

#include "scheduler_stubs.h"
#include "../../src/kernel/core/sync.c"
#include "../../src/kernel/lib/stackprot.c"
static uint64_t read_tsc_serialized(void) { return ktime_cycles(); }
#include "../../src/kernel/core/irq_budget.c"
#include "../../src/kernel/core/registry.c"
#include "../../src/kernel/drivers/ata_dma.c"
static unsigned ata_stamp_reads;
static uint64_t ata_command_clock(void) { return (uint64_t)++ata_stamp_reads*1000; }
#define ktime_cycles ata_command_clock
#include "../../src/kernel/drivers/ata.c"
#undef ktime_cycles
#include "../../src/kernel/drivers/blkpart.c"
#include "../../src/kernel/drivers/ata_probe.c"
#include "../../src/kernel/drivers/optical.c"
#include "../../src/kernel/drivers/optical_cd.c"
/* f5-12B2 CD-audio devctls (media_cd.c) against a flat fake user space. */
/* abi.h redefines these libc names (-D_POSIX_C_SOURCE exposes them); the
 * harness uses none of them. */
#undef CLOCKS_PER_SEC
#undef WIFEXITED
#undef WEXITSTATUS
#undef WIFSIGNALED
#undef WTERMSIG
#include <ciuki/files.h>
#include <ciuki/uaddr.h>
#define CD_USER_BASE 0x10000u
static uint8_t cd_user[160 * 1024];
static unsigned cd_user_calls, cd_user_fail_at; /* fail the Nth user copy of a call (1-based) */
int copy_from_user(void *dst, uint32_t src, uint32_t bytes)
{
    if (++cd_user_calls == cd_user_fail_at || src < CD_USER_BASE || bytes > sizeof(cd_user) || src - CD_USER_BASE > sizeof(cd_user) - bytes) return -EFAULT;
    memcpy(dst, cd_user + (src - CD_USER_BASE), bytes); return 0;
}
int copy_to_user(uint32_t dst, const void *src, uint32_t bytes)
{
    if (++cd_user_calls == cd_user_fail_at || dst < CD_USER_BASE || bytes > sizeof(cd_user) || dst - CD_USER_BASE > sizeof(cd_user) - bytes) return -EFAULT;
    memcpy(cd_user + (dst - CD_USER_BASE), src, bytes); return 0;
}
bool ua_range(const struct uaddr *u, uint32_t address, uint32_t bytes, uint32_t prot)
{
    (void)u; (void)prot;
    return address >= CD_USER_BASE && bytes <= sizeof(cd_user) && address - CD_USER_BASE <= sizeof(cd_user) - bytes;
}
int px_error(int e) { if (e == -FS_EQUARANTINED || e == -FS_EUCLEAN || e == -FS_ELOOP) return -EIO; return e; }
#include "../../src/kernel/drivers/media_cd.c"

enum fault { GOOD, ERR, DF, BUSY, NO_DRQ, EXTRA_DRQ, NO_IRQ, BAD_IDENTIFY, BAD_FLUSH, SLOW_FLUSH, BAD_MULTIPLE,
             DMA_ERROR, DMA_ACTIVE, DMA_NO_BM_IRQ, DMA_MODE_ERROR, DMA_STUCK_MASTER, DMA_ERROR_NO_IRQ };
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
    unsigned phase_sectors, phase_left, bursts, max_burst, delays;
    uint8_t multiple[2];
    uint64_t lba, issued_tick, delayed_irq;
    bool pending, immediate, writing, stalled_clock, fail_delay;
    uint8_t statuses[8];
    unsigned status_count, status_index;
    uint8_t bm_command, bm_status;
    uint32_t bm_table, direct_phys;
    void *direct_buffer;
    uint8_t *media_bytes;
    uint32_t media_sectors;
    unsigned dma_starts, dma_stops, dma_completions;
    bool dma_command, noncontiguous, early_dma_irq, early_dma_ready, reset_loses_udma, absent_ff;
    /* Discovery model: per-unit reset publication and task files, with host
     * task-file writes broadcast to both units as in QEMU ide/core.c. */
    bool discovery_model, reset_started, hide_packet[2], signature_seen[2], signature_published[2];
    bool zero_transient[2], unstable_signature;
    uint8_t taskfile[2][6], unit_status[2], unit_command[2], abort_error;
    uint16_t reset_low[2], abort_low;
    uint8_t packet_status[2];
    bool packet_transition[2], low_transition[2], low_settled[2];
    uint64_t signature_at[2], busy_until[2];
    unsigned settle_ms[2], busy_ms[2], signature_reads[2], reset_count;
    unsigned identify_ata[2], identify_atapi[2], premature_commands;
    bool defer_abort, bad_abort_signature;
    unsigned abort_status_reads;
    uint8_t abort_status;
    /* f1-92 interrupt-edge models. late_edge: an edge latched before the
     * master was prepared (no BM Interrupt) delivered right after a DMA
     * command; dma_deferred: Start leaves the transfer live (BSY/DRQ, Active)
     * until the next scheduler step; stray_after_start: one edge without BM
     * Interrupt while that transfer is live; pio_early_edge: an edge while a
     * PIO data-in command is still BSY, DRQ after busy_reads status reads. */
    bool late_edge, dma_deferred, dma_in_flight, stray_after_start, pio_early_edge;
    bool late_edge_bm, stray_bm_irq;   /* the edge is the drive's INTRQ: BM Interrupt latched */
    uint8_t late_edge_status, live_status;
    unsigned busy_reads, dma_aborts;
};
static struct fake *running;
static void dma_bus_start(struct fake *f);
static void dma_bus_finish(struct fake *f);
/* f1-92: called at every Command register write (request-cancellation and
 * competing-reader models of the T23 trial). */
static void (*command_hook)(struct fake *f, uint8_t command);

static void fake_native_irq(struct trap_frame *tf)
{
    CHECK(tf->vector == 0x20 + running->channel.irq);
    ata_channel_irq(&running->channel);
}
static void native_attach(struct fake *f, unsigned index)
{
    registry_init();
    f->channel.base = index ? 0x170 : 0x1f0;
    f->channel.control = index ? 0x376 : 0x3f6;
    f->channel.irq = index ? 15 : 14;
    CHECK(!claim_channel(&f->channel, index));
    f->channel.native = true;
    device_control(&f->channel, ATA_NIEN);
    irq_set_handler(f->channel.irq, fake_native_irq);
    pic_unmask(f->channel.irq);
}

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
    if (f->channel.native) {
        CHECK(!masked[f->channel.irq] && handlers[f->channel.irq]);
        struct trap_frame tf = { .vector = 0x20 + f->channel.irq };
        handlers[f->channel.irq](&tf);
    } else ata_channel_irq(&f->channel);
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
    if (f->channel.dma.ops && port == f->channel.dma.base) return f->bm_command;
    if (f->channel.dma.ops && port == f->channel.dma.base + 2) return f->bm_status;
    unsigned r = port - f->channel.base;
    if (f->discovery_model && f->reset_started && !f->unit_command[f->selected]) {
        unsigned u = f->selected;
        if (g_ticks < f->busy_until[u]) f->status = ATA_BSY;
        else {
            f->status = f->kind[u] == DISK ||
                (g_ticks < f->signature_at[u] && !f->zero_transient[u]) || f->hide_packet[u] ? ATA_DRDY : 0;
            if (!f->signature_published[u] && g_ticks >= f->signature_at[u]) {
                f->signature_published[u] = true;
                uint16_t low = f->kind[u] == ABSENT || f->low_transition[u] ? 0 : f->reset_low[u];
                f->taskfile[u][2] = low & 255; f->taskfile[u][3] = low >> 8;
                f->taskfile[u][4] = f->kind[u] == PACKET && !f->hide_packet[u] ? 0x14 : 0;
                f->taskfile[u][5] = f->kind[u] == PACKET && !f->hide_packet[u] ? 0xeb : 0;
            }
            if (f->signature_published[u] && f->kind[u] == PACKET && !f->hide_packet[u]) {
                bool first = g_ticks == f->signature_at[u];
                f->status = first && f->packet_transition[u] ? 0 : f->packet_status[u];
                /* Publish the SC/Low transition once. Later host writes
                 * must remain broadcast, rather than being reconstructed. */
                if (f->low_transition[u] && !f->low_settled[u] && !first) {
                    f->taskfile[u][2] = f->reset_low[u] & 255;
                    f->taskfile[u][3] = f->reset_low[u] >> 8;
                    f->low_settled[u] = true;
                }
            }
        }
    }
    if (port == f->channel.control || r == 7) {
        if (r == 7 && f->pending) { f->pending = false; f->irq_acks++; }
        if (f->busy_reads) {
            /* The early edge's PIO command becomes ready after N reads. */
            if (!--f->busy_reads) { f->status = 0x48; interrupt(f); }
            return ATA_BSY;
        }
        if (f->kind[f->selected] == ABSENT)
            return f->absent_ff ? 0xff : 0;
        if (port == f->channel.control && f->abort_status_reads) {
            f->abort_status_reads--;
            return ATA_DRDY; /* EC abort arrives just after this sample. */
        }
        if (f->status_index < f->status_count)
            return f->statuses[f->status_index++];
        return f->status;
    }
    if (r == 1) { f->errors++; return f->command ? f->abort_error : 1; }
    if (f->discovery_model && r >= 2 && r <= 5) {
        CHECK(!f->reset_started || g_ticks >= f->busy_until[f->selected]);
        if (r == 4 && f->unstable_signature && f->reset_started)
            f->taskfile[f->selected][r] ^= 1;
        f->signature_reads[f->selected]++;
        if (r == 5 && g_ticks >= f->signature_at[f->selected])
            f->signature_seen[f->selected] = true;
        return f->taskfile[f->selected][r];
    }
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
    if (f->channel.dma.ops && port == f->channel.dma.base) {
        CHECK(!((value ^ f->bm_command) & 8) || !(f->bm_command & 1));
        f->bm_command = value;
        if (value & 1) { f->dma_starts++; dma_bus_start(f); }
        else {
            f->dma_stops++;
            if (f->dma_in_flight) { f->dma_in_flight = false; f->dma_aborts++; } /* data discarded */
            if (f->fault != DMA_STUCK_MASTER) f->bm_status &= ~1u;
        }
        return;
    }
    if (f->channel.dma.ops && port == f->channel.dma.base + 2) {
        CHECK((value & 0x60) == (f->bm_status & 0x60));
        f->bm_status = (f->bm_status & 0x81) | (value & 0x60) | ((f->bm_status & 6) & ~value);
        return;
    }
    unsigned r = port - f->channel.base;
    if (port == f->channel.control) {
        f->control = value;
        if (value & 4) {
            f->command = 0; f->status = 0x40; f->pending = false; memset(f->multiple,0,sizeof(f->multiple));
            if (f->reset_loses_udma) for (unsigned u = 0; u < 2; u++) { f->identify[u][88] &= 255; f->identify[u][63] = 0x407; }
            if (f->discovery_model) {
                f->reset_started = true; f->reset_count++;
                for (unsigned u = 0; u < 2; u++) {
                    f->signature_at[u] = g_ticks + 2 + f->settle_ms[u];
                    f->busy_until[u] = g_ticks + 2 + f->busy_ms[u];
                    f->unit_status[u] = ATA_BSY; f->unit_command[u] = 0;
                    f->signature_published[u] = false;
                    f->signature_seen[u] = false;
                    f->low_settled[u] = false;
                    memset(f->taskfile[u], 0, sizeof(f->taskfile[u]));
                    /* A transient ATA-looking tuple before PACKET publication. */
                    f->taskfile[u][2] = f->taskfile[u][3] = f->zero_transient[u] ? 0 : 1;
                }
            }
        }
        return;
    }
    if (r == 6) {
        if (f->discovery_model) {
            f->unit_status[f->selected] = f->status;
            f->unit_command[f->selected] = f->command;
        }
        f->selected = (value >> 4) & 1;
        f->head = value;
        f->command = 0;             /* reset signature is preserved per device in this model */
        f->status = 0x40;
        if (f->discovery_model) {
            f->status = f->unit_status[f->selected];
            f->command = f->unit_command[f->selected];
        }
        return;
    }
    if (r < 6) {
        f->registers[r][1] = f->registers[r][0];
        f->registers[r][0] = value;
        if (f->discovery_model)
            f->taskfile[0][r] = f->taskfile[1][r] = value;
        return;
    }
    CHECK(r == 7);
    f->command = value;
    f->commands++;
    if (command_hook) command_hook(f, value);
    if (f->discovery_model) {
        if (f->reset_started && (!f->signature_seen[0] || !f->signature_seen[1]))
            f->premature_commands++;
        f->unit_command[f->selected] = value;
        if (value == ATA_IDENTIFY) f->identify_ata[f->selected]++;
        if (value == 0xa1) f->identify_atapi[f->selected]++;
    }
    f->issued_tick = g_ticks;
    f->words = 0;
    f->completed_blocks = 0;
    f->dma_command = value == 0xc8 || value == 0xca || value == 0x25 || value == 0x35;
    f->writing = value == 0x30 || value == 0x34 || value == 0xc5 || value == 0x39 || value == 0xca || value == 0x35;
    bool ext = value == 0x24 || value == 0x34 || value == 0x29 || value == 0x39 || value == 0x25 || value == 0x35;
    bool multiple = value == 0xc4 || value == 0xc5 || value == 0x29 || value == 0x39;
    if (multiple) CHECK(f->multiple[f->selected]!=0);
    f->lba = f->registers[3][0] | (uint64_t)f->registers[4][0] << 8 | (uint64_t)f->registers[5][0] << 16;
    if (ext)
        f->lba |= (uint64_t)f->registers[3][1] << 24 | (uint64_t)f->registers[4][1] << 32 |
                  (uint64_t)f->registers[5][1] << 40;
    else
        f->lba |= (uint64_t)(f->head & 15) << 24;
    f->blocks = (value == 0xec || value == 0xa1) ? 1 : f->registers[2][0] + (ext ? (unsigned)f->registers[2][1] << 8 : 0);
    if (!f->blocks)
        f->blocks = 256;
    f->phase_sectors=multiple ? f->multiple[f->selected] : 1;
    f->phase_left=f->blocks<f->phase_sectors ? f->blocks : f->phase_sectors;
    bool flush = value == 0xe7 || value == 0xea;
    f->status = flush || value==0xc6 || value==0xef || f->dma_command ? 0x40 : 0x48;
    if (value == 0xef) {
        CHECK(f->registers[1][0] == 3 && f->registers[2][0] == (0x40 | f->channel.devices[f->selected].udma_mode));
        if (f->fault == DMA_MODE_ERROR) f->status = 0x41;
    }
    if (value==0xc6) {
        CHECK(f->registers[2][0]==(f->identify[f->selected][47]&255));
        f->multiple[f->selected]=f->registers[2][0];
        f->identify[f->selected][59]=0x100|f->multiple[f->selected];
    }
    if (f->fault == ERR || (f->fault == BAD_IDENTIFY && value == 0xec) || (f->fault == BAD_FLUSH && flush) ||
        (f->fault == BAD_MULTIPLE && value == 0xc6))
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
    if (f->discovery_model && f->kind[f->selected] == PACKET && value == ATA_IDENTIFY) {
        unsigned u = f->selected;
        f->taskfile[u][2] = f->abort_low & 255; f->taskfile[u][3] = f->abort_low >> 8;
        f->taskfile[u][4] = 0x14; f->taskfile[u][5] = f->bad_abort_signature ? 0 : 0xeb;
        f->status = f->abort_status;
        if (f->defer_abort) f->abort_status_reads = 1;
    }
    if (f->dma_command) {
        if (f->early_dma_irq) {
            f->status = f->early_dma_ready ? 0x40 : 0x41;
            if (f->early_dma_ready) f->bm_status |= ATA_BM_IRQ;
            interrupt(f);
        }
        if (f->late_edge) {
            /* The PIC delivers an edge latched before ata_dma_prepare cleared
             * the master: the drive shows a stale DRDY, the BM shows nothing. */
            f->late_edge = false;
            if (f->late_edge_status) f->status = f->late_edge_status;
            if (f->late_edge_bm) f->bm_status |= ATA_BM_IRQ;
            f->pending = true; deliver(f);
        }
        return;
    }
    if (f->pio_early_edge && !f->writing) {
        /* ATA-6 9.5 DPIOI0: BSY while preparing. An edge arrives anyway. */
        f->pio_early_edge = false;
        f->status = ATA_BSY; f->busy_reads = 4;
        f->pending = true; deliver(f);
        return;
    }
    if (f->fault != NO_IRQ && (!f->writing || f->fault == ERR || f->fault == DF))
        interrupt(f);
}
static void block_end(struct fake *f)
{
    f->words = 0;
    CHECK(f->blocks > 0);
    f->blocks--;
    if (f->blocks) f->lba++;
    if (--f->phase_left) return;
    f->completed_blocks++;
    if (f->blocks) {
        f->phase_left=f->blocks<f->phase_sectors ? f->blocks : f->phase_sectors;
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
    if (native_dma_ports && port == 0xcfc) return native_dma_pci_command;
    struct fake *f = ctx;
    CHECK(port == f->channel.base && f->status == 0x48 && !f->writing);
    uint16_t v = (f->command == 0xec || f->command == 0xa1) ? f->identify[f->selected][f->words] :
        f->media_bytes ? fs_rd16(f->media_bytes + f->lba * 512 + f->words * 2) : sector_word(f->lba, f->words);
    f->data_reads++;
    if (++f->words == 256)
        block_end(f);
    return v;
}
static void fake_out16(void *ctx, uint16_t port, uint16_t value)
{
    if (native_dma_ports && port == 0xcfc) {
        native_dma_config_writes++;
        if (!native_dma_config_fail) native_dma_pci_command = value;
        return;
    }
    struct fake *f = ctx;
    CHECK(port == f->channel.base && f->status == 0x48 && f->writing);
    if (f->media_bytes) fs_wr16(f->media_bytes + f->lba * 512 + f->words * 2, value);
    else CHECK(value == sector_word(f->lba, f->words));
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
    f->delays++;
    return f->fail_delay ? -EFAULT : 0;
}
static void fake_insw(void *ctx, uint16_t port, void *buf, uint32_t words)
{
    struct fake *f=ctx; f->bursts++;
    if (words>f->max_burst) f->max_burst=words;
    for (unsigned i=0;i<words;i++) fs_wr16((uint8_t *)buf+i*2,fake_in16(ctx,port));
}
static void fake_outsw(void *ctx, uint16_t port, const void *buf, uint32_t words)
{
    struct fake *f=ctx; f->bursts++;
    if (words>f->max_burst) f->max_burst=words;
    for (unsigned i=0;i<words;i++) fake_out16(ctx,port,fs_rd16((const uint8_t *)buf+i*2));
}
static const struct ata_port_ops fake_ops = {
    fake_in8, fake_out8, fake_in16, fake_out16, fake_now, fake_deadline, fake_delay,
    fake_insw, fake_outsw,
};
static void irq_schedule(void)
{
    struct fake *f = running;
    /* A deferred DMA transfer ends one scheduler step after any edge that
     * is already pending (the stray edge is delivered first). */
    if (f->dma_in_flight && !f->pending) dma_bus_finish(f);
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
static void stale_expire(void) { on_schedule = irq_schedule; } /* this step expires the wait */
static void stale_schedule(void)
{
    struct ata_channel *c = &running->channel;
    c->irq_generation = c->request_generation - 1;
    c->irq_sequence++;
    kwait_wake_all(&c->wait);
    on_schedule = stale_expire;     /* next scheduler step expires the unmatched wait */
}
static void fresh(struct fake *f, bool ext)
{
    memset(f, 0, sizeof(*f));
    f->kind[0] = DISK;
    f->abort_error = 4;
    f->abort_status = ATA_DRDY | ATA_ERR;
    f->abort_low = f->reset_low[0] = f->reset_low[1] = 0x0101;
    captured_logs[0] = 0;
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
    CHECK(ata_channel_discover(&f.channel) == 1 && f.channel.devices[1].atapi && !f.channel.devices[1].identified && f.commands == 2);
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
            CHECK(f.commands - commands == (n + 127) / 128 && f.delivered - irqs == n);
            CHECK(!f.channel.active && !f.channel.mutex.owner);
            CHECK(f.command == (ext ? 0x24 : 0x20));
            commands = f.commands; irqs = f.delivered;
            CHECK(b->write(b, lba, n, buf + 1) == 0);
            CHECK(f.commands - commands == (n + 127) / 128 && f.delivered - irqs == n);
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

static void test_multiple(void)
{
    struct fake f;
    uint8_t *buf=malloc(513*512+1), *expected=malloc(513*512);
    CHECK(buf && expected);
    static const unsigned counts[]={1,2,15,16,17,127,128,256,513};
    for (unsigned ext=0;ext<2;ext++) for (unsigned size=16;size<=128;size*=8) {
        fresh(&f,ext!=0);
        f.identify[0][47]=0x8000|size;
        /* Invalid current-setting field must not disable word 47 support. */
        f.identify[0][59]=size;
        CHECK(ata_channel_discover(&f.channel)==1 && f.commands==2 && f.channel.devices[0].command==0xc6);
        struct blkdev *b=&f.channel.devices[0].block;
        CHECK(f.channel.devices[0].multiple_sectors==size);
        uint64_t lba=ext ? 0x123456789ull : 0x81234;
        for (unsigned i=0;i<sizeof(counts)/sizeof(counts[0]);i++) {
            unsigned n=counts[i], commands=f.commands, irqs=f.delivered, delays=f.delays, y=yields;
            unsigned phases=(n+size-1)/size;
            fill(expected,lba,n); memset(buf,0,n*512+1);
            f.immediate=(i&1)!=0;
            CHECK(!b->read(b,lba,n,buf+1) && !memcmp(buf+1,expected,n*512) && !buf[0]);
            CHECK(f.commands-commands==(n+127)/128 && f.delivered-irqs==phases);
            CHECK(f.command==(ext ? 0x29 : 0xc4) && f.max_burst<=1024);
            CHECK(f.delays-delays<n*256); /* no microsecond delay per word */
            if (n>4) CHECK(yields>y);
            commands=f.commands; irqs=f.delivered;
            CHECK(!b->write(b,lba,n,buf+1));
            CHECK(f.commands-commands==(n+127)/128 && f.delivered-irqs==phases);
            CHECK(f.command==(ext ? 0x39 : 0xc5) && !f.channel.active && !f.channel.mutex.owner);
            CHECK(f.irq_acks==f.delivered && !b->quarantined);
        }
    }
    /* Valid-but-disabled word 59 also requires SET; invalid word 47 uses
     * the sector protocol without issuing a speculative SET. */
    for (unsigned i=0;i<4;i++) {
        fresh(&f,false); f.identify[0][47]=i==0 ? 16 : i==1 ? 0 : i==2 ? 3 : 255;
        f.identify[0][59]=0x100;
        CHECK(ata_channel_discover(&f.channel)==1 && f.commands==(i==0 ? 2u : 1u));
        CHECK(f.channel.devices[0].multiple_sectors==(i==0 ? 16u : 0u));
    }
    /* Failed SET and failed later multiple block never fall back/retry. */
    fresh(&f,false); f.identify[0][47]=16; f.fault=BAD_MULTIPLE;
    CHECK(ata_channel_discover(&f.channel)==-FS_EIO && f.channel.stopped && f.commands==2);
    struct blkdev *b=&f.channel.devices[0].block;
    for (unsigned write=0;write<2;write++) {
        fresh(&f,false); f.identify[0][47]=16; CHECK(ata_channel_discover(&f.channel)==1);
        b=&f.channel.devices[0].block; f.fail_after=1;
        fill(buf,0,17); unsigned commands=f.commands, words=write ? f.data_writes : f.data_reads;
        CHECK((write ? b->write(b,0,17,buf) : b->read(b,0,17,buf))==-FS_EIO);
        CHECK((write ? f.data_writes : f.data_reads)-words==16*256);
        CHECK(f.commands==commands+1 && b->quarantined);
        CHECK(b->read(b,0,17,buf)==-FS_EQUARANTINED && f.commands==commands+1);
    }
    /* The optional burst boundary may be absent on scripted ports. */
    fresh(&f,false); struct ata_port_ops word_ops=fake_ops; word_ops.insw=0; word_ops.outsw=0;
    f.channel.ops=&word_ops; f.identify[0][47]=16; CHECK(ata_channel_discover(&f.channel)==1);
    b=&f.channel.devices[0].block; fill(expected,0,17);
    CHECK(!b->read(b,0,17,buf) && !memcmp(buf,expected,17*512));
    CHECK(!b->write(b,0,17,buf) && !f.bursts);
    free(buf); free(expected);
    printf("ata MULTIPLE: 28/48-bit, SET validity, short blocks, 128-sector blocks, IRQ counts, bounded bursts, fallback, quarantine PASS\n");
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
    irq_set_handler(14, fake_native_irq);
    pic_unmask(14);
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
#include "atapi_test.inc"
#include "ata_dma_test.inc"
#include "ata_f192_test.inc"

static void discovery_fresh(struct fake *f, unsigned topology, bool dma)
{
    if (dma) dma_fresh(f, false); else fresh(f, false);
    f->discovery_model = true;
    for (unsigned u = 0; u < 2; u++) {
        f->kind[u] = topology & (1u << u) ? PACKET : DISK;
        f->unit_status[u] = ATA_DRDY;
        f->taskfile[u][2] = f->taskfile[u][3] = 1;
        f->taskfile[u][4] = f->kind[u] == PACKET ? 0x14 : 0;
        f->taskfile[u][5] = f->kind[u] == PACKET ? 0xeb : 0;
        if (f->kind[u] == PACKET) f->identify[u][0] = 0x8500;
    }
}

static void test_t23_discovery(void)
{
    capture_logs = true;
    /* Single optical master: the first reset sample is 00/14/EB, then
     * DRDY|DSC. SC/Low may also change while Mid/High remain decisive. */
    for (unsigned dma = 0; dma < 2; dma++) {
        for (unsigned transient = 0; transient < 2; transient++) {
            struct fake f;
            discovery_fresh(&f, 1, dma); f.kind[1] = ABSENT;
            f.packet_status[0] = 0x50; f.packet_transition[0] = true;
            f.low_transition[0] = transient;
            f.reset_low[0] = transient ? 0x0302 : 0x0101;
            f.unit_status[0] = 0;
            native_attach(&f, 1);
            CHECK(ata_channel_discover(&f.channel) == 1 && !f.channel.stopped);
            CHECK(f.channel.devices[0].atapi && f.channel.devices[0].identified);
            CHECK(!f.channel.devices[1].present && !f.identify_ata[0] && f.identify_atapi[0] == 1);
            CHECK(f.reset_count == 1 && !f.premature_commands);
            CHECK(f.signature_reads[0] == 8 + 4 * dma); /* exactly two reset samples */
            const char *first = strstr(captured_logs, transient ?
                "[ata1] unit=0 status=00 signature=000014eb\n" :
                "[ata1] unit=0 status=00 signature=010114eb\n");
            const char *second = strstr(captured_logs, transient ?
                "[ata1] unit=0 status=50 signature=020314eb\n" :
                "[ata1] unit=0 status=50 signature=010114eb\n");
            const char *identified = strstr(captured_logs, "[ata1] unit=0 kind=atapi");
            CHECK(first && second && identified && first < second && second < identified);
            CHECK(strstr(captured_logs, "[ata1] unit=1 status=00 signature=00000000\n"));
            CHECK(!strstr(captured_logs, "fault") && !f.channel.active && (f.control & ATA_NIEN));
        }
    }
    capture_logs = false;
    /* Zero or DRDY|DSC status, including a permanently incomplete SC/Low
     * tuple, with/without BSY and the pre-reset firmware probe. */
    const uint16_t lows[] = { 0x0101, 0, 0x0302 };
    for (unsigned dma = 0; dma < 2; dma++) {
        for (unsigned ready = 0; ready < 2; ready++) {
            for (unsigned low = 0; low < 3; low++) {
                for (unsigned busy = 0; busy < 2; busy++) {
                    struct fake f;
                    discovery_fresh(&f, 1, dma); f.kind[1] = ABSENT;
                    f.reset_low[0] = lows[low]; f.packet_status[0] = ready ? 0x50 : 0;
                    f.busy_ms[0] = busy ? 5 : 0;
                    CHECK(ata_channel_discover(&f.channel) == 1 && !f.channel.stopped);
                    CHECK(f.channel.devices[0].atapi && f.channel.devices[0].identified);
                    CHECK(!f.identify_ata[0] && f.identify_atapi[0] == 1);
                    CHECK(!f.premature_commands && f.reset_count == 1);
                }
            }
        }
    }
    capture_logs = true;
    /* Late PACKET signature: EC aborts with T23 status 51/error 04, then
     * exactly one A1, through the IRQ path and both firmware poll paths. */
    for (unsigned dma = 0; dma < 2; dma++) {
        for (unsigned deferred = 0; deferred < 2; deferred++) {
            for (unsigned low = 0; low < 3; low++) {
                for (unsigned immediate = 0; immediate < 2; immediate++) {
                    struct fake f;
                    discovery_fresh(&f, 1, dma); f.kind[1] = ABSENT;
                    if (dma) f.taskfile[0][4] = f.taskfile[0][5] = 0;
                    else f.hide_packet[0] = true;
                    f.abort_low = lows[low]; f.abort_status = 0x51;
                    f.packet_status[0] = 0x50; f.packet_transition[0] = true;
                    f.defer_abort = deferred; f.immediate = immediate;
                    native_attach(&f, 1);
                    CHECK(ata_channel_discover(&f.channel) == 1 && !f.channel.stopped);
                    CHECK(f.channel.devices[0].atapi && f.channel.devices[0].identified);
                    CHECK(f.identify_ata[0] == 1 && f.identify_atapi[0] == 1 && f.commands == 2);
                    CHECK(f.reset_count == 1 && !f.premature_commands);
                    CHECK(!f.channel.active && !f.channel.request_generation &&
                          !f.channel.mutex.owner && (f.control & ATA_NIEN));
                    CHECK(!f.channel.devices[0].block.quarantined && !masked[15]);
                    const char *aborted = strstr(captured_logs, low == 0 ?
                        "[ata1] unit=0 status=51 signature=010114eb\n" : low == 1 ?
                        "[ata1] unit=0 status=51 signature=000014eb\n" :
                        "[ata1] unit=0 status=51 signature=020314eb\n");
                    const char *identified = strstr(captured_logs, "[ata1] unit=0 kind=atapi");
                    CHECK(aborted && identified && aborted < identified && !strstr(captured_logs, "fault"));
                }
            }
        }
    }
    /* A pre-reset failure must retain both the sampled tuple and the
     * abort tuple before containment; its missing 14/EB still fails. */
    struct fake f;
    discovery_fresh(&f, 1, true); f.kind[1] = ABSENT;
    f.taskfile[0][4] = f.taskfile[0][5] = 0;
    f.abort_status = 0x51; f.bad_abort_signature = true;
    native_attach(&f, 1);
    CHECK(ata_channel_discover(&f.channel) == -FS_EIO && f.channel.stopped);
    CHECK(!f.reset_count && f.identify_ata[0] == 1 && !f.identify_atapi[0]);
    const char *sample = strstr(captured_logs, "[ata1] unit=0 status=40 signature=01010000\n");
    const char *aborted = strstr(captured_logs, "[ata1] unit=0 status=51 signature=01011400\n");
    const char *fault = strstr(captured_logs, "[ata1] fault unit=0 command=ec");
    CHECK(sample && aborted && fault && sample < aborted && aborted < fault);
    capture_logs = false;
    puts("ATA T23 discovery: zero/DRDY|DSC status, SC/Low transients, BSY wait, status 51 EC ABRT -> A1, signature logs: PASS");
}

static void test_discovery_topologies(void)
{
    for (unsigned dma = 0; dma < 2; dma++) {
        for (unsigned topology = 0; topology < 4; topology++) {
            for (unsigned delayed = 0; delayed < 4; delayed++) {
                struct fake f;
                discovery_fresh(&f, topology, dma);
                if (delayed == 1) f.settle_ms[0] = f.settle_ms[1] = 1;
                if (delayed == 2) { f.busy_ms[1] = 5; f.settle_ms[1] = 5; }
                if (delayed == 3) {
                    f.settle_ms[0] = f.settle_ms[1] = 1;
                    f.zero_transient[0] = f.zero_transient[1] = true;
                }
                /* SET MULTIPLE on the master overwrites the slave's SC:
                 * pass 1 must finish before this command can be issued. */
                if (f.kind[0] == DISK) f.identify[0][47] = 16;
                int result = ata_channel_discover(&f.channel);
                CHECK(result == 2 && !f.channel.stopped);
                CHECK(f.reset_count == 1 && !f.premature_commands);
                for (unsigned u = 0; u < 2; u++) {
                    struct ata_device *d = &f.channel.devices[u];
                    CHECK(d->present && d->identified && !d->block.quarantined);
                    CHECK(d->atapi == (f.kind[u] == PACKET));
                    CHECK(f.identify_atapi[u] == (unsigned)d->atapi);
                    CHECK(f.identify_ata[u] == (d->atapi ? 0u : 1u + dma));
                    CHECK(f.signature_reads[u] >= 4);
                }
                CHECK(!f.channel.active && !f.channel.mutex.owner && (f.control & ATA_NIEN));
            }
        }
    }
    /* A stable ATA-looking reset tuple can still belong to a PACKET unit.
     * Its EC ABRT restores the signature, allowing exactly one A1. */
    for (unsigned unit = 0; unit < 2; unit++) {
        for (unsigned immediate = 0; immediate < 2; immediate++) {
            struct fake f;
            discovery_fresh(&f, 1u << unit, false);
            f.hide_packet[unit] = true; f.immediate = immediate;
            native_attach(&f, 1);
            unsigned pic_writes = masks + unmasks;
            CHECK(ata_channel_discover(&f.channel) == 2 && !f.channel.stopped);
            CHECK(f.channel.devices[unit].atapi && f.channel.devices[unit].identified);
            CHECK(f.identify_ata[unit] == 1 && f.identify_atapi[unit] == 1);
            CHECK(f.commands == 3 && f.data_reads == 512 && f.reset_count == 1);
            CHECK(!f.premature_commands && !f.channel.active && (f.control & ATA_NIEN));
            CHECK(masks + unmasks == pic_writes && !masked[15]);
        }
    }
    /* Firmware's pre-SRST probe also issues EC with nIEN. Reproduce the
     * race where its first sample is idle and the DRQ poll sees ABRT. */
    for (unsigned unit = 0; unit < 2; unit++) {
        struct fake f;
        discovery_fresh(&f, 1u << unit, true);
        f.taskfile[unit][4] = f.taskfile[unit][5] = 0;
        f.defer_abort = true;
        CHECK(ata_channel_discover(&f.channel) == 2 && !f.channel.stopped);
        CHECK(f.identify_ata[unit] == 1 && f.identify_atapi[unit] == 1);
        CHECK(f.reset_count == 1 && !f.premature_commands);
    }
    for (unsigned fault = 0; fault < 7; fault++) {
        struct fake f;
        discovery_fresh(&f, 1, false); f.hide_packet[0] = true;
        if (fault == 0) f.abort_error = 0x10;
        if (fault == 1) f.bad_abort_signature = true;
        if (fault == 2) f.abort_status |= ATA_DF;
        if (fault == 3) f.abort_status |= ATA_DRQ;
        if (fault == 4) f.fault = NO_IRQ;
        if (fault == 5) f.abort_status |= ATA_BSY;
        if (fault == 6) f.fault = ERR; /* EC classifies; A1 really fails. */
        uint64_t start = g_ticks;
        CHECK(ata_channel_discover(&f.channel) == -FS_EIO && f.channel.stopped);
        CHECK(g_ticks - start <= ATA_COMMAND_MS + 6);
        CHECK(f.identify_ata[0] == 1 && f.identify_atapi[0] == (unsigned)(fault == 6));
        CHECK(f.channel.devices[0].block.quarantined && f.channel.devices[1].block.quarantined);
        CHECK(!f.channel.active && !f.channel.mutex.owner && (f.control & ATA_NIEN));
        unsigned writes = f.writes;
        CHECK(ata_channel_discover(&f.channel) == -FS_EQUARANTINED && f.writes == writes);
    }
    struct fake f;
    discovery_fresh(&f, 1, false); f.hide_packet[0] = true;
    f.identify[0][0] = 0x8100; /* Unsupported PACKET type still skips safely. */
    CHECK(ata_channel_discover(&f.channel) == 1 && !f.channel.stopped);
    CHECK(f.channel.devices[0].atapi && !f.channel.devices[0].identified && f.channel.devices[1].identified);
    discovery_fresh(&f, 3, false);
    f.busy_ms[1] = f.settle_ms[1] = 2 * ATA_COMMAND_MS;
    CHECK(ata_channel_discover(&f.channel) == -FS_EIO && f.channel.stopped);
    CHECK(!f.commands && f.reset_count == 1 && !f.channel.devices[0].issued);
    discovery_fresh(&f, 3, false); f.unstable_signature = true;
    uint64_t start = g_ticks;
    CHECK(ata_channel_discover(&f.channel) == -FS_EIO && f.channel.stopped);
    CHECK(g_ticks - start == ATA_COMMAND_MS + 2 && !f.commands && !f.channel.active);
    puts("ATA discovery: four two-unit topologies, delayed signatures/BSY, broadcast writes, EC ABRT -> A1: PASS");
}
static void test_constant_pic_masks(void)
{
    uint8_t *data = malloc(257 * 512), *expected = malloc(257 * 512);
    CHECK(data && expected);
    for (unsigned index = 0; index < 2; index++) for (unsigned ext = 0; ext < 2; ext++)
        for (unsigned mode = 0; mode < 3; mode++) {
            struct fake f;
            if (mode == 2) dma_fresh(&f, ext);
            else fresh(&f, ext);
            if (mode == 1) f.identify[0][47] = 16;
            native_attach(&f, index);
            unsigned before = masks + unmasks;
            CHECK(ata_channel_discover(&f.channel) == 1);
            for (unsigned i = 0; i < 3; i++)
                CHECK(!registry_activate(f.channel.claims[i], f.channel.generations[i]));
            CHECK(masks + unmasks == before && !masked[f.channel.irq] && (f.control & ATA_NIEN));
            struct blkdev *b = &f.channel.devices[0].block;
            const unsigned counts[] = { 1, 2, 257 };
            for (unsigned i = 0; i < ARRAY_SIZE(counts); i++) {
                unsigned n = counts[i];
                f.immediate = (i & 1) != 0;
                fill(expected, 7, n);
                CHECK(!b->read(b, 7, n, data) && !memcmp(data, expected, n * 512));
                CHECK(!b->write(b, 7, n, data) && !b->flush(b));
                CHECK((f.control & ATA_NIEN) && !f.channel.active && !masked[f.channel.irq]);
                CHECK(masks + unmasks == before);
                /* A late edge is acknowledged without waking a new request. */
                uint32_t sequence = f.channel.irq_sequence;
                f.pending = true;
                struct trap_frame tf = { .vector = 0x20 + f.channel.irq };
                fake_native_irq(&tf);
                CHECK(!f.pending && f.channel.irq_sequence == sequence && masks + unmasks == before);
            }
            CHECK(ata_channel_idle(&f.channel) && (f.control & ATA_NIEN));
            CHECK(!handlers[f.channel.irq] && !masked[f.channel.irq] && masks + unmasks == before);
        }
    /* Polled packets share the same open line with a native ATA peer. */
    struct packet_fake f;
    packet_fresh(&f, 12);
    native_attach(&f.disk, 0);
    for (unsigned i = 0; i < 3; i++)
        CHECK(!registry_activate(f.disk.channel.claims[i], f.disk.channel.generations[i]));
    unsigned before = masks + unmasks;
    f.responses = true;
    struct ata_device *d = &f.disk.channel.devices[1];
    for (unsigned i = 0; i < 8; i++) {
        CHECK(!atapi_ready(d) && !d->block.read(&d->block, 7, 1, data));
        CHECK((f.disk.control & ATA_NIEN) && masks + unmasks == before);
        CHECK(!f.disk.channel.devices[0].block.read(&f.disk.channel.devices[0].block, 0, 1, data));
        CHECK(masks + unmasks == before && !masked[14]);
    }
    f.disk.fault = BUSY;
    CHECK(f.disk.channel.devices[0].block.read(&f.disk.channel.devices[0].block, 0, 1, data) == -FS_EIO);
    CHECK(masked[14] && masks + unmasks == before + 1);
    before = masks + unmasks;
    CHECK(f.disk.channel.devices[0].block.read(&f.disk.channel.devices[0].block, 0, 1, data) == -FS_EQUARANTINED);
    CHECK(masks + unmasks == before);
    /* A failed clean-release proof is exceptional containment, not a
     * per-request mask. Its channel cannot accept another command. */
    struct fake busy;
    fresh(&busy, false);
    native_attach(&busy, 1);
    before = masks + unmasks;
    busy.status = ATA_BSY;
    CHECK(!ata_channel_idle(&busy.channel) && (busy.control & ATA_NIEN) && masked[15]);
    CHECK(masks + unmasks == before + 1 && busy.channel.quiescing);
    free(data); free(expected);
    puts("ATA PIC: primary/secondary LBA28/48 PIO/MULTIPLE/DMA/FLUSH, ATAPI/peer, late IRQ and clean stop=0 writes after attach; quarantine=1 mask PASS");
}
static void test_emergency_recorder(void)
{
    for (unsigned ext=0;ext<2;ext++) {
        struct fake f;struct blkdev *d=qualify(&f,!!ext);
        uint64_t lba=ext ? 0x123456789ull : 8192;uint8_t data[512];fill(data,lba,1);
        unsigned commands=f.commands,before_yields=yields,before_waits=schedules;
        flags=0;CHECK(d->emergency_write && !d->emergency_write(d,lba,data));
        CHECK(f.commands==commands+2 && f.data_writes==256 && !flags);
        CHECK(yields==before_yields && schedules==before_waits && !f.channel.active && !f.channel.mutex.owner);
        unsigned writes=f.writes;
        f.channel.active=&f.channel.devices[0];CHECK(d->emergency_write(d,lba,data)==-FS_EBUSY);
        f.channel.active=0;f.channel.mutex.owner=&actor;CHECK(d->emergency_write(d,lba,data)==-FS_EBUSY);
        f.channel.mutex.owner=0;f.channel.stopped=true;CHECK(d->emergency_write(d,lba,data)==-FS_EBUSY);
        f.channel.stopped=false;f.status=0x80;CHECK(d->emergency_write(d,lba,data)==-FS_EBUSY);
        CHECK(f.writes==writes);
        f.status=0x40;f.fault=BUSY;commands=f.commands;
        CHECK(d->emergency_write(d,lba,data)==-FS_EIO && f.commands==commands+1);
        CHECK(yields==before_yields && schedules==before_waits);
        flags=0x200;
    }
    puts("PASS ATA emergency recorder: LBA28/LBA48 PIO + FLUSH, no scheduler/locks, busy/owned/quarantined refusal, finite stalled-command polls/no replay");
}
int main(void)
{
    test_t23_discovery(); test_discovery_topologies(); test_identify(); test_data(); test_multiple(); test_faults(); test_views(); test_claims(); test_probe_fixtures();
    test_storage_cache_faults(); test_packets(); test_optical_lifecycle(); test_cd_audio(); test_media_cd();
    test_dma(); test_f192(); test_constant_pic_masks(); test_emergency_recorder();
    printf("ata/blkpart host tests: %s (waits=%u wakes=%u yields=%u)\n",
           failures ? "FAIL" : "PASS", schedules, wakes, yields);
    return failures ? 1 : 0;
}
