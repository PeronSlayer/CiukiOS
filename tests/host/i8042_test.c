/* T0: production controller/decoders and registry with fake hardware/time.
 * scripts/test/host_kernel_tests.sh, ASan/UBSan.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ciuki/kernel.h>
#include <ciuki/task.h>
#include <ciuki/sync.h>
#include <ciuki/work.h>
#include <ciuki/registry.h>
#include <ciuki/i8042.h>
#include <ciuki/fwinput.h>
#include <ciuki/biosvm.h>
#include <ciuki/init.h>

static unsigned failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* Substitute only privileged hardware and scheduler boundaries. The queue,
 * command engine, mutexes, generations and registry are production sources. */
#define CIUKI_CPU_H
static uint32_t host_flags = 0x200;
static uint32_t read_eflags(void) { return host_flags; }
static uint32_t irq_save(void) { uint32_t f = host_flags; host_flags = 0; return f; }
static void irq_restore(uint32_t f) { host_flags = f; }
static void outl(uint16_t port, uint32_t v) { (void)port; (void)v; }
static uint32_t inl(uint16_t port) { (void)port; return UINT32_MAX; }

struct fake_byte { uint8_t status, data; uint64_t ready; };
struct fake_bus {
    struct fake_byte bytes[64];
    unsigned head, count, writes, reads, sends, resets, config_writes, selftests;
    uint64_t now, ibf_until;
    uint32_t pause_step, ack_delay, selftest_delay, write_delay;
    uint8_t config, last_config, mouse_id, kbd_test_reply, aux_test_reply, loop_reply;
    unsigned resend_count;
    bool aux, config_next, missing, stuck_ibf, stuck_obf, stalled, bad_selftest;
    bool wrong_aux, mixed, irq_reply, stop_stuck, selftest_reenable, bad_ack;
    bool loop_next, loop_untagged, sticky_translation, ignore_enable_config;
    bool ignore_enable_commands, drop_irq_config, unstable_config, missing_aux;
    bool final_obf, obf_after_ack, bad_selftest_status;
    bool silent_kbd_test, silent_aux_test, config_ack, t23;
    uint8_t missing_command;
    uint8_t controller_ack_command, status_base;
    uint32_t controller_ack_delay, iface_delay;
    unsigned controller_ack_count;
    unsigned config_reads;
};
static struct fake_bus hw;
static irq_handler_t host_handlers[16];
static unsigned pic_changes, host_yields;
static bool host_masked[16];
static uint8_t host_pages[4][PAGE_SIZE];
static bool host_page_used[4];
static struct task *host_survivor;
static uint32_t host_survivor_data;
static bool replay_input;
static unsigned replay_cycles, record_count, record_pass, record_deferred, record_ready;
static size_t longest_record;
static bool print_records;
static unsigned firmware_selftests, firmware_records, absent_records, refused_records;
static int fake_selftest_result;
static unsigned host_allocations;
static char last_log[256], expected_end[128];
static char init_logs[I8042_INIT_STEPS][256];
static unsigned init_log_count, setup_records;
static uint32_t log_delay_ms;
static unsigned record_fail;
static struct task host_task = { .state = T_RUNNING };
struct task *g_current = &host_task;
volatile uint64_t g_ticks;
volatile bool g_need_resched;
uint64_t g_tsc_per_ms;
bool g_cpu_tsc;
struct ciuki_boot_info g_boot;

static void fake_push(struct fake_bus *b, bool aux, uint8_t data, uint32_t delay)
{
    CHECK(b->count < ARRAY_SIZE(b->bytes));
    if (b->count == ARRAY_SIZE(b->bytes)) return;
    unsigned n = (b->head + b->count++) % ARRAY_SIZE(b->bytes);
    b->bytes[n] = (struct fake_byte){ aux ? 0x21 : 0x01, data, b->now + delay };
}
static uint8_t fake_read(void *arg, uint16_t port)
{
    struct fake_bus *b = arg;
    b->reads++;
    if (port == 0x64) {
        uint8_t status = b->status_base | ((b->stuck_ibf || b->now < b->ibf_until) ? 2 : 0);
        if (b->stuck_obf) return status | 1;
        if (b->count && b->bytes[b->head].ready <= b->now) status |= b->bytes[b->head].status;
        return status;
    }
    if (b->stuck_obf) return 0;
    CHECK(b->count && b->bytes[b->head].ready <= b->now);
    if (!b->count) return 0;
    uint8_t data = b->bytes[b->head].data;
    b->head = (b->head + 1) % ARRAY_SIZE(b->bytes);
    b->count--;
    if (!b->count && b->obf_after_ack) b->stuck_obf = true;
    return data;
}
static void fake_write(void *arg, uint16_t port, uint8_t data)
{
    struct fake_bus *b = arg;
    b->writes++;
    b->ibf_until = b->now + b->write_delay;
    if (port == 0x64) {
        if (data == 0xD1 || data >= 0xF0) b->resets++;
        if (data == b->controller_ack_command)
            for (unsigned i = 0; i < b->controller_ack_count; i++)
                fake_push(b, false, 0xFA, b->controller_ack_delay);
        switch (data) {
        case 0xAD:
            b->config |= 0x10;
            if (b->t23 && b->selftests) b->status_base = 0x1C;
            if (b->stop_stuck) b->stuck_ibf = true;
            break;
        case 0xA7: b->config |= 0x20; break;
        case 0xAE: if (!b->ignore_enable_commands) b->config &= ~0x10; break;
        case 0xA8: if (!b->ignore_enable_commands) b->config &= ~0x20; break;
        case 0x20:
            b->config_reads++;
            fake_push(b, false, b->config ^ (b->unstable_config && (b->config_reads & 1) ? 0x04 : 0), 0);
            break;
        case 0x60: b->config_next = true; break;
        case 0xAA:
            b->selftests++;
            if (b->t23) b->status_base = 0x18;
            b->config = b->selftest_reenable ? 0x43 : 0x73;
            fake_push(b, false, b->bad_selftest ? 0xFC : 0x55, b->selftest_delay);
            if (b->bad_selftest_status) b->bytes[(b->head + b->count - 1) % ARRAY_SIZE(b->bytes)].status |= 0x80;
            break;
        case 0xAB:
            if (!b->silent_kbd_test) fake_push(b, false, b->kbd_test_reply, b->iface_delay);
            break;
        case 0xA9:
            if (!b->silent_aux_test) fake_push(b, false, b->aux_test_reply, b->iface_delay);
            break;
        case 0xD3: b->loop_next = true; break;
        case 0xD4: b->aux = true; break;
        default: CHECK(false); break;
        }
        return;
    }
    if (data == 0xFF) b->resets++;
    if (b->config_next) {
        b->config_next = false;
        b->last_config = data;
        if (b->ignore_enable_config) data |= b->config & 0x30;
        if (b->sticky_translation) data |= 0x40;
        if (b->drop_irq_config) data &= ~0x03;
        b->config = data;
        b->config_writes++;
        if (b->t23) b->status_base = b->config_writes == 1 ? 0x14 : 0x1C;
        if (b->config_ack) fake_push(b, false, 0xFA, b->controller_ack_delay);
        return;
    }
    if (b->loop_next) {
        b->loop_next = false;
        CHECK(data == 0x5A);
        fake_push(b, !b->loop_untagged, b->loop_reply, 0);
        return;
    }
    b->sends++;
    if (!b->missing && !(b->missing_aux && b->aux) && data != b->missing_command) {
        if (b->mixed) {
            fake_push(b, false, 0x1C, 0);
            fake_push(b, true, 0x08, 0);
            fake_push(b, true, 0x02, 0);
            fake_push(b, true, 0x01, 0);
            fake_push(b, false, 0xF0, 0);
            fake_push(b, false, 0x1C, 0);
        }
        uint8_t reply = b->sends <= b->resend_count ? 0xFE : 0xFA;
        fake_push(b, b->aux ^ b->wrong_aux, reply, b->ack_delay);
        if (b->bad_ack) b->bytes[(b->head + b->count - 1) % ARRAY_SIZE(b->bytes)].status |= 0x80;
        if (b->aux && data == 0xF2 && reply == 0xFA)
            fake_push(b, true, b->mouse_id, b->ack_delay);
        if (b->aux && data == 0xF4 && b->final_obf) b->obf_after_ack = true;
    }
    b->aux = false;
}
static uint64_t fake_now(void *arg) { return ((struct fake_bus *)arg)->now; }
static void fake_pause(void *arg)
{
    struct fake_bus *b = arg;
    CHECK(host_flags & 0x200);
    if (!b->stalled) b->now += b->pause_step;
    if (b == &hw) {
        g_ticks = b->now;
        if (b->irq_reply && host_handlers[1] && b->count && b->bytes[b->head].ready <= b->now) {
            uint32_t f = irq_save();
            struct trap_frame tf = { .vector = 0x21 };
            host_handlers[1](&tf);
            irq_restore(f);
        }
    }
}
static const struct i8042_test_io fake_io = { fake_read, fake_write, fake_now, fake_pause };
static uint8_t inb(uint16_t port) { return fake_read(&hw, port); }
static void outb(uint16_t port, uint8_t data)
{
    if (port == 0x60 || port == 0x64) fake_write(&hw, port, data);
}

