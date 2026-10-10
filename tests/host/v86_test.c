/* T0: production V86 monitor, firmware adapter and worker with fake CPU,
 * scheduler, physical pages and ports. No second decoder/state machine.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <ciuki/kernel.h>
#include <ciuki/task.h>
#include <ciuki/registry.h>
#include <ciuki/biosvm.h>
#include <ciuki/fwinput.h>
#include <ciuki/process.h>

static unsigned checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #c); failures++; } } while (0)
static uint8_t ram[4 * 1024 * 1024];
static uint8_t physical[65536];
static unsigned hw_reads, hw_writes, enters;
static uint32_t flags = V86_IF, cr3, esp0, alloc_page = 0x200000;
static struct task caller, worker_fake, native_fake, adapter_fake;
static unsigned proc_switches, vm_preemptions;
static bool native_tls;
static irq_handler_t irq_fake[16];
static unsigned execution_fault;
static jmp_buf leave_env;
static char diagnostic[128][240];
static unsigned diagnostic_count, diagnostic_max;
static gen_t next_generation;
static bool fail_alloc, fail_task;
static bool fail_low_pt, reject_queue;
static uint32_t cr4_fake, setup_ticks, completion_delay;
static int mouse_error_function = -1;
static uint16_t mouse_error_ax, mouse_error_flags;
static unsigned mouse_calls;
static bool worker_until_idle;
static jmp_buf worker_idle_env;
static unsigned worker_waits, worker_sleeps, reflection_ticks;

struct task *g_current;
volatile uint64_t g_ticks;
volatile bool g_need_resched;
struct ciuki_boot_info g_boot;
bool g_cpu_tsc;
uint64_t g_tsc_per_ms;

#define CIUKI_CPU_H
#define P2V(p) ((void *)(ram + (p)))
#define V2P(p) ((uint32_t)((uint8_t *)(p) - ram))
static uint32_t read_eflags(void) { return flags; }
static uint32_t irq_save(void) { uint32_t f = flags; flags = 0; return f; }
static void irq_restore(uint32_t f) { flags = f; }
static void cli(void) { flags = 0; }
static void sti(void) { flags = V86_IF; }
static uint32_t read_cr3(void) { return cr3; }
static void write_cr3(uint32_t x) { cr3 = x; }
static uint32_t read_cr4(void) { return cr4_fake; }
static uint32_t read_cr2(void) { return 0xDEAD000; }
static uint8_t inb(uint16_t p) { hw_reads++; return physical[p]; }
static void outb(uint16_t p, uint8_t b) { hw_writes++; physical[p] = b; }
static uint32_t inl(uint16_t p) { (void)p; CHECK(false); return UINT32_MAX; }
static void outl(uint16_t p, uint32_t value) { (void)p; (void)value; CHECK(false); }

void klog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    unsigned slot = diagnostic_count++ % ARRAY_SIZE(diagnostic);
    int n = vsnprintf(diagnostic[slot], sizeof(diagnostic[slot]), fmt, ap);
    va_end(ap);
    CHECK(n >= 0 && n < 240);
    if (n > 0 && (unsigned)n > diagnostic_max) diagnostic_max = (unsigned)n;
}
static bool logged(const char *text)
{
    for (unsigned i = 0; i < ARRAY_SIZE(diagnostic); i++)
        if (strstr(diagnostic[i], text)) return true;
    return false;
}
void panic(const char *fmt, ...) { (void)fmt; CHECK(false); exit(2); }
void stackprot_init(void) {}
gen_t gen_alloc(void) { return next_generation == UINT32_MAX ? 0 : ++next_generation; }
bool gen_matches(gen_t a, gen_t b) { return a && a == b; }
uint64_t ktime_cycles(void) { return 0; }
bool ktime_elapsed_us(uint64_t start, uint32_t us) { (void)start; (void)us; return false; }
int kwork_init(void) { return 0; }
void kwork_yield(void) { CHECK(false); }
bool kwork_queue(void (*fn)(void *), void *arg) { (void)fn; (void)arg; CHECK(false); return false; }

void tss_set_kernel_stack(uint32_t x) { esp0 = x; }
uint64_t deadline_after_ms(uint32_t ms) { return g_ticks + ms; }
bool deadline_passed(uint64_t d) { return g_ticks - d < (1ull << 63); }
void kwait_init(struct kwait *q) { q->head = 0; }
void kwait_wake_all(struct kwait *q) { (void)q; }
void kmutex_init(struct kmutex *m) { m->owner = 0; }
void kmutex_lock(struct kmutex *m) { CHECK(flags & V86_IF); CHECK(!m->owner); m->owner = g_current; }
void kmutex_unlock(struct kmutex *m) { CHECK(m->owner == g_current); m->owner = 0; }
/* The real process/TLS hook is exercised by proc_test. This scheduler fake
 * checks its composition with the real BIOS VM hooks across both directions
 * of a preemption, including the suspended worker's continuation stack. */
void proc_task_switch(struct task *next)
{
    proc_switches++;
    native_tls = next == &native_fake;
    CHECK(cr3 == biosvm_task_cr3(next, next->user ? next->as.pd_phys : 0));
    CHECK(esp0 == biosvm_task_esp0(next, (uint32_t)(uintptr_t)next->kstack + KSTACK_SIZE));
}
static void switch_fake(struct task *next)
{
    g_current = next;
    write_cr3(biosvm_task_cr3(next, next->user ? next->as.pd_phys : 0));
    tss_set_kernel_stack(biosvm_task_esp0(next, (uint32_t)(uintptr_t)next->kstack + KSTACK_SIZE));
    proc_task_switch(next);
}
void schedule(void)
{
    CHECK(flags & V86_IF);
    struct task *old = g_current;
    uint32_t old_cr3 = cr3, old_esp0 = esp0;
    switch_fake(&native_fake);
    CHECK(native_tls && cr3 == native_fake.as.pd_phys);
    CHECK(esp0 == (uint32_t)(uintptr_t)native_fake.kstack + KSTACK_SIZE);
    switch_fake(old);
    CHECK(!native_tls && cr3 == old_cr3 && esp0 == old_esp0);
    vm_preemptions++;
    g_need_resched = false;
}
uint32_t pmm_alloc(void)
{
    if (fail_alloc || (fail_low_pt && alloc_page == 0x201000)) return 0;
    uint32_t p = alloc_page; alloc_page += PAGE_SIZE; CHECK(alloc_page < sizeof(ram)); return p;
}
void pmm_free(uint32_t p) { CHECK(p >= 0x200000 && p < alloc_page); }
bool pmm_is_reserved(uint32_t p) { return p < 0x100000; }
int as_create(struct aspace *as) { as->pd_phys = pmm_alloc(); if (!as->pd_phys) return -ENOMEM; memset(P2V(as->pd_phys), 0, PAGE_SIZE); return 0; }
struct task *task_create_kernel(const char *n, void (*fn)(void *), void *a, enum task_prio p)
{
    (void)fn; (void)a;
    if (fail_task) return 0;
    struct task *t = !strcmp(n, "firmware-queue") ? &adapter_fake : &worker_fake;
    *t = (struct task){.state = T_BLOCKED, .prio = p, .kstack = (void *)(uintptr_t)0xF0001000};
    return t;
}
void task_start(struct task *t) { CHECK(t->state == T_BLOCKED); t->state = T_READY; }
void task_kill(struct task *t, int c) { (void)c; t->state = T_ZOMBIE; }
void task_reap(struct task *t) { CHECK(t->state == T_ZOMBIE); }
void irq_set_handler(unsigned n, irq_handler_t h) { CHECK(n < 16); irq_fake[n] = h; }
void pic_unmask(unsigned n)
{
    CHECK(irq_fake[n] != 0);
    physical[n < 8 ? 0x21 : 0xA1] &= (uint8_t)~(1u << (n & 7));
    if (n >= 8) physical[0x21] &= (uint8_t)~4u;
}
void pic_mask(unsigned n) { CHECK(n < 16); }
void panic_frame(struct trap_frame *tf, const char *s) { (void)tf; fprintf(stderr, "PANIC %s\n", s); exit(2); }
__asm__(".pushsection .rodata\n.globl biosvm_mouse_stub, biosvm_mouse_stub_end\nbiosvm_mouse_stub:\n.byte 0xcb\nbiosvm_mouse_stub_end:\n.popsection\n");

#include "../../src/kernel/core/registry.c"
#include "../../src/kernel/vm/v86.c"
#include "../../src/kernel/vm/biosvm.c"
#include "../../src/kernel/vm/fwinput.c"

/* Exercise the real boot/adapter/safe path. Only the public input queue and
 * unrelated storage/display boundaries are fake; i8042_test covers the queue. */
