/* f3-04 host runner: boots the real DOSBOOT.IMG in one DOS VM with the
 * production dosvm/v86/BIOS/XMS/EMS/DPMI code and libx86emu as the guest
 * CPU (dosvm_guest_cpu.c), types payload commands at the shell and reports
 * their verdict lines and the kernel DPMI host state. PMM, scheduler, VFS
 * transport and time are host fakes; one simulated millisecond per SLICE
 * guest instructions. Not QEMU or hardware evidence: a pre-QEMU check of
 * the payloads against the real kernel host paths.
 * Usage: dosvm_guest DOSBOOT.IMG [COMMAND NAME | fault:COMMAND NAME | gate:COMMAND NAME |
 *        keys:\VMVIDEO.COM VMVIDEO]...
 * (gate: the VM's DPMI flag cleared; the client must report FAIL 01 01.
 * keys: f3-02 window typing: set-1 make/break codes into the focused VM's
 * 8042 (dosvm_input_event), IRQ1 and the guest INT 9 to the shell blocked in
 * INT 16h AH=00h; VMVIDEO must stop at stage 1 with flags 3, then each Enter
 * through the 8042 advances one stage until the prompt returns.)
 * GUEST_TCG=1 models QEMU TCG: no data-segment limit checks and TCG's CPUID
 * signature (docs/validation/2026-10-09-f0/README.md, f3-04 deviation).
 * f3-06: GUEST_BUDGET=<instructions per command> for long workloads, e.g. the
 * base image of a local DOOM fixture (scripts/test/dos_fixtures.py; extract
 * CIUKIGAM.IMG with mcopy): GUEST_BUDGET=9000000000 dosvm_guest base.img
 * '\RUNREC.COM \DOOM DOOM.EXE -config CIUKI.CFG -nosound -timedemo demo1' RUNREC
 * ends with the timed line on the screen and RUNREC FAIL R01 T00 (exit 1).
 * With an rc: prefix and GUEST_EXPECT_TEXT='timed 1710 gametics in', the
 * runner goes on to the shell prompt and checks the verdict-time screen.
 * SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <unistd.h>
#undef _SC_PAGESIZE
#undef _SC_OPEN_MAX
#undef _SC_ARG_MAX
#undef _SC_THREAD_KEYS_MAX
#undef _SC_THREAD_STACK_MIN
#undef WIFEXITED
#undef WEXITSTATUS
#undef WIFSIGNALED
#undef WTERMSIG
#include <ciuki/abi.h>
#define CIUKI_CPU_H
#include <ciuki/kernel.h>
#include <ciuki/task.h>
#include <ciuki/input.h>
#include <ciuki/dosvm.h>
#include <ciuki/sha256.h>
#include "dosvm_guest.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c); exit(1); } } while (0)
#define KERNEL_VBASE 0xC0000000u
#define P2V(p) ((void *)(ram + (p)))
#define V2P(p) ((uint32_t)((uint8_t *)(p) - ram))
#define HOST_PAGES 32768u
#define SLICE 4000u
static unsigned used_pages, unexpected;
static bool allocated[HOST_PAGES];
static uint8_t *ram;
static uint32_t cpu_cr3, cpu_flags = V86_IF, next_pfn = 4096, fake_cr2;
static uint32_t kpd[1024], kpd_phys;
static struct task caller;
struct task *g_current = &caller;
uint32_t g_task_count, g_user_mappings;
volatile uint64_t g_ticks;
volatile bool g_need_resched;
static uint32_t read_cr3(void) { return cpu_cr3; }
static void write_cr3(uint32_t x) { cpu_cr3 = x; }
static uint32_t read_cr2(void) { return fake_cr2; }
static void cli(void) { cpu_flags = 0; }
static void sti(void) { cpu_flags = V86_IF; }
static void invlpg(uint32_t va) { (void)va; }
static uint32_t *table_va(uint32_t p) { return P2V(p); }
static uint32_t irq_save(void) { uint32_t f = cpu_flags; cli(); return f; }
static void irq_restore(uint32_t f) { cpu_flags = f; }

void arch_ldt_load(const void *base, unsigned entries) { (void)base; (void)entries; }
/* Debug registers of the runner's CPU: DR0-DR3/DR7 as loaded by the VM,
 * data breakpoints checked by the CPU wrapper on every guest access. */