void pic_mask(unsigned irq) { CHECK(irq < 16); host_masked[irq] = true; pic_changes++; }
void pic_unmask(unsigned irq)
{
    CHECK(irq < 16 && host_handlers[irq]);
    host_masked[irq] = false;
    if (irq >= 8) host_masked[2] = false;
    pic_changes++;
}
void irq_set_handler(unsigned irq, irq_handler_t h) { CHECK(irq < 16); host_handlers[irq] = h; }
void klog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_log, sizeof(last_log), fmt, ap);
    va_end(ap);
    if (!strncmp(last_log, "[i8042] step=", 13)) {
        if (strstr(last_log, " index=1 ")) init_log_count = 0;
        CHECK(init_log_count < I8042_INIT_STEPS);
        if (init_log_count < I8042_INIT_STEPS)
            strcpy(init_logs[init_log_count++], last_log);
        if (print_records) puts(last_log);
        hw.now += log_delay_ms;
        g_ticks = hw.now;
    }
}
int ksnprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int result = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return result;
}
__attribute__((noreturn)) void panic(const char *fmt, ...) { fprintf(stderr, "panic: %s\n", fmt); exit(2); }
void *kzalloc(size_t n)
{
    host_allocations++;
    /* Match core/kheap.c's largest class, including its 8-byte header. */
    return n <= 2048 - 8 ? calloc(1, n) : 0;
}
void kfree(void *p) { free(p); }
void task_start(struct task *t) { t->state = T_READY; }
void schedule(void) { CHECK(false); }   /* all tested command mutexes are uncontended */
void task_yield(void) { CHECK(host_flags & 0x200); host_yields++; }
static void *host_p2v(uint32_t p)
{
    CHECK(p && p <= sizeof(host_pages) && !(p % PAGE_SIZE));
    return host_pages[p / PAGE_SIZE - 1];
}
#define P2V(p) host_p2v(p)
uint32_t pmm_alloc(void)
{
    for (unsigned i = 0; i < ARRAY_SIZE(host_pages); i++)
        if (!host_page_used[i]) { host_page_used[i] = true; return (i + 1) * PAGE_SIZE; }
    return 0;
}
void pmm_free(uint32_t p) { CHECK(host_page_used[p / PAGE_SIZE - 1]); host_page_used[p / PAGE_SIZE - 1] = false; }
int as_map(struct aspace *as, uint32_t va, uint32_t p, uint32_t fl)
{
    (void)fl;
    CHECK(as == &host_survivor->as);
    as->pages++;
    if (va == 0x00400000) host_survivor_data = p;
    return 0;
}
struct task *task_create_user(const char *name, enum task_prio prio, uint32_t entry, uint32_t esp,
                              uint32_t eax, uint32_t ebx, uint32_t ecx)
{
    (void)name; (void)prio; (void)ecx;
    CHECK(!host_survivor && entry == 0x00401000 && esp == 0xBFFFFFF0 && eax == 7 && ebx == 0x5EED5EED);
    host_survivor = calloc(1, sizeof(*host_survivor));
    return host_survivor;
}
bool task_alive(const struct task *t) { return t->state != T_ZOMBIE && t->state != T_DEAD; }
void task_kill(struct task *t, int code) { t->exit_code = code; t->state = T_ZOMBIE; }
void task_reap(struct task *t)
{
    CHECK(t == host_survivor && t->state == T_ZOMBIE);
    /* All three pages are owned by this one fake address space. */
    memset(host_page_used, 0, sizeof(host_page_used));
    free(t);
    host_survivor = 0;
    host_survivor_data = 0;
}
static void replay_byte(bool aux, uint8_t byte)
{
    fake_push(&hw, aux, byte, 0);
    CHECK(host_handlers[1] != 0);
    uint32_t f = irq_save();
    struct trap_frame tf = { .vector = 0x21 };
    host_handlers[1](&tf);
    irq_restore(f);
}
static struct fwinput_event fw_events[64];
static unsigned fw_head, fw_count;
static uint64_t fake_fw_loss;
static enum biosvm_backend fake_fw_state = BIOSVM_READY;
static void fw_push(unsigned type, unsigned code, int32_t value)
{
    CHECK(fw_count < ARRAY_SIZE(fw_events));
    fw_events[(fw_head + fw_count++) % ARRAY_SIZE(fw_events)] =
        (struct fwinput_event){type, code, value, g_ticks};
}
int fwinput_init(void) { return 0; }
unsigned fwinput_poll(struct fwinput_event *out, unsigned max)
{
    unsigned n = 0;
    while (n < max && fw_count) {
        out[n++] = fw_events[fw_head];
        fw_head = (fw_head + 1) % ARRAY_SIZE(fw_events);
        fw_count--;
    }
    return n;
}
void fwinput_stats(struct fwinput_stats *out) { *out = (struct fwinput_stats){ .loss = fake_fw_loss }; }
void fwinput_backend_state(struct fwinput_backend_state *out)
{
    *out = (struct fwinput_backend_state){ .keyboard = true, .mouse = true, .key_releases = true,
                                         .disabled = fake_fw_state == BIOSVM_DISABLED_BACKEND };
}
enum biosvm_backend biosvm_backend_state(void) { return fake_fw_state; }
struct task *task_create_kernel(const char *name, void (*fn)(void *), void *arg, enum task_prio prio)
{
    static struct task t;
    CHECK(!strcmp(name, "firmware-queue") && fn && !arg && prio == P_DEVICE);
    t = (struct task){ .state = T_BLOCKED };
    return &t;
}

void task_sleep_ms(uint32_t ms)
{
    CHECK(host_flags & 0x200);
    for (unsigned n = 0; n < ms; n++) {
        hw.now++;
        g_ticks = hw.now;
        if (host_survivor && host_survivor_data && task_alive(host_survivor)) {
            uint32_t *data = host_p2v(host_survivor_data);
            data[0]++;
            data[2] = 0x5EED5EED;
        }
        if (replay_input && replay_cycles < 100) {
            if (g_boot.input_policy == CBI_INPUT_FIRMWARE) {
                fw_push(FWINPUT_KEY, 0x1E, 1);
                fw_push(FWINPUT_TEXT, 0, 'a');
                fw_push(FWINPUT_REL, FWINPUT_X, 2);
                fw_push(FWINPUT_REL, FWINPUT_Y, -1);
                fw_push(FWINPUT_KEY, 0x1E, 0);
                if (replay_cycles % 10 == 0) {
                    fw_push(FWINPUT_BUTTON, 0, 1);
                    fw_push(FWINPUT_BUTTON, 0, 0);
                }
                fwinput_adapter_step();
                replay_cycles++;
                continue;
            }
            replay_byte(false, 0x1C);
            replay_byte(true, 0x08); replay_byte(true, 2); replay_byte(true, 1);
            replay_byte(false, 0xF0); replay_byte(false, 0x1C);
            if (replay_cycles % 10 == 0) {
                replay_byte(true, 0x09); replay_byte(true, 0); replay_byte(true, 0);
                replay_byte(true, 0x08); replay_byte(true, 0); replay_byte(true, 0);
            }
            replay_cycles++;
        }
    }
}
void rec_emit(const char *probe, const char *event, const char *fmt, ...)
{
    char extra[512] = { 0 }, record[768];
    va_list ap;
    va_start(ap, fmt);
    if (fmt) vsnprintf(extra, sizeof(extra), fmt, ap);
    va_end(ap);
    int len = snprintf(record, sizeof(record), "CIUKI_TEST v=1 run=12ab34cd seq=%06u probe=%s event=%s%s%s",
                       ++record_count, probe, event, *extra ? " " : "", extra);
    if ((size_t)len > longest_record) longest_record = (size_t)len;
    CHECK(len <= 240);
    if (len > 240) printf("oversized record: %s\n", record);
    if (print_records) puts(record);
    if (!strcmp(probe, "input") && !strncmp(extra, "case=setup step=", 16)) setup_records++;
    if (strstr(extra, "case=firmware_overrun ") || strstr(extra, "case=disallowed_io ")) {
        firmware_records++;
        if (strstr(extra, "status=not_run reason=firmware_backend_absent")) absent_records++;
        if (strstr(extra, "status=not_run reason=firmware_selftest_qemu_only")) refused_records++;
    }
    if (!strcmp(event, "READY")) record_ready++;
    if (!strcmp(event, "END")) {
        if (!strcmp(extra, "status=PASS")) record_pass++;
        else if (strstr(extra, "status=not_run")) record_deferred++;
        else if (*expected_end && !strcmp(extra, expected_end)) record_fail++;
        else { printf("probe failure: %s\n", record); CHECK(false); }
    }
}
/* Linked symbols stand in for the existing payload blob; fake scheduling
 * above models its progress, while QEMU must execute the real ring-3 code. */
__asm__(".pushsection .rodata\n"
        ".global payload_start\npayload_start:\n.byte 0\n"
        ".global payload_end\npayload_end:\n.popsection\n");
int kwork_init(void) { return 0; }
bool kwork_queue(kwork_fn fn, void *arg) { (void)fn; (void)arg; CHECK(false); return false; }
void kwork_yield(void) { CHECK(false); }

/* sync.c contains RDTSC, never executed: g_cpu_tsc is false in this test.
 * Replace ONLY udelay: privileged native pacing becomes fake PIT time. */
#define udelay sync_udelay
#include "../../src/kernel/core/sync.c"
#undef udelay
int udelay(uint32_t us)
{
    CHECK(us <= 1000 && ((host_flags & 0x200) || us <= 50));
    if (host_flags & 0x200) fake_pause(&hw);
    return 0;
}
#include "../../src/kernel/lib/stackprot.c"
#include "../../src/kernel/core/registry.c"
#include "../../src/kernel/drivers/i8042.c"
#include "../../src/kernel/drivers/fwinput_adapter.c"
#include "../../src/kernel/drivers/i8042_probe.c"

