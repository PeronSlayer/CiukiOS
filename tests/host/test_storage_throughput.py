"""Replay the production storage task with a fake clock and storage sinks."""
import os
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


if __name__ == '__main__':
    unittest.main()
