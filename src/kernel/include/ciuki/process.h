/* Native process ownership, separate from F0 scheduler tasks.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_PROCESS_H
#define CIUKI_PROCESS_H
#include <ciuki/elf.h>
#include <ciuki/waitword.h>
#include <ciuki/task.h>
#include <ciuki/sync.h>

/* The later descriptor implementation supplies retain/release; grants set
 * inheritable=false. Slots share descriptions, never consume parent refs. */
struct proc_object {
    void (*retain)(struct proc_object *object);
    void (*release)(struct proc_object *object);
    bool inheritable;
};
struct proc_fd { struct proc_object *object; uint32_t flags; };
enum proc_state { PROC_PREPARING, PROC_LIVE, PROC_STOPPING, PROC_ZOMBIE };
struct proc_launch {
    uint64_t click_us, spawn_us, elf_us, surface_us, configure_us, damage_us, frame_us;
    uint64_t volume_bytes, volume_reads, disk_bytes, disk_reads, image_bytes, image_reads;
    uint64_t dma_bytes, dma_reads, dma_bounces, dma_ms;
    uint32_t dma_max;
    uint32_t volume_max, disk_max, image_max, page_faults;
    bool active, configured, damaged, finished;
    bool quiet; /* fx-launch: quiet boot, spawn stage only (one record at elf) */
};
enum proc_personality { PROC_NATIVE, PROC_WIN32 };
/* Pure translations, not permission to elevate. SCHED_OTHER is policy 0;
 * unsupported realtime policies/classes remain rejected by public APIs. */
int proc_priority_posix(int policy, int priority, int nice_value);
int proc_priority_win32(uint32_t priority_class, int relative, bool foreground);
struct security_subject;
struct w32_delivery;
struct w32_process;
struct w32_thread;
struct process {
    uint32_t pid, ppid, pgid, slot;
    struct security_subject *subject; /* immutable; assigned before publication */
    enum proc_personality personality;
    struct w32_process *w32;
    uint32_t w32_image, w32_peb, w32_dispatcher, w32_stack_reserve, w32_stack_commit;
    uint32_t w32_source, w32_source_bytes;
    char w32_source_name[778]; /* MAX_PATH ACP characters encoded as UTF-8 */
    enum proc_state state;
    int32_t status, raw_exit;
    uint32_t fault_vector, fault_error, fault_eip, fault_address;
    struct uaddr *memory;
    struct proc_fd *fds;
    uint32_t fd_generation[CIUKI_OPEN_MAX]; /* poll/socket close-reuse identity */
    struct ciuki_sigaction *actions; /* index signo; signal semantics in f2-04 */
    void *cwd;
    void (*cwd_retain)(void *cwd);
    void (*cwd_release)(void *cwd);
    uint64_t pending, cpu_ticks;
    struct kwait changed;
    uint32_t threads, live_threads;
    bool published, dosvm_capability, programs_capability;
    /* f2-100: supervisor/desktop-authorized desktop and Control Panel only;
     * network configuration authority is delegated explicitly at spawn. */
    bool network_capability;
    bool dhcp_capability; /* G26B: authorized Terminal RELEASE/RENEW only. */
    /* G1: authority to set the clock and the system time zone (the
     * SE_SYSTEMTIME_NAME equivalent): desktop and Control Panel only,
     * delegated like network_capability, never inherited. */
    bool time_capability;
    /* G14: authority for system call 99's SET, ENABLE and SCANDISK (the
     * startup configuration and the disk check): the desktop, Control Panel,
     * Terminal, ScanDisk and System Configuration it starts; never
     * inherited or path-derived from any other launcher. */
    bool recovery_capability;
    /* f9-01 D5 / f9-02: destination-disk authority for /dev/ata0..3
     * (mutating requests and the lease). Set only by the supervisor's
     * Setup-mode spawn of /Programs/setup, before publication; zero in every
     * other process (kzalloc), never inherited by children or descriptors,
     * gone with the process. */
    bool setup_capability;
    /* f1-98: the desktop and its Win32 window server keep a minimum CPU
     * share (task.c share_group); every thread inherits it at creation. */
    bool share_group;
    /* f2-33 enumeration: image basename (printable, NUL-terminated) and the
     * g_ticks value at publication. */
    char name[CIUKI_PROC_NAME_MAX];
    uint64_t start_tick;
    struct proc_launch launch;
    uint64_t launch_click_us;
    bool launch_pending;
};
struct proc_thread {
    uint32_t tid, slot, value, tls, stack_base, stack_bytes;
    uint32_t teb, emergency, stack_limit, stack_guard, w32_reserve;
    struct w32_delivery *w32_delivery;
    struct w32_thread *w32;
    struct process *process;
    struct process *launch_loading; /* provisional child for this thread's spawn I/O */
    struct task *task;
    uint64_t mask, pending;
    struct ua_pin tls_pin;
    struct ww_waiter word_wait;
    struct proc_thread *joiner;
    /* A stopped kernel invocation may own provisional resources. The reaper
     * invokes this once, before closing process objects or freeing stacks. */
    void (*cleanup)(void *operation);
    void *operation;
    bool detached, exiting, stopped, retained, in_handler, interrupted, in_syscall;
};
struct proc_ledger { uint32_t processes, threads, zombies, handles, extents, backing, tables; };