int biosvm_selftest(struct biosvm_selftest_report *out)
{
    CHECK(fake_fw_state == BIOSVM_READY && !fixture && host_survivor);
    firmware_selftests++;
    if (!(g_boot.flags & CBI_F_SMBIOS_QEMU)) return -V86_EPERM;
    *out = (struct biosvm_selftest_report){
        .pic_before = { 0xF9, 0xEF }, .pic_after = { 0xF9, 0xEF },
        .pit_before = 0x34, .pit_after = 0x34,
        .policy_result = 0, .denied_result = -V86_EPERM,
        .timeout_result = -V86_ETIMEDOUT, .later_result = -V86_EIO,
        .disallowed = 1, .timeouts = 1, .disabled = true,
        .pic_unchanged = true, .pit_unchanged = true, .mappings_ok = true,
    };
    return fake_selftest_result;
}

static void reset_native(void)
{
    i8042_fault_end();
    memset(&native, 0, sizeof(native));
    adapter = 0; generation = 0; loss_seen = 0;
    fw_head = fw_count = 0; fake_fw_loss = 0;
    fake_fw_state = BIOSVM_READY;
    memset(&hw, 0, sizeof(hw));
    hw.pause_step = 1;
    hw.config = 0x47;                 /* preserve system flag, disable translation/IRQs */
    hw.selftest_reenable = true;
    hw.loop_reply = 0x5A;
    init_log_count = setup_records = 0;
    log_delay_ms = 0;
    memset(host_handlers, 0, sizeof(host_handlers));
    memset(host_masked, 1, sizeof(host_masked));
    memset(&g_boot, 0, sizeof(g_boot));
    host_flags = 0x200;
    g_ticks = 0;
    pic_changes = 0;
    registry_init();
}
static void select_fault(const char *selector)
{
    memset(g_boot.test_request, 0, sizeof(g_boot.test_request));
    g_boot.flags = CBI_F_TEST_REQUEST;
    g_boot.test_request_len = (uint32_t)strlen(selector);
    memcpy(g_boot.test_request, selector, g_boot.test_request_len);
}
static void begin_fixture(struct fake_bus *b)
{
    memset(b, 0, sizeof(*b));
    b->pause_step = 1;
    select_fault("f1:input-fault run=12ab34cd");
    CHECK(i8042_fault_begin(&fake_io, b) == 0);
}
static void feed(bool aux, uint8_t byte) { CHECK(i8042_fault_capture(aux ? 0x21 : 1, byte) == 0); }
static struct input_digest fixture_digest(void)
{
    struct input_digest d;
    struct input_event e;
    uint64_t seq = 0;
    input_digest_init(&d);
    while (i8042_fault_read(&e)) {
        CHECK(e.sequence > seq && e.generation && e.source == INPUT_NATIVE);
        seq = e.sequence;
        input_digest_add(&d, &e);
    }
    return d;
}

static void test_policy_and_lifecycle(void)
{
    reset_native();
    unsigned count = registry_count(), reads = hw.reads, writes = hw.writes;
    g_boot.input_policy = CBI_INPUT_FIRMWARE;
    CHECK(i8042_init() == -ENOSYS);
    CHECK(hw.reads == reads && hw.writes == writes && !pic_changes && registry_count() == count);
    CHECK(!strcmp(i8042_backend(), "firmware"));
    reset_native();
    g_boot.flags = CBI_F_INPUT_FORCED;
    CHECK(i8042_init() == -ENOSYS && !hw.reads && !hw.writes && !pic_changes);
    reset_native();
    CHECK(registry_claim(RES_IRQ, 12, 13, "other", false) >= 0);
    reads = hw.reads; writes = hw.writes;
    CHECK(i8042_init() == -EINVAL);
    CHECK(hw.reads == reads && hw.writes == writes && !pic_changes);
    for (unsigned i = 0; i < registry_count(); i++)
        CHECK(strcmp(registry_get(i)->owner, "i8042"));

    reset_native();
    unsigned owners = 0;
    CHECK(i8042_init() == 0);
    struct i8042_stats s;
    i8042_snapshot(&s);
    CHECK(s.active && !s.pending_command && !s.quarantined && s.generation);
    CHECK(!(hw.config & 0x70) && (hw.config & 7) == 7 && !hw.resets && hw.selftests == 1);
    CHECK(s.last_elapsed_ms <= I8042_SETUP_MS && !host_masked[1] && !host_masked[12] && !host_masked[2]);
    CHECK(host_handlers[1] && host_handlers[12]);
    for (unsigned i = 0; i < registry_count(); i++) {
        const struct resource *r = registry_get(i);
        if (!strcmp(r->owner, "i8042")) { owners++; CHECK(r->state == RS_ACTIVE && !r->shareable); }
    }
    CHECK(owners == 4);
    reads = hw.reads; writes = hw.writes;
    CHECK(i8042_init() == 0 && hw.reads == reads && hw.writes == writes);
    CHECK(i8042_stop(s.generation + 1) == -EINVAL && hw.reads == reads && hw.writes == writes);
    /* The interrupt's IRQ number is not used to guess the byte's source. */
    fake_push(&hw, false, 0x1C, 0);
    fake_push(&hw, false, 0xF0, 0);
    fake_push(&hw, false, 0x1C, 0);
    struct trap_frame tf = { .vector = 0x2C };
    for (unsigned i = 0; i < 3; i++) {
        unsigned before = hw.count;
        uint32_t f = irq_save();
        host_handlers[12](&tf);
        irq_restore(f);
        CHECK(hw.count + 1 == before);
    }
    struct input_event event;
    CHECK(input_read(&event) && event.type == INPUT_KEY && event.code == INPUT_KEY_A && event.value == 1);
    CHECK(input_read(&event) && event.type == INPUT_TEXT && event.code == 'a');
    CHECK(input_read(&event) && event.value == 0 && !input_read(&event));
    hw.ack_delay = 1;
    hw.irq_reply = true;
    CHECK(device_command(&native, false, 0xF4, hw.now + 100) == 0);
    CHECK(!native.stats.pending_command && !hw.count && !input_read(&event));
    hw.ack_delay = 0;
    hw.irq_reply = false;
    /* A stale generation must prevent both IRQ reads and shutdown writes. */
    res[native.handle[0]].generation++;
    reads = hw.reads; writes = hw.writes;
    host_handlers[1](&tf);
    CHECK(hw.reads == reads && i8042_stop(s.generation) == -EINVAL && hw.writes == writes);
    res[native.handle[0]].generation--;
    CHECK(i8042_stop(s.generation) == 0);
    CHECK((hw.config & 0x33) == 0x30 && host_masked[1] && host_masked[12]);
    CHECK(!host_handlers[1] && !host_handlers[12] && !hw.count && !hw.resets);
    for (unsigned i = 0; i < registry_count(); i++)
        if (!strcmp(registry_get(i)->owner, "i8042")) CHECK(registry_get(i)->state == RS_RELEASED);
    CHECK(i8042_init() == 0);
    i8042_snapshot(&s);
    CHECK(s.generation && i8042_stop(s.generation) == 0);
    printf("i8042 policy/claims/IRQ/quiescence: PASS\n");
}

static void test_native_failures(void)
{
    reset_native();
    hw.missing = true;
    CHECK(i8042_init() == -I8042_ETIMEDOUT);
    struct i8042_stats s;
    i8042_snapshot(&s);
    CHECK(s.quarantined && !s.active && !s.pending_command && s.last_elapsed_ms <= 500);
    unsigned reads = hw.reads, writes = hw.writes;
    CHECK(i8042_init() == -I8042_EIO && hw.reads == reads && hw.writes == writes);
    unsigned owners = 0;
    for (unsigned i = 0; i < registry_count(); i++)
        if (!strcmp(registry_get(i)->owner, "i8042")) { owners++; CHECK(registry_get(i)->state == RS_QUARANTINED); }
    CHECK(owners == 4 && !hw.resets && host_masked[1] && host_masked[12]);
    reset_native();
    hw.ack_delay = 60;                   /* cumulative setup must stop at 500, not 9*100 */
    CHECK(i8042_init() == -I8042_ETIMEDOUT);
    i8042_snapshot(&s);
    CHECK(s.last_elapsed_ms == 500 && s.quarantined && !hw.resets);
    reset_native();
    hw.stuck_ibf = true;
    CHECK(i8042_init() == -I8042_ETIMEDOUT && hw.now == I8042_REPLY_MS && !hw.writes);
    reset_native();
    hw.stuck_obf = true;
    CHECK(i8042_init() == -I8042_ETIMEDOUT && hw.now == I8042_REPLY_MS && !hw.writes);
    reset_native();
    hw.bad_selftest = true;
    CHECK(i8042_init() == -I8042_EIO && !hw.resets && hw.selftests == 1);
    reset_native();
    hw.mouse_id = 3;                    /* F6 does not undo IntelliMouse negotiation */
    CHECK(i8042_init() == -I8042_EPROTO && !hw.resets);
    reset_native();
    CHECK(i8042_init() == 0);
    i8042_snapshot(&s);
    hw.stop_stuck = true;
    CHECK(i8042_stop(s.generation) == -I8042_ETIMEDOUT);
    i8042_snapshot(&s);
    CHECK(s.quarantined && !host_handlers[1] && !host_handlers[12]);
    printf("i8042 native setup/timeout/quarantine: PASS\n");
}

