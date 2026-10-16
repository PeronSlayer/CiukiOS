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
 * f3-10 menu:COMMAND NAME continues beyond video startup, checks IRQ0/DMA
 * progress, opens/navigates/closes DOOM's menu through the virtual 8042 and
 * compares the rendered title/menu frames. GUEST_MENU_SOUND=1 additionally
 * requires SB transfer/IRQ progress (leave unset for -nosound).
 * GUEST_IRQ0_RESTORE=<PM EIP> advances the PIT once at that armed POPF
 * #DB, before the production monitor restores virtual IF. This forces an
 * IRQ0 edge in the return-boundary window without changing guest bytes.
 * GUEST_UART_BAUD=38400 with GUEST_VERBOSE=1 advances device time during
 * monitor diagnostic output (8N1). This models the physical timer running
 * while the kernel polls the UART; the zero-time logger misses IRQ nesting.
 * SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <stdbool.h>
#include <time.h>
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
#undef CLOCKS_PER_SEC
#include <ciuki/abi.h>
#define CIUKI_CPU_H
uint64_t g_tsc_per_ms;
static bool guest_perf;
static uint64_t read_tsc_serialized(void)
{
    if (!guest_perf) return 0;
    struct timespec t;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t)) abort();
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
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
static unsigned guest_slice = SLICE;
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
struct ciuki_boot_info g_boot;
static uint32_t read_cr3(void) { return cpu_cr3; }
static void write_cr3(uint32_t x) { cpu_cr3 = x; }
static uint32_t read_cr2(void) { return fake_cr2; }
static void cli(void) { cpu_flags = 0; }
static void sti(void) { cpu_flags = V86_IF; }
static void invlpg(uint32_t va) { (void)va; }
static uint32_t *table_va(uint32_t p) { return P2V(p); }
static uint32_t irq_save(void) { uint32_t f = cpu_flags; cli(); return f; }
static void irq_restore(uint32_t f) { cpu_flags = f; }
static unsigned uart_baud, uart_fraction, uart_records;
static uint64_t uart_ms;
static void uart_delay(unsigned bytes)
{
    if (!uart_baud) return;
    CHECK(cpu_flags & V86_IF);
    /* 8N1: ten wire bits per byte; retain sub-millisecond fractions. */
    uint64_t wire = uart_fraction + (uint64_t)bytes * 10000;
    unsigned ms = wire / uart_baud;
    uart_fraction = wire % uart_baud;
    uart_records++;
    uart_ms += ms;
    g_ticks += ms;
    dosvm_tick();
}

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
#include "dosvm_fullscreen_stubs.inc"
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
void klog(const char *fmt, ...)
{
    if (!uart_baud) return;
    char line[256];
    va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof(line), fmt, ap); va_end(ap);
    printf("  log %s\n", line);
    uart_delay((unsigned)strlen(line) + 2); /* CRLF */
}
void klog_diag(const char *fmt,...)
{
    if (!(g_boot.flags & CBI_F_DIAG_VERBOSE) || !uart_baud) return;
    char line[256];va_list ap;va_start(ap,fmt);
    vsnprintf(line,sizeof(line),fmt,ap);va_end(ap);klog("%s",line);
}

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
void kpreempt_point(void) { if ((cpu_flags & V86_IF) && g_need_resched) schedule(); }
void crit_begin(void) { }
void crit_end(void) { }
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
/* The image's real size: a cut base (the bundled games disk, owner decision
 * 2026-10-12) must be read through dosvm_disk.c's zero fill, never past its end. */