#include <ciuki/input.h>
#include <ciuki/i8042.h>
#include <ciuki/fbdev.h>
#include <ciuki/storage.h>
static struct i8042_stats public_input;
static unsigned public_events;
static struct fwinput_event public_delivered[16];
static bool safe_input, safe_backend, safe_pass;
void task_sleep_ms(uint32_t ms) { g_ticks += ms; if (worker_until_idle) worker_sleeps++; }
bool input_firmware_begin(gen_t gen)
{
    CHECK(gen && !public_input.active);
    if (reject_queue) return false;
    public_input = (struct i8042_stats){.active = true, .firmware = true, .generation = gen};
    return true;
}
void input_firmware_event(const struct fwinput_event *e, gen_t gen)
{
    CHECK(e && gen == public_input.generation);
    if (public_events < ARRAY_SIZE(public_delivered)) public_delivered[public_events] = *e;
    public_events++;
}
void input_firmware_loss(uint64_t lost) { (void)lost; }
void input_firmware_disable(gen_t gen)
{ CHECK(gen == public_input.generation); public_input.active = false; public_input.quarantined = true; }
void i8042_snapshot(struct i8042_stats *out) { *out = public_input; }
int i8042_init(void) { CHECK(false); return -ENOSYS; }
int fbdev_init(void) { CHECK(false); return -ENOSYS; }
const struct fb_device *fbdev_get(void) { static struct fb_device fb; return &fb; }
int ata_init(void) { CHECK(false); return -ENOSYS; }
struct ata_device *ata_device_get(unsigned c, unsigned u) { (void)c; (void)u; CHECK(false); return 0; }
void storage_init(void) {}
struct storage *storage_get(void) { static struct storage s; return &s; }
void file_clock_start(int64_t epoch, bool qualified) { (void)epoch; (void)qualified; }
int files_bootstrap(struct vfs *vfs) { CHECK(vfs); return 0; }
void console_write(const char *s, size_t n) { CHECK(s && n); }
int probe_bootlog(void) { CHECK(false); return 1; }
void rec_emit(const char *probe, const char *event, const char *fmt, ...)
{
    char line[240];
    va_list ap;
    va_start(ap, fmt);
    int n = fmt ? vsnprintf(line, sizeof(line), fmt, ap) : 0;
    va_end(ap);
    CHECK(n >= 0 && n < 240 && !strcmp(probe, "safe"));
    if (!fmt) return;
    safe_input |= strstr(line, "input_works=1") != 0;
    safe_backend |= strstr(line, "input=1 backend=firmware") != 0;
    safe_pass |= !strcmp(event, "END") && strstr(line, "status=PASS") != 0;
}
#include "../../src/kernel/drivers/fwinput_adapter.c"
#include "../../src/kernel/core/init.c"
#include "../../src/kernel/probes/safe_probe.c"

void v86_leave(uint32_t saved)
{
    CHECK(saved == 0x12345678);
    longjmp(leave_env, 1);
}

void v86_enter(const struct v86_frame *initial, uint32_t *saved)
{
    CHECK(g_current == vm_thread);
    CHECK(request_mutex.owner != g_current);
    CHECK(cr3 == vm_as.pd_phys);
    enters++;
    *saved = 0x12345678;
    tss_set_kernel_stack(*saved);
    g_need_resched = true;
    struct v86_frame live = *initial;
    unsigned function = initial->tf.eax & 0xFF;
    bool mouse_setup_call = initial->tf.cs == 0xF000 && initial->tf.eip == 0x400 &&
                            (initial->tf.eax & 0xFF00) == 0xC200;
    if (mouse_setup_call) mouse_calls++;
    if (setjmp(leave_env)) {
        sti();
        return;
    }
    for (unsigned steps = 0; steps < 1000; steps++) {
        uint8_t *code = vm_memory(0, (live.tf.cs << 4) + live.tf.eip, 2, false);
        CHECK(code != 0);
        if (!code)
            exit(2);
        if (code[0] == 0xCF && !request_regs.interrupt)
            g_ticks += reflection_ticks;
        if (mouse_setup_call && code[0] == 0xCF) {
            g_ticks += setup_ticks;
            if ((int)function == mouse_error_function) {
                live.tf.eax = mouse_error_ax;
                uint8_t *saved_flags = vm_memory(0, (live.tf.user_ss << 4) + live.tf.user_esp + 4, 2, true);
                CHECK(saved_flags);
                putword(saved_flags, 2, getword(saved_flags, 2) | mouse_error_flags);
            }
        }
        if (execution_fault == 1) {
            live.tf.vector = 14;
        } else if (execution_fault == 3) {
            live.tf.vector = 6;
        } else if (execution_fault == 2) {
            /* Non-trapping CLI/JMP-style firmware overrun: timer catches it. */
            g_ticks = firmware.deadline;
            live.tf.vector = 0x20;
        } else if (code[0] == 0xB0) {
            live.tf.eax = (live.tf.eax & ~0xFFu) | code[1];
            live.tf.eip += 2;
            continue;
        } else if (code[0] == 0xB8 || code[0] == 0xBA) {
            uint32_t *reg = code[0] == 0xB8 ? &live.tf.eax : &live.tf.edx;
            *reg = (*reg & ~0xFFFFu) | getword(code + 1, 2);
            live.tf.eip += 3;
            continue;
        } else if (code[0] == 0x8E && code[1] == 0xD8) {
            live.ds = (uint16_t)live.tf.eax;
            live.tf.eip += 2;
            continue;
        } else if (code[0] == 0xA3) {
            /* Ordinary guest MOV [moffs16],AX; the V86 CPU executes this
             * without a trap. Use the production VM mapping permission. */
            uint8_t *p = vm_memory(0, (live.ds << 4) + getword(code + 1, 2), 2, true);
            CHECK(p != 0);
            if (!p) exit(2);
            putword(p, 2, live.tf.eax);
            live.tf.eip += 3;
            continue;
        } else if (code[0] == 0x80 && code[1] == 0xFC) {
            live.tf.eflags &= ~V86_ZF;
            if (((live.tf.eax >> 8) & 0xFF) == code[2]) live.tf.eflags |= V86_ZF;
            live.tf.eip += 3;
            continue;
        } else if (code[0] == 0x74) {
            live.tf.eip += 2;
            if (live.tf.eflags & V86_ZF) live.tf.eip += (int8_t)code[1];
            continue;
        } else if (code[0] == 0x90) {
            live.tf.eip++;
            if (!firmware.shadow)
                continue;
            live.tf.vector = 1;
        } else {
            live.tf.vector = 13;
        }
        biosvm_trap(&live.tf);
        if (!execution_fault && code[0] == 0xE4)
            CHECK((live.tf.eax & 0xFF) == physical[code[1]]);
        sti();
    }
    CHECK(false);
    exit(2);
}

bool kwait_wait_until(struct kwait *q, kwait_cond_fn cond, void *arg, uint64_t d)
{
    CHECK(flags & V86_IF);
    if (q == &wake && worker_until_idle) {
        CHECK(++worker_waits <= 4); /* stuck readiness must fail, not hang */
        if (worker_waits > 4) exit(2);
        if (!cond(arg)) longjmp(worker_idle_env, 1);
        return true;
    }
    if (q == &completion && request_pending) {
        struct task *old = g_current;
        switch_fake(vm_thread);
        request_result = run_vm(&request_regs, request_ms, request_test);
        request_pending = false;
        request_done = true;
        switch_fake(old);
        if (request_regs.interrupt == 0x15) g_ticks += completion_delay;
    } else if (!cond(arg)) {
        g_ticks = d;
    }
    return cond(arg);
}

/* Decoder tests use a memory policy unrelated to the runtime's layout. */
static uint8_t permissions[256];
static void *test_memory(void *arg, uint32_t linear, unsigned n, bool write)
{
    (void)arg;
    if (!n || linear >= 0x100000 || n > 0x100000 - linear)
        return 0;
    for (unsigned p = linear / PAGE_SIZE; p <= (linear + n - 1) / PAGE_SIZE; p++)
        if (!(permissions[p] & 1) || (write && !(permissions[p] & 2)))
            return 0;
    return ram + linear;
}
static uint8_t test_in(void *arg, uint16_t p) { (void)arg; return inb(p); }
static void test_out(void *arg, uint16_t p, uint8_t b) { (void)arg; outb(p, b); }
static struct v86 v;
static struct v86_frame f;
static void fixture(const uint8_t *code, unsigned n)
{
    memset(ram, 0, 0x100000);
    memset(permissions, 3, sizeof(permissions));
    const struct v86_ops ops = {test_memory, test_in, test_out, 0};
    v86_init(&v, &ops);
    CHECK(v86_begin(&v, 0, 100) == 0);
    f = (struct v86_frame){.tf = {.cs = 0x2000, .eip = 0x100, .eflags = V86_VM | V86_IF | 2,
                                    .user_ss = 0x3000, .user_esp = 0x1000}, .ds = 0x4000, .es = 0x5000, .fs = 0x6000, .gs = 0x7000};
    memcpy(ram + 0x20100, code, n);
    hw_writes = hw_reads = 0;
}
#define FIX(...) do { const uint8_t c[] = {__VA_ARGS__}; fixture(c, sizeof(c)); } while (0)
static void ivt(unsigned vector, uint16_t cs, uint16_t ip)
{
    ram[vector * 4] = (uint8_t)ip; ram[vector * 4 + 1] = ip >> 8;
    ram[vector * 4 + 2] = (uint8_t)cs; ram[vector * 4 + 3] = cs >> 8;
}