static struct i8042_init_record init_failure(enum i8042_init_step step, int result)
{
    struct i8042_init_record r = { 0 }, next;
    CHECK(native.init_count && native.init_count <= I8042_INIT_STEPS);
    CHECK(init_log_count == native.init_count);
    for (unsigned i = 0; i < native.init_count; i++) {
        CHECK(i8042_init_record(i, &r));
        CHECK(r.result == (i + 1 == native.init_count ? result : 0));
        char line[I8042_INIT_LINE];
        i8042_init_format(line, i, &r);
        CHECK(!strcmp(line, init_logs[i] + strlen("[i8042] ")));
        CHECK(r.elapsed_ms <= I8042_SETUP_MS);
    }
    CHECK(r.step == step && r.result == result);
    CHECK(!i8042_init_record(native.init_count, &next));
    CHECK(!i8042_init_record(0, 0));
    unsigned reads = hw.reads, writes = hw.writes, logs = init_log_count;
    CHECK(i8042_init() == -I8042_EIO);
    CHECK(hw.reads == reads && hw.writes == writes && init_log_count == logs);
    CHECK(native.stats.quarantined && !native.stats.active && !native.accepting);
    CHECK(!host_handlers[1] && !host_handlers[12] && host_masked[1] && host_masked[12]);
    for (unsigned i = 0; i < ARRAY_SIZE(native.handle); i++)
        CHECK(registry_get(native.handle[i])->state == RS_QUARANTINED);
    CHECK(!hw.resets && hw.selftests <= 1);
    return r;
}

static void test_init_diagnostics(void)
{
    reset_native();
    hw.selftest_delay = I8042_REPLY_MS - 1;
    CHECK(i8042_init() == 0 && native.init_count == 15);
    struct i8042_init_record r;
    CHECK(i8042_init_record(3, &r) && r.step == I8042_INIT_SELF_TEST);
    CHECK(r.command == 0xAA && r.bytes == 1 && r.reply == 0x55 && r.elapsed_ms == 199);
    CHECK(hw.config_writes == 5 && hw.selftests == 1 && !hw.resets);
    CHECK(native.stats.last_elapsed_ms < I8042_SETUP_MS && init_log_count == 15);
    for (unsigned i = 0; i < native.init_count; i++) CHECK(!native.init_records[i].result);
    reset_native();
    hw.selftest_delay = I8042_REPLY_MS;
    CHECK(i8042_init() == -I8042_ETIMEDOUT);
    r = init_failure(I8042_INIT_SELF_TEST, -I8042_ETIMEDOUT);
    CHECK(r.command == 0xAA && !r.bytes && r.elapsed_ms == I8042_REPLY_MS);
    reset_native();
    hw.bad_selftest = true;
    CHECK(i8042_init() == -I8042_EIO);
    r = init_failure(I8042_INIT_SELF_TEST, -I8042_EIO);
    CHECK(r.reply == 0xFC && r.first == 0xFC && r.bytes == 1);
    reset_native();
    hw.bad_selftest_status = true;
    CHECK(i8042_init() == -I8042_EIO);
    r = init_failure(I8042_INIT_SELF_TEST, -I8042_EIO);
    CHECK(r.reply == 0x55 && r.bytes == 1 && (r.status_reply & 0x80));
    reset_native();
    hw.sticky_translation = true;
    CHECK(i8042_init() == -I8042_EIO);
    r = init_failure(I8042_INIT_CONFIG_WRITE, -I8042_EIO);
    CHECK(r.command == 0x60 && (r.reply & 0x40) && hw.config_writes == 2);
    reset_native();
    hw.unstable_config = true;
    CHECK(i8042_init() == -I8042_EIO);
    r = init_failure(I8042_INIT_CONFIG_READ, -I8042_EIO);
    CHECK(r.bytes == 10 && r.first != r.reply && hw.config_reads == 10 && !hw.selftests);
    reset_native();
    hw.kbd_test_reply = 0x01;
    CHECK(i8042_init() == 0);
    CHECK(i8042_init_record(5, &r) && r.step == I8042_INIT_IFACE_KBD && !r.result);
    CHECK(r.command == 0xAB && r.reply == 0x01 && r.bytes == 1);
    reset_native();
    hw.aux_test_reply = 0x03;
    CHECK(i8042_init() == 0);          /* internal loopback + real ACK/ID qualify AUX */
    CHECK(i8042_init_record(6, &r) && r.step == I8042_INIT_IFACE_AUX && !r.result);
    CHECK(r.command == 0xA9 && r.first == 0x03 && r.reply == 0x5A && r.bytes == 2);
    reset_native();
    hw.aux_test_reply = 0x03; hw.loop_reply = 0x00;
    CHECK(i8042_init() == -I8042_EIO);
    r = init_failure(I8042_INIT_IFACE_AUX, -I8042_EIO);
    CHECK(r.first == 0x03 && r.reply == 0x00 && r.bytes == 2);
    reset_native();
    hw.aux_test_reply = 0x03; hw.loop_untagged = true;
    CHECK(i8042_init() == -I8042_ETIMEDOUT);
    r = init_failure(I8042_INIT_IFACE_AUX, -I8042_ETIMEDOUT);
    CHECK(r.first == 0x03 && r.reply == 0x5A); /* value alone cannot qualify AUX */
    reset_native();
    hw.aux_test_reply = 0x03; hw.missing_aux = true;
    CHECK(i8042_init() == -I8042_ETIMEDOUT);
    r = init_failure(I8042_INIT_RESET_AUX, -I8042_ETIMEDOUT);
    CHECK(r.command == 0xF5 && !r.bytes);
    reset_native();
    hw.missing_command = 0xF0;
    CHECK(i8042_init() == -I8042_ETIMEDOUT);
    r = init_failure(I8042_INIT_RESET_KBD, -I8042_ETIMEDOUT);
    CHECK(r.command == 0xF0 && !r.bytes); /* earlier F5 ACK is not this reply */
    reset_native();
    hw.ignore_enable_config = true;
    CHECK(i8042_init() == 0 && !(hw.config & 0x70) && (hw.config & 3) == 3);
    reset_native();
    hw.ignore_enable_config = hw.ignore_enable_commands = true;
    CHECK(i8042_init() == -I8042_EIO);
    r = init_failure(I8042_INIT_ENABLE, -I8042_EIO);
    CHECK(r.command == 0x20 && (r.reply & 0x30));
    reset_native();
    hw.drop_irq_config = true;
    CHECK(i8042_init() == -I8042_EIO);
    r = init_failure(I8042_INIT_ENABLE, -I8042_EIO);
    CHECK(r.command == 0x20 && !(r.reply & 3));
    reset_native();
    hw.stuck_obf = true;
    CHECK(i8042_init() == -I8042_ETIMEDOUT);
    r = init_failure(I8042_INIT_FLUSH, -I8042_ETIMEDOUT);
    CHECK((r.status_before & 1) && (r.status_after & 1) && r.bytes && r.reply == 0);
    reset_native();
    fake_push(&hw, false, 0xE0, 0);     /* drain firmware's prefix without decoder state */
    fake_push(&hw, true, 0x08, 0);
    CHECK(i8042_init() == 0 && !hw.count && !native.queue.stats.pending);
    CHECK(!native.queue.extended && !native.queue.mouse_pos);
    reset_native();
    hw.mixed = true;                  /* firmware traffic interleaved with setup ACKs */
    CHECK(i8042_init() == 0 && !native.queue.stats.pending && !native.queue.stats.state_lost);
    CHECK(!native.queue.extended && !native.queue.mouse_pos && !native.queue.stats.keys_down);
    reset_native();
    hw.final_obf = true;
    CHECK(i8042_init() == -I8042_ETIMEDOUT);
    r = init_failure(I8042_INIT_FLUSH, -I8042_ETIMEDOUT);
    CHECK(r.command == 0 && (r.status_after & 1) && r.bytes);
    reset_native();
    hw.write_delay = 1;
    log_delay_ms = 100;
    CHECK(i8042_init() == 0 && native.stats.last_elapsed_ms < I8042_SETUP_MS);
    CHECK(hw.now >= 1500 && init_log_count == 15); /* logging spends no setup budget */
    printf("i8042 init trace/config/self-test/AUX/enable/drain/deadline replay: PASS\n");
}

static void t23_bus(void)
{
    hw.t23 = true;
    hw.config = 0x77;
    hw.status_base = 0x1C;
    hw.kbd_test_reply = 0xFA;
    hw.iface_delay = 1;
}

