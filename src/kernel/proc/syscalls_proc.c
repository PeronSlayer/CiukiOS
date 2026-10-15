/* Native syscall rows owned by f2-02. Wire records/constants come from abi.h.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/process.h>
#include <ciuki/signal.h>
uint32_t supervisor_desktop_pid(void); /* proc/supervisor.c */
/* Standalone process harnesses have no console; they may supply these sinks. */
#pragma weak console_quiet
#pragma weak klog

uint64_t proc_launch_now(void)
{
    uint32_t flags = irq_save();
    uint64_t us = g_ticks * 1000;
    irq_restore(flags);
    return us;
}
bool proc_launch_enabled(void) { return console_quiet && !console_quiet(); }

void proc_launch_start(struct process *parent, struct process *child, uint64_t spawn_us, uint64_t click_us)
{
    if (!proc_launch_enabled()) return;
    child->launch.active = true;
    child->launch.spawn_us = spawn_us;
    child->launch.click_us = click_us;
    struct proc_thread *self = proc_thread_for(g_current);
    if (self && self->process == parent) self->launch_loading = child;
}
void proc_launch_elf(struct process *p)
{
    if (!p->launch.active || !proc_launch_enabled()) return;
    p->launch.elf_us = proc_launch_now();
    if (klog) klog("[launch-stage] name=%s pid=%u spawn_us=%llu elf_us=%llu read_bytes=%llu disk_bytes=%llu",
                  p->name, p->pid, p->launch.spawn_us, p->launch.elf_us,
                  p->launch.volume_bytes, p->launch.disk_bytes);
}
static struct proc_launch *launch_io(void)
{
    if (!proc_launch_enabled()) return 0;
    struct process *p = proc_current();
    struct proc_thread *self = proc_thread_for(g_current);
    if (self && self->launch_loading) p = self->launch_loading;
    return p && p->launch.active && !p->launch.finished ? &p->launch : 0;
}
void proc_launch_volume_read(uint32_t requested, size_t done)
{
    struct proc_launch *l = launch_io();
    if (!l) return;
    l->volume_reads++; l->volume_bytes += done;
    if (requested > l->volume_max) l->volume_max = requested;
}
void proc_launch_disk_read(uint32_t bytes)
{
    struct proc_launch *l = launch_io();
    if (!l) return;
    l->disk_reads++; l->disk_bytes += bytes;
    if (bytes > l->disk_max) l->disk_max = bytes;
}
void proc_launch_dma_read(uint32_t bytes, uint64_t ms, bool bounce)
{
    struct proc_launch *l = launch_io();
    if (!l) return;
    l->dma_reads++; l->dma_bytes += bytes; l->dma_ms += ms;
    if (bounce) l->dma_bounces++;
    if (bytes > l->dma_max) l->dma_max = bytes;
}
void proc_launch_image_read(uint32_t bytes)
{
    struct proc_launch *l = launch_io();
    if (!l) return;
    l->image_reads++; l->image_bytes += bytes;
    if (bytes > l->image_max) l->image_max = bytes;
}
void proc_launch_surface(struct process *p)
{
    if (p && p->launch.active && !p->launch.surface_us && proc_launch_enabled())
        p->launch.surface_us = proc_launch_now();
}
int proc_launch_mark(struct process *caller, uint32_t event, uint32_t pid, uint64_t click_us)
{
    if (event > CIUKI_LAUNCH_FRAME || (event != CIUKI_LAUNCH_BEGIN && click_us) ||
        (event <= CIUKI_LAUNCH_BEGIN && pid)) return -EINVAL;
    if (event == CIUKI_LAUNCH_STATUS) return proc_launch_enabled();
    if (!caller || caller->pid != supervisor_desktop_pid()) return -EPERM;
    uint64_t now = proc_launch_now();
    if (event == CIUKI_LAUNCH_BEGIN && click_us > now) return -EINVAL;
    if (!proc_launch_enabled()) return 0;
    if (event == CIUKI_LAUNCH_BEGIN) {
        caller->launch_click_us = click_us; caller->launch_pending = true;
        return 0;
    }
    struct process *p = proc_find(pid);
    if (!p || p->ppid != caller->pid) return -ESRCH;
    struct proc_launch *l = &p->launch;
    if (!l->active || l->finished) return 0;
    if (event == CIUKI_LAUNCH_CONFIGURE) {
        if (!l->configured) { l->configure_us = now; l->configured = true; }
    } else if (event == CIUKI_LAUNCH_DAMAGE) {
        if (!l->configured) return -EINVAL;
        if (!l->damaged) { l->damage_us = now; l->damaged = true; }
    } else {
        if (!l->damaged) return -EINVAL;
        l->frame_us = now; l->finished = true;
        if (klog) {
            klog("[launch] name=%s pid=%u click_us=%llu spawn_us=%llu elf_us=%llu first_frame_us=%llu total_us=%llu resolution_us=1000",
                 p->name, p->pid, l->click_us, l->spawn_us, l->elf_us, l->frame_us, l->frame_us-l->click_us);
            klog("[launch-detail] pid=%u surface_us=%llu configure_us=%llu damage_us=%llu page_faults=%u",
                 p->pid, l->surface_us, l->configure_us, l->damage_us, l->page_faults);
            klog("[launch-io] pid=%u read_bytes=%llu read_requests=%llu read_max=%u disk_bytes=%llu disk_requests=%llu disk_max=%u",
                 p->pid, l->volume_bytes, l->volume_reads, l->volume_max, l->disk_bytes, l->disk_reads, l->disk_max);
            /* Separate bounded record: the console/serial sink has 256 bytes. */
            klog("[launch-io] pid=%u dma_bytes=%llu dma_requests=%llu dma_max=%u dma_bounces=%llu dma_ms=%llu",
                 p->pid, l->dma_bytes, l->dma_reads, l->dma_max, l->dma_bounces, l->dma_ms);
            klog("[launch-image] pid=%u image_bytes=%llu image_requests=%llu image_max=%u",
                 p->pid,
                 l->image_bytes, l->image_reads, l->image_max);
        }
    }
    return 0;
}

