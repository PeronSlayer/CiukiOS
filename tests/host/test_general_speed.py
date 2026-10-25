"""101-a production allocator/descriptor replays; no image or privileged code."""
# SPDX-License-Identifier: GPL-2.0-only
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    match = re.search(r'^void ' + name + r'\([^\n]*\)\s*\{', source, re.M)
    start, end, depth = match.start(), match.end(), 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class GeneralSpeedTests(unittest.TestCase):
    def compile_run(self, folder, source):
        path = folder / 'replay.c'
        path.write_text(source)
        binary = folder / 'replay'
        subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                        '-I', str(ROOT / 'src/kernel/include'), '-I', str(folder),
                        str(path), '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], text=True, capture_output=True)
        print(result.stdout, end='')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_normal_window_policy(self):
        out = ROOT / 'build/host'
        out.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='101-windows-', dir=out) as directory:
            folder = Path(directory)
            source = (ROOT / 'src/kernel/core/cpustat.c').read_text()
            begin = source.index('    bool logged=', source.index('static void cpu_emit')) if '    bool logged=' in source else source.index('    bool emit =', source.index('static void cpu_emit'))
            end = source.index('    if (g_current)', begin)
            policy = source[begin:end]
            harness = r'''
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <ciuki/boot_info.h>
static struct { unsigned flags; } g_boot;
static bool forced,threshold=true;
static uint64_t automatic_window __attribute__((unused));
static uint64_t __attribute__((unused)) per_ms(void) { return 1000; }
static bool window(uint64_t now) {
    (void)now;
'''
            harness += policy + '    return emit;\n}\n'
            harness += r'''
int main(void) {
    setvbuf(stdout,NULL,_IONBF,0);
    unsigned n=0;
    for (unsigned i=0;i<30;i++) n+=window((i+1)*2000000ull);
    printf("101-a policy replay: normal_windows=30 emitted=%u\n",n); assert(n<=1);
    g_boot.flags=CBI_F_VERBOSE; n=0;
    for (unsigned i=0;i<30;i++) n+=window((i+31)*2000000ull);
    assert(n==30); puts("PASS 101-a logged boot: all 30 windows retained");
    return 0;
}
'''
            self.compile_run(folder, harness)

    def test_occupied_prefix(self):
        out = ROOT / 'build/host'
        out.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='101-pmm-', dir=out) as directory:
            folder = Path(directory)
            source = (ROOT / 'src/kernel/core/pmm.c').read_text()
            source = source.replace('static bool used(uint32_t pfn) {',
                                    'static unsigned probes, word_probes;\nstatic bool used(uint32_t pfn) { probes++;')
            source = source.replace('uint32_t occupied=bitmap[', 'word_probes++; uint32_t occupied=bitmap[')
            (folder / 'pmm.inc').write_text(source)
            harness = (ROOT / 'tests/host/pmm_contig_test.c').read_text()
            harness = harness.replace('int main(void)', 'int existing_main(void)')
            harness = harness.replace('../../src/kernel/core/pmm.c', str(folder / 'pmm.inc'))
            harness = harness.replace('../../src/kernel/core/ledger.c', str(ROOT / 'src/kernel/core/ledger.c'))
            harness += '''
int main(void) {
    setvbuf(stdout,NULL,_IONBF,0);
    if (existing_main()) return 1;
    fresh();
    /* The occupied prefix is resident kernel/cache/user memory. Repeated
     * directory parser workspaces must not examine every occupied page. */
    for (unsigned p=4096;p<12288;p++) if (!used(p)) {
        set_used(p); zone_free[ZONE_NORMAL]--; ledger_alloc_page(p);
    }
    unsigned total=0, words=0;
    for (unsigned turn=0;turn<60;turn++) {
        probes=word_probes=0; uint32_t run=pmm_alloc_contig(16); total+=probes; words+=word_probes;
        CHECK(run==12288u*PAGE_SIZE); release(run,16);
    }
    printf("101-a allocator replay: scans=60 occupied_pages=8192 page_probes=%u occupied_word_probes=%u total=%u\\n",total,words,total+words);
    CHECK(total+words<=60u*384u); CHECK(ledger_reconcile());
    return failures ? 1 : 0;
}
'''
            self.compile_run(folder, harness)

    def test_default_deny_switch(self):
        out = ROOT / 'build/host'
        out.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='101-tss-', dir=out) as directory:
            folder = Path(directory)
            desc = (ROOT / 'src/kernel/arch/desc.c').read_text()
            source = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
'''
            source += '#include "' + str(ROOT / 'src/kernel/arch/io_permissions.h') + '"\n'
            source += '#include "' + str(ROOT / 'src/kernel/arch/vga_io.h') + '"\n'
            source += r'''
static struct { uint8_t io_bitmap[ARCH_IO_BYTES]; } g_tss;
static bool io_denied __attribute__((unused))=true;
static unsigned bitmap_calls;
static bool measured_bitmap(uint8_t *m,const struct arch_io_range *r,unsigned n)
{ bitmap_calls++; return arch_io_bitmap(m,r,n); }
#define arch_io_bitmap measured_bitmap
'''
            source += function(desc, 'arch_io_install') + '\n' + function(desc, 'arch_vga_io')
            source += r'''
int main(void) {
    setvbuf(stdout,NULL,_IONBF,0);
    memset(g_tss.io_bitmap,0xff,sizeof(g_tss.io_bitmap));
    for (unsigned i=0;i<11594;i++) arch_io_install(NULL,0);
    printf("101-a switch replay: switches=11594 bitmap_clears=%u bytes=%u\n",bitmap_calls,bitmap_calls*ARCH_IO_BYTES);
    assert(bitmap_calls<=1);
    struct arch_io_range grant={0x300,0x310}, invalid={65535,65537};
    arch_io_install(&grant,1); assert(g_tss.io_bitmap[0x300/8]==0);
    arch_io_install(NULL,0); assert(g_tss.io_bitmap[0x300/8]==0xff);
    arch_vga_io(true); assert(g_tss.io_bitmap[0x3c0/8]!=0xff);
    arch_io_install(NULL,0);
    for (unsigned i=0;i<ARCH_IO_BYTES;i++) assert(g_tss.io_bitmap[i]==0xff);
    arch_io_install(&grant,1); arch_io_install(&invalid,1);
    for (unsigned i=0;i<ARCH_IO_BYTES;i++) assert(g_tss.io_bitmap[i]==0xff);
    unsigned before=bitmap_calls; arch_io_install(NULL,0); assert(bitmap_calls==before);
    puts("PASS 101-a TSS: cached deny, NDIS/VGA revocation, invalid grants fail closed, sentinel");
    return 0;
}
'''
            self.compile_run(folder, source)


if __name__ == '__main__':
    unittest.main()