static void test_advisory_interfaces_and_controller_acks(void)
{
    reset_native();
    t23_bus();
    CHECK(i8042_init() == 0 && native.stats.active && !native.stats.quarantined);
    CHECK(native.stats.initial_config == 0x77 && hw.last_config == 0x07);
    CHECK(native.init_count == 15 && hw.sends == 10 && !hw.resets);
    const uint8_t replies[] = { 0x00, 0x01, 0x03, 0x55, 0xFA, 0xFF };
    for (unsigned i = 0; i < ARRAY_SIZE(replies); i++) {
        reset_native();
        hw.kbd_test_reply = hw.aux_test_reply = replies[i];
        CHECK(i8042_init() == 0 && native.stats.active);
        struct i8042_init_record r;
        CHECK(i8042_init_record(5, &r) && !r.result && r.reply == replies[i] && r.bytes == 1);
        CHECK(i8042_init_record(6, &r) && !r.result && r.first == replies[i]);
        CHECK(r.bytes == (replies[i] ? 2u : 1u));
    }
    reset_native();
    hw.silent_kbd_test = hw.silent_aux_test = true;
    CHECK(i8042_init() == 0 && native.stats.last_elapsed_ms < I8042_SETUP_MS);
    struct i8042_init_record r;
    CHECK(i8042_init_record(5, &r) && !r.result && !r.bytes && r.elapsed_ms == I8042_REPLY_MS);
    CHECK(i8042_init_record(6, &r) && !r.result && r.reply == 0x5A && r.bytes == 1);
    CHECK(r.elapsed_ms >= I8042_REPLY_MS && hw.sends == 10);
    reset_native();
    hw.silent_kbd_test = hw.silent_aux_test = true;
    hw.missing_command = 0xF5;
    CHECK(i8042_init() == -I8042_ETIMEDOUT && native.stats.last_elapsed_ms == I8042_SETUP_MS);
    r = init_failure(I8042_INIT_RESET_KBD, -I8042_ETIMEDOUT);
    CHECK(r.command == 0xF5 && !r.bytes);
    /* A late interface byte remains controller traffic, including at the
     * diagnostic deadline; it cannot acknowledge the later F5. */
    reset_native();
    hw.iface_delay = I8042_REPLY_MS;
    hw.kbd_test_reply = hw.aux_test_reply = 0xFA;
    hw.missing_command = 0xF5;
    CHECK(i8042_init() == -I8042_ETIMEDOUT);
    r = init_failure(I8042_INIT_RESET_KBD, -I8042_ETIMEDOUT);
    CHECK(r.command == 0xF5 && !r.bytes);
    const uint8_t commands[] = { 0xAD, 0xA7, 0x60, 0xAE, 0xA8, 0xD3, 0xD4 };
    for (unsigned i = 0; i < ARRAY_SIZE(commands); i++) {
        reset_native();
        t23_bus();
        hw.aux_test_reply = 0xFA;     /* exercise D3 qualification too */
        hw.controller_ack_command = commands[i];
        hw.controller_ack_count = 1;
        hw.controller_ack_delay = 1;
        hw.config_ack = commands[i] == 0x60;
        CHECK(i8042_init() == 0 && !hw.count && !native.queue.stats.pending);
        unsigned stray = 0;
        for (unsigned j = 0; j < native.init_count; j++) stray += native.init_records[j].stray_ack;
        CHECK(stray && !native.queue.stats.errors && !native.queue.stats.state_lost);
        CHECK(hw.sends == 10 && !hw.resets);
        reset_native();
        t23_bus();
        hw.aux_test_reply = 0xFA;
        hw.controller_ack_command = commands[i];
        hw.controller_ack_count = 1;
        hw.controller_ack_delay = 1;
        hw.config_ack = commands[i] == 0x60;
        hw.missing_command = 0xF5;
        CHECK(i8042_init() == -I8042_ETIMEDOUT);
        r = init_failure(I8042_INIT_RESET_KBD, -I8042_ETIMEDOUT);
        CHECK(r.command == 0xF5 && !r.bytes);
    }
    reset_native();
    hw.controller_ack_command = 0xD4;
    hw.controller_ack_count = 1;
    hw.controller_ack_delay = hw.write_delay = 5;
    hw.missing_aux = true;
    CHECK(i8042_init() == -I8042_ETIMEDOUT);
    r = init_failure(I8042_INIT_RESET_AUX, -I8042_ETIMEDOUT);
    CHECK(r.command == 0xF5 && r.stray_ack && r.bytes == 1 && r.reply == 0xFA);
    reset_native();
    hw.controller_ack_command = 0x60;
    hw.controller_ack_count = 2;       /* only one stray per controller write */
    CHECK(i8042_init() == -I8042_EPROTO);
    r = init_failure(I8042_INIT_CONFIG_WRITE, -I8042_EPROTO);
    CHECK(r.stray_ack && r.bytes == 2 && !hw.sends);
    printf("i8042 T23/advisory interfaces/single stray ACK/device ACK isolation: PASS\n");
}

static void input_record_replay(const char *mode)
{
    reset_native();
    g_boot.flags = CBI_F_SMBIOS_QEMU;
    int result = 0;
    if (!strcmp(mode, "input-translation")) { hw.sticky_translation = true; result = -I8042_EIO; }
    else if (!strcmp(mode, "input-selftest-delayed")) hw.selftest_delay = 199;
    else if (!strcmp(mode, "input-selftest-late")) { hw.selftest_delay = 200; result = -I8042_ETIMEDOUT; }
    else if (!strcmp(mode, "input-aux-quirk")) hw.aux_test_reply = 3;
    else if (!strcmp(mode, "input-t23")) t23_bus();
    else if (!strcmp(mode, "input-t23-stray")) {
        t23_bus(); hw.controller_ack_command = 0x60; hw.controller_ack_count = 1;
        hw.controller_ack_delay = 1; hw.config_ack = true;
    } else if (!strcmp(mode, "input-iface-silent")) hw.silent_kbd_test = hw.silent_aux_test = true;
    else if (!strcmp(mode, "input-t23-no-device-ack")) {
        t23_bus(); hw.missing_command = 0xF5; result = -I8042_ETIMEDOUT;
    } else if (!strcmp(mode, "input-t23-stray-no-device-ack")) {
        t23_bus(); hw.controller_ack_command = 0x60; hw.controller_ack_count = 1;
        hw.controller_ack_delay = 1; hw.config_ack = true;
        hw.missing_command = 0xF5; result = -I8042_ETIMEDOUT;
    }
    else if (!strcmp(mode, "input-aux-badloop")) {
        hw.aux_test_reply = 3; hw.loop_reply = 0; result = -I8042_EIO;
    } else if (!strcmp(mode, "input-aux-missing")) {
        hw.aux_test_reply = 3; hw.missing_aux = true; result = -I8042_ETIMEDOUT;
    } else if (!strcmp(mode, "input-stuck-obf")) { hw.stuck_obf = true; result = -I8042_ETIMEDOUT; }
    else if (!strcmp(mode, "input-final-obf")) { hw.final_obf = true; result = -I8042_ETIMEDOUT; }
    else if (!strcmp(mode, "input-enable-failed")) {
        hw.ignore_enable_config = hw.ignore_enable_commands = true; result = -I8042_EIO;
    } else CHECK(!strcmp(mode, "input-records"));
    /* Emulate boot activation, then probe replay from active/quarantined state. */
    CHECK(i8042_init() == result);
    if (print_records) printf("[init] input result=%s error=%d\n", result ? "failed" : "ready", result);
    unsigned reads = hw.reads, writes = hw.writes;
    if (result) strcpy(expected_end, "status=FAIL reason=setup");
    replay_input = !result;
    replay_cycles = 0;
    CHECK(probe_input() == !!result);
    CHECK(setup_records == native.init_count);
    if (!result) CHECK(native.stats.active && record_ready && record_pass && !record_fail);
    if (result) CHECK(hw.reads == reads && hw.writes == writes && !record_ready);
    expected_end[0] = 0;
    replay_input = false;
}

