/* Production shutdown ordering: no register path bypasses the qualified owner.
 * The real power_source arbiter is included: ACPI and APM soft-off share it. */
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
#include <ciuki/power_source.h>
static struct storage disk;
static struct process desktop;
static int sync_error, quiesce_error, off_error;
static bool qualified, setup_active;
static char order[32], durable[256], last[256];
static unsigned stages, checkpoints;
bool setup_mode(void) { return setup_active; }
static jmp_buf reboot_return;
int power_devices_quiesce(void) __attribute__((weak));
static void stage(char c) { assert(stages<sizeof(order)-1); order[stages++]=c; order[stages]=0; }
struct process *proc_current(void) { return &desktop; }
uint32_t supervisor_desktop_pid(void) { return 42; }
struct storage *storage_get(void) { return &disk; }
static int (*during_sync)(void);
static int nested_result;
int storage_sync(void)
{
    stage('S');
    if (during_sync) { int (*nested)(void)=during_sync; during_sync=0; nested_result=nested(); }
    if (!sync_error) disk.stopped=true;
    return sync_error;
}
void net_shutdown(void) { assert(disk.stopped); stage('N'); }
int power_devices_quiesce(void) { assert(disk.stopped); net_shutdown(); stage('Q'); return quiesce_error; }
/* fx-9b: the audio settings flush (a key-applied master to AUDIO.CFG) runs
 * before the storage flush, only while a test enables the stage. */
static bool audio_stage;
static int audio_error;
int audio_settings_flush(void) __attribute__((weak));
int audio_settings_flush(void) { assert(!disk.stopped); if (audio_stage) stage('A'); return audio_stage ? audio_error : 0; }
static bool provider_can_off(void *ctx) { (void)ctx; return qualified; }
static int provider_off(void *ctx)
{
    (void)ctx; /* every soft-off follows the latch, after S and the last N/Q */
    assert(stages>=3 && (order[0]=='S' || !strncmp(order,"AS",2)) && strstr(order,"NQ") && !power_source_orderly_shutdown(0));
    stage('P'); return off_error;
}
void klog(const char *fmt, ...) { va_list ap; va_start(ap,fmt); vsnprintf(last,sizeof(last),fmt,ap); va_end(ap); }
int blackbox_checkpoint(void) { checkpoints++; strcpy(durable,last); return 0; }
__attribute__((noreturn)) void kernel_reboot(void) { stage('R'); longjmp(reboot_return,1); }
#include "../../src/kernel/core/power_source.c"
#include "../../src/kernel/core/power.c"
static void provide(enum power_source_kind kind)
{
    power_source_release(source.provider, false, 0);
    const struct power_source_ops ops = {.soft_off=provider_off, .can_off=kind == POWER_SOURCE_ACPI ? provider_can_off : NULL};
    assert(!power_source_reserve(kind, &ops) && !power_source_activate(kind));
}
static void reset(void)
{
    disk=(struct storage){.ready=true}; desktop.pid=42; stages=checkpoints=0; audio_stage=false; audio_error=0;
    desktop.setup_capability=false; setup_active=false;
    power_progress=0; stopped=false; power_running=storage_flushed=false; power_requests=0;
    during_sync=0; nested_result=0;
    order[0]=durable[0]=last[0]=0; sync_error=quiesce_error=0; off_error=-EIO; qualified=true;
    provide(POWER_SOURCE_ACPI);
}
/* f9 Setup authority: only Setup's capability holder in Setup mode, and it
 * takes the desktop's orderly path (flush, quiesce, latch, reset or soft-off). */
static void setup_caller(void) { reset(); desktop.pid=7; desktop.setup_capability=true; setup_active=true; }
static void test_setup_authority(void)
{
    reset(); desktop.pid=7; desktop.setup_capability=true;
    assert(power_syscall(CIUKI_POWER_QUERY)==-EPERM && power_syscall(CIUKI_POWER_OFF)==-EPERM && !stages);
    reset(); desktop.pid=7; setup_active=true;
    assert(power_syscall(CIUKI_POWER_QUERY)==-EPERM && power_syscall(CIUKI_POWER_REBOOT)==-EPERM && !stages);
    setup_caller(); assert(power_syscall(CIUKI_POWER_QUERY)==3 && !stages);
    assert(power_source_soft_off()==-EBUSY && !stages); /* no orderly proof yet */
    assert(power_syscall(CIUKI_POWER_OFF)==-EIO && !strcmp(order,"SNQP") && strstr(durable,"acpi soft-off"));
    assert(power_syscall(CIUKI_POWER_QUERY)&CIUKI_POWER_STOPPED);
    setup_caller(); provide(POWER_SOURCE_APM); assert(power_syscall(CIUKI_POWER_QUERY)==3);
    assert(power_syscall(CIUKI_POWER_OFF)==-EIO && !strcmp(order,"SNQP") && strstr(durable,"apm soft-off"));
    setup_caller(); qualified=false;
    assert(power_syscall(CIUKI_POWER_OFF)==-ENOTSUP && !strcmp(order,"SNQ") && strstr(durable,"safe to turn off"));
    for (unsigned action=1;action<=2;action++) {
        setup_caller(); sync_error=-EBUSY;
        assert(power_syscall(action)==-EBUSY && !strcmp(order,"S") && !disk.stopped);
        setup_caller(); quiesce_error=-EBUSY;
        assert(power_syscall(action)==-EBUSY && !strcmp(order,"SNQ"));
    }
    setup_caller(); if (!setjmp(reboot_return)) { (void)power_syscall(CIUKI_POWER_REBOOT); assert(0); }
    assert(!strcmp(order,"SNQR"));
    puts("PASS power runtime Setup authority: capability needs Setup mode, same flush/quiesce/latch path, ACPI and APM soft-off, reset");
}
/* fx-5: the T23 trial of image 9a60215f. The first flush met a transient
 * holder (EBUSY), the desktop retried 250 ms later: the second request runs
 * the whole sequence once. Completed stages are latched; a concurrent
 * request is refused without touching any stage. */
