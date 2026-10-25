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
/* T23 c42f556e forced actions: a scripted clock and bounded flush worker. */
static uint64_t fake_ms;
static unsigned force_flushes;
static int force_flush_result; /* -EAGAIN: never completes */
uint64_t deadline_after_ms(uint32_t ms) { return fake_ms + ms; }
bool deadline_passed(uint64_t deadline) { return fake_ms >= deadline; }
void task_sleep_ms(uint32_t ms) { fake_ms += ms; }
int storage_sync_bounded(uint64_t end)
{
    if (!force_flushes++) stage('F');
    (void)end; return force_flush_result;
}
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
/* G11 (docs/design/power-policy.md): the drive timer through a scripted
 * ATA backend, the standby qualification, the EVENTS counters. */
static unsigned g11_fixed, g11_refuse, g11_calls;
static uint32_t g11_seconds;
int ata_standby_timer_all(uint32_t seconds, struct ata_standby_result *out)
{
    uint32_t applied; (void)ata_standby_count(seconds, &applied);
    g11_calls++; g11_seconds = seconds;
    *out = (struct ata_standby_result){0, 0, applied, 0};
    for (unsigned i = 0; i < g11_fixed; i++) {
        if (i < g11_refuse) { out->refused++; if (!out->error) out->error = -95; }
        else out->disks++;
    }
    return (int)g11_fixed;
}
static bool g11_resume, g11_capable;
static int g11_standby_result, g11_standbys;
static bool g11_nested;
bool power_devices_resume_ready(void) { return g11_resume; }
static bool g11_can_standby(void *ctx) { (void)ctx; return g11_capable; }
static int g11_standby(void *ctx) { (void)ctx; g11_standbys++;
    if (g11_nested) {
        assert(power_syscall(CIUKI_POWER_STANDBY)==-EBUSY);
        assert(power_syscall(CIUKI_POWER_CANCEL_STANDBY)==-EBUSY);
        assert(power_syscall(CIUKI_POWER_OFF)==-EBUSY);
        assert(power_syscall(CIUKI_POWER_REBOOT)==-EBUSY);
    }
    return g11_standby_result; }