static void test_commands(void)
{
    struct fake_bus b;
    begin_fixture(&b);
    CHECK(i8042_fault_command(false, 0xF4) == 0 && b.sends == 1);
    CHECK(i8042_fault_command(true, 0xF4) == 0 && b.sends == 2);
    CHECK(!b.resets);
    i8042_fault_end();
    begin_fixture(&b);
    b.resend_count = 2;
    CHECK(i8042_fault_command(true, 0xF4) == 0 && b.sends == 3);
    struct i8042_stats s;
    i8042_fault_snapshot(&s, 0);
    CHECK(s.resends == 2 && !s.quarantined && s.last_elapsed_ms <= I8042_REPLY_MS);
    i8042_fault_end();
    begin_fixture(&b);
    b.resend_count = 100;
    CHECK(i8042_fault_command(false, 0xF4) == -I8042_EPROTO && b.sends == 3);
    i8042_fault_snapshot(&s, 0);
    CHECK(s.resends == 2 && s.quarantined && !s.pending_command);
    unsigned writes = b.writes, reads = b.reads;
    CHECK(i8042_fault_command(false, 0xF4) == -I8042_EIO && b.writes == writes && b.reads == reads);
    i8042_fault_end();
    begin_fixture(&b);
    b.resend_count = 2; b.ack_delay = 75;
    CHECK(i8042_fault_command(false, 0xF4) == -I8042_ETIMEDOUT && b.now == I8042_REPLY_MS);
    i8042_fault_snapshot(&s, 0);
    CHECK(s.resends == 2 && s.last_elapsed_ms == I8042_REPLY_MS && s.quarantined);
    i8042_fault_end();
    begin_fixture(&b);
    b.missing = true;
    CHECK(i8042_fault_command(false, 0xF4) == -I8042_ETIMEDOUT && b.now == I8042_REPLY_MS);
    i8042_fault_end();
    begin_fixture(&b);
    b.ack_delay = I8042_REPLY_MS;        /* arriving at deadline is too late */
    CHECK(i8042_fault_command(false, 0xF4) == -I8042_ETIMEDOUT);
    i8042_fault_end();
    begin_fixture(&b);
    b.ack_delay = I8042_REPLY_MS - 1;
    CHECK(i8042_fault_command(false, 0xF4) == 0);
    i8042_fault_end();
    begin_fixture(&b);
    b.wrong_aux = true;
    CHECK(i8042_fault_command(false, 0xF4) == -I8042_ETIMEDOUT);
    i8042_fault_end();
    begin_fixture(&b);
    b.bad_ack = true;
    CHECK(i8042_fault_command(false, 0xF4) == -I8042_EIO);
    struct input_stats queue;
    i8042_fault_snapshot(&s, &queue);
    CHECK(s.quarantined && !s.pending_command && queue.errors == 1 && queue.state_lost);
    i8042_fault_end();
    begin_fixture(&b);
    b.stalled = b.missing = true;
    unsigned yields = host_yields;
    CHECK(i8042_fault_command(false, 0xF4) == -EFAULT);
    i8042_fault_snapshot(&s, 0);
    CHECK(s.stalled == 1 && s.last_elapsed_ms == 0 && s.quarantined && host_yields > yields);
    i8042_fault_end();
    begin_fixture(&b);
    b.mixed = true;
    CHECK(i8042_fault_command(false, 0xF4) == 0);
    struct input_digest d = fixture_digest();
    CHECK(d.characters == 1 && d.key_transitions == 2 && d.x == 2 && d.y == -1 && d.events == 5);
    i8042_fault_end();
    begin_fixture(&b);
    b.mixed = true;
    CHECK(i8042_fault_command(true, 0xF4) == 0);
    d = fixture_digest();
    CHECK(d.characters == 1 && d.key_transitions == 2 && d.x == 2 && d.y == -1 && d.events == 5);
    i8042_fault_end();
    begin_fixture(&b);
    writes = b.writes;
    CHECK(i8042_fault_command(false, 0xFF) == -EINVAL && b.writes == writes);
    i8042_fault_end();
    /* Modulo deadline arithmetic survives wrapping ticks. */
    begin_fixture(&b);
    b.now = UINT64_MAX - 20; b.missing = true;
    CHECK(i8042_fault_command(false, 0xF4) == -I8042_ETIMEDOUT);
    i8042_fault_snapshot(&s, 0);
    CHECK(s.last_elapsed_ms == I8042_REPLY_MS);
    i8042_fault_end();
    printf("i8042 ACK/RESEND/deadlines/stalled-clock/mixed bytes: PASS\n");
}

static void test_keyboard_and_mouse(void)
{
    struct fake_bus b;
    begin_fixture(&b);
    feed(false, 0xE0); feed(false, 0x75); /* up arrow */
    struct input_stats q;
    i8042_fault_snapshot(0, &q);
    CHECK(q.keys_down == 1);
    feed(false, 0xE0); feed(false, 0xF0); feed(false, 0x75);
    struct input_event e;
    CHECK(i8042_fault_read(&e) && e.code == 0x148 && e.value == 1);
    CHECK(i8042_fault_read(&e) && e.code == 0x148 && e.value == 0);
    feed(false, 0x1C); feed(false, 0x1C); /* repeat KEY and TEXT, only one down */
    feed(false, 0xF0); feed(false, 0x1C); feed(false, 0xF0); feed(false, 0x1C);
    struct input_digest d;
    input_digest_init(&d);
    const unsigned types[] = { INPUT_KEY, INPUT_TEXT, INPUT_KEY, INPUT_TEXT, INPUT_KEY };
    const int values[] = { 1, 0, 2, 0, 0 };
    for (unsigned i = 0; i < ARRAY_SIZE(types); i++) {
        CHECK(i8042_fault_read(&e) && e.type == types[i] && e.value == values[i]);
        CHECK(e.code == (types[i] == INPUT_KEY ? INPUT_KEY_A : 'a') && !e.flags && !e.lost_count);
        input_digest_add(&d, &e);
    }
    CHECK(!i8042_fault_read(&e) && d.characters == 2 && d.key_transitions == 2);
    i8042_fault_snapshot(0, &q);
    CHECK(q.repeats == 1 && q.duplicates == 1 && !q.keys_down);
    const uint8_t pause_bytes[] = { 0xE1, 0x14, 0x77, 0xE1, 0xF0, 0x14, 0xF0, 0x77 };
    for (unsigned i = 0; i < ARRAY_SIZE(pause_bytes); i++) feed(false, pause_bytes[i]);
    CHECK(i8042_fault_read(&e) && e.code == INPUT_KEY_PAUSE && e.value == 1);
    CHECK(i8042_fault_read(&e) && e.code == INPUT_KEY_PAUSE && e.value == 0);
    const uint8_t print_bytes[] = { 0xE0, 0x12, 0xE0, 0x7C, 0xE0, 0xF0, 0x7C, 0xE0, 0xF0, 0x12 };
    for (unsigned i = 0; i < ARRAY_SIZE(print_bytes); i++) feed(false, print_bytes[i]);
    d = fixture_digest();
    CHECK(d.key_transitions == 2);
    CHECK(input_unshifted(0x0B) == '0' && input_unshifted(0x02) == '1' && !input_unshifted(0x148));
    /* Invalid headers, overflow header, then valid negative 9-bit x/y. */
    feed(true, 0); feed(true, 0xC8); feed(true, 0);
    feed(true, 0x38); feed(true, 0xFE); feed(true, 0xFF);
    d = fixture_digest();
    CHECK(d.x == -2 && d.y == 1 && !d.button_transitions);
    i8042_fault_snapshot(0, &q);
    CHECK(q.resync == 3);
    feed(true, 0x0F); feed(true, 200); feed(true, 200); /* positive >127 */
    feed(true, 0x08); feed(true, 0); feed(true, 0);
    d = fixture_digest();
    CHECK(d.x == 200 && d.y == -200 && d.button_transitions == 6);
    feed(false, 0x1C);
    i8042_fault_snapshot(0, &q);
    CHECK(q.keys_down == 1);             /* stuck-key evidence is independent of drain */
    fixture_digest();
    i8042_fault_snapshot(0, &q);
    CHECK(q.keys_down == 1);
    CHECK(i8042_fault_capture(0xC1, 0xF0) == 0);
    i8042_fault_snapshot(0, &q);
    CHECK(q.errors == 1 && q.state_lost);
    i8042_fault_end();
    printf("i8042 set-2/E0/F0/Pause/PrintScreen/stuck keys/mouse resync: PASS\n");
}

static void test_queue_and_stimulus(void)
{
    struct fake_bus b;
    begin_fixture(&b);
    for (unsigned i = 0; i < 130; i++) { feed(false, 0x1C); feed(false, 0xF0); feed(false, 0x1C); }
    struct input_stats q;
    i8042_fault_snapshot(0, &q);
    CHECK(q.pending == 135 && q.overflow == INPUT_CAPACITY && q.resync == 1 && q.state_lost && !q.keys_down);
    struct input_event e;
    CHECK(i8042_fault_read(&e) && e.type == INPUT_RESYNC && e.sequence == 257 &&
          e.lost_count == INPUT_CAPACITY && !e.value && e.flags == INPUT_F_RESYNC);
    for (unsigned i = INPUT_CAPACITY; i < 130 * 3; i++) {
        CHECK(i8042_fault_read(&e));
        CHECK(e.sequence == i + 2 && !e.flags && !e.lost_count);
        CHECK(e.type == (i % 3 == 1 ? INPUT_TEXT : INPUT_KEY) && e.value == (i % 3 == 0));
    }
    CHECK(!i8042_fault_read(&e));
    feed(false, 0x1C);
    CHECK(i8042_fault_read(&e) && e.sequence == 392 && e.type == INPUT_KEY && e.value == 1 && !e.flags);
    CHECK(i8042_fault_read(&e) && e.type == INPUT_TEXT && e.sequence == 393 && !e.flags);
    i8042_fault_end();

    /* Overflow on motion snapshots all buttons from the triggering packet,
     * even if a newer packet releases them before the consumer runs. */
    begin_fixture(&b);
    b.now = 77;
    for (unsigned i = 0; i < INPUT_CAPACITY / 2; i++) {
        feed(false, 0xE0); feed(false, 0x75);
        feed(false, 0xE0); feed(false, 0xF0); feed(false, 0x75);
    }
    i8042_fault_snapshot(0, &q);
    CHECK(q.pending == INPUT_CAPACITY && !q.overflow);
    feed(true, 0x0F); feed(true, 2); feed(true, 1);
    feed(true, 0x08); feed(true, 0); feed(true, 0);
    i8042_fault_snapshot(0, &q);
    CHECK(q.pending == 9 && q.overflow == INPUT_CAPACITY && q.resync == 1 && !q.buttons);
    CHECK(i8042_fault_read(&e) && e.type == INPUT_RESYNC && e.value == 7 && e.lost_count == INPUT_CAPACITY);
    CHECK(e.sequence == 257 && e.tick == 77 && e.source == INPUT_NATIVE && e.generation == fixture->stats.generation);
    unsigned fresh = 0;
    while (i8042_fault_read(&e)) { CHECK(!e.flags && !e.lost_count && e.sequence == 258 + fresh); fresh++; }
    CHECK(fresh == 8);
    /* Two full stale batches are counted cumulatively; the second marker
     * replaces the first and no fresh triggering transition is dropped. */
    for (unsigned i = 0; i < 260; i++) {
        feed(false, 0xE0); feed(false, 0x75);
        feed(false, 0xE0); feed(false, 0xF0); feed(false, 0x75);
    }
    i8042_fault_snapshot(0, &q);
    CHECK(q.overflow == 3 * INPUT_CAPACITY && q.resync == 3 && q.pending == 10 && !q.keys_down);
    CHECK(i8042_fault_read(&e) && e.type == INPUT_RESYNC && e.lost_count == 3 * INPUT_CAPACITY && !e.value);
    uint64_t seq = e.sequence;
    fresh = 0;
    while (i8042_fault_read(&e)) { CHECK(!e.flags && e.type == INPUT_KEY && e.sequence == ++seq); fresh++; }
    CHECK(fresh == 9);
    i8042_fault_end();

    begin_fixture(&b);
    struct input_digest d;
    input_digest_init(&d);
    unsigned changes = 0;
    for (unsigned i = 0; i < 100; i++) {
        b.now++;
        feed(false, 0x1C);
        feed(true, 0x08); feed(true, 2); feed(true, 1);
        feed(false, 0xF0); feed(false, 0x1C);
        if (i % 10 == 0) {
            feed(true, 0x09); feed(true, 0); feed(true, 0);
            feed(true, 0x08); feed(true, 0); feed(true, 0);
            changes += 2;
        }
        while (i8042_fault_read(&e)) {
            CHECK(e.tick == b.now && !e.flags);
            input_digest_add(&d, &e);
        }
    }
    i8042_fault_snapshot(0, &q);
    CHECK(d.characters == 100 && d.key_transitions == 200 && d.button_transitions == 20);
    CHECK(changes == 20 && d.x == 200 && d.y == -100 && d.events == 520);
    CHECK(!q.overflow && !q.resync && !q.duplicates && !q.repeats && !q.keys_down && !q.buttons && !q.pending);
    printf("i8042 stimulus: characters=%llu keys=%llu buttons=%llu x=%lld y=%lld overflow=%llu resync=%llu digest=%08x text_digest=%08x PASS\n",
           (unsigned long long)d.characters, (unsigned long long)d.key_transitions,
           (unsigned long long)d.button_transitions, (long long)d.x, (long long)d.y,
           (unsigned long long)q.overflow, (unsigned long long)q.resync, d.hash, d.text_hash);
    i8042_fault_end();
}