static uint64_t disk_bytes;
int vfs_stat(struct vfs_table *table, const char *path, struct fat_entry *entry)
{ (void)table; (void)path; memset(entry, 0, sizeof(*entry)); entry->size = (uint32_t)disk_bytes; return 0; }
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
    CHECK(handle || pos[0] + bytes <= disk_bytes);
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
bool rec_active(void) { return true; } /* host replay owns its record scope */
void rec_diag_disk_bootlog(const char *probe, const char *fmt, ...)
{
    if (!guest_perf) return;
    char extra[240]; va_list ap; va_start(ap, fmt);
    int n = vsnprintf(extra, sizeof(extra), fmt, ap); va_end(ap);
    CHECK(n >= 0 && n < (int)sizeof(extra));
    printf("  perf %s %s\n", probe, extra);
}
void rec_diag(const char *probe, const char *fmt, ...)
{
    if (!(g_boot.flags & CBI_F_VERBOSE)) return;
    CHECK(cpu_flags & V86_IF);
    char extra[240]; va_list ap; va_start(ap,fmt);
    int n=vsnprintf(extra,sizeof(extra),fmt,ap); va_end(ap);
    CHECK(n>=0 && n<(int)sizeof(extra));
    rec_emit(probe,"DATA","%s",extra);
    char wire[512];
    n = snprintf(wire, sizeof(wire), "CIUKI_TEST v=1 run=00000000 seq=000000 probe=%s event=DATA %s\r\n", probe, extra);
    CHECK(n > 0 && n < (int)sizeof(wire));
    uart_delay((unsigned)n);
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
/* Abandon an empty BIOS keyboard wait or cooperative DOS input idle call.
 * The fake CPU retains the pre-trap frame and retries that same INT when
 * typing resumes. f3-17's COMMAND prompt yields via INT 28h before INT 16h;
 * treating only F000:0160 as idle would spin through its whole test budget.
 * This is a host transport shortcut, never a change to guest instructions. */
static void input_hook(void)
{
    if (!gv || gv->frame_pm || gv->key_count || dosvm_kbd_irq_ready(gv)) return;
    uint8_t op[2] = {0};
    uint32_t ip = gv->frame.tf.eip;
    bool cooperative = gv->idle.sleeping && ip >= 2 &&
        !dosvm_read(gv, (gv->frame.tf.cs << 4) + ip - 2, op, sizeof(op)) && op[0] == 0xCD &&
        (op[1] == 0x28 || (op[1] == 0x2F && (uint16_t)gv->frame.tf.eax == 0x1680));
    if (cooperative || (gv->frame.tf.cs == 0xF000 && ip == 0x16 * 16 + 2)) {
        gv->info.state = DOSVM_RUNNABLE;
        gv->idle.sleeping = false; gv->idle.last_tick = g_ticks;
        waiting_input = true;
        longjmp(idle_return, 1);
    }
}
static struct v86_frame cpu;
static uint64_t instructions, traps;
/* Optional startup cost replay, usable with both pre-f3-14 and current VM
 * sources. Classify the trapped bytes, never execute or patch guest code. */
static uint64_t replay_cost[14];
static unsigned replay_cause(const struct v86_frame *f)
{
    unsigned vector = f->tf.vector;
    bool pm = !(f->tf.eflags & V86_VM);
    if (vector == 1) return 11;
    if (vector == 0x20) return 12;
    if (vector == 14 && fake_cr2 >= 0xA0000 && fake_cr2 < 0xC0000) return 7;
    if (vector != 13) return 13;
    if (pm && (f->tf.err & 3) == 2) return 3;
    uint8_t bytes[15] = {0};
    uint32_t linear = pm ? dpmi_fault_linear(gv, f) : (f->tf.cs << 4) + f->tf.eip;
    if (dosvm_read(gv, linear, bytes, sizeof(bytes))) return 13;
    unsigned at = 0;
    while (at < sizeof(bytes) && (bytes[at] == 0x66 || bytes[at] == 0x67 || bytes[at] == 0xF2 || bytes[at] == 0xF3 ||
        bytes[at] == 0x26 || bytes[at] == 0x2E || bytes[at] == 0x36 || bytes[at] == 0x3E || bytes[at] == 0x64 || bytes[at] == 0x65)) at++;
    if (at == sizeof(bytes)) return 13;
    unsigned op = bytes[at];
    if (!pm && op == 0xCD) return 0;
    if (!pm && op == 0xCF) return 1;
    if ((op >= 0xE4 && op <= 0xE7) || (op >= 0xEC && op <= 0xEF) || (op >= 0x6C && op <= 0x6F)) {
        unsigned port = (uint16_t)f->tf.edx;
        if (op >= 0xE4 && op <= 0xE7) {
            if (at + 1 == sizeof(bytes)) return 13;
            port = bytes[at + 1];
        }
        if (port >= 0x3B0 && port <= 0x3DF) return 6;
        if ((port >= 0x40 && port <= 0x43) || port == 0x61 || port == 0x20 || port == 0x21 || port == 0xA0 || port == 0xA1) return 8;
        if (port == 0x60 || port == 0x64) return 9;
        return 10;
    }
    return pm ? (op == 0xFA || op == 0xFB ? 4 : 13) : 2;
}
static uint64_t db_rm, db_flags, db_return, db_other;
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
#include "dosvm_guest_exec.inc"
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
    unsigned cost = getenv("GUEST_COSTS") ? replay_cause(&hw) : 13;
    g_current = gv->task; gv->continuation = 0x12345678; cpu_flags = 0;
    traps++;
    if (vector == 1) {
        const char *owner = dosvm_step_owner(gv, &cpu, dpmi_debug_status(gv, debug_dr6));
        if (!strcmp(owner, "rm_ss") || !strcmp(owner, "rm_irq")) db_rm++;
        else if (!strcmp(owner, "pm_flags")) db_flags++;
        else if (!strcmp(owner, "pm_return")) db_return++;
        else db_other++;
    }
    if (setjmp(idle_return)) { g_current = &caller; sti(); return; }
    if (!setjmp(guest_return)) { CHECK(dosvm_trap(&hw.tf)); cpu = hw; }
    else cpu = gv->frame;
    if (getenv("GUEST_COSTS")) {
        if ((hw.tf.eflags & V86_VM) != (cpu.tf.eflags & V86_VM)) cost = 5;
        replay_cost[cost]++;
    }
    g_current = &caller; sti();
}
static bool stopped(void) { return gv->info.state >= DOSVM_STOPPING; }
/* f3-09 startup replay: reach the application's unchained graphics and
 * two page flips with a running PM timer. This is not a timedemo verdict. */