static unsigned g11_cancels;
static int g11_cancel(void *ctx) { (void)ctx;g11_cancels++;return 0; }
static void g11_provide(enum power_source_kind kind, bool sleep)
{
    power_source_release(source.provider, false, 0);
    const struct power_source_ops ops = {.soft_off=provider_off, .can_standby=sleep ? g11_can_standby : NULL,
                                         .standby=sleep ? g11_standby : NULL, .cancel_standby=g11_cancel};
    assert(!power_source_reserve(kind, &ops) && !power_source_activate(kind));
}
static void test_g11(void)
{
    /* ATA/ATAPI-6 8.14 IDLE Count: 5 s units to 240, then 30 min units to
     * 251 (5 h 30 min), always rounded up and clamped. */
    static const uint32_t in[] = {0, 1, 5, 6, 60, 1200, 1201, 1800, 1801, 3600, 18000, 19800, 99999};
    static const uint8_t count[] = {0, 1, 1, 2, 12, 240, 241, 241, 242, 242, 250, 251, 251};
    static const uint32_t applied[] = {0, 5, 5, 10, 60, 1200, 1800, 1800, 3600, 3600, 18000, 19800, 19800};
    for (unsigned i = 0; i < sizeof(in)/sizeof(*in); i++) {
        uint32_t a = 1; assert(ata_standby_count(in[i], &a) == count[i] && a == applied[i]);
    }
    /* Standby qualification: provider, firmware capability, device resume. */
    reset(); g11_resume = g11_capable = false; g11_standbys = 0;
    assert(power_source_standby_reason() == CIUKI_POWER_STANDBY_NO_FIRMWARE);
    assert(!(power_syscall(CIUKI_POWER_QUERY) & CIUKI_POWER_CAN_STANDBY));
    assert(power_syscall(CIUKI_POWER_STANDBY) == -ENOTSUP && strstr(last, "standby refused reason=2") && !stages);
    g11_provide(POWER_SOURCE_APM, true);
    assert(power_source_standby_reason() == CIUKI_POWER_STANDBY_NO_FIRMWARE); /* no 5310h capability */
    g11_capable = true;
    assert(power_source_standby_reason() == CIUKI_POWER_STANDBY_NO_RESUME);
    assert(power_syscall(CIUKI_POWER_STANDBY) == -ENOTSUP && !g11_standbys);
    g11_resume = true;
    assert(power_source_standby_reason() == CIUKI_POWER_STANDBY_AVAILABLE);
    assert(power_syscall(CIUKI_POWER_QUERY) == (3 | CIUKI_POWER_CAN_STANDBY));
    g11_nested=true; g11_standby_result = 0; assert(power_syscall(CIUKI_POWER_STANDBY) == 0 && g11_standbys == 1 && !stages);
    assert(!power_syscall(CIUKI_POWER_CANCEL_STANDBY) && g11_cancels==1);
    g11_nested=false; g11_standby_result = -EIO; assert(power_syscall(CIUKI_POWER_STANDBY) == -EIO && g11_standbys == 2);
    desktop.pid = 7; assert(power_syscall(CIUKI_POWER_STANDBY) == -EPERM && g11_standbys == 2); desktop.pid = 42;
    /* After the orderly stop nothing can stand by. */
    assert(power_syscall(CIUKI_POWER_OFF) == -EIO && !strcmp(order, "SNQP"));
    assert(!(power_syscall(CIUKI_POWER_QUERY) & CIUKI_POWER_CAN_STANDBY));
    assert(power_syscall(CIUKI_POWER_STANDBY) == -EBUSY && g11_standbys == 2);
    reset(); power_source_release(POWER_SOURCE_ACPI, false, 0);
    assert(power_source_standby_reason() == CIUKI_POWER_STANDBY_NO_PROVIDER);
    /* APM event counters: boot-monotonic, APM only, active provider only. */
    reset(); g11_provide(POWER_SOURCE_APM, false);
    struct power_source_counters before, after; power_source_counters(&before);
    static const uint16_t events[] = {1, 2, 9, 10, 5, 8, 3, 4, 11, 6, 12, 7, 0x0200};
    for (unsigned i = 0; i < sizeof(events)/sizeof(*events); i++) power_source_event(POWER_SOURCE_APM, events[i], 0);
    power_source_event(POWER_SOURCE_ACPI, 5, 0);
    power_source_counters(&after);
    assert(after.sleep_requests - before.sleep_requests == 4 && after.battery_low - before.battery_low == 1 &&
           after.critical - before.critical == 1 && after.resumes - before.resumes == 3 &&
           after.status_changes - before.status_changes == 2);
    g11_provide(POWER_SOURCE_APM, false); /* a new reservation keeps the counts */
    power_source_counters(&before); assert(!memcmp(&before, &after, sizeof(before)));
    struct ciuki_power_events ev; memset(&ev, 0xa5, sizeof(ev));
    disk_seconds = CIUKI_POWER_UNKNOWN; power_events_fill(&ev);
    assert(ev.sleep_requests == after.sleep_requests && ev.battery_low == after.battery_low && ev.critical == after.critical &&
           ev.resumes == after.resumes && ev.status_changes == after.status_changes &&
           ev.standby_reason == CIUKI_POWER_STANDBY_NO_FIRMWARE && ev.disk_seconds == CIUKI_POWER_UNKNOWN && !ev.reserved);
    /* POLICY: validated record, every fixed disk, refusals counted. */
    struct ciuki_power_policy pol = {.version = CIUKI_POWER_POLICY_VERSION, .size = sizeof(pol), .disk_seconds = 1500};
    struct ciuki_power_policy bad = pol; bad.version = 2; assert(power_policy_apply(&bad) == -EINVAL);
    bad = pol; bad.size = 28; assert(power_policy_apply(&bad) == -EINVAL);
    bad = pol; bad.reserved = 1; assert(power_policy_apply(&bad) == -EINVAL);
    bad = pol; bad.disk_seconds = 19801; assert(power_policy_apply(&bad) == -EINVAL);
    assert(!g11_calls);
    g11_fixed = 0; assert(power_policy_apply(&pol) == -ENODEV && disk_seconds == CIUKI_POWER_UNKNOWN);
    g11_fixed = 2; g11_refuse = 1;
    assert(power_policy_apply(&pol) == 0 && pol.disks == 1 && pol.refused == 1 && pol.applied_seconds == 1800 &&
           pol.error == -95 && g11_seconds == 1500 && disk_seconds == 1800 && strstr(last, "policy disk_seconds=1500 applied=1800"));
    g11_refuse = 2; pol.disk_seconds = 0;
    assert(power_policy_apply(&pol) == -95 && !pol.disks && pol.refused == 2 && disk_seconds == 1800);
    g11_refuse = 0; assert(power_policy_apply(&pol) == 0 && pol.disks == 2 && disk_seconds == 0);
    power_events_fill(&ev); assert(ev.disk_seconds == 0);
    /* GetSystemPowerStatus values of the record (SYSTEM_POWER_STATUS). */
    struct power_source_win32 w;
    reset(); power_source_release(POWER_SOURCE_ACPI, false, 0); acpi_record_valid = false;
    power_source_win32_status(&w); assert(w.ac == 255 && w.battery_flag == 255 && w.percent == 255 && w.seconds == UINT32_MAX);
    g11_provide(POWER_SOURCE_APM, false);
    struct power_source_status apm = {.ac = 0, .percent = 78, .seconds = 5100, .battery_flags = 1, .units = 2,
        .battery = {{1, 78, 5100, 1}, {0, POWER_SOURCE_UNKNOWN, POWER_SOURCE_UNKNOWN, 0x10}}};
    power_source_publish(POWER_SOURCE_APM, &apm);
    power_source_win32_status(&w); assert(w.ac == 0 && w.battery_flag == 1 && w.percent == 78 && w.seconds == 5100);
    apm.ac = 2; apm.battery[0] = (struct power_source_battery){1, 4, 300, 4}; power_source_publish(POWER_SOURCE_APM, &apm);
    power_source_win32_status(&w); assert(w.ac == 0 && w.battery_flag == 6 && w.percent == 4 && w.seconds == 300);
    apm.ac = 1; apm.battery[0] = (struct power_source_battery){1, 40, 9000, 8}; power_source_publish(POWER_SOURCE_APM, &apm);
    power_source_win32_status(&w); assert(w.ac == 1 && w.battery_flag == 8 && w.percent == 40 && w.seconds == UINT32_MAX);
    apm.units = 1; apm.battery[0] = (struct power_source_battery){0, POWER_SOURCE_UNKNOWN, POWER_SOURCE_UNKNOWN, 0x10};
    power_source_publish(POWER_SOURCE_APM, &apm);
    power_source_win32_status(&w); assert(w.ac == 1 && w.battery_flag == 128 && w.percent == 255);
    puts("PASS power runtime G11: ATA standby count encoding, standby reasons (provider, firmware, device resume), CAN_STANDBY, STANDBY authority and stop latch, APM event counters, EVENTS record, POLICY validation and refusals, SYSTEM_POWER_STATUS");
}
/* T23 trial c42f556e: a REBOOT/OFF that cannot finish (a disk operation or
 * a program that does not end) must not leave the machine without a way to
 * stop: the forced actions run beside the blocked call, flush for at most
 * POWER_FORCE_FLUSH_MS through the bounded worker, latch UNCERTAIN, then
 * reset or soft-off. Before them, call 73 action 9/10 was EINVAL. */
