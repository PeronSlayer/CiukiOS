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
static void drain_input_deferred(void);
static void irq_restore(uint32_t f) { host_flags = f; if (f & 0x200) drain_input_deferred(); }
static void outl(uint16_t port, uint32_t v) { (void)port; (void)v; }
static uint32_t inl(uint16_t port) { (void)port; return UINT32_MAX; }
static uint16_t cpu_inw(uint16_t port) { (void)port; return UINT16_MAX; }

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
    uint8_t mouse_capability, rate_history[3], aux_trace[256], fault_reply, query_reply;
    unsigned rate_count, aux_sends, fail_aux_send, id_queries, fail_id_query;
    bool rate_next, persistent_fault, missing_id, wrong_id_aux;
    unsigned touchpad_kind, alps_model, slice_count, repeat_count;
    uint8_t slice, repeat, syn_mode, syn_major;
    uint32_t syn_model, syn_caps;
    bool resolution_next, rollback_bad, absolute, mode_mismatch, query_missing_once, burst_reply;
    unsigned missing_query_byte;
};
static struct fake_bus hw;
static void fake_reply_burst(struct fake_bus *b);
static irq_handler_t host_handlers[16];
static unsigned pic_changes, host_yields;
static bool host_masked[16];
static uint8_t host_pages[4][PAGE_SIZE];
static bool host_page_used[4];
static struct task *host_survivor;
static uint32_t host_survivor_data;
static bool replay_input;
static struct {
    bool enabled, armed, repeat, hold_key, hold_button, loss, lose_lease, continuous, late, capture;
    unsigned a, other, clicks, at;
    int dx, dy;
    uint64_t start, last_event;
} operator_script;
static void operator_replay_step(void);
static char input_arm[256], input_counts[256], input_motion[256], input_queue_record[256];
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
/* Standalone controller tests have no boot activation task. */
static bool native_policy_override;
bool drivers_input_firmware(void)
{
    return !native_policy_override && (g_boot.input_policy == CBI_INPUT_FIRMWARE ||
                                      (g_boot.flags & CBI_F_INPUT_FORCED));
}

static void fake_push(struct fake_bus *b, bool aux, uint8_t data, uint32_t delay)
{
    CHECK(b->count < ARRAY_SIZE(b->bytes));
    if (b->count == ARRAY_SIZE(b->bytes)) return;
    unsigned n = (b->head + b->count++) % ARRAY_SIZE(b->bytes);
    b->bytes[n] = (struct fake_byte){ aux ? 0x21 : 0x01, data, b->now + delay };
}
/* fx-3: controller port accesses made with IF clear outside an IRQ handler
 * (the kernel's former `irqoff_section=i8042_sample`). */
static unsigned irqoff_port_accesses;
static bool host_in_irq;
static uint8_t fake_read(void *arg, uint16_t port)
{
    struct fake_bus *b = arg;
    b->reads++;
    if (!(host_flags & 0x200) && !host_in_irq) irqoff_port_accesses++;
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
    if (!(host_flags & 0x200) && !host_in_irq) irqoff_port_accesses++;
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
    if (b->aux) {
        CHECK(b->aux_sends < ARRAY_SIZE(b->aux_trace));
        if (b->aux_sends < ARRAY_SIZE(b->aux_trace)) b->aux_trace[b->aux_sends] = data;
        b->aux_sends++;
        if (b->fail_aux_send && (b->aux_sends == b->fail_aux_send ||
                                (b->persistent_fault && b->aux_sends > b->fail_aux_send))) {
            if (b->fault_reply) fake_push(b, true, b->fault_reply, b->ack_delay);
            b->aux = false;
            return;
        }
    }
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
        if (b->aux && reply == 0xFA) {
            if (b->resolution_next) {
                b->resolution_next = false;
                b->slice = (uint8_t)((b->slice << 2) | (data & 3));
                b->slice_count++;
                b->repeat_count = 0;
            } else if (b->rate_next) {
                b->rate_next = false;
                b->rate_history[0] = b->rate_history[1];
                b->rate_history[1] = b->rate_history[2];
                b->rate_history[2] = data;
                b->rate_count++;
                if (b->touchpad_kind == 1 && b->slice_count == 4 && data == 20) {
                    b->syn_mode = b->slice;
                    b->absolute = !!(b->syn_mode & 0x80);
                }
                if (b->mouse_capability >= 3 && b->rate_history[0] == 200 &&
                    b->rate_history[1] == 100 && data == 80) b->mouse_id = 3;
                if (b->mouse_capability == 4 && b->mouse_id == 3 &&
                    b->rate_history[0] == 200 && b->rate_history[1] == 200 && data == 80)
                    b->mouse_id = 4;
            } else if (data == 0xE8) b->resolution_next = true;
            else if (data == 0xE6 || data == 0xE7 || data == 0xF5) {
                b->repeat_count = b->repeat == data ? b->repeat_count + 1 : 1;
                b->repeat = data;
                if (data == 0xE6) b->slice_count = b->slice = 0;
            } else if (data == 0xF3) b->rate_next = true;
            else if (data == 0xF6) {
                b->absolute = false;
                b->resolution_next = b->rate_next = false;
                b->slice_count = b->repeat_count = 0;
            } else if (data == 0xE9 || data == 0xEB) {
                uint32_t value = data == 0xEB ? 0x080000 : 100;
                if (data == 0xEB && b->rollback_bad) value = 0xF80000;
                if (data == 0xE9 && b->touchpad_kind == 1 && b->slice_count == 4) {
                    switch (b->slice) {
                    case 0: value = 0x024700 | (b->syn_major ? b->syn_major : 4); break;
                    case 1: value = 0x3B4700 | (b->syn_mode ^ (b->mode_mismatch ? 0x80 : 0)); break;
                    case 2: value = b->syn_caps; break;
                    case 3: value = b->syn_model; break;
                    }
                }
                if (data == 0xE9 && b->touchpad_kind == 2 && b->repeat == 0xE7 && b->repeat_count == 3) {
                    /* Independent signature corpus; includes unsupported v3. */
                    static const uint32_t signatures[] = {
                        0x33020a,0x22020a,0x220214,0x520114,0x620214,
                        0x63020a,0x630228,0x63023c,0x630250,0x730250,
                        0x20020e,0x320214,0x53020a,0x530214,0x6003c8,
                        0x630214,0x630264,0x6303c8,0x73000a,0x73020a,0x730214,
                        0x730264};
                    value = signatures[b->alps_model];
                }
                for (unsigned i = 0; i < 3; i++)
                    if (b->missing_query_byte != i + 1)
                        fake_push(b, true, (uint8_t)(value >> (16 - i * 8)), b->ack_delay);
                if (b->query_missing_once) b->missing_query_byte = 0;
            }
            else if (data == 0xF2) {
                b->id_queries++;
                bool fault = b->id_queries == b->fail_id_query;
                if (!(fault && b->missing_id))
                    fake_push(b, !(fault && b->wrong_id_aux),
                              fault ? b->query_reply : b->mouse_id, b->ack_delay);
            }
        }
        if (b->aux && data == 0xF4 && b->final_obf) b->obf_after_ack = true;
    }
    if (b->burst_reply) fake_reply_burst(b);
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
            host_in_irq = true;
            host_handlers[1](&tf);
            host_in_irq = false;
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