extern char copy_user_fault_start[], copy_user_fault_end[], copy_user_fixup[];
static const struct { const char *start, *end, *fixup; } copy_fixups[] = {
    { copy_user_fault_start, copy_user_fault_end, copy_user_fixup }
};

void proc_trap_dispatch(struct trap_frame *tf)
{
    if (tf->vector == 14) {
        struct process *p = proc_current();
        if (p && p->launch.active && !p->launch.finished && proc_launch_enabled()) p->launch.page_faults++;
    }
    /* The complete frame has already been saved by isr_common. Match only a
     * kernel #PF at the copy instruction; every other fault follows F0's
     * unchanged expected-fault/panic/user-fault dispatcher. */
    if (tf->vector == 14 && !(tf->cs & 3))
        for (unsigned i = 0; i < ARRAY_SIZE(copy_fixups); i++)
            if (tf->eip >= (uint32_t)(uintptr_t)copy_fixups[i].start &&
                tf->eip < (uint32_t)(uintptr_t)copy_fixups[i].end) {
                tf->eip = (uint32_t)(uintptr_t)copy_fixups[i].fixup;
                return;
            }
    struct proc_thread *thread = tf->vector == 0x80 && (tf->cs & 3) == 3 ? proc_thread_for(g_current) : 0;
    if (thread)
        thread->in_syscall = true;
    trap_dispatch(tf);
    if (thread) {
        thread->in_syscall = false;
        if (thread->process->state == PROC_STOPPING)
            task_exit(thread->process->raw_exit);
    }
}

/* f2-33 termination authority. PID 1 installs one object in the desktop it
 * bootstraps (CIUKI_TASKS_FD); the desktop may name it in an explicit spawn
 * list for the Task Manager. Default inheritance covers only fds 0-2, so it
 * never travels implicitly. It is honoured only for its owner or the owner's
 * direct children, so a further hand-off grants nothing. */
struct proc_authority { struct proc_object object; uint32_t refs, owner; };
static void authority_retain(struct proc_object *o) { ((struct proc_authority *)o)->refs++; }
static void authority_release(struct proc_object *o)
{
    struct proc_authority *a = (struct proc_authority *)o;
    if (!--a->refs)
        kfree(a);
}
bool proc_authority_object(const struct proc_object *o) { return o && o->release == authority_release; }

