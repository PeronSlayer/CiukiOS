/* Production power syscall with fake storage/port/time boundaries. */
#define POWER_HOST 1
#define POWER_HOST_RUNTIME 1
#define CIUKI_CPU_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <ciuki/kernel.h>
#include <ciuki/mm.h>
#include <ciuki/process.h>
static uint32_t irq_save(void);
static void irq_restore(uint32_t);
#define P2V(p) ((void *)(uintptr_t)(p))
#define V2P(p) ((uint32_t)(uintptr_t)(p))
#include <ciuki/storage.h>
#include <ciuki/reboot.h>
static unsigned failures, sync_calls, writes, enables, sleeps, reboots;
static int sync_error;
static unsigned ownership_after;
static uint16_t pm1a, pm1b;
static uint32_t irq_flags = 0x200;
static bool timeout_logged;
static struct storage disk;
static struct process desktop;
struct ciuki_boot_info g_boot;
static jmp_buf reboot_return;
#define CHECK(c) do { if (!(c)) { printf("FAIL %d %s\n", __LINE__, #c); failures++; } } while (0)
struct process *proc_current(void) { return &desktop; }
uint32_t supervisor_desktop_pid(void) { return 42; }
struct storage *storage_get(void) { return &disk; }
int storage_sync(void) { sync_calls++; disk.stopped = true; return sync_error; }
void *vmm_map_mmio(uint32_t p, uint32_t n, bool wc) { (void)p; (void)n; (void)wc; return 0; }
static uint32_t irq_save(void) { uint32_t f = irq_flags; irq_flags = 0; return f; }
static void irq_restore(uint32_t f) { irq_flags = f; }
static uint16_t inw(uint16_t p) { CHECK(p == 0x604 || p == 0x644); return p == 0x604 ? pm1a : pm1b; }
static void outw(uint16_t p, uint16_t v) { CHECK(!irq_flags && (p == 0x604 || p == 0x644)); (void)v; writes++; }
static void outb(uint16_t p, uint8_t v) { CHECK(p == 0xB2 && v == 0xF1); enables++; }
void task_sleep_ms(uint32_t ms)
{
    CHECK(irq_flags == 0x200);
    if (ms == 10 && ++sleeps == ownership_after) pm1a = pm1b = 1;
}
void klog(const char *fmt, ...)
{
    if (strstr(fmt, "SCI_EN")) timeout_logged = true;
}
__attribute__((noreturn)) void kernel_reboot(void) { reboots++; longjmp(reboot_return, 1); }
#include "../../src/kernel/core/power.c"
static void reset_power(void)
{
    disk = (struct storage){.ready = true}; desktop.pid = 42;
    soft_off = (struct acpi_soft_off){.pm1a_cnt=0x604, .smi_cmd=0xB2, .acpi_enable=0xF1, .slp_typa=5, .slp_typb=5};
    soft_off_state = 1;
    sync_calls = writes = enables = sleeps = reboots = 0;
    pm1a = pm1b = 1; sync_error = 0; ownership_after = 0; timeout_logged = false;
}
static void test_power_sync_retry(void)
{
    for (unsigned action = CIUKI_POWER_REBOOT; action <= CIUKI_POWER_OFF; action++) {
        for (unsigned error = 0; error < 2; error++) {
            reset_power(); sync_error = error ? -EIO : -EBUSY;
            for (unsigned attempt = 0; attempt < 2; attempt++) {
                if (!setjmp(reboot_return)) CHECK(power_syscall(action) == sync_error);
                else CHECK(false);
                CHECK(sync_calls == attempt + 1 && !writes && !reboots);
            }
            sync_error = 0;
            if (!setjmp(reboot_return)) {
                CHECK(power_syscall(action) == -EIO); CHECK(action == CIUKI_POWER_OFF);
            } else CHECK(action == CIUKI_POWER_REBOOT);
            CHECK(sync_calls == 3 && (action == CIUKI_POWER_OFF ? writes == 2 : reboots == 1));
        }
    }
    reset_power(); desktop.pid = 7; CHECK(power_syscall(CIUKI_POWER_OFF) == -EPERM && !sync_calls && !writes);
    puts("power sync retry: EBUSY/EIO retained, every request syncs, hardware only after success PASS");
}
static void test_power_sci(void)
{
    for (unsigned form = 0; form < 3; form++) {
        reset_power(); pm1a = 0;
        if (form == 1) soft_off.smi_cmd = 0;
        if (form == 2) { pm1a = 1; pm1b = 0; soft_off.pm1b_cnt = 0x644; }
        if (!setjmp(reboot_return)) { (void)power_syscall(CIUKI_POWER_OFF); CHECK(false); }
        CHECK(!writes && reboots == 1 && timeout_logged);
        CHECK(sleeps == (form == 1 ? 0u : 300u));
    }
    reset_power(); pm1a = pm1b = 0; soft_off.pm1b_cnt = 0x644; ownership_after = 3;
    CHECK(power_syscall(CIUKI_POWER_OFF) == -EIO && writes == 4 && enables == 1 && sleeps == 3 && !reboots);
    reset_power(); CHECK(power_syscall(CIUKI_POWER_OFF) == -EIO && writes == 2 && !enables);
    puts("power SCI_EN: missing handshake/PM1a/PM1b timeout refuses PM writes and reboots, ownership succeeds PASS");
}
int main(void)
{
    if (!getenv("POWER_REVIEW") || !strcmp(getenv("POWER_REVIEW"), "5")) test_power_sync_retry();
    if (!getenv("POWER_REVIEW") || !strcmp(getenv("POWER_REVIEW"), "7")) test_power_sci();
    printf("power runtime: %u failures\n", failures); return failures != 0;
}