static void test_flags_interrupts(void)
{
    FIX(0xFA, 0xFB, 0x9C, 0x9D);
    CHECK(v86_emulate(&v, &f) == 0 && !v.vif && (f.tf.eflags & V86_IF));
    CHECK(v86_emulate(&v, &f) == 0 && v.vif && v.shadow && (f.tf.eflags & V86_TF));
    CHECK(v86_emulate(&v, &f) == 0 && !v.shadow && !(f.tf.eflags & V86_TF));
    CHECK(f.tf.user_esp == 0xFFE && getword(ram + 0x30FFE, 2) == 0x202);
    putword(ram + 0x30FFE, 2, 0xFFFF);
    CHECK(v86_emulate(&v, &f) == 0 && v.vif && !(f.tf.eflags & (0x3000 | 0x4000 | V86_TF)));
    FIX(0x66, 0x9C, 0x66, 0x9D);
    CHECK(v86_emulate(&v, &f) == 0 && f.tf.user_esp == 0xFFC);
    CHECK(getword(ram + 0x30FFC, 4) == 0x202);
    putword(ram + 0x30FFC, 4, UINT32_MAX & ~V86_IF);
    CHECK(v86_emulate(&v, &f) == 0 && !v.vif && (f.tf.eflags & V86_IF));
    FIX(0xCD, 0x16);
    ivt(0x16, 0xF000, 0x1234);
    v.sentinel_cs = 0x2000; v.sentinel_ip = 0x102; v.sentinel_ss = 0x3000; v.sentinel_sp = 0x1000;
    CHECK(v86_emulate(&v, &f) == 0 && f.tf.cs == 0xF000 && f.tf.eip == 0x1234 && !v.vif);
    CHECK(f.tf.user_esp == 0xFFA && getword(ram + 0x30FFA, 2) == 0x102);
    CHECK(getword(ram + 0x30FFC, 2) == 0x2000 && getword(ram + 0x30FFE, 2) == 0x202);
    ram[0xF1234] = 0xCF;
    CHECK(v86_emulate(&v, &f) == 1 && v.state == V86_DONE && f.tf.user_esp == 0x1000);
    FIX(0xCD, 0x16); ivt(0x16, 0xF000, 0x1234); f.tf.user_esp = 4;
    CHECK(!v86_emulate(&v, &f) && f.tf.user_esp == 0xFFFE);
    CHECK(getword(ram + 0x3FFFE, 2) == 0x102 && getword(ram + 0x30000, 2) == 0x2000);
    ram[0xF1234] = 0xCF;
    CHECK(!v86_emulate(&v, &f) && f.tf.user_esp == 4 && f.tf.cs == 0x2000 && f.tf.eip == 0x102);
    FIX(0x66, 0xCF);
    putword(ram + 0x31000, 4, 0x888); putword(ram + 0x31004, 4, 0x1234);
    putword(ram + 0x31008, 4, 0xFFFFFFFF);
    CHECK(v86_emulate(&v, &f) == 0 && f.tf.cs == 0x1234 && f.tf.eip == 0x888 && f.tf.user_esp == 0x100C);
    CHECK((f.tf.eflags & (0x3000 | 0x4000 | V86_TF)) == 0);
    FIX(0xCC); ivt(3, 0xF000, 0x222);
    CHECK(v86_emulate(&v, &f) == 0 && f.tf.eip == 0x222);
    FIX(0xCE); ivt(4, 0xF000, 0x333);
    CHECK(v86_emulate(&v, &f) == 0 && f.tf.eip == 0x101);
    f.tf.eip = 0x100; f.tf.eflags |= 0x800;
    CHECK(v86_emulate(&v, &f) == 0 && f.tf.eip == 0x333);
    FIX(0xFA, 0xFB, 0xFB);
    CHECK(!v86_emulate(&v, &f)); CHECK(!v86_emulate(&v, &f));
    CHECK(!v86_emulate(&v, &f) && !v.shadow && !(f.tf.eflags & V86_TF));
    FIX(0xFA, 0xFB);
    CHECK(!v86_emulate(&v, &f)); CHECK(!v86_emulate(&v, &f));
    CHECK(!v86_debug_step(&v, &f) && !v.shadow);
    FIX(0x0F, 0x20, 0xC0);
    CHECK(v86_emulate(&v, &f) == -EFAULT && v.fault.cs == 0x2000 && v.fault.ip == 0x100 && v.fault.bytes[0] == 0x0F);
    FIX(0xF0, 0xFA);
    CHECK(v86_emulate(&v, &f) == -EFAULT);
    FIX(0x66, 0xCF); putword(ram + 0x31000, 4, 0x10000);
    CHECK(v86_emulate(&v, &f) == -EFAULT);
    FIX(0xCD, 9); ivt(9, 0xF000, 1); permissions[0x30] = 1;
    CHECK(v86_emulate(&v, &f) == -EFAULT && f.tf.user_esp == 0x1000);
    uint8_t prefixes[16]; memset(prefixes, 0x66, sizeof(prefixes)); prefixes[15] = 0xFA;
    fixture(prefixes, sizeof(prefixes));
    CHECK(v86_emulate(&v, &f) == -EFAULT);
}

static void test_io_decoder(void)
{
    static const uint8_t ops[] = {0xE4,0xE5,0xE6,0xE7,0xEC,0xED,0xEE,0xEF};
    for (unsigned k = 0; k < ARRAY_SIZE(ops); k++) {
        for (unsigned size = 0; size < 2; size++) {
            uint8_t code[] = {0x66, ops[k], 0x60};
            fixture(code + !size, sizeof(code) - !size);
            f.tf.edx = 0x60; f.tf.eax = 0xABCDEF7B; physical[0x60] = 0x55;
            int rc = v86_emulate(&v, &f);
            if (ops[k] & 1) {
                CHECK(rc == -V86_EPERM && !hw_reads && !hw_writes);
            } else {
                CHECK(rc == 0);
                CHECK((ops[k] & 2) ? hw_writes == 1 && physical[0x60] == 0x7B :
                                      hw_reads == 1 && f.tf.eax == 0xABCDEF55);
                CHECK(f.tf.eip == 0x100 + size + (ops[k] < 0xE8 ? 2u : 1u));
            }
        }
    }
    static const uint8_t prefixes[] = {0x26,0x2E,0x36,0x3E,0x64,0x65};
    static const uint32_t bases[] = {0x50000,0x20000,0x30000,0x40000,0x60000,0x70000};
    for (unsigned k = 0; k < ARRAY_SIZE(prefixes); k++) {
        uint8_t code[] = {prefixes[k], 0xF3, 0x6E}; fixture(code, sizeof(code));
        f.tf.esi = 0x20; f.tf.ecx = 2; f.tf.edx = 0x64;
        ram[bases[k] + 0x20] = 0x11; ram[bases[k] + 0x21] = 0x22;
        CHECK(!v86_emulate(&v, &f) && f.tf.eip == 0x100 && f.tf.ecx == 1 && physical[0x64] == 0x11);
        CHECK(!v86_emulate(&v, &f) && f.tf.eip == 0x103 && f.tf.ecx == 0 && physical[0x64] == 0x22);
    }
    FIX(0x67, 0xF2, 0x26, 0x6C);
    f.tf.edx = 0x60; f.tf.edi = 0x33; f.tf.ecx = 2; physical[0x60] = 0xAB;
    f.tf.eflags |= 0x400;
    CHECK(!v86_emulate(&v, &f) && ram[0x50033] == 0xAB && f.tf.edi == 0x32);
    CHECK(!v86_emulate(&v, &f) && ram[0x50032] == 0xAB && f.tf.ecx == 0);
    FIX(0xF3, 0x6C); f.tf.ecx = 0; f.tf.edi = UINT32_MAX;
    CHECK(!v86_emulate(&v, &f) && !hw_reads && f.tf.eip == 0x102);
    FIX(0x67, 0x6C); f.tf.edx = 0x60; f.tf.edi = 0x10000;
    CHECK(v86_emulate(&v, &f) == -EFAULT && !hw_reads);
    FIX(0x6C); f.tf.edx = 0x60; permissions[0x50] = 1;
    CHECK(v86_emulate(&v, &f) == -EFAULT && !hw_reads);
    FIX(0x6D); f.tf.edx = 0x60;
    CHECK(v86_emulate(&v, &f) == -V86_EPERM && !hw_reads);
    FIX(0x66, 0x6F); f.tf.edx = 0x60;
    CHECK(v86_emulate(&v, &f) == -V86_EPERM && !hw_writes);
    FIX(0x6E); f.tf.edx = 0x60; f.tf.esi = 0xFFFF; ram[0x4FFFF] = 0x12;
    CHECK(!v86_emulate(&v, &f) && (uint16_t)f.tf.esi == 0 && physical[0x60] == 0x12);
}

