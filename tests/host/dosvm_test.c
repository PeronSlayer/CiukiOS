/* T0: production DOS lifecycle/BIOS/disk/monitor and paging functions.
 * Only CPU, scheduler, PMM and VFS transport are fake. No DOS boot claimed.
 * SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#ifdef CIUKI_DOSVM_GUEST_CPU
#include "dosvm_guest.h"
#endif
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
static void host_clock_tick(void);

#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c); exit(1); } } while (0)
#define KERNEL_VBASE 0xC0000000u
#define P2V(p) ((void *)(ram + (p)))
#define V2P(p) ((uint32_t)((uint8_t *)(p) - ram))
#define HOST_PAGES 32768u
static unsigned checks, used_pages, heap_blocks, allocation_calls, fail_at;
static bool allocated[HOST_PAGES];
static uint8_t *ram;
static unsigned cr3_reloads;
static uint32_t cpu_cr3, cpu_flags = V86_IF, next_pfn = 4096;
static uint32_t kpd[1024], kpd_phys;
static struct task caller;
struct task *g_current = &caller;
uint32_t g_task_count, g_user_mappings;
volatile uint64_t g_ticks;
volatile bool g_need_resched;
struct ciuki_boot_info g_boot;
static uint32_t read_cr3(void) { return cpu_cr3; }
uint64_t g_tsc_per_ms;
static uint64_t fake_tsc;
static uint64_t (*tsc_clock_hook)(void);
static uint64_t read_tsc_serialized(void) { return tsc_clock_hook ? tsc_clock_hook() : fake_tsc; }
static void write_cr3(uint32_t x) { cpu_cr3 = x; cr3_reloads++; }
static uint32_t fake_cr2 = 0xDEAD000;
static uint32_t read_cr2(void) { return fake_cr2; }
static void cli(void) { cpu_flags = 0; }
static void sti(void) { cpu_flags = V86_IF; }
static unsigned invalidations;
static void invlpg(uint32_t va) { (void)va; invalidations++; }
static uint32_t *table_va(uint32_t p) { return P2V(p); }
static uint32_t irq_save(void) { uint32_t f=cpu_flags; cli(); return f; }
static void irq_restore(uint32_t f) { cpu_flags=f; }
/* f3-04 architectural seams: LDT register, debug registers, CPUID family. */
static const void *ldt_loaded; static unsigned ldt_entries;
static uint32_t debug_dr7, debug_addr[4], debug_dr6;
void arch_ldt_load(const void *base, unsigned entries) { if (!entries) base=0; ldt_loaded=base; ldt_entries=base?entries:0; }
void arch_debug_load(const uint32_t a[4], uint32_t dr7) { debug_dr7=dr7; for (unsigned i=0;i<4;i++) debug_addr[i]=dr7?a[i]:0; }
uint32_t arch_debug_take(void) { uint32_t d=debug_dr6; debug_dr6=0; return d; }
uint8_t arch_cpu_family(void) { return 6; }
#include "../../src/kernel/vm/dosvm_internal.h"
#include "dosvm_fullscreen_seams.inc"
static bool allocation_fails(void) { return ++allocation_calls == fail_at; }
/* Deterministic work accounting, enabled only for the F0-41 replay. */
static bool creation_cost_enabled, creation_cost_points;
static uint64_t creation_cost_ns, creation_cost_span, creation_cost_max;
static void creation_cost_charge(uint64_t ns)
{ if (creation_cost_enabled) { creation_cost_ns += ns; creation_cost_span += ns; } }
static void creation_cost_boundary(void)
{
    if (creation_cost_span > creation_cost_max) creation_cost_max = creation_cost_span;
    creation_cost_span = 0;
}
uint32_t pmm_alloc(void)
{
    creation_cost_charge(4000);
    if (allocation_fails()) return 0;
    /* Deliberately separate consecutive mappings in physical memory. */
    for (unsigned n = 0; n < HOST_PAGES; n++) {
        unsigned p = next_pfn; next_pfn += 2;
        if (next_pfn >= HOST_PAGES) next_pfn = 4096 + ((next_pfn & 1) ^ 1);
        if (!allocated[p]) { allocated[p] = true; used_pages++; memset(P2V(p * PAGE_SIZE),0xA5,PAGE_SIZE); return p * PAGE_SIZE; }
    }
    return 0;
}
uint32_t pmm_alloc_zone(int zone) { CHECK(zone == ZONE_NORMAL); return pmm_alloc(); }
void pmm_free(uint32_t p) { CHECK(p >= 0x1000000 && !(p & 4095) && p / PAGE_SIZE < HOST_PAGES && allocated[p / PAGE_SIZE]); allocated[p / PAGE_SIZE] = false; used_pages--; }
uint32_t pmm_free_count(void) { return HOST_PAGES - 4096 - used_pages; }
uint32_t vmm_kernel_pd(void) { return kpd_phys; }
void panic(const char *fmt, ...) { fprintf(stderr,"unexpected panic: %s\n",fmt); abort(); }
static unsigned dosvm_logs;
static char last_dosvm_log[256];
static char last_disk_log[256], last_launch_log[256];
static unsigned log_sequence, launch_stages, launch_errors, launch_stage_sequence, launch_error_sequence;
static unsigned launch_stage_pages;
static unsigned vm_audio_logs;
static char lifecycle_stop[256],lifecycle_cleanup[256];
static char last_vm_audio_log[256];
void klog(const char *fmt, ...)
{
    log_sequence++;
    if (!strncmp(fmt, "[launch", 7)) {
        CHECK(cpu_flags & V86_IF);
        va_list ap; va_start(ap, fmt);
        int n = vsnprintf(last_launch_log, sizeof(last_launch_log), fmt, ap);
        va_end(ap);
        CHECK(n > 0 && n < (int)sizeof(last_launch_log));
        if (!strncmp(fmt, "[launch-stage]", 14)) {
            launch_stages++; launch_stage_sequence = log_sequence; launch_stage_pages = used_pages;
        } else { launch_errors++; launch_error_sequence = log_sequence; }
        return;
    }
    if (!strncmp(fmt, "[vm-audio]", 10)) {
        CHECK(cpu_flags & V86_IF);
        va_list ap; va_start(ap, fmt);
        int n = vsnprintf(last_vm_audio_log, sizeof(last_vm_audio_log), fmt, ap);
        va_end(ap);
        CHECK(n > 0 && n < (int)sizeof(last_vm_audio_log)); vm_audio_logs++; return;
    }
    if (strncmp(fmt, "[dosvm]", 7)) return;
    CHECK(cpu_flags & V86_IF);
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(last_dosvm_log, sizeof(last_dosvm_log), fmt, ap);
    va_end(ap);
    CHECK(n > 0 && n < (int)sizeof(last_dosvm_log));
    if (!strncmp(fmt, "[dosvm] disk", 12)) strcpy(last_disk_log, last_dosvm_log);
    if (strstr(last_dosvm_log,"lifecycle stage=stop")) strcpy(lifecycle_stop,last_dosvm_log);
    if (strstr(last_dosvm_log,"lifecycle stage=cleanup")) strcpy(lifecycle_cleanup,last_dosvm_log);
    dosvm_logs++;
}
void klog_diag(const char *fmt,...)
{
    if (!(g_boot.flags & CBI_F_DIAG_VERBOSE)) return;
    CHECK(cpu_flags & V86_IF);
    va_list ap;va_start(ap,fmt);
    int n=vsnprintf(last_vm_audio_log,sizeof(last_vm_audio_log),fmt,ap);va_end(ap);
    CHECK(n>0 && n<128);vm_audio_logs++;
}
/* The production heap (core/kheap.c) has power-of-two classes up to 2048
 * bytes with an 8-byte header: larger requests always fail there, so they
 * fail here too (f3-06 lookup table, vm-window QEMU 2026-10-12). The fake
 * scheduler's own task/stack storage uses harness_alloc instead. */
static void *harness_alloc(size_t n) { if (allocation_fails()) return 0; void *p = malloc(n); if (p) heap_blocks++; return p; }
void *kmalloc(size_t n) { return n > 2040 ? 0 : harness_alloc(n); }
void kfree(void *p) { if (p) { CHECK(heap_blocks); heap_blocks--; free(p); } }
/* Generated from the unmodified production user-address-space section. */
#include "dosvm_aspace.inc"

