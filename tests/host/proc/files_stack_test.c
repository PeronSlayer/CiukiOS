/* f2-20: production fd-table kernel cases and FAT-backed ELF loading on a
 * bounded stack. Linux startup/TLS and ASan frames stay outside the budget.
 * SPDX-License-Identifier: GPL-2.0-only */
#define FD_TABLE_STACK_TEST
#define rec_emit files_fixture_rec_emit
#include "files_test.c"
#undef rec_emit
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <ucontext.h>
#include <ciuki/storage.h>

static char records[16384];
static unsigned record_bytes;
void rec_emit(const char *probe, const char *event, const char *format, ...)
{
    CHECK(!strcmp(probe, "fd-table") && !strcmp(event, "DATA"));
    va_list args;
    va_start(args, format);
    int n = kvsnprintf(records + record_bytes, sizeof(records) - record_bytes, format, args);
    va_end(args);
    CHECK(n >= 0 && (unsigned)n + record_bytes + 1 < sizeof(records));
    record_bytes += (unsigned)n;
    records[record_bytes++] = '\n';
}
static struct storage host_storage;
struct storage *storage_get(void) { return &host_storage; }
struct storage_volume *storage_volume(struct storage *s, unsigned drive)
{ return s && drive < 26 ? &s->volumes[drive] : 0; }
#include "../../../src/kernel/probes/f2_probes_files.c"

static ucontext_t caller_context, probe_context;
static uint8_t *mapping, *bottom;
static unsigned stack_bytes, irq_count, io_depth, irq_depth, irq_water;
static bool bounded, inject_irq;
static int (*device_read)(struct blkdev *, uint64_t, uint32_t, void *);
static int (*stack_write_fn)(struct blkdev *, uint64_t, uint32_t, const void *);
static int (*device_flush)(struct blkdev *);

/* Same-CPL IA-32 entry: EFLAGS/CS/EIP, vector/error, PUSHA, four segments
 * (isr.asm: 17 words). Reserve another 512 bytes for dispatcher/handler calls.
 * Touch every byte; keep both allocations alive until the callback returns. */