static void port_write(unsigned p, unsigned x) { uint32_t value = x; CHECK(!v86_io(&v, (uint16_t)p, 1, true, &value)); }
static unsigned port_read(unsigned p) { uint32_t value = 0; CHECK(!v86_io(&v, (uint16_t)p, 1, false, &value)); return value; }
static void test_devices_deadline(void)
{
    FIX(0xF4);
    ivt(9, 0xF000, 0x100); ivt(0x74, 0xF000, 0x200);
    v86_irq_raise(&v, 12); v86_irq_raise(&v, 1);
    CHECK(v86_irq_deliver(&v, &f) == 1 && f.tf.eip == 0x100 && v.pic[0].isr == 2);
    v.vif = true;
    CHECK(v86_irq_deliver(&v, &f) == 0); /* IRQ1 in service blocks IRQ2 */
    port_write(0x20, 0x0B); CHECK(port_read(0x20) == 2);
    port_write(0x20, 0x61);
    CHECK(v86_irq_deliver(&v, &f) == 1 && f.tf.eip == 0x200 && v.pic[0].isr == 4 && v.pic[1].isr == 16);
    port_write(0xA0, 0x20); CHECK(!v.pic[1].isr && v.pic[0].isr == 4);
    port_write(0x20, 0x62); CHECK(!v.pic[0].isr);
    port_write(0x21, 0xFF); CHECK(port_read(0x21) == 0xFF);
    v86_irq_raise(&v, 1); v.vif = true;
    CHECK(!v86_irq_deliver(&v, &f));
    port_write(0x20, 0x0A); CHECK(port_read(0x20) & 2);
    port_write(0x20, 0x11); port_write(0x21, 0x28); port_write(0x21, 4); port_write(0x21, 1);
    CHECK(v.pic[0].base == 0x28 && !v.pic[0].init && !v.pic[0].auto_eoi);
    port_write(0x20, 0xC1); CHECK(v.pic[0].priority == 1);
    port_write(0x20, 0x0C); v86_irq_raise(&v, 3);
    CHECK(port_read(0x20) == 0x83 && v.pic[0].isr == 8);
    port_write(0x20, 0xE3); CHECK(!v.pic[0].isr && v.pic[0].priority == 3);
    v.ticks = 100; uint16_t expected = (uint16_t)(0u - 100u * 1193u);
    port_write(0x43, 0); v.ticks = 500;
    CHECK(port_read(0x40) == (expected & 0xFF)); CHECK(port_read(0x40) == expected >> 8);
    port_write(0x43, 0x36); port_write(0x40, 0x12); port_write(0x61, 0xFF);
    CHECK(port_read(0x61) <= 0x30 && !hw_writes);
    v.rtc_valid = true; v.rtc[0x12] = 0x56;
    port_write(0x70, 0x92); port_write(0x71, 0xAA);
    CHECK(port_read(0x70) == 0x92 && port_read(0x71) == 0x56 && !hw_writes && !hw_reads);
    uint32_t val = 0;
    CHECK(v86_io(&v, 0x80, 1, true, &val) == -V86_EPERM && v.stats.disallowed_io == 1);
    CHECK(v86_io(&v, 0xFFFF, 4, false, &val) == -V86_EPERM && !hw_writes);
    FIX(0xF4);
    CHECK(v86_emulate(&v, &f) == 2 && v.state == V86_HALTED);
    ivt(9, 0xF000, 0x100); v86_irq_raise(&v, 1);
    CHECK(v86_irq_deliver(&v, &f) == 1 && v.state == V86_RUNNING);
    CHECK(!v86_check_deadline(&v, 99));
    CHECK(v86_check_deadline(&v, 100) == -V86_ETIMEDOUT && v.state == V86_DISABLED);
    CHECK(v86_check_deadline(&v, 101) == -V86_ETIMEDOUT && v.stats.timeouts == 1);
    CHECK(v86_begin(&v, 102, 100) == -V86_EIO);
    v.state = V86_IDLE;
    CHECK(!v86_begin(&v, UINT64_MAX - 10, 20));
    CHECK(!v86_check_deadline(&v, UINT64_MAX)); CHECK(v86_check_deadline(&v, 9) == -V86_ETIMEDOUT);
    /* Exhaustive byte-port policy: no other port can reach hardware,
     * irrespective of the virtual device's current register state. */
    FIX(0xFA);
    v.rtc_valid = true;
    for (unsigned port = 0; port < 65536; port++) {
        unsigned before = hw_writes;
        val = 0;
        int rc = v86_io(&v, (uint16_t)port, 1, true, &val);
        bool pass = port == 0x60 || port == 0x64;
        bool modeled = port == 0x20 || port == 0x21 || port == 0xA0 || port == 0xA1 ||
                       (port >= 0x40 && port <= 0x43) || port == 0x61 || port == 0x70 || port == 0x71;
        CHECK(hw_writes == before + pass && rc == (pass || modeled ? 0 : -V86_EPERM));
    }
    CHECK(hw_writes == 2);
    FIX(0xFA, 0xFB, 0xF4); ivt(9, 0xF000, 0x55);
    CHECK(!v86_emulate(&v, &f)); v86_irq_raise(&v, 1);
    CHECK(!v86_irq_deliver(&v, &f));
    CHECK(!v86_emulate(&v, &f) && v.shadow);
    CHECK(!v86_irq_deliver(&v, &f));
    CHECK(v86_emulate(&v, &f) == 2 && !v.shadow);
    CHECK(v86_irq_deliver(&v, &f) == 1 && f.tf.eip == 0x55);
}

static void test_input_decoders(void)
{
    struct fwinput_decoder d = {0};
    struct fwinput_event ev[FWINPUT_CAPACITY + 1];
    fwinput_decode_scan(&d, 0x1E, 1); fwinput_decode_scan(&d, 0x1E, 2);
    fwinput_decode_bios(&d, 0x1E61, 2); fwinput_decode_scan(&d, 0x9E, 3);
    CHECK(fwinput_decode_poll(&d, ev, 10) == 3);
    CHECK(ev[0].type == FWINPUT_KEY && ev[0].code == 0x1E && ev[0].value == 1);
    CHECK(ev[1].type == FWINPUT_TEXT && ev[1].value == 'a');
    CHECK(ev[2].type == FWINPUT_KEY && ev[2].value == 0 && ev[2].tick == 3);
    fwinput_decode_scan(&d, 0xE0, 4); fwinput_decode_scan(&d, 0x1D, 4);
    fwinput_decode_scan(&d, 0xE0, 5); fwinput_decode_scan(&d, 0x9D, 5);
    CHECK(fwinput_decode_poll(&d, ev, 10) == 2 && ev[0].code == 0x11D && ev[1].value == 0);
    static const uint8_t pause[] = {0xE1,0x1D,0x45,0xE1,0x9D,0xC5};
    for (unsigned i = 0; i < sizeof(pause); i++) fwinput_decode_scan(&d, pause[i], 6);
    CHECK(fwinput_decode_poll(&d, ev, 10) == 2 && ev[0].code == 0x145 && ev[1].value == 0);
    fwinput_decode_scan(&d, 0xFA, 7); fwinput_decode_scan(&d, 0xFE, 7);
    fwinput_decode_scan(&d, 0xE0, 7); fwinput_decode_scan(&d, 0x2A, 7);
    fwinput_decode_bios(&d, 0x8500, 7); fwinput_decode_bios(&d, 0x48E0, 7);
    CHECK(!fwinput_decode_poll(&d, ev, 10)); /* no fabricated BIOS breaks */
    fwinput_decode_mouse(&d, 0, 8);
    fwinput_decode_mouse(&d, 0x29, 8); fwinput_decode_mouse(&d, 2, 8); fwinput_decode_mouse(&d, 0xFF, 8);
    CHECK(fwinput_decode_poll(&d, ev, 10) == 3);
    CHECK(ev[0].code == FWINPUT_X && ev[0].value == 2);
    CHECK(ev[1].code == FWINPUT_Y && ev[1].value == 1);
    CHECK(ev[2].type == FWINPUT_BUTTON && ev[2].value == 1);
    fwinput_decode_mouse(&d, 8, 9); fwinput_decode_mouse(&d, 200, 9); fwinput_decode_mouse(&d, 128, 9);
    CHECK(fwinput_decode_poll(&d, ev, 10) == 3 && ev[0].value == 200 && ev[1].value == -128 && ev[2].value == 0);
    fwinput_decode_mouse(&d, 0xC8, 10); fwinput_decode_mouse(&d, 0, 10); fwinput_decode_mouse(&d, 0, 10);
    CHECK(fwinput_decode_poll(&d, ev, 10) == 1 && ev[0].type == FWINPUT_RESYNC);
    CHECK(d.stats.loss == 1 && d.stats.malformed == 2);
    for (unsigned i = 0; i < FWINPUT_CAPACITY + 1; i++) fwinput_decode_bios(&d, 0x1E61, 11);
    CHECK(d.stats.loss == FWINPUT_CAPACITY + 1);
    CHECK(fwinput_decode_poll(&d, ev, FWINPUT_CAPACITY + 1) == 2 && ev[0].type == FWINPUT_RESYNC);
    for (unsigned i = 0; i < FWINPUT_CAPACITY; i++) fwinput_decode_bios(&d, 0x1E61, 12);
    fwinput_decode_scan(&d, 0x1E, 13); fwinput_decode_scan(&d, 0x9E, 14);
    CHECK(fwinput_decode_poll(&d, ev, 10) == 3 && ev[0].type == FWINPUT_RESYNC &&
          ev[0].tick == 13 && ev[1].value == 1 && ev[2].value == 0);
    uint64_t matched = d.stats.text_matched;
    fwinput_decode_bios(&d, 0x1E61, 15);
    CHECK(d.stats.text_matched == matched + 1); /* make survived event overflow */
    d.pending_makes[0x1E] = UINT16_MAX;
    fwinput_decode_scan(&d, 0x1E, 16);
    CHECK(d.pending_makes[0x1E] == UINT16_MAX && d.stats.agreement_overflow == 1);
}

static void firmware_byte(uint8_t status, uint8_t byte, unsigned irq)
{
    /* Execute the production reflection/trap path: one firmware status read,
     * one data read, then virtual EOI and IRET. No observer-side port read. */
    static const uint8_t key[] = {0xE4,0x64,0xE4,0x60,0xB0,0x20,0xE6,0x20,0xCF};
    static const uint8_t aux[] = {0xE4,0x64,0xE4,0x60,0xB0,0x20,0xE6,0xA0,0xE6,0x20,0xCF};
    memcpy(ram + 0xF0200, key, sizeof(key));
    memcpy(ram + 0xF0300, aux, sizeof(aux));
    ivt(9, 0xF000, 0x200); ivt(0x74, 0xF000, 0x300);
    physical[0x64] = status; physical[0x60] = byte;
    struct trap_frame input = {.vector = 0x20 + irq};
    irq_fake[irq](&input);
    struct task *old = g_current;
    switch_fake(vm_thread);
    unsigned reads = hw_reads, writes = hw_writes;
    struct biosvm_regs r = {0};
    CHECK(!run_vm(&r, 100, 0));
    CHECK(hw_reads == reads + 2 && hw_writes == writes);
    CHECK(!firmware.pic[0].isr && !firmware.pic[1].isr);
    switch_fake(old);
}

