/* Real BOOT.LOG watermark queue and timeout beneath the display checkpoint.
 * Storage transport is the existing bounded VFS fake, never FS_HOST waits.
 * SPDX-License-Identifier: GPL-2.0-only */
#define main bootlog_fixture_main
#include "bootlog_test.c"
#undef main
#undef CLOCKS_PER_SEC
#undef WIFEXITED
#undef WEXITSTATUS
#undef WIFSIGNALED
#undef WTERMSIG
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#undef irq_save
#undef irq_restore
#define irq_save() 0x200u
#define irq_restore(f) ((void)(f))
struct ciuki_boot_info g_boot;
struct task *g_current;
volatile uint64_t g_ticks;
static unsigned raw_calls,log_failures;
static char last_line[256]; /* T23 c42f556e: the step record's text */
static bool worker_drains; /* fx-22: the storage worker commits during a wait */
int blackbox_checkpoint(void) { raw_calls++;return 0; }
void task_sleep_ms(uint32_t ms)
{
    model_now+=ms;g_ticks+=ms;
    if (worker_drains && bootlog_due(model_now)) CHECK(!bootlog_drain());
}
uint64_t deadline_after_ms(uint32_t ms) { return g_ticks+ms; }
bool deadline_passed(uint64_t d) { return g_ticks-d<(UINT64_C(1)<<63); }
void klog(const char *format,...)
{
    char line[256];va_list ap;va_start(ap,format);
    int n=vsnprintf(line,sizeof(line),format,ap);va_end(ap);
    CHECK(n>0 && n<(int)sizeof(line));
    memcpy(last_line,line,(size_t)n+1);
    bootlog_capture(line,(size_t)n);
    if (strstr(format,"fullscreen_log error=")) log_failures++;
}
#include "../../src/kernel/core/display.c"
int main(void)
{
    reset_model();CHECK(!bootlog_activate(&fs,true,1));
    unsigned before=calls;
    /* Before: a non-draining sink consumes the full 2000 ms timeout. */
    CHECK(display_fullscreen_checkpoint("watchdog-return",0)==-EAGAIN);
    CHECK(model_now==2000 && raw_calls==1 && calls==before && log_failures==1);
    printf("F1-80 real bootlog before checkpoint_ms=%u result=-11 storage_calls_on_caller=0\n",model_now);
    fullscreen_display=true;display_fullscreen_return_request();
    uint32_t began=model_now;
    /* fx-22: each return step waits at most 50 ms, 200 ms per return, for
     * its commit; then it stays queued. The caller still does no storage
     * call and no raw-trail sync. */
    CHECK(!display_fullscreen_checkpoint("watchdog-return",0));
    CHECK(model_now-began==RECOVERY_SYNC_STEP_MS);
    CHECK(!display_fullscreen_checkpoint("guest-revoke",0));
    CHECK(!display_fullscreen_checkpoint("vga-capture",0));
    CHECK(!display_fullscreen_checkpoint("native-restore-streams",0));
    CHECK(model_now-began==RECOVERY_SYNC_TOTAL_MS);
    CHECK(!display_fullscreen_checkpoint("desktop-restored",0));
    CHECK(model_now-began==RECOVERY_SYNC_TOTAL_MS && calls==before && raw_calls==1 && log_failures==1);
    unsigned queued_ms=model_now-began;
    CHECK(sync_target>sync_done && bootlog_due(model_now));
    uint64_t target=sync_target;
    drain();CHECK(sync_done>=target && commits && published_size);
    CHECK(!bootlog_try_sync());
    printf("fx-22 real bootlog, sink not draining: five return checkpoints_ms=%u (bound %u) result=0 caller_io=0 eventual_commits=%u PASS\n",
           queued_ms,RECOVERY_SYNC_TOTAL_MS,commits);
    /* A draining worker makes each step durable before the next one. */
    fullscreen_return_timed=false;fullscreen_recovering=false;display_fullscreen_return_request();
    worker_drains=true;
    unsigned commits_before=commits;
    began=model_now;
    CHECK(!display_fullscreen_checkpoint("vga-capture",0));
    CHECK(sync_done>=stats.captured && commits>commits_before);
    CHECK(!display_fullscreen_checkpoint("vga-captured",0));
    CHECK(sync_done>=stats.captured && model_now-began<=4);
    worker_drains=false;
    printf("fx-22 real bootlog, draining worker: two durable steps in %u ms, commits=%u PASS\n",
           model_now-began,commits-commits_before);
    /* T23 c42f556e (docs/validation/t23-war2-fullscreen-2026-10-25.md): the
     * first-guest-frame step of a running full screen is recorded and its
     * commit requested, never waited for, even with a sink that does not
     * drain (the durable checkpoint above waits 2000 ms and fails -EAGAIN). */
    fullscreen_return_timed=false;fullscreen_recovering=false;
    unsigned failures=log_failures,raw_before=raw_calls;
    uint64_t requested=sync_target;
    began=model_now;
    CHECK(!display_fullscreen_record("first-guest-frame",0));
    CHECK(model_now==began && raw_calls==raw_before && log_failures==failures);
    CHECK(strstr(last_line,"fullscreen_step=first-guest-frame result=0") && sync_target>requested);
    printf("T23 c42f556e first-guest-frame record, sink not draining: waited %u ms, commit requested PASS\n",
           model_now-began);
    drain();CHECK(sync_done>=sync_target);
    /* An entry's first step counts from its request, not from the previous
     * transaction's last step (T23: vm-park step_ms=4391). */
    worker_drains=true;
    CHECK(!display_fullscreen_checkpoint("desktop-restored",0));
    g_ticks+=4391;
    display_fullscreen_entry_request();
    CHECK(!display_fullscreen_checkpoint("vm-park",0));
    worker_drains=false;
    CHECK(strstr(last_line,"fullscreen_step=vm-park result=0") && strstr(last_line,"step_ms=0"));
    puts("T23 c42f556e entry request: vm-park step_ms=0 after a 4391 ms idle gap PASS");
    return 0;
}