static struct { struct task *t; void (*fn)(void *); void *arg; } tasks[DOSVM_LIMIT];
static jmp_buf guest_return, worker_return;
static void (*sleep_hook)(void);
static void (*schedule_hook)(void);
static void (*preempt_hook)(void);
static unsigned preempt_points;
static void (*guest_enter_hook)(const struct v86_frame *);
static unsigned native_progress, guest_enters;
static bool guest_halt;
struct task *task_create_kernel(const char *name, void (*fn)(void *), void *arg, enum task_prio prio)
{
    CHECK(!strcmp(name,"dosvm") && prio == P_NORMAL);
    struct task *t = harness_alloc(sizeof(*t));
    if (!t) return 0;
    memset(t,0,sizeof(*t)); t->state = T_BLOCKED;
    t->kstack = harness_alloc(KSTACK_SIZE); t->fpu_alloc = harness_alloc(512);
    if (!t->kstack || !t->fpu_alloc) { kfree(t->kstack); kfree(t->fpu_alloc); kfree(t); return 0; }
    t->fpu_area = t->fpu_alloc;
    for (unsigned i = 0; i < DOSVM_LIMIT; i++) if (!tasks[i].t) { tasks[i].t=t; tasks[i].fn=fn; tasks[i].arg=arg; g_task_count++; return t; }
    CHECK(false); return 0;
}
void task_start(struct task *t) { CHECK(t->state == T_BLOCKED); t->state = T_READY; }
bool task_alive(const struct task *t) { return t->state < T_ZOMBIE; }
void task_kill(struct task *t, int reason) { t->state = T_ZOMBIE; t->exit_code = reason; }
void task_reap(struct task *t)
{
    CHECK(t != g_current && t->state == T_ZOMBIE);
    for (unsigned i = 0; i < DOSVM_LIMIT; i++) if (tasks[i].t == t) tasks[i].t=0;
    g_task_count--; kfree(t->kstack); kfree(t->fpu_alloc); kfree(t);
}
void task_exit(int reason) { task_kill(g_current,reason); longjmp(worker_return,1); }
void schedule(void) { native_progress++; g_need_resched = false; if (schedule_hook) schedule_hook(); }
void kpreempt_point(void)
{
    if (!(cpu_flags & V86_IF) || (g_current && g_current->preempt_holds)) return;
    if (creation_cost_enabled && creation_cost_points) {
        creation_cost_charge(200); creation_cost_boundary();
    }
    preempt_points++;
    if (preempt_hook) preempt_hook();
    if (g_need_resched) schedule();
}
void crit_begin(void) { }
void crit_end(void) { }
void task_sleep_ms(uint32_t ms)
{
    CHECK(cpu_flags & V86_IF); g_ticks += ms; host_clock_tick(); native_progress++;
    if (sleep_hook) sleep_hook();
    for (unsigned i = 0; i < DOSVM_LIMIT; i++) if (tasks[i].t) {
        struct dosvm *v = tasks[i].arg;
        if (v->info.state == DOSVM_STOPPING) task_kill(tasks[i].t,v->info.reason);
    }
}
void tss_set_kernel_stack(uint32_t sp) { (void)sp; }
void fpu_handle_nm(void) { CHECK(g_current->fpu_area != 0); }
gen_t gen_alloc(void) { static gen_t generation; return ++generation; }
bool gen_matches(gen_t a, gen_t b) { return a && a == b; }
uint64_t deadline_after_ms(uint32_t ms) { return g_ticks + ms; }
bool deadline_passed(uint64_t t) { return g_ticks - t < (1ull << 63); }
void kwait_init(struct kwait *q) { q->head=0; }
void kwait_wake_all(struct kwait *q) { (void)q; }
void kwait_wake_io(struct kwait *q) { (void)q; }
void kmutex_init(struct kmutex *m) { memset(m,0,sizeof(*m)); }
void kmutex_lock(struct kmutex *m) { CHECK((cpu_flags&V86_IF) && !m->owner); m->owner=g_current; }
void kmutex_unlock(struct kmutex *m) { CHECK(m->owner==g_current); m->owner=0; }
static unsigned wait_calls;
static uint64_t wait_deadline;
static void (*wait_register_hook)(void);
bool kwait_wait_until(struct kwait *q, kwait_cond_fn fn, void *arg, uint64_t until)
{
    (void)q; wait_calls++; wait_deadline = until;
    if (wait_register_hook) wait_register_hook();
    while (!fn(arg) && !deadline_passed(until)) task_sleep_ms(1);
    return fn(arg);
}
static bool input_pending;
static struct input_event input_next;
bool input_read(struct input_event *e) { if (!input_pending) return false; *e=input_next; input_pending=false; return true; }
void v86_leave(uint32_t continuation) { CHECK(continuation == 0x12345678); sti(); longjmp(guest_return,1); }
void v86_enter(const struct v86_frame *initial, uint32_t *continuation)
{
    *continuation=0x12345678; guest_enters++;
    sti(); /* trap_return's IRET enables physical IF in either guest mode */
    if (guest_enter_hook) { guest_enter_hook(initial); return; }
    struct v86_frame f = *initial; f.tf.vector=13; f.tf.edx=DOSVM_FAULT_PORT;
    if (guest_halt) {
        uint8_t opcode=0xF4;
        for (unsigned i=0;i<DOSVM_LIMIT;i++) if (tasks[i].t==g_current)
            CHECK(!dosvm_write(tasks[i].arg,f.tf.eip,&opcode,1));
    }
    CHECK(dosvm_task_cr3(g_current,0) == cpu_cr3 && dosvm_task_esp0(g_current,0) == *continuation);
    if (!setjmp(guest_return)) { dosvm_trap(&f.tf); CHECK(false); }
}
static void run_worker(struct dosvm *v)
{
    for (unsigned i=0;i<DOSVM_LIMIT;i++) if (tasks[i].t==v->task) {
        g_current=v->task;
        if (!setjmp(worker_return)) tasks[i].fn(tasks[i].arg);
        g_current=&caller; sti(); CHECK(cpu_cr3==kpd_phys); return;
    }
    CHECK(false);
}

/* File-backed fake VFS transport. Production overlay indexing remains real. */
static struct storage store = {.ready=true};
static struct { struct vfs_table *table; FILE *cow; bool base_open, cow_open; uint64_t pos[2], base_bytes; char path[80]; void (*base_sector)(uint32_t, void *); } files[DOSVM_LIMIT];
static unsigned handles, tables, scratch, disk_reads, disk_writes, commits, deletes;
static unsigned io_calls, io_fail_at;
static bool short_read, short_write, fail_delete, fail_commit, fail_close, bad_signature;
static bool fail_read_ahead;
static void (*disk_read_hook)(void);
static void (*disk_base_sector)(uint32_t, void *);
static bool io_fails(void) { return ++io_calls == io_fail_at; }
static unsigned file_slot(struct vfs_table *table) { for (unsigned i=0;i<DOSVM_LIMIT;i++) if (files[i].table==table) return i; CHECK(false); return 0; }
static void disk_owned(struct vfs_table *table)
{ struct dosvm *v=(void *)((uint8_t *)table - offsetof(struct dosvm,disk_table)); CHECK(v->disk_busy); }
struct storage *storage_get(void) { return &store; }
int vfs_table_init(struct vfs *vfs, struct vfs_table *table, unsigned drive)
{
    CHECK(vfs==&store.vfs && drive==2);
    if (io_fails()) return -V86_EIO;
    for (unsigned i=0;i<DOSVM_LIMIT;i++) if (!files[i].table) { memset(&files[i],0,sizeof(files[i])); files[i].table=table; tables++; return 0; }
    return -ENOSPC;
}
void vfs_table_destroy(struct vfs_table *table)
{ unsigned i=file_slot(table); CHECK(!files[i].cow && !files[i].base_open && !files[i].cow_open); files[i].table=0; tables--; }
/* f3-06: the canonical payload or a further workload base image. The fake
 * workload disk is drive D: and exists only while workload_disk is set; the
 * bundled games disk (owner decision 2026-10-12) is C:\\GAMES\\CIUKIGAM.IMG,
 * present while bundled_disk is set, bundled_bytes long (cut images). */