static uint32_t startup_irqs;
static unsigned restore_edges;
static bool video_started(void)
{
    struct dosvm_video_info video;
    return dosvm_video_info(gv->info.key, &video) && video.mode == 0x13 && video.unchained &&
        video.starts >= 2 && gv->dpmi.stats.irqs_pm - startup_irqs >= 10;
}
/* Run until done() or the VM stops or the instruction budget runs out. */
static int perf_row(void *arg, unsigned y, const void *pixels, unsigned bytes)
{ (void)arg; (void)y; (void)pixels; (void)bytes; return 0; }
static int run(uint64_t budget, bool (*done)(void))
{
    if (guest_perf) { dosvm_task_switch(gv->task); perf_phase(gv, 1); }
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
            quake_exec_trap(&ev);
            trace_trap(&ev);
            bool was_pm = frame_pm();
            /* DR6 is sticky until arch_debug_take clears it. Disabled
             * breakpoint comparisons are permitted alongside BS (SDM
             * 18.2.3); opt in for replay without altering the guest image. */
            debug_dr6 |= ev.dr6;
            const char *restore_pc = getenv("GUEST_IRQ0_RESTORE");
            if (restore_pc && !restore_edges && s.pm && ev.vector == 1 &&
                (ev.dr6 & (1u << 14)) && gv->dpmi.if_armed &&
                s.eip == strtoul(restore_pc, 0, 0)) {
                uint64_t edges = gv->pit.edges;
                dosvm_pit_advance(gv, 8);
                restore_edges++;
                printf("restore edge: pc=%04x:%08x vif=%u armed=%u irr=%02x isr=%02x edges=%llu->%llu\n",
                       s.cs, s.eip, gv->monitor.vif, gv->dpmi.if_armed,
                       gv->monitor.pic[0].irr, gv->monitor.pic[0].isr,
                       (unsigned long long)edges, (unsigned long long)gv->pit.edges);
            }
            const char *unused = getenv("GUEST_DR6_UNUSED");
            if (unused && (ev.dr6 & 0x4000)) {
                uint32_t bits = (uint32_t)strtoul(unused, 0, 0) & 15;
                for (unsigned j = 0; j < 4; j++)
                    if (debug_dr7 & (1u << (2 * j))) bits &= ~(1u << j);
                debug_dr6 |= bits;
            }
            deliver(ev.vector, ev.err, ev.cr2);
            if (quake_exec_mode && quake_alias_calls && !quake_alias_returned &&
                frame_pm() && cpu.tf.cs == quake_alias_cs && cpu.tf.eip == quake_alias_ip &&
                !(cpu.tf.eflags & V86_CF)) {
                struct dpmi_seg source, alias;
                CHECK(dpmi_seg(gv, cpu.tf.ds, &source) && dpmi_seg(gv, cpu.tf.eax, &alias));
                CHECK(!alias.code && alias.writable && alias.base == source.base && alias.limit == source.limit);
                quake_alias_returned = true;
            }
            if (trace && was_pm != frame_pm())
                printf("    trace switch -> %s %04x:%08x ss:sp=%04x:%08x depth=%u\n", frame_pm() ? "PM" : "V86",
                       cpu.tf.cs & 0xFFFF, cpu.tf.eip, cpu.tf.user_ss & 0xFFFF, cpu.tf.user_esp, gv->dpmi.depth);
        }
        if (stopped() || unexpected) break;
        if (ev.executed >= slice_left && !(rc == GCPU_BUDGET && ev.ss_shadow)) {
            slice_left = guest_slice;
            g_ticks++;
            if (guest_perf) perf_phase(gv, 2);
            dosvm_tick();
            deliver(0x20, 0, 0);        /* the host's 1 kHz timer IRQ: a delivery point */
            if (guest_perf && !(g_ticks % 16)) {
                dosvm_task_switch(&caller);
                struct ciuki_rect damage;
                dosvm_vga_present(gv, perf_row, 0, g_ticks, &damage);
                dosvm_task_switch(gv->task);
            }
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
/* f3-02: a line typed the way /Programs/dosbox forwards a window's keys. */
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
    /* The actual COMMAND.COM prompt must release its slice before typing. */
    waiting_input = false;
    uint32_t blocks = gv->idle.blocks;
    int r = run(400000000ull, 0);
    printf("  -> shell idle: wait=%d idle_blocks=%u idle_reasons=%x\n", r, gv->idle.blocks, gv->idle.reasons);
    if (r != 2 || gv->idle.blocks <= blocks || !(gv->idle.reasons & (1u << DOSVM_IDLE_28))) return 1;
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
static uint64_t menu_until;
static bool menu_elapsed(void) { return g_ticks >= menu_until; }
static bool menu_wait(uint32_t ms)
{
    menu_until = g_ticks + ms;
    return run((uint64_t)ms * guest_slice * 4, menu_elapsed) == 1;
}
static bool menu_frame(uint8_t digest[32])
{
    struct dosvm_frame f;
    if (dosvm_frame_digest(gv, 0, &f) || f.text || f.mode != 0x13) return false;
    memcpy(digest, f.digest, sizeof(f.digest));
    return true;
}
static bool menu_timer_progress(uint32_t ticks, uint64_t begin, uint64_t wire_begin,
                                unsigned low, unsigned high)
{
    if (!uart_baud) return ticks >= low && ticks <= high;
    /* The PIT keeps running during output but the PIC has one IRR bit:
     * several edges can coalesce while the worker polls the UART. Bound
     * deliveries by runnable time and total wall time, retaining the
     * original eight-tick phase tolerance. A stopped timer cannot pass. */
    uint64_t elapsed = g_ticks - begin, wire = uart_ms - wire_begin;
    CHECK(elapsed >= wire);
    uint64_t minimum = (elapsed - wire) * 140 / 1000;
    return minimum > 8 && ticks >= minimum - 8 && ticks <= elapsed * 140 / 1000 + 8;
}
/* Compare stable title -> Escape menu -> Down item -> Escape title. Timer
 * delivery alone is not proof that DOOM consumes its input: rendering must
 * change and closing the menu must restore the original title pixels. */
static bool menu_replay(void)
{
    bool sound = getenv("GUEST_MENU_SOUND") != 0;
    uint8_t title[32], menu[32], down[32], closed[32];
    if (getenv("GUEST_MENU_RING")) ring_on = 1;
    CHECK(dosvm_input_focus(dosvm_key(gv), true));
    if (!menu_wait(500) || !menu_frame(title)) return false;
    uint64_t begin = g_ticks, wire_begin = uart_ms;
    uint32_t timer = gv->monitor.stats.delivered[0], keys = gv->monitor.stats.delivered[1];
    uint32_t reads = gv->kbd.reads, units = gv->audio.sb.units, sb_irqs = gv->audio.sb.irq_edges;
    uint32_t flips = gv->video_starts;
    window_key(0x01, 1); window_key(0x01, 0);
    if (!menu_wait(400) || !menu_frame(menu)) return false;
    window_key(0x150, 1); window_key(0x150, 0);
    if (!menu_wait(400) || !menu_frame(down)) return false;
    window_key(0x01, 1); window_key(0x01, 0);
    if (!menu_wait(400) || !menu_frame(closed)) return false;
    uint32_t ticks = gv->monitor.stats.delivered[0] - timer;
    uint32_t irq1 = gv->monitor.stats.delivered[1] - keys, bytes = gv->kbd.reads - reads;
    uint32_t transfers = gv->audio.sb.units - units, edges = gv->audio.sb.irq_edges - sb_irqs;
    bool opened = memcmp(title, menu, 32) != 0, moved = memcmp(menu, down, 32) != 0;
    bool restored = memcmp(title, closed, 32) == 0;
    bool images = opened && moved && restored;
    bool pass = images && menu_timer_progress(ticks, begin, wire_begin, 160, 175) && irq1 == 8 && bytes == 8 && !gv->kbd.count &&
                gv->video_starts > flips && (!sound || (transfers && edges));
    printf("menu replay: %s ms=%llu uart_ms=%llu irq0=%u irq1=%u bytes=%u images=%u/%u/%u flips=%u dma_units=%u sb_edges=%u vif=%u pending=%02x/%02x\n",
           pass ? "PASS" : "FAIL", (unsigned long long)(g_ticks - begin), (unsigned long long)(uart_ms - wire_begin), ticks, irq1, bytes, opened, moved, restored, gv->video_starts - flips,
           transfers, edges, gv->monitor.vif, gv->monitor.pic[0].irr, gv->monitor.pic[0].isr);
    if (!pass) {
        printf("menu PC: %04x:%08x linear=%08x flags=%08x arm=%u sh=%u depth=%u seed=%04x:%08x\n",
               cpu.tf.cs & 0xFFFF, cpu.tf.eip, dpmi_fault_linear(gv, &cpu), cpu.tf.eflags,
               gv->dpmi.if_armed, gv->dpmi.shadow, gv->dpmi.depth, gv->dpmi.decode_cs, gv->dpmi.decode_eip);
        ring_dump();
    }
    if (!pass) return false;
    /* Continue long enough for two periodic verbose samples and for a
     * stall after the last key to fail. The title digest comparison above
     * finishes before DOOM's attract sequence can replace that page. */
    begin = g_ticks; wire_begin = uart_ms; timer = gv->monitor.stats.delivered[0];
    units = gv->audio.sb.units; sb_irqs = gv->audio.sb.irq_edges; flips = gv->video_starts;
    if (!menu_wait(2400)) return false;
    ticks = gv->monitor.stats.delivered[0] - timer;
    transfers = gv->audio.sb.units - units; edges = gv->audio.sb.irq_edges - sb_irqs;
    pass = menu_timer_progress(ticks, begin, wire_begin, 330, 340) && gv->video_starts > flips &&
           (!sound || (transfers && edges));
    printf("menu sustain: %s ms=%llu uart_ms=%llu irq0=%u flips=%u dma_units=%u sb_edges=%u heartbeats=%u\n",
           pass ? "PASS" : "FAIL", (unsigned long long)(g_ticks - begin), (unsigned long long)(uart_ms - wire_begin), ticks,
           gv->video_starts - flips, transfers, edges, gv->trace.heartbeats);
    return pass;
}
static void stats(const char *name)
{
    if (getenv("GUEST_COSTS")) {
        static const char *const causes[14] = {"rm_int", "rm_iret", "rm_flags", "pm_int", "pm_flags", "switch",
            "vga_io", "vga_pf", "pit_pic", "kbd", "io", "db", "timer", "other"};
        for (unsigned i = 0; i < 14; i++) printf("  replay_cost phase=%s cause=%s exits=%llu\n", name, causes[i],
            (unsigned long long)replay_cost[i]);
        printf("  replay_vectors phase=%s gp=%u pf=%u db=%u timer=%u other=%u pm=%u\n", name,
            gv->perf.exits[0], gv->perf.exits[1], gv->perf.exits[2], gv->perf.exits[3], gv->perf.exits[4], gv->perf.exits[5]);
    }
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
    if (argc < 2) { fprintf(stderr, "usage: %s DOSBOOT.IMG|CIUKIGAM.IMG [COMMAND NAME]...\n", argv[0]); return 2; }
    setvbuf(stdout, 0, _IOLBF, 0);
    guest_perf = getenv("GUEST_PERF") != 0;
    if (guest_perf) {
        g_tsc_per_ms = 1000000;
        puts("host perf: CPU=thread nanoseconds, interval ms=instruction-clock; guest includes emulation, paging/IRQ transport are fakes; NOT hardware share/FPS");
    }
    const char *baud = getenv("GUEST_UART_BAUD");
    if (baud) {
        char *end;
        unsigned long value = strtoul(baud, &end, 10);
        CHECK(*baud && !*end && value >= 110 && value <= 4000000 && getenv("GUEST_VERBOSE"));
        uart_baud = (unsigned)value;
    }
    const char *slice = getenv("GUEST_SLICE");
    if (slice) {
        unsigned long value = strtoul(slice, 0, 10);
        CHECK(value >= 100 && value <= 1000000);
        slice_left = guest_slice = (unsigned)value;
    }
    ram = calloc(HOST_PAGES, PAGE_SIZE); CHECK(ram);
    disk_image = fopen(argv[1], "rb"); CHECK(disk_image);
    if (getenv("GUEST_VERBOSE")) g_boot.flags |= CBI_F_VERBOSE;
    CHECK(fseek(disk_image, 0, SEEK_END) == 0); disk_bytes = (uint64_t)ftell(disk_image);
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
    if (getenv("GUEST_COSTS")) stats("boot");
    for (int i = 2; i + 1 < argc; i += 2) {
        bool fault = !strncmp(argv[i], "fault:", 6), gate = !strncmp(argv[i], "gate:", 5);
        /* f3-06 rc: the payload reports a child's exit code (RUNREC): any
         * verdict counts; the shell then gets its prompt back, and the screen
         * latched at the verdict must still hold GUEST_EXPECT_TEXT. */
        bool rc = !strncmp(argv[i], "rc:", 3);
        bool video = !strncmp(argv[i], "video:", 6);
        bool menu = !strncmp(argv[i], "menu:", 5);
        quake_exec_mode = !strncmp(argv[i], "quake:", 6);
        if (!strncmp(argv[i], "keys:", 5)) {
            CHECK(!strcmp(argv[i + 1], "VMVIDEO"));
            printf("run %s through window keys (expect VMVIDEO stage 1, then stages 2-6)\n", argv[i] + 5);
            if (window_vmvideo(argv[i] + 5)) { failures++; screen(); break; }
            continue;
        }
        const char *command = fault ? argv[i] + 6 : gate ? argv[i] + 5 : rc ? argv[i] + 3 : menu ? argv[i] + 5 : video || quake_exec_mode ? argv[i] + 6 : argv[i];
        gv->dpmi_enabled = !gate;       /* gate: as a VM created without dosvm_config.dpmi */
        uint64_t start = instructions, ms = g_ticks;
        dosvm_expect(gv, argv[i + 1]);
        startup_irqs = gv->dpmi.stats.irqs_pm;
        printf("run %s (expect %s%s)\n", command, argv[i + 1], fault ? ", VM fault" : gate ? ", no DPMI host" : "");
        type(command);
        /* GUEST_BUDGET (instructions per command): long workloads (f3-06 DOOM). */
        const char *budget = getenv("GUEST_BUDGET");
        r = run(budget ? strtoull(budget, 0, 10) : 2000000000ull,
                fault ? 0 : quake_exec_mode ? quake_exec_done : video || menu ? video_started : verdict);
        if (quake_exec_mode) {
            bool pass = r == 1 && quake_alias_returned && quake_alias_calls == 1 &&
                gv->dpmi.active && !gv->info.verdict_done && !gv->dpmi.stats.terminations && !gv->dpmi.stats.faults;
            printf("f3-28 Quake CRT data alias: %s (EXEC=%u reopened=%u aliases=%u active=%u entries=%u terminations=%u verdict=%s)\n",
                   pass ? "PASS" : "FAIL", quake_exec_checked, quake_reopened, quake_alias_calls,
                   gv->dpmi.active, gv->dpmi.stats.entries, gv->dpmi.stats.terminations, gv->info.verdict_detail);
            if (!pass) { failures++; screen(); }
            break;
        }
        if (video || menu) {
            printf("video startup replay: %s (%llu instructions, %llu ms, PM timer IRQs=%u)\n",
                   r == 1 ? "PASS" : "FAIL", (unsigned long long)(instructions - start),
                   (unsigned long long)(g_ticks - ms), gv->dpmi.stats.irqs_pm - startup_irqs);
            stats("video-startup");
            if (r != 1) { failures++; screen(); }
            else if (menu && !menu_replay()) { failures++; fault_dump(); screen(); }
            break;
        }
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
    if (guest_perf) {
        perf_phase(gv, 0);
        printf("host totals: guest_emulator_us=%llu monitor_us=%llu present_us=%llu harvest_us=%llu audio_us=%llu\n",
               (unsigned long long)perf_us(gv->perf.guest_cycles), (unsigned long long)perf_us(gv->perf.monitor_cycles),
               (unsigned long long)perf_us(gv->perf.present_cycles), (unsigned long long)perf_us(gv->perf.harvest_cycles),
               (unsigned long long)perf_us(gv->perf.audio_cycles));
    }
    if (getenv("GUEST_IRQ0_RESTORE") && restore_edges != 1) {
        printf("restore edge: FAIL (requested PM boundary was not reached)\n");
        failures++;
    }
    printf("debug traps: rm=%llu flags=%llu returns=%llu other=%llu\n",
           (unsigned long long)db_rm, (unsigned long long)db_flags,
           (unsigned long long)db_return, (unsigned long long)db_other);
    if (uart_baud) printf("UART timing: baud=%u records=%u ms=%llu\n", uart_baud, uart_records, (unsigned long long)uart_ms);
    printf("guest runner: %s (%llu instructions, %llu traps, %u unexpected)\n", failures || unexpected ? "FAIL" : "PASS",
           (unsigned long long)instructions, (unsigned long long)traps, unexpected);
    if (cow) { fclose(cow); unlink(cow_path); }
    gcpu_done();
    return failures || unexpected;
}
