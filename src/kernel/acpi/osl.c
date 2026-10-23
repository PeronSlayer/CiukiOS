/* ACPICA OSL: serialized bounded inventory and the approved power owner.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "runtime.h"
#include "acclib.h"
#include <stdarg.h>

const struct fw_ops *fw_os;
/* fx-3 (T23 trial 9a60215f: `[acpi] inventory status=17 devices=1`, the
 * acpi-power task `device_overrun count=49` while the desktop started): the
 * budget was 250 ms of wall clock per operation, so time the P_DEVICE scan
 * task spent waiting for the CPU counted as firmware work. `ms` is now the
 * scan task's own execution time (cpustat run_cycles through
 * fw_ops.run_clock); a wall cap keeps sleeping/starved operations bounded:
 * max(ms, FW_WALL_CAP_MS) per operation, FW_SCAN_WALL_MS per scan. ACPICA
 * itself bounds a While loop by 30 s of timer (ACPI_MAX_LOOP_TIMEOUT,
 * dscontrol.c) and one Sleep by ACPI_MAX_SLEEP (2 s, acconfig.h); the caps
 * stay below those and the steps bound is unchanged. The first expiry and
 * the first failure are kept for one honest record per scan. */
static uint64_t deadline, run_deadline, run_start, wall_start, scan_wall_start, scan_run_start;
static uint32_t steps_left, steps_done, allocated, peak, run_limit_ms, wall_cap_ms;
static ACPI_STATUS failure;
static ACPI_STATUS run_failure;
static bool budgeting, scanning;
static const char *stage_name = "none";
static char stage_node[5];
static struct fw_budget_report report;
struct allocation {
    struct allocation *next, *prev;
    size_t bytes, charge;
};
static struct allocation *allocations;
struct mapping { void *ptr; uint32_t address, length, refs; };
static struct mapping mappings[FW_MAX];
struct semaphore { uint32_t units, max, waiters; };
/* IRQ/GPE callbacks are refused in read-only mode. Owned event methods run
 * on this bounded queue; EC byte progress is independent of that worker. */
static struct { ACPI_OSD_EXEC_CALLBACK fn; void *arg; } queue[16];
static unsigned queued;

