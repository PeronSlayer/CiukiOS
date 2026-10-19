"""F0-20 stall regressions ported to f0-21 scheduler/CPU accounting."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from vm_creation_model import write_model

ROOT = Path(__file__).resolve().parents[2]


class SystemStallsTests(unittest.TestCase):
    def test_authorized_desktop_stall_emits_immediately(self):
        source = (ROOT / 'src/kernel/core/syscall.c').read_text()
        begin = source.index('static int32_t sys_debug_write(')
        end = source.index('\nstatic int32_t sys_probe_report(', begin)
        harness = r'''
#include <assert.h>
#include <string.h>
#include <ciuki/kernel.h>
#include <ciuki/task.h>
#include <ciuki/process.h>
#include <ciuki/cpustat.h>
static struct task task;
struct task *g_current=&task;
static struct process desktop;
static bool owner=true,range_ok=true,copy_ok=true;
static unsigned logs,forced;
static const char *input;
struct process *proc_current(void) { return &desktop; }
#pragma weak grants_display_owner
bool grants_display_owner(const struct process *p) { assert(p==&desktop);return owner; }
bool as_range_ok(const struct aspace *as,uint32_t va,uint32_t n,bool write)
{ assert(as==&task.as && va==0x800000 && n<=240 && !write);return range_ok; }
int copy_user(void *out,const void *in,uint32_t n)
{ assert((uintptr_t)in==0x800000);if(!copy_ok)return -1;memcpy(out,input,n);return 0; }
void klog(const char *fmt,...) { assert(!strcmp(fmt,"[user %u] %s"));logs++; }
void cpu_stall_record(const char *text,size_t bytes)
{ assert(owner && logs==forced+1 && bytes==strlen(input) && !memcmp(text,input,bytes));forced++; }
'''
        harness += source[begin:end] + r'''
int main(void)
{
    input="[desktop] stall stage=shell worker=wallpaper/settings no_frame_ms=2100 input_pending=1";
    unsigned n=(unsigned)strlen(input);
    assert(sys_debug_write(0x800000,n)==(int)n && forced==1);
    owner=false;assert(sys_debug_write(0x800000,n)==(int)n && forced==1);
    owner=true;input="ordinary record";assert(sys_debug_write(0x800000,15)==15 && forced==1);
    range_ok=false;assert(sys_debug_write(0x800000,15)==-EFAULT && logs==3);
    range_ok=true;copy_ok=false;assert(sys_debug_write(0x800000,15)==-EFAULT && logs==3);
    assert(sys_debug_write(0x800000,241)==-EINVAL && logs==3);
    return 0;
}
'''
        base = ROOT / 'build/host'
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='stall-authority-', dir=base) as scratch:
            scratch = Path(scratch)
            path = scratch / 'test.c'
            path.write_text(harness)
            binary = scratch / 'test'
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-Wno-int-to-void-pointer-cast', '-fsanitize=address,undefined',
                            '-I', str(ROOT / 'src/kernel/include'), str(path), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True,
                           env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})

    def test_device_desktop_replay_with_cpustat(self):
        source = (ROOT / 'src/kernel/core/task.c').read_text()
        declarations = source[source.index('static struct task *rq_head'):source.index('static void rq_push')]
        functions = []
        for name in ('rq_push', 'rq_pop', 'rq_remove', 'rq_any', 'schedule', 'sched_tick',
                     'task_sleep_ms', 'task_start', 'sched_tasks', 'kpreempt_point'):
            match = re.search(r'^(?:static )?[^\n]+\b' + name + r'\([^\n]*\)\s*\{', source, re.M)
            self.assertIsNotNone(match, name)
            depth, end = 1, match.end()
            while depth:
                depth += (source[end] == '{') - (source[end] == '}')
                end += 1
            functions.append(source[match.start():end])
        base = ROOT / 'build/host'
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='system-stalls-', dir=base) as scratch:
            scratch = Path(scratch)
            (scratch / 'latency_scheduler.inc').write_text(declarations + '\n'.join(functions))
            write_model(scratch / 'vm_creation_model.inc')
            binary = scratch / 'system-stalls'
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                            '-I', str(ROOT / 'src/kernel/include'), '-I', str(scratch),
                            str(ROOT / 'tests/host/system_stalls_test.c'),
                            str(ROOT / 'src/kernel/lib/fmt.c'), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True,
                                    env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout.strip())


if __name__ == '__main__':
    unittest.main()
