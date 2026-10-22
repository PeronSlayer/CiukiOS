/* ACPICA OSL: serialized, bounded, read-only firmware inventory.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "internal.h"
#include "acclib.h"
#include <stdarg.h>

const struct fw_ops *fw_os;
static uint64_t deadline;
static uint32_t steps_left, steps_done, allocated, peak;
static ACPI_STATUS failure;
static ACPI_STATUS run_failure;
static bool budgeting;
struct allocation {
    struct allocation *next, *prev;
    size_t bytes, charge;
};
static struct allocation *allocations;
struct mapping { void *ptr; uint32_t address, length, refs; };
static struct mapping mappings[FW_MAX];
struct semaphore { uint32_t units, max, waiters; };
/* No IRQ/GPE/EC callbacks are admitted in read-only mode. A bounded queue
 * remains available for non-event notification work on the inventory thread. */
static struct { ACPI_OSD_EXEC_CALLBACK fn; void *arg; } queue[16];
static unsigned queued;

void fw_bind(const struct fw_ops *ops) { fw_os = ops; failure = run_failure = AE_OK; peak = allocated; }
void fw_fail(ACPI_STATUS status)
{
    if (!failure && status) failure = status;
    if (!run_failure && status) run_failure = status;
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
    deadline = fw_os->clock() + (uint64_t)ms * 10000;
}
void fw_budget_end(void) { budgeting = false; }
void fw_budget_cancel(void) { fw_fail(AE_TIME); }
ACPI_STATUS AcpiCiukiBudgetStep(void)
{
    if (failure) return failure;
    if (!budgeting) return AE_OK; /* ACPICA shutdown is always allowed to unwind. */
    if (!steps_left || fw_os->clock() >= deadline) { fw_fail(AE_TIME); return failure; }
    steps_left--;
    if (!(++steps_done & 63u)) fw_os->pause(0);
    return AE_OK;
}
void fw_alloc_stats(uint32_t *live, uint32_t *high) { *live = allocated; *high = peak; }
void *AcpiOsAllocate(ACPI_SIZE n)
{
    if (!fw_os || !n || n > FW_ALLOC_MAX - sizeof(struct allocation)) return NULL;
    size_t charge = fw_os->charge(n + sizeof(struct allocation));
    if (charge > FW_ALLOC_MAX - allocated) { fw_fail(AE_NO_MEMORY); return NULL; }
    struct allocation *a = fw_os->alloc(n + sizeof(*a));
    if (!a) { fw_fail(AE_NO_MEMORY); return NULL; }
    a->bytes = n; a->charge = charge; a->prev = NULL; a->next = allocations;
    if (allocations) allocations->prev = a;
    allocations = a; allocated += charge; if (allocated > peak) peak = allocated;
    return a + 1;
}
void *AcpiOsAllocateZeroed(ACPI_SIZE n)
{ void *p = AcpiOsAllocate(n); if (p) memset(p, 0, n); return p; }
void AcpiOsFree(void *ptr)
{
    if (!ptr) return;
    struct allocation *a = (struct allocation *)ptr - 1;
    if (a->prev) a->prev->next = a->next; else allocations = a->next;
    if (a->next) a->next->prev = a->prev;
    allocated -= a->charge; fw_os->free(a);
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
    if (type != OSL_NOTIFY_HANDLER) return AE_ACCESS;
    if (queued == 16) return AE_LIMIT;
    queue[queued].fn = fn; queue[queued++].arg = arg; return AE_OK;
}
void AcpiOsWaitEventsComplete(void)
{
    /* Pop before dispatch: callbacks cannot extend this drain indefinitely. */
    unsigned count = queued;
    while (count-- && queued && !AcpiCiukiBudgetStep()) {
        ACPI_OSD_EXEC_CALLBACK fn = queue[0].fn; void *arg = queue[0].arg;
        for (unsigned i = 1; i < queued; i++) queue[i - 1] = queue[i];
        queued--; fn(arg); fw_os->pause(0);
    }
}
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
ACPI_STATUS AcpiOsWritePort(ACPI_IO_ADDRESS a, UINT32 v, UINT32 w) { (void)a; (void)v; (void)w; fw_fail(AE_ACCESS); return AE_ACCESS; }
ACPI_STATUS AcpiOsWriteMemory(ACPI_PHYSICAL_ADDRESS a, UINT64 v, UINT32 w) { (void)a; (void)v; (void)w; fw_fail(AE_ACCESS); return AE_ACCESS; }
ACPI_STATUS AcpiOsWritePciConfiguration(ACPI_PCI_ID *p, UINT32 a, UINT64 v, UINT32 w) { (void)p; (void)a; (void)v; (void)w; fw_fail(AE_ACCESS); return AE_ACCESS; }
ACPI_STATUS AcpiOsInstallInterruptHandler(UINT32 n, ACPI_OSD_HANDLER fn, void *ctx) { (void)n; (void)fn; (void)ctx; return AE_ACCESS; }
ACPI_STATUS AcpiOsRemoveInterruptHandler(UINT32 n, ACPI_OSD_HANDLER fn) { (void)n; (void)fn; return AE_ACCESS; }
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