static uint32_t debug_addr[4], debug_dr7, debug_dr6;
void arch_debug_load(const uint32_t a[4], uint32_t dr7) { for (unsigned i = 0; i < 4; i++) debug_addr[i] = a[i]; debug_dr7 = dr7; }
uint32_t arch_debug_take(void) { uint32_t d = debug_dr6; debug_dr6 = 0; return d; }
static unsigned debug_watch(uint32_t lin, unsigned n, bool write)
{
    unsigned hits = 0;
    for (unsigned i = 0; i < 4; i++) {
        if (!(debug_dr7 & (1u << (i * 2)))) continue;
        unsigned rw = (debug_dr7 >> (16 + i * 4)) & 3, len = ((debug_dr7 >> (18 + i * 4)) & 3) + 1;
        if (rw == 0 || rw == 2 || (rw == 1 && !write)) continue;
        if (len == 3) len = 4;
        if (lin < debug_addr[i] + len && debug_addr[i] < lin + n) hits |= 1u << i;
    }
    return hits;
}
uint8_t arch_cpu_family(void) { return 6; }
#include "../../src/kernel/vm/dosvm_internal.h"
uint32_t pmm_alloc(void)
{
    for (unsigned n = 0; n < HOST_PAGES; n++) {
        unsigned p = next_pfn++;
        if (next_pfn >= HOST_PAGES) next_pfn = 4096;
        if (p >= 4096 && !allocated[p]) { allocated[p] = true; used_pages++; return p * PAGE_SIZE; }
    }
    return 0;
}
uint32_t pmm_alloc_zone(int zone) { (void)zone; return pmm_alloc(); }
void pmm_free(uint32_t p) { allocated[p / PAGE_SIZE] = false; used_pages--; }
uint32_t pmm_free_count(void) { return HOST_PAGES - 4096 - used_pages; }
uint32_t vmm_kernel_pd(void) { return kpd_phys; }
void panic(const char *fmt, ...) { fprintf(stderr, "panic: %s\n", fmt); abort(); }
void klog(const char *fmt, ...) { (void)fmt; }
void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }
#include "dosvm_aspace.inc"


static jmp_buf guest_return, idle_return;
static bool waiting_input;
static void (*sleep_hook)(void);
struct task *task_create_kernel(const char *name, void (*fn)(void *), void *arg, enum task_prio prio)
{
    (void)name; (void)fn; (void)arg; (void)prio;
    struct task *t = calloc(1, sizeof(*t));
    t->state = T_BLOCKED; t->kstack = malloc(KSTACK_SIZE); t->fpu_alloc = malloc(512); t->fpu_area = t->fpu_alloc;
    g_task_count++;
    return t;
}
void task_start(struct task *t) { t->state = T_READY; }
bool task_alive(const struct task *t) { return t->state < T_ZOMBIE; }
void task_kill(struct task *t, int reason) { t->state = T_ZOMBIE; t->exit_code = reason; }
void task_reap(struct task *t) { g_task_count--; free(t->kstack); free(t->fpu_alloc); free(t); }
void task_exit(int reason) { (void)reason; abort(); }
void schedule(void) { g_need_resched = false; }
void task_sleep_ms(uint32_t ms)
{
    g_ticks += ms;
    dosvm_tick();
    if (sleep_hook) sleep_hook();
}
void tss_set_kernel_stack(uint32_t sp) { (void)sp; }
void fpu_handle_nm(void) {}
gen_t gen_alloc(void) { static gen_t generation; return ++generation; }
bool gen_matches(gen_t a, gen_t b) { return a && a == b; }
uint64_t deadline_after_ms(uint32_t ms) { return g_ticks + ms; }
bool deadline_passed(uint64_t t) { return g_ticks - t < (1ull << 63); }
void kwait_init(struct kwait *q) { q->head = 0; }
void kwait_wake_all(struct kwait *q) { (void)q; }
void kmutex_init(struct kmutex *m) { memset(m, 0, sizeof(*m)); }
void kmutex_lock(struct kmutex *m) { m->owner = g_current; }
void kmutex_unlock(struct kmutex *m) { m->owner = 0; }
bool kwait_wait_until(struct kwait *q, kwait_cond_fn fn, void *arg, uint64_t until)
{ (void)q; while (!fn(arg) && !deadline_passed(until)) task_sleep_ms(1); return fn(arg); }
bool input_read(struct input_event *e) { (void)e; return false; }
void v86_leave(uint32_t continuation) { (void)continuation; longjmp(guest_return, 1); }
void v86_enter(const struct v86_frame *initial, uint32_t *continuation) { (void)initial; (void)continuation; abort(); }