static char last_base[32];
static bool workload_disk, bundled_disk = true, bundled_directory;
static char workload_drive = 'D';
static uint32_t bundled_bytes = DOSVM_SECTORS * 512u;
static unsigned workload_scans, bundled_scans;
static int fixture_errors[26], bundled_error, bundled_open_error;
static bool fixture_path(const char *path) { return strlen(path) == 15 && path[0] >= 'D' && path[0] <= 'Z' && path[1] == ':' && !strcmp(path + 2, "\\CIUKIGAM.IMG"); }
static bool base_path(const char *path) { return !strcmp(path,"C:\\SYSTEM\\DOS\\DOSBOOT.IMG") || fixture_path(path) || !strcmp(path,"C:\\GAMES\\CIUKIGAM.IMG"); }
static uint64_t base_bytes(const char *path) { return !strcmp(path,"C:\\GAMES\\CIUKIGAM.IMG") ? bundled_bytes : DOSVM_SECTORS * 512u; }
int vfs_stat(struct vfs_table *table, const char *path, struct fat_entry *entry)
{
    (void)file_slot(table);
    if (fixture_path(path)) {
        CHECK(path[0] >= 'D' && path[0] <= 'Z');
        if (path[0] == 'D') workload_scans++;
        if (fixture_errors[path[0] - 'A']) return fixture_errors[path[0] - 'A'];
        if (!workload_disk || path[0] != workload_drive) return -ENOENT;
    }
    if (!strcmp(path,"C:\\GAMES\\CIUKIGAM.IMG")) { bundled_scans++; if (bundled_error) return bundled_error; if (!bundled_disk) return -ENOENT; }
    CHECK(base_path(path)); snprintf(last_base,sizeof(last_base),"%s",path); if (io_fails()) return -V86_EIO;
    memset(entry,0,sizeof(*entry)); entry->size=(uint32_t)base_bytes(path);
    if (bundled_directory && !strcmp(path,"C:\\GAMES\\CIUKIGAM.IMG")) entry->attr=0x10;
    return 0;
}
int vfs_open(struct vfs_table *table, const char *path, unsigned access, enum vfs_share share, uint8_t attr)
{
    unsigned i=file_slot(table); CHECK(attr==0); if (io_fails()) return -V86_EIO;
    if (!strcmp(path,"C:\\GAMES\\CIUKIGAM.IMG") && bundled_open_error) return bundled_open_error;
    if (base_path(path)) { CHECK(access==VFS_READ && share==VFS_DENY_WRITE); files[i].base_open=true; files[i].base_bytes=base_bytes(path); files[i].base_sector=disk_base_sector; handles++; return 0; }
    CHECK(access==(VFS_READ|VFS_WRITE|VFS_CREATE|VFS_EXCLUSIVE) && share==VFS_DENY_ALL && strstr(path,".COW"));
    snprintf(files[i].path,sizeof(files[i].path),"build/host/dosvm-cow-XXXXXX");
    int fd=mkstemp(files[i].path); CHECK(fd>=0); files[i].cow=fdopen(fd,"w+b"); CHECK(files[i].cow);
    files[i].cow_open=true; scratch++; handles++; return 1;
}
int vfs_close(struct vfs_table *table, int handle)
{ unsigned i=file_slot(table); if (handle==0) { CHECK(files[i].base_open); files[i].base_open=false; } else { CHECK(handle==1 && files[i].cow_open); files[i].cow_open=false; } handles--; return fail_close ? -V86_EIO : 0; }
int vfs_delete(struct vfs_table *table, const char *path)
{ unsigned i=file_slot(table); CHECK(strstr(path,".COW") && !files[i].cow_open); if (fail_delete) return -V86_EIO; CHECK(files[i].cow); CHECK(fclose(files[i].cow)==0 && unlink(files[i].path)==0); files[i].cow=0; scratch--; deletes++; return 0; }
int vfs_seek(struct vfs_table *table, int handle, int64_t offset, enum vfs_whence whence, uint64_t *position)
{ unsigned i=file_slot(table); disk_owned(table); CHECK(handle>=0 && handle<=1 && whence==VFS_SEEK_SET && offset>=0); if (io_fails()) return -V86_EIO; files[i].pos[handle]=offset; *position=offset; return 0; }
int vfs_read(struct vfs_table *table, int handle, void *data, size_t bytes, size_t *done)
{
    unsigned i=file_slot(table); disk_owned(table);
    CHECK(bytes && !(bytes % 512) && bytes <= DOSVM_READ_SECTORS * 512u && (!handle || bytes == 512));
    disk_reads++;
    if (disk_read_hook) disk_read_hook();
    if (io_fails() || (fail_read_ahead && bytes > 512)) return -V86_EIO;
    *done=short_read ? 511 : bytes;
    if (!handle) {
        CHECK(files[i].pos[0]+bytes<=files[i].base_bytes);
        for (size_t at=0;at<bytes;at+=512) {
            uint32_t lba=(uint32_t)(files[i].pos[0]/512+at/512);
            if (files[i].base_sector) files[i].base_sector(lba,(uint8_t *)data+at);
            else memset((uint8_t *)data+at,lba%251,512);
        }
        if (!files[i].base_sector && !files[i].pos[0]) { ((uint8_t *)data)[0]=0xEE; ((uint8_t *)data)[510]=bad_signature ? 0 : 0x55; ((uint8_t *)data)[511]=0xAA; }
    }
    else { CHECK(fseek(files[i].cow,(long)files[i].pos[1],SEEK_SET)==0); CHECK(fread(data,1,*done,files[i].cow)==*done); }
    return 0;
}
int vfs_write(struct vfs_table *table, int handle, const void *data, size_t bytes, size_t *done)
{ unsigned i=file_slot(table); disk_owned(table); CHECK(handle==1 && bytes==512); disk_writes++; if (io_fails()) return -V86_EIO; *done=short_write ? 511 : bytes; CHECK(fseek(files[i].cow,(long)files[i].pos[1],SEEK_SET)==0); CHECK(fwrite(data,1,*done,files[i].cow)==*done); return 0; }
int vfs_commit(struct vfs_table *table, int handle)
{ unsigned i=file_slot(table); CHECK(handle==1); commits++; if (fail_commit) return -V86_EIO; CHECK(fflush(files[i].cow)==0); return 0; }

static char last_record[256];
static char recent_records[8][256];
static unsigned records, guest_records, max_record;
static void (*diag_time_hook)(void);
static bool evidence_active = true;
bool rec_active(void) { return evidence_active; }
static unsigned disk_records;
static char tracker_record[256], vm_start_record[256];
static char fault_pc_record[256], fault_regs_record[256], fault_stack_record[256];
static unsigned fault_pc_records, fault_regs_records, fault_stack_records;
void rec_diag_disk_bootlog(const char *probe,const char *fmt,...)
{
    char extra[240],wire[256]; va_list ap; va_start(ap,fmt);
    int n=vsnprintf(extra,sizeof(extra),fmt,ap); va_end(ap);
    CHECK(n>=0 && n<(int)sizeof(extra));
    n=snprintf(wire,sizeof(wire),"CIUKI_TEST v=1 run=00000000 seq=999999 probe=%s event=DATA %s",probe,extra);
    CHECK(n>0 && n<=240); disk_records++;
    if (strstr(extra, "group=vm_perf_tracker ")) strcpy(tracker_record, wire);
    if (strstr(extra, "group=vm_start ")) strcpy(vm_start_record, wire);
    if (strstr(extra, "group=vm_fault_pc ")) { strcpy(fault_pc_record, wire); fault_pc_records++; }
    if (strstr(extra, "group=vm_fault ")) { strcpy(fault_regs_record, wire); fault_regs_records++; }
    if (strstr(extra, "group=vm_stack part=0 ")) { strcpy(fault_stack_record, wire); fault_stack_records++; }
    if ((unsigned)n > max_record) max_record = n;
}
void rec_emit(const char *probe, const char *event, const char *fmt, ...)
{
    char extra[240]; va_list ap; va_start(ap,fmt); int n=vsnprintf(extra,sizeof(extra),fmt,ap); va_end(ap);
    CHECK(n>=0 && n<(int)sizeof(extra) && (!strcmp(probe,"vm-boot") || !strcmp(probe,"vm-video") || !strcmp(probe,"vm-window") || !strcmp(probe,"vm-fullscreen")));
    n=snprintf(last_record,sizeof(last_record),"CIUKI_TEST v=1 run=12345678 seq=%06u probe=%s event=%s %s",++records,probe,event,extra);
    CHECK(n>0 && n<=240); if ((unsigned)n>max_record) max_record=n;
    memcpy(recent_records[(records - 1) % 8], last_record, sizeof(last_record));
    if (strstr(extra,"group=guest ")) guest_records++;
}
void rec_diag(const char *probe, const char *fmt, ...)
{
    if (!(g_boot.flags & (CBI_F_VERBOSE | CBI_F_DIAG_VERBOSE))) return;
    CHECK(cpu_flags & V86_IF);
    char extra[240]; va_list ap; va_start(ap,fmt);
    int n=vsnprintf(extra,sizeof(extra),fmt,ap); va_end(ap);
    CHECK(n>=0 && n<(int)sizeof(extra));
    rec_emit(probe,"DATA","%s",extra);
    if (diag_time_hook) diag_time_hook();
}
#include "../../src/kernel/vm/dosvm_pic.c"
#include "../../src/kernel/vm/v86.c"
#include "dosvm_work.inc"
static void *creation_cost_clear(void *p, int value, size_t n)
{ creation_cost_charge(n * 8000 / PAGE_SIZE); return memset(p, value, n); }
static void *creation_cost_copy(void *to, const void *from, size_t n)
{ creation_cost_charge(n * 8000 / PAGE_SIZE); return memcpy(to, from, n); }
static int creation_cost_map(struct aspace *as, uint32_t va, uint32_t phys, uint32_t flags)
{ creation_cost_charge(6000); return as_map(as, va, phys, flags); }
static uint8_t creation_cost_peek(const cvga_state *v, uint32_t at)
{ creation_cost_charge(2000); return cvga_peek_vram(v, at); }
static void creation_cost_store(cvga_state *v, uint32_t at, uint8_t byte)
{ creation_cost_charge(2000); cvga_write_vram(v, at, byte); }
#define memset creation_cost_clear
#define memcpy creation_cost_copy
#define as_map creation_cost_map
#define cvga_peek_vram creation_cost_peek
#define cvga_write_vram creation_cost_store
#include "../../src/kernel/vm/dosvm.c"
#include "../../src/kernel/vm/dosvm_disk.c"
#include "../../src/kernel/vm/dosvm_bios.c"
#include "../../src/kernel/vm/dosvm_pit.c"
#include "../../src/kernel/vm/dosvm_kbd.c"
#include "../../src/kernel/vm/dosvm_mouse.c"
#include "../../src/kernel/vm/dosvm_xms.c"
#include "../../src/kernel/vm/dosvm_umb.c"
#include "../../src/kernel/vm/dosvm_ems.c"
/* f3-02 production VGA adapter and native window boundary; fakes and cases
 * in dosvm_video_test.inc. The font table is generated from the asset the
 * kernel's font.asm includes (scripts/test/host_kernel_tests.sh). */