static int second_request(void) { return power_syscall(CIUKI_POWER_OFF); }
static void test_repeated_requests(void)
{
    reset(); sync_error=-EBUSY;
    assert(power_syscall(CIUKI_POWER_OFF)==-EBUSY && !strcmp(order,"S") && !disk.stopped);
    assert(power_syscall(CIUKI_POWER_QUERY)==3); /* no UNCERTAIN: the session continues */
    assert(strstr(durable,"storage flush failed: -16"));
    sync_error=0;
    assert(power_syscall(CIUKI_POWER_OFF)==-EIO && !strcmp(order,"SSNQP") && disk.stopped);
    assert(power_syscall(CIUKI_POWER_QUERY)&CIUKI_POWER_STOPPED);
    assert(strstr(last,"acpi soft-off returned error=-5") && power_requests==2);
    /* After the stopped latch a repeat goes straight to the provider. */
    assert(power_syscall(CIUKI_POWER_OFF)==-EIO && !strcmp(order,"SSNQPP"));
    /* Durable storage is not flushed again when a device stop must be retried. */
    reset(); quiesce_error=-EBUSY;
    assert(power_syscall(CIUKI_POWER_OFF)==-EBUSY && !strcmp(order,"SNQ"));
    assert(power_syscall(CIUKI_POWER_QUERY)&CIUKI_POWER_UNCERTAIN);
    quiesce_error=0;
    assert(power_syscall(CIUKI_POWER_OFF)==-EIO && !strcmp(order,"SNQNQP"));
    /* A request arriving while one runs (here: inside its flush) is refused. */
    reset(); during_sync=second_request;
    assert(power_syscall(CIUKI_POWER_OFF)==-EIO && nested_result==-EBUSY && !strcmp(order,"SNQP"));
    assert(power_requests==1 && !power_running);
    reset(); during_sync=second_request; sync_error=-EBUSY;
    assert(power_syscall(CIUKI_POWER_REBOOT)==-EBUSY && nested_result==-EBUSY && !strcmp(order,"S") && !power_running);
    puts("PASS power runtime fx-5: EBUSY flush retried by a second request completes once (SSNQP), stages latched, stopped repeat goes to the provider, concurrent request refused");
}
/* fx-9b: the pending audio preference is written before the storage flush
 * makes it durable, never after it, and a failure never stops the shutdown. */
static void test_audio_settings_flush(void)
{
    reset(); audio_stage=true;
    assert(power_syscall(CIUKI_POWER_OFF)==-EIO && !strcmp(order,"ASNQP"));
    reset(); audio_stage=true; audio_error=-EROFS;
    assert(power_syscall(CIUKI_POWER_OFF)==-EIO && !strcmp(order,"ASNQP"));
    reset(); audio_stage=true; sync_error=-EBUSY;
    assert(power_syscall(CIUKI_POWER_OFF)==-EBUSY && !strcmp(order,"AS"));
    sync_error=0; assert(power_syscall(CIUKI_POWER_OFF)==-EIO && !strcmp(order,"ASASNQP"));
    reset(); audio_stage=true; quiesce_error=-EBUSY;
    assert(power_syscall(CIUKI_POWER_OFF)==-EBUSY && !strcmp(order,"ASNQ"));
    quiesce_error=0; assert(power_syscall(CIUKI_POWER_OFF)==-EIO && !strcmp(order,"ASNQNQP")); /* durable: not again */
    puts("PASS power runtime fx-9b: audio settings flush before the storage flush, failure tolerated, repeated only while storage is not durable");
}
int main(void)
{
    test_audio_settings_flush();
    reset(); assert(power_source_soft_off()==-EBUSY && !stages); /* no orderly proof yet */
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
    assert(strstr(durable,"safe to turn off") && strstr(durable,"provider=acpi"));
    /* APM uses the same path; a released provider leaves no soft-off. */
    reset(); provide(POWER_SOURCE_APM); assert(power_syscall(CIUKI_POWER_QUERY)==3);
    assert(power_syscall(CIUKI_POWER_OFF)==-EIO && !strcmp(order,"SNQP") && strstr(durable,"apm soft-off"));
    reset(); power_source_release(POWER_SOURCE_ACPI, false, 0); assert(power_syscall(CIUKI_POWER_QUERY)==1);
    assert(power_syscall(CIUKI_POWER_OFF)==-ENOTSUP && !strcmp(order,"SNQ") && strstr(durable,"provider=none"));
    reset(); if (!setjmp(reboot_return)) { (void)power_syscall(CIUKI_POWER_REBOOT); assert(0); }
    assert(!strcmp(order,"SNQR"));
    puts("PASS power runtime: authority, query, flush refusal, quiesce refusal, one ACPI/APM soft-off path after the stopped latch, retained diagnostics, reboot");
    test_setup_authority();
    test_repeated_requests();
    return 0;
}