/* VFS transport: the real image read-only, a per-run scratch overlay file. */
static struct storage store = {.ready = true};
static FILE *disk_image, *cow;
static char cow_path[96];
static uint64_t pos[2];
struct storage *storage_get(void) { return &store; }
int vfs_table_init(struct vfs *vfs, struct vfs_table *table, unsigned drive) { (void)vfs; (void)table; (void)drive; return 0; }
void vfs_table_destroy(struct vfs_table *table) { (void)table; }
int vfs_stat(struct vfs_table *table, const char *path, struct fat_entry *entry)
{ (void)table; (void)path; memset(entry, 0, sizeof(*entry)); entry->size = DOSVM_SECTORS * 512u; return 0; }
int vfs_open(struct vfs_table *table, const char *path, unsigned access, enum vfs_share share, uint8_t attr)
{
    (void)table; (void)access; (void)share; (void)attr;
    if (strstr(path, "DOSBOOT.IMG")) return 0;
    snprintf(cow_path, sizeof(cow_path), "build/host/dosvm-guest-XXXXXX");
    int fd = mkstemp(cow_path); CHECK(fd >= 0); cow = fdopen(fd, "w+b"); CHECK(cow);
    return 1;
}
int vfs_close(struct vfs_table *table, int handle) { (void)table; (void)handle; return 0; }
int vfs_delete(struct vfs_table *table, const char *path)
{ (void)table; (void)path; if (cow) { fclose(cow); unlink(cow_path); cow = 0; } return 0; }
int vfs_seek(struct vfs_table *table, int handle, int64_t offset, enum vfs_whence whence, uint64_t *position)
{ (void)table; (void)whence; pos[handle] = (uint64_t)offset; *position = (uint64_t)offset; return 0; }
int vfs_read(struct vfs_table *table, int handle, void *data, size_t bytes, size_t *done)
{
    (void)table;
    FILE *f = handle ? cow : disk_image;
    CHECK(fseek(f, (long)pos[handle], SEEK_SET) == 0);
    size_t n = fread(data, 1, bytes, f);
    if (n < bytes) memset((uint8_t *)data + n, 0, bytes - n);
    *done = bytes;
    return 0;
}
int vfs_write(struct vfs_table *table, int handle, const void *data, size_t bytes, size_t *done)
{ (void)table; CHECK(handle == 1 && fseek(cow, (long)pos[1], SEEK_SET) == 0); *done = fwrite(data, 1, bytes, cow); return 0; }
int vfs_commit(struct vfs_table *table, int handle) { (void)table; (void)handle; return 0; }

static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }
void rec_emit(const char *probe, const char *event, const char *fmt, ...)
{
    char extra[256];
    va_list ap; va_start(ap, fmt); vsnprintf(extra, sizeof(extra), fmt, ap); va_end(ap);
    const char *data = strstr(extra, " data=");
    if (strstr(extra, "group=guest") && data) {
        char text[100]; unsigned k = 0;
        for (const char *p = data + 6; p[0] && p[1] && k + 1 < sizeof(text); p += 2) text[k++] = (char)(hexv(p[0]) * 16 + hexv(p[1]));
        text[k] = 0;
        printf("  guest: %s\n", text);
        return;
    }
    printf("  record %s %s %s\n", probe, event, extra);
}
#include "../../src/kernel/vm/dosvm_pic.c"
#include "../../src/kernel/vm/v86.c"
#include "../../src/kernel/vm/dosvm.c"
#include "../../src/kernel/vm/dosvm_disk.c"
#include "../../src/kernel/vm/dosvm_bios.c"
#include "../../src/kernel/vm/dosvm_pit.c"
#include "../../src/kernel/vm/dosvm_kbd.c"
#include "../../src/kernel/vm/dosvm_mouse.c"
#include "../../src/kernel/vm/dosvm_xms.c"
#include "../../src/kernel/vm/dosvm_umb.c"
#include "../../src/kernel/vm/dosvm_ems.c"
/* f3-02 VGA adapter and f3-05 audio devices, created with every VM since
 * the F3 merge; the VGA models and presenter link as separate units, as in
 * the kernel (scripts/test/host_kernel_tests.sh). */
#include "dosvm_font.inc"
#include "../../src/kernel/vm/dosvm_vga.c"
#include "../../src/kernel/vm/dosvm_dma.c"
#include "../../src/kernel/vm/dosvm_opl.c"
#include "../../src/kernel/vm/dosvm_sb16.c"
#include "../../src/kernel/vm/dpmi_desc.c"
#include "../../src/kernel/vm/dpmi_switch.c"
#include "../../src/kernel/vm/dpmi_dispatch.c"
#include "../../src/kernel/vm/dpmi.c"

/* ---- guest CPU glue ---- */
static struct dosvm *gv;
/* The shell blocks in INT 16h/00h inside the monitor (synthetic ROM stub
 * F000:0160). With no typed input left, abandon that trap: the guest frame
 * is still the one at the INT FEh, so the next delivery re-executes it. */