int proc_authority_install(struct process *p)
{
    if (!p || p == proc_supervisor() || !p->fds)
        return -EPERM;
    int fd = -1;
    for (int i = 3; i < CIUKI_OPEN_MAX && fd < 0; i++)
        if (!p->fds[i].object)
            fd = i;
    if (fd < 0)
        return -EMFILE;
    struct proc_authority *a = kzalloc(sizeof(*a));
    if (!a)
        return -ENOMEM;
    a->object = (struct proc_object){ authority_retain, authority_release, true };
    a->refs = 1;
    a->owner = p->pid;
    p->fds[fd] = (struct proc_fd){ &a->object, 0 };
    return fd;
}

/* Call 70: N-class. The whole caller range is validated before the snapshot;
 * only the system record and the written process records are stored. The
 * kernel heap stops at 2 KiB objects, so the 5184-byte snapshot lives in one
 * static buffer: nothing between snapshot and copy-out waits, and kernel code
 * is never preempted (execution-abi.md, Locking), so no two calls share it. */
static struct {
    struct ciuki_proc_system system;
    struct ciuki_proc_record records[CIUKI_PROCESS_MAX];
} proc_list_buffer;
_Static_assert(sizeof(proc_list_buffer) == CIUKI_PROC_LIST_MIN + CIUKI_PROCESS_MAX * sizeof(struct ciuki_proc_record),
               "proc_list snapshot layout");
int proc_list(uint32_t buffer, uint32_t bytes)
{
    struct process *p = proc_current();
    if (bytes < CIUKI_PROC_LIST_MIN || bytes > CIUKI_PROC_LIST_MAX)
        return -EINVAL;
    if (!p || buffer > UINT32_MAX - bytes || !ua_range(p->memory, buffer, bytes, PROT_READ | PROT_WRITE))
        return -EFAULT;
    uint32_t capacity = (bytes - sizeof(struct ciuki_proc_system)) / sizeof(struct ciuki_proc_record);
    if (capacity > CIUKI_PROCESS_MAX)
        capacity = CIUKI_PROCESS_MAX;
    struct ciuki_proc_system *sys = &proc_list_buffer.system;
    struct kheap_ledger heap;
    kheap_snapshot(&heap);
    unsigned total = 0;
    unsigned records = proc_list_snapshot(proc_list_buffer.records, capacity, &total);
    uint32_t flags = irq_save();
    *sys = (struct ciuki_proc_system){ .version = CIUKI_PROC_VERSION, .size = sizeof(*sys),
        .tick_hz = 1000, .records = records, .total_ticks = g_ticks, .idle_ticks = g_idle_ticks,
        .usable_pages = pmm_total_usable(), .free_pages = pmm_free_count(),
        .kheap_pages = (uint32_t)heap.pages, .kheap_bytes = (uint32_t)heap.in_use,
        .kheap_high_water = (uint32_t)heap.peak, .processes = total };
    irq_restore(flags);
    int err = copy_to_user(buffer, &proc_list_buffer, sizeof(*sys) + records * sizeof(struct ciuki_proc_record));
    return err ? err : (int)records;
}

/* Call 71: N-class termination request. Order: arguments (EINVAL), the
 * optional authority descriptor (EBADF), then the target (EPERM/ESRCH). */
