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
static unsigned hw_reads, hw_writes, enters, quarantines;
static uint32_t flags = V86_IF, cr3, esp0, alloc_page = 0x200000;
static struct task caller, worker_fake, native_fake;
static unsigned proc_switches, vm_preemptions;
static bool native_tls;
static irq_handler_t irq_fake[16];
static struct resource resources[4];
static unsigned resource_count;
static unsigned execution_fault;
static jmp_buf leave_env;

struct task *g_current;
volatile uint64_t g_ticks;
volatile bool g_need_resched;
struct ciuki_boot_info g_boot;

#define CIUKI_CPU_H
#define P2V(p) ((void *)(ram + (p)))
static uint32_t read_eflags(void) { return flags; }
static uint32_t irq_save(void) { uint32_t f = flags; flags = 0; return f; }
static void irq_restore(uint32_t f) { flags = f; }
static void cli(void) { flags = 0; }
static void sti(void) { flags = V86_IF; }
static uint32_t read_cr3(void) { return cr3; }
static void write_cr3(uint32_t x) { cr3 = x; }
static uint32_t read_cr4(void) { return 0; }
static uint32_t read_cr2(void) { return 0xDEAD000; }
static uint8_t inb(uint16_t p) { hw_reads++; return physical[p]; }
static void outb(uint16_t p, uint8_t b) { hw_writes++; physical[p] = b; }

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
uint32_t pmm_alloc(void) { uint32_t p = alloc_page; alloc_page += PAGE_SIZE; CHECK(alloc_page < sizeof(ram)); return p; }
void pmm_free(uint32_t p) { CHECK(p >= 0x200000 && p < alloc_page); }
bool pmm_is_reserved(uint32_t p) { return p < 0x100000; }
int as_create(struct aspace *as) { as->pd_phys = pmm_alloc(); memset(P2V(as->pd_phys), 0, PAGE_SIZE); return 0; }
struct task *task_create_kernel(const char *n, void (*fn)(void *), void *a, enum task_prio p)
{
    (void)n; (void)fn; (void)a;
    worker_fake = (struct task){.state = T_BLOCKED, .prio = p, .kstack = (void *)(uintptr_t)0xF0001000};
    return &worker_fake;
}
void task_start(struct task *t) { CHECK(t->state == T_BLOCKED); t->state = T_READY; }
void task_kill(struct task *t, int c) { (void)c; t->state = T_ZOMBIE; }
void task_reap(struct task *t) { CHECK(t->state == T_ZOMBIE); }
void irq_set_handler(unsigned n, irq_handler_t h) { CHECK(n < 16); irq_fake[n] = h; }
void pic_unmask(unsigned n) { CHECK(irq_fake[n] != 0); }
void panic_frame(struct trap_frame *tf, const char *s) { (void)tf; fprintf(stderr, "PANIC %s\n", s); exit(2); }
unsigned registry_count(void) { return resource_count; }
const struct resource *registry_get(unsigned i) { CHECK(i < resource_count); return &resources[i]; }
int registry_claim_reserved(int h, gen_t g, const char *o)
{
    CHECK(resources[h].state == RS_FIRMWARE && resources[h].generation == g);
    resources[h].state = RS_CLAIMED; resources[h].generation++; resources[h].owner = o;
    return 0;
}
int registry_claim(enum res_type t, uint32_t s, uint32_t e, const char *o, bool shared)
{
    CHECK(resource_count < 4);
    unsigned n = resource_count++;
    resources[n] = (struct resource){t, s, e, o, n + 1, RS_CLAIMED, shared};
    return (int)n;
}
int registry_activate(int h, gen_t g) { CHECK(resources[h].generation == g); resources[h].state = RS_ACTIVE; return 0; }
int registry_quarantine(int h, gen_t g)
{
    CHECK(resources[h].generation == g); resources[h].state = RS_QUARANTINED; quarantines++;
    return 0;
}
__asm__(".pushsection .rodata\n.globl biosvm_mouse_stub, biosvm_mouse_stub_end\nbiosvm_mouse_stub:\n.byte 0xcb\nbiosvm_mouse_stub_end:\n.popsection\n");

#include "../../src/kernel/vm/v86.c"
#include "../../src/kernel/vm/biosvm.c"
#include "../../src/kernel/vm/fwinput.c"

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
    if (setjmp(leave_env)) {
        sti();
        return;
    }
    for (unsigned steps = 0; steps < 1000; steps++) {
        uint8_t *code = vm_memory(0, (live.tf.cs << 4) + live.tf.eip, 2, false);
        CHECK(code != 0);
        if (!code)
            exit(2);
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
        } else if (code[0] == 0xB8) {
            live.tf.eax = (live.tf.eax & ~0xFFFFu) | getword(code + 1, 2);
            live.tf.eip += 3;
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
    if (q == &completion && request_pending) {
        struct task *old = g_current;
        switch_fake(vm_thread);
        request_result = run_vm(&request_regs, request_ms, request_test);
        request_pending = false;
        request_done = true;
        switch_fake(old);
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
    resources[0] = (struct resource){RES_PORT,0x60,0x61,"input",1,RS_FIRMWARE,false};
    resources[1] = (struct resource){RES_PORT,0x64,0x65,"input",2,RS_FIRMWARE,false};
    resource_count = 2;
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
    CHECK(report.disabled && report.timeouts == 1 && report.disallowed == 1 && !quarantines);
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
    CHECK(biosvm_call(&r, 100) == -V86_ETIMEDOUT && quarantines == 4);
    previous = enters;
    CHECK(biosvm_call(&r, 100) == -V86_EIO && enters == previous);
    CHECK(biosvm_backend_state() == BIOSVM_DISABLED_BACKEND && biosvm_reset_for_test() == -V86_EPERM);
    struct fwinput_event event;
    CHECK(fwinput_poll(&event, 1) == 1 && event.type == FWINPUT_RESYNC && !fwinput_poll(&event, 1));
    for (unsigned i = 0; i < 4; i++) CHECK(resources[i].state == RS_QUARANTINED);
    CHECK(firmware.fault.cs == 0xF000 && firmware.fault.ip == 0x100);
    CHECK(esp0 == 0xE0003000 && proc_switches > 0 && vm_preemptions > 0 && !native_tls);
    fwinput_backend_state(&state);
    CHECK(state.disabled && !state.keyboard && !state.mouse && !state.key_releases);
}

int main(void)
{
    test_flags_interrupts();
    test_io_decoder();
    test_devices_deadline();
    test_input_decoders();
    test_worker();
    printf("v86/firmware host tests: %s (%u checks, %u failures)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