static void input_hook(void)
{
    if (gv && !gv->frame_pm && gv->frame.tf.cs == 0xF000 && gv->frame.tf.eip == 0x16 * 16 + 2 && !gv->key_count) {
        gv->info.state = DOSVM_RUNNABLE;
        waiting_input = true;
        longjmp(idle_return, 1);
    }
}
static struct v86_frame cpu;
static uint64_t instructions, traps;
static uint32_t rstack_low = 0xFFFF;   /* lowest SP seen on the DPMI host real-mode stack */
static int trace = -1, ring_on = -1;
#define RING 3000u
static struct ring_entry { struct gcpu_state s; uint8_t b[6]; unsigned depth; } ring[RING];
static unsigned ring_pos;
static void ring_dump(void)
{
    if (ring_on <= 0) return;
    unsigned first = ring_pos > RING ? ring_pos - RING : 0;
    for (unsigned i = first; i < ring_pos; i++) {
        const struct ring_entry *r = &ring[i % RING];
        printf("    pm %04x:%08x %02x %02x %02x %02x %02x %02x eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x ss:esp=%04x:%08x ds=%04x es=%04x fs=%04x d=%u\n",
               r->s.cs, r->s.eip, r->b[0], r->b[1], r->b[2], r->b[3], r->b[4], r->b[5], r->s.eax, r->s.ebx, r->s.ecx, r->s.edx,
               r->s.esi, r->s.edi, r->s.ebp, r->s.ss, r->s.esp, r->s.ds, r->s.es, r->s.fs, r->depth);
    }
}
static bool frame_pm(void);
static void trace_trap(const struct gcpu_event *ev)
{
    if (trace < 0) trace = getenv("GUEST_TRACE") != 0;
    if (!trace) return;
    bool pm = frame_pm();
    uint32_t lin = pm ? dpmi_fault_linear(gv, &cpu) : ((cpu.tf.cs & 0xFFFF) << 4) + (cpu.tf.eip & 0xFFFF);
    uint8_t op[2] = {0};
    dosvm_read(gv, lin, op, 2);
    uint16_t nest = 0, slot = 0; dosvm_read(gv, 0x3000 + 0xAADC, &nest, 2); dosvm_read(gv, 0x3000 + 0x9C4D, &slot, 2);
    if (pm && ev->vector != 0x20 && !(ev->vector == 13 && (ev->err & 3) == 2) &&
        !(ev->vector == 13 && !ev->err && (op[0] == 0xFA || op[0] == 0xFB || op[0] == 0xE4 || op[0] == 0xE6 || op[0] == 0xEC || op[0] == 0xEE)))
        printf("    trace PM trap vec=%u err=%x at %04x:%08x op=%02x %02x ss:esp=%04x:%08x depth=%u\n", ev->vector, ev->err,
               cpu.tf.cs & 0xFFFF, cpu.tf.eip, op[0], op[1], cpu.tf.user_ss & 0xFFFF, cpu.tf.user_esp, gv->dpmi.depth);
    if (ev->vector == 13 && op[0] == 0xCD && (op[1] == 0x21 || op[1] == 0x31 || op[1] == 0x2F || op[1] == 0x20))
        printf("    trace nest=%u slot=%u %s %04x:%04x INT %02x ax=%04x bx=%04x cx=%04x dx=%04x ss:sp=%04x:%04x ds=%04x es=%04x depth=%u clients=%u\n", nest, slot,
               pm ? "PM " : "V86", cpu.tf.cs & 0xFFFF, cpu.tf.eip & 0xFFFF, op[1], cpu.tf.eax & 0xFFFF, cpu.tf.ebx & 0xFFFF,
               cpu.tf.ecx & 0xFFFF, cpu.tf.edx & 0xFFFF, cpu.tf.user_ss & 0xFFFF, cpu.tf.user_esp & 0xFFFF,
               (pm ? cpu.tf.ds : cpu.ds) & 0xFFFF, (pm ? cpu.tf.es : cpu.es) & 0xFFFF, gv->dpmi.depth, gv->dpmi.clients);
}
static unsigned slice_left = SLICE;
static bool g_read(uint32_t lin, void *b, unsigned n) { return !dosvm_read(gv, lin, b, n); }
static bool g_write(uint32_t lin, const void *b, unsigned n) { return !dosvm_write(gv, lin, b, n); }
static void g_descriptor(uint16_t sel, bool write, uint32_t d[2])
{
    (void)write;
    d[0] = d[1] = 0;
    if (sel & 4) {
        unsigned i = sel >> 3;
        if (gv->dpmi.active && i < DPMI_LDT_ENTRIES) { d[0] = gv->dpmi.ldt[i][0]; d[1] = gv->dpmi.ldt[i][1]; }
        return;
    }
    if ((sel & ~7u) == 0x18) { d[0] = 0x0000FFFFu; d[1] = 0x00CFFA00u; }
    if ((sel & ~7u) == 0x20) { d[0] = 0x0000FFFFu; d[1] = 0x00CFF200u; }
}
static void g_unexpected(const char *what)
{
    unexpected++;
    fprintf(stderr, "unexpected: %s at %04x:%08x pm=%u\n", what, cpu.tf.cs & 0xFFFF, cpu.tf.eip, gv->frame_pm);
}
static const struct gcpu_ops guest_ops = {g_read, g_write, g_descriptor, g_unexpected, debug_watch};