#include "dosvm_font.inc"
#include "../../src/kernel/vm/dosvm_vga.c"
#include "../../src/kernel/vm/dosvm_vga_vbe.c"
#include "../../src/kernel/vm/dosvm_client.c"
#include "../../src/kernel/vm/vga_hardware.c"
#include "../../src/kernel/vm/dosvm_fullscreen.c"
#include "../../src/kernel/vm/dosvm_dma.c"
#include "../../src/kernel/vm/dosvm_opl.c"
#include "../../src/kernel/vm/dosvm_sb16.c"
#undef cvga_write_vram
#undef cvga_peek_vram
#undef as_map
#undef memcpy
#undef memset
#include "../../src/kernel/vm/dpmi_desc.c"
#include "../../src/kernel/vm/dpmi_switch.c"
#include "../../src/kernel/vm/dpmi_dispatch.c"
#include "../../src/kernel/vm/dpmi.c"

static struct dosvm *create(void)
{ struct dosvm *v; struct dosvm_config c={.commitment=DOSVM_LOW_CAP}; CHECK(dosvm_create(&c,&v)==0 && v); CHECK(v->info.state==DOSVM_CREATING && v->info.resident<=DOSVM_LOW_CAP); return v; }
static void empty(void)
{ struct dosvm_ledger l; dosvm_snapshot(&l); CHECK(!l.vms && !l.pages && !l.tasks && !l.handles && !l.devices && !l.resident && !l.reserved); CHECK(!used_pages && !heap_blocks && !handles && !tables && !scratch && !g_task_count && !g_user_mappings); for (unsigned i=0;i<DOSVM_LIMIT;i++) CHECK(!slots[i].disk_busy); }
static void destroy(struct dosvm *v, bool forced)
{ struct dosvm_key key=dosvm_key(v); CHECK(dosvm_destroy(key,forced)==0); CHECK(dosvm_destroy(key,forced)==0 && !dosvm_valid(key) && !dosvm_keyboard(key,'X') && !dosvm_irq_raise(key,0)); }
static void line(struct dosvm *v, const char *text)
{ while (*text) CHECK(dosvm_record_byte(v,*text++)==0); CHECK(dosvm_record_byte(v,10)==0); }
static void test_admission(void)
{
    struct dosvm_admission a;
    CHECK(dosvm_admit(40ull<<20,0,0,DOSVM_LOW_CAP,&a) && a.outstanding_bytes==0 && a.headroom_bytes==(32u<<20));
    CHECK(!dosvm_admit((40ull<<20)-1,0,0,DOSVM_LOW_CAP,&a));
    CHECK(dosvm_admit(47ull<<20,8ull<<20,1ull<<20,DOSVM_LOW_CAP,&a) && a.outstanding_bytes==(7ull<<20));
    CHECK(!dosvm_admit(46ull<<20,8ull<<20,1ull<<20,DOSVM_LOW_CAP,0));
    CHECK(!dosvm_admit(UINT64_MAX,1,2,DOSVM_LOW_CAP,0));
    CHECK(!dosvm_admit(UINT64_MAX,0,0,DOSVM_LOW_CAP-1,0));
    CHECK(!dosvm_admit(UINT64_MAX,0,0,DOSVM_RESIDENT_CAP+4096,0));
}
static void test_creation_failures(void)
{
    allocation_calls=0; struct dosvm *v=create(); unsigned allocations=allocation_calls; destroy(v,true); empty();
    for (unsigned i=1;i<=allocations;i++) { allocation_calls=0; fail_at=i; v=(void *)1; CHECK(dosvm_create(0,&v)==-ENOMEM && !v); fail_at=0; empty(); }
    for (unsigned i=1;i<=4;i++) { io_calls=0; io_fail_at=i; CHECK(dosvm_create(0,&v)<0 && !v); io_fail_at=0; empty(); }
    store.stopped=true; CHECK(dosvm_create(0,&v)<0 && !v); store.stopped=false; empty();
    v=create(); bad_signature=true; CHECK(dosvm_boot(v)<0 && v->info.state==DOSVM_CREATING); bad_signature=false; destroy(v,true); empty();
    printf("DOS creation failures: %u allocation sites + 4 VFS sites unwound\n",allocations);
}
static void test_memory_disk(void)
{
    struct dosvm *a=create(), *b=create(); uint8_t bytes[512], got[512]; memset(bytes,0x7B,sizeof(bytes));
    CHECK(a->frames[0]>=0x1000000 && a->frames[1]!=a->frames[0]+4096);
    CHECK(dosvm_write(a,0xFFF,bytes,sizeof(bytes))==0 && dosvm_read(a,0xFFF,got,sizeof(got))==0 && !memcmp(bytes,got,512));
    CHECK(dosvm_read(b,0xFFF,got,512)==0 && memcmp(bytes,got,512));
    CHECK(!dosvm_range(a,0x10FFEF,2,true) && !dosvm_range(a,UINT32_MAX,512,true));
    CHECK(dosvm_write(a,0xF0000,bytes,1)==-EFAULT);
    CHECK(dosvm_write(a,0xBFFF0,bytes,32)==-EFAULT); CHECK(dosvm_read(a,0xBFFF0,got,16)==0 && got[0]==0x20 && got[1]==0x07); /* f3-02: cleared mode 3 text cell through the VGA model */
    uint32_t x=0x12345678,y=0; CHECK(dosvm_write(a,0,&x,4)==0 && dosvm_set_a20(a,false)==0);
    CHECK(dosvm_read(a,0x100000,&y,4)==0 && y==x); CHECK(dosvm_set_a20(a,true)==0 && dosvm_read(a,0x100000,&y,4)==0 && !y);
    CHECK(dosvm_disk_write(a,77,bytes)==0 && dosvm_disk_read(a,77,got)==0 && !memcmp(bytes,got,512));
    CHECK(dosvm_disk_read(b,77,got)==0 && memcmp(bytes,got,512));
    memset(bytes,0x42,512); CHECK(dosvm_disk_write(a,77,bytes)==0 && a->sector_count==1);
    short_write=true; CHECK(dosvm_disk_write(a,78,bytes)<0 && a->sector_count==1); short_write=false;
    CHECK(dosvm_disk_read(a,78,got)==0 && got[0]==78);
    short_read=true; CHECK(dosvm_disk_read(a,128,got)<0); short_read=false;
    CHECK(dosvm_disk_write(a,DOSVM_SECTORS,bytes)==-EINVAL);
    for (unsigned i=0;i<DOSVM_OVERLAY_SECTORS;i++) if (i!=77) CHECK(dosvm_disk_write(a,i,bytes)==0);
    unsigned issued=disk_writes; CHECK(dosvm_disk_write(a,1024,bytes)==-ENOSPC && disk_writes==issued);
    CHECK(dosvm_set_a20(a,false)==0); destroy(a,true); CHECK(a->info.data_loss); destroy(b,false); empty();
}
static struct v86_frame disk_frame(unsigned ax)
{ return (struct v86_frame){.tf={.eax=ax,.edx=0x80,.ecx=1,.ebx=0xFFF,.eflags=V86_VM|V86_IF|2},.es=0x1000,.ds=0x2000}; }
static void test_bios_bounds(void)
{
    struct dosvm *v=create(); struct v86_frame f=disk_frame(0x0202); uint8_t data[1024];
    CHECK(dosvm_bios(v,&f,0x13)==0 && !(f.tf.eflags&V86_CF)); CHECK(dosvm_read(v,0x10FFF,data,1024)==0 && data[510]==0x55 && data[512]==1);
    unsigned reads=disk_reads,writes=disk_writes;
    f=disk_frame(0x0201); f.tf.edx=0x81; CHECK(!dosvm_bios(v,&f,0x13) && (f.tf.eflags&V86_CF) && disk_reads==reads);
    f=disk_frame(0x0202); f.tf.ebx=0xFFFF; CHECK(!dosvm_bios(v,&f,0x13) && (f.tf.eflags&V86_CF) && disk_reads==reads);
    f=disk_frame(0x0201); f.es=0xF000; CHECK(!dosvm_bios(v,&f,0x13) && (f.tf.eflags&V86_CF) && disk_reads==reads);
    f=disk_frame(0x0300); CHECK(!dosvm_bios(v,&f,0x13) && (f.tf.eflags&V86_CF) && disk_writes==writes);
    uint8_t dap[16]={16,0,2,0,0xFF,0x0F,0,0x10,0xFE,0xFF,3,0,0,0,0,0};
    CHECK(dosvm_write(v,0x20000,dap,16)==0); f=disk_frame(0x4200); CHECK(!dosvm_bios(v,&f,0x13) && !(f.tf.eflags&V86_CF) && disk_reads==reads+1);
    CHECK(!dosvm_read(v,0x10FFF,data,1024) && data[0]==(DOSVM_SECTORS-2)%251 && data[512]==(DOSVM_SECTORS-1)%251);
    reads=disk_reads; dap[2]=3; CHECK(dosvm_write(v,0x20000,dap,16)==0); f=disk_frame(0x4200); CHECK(!dosvm_bios(v,&f,0x13) && (f.tf.eflags&V86_CF) && disk_reads==reads);
    CHECK(dosvm_read(v,0x20000,data,16)==0 && data[2]==3);
    dap[2]=1; dap[12]=1; CHECK(dosvm_write(v,0x20000,dap,16)==0); f=disk_frame(0x4300); CHECK(!dosvm_bios(v,&f,0x13) && (f.tf.eflags&V86_CF) && disk_writes==writes);
    dap[12]=0; dap[0]=24; CHECK(dosvm_write(v,0x20000,dap,16)==0); f=disk_frame(0x4200); CHECK(!dosvm_bios(v,&f,0x13) && (f.tf.eflags&V86_CF) && disk_reads==reads);
    f=disk_frame(0x0800); CHECK(!dosvm_bios(v,&f,0x13) && (f.tf.ecx&63)==63 && (f.tf.edx>>8)==15);
    f=disk_frame(0x4100); f.tf.ebx=0x55AA; CHECK(!dosvm_bios(v,&f,0x13) && (uint16_t)f.tf.ebx==0xAA55);
    f=disk_frame(0xE820); CHECK(!dosvm_bios(v,&f,0x15) && (f.tf.eflags&V86_CF));
    v->clock_ms=3661000; f=disk_frame(0x0200); CHECK(!dosvm_bios(v,&f,0x1A) && f.tf.ecx==0x0101 && f.tf.edx==0x0100);
    v->clock_ms=86400000; f=disk_frame(0x0400); CHECK(!dosvm_bios(v,&f,0x1A) && f.tf.ecx==0x2026 && f.tf.edx==0x1012);
    /* Exercise the actual guest INT -> ROM service -> IRET convention,
     * including carry propagation into the original saved FLAGS. */
    const uint8_t instruction[]={0xCD,0x13}; CHECK(!dosvm_write(v,0x2000,instruction,2));
    f=disk_frame(0x0201); f.tf.cs=0; f.tf.eip=0x2000; f.tf.user_ss=0; f.tf.user_esp=0x3000; f.tf.edx=0x81;
    v->monitor.state=V86_RUNNING; v->monitor.vif=true;
    CHECK(!v86_emulate(&v->monitor,&f) && f.tf.cs==0xF000 && f.tf.eip==0x130);
    CHECK(!v86_emulate(&v->monitor,&f) && f.tf.eip==0x132 && (f.tf.eflags&V86_CF));
    CHECK(!v86_emulate(&v->monitor,&f) && f.tf.cs==0 && f.tf.eip==0x2002 && f.tf.user_esp==0x3000 && v->monitor.vif && (f.tf.eflags&V86_CF));
    destroy(v,false); empty();
}
static int port_handler(struct dosvm *v, uint16_t port, unsigned width, bool write, uint32_t *value, void *arg)
{ CHECK(v && port==0x300 && width==4 && !write && arg==(void *)7); *value=0xBEEF; return 0; }
static struct dosvm *waiting;
static void enqueue_key(void) { CHECK(dosvm_keyboard(dosvm_key(waiting),0x1E61)); sleep_hook=0; }
static void test_lifecycle_records(void)
{
    struct dosvm *v=create(); struct dosvm_key expired=dosvm_key(v); CHECK(dosvm_boot(v)==0 && v->info.state==DOSVM_BOOTING && !(v->frame.tf.eflags&0x3000));
    g_ticks+=1000; line(v,"DOSREADY 05.00 free=8000 C\r"); CHECK(v->info.dos_ready && v->info.free_paragraphs==0x8000 && v->info.drive==2 && v->info.state==DOSVM_RUNNABLE);
    unsigned count=guest_records; line(v,"CIUKI_TEST event=END status=PASS"); CHECK(guest_records==count+1 && strstr(last_record,"group=guest") && !strstr(last_record,"status=PASS"));
    for(unsigned i=0;i<80;i++) CHECK(!dosvm_record_byte(v,'X')); CHECK(!dosvm_record_byte(v,10)); CHECK(strstr(last_record,"part=2 parts=2"));
    count=guest_records; for(unsigned i=0;i<81;i++) CHECK(!dosvm_record_byte(v,'X')); CHECK(!dosvm_record_byte(v,10)); CHECK(guest_records==count);
    CHECK(!dosvm_record_byte(v,1)); line(v,"invalid"); CHECK(guest_records==count);
    unsigned accepted=v->info.guest_records; for(unsigned i=0;i<20;i++) line(v,"bounded"); CHECK(v->info.guest_records==16 && v->info.dropped_records>=9 && accepted<16);
    g_ticks+=1000; line(v,"new-window"); CHECK(v->info.guest_records==17);
    struct v86_frame peek={.tf={.eax=0x100}};
    input_next=(struct input_event){.type=INPUT_TEXT,.code='q',.tick=v->input_since-1}; input_pending=true;
    CHECK(!dosvm_bios(v,&peek,0x16) && (peek.tf.eflags&V86_ZF) && !v->key_count && !input_pending);
    input_next.tick=g_ticks; input_pending=true; peek.tf.eax=0x100;
    CHECK(!dosvm_bios(v,&peek,0x16) && peek.tf.eax=='q' && v->key_count==1 && !(peek.tf.eflags&V86_ZF));
    peek.tf.eax=0; CHECK(!dosvm_bios(v,&peek,0x16) && peek.tf.eax=='q' && !v->key_count);
    uint32_t value=0; CHECK(v86_io(&v->monitor,0x1F0,4,false,&value)==0 && value==UINT32_MAX);
    CHECK(!dosvm_port_register(expired,0x300,0x300,port_handler,(void *)7)); CHECK(!v86_io(&v->monitor,0x300,4,false,&value) && value==0xBEEF);
    CHECK(dosvm_port_register(expired,0x300,0x301,port_handler,0)<0 && dosvm_port_register(expired,0x20,0x20,port_handler,0)<0); CHECK(!dosvm_port_unregister(expired,0x300));
    struct v86_frame f={0}; waiting=v; sleep_hook=enqueue_key; unsigned progress=native_progress;
    CHECK(!dosvm_bios(v,&f,0x16) && f.tf.eax==0x1E61 && native_progress>progress && v->info.state==DOSVM_RUNNABLE);
    CHECK(dosvm_keyboard(expired,0x3062)); f.tf.eax=0x100; CHECK(!dosvm_bios(v,&f,0x16) && f.tf.eax==0x3062 && v->key_count==1);
    lifecycle_stop[0]=lifecycle_cleanup[0]=0;
    unsigned pc_before=fault_pc_records, regs_before=fault_regs_records, stack_before=fault_stack_records;
    /* Hardware boots have no active probe, and a full verbose queue must
     * not consume the fault's protected register/first-stack publications. */
    bool was_active=evidence_active; uint32_t boot_flags=g_boot.flags;
    evidence_active=false; g_boot.flags=0;
    v->deferred_trace.count=ARRAY_SIZE(v->deferred_trace.line);
    v->frame.tf.eax=0x1234;
    v->frame.tf.user_ss=0x1000; v->frame.tf.user_esp=0x8000;
    const uint16_t stack_words[2]={0xCAFE,0xBEEF};
    CHECK(!dosvm_write(v,0x18000,stack_words,sizeof(stack_words)));
    run_worker(v); CHECK(v->info.state==DOSVM_STOPPING && v->info.reason==DOSVM_FAULT && v->info.fault_vector==13);
    CHECK(fault_pc_records==pc_before+1 && fault_regs_records==regs_before+1 && fault_stack_records==stack_before+1);
    CHECK(strstr(fault_pc_record,"depth=0 kind=0 flags=0") &&
          strstr(fault_regs_record,"group=vm_fault pm=0 ax=1234") &&
          strstr(fault_stack_record,"words=cafe,beef,"));
    evidence_active=was_active; g_boot.flags=boot_flags;
    CHECK(strstr(lifecycle_stop,"stage=stop vm=") && strstr(lifecycle_stop,"vector=13 eip="));
    destroy(v,true); empty();
    CHECK(strstr(lifecycle_cleanup,"stage=cleanup vm=") && strstr(lifecycle_cleanup,"result=0") && strstr(lifecycle_cleanup,"retained=0"));
    struct dosvm *next=create(); CHECK(dosvm_key(next).id==expired.id && dosvm_key(next).generation!=expired.generation && !dosvm_keyboard(expired,'X'));
    CHECK(!dosvm_boot(next)); value=1; CHECK(!v86_io(&next->monitor,0x92,1,true,&value) && next->info.reason==DOSVM_RESET); destroy(next,true); empty();
    /* OUT hands the handler the whole EAX: a dirty AH must not hide the command byte (QEMU 2026-10-11). */
    next=create(); CHECK(!dosvm_boot(next)); CHECK(!dosvm_set_a20(next,false)); value=0xFFD1; CHECK(!v86_io(&next->monitor,0x64,1,true,&value)); value=0xFF03; CHECK(!v86_io(&next->monitor,0x60,1,true,&value) && next->a20);
    value=0xFFD0; CHECK(!v86_io(&next->monitor,0x64,1,true,&value)); value=0; CHECK(!v86_io(&next->monitor,0x64,1,false,&value) && (value&1));
    value=0; CHECK(!v86_io(&next->monitor,0x60,1,false,&value) && (value&2)); value=0; CHECK(!v86_io(&next->monitor,0x64,1,false,&value) && !(value&1));
    value=0xFF00; CHECK(!v86_io(&next->monitor,DOSVM_CONTROL_PORT,1,true,&value) && next->info.state==DOSVM_STOPPING && next->info.reason==DOSVM_EXIT); destroy(next,true); empty();
    next=create(); CHECK(!dosvm_boot(next)); next->monitor.vif=false; next->boot_deadline=g_ticks+2;
    guest_halt=true; run_worker(next); guest_halt=false;
    CHECK(next->info.reason==DOSVM_TIMEOUT && next->info.state==DOSVM_STOPPING); destroy(next,true); empty();
}
static void test_cleanup_errors(void)
{
    struct dosvm *v=create(); uint8_t sector[512]={7}; CHECK(!dosvm_disk_write(v,5,sector)); unsigned committed=commits;
    fail_delete=true; CHECK(dosvm_destroy(dosvm_key(v),false)==-V86_EIO && v->info.state==DOSVM_STOPPING && !used_pages && !handles && scratch==1 && tables==1 && commits==committed+1);
    fail_delete=false; destroy(v,false); empty();
    v=create(); CHECK(!dosvm_disk_write(v,5,sector)); fail_commit=true; CHECK(dosvm_destroy(dosvm_key(v),false)==-V86_EIO && v->info.data_loss && v->info.state==DOSVM_DEAD); fail_commit=false; empty();
    v=create(); fail_close=true; CHECK(dosvm_destroy(dosvm_key(v),true)==-V86_EIO && v->info.state==DOSVM_DEAD); fail_close=false; empty();
    v=create(); CHECK(!dosvm_disk_write(v,5,sector)); committed=commits; destroy(v,true); CHECK(commits==committed && v->info.data_loss); empty();
}
static struct v86_frame xf(unsigned ah, unsigned dx)
{ return (struct v86_frame){.tf={.eax=ah<<8,.edx=dx,.eflags=V86_VM|V86_IF|2}}; }
static struct v86_frame ef(unsigned ax, unsigned dx, unsigned bx)
{ return (struct v86_frame){.tf={.eax=ax,.edx=dx,.ebx=bx,.eflags=V86_VM|V86_IF|2}}; }
#define AX(f) ((f).tf.eax&0xFFFFu)
#define BX(f) ((f).tf.ebx&0xFFFFu)
#define DX(f) ((f).tf.edx&0xFFFFu)
#define XMS(v,f) (!dosvm_xms((v),&(f)))
#define EMS(v,f) (!dosvm_ems((v),&(f)))
#define EMSERR(f) (((f).tf.eax>>8)&255u)
/* f3-03 memory services (lead, 2026-10-11): XMS/EMS/UMB state machines and the
 * move/resize validators against the fake VM memory; windows are page aliases. */