void proc_init(void);
struct process *proc_supervisor(void);
struct process *proc_find(uint32_t pid);
/* Retained published slot; no waits between lookup and use. */
struct process *proc_at_slot(unsigned slot);
/* Security teardown includes unpublished reservations; never exposed to users. */
struct process *proc_reserved_at_slot(unsigned slot);
struct proc_thread *proc_thread_for(const struct task *task);
struct proc_thread *proc_thread_find(struct process *p, uint32_t tid);
struct proc_thread *proc_thread_slot(unsigned slot);
struct process *proc_current(void);
void proc_snapshot(struct proc_ledger *out);
int proc_prepare(struct process *parent, struct process **out);
void proc_discard(struct process *p);
int proc_inherit(struct process *child, const struct process *parent,
                 const struct ciuki_spawn_fd *list, uint32_t count);
int proc_fd_validate(const struct process *parent, const struct ciuki_spawn_fd *list, uint32_t count);
void proc_publish(struct process *p, uint32_t flags);
void proc_stop(struct process *p, int raw_code, uint32_t signal);
void proc_collect(void); /* supervisor only, after switching away */
int proc_wait_select(struct process *parent, int32_t pid, struct process **out);
int proc_reap(struct process *parent, struct process *child);
int proc_waitpid(int32_t pid, uint32_t status, uint32_t options);
/* Called by task.c, preserving the frozen F0 branch when no sidecar exists. */
bool proc_task_exiting(struct task *task, int code);
void proc_task_switch(struct task *next);
void proc_notify(void);
void proc_interrupt(struct proc_thread *thread);
void proc_set_file_ops(const struct ciuki_file_ops *ops);
const struct ciuki_file_ops *proc_get_file_ops(void);
int proc_spawn_file(struct process *parent, const struct ciuki_file *file,
                    const struct proc_strings *strings, const struct ciuki_spawn_fd *fds,
                    uint32_t fd_count, uint32_t flags, uint64_t mask, struct process **out);
int proc_spawn(uint32_t args);
int proc_thread_prepare(struct process *p, uint32_t entry, uint32_t argument,
                        uint32_t trampoline, uint32_t stack_bytes, uint32_t flags,
                        uint64_t mask, bool main, struct proc_thread **out);
void proc_thread_discard(struct proc_thread *thread);
/* f1-98: mark p and its existing threads as members of the desktop's
 * minimum-share group; later threads inherit it in task_create_native. */
void proc_share_group(struct process *p);
void proc_thread_collect(struct proc_thread *thread);
void proc_thread_cleanup(struct proc_thread *thread);
int proc_thread_create(uint32_t args);
__attribute__((noreturn)) void proc_thread_exit(uint32_t value);
int proc_thread_join(uint32_t tid, uint32_t value);
int proc_thread_detach(uint32_t tid);
int proc_tls_set(uint32_t base, uint32_t bytes);
int32_t proc_syscall(struct trap_frame *tf);
uint64_t proc_launch_now(void);
bool proc_launch_enabled(void);
void proc_launch_start(struct process *parent, struct process *child, uint64_t spawn_us, uint64_t click_us);
void proc_launch_elf(struct process *child);
void proc_launch_volume_read(uint32_t requested, size_t done);
void proc_launch_disk_read(uint32_t bytes);
void proc_launch_dma_read(uint32_t bytes, uint64_t ms, bool bounce);
void proc_launch_image_read(uint32_t bytes);
void proc_launch_surface(struct process *p);
int proc_launch_mark(struct process *caller, uint32_t event, uint32_t pid, uint64_t click_us);
/* f2-33: enumeration and the delegated termination authority. */
void proc_set_name(struct process *p, const char *path);
unsigned proc_list_snapshot(struct ciuki_proc_record *out, unsigned capacity, unsigned *total);
int proc_list(uint32_t buffer, uint32_t bytes);
int proc_terminate(struct process *caller, int32_t pid, uint32_t signo, int32_t authority_fd);
int proc_authority_install(struct process *p);
bool proc_authority_object(const struct proc_object *o);
/* Native scheduler frames share the process page directory; task.as is a
 * borrowed view for the unchanged F0 debug/probe copy paths. */
struct task *task_create_native(struct process *p, uint32_t entry, uint32_t esp);
void task_native_frame(struct task *task, uint32_t esp);
void arch_tls_switch(uint32_t base, bool native);

/* The runner directive supplies selector dispatch and linker start/end
 * symbols. Keep registration independent of probe implementation order. */
struct ciuki_f2_probe { const char *name; int (*run)(void); };
#define CIUKI_F2_PROBE(name, fn) \
    static const struct ciuki_f2_probe f2_registration_##fn \
    __attribute__((used, section(".f2probes"), aligned(4))) = { name, fn }
int probe_f2_elf_load(void);
int probe_f2_spawn_wait(void);
int probe_f2_mmap(void);
int probe_f2_threads_wait(void);
#endif
