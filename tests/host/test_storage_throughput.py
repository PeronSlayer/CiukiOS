"""Replay the production storage task with a fake clock and storage sinks."""
import os
import re
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class StorageTaskTests(unittest.TestCase):
    def test_log_and_writeback_run_in_storage_task(self):
        if not shutil.which('clang'):
            self.skipTest('clang unavailable')
        source = (ROOT / 'src/kernel/fs/mount.c').read_text()
        begin = source.index('static void storage_timer(void *arg)\n')
        end = source.index('\n/* f9-02: a published destination needs the writer', begin)
        # dec1/G29d: the start-log task follows the timer; it is not part of the slice.
        startup = source.find('\n/* G29d: program starts reach CIUKIST.LOG', begin)
        if 0 <= startup < end: end = startup
        # Compile the exact function. All substitutions are outside it at
        # the scheduler/clock/log/cache boundary; no modeled work loop.
        function = source[begin:end]
        prefix = r'''
#include <ciuki/storage.h>
#include <ciuki/bootlog.h>
#include <ciuki/blackbox.h>
#pragma weak blackbox_flush /* as in mount.c: absent on the host, so the timer skips it */
#include <ciuki/bootstat.h>
#pragma weak bootstat_service /* G14, as in mount.c: absent here */
#include <assert.h>
#include <setjmp.h>
static struct storage s;
static uint32_t last_log;
static unsigned steps, drains, writebacks, due_checks;
static uint32_t clock_ms;
static bool written;
static jmp_buf exited;
static uint32_t fixture_now(void) { return clock_ms; }
#define fs_now_ms fixture_now
void cache_writer_wait(struct block_cache *cache, uint32_t ms) {
    const uint32_t times[]={10,100,1010,5000,5100};
    assert(cache==&s.cache && ms==10 && steps<5);
    clock_ms=times[steps++];
    if (steps==5) s.stopped=true;
}
static void task_exit(int status) { assert(status==0); longjmp(exited,1); }
bool bootlog_due(uint32_t now) {
    due_checks++;
    return !last_log || (uint32_t)(now-last_log)>=BOOTLOG_DRAIN_MS;
}
int bootlog_drain(void) {
    assert(!s.stopped);
    assert(clock_ms==10 || clock_ms==1010 || clock_ms==5000);
    drains++; last_log=clock_ms; clock_ms+=50; /* sleeping transfer, no shared work callback */
    return 0;
}
bool cache_writeback_due(const struct block_cache *c, uint32_t now) {
    assert(c==&s.cache);
    return !written && now>=5000;
}
void klog(const char *fmt, ...) { (void)fmt; } /* fx-23 writer_busy line */
unsigned storage_hold_watch(struct storage *arg, uint32_t now) { (void)arg; (void)now; return 0; } /* fx-23 */
unsigned storage_census_service(struct storage *arg, uint32_t now) { (void)arg; (void)now; return 0; } /* fx-17b */
/* T23 2ca3b0b6: the progress stamp cpustat reads (mount.c); each pass is
 * stamped at its start and at every stage, the task at start and exit. */
static unsigned progress_start, progress_stop, progress_idle, progress_bootlog;
void storage_writer_progress(struct storage *arg, const char *stage) {
    assert(arg==&s);
    progress_start+=!strcmp(stage,"start"); progress_stop+=!strcmp(stage,"stop");
    progress_idle+=!strcmp(stage,"idle"); progress_bootlog+=!strcmp(stage,"bootlog");
}
int storage_writeback(struct storage *arg, uint32_t now) {
    assert(arg==&s && !s.stopped);
    assert(now==60 || now==100 || now==1060 || now==5050);
    writebacks++;
    if (now>=5000) written=true;
    return 0;
}
'''
        suffix = r'''
int main(void) {
    if (!setjmp(exited)) storage_timer(&s);
    assert(steps==5 && due_checks==5 && drains==3 && writebacks==4 && written);
    assert(last_log==5000);
    assert(progress_start==1 && progress_idle==5 && progress_bootlog>=drains && progress_stop==1);
    return 0;
}
'''
        scratch = ROOT / 'build/host/storage-task'
        scratch.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scratch) as temp:
            temp = Path(temp)
            path = temp / 'storage-task.c'
            path.write_text(prefix + function + suffix)
            env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0:halt_on_error=1',
                       UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
            subprocess.run(['clang', '-DFS_HOST', '-D_POSIX_C_SOURCE=200809L',
                            '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-pthread',
                            '-I' + str(ROOT / 'src/kernel/include'), str(path),
                            '-o', str(temp / 'storage-task')], check=True, env=env)
            subprocess.run([str(temp / 'storage-task')], check=True, env=env)

    def test_stall_poll_never_waits_for_storage(self):
        # T23 2ca3b0b6 review P1: the cpustat stall poll observes a stuck
        # storage writer or executor; nothing it calls may wait for either.
        source = (ROOT / 'src/kernel/fs/mount.c').read_text()
        begin = source.index('unsigned storage_stall_poll(uint32_t now)\n')
        end = source.index('\n}\n', begin)
        body = source[begin:end]
        for forbidden in ('blackbox_checkpoint', 'blackbox_flush(', 'klog(', 'vfs_', 'fs_lock_take',
                          'fs_event_wait', 'kwait_wait', 'task_sleep', 'blk_', 'cache_', 'storage_writeback'):
            self.assertNotIn(forbidden, body, forbidden)
        self.assertIn('klog_periodic(', body)
        self.assertIn('blackbox_flush_request()', body)
        # The request is a counter and a wake (blackbox.c, kernel build).
        blackbox = (ROOT / 'src/kernel/core/blackbox.c').read_text()
        kernel = blackbox[blackbox.index('#else\nstatic struct task *flusher;'):]
        request = kernel[kernel.index('void blackbox_flush_request(void) {'):]
        request = request[:request.index('\n}\n')]
        for forbidden in ('checkpoint', 'flush_records', 'kwait_wait', 'task_sleep', 'fs_lock'):
            self.assertNotIn(forbidden, request, forbidden)
        self.assertIn('kwait_wake_all(&flusher_wait)', request)
        # Its records are protected boot-log lines, never budgeted statistics.
        bootlog = (ROOT / 'src/kernel/core/bootlog.c').read_text()
        table = bootlog[bootlog.index('statistic_periodic[]={'):]
        prefixes = re.findall(r'"([^"]+)"', table[:table.index('};')])
        for record in ('[storage] writer_stall ', '[storage] lock_wait '):
            self.assertFalse(any(record.startswith(prefix) for prefix in prefixes), record)


if __name__ == '__main__':
    unittest.main()