static void test_observed_input(void)
{
    struct fwinput_event ev[FWINPUT_CAPACITY + 1];
    memset(&decoder, 0, sizeof(decoder));
    g_ticks = 200;
    firmware_byte(1, 0x1E, 1);
    firmware_byte(1, 0x1E, 1); /* typematic make: one transition, two texts */
    firmware_byte(1, 0x9E, 1);
    firmware_byte(1, 0xE0, 1);
    firmware_byte(0x21, 0x1E, 12); /* AUX must neither produce keys nor lose E0 */
    firmware_byte(1, 0x1D, 1);
    firmware_byte(1, 0xE0, 1);
    firmware_byte(1, 0x9D, 1);
    CHECK(fwinput_poll(ev, ARRAY_SIZE(ev)) == 4);
    CHECK(ev[0].type == FWINPUT_KEY && ev[0].code == 0x1E && ev[0].value == 1 && ev[0].tick == 200);
    CHECK(ev[1].code == 0x1E && ev[1].value == 0);
    CHECK(ev[2].code == 0x11D && ev[2].value == 1 && ev[3].code == 0x11D && !ev[3].value);
    fwinput_decode_bios(&decoder, 0x1E61, 201);
    fwinput_decode_bios(&decoder, 0x1E61, 201);
    fwinput_decode_bios(&decoder, 0x3062, 201); /* unmatched BIOS text stays visible */
    CHECK(fwinput_poll(ev, ARRAY_SIZE(ev)) == 3 && ev[0].value == 'a' && ev[2].value == 'b');
    struct fwinput_stats stats;
    fwinput_stats(&stats);
    CHECK(stats.observed_bytes == 8 && stats.aux_bytes == 1 && stats.scan_bytes == 7);
    CHECK(stats.makes == 3 && stats.keys == 4 && stats.text_matched == 2 && stats.text_unmatched == 1);
    CHECK(!stats.loss && !stats.resyncs && !stats.packets && !stats.agreement_overflow);

    /* Normal command polling (no IRQ1 in service) must not generate keys,
     * including reply data that looks like a scan or Shift break. */
    static const uint8_t read[] = {0xE4,0x64,0xE4,0x60,0xCF};
    memcpy(ram + 0xF0100, read, sizeof(read));
    static const uint8_t replies[] = {0xFA,0xFE,0xAA,0x1E};
    for (unsigned i = 0; i < sizeof(replies); i++) {
        physical[0x64] = 1; physical[0x60] = replies[i];
        struct biosvm_regs r = {.eax = 0x1100, .interrupt = 0x16};
        unsigned reads = hw_reads;
        CHECK(!biosvm_call(&r, 100) && hw_reads == reads + 2);
    }
    CHECK(!fwinput_poll(ev, ARRAY_SIZE(ev)) && decoder.stats.scan_bytes == 7);
    ram[0xF0100] = 0xCF;

    /* E0 keypad Enter uses BIOS AH=E0; compare its raw position, not AH. */
    firmware_byte(1, 0xE0, 1); firmware_byte(1, 0x1C, 1);
    firmware_byte(1, 0xE0, 1); firmware_byte(1, 0x9C, 1);
    fwinput_decode_bios(&decoder, 0xE00D, 202);
    CHECK(fwinput_poll(ev, ARRAY_SIZE(ev)) == 3 && ev[0].code == 0x11C && ev[2].value == '\r');
    CHECK(decoder.stats.text_matched == 3);

    /* AUX bytes that resemble key/error/ACK bytes do not enter the scan or
     * mouse decoder. Publish one callback packet, and consume it only once. */
    firmware_byte(0x21, 0xFA, 12);
    firmware_byte(0x21, 0x29, 12); firmware_byte(0x21, 2, 12); firmware_byte(0x21, 0xFF, 12);
    CHECK(!fwinput_poll(ev, ARRAY_SIZE(ev)) && !decoder.stats.packets);
    volatile struct biosvm_mouse_ring *ring = P2V(BIOSVM_SCRATCH + BIOSVM_MOUSE_RING);
    ring->packets[0][0] = 0x29; ring->packets[0][1] = 2; ring->packets[0][2] = 0xFF;
    ring->head = 1;
    backend.mouse = true;
    switch_fake(vm_thread);
    service_input();
    service_input();
    switch_fake(&caller);
    CHECK(fwinput_poll(ev, ARRAY_SIZE(ev)) == 3 && decoder.stats.packets == 1);
    CHECK(ev[0].type == FWINPUT_REL && ev[0].value == 2 && ev[1].value == 1 && ev[2].type == FWINPUT_BUTTON);
    backend.mouse = false;

    /* Status errors and malformed sequences reset held state; a fresh make
     * is delivered after the explicit resync. Overflow is observable too. */
    firmware_byte(1, 0xE1, 1); firmware_byte(1, 0x20, 1);
    CHECK(fwinput_poll(ev, ARRAY_SIZE(ev)) == 1 && ev[0].type == FWINPUT_RESYNC);
    firmware_byte(0x81, 0x1E, 1);
    firmware_byte(1, 0x1E, 1); firmware_byte(1, 0x9E, 1);
    CHECK(fwinput_poll(ev, ARRAY_SIZE(ev)) == 3 && ev[0].type == FWINPUT_RESYNC && ev[1].value == 1 && !ev[2].value);
    CHECK(decoder.stats.resyncs == 2 && decoder.stats.loss == 2);
    for (unsigned i = 0; i < FWINPUT_CAPACITY + 2; i++)
        firmware_byte(1, i & 1 ? 0x9E : 0x1E, 1);
    CHECK(fwinput_poll(ev, ARRAY_SIZE(ev)) == 3 && ev[0].type == FWINPUT_RESYNC);
    CHECK(decoder.stats.loss == FWINPUT_CAPACITY + 2);

    /* Missing/freshly consumed status must not reuse an old AUX decision. */
    memcpy(ram + 0xF0200, (uint8_t[]){0xE4,0x60,0xB0,0x20,0xE6,0x20,0xCF}, 7);
    pending_irqs = 2;
    switch_fake(vm_thread);
    struct biosvm_regs r = {0};
    CHECK(!run_vm(&r, 100, 0));
    switch_fake(&caller);
    CHECK(fwinput_poll(ev, ARRAY_SIZE(ev)) == 1 && ev[0].type == FWINPUT_RESYNC);
    CHECK(decoder.stats.resyncs == 4);
}

static void test_runtime_mappings(void)
{
    const uint32_t *pt = P2V(low_pt), *pd = P2V(vm_as.pd_phys);
    CHECK(pd[0] == (low_pt | PTE_P | PTE_U | PTE_W));
    for (unsigned i = 0; i < 1024; i++) {
        bool rw = i == 0 || i == 0x10 || i == 0x11 || i == 0x9F || (i >= 0xC0 && i < 0xF0);
        bool ro = i >= 0xF0 && i < 0x100;
        uint32_t expected = rw || ro ? i * PAGE_SIZE | PTE_P | PTE_U | (rw ? PTE_W : 0) : 0;
        if (i >= 0xC0 && i < 0x100)
            expected |= PTE_PCD | PTE_PWT;
        CHECK(pt[i] == expected);
    }
    CHECK(vm_memory(0, 0xC0000, 0x30000, true) == ram + 0xC0000);
    CHECK(!vm_memory(0, 0xEFFFF, 2, true) && vm_memory(0, 0xEFFFF, 2, false));
    CHECK(vm_memory(0, 0xFFFFF, 1, false) && !vm_memory(0, 0xFFFFF, 1, true));
    struct biosvm_selftest_report report;
    CHECK(check_mappings(&report));
    ((uint32_t *)pd)[0] &= ~PTE_W;
    CHECK(!check_mappings(&report));
    ((uint32_t *)pd)[0] |= PTE_W;
    CHECK(report.mappings[4].start == 0xC0000 && report.mappings[4].end == 0xF0000 &&
          (report.mappings[4].flags & PTE_W) && !(report.mappings[5].flags & PTE_W));
    ((uint32_t *)pt)[0xF0] |= PTE_W;
    CHECK(!check_mappings(&report));
    ((uint32_t *)pt)[0xF0] &= ~PTE_W;
    ((uint32_t *)pt)[0xC0] |= 0x60;
    CHECK(check_mappings(&report)); /* accessed/dirty are normal hardware state */
}

