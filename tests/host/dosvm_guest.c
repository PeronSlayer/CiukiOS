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
 * signature and the production TF tracker selection.
 * GUEST_TRACKER=tf|breakpoint selects the production VM platform flag for
 * comparison on the architectural host CPU (including segmented #DB).
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
 * war2vbe: GUEST_INT10_TRACE=1 prints the INT 10h calls the VM's BIOS serves;
 * GUEST_PLAY_KEYS=01@12000,... presses window keys during play:, whose end
 * prints the window picture (H61 PLAY FRAME, GUEST_PLAY_SHOT=<file.ppm>).
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
static void host_clock_tick(void);
#define CIUKI_CPU_H
uint64_t g_tsc_per_ms;
static bool guest_perf;
static uint64_t presenter_sample_max;
/* fx-27: the monitor's clock reads are counted by kind. On the T23 a
 * dosvm_perf_clock read is read_tsc_serialized (two CPUIDs and RDTSC) and a
 * dosvm_perf_task_clock read is cpu_path_clock (irq_save/RDTSC); here both
 * are a clock_gettime system call that costs far more than either. */
static uint64_t host_clock_calls;
static uint64_t read_tsc_serialized(void)
{
    if (!guest_perf) return 0;
    host_clock_calls++;
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
#ifdef CIUKI_DOSVM_FILE_CPU
static unsigned h50_fail_pages;
#endif
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
    host_clock_tick();
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
/* G5b: the DOS clipboard upcall resolves its client process and copies
 * through user memory; the guest-execution harness never posts a request. */
struct process *proc_find(uint32_t pid) { (void)pid; return NULL; }
int copy_from_user(void *dst, uint32_t src, uint32_t bytes) { (void)dst; (void)src; (void)bytes; return -EFAULT; }
int copy_to_user(uint32_t dst, const void *src, uint32_t bytes) { (void)dst; (void)src; (void)bytes; return -EFAULT; }
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
uint32_t pmm_alloc_contig(uint32_t pages)
{
#ifdef CIUKI_DOSVM_FILE_CPU
    if (h50_fail_pages && pages==h50_fail_pages) { h50_fail_pages=0; return 0; }
#endif
    for (unsigned first = 4096; first + pages <= HOST_PAGES; first++) {
        unsigned n = 0;
        while (n < pages && !allocated[first + n]) n++;
        if (n != pages) { first += n; continue; }
        for (n = 0; n < pages; n++) { allocated[first + n] = true; used_pages++; }
        return first * PAGE_SIZE;
    }
    return 0;
}
uint32_t pmm_alloc_zone(int zone) { (void)zone; return pmm_alloc(); }
void pmm_free(uint32_t p) { allocated[p / PAGE_SIZE] = false; used_pages--; }
uint32_t pmm_free_count(void) { return HOST_PAGES - 4096 - used_pages; }
uint32_t vmm_kernel_pd(void) { return kpd_phys; }
void panic(const char *fmt, ...) { fprintf(stderr, "panic: %s\n", fmt); abort(); }
/* f1-86: the periodic VM exit profile logs through klog_periodic; ignored here like klog. */
void klog_periodic(const char *fmt, ...) { (void)fmt; }
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
/* dosvm.c revokes a forwarded input boost at DOS focus transfer; no
 * native input forwarding exists in this runner. */
void task_input_withdraw(struct task *t) { (void)t; }
void schedule(void) { g_need_resched = false; }
void kpreempt_point(void) { if ((cpu_flags & V86_IF) && g_need_resched) schedule(); }
void crit_begin(void) { }
void crit_end(void) { }
void task_sleep_ms(uint32_t ms)
{
    g_ticks += ms;
    host_clock_tick();
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
void kwait_wake_ready(struct kwait *q) { (void)q; }
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
static FILE *disk_image, *cow, *sidecar;
static char cow_path[96];
static const char *disk_path;
static uint64_t pos[3];
struct storage *storage_get(void) { return &store; }
#ifdef CIUKI_DOSVM_FILE_CPU
#define vfs_table_init transport_vfs_table_init
#define vfs_table_destroy transport_vfs_table_destroy
#define vfs_stat transport_vfs_stat
#define vfs_open transport_vfs_open
#define vfs_close transport_vfs_close
#define vfs_delete transport_vfs_delete
#define vfs_seek transport_vfs_seek
#define vfs_read transport_vfs_read
#define vfs_write transport_vfs_write
#define vfs_commit transport_vfs_commit
#define vfs_handle_version transport_vfs_handle_version
#define vfs_handle_stat transport_vfs_handle_stat
#define vfs_version_pin(...) transport_vfs_version_pin(__VA_ARGS__)
#define vfs_version_unpin transport_vfs_version_unpin
#endif
int vfs_table_init(struct vfs *vfs, struct vfs_table *table, unsigned drive) { (void)vfs; (void)table; (void)drive; return 0; }
void vfs_table_destroy(struct vfs_table *table) { (void)table; }
int vfs_handle_stat(struct vfs_table *table,int h,struct fat_entry *entry,uint64_t *pos)
{ (void)table;(void)h;(void)entry;(void)pos;return -FS_EOPNOTSUPP; }
/* The image's real size: a cut base (the bundled games disk, owner decision
 * 2026-10-12) must be read through dosvm_disk.c's zero fill, never past its end. */
static uint64_t disk_bytes;
int vfs_stat(struct vfs_table *table, const char *path, struct fat_entry *entry)
{ (void)table; (void)path; memset(entry, 0, sizeof(*entry)); entry->size = (uint32_t)disk_bytes; return 0; }
int vfs_open(struct vfs_table *table, const char *path, unsigned access, enum vfs_share share, uint8_t attr)
{
    (void)table; (void)access; (void)share; (void)attr;
    if (strstr(path, ".IMG")) return 0; /* the explicit host fixture, never a runtime selector */
    size_t length = strlen(path);
    if (length > 4 && !strcmp(path + length - 4, ".SHA")) {
        /* f3-61: kernel=freedos-1.3/unified sidecar, next to the host image. */
        char host[512]; size_t n = strlen(disk_path);
        CHECK(n > 4 && n < sizeof(host) && !strcmp(disk_path + n - 4, ".IMG"));
        memcpy(host, disk_path, n - 4); memcpy(host + n - 4, ".SHA", 5);
        CHECK(!sidecar); sidecar = fopen(host, "rb"); CHECK(sidecar);
        pos[2] = 0;
        return 2;
    }
    snprintf(cow_path, sizeof(cow_path), "build/host/dosvm-guest-XXXXXX");
    int fd = mkstemp(cow_path); CHECK(fd >= 0); cow = fdopen(fd, "w+b"); CHECK(cow);
    return 1;
}
int vfs_close(struct vfs_table *table, int handle)
{ (void)table; if (handle == 2 && sidecar) { fclose(sidecar); sidecar = 0; } return 0; }
int vfs_delete(struct vfs_table *table, const char *path)
{ (void)table; (void)path; if (cow) { fclose(cow); unlink(cow_path); cow = 0; } return 0; }
int vfs_seek(struct vfs_table *table, int handle, int64_t offset, enum vfs_whence whence, uint64_t *position)
{ (void)table; (void)whence; pos[handle] = (uint64_t)offset; *position = (uint64_t)offset; return 0; }
int vfs_read(struct vfs_table *table, int handle, void *data, size_t bytes, size_t *done)
{
    (void)table;
    if (handle == 2) { *done = fread(data, 1, bytes, sidecar); pos[2] += *done; return 0; }
    FILE *f = handle ? cow : disk_image;
    CHECK(handle || pos[0] + bytes <= disk_bytes);
    CHECK(fseek(f, (long)pos[handle], SEEK_SET) == 0);
    size_t n = fread(data, 1, bytes, f);
    if (n < bytes) memset((uint8_t *)data + n, 0, bytes - n);
    *done = bytes;
    pos[handle] += bytes;   /* sequential readers (base hash) seek once */
    return 0;
}
int vfs_write(struct vfs_table *table, int handle, const void *data, size_t bytes, size_t *done)
{ (void)table; CHECK(handle == 1 && fseek(cow, (long)pos[1], SEEK_SET) == 0); *done = fwrite(data, 1, bytes, cow); pos[1] += *done; return 0; }
int vfs_commit(struct vfs_table *table, int handle) { (void)table; (void)handle; return 0; }
/* f3-51c: no content version; a game/kernel base would be hashed each launch. */
int vfs_handle_version(struct vfs_table *table, int handle, struct vfs_file_version *out)
{ (void)table; (void)handle; (void)out; return -FS_EOPNOTSUPP; }

int vfs_version_pin(struct vfs_table *t,int h,const struct vfs_file_version *v,struct vfs_version_pin *pin)
{ (void)t; (void)h; (void)v; *pin=(struct vfs_version_pin){0}; return -FS_EOPNOTSUPP; }
void vfs_version_unpin(struct vfs_version_pin *pin) { *pin=(struct vfs_version_pin){0}; }

static struct dosvm *gv;
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
#ifdef CIUKI_DOSVM_FILE_CPU
        /* Transport adapter for the unchanged, hash-pinned f3-48 COM format. */
        if (gv && !strncmp(gv->verdict_name,"D48",3) && !strncmp(text,gv->verdict_name,6) && text[6]==' ') {
            bool pass=!strcmp(text+7,"PASS 0000");
            bool fail=!strncmp(text+7,"FAIL ",5) && strlen(text+7)==9;
            if (pass || fail) { gv->info.verdict_done=true; gv->info.verdict_pass=pass;
                if (fail) memcpy(gv->info.verdict_detail,text+12,5); }
        }
#endif
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
#include "dosvm_work.inc"
/* This runner opens explicit fixture images, without a managed games volume. */
int dosvm_game_program(unsigned game, bool remove, uint32_t *size)
{
    (void)game; (void)remove; (void)size;
    return -ENOSYS;
}
#include "../../src/kernel/vm/dosvm.c"
/* This CPU/storage harness admits no LAN port or network-enabled launch.
 * The real network adapters run in tests/host/vm/lifecycle_test.c instead. */
void dosvm_network_service(struct dosvm *v, uint64_t now) { (void)v; (void)now; }
int dosvm_network_dispatch(struct dosvm *v, struct v86_frame *f) { (void)v; (void)f; return 0; }
int dosvm_network_interrupt(struct dosvm *v, struct v86_frame *f, uint8_t vector)
{ (void)v; (void)f; (void)vector; return -ENOSYS; }
int dosvm_network_detach(struct dosvm *v) { (void)v; return 0; }
void dosvm_network_process_exit(struct dosvm *v, uint16_t psp) { (void)v; (void)psp; }
#ifndef CIUKI_DOSVM_FILE_CPU
#include "dosvm_request_context.inc"
void fs_service(void) { }
#endif
/* Explicit guest fixtures have no native persistent title store. */
int dos_save_store_open(struct dos_save_file *f,struct dos_save *s,const struct dos_save_identity *id,
                        char path[128],unsigned *archived)
{
    (void)s; *archived=0; f->fd=-1; f->absent=true;
    snprintf(path,128,DOS_SAVE_ROOT "\\G%uK%u%s.COW",id->game,id->kernel,id->base);
    return -FS_EROFS;
}
#include "../../src/kernel/vm/dos_save.c"
#include "../../src/kernel/vm/dosvm_disk.c"
#ifdef CIUKI_DOSVM_FILE_CPU
#undef vfs_table_init
#undef vfs_table_destroy
#undef vfs_stat
#undef vfs_open
#undef vfs_close
#undef vfs_delete
#undef vfs_seek
#undef vfs_read
#undef vfs_write
#undef vfs_commit
#undef vfs_handle_version
#undef vfs_handle_stat
#undef vfs_version_pin
#undef vfs_version_unpin
#endif

/* GUEST_INT10_TRACE=1: every INT 10h the VM's BIOS serves (the end of the
 * guest's own INT 10h chain, V86 or reflected from a DPMI client) as one
 * line with its input and output registers; teletype/cursor/scroll calls
 * are left out, VBE 4F00h/4F01h also print the start of the returned
 * block. Observation only (the war2vbe VESA trace, 2026-10-25). */
static int guest_int10_traced(struct dosvm *v, struct v86_frame *f);
#define dosvm_vga_bios guest_int10_traced
#include "../../src/kernel/vm/dosvm_bios.c"
#undef dosvm_vga_bios
#include "../../src/kernel/vm/dosvm_floppy.c"
#include "../../src/kernel/vm/floppy_model.c"
#include "../../src/kernel/drivers/floppy_media.c"
#include "../../src/kernel/vm/dosvm_pit.c"
#include "../../src/kernel/vm/dosvm_kbd.c"
#include "../../src/kernel/vm/dosvm_mouse.c"
#include "dosvm_uart_seams.inc"
#include "../../src/kernel/vm/uart16550.c"
#include "../../src/kernel/vm/dosvm_uart.c"
#include "../../src/kernel/vm/dosvm_xms.c"
#include "../../src/kernel/vm/dosvm_pool.c"
#include "../../src/kernel/vm/dosvm_umb.c"
#include "../../src/kernel/vm/dosvm_ems.c"
/* f3-02 VGA adapter and f3-05 audio devices, created with every VM since
 * the F3 merge; the VGA models and presenter link as separate units, as in
 * the kernel (scripts/test/host_kernel_tests.sh). */
#include "dosvm_font.inc"
#include "../../src/kernel/vm/dosvm_vga.c"
#include "../../src/kernel/vm/dosvm_vga_vbe.c"
static int guest_int10_traced(struct dosvm *v, struct v86_frame *f)
{
    static int on = -1;
    static unsigned long count;
    if (on < 0) on = getenv("GUEST_INT10_TRACE") != 0;
    unsigned ax = (uint16_t)f->tf.eax, bx = (uint16_t)f->tf.ebx, cx = (uint16_t)f->tf.ecx, dx = (uint16_t)f->tf.edx;
    unsigned es = (uint16_t)f->es, di = (uint16_t)f->tf.edi, ah = ax >> 8;
    int e = dosvm_vga_bios(v, f);
    if (!on || ah == 2 || ah == 3 || ah == 6 || ah == 7 || ah == 8 || ah == 9 || ah == 0x0A || ah == 0x0E || ah == 0x13)
        return e;
    printf("I10 n=%lu ax=%04x bx=%04x cx=%04x dx=%04x es:di=%04x:%04x -> e=%d ax=%04x bx=%04x cx=%04x dx=%04x vbe=%x ms=%llu\n",
           ++count, ax, bx, cx, dx, es, di, e, (uint16_t)f->tf.eax, (uint16_t)f->tf.ebx, (uint16_t)f->tf.ecx,
           (uint16_t)f->tf.edx, v->vbe_mode, (unsigned long long)g_ticks);
    if (!e && (ax == 0x4F00 || ax == 0x4F01) && (uint16_t)f->tf.eax == 0x004F) {
        uint8_t block[64];
        if (!dosvm_read(v, ((uint32_t)es << 4) + di, block, sizeof(block))) {
            printf("I10   block");
            for (unsigned i = 0; i < sizeof(block); i++) printf(" %02x", block[i]);
            printf("\n");
        }
    }
    return e;
}
#include "../../src/kernel/vm/dosvm_dma.c"
#include "../../src/kernel/vm/dosvm_opl.c"
#include "../../src/kernel/vm/dosvm_sb16.c"
#include "../../src/kernel/vm/dpmi_desc.c"
#include "../../src/kernel/vm/dpmi_switch.c"
#include "../../src/kernel/vm/dpmi_dispatch.c"
#include "../../src/kernel/vm/dpmi.c"
#ifdef CIUKI_DOSVM_FILE_CPU
#include "../../src/kernel/vm/dos_file_vm.c"
#endif
static unsigned guest_kernel;   /* f3-61: 0 CiukiDOS, 1 freedos-1.3, 2 unified */
#ifdef CIUKI_DOSVM_FILE_CPU
static void unified_report(const char *when)
{
    const struct unified_runtime *u=(const struct unified_runtime *)gv->unified;
    if (!u) { printf("H61 IFS %s kernel=%u bound=0 active=0\n",when,guest_kernel); return; }
    const struct dfv2_ifs_stats *t=&u->ifs.stats;
    printf("H61 IFS %s kernel=%u bound=1 active=%u owned=%x requests=%u owned_requests=%u chained=%u failed=%u critical=%u "
           "retries=%u expired=%u forged=%u opens=%u closes=%u reads=%u writes=%u finds=%u read_bytes=%llu written_bytes=%llu "
           "held=%u refusals=%u why=%s\n",when,guest_kernel,u->ifs.active,u->ifs.owned,t->requests,t->owned,t->chained,t->failed,
           t->critical,t->retries,t->expired,t->forged,t->opens,t->closes,t->reads,t->writes,t->finds,
           (unsigned long long)t->read_bytes,(unsigned long long)t->written_bytes,dfv2_ifs_held(&u->ifs),t->refusals,
           u->ifs.why ? u->ifs.why : "-");
    /* f3-61c: the first failed requests, as the vm-game probe records them. */
    for (unsigned i=0;i<t->failed && i<DFV2_IFS_FAILURES;i++)
        printf("H61 IFS %s failed n=%u fn=%04x error=%u name=%s\n",when,i+1,u->ifs.failure[i].fn,u->ifs.failure[i].error,
               u->ifs.failure[i].name[0] ? u->ifs.failure[i].name : "-");
}
#else
static void unified_report(const char *when) { (void)when; }
#endif

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
#ifdef CIUKI_DOSVM_FILE_CPU
    /* A probe's 1680h yield is not the command interpreter waiting for input. */
    if (cooperative && !strncmp(gv->verdict_name,"D48",3) && !gv->info.verdict_done) return;
#endif
    if (cooperative || (gv->frame.tf.cs == 0xF000 && ip == 0x16 * 16 + 2)) {
        gv->info.state = DOSVM_RUNNABLE;
        gv->idle.sleeping = false; gv->idle.last_tick = g_ticks;
        waiting_input = true;
        longjmp(idle_return, 1);
    }
}
static struct v86_frame cpu;
static uint64_t instructions, traps;
static struct v86_frame last_cli;
static uint16_t last_cli_seed_cs;
static uint32_t last_cli_seed_ip;
static unsigned last_cli_depth;
/* Optional startup cost replay, usable with both pre-f3-14 and current VM
 * sources. Classify the trapped bytes, never execute or patch guest code. */
static uint64_t replay_cost[14], replay_ns[14];
/* fx-16 GUEST_T23=1 (with GUEST_PERF=1): the T23 time model of trial
 * e831e685. Every monitor exit costs wall time: this host's measured
 * monitor time of that exit (production trap_cycles) times the ratio of the
 * T23's average for its cause (BOOT.LOG vm_perf_cost, banks n=7 and n=8,
 * DOOM full screen) to this host's HEAD average for the same cause in the
 * same replay window (fx-16 report, calibration table). An exit made
 * cheaper on the host is cheaper on the model T23 by the same factor. Monitor
 * time and the trial's off-CPU time (vm-clock 40 us each ms, refill/device
 * work 1.2 ms each 10 ms, a 7 ms desktop stretch each 100 ms: about 23 %,
 * the trial measured 30 % off-CPU) consume the instruction clock; PIT ticks
 * inside them are sampled as monitor time (VM current, no trap) or as
 * descheduled time (another task current). Not hardware evidence. */
static bool t23_model;
/* GUEST_T23_FULLSCREEN=1: the full-screen DOS VM owns the physical VGA
 * ports and aperture (arch_vga_io, map_aperture): its VGA cycles are not
 * monitor exits on the T23; the runner still emulates them, charged 1 us. */
static bool t23_fullscreen;
static uint64_t t23_monitor, t23_off;       /* owed time, instruction units */
static uint64_t t23_charged_ns[DOSVM_COST_COUNT], t23_exits[DOSVM_COST_COUNT];
static double t23_ratio(unsigned cause)
{
    /* T23 us/exit (n=7+8) / host HEAD us/exit (shooting window). */
    switch (cause) {
    case DOSVM_COST_IO: return 21.6 / 0.79;
    case DOSVM_COST_PIT_PIC: return 19.2 / 0.79;
    case DOSVM_COST_PM_FLAGS: return 56.6 / 3.04;
    case DOSVM_COST_DB: return 37.4 / 0.83;
    case DOSVM_COST_TIMER: return 60.7 / 4.84;
    case DOSVM_COST_VGA_IO: return 19.7 / 1.86;
    default: return 21.6 / 0.79;           /* the I/O exit ratio */
    }
}
/* fx-27 GUEST_T23_MODEL=fx27, a two-point model. The fx-16 ratio scales
 * the host's whole measured exit time; on this host most of that time is
 * the monitor's own clock_gettime calls (about 0.43 us each, three per VGA
 * exit) and the rest varies with this host's cache state, which the lean
 * path itself changes (fewer exits keep the full path's code warm here).
 * So: an ordinary exit of a fixed-length path (port I/O, CLI/STI, #DB,
 * timer, V86 flag/IRET) costs exactly the T23's measured average for its
 * cause, whatever the host saw; an exit of variable work (DOS/BIOS/DPMI
 * services, mode switches, aperture faults) keeps the fx-16 scaling. A lean
 * exit costs the T23 average of its cause scaled by its host code time
 * (clock calls removed) over the HEAD code time of that cause, plus the
 * T23 cost of its own clock reads. Agner Fog's instruction tables give
 * CPUID about 130 and RDTSC about 42 clocks (Pentium M, the T23's P6
 * lineage): read_tsc_serialized (CPUID, RDTSC, CPUID) is about 300 cycles
 * = 265 ns at 1.13 GHz; cpu_path_clock (RDTSC plus the irq_save/restore
 * section stamps) about 150 ns. Host-specific HEAD code times (this
 * workstation, HEAD 6b0a5481 sources, DOOM shooting replay, t23x_calibration
 * lines): vga_io 589 ns, pit_pic 375 ns, io 349 ns. */
static bool t23_fx27;
static double host_clock_ns;
#define T23_SERIALIZED_NS 265.0
#define T23_PATH_NS 150.0
static uint64_t t23x_code_ns[DOSVM_COST_COUNT], t23x_ser[DOSVM_COST_COUNT], t23x_path[DOSVM_COST_COUNT];
/* fx-27: the same split for exits retired on the lean path. */
static uint64_t t23x_lean_ns[DOSVM_COST_COUNT], t23x_lean_exits[DOSVM_COST_COUNT], t23x_lean_charged[DOSVM_COST_COUNT];
static bool t23x_lean_exit;
/* T23 us per exit (trial e831e685 n=7/8 as fx-16; V86 flag/IRET exits and
 * the keyboard from trial f441c5bd n=3); 0: variable work, fx-16 scaling. */
static double t23x_average(unsigned cause)
{
    switch (cause) {
    case DOSVM_COST_IO: return 21.6;
    case DOSVM_COST_PIT_PIC: return 19.2;
    case DOSVM_COST_PM_FLAGS: return 56.6;
    case DOSVM_COST_DB: return 37.4;
    case DOSVM_COST_TIMER: return 60.7;
    case DOSVM_COST_VGA_IO: return 19.7;
    case DOSVM_COST_KBD: return 40.0;
    case DOSVM_COST_RM_IRET: return 12.7;
    case DOSVM_COST_RM_PUSHF: return 13.5;
    case DOSVM_COST_RM_POPF: return 13.9;
    case DOSVM_COST_RM_CLI: return 11.9;
    case DOSVM_COST_RM_STI: return 12.2;
    default: return 0;
    }
}
static double t23x_head_code_ns(unsigned cause)
{
    return cause == DOSVM_COST_VGA_IO ? 589.1 : cause == DOSVM_COST_PIT_PIC ? 375.1 : 348.7;
}
static void t23_charge_calls(uint64_t serialized, uint64_t path)
{
    if (!t23_model || !g_tsc_per_ms) return;
    unsigned cause = gv->perf.cause < DOSVM_COST_COUNT ? gv->perf.cause : DOSVM_COST_OTHER;
    double trap = (double)gv->perf.trap_cycles * 1000000.0 / (double)g_tsc_per_ms;
    /* Boundary reads: the entry and exit stamps (half of each inside the
     * interval) and the return to guest time after it. */
    uint64_t inner = serialized >= 3 ? serialized - 3 : 0;
    double code = trap - (double)(inner + 1 + path) * host_clock_ns;
    if (code < 0) code = 0;
    t23x_code_ns[cause] += (uint64_t)code; t23x_ser[cause] += inner + 1; t23x_path[cause] += path;
    double clocks = (double)(inner + 1) * T23_SERIALIZED_NS + (double)path * T23_PATH_NS;
    double average = t23x_average(cause) * 1000.0, ns;
    if (t23x_lean_exit) {
        /* HEAD's ordinary exit of the same cause: one serialized read, and
         * two path reads for a VGA register write. */
        double head_clocks = T23_SERIALIZED_NS + (cause == DOSVM_COST_VGA_IO ? 2 * T23_PATH_NS : 0);
        double base = average ? average : 21600.0;
        ns = (base - head_clocks) * code / t23x_head_code_ns(cause) + clocks;
        t23x_lean_ns[cause] += (uint64_t)code; t23x_lean_exits[cause]++; t23x_lean_charged[cause] += (uint64_t)ns;
    } else ns = average ? average : trap * t23_ratio(cause);
    if (!t23_fx27) return;
    if (t23_fullscreen && (cause == DOSVM_COST_VGA_IO || cause == DOSVM_COST_VGA_PF)) ns = 1000.0;
    t23_charged_ns[cause] += (uint64_t)ns; t23_exits[cause]++;
    t23_monitor += (uint64_t)(ns * guest_slice / 1000000.0);
}
static void t23_charge(void)
{
    if (!t23_model || !g_tsc_per_ms || t23_fx27) return;
    unsigned cause = gv->perf.cause < DOSVM_COST_COUNT ? gv->perf.cause : DOSVM_COST_OTHER;
    double ns = (double)gv->perf.trap_cycles * 1000000.0 / (double)g_tsc_per_ms * t23_ratio(cause);
    if (t23_fullscreen && (cause == DOSVM_COST_VGA_IO || cause == DOSVM_COST_VGA_PF)) ns = 1000.0;
    t23_charged_ns[cause] += (uint64_t)ns; t23_exits[cause]++;
    t23_monitor += (uint64_t)(ns * guest_slice / 1000000.0);
}
static uint64_t replay_clock(void)
{
    struct timespec t;
    CHECK(!clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t));
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
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
/* f3-61d: #DB exits by monitor owner (dosvm_step_owner), for the TCG cost
 * analysis of a windowed game (one #DB exit is a full monitor round trip). */
static struct { const char *owner; uint64_t count; } db_owners[16];
static void db_owner_count(const char *owner)
{
    for (unsigned i = 0; i < 16; i++) {
        if (!db_owners[i].owner) db_owners[i].owner = owner;
        if (!strcmp(db_owners[i].owner, owner)) { db_owners[i].count++; return; }
    }
}
static uint64_t db_return_tf, db_return_breakpoint;
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
/* f3-61 GUEST_INT21_TRACE=1: every INT 21h the guest's DOS kernel serves,
 * as one entry/return line. V86 INT 21h is matched to the kernel's IRET
 * (trapped, IOPL-sensitive) by return address and stack; DPMI 0300h and
 * reflected PM INT 21h are matched at the host's real-mode return stub,
 * whose top record still holds the caller's input registers. Observation
 * only: the guest and the production monitor see the same traps. */
static int i21_on = -1;
static unsigned long i21_count;
struct i21_entry { uint16_t cs, ip, ss, sp, ax, bx, cx, dx, si, ds; char path[48]; };
static struct i21_entry i21_stack[48];
static unsigned i21_depth;
static void i21_string(uint32_t linear, char *out, size_t n)
{
    size_t k = 0;
    for (; k + 1 < n; k++) {
        uint8_t c = 0;
        if (dosvm_read(gv, linear + (uint32_t)k, &c, 1) || !c) break;
        out[k] = c >= 32 && c < 127 && c != '"' ? (char)c : '?';
    }
    out[k] = 0;
}
static void i21_path(struct i21_entry *e)
{
    unsigned ah = e->ax >> 8, al = e->ax & 255;
    e->path[0] = 0;
    bool dx = ah == 0x39 || ah == 0x3A || ah == 0x3B || ah == 0x3C || ah == 0x3D || ah == 0x41 || ah == 0x43 ||
              ah == 0x4B || ah == 0x4E || ah == 0x56 || ah == 0x5A || ah == 0x5B || (ah == 0x71 && al != 0xA1);
    if (dx) i21_string(((uint32_t)e->ds << 4) + e->dx, e->path, sizeof(e->path));
    else if (ah == 0x6C) i21_string(((uint32_t)e->ds << 4) + e->si, e->path, sizeof(e->path));
}
static void i21_line(const char *src, const struct i21_entry *e, uint32_t ax, uint32_t bx, uint32_t cx, uint32_t dx, uint32_t flags)
{
    printf("I21 n=%lu src=%s ax=%04x bx=%04x cx=%04x dx=%04x%s%s%s -> ax=%04x bx=%04x cx=%04x dx=%04x cf=%u ms=%llu\n",
           ++i21_count, src, e->ax, e->bx, e->cx, e->dx, e->path[0] ? " path=\"" : "", e->path, e->path[0] ? "\"" : "",
           ax & 0xFFFF, bx & 0xFFFF, cx & 0xFFFF, dx & 0xFFFF, flags & 1, (unsigned long long)g_ticks);
}
static void i21_observe(const struct gcpu_event *ev)
{
    if (i21_on < 0) i21_on = getenv("GUEST_INT21_TRACE") != 0;
    if (!i21_on || ev->vector != 13 || frame_pm()) return;
    uint16_t cs = (uint16_t)cpu.tf.cs, ip = (uint16_t)cpu.tf.eip, ss = (uint16_t)cpu.tf.user_ss, sp = (uint16_t)cpu.tf.user_esp;
    uint8_t op[2] = {0};
    if (dosvm_read(gv, ((uint32_t)cs << 4) + ip, op, 2)) return;
    if (op[0] == 0xCD && op[1] == 0x21) {
        if (i21_depth == sizeof(i21_stack) / sizeof(i21_stack[0])) {
            memmove(i21_stack, i21_stack + 1, sizeof(i21_stack) - sizeof(i21_stack[0])); i21_depth--;
        }
        struct i21_entry *e = &i21_stack[i21_depth++];
        *e = (struct i21_entry){cs, (uint16_t)(ip + 2), ss, sp, (uint16_t)cpu.tf.eax, (uint16_t)cpu.tf.ebx,
                                (uint16_t)cpu.tf.ecx, (uint16_t)cpu.tf.edx, (uint16_t)cpu.tf.esi, (uint16_t)cpu.ds, ""};
        i21_path(e);
        return;
    }
    if (op[0] == 0xCF) {
        uint16_t frame[3];
        if (dosvm_read(gv, ((uint32_t)ss << 4) + sp, frame, sizeof(frame))) return;
        for (unsigned k = i21_depth; k-- > 0;) {
            const struct i21_entry *e = &i21_stack[k];
            if (e->cs == frame[1] && e->ip == frame[0] && e->ss == ss && e->sp == (uint16_t)(sp + 6)) {
                i21_line("v86", e, cpu.tf.eax, cpu.tf.ebx, cpu.tf.ecx, cpu.tf.edx, frame[2]);
                i21_depth = k;
                return;
            }
        }
        return;
    }
    if (op[0] == 0xCD && op[1] == 0xFE && cs == 0xF000 && ip == DPMI_RM_RETURN && gv->dpmi.depth) {
        const struct dpmi_record *r = &gv->dpmi.rec[gv->dpmi.depth - 1];
        if (r->kind != DPMI_REC_REALCALL && (r->kind != DPMI_REC_REFLECT || r->vector != 0x21)) return;
        struct i21_entry e = {0};
        if (r->kind == DPMI_REC_REALCALL) {
            uint32_t lin; uint8_t b[0x32]; uint16_t ivt[2];
            if (!dpmi_ptr(gv, r->sel, r->off, sizeof(b), false, &lin) || dosvm_read(gv, lin, b, sizeof(b)) ||
                dosvm_read(gv, 0x21 * 4, ivt, sizeof(ivt))) return;
            /* 0300h BL=21h, or 0301h/0302h straight into the INT 21h vector
             * (DOS/4GW calls the kernel's handler with 0302h). */
            bool vector = r->arg[0] == 0x0300 ? r->vector == 0x21 : vm_u16(b + 0x2C) == ivt[1] && vm_u16(b + 0x2A) == ivt[0];
            if (!vector) return;
            e.ax = vm_u16(b + 0x1C); e.bx = vm_u16(b + 0x10); e.cx = vm_u16(b + 0x18); e.dx = vm_u16(b + 0x14);
            e.si = vm_u16(b + 0x04); e.ds = vm_u16(b + 0x24);
            i21_path(&e);
        } else {
            e.ax = (uint16_t)r->saved.tf.eax; e.bx = (uint16_t)r->saved.tf.ebx;
            e.cx = (uint16_t)r->saved.tf.ecx; e.dx = (uint16_t)r->saved.tf.edx;
        }
        i21_line(r->kind != DPMI_REC_REALCALL ? "reflect" : r->arg[0] == 0x0300 ? "dpmi0300" : r->arg[0] == 0x0301 ? "dpmi0301" : "dpmi0302", &e, cpu.tf.eax, cpu.tf.ebx, cpu.tf.ecx, cpu.tf.edx,
                 cpu.tf.eflags);
    }
}
static unsigned slice_left = SLICE;
/* fx-27: guest cycles to A0000h-BFFFFh obey the VM's page tables as the
 * CPU does (Intel SDM Vol. 3A 4.6 access rights, 4.8 accessed/dirty
 * flags): a present user page (writable for a store) is accessed in its
 * mapped frame (a plane shadow or the chain-4 linear backing) with A, and D
 * for a store, set in the PTE; anything else is a #PF before any byte
 * moves, with P set when the PTE was present (a protection violation, as
 * the supervisor-only window produces on hardware) and U (CPL 3). Before
 * fx-27 these cycles reached the VGA model directly at no cost, so the
 * runner never produced the aperture faults a closed window costs. */
static bool aperture_fault_present;
static uint64_t aperture_direct_bytes, aperture_faults;
static uint32_t *aperture_pte(uint32_t lin)
{
    uint32_t pde = ((uint32_t *)P2V(gv->as.pd_phys))[lin >> 22];
    if (!(pde & PTE_P)) return 0;
    return &((uint32_t *)P2V(pde & ~0xFFFu))[(lin >> 12) & 1023];
}
static bool aperture_access(uint32_t lin, void *b, unsigned n, bool write)
{
    /* Validate every page of the access first: a split access faults
     * without a partial store, as on hardware. */
    for (uint32_t at = lin & ~0xFFFu; at < lin + n; at += PAGE_SIZE) {
        uint32_t *pte = aperture_pte(at);
        if (!pte || !(*pte & PTE_P) || !(*pte & PTE_U) || (write && !(*pte & PTE_W))) {
            aperture_fault_present = pte && (*pte & PTE_P);
            aperture_faults++;
            return false;
        }
    }
    uint8_t *bytes = b;
    for (unsigned i = 0; i < n; i++) {
        uint32_t *pte = aperture_pte(lin + i), at = lin + i, frame = *pte & ~0xFFFu;
        /* Model planes live in the static VM slot outside fake RAM (as in
         * dosvm_workloads_test.inc): resolve a plane frame explicitly. */
        uint8_t *cell = 0;
        if (at < 0xB0000) for (unsigned plane = 0; plane < 4 && !cell; plane++)
            if (frame == plane_frame(gv, plane, (at - 0xA0000) / PAGE_SIZE))
                cell = &gv->vga.plane[plane][at - 0xA0000];
        if (!cell) { CHECK(frame < HOST_PAGES * PAGE_SIZE); cell = (uint8_t *)P2V(frame | (at & 0xFFFu)); }
        *pte |= write ? 0x60u : 0x20u;
        if (write) *cell = bytes[i]; else bytes[i] = *cell;
    }
    aperture_direct_bytes += n;
    return true;
}
static bool aperture(uint32_t lin, unsigned n) { return lin < 0xC0000 && lin + n > 0xA0000; }
static bool g_read(uint32_t lin, void *b, unsigned n)
{ return aperture(lin, n) ? aperture_access(lin, b, n, false) : !dosvm_read(gv, lin, b, n); }
static bool g_write(uint32_t lin, const void *b, unsigned n)
{ return aperture(lin, n) ? aperture_access(lin, (void *)b, n, true) : !dosvm_write(gv, lin, b, n); }
static void g_descriptor(uint16_t sel, bool write, uint32_t d[2])
{
    (void)write;
    d[0] = d[1] = 0;
    if (sel & 4) {
        unsigned i = sel >> 3;
        if (gv->dpmi.active && i < DPMI_LDT_ENTRIES) { d[0] = gv->dpmi.ldt[i][0]; d[1] = gv->dpmi.ldt[i][1]; }
        return;
    }
    /* The GDT a DPMI worker runs with: flat user code/data, and selector
     * 40h (the VM's BIOS data area) while the VM's LDT is loaded. */
    dpmi_gdt_descriptor(gv, sel, d);
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
    memcpy(s->dr, debug_addr, sizeof(s->dr)); s->dr7 = debug_dr7;
    if (gv->return_tracker_tf) for (unsigned i = 0; i < DPMI_WATCH; i++)
        CHECK(gv->dpmi.watch[i].used || !(debug_dr7 & (3u << (i * 2))));
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
/* fx-27 GUEST_LEAN_DIAG=1: why port exits did not take the lean path. */
static uint64_t lean_why[24];
static const char *const lean_names[24] = {"lean_or_tail", "state", "fullscreen", "idle_poll", "network",
    "sb_entry", "halted", "resched", "pm_inactive", "pm_shadow", "pm_if_image", "pm_if_recovery",
    "pm_return", "pm_dr7", "pm_depth", "pm_host_cs", "pm_tf", "pm_seed", "v86_dpmi", "v86_shadow_tf",
    "v86_mouse", "not_port", 0, 0};
static void lean_diag(const struct v86_frame *hw)
{
#ifdef FX27_HEAD_BASELINE
    (void)hw; return;           /* baseline sources: no lean path */
#endif
#ifndef FX27_HEAD_BASELINE
    /* GUEST_NO_LEAN=1: the same production state that refuses lean exits
     * for an IPX VM, for A/B replays of the lean path alone. */
    if (getenv("GUEST_NO_LEAN")) gv->network_attached = true;
#endif
    if (!getenv("GUEST_LEAN_DIAG") || hw->tf.vector != 13 || hw->tf.err) return;
    bool pm = !(hw->tf.eflags & V86_VM);
    uint8_t op = 0;
    uint32_t lin = pm ? dpmi_fault_linear(gv, hw) : (hw->tf.cs << 4) + hw->tf.eip;
    if (dosvm_read(gv, lin, &op, 1)) return;
    if (op == 0x66 && dosvm_read(gv, lin + 1, &op, 1)) return;
    if (op < 0xE4 || (op > 0xE7 && op < 0xEC) || op > 0xEF) return;
    const struct dpmi *d = &gv->dpmi;
    unsigned why = 0;
    if (gv->info.state >= DOSVM_STOPPING || !gv->info.dos_ready) why = 1;
    else if (gv->fullscreen || gv->fullscreen_hold || gv->fullscreen_suspended) why = 2;
    else if (gv->idle.depth) why = 3;
#ifndef FX27_HEAD_BASELINE
    else if (gv->network_attached) why = 4;
#endif
    else if (gv->audio.sb.irq_entry_ready) why = 5;
    else if (gv->monitor.state == V86_HALTED) why = 6;
    else if (g_need_resched) why = 7;
    else if (pm) {
        if (!d->active) why = 8;
        else if (d->shadow) why = 9;
        /* why 10 (pm_if_image) retired: an armed flags image is lean (fx-27). */
        else if (d->if_waiting || d->if_step || d->if_restore_step) why = 11;
        else if (d->return_steps || d->return_step || d->return_replan) why = 12;
        else if (d->dr7) why = 13;
        else if (d->depth) why = 14;
        else if ((uint16_t)hw->tf.cs == DPMI_HOST_CS) why = 15;
        else if (hw->tf.eflags & (V86_TF | (1u << 14))) why = 16;
        else if (dpmi_seed_wanted(gv)) why = 17;
    } else if (d->active) why = 18;
    else if (gv->monitor.shadow || (hw->tf.eflags & V86_TF)) why = 19;
    else if (gv->mouse.pending_mask && gv->mouse.mask && !gv->mouse.active) why = 20;
    lean_why[why]++;
}
static void deliver(unsigned vector, uint32_t err, uint32_t cr2)
{
    struct v86_frame hw = cpu;
    hw.tf.vector = vector; hw.tf.err = err; fake_cr2 = cr2;
    lean_diag(&hw);
    unsigned cost = getenv("GUEST_COSTS") ? replay_cause(&hw) : 13;
    uint64_t begin = getenv("GUEST_COSTS") ? replay_clock() : 0;
    g_current = gv->task; gv->continuation = 0x12345678; cpu_flags = 0;
    unsigned forced = gv->perf.if_enable[DOSVM_IF_FORCED];
    if (guest_perf && vector == 13 && !err && !(hw.tf.eflags & V86_VM)) {
        uint8_t op;
        if (!dosvm_read(gv, dpmi_fault_linear(gv, &hw), &op, 1) && op == 0xFA) {
            last_cli = hw; last_cli_seed_cs = gv->dpmi.decode_cs;
            last_cli_seed_ip = gv->dpmi.decode_eip; last_cli_depth = gv->dpmi.depth;
        }
    }
    traps++;
    if (vector == 1) {
        const char *owner = dosvm_step_owner(gv, &cpu, dpmi_debug_status(gv, debug_dr6));
        db_owner_count(owner);
        if (!strcmp(owner, "rm_ss") || !strcmp(owner, "rm_irq")) db_rm++;
        else if (!strcmp(owner, "pm_flags")) db_flags++;
        else if (!strcmp(owner, "pm_return") || !strcmp(owner, "pm_checkpoint")) {
            db_return++;
            if (!strcmp(owner, "pm_checkpoint")) db_return_breakpoint++;
            else db_return_tf++;
        }
        else db_other++;
    }
    if (setjmp(idle_return)) { g_current = &caller; sti(); return; }
    /* The runner has no cpu_path_clock, so dosvm_perf_task_clock reads the
     * same host clock: its reads are the two per VGA register byte written
     * (dosvm_vga_port) and per dirty collection (video_collect). */
#ifndef FX27_HEAD_BASELINE
    uint32_t lean_before = gv->perf.lean_exits;
#endif
    uint64_t calls = host_clock_calls, harvest = gv->perf.harvest_cycles, ports = 0;
    for (unsigned i = 0; i < DOSVM_VGA_REGS; i++) ports += gv->perf.port_count[i];
    if (!setjmp(guest_return)) { CHECK(dosvm_trap(&hw.tf)); cpu = hw; }
    else cpu = gv->frame;
    t23_charge();
    uint64_t after = 0;
    for (unsigned i = 0; i < DOSVM_VGA_REGS; i++) after += gv->perf.port_count[i];
    uint64_t path = 2 * (after - ports) + (gv->perf.harvest_cycles != harvest ? 2 : 0), total = host_clock_calls - calls;
    if (path > total) path = total;
#ifndef FX27_HEAD_BASELINE
    t23x_lean_exit = gv->perf.lean_exits != lean_before;
#endif
    t23_charge_calls(total - path, path);
    if (guest_perf && gv->perf.if_enable[DOSVM_IF_FORCED] != forced)
        printf("isr_forced cli=%04x:%08x ss:sp=%04x:%08x depth=%u seed=%04x:%08x at=%04x:%08x\n",
               last_cli.tf.cs, last_cli.tf.eip, last_cli.tf.user_ss, last_cli.tf.user_esp,
               last_cli_depth, last_cli_seed_cs, last_cli_seed_ip, hw.tf.cs, hw.tf.eip);
    if (getenv("GUEST_COSTS")) {
        if ((hw.tf.eflags & V86_VM) != (cpu.tf.eflags & V86_VM)) cost = 5;
        replay_cost[cost]++;
        replay_ns[cost] += replay_clock() - begin;
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
static int perf_row(void *arg, unsigned y, const void *pixels, unsigned bytes)
{ (void)arg; (void)y; (void)pixels; (void)bytes; return 0; }
static void guest_timer_tick(void)
{
    g_ticks++;
    if (guest_perf) perf_phase(gv, 2);
    host_clock_tick();
    deliver(0x20, 0, 0);        /* the host's 1 kHz timer IRQ: a delivery point */
    if (guest_perf && !(g_ticks % 16)) {
        dosvm_task_switch(&caller);
        struct ciuki_rect damage;
        /* Time one whole sample, outside production accounting.
         * Per-yield clocks would dominate the cheap row groups. */
        uint64_t presenter_begin = replay_clock();
        dosvm_vga_present(gv, perf_row, 0, g_ticks, &damage);
        uint64_t elapsed = replay_clock() - presenter_begin;
        if (elapsed > presenter_sample_max) presenter_sample_max = elapsed;
        dosvm_task_switch(gv->task);
    }
    if (t23_model) {
        uint64_t off_us = 40 + (g_ticks % 10 == 0 ? 1200 : 0) + (g_ticks % 100 == 50 ? 7000 : 0);
        t23_off += off_us * guest_slice / 1000;
    }
}
/* fx-16: consume owed monitor and off-CPU time before the guest runs on.
 * A tick inside monitor time interrupts the VM's kernel code (sampled as
 * running, no trap); a tick inside off-CPU time interrupts another task. */
static void t23_settle(void)
{
    while (t23_model && (t23_monitor || t23_off) && !stopped()) {
        bool off = !t23_monitor;
        uint64_t *debt = off ? &t23_off : &t23_monitor;
        if (*debt < slice_left) { slice_left -= (unsigned)*debt; *debt = 0; continue; }
        *debt -= slice_left; slice_left = guest_slice;
        g_ticks++;
        g_current = off ? &caller : gv->task;
        host_clock_tick();
        g_current = gv->task;
        uint64_t off_us = 40 + (g_ticks % 10 == 0 ? 1200 : 0) + (g_ticks % 100 == 50 ? 7000 : 0);
        t23_off += off_us * guest_slice / 1000;
    }
}
/* Run until done() or the VM stops or the instruction budget runs out. */
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
        g_current = gv->task; /* guest execution/ticks belong to the VM */
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
            i21_observe(&ev);
            if (ev.vector == 14 && aperture(ev.cr2, 1) && aperture_fault_present) ev.err |= 1;
            aperture_fault_present = false;
            deliver(ev.vector, ev.err, ev.cr2);
            t23_settle();
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
            guest_timer_tick();
            t23_settle();
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
/* t23-dosperf: the T23 model's charged monitor time so far (ms). */
static double t23_total_ms(void)
{
    uint64_t total = 0;
    for (unsigned i = 0; i < DOSVM_COST_COUNT; i++) total += t23_charged_ns[i];
    return total / 1e6;
}
/* f3-61 play:COMMAND NAME: a game started the way the DOS Box types it,
 * run for GUEST_PLAY_MS simulated milliseconds (default 30000) or until
 * the shell prompt returns or the VM stops. One progress line per second
 * (video mode, page flips, PM timer IRQs, DPMI, IFS counters), then the
 * text screen: the game's exit or error message, if any. */
static void play_progress(uint64_t begin)
{
    struct dosvm_video_info video = {0};
    dosvm_video_info(dosvm_key(gv), &video);
    unsigned reads = 0, failed = 0, critical = 0;
#ifdef CIUKI_DOSVM_FILE_CPU
    const struct unified_runtime *u = (const struct unified_runtime *)gv->unified;
    if (u) { reads = u->ifs.stats.reads; failed = u->ifs.stats.failed; critical = u->ifs.stats.critical; }
#endif
    unsigned xms_kb = 0;   /* XMS and EMS the game holds (DOS Box memory, dos_memory.py) */
    for (unsigned i = 0; i < VM_MEM_HANDLES; i++) if (gv->memory.xms[i].used) xms_kb += gv->memory.xms[i].bytes / 1024;
    printf("H61 PLAY t=%llu mode=%02x vbe=%x unchained=%u flips=%u irq_pm=%u dpmi=%u clients=%u terminations=%u faults=%u "
           "sb_units=%u ifs_reads=%u ifs_failed=%u ifs_critical=%u instr=%llu xms_kb=%u ems_kb=%u\n",
           (unsigned long long)(g_ticks - begin), video.mode, gv->vbe_mode, video.unchained, video.starts,
           gv->dpmi.stats.irqs_pm, gv->dpmi.active, gv->dpmi.clients, gv->dpmi.stats.terminations, gv->dpmi.stats.faults,
           gv->audio.sb.units, reads, failed, critical,
           (unsigned long long)instructions, xms_kb, (unsigned)(gv->memory.ems_bytes / 1024));
    /* t23-dosperf GUEST_PLAY_PERF=1: cumulative exit causes, lean exits,
     * the fx-16 PM CLI proof cache (BOOT.LOG [vm-audio] cadence_clock
     * cli_hits/cli_learned/cli_missed) and the seed demand that keeps
     * port exits off the lean path, once per simulated second. */
    if (getenv("GUEST_PLAY_PERF")) {
        const struct dpmi_cli_stats *c = &gv->dpmi.cli;
        const uint32_t *n = gv->perf.cost_count;
        bool wanted = !gv->dpmi.cli_settled || g_ticks - gv->dpmi.cli_miss_tick < 1000u;
        printf("H61 PERF t=%llu traps=%llu lean=%u pm_flags=%u pm_int=%u pit_pic=%u vga_io=%u io=%u db=%u timer=%u "
               "rm=%u cli_cand=%u hits=%u proved=%u pattern=%u control=%u missed=%u unprovable=%u seeds=%u skipped=%u "
               "seed_wanted=%u irq0_lost=%llu irq0_reinjected=%llu frames=%u t23_ms=%.1f t23_pit_ms=%.1f t23_flags_ms=%.1f dac=%u armed=%u ret=%u\n",
               (unsigned long long)(g_ticks - begin), (unsigned long long)traps, gv->perf.lean_exits,
               n[DOSVM_COST_PM_FLAGS], n[DOSVM_COST_PM_INT], n[DOSVM_COST_PIT_PIC], n[DOSVM_COST_VGA_IO],
               n[DOSVM_COST_IO], n[DOSVM_COST_DB], n[DOSVM_COST_TIMER],
               n[DOSVM_COST_RM_INT] + n[DOSVM_COST_RM_IRET] + n[DOSVM_COST_RM_FLAGS] + n[DOSVM_COST_RM_PUSHF] +
               n[DOSVM_COST_RM_POPF] + n[DOSVM_COST_RM_CLI] + n[DOSVM_COST_RM_STI],
               c->candidates, c->hits, c->proved, c->pattern, c->control, c->missed, c->unprovable,
               c->seeds, c->seeds_skipped, wanted, (unsigned long long)gv->pit.lost_edges,
               (unsigned long long)gv->pit.reinjected, gv->video_frames, t23_total_ms(),
               t23_charged_ns[DOSVM_COST_PIT_PIC] / 1e6, t23_charged_ns[DOSVM_COST_PM_FLAGS] / 1e6,
               gv->perf.dac_batched, gv->dpmi.if_armed, gv->perf.return_armed);
    }
}
/* GUEST_PLAY_KEYS="01@12000,1c@20000": a window key (set-1 code, hex) is
 * pressed and released at that simulated millisecond of the play run, as
 * a player skips an intro (war2vbe: the Warcraft II demo's movies). */
static bool menu_wait(uint32_t ms);
static void play_keys(uint64_t elapsed)
{
    const char *keys = getenv("GUEST_PLAY_KEYS");
    static unsigned done;
    unsigned index = 0;
    for (const char *k = keys; k && *k; index++) {
        char *end;
        unsigned code = (unsigned)strtoul(k, &end, 16);
        CHECK(*end == '@');
        unsigned long long at = strtoull(end + 1, &end, 10);
        if (index >= done && elapsed >= at) {
            CHECK(dosvm_input_focus(dosvm_key(gv), true));
            window_key(code, 1); menu_wait(80); window_key(code, 0);
            printf("H61 PLAY KEY code=%02x t=%llu\n", code, (unsigned long long)elapsed);
            done = index + 1;
        }
        k = *end == ',' ? end + 1 : end;
    }
}
/* The window picture at the end of a play run: what the DOS window shows
 * (dosvm_frame_capture, the presenter's own rows), its non-black pixel
 * count, and with GUEST_PLAY_SHOT=<file.ppm> the 640x480 image itself. */
struct play_shot { FILE *ppm; unsigned nonblack; };
static int play_shot_row(void *arg, unsigned y, const void *pixels, unsigned bytes)
{
    struct play_shot *shot = arg;
    const uint32_t *p = pixels;
    (void)y;
    for (unsigned x = 0; x < bytes / 4; x++) {
        if (p[x] & 0xFFFFFF) shot->nonblack++;
        if (shot->ppm) { uint8_t rgb[3] = {(uint8_t)(p[x] >> 16), (uint8_t)(p[x] >> 8), (uint8_t)p[x]}; fwrite(rgb, 1, 3, shot->ppm); }
    }
    return 0;
}
static void play_frame(void)
{
    const char *path = getenv("GUEST_PLAY_SHOT");
    struct play_shot shot = {path ? fopen(path, "wb") : 0, 0};
    if (shot.ppm) fprintf(shot.ppm, "P6\n%u %u\n255\n", DOSVM_VIDEO_WIDTH, DOSVM_VIDEO_HEIGHT);
    struct dosvm_frame frame = {0};
    int e = dosvm_frame_capture(gv, g_ticks, &frame, play_shot_row, &shot);
    if (shot.ppm) fclose(shot.ppm);
    printf("H61 PLAY FRAME e=%d mode=%x %ux%u text=%u nonblack=%u\n", e, frame.mode, frame.width, frame.height,
           frame.text, shot.nonblack);
}
static int play_run(void)
{
    const char *limit = getenv("GUEST_PLAY_MS");
    uint64_t total = limit ? strtoull(limit, 0, 10) : 30000, begin = g_ticks;
    int r = 0;
    while (g_ticks - begin < total) {
        play_keys(g_ticks - begin);
        menu_until = g_ticks + 1000;
        if (menu_until > begin + total) menu_until = begin + total;
        r = run(4000000000ull, menu_elapsed);
        play_progress(begin);
        if (r == 2 && prompt()) break;      /* the game returned to the shell */
        if (r == -1 || stopped()) break;
        if (r == 0) { printf("H61 PLAY instruction budget exhausted\n"); break; }
        if (r == 2) waiting_input = false; /* a game waiting for a key: keep time running */
    }
    return r;
}
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
#include "dosvm_guest_isr.inc"
static void stats(const char *name)
{
    if (getenv("GUEST_COSTS")) {
        static const char *const causes[14] = {"rm_int", "rm_iret", "rm_flags", "pm_int", "pm_flags", "switch",
            "vga_io", "vga_pf", "pit_pic", "kbd", "io", "db", "timer", "other"};
        for (unsigned i = 0; i < 14; i++) printf("  replay_cost phase=%s cause=%s exits=%llu us=%llu avg_ns=%llu\n", name, causes[i],
            (unsigned long long)replay_cost[i], (unsigned long long)(replay_ns[i] / 1000),
            (unsigned long long)(replay_cost[i] ? replay_ns[i] / replay_cost[i] : 0));
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
    t23_model = guest_perf && getenv("GUEST_T23") != 0;
    t23_fx27 = t23_model && getenv("GUEST_T23_MODEL") && !strcmp(getenv("GUEST_T23_MODEL"), "fx27");
    if (guest_perf) {
        /* Host cost of one monitor clock read (a CLOCK_THREAD_CPUTIME_ID
         * system call); the median of five 20000-call batches. */
        double batch[5];
        for (unsigned b = 0; b < 5; b++) {
            struct timespec t0, t1, t;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            for (unsigned i = 0; i < 20000; i++) clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            batch[b] = ((double)(t1.tv_sec - t0.tv_sec) * 1e9 + (double)(t1.tv_nsec - t0.tv_nsec)) / 20000.0;
        }
        for (unsigned i = 0; i < 5; i++) for (unsigned j = i + 1; j < 5; j++)
            if (batch[j] < batch[i]) { double x = batch[i]; batch[i] = batch[j]; batch[j] = x; }
        host_clock_ns = batch[2];
        printf("host clock: %.1f ns per monitor clock read (thread CPU clock system call)\n", host_clock_ns);
    }
    t23_fullscreen = t23_model && getenv("GUEST_T23_FULLSCREEN") != 0;
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
    disk_path = argv[1];
    disk_image = fopen(argv[1], "rb"); CHECK(disk_image);
    if (getenv("GUEST_VERBOSE")) g_boot.flags |= CBI_F_VERBOSE;
    const char *tracker = getenv("GUEST_TRACKER");
    if (getenv("GUEST_TCG") || (tracker && !strcmp(tracker, "tf"))) g_boot.flags |= CBI_F_SMBIOS_QEMU;
    if (tracker) {
        CHECK(!strcmp(tracker, "tf") || !strcmp(tracker, "breakpoint"));
        if (!strcmp(tracker, "breakpoint")) g_boot.flags &= ~CBI_F_SMBIOS_QEMU;
    }
    CHECK(fseek(disk_image, 0, SEEK_END) == 0); disk_bytes = (uint64_t)ftell(disk_image);
#ifdef CIUKI_DOSVM_FILE_CPU
    CHECK(!cache_init(&store.cache,65536)); vfs_init(&store.vfs);
#endif
    gcpu_init(&guest_ops);
    struct dosvm_config config = {.commitment = 0, .probe = "dpmi", .dpmi = true, .base="HOST-FIXTURE.IMG"};
#ifdef CIUKI_DOSVM_FILE_CPU
    const char *bridge_policy=getenv("GUEST_NO_FILE_HOOK");
    config.disable_file_bridge=bridge_policy && !strcmp(bridge_policy,"disabled");
    /* f3-61: the per-game DOS kernel exactly as /Programs/dosbox creates it
     * (dosvm_client.c): the pinned base, its SHA sidecar and the v1 bridge
     * disabled for every kernel other than CiukiDOS. */
    const char *kernel_choice=getenv("GUEST_KERNEL");
    if (kernel_choice) {
        CHECK(!strcmp(kernel_choice,"freedos-1.3") || !strcmp(kernel_choice,"unified"));
        guest_kernel=!strcmp(kernel_choice,"unified") ? 2 : 1;
        /* The pristine reference exists only inside this host transport. */
        config.kernel=guest_kernel==2 ? 2 : 0; config.disable_file_bridge=true;
        config.base=guest_kernel==2 ? "C:\\SYSTEM\\DOS\\UNIFIED.IMG" : "HOST-REFERENCE.IMG";
        config.probe="vm-window";
    }
#endif
    CHECK(dosvm_create(&config, &gv) == 0);
    printf("return tracker: %s (production VM creation)\n", gv->return_tracker_tf ? "tf" : "breakpoint");
    /* GUEST_NO_X87=1: a machine without a coprocessor. The BIOS equipment
     * word (0040:0010 bit 1) reports none and FNINIT/FNSTCW/FNSTSW store
     * nothing (dosvm_guest_cpu.c), so a Borland C program selects its own
     * INT 34h-3Dh x87 emulator and Open Watcom's start-up finds no 8087;
     * libx86emu has no x87 (Wolfenstein 3-D, tests/host/dos/dos_memory.py;
     * DOS/4GW clients, tests/host/dos/dos_extenders.py). */
    if (getenv("GUEST_NO_X87")) {
        uint16_t equipment = 0;
        CHECK(!dosvm_read(gv, 0x410, &equipment, 2));
        equipment &= (uint16_t)~2u;
        CHECK(!dosvm_write(gv, 0x410, &equipment, 2));
    }
#ifdef CIUKI_DOSVM_FILE_CPU
    const char *disabled=getenv("GUEST_NO_FILE_HOOK");
    if (disabled && !strcmp(disabled,"alloc")) h50_fail_pages=(sizeof(struct file_runtime)+PAGE_SIZE-1)/PAGE_SIZE;
    else if (disabled && strcmp(disabled,"disabled")) {
        uint8_t *descriptor=(uint8_t *)P2V(gv->frames[0xf1])+(DOS_HOOK_DESCRIPTOR&0xfff);
        if (!strcmp(disabled,"swap")) {
            uint32_t gate=vm_u32(descriptor+12); vm_w32(descriptor+12,vm_u32(descriptor+16)); vm_w32(descriptor+16,gate);
        } else if (!strcmp(disabled,"overlap")) vm_w32(descriptor+16,vm_u32(descriptor+12));
        else if (!strcmp(disabled,"occupied")) vm_w16(descriptor+12,DPMI_RM_RETURN);
        else if (!strcmp(disabled,"outside")) vm_w16(descriptor+16,0xfffe);
        else if (!strcmp(disabled,"old")) vm_w16(descriptor+4,0);
        else if (!strcmp(disabled,"short")) vm_w16(descriptor+6,19);
        else descriptor[0]='?';
    }
#endif
#ifdef CIUKI_DOSVM_FILE_CPU
    /* f3-61 replay of images before the fix: the DFV2 gates go unserved, so
     * CIUKIDF.SYS finds no host and the fork runs unbound on its local FAT
     * (the b453bc92 T23 state). The ROM and base stay those of kernel=unified. */
    if (guest_kernel==2 && getenv("GUEST_UNIFIED_UNBOUND")) gv->kernel=0;
#endif
    CHECK(dosvm_boot(gv) == 0);
    cpu = gv->frame;
    int failures = 0;
    sleep_hook = input_hook;
    int r = run(400000000ull, booted);
    printf("boot: %s after %llu instructions, %llu traps, %llu ms\n", r > 0 ? "prompt" : "FAILED",
           (unsigned long long)instructions, (unsigned long long)traps, (unsigned long long)g_ticks);
    if (r <= 0) { screen(); return 1; }
    if (guest_kernel) {
        unified_report("boot"); screen();
        printf("H61 SHELL exec_pending=%u overflow=%u shell_idle=%u\n",gv->exec_pending,gv->exec_overflow,dosvm_shell_idle(gv));
        for (unsigned k=0;k<gv->exec_pending;k++)
            printf("H61 SHELL frame=%u %04x:%04x ss:sp=%04x:%04x\n",k,gv->exec_frame[k].cs,gv->exec_frame[k].ip,gv->exec_frame[k].ss,gv->exec_frame[k].sp);
    }
#ifdef CIUKI_DOSVM_FILE_CPU
    if (getenv("GUEST_FILE_TRACE") && gv->files) gv->files->trace_enabled=true;
#endif
    if (getenv("GUEST_COSTS")) stats("boot");
    for (int i = 2; i + 1 < argc; i += 2) {
#ifdef CIUKI_DOSVM_FILE_CPU
        if (!strncmp(argv[i],"shell:",6)) {
            printf("H50 SHELL %s\n",argv[i]+6);
            type(argv[i]+6);
            CHECK(run(30000000ull,0)==2 && prompt());
            if (gv->files && argv[i+1][0]=='?') {
                struct fat_entry entry;
                int found=vfs_stat(gv->files->view[2].table,argv[i+1]+2,&entry);
                CHECK(argv[i+1][1]=='+' ? !found : found==-FS_ENOENT);
            }
            continue;
        }
#endif
        bool fault = !strncmp(argv[i], "fault:", 6), gate = !strncmp(argv[i], "gate:", 5);
        /* f3-06 rc: the payload reports a child's exit code (RUNREC): any
         * verdict counts; the shell then gets its prompt back, and the screen
         * latched at the verdict must still hold GUEST_EXPECT_TEXT. */
        bool rc = !strncmp(argv[i], "rc:", 3);
        bool video = !strncmp(argv[i], "video:", 6);
        bool menu = !strncmp(argv[i], "menu:", 5);
        bool isr = !strncmp(argv[i], "isr:", 4);
        bool play = !strncmp(argv[i], "play:", 5);
        quake_exec_mode = !strncmp(argv[i], "quake:", 6);
        if (!strncmp(argv[i], "keys:", 5)) {
            CHECK(!strcmp(argv[i + 1], "VMVIDEO"));
            printf("run %s through window keys (expect VMVIDEO stage 1, then stages 2-6)\n", argv[i] + 5);
            if (window_vmvideo(argv[i] + 5)) { failures++; screen(); break; }
            continue;
        }
        const char *command = fault ? argv[i] + 6 : gate ? argv[i] + 5 : rc ? argv[i] + 3 : isr ? argv[i] + 4 : menu || play ? argv[i] + 5 : video || quake_exec_mode ? argv[i] + 6 : argv[i];
        gv->dpmi_enabled = !gate;       /* gate: as a VM created without dosvm_config.dpmi */
        uint64_t start = instructions, ms = g_ticks;
#ifdef CIUKI_DOSVM_FILE_CPU
        /* Real monitor throttles records to 16/s; controller waits between COMs. */
        task_sleep_ms(1000);
#endif
        dosvm_expect(gv, argv[i + 1]);
        startup_irqs = gv->dpmi.stats.irqs_pm;
        printf("run %s (expect %s%s)\n", command, argv[i + 1], fault ? ", VM fault" : gate ? ", no DPMI host" : "");
#ifdef CIUKI_DOSVM_FILE_CPU
        if (gv->files && getenv("GUEST_FILE_TRACE")) {
            struct dos_file_trace discarded; while (dos_file_trace_pop(gv->files,&discarded)) { }
        }
#endif
        type(command);
        if (play) {
            int pr = play_run();
            bool returned = pr == 2 && prompt();
            /* f3-61 GUEST_PLAY_QUIT=1: leave DOOM through its own menu (Esc,
             * Up to Quit Game, Enter, y) as a player does, then require the
             * shell prompt: the EXEC frame completes and the shell is idle. */
            if (!returned && !stopped() && getenv("GUEST_PLAY_QUIT")) {
                CHECK(gv->exec_pending && !dosvm_shell_idle(gv));
                CHECK(dosvm_input_focus(dosvm_key(gv), true));
                static const unsigned quit_keys[] = {0x01, 0x148, 0x1C, 0x15};
                for (unsigned k = 0; k < sizeof(quit_keys) / sizeof(quit_keys[0]); k++) {
                    window_key(quit_keys[k], 1); menu_wait(80); window_key(quit_keys[k], 0); menu_wait(700);
                }
                menu_until = g_ticks + 60000;
                pr = run(4000000000ull, menu_elapsed);
                returned = pr == 2 && prompt();
                if (pr == 2) { waiting_input = false; run(20000000ull, 0); }
                printf("H61 QUIT returned=%u exec_pending=%u overflow=%u shell_idle=%u dpmi=%u clients=%u\n", returned,
                       gv->exec_pending, gv->exec_overflow, dosvm_shell_idle(gv), gv->dpmi.active, gv->dpmi.clients);
                if (!returned || gv->exec_pending || !dosvm_shell_idle(gv)) failures++;
            }
            if (!stopped()) play_frame();
            printf("H61 PLAY END %s returned=%u stopped=%u reason=%u vector=%u eip=%08x console=\"%s\"\n", argv[i + 1],
                   returned, stopped(), gv->info.reason, gv->info.fault_vector, gv->info.fault_eip, gv->console_line);
            unified_report("play");
            stats("play");
            fault_dump();
            screen();
            if (stopped()) failures++;
            break;
        }
        /* GUEST_BUDGET (instructions per command): long workloads (f3-06 DOOM). */
        const char *budget = getenv("GUEST_BUDGET");
        r = run(budget ? strtoull(budget, 0, 10) : 2000000000ull,
                fault ? 0 : quake_exec_mode ? quake_exec_done : video || menu || isr ? video_started : verdict);
        if (isr) {
            bool pass = r == 1 && isr_replay();
            printf("ISR shooting replay: %s\n", pass ? "PASS" : "FAIL");
            if (!pass) { failures++; fault_dump(); screen(); }
            break;
        }
        if (quake_exec_mode) {
            bool pass = r == 1 && quake_alias_returned && quake_alias_calls == 1 &&
                gv->dpmi.active && !gv->info.verdict_done && !gv->dpmi.stats.terminations && !gv->dpmi.stats.faults;
            printf("f3-28 Quake CRT data alias: %s (EXEC=%u reopened=%u aliases=%u active=%u entries=%u terminations=%u verdict=%s)\n",
                   pass ? "PASS" : "FAIL", quake_exec_checked, quake_reopened, quake_alias_calls,
                   gv->dpmi.active, gv->dpmi.stats.entries, gv->dpmi.stats.terminations, gv->info.verdict_detail);
            if (getenv("GUEST_COSTS")) stats("quake-crt");
            if (!pass) { failures++; screen(); }
            break;
        }
        if (video || menu) {
            printf("video startup replay: %s (%llu instructions, %llu ms, PM timer IRQs=%u)\n",
                   r == 1 ? "PASS" : "FAIL", (unsigned long long)(instructions - start),
                   (unsigned long long)(g_ticks - ms), gv->dpmi.stats.irqs_pm - startup_irqs);
            stats("video-startup");
            if (r != 1) { failures++; screen(); }
            else if (menu) {
                bool pass = menu_replay();
                if (getenv("GUEST_COSTS")) stats("menu");
                if (!pass) { failures++; fault_dump(); screen(); }
            }
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
        if (!gv->info.verdict_pass) {
#ifdef CIUKI_DOSVM_FILE_CPU
            if (gv->files) {
                struct dos_file_trace tr;
                while (dos_file_trace_pop(gv->files,&tr)) printf("H50 trace seq=%llu ret=%u ax=%04x bx=%04x cx=%04x dx=%04x ds=%04x flags=%04x cs:ip=%04x:%04x psp=%04x\n",
                    (unsigned long long)tr.sequence,tr.returning,tr.frame.ax,tr.frame.bx,tr.frame.cx,tr.frame.dx,tr.frame.ds,tr.frame.flags,tr.frame.cs,tr.frame.ip,tr.psp);
            }
#endif
            failures++; screen(); break;
        }
        r = run(400000000ull, 0);
        stats("after-exit");
        /* f3-61 launch state once the program returned (FreeCOM may have
         * reloaded itself from its XMS swap): no pending EXEC, shell idle. */
        if (guest_kernel)
            printf("H61 SHELL after %s exec_pending=%u overflow=%u shell_idle=%u\n", argv[i + 1],
                   gv->exec_pending, gv->exec_overflow, dosvm_shell_idle(gv));
#ifdef CIUKI_DOSVM_FILE_CPU
        if (gv->files) {
            unsigned processes=0; for (unsigned n=0;n<DOS_FILE_PSPS;n++) processes+=gv->files->process[n].used;
            printf("H50 live PSPs=%u selected=%04x owning=%04x\n",processes,gv->files->selected,gv->files->owning);
        }
#endif
        if (r != 2 || gv->dpmi.active) { failures++; screen(); break; }
    }
    if (guest_perf) {
        perf_phase(gv, 0);
        printf("host totals: guest_emulator_us=%llu monitor_us=%llu present_us=%llu harvest_us=%llu audio_us=%llu\n",
               (unsigned long long)perf_us(gv->perf.guest_cycles), (unsigned long long)perf_us(gv->perf.monitor_cycles),
               (unsigned long long)perf_us(gv->perf.present_cycles), (unsigned long long)perf_us(gv->perf.harvest_cycles),
               (unsigned long long)perf_us(gv->perf.audio_cycles));
        printf("host presenter: max_sample_us=%llu (whole-sample thread CPU; no cpustat/physical timer)\n",
               (unsigned long long)(presenter_sample_max / 1000));
#ifdef FX27_HEAD_BASELINE
        if (0) {
            printf("lean diag:");
#else
        if (getenv("GUEST_LEAN_DIAG")) {
            printf("lean diag: lean_exits=%u", gv->perf.lean_exits);
#endif
            for (unsigned i = 0; i < 24; i++) if (lean_why[i]) printf(" %s=%llu", lean_names[i], (unsigned long long)lean_why[i]);
            printf("\n");
        }
        printf("guest aperture: direct_bytes=%llu faults=%llu linear_maps=%u plane_maps=%u harvests=%u\n",
               (unsigned long long)aperture_direct_bytes, (unsigned long long)aperture_faults,
               gv->video_linear_maps, gv->video_maps, gv->perf.harvests);
    }
    if (getenv("GUEST_IRQ0_RESTORE") && restore_edges != 1) {
        printf("restore edge: FAIL (requested PM boundary was not reached)\n");
        failures++;
    }
    printf("debug traps: rm=%llu flags=%llu returns=%llu other=%llu\n",
           (unsigned long long)db_rm, (unsigned long long)db_flags,
           (unsigned long long)db_return, (unsigned long long)db_other);
    printf("debug trap owners:");
    for (unsigned i = 0; i < 16 && db_owners[i].owner; i++)
        printf(" %s=%llu", db_owners[i].owner, (unsigned long long)db_owners[i].count);
    printf("\n");
    printf("return causes: tf=%llu breakpoint=%llu armed=%u hits=%u expired=%u fallback=%u reacquired=%u segmented=%u\n",
           (unsigned long long)db_return_tf, (unsigned long long)db_return_breakpoint,
           gv->perf.return_armed, gv->perf.return_hits, gv->perf.return_expired,
           gv->perf.return_fallbacks, gv->perf.return_reacquired, gv->perf.return_segmented);
    if (uart_baud) printf("UART timing: baud=%u records=%u ms=%llu\n", uart_baud, uart_records, (unsigned long long)uart_ms);
    printf("guest runner: %s (%llu instructions, %llu traps, %u unexpected)\n", failures || unexpected ? "FAIL" : "PASS",
           (unsigned long long)instructions, (unsigned long long)traps, unexpected);
#ifdef CIUKI_DOSVM_FILE_CPU
    printf("H50 CPU binding=%u dispatches=%llu resident=%u\n",gv->files && gv->files->personality.live,
           (unsigned long long)(gv->files ? gv->files->sequence : 0),gv->info.resident);
    if (guest_kernel) {
        /* f3-61: the v1 bridge never binds a FreeDOS kernel; the unified
         * fork binds DFV2 and activates its IFS (C: owned). */
        unified_report("end");
        CHECK(!gv->files);
        if (guest_kernel==1 || getenv("GUEST_UNIFIED_UNBOUND")) CHECK(!gv->unified);
        else {
            const struct unified_runtime *u=(const struct unified_runtime *)gv->unified;
            CHECK(u && u->ifs.active && (u->ifs.owned&4) && !u->ifs.stats.forged && gv->file_volume==&((struct unified_runtime *)gv->unified)->volume);
        }
    }
    else if (!getenv("GUEST_NO_FILE_HOOK")) CHECK(gv->files && gv->files->personality.live);
    else CHECK(!gv->files);
    unsigned file_pages=(gv->file_bytes+gv->unified_bytes)/PAGE_SIZE, before_pages=used_pages;
    CHECK(!dosvm_file_destroy(gv));
    CHECK(!gv->files && !gv->file_volume && !gv->unified && used_pages+file_pages==before_pages);
    vfs_destroy(&store.vfs); cache_destroy(&store.cache);
#endif
    if (cow) { fclose(cow); unlink(cow_path); }
    gcpu_done();
    return failures || unexpected;
}
