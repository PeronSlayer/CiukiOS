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
struct process {
    uint32_t pid, ppid, pgid, slot;
    enum proc_state state;
    int32_t status, raw_exit;
    uint32_t fault_vector, fault_error, fault_eip, fault_address;
    struct uaddr *memory;
    struct proc_fd *fds;
    struct ciuki_sigaction *actions; /* index signo; signal semantics in f2-04 */
    void *cwd;
    void (*cwd_retain)(void *cwd);
    void (*cwd_release)(void *cwd);
    uint64_t pending, cpu_ticks;
    struct kwait changed;
    uint32_t threads, live_threads;
    bool published;
};
struct proc_thread {
    uint32_t tid, slot, value, tls, stack_base, stack_bytes;
    struct process *process;
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
void proc_thread_collect(struct proc_thread *thread);
void proc_thread_cleanup(struct proc_thread *thread);
int proc_thread_create(uint32_t args);
__attribute__((noreturn)) void proc_thread_exit(uint32_t value);
int proc_thread_join(uint32_t tid, uint32_t value);
int proc_thread_detach(uint32_t tid);
int proc_tls_set(uint32_t base, uint32_t bytes);
int32_t proc_syscall(struct trap_frame *tf);
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