static void test_worker(void)
{
    memset(ram, 0, sizeof(ram));
    putword(ram + 0x40E, 2, 0x9FC0); putword(ram + 0x413, 2, 639); ram[0x9FC00] = 1;
    putword(ram + 0x41A, 2, 0x1E); putword(ram + 0x41C, 2, 0x20);
    ivt(0x16, 0xF000, 0x100); ram[0xF0100] = 0xCF;
    ivt(0x15, 0xF000, 0x100); ivt(9, 0xF000, 0x100); ivt(0x74, 0xF000, 0x100);
    g_current = &caller; caller.state = T_RUNNING; flags = V86_IF;
    caller.kstack = (void *)(uintptr_t)0xE0001000;
    native_fake = (struct task){.user = true, .as = {.pd_phys = 0x330000},
                                .kstack = (void *)(uintptr_t)0xD0001000};
    g_boot.e820_count = 1;
    g_boot.e820[0] = (struct ciuki_e820){0, 0x9FC00, CBI_E820_RAM, 1};
    registry_init();
    CHECK(biosvm_init() == -V86_EPERM);
    g_boot.input_policy = CBI_INPUT_FIRMWARE;
    CHECK(!biosvm_init() && biosvm_backend_state() == BIOSVM_READY);
    test_runtime_mappings();
    CHECK(vm_memory(0, 0, 4, true) == ram);
    CHECK(!vm_memory(0, 0x12000, 1, false) && !vm_memory(0, 0xF0000, 1, true));
    CHECK(vm_memory(0, 0x9FC00, 1, true) == ram + 0x9FC00);
    CHECK(!vm_memory(0, 0xFFFFF, 2, false));
    struct biosvm_regs r = {.eax = 0x1100, .interrupt = 0x16};
    CHECK(!biosvm_call(&r, 100) && !request_mutex.owner && !continuation && cr3 == 0);
    CHECK(biosvm_call(&r, 101) == -V86_EPERM);
    r.interrupt = 0x13; CHECK(biosvm_call(&r, 100) == -V86_EPERM);
    r.interrupt = 0x16; r.eax = 0x1000; putword(ram + 0x41C, 2, 0x1E);
    unsigned previous = enters; CHECK(biosvm_call(&r, 100) == -EINVAL && enters == previous);
    CHECK(biosvm_task_cr3(&caller, 123) == 123 && biosvm_task_esp0(&caller, 456) == 456);
    continuation = 0xABC;
    CHECK(biosvm_task_cr3(vm_thread, 0) == vm_as.pd_phys && biosvm_task_esp0(vm_thread, 0) == 0xABC);
    continuation = 0;
    /* Kernel initiation still rejects these services; the same INT inside
     * an allowed call reflects and returns through the IVT. */
    r.interrupt = 0x15; r.eax = 0x4F00;
    previous = enters;
    CHECK(biosvm_call(&r, 100) == -V86_EPERM && enters == previous);
    static const uint8_t internal[] = {0xB8,0x00,0x4F,0xCD,0x15,0xCD,0x13,0xCF};
    memcpy(ram + 0xF0100, internal, sizeof(internal));
    ivt(0x15, 0xF000, 0x400); ivt(0x13, 0xF000, 0x400); ram[0xF0400] = 0xCF;
    r.interrupt = 0x16; r.eax = 0x1100;
    uint64_t ints = firmware.stats.insn[V86_INT];
    CHECK(!biosvm_call(&r, 100) && firmware.stats.insn[V86_INT] == ints + 2 && r.eax == 0x4F00);
    ram[0xF0100] = 0xCF;
    ivt(0x15, 0xF000, 0x100);
    static const char selector[] = "f1:input-fault run=12345678 platform=e500";
    memcpy(g_boot.test_request, selector, sizeof(selector)); g_boot.test_request_len = sizeof(selector) - 1;
    g_boot.flags = CBI_F_SMBIOS_QEMU | CBI_F_TEST_REQUEST | CBI_F_INPUT_FORCED;
    struct biosvm_selftest_report report;
    physical[0x40] = 0x34; physical[0x21] = 0xF9; physical[0xA1] = 0xEF;
    CHECK(!biosvm_selftest(&report));
    CHECK(report.disabled && report.timeouts == 1 && report.disallowed == 1 && !counters.quarantines);
    CHECK(report.mappings_ok && report.mappings[4].end == 0xF0000);
    CHECK(biosvm_backend_state() == BIOSVM_READY);
    CHECK(!fwinput_init()); /* fake BIOS returns unsupported C205 */
    struct fwinput_backend_state state;
    fwinput_backend_state(&state);
    CHECK(state.keyboard && !state.mouse && state.key_releases && state.setup_error == -ENOSYS);
    test_observed_input();
    /* #PF and #UD terminate only a synthetic execution. Real faults below
     * retain all four leases and cannot be reset through the test hook. */
    synthetic = true;
    /* Internal reflection never widens I/O permissions or the deadline. */
    memcpy(ram + 0xF0100, internal, sizeof(internal));
    ivt(0x15, 0xF000, 0x400); ram[0xF0400] = 0xE4; ram[0xF0401] = 0x80;
    r.interrupt = 0x16; r.eax = 0x1100;
    CHECK(biosvm_call(&r, 100) == -V86_EPERM && firmware.fault.cs == 0xF000 && firmware.fault.ip == 0x400);
    CHECK(!biosvm_reset_for_test());
    ram[0xF0400] = 0xF4;
    r.eax = 0x1100;
    CHECK(biosvm_call(&r, 100) == -V86_ETIMEDOUT);
    CHECK(!biosvm_reset_for_test());
    ram[0xF0100] = 0xCF;
    execution_fault = 1; r.eax = 0x1100;
    CHECK(biosvm_call(&r, 100) == -EFAULT && firmware.fault.address == 0xDEAD000);
    CHECK(!biosvm_reset_for_test());
    execution_fault = 3;
    CHECK(biosvm_call(&r, 100) == -EFAULT && firmware.fault.vector == 6);
    CHECK(!biosvm_reset_for_test());
    execution_fault = 0;
    ram[0xF0100] = 0x0F; ram[0xF0101] = 0x0B;
    CHECK(biosvm_call(&r, 100) == -EFAULT && firmware.fault.bytes[0] == 0x0F);
    CHECK(!biosvm_reset_for_test());
    ram[0xF0100] = 0xCF;
    synthetic = false;
    execution_fault = 2; r.eax = 0x1100;
    CHECK(biosvm_call(&r, 100) == -V86_ETIMEDOUT && counters.quarantines == 4);
    previous = enters;
    CHECK(biosvm_call(&r, 100) == -V86_EIO && enters == previous);
    CHECK(biosvm_backend_state() == BIOSVM_DISABLED_BACKEND && biosvm_reset_for_test() == -V86_EPERM);
    struct fwinput_event event;
    CHECK(fwinput_poll(&event, 1) == 1 && event.type == FWINPUT_RESYNC && !fwinput_poll(&event, 1));
    for (unsigned i = 0; i < 4; i++) CHECK(registry_get(leases[i].handle)->state == RS_QUARANTINED);
    CHECK(firmware.fault.cs == 0xF000 && firmware.fault.ip == 0x100);
    CHECK(esp0 == 0xE0003000 && proc_switches > 0 && vm_preemptions > 0 && !native_tls);
    fwinput_backend_state(&state);
    CHECK(state.disabled && !state.keyboard && !state.mouse && !state.key_releases);
}

static void boot_fixture(bool irq_reservations)
{
    memset(ram, 0, sizeof(ram));
    memset(&firmware, 0, sizeof(firmware));
    memset(&decoder, 0, sizeof(decoder));
    memset(&backend, 0, sizeof(backend));
    memset(&state, 0, sizeof(state));
    memset(&public_input, 0, sizeof(public_input));
    memset(diagnostic, 0, sizeof(diagnostic));
    memset(&delivery, 0, sizeof(delivery));
    memset(irq_fake, 0, sizeof(irq_fake));
    memset(public_delivered, 0, sizeof(public_delivered));
    physical[0x21] = physical[0xA1] = 0xFF;
    input_wait = 0; input_reflections = 0;
    reflected_seen = loss_seen = poll_deadline = budget_violations = 0;
    adapter_wakes = adapter_polls = adapter_drains = adapter_events = 0;
    worker_until_idle = false;
    worker_waits = worker_sleeps = reflection_ticks = 0;
    diagnostic_count = 0;
    safe_input = safe_backend = safe_pass = false;
    initialized = quarantined = synthetic = started = false;
    request_pending = request_done = controller_status_valid = false;
    vm_thread = adapter = 0;
    input_service = 0; input_observer = 0;
    low_pt = vm_as.pd_phys = continuation = 0;
    lease_count = pending_irqs = mouse_lost = generation = 0;
    activation_count = execution_fault = public_events = 0;
    alloc_page = 0x200000;
    fail_alloc = fail_task = false;
    fail_low_pt = reject_queue = false;
    cr4_fake = setup_ticks = completion_delay = mouse_calls = next_generation = 0;
    mouse_error_function = -1;
    mouse_error_ax = mouse_error_flags = 0;
    flags = V86_IF; g_ticks = 100; g_current = &caller;
    g_boot = (struct ciuki_boot_info){.input_policy = CBI_INPUT_FIRMWARE,
        .flags = CBI_F_SAFE_MODE | CBI_F_TEXT_MODE | CBI_F_SMBIOS_QEMU |
                 CBI_F_TEST_REQUEST | CBI_F_INPUT_FORCED, .e820_count = 1};
    g_boot.e820[0] = (struct ciuki_e820){0, 0x9FC00, CBI_E820_RAM, 1};
    static const char selector[] = "f1:safe run=12345678 platform=e500 safe=1";
    memcpy(g_boot.test_request, selector, sizeof(selector));
    g_boot.test_request_len = sizeof(selector) - 1;
    putword(ram + 0x40E, 2, 0x9FC0); putword(ram + 0x413, 2, 639); ram[0x9FC00] = 1;
    putword(ram + 0x41A, 2, 0x1E); putword(ram + 0x41C, 2, 0x1E);
    ivt(0x16, 0xF000, 0x100); ram[0xF0100] = 0xCF;
    /* The SeaBIOS timer_read I/O used by PS/2 reply waits, then success.
     * This executes monitor traps, not the complete SeaBIOS ROM. */
    static const uint8_t mouse[] = {0xBA,0x08,0x06,0x66,0xED,0xB8,0,0,0xCF};
    ivt(0x15, 0xF000, 0x400); memcpy(ram + 0xF0400, mouse, sizeof(mouse));
    registry_init();
    if (irq_reservations) {
        CHECK(claim(RES_IRQ, 1, 2, "input", false, RS_FIRMWARE) >= 0);
        CHECK(claim(RES_IRQ, 12, 13, "input", false, RS_FIRMWARE) >= 0);
    }
    hw_reads = hw_writes = enters = 0;
}