static bool frame_pm(void) { return !(cpu.tf.eflags & V86_VM); }
static void to_state(struct gcpu_state *s)
{
    bool pm = frame_pm();
    memset(s, 0, sizeof(*s));
    s->eax = cpu.tf.eax; s->ebx = cpu.tf.ebx; s->ecx = cpu.tf.ecx; s->edx = cpu.tf.edx;
    s->esi = cpu.tf.esi; s->edi = cpu.tf.edi; s->ebp = cpu.tf.ebp; s->esp = cpu.tf.user_esp;
    s->eip = cpu.tf.eip; s->eflags = cpu.tf.eflags; s->pm = pm;
    s->cs = (uint16_t)cpu.tf.cs; s->ss = (uint16_t)cpu.tf.user_ss;
    s->ds = (uint16_t)(pm ? cpu.tf.ds : cpu.ds); s->es = (uint16_t)(pm ? cpu.tf.es : cpu.es);
    s->fs = (uint16_t)(pm ? cpu.tf.fs : cpu.fs); s->gs = (uint16_t)(pm ? cpu.tf.gs : cpu.gs);
    s->ldt_limit = DPMI_LDT_ENTRIES * 8 - 1;
}
static void from_state(const struct gcpu_state *s)
{
    cpu.tf.eax = s->eax; cpu.tf.ebx = s->ebx; cpu.tf.ecx = s->ecx; cpu.tf.edx = s->edx;
    cpu.tf.esi = s->esi; cpu.tf.edi = s->edi; cpu.tf.ebp = s->ebp; cpu.tf.user_esp = s->esp;
    cpu.tf.eip = s->eip; cpu.tf.cs = s->cs; cpu.tf.user_ss = s->ss;
    cpu.tf.eflags = (s->eflags & ~V86_VM) | (s->pm ? 0 : V86_VM);
    if (s->pm) { cpu.tf.ds = s->ds; cpu.tf.es = s->es; cpu.tf.fs = s->fs; cpu.tf.gs = s->gs; }
    else { cpu.ds = s->ds; cpu.es = s->es; cpu.fs = s->fs; cpu.gs = s->gs; }
}
/* One trap into the production monitor, exactly as trap_return/v86_enter
 * would resume the worker: in place, or re-entered after a mode switch. */
