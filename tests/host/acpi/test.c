/* Real ACPICA, fake reserved ROM and ownership. SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "internal.h"
#include <ciuki/registry.h>

static unsigned char *rom;
static uint64_t now;
static int fail_after = -1, alloc_calls, maps, accesses, pauses;
static size_t live;
static struct resource lease;
static struct pci_func function = {.bus = 0, .dev = 7, .fn = 0};
bool registry_valid(int h, gen_t gen) { return h == 0 && gen == lease.generation && lease.state == RS_ACTIVE; }
const struct resource *registry_get(unsigned h) { return h ? NULL : &lease; }
unsigned pci_count(void) { return 1; }
const struct pci_func *pci_get(unsigned n) { return n ? NULL : &function; }
static void *allocate(size_t n)
{
    if (alloc_calls++ == fail_after) return NULL;
    size_t *p = calloc(1, n + 16); if (!p) return NULL;
    *p = n; live += n; return p + 2;
}
static void release(void *p) { size_t *a = (size_t *)p - 2; live -= *a; free(a); }
static size_t charge(size_t n) { return n + 16; }
static const void *map(uint32_t a, uint32_t n)
{
    if (!n || a > 0x100000 || n > 0x100000 - a) return NULL;
    maps++; return rom + a;
}
static void unmap(const void *p, uint32_t n) { assert(p >= (const void *)rom && n); maps--; assert(maps >= 0); }
static uint64_t clock100ns(void) { return now; }
static void pause_ms(uint32_t n) { pauses++; now += n ? (uint64_t)n * 10000 : 1; }
static int stall_us(uint32_t n) { now += (uint64_t)n * 10; return 0; }
static uint64_t thread(void) { return 1; }
static uint32_t lock(void) { return 0x200; }
static void unlock(uint32_t n) { assert(n == 0x200); }
static ACPI_STATUS access(unsigned space, uint64_t a, unsigned width, UINT64 *v, const ACPI_PCI_ID *pci)
{
    if (!fw_region_check(space, a, width, pci)) return AE_ACCESS;
    accesses++; *v = 42; return AE_OK;
}
/* Read-only inventory: no writes, routes or ACPI/APM power provider seam. */
static const struct fw_ops ops = {allocate, release, charge, map, unmap, clock100ns, pause_ms, stall_us, thread, lock, unlock, access,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};
static void callback(void *p) { (*(int *)p)++; }
static void osl(void)
{
    fw_bind(&ops); fw_budget_begin(100, 10);
    assert(AcpiOsInstallInterruptHandler(9, NULL, NULL) == AE_ACCESS);
    assert(AcpiOsEnterSleep(5, 0, 0) == AE_ACCESS);
    UINT64 v = 0; UINT32 port;
    assert(AcpiOsReadPort(0x100000080ull, &port, 8) == AE_BAD_ADDRESS);
    assert(AcpiOsReadMemory(0x100000000ull, &v, 32) == AE_BAD_ADDRESS);
    assert(AcpiOsReadMemory(0x1001, &v, 32) == AE_BAD_PARAMETER);
    assert(AcpiOsReadPort(0x80, &port, 64) == AE_BAD_PARAMETER);
    ACPI_PCI_ID pci = {0, 0, 7, 0};
    assert(AcpiOsReadPciConfiguration(&pci, 256, &v, 8) == AE_BAD_ADDRESS);
    assert(AcpiOsReadPciConfiguration(&pci, 3, &v, 16) == AE_BAD_PARAMETER);
    assert(AcpiOsWritePort(0x80, 1, 8) == AE_ACCESS);
    fw_budget_begin(100, 10);
    ACPI_SEMAPHORE sem; assert(!AcpiOsCreateSemaphore(3, 1, &sem));
    assert(AcpiOsWaitSemaphore(sem, 2, 0) == AE_TIME);
    assert(!AcpiOsSignalSemaphore(sem, 1)); assert(!AcpiOsWaitSemaphore(sem, 2, 0));
    assert(AcpiOsSignalSemaphore(sem, 4) == AE_LIMIT);
    assert(AcpiOsWaitSemaphore(sem, 1, 0xffff) == AE_TIME);
    assert(!AcpiOsDeleteSemaphore(sem));
    fw_budget_begin(100, 10);
    ACPI_MUTEX mutex; assert(!AcpiOsCreateMutex(&mutex));
    assert(!AcpiOsAcquireMutex(mutex, 0)); assert(AcpiOsAcquireMutex(mutex, 0) == AE_TIME);
    AcpiOsReleaseMutex(mutex); AcpiOsDeleteMutex(mutex);
    ACPI_SPINLOCK spin; assert(!AcpiOsCreateLock(&spin));
    ACPI_CPU_FLAGS f = AcpiOsAcquireLock(spin); AcpiOsReleaseLock(spin, f); AcpiOsDeleteLock(spin);
    int called = 0;
    assert(AcpiOsExecute(OSL_GPE_HANDLER, callback, &called) == AE_ACCESS);
    for (unsigned i = 0; i < 16; i++) assert(!AcpiOsExecute(OSL_NOTIFY_HANDLER, callback, &called));
    assert(AcpiOsExecute(OSL_NOTIFY_HANDLER, callback, &called) == AE_LIMIT);
    fw_budget_begin(100, 10); AcpiOsWaitEventsComplete(); assert(called == 16);
    fw_budget_begin(1, 10); assert(!AcpiCiukiBudgetStep()); assert(AcpiCiukiBudgetStep() == AE_TIME);
    fw_budget_begin(100, 10); now += 100000; assert(AcpiCiukiBudgetStep() == AE_TIME);
    fw_budget_begin(100, 10); fw_budget_cancel(); assert(AcpiCiukiBudgetStep() == AE_TIME);
    fw_budget_begin(100, 10); assert(!AcpiOsMapMemory(0x100000000ull, 4));
    fw_budget_begin(100, 10); assert(!fw_tables_discover(0xe0000));
    void *mapped = AcpiOsMapMemory(0x10000, 36); assert(mapped && AcpiOsReadable(mapped, 36) && !AcpiOsWritable(mapped, 1));
    ACPI_PHYSICAL_ADDRESS physical; assert(!AcpiOsGetPhysicalAddress(mapped, &physical) && physical == 0x10000);
    AcpiOsUnmapMemory(mapped, 36); assert(!AcpiOsMapMemory(0x80000, 36));
    fw_budget_begin(100, 10);
    void *p = AcpiOsAllocate(64); assert(p && AcpiOsWritable(p, 64));
    assert(!AcpiOsWritable(p, 65)); AcpiOsFree(p);
    void *blocks[32] = {0}; unsigned n = 0;
    fw_budget_begin(100, 10);
    while (n < 32 && (blocks[n] = AcpiOsAllocate(65536))) n++;
    assert(n && n < 32 && fw_error() == AE_NO_MEMORY);
    while (n) AcpiOsFree(blocks[--n]);
    fw_budget_end(); fw_os_cleanup(); assert(!live && !maps && !accesses);
}
static void broker(void)
{
    fw_bind(&ops);
    lease = (struct resource){RES_PORT, 0x60, 0x65, "i8042", 7, RS_ACTIVE, false};
    assert(fw_region_grant(0, 0, 7, NULL) < 0);
    lease.owner = "acpi-inventory";
    assert(!fw_region_grant(0, 0, 7, NULL));
    assert(fw_region_check(0, 0x60, 8, NULL)); assert(!fw_region_check(0, 0x64, 16, NULL));
    fw_budget_begin(100, 10); UINT32 port = 0;
    assert(!AcpiOsReadPort(0x60, &port, 8) && port == 42 && accesses == 1);
    lease.generation++; assert(!fw_region_check(0, 0x60, 8, NULL));
    assert(AcpiOsReadPort(0x60, &port, 8) == AE_ACCESS && accesses == 1);
    lease = (struct resource){RES_MMIO, 0x1000, 0x2000, "acpi-inventory", 9, RS_ACTIVE, false};
    assert(!fw_region_grant(1, 0, 9, NULL)); assert(fw_region_check(1, 0x1000, 64, NULL));
    assert(!fw_region_check(1, 0x100001000ull, 64, NULL));
    fw_budget_begin(100, 10); UINT64 memory = 0;
    assert(!AcpiOsReadMemory(0x1000, &memory, 64) && memory == 42 && accesses == 2);
    assert(AcpiOsWriteMemory(0x1000, 0, 64) == AE_ACCESS && accesses == 2);
    lease.state = RS_QUARANTINED; assert(!fw_region_check(1, 0x1000, 8, NULL));
    ACPI_PCI_ID pci = {0, 0, 7, 0};
    lease = (struct resource){RES_PORT, 0xcf8, 0xd00, "acpi-inventory", 10, RS_ACTIVE, false};
    assert(!fw_region_grant(2, 0, 10, &pci)); assert(fw_region_check(2, 0, 32, &pci));
    fw_budget_begin(100, 10);
    assert(!AcpiOsReadPciConfiguration(&pci, 0, &memory, 32) && memory == 42 && accesses == 3);
    assert(AcpiOsWritePciConfiguration(&pci, 0, 0, 32) == AE_ACCESS && accesses == 3);
    pci.Device++; assert(!fw_region_check(2, 0, 32, &pci));
    assert(!fw_region_check(2, 255, 16, &pci));
    fw_budget_end(); fw_os_cleanup();
}
int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    assert(argc >= 3);
    char path[1024]; snprintf(path, sizeof(path), "%s/%s.bin", argv[1], argv[2]);
    FILE *f = fopen(path, "rb"); assert(f);
    rom = mmap(NULL, 0x100000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(rom != MAP_FAILED && fread(rom, 1, 0x100000, f) == 0x100000); fclose(f);
    assert(!mprotect(rom, 0x100000, PROT_READ)); /* Any firmware write is a real fault. */
    const char *scenario = argv[2];
    if (!strcmp(scenario, "osl")) osl();
    else if (!strcmp(scenario, "broker")) broker();
    else {
        if (!strcmp(scenario, "allocation")) { assert(argc == 4); fail_after = atoi(argv[3]); }
        ACPI_STATUS s;
        if (!strcmp(scenario, "start-failure")) { s = AE_NO_MEMORY; fw_inventory_unavailable(s); }
        else s = fw_inventory_run(&ops, !strcmp(scenario, "no-acpi") ? 0 : 0xe0000);
        struct ciuki_firmware_page p = {.version = 2, .size = sizeof(p)};
        assert(!firmware_inventory_page(&p));
        assert(!live && !maps && !accesses && !(p.flags & CIUKI_FIRMWARE_PENDING));
        if (!strncmp(scenario,"serial",6)) {
            bool found=false;
            for (unsigned i=0;i<p.count;i++) {
                const struct ciuki_firmware_record *r=&p.records[i];
                if (strcmp(r->hid,"PNP0501") && strcmp(r->hid,"ACME0001")) continue;
                found=true;
                if (!strcmp(scenario,"serial-disabled")) assert(r->state==CIUKI_FIRMWARE_UNSUPPORTED && !r->resource_count);
                else if (!strcmp(scenario,"serial-variable")) assert(r->state==CIUKI_FIRMWARE_UNSUPPORTED && r->flags);
                else {
                    assert(r->resource_count==2 && r->resources[0].start==0x3f8 && r->resources[0].end==0x400);
                    unsigned expected=!strcmp(scenario,"serial-level") ? 6 : !strcmp(scenario,"serial-low") ? 3 : 7;
                    assert(r->resources[1].flags==expected && r->resources[1].start==4 && r->resources[1].end==5);
                    if (!strcmp(scenario,"serial-cid")) assert(r->cid_count==2);
                }
            }
            assert(found);
        }
        if (!strcmp(scenario, "inventory") || !strcmp(scenario, "ssdt") || !strcmp(scenario, "page") || !strcmp(scenario, "pnp") || !strcmp(scenario, "rsdt")) {
            /* ACPICA's predefined _SB_ and _TZ_ device containers are part
             * of the hierarchy, in addition to the seven fixture devices. */
            assert(!s); assert(p.total == (!strcmp(scenario, "pnp") ? 11u : 10u));
            unsigned seen = 0, cid = 0, crs = 0, battery = 0, ac = 0, thermal = 0;
            for (;;) {
                for (unsigned i = 0; i < p.count; i++) {
                    const struct ciuki_firmware_record *r = &p.records[i]; seen++;
                    if (!strcmp(r->hid, "PNP0A03")) { assert(r->cid_count == 2); cid++; }
                    if (!strcmp(r->hid, "PNP0303")) {
                        assert(r->parent && r->resource_count == 2 && r->resources[0].start == 0x60 && r->resources[0].end == 0x61);
                        assert(r->resources[1].start == 1 && r->state == CIUKI_FIRMWARE_ROUTING_UNQUALIFIED); crs++;
                    }
                    if (r->capabilities & CIUKI_FW_CAP_BATTERY) battery++;
                    if (r->capabilities & CIUKI_FW_CAP_AC) ac++;
                    if (r->capabilities & CIUKI_FW_CAP_THERMAL) thermal++;
                }
                if (!p.next) break;
                p.cursor = p.next; assert(!firmware_inventory_page(&p));
            }
            assert(seen == p.total && cid && crs && battery && ac && thermal);
            p.generation++; assert(firmware_inventory_page(&p) == -11);
            p.generation = 0; p.cursor = 999; assert(firmware_inventory_page(&p) == -22);
            p.cursor = 1; assert(firmware_inventory_page(&p) == -22);
            p.cursor = 0; p.reserved = 1; assert(firmware_inventory_page(&p) == -22);
            printf("PASS acpi %s: devices=%u tables=%u peak=%u pauses=%d allocations=%d\n", scenario, seen, fw_table_count, p.peak_bytes, pauses, alloc_calls);
        } else if (!strcmp(scenario, "loop") || !strcmp(scenario, "sleep") || !strcmp(scenario, "write") || !strcmp(scenario, "region") || !strcmp(scenario, "ec")) {
            assert(p.flags & CIUKI_FIRMWARE_INCOMPLETE);
            bool failed = false; for (unsigned i = 0; i < p.count; i++) if (p.records[i].state == CIUKI_FIRMWARE_UNSUPPORTED) failed = true;
            assert(failed); assert(pauses < 3000); printf("PASS acpi %s: bounded unsupported method\n", scenario);
        } else if (!strcmp(scenario, "large")) {
            assert(p.total == 64 && p.flags & CIUKI_FIRMWARE_TRUNCATED);
        } else if (!strcmp(scenario, "eisa")) {
            bool found = false; for (unsigned i = 0; i < p.count; i++) if (!strcmp(p.records[i].hid, "PNP0303")) found = true;
            assert(!s && found);
        } else if (!strcmp(scenario, "mem64")) {
            bool found = false;
            for (unsigned i = 0; i < p.count; i++) if (p.records[i].resource_count) {
                assert(p.records[i].resources[0].start == 0x100000000ull && p.records[i].state == CIUKI_FIRMWARE_UNSUPPORTED); found = true;
            }
            assert(found && p.flags & CIUKI_FIRMWARE_INCOMPLETE);
        } else if (!strncmp(scenario,"serial",6) && strcmp(scenario,"serial-variable")) {
            assert(!s && !(p.flags&CIUKI_FIRMWARE_INCOMPLETE));
            printf("PASS acpi %s: assigned serial status and IRQ facts\n",scenario);
        } else if (!strcmp(scenario, "allocation")) {
            assert(s || p.flags & CIUKI_FIRMWARE_INCOMPLETE || fail_after >= alloc_calls);
        } else {
            assert(s || p.flags & CIUKI_FIRMWARE_INCOMPLETE);
            printf("PASS acpi %s: rejected status=%u\n", scenario, s);
        }
    }
    munmap(rom, 0x100000);
    printf("PASS assertions %s\n", scenario);
    return 0;
}