static void test_hook_selection(void)
{
    struct fake_bus b = { .pause_step = 1 };
    g_boot.flags = 0;
    CHECK(i8042_fault_begin(&fake_io, &b) == -ENOSYS);
    CHECK(i8042_fault_command(false, 0xF4) == -ENOSYS && i8042_fault_capture(1, 0x1C) == -ENOSYS);
    static const char *bad[] = {
        "f1:input run=12ab34cd", "f1:input-fault run=1234567", "f1:input-fault run=1234567g",
        "f1:input-fault run=12ab34cd safe=1 safe=1", "f1:input-fault run=12ab34cd unknown=1",
        "f1:input-fault run=12ab34cd platform=e500",
    };
    for (unsigned i = 0; i < ARRAY_SIZE(bad); i++) {
        select_fault(bad[i]);
        CHECK(i8042_fault_begin(&fake_io, &b) == -ENOSYS);
    }
    select_fault("f1:input-fault run=12ab34cd safe=1");
    host_flags = 0;
    CHECK(i8042_fault_begin(&fake_io, &b) == -EINVAL && strstr(last_log, "reason=interrupts_disabled"));
    host_flags = 0x200;
    g_current = 0;
    CHECK(i8042_fault_begin(&fake_io, &b) == -EINVAL && strstr(last_log, "reason=no_task"));
    g_current = &host_task;
    CHECK(i8042_fault_begin(0, &b) == -EINVAL && strstr(last_log, "reason=callbacks"));
    for (unsigned i = 0; i < 4; i++) {
        struct i8042_test_io io = fake_io;
        if (i == 0) io.read = 0;
        if (i == 1) io.write = 0;
        if (i == 2) io.now = 0;
        if (i == 3) io.pause = 0;
        CHECK(i8042_fault_begin(&io, &b) == -EINVAL && strstr(last_log, "reason=callbacks"));
    }
    gen_t saved_gen = last_gen;
    last_gen = UINT32_MAX;
    CHECK(i8042_fault_begin(&fake_io, &b) == -ENOSPC && !fixture && strstr(last_log, "reason=generation"));
    last_gen = saved_gen;
    CHECK(i8042_fault_begin(&fake_io, &b) == 0);
    gen_t first = fixture->stats.generation;
    CHECK(i8042_fault_begin(&fake_io, &b) == -EINVAL && strstr(last_log, "reason=fixture_present"));
    CHECK(fixture->stats.generation == first);
    i8042_fault_end();
    CHECK(!fixture_storage.io && !fixture_storage.io_arg && !fixture_storage.stats.generation);
    select_fault("f1:input-fault run=12ab34cd platform=e500 safe=1");
    g_boot.flags |= CBI_F_INPUT_FORCED;
    CHECK(i8042_fault_begin(&fake_io, &b) == 0);
    CHECK(fixture->stats.generation > first && !fixture->queue.stats.pending);
    i8042_fault_end();
    select_fault("f1:input-fault run=00000002");
    g_boot.test_request_len++; /* a counted NUL is invalid, as in selector.c */
    CHECK(i8042_fault_begin(&fake_io, &b) == -ENOSYS && strstr(last_log, "reason=selector") &&
          strstr(last_log, "selector='f1:input-fault run=00000002'") && strstr(last_log, "length=28"));
    memset(g_boot.test_request, 'X', sizeof(g_boot.test_request));
    g_boot.test_request_len = CIUKI_TEST_REQ_MAX + 1;
    CHECK(i8042_fault_begin(&fake_io, &b) == -ENOSYS && strstr(last_log, "length=65"));
    CHECK(!b.writes && !b.reads);
    printf("i8042 runtime fault-hook selection: PASS\n");
}

static void test_probe_records(void)
{
    reset_native();
    replay_cycles = 0;
    replay_input = true;
    CHECK(probe_input() == 0);
    replay_input = false;
    CHECK(replay_cycles == 100 && record_ready == 1 && record_pass == 1 && !record_deferred);
    CHECK(native.stats.active && !native.stats.quarantined);
    reset_native();
    g_boot.input_policy = CBI_INPUT_FIRMWARE;
    replay_input = true;
    replay_cycles = 0;
    CHECK(probe_input() == 0 && !record_deferred && record_pass == 2);
    replay_input = false;
    CHECK(!hw.reads && !hw.writes && !pic_changes);
    reset_native();
    select_fault("f1:input-fault run=12ab34cd");
    struct registry_stats before, after;
    registry_snapshot(&before);
    CHECK(probe_input_fault() == 0);
    registry_snapshot(&after);
    CHECK(record_pass == 3 && !host_survivor && !fixture && !hw.reads && !hw.writes && !pic_changes);
    CHECK(before.claims == after.claims && before.live == after.live && before.quarantines == after.quarantines);
    for (unsigned i = 0; i < ARRAY_SIZE(host_pages); i++) CHECK(!host_page_used[i]);
    printf("i8042 probe orchestration/records: PASS (%u records, maximum %zu bytes, native PASS/firmware PASS/fault PASS)\n",
           record_count, longest_record);
}

static void test_native_fault_probe(void)
{
    reset_native();
    /* menu.inc selector_accept copies the ASCII bytes; its length excludes
     * the terminator. drivers_init activates native input before dispatch. */
    static const char selector[] = "f1:input-fault run=00000002";
    select_fault(selector);
    CHECK(g_boot.test_request_len == sizeof(selector) - 1);
    CHECK(i8042_init() == 0 && native.stats.active && claims_active(&native));
    struct controller saved = native;
    struct registry_stats before, after;
    registry_snapshot(&before);
    unsigned reads = hw.reads, writes = hw.writes, pic = pic_changes, passes = record_pass;
    /* core/kheap.c: largest class is 2048 bytes, including an 8-byte header.
     * An unrestricted libc calloc hid the guest's oversized fixture request. */
    CHECK(sizeof(struct controller) > 2048 - 8 && !kzalloc(sizeof(struct controller)));
    unsigned allocations = host_allocations;
    fake_fw_state = BIOSVM_OFF;
    unsigned calls = firmware_selftests, absent = absent_records;
    CHECK(probe_input_fault() == 0);
    CHECK(firmware_selftests == calls && absent_records == absent + 2);
    CHECK(host_allocations == allocations);
    registry_snapshot(&after);
    CHECK(record_pass == passes + 1 && !fixture && !host_survivor);
    CHECK(!memcmp(&native, &saved, sizeof(native)) && claims_active(&native));
    CHECK(hw.reads == reads && hw.writes == writes && pic_changes == pic);
    CHECK(before.claims == after.claims && before.live == after.live && before.quarantines == after.quarantines);
    for (unsigned i = 0; i < ARRAY_SIZE(host_pages); i++) CHECK(!host_page_used[i]);
    printf("i8042 native-active fault probe with kernel heap limit: %s (controller=%zu heap_max=2040)\n",
           record_pass == passes + 1 ? "PASS" : "FAIL", sizeof(struct controller));
}