void fw_bind(const struct fw_ops *ops)
{
    fw_os = ops; failure = run_failure = AE_OK; peak = allocated;
    memset(&report, 0, sizeof(report)); report.reason = report.stage = report.first_stage = "none";
    stage_name = "none"; memset(stage_node, 0, sizeof(stage_node)); scanning = budgeting = false;
}
static uint64_t run_now(void) { return fw_os->run_clock ? fw_os->run_clock() : fw_os->clock(); }
static uint32_t us_of(uint64_t ticks100ns) { uint64_t us = ticks100ns / 10; return us > UINT32_MAX ? UINT32_MAX : (uint32_t)us; }
void fw_fail(ACPI_STATUS status)
{
    if (!failure && status) failure = status;
    if (!run_failure && status) run_failure = status;
    if (status && !report.first_error) {
        report.first_error = status; report.first_stage = stage_name;
        memcpy(report.first_node, stage_node, sizeof(stage_node));
    }
}
void fw_budget_stage(const char *stage, ACPI_HANDLE node)
{
    stage_name = stage ? stage : "none";
    memset(stage_node, 0, sizeof(stage_node));
    if (node) {
        /* Caller-supplied buffer: no allocation; the name is four bytes. */
        ACPI_BUFFER name = {sizeof(stage_node), stage_node};
        if (AcpiGetName(node, ACPI_SINGLE_NAME, &name)) memset(stage_node, 0, sizeof(stage_node));
        stage_node[4] = 0;
    }
}
void fw_scan_begin(void)
{
    scanning = true; scan_wall_start = fw_os->clock(); scan_run_start = run_now();
    report.run_clock = fw_os->run_clock != NULL;
}
void fw_scan_end(void)
{
    if (!scanning) return;
    scanning = false;
    report.scan_wall_us = us_of(fw_os->clock() - scan_wall_start);
    report.scan_run_us = us_of(run_now() - scan_run_start);
}
void fw_budget_snapshot(struct fw_budget_report *out) { *out = report; }
static ACPI_STATUS expire(const char *reason)
{
    fw_fail(AE_TIME);
    if (!report.expired) {
        report.expired = true; report.reason = reason; report.stage = stage_name;
        memcpy(report.node, stage_node, sizeof(stage_node));
        report.run_us = us_of(run_now() - run_start); report.wall_us = us_of(fw_os->clock() - wall_start);
        report.steps = steps_done; report.run_limit_ms = run_limit_ms; report.wall_cap_ms = wall_cap_ms;
    }
    return failure;
}
ACPI_STATUS fw_error(void) { return failure; }
ACPI_STATUS fw_run_error(void) { return run_failure; }
ACPI_STATUS AcpiCiukiParseResult(ACPI_STATUS s)
{
    if (s && (s & AE_CODE_MASK) != AE_CODE_CONTROL) fw_fail(s);
    return s;
}
void fw_budget_begin(uint32_t steps, uint32_t ms)
{
    failure = AE_OK; steps_left = steps; steps_done = 0; budgeting = true;
    wall_start = fw_os->clock(); run_start = run_now();
    run_limit_ms = ms; wall_cap_ms = ms > FW_WALL_CAP_MS ? ms : FW_WALL_CAP_MS;
    deadline = wall_start + (uint64_t)wall_cap_ms * 10000;
    run_deadline = run_start + (uint64_t)ms * 10000;
    if (scanning) {
        uint64_t scan_end = scan_wall_start + (uint64_t)FW_SCAN_WALL_MS * 10000;
        if (deadline > scan_end) deadline = scan_end;
    }
}
void fw_budget_end(void) { budgeting = false; }
void fw_budget_cancel(void) { (void)expire("cancel"); }
ACPI_STATUS AcpiCiukiBudgetStep(void)
{
    if (fw_interrupt) return AE_OK;
    if (failure) return failure;
    if (!budgeting) return AE_OK; /* ACPICA shutdown is always allowed to unwind. */
    if (!steps_left) return expire("steps");
    uint64_t now = fw_os->clock();
    if (now >= deadline)
        return expire(scanning && now >= scan_wall_start + (uint64_t)FW_SCAN_WALL_MS * 10000 ? "scan_wall" : "wall");
    if (run_now() >= run_deadline) return expire("run");
    steps_left--;
    if (!(++steps_done & 63u)) fw_os->pause(0);
    return AE_OK;
}
void fw_alloc_stats(uint32_t *live, uint32_t *high) { *live = allocated; *high = peak; }
void *AcpiOsAllocate(ACPI_SIZE n)
{
    if (!fw_os || !n || n > FW_ALLOC_MAX - sizeof(struct allocation)) return NULL;
    uint32_t flags = fw_os->lock();
    size_t charge = fw_os->charge(n + sizeof(struct allocation));
    if (charge > FW_ALLOC_MAX - allocated) { fw_fail(AE_NO_MEMORY); fw_os->unlock(flags); return NULL; }
    struct allocation *a = fw_os->alloc(n + sizeof(*a));
    if (!a) { fw_fail(AE_NO_MEMORY); fw_os->unlock(flags); return NULL; }
    a->bytes = n; a->charge = charge; a->prev = NULL; a->next = allocations;
    if (allocations) allocations->prev = a;
    allocations = a; allocated += charge; if (allocated > peak) peak = allocated;
    fw_os->unlock(flags); return a + 1;
}
void *AcpiOsAllocateZeroed(ACPI_SIZE n)
{ void *p = AcpiOsAllocate(n); if (p) memset(p, 0, n); return p; }
void AcpiOsFree(void *ptr)
{
    if (!ptr) return;
    uint32_t flags = fw_os->lock();
    struct allocation *a = (struct allocation *)ptr - 1;
    if (a->prev) a->prev->next = a->next; else allocations = a->next;
    if (a->next) a->next->prev = a->prev;
    allocated -= a->charge; fw_os->free(a);
    fw_os->unlock(flags);
}
ACPI_STATUS AcpiOsInitialize(void) { return fw_os ? AE_OK : AE_NOT_CONFIGURED; }
ACPI_STATUS AcpiOsTerminate(void) { return AE_OK; }
ACPI_PHYSICAL_ADDRESS AcpiOsGetRootPointer(void) { return fw_rsdp; }
ACPI_STATUS AcpiOsPredefinedOverride(const ACPI_PREDEFINED_NAMES *v, ACPI_STRING *out)
{ if (!v || !out) return AE_BAD_PARAMETER; *out = NULL; return AE_OK; }
ACPI_STATUS AcpiOsTableOverride(ACPI_TABLE_HEADER *v, ACPI_TABLE_HEADER **out)
{ if (!v || !out) return AE_BAD_PARAMETER; *out = NULL; return AE_OK; }
ACPI_STATUS AcpiOsPhysicalTableOverride(ACPI_TABLE_HEADER *v, ACPI_PHYSICAL_ADDRESS *out, UINT32 *n)
{ if (!v || !out || !n) return AE_BAD_PARAMETER; *out = 0; *n = 0; return AE_OK; }
void *AcpiOsMapMemory(ACPI_PHYSICAL_ADDRESS address, ACPI_SIZE length)
{
    if (!length || address > UINT32_MAX || length > FW_TABLE_MAX ||
        address + length > UINT32_MAX) { fw_fail(AE_BAD_ADDRESS); return NULL; }
    bool allowed = false;
    for (unsigned i = 0; i < fw_table_count; i++)
        if (address >= fw_tables[i].address && address + length <=
            (uint64_t)fw_tables[i].address + fw_tables[i].length) allowed = true;
    if (!allowed) { fw_fail(AE_ACCESS); return NULL; }
    for (unsigned i = 0; i < FW_MAX; i++) if (mappings[i].refs &&
        mappings[i].address == address && mappings[i].length == length) {
        if (mappings[i].refs == UINT32_MAX) return NULL;
        mappings[i].refs++; return mappings[i].ptr;
    }
    for (unsigned i = 0; i < FW_MAX; i++) if (!mappings[i].refs) {
        void *p = (void *)fw_os->table_map(address, length);
        if (!p) { fw_fail(AE_NO_MEMORY); return NULL; }
        mappings[i] = (struct mapping){p, address, length, 1}; return p;
    }
    fw_fail(AE_LIMIT); return NULL;
}
void AcpiOsUnmapMemory(void *ptr, ACPI_SIZE length)
{
    for (unsigned i = 0; i < FW_MAX; i++) if (mappings[i].refs &&
        mappings[i].ptr == ptr && mappings[i].length == length) {
        if (!--mappings[i].refs) fw_os->table_unmap(ptr, length);
        return;
    }
}
ACPI_STATUS AcpiOsGetPhysicalAddress(void *ptr, ACPI_PHYSICAL_ADDRESS *out)
{
    if (!out) return AE_BAD_PARAMETER;
    for (unsigned i = 0; i < FW_MAX; i++) if (mappings[i].refs &&
        (uintptr_t)ptr >= (uintptr_t)mappings[i].ptr &&
        (uintptr_t)ptr - (uintptr_t)mappings[i].ptr < mappings[i].length) {
        *out = mappings[i].address + ((uintptr_t)ptr - (uintptr_t)mappings[i].ptr); return AE_OK;
    }
    return AE_BAD_ADDRESS;
}
BOOLEAN AcpiOsReadable(void *ptr, ACPI_SIZE n)
{
    uintptr_t p = (uintptr_t)ptr;
    if (!p || n > UINTPTR_MAX - p) return FALSE;
    for (struct allocation *a = allocations; a; a = a->next)
        if (p >= (uintptr_t)(a + 1) && p + n <= (uintptr_t)(a + 1) + a->bytes) return TRUE;
    for (unsigned i = 0; i < FW_MAX; i++) if (mappings[i].refs &&
        p >= (uintptr_t)mappings[i].ptr && p + n <= (uintptr_t)mappings[i].ptr + mappings[i].length) return TRUE;
    return FALSE;
}
BOOLEAN AcpiOsWritable(void *ptr, ACPI_SIZE n)
{
    uintptr_t p = (uintptr_t)ptr;
    if (!p || n > UINTPTR_MAX - p) return FALSE;
    for (struct allocation *a = allocations; a; a = a->next)
        if (p >= (uintptr_t)(a + 1) && p + n <= (uintptr_t)(a + 1) + a->bytes) return TRUE;
    return FALSE; /* Firmware mappings are never writable. */
}
ACPI_STATUS AcpiOsCreateLock(ACPI_SPINLOCK *out)
{ if (!out) return AE_BAD_PARAMETER; *out = AcpiOsAllocate(1); return *out ? AE_OK : AE_NO_MEMORY; }
void AcpiOsDeleteLock(ACPI_SPINLOCK lock) { AcpiOsFree(lock); }
ACPI_CPU_FLAGS AcpiOsAcquireLock(ACPI_SPINLOCK lock) { (void)lock; return fw_os->lock(); }
void AcpiOsReleaseLock(ACPI_SPINLOCK lock, ACPI_CPU_FLAGS flags) { (void)lock; fw_os->unlock(flags); }
ACPI_STATUS AcpiOsCreateSemaphore(UINT32 max, UINT32 initial, ACPI_SEMAPHORE *out)
{
    if (!out || !max || initial > max) return AE_BAD_PARAMETER;
    struct semaphore *s = AcpiOsAllocate(sizeof(*s)); if (!s) return AE_NO_MEMORY;
    *s = (struct semaphore){initial, max, 0}; *out = s; return AE_OK;
}
ACPI_STATUS AcpiOsDeleteSemaphore(ACPI_SEMAPHORE handle)
{
    struct semaphore *s = handle;
    if (!s) return AE_BAD_PARAMETER;
    if (s->waiters) return AE_TIME;
    AcpiOsFree(s); return AE_OK;
}
ACPI_STATUS AcpiOsWaitSemaphore(ACPI_SEMAPHORE handle, UINT32 units, UINT16 timeout)
{
    struct semaphore *s = handle;
    if (!s || !units || units > s->max) return AE_BAD_PARAMETER;
    uint64_t end = fw_os->clock() + (uint64_t)timeout * 10000;
    /* Infinite firmware waits are clipped to the transaction budget. */
    if (budgeting && end > deadline) end = deadline;
    s->waiters++;
    for (unsigned tries = 0; tries < 250; tries++) {
        uint32_t flags = fw_os->lock();
        if (s->units >= units) { s->units -= units; fw_os->unlock(flags); s->waiters--; return AE_OK; }
        fw_os->unlock(flags);
        if (!timeout || fw_os->clock() >= end || ACPI_FAILURE(AcpiCiukiBudgetStep())) break;
        fw_os->pause(1);
    }
    /* fx-3: a timed-out wait (no fw_fail of its own) is counted for the record. */
    if (timeout && report.semaphore_timeouts != UINT32_MAX) report.semaphore_timeouts++;
    s->waiters--; return AE_TIME;
}
ACPI_STATUS AcpiOsSignalSemaphore(ACPI_SEMAPHORE handle, UINT32 units)
{
    struct semaphore *s = handle;
    if (!s || !units) return AE_BAD_PARAMETER;
    uint32_t flags = fw_os->lock();
    ACPI_STATUS status = units > s->max - s->units ? AE_LIMIT : AE_OK;
    if (!status) s->units += units;
    fw_os->unlock(flags); return status;
}
ACPI_STATUS AcpiOsCreateMutex(ACPI_MUTEX *out) { return AcpiOsCreateSemaphore(1, 1, out); }
void AcpiOsDeleteMutex(ACPI_MUTEX mutex) { (void)AcpiOsDeleteSemaphore(mutex); }
ACPI_STATUS AcpiOsAcquireMutex(ACPI_MUTEX mutex, UINT16 timeout) { return AcpiOsWaitSemaphore(mutex, 1, timeout); }
void AcpiOsReleaseMutex(ACPI_MUTEX mutex) { (void)AcpiOsSignalSemaphore(mutex, 1); }
ACPI_THREAD_ID AcpiOsGetThreadId(void) { return fw_os->thread(); }
UINT64 AcpiOsGetTimer(void) { return fw_os->clock(); }
ACPI_STATUS AcpiOsExecute(ACPI_EXECUTE_TYPE type, ACPI_OSD_EXEC_CALLBACK fn, void *arg)
{
    if (!fn) return AE_BAD_PARAMETER;
    if (type != OSL_NOTIFY_HANDLER && (!fw_runtime || type != OSL_GPE_HANDLER)) return AE_ACCESS;
    uint32_t flags = fw_os->lock();
    if (queued == 16) { fw_os->unlock(flags); fw_power.event_overflows++; return AE_LIMIT; }
    queue[queued].fn = fn; queue[queued++].arg = arg; fw_os->unlock(flags); return AE_OK;
}
void fw_queue_drain(unsigned limit)
{
    /* Pop before dispatch: callbacks cannot extend this drain indefinitely. */
    unsigned count = queued < limit ? queued : limit;
    while (count-- && queued && !AcpiCiukiBudgetStep()) {
        uint32_t flags = fw_os->lock();
        ACPI_OSD_EXEC_CALLBACK fn = queue[0].fn; void *arg = queue[0].arg;
        for (unsigned i = 1; i < queued; i++) queue[i - 1] = queue[i];
        queued--; fw_os->unlock(flags); fn(arg); fw_os->pause(0);
    }
}
void AcpiOsWaitEventsComplete(void) { fw_queue_drain(16); }
void AcpiOsSleep(UINT64 ms)
{
    if (ms > 250) { fw_fail(AE_TIME); return; }
    for (UINT64 i = 0; i < ms; i++) {
        if (AcpiCiukiBudgetStep()) return;
        fw_os->pause(1);
    }
}
void AcpiOsStall(UINT32 us)
{
    if (us > 1000) { fw_fail(AE_TIME); return; }
    if (AcpiCiukiBudgetStep()) return;
    if (us && fw_os->stall(us)) fw_fail(AE_TIME);
    (void)AcpiCiukiBudgetStep();
}
static ACPI_STATUS access(unsigned space, uint64_t address, unsigned width, UINT64 *value, const ACPI_PCI_ID *pci)
{
    if (!value || (width != 8 && width != 16 && width != 32 && (width != 64 || space != 1))) return AE_BAD_PARAMETER;
    if (address > UINT32_MAX || address + width / 8 > (space == 0 ? 0x10000ull : space == 2 ? 256ull : UINT32_MAX)) return AE_BAD_ADDRESS;
    if (space == 1 && address % (width / 8)) return AE_BAD_PARAMETER;
    if (space == 2 && (!pci || pci->Segment || pci->Bus > 255 || pci->Device > 31 || pci->Function > 7 || address % (width / 8))) return AE_BAD_PARAMETER;
    if (AcpiCiukiBudgetStep()) return fw_error();
    ACPI_STATUS status = fw_os->access(space, address, width, value, pci);
    if (status) fw_fail(status);
    return status;
}
ACPI_STATUS AcpiOsReadPort(ACPI_IO_ADDRESS a, UINT32 *v, UINT32 w)
{ UINT64 n = 0; if (!v) return AE_BAD_PARAMETER; ACPI_STATUS s = access(0, a, w, &n, NULL); *v = (UINT32)n; return s; }
ACPI_STATUS AcpiOsReadMemory(ACPI_PHYSICAL_ADDRESS a, UINT64 *v, UINT32 w) { return access(1, a, w, v, NULL); }
ACPI_STATUS AcpiOsReadPciConfiguration(ACPI_PCI_ID *p, UINT32 a, UINT64 *v, UINT32 w) { return access(2, a, w, v, p); }
static ACPI_STATUS write(unsigned space, uint64_t a, unsigned w, UINT64 v, const ACPI_PCI_ID *p)
{
    ACPI_STATUS s = AE_ACCESS;
    if (fw_writes && fw_os->write && !AcpiCiukiBudgetStep() && fw_region_check(space, a, w, p))
        s = fw_os->write(space, a, w, v, p);
    if (s) fw_fail(s);
    return s;
}
ACPI_STATUS AcpiOsWritePort(ACPI_IO_ADDRESS a, UINT32 v, UINT32 w) { return write(0, a, w, v, NULL); }
ACPI_STATUS AcpiOsWriteMemory(ACPI_PHYSICAL_ADDRESS a, UINT64 v, UINT32 w) { return write(1, a, w, v, NULL); }
ACPI_STATUS AcpiOsWritePciConfiguration(ACPI_PCI_ID *p, UINT32 a, UINT64 v, UINT32 w) { return write(2, a, w, v, p); }
ACPI_STATUS AcpiOsInstallInterruptHandler(UINT32 n, ACPI_OSD_HANDLER fn, void *ctx)
{ return fw_runtime && fw_os->sci ? fw_os->sci(n, fn, ctx) : AE_ACCESS; }
ACPI_STATUS AcpiOsRemoveInterruptHandler(UINT32 n, ACPI_OSD_HANDLER fn)
{ return fw_runtime && fw_os->unsci ? fw_os->unsci(n, fn) : AE_ACCESS; }
ACPI_STATUS AcpiOsEnterSleep(UINT8 n, UINT32 a, UINT32 b) { (void)n; (void)a; (void)b; return AE_ACCESS; }
ACPI_STATUS AcpiOsSignal(UINT32 n, void *info) { (void)n; (void)info; fw_fail(AE_ERROR); return AE_ERROR; }
void AcpiOsPrintf(const char *format, ...) { (void)format; }
void AcpiOsVprintf(const char *format, va_list args) { (void)format; (void)args; }
void AcpiOsRedirectOutput(void *out) { (void)out; }
void fw_os_cleanup(void)
{
    queued = 0;
    for (unsigned i = 0; i < FW_MAX; i++) if (mappings[i].refs) {
        fw_os->table_unmap(mappings[i].ptr, mappings[i].length); mappings[i].refs = 0;
    }
    /* Terminate normally releases objects; retained allocations on partial
     * initialization are still owned by this one-shot service. */
    while (allocations) AcpiOsFree(allocations + 1);
}