static void deliver(unsigned vector, uint32_t err, uint32_t cr2)
{
    struct v86_frame hw = cpu;
    hw.tf.vector = vector; hw.tf.err = err; fake_cr2 = cr2;
    g_current = gv->task; gv->continuation = 0x12345678; cpu_flags = 0;
    traps++;
    if (setjmp(idle_return)) { g_current = &caller; sti(); return; }
    if (!setjmp(guest_return)) { CHECK(dosvm_trap(&hw.tf)); cpu = hw; }
    else cpu = gv->frame;
    g_current = &caller; sti();
}
static bool stopped(void) { return gv->info.state >= DOSVM_STOPPING; }
/* Run until done() or the VM stops or the instruction budget runs out. */
static int run(uint64_t budget, bool (*done)(void))
{
    while (budget && !stopped()) {
        if (done && done()) return 1;
        if (waiting_input) return 2;
        struct gcpu_state s;
        struct gcpu_event ev;
        to_state(&s);
        static unsigned steps;
        if (trace && gv->dpmi.stats.terminations && steps < 4000 && !s.pm) {
            steps++;
            uint8_t b[4] = {0}; dosvm_read(gv, ((uint32_t)s.cs << 4) + (s.eip & 0xFFFF), b, 4);
            printf("    step %04x:%04x %02x %02x %02x %02x ax=%04x bx=%04x cx=%04x dx=%04x si=%04x di=%04x bp=%04x sp=%04x:%04x ds=%04x es=%04x fl=%04x\n",
                   s.cs, s.eip & 0xFFFF, b[0], b[1], b[2], b[3], s.eax & 0xFFFF, s.ebx & 0xFFFF, s.ecx & 0xFFFF, s.edx & 0xFFFF,
                   s.esi & 0xFFFF, s.edi & 0xFFFF, s.ebp & 0xFFFF, s.ss, s.esp & 0xFFFF, s.ds, s.es, s.eflags & 0xFFFF);
        }
        unsigned n = slice_left;
        if (ring_on < 0) ring_on = getenv("GUEST_RING") != 0;
        if (ring_on && s.pm) {
            n = 1;
            struct ring_entry *r = &ring[ring_pos++ % RING];
            r->s = s;
            dosvm_read(gv, dpmi_fault_linear(gv, &cpu), r->b, sizeof(r->b));
            r->depth = gv->dpmi.depth;
        }
        int rc = gcpu_run(&s, n, &ev);
        from_state(&s);
        if (!s.pm && s.ss == DPMI_RSTACK_SEG && (s.esp & 0xFFFF) < rstack_low) rstack_low = s.esp & 0xFFFF;
        instructions += ev.executed;
        budget = ev.executed >= budget ? 0 : budget - ev.executed;
        if (rc != GCPU_BUDGET) {
            trace_trap(&ev);
            bool was_pm = frame_pm();
            debug_dr6 = ev.dr6;
            deliver(ev.vector, ev.err, ev.cr2);
            if (trace && was_pm != frame_pm())
                printf("    trace switch -> %s %04x:%08x ss:sp=%04x:%08x depth=%u\n", frame_pm() ? "PM" : "V86",
                       cpu.tf.cs & 0xFFFF, cpu.tf.eip, cpu.tf.user_ss & 0xFFFF, cpu.tf.user_esp, gv->dpmi.depth);
        }
        if (stopped()) break;
        if (ev.executed >= slice_left && !(rc == GCPU_BUDGET && ev.ss_shadow)) {
            slice_left = SLICE;
            g_ticks++;
            dosvm_tick();
            deliver(0x20, 0, 0);        /* the host's 1 kHz timer IRQ: a delivery point */
        } else if (ev.executed < slice_left) slice_left -= ev.executed ? ev.executed : (rc == GCPU_BUDGET ? 1 : 0);
        else slice_left = 1;            /* MOV SS shadow: the timer waits one instruction */
    }
    return done && done() ? 1 : waiting_input ? 2 : stopped() ? -1 : 0;
}
static bool prompt(void)
{
    const uint16_t *text = dosvm_text(gv, 0);
    for (unsigned row = 0; row < 25; row++) {
        bool drive = false;
        for (unsigned col = 0; col < 80; col++) {
            unsigned i = row * 80 + col;
            if (col < 78 && (uint8_t)text[i] == 'C' && (uint8_t)text[i + 1] == ':' && (uint8_t)text[i + 2] == '\\') drive = true;
            if (drive && (uint8_t)text[i] == '>') return true;
        }
    }
    return false;
}
static bool booted(void) { return gv->info.dos_ready && prompt(); }
static bool verdict(void) { return gv->info.verdict_done; }
static void screen_of(const uint16_t *text, const char *prefix)
{
    for (unsigned row = 0; row < 25; row++) {
        char line[81]; unsigned len = 0;
        for (unsigned col = 0; col < 80; col++) {
            uint8_t ch = (uint8_t)text[row * 80 + col];
            line[col] = ch >= 32 && ch < 127 ? (char)ch : '.';
            if (ch > 32 && ch < 127) len = col + 1;
        }
        line[len] = 0;
        if (len) printf("  %s %s\n", prefix, line);
    }
}
static void screen(void) { screen_of(dosvm_text(gv, 0), "|"); }
/* f3-06: does the screen latched at the verdict line hold text? */
static bool latched_has(const char *text)
{
    const uint16_t *cells = dosvm_verdict_text(gv);
    if (!cells || !text) return false;
    size_t n = strlen(text);
    for (unsigned row = 0; row < 25; row++) {
        char line[81];
        for (unsigned col = 0; col < 80; col++) line[col] = (char)(uint8_t)cells[row * 80 + col];
        line[80] = 0;
        for (unsigned col = 0; col + n <= 80; col++) if (!memcmp(line + col, text, n)) return true;
    }
    return false;
}
static void fault_dump(void)
{
    if (!stopped()) return;
    ring_dump();
    const struct v86_frame *f = &gv->frame;
    uint8_t code[12] = {0};
    dosvm_read(gv, gv->info.fault_eip, code, sizeof(code));
    printf("  stop: reason=%u vector=%u eip_lin=%08x err=%x cr2=%08x pm=%u cs=%04x:%08x ss=%04x:%08x ax=%08x bx=%08x cx=%08x dx=%08x si=%08x di=%08x bp=%08x ds=%04x es=%04x depth=%u\n",
           gv->info.reason, gv->info.fault_vector, gv->info.fault_eip, gv->info.fault_error, gv->info.fault_address, gv->frame_pm,
           f->tf.cs & 0xFFFF, f->tf.eip, f->tf.user_ss & 0xFFFF, f->tf.user_esp, f->tf.eax, f->tf.ebx, f->tf.ecx, f->tf.edx,
           f->tf.esi, f->tf.edi, f->tf.ebp, (gv->frame_pm ? f->tf.ds : f->ds) & 0xFFFF, (gv->frame_pm ? f->tf.es : f->es) & 0xFFFF, gv->dpmi.depth);
    printf("  code:");
    for (unsigned i = 0; i < sizeof(code); i++) printf(" %02x", code[i]);
    printf("\n");
}
/* f3-06: the BIOS buffer holds 15 keys; long command lines (RUNREC
 * launchers) wait for the shell to take keys instead of failing. */
