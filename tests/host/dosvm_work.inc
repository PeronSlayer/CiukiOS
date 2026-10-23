/* Production F0-46 ring, with only its task/CPU transport replaced, and the
 * production fx-9 VM clock service (vm/dosvm_clock.c) with only its task
 * transport replaced: the PIT top half's O(1) signal is counted and
 * host_clock_tick runs the service the vm-clock task runs after it. Included
 * by both DOS VM host harnesses before the VM implementation. */
#include <ciuki/work.h>
static struct task vm_clock_worker;
static unsigned vm_clock_yields;
static uint32_t read_eflags(void) { return cpu_flags; }
bool g_cpu_tsc;
uint64_t ktime_cycles(void) { return read_tsc_serialized(); }
static struct task *vm_work_create(const char *name, void (*fn)(void *),
                                   void *arg, enum task_prio prio)
{
    CHECK(!strcmp(name, "device-work") && fn && !arg && prio == P_DEVICE);
    vm_clock_worker.state = T_BLOCKED;
    return &vm_clock_worker;
}
static void vm_work_start(struct task *t)
{ CHECK(t == &vm_clock_worker); t->state = T_READY; }
static void vm_work_yield(void)
{ CHECK(g_current == &vm_clock_worker); vm_clock_yields++; }
#define task_create_kernel vm_work_create
#define task_start vm_work_start
#define task_yield vm_work_yield
#define worker vm_work_worker
#define stats vm_work_stats
#define ring vm_work_ring
#include "../../src/kernel/core/work.c"
#undef ring
#undef stats
#undef worker
#undef task_yield
#undef task_start
#undef task_create_kernel
static bool vm_work_one(void)
{
    struct task *saved = g_current;
    g_current = &vm_clock_worker;
    bool ran = kwork_run_one();
    g_current = saved;
    return ran;
}
static void vm_work_drain(void)
{
    unsigned guard = 0;
    while (vm_work_one()) CHECK(++guard < 20000);
}
/* fx-9 transport: the vm-clock task exists once (dosvm_clock_start) and the
 * PIT top half signals it (dosvm_clock_signal); no ring entry is involved. */
static unsigned vm_clock_signals, vm_clock_starts;
int dosvm_clock_start(void) { vm_clock_starts++; return 0; }
void dosvm_clock_signal(void) { vm_clock_signals++; }
#define DOSVM_CLOCK_HOST_TRANSPORT
#include "../../src/kernel/vm/dosvm_clock.c"
#undef DOSVM_CLOCK_HOST_TRANSPORT
static void host_clock_tick(void)
{
    CHECK(!kwork_init());
    dosvm_tick();          /* the PIT top half: samples, signals */
    dosvm_clock_service(); /* the vm-clock task's service */
    vm_work_drain();       /* other deferred work of the same tick */
}
