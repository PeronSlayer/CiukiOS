"""Compile the exact desktop mixer helper definitions against mocked I/O."""
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DESKTOP = ROOT / 'src/apps/desktop.c'


def c_function(source, name):
    match = re.search(r'(?m)^static\s+int\s+' + re.escape(name) + r'\s*\([^)]*\)\s*\{', source)
    if not match:
        raise AssertionError(f'cannot find production function {name}')
    start = match.start()
    brace = source.index('{', match.start())
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == '{':
            depth += 1
        elif source[pos] == '}':
            depth -= 1
            if depth == 0:
                return source[start:pos + 1]
    raise AssertionError(f'unclosed production function {name}')


HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
struct regs { u16 ax, bx, cx, dx, di, ds, es; };
static u16 top_mixer = 0x1000, top_nabm = 0x2000, top_audio_bdf = 0x00f2;
static u16 pci_command, master_value, write_value;
static int fail_pci_read, fail_command_read_on, fail_enable_write, fail_restore_write, pci_writes;
static int io_reads, io_writes, cas_busy, cas_invalid, cas_owned, master_reads;
static int cas_busy_after_write, override_readback;
static u16 forced_readback;
static int enable_value, restored_value;
static u16 app_seg(void) { return 0x1000; }
static void mem_set(void *p, int v, unsigned n) { memset(p, v, n); }
static int intr(u8 vector, struct regs *r)
{
    if (vector != 0x1a) return 1;
    if (r->ax == 0xb109) {
        ++io_reads;
        if (fail_pci_read || io_reads == fail_command_read_on) return 1;
        if (r->di == 4) r->cx = pci_command;
        else return 1;
        r->ax = 0;
        return 0;
    }
    if (r->ax == 0xb10c && r->di == 4) {
        ++pci_writes;
        if (r->cx & 1) {
            enable_value = r->cx;
            if (fail_enable_write) return 1;
            pci_command = r->cx;
        } else {
            restored_value = r->cx;
            if (fail_restore_write) return 1;
            pci_command = r->cx;
        }
        r->ax = 0;
        return 0;
    }
    return 1;
}
static u8 top_inb(u16 port)
{
    assert(port == top_nabm + 0x34);
    if (cas_invalid) return 0xff;
    if (cas_busy > 0) { --cas_busy; return 1; }
    cas_owned = 1;
    return 0;
}
static u16 top_inw(u16 port)
{
    assert(port == top_mixer + 2);
    assert(cas_owned);
    cas_owned = 0;
    ++master_reads;
    return master_value;
}
static void top_outw(u16 port, u16 value)
{
    assert(port == top_mixer + 2);
    assert(cas_owned);
    cas_owned = 0;
    ++io_writes;
    write_value = value;
    master_value = override_readback ? forced_readback : value;
    cas_busy = cas_busy_after_write;
}
'''


def build_harness():
    source = DESKTOP.read_text(encoding='utf-8')
    helpers = '\n\n'.join(c_function(source, name) for name in
                            ('top_pci_word', 'top_pci_write_word', 'top_mixer_access'))
    return HARNESS + '\n' + helpers + r'''
static void reset(void)
{
    pci_command = 0x0144; master_value = 0x1234; write_value = 0;
    fail_pci_read = fail_command_read_on = fail_enable_write = fail_restore_write = 0;
    pci_writes = io_reads = io_writes = cas_busy = cas_invalid = cas_owned = master_reads = 0;
    cas_busy_after_write = override_readback = 0; forced_readback = 0;
    enable_value = restored_value = 0;
}
int main(void)
{
    u16 value;
    int ok;
    reset();
    value = 0;
    ok = top_mixer_access(0, &value);
    assert(ok && value == 0x1234);
    assert(pci_command == 0x0144 && enable_value == 0x0145 && restored_value == 0x0144);
    assert(pci_writes == 2 && master_reads == 1 && cas_owned == 0);

    reset(); pci_command = 0x0405; value = 0;
    ok = top_mixer_access(0, &value);
    assert(ok && value == 0x1234 && pci_writes == 0);

    reset(); master_value = 0xffff; value = 0;
    assert(!top_mixer_access(0, &value));
    assert(pci_command == 0x0144 && restored_value == 0x0144);

    reset(); fail_pci_read = 1; value = 0;
    assert(!top_mixer_access(0, &value) && pci_writes == 0);

    reset(); fail_enable_write = 1; value = 0;
    assert(!top_mixer_access(0, &value));
    assert(pci_command == 0x0144 && restored_value == 0x0144);

    reset(); fail_command_read_on = 2; value = 0;
    assert(!top_mixer_access(0, &value));
    assert(pci_command == 0x0144 && restored_value == 0x0144 && pci_writes == 2);

    reset(); cas_busy = 4096; value = 0;
    assert(!top_mixer_access(0, &value));
    assert(master_reads == 0 && pci_command == 0x0144);

    reset(); cas_invalid = 1; value = 0;
    assert(!top_mixer_access(0, &value));
    assert(master_reads == 0 && pci_command == 0x0144);

    reset(); value = 0x5a5a;
    ok = top_mixer_access(1, &value);
    assert(ok && write_value == 0x5a5a && value == 0x5a5a && master_reads == 1);
    assert(pci_command == 0x0144 && pci_writes == 2);

    reset(); value = 0x5a5a; override_readback = 1; forced_readback = 0x3456;
    ok = top_mixer_access(1, &value);
    assert(ok && write_value == 0x5a5a && value == 0x3456);

    reset(); value = 0x4321; cas_busy_after_write = 4096;
    assert(!top_mixer_access(1, &value));
    assert(master_reads == 0 && pci_command == 0x0144);

    reset(); pci_command = 0x0005; value = 0x6789;
    ok = top_mixer_access(1, &value);
    assert(ok && value == 0x6789 && pci_writes == 0);

    reset(); fail_restore_write = 1; value = 0x4321;
    assert(!top_mixer_access(1, &value));
    assert(pci_command == 0x0145 && restored_value == 0x0144);
    return 0;
}
'''


class DesktopMixerHostTests(unittest.TestCase):
    def test_production_mixer_helpers_with_mock_hardware(self):
        compiler = shutil.which('cc')
        if not compiler:
            self.skipTest('host C compiler not available')
        with tempfile.TemporaryDirectory(prefix='desktop-mixer-') as tmp:
            src = Path(tmp) / 'desktop_mixer_host.c'
            exe = Path(tmp) / 'desktop_mixer_host'
            src.write_text(build_harness(), encoding='utf-8')
            subprocess.run([compiler, '-std=c99', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