static bool key_room(void) { return !dosvm_keyboard_refresh(gv) && gv->key_count < 14; }
static void type(const char *command)
{
    for (unsigned i = 0; ; i++) {
        uint16_t code = command[i] ? (uint8_t)command[i] : 0x1C0D;
        for (unsigned tries = 0; !dosvm_keyboard(dosvm_key(gv), code); tries++) {
            CHECK(tries < 1000 && !stopped());
            waiting_input = false;
            run(2000000ull, key_room);
        }
        if (!command[i]) break;
    }
    waiting_input = false;
}
/* f3-02: a line typed the way /bin/dosbox forwards a window's keys. */
static void window_key(unsigned code, int value)
{
    struct ciuki_input_event e = {.type = CIUKI_INPUT_KEY, .code = (int32_t)code, .value = value};
    CHECK(dosvm_input_event(dosvm_key(gv), &e));
}
static unsigned window_code(char c, bool *shift)
{
    static const char row1[] = "1234567890", qwerty[] = "qwertyuiop", home[] = "asdfghjkl", bottom[] = "zxcvbnm";
    char lower = c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
    *shift = c >= 'A' && c <= 'Z';
    const char *at;
    if ((at = strchr(row1, lower)) && lower) return 0x02 + (unsigned)(at - row1);
    if ((at = strchr(qwerty, lower)) && lower) return 0x10 + (unsigned)(at - qwerty);
    if ((at = strchr(home, lower)) && lower) return 0x1E + (unsigned)(at - home);
    if ((at = strchr(bottom, lower)) && lower) return 0x2C + (unsigned)(at - bottom);
    switch (c) {
    case '\\': return 0x2B; case '.': return 0x34; case ' ': return 0x39; case '-': return 0x0C;
    case ':': *shift = true; return 0x27;
    default: CHECK(false); return 0;
    }
}
static void window_type(const char *command)
{
    CHECK(dosvm_input_focus(dosvm_key(gv), true));
    bool held = false;
    for (unsigned i = 0; ; i++) {
        bool shift = false;
        unsigned code = command[i] ? window_code(command[i], &shift) : 0x1C;
        if (shift != held) { window_key(0x2A, shift); held = shift; }
        window_key(code, 1); window_key(code, 0);
        if (!command[i]) break;
    }
    if (held) window_key(0x2A, 0);
    waiting_input = false;
}
/* VMVIDEO through window keys: stage 1 exactly, then Enter per stage. */
static int window_vmvideo(const char *command)
{
    /* The shell is blocked in INT 16h AH=00h before the first key, as when
     * the runner starts typing after the ARM record. */
    waiting_input = false;
    int r = run(400000000ull, 0);
    printf("  -> shell idle: wait=%d (2 = blocked in INT 16h AH=00h)\n", r);
    if (r != 2) return 1;
    window_type(command);
    r = run(2000000000ull, 0);
    struct dosvm_frame frame;
    char hex[65] = "none";
    if (!dosvm_frame_digest(gv, 0, &frame)) sha256_hex(frame.digest, hex);
    printf("  -> window keys: wait=%d stage=%u flags=%u mode=%u %ux%u text=%u queued=%u bios=%u digest=%s\n", r,
           gv->info.video_stage, gv->info.video_flags, frame.mode, frame.width, frame.height, frame.text,
           gv->kbd.count, gv->key_count, hex);
    if (r != 2 || gv->info.video_stage != 1 || gv->info.video_flags != 3 || frame.mode != 3 ||
        frame.width != 640 || frame.height != 400 || !frame.text || gv->kbd.count) return 1;
    for (uint32_t stage = 2; stage <= 6; stage++) {
        window_key(0x1C, 1); window_key(0x1C, 0);
        waiting_input = false;
        r = run(2000000000ull, 0);
        printf("  -> Enter: wait=%d stage=%u flags=%u\n", r, gv->info.video_stage, gv->info.video_flags);
        if (r != 2 || gv->info.video_stage != stage) return 1;
    }
    return prompt() ? 0 : 1;
}
static void stats(const char *name)
{
    struct dpmi_info info;
    CHECK(dosvm_dpmi_info(dosvm_key(gv), &info));
    printf("  dpmi %s: active=%u clients=%u selectors=%u blocks=%u callbacks=%u watch=%u depth=%u pool=%u entries=%u fails=%u term=%u faults=%u irq_pm=%u irq_rm=%u soft=%u exc=%u cb=%u raw=%u realcalls=%u max_depth=%u stale=%u\n",
           name, info.active, info.clients, info.selectors, info.blocks, info.callbacks, info.watchpoints, info.depth,
           info.pool_used, info.stats.entries, info.stats.entry_failures, info.stats.terminations, info.stats.faults,
           info.stats.irqs_pm, info.stats.irqs_reflected, info.stats.soft_reflected, info.stats.exceptions,
           info.stats.callbacks, info.stats.raw_switches, info.stats.realcalls, info.stats.max_depth, info.stats.stale);
    printf("  host real-mode stack lowest SP=%04x\n", rstack_low);
    struct dosvm_video_info video;
    if (dosvm_video_info(dosvm_key(gv), &video))
        printf("  video: mode=%02x unchained=%u %ux%u faults=%u pm_faults=%u flips=%u windows=%u harvested=%u\n",
               video.mode, video.unchained, video.width, video.height, video.faults, video.pm_faults, video.starts,
               video.maps, video.flushes);
    printf("  rows ok=%08x%08x%08x fail=%08x%08x%08x\n", info.stats.rows_ok[2], info.stats.rows_ok[1], info.stats.rows_ok[0],
           info.stats.rows_fail[2], info.stats.rows_fail[1], info.stats.rows_fail[0]);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s DOSBOOT.IMG [COMMAND NAME]...\n", argv[0]); return 2; }
    setvbuf(stdout, 0, _IOLBF, 0);
    ram = calloc(HOST_PAGES, PAGE_SIZE); CHECK(ram);
    disk_image = fopen(argv[1], "rb"); CHECK(disk_image);
    gcpu_init(&guest_ops);
    struct dosvm_config config = {.commitment = 0, .probe = "dpmi", .dpmi = true};
    CHECK(dosvm_create(&config, &gv) == 0);
    CHECK(dosvm_boot(gv) == 0);
    cpu = gv->frame;
    int failures = 0;
    sleep_hook = input_hook;
    int r = run(400000000ull, booted);
    printf("boot: %s after %llu instructions, %llu traps, %llu ms\n", r > 0 ? "prompt" : "FAILED",
           (unsigned long long)instructions, (unsigned long long)traps, (unsigned long long)g_ticks);
    if (r <= 0) { screen(); return 1; }
    for (int i = 2; i + 1 < argc; i += 2) {
        bool fault = !strncmp(argv[i], "fault:", 6), gate = !strncmp(argv[i], "gate:", 5);
        /* f3-06 rc: the payload reports a child's exit code (RUNREC): any
         * verdict counts; the shell then gets its prompt back, and the screen
         * latched at the verdict must still hold GUEST_EXPECT_TEXT. */
        bool rc = !strncmp(argv[i], "rc:", 3);
        if (!strncmp(argv[i], "keys:", 5)) {
            CHECK(!strcmp(argv[i + 1], "VMVIDEO"));
            printf("run %s through window keys (expect VMVIDEO stage 1, then stages 2-6)\n", argv[i] + 5);
            if (window_vmvideo(argv[i] + 5)) { failures++; screen(); break; }
            continue;
        }
        const char *command = fault ? argv[i] + 6 : gate ? argv[i] + 5 : rc ? argv[i] + 3 : argv[i];
        gv->dpmi_enabled = !gate;       /* gate: as a VM created without dosvm_config.dpmi */
        uint64_t start = instructions, ms = g_ticks;
        dosvm_expect(gv, argv[i + 1]);
        printf("run %s (expect %s%s)\n", command, argv[i + 1], fault ? ", VM fault" : gate ? ", no DPMI host" : "");
        type(command);
        /* GUEST_BUDGET (instructions per command): long workloads (f3-06 DOOM). */
        const char *budget = getenv("GUEST_BUDGET");
        r = run(budget ? strtoull(budget, 0, 10) : 2000000000ull, fault ? 0 : verdict);
        printf("  -> %s %s detail=%s state=%u reason=%u vector=%u after %llu instructions, %llu ms\n",
               argv[i + 1], gv->info.verdict_done ? (gv->info.verdict_pass ? "PASS" : "FAIL") : "NO-VERDICT",
               gv->info.verdict_detail, gv->info.state, gv->info.reason, gv->info.fault_vector,
               (unsigned long long)(instructions - start), (unsigned long long)(g_ticks - ms));
        stats(argv[i + 1]);
        fault_dump();
        if (fault) {
            if (!stopped() || gv->info.reason != DOSVM_FAULT) failures++;
            break;
        }
        if (gate) {
            bool refused = gv->info.verdict_done && !gv->info.verdict_pass && !strcmp(gv->info.verdict_detail, "01_01");
            r = run(400000000ull, 0);
            if (!refused || r != 2 || gv->dpmi.stats.entries) { failures++; screen(); break; }
            continue;
        }
        if (rc) {
            if (!gv->info.verdict_done) { failures++; screen(); break; }
            r = run(400000000ull, 0);
            printf("  live screen with the prompt back:\n"); screen();
            printf("  screen latched at the verdict line:\n"); screen_of(dosvm_verdict_text(gv), "latched |");
            const char *expect = getenv("GUEST_EXPECT_TEXT");
            if (r != 2 || gv->dpmi.active || (expect && !latched_has(expect))) { failures++; break; }
            continue;
        }
        if (!gv->info.verdict_pass) { failures++; screen(); break; }
        r = run(400000000ull, 0);
        stats("after-exit");
        if (r != 2 || gv->dpmi.active) { failures++; screen(); break; }
    }
    printf("guest runner: %s (%llu instructions, %llu traps, %u unexpected)\n", failures || unexpected ? "FAIL" : "PASS",
           (unsigned long long)instructions, (unsigned long long)traps, unexpected);
    if (cow) { fclose(cow); unlink(cow_path); }
    gcpu_done();
    return failures || unexpected;
}