static void test_firmware_boot(void)
{
    for (unsigned reserved = 0; reserved < 2; reserved++) {
        boot_fixture(reserved);
        unsigned count = registry_count();
        drivers_init();
        CHECK(logged("[init] input"));
        CHECK(!state.input_error && public_input.active);
        CHECK(registry_count() == count + (reserved ? 0 : 2));
        CHECK(!counters.conflicts && !hw_reads && !hw_writes);
        CHECK(!probe_safe() && safe_input && safe_backend && safe_pass);
        if (state.input_error) {
            printf("firmware boot reproduction: IRQ reservations=%u input_error=%d VM result=%d\n",
                   reserved, state.input_error, firmware.result);
            continue;
        }
        CHECK(backend.mouse && backend.keyboard && !backend.setup_error);
        CHECK(mouse_calls == 5 && firmware.stats.io[V86_PMTIMER] == 5);
        CHECK(decoder.stats.mouse_functions == ((1u << 5) | (1u << 2) | (1u << 3) | (1u << 7) | 1));
        for (unsigned i = 0; i < 4; i++) {
            const struct resource *r = registry_get(leases[i].handle);
            CHECK(r->state == RS_ACTIVE && !strcmp(r->owner, "firmware-input") && !r->shareable);
        }
        unsigned before = enters;
        drivers_init();
        CHECK(enters == before);
        firmware_byte(1, 0x1E, 1); firmware_byte(1, 0x9E, 1);
        CHECK(fwinput_adapter_step() == 2 && public_events == 2);
    }
    printf("firmware boot/safe: input_works=1 backend=firmware (current registry and reserved IRQs)\n");
}

static void test_retained_irq(void)
{
    boot_fixture(true);
    CHECK(!fwinput_adapter_init());
    /* Both edges arrive before the worker runs. IRQ1 wins fixed priority;
     * its IRET must not strand IRQ12 already captured into the virtual PIC. */
    static const uint8_t key[] = {0xB0,0x20,0xE6,0x20,0xCF};
    static const uint8_t aux[] = {0xB0,0x20,0xE6,0xA0,0xE6,0x20,0xCF};
    ivt(9, 0xF000, 0x200); memcpy(ram + 0xF0200, key, sizeof(key));
    ivt(0x74, 0xF000, 0x300); memcpy(ram + 0xF0300, aux, sizeof(aux));
    struct trap_frame irq = {.vector = 0x21};
    irq_fake[1](&irq);
    irq.vector = 0x2C; irq_fake[12](&irq);
    CHECK(worker_ready(0));
    switch_fake(vm_thread);
    struct biosvm_regs r = {0};
    CHECK(!run_vm(&r, 100, 0));
    CHECK(!pending_irqs && (firmware.pic[1].irr & 0x10));
    CHECK(worker_ready(0));
    CHECK(!run_vm(&r, 100, 0));
    CHECK(!firmware.pic[1].irr && !firmware.pic[0].isr && !firmware.pic[1].isr);
    CHECK(!worker_ready(0));
    /* Masking either the slave source or the master cascade parks it.
     * Eligibility ignores a finished call's VIF but retains IMR/ISR rules. */
    v86_irq_raise(&firmware, 12);
    firmware.pic[1].imr |= 0x10;
    CHECK(!worker_ready(0) && (firmware.pic[1].irr & 0x10));
    firmware.pic[1].imr &= ~0x10u; firmware.pic[0].imr |= 4;
    CHECK(!worker_ready(0));
    firmware.pic[0].imr &= ~4u; firmware.pic[0].isr |= 2;
    CHECK(!worker_ready(0));
    firmware.pic[0].isr = 0; firmware.vif = false;
    CHECK(worker_ready(0));
    /* Exercise the real worker loop, including a >1ms first ISR and its
     * mandatory block before the retained slave reflection. */
    pending_irqs = 2;
    reflection_ticks = 2;
    request_regs.interrupt = 0;
    worker_until_idle = true;
    if (!setjmp(worker_idle_env)) worker_main(0);
    worker_until_idle = false;
    CHECK(worker_waits == 2 && worker_sleeps == 1 && delivery.budget_yields == 1);
    CHECK(delivery.entries[0] == 2 && delivery.entries[1] == 2);
    CHECK(delivery.done[0] == 2 && delivery.done[1] == 2 && !worker_ready(0));
    switch_fake(&caller);
}

static void fake_physical_irq(unsigned irq)
{
    /* Dispatcher boundary fake: device handler before specific physical EOI.
     * Production trap.c is inspected/build-tested; CPU entry is not emulated. */
    struct trap_frame tf = {.vector = 0x20 + irq};
    uint32_t saved = irq_save();
    biosvm_account_irq(irq, false, irq_fake[irq] != 0);
    if (irq_fake[irq]) irq_fake[irq](&tf);
    if (irq >= 8) outb(0xA0, 0x60 | (irq - 8));
    outb(0x20, 0x60 | (irq >= 8 ? 2 : irq));
    biosvm_account_irq(irq, true, irq_fake[irq] != 0);
    irq_restore(saved);
}

static void test_irq_bda_delivery(void)
{
    boot_fixture(true);
    CHECK(!fwinput_adapter_init());
    CHECK(!(physical[0x21] & 6) && !(physical[0xA1] & 0x10));
    /* Scripted ISR consumes port60 through the production #GP path, writes
     * the actual BDA buffer/producer, EOIs only the virtual PIC, and IRETs. */
    static const uint8_t key[] = {
        0xE4,0x64,0xE4,0x60,0xB8,0x40,0,0x8E,0xD8,
        0xB8,0x61,0x1E,0xA3,0x1E,0,0xB8,0x20,0,0xA3,0x1C,0,
        0xB0,0x20,0xE6,0x20,0xCF,
    };
    /* AH=11 returns the word; AH=10 additionally advances the BDA head.
     * Neither function reads the physical controller. */
    static const uint8_t keyboard[] = {
        0x80,0xFC,0x11,0x74,0x0B,0xB8,0x40,0,0x8E,0xD8,
        0xB8,0x20,0,0xA3,0x1A,0,0xB8,0x61,0x1E,0xCF,
    };
    ivt(9, 0xF000, 0x200); memcpy(ram + 0xF0200, key, sizeof(key));
    memcpy(ram + 0xF0100, keyboard, sizeof(keyboard));
    physical[0x64] = 1; physical[0x60] = 0x1E;
    fake_physical_irq(1);
    CHECK(pending_irqs == 2 && !biosvm_keyboard_pending());
    unsigned writes = hw_writes, reads = hw_reads, before = enters;
    switch_fake(vm_thread);
    struct biosvm_regs r = {0};
    CHECK(!run_vm(&r, 100, 0));
    CHECK(biosvm_keyboard_pending() && getword(ram + 0x41E, 2) == 0x1E61);
    CHECK(getword(ram + 0x41A, 2) == 0x1E && getword(ram + 0x41C, 2) == 0x20);
    service_input();
    CHECK(!biosvm_keyboard_pending() && enters == before + 3);
    CHECK(hw_reads == reads + 2 && hw_writes == writes && physical[0x20] == 0x61);
    switch_fake(&caller);
    CHECK(adapter_ready(0) && fwinput_adapter_step() == 2 && public_events == 2);
    CHECK(public_delivered[0].type == FWINPUT_KEY && public_delivered[0].code == 0x1E &&
          public_delivered[0].value == 1);
    CHECK(public_delivered[1].type == FWINPUT_TEXT && public_delivered[1].value == 'a');
    CHECK(decoder.stats.text_matched == 1 && !decoder.stats.loss);
    before = enters;
    switch_fake(vm_thread); service_input(); switch_fake(&caller);
    CHECK(enters == before && !fwinput_adapter_step());
    struct biosvm_input_diag d;
    biosvm_input_snapshot(&d);
    CHECK(d.arrivals[0] == 1 && d.dispatched[0] == 1 && d.eois[0] == 1 && d.queued[0] == 1);
    CHECK(d.reflected[0] == 1 && d.entries[0] == 1 && d.done[0] == 1 && d.port60 == 1);
    CHECK(d.bda_head == 0x20 && d.bda_tail == 0x20 && !d.disabled && !d.active);
    CHECK(d.last_status == 1 && d.last_byte == 0x1E && adapter_events == 2 && adapter_drains == 1);
    /* Diagnostics do not consume data, enter firmware or mutate either PIC. */
    writes = hw_writes; reads = hw_reads;
    fwinput_adapter_log_delivery();
    CHECK(hw_reads == reads + 2 && hw_writes == writes && enters == before);
    CHECK(logged("irq=1 arrivals=1 dispatched=1 eoi=1 queued=1") && logged("drains=1 events=2"));
    /* Even the largest counter values fit the bounded klog line size. */
    memset(&delivery, 0xFF, sizeof(delivery));
    memset(&decoder.stats, 0xFF, sizeof(decoder.stats));
    adapter_wakes = adapter_polls = adapter_drains = adapter_events = budget_violations = UINT64_MAX;
    fwinput_adapter_log_delivery();
    printf("firmware IRQ delivery: PASS (physical IRQ1 -> trapped IN -> BDA -> INT16 -> adapter key/text)\n");
}