void pic_mask(unsigned irq)
{
    CHECK(irq < 16);
    if (!host_masked[irq]) { host_masked[irq] = true; pic_changes++; }
}
void pic_unmask(unsigned irq)
{
    CHECK(irq < 16 && host_handlers[irq]);
    if (irq >= 8 && host_masked[2]) { host_masked[2] = false; pic_changes++; }
    if (host_masked[irq]) { host_masked[irq] = false; pic_changes++; }
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
        if (operator_script.enabled && operator_script.armed) operator_replay_step();
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
    if (!strcmp(probe, "input")) {
        if (!strcmp(event, "ARM")) { strcpy(input_arm, extra); operator_script.armed = true; }
        if (!strncmp(extra, "case=counts ", 12)) strcpy(input_counts, extra);
        if (!strncmp(extra, "case=motion ", 12)) strcpy(input_motion, extra);
        if (!strncmp(extra, "case=queue ", 11)) strcpy(input_queue_record, extra);
    }
    if (!strcmp(probe, "input") && !strncmp(extra, "case=setup step=", 16)) setup_records++;
    if (strstr(extra, "case=firmware_overrun ") || strstr(extra, "case=disallowed_io ")) {
        firmware_records++;
        if (strstr(extra, "status=not_run reason=firmware_backend_absent")) absent_records++;
        if (strstr(extra, "status=not_run reason=firmware_selftest_qemu_only")) refused_records++;
    }
    if (!strcmp(event, "READY")) record_ready++;
    if (!strcmp(event, "END")) {
        if (!strcmp(extra, "status=PASS")) record_pass++;
        else if (strstr(extra, "status=not_run") || strstr(extra, "status=NOT_RUN")) record_deferred++;
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
static kwork_fn input_deferred;
static bool defer_input_delivery;
static void *input_deferred_arg;
bool kwork_queue(kwork_fn fn, void *arg) { input_deferred=fn;input_deferred_arg=arg;return true; }
static void drain_input_deferred(void)
{
    static bool draining;
    if(draining || defer_input_delivery) return;
    draining=true;
    for(unsigned n=0;input_deferred && n<1024;n++) { kwork_fn fn=input_deferred;input_deferred=0;fn(input_deferred_arg); }
    draining=false;
}
void kwork_yield(void) { CHECK(false); }

/* sync.c contains RDTSC, never executed: g_cpu_tsc is false in this test.
 * Replace ONLY udelay: privileged native pacing becomes fake PIT time. */
#define udelay sync_udelay
#include "../../tests/host/scheduler_stubs.h"
#include "../../src/kernel/core/sync.c"
#undef udelay
int udelay(uint32_t us)
{
    CHECK(us <= 1000 && ((host_flags & 0x200) || us <= 50));
    if (host_flags & 0x200) fake_pause(&hw);
    return 0;
}
#include "../../src/kernel/lib/stackprot.c"
static uint64_t read_tsc_serialized(void) { return ktime_cycles(); }
#include "../../src/kernel/core/irq_budget.c"
#include "../../src/kernel/core/registry.c"
#include "../../src/kernel/drivers/i8042.c"

static void fake_reply_burst(struct fake_bus *b)
{
    if (b != &hw) return;
    while (b->count && b->bytes[b->head].ready <= b->now) {
        uint8_t status = fake_read(b, 0x64);
        uint8_t data = fake_read(b, 0x60);
        capture_at(&native, status, data, b->now);
    }
}
#include "../../src/kernel/drivers/fwinput_adapter.c"
#include "../../src/kernel/probes/selector.c"
static unsigned operator_prompts;
static uint64_t operator_prompt_at;
void console_operator_prompt(bool announce)
{
    if (announce) { operator_prompts++; operator_prompt_at=deadline_after_ms(0); }
}
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
    native_policy_override = false;
    i8042_fault_end();
    memset(&native, 0, sizeof(native));
    adapter = 0; generation = 0; loss_seen = 0;
    fw_head = fw_count = 0; fake_fw_loss = 0;
    fake_fw_state = BIOSVM_READY;
    memset(&hw, 0, sizeof(hw));
    hw.pause_step = 1;
    hw.missing_command = 0xFF; /* zero is now a valid resolution argument */
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
static void operator_motion(int dx, int dy)
{
    if (g_boot.input_policy == CBI_INPUT_FIRMWARE) {
        fw_push(FWINPUT_REL, FWINPUT_X, dx);
        fw_push(FWINPUT_REL, FWINPUT_Y, dy);
        fwinput_adapter_step();
    } else {
        int raw_y = -dy;
        replay_byte(true, 0x08 | native.queue.stats.buttons | (dx < 0 ? 0x10 : 0) | (raw_y < 0 ? 0x20 : 0));
        replay_byte(true, (uint8_t)dx); replay_byte(true, (uint8_t)raw_y);
    }
    operator_script.last_event = hw.now;
}
static void operator_cycle(bool a, bool repeat, bool release_key, bool click, bool release_button)
{
    if (g_boot.input_policy == CBI_INPUT_FIRMWARE) {
        unsigned code = a ? INPUT_KEY_A : 0x30;
        fw_push(FWINPUT_KEY, code, 1); fw_push(FWINPUT_TEXT, 0, a ? 'a' : 'b');
        /* Firmware typematic yields another TEXT opportunity without another
         * KEY event (fwinput.c, held-state change only). */
        if (repeat) fw_push(FWINPUT_TEXT, 0, a ? 'a' : 'b');
        if (release_key) fw_push(FWINPUT_KEY, code, 0);
        if (click) {
            fw_push(FWINPUT_BUTTON, INPUT_LEFT, 1);
            if (release_button) fw_push(FWINPUT_BUTTON, INPUT_LEFT, 0);
        }
        fwinput_adapter_step();
    } else {
        uint8_t scan = a ? 0x1c : 0x32;
        replay_byte(false, scan);
        if (repeat) replay_byte(false, scan);
        if (release_key) { replay_byte(false, 0xf0); replay_byte(false, scan); }
        if (click) {
            replay_byte(true, 0x09); replay_byte(true, 0); replay_byte(true, 0);
            if (release_button) { replay_byte(true, 0x08); replay_byte(true, 0); replay_byte(true, 0); }
        }
    }
    operator_script.last_event = hw.now;
}
static void operator_replay_step(void)
{
    if (!operator_script.start) operator_script.start = hw.now;
    unsigned total = operator_script.a + operator_script.other;
    uint64_t elapsed = hw.now - operator_script.start;
    if (operator_script.at < total && elapsed >= 100 * operator_script.at) {
        unsigned at = operator_script.at++;
        operator_cycle(at < operator_script.a,
                       operator_script.capture ? at < 13 : operator_script.repeat && !at,
                       !operator_script.hold_key || at + 1 < total,
                       at < operator_script.clicks,
                       !operator_script.hold_button || at + 1 < operator_script.clicks);
        operator_motion(operator_script.capture ? (at == 99 ? 4 : 37) : operator_script.dx,
                        operator_script.capture ? (at == 99 ? -8 : -48) : operator_script.dy);
        if (at + 1 == total) {
            if (operator_script.loss) native.queue.stats.overflow++;
            if (operator_script.lose_lease) native.stats.active = false;
        }
    }
    if (operator_script.late && elapsed == total * 100 + 500)
        operator_cycle(false, false, true, false, true);
    if (operator_script.continuous && elapsed % 500 == 0) operator_motion(1, 0);
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
    /* F0-37: effective native selection overrides copied firmware/safe flags.
     * Exercise the real acquisition and command sequence, plus USB delivery. */
    reset_native();
    g_boot.input_policy = CBI_INPUT_FIRMWARE;
    g_boot.flags = CBI_F_INPUT_FORCED | CBI_F_SAFE_MODE;
    native_policy_override = true;
    CHECK(!i8042_init() && native.stats.active && !native.stats.firmware);
    unsigned events_before = native.queue.stats.pending;
    input_usb_event(INPUT_KEY, INPUT_KEY_A, 1, 123, 1);
    CHECK(native.queue.stats.pending > events_before);
    CHECK(g_boot.input_policy == CBI_INPUT_FIRMWARE &&
          g_boot.flags == (CBI_F_INPUT_FORCED | CBI_F_SAFE_MODE));
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
    struct input_devices devices;input_devices_snapshot(&devices);
    CHECK(devices.ps2==2 && !devices.firmware && !devices.usb);
    CHECK(!(hw.config & 0x70) && (hw.config & 7) == 7 && !hw.resets && hw.selftests == 1);
    CHECK(s.last_elapsed_ms <= I8042_SETUP_MS && !host_masked[1] && !host_masked[12] && !host_masked[2]);
    CHECK(host_handlers[1] && host_handlers[12]);
    CHECK(pic_changes == 3);          /* IRQ1, cascade and IRQ12, once */
    unsigned pic_before = pic_changes;
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
    uint64_t captured_tick = g_ticks; defer_input_delivery = true;
    for (unsigned i = 0; i < 3; i++) {
        unsigned before = hw.count;
        uint32_t f = irq_save();
        host_handlers[12](&tf);
        irq_restore(f);
        CHECK(hw.count + 1 == before);
    }
    hw.now += 5; g_ticks = hw.now; defer_input_delivery = false; drain_input_deferred();
    struct input_event event;
    CHECK(input_read(&event) && event.type == INPUT_KEY && event.code == INPUT_KEY_A && event.value == 1 && event.tick == captured_tick);
    CHECK(input_read(&event) && event.type == INPUT_TEXT && event.code == 'a');
    CHECK(input_read(&event) && event.value == 0 && !input_read(&event));
    hw.ack_delay = 1;
    hw.irq_reply = true;
    for (unsigned i = 0; i < 16; i++) {
        kmutex_lock(&native.mutex);
        CHECK(device_command(&native, i & 1, 0xF4, hw.now + 100) == 0);
        kmutex_unlock(&native.mutex);
        CHECK(pic_changes == pic_before && (hw.config & IRQ_BITS) == IRQ_BITS);
    }
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
    input_devices_snapshot(&devices);CHECK(!devices.ps2 && !devices.firmware && !devices.usb);
    CHECK((hw.config & 0x33) == 0x30 && !host_masked[1] && !host_masked[12]);
    CHECK(pic_changes == pic_before);
    CHECK(!host_handlers[1] && !host_handlers[12] && !hw.count && !hw.resets);
    for (unsigned i = 0; i < registry_count(); i++)
        if (!strcmp(registry_get(i)->owner, "i8042")) CHECK(registry_get(i)->state == RS_RELEASED);
    CHECK(i8042_init() == 0);
    i8042_snapshot(&s);
    CHECK(s.generation && i8042_stop(s.generation) == 0);
    CHECK(pic_changes == pic_before); /* no OUT on reactivation either */
    puts("i8042 PIC: IRQ/data/16 keyboard+AUX commands/clean stop+restart=0 writes after init; command byte IRQ gates verified PASS");
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
    hw.mouse_id = 2;                    /* an unknown protocol cannot be decoded */
    CHECK(i8042_init() == -I8042_EPROTO && !hw.resets);
    reset_native();
    CHECK(i8042_init() == 0);
    i8042_snapshot(&s);
    hw.stop_stuck = true;
    unsigned pic_before = pic_changes;
    CHECK(i8042_stop(s.generation) == -I8042_ETIMEDOUT);
    i8042_snapshot(&s);
    CHECK(s.quarantined && !host_handlers[1] && !host_handlers[12]);
    CHECK(host_masked[1] && host_masked[12] && pic_changes == pic_before + 2);
    CHECK(i8042_init() == -I8042_EIO && pic_changes == pic_before + 2);
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

static void test_mouse_negotiation(void)
{
    /* Exact additional discovery/recovery prefix; wheel sequences unchanged. */
#define TP_PREFIX 0xF5,0xF6,0xE6,0xE8,0,0xE8,0,0xE8,0,0xE8,0,0xE9, \
    0xE8,0,0xE6,0xE6,0xE6,0xE9,0xE8,0,0xE7,0xE7,0xE7,0xE9,0xF6,0xF5,0xEA,0xEB
    const uint8_t standard[] = { TP_PREFIX, 0xF2, 0xF3, 200, 0xF3, 100,
                                0xF3, 80, 0xF2, 0xEA, 0xE6, 0xF4 };
    const uint8_t extended[] = { TP_PREFIX, 0xF2, 0xF3, 200, 0xF3, 100,
                                0xF3, 80, 0xF2, 0xF3, 200, 0xF3, 200,
                                0xF3, 80, 0xF2, 0xEA, 0xE6, 0xF4 };
    const uint8_t explorer[] = { TP_PREFIX, 0xF2, 0xEA, 0xE6, 0xF4 };
#undef TP_PREFIX
    const uint8_t ids[] = { 0, 3, 4 };
    for (unsigned i = 0; i < ARRAY_SIZE(ids); i++) {
        reset_native();
        hw.mouse_capability = ids[i];
        CHECK(!i8042_init());
        struct i8042_stats s;
        i8042_snapshot(&s);
        CHECK(s.aux_id == ids[i] && s.active && !s.pending_command && !s.quarantined);
        const uint8_t *trace = i ? extended : standard;
        unsigned length = i ? sizeof(extended) : sizeof(standard);
        CHECK(hw.aux_sends == length && !memcmp(hw.aux_trace, trace, length));
        CHECK(hw.rate_count == (i ? 6u : 3u) && hw.id_queries == (i ? 3u : 2u));
        CHECK(s.last_elapsed_ms <= I8042_SETUP_MS && !hw.resets && native.init_count == 15);
        CHECK(!native.queue.stats.pending && !native.queue.mouse_pos);
        char expected[80];
        snprintf(expected, sizeof(expected), "[i8042] aux id=%u protocol=%s", ids[i],
                 ids[i] == 4 ? "explorer" : ids[i] == 3 ? "intellimouse" : "ps2");
        CHECK(!strcmp(last_log, expected));
    }
    /* F6 must not cause a negotiated device to be mistaken for ID 0. */
    for (unsigned i = 1; i < ARRAY_SIZE(ids); i++) {
        reset_native();
        hw.mouse_id = hw.mouse_capability = ids[i];
        CHECK(!i8042_init() && native.stats.aux_id == ids[i] && !hw.resets);
        if (ids[i] == 4)
            CHECK(hw.aux_sends == sizeof(explorer) && !memcmp(hw.aux_trace, explorer, sizeof(explorer)));
    }
    /* A fresh supported ID proves the lower format, even after stage two. */
    reset_native();
    hw.mouse_capability = 4; hw.fail_id_query = 3; hw.query_reply = 0;
    CHECK(!i8042_init() && native.stats.aux_id == 0 && !hw.resets);

    /* Refuse every unknown ID, including reply-looking bytes, at each query. */
    const uint8_t unknown[] = { 1, 2, 5, 0xFA, 0xFE, 0xAA, 0xFF };
    for (unsigned query = 1; query <= 3; query++) {
        for (unsigned i = 0; i < ARRAY_SIZE(unknown); i++) {
            reset_native();
            hw.mouse_capability = 4; hw.fail_id_query = query; hw.query_reply = unknown[i];
            CHECK(i8042_init() == -I8042_EPROTO);
            struct i8042_init_record r = init_failure(I8042_INIT_RESET_AUX, -I8042_EPROTO);
            CHECK(r.command == 0xF2 && r.reply == unknown[i]);
            CHECK(native.stats.aux_id == (query == 3 ? 3 : 0));
            CHECK(native.stats.quarantined && !native.stats.active && !native.stats.pending_command);
            CHECK(!hw.resets && host_masked[1] && host_masked[12]);
            unsigned writes = hw.writes, reads = hw.reads;
            CHECK(i8042_init() == -I8042_EIO && writes == hw.writes && reads == hw.reads);
            for (unsigned j = 0; j < ARRAY_SIZE(native.handle); j++)
                CHECK(registry_get(native.handle[j])->state == RS_QUARANTINED);
        }
        for (unsigned mode = 0; mode < 2; mode++) {
            reset_native();
            hw.mouse_capability = 4; hw.fail_id_query = query;
            hw.missing_id = !mode; hw.wrong_id_aux = mode;
            CHECK(i8042_init() == -I8042_ETIMEDOUT);
            CHECK(native.stats.quarantined && !native.stats.pending_command && !hw.resets);
            CHECK(native.stats.last_elapsed_ms <= I8042_SETUP_MS);
        }
    }
    /* Every rate command, parameter and GETID ACK can fail. Never enable
     * streaming from an old ID after an uncertain protocol transaction. */
    const uint8_t replies[] = { 0, 0xFC, 0xFD, 0xFE };
    for (unsigned send = 30; send <= 43; send++) {
        for (unsigned mode = 0; mode < ARRAY_SIZE(replies); mode++) {
            reset_native();
            hw.mouse_capability = 4; hw.fail_aux_send = send;
            hw.fault_reply = replies[mode]; hw.persistent_fault = true;
            int expected = mode ? -I8042_EPROTO : -I8042_ETIMEDOUT;
            CHECK(i8042_init() == expected);
            struct i8042_init_record r = init_failure(I8042_INIT_RESET_AUX, expected);
            CHECK(r.command == extended[send - 1]);
            CHECK(hw.aux_sends == send + (replies[mode] == 0xFE ? I8042_RESENDS : 0));
            CHECK(native.stats.quarantined && !native.stats.active && !native.stats.pending_command);
            CHECK(native.stats.last_elapsed_ms <= I8042_SETUP_MS && !hw.resets);
        }
        reset_native();
        hw.mouse_capability = 4; hw.fail_aux_send = send; hw.fault_reply = 0xFE;
        CHECK(!i8042_init() && native.stats.aux_id == 4 && native.stats.resends == 1);
        CHECK(hw.aux_trace[send - 1] == hw.aux_trace[send] && hw.aux_sends == sizeof(extended) + 1);
    }
    reset_native();
    hw.mouse_capability = 4; hw.ack_delay = 3; hw.write_delay = 1; hw.mixed = true;
    CHECK(!i8042_init() && native.stats.aux_id == 4 && !native.queue.stats.pending);
    CHECK(!native.queue.extended && !native.queue.mouse_pos && !native.queue.stats.keys_down);
    puts("i8042 ID 0/3/4: exact rate sequences, proven fallback, unknown/missing/wrong-source IDs, every negotiation ACK refusal/RESEND PASS");
}

static void native_packet(const uint8_t *packet, unsigned length)
{
    for (unsigned i = 0; i < length; i++) capture_at(&native, 0x21, packet[i], g_ticks);
}

static void native_event(unsigned type, unsigned code, int value)
{
    struct input_event e;
    CHECK(input_read(&e));
    CHECK(e.type == type && e.code == code && e.value == value);
    CHECK(e.source == INPUT_NATIVE && e.generation == native.stats.generation && e.tick == g_ticks);
}

static void test_extended_mouse_packets(void)
{
    struct input_event e;
    for (unsigned id = 3; id <= 4; id++) {
        reset_native(); hw.mouse_capability = id;
        CHECK(!i8042_init());
        g_ticks = 123;
        /* Every signed byte (ID 3) / nibble+button combination (ID 4). */
        for (unsigned byte = 0; byte < (id == 3 ? 256u : 64u); byte++) {
            uint8_t packet[] = { 0x38, 0xFE, 0xFF, (uint8_t)byte };
            unsigned before = native.queue.stats.buttons;
            native_packet(packet, sizeof(packet));
            native_event(INPUT_REL, INPUT_X, -2);
            native_event(INPUT_REL, INPUT_Y, 1);
            int wheel = id == 3 ? (int)byte - ((byte & 0x80) ? 256 : 0) :
                                  (int)(byte & 15) - ((byte & 8) ? 16 : 0);
            if (wheel) native_event(INPUT_REL, INPUT_WHEEL, -wheel);
            unsigned buttons = id == 4 ? (byte & 0x30) >> 1 : 0;
            for (unsigned i = 0; i < 5; i++)
                if ((before ^ buttons) & (1u << i))
                    native_event(INPUT_BTN, i, !!(buttons & (1u << i)));
            CHECK(!input_read(&e) && native.queue.stats.buttons == buttons);
        }
        uint8_t all[] = { 0x0F, 200, 200, id == 4 ? 0x30 : 0 };
        native_packet(all, 3);
        CHECK(!input_read(&e));         /* never publish three of four bytes */
        native_packet(all + 3, 1);
        native_event(INPUT_REL, INPUT_X, 200); native_event(INPUT_REL, INPUT_Y, -200);
        for (unsigned i = 0; i < 3; i++) native_event(INPUT_BTN, i, 1);
        CHECK(!input_read(&e));
        uint8_t release[] = { 0x38, 0, 0, 0 }; /* zero payload with sign bits is zero */
        native_packet(release, 4);
        for (unsigned i = 0; i < (id == 4 ? 5u : 3u); i++) native_event(INPUT_BTN, i, 0);
        CHECK(!input_read(&e));

        /* Lost suffix of a packet, then idle: recover without publishing
         * stale coordinates/buttons. Exercise every partial packet length. */
        for (unsigned prefix = 1; prefix < 4; prefix++) {
            native_packet(all, prefix);
            CHECK(!input_read(&e));
            uint64_t resync = native.queue.stats.resync;
            g_ticks += MOUSE_PACKET_GAP_MS + 1;
            uint8_t clean[] = { 0x08, 2, 0, 1 };
            native_packet(clean, 4);
            native_event(INPUT_REL, INPUT_X, 2); native_event(INPUT_REL, INPUT_WHEEL, -1);
            CHECK(!input_read(&e) && native.queue.stats.resync == resync + 1);
        }
        /* A status error aborts partial packets; ACK/RESEND outside a
         * transaction are rejected as headers, not decoded as wheel data. */
        native_packet(all, 2);
        capture_at(&native, 0xE1, 0, g_ticks);
        uint8_t noise[] = { 0xFA, 0xFE, 0x00, 0xC8 };
        native_packet(noise, sizeof(noise));
        native_packet(release, 4);
        CHECK(!input_read(&e) && !native.queue.mouse_pos && native.queue.stats.errors == 1);
        capture_at(&native, 1, 0x1C, g_ticks);
        native_event(INPUT_KEY, INPUT_KEY_A, 1); native_event(INPUT_TEXT, 'a', 0);
        capture_at(&native, 1, 0xF0, g_ticks); capture_at(&native, 1, 0x1C, g_ticks);
        native_event(INPUT_KEY, INPUT_KEY_A, 0);
        CHECK(!input_read(&e));
    }
    reset_native(); hw.mouse_capability = 4; CHECK(!i8042_init());
    const uint8_t bad[] = { 0x0F, 0, 0, 0xC0 };
    native_packet(bad, 4);
    CHECK(!input_read(&e) && !native.queue.stats.buttons && !native.queue.mouse_pos);
    /* Missing payload creates an invalid fourth byte; retain suffix header. */
    const uint8_t lost[] = { 0x08, 0x08, 0, 0x80, 0 };
    native_packet(lost, sizeof(lost));
    native_event(INPUT_REL, INPUT_Y, -128);
    CHECK(!input_read(&e) && native.queue.stats.resync == 2 && !native.queue.mouse_pos);
    /* Overflow RESYNC snapshots include held buttons 4/5, and USB's three
     * buttons neither erase nor alias those native bits. */
    uint8_t side[] = { 0x08, 0, 0, 0x30 };
    native_packet(side, 4);
    native_event(INPUT_BTN, INPUT_SIDE, 1); native_event(INPUT_BTN, INPUT_EXTRA, 1);
    input_usb_event(INPUT_BTN, INPUT_LEFT, 1, 77, g_ticks);
    CHECK(input_read(&e) && e.source == INPUT_USB && e.code == INPUT_LEFT && e.value == 1);
    native_packet(side, 4); CHECK(!input_read(&e) && native.queue.stats.buttons == 25);
    side[1] = 1;
    for (unsigned i = 0; i <= INPUT_CAPACITY; i++) native_packet(side, 4);
    CHECK(input_read(&e) && e.type == INPUT_RESYNC && e.value == 25 && e.lost_count == INPUT_CAPACITY);
    CHECK(input_read(&e) && e.type == INPUT_REL && e.code == INPUT_X && e.value == 1);
    CHECK(!input_read(&e));
    side[1] = side[3] = 0;
    native_packet(side, 4);
    native_event(INPUT_BTN, INPUT_SIDE, 0); native_event(INPUT_BTN, INPUT_EXTRA, 0);
    CHECK(!input_read(&e) && native.queue.stats.buttons == 1);
    input_usb_event(INPUT_BTN, INPUT_LEFT, 0, 77, g_ticks);
    CHECK(input_read(&e) && e.source == INPUT_USB && !e.value && !input_read(&e));
    puts("i8042 ID 3/4 decode: exhaustive signed wheel/buttons, motion, no early events, lost-byte/gap/status/reserved-byte resync, USB merge/overflow PASS");
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

/* fx-3: the T23 trial recorded `irqoff_section=i8042_sample irqoff_max_us=2156`
 * (one firmware SMI inside a polled sample taken with IF clear). Setup,
 * live commands, stop and the fault fixture now sample the controller with
 * interrupts enabled; while the IRQ path is live only input_irq reads 60h. */
static void test_irqoff_sampling(void)
{
    struct i8042_stats s;
    struct input_event event;
    reset_native();
    t23_bus();
    hw.ack_delay = 1;
    irqoff_port_accesses = 0;
    unsigned reads = hw.reads;
    CHECK(i8042_init() == 0 && native.stats.active && hw.reads > reads);
    CHECK(irqoff_port_accesses == 0 && (host_flags & 0x200));
    /* Live path: replies arrive through input_irq/input_work only. */
    hw.irq_reply = true;
    unsigned handler_reads = 0;
    for (unsigned i = 0; i < 8; i++) {
        unsigned before = hw.reads;
        kmutex_lock(&native.mutex);
        CHECK(device_command(&native, i & 1, 0xF4, hw.now + 100) == 0);
        kmutex_unlock(&native.mutex);
        CHECK(hw.reads > before);
        handler_reads += hw.reads - before;
    }
    CHECK(handler_reads && irqoff_port_accesses == 0 && !native.stats.pending_command);
    CHECK(!hw.count && !input_read(&event));
    hw.irq_reply = false;
    /* A pending byte while live is never read by the command engine. */
    fake_push(&hw, false, 0x1C, 0);
    reads = hw.reads;
    kmutex_lock(&native.mutex);
    struct poll_deadline d = limit(&native, hw.now + 5, 5);
    CHECK(drain(&native, &d, false) == 0 && !capture_one(&native, false));
    kmutex_unlock(&native.mutex);
    CHECK(hw.count == 1 && hw.reads == reads);
    host_in_irq = true;
    uint32_t f = irq_save();
    struct trap_frame tf = { .vector = 0x21 };
    host_handlers[1](&tf);
    irq_restore(f);
    host_in_irq = false;
    CHECK(!hw.count && input_read(&event) && event.type == INPUT_KEY && event.code == INPUT_KEY_A);
    while (input_read(&event)) {}
    i8042_snapshot(&s);
    CHECK(i8042_stop(s.generation) == 0 && irqoff_port_accesses == 0);
    hw.ack_delay = 0;
    printf("i8042 interrupt-on sampling: setup/live commands/stop, zero IF-off port accesses outside input_irq PASS\n");
}

static void test_advisory_interfaces_and_controller_acks(void)
{
    reset_native();
    t23_bus();
    CHECK(i8042_init() == 0 && native.stats.active && !native.stats.quarantined);
    CHECK(native.stats.initial_config == 0x77 && hw.last_config == 0x07);
    CHECK(native.init_count == 15 && hw.sends == 43 && !hw.resets); /* includes wheel probe */
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
    CHECK(r.elapsed_ms >= I8042_REPLY_MS && hw.sends == 17); /* probe skipped after slow diagnostics */
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
        CHECK(hw.sends == 43 && !hw.resets);
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
    CHECK(strstr(input_arm, "criteria=exact keys=100 moves=100 buttons=10 x=2 y=-1"));
    CHECK(strstr(input_counts, "criteria=exact") && strstr(input_counts, "characters=100 key_transitions=200 button_transitions=20"));
    CHECK(native.stats.active && !native.stats.quarantined);
    reset_native();
    g_boot.input_policy = CBI_INPUT_FIRMWARE;
    replay_input = true;
    replay_cycles = 0;
    CHECK(probe_input() == 0 && !record_deferred && record_pass == 2);
    replay_input = false;
    CHECK(!hw.reads && !hw.writes && !pic_changes);
    /* Sweep-only wait retains the first event and counts a complete replay.
     * No event ends after 60 s, rather than entering the 120 s window. */
    reset_native(); probes_operator_mode = true;
    replay_input = true; replay_cycles = 0;
    CHECK(probe_input() == 0 && replay_cycles == 100 && operator_prompts == 1);
    replay_input = false;
    reset_native();
    unsigned skipped = record_deferred;
    CHECK(probe_input() == -ECANCELED && record_deferred == skipped + 1);
    CHECK(deadline_after_ms(0) - operator_prompt_at == 60000 && operator_prompts == 2);
    CHECK(native.stats.active && !native.stats.quarantined);
    probes_operator_mode = false;
    reset_native();
    select_fault("f1:input-fault run=12ab34cd");
    struct registry_stats before, after;
    registry_snapshot(&before);
    CHECK(probe_input_fault() == 0);
    registry_snapshot(&after);
    CHECK(record_pass == 4 && !host_survivor && !fixture && !hw.reads && !hw.writes && !pic_changes);
    CHECK(before.claims == after.claims && before.live == after.live && before.quarantines == after.quarantines);
    for (unsigned i = 0; i < ARRAY_SIZE(host_pages); i++) CHECK(!host_page_used[i]);
    printf("i8042 probe orchestration/records: PASS (%u records, maximum %zu bytes, native PASS/firmware PASS/fault PASS)\n",
           record_count, longest_record);
}

static void test_operator_criteria(void)
{
    /* Drive the real probe through native decoding and the firmware adapter.
     * Low totals must pass only after an answered operator prompt, with the
     * first retained event, arbitrary signed motion and other text counted. */
    for (unsigned firmware = 0; firmware < 2; firmware++) {
        for (unsigned variant = 0; variant < 11; variant++) {
            reset_native();
            probes_operator_mode = variant != 10;
            if (firmware) g_boot.input_policy = CBI_INPUT_FIRMWARE;
            memset(&operator_script, 0, sizeof(operator_script));
            operator_script.enabled = true;
            operator_script.a = 10; operator_script.other = 3; operator_script.clicks = 1;
            operator_script.dx = -3; operator_script.dy = -2;
            if (variant == 1) operator_script.a = 9;
            if (variant == 2) operator_script.clicks = 0;
            if (variant == 3) { operator_script.dx = -1; operator_script.dy = 0; }
            if (variant == 4) operator_script.repeat = true;
            if (variant == 5) operator_script.hold_key = true;
            if (variant == 6) { operator_script.clicks = 2; operator_script.hold_button = true; }
            if (variant == 7) operator_script.loss = true;
            if (variant == 8) operator_script.lose_lease = true;
            if (variant == 9) operator_script.late = true;
            bool pass = variant == 0 || variant == 4 || variant == 9; /* 4: typematic repeats tolerated (lead amendment) */
            strcpy(expected_end, "status=FAIL reason=stimulus_or_lease");
            CHECK(probe_input() == !pass);
            CHECK(operator_script.at == operator_script.a + operator_script.other);
            CHECK(strstr(input_arm, probes_operator_mode ? "criteria=operator" : "criteria=exact"));
            CHECK(strstr(input_counts, probes_operator_mode ? "criteria=operator" : "criteria=exact"));
            if (pass && variant == 4) CHECK(firmware ? strstr(input_queue_record, "repeats=0") != NULL :
                                            strstr(input_queue_record, "repeats=") && !strstr(input_queue_record, "repeats=0"));
            if (pass && variant != 4) {
                CHECK(hw.now - operator_script.last_event == 1000);
                CHECK(hw.now - operator_script.start < 120000);
                CHECK(strstr(input_counts, variant == 9 ? "characters=14 a_characters=10" : "characters=13 a_characters=10"));
                CHECK(strstr(input_counts, "a_min=10 key_min=20 button_min=2 motion_min=50"));
                CHECK(strstr(input_motion, "x=-39 y=-26"));
            }
            operator_script.enabled = false;
        }
    }
    reset_native(); probes_operator_mode = true;
    memset(&operator_script, 0, sizeof(operator_script));
    operator_script.enabled = operator_script.continuous = true;
    operator_script.a = 10; operator_script.clicks = 1; operator_script.dx = -5;
    CHECK(probe_input() == 0);
    CHECK(hw.now - operator_script.start >= 120000 && hw.now - operator_script.last_event < 1000);
    operator_script.enabled = false;
    /* Reproduce the capture's aggregate stimulus, including its 13 typematic
     * repeats: under operator criteria they are recorded and the probe passes. */
    reset_native();
    memset(&operator_script, 0, sizeof(operator_script));
    operator_script.enabled = operator_script.capture = true;
    operator_script.a = 100; operator_script.clicks = 37;
    CHECK(probe_input() == 0);
    CHECK(strstr(input_counts, "characters=113 a_characters=113 key_transitions=200 button_transitions=74"));
    CHECK(strstr(input_motion, "x=3667 y=-4760") && strstr(input_queue_record, "repeats=13"));
    operator_script.enabled = false; probes_operator_mode = false; expected_end[0] = 0;
    printf("operator input criteria: PASS (native/firmware, first event, other text, negative motion, quiet/tail/deadline, missing stimulus, repeats/loss/stuck/lease rejection, T23 aggregates)\n");
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

static void test_usb_shared_queue(void)
{
    reset_native();
    struct input_devices devices;
    input_devices_snapshot(&devices);CHECK(!devices.ps2 && !devices.firmware && !devices.usb);
    input_devices_changed(INPUT_USB,true);
    input_devices_snapshot(&devices);CHECK(!devices.ps2 && devices.usb==1);
    CHECK(strstr(last_log,"[input] devices=1 source=usb event=attach"));
    /* A failed PS/2 controller must not prevent independent USB input. */
    native.stats.quarantined = true;
    input_devices_snapshot(&devices);CHECK(devices.usb==1);
    input_usb_event(INPUT_KEY, INPUT_KEY_A, 1, 71, 1);
    struct input_event e;
    CHECK(input_read(&e) && e.type == INPUT_KEY && e.value == 1 && e.source == INPUT_USB && e.generation == 71);
    CHECK(input_read(&e) && e.type == INPUT_TEXT && e.code == 'a');
    key_transition(&native, INPUT_KEY_A, true, 2);
    CHECK(!input_read(&e));
    input_usb_event(INPUT_KEY, INPUT_KEY_A, 0, 71, 3);
    CHECK(!input_read(&e) && native.queue.stats.keys_down == 1);
    key_transition(&native, INPUT_KEY_A, false, 4);
    CHECK(input_read(&e) && e.type == INPUT_KEY && e.value == 0);
    input_usb_event(INPUT_KEY, INPUT_KEY_A, 1, 72, 5);
    input_usb_event(INPUT_KEY, INPUT_KEY_A, 1, 73, 6);
    input_usb_event(INPUT_KEY, INPUT_KEY_A, 0, 72, 7);
    CHECK(native.queue.stats.keys_down == 1);
    input_usb_event(INPUT_KEY, INPUT_KEY_A, 2, 73, 8);
    CHECK(native.queue.stats.repeats == 1);
    input_usb_event(INPUT_KEY, INPUT_KEY_A, 0, 73, 9);
    CHECK(!native.queue.stats.keys_down);
    while (input_read(&e)) ;
    input_usb_event(INPUT_BTN, INPUT_LEFT, 1, 73, 10);
    mouse_byte(&native, 9, 10); mouse_byte(&native, 0, 10); mouse_byte(&native, 0, 10);
    CHECK(input_read(&e) && e.type == INPUT_BTN && e.value == 1);
    input_usb_event(INPUT_BTN, INPUT_LEFT, 0, 73, 11);
    CHECK(!input_read(&e) && native.queue.stats.buttons == 1);
    mouse_byte(&native, 8, 12); mouse_byte(&native, 0, 12); mouse_byte(&native, 0, 12);
    CHECK(input_read(&e) && e.type == INPUT_BTN && !e.value);
    input_usb_event(INPUT_REL, INPUT_WHEEL, -2, 73, 13);
    CHECK(input_read(&e) && e.type == INPUT_REL && e.code == INPUT_WHEEL && e.value == -2 && e.source == INPUT_USB);
    g_boot.input_policy = CBI_INPUT_FIRMWARE;
    input_usb_event(INPUT_KEY, INPUT_KEY_A, 1, 73, 14);
    CHECK(!input_read(&e) && !native.queue.stats.keys_down);
    CHECK(!hw.reads && !hw.writes && !pic_changes);
    input_devices_changed(INPUT_USB,false);
    input_devices_snapshot(&devices);CHECK(!devices.ps2 && !devices.firmware && !devices.usb);
    CHECK(strstr(last_log,"[input] devices=0 source=usb event=detach"));
    printf("USB shared input: PASS (PS/2 absence, merged keys/buttons, two keyboards, repeat, wheel, firmware guard)\n");
}

#include "../../tests/host/i8042_touchpad_test.inc"

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
    test_mouse_negotiation();
    test_extended_mouse_packets();
    test_touchpad_protocols();
    test_advisory_interfaces_and_controller_acks();
    test_irqoff_sampling();
    test_hook_selection();
    test_commands();
    test_keyboard_and_mouse();
    test_queue_and_stimulus();
    test_probe_records();
    test_operator_criteria();
    test_native_fault_probe();
    test_firmware_fault_probe(false, false, false);
    test_firmware_fault_probe(true, false, false);
    test_firmware_fault_probe(false, true, false);
    test_firmware_fault_probe(false, false, true);
    test_fault_probe_refusals();
    test_firmware_mapping();
    test_usb_shared_queue();
    i8042_fault_end();
    printf("i8042: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