static void test_memory_services(void)
{
    struct dosvm *v; struct dosvm_config c={.commitment=DOSVM_LOW_CAP+4u*1024*1024};
    CHECK(dosvm_create(&c,&v)==0 && v && v->memory.quota==4u*1024*1024 && v->info.pool_quota==v->memory.quota && !v->info.pool_used);
    for (uint32_t off=0;off<v->memory.quota;off+=PAGE_SIZE) CHECK(as_lookup(&v->as,VM_POOL_BASE+off,0));
    for (unsigned i=0;i<16;i++) CHECK(!as_lookup(&v->as,0xE0000+i*PAGE_SIZE,0));
    CHECK(as_lookup(&v->as,0xC8000,0)==v->frames[0xC8] && v->memory.frame_segment==0xE000 && v->memory.umb_enabled);
    struct v86_frame f;
    f=xf(0,0); CHECK(XMS(v,f) && AX(f)==0x300 && BX(f)==0x100 && DX(f)==1);
    f=xf(1,0xFFFF); CHECK(XMS(v,f) && AX(f)==1 && v->memory.hma);
    f=xf(1,0xFFFF); CHECK(XMS(v,f) && !AX(f) && (f.tf.ebx&255)==0x91);
    f=xf(2,0); CHECK(XMS(v,f) && AX(f)==1 && !v->memory.hma);
    f=xf(2,0); CHECK(XMS(v,f) && (f.tf.ebx&255)==0x93);
    f=xf(3,0); CHECK(XMS(v,f) && AX(f)==1 && v->a20 && v->memory.global_a20==1);
    f=xf(5,0); CHECK(XMS(v,f) && AX(f)==1 && v->memory.local_a20==1);
    f=xf(4,0); CHECK(XMS(v,f) && (f.tf.ebx&255)==0x94 && v->a20 && !v->memory.global_a20);
    f=xf(6,0); CHECK(XMS(v,f) && AX(f)==1 && !v->a20 && !v->memory.local_a20);
    f=xf(7,0); CHECK(XMS(v,f) && !AX(f));
    f=xf(8,0); CHECK(XMS(v,f) && AX(f)==4096 && DX(f)==4096 && !(f.tf.ebx&255));
    f=xf(0x88,0); CHECK(XMS(v,f) && f.tf.eax==4096 && f.tf.edx==4096 && f.tf.ecx==VM_POOL_BASE+v->memory.quota-1);
    f=xf(9,4097); CHECK(XMS(v,f) && !AX(f) && (f.tf.ebx&255)==0xA0 && !DX(f));
    f=xf(9,64); CHECK(XMS(v,f) && AX(f)==1 && DX(f)==1); unsigned h1=1;
    CHECK(v->memory.xms[0].used && v->memory.xms[0].base==VM_POOL_BASE && v->memory.xms[0].bytes==65536 && v->memory.xms[0].pages==16 && v->info.pool_used==65536);
    f=xf(0xE,h1); CHECK(XMS(v,f) && AX(f)==1 && DX(f)==64 && BX(f)==63);
    f=xf(0xC,h1); CHECK(XMS(v,f) && AX(f)==1 && ((DX(f)<<16)|BX(f))==VM_POOL_BASE && v->memory.xms[0].locks==1);
    f=xf(0xA,h1); CHECK(XMS(v,f) && !AX(f) && (f.tf.ebx&255)==0xAB);
    f=xf(0xF,h1); f.tf.ebx=128; CHECK(XMS(v,f) && (f.tf.ebx&255)==0xAB);
    f=xf(0xD,h1); CHECK(XMS(v,f) && AX(f)==1 && !v->memory.xms[0].locks);
    f=xf(0xD,h1); CHECK(XMS(v,f) && (f.tf.ebx&255)==0xAA);
    f=xf(0xD,5); CHECK(XMS(v,f) && (f.tf.ebx&255)==0xA2);
    f=xf(9,64); CHECK(XMS(v,f) && AX(f)==1 && DX(f)==2 && v->memory.xms[1].base==VM_POOL_BASE+65536);
    uint8_t pattern[512], back[512]; for (unsigned i=0;i<sizeof pattern;i++) pattern[i]=(uint8_t)(i*7+3);
    CHECK(!dosvm_write(v,VM_POOL_BASE,pattern,sizeof pattern));
    /* Growth past an occupied neighbour relocates the block and keeps its bytes. */
    f=xf(0xF,h1); f.tf.ebx=128; CHECK(XMS(v,f) && AX(f)==1 && v->memory.xms[0].base==VM_POOL_BASE+131072 && v->memory.xms[0].pages==32 && v->memory.xms[0].bytes==131072);
    CHECK(!dosvm_read(v,v->memory.xms[0].base,back,sizeof back) && !memcmp(back,pattern,sizeof back) && v->info.pool_used==48*PAGE_SIZE);
    CHECK(!dosvm_read(v,VM_POOL_BASE,back,sizeof back) && !memcmp(back,pattern,sizeof back)); /* freed pages keep bytes until reuse; reuse zeroes */
    f=xf(0xF,h1); f.tf.ebx=32; CHECK(XMS(v,f) && AX(f)==1 && v->memory.xms[0].pages==8 && v->memory.xms[0].bytes==32768 && v->info.pool_used==24*PAGE_SIZE);
    f=xf(0x8F,h1); f.tf.ebx=0xFFFFFFFFu; CHECK(XMS(v,f) && (f.tf.ebx&255)==0xA0 && v->memory.xms[0].pages==8);
    f=xf(9,16); CHECK(XMS(v,f) && AX(f)==1 && DX(f)==3 && v->memory.xms[2].base==VM_POOL_BASE); /* first fit reuses the hole, zeroed */
    CHECK(!dosvm_read(v,VM_POOL_BASE,back,sizeof back) && back[0]==0 && back[511]==0);
    f=xf(0xA,3); CHECK(XMS(v,f) && AX(f)==1 && !v->memory.xms[2].used);
    /* Move validator: conventional -> EMB -> conventional, then each error code. */
    uint8_t move[16]={0}; CHECK(!dosvm_write(v,0x10000,pattern,sizeof pattern));
    vm_w32(move,512); vm_w16(move+4,0); vm_w32(move+6,0x10000000u); vm_w16(move+10,h1); vm_w32(move+12,0);
    CHECK(!dosvm_write(v,0x20000,move,sizeof move)); f=xf(0xB,0); f.ds=0x2000; CHECK(XMS(v,f) && AX(f)==1 && !v->a20);
    CHECK(!dosvm_read(v,v->memory.xms[0].base,back,sizeof back) && !memcmp(back,pattern,sizeof back));
    vm_w16(move+4,h1); vm_w32(move+6,0); vm_w16(move+10,0); vm_w32(move+12,0x30000000u);
    CHECK(!dosvm_write(v,0x20000,move,sizeof move)); f=xf(0xB,0); f.ds=0x2000; CHECK(XMS(v,f) && AX(f)==1);
    CHECK(!dosvm_read(v,0x30000,back,sizeof back) && !memcmp(back,pattern,sizeof back));
    vm_w32(move,511); CHECK(!dosvm_write(v,0x20000,move,sizeof move)); f=xf(0xB,0); f.ds=0x2000; CHECK(XMS(v,f) && (f.tf.ebx&255)==0xA7);
    vm_w32(move,512); vm_w16(move+4,9); CHECK(!dosvm_write(v,0x20000,move,sizeof move)); f=xf(0xB,0); f.ds=0x2000; CHECK(XMS(v,f) && (f.tf.ebx&255)==0xA3);
    vm_w16(move+4,h1); vm_w32(move+6,40000); CHECK(!dosvm_write(v,0x20000,move,sizeof move)); f=xf(0xB,0); f.ds=0x2000; CHECK(XMS(v,f) && (f.tf.ebx&255)==0xA4);
    vm_w32(move+6,32512); CHECK(!dosvm_write(v,0x20000,move,sizeof move)); f=xf(0xB,0); f.ds=0x2000; CHECK(XMS(v,f) && (f.tf.ebx&255)==0xA7);
    vm_w32(move+6,0); vm_w16(move+10,9); CHECK(!dosvm_write(v,0x20000,move,sizeof move)); f=xf(0xB,0); f.ds=0x2000; CHECK(XMS(v,f) && (f.tf.ebx&255)==0xA5);
    vm_w16(move+10,2); vm_w32(move+12,65100); CHECK(!dosvm_write(v,0x20000,move,sizeof move)); f=xf(0xB,0); f.ds=0x2000; CHECK(XMS(v,f) && (f.tf.ebx&255)==0xA7);
    vm_w32(move+12,70000); CHECK(!dosvm_write(v,0x20000,move,sizeof move)); f=xf(0xB,0); f.ds=0x2000; CHECK(XMS(v,f) && (f.tf.ebx&255)==0xA6);
    /* UMB arena: C900h..DFFFh in whole pages, exclusive of the E000h frame. */
    f=xf(0x10,0xFFFF); CHECK(XMS(v,f) && !AX(f) && (f.tf.ebx&255)==0xB0 && DX(f)==23*256);
    f=xf(0x10,17); CHECK(XMS(v,f) && AX(f)==1 && BX(f)==0xC900 && DX(f)==256 && v->memory.umb[0]==1);
    f=xf(0x12,0xC900); f.tf.ebx=512; CHECK(XMS(v,f) && AX(f)==1 && v->memory.umb[0]==2 && v->memory.umb[1]==0xFFFF);
    f=xf(0x10,23*256); CHECK(XMS(v,f) && (f.tf.ebx&255)==0xB0 && DX(f)==21*256);
    f=xf(0x11,0xC900); CHECK(XMS(v,f) && AX(f)==1 && !v->memory.umb[0] && !v->memory.umb[1]);
    f=xf(0x11,0xC900); CHECK(XMS(v,f) && (f.tf.ebx&255)==0xB2);
    f=xf(0x11,0xE000); CHECK(XMS(v,f) && (f.tf.ebx&255)==0xB2);
    f=xf(0x8A,0); CHECK(XMS(v,f) && !AX(f) && (f.tf.ebx&255)==0x80);
    CHECK(v->info.xms_calls==((1u<<0x13)-1u|1u<<19|1u<<22));
    /* EMS: handles, page-table windows, save/restore, realloc, release. */
    f=ef(0x4000,0,0); CHECK(EMS(v,f) && !EMSERR(f));
    f=ef(0x4100,0,0); CHECK(EMS(v,f) && !EMSERR(f) && BX(f)==0xE000);
    f=ef(0x4200,0,0); CHECK(EMS(v,f) && !EMSERR(f) && DX(f)==256 && BX(f)==250);
    f=ef(0x4300,0,0); CHECK(EMS(v,f) && EMSERR(f)==0x89);
    f=ef(0x4300,0,257); CHECK(EMS(v,f) && EMSERR(f)==0x87);
    f=ef(0x4300,0,251); CHECK(EMS(v,f) && EMSERR(f)==0x88);
    f=ef(0x4300,0,4); CHECK(EMS(v,f) && !EMSERR(f) && DX(f)==1 && v->memory.ems[0].block.used && v->memory.ems[0].block.base==VM_POOL_BASE && v->memory.ems_bytes==65536);
    uint32_t base=v->memory.ems[0].block.base;
    f=ef(0x4400,1,0); CHECK(EMS(v,f) && !EMSERR(f));
    for (unsigned j=0;j<4;j++) CHECK(as_lookup(&v->as,0xE0000+j*PAGE_SIZE,0)==as_lookup(&v->as,base+j*PAGE_SIZE,0));
    CHECK(!as_lookup(&v->as,0xE4000,0));
    CHECK(!dosvm_write(v,0xE0100,pattern,sizeof pattern) && !dosvm_read(v,base+0x100,back,sizeof back) && !memcmp(back,pattern,sizeof back));
    f=ef(0x4401,1,3); CHECK(EMS(v,f) && !EMSERR(f) && as_lookup(&v->as,0xE4000,0)==as_lookup(&v->as,base+3*VM_EMS_PAGE,0));
    f=ef(0x4400,1,5); CHECK(EMS(v,f) && EMSERR(f)==0x8A);
    f=ef(0x4404,1,0); CHECK(EMS(v,f) && EMSERR(f)==0x8B);
    f=ef(0x4400,7,0); CHECK(EMS(v,f) && EMSERR(f)==0x83);
    f=ef(0x4600,0,0); CHECK(EMS(v,f) && (f.tf.eax&255)==0x40);
    f=ef(0x4B00,0,0); CHECK(EMS(v,f) && BX(f)==1);
    f=ef(0x4C00,1,0); CHECK(EMS(v,f) && BX(f)==4);
    f=ef(0x4D00,0,0); f.es=0x4000; CHECK(EMS(v,f) && !EMSERR(f) && BX(f)==1 && !dosvm_read(v,0x40000,back,4) && back[0]==1 && !back[1] && back[2]==4 && !back[3]);
    f=ef(0x4700,1,0); CHECK(EMS(v,f) && !EMSERR(f) && v->memory.ems[0].saved);
    f=ef(0x4700,1,0); CHECK(EMS(v,f) && EMSERR(f)==0x8D);
    f=ef(0x4400,1,0xFFFF); CHECK(EMS(v,f) && !EMSERR(f) && !as_lookup(&v->as,0xE0000,0));
    f=ef(0x4800,1,0); CHECK(EMS(v,f) && !EMSERR(f) && !v->memory.ems[0].saved && as_lookup(&v->as,0xE0000,0)==as_lookup(&v->as,base,0));
    f=ef(0x4800,1,0); CHECK(EMS(v,f) && EMSERR(f)==0x8E);
    f=ef(0x5100,1,8); CHECK(EMS(v,f) && !EMSERR(f) && v->memory.ems[0].block.pages==32 && v->memory.ems[0].block.base==VM_POOL_BASE+40*PAGE_SIZE && v->memory.ems_bytes==131072);
    base=v->memory.ems[0].block.base;
    CHECK(as_lookup(&v->as,0xE0000,0)==as_lookup(&v->as,base,0) && as_lookup(&v->as,0xE4000,0)==as_lookup(&v->as,base+3*VM_EMS_PAGE,0));
    CHECK(!dosvm_read(v,base+0x100,back,sizeof back) && !memcmp(back,pattern,sizeof back));
    f=ef(0x5100,1,2); CHECK(EMS(v,f) && !EMSERR(f) && v->memory.ems[0].block.pages==8 && v->memory.ems_bytes==32768);
    CHECK(as_lookup(&v->as,0xE0000,0)==as_lookup(&v->as,base,0) && !as_lookup(&v->as,0xE4000,0) && !v->memory.map[1].handle);
    f=ef(0x5100,1,257); CHECK(EMS(v,f) && EMSERR(f)==0x87);
    f=ef(0x4500,1,0); CHECK(EMS(v,f) && !EMSERR(f) && !v->memory.ems[0].block.used && !v->memory.ems_bytes && !as_lookup(&v->as,0xE0000,0) && v->info.pool_used==24*PAGE_SIZE);
    f=ef(0x4500,1,0); CHECK(EMS(v,f) && EMSERR(f)==0x83);
    f=ef(0x4900,0,0); CHECK(EMS(v,f) && EMSERR(f)==0x84);
    f=ef(0x5900,0,0); CHECK(EMS(v,f) && EMSERR(f)==0xA4);
    f=ef(0x5E00,0,0); CHECK(EMS(v,f) && EMSERR(f)==0x84);
    CHECK((v->info.ems_calls&(1u<<0|1u<<1|1u<<2|1u<<3|1u<<4|1u<<5|1u<<6|1u<<7|1u<<8|1u<<0x11))==(1u<<0|1u<<1|1u<<2|1u<<3|1u<<4|1u<<5|1u<<6|1u<<7|1u<<8|1u<<0x11));
    destroy(v,true); empty();
    /* Aliases still mapped at destruction: every page freed exactly once. */
    CHECK(dosvm_create(&c,&v)==0 && v);
    f=ef(0x4300,0,2); CHECK(EMS(v,f) && !EMSERR(f) && DX(f)==1);
    f=ef(0x4400,1,1); CHECK(EMS(v,f) && !EMSERR(f) && as_lookup(&v->as,0xE0000,0));
    f=xf(9,32); CHECK(XMS(v,f) && AX(f)==1);
    destroy(v,true); empty();
    /* Pool backing failure during creation unwinds the whole VM. */
    allocation_calls=0; struct dosvm *t=create(); unsigned n=allocation_calls; destroy(t,true); empty();
    allocation_calls=0; fail_at=n+50; v=(void *)1; CHECK(dosvm_create(0,&v)==-ENOMEM && !v); fail_at=0; empty();
    printf("DOS memory services: XMS/EMS/UMB state machines and move/resize validators PASS\n");
}
#include "dosvm_devices_test.inc"
#include "dosvm_video_test.inc"
#include "dosvm_audio_test.inc"
#include "dosvm_dpmi_test.inc"
#include "dosvm_timer_test.inc"
#include "dosvm_vmaudio_test.inc"  /* f4-05: VM sources in the mixer (separate units) */
#include "dosvm_game_audio_test.inc" /* f4-13: physical cursor and game protocols */
#include "dosvm_workloads_test.inc"
#include "dosvm_menu_test.inc"
#include "dosvm_irq_stall_test.inc"
#include "dosvm_isr_test.inc"
#include "dosvm_speed_test.inc"
#include "dosvm_cost_test.inc"
#include "dosvm_exit_test.inc"
#include "dosvm_idle_test.inc"
#include "dosvm_fullscreen_test.inc"
#include "dosvm_quake_start_test.inc"
#include "dosvm_startup_test.inc"
#include "dosvm_creation_test.inc"
int main(void)
{
    ram=calloc(HOST_PAGES,PAGE_SIZE); CHECK(ram);
    if (getenv("CIUKI_VM_AUDIO_COST")) { test_vm_source_budget(); free(ram); return 0; }
    if (getenv("CIUKI_VM_QUAKE_START")) { test_quake_start(); free(ram); return 0; }
    if (getenv("CIUKI_VM_BOOT_START")) { test_startup_boot(); free(ram); return 0; }
    if (getenv("CIUKI_VM_EXIT_TEST")) { test_exit_cost(); free(ram); return 0; }
    test_maximum_creation();
    test_admission(); test_creation_failures(); test_memory_disk(); test_bios_bounds(); test_lifecycle_records(); test_cleanup_errors(); test_memory_services(); test_devices(); test_audio(); test_dpmi();
    test_video();
    test_vm_audio();
    test_game_audio();
    test_workloads();
    test_timer();
    test_menu();
    test_irq_stall();
    test_isr_recovery_boundary(); test_isr_nested_hardware();
    test_isr_flat_private_stack();
    test_isr_return_evidence(); test_isr_expiry(); test_if_cli_epoch(); test_if_handler_scope();
    test_speed();
    test_cost_bursts();
    test_exit_cost();
    test_idle();
    test_fullscreen_transport();
    test_fullscreen_service();
    test_fullscreen_more_modes();
    test_fullscreen_vbe();
    test_startup();
    test_quake_start();
    CHECK(guest_enters==4 && deletes>1); /* includes f3-40 worker IRQ reentry */
    test_startup_boot();
    free(ram);
    printf("dosvm host tests: PASS (%u checks, max record %u bytes)\n",checks,max_record); return 0;
}