static unsigned forced_offs, ordinary_offs;
static int forced_off(void *ctx) { (void)ctx; stage('P'); forced_offs++; return -EIO; }
static int ordinary_off(void *ctx) { (void)ctx; ordinary_offs++; return -EIO; }   /* must never run on the forced path */
/* Review 2, finding 5: the platform's nonblocking device fence (power_platform.c
 * quiesce step) as the forced actions see it: -EAGAIN while the stop runs. */
static int fence_result; static unsigned fence_steps;
static int fake_fence(void *ctx, uint64_t end) { (void)ctx; (void)end; if (!fence_steps++) stage('D'); return fence_result; }
static int fake_resume(void *ctx, uint16_t event, uint16_t info) { (void)ctx; (void)event; (void)info; return 0; }
static void test_forced(void)
{
    { const struct power_source_platform services={.quiesce_dma=fake_fence,.resume=fake_resume};
      assert(!power_source_set_platform(&services)); }
    fence_result=0; fence_steps=0;
    reset(); fake_ms=0; force_flushes=0; force_flush_result=0;
    desktop.pid=7; assert(power_syscall(CIUKI_POWER_FORCE_REBOOT)==-EPERM && !stages);
    reset(); force_flushes=0; power_running=true;   /* an orderly OFF blocked on another thread */
    assert(power_syscall(CIUKI_POWER_OFF)==-EBUSY && !stages);
    if (!setjmp(reboot_return)) { (void)power_syscall(CIUKI_POWER_FORCE_REBOOT); assert(0); }
    assert(!strcmp(order,"FDR") && (power_progress&CIUKI_POWER_UNCERTAIN) && strstr(last,"forced-reset"));
    /* A flush worker that never completes ends at its 3 s bound; a fence
     * that never completes ends at its 2 s bound and a reset still follows. */
    reset(); force_flushes=0; fence_steps=0; force_flush_result=-EAGAIN; fence_result=-EAGAIN; fake_ms=1000; power_running=true;
    if (!setjmp(reboot_return)) { (void)power_syscall(CIUKI_POWER_FORCE_REBOOT); assert(0); }
    assert(!strcmp(order,"FDR") && fake_ms>=1000+POWER_FORCE_FLUSH_MS+POWER_FORCE_QUIESCE_MS && fake_ms<1000+POWER_FORCE_FLUSH_MS+POWER_FORCE_QUIESCE_MS+40);
    assert(force_flushes>=POWER_FORCE_FLUSH_MS/10 && force_flushes<=POWER_FORCE_FLUSH_MS/10+2);
    assert(fence_steps>=POWER_FORCE_QUIESCE_MS/10 && fence_steps<=POWER_FORCE_QUIESCE_MS/10+2);
    /* Forced off with the fence timing out: refused (EBUSY), no soft-off, no
     * proof published (review 2, finding 5: active DMA never reaches the BIOS). */
    reset(); force_flushes=0; fence_steps=0; force_flush_result=0; fence_result=-EAGAIN; power_running=true; forced_offs=0; ordinary_offs=0;
    power_source_release(source.provider, false, 0);
    { const struct power_source_ops ops = {.soft_off=ordinary_off,.soft_off_forced=forced_off};
      assert(!power_source_reserve(POWER_SOURCE_ACPI, &ops) && !power_source_activate(POWER_SOURCE_ACPI)); }
    assert(power_syscall(CIUKI_POWER_FORCE_OFF)==-EBUSY && !strcmp(order,"FD") && !forced_offs && !ordinary_offs);
    assert(!power_source_emergency_stopped() && power_source_orderly_shutdown(0)==-EBUSY && strstr(last,"power button"));
    assert(power_syscall(CIUKI_POWER_QUERY)&CIUKI_POWER_UNCERTAIN);
    /* The fence proved every device stopped: the emergency latch (not the
     * orderly one), then the provider's soft-off. */
    reset(); force_flushes=0; fence_steps=0; fence_result=0; power_running=true; forced_offs=0; ordinary_offs=0;
    power_source_release(source.provider, false, 0);
    { const struct power_source_ops ops = {.soft_off=ordinary_off,.soft_off_forced=forced_off};
      assert(!power_source_reserve(POWER_SOURCE_ACPI, &ops) && !power_source_activate(POWER_SOURCE_ACPI)); }
    assert(power_syscall(CIUKI_POWER_FORCE_OFF)==-EIO && !strcmp(order,"FDP") && forced_offs==1 && !ordinary_offs);
    /* Review 3, finding 4: the emergency latch is not the orderly proof (APM
     * reads that one before BIOS Off) and the ordinary entry refuses it. */
    assert(power_source_emergency_stopped() && !stopped && power_source_orderly_shutdown(0)==-EBUSY && power_source_soft_off()==-EBUSY && !ordinary_offs);
    assert(power_source_soft_off_forced()==-EIO && forced_offs==2);
    emergency_stopped=false;
    assert(power_source_soft_off_forced()==-EBUSY && forced_offs==2);
    /* A provider without a forced entry refuses the forced Off (ENOTSUP) with the latch set. */
    reset(); force_flushes=0; fence_steps=0; fence_result=0; power_running=true; forced_offs=0;
    power_source_release(source.provider, false, 0);
    { const struct power_source_ops ops = {.soft_off=ordinary_off};
      assert(!power_source_reserve(POWER_SOURCE_ACPI, &ops) && !power_source_activate(POWER_SOURCE_ACPI)); }
    assert(power_syscall(CIUKI_POWER_FORCE_OFF)==-ENOTSUP && !strcmp(order,"FD") && !ordinary_offs);
    emergency_stopped=false;
    reset(); force_flushes=0; fence_steps=0; power_running=true; power_source_release(source.provider, false, 0);
    assert(power_syscall(CIUKI_POWER_FORCE_OFF)==-ENOTSUP && !strcmp(order,"FD"));
    /* Storage already durable: no second flush; the fence still runs. */
    reset(); force_flushes=0; fence_steps=0; storage_flushed=true;
    if (!setjmp(reboot_return)) { (void)power_syscall(CIUKI_POWER_FORCE_REBOOT); assert(0); }
    assert(!strcmp(order,"DR") && !force_flushes);
    /* No platform (a safe-mode boot): the fence is unavailable, a forced off is refused. */
    platform_ready=false; fence_steps=0;
    reset(); force_flushes=0; power_running=true; forced_offs=0;
    power_source_release(source.provider, false, 0);
    { const struct power_source_ops ops = {.soft_off=forced_off};
      assert(!power_source_reserve(POWER_SOURCE_ACPI, &ops) && !power_source_activate(POWER_SOURCE_ACPI)); }
    assert(power_syscall(CIUKI_POWER_FORCE_OFF)==-EBUSY && !strcmp(order,"F") && !fence_steps && !forced_offs);
    emergency_stopped=false;
    puts("PASS power runtime T23 forced: runs beside a blocked REBOOT/OFF, flush bounded to 3 s, device fence bounded to 2 s, reset regardless, soft-off only on a proved fence through the provider's forced entry (the orderly proof stays strict), desktop only");
}
int main(void)
{
    test_forced();
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
    test_g11();
    return 0;
}