static void test_firmware_fault_probe(bool absent, bool failed, bool refused)
{
    reset_native();
    select_fault("f1:input-fault run=12ab34cd platform=e500");
    g_boot.flags |= CBI_F_INPUT_FORCED;
    if (!refused) g_boot.flags |= CBI_F_SMBIOS_QEMU;
    g_boot.input_policy = CBI_INPUT_FIRMWARE;
    fake_fw_state = absent ? BIOSVM_OFF : BIOSVM_READY;
    fake_selftest_result = failed ? -EFAULT : 0;
    unsigned calls = firmware_selftests, records = firmware_records, skipped = absent_records;
    unsigned refused_before = refused_records;
    struct registry_stats before, after;
    registry_snapshot(&before);
    if (failed) snprintf(expected_end, sizeof(expected_end), "status=FAIL reason=fault_or_survivor");
    CHECK(probe_input_fault() == (failed ? 1 : 0));
    expected_end[0] = 0;
    fake_selftest_result = 0;
    registry_snapshot(&after);
    CHECK(firmware_selftests == calls + !absent);
    CHECK(firmware_records == records + (absent ? 2 : refused ? 4 : 5));
    CHECK(absent_records == skipped + (absent ? 2 : 0));
    CHECK(refused_records == refused_before + (refused ? 2 : 0));
    CHECK(!fixture && !host_survivor && !hw.reads && !hw.writes && !pic_changes);
    CHECK(before.claims == after.claims && before.live == after.live && before.quarantines == after.quarantines);
    for (unsigned i = 0; i < ARRAY_SIZE(host_pages); i++) CHECK(!host_page_used[i]);
    printf("i8042 firmware fault records: PASS (backend=%s selftest=%s, survivor and lease cleanup)\n",
           absent ? "absent" : refused ? "selftest-refused" : "ready", failed ? "failed" : "passed");
}

static void test_fault_probe_refusals(void)
{
    reset_native();
    select_fault("f1:input-fault run=0bd02930");
    CHECK(i8042_init() == 0);
    struct controller saved = native;
    unsigned reads = hw.reads, writes = hw.writes, pic = pic_changes;
    for (unsigned i = 0; i < 3; i++) {
        struct fake_bus b = { .pause_step = 1 };
        gen_t saved_gen = last_gen;
        int err;
        if (i == 0) {
            g_boot.flags &= ~CBI_F_TEST_REQUEST;
            err = -ENOSYS;
        } else if (i == 1) {
            g_boot.flags |= CBI_F_TEST_REQUEST;
            CHECK(i8042_fault_begin(&fake_io, &b) == 0);
            err = -EINVAL;
        } else {
            last_gen = UINT32_MAX;
            err = -ENOSPC;
        }
        snprintf(expected_end, sizeof(expected_end), "status=FAIL reason=fault_begin:%d", err);
        unsigned failed = record_fail;
        CHECK(probe_input_fault() == 1 && record_fail == failed + 1);
        expected_end[0] = 0;
        CHECK(!host_survivor);
        for (unsigned j = 0; j < ARRAY_SIZE(host_pages); j++) CHECK(!host_page_used[j]);
        /* Refusing a second begin must leave the first fixture intact. */
        if (i == 1) CHECK(fixture && fixture->io_arg == &b && fixture->stats.active);
        else CHECK(!fixture);
        i8042_fault_end();
        if (i == 2) last_gen = saved_gen;
    }
    CHECK(!memcmp(&native, &saved, sizeof(native)) && claims_active(&native));
    CHECK(hw.reads == reads && hw.writes == writes && pic_changes == pic);
    printf("i8042 fault refusal diagnostics/END errno/survivor cleanup: PASS\n");
}

static void test_firmware_mapping(void)
{
    reset_native();
    CHECK(fwinput_adapter_init() == -ENOSYS && !hw.reads && !hw.writes);
    g_boot.input_policy = CBI_INPUT_FIRMWARE;
    CHECK(!fwinput_adapter_init());
    gen_t saved = fwinput_adapter_generation();
    CHECK(saved && !fwinput_adapter_init() && saved == fwinput_adapter_generation());
    struct input_event e;
    fw_push(FWINPUT_KEY, 0x1E, 1);
    fw_push(FWINPUT_TEXT, 0, 'a');
    fw_push(FWINPUT_KEY, 0x1E, 0);
    fw_push(FWINPUT_KEY, 0x148, 1);
    fw_push(FWINPUT_KEY, 0x148, 0);
    fw_push(FWINPUT_KEY, 0x145, 1);
    fw_push(FWINPUT_KEY, 0x145, 0);
    fw_push(FWINPUT_REL, FWINPUT_X, 2);
    fw_push(FWINPUT_REL, FWINPUT_Y, -1);
    fw_push(FWINPUT_BUTTON, INPUT_LEFT, 1);
    fw_push(FWINPUT_BUTTON, INPUT_LEFT, 0);
    CHECK(fwinput_adapter_step() == 11);
    struct input_digest d;
    input_digest_init(&d);
    unsigned n = 0;
    while (input_read(&e)) {
        CHECK(e.sequence == ++n && e.source == INPUT_FIRMWARE && e.generation == saved && !e.flags);
        if (n == 6 || n == 7) CHECK(e.code == INPUT_KEY_PAUSE);
        input_digest_add(&d, &e);
    }
    CHECK(d.characters == 1 && d.key_transitions == 6 && d.button_transitions == 2 && d.x == 2 && d.y == -1);
    struct input_stats q;
    input_snapshot(&q);
    CHECK(!q.keys_down && !q.buttons && !q.errors && !q.overflow && !q.pending);
    struct fwinput_event stale = { .type = FWINPUT_KEY, .code = INPUT_KEY_A, .value = 1 };
    input_firmware_event(&stale, saved + 1);
    CHECK(!input_read(&e));
    fw_push(FWINPUT_KEY, INPUT_KEY_A, 1);
    fw_push(FWINPUT_KEY, INPUT_KEY_A, 1);
    fw_push(FWINPUT_TEXT, 0, 'a');
    fw_push(FWINPUT_KEY, INPUT_KEY_A, 0);
    CHECK(fwinput_adapter_step() == 4);
    CHECK(input_read(&e) && e.type == INPUT_KEY && e.value == 1);
    CHECK(input_read(&e) && e.type == INPUT_KEY && e.value == 2);
    CHECK(input_read(&e) && e.type == INPUT_TEXT && e.code == 'a');
    CHECK(input_read(&e) && e.type == INPUT_KEY && !e.value && !input_read(&e));
    input_snapshot(&q); CHECK(q.repeats == 1 && !q.keys_down);
    fake_fw_loss = 7;
    fw_push(FWINPUT_RESYNC, 0, 0);
    CHECK(fwinput_adapter_step() == 1 && input_read(&e) && e.type == INPUT_RESYNC &&
          e.flags == INPUT_F_RESYNC && e.lost_count == 7);
    input_snapshot(&q);
    CHECK(q.overflow == 7 && q.resync == 1 && q.state_lost);
    fw_push(FWINPUT_KEY, INPUT_KEY_A, 1);
    fwinput_adapter_step();
    CHECK(input_read(&e) && e.type == INPUT_KEY && !e.flags && !e.lost_count);
    for (unsigned i = 0; i < 130; i++) {
        struct fwinput_event press = { .type = FWINPUT_KEY, .code = 0x148, .value = 1 };
        input_firmware_event(&press, saved);
        press.value = 0;
        input_firmware_event(&press, saved);
    }
    input_snapshot(&q);
    CHECK(q.pending == 5 && q.overflow == 7 + INPUT_CAPACITY && q.resync == 2);
    CHECK(input_read(&e) && e.type == INPUT_RESYNC && e.lost_count == 7 + INPUT_CAPACITY && !e.value);
    unsigned fresh = 0;
    while (input_read(&e)) { CHECK(e.type == INPUT_KEY && !e.flags && e.source == INPUT_FIRMWARE && e.generation == saved); fresh++; }
    CHECK(fresh == 4);
    fake_fw_state = BIOSVM_DISABLED_BACKEND;
    CHECK(fwinput_adapter_init() == -V86_EIO);
    fwinput_adapter_step();
    struct i8042_stats driver;
    i8042_snapshot(&driver);
    CHECK(driver.quarantined && !driver.active && i8042_init() == -I8042_EIO);
    CHECK(!hw.reads && !hw.writes && !pic_changes);
    printf("firmware queue bridge: PASS (set-1/E0/Pause/text/motion/buttons, generation/source/sequence, loss, quarantine)\n");
}

int main(int argc, char **argv)
{
    if (argc == 2) {
        if (!strncmp(argv[1], "input-", 6)) {
            print_records = true;
            input_record_replay(argv[1]);
            return failures ? 1 : 0;
        }
        bool absent = !strcmp(argv[1], "firmware-absent");
        bool failed = !strcmp(argv[1], "firmware-failed");
        bool refused = !strcmp(argv[1], "firmware-refused");
        CHECK(absent || failed || refused || !strcmp(argv[1], "firmware-records"));
        print_records = true;
        test_firmware_fault_probe(absent, failed, refused);
        return failures ? 1 : 0;
    }
    test_policy_and_lifecycle();
    test_native_failures();
    test_init_diagnostics();
    test_advisory_interfaces_and_controller_acks();
    test_hook_selection();
    test_commands();
    test_keyboard_and_mouse();
    test_queue_and_stimulus();
    test_probe_records();
    test_native_fault_probe();
    test_firmware_fault_probe(false, false, false);
    test_firmware_fault_probe(true, false, false);
    test_firmware_fault_probe(false, true, false);
    test_firmware_fault_probe(false, false, true);
    test_fault_probe_refusals();
    test_firmware_mapping();
    i8042_fault_end();
    printf("i8042: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
