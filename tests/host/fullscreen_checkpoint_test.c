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
int blackbox_checkpoint(void) { raw_calls++;return 0; }
void task_sleep_ms(uint32_t ms) { model_now+=ms;g_ticks+=ms; }
void klog(const char *format,...)
{
    char line[256];va_list ap;va_start(ap,format);
    int n=vsnprintf(line,sizeof(line),format,ap);va_end(ap);
    CHECK(n>0 && n<(int)sizeof(line));
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
    CHECK(!display_fullscreen_checkpoint("watchdog-return",0));
    CHECK(!display_fullscreen_checkpoint("guest-revoke",0));
    CHECK(!display_fullscreen_checkpoint("native-restore-streams",0));
    CHECK(!display_fullscreen_checkpoint("desktop-restored",0));
    CHECK(model_now==began && calls==before && raw_calls==1 && log_failures==1);
    unsigned queued_ms=model_now-began;
    CHECK(sync_target>sync_done && bootlog_due(model_now));
    uint64_t target=sync_target;
    drain();CHECK(sync_done>=target && commits && published_size);
    CHECK(!bootlog_try_sync());
    printf("F1-80 real bootlog after four_checkpoints_ms=%u result=0 caller_io=0 eventual_commits=%u PASS\n",
           queued_ms,commits);
    return 0;
}