int proc_terminate(struct process *caller, int32_t pid, uint32_t signo, int32_t authority_fd)
{
    if ((signo != SIGTERM && signo != SIGKILL) || pid <= 0 || (uint32_t)pid == caller->pid)
        return -EINVAL;
    bool authority = false;
    uint32_t owner_pid = 0;
    if (authority_fd != (int32_t)CIUKI_PROC_NO_AUTHORITY) {
        if (authority_fd < 0 || authority_fd >= CIUKI_OPEN_MAX || !caller->fds ||
            !proc_authority_object(caller->fds[authority_fd].object))
            return -EBADF;
        const struct proc_authority *a = (const struct proc_authority *)caller->fds[authority_fd].object;
        owner_pid = a->owner;
        authority = caller->pid == owner_pid || caller->ppid == owner_pid;
    }
    if (pid == 1)
        return -EPERM;
    struct process *target = proc_find((uint32_t)pid);
    if (!target || target->state != PROC_LIVE)
        return -ESRCH;
    /* The desktop's group (the desktop and the programs it launched) is ended
     * only through the desktop's close path, never directly by another caller. */
    uint32_t desktop_pid = supervisor_desktop_pid();
    struct process *desktop = desktop_pid ? proc_find(desktop_pid) : 0;
    if (desktop && target->pgid == desktop->pgid && caller != desktop)
        return -EPERM;
    /* The same holds for the authority's owner, whatever PID 1 recorded: a
     * delegate never ends the process that delegated to it, nor its group. */
    struct process *owner = owner_pid ? proc_find(owner_pid) : 0;
    if (owner && target->pgid == owner->pgid && caller != owner)
        return -EPERM;
    if (!target->live_threads)
        return -EPERM;                  /* kernel controller owner, no user thread */
    bool descendant = false;
    struct process *at = target;
    for (unsigned depth = 0; at && depth < CIUKI_PROCESS_MAX && !descendant; depth++) {
        descendant = at->ppid == caller->pid;
        at = at->ppid > 1 ? proc_find(at->ppid) : 0;
    }
    if (!descendant && !authority)
        return -EPERM;
    proc_signal_post(target, 0, signo, caller->pid);
    return 0;
}

int32_t proc_syscall(struct trap_frame *tf)
{
    struct process *p = proc_current();
    if (!p)
        return -ENOSYS;                 /* F0 tasks retain their frozen interface */
    if (p->personality == PROC_WIN32 &&
        (tf->eax == CIUKI_SYS_MMAP || tf->eax == CIUKI_SYS_BRK ||
         tf->eax == CIUKI_SYS_MUNMAP || tf->eax == CIUKI_SYS_MPROTECT ||
         tf->eax == CIUKI_SYS_TLS_SET || tf->eax == CIUKI_SYS_THREAD_CREATE)) return -EPERM;
    switch (tf->eax) {
    case CIUKI_SYS_LAUNCH_MARK:
        if (tf->edi || tf->ebp) return -EINVAL;
        return proc_launch_mark(p, tf->ebx, tf->ecx, (uint64_t)tf->edx | (uint64_t)tf->esi << 32);
    case CIUKI_SYS_SPAWN: return proc_spawn(tf->ebx);
    case CIUKI_SYS_WAITPID: return proc_waitpid((int32_t)tf->ebx, tf->ecx, tf->edx);
    case CIUKI_SYS_GETPID: return (int32_t)p->pid;
    case CIUKI_SYS_GETPPID: return (int32_t)p->ppid;
    case CIUKI_SYS_THREAD_CREATE: return proc_thread_create(tf->ebx);
    case CIUKI_SYS_THREAD_EXIT: proc_thread_exit(tf->ebx);
    case CIUKI_SYS_TLS_SET: return proc_tls_set(tf->ebx, tf->ecx);
    case CIUKI_SYS_WAIT_WORD: return proc_wait_word(tf->ebx, tf->ecx, tf->edx, tf->esi);
    case CIUKI_SYS_WAKE_WORD: return proc_wake_word(tf->ebx, tf->ecx);
    case CIUKI_SYS_MMAP: {
        struct ciuki_mmap_args a;
        int err = copy_from_user(&a, tf->ebx, sizeof(a));
        return err ? err : ua_mmap(p->memory, &a);
    }
    case CIUKI_SYS_MUNMAP: return ua_munmap(p->memory, tf->ebx, tf->ecx);
    case CIUKI_SYS_MPROTECT: return ua_mprotect(p->memory, tf->ebx, tf->ecx, tf->edx);
    case CIUKI_SYS_BRK: return ua_brk(p->memory, tf->ebx);
    case CIUKI_SYS_THREAD_JOIN: return proc_thread_join(tf->ebx, tf->ecx);
    case CIUKI_SYS_THREAD_DETACH: return proc_thread_detach(tf->ebx);
    case CIUKI_SYS_PROC_LIST: return proc_list(tf->ebx, tf->ecx);
    case CIUKI_SYS_PROC_TERMINATE: return proc_terminate(p, (int32_t)tf->ebx, tf->ecx, (int32_t)tf->edx);
    default: return -ENOSYS;
    }
}