static __attribute__((noinline)) void simulated_interrupt(void)
{
    volatile uint32_t frame[17];
    volatile uint8_t handler[512];
    for (unsigned i = 0; i < ARRAY_SIZE(frame); i++) frame[i] = 0x202u + i;
    for (unsigned i = 0; i < sizeof(handler); i++) handler[i] = (uint8_t)i;
    uintptr_t sp = (uintptr_t)&handler[0];
    CHECK(sp >= (uintptr_t)bottom && sp < (uintptr_t)bottom + stack_bytes);
    unsigned depth = (unsigned)((uintptr_t)bottom + stack_bytes - sp);
    if (depth > irq_depth) irq_depth = depth;
    if (stack_bytes == KSTACK_SIZE) {
        struct task measured = { .kstack = bottom };
        unsigned water = task_stack_high_water(&measured);
        if (water > irq_water) irq_water = water;
    }
    irq_count++;
    CHECK(frame[16] == 0x212u && handler[511] == 255);
}
static __attribute__((noinline)) void interrupt_at_io(void)
{
    volatile uint8_t marker = 0;
    if (!bounded) return;
    unsigned depth = (unsigned)((uintptr_t)bottom + stack_bytes - (uintptr_t)&marker);
    if (depth > io_depth) io_depth = depth;
    if (inject_irq) simulated_interrupt();
    CHECK(!marker);
}
static int interrupted_read(struct blkdev *d, uint64_t lba, uint32_t n, void *b)
{
    interrupt_at_io();
    return device_read(d, lba, n, b);
}
static int interrupted_write(struct blkdev *d, uint64_t lba, uint32_t n, const void *b)
{
    interrupt_at_io();
    host_storage.volumes[2].writes += n;
    return stack_write_fn(d, lba, n, b);
}
static int interrupted_flush(struct blkdev *d)
{
    interrupt_at_io();
    return device_flush(d);
}
static struct ciuki_file executable;
static int inherited_fd;
static __attribute__((noinline)) unsigned overflow(unsigned depth)
{
    volatile uint8_t bytes[512];
    for (unsigned i = 0; i < sizeof(bytes); i++) bytes[i] = (uint8_t)depth;
    unsigned n = depth ? overflow(depth - 1) : 0;
    return n + bytes[depth % sizeof(bytes)];
}
static bool overflow_control;
static void bounded_entry(void)
{
    bounded = true;
    if (overflow_control) { (void)overflow(64); abort(); }
    CHECK(kernel_cases(client));
    /* The fixture's ELF is stored in FAT, read with the production file
     * adapter, then validated/loaded and given an explicitly inherited fd. */
    struct proc_strings *args = proc_strings_new();
    CHECK(args && !proc_strings_add(args, "files", sizeof("files"), false));
    CHECK(!image_open(client->cwd, "/stack.elf", &executable));
    inherited_fd = file_open(client, "/dev/null", O_RDWR, 0); CHECK(inherited_fd >= 0);
    struct ciuki_spawn_fd inherit = { inherited_fd, 17 };
    struct process *child = 0;
    int spawned = proc_spawn_file(client, &executable, args, &inherit, 1, 0, 0, &child);
    if (spawned < 0) fprintf(stderr, "FAT-backed spawn error=%d\n", spawned);
    CHECK(spawned > 0);
    CHECK(child->fds[17].object == client->fds[inherited_fd].object);
    proc_strings_free(args);
    executable.close(executable.cookie);
    stop_collect(child);
    CHECK(durable_cases(client));
    bounded = false;
}
int main(int argc, char **argv)
{
    CHECK(argc == 6);
    stack_bytes = (unsigned)strtoul(argv[4], 0, 10);
    inject_irq = strcmp(argv[5], "plain") != 0;
    overflow_control = !strcmp(argv[5], "overflow");
    const struct rlimit no_core = { 0, 0 };
    CHECK(!setrlimit(RLIMIT_CORE, &no_core));
    ram = calloc(HOST_PAGES, PAGE_SIZE); CHECK(ram);
    controller.state = T_RUNNING; g_current = &controller; proc_init();
    CHECK(!fake_open(&disk[0], argv[1]) && !fake_open(&disk[1], argv[2]));
    disk[0].undo_enabled = disk[1].undo_enabled = true;
    CHECK(!cache_init(&cache, 0)); vfs_init(&vfs);
    for (unsigned i = 0; i < 2; i++) {
        CHECK(!fat_mount(&volumes[i], &cache, &disk[i].dev, 0, disk[i].dev.capacity, i == 0));
        CHECK(!vfs_attach(&vfs, i + 2, &volumes[i]));
    }
    CHECK(!files_attach(&vfs, &space));
    client_thread = make_process(proc_supervisor(), &client);
    g_current = client_thread->task; g_current->state = T_RUNNING;
    client->cwd = space->root; px_retain(client->cwd);
    client->cwd_retain = px_retain; client->cwd_release = px_release;
    FILE *payload = fopen(argv[3], "rb"); CHECK(payload);
    static uint8_t elf[16384];
    size_t bytes = fread(elf, 1, sizeof(elf), payload); CHECK(bytes && feof(payload));
    fclose(payload);
    int fd = file_open(client, "/stack.elf", O_CREAT | O_EXCL | O_RDWR, 0666); CHECK(fd >= 0);
    CHECK(io(fd, elf, bytes, true, false, 0) == (int)bytes && !file_close(client, fd));
    device_read = disk[0].dev.read; stack_write_fn = disk[0].dev.write; device_flush = disk[0].dev.flush;
    disk[0].dev.read = interrupted_read; disk[0].dev.write = interrupted_write; disk[0].dev.flush = interrupted_flush;
    unsigned page = (unsigned)getauxval(AT_PAGESZ); CHECK(page && stack_bytes && !(stack_bytes % page));
    mapping = mmap(0, stack_bytes + 2 * page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(mapping != MAP_FAILED);
    bottom = mapping + page;
    CHECK(!mprotect(bottom, stack_bytes, PROT_READ | PROT_WRITE));
    if (stack_bytes == KSTACK_SIZE) task_stack_init(bottom);
    else { memset(bottom, KSTACK_FILL, stack_bytes); *(uint32_t *)bottom = KSTACK_CANARY; }
    client_thread->task->kstack = bottom;
    CHECK(!getcontext(&probe_context));
    probe_context.uc_stack.ss_sp = bottom; probe_context.uc_stack.ss_size = stack_bytes;
    probe_context.uc_link = &caller_context;
    makecontext(&probe_context, bounded_entry, 0);
    CHECK(!swapcontext(&caller_context, &probe_context));
    unsigned untouched = sizeof(uint32_t);
    while (untouched < stack_bytes && bottom[untouched] == KSTACK_FILL) untouched++;
    unsigned water = stack_bytes - untouched;
    if (stack_bytes == KSTACK_SIZE) CHECK(water == task_stack_high_water(client_thread->task));
    CHECK(*(uint32_t *)bottom == KSTACK_CANARY);
    CHECK(io_depth && (!inject_irq || (irq_count && irq_depth > io_depth && water >= irq_water)));
    fwrite(records, 1, record_bytes, stdout);
    printf("case=stack task=fd-host size=%u high_water=%u io_depth=%u irq_depth=%u irq_count=%u irq_water=%u\n",
           stack_bytes, water, io_depth, irq_depth, irq_count, irq_water);
    client_thread->task->kstack = 0;
    CHECK(!munmap(mapping, stack_bytes + 2 * page));
    all_closed(); stop_collect(client);
    struct file_ledger ledger; files_snapshot(space, &ledger); CHECK(!ledger.descriptions && !ledger.pins);
    for (unsigned i = 0; i < 2; i++) CHECK(!vfs_detach(&vfs, i + 2));
    vfs_destroy(&vfs); cache_destroy(&cache);
    for (unsigned i = 0; i < 2; i++) { fake_reset(&disk[i]); fake_close(&disk[i]); }
    CHECK(!pages_used && heap_blocks == 1);
    free(ram);
    return 0;
}