static void test_setup_failures(void)
{
    static const unsigned functions[] = {5, 2, 3, 7, 0};
    for (unsigned i = 0; i < ARRAY_SIZE(functions); i++) {
        for (unsigned carry = 0; carry < 2; carry++) {
            boot_fixture(true);
            mouse_error_function = (int)functions[i];
            mouse_error_ax = carry ? 0 : 0x0400;
            mouse_error_flags = carry ? V86_CF : 0;
            drivers_init();
            CHECK(!state.input_error && backend.keyboard && !backend.mouse && backend.setup_error == -ENOSYS);
            CHECK(mouse_calls == i + 1 && !counters.quarantines && biosvm_backend_state() == BIOSVM_READY);
            CHECK(!probe_safe() && safe_input && safe_backend && safe_pass);
            CHECK(logged("step=mouse_service") && logged("regs=in int=15") && logged("regs=out int=15"));
            firmware_byte(1, 0x1E, 1); firmware_byte(1, 0x9E, 1);
            CHECK(fwinput_adapter_step() == 2 && public_events == 2);
        }
    }
    boot_fixture(true);
    setup_ticks = 101; /* one total 500-tick budget, never 500 per function */
    drivers_init();
    CHECK(state.input_error == -V86_EIO && backend.setup_error == -V86_ETIMEDOUT && mouse_calls == 5);
    CHECK(counters.quarantines == 4 && probe_safe() == 1 && !safe_input && !safe_pass);
    CHECK(logged("error=-110 vector=13") && logged("deadline_ms=96") && logged("elapsed=101"));
    unsigned before = enters;
    CHECK(fwinput_init() == -V86_EIO && biosvm_init() == -V86_EIO && enters == before);
    CHECK(logged("step=already_disabled") && logged("step=already_quarantined"));

    boot_fixture(true);
    completion_delay = 500; /* completed call, then scheduling consumes budget */
    drivers_init();
    CHECK(!state.input_error && backend.setup_error == -V86_ETIMEDOUT && !backend.mouse && mouse_calls == 1);
    CHECK(!counters.quarantines && !probe_safe() && safe_input);
    CHECK(logged("step=setup_budget") && logged("elapsed=500"));

    boot_fixture(true);
    g_boot.flags &= ~CBI_F_SMBIOS_QEMU;
    drivers_init();
    CHECK(state.input_error == -V86_EIO && counters.quarantines == 4 && !hw_reads && !hw_writes);
    CHECK(logged("port=0608 width=4 write=0") && logged("cs=f000 ip=0403") && logged("regs=in int=15 eax=0000c205"));
    CHECK(!public_input.active && !safe_input && probe_safe() == 1);

    boot_fixture(true);
    fail_task = true;
    CHECK(fwinput_adapter_init() == -ENOMEM && logged("step=worker_create"));
    boot_fixture(true);
    CHECK(!fwinput_init());
    next_generation = UINT32_MAX;
    CHECK(fwinput_adapter_init() == -ENOSPC && logged("step=generation") && !adapter && !generation);
    boot_fixture(true);
    reject_queue = true;
    CHECK(fwinput_adapter_init() == -EINVAL && logged("step=queue_begin") && !adapter && !generation);
    CHECK(biosvm_backend_state() == BIOSVM_READY); /* leases never released on adapter failure */
    boot_fixture(true);
    g_boot.input_policy = CBI_INPUT_NATIVE; g_boot.flags &= ~CBI_F_INPUT_FORCED;
    CHECK(fwinput_adapter_init() == -ENOSYS && logged("step=input_policy") && !enters);
}

static void test_lease_failures(void)
{
    for (unsigned resource = 0; resource < 4; resource++) {
        for (unsigned bad = 0; bad < 5; bad++) {
            boot_fixture(true);
            unsigned h = resource < 2 ? 6 + resource : nres - 4 + resource;
            CHECK(res[h].type == (resource < 2 ? RES_PORT : RES_IRQ));
            if (!bad) res[h].owner = "native-owner";
            if (bad == 1) res[h].state = RS_ACTIVE;
            if (bad == 2) res[h].state = RS_QUARANTINED;
            if (bad == 3) res[h].shareable = true;
            if (bad == 4) res[h].end++;
            struct resource saved[MAX_RES];
            memcpy(saved, res, sizeof(saved));
            CHECK(biosvm_init() == -V86_EPERM && !lease_count);
            CHECK(!memcmp(saved, res, sizeof(saved)) && !hw_reads && !hw_writes && !enters);
            CHECK(logged("step=lease_conflict") && !vm_thread && !low_pt && !vm_as.pd_phys);
        }
    }
    boot_fixture(true);
    next_generation = UINT32_MAX - 1; /* first transfer succeeds, next fails */
    CHECK(biosvm_init() == -ENOSPC && lease_count == 1 && counters.quarantines == 1);
    CHECK(registry_get(leases[0].handle)->state == RS_QUARANTINED && !enters);
    CHECK(logged("step=lease_transfer") && logged("result=-28"));
    boot_fixture(true);
    res[6].state = RS_RELEASED;
    CHECK(fwinput_init() == -V86_EPERM && !lease_count && !enters);
    CHECK(logged("step=input_reservations_missing") && logged("[fwinput] step=biosvm_init"));

    boot_fixture(true);
    gen_t old = res[6].generation;
    CHECK(!biosvm_init());
    CHECK(registry_claim_reserved(6, old, "intruder") == -EINVAL && !strcmp(res[6].owner, "firmware-input"));
    CHECK(registry_claim(RES_IRQ, 12, 13, "intruder", false) == -EINVAL && !enters);

    boot_fixture(true);
    flags = 0;
    CHECK(biosvm_init() == -V86_EPERM && logged("step=thread_context"));
    CHECK(fwinput_adapter_init() == -EINVAL && logged("[fwinput-adapter] step=thread_context"));
    flags = V86_IF; cr4_fake = 1;
    CHECK(biosvm_init() == -V86_EPERM && logged("step=vme_pvi"));
    boot_fixture(true);
    g_boot.e820_count = 0;
    CHECK(biosvm_init() == -EFAULT && logged("step=scratch_reservation"));
    boot_fixture(true);
    ram[0x40F] = 0;
    CHECK(biosvm_init() == -EFAULT && logged("step=ebda_base"));
    boot_fixture(true);
    ram[0x9FC00] = 0;
    CHECK(biosvm_init() == -EFAULT && logged("step=ebda_size"));
    boot_fixture(true);
    fail_alloc = true;
    CHECK(biosvm_init() == -ENOMEM && logged("step=address_space"));
    boot_fixture(true);
    fail_low_pt = true;
    CHECK(biosvm_init() == -ENOMEM && logged("step=page_table"));
    boot_fixture(true);
    fail_task = true;
    CHECK(biosvm_init() == -ENOMEM && logged("step=worker_create"));
    CHECK(!low_pt && !vm_as.pd_phys && !lease_count);
}

static void test_pm_timer(void)
{
    FIX(0x66,0xED);
    f.tf.edx = 0x608;
    CHECK(v86_emulate(&v, &f) == -V86_EPERM && !hw_reads && !hw_writes);
    FIX(0x66,0xED);
    f.tf.edx = 0x608; v.qemu_pmtimer = true; v.ticks = 123;
    CHECK(!v86_emulate(&v, &f) && f.tf.eax == 123 * 3579u && !hw_reads && !hw_writes);
    uint32_t value = 0;
    v.ticks = UINT32_MAX;
    CHECK(!v86_io(&v, 0x608, 4, false, &value) && value == ((UINT32_MAX * 3579u) & 0xFFFFFFu));
    v.ticks++;
    CHECK(!v86_io(&v, 0x608, 4, false, &value) && !value);
    CHECK(v86_io(&v, 0x608, 1, false, &value) == -V86_EPERM);
    CHECK(v86_io(&v, 0x608, 2, false, &value) == -V86_EPERM);
    CHECK(v86_io(&v, 0x608, 4, true, &value) == -V86_EPERM);
    CHECK(v86_io(&v, 0x609, 4, false, &value) == -V86_EPERM);
    CHECK(v86_io(&v, 0x604, 2, true, &value) == -V86_EPERM);
    CHECK(!hw_reads && !hw_writes);
}

int main(void)
{
    test_flags_interrupts();
    test_io_decoder();
    test_devices_deadline();
    test_input_decoders();
    test_worker();
    test_firmware_boot();
    test_retained_irq();
    test_irq_bda_delivery();
    test_setup_failures();
    test_lease_failures();
    test_pm_timer();
    printf("firmware diagnostics: max=%u bytes (strictly below 240)\n", diagnostic_max);
    printf("v86/firmware host tests: %s (%u checks, %u failures)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
