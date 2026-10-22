/* Production shutdown ordering: no register path bypasses the qualified owner. */
#define POWER_HOST 1
#define POWER_HOST_RUNTIME 1
#define CIUKI_CPU_H
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include <ciuki/kernel.h>
#include <ciuki/mm.h>
#include <ciuki/process.h>
static inline uint32_t irq_save(void) { return 0x200; }
static inline void irq_restore(uint32_t flags) { (void)flags; }
#define P2V(p) ((void *)(uintptr_t)(p))
#define V2P(p) ((uint32_t)(uintptr_t)(p))
#include <ciuki/storage.h>
#include <ciuki/reboot.h>
static struct storage disk;
static struct process desktop;
static int sync_error, quiesce_error, off_error;
static bool qualified;
static char order[32], durable[256], last[256];
static unsigned stages, checkpoints;
static jmp_buf reboot_return;
bool acpi_power_can_off(void) __attribute__((weak));
int acpi_power_off_owned(void) __attribute__((weak));
int power_devices_quiesce(void) __attribute__((weak));
static void stage(char c) { assert(stages<sizeof(order)-1); order[stages++]=c; order[stages]=0; }
struct process *proc_current(void) { return &desktop; }
uint32_t supervisor_desktop_pid(void) { return 42; }
struct storage *storage_get(void) { return &disk; }
int storage_sync(void) { stage('S'); if (!sync_error) disk.stopped=true; return sync_error; }
void net_shutdown(void) { assert(disk.stopped); stage('N'); }
int power_devices_quiesce(void) { assert(disk.stopped); net_shutdown(); stage('Q'); return quiesce_error; }
bool acpi_power_can_off(void) { return qualified; }
int acpi_power_off_owned(void) { assert(!strcmp(order,"SNQ")); stage('P'); return off_error; }
void klog(const char *fmt, ...) { va_list ap; va_start(ap,fmt); vsnprintf(last,sizeof(last),fmt,ap); va_end(ap); }
int blackbox_checkpoint(void) { checkpoints++; strcpy(durable,last); return 0; }
__attribute__((noreturn)) void kernel_reboot(void) { stage('R'); longjmp(reboot_return,1); }
#include "../../src/kernel/core/power.c"
static void reset(void)
{
    disk=(struct storage){.ready=true}; desktop.pid=42; stages=checkpoints=0;
    power_progress=0;
    order[0]=durable[0]=last[0]=0; sync_error=quiesce_error=0; off_error=-EIO; qualified=true;
}
int main(void)
{
    reset(); desktop.pid=7; assert(power_syscall(CIUKI_POWER_OFF)==-EPERM && !stages);
    reset(); assert(power_syscall(99)==-EINVAL && !stages);
    assert(power_syscall(CIUKI_POWER_QUERY)==3 && !stages);
    qualified=false; assert(power_syscall(CIUKI_POWER_QUERY)==1 && !stages);
    for (unsigned action=1;action<=2;action++) {
        reset(); sync_error=-EBUSY;
        assert(power_syscall(action)==-EBUSY && !strcmp(order,"S") && !disk.stopped);
        assert(strstr(durable,"storage flush failed"));
        reset(); sync_error=-EIO; assert(power_syscall(action)==-EIO && !strcmp(order,"S"));
        reset(); quiesce_error=-EBUSY; assert(power_syscall(action)==-EBUSY && !strcmp(order,"SNQ"));
        assert(power_syscall(CIUKI_POWER_QUERY)&CIUKI_POWER_UNCERTAIN);
        assert(strstr(durable,"native quiesce failed"));
        reset(); quiesce_error=-EIO; assert(power_syscall(action)==-EBUSY && !strcmp(order,"SNQ"));
    }
    reset(); assert(power_syscall(CIUKI_POWER_OFF)==-EIO && !strcmp(order,"SNQP"));
    assert(power_syscall(CIUKI_POWER_QUERY)&CIUKI_POWER_STOPPED);
    assert(strstr(durable,"stopped devices and leases retained") && checkpoints==2);
    reset(); qualified=false;
    assert(power_syscall(CIUKI_POWER_OFF)==-ENOTSUP && !strcmp(order,"SNQ"));
    assert(strstr(durable,"safe to turn off"));
    reset(); if (!setjmp(reboot_return)) { (void)power_syscall(CIUKI_POWER_REBOOT); assert(0); }
    assert(!strcmp(order,"SNQR"));
    puts("PASS power runtime: authority, query, flush refusal, quiesce refusal, S5 worker ordering, retained diagnostics, reboot");
    return 0;
}
