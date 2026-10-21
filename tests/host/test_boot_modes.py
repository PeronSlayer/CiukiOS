"""Quiet normal boot and power-control checks (docs/boot.md); no QEMU.

Loader: fixed 24576-byte image, screen-quiet stages in menu.inc, the menu
model. Kernel: the quiet-console predicate, console.c behaviour on a fake
LFB, the wake points, and the call-73 wiring and SDK wrapper.
"""
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/test'))
sys.path.append(str(ROOT / 'scripts'))  # loader_budget, ciuki_version
import loader_budget
from loader_model import BootMenu, LoaderUART
from loader_uart_machine import LoaderUARTMachine
from loader_parser_machine import LoaderParserMachine, SYMBOLS
import loader_model
import probe_tables
from physical import cfg_selector
from run import selector as runner_selector

LOADER_BYTES = 24576
CONSOLE_HARNESS = r'''
#define main framebuffer_fixture_main
#include "tests/host/fbdev_test.c"
#undef main
#undef GUARD
#define CIUKI_CPU_H
#define P2V(p) ((void *)(uintptr_t)(p))
static unsigned host_if = 0x200;
static uint32_t irq_save(void) { unsigned old = host_if; host_if = 0; return old; }
static void irq_restore(uint32_t flags) { host_if = flags; }
const uint8_t font_cfn_regular[95+95*32] = { [95+('a'-32)*32+1] = 0x80 };
void serial_write(const char *s, size_t n) { CHECK(s && n); }
void kpreempt_point(void) { }
void task_sleep_ms(uint32_t ms) { CHECK(ms == 100); }
static void *console_map(uint32_t phys, uint32_t size)
{
    CHECK(phys == g_boot.fb_phys && size == g_boot.fb_pitch * g_boot.fb_height);
    return fake_lfb;
}
#define vmm_map_framebuffer console_map
#define pack console_pack
#include "src/kernel/core/console.c"
#undef pack
#undef vmm_map_framebuffer
'''


def scratch():
    base = ROOT / 'build/host'
    base.mkdir(parents=True, exist_ok=True)
    return tempfile.TemporaryDirectory(prefix='boot-modes-', dir=base)


class LoaderTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('nasm') and shutil.which('ndisasm'), 'requires NASM tools')
    def test_assembled_uart_paths_match_model(self):
        with scratch() as folder:
            source = Path(folder) / 'uart.asm'
            binary = Path(folder) / 'uart.bin'
            # Only export addresses; the first 24576 bytes are the production loader.
            source.write_text('%include "src/boot/ciukldr.asm"\nsection .uart_symbols\n'
                              'dw uart_init, uart_putc, uart_budget, boot_info\n')
            subprocess.run(['nasm', '-f', 'bin', str(source), '-o', str(binary)], cwd=ROOT, check=True)
            self.assertEqual(binary.stat().st_size, LOADER_BYTES + 8)
            cases = ({}, {'bda': 0}, {'bda': 0x2f8}, {'scratch': False},
                     {'lsr': 0xff}, {'divisor_low': 0}, {'divisor_high': 1})
            for options in cases:
                with self.subTest(options=options):
                    model = LoaderUART(flags=0x2003, **options)
                    machine = LoaderUARTMachine(binary, **options)
                    # Discovery must replace stale UART fields without losing other flags.
                    machine.set_field(8, 0x2003)
                    machine.set_field(0x38, 0x1234)
                    machine.set_field(0x3c, 0x4321)
                    machine.run(machine.init)
                    self.assertEqual((machine.field(15, 1), machine.field(0x38), machine.field(0x3c),
                                      machine.field(8)), (model.state, model.base, model.divisor, model.flags))
                    if options.get('bda', 0x3f8) != 0x3f8:
                        self.assertEqual(machine.reads, [])
                    else:
                        self.assertEqual(machine.ports[0x3ff], 0x37)
                    if model.state in (1, 5):
                        self.assertEqual(machine.ports[0x3fb], 3)
                    machine.run(machine.putc)
                    model.putc(65, options.get('lsr', 0x60))
                    self.assertEqual(machine.sent, model.sent)
            for lsr, budget in ((0x60, 1), (0, 16384), (0xff, 16384), (0x60, 0)):
                with self.subTest(lsr=lsr, budget=budget):
                    machine = LoaderUARTMachine(binary)
                    machine.set_field(8, 0x2003)
                    machine.run(machine.init)
                    self.assertEqual(machine.read(f'word [0x{machine.budget:x}]'), 16384)
                    machine.write(f'word [0x{machine.budget:x}]', budget)
                    machine.lsr = lsr
                    model = LoaderUART(flags=0x2003)
                    model.budget = budget
                    for char in (65, 66, 67):
                        saved = {reg: machine.read(reg) for reg in ('cx', 'dx')}
                        machine.run(machine.putc, char)
                        self.assertEqual({reg: machine.read(reg) for reg in saved}, saved)
                        model.putc(char, lsr)
                        self.assertEqual((machine.field(15, 1), machine.field(8), machine.field(0x38),
                                          machine.field(0x3c), machine.sent),
                                         (model.state, model.flags, model.base, model.divisor, model.sent))
                    self.assertEqual(machine.reads.count(0x3fd),
                                     4097 if lsr == 0 else 1 if budget == 0 else 2)
                    self.assertEqual(machine.read('ax') & 255, 67)

    @unittest.skipUnless(shutil.which('nasm'), 'requires nasm')
    def test_boot_info_cleared_at_every_entry_before_discovery(self):
        with scratch() as folder:
            binary = Path(folder) / 'ciukldr.bin'
            subprocess.run(['nasm', '-f', 'bin', str(ROOT / 'src/boot/ciukldr.asm'),
                            '-o', str(binary)], cwd=ROOT, check=True)
            data = binary.read_bytes()
        entry, = struct.unpack_from('<H', data, 12)
        # Decode the entire straight-line entry prefix, including ES=CS and
        # CLD; no BIOS call/branch/interrupt can precede REP STOSB.
        prefix = bytes.fromhex('fa fc 31c0 8ed0 66bc007c0000 8ee0 8cc8 8ed8 8ec0 31c0 bf')
        self.assertEqual(data[entry:entry + len(prefix)], prefix)
        offset = entry + len(prefix)
        boot_info, = struct.unpack_from('<H', data, offset)
        self.assertEqual(data[offset + 2:offset + 8], bytes.fromhex('b94413 f3aa fb'))
        self.assertEqual(data[offset + 8:offset + 12], b'\x88\x16' + struct.pack('<H', boot_info + 0xC))
        self.assertEqual(data[boot_info:boot_info + 0x1344], bytes(0x1344))
        # Every field, including the C/NASM policy byte at 0x00E, is cleared
        # on cold entry and re-entry after stale in-memory state.
        self.assertLess(boot_info + 0x1344, LOADER_BYTES)

    def test_e500_detection_sets_provenance_before_a20_unless_forced(self):
        loader = (ROOT / 'src/boot/ciukldr.asm').read_text()
        self.assertIn('%include "src/boot/input_platform.inc"', loader)
        policy = loader.split('call fwcfg_request', 1)[1].split('call a20_enable', 1)[0]
        self.assertEqual([line.strip() for line in policy.splitlines() if line.strip()], [
            'test dword [B(flags)], CBI_F_INPUT_FORCED', 'jnz .policy',
            'call input_platform_firmware_first', 'jnc .policy',
            'mov byte [B(input_policy)], 1',
            'or dword [B(flags)], CBI_F_INPUT_DETECTED', '.policy:',
        ])
        self.assertIn('%define CBI_F_INPUT_DETECTED  (1 << 14)',
                      (ROOT / 'src/boot/boot_info.inc').read_text())
        menu = (ROOT / 'src/boot/ciukldr/menu.inc').read_text()
        accept = menu.split('selector_accept:', 1)[1].split('chain_probe_names', 1)[0]
        self.assertIn('mov byte [B(input_policy)], 1', accept)
        self.assertIn('or dword [B(flags)], CBI_F_INPUT_FORCED', accept)

    @unittest.skipUnless(shutil.which('nasm'), 'requires nasm')
    def test_loader_image_keeps_its_fixed_size_and_hint(self):
        with scratch() as folder:
            for version in (None, '0.9.0 pre-alpha', loader_budget.load()['display']):
                binary = Path(folder) / 'ciukldr.bin'
                listing = Path(folder) / 'ciukldr.lst'
                loader_budget.assemble(binary, listing, version)
                budget = loader_budget.measure(binary, listing)
                print(f'CIUKLDR free bytes ({version or "unknown"}): {budget["free"]}', flush=True)
                self.assertGreaterEqual(budget['free'], 512)
                data = binary.read_bytes()
                self.assertEqual(len(data), LOADER_BYTES)
                self.assertEqual(data[-32:], b'CLDRPRG1' + bytes(24))
                self.assertIn(BootMenu.SAFE_CONFIRMATION.encode() + b'\r\n\0', data)
                self.assertIn(BootMenu.MENU_HINT.encode(), data)
                self.assertNotIn(BootMenu.MENU_HINT.encode() + b'\0', data, 'AH=13h takes a counted string')
                # The relocated ranking table lies after the image, inside loader_end.
                text = listing.read_text()
                self.assertRegex(text, r'00006000 <res 400h>\s+mode_scores resw 512')
                self.assertRegex(text, r'00006400 <res 41h>\s+selector_buffer resb 65')
                self.assertIn('mov dword [B(loader_end)], LOAD_BASE+loader_bss_end', text)

    def test_menu_stages_keep_serial_and_quiet_only_the_screen(self):
        menu = (ROOT / 'src/boot/ciukldr/menu.inc').read_text()
        body = menu.split('boot_menu:', 1)[1].split('source_menu db', 1)[0]
        quiet = body.split('.menu:', 1)[1].split('.restart:', 1)[0]
        stages = ['test dword [B(flags)], CBI_F_TEST_REQUEST | CBI_F_SAFE_MODE', 'jnz .announce',
                  'mov ax, 3', 'mov ax, 0x1300', 'mov cx, menu_hint_end-menu_hint', 'mov dx, 0x1800',
                  'mov bp, menu_hint', 'int 0x10', 'mov byte [screen_quiet], 1',
                  '.announce:', 'mov si, menu_text', 'call print']
        at = 0
        for stage in stages:                       # each stage after the previous one
            at = quiet.index(stage, at) + len(stage)
        for label, end in (('.safe:', '.cancel_cfg:'), ('.log:', '.probe:'), ('.probe:', '.line:')):
            stage = body.split(label, 1)[1].split(end, 1)[0]
            self.assertIn('mov byte [screen_quiet], 0', stage, label)
        log = body.split('.log:', 1)[1].split('.probe:', 1)[0]
        self.assertIn('xor dword [B(flags)], CBI_F_SERIAL_LOG', log)
        self.assertIn('or byte [B(flags)+1], CBI_F_VERBOSE >> 8', log)
        platform = (ROOT / 'src/boot/ciukldr/platform.inc').read_text()
        printer = platform.split('\nprint:', 1)[1].split('uart_putc:', 1)[0]
        self.assertLess(printer.index('cmp byte [screen_quiet], 0'), printer.index('int 0x10'))
        self.assertLess(printer.index('.serial:'), printer.index('call uart_putc'))
        fatal = (ROOT / 'src/boot/ciukldr.asm').read_text().split('fatal:', 1)[1].split('.halt:', 1)[0]
        self.assertLess(fatal.index('mov byte [screen_quiet], 0'), fatal.index('call print'))
        self.assertIn('%define CBI_F_VERBOSE         (1 << 11)', (ROOT / 'src/boot/boot_info.inc').read_text())

    def test_menu_model_quiet_screen_and_verbose_choice(self):
        menu = BootMenu()
        self.assertTrue(menu.screen_quiet)
        self.assertEqual(menu.output, ['SELECT_READY'])          # serial copy unchanged
        menu.poll(BootMenu.MENU_TICKS)
        self.assertTrue(menu.screen_quiet)
        self.assertFalse(menu.verbose)
        self.assertEqual(menu.output[-1], 'L:SELECT_SOURCE=menu')
        menu = BootMenu()
        menu.poll(5, 'L')
        self.assertTrue(menu.verbose)
        self.assertFalse(menu.screen_quiet)
        self.assertFalse(menu.serial)                             # bit 1 still toggles
        menu = BootMenu()
        menu.poll(5, 's')
        self.assertFalse(menu.screen_quiet)
        self.assertFalse(menu.verbose)
        self.assertFalse(BootMenu(request='all:sweep run=87654321 step=9').screen_quiet)
        self.assertFalse(BootMenu(safe=True).screen_quiet)
        self.assertFalse(BootMenu(request='f1:safe run=12345678', source='fw_cfg').screen_quiet)


@unittest.skipUnless(shutil.which('nasm') and shutil.which('ndisasm'), 'requires NASM tools')
class LoaderParserTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.folder = scratch()
        source = Path(cls.folder.name) / 'parser.asm'
        binary = source.with_suffix('.bin')
        symbols = (*SYMBOLS, 'raw_map', 'ph_table', 'elf_load', 'elf_load.validate',
                   'file_seek', 'loader_bss_end')
        source.write_text('%include "src/boot/ciukldr.asm"\nsection .parser_symbols align=1\ndw '
                          + ','.join(symbols) + '\n')
        subprocess.run(['nasm', '-f', 'bin', str(source), '-o', str(binary)], cwd=ROOT, check=True)
        cls.machine = LoaderParserMachine(binary, symbols)

    @classmethod
    def tearDownClass(cls):
        cls.folder.cleanup()

    def test_assembled_selectors_match_existing_models(self):
        requests = set()
        for phase, names in probe_tables.menu_names().items():
            for name in names:
                base = f'f{phase}:{name} run=1234aBcD'
                requests.update((base, base + 'x', base + ' ', base.replace(' run=', 'x run='),
                                 base + ' platform=e500', base + ' safe=1',
                                 base + ' platform=e500 safe=1'))
        for phase in ('f0', 'f1', 'f2', 'f3', 'all'):
            for suffix in ('', ' step=0', ' step=00', ' step=63 state=0007FFFF',
                           ' step=64', ' step=000', ' step=1 state=00080000',
                           ' step=1 state=0000000g', ' state=00000000'):
                requests.add(f'{phase}:sweep run=12345678' + suffix)
        for title in ('a', 'a'*23, 'a'*24, '', 'A', '0a', 'a-0', 'a_b'):
            requests.add('f3:title run=12345678 title=' + title)
        # f3-55 DOS conformance grammar: exactly three suffixes, every source.
        for suffix in ('', ' guest=freedos', ' guest=ciukidos', ' guest=ciukidos kernel=unified',
                       ' guest=freedos kernel=unified', ' guest=unified', ' guest=ciukido',
                       ' guest=freedosx', ' guest=ciukidos kernel=master', ' guest=ciukidos kernel=unifie',
                       ' guest=ciukidos kernel=unified ', ' guest=freedos guest=ciukidos',
                       ' guest=freedos platform=e500', ' platform=e500 guest=freedos',
                       ' guest=ciukidos safe=1', ' guest=ciukidos kernel=unified x=1'):
            requests.add('f3:dos-conformance run=1234aBcD' + suffix)
            requests.add('f2:dos-conformance run=1234aBcD' + suffix)
        for phase, name in ((2, 'crash-isolation'), (0, 'boot'), (3, 'vm-boot')):
            for suffix in (' server=desktop', ' server=standin', ' safe=1 server=desktop',
                           ' platform=e500 server=standin', ' server=other', ' safe=1 platform=e500'):
                requests.add(f'f{phase}:{name} run=12345678' + suffix)
        requests.update(('', 'f7:boot run=12345678', 'f0:boot run=1234567g', 'x'*65,
                         'f0:boot run=12345678\x00', 'f2:signals-fault'))
        # f6-18 browser grammar: http[s]://host[:port]/path, URL <= 40 bytes,
        # optional ' now=1', every source (selector.c browser_url).
        browser = 'f6:browser run=1234aBcD'
        for suffix in ('', ' url=https://fixture.test/page', ' url=http://fixture.test/upgrade',
                       ' url=https://fixture.test/page now=1', ' url=https://h:8443/a_b-c.d/',
                       ' url=HTTPS://h/', ' url=https://H-1.x/', ' url=http://h/', ' url=http://h:/',
                       ' url=http://h:123456/', ' url=http://h:65535/', ' url=http://:80/',
                       ' url=https://fixture.test', ' url=ftp://fixture.test/page', ' url=https:/h/',
                       ' url=httpss://h/', ' url=https://fixture.test/p?q=1', ' url=https://fixture.test/a=b',
                       ' url=https://h_1/', ' url=https://h/~', ' url=https://h/page now=0',
                       ' url=https://h/page now=1 now=1', ' url=https://h/page platform=e500',
                       ' url=https://h/page safe=1', '  url=https://h/page', ' url=https://h/page ',
                       ' url=https://h/' + 'a' * 25, ' url=https://h/' + 'a' * 26,
                       ' url=https://fixture.test/' + 'a' * 9 + ' now=1',
                       ' url=https://fixture.test/' + 'a' * 15, ' url=https://fixture.test/' + 'a' * 16):
            requests.add(browser + suffix)
            requests.add('f5:browser run=1234aBcD' + suffix)
        # Name prefixes and extensions never select a neighbouring probe.
        for phase in ('f0', 'f1', 'f2', 'f4', 'f5', 'all', 'f/', 'g0'):
            for name in ('boo', 'bootinfox', 'input-faul', 'crash-isolatio', 'vm-audio-jitte',
                         'w32-', 'sweepx', 'swee', 'title'):
                requests.add(f'{phase}:{name} run=12345678')
        for source in (0, 1, 2):
            for request in sorted(requests):
                with self.subTest(source=source, request=request):
                    try:
                        selected = (cfg_selector(request) if source == 2 else
                                    runner_selector(request, 'fw_cfg' if source else 'menu', bool(source)))
                    except ValueError:
                        self.assertFalse(self.machine.select(request, source))
                        continue
                    self.assertTrue(self.machine.select(request, source))
                    self.assertEqual(self.machine.read('cx'), len(request))
                    self.assertEqual(self.machine.read('si'), self.machine.symbols['selector_buffer'])
                    self.assertEqual(self.machine.var('selector_force', bits=8), bool(selected.get('platform')))
                    self.assertEqual(self.machine.var('selector_safe', bits=8), selected.get('safe', False))
                    self.assertEqual(self.machine.var('selector_sweep', bits=8), selected['probe'] == 'sweep')
                    self.machine.run('selector_accept')
                    self.assertEqual(self.machine.field(0x66, 2), len(request))
                    start = 0x20000 + self.machine.info + 0x68
                    self.assertEqual(self.machine.mem[start:start+len(request)], request.encode())

    def test_assembled_options_match_model_and_keep_exact_boundaries(self):
        options = ('', 'safe=0', 'safe=1 safe=0', 'serial=0 serial=1', 'accel=0',
                   ' safe=1\r\nserial=0 mode=0x3FfF accel=0 ', 'mode=0x0000',
                   'mode=0x4000', 'mode=0x123', 'mode=0x12345', 'mode=0x000g',
                   'accel=1', 'xaccel=0', 'accel=00', 'safe=10', 'serial=00',
                   'safe=0\tserial=1', 'safe=1\x00', 'A'*127, ' '*127, ' '*128)
        for flags in (0, 1, 2, 3):
            for text in options:
                with self.subTest(flags=flags, text=text):
                    try:
                        expected = loader_model.boot_options(text, bool(flags & 1), bool(flags & 2))
                    except ValueError:
                        self.assertFalse(self.machine.config(text, flags))
                        continue
                    self.assertTrue(self.machine.config(text, flags))
                    self.assertEqual(self.machine.field(8) & 3, expected['safe'] | (expected['serial'] << 1))
                    self.assertEqual(self.machine.var('preferred_mode'),
                                     0xffff if expected['mode'] is None else expected['mode'])
        text = 'safe=1\nprobe=f1:safe run=12345678\nserial=0'
        self.assertTrue(self.machine.config(text))
        self.assertEqual(self.machine.field(8) & 0x43, 0x41)
        self.assertFalse(self.machine.config(text + '\nprobe=f0:core run=12345678'))

    def test_elf_fills_shared_scratch_before_first_header_validation(self):
        m = self.machine
        self.assertEqual(m.symbols['ph_table'], m.symbols['raw_map'])
        self.assertEqual(m.symbols['loader_bss_end'], 0x6841)
        for count in (1, 32):
            m.reset()
            header = bytearray(52)
            header[:7] = b'\x7fELF\x01\x01\x01'
            struct.pack_into('<HHI', header, 16, 2, 3, 1)
            struct.pack_into('<I', header, 28, 52)
            struct.pack_into('<HHH', header, 40, 52, 32, count)
            payload = bytes((i*17+3) % 256 for i in range(count*32))
            data = header + payload
            m.var('found_size', len(data), 32)
            m.var('file_size', len(data), 32)
            m.hooks[m.symbols['directory_find']] = lambda: setattr(m, 'carry', False)
            m.hooks[m.symbols['file_open']] = lambda: None
            cursor = 0
            def seek():
                nonlocal cursor
                cursor = m.read('eax')
            def read():
                nonlocal cursor
                start, length = 0x20000+m.read('di'), m.read('cx')
                m.mem[start:start+length] = data[cursor:cursor+length]
                cursor += length
            m.hooks[m.symbols['file_seek']] = seek
            m.hooks[m.symbols['file_read']] = read
            raw = 0x20000 + m.symbols['raw_map']
            m.mem[raw:raw+3072] = b'\xa5'*3072
            normalized = 0x20000 + m.info + 0x4f0
            before = bytes(m.mem[normalized:normalized+128*24])
            self.assertTrue(m.run('elf_load', stop=m.symbols['elf_load.validate']))
            self.assertEqual(m.mem[raw:raw+len(payload)], payload)
            self.assertEqual(m.mem[raw+len(payload):raw+3072], b'\xa5'*(3072-len(payload)))
            self.assertEqual(m.mem[normalized:normalized+128*24], before)


class KernelTests(unittest.TestCase):
    def compile_and_run(self, folder, name, source, extra=()):
        if not shutil.which('clang'):
            self.skipTest('host clang unavailable')
        harness = Path(folder) / (name + '.c')
        harness.write_text(source)
        binary = Path(folder) / name
        built = subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                                '-fsanitize=address,undefined', '-I', str(ROOT),
                                '-I', str(ROOT / 'src/kernel/include'), *extra, str(harness), '-o', str(binary)],
                               capture_output=True, text=True)
        self.assertEqual(built.returncode, 0, built.stderr)
        result = subprocess.run([str(binary)], capture_output=True, text=True,
                                env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def test_quiet_console_predicate(self):
        with scratch() as folder:
            out = self.compile_and_run(folder, 'quiet_predicate', r'''
#include <stdio.h>
#include "src/kernel/include/ciuki/boot_info.h"
int main(void)
{
    unsigned bad = 0;
    bad += !cbi_quiet_console(CBI_F_SERIAL_LOG | CBI_F_VBE_CTRL_VALID | CBI_F_EDID_VALID | (3u << CBI_F_A20_SHIFT));
    bad += !cbi_quiet_console(0);
    bad += !cbi_quiet_console(CBI_F_INPUT_DETECTED);
    bad += !!cbi_quiet_console(CBI_F_TEST_REQUEST);
    bad += !!cbi_quiet_console(CBI_F_SAFE_MODE);
    bad += !!cbi_quiet_console(CBI_F_VERBOSE);
    bad += !!cbi_quiet_console(CBI_F_TEXT_MODE);
    bad += !!cbi_quiet_console(CBI_F_HW_CHECK);
    bad += CBI_F_VERBOSE != 0x800u || CBI_F_DIAG_VERBOSE != 0x1000u;
    bad += CBI_F_INPUT_DETECTED != 0x4000u || CBI_F_DEFINED_MASK != 0x7FFFu;
    printf("quiet predicate: %u failures\n", bad);
    return bad != 0;
}
''')
            self.assertIn('0 failures', out)

    def test_console_quiet_keeps_the_framebuffer_until_woken(self):
        with scratch() as folder:
            out = self.compile_and_run(folder, 'console_quiet', CONSOLE_HARNESS + r'''
int main(void)
{
    enum { PITCH = 80, HEIGHT = 35, SIZE = PITCH * HEIGHT };
    uint8_t *saved = malloc(SIZE);
    fake_lfb = malloc(SIZE);
    CHECK(saved && fake_lfb);
    boot_fixture(true);
    g_boot.fb_height = HEIGHT; g_boot.fb_pitch = PITCH;
    g_boot.vbe_mode_info[20] = HEIGHT; g_boot.vbe_mode_info[50] = PITCH;
    reservation.end = g_boot.fb_phys + SIZE;
    memset(fake_lfb, 0x5A, SIZE);                 /* what the loader left */
    memcpy(saved, fake_lfb, SIZE);
    CHECK(fbdev_init() == 0);
    console_set_quiet(cbi_quiet_console(g_boot.flags));
    CHECK(console_quiet() && console_init_lfb());
    CHECK(!memcmp(saved, fake_lfb, SIZE));        /* no clear on a quiet boot */
    for (unsigned i = 0; i < rows + 5; i++) console_write("a\n", 2);
    console_write("a", 1);
    CHECK(!memcmp(saved, fake_lfb, SIZE) && hist_count == rows + 5 && !drawing);
    /* A grant cycle while quiet still draws nothing (release alone). */
    console_display_owned(true);
    console_write("a\n", 2);
    console_display_owned(false);
    console_write("a\n", 2);
    CHECK(!memcmp(saved, fake_lfb, SIZE) && !drawing);
    /* Overlays (operator prompt, evidence pages) still draw. */
    console_operator_prompt(false);
    CHECK(memcmp(saved, fake_lfb, SIZE) != 0 && !drawing);
    memset(fake_lfb, 0x5A, SIZE);
    /* Desktop failure: wake schedules the coalesced history repaint. */
    console_wake();
    CHECK(!console_quiet() && !memcmp(saved, fake_lfb, SIZE));
    console_write("a", 1);
    CHECK(!memcmp(saved, fake_lfb, SIZE) && !drawing);
    console_service();
    CHECK(memcmp(saved, fake_lfb, SIZE) != 0 && !drawing);
    /* Panic on a quiet boot: console_reclaim alone switches drawing on. */
    console_set_quiet(true);
    memset(fake_lfb, 0x5A, SIZE);
    console_write("a\n", 2);
    CHECK(!memcmp(saved, fake_lfb, SIZE));
    console_display_owned(true);
    console_reclaim();
    CHECK(!console_quiet());
    console_write("a", 1);
    CHECK(memcmp(saved, fake_lfb, SIZE) != 0 && !drawing);
    /* Verbose boots keep drawing from the first line, as before. */
    for (uint32_t f = CBI_F_TEST_REQUEST; f; f = f == CBI_F_TEST_REQUEST ? CBI_F_SAFE_MODE :
                                               f == CBI_F_SAFE_MODE ? CBI_F_VERBOSE : 0) {
        console_set_quiet(cbi_quiet_console(g_boot.flags | f));
        memset(fake_lfb, 0x5A, SIZE);
        CHECK(console_init_lfb() && memcmp(saved, fake_lfb, SIZE) != 0);
    }
    free(saved); free(fake_lfb);
    printf("console quiet boot: %u failures\n", failures);
    return failures ? 1 : 0;
}
''')
            self.assertIn('0 failures', out)

    def test_verbose_console_scroll_respects_desktop_ownership(self):
        with scratch() as folder:
            out = self.compile_and_run(folder, 'console_desktop_owned', CONSOLE_HARNESS + r'''
int main(void)
{
    enum { PITCH=80, HEIGHT=35, SIZE=PITCH*HEIGHT };
    fake_lfb=malloc(SIZE);uint8_t *saved=malloc(SIZE);CHECK(fake_lfb && saved);
    boot_fixture(true);g_boot.flags|=CBI_F_VERBOSE;
    g_boot.fb_height=HEIGHT;g_boot.fb_pitch=PITCH;
    g_boot.vbe_mode_info[20]=HEIGHT;g_boot.vbe_mode_info[50]=PITCH;
    reservation.end=g_boot.fb_phys+SIZE;
    CHECK(!fbdev_init());console_set_quiet(false);CHECK(console_init_lfb());
    console_display_owned(true);memset(fake_lfb,0x5a,SIZE);memcpy(saved,fake_lfb,SIZE);
    for(unsigned i=0;i<5000;i++)console_write("a verbose USB/CPU record wraps and scrolls history\n",49);
    CHECK(!memcmp(saved,fake_lfb,SIZE) && hist_count==HIST_LINES && !drawing);
    /* Panic takes ownership explicitly and paints again. */
    console_reclaim();console_write("a",1);CHECK(memcmp(saved,fake_lfb,SIZE));
    free(fake_lfb);free(saved);return failures ? 1 : 0;
}
''')
            self.assertEqual(out, '')

    def test_quiet_boot_draws_the_splash_photograph(self):
        with scratch() as folder:
            splash = Path(folder) / 'splash.rgb565'
            splash.write_bytes(b'\x00\xf8' * (512 * 384))       # pure red RGB565
            out = self.compile_and_run(folder, 'console_splash', CONSOLE_HARNESS + r'''
int main(void)
{
    enum { PITCH = 80, HEIGHT = 35, SIZE = PITCH * HEIGHT };
    fake_lfb = malloc(SIZE);
    CHECK(fake_lfb != 0);
    boot_fixture(true);
    g_boot.fb_height = HEIGHT; g_boot.fb_pitch = PITCH;
    g_boot.vbe_mode_info[20] = HEIGHT; g_boot.vbe_mode_info[50] = PITCH;
    reservation.end = g_boot.fb_phys + SIZE;
    memset(fake_lfb, 0x5A, SIZE);
    CHECK(fbdev_init() == 0);
    console_set_quiet(true);
    CHECK(console_init_lfb());
    /* 17x35: the 4:3 area is 17x12 at y 11..22; black elsewhere. */
    for (unsigned y = 0; y < HEIGHT; y++)
        for (unsigned x = 0; x < 17; x++) {
            uint32_t px; memcpy(&px, fake_lfb + y * PITCH + x * 4, 4);
            CHECK(px == ((y >= 11 && y < 23) ? 0xFF0000u : 0u));
        }
    CHECK(fake_lfb[17 * 4] == 0x5A);              /* pitch padding untouched */
    console_write("a\n", 2);
    uint32_t px; memcpy(&px, fake_lfb + 11 * PITCH, 4);
    CHECK(px == 0xFF0000u);                       /* still the splash */
    free(fake_lfb);
    printf("console splash: %u failures\n", failures);
    return failures ? 1 : 0;
}
''', extra=(f'-DCIUKI_SPLASH_BIN="{splash}"',))
            self.assertIn('0 failures', out)

    def test_splash_conversion_is_the_pinned_photograph(self):
        import hashlib, importlib.util
        spec = importlib.util.spec_from_file_location('convert_splash', ROOT / 'scripts/convert_splash.py')
        module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        self.assertEqual(module.SOURCE, ROOT / 'misc/CiukiOS_SplashScreen.png')
        data = module.convert()
        self.assertEqual(len(data), 512 * 384 * 2)
        self.assertEqual(hashlib.sha256(data).hexdigest(), module.OUTPUT_SHA256)
        build = (ROOT / 'scripts/build_kernel.py').read_text()
        self.assertIn('runpy.run_path(str(ROOT / "scripts/convert_splash.py"))["build"](OUT / "splash.rgb565")', build)
        self.assertIn("f'-DCIUKI_SPLASH_BIN=\"{splash}\"',", build)

    def test_wake_points_and_quiet_decision(self):
        main = (ROOT / 'src/kernel/core/main.c').read_text()
        self.assertLess(main.index('console_set_quiet(!text && cbi_quiet_console(g_boot.flags));'),
                        main.index('if (!text && !console_init_lfb())'))
        supervisor = (ROOT / 'src/kernel/proc/supervisor.c').read_text()
        for line in ('klog("[supervisor] desktop unavailable', 'klog("[supervisor] desktop stopped'):
            self.assertIn('console_wake();', supervisor[:supervisor.index(line)][-120:], line)
        output = (ROOT / 'src/kernel/core/output.c').read_text()
        for function in ('void panic(', 'void panic_frame(', 'void df_handler('):
            body = output.split(function, 1)[1].split('halt_forever();\n}', 1)[0]
            self.assertLess(body.index('console_reclaim();'), body.index('kputs_raw('))
        console = (ROOT / 'src/kernel/core/console.c').read_text()
        reclaim = console.split('void console_reclaim(void)', 1)[1].split('}', 1)[0]
        self.assertIn('quiet = false;', reclaim)
        text = console.split('void console_init_text(void)', 1)[1].split('}', 1)[0]
        self.assertIn('quiet = false;', text)
        probes = (ROOT / 'src/kernel/probes/probes.c').read_text()
        normal = probes.split('if (!(g_boot.flags & CBI_F_TEST_REQUEST)) {', 1)[1].split('struct probe_selection selection;', 1)[0]
        self.assertLess(normal.index('task_create_kernel("quiet-watchdog"'), normal.index('drivers_init();'))
        self.assertIn('#define QUIET_WATCHDOG_MS 120000u', probes)

    def test_power_call_wiring_and_sdk_wrapper(self):
        abi = (ROOT / 'src/kernel/include/ciuki/abi.h').read_text()
        self.assertRegex(abi, r'CIUKI_SYS_DOSVM = 72,\n    CIUKI_SYS_POWER = 73,\n    CIUKI_SYS_DISPLAY = 74,\n    CIUKI_SYS_LAUNCH_MARK = 75,\n    CIUKI_SYS_DEVICES = 76,\n    CIUKI_SYS_DISPLAY_ACCEL = 77,\n    CIUKI_SYS_SOCKET = 78,\n    CIUKI_SYS_BIND = 79,\n    CIUKI_SYS_CONNECT = 80,\n    CIUKI_SYS_LISTEN = 81,\n    CIUKI_SYS_ACCEPT = 82,\n    CIUKI_SYS_SENDTO = 83,\n    CIUKI_SYS_RECVFROM = 84,\n    CIUKI_SYS_SHUTDOWN = 85,\n    CIUKI_SYS_SOCKNAME = 86,\n    CIUKI_SYS_SOCKOPT = 87,\n    CIUKI_SYS_POLL = 88,\n    CIUKI_SYS_RESOLVE4 = 89,\n    CIUKI_SYS_NETCTL = 90,\n    CIUKI_SYS_GETENTROPY = 91,\n};')
        for name, value in (('REBOOT', 1), ('OFF', 2), ('QUERY', 3), ('CAN_REBOOT', 1), ('CAN_OFF', 2)):
            self.assertIn(f'#define CIUKI_POWER_{name} {value}u', abi)
        syscall = (ROOT / 'src/kernel/core/syscall.c').read_text()
        self.assertIn('case CIUKI_SYS_POWER:', syscall)
        self.assertIn('extern int32_t power_syscall(uint32_t action) __attribute__((weak));', syscall)
        power = (ROOT / 'src/kernel/core/power.c').read_text()
        body = power.split('int32_t power_syscall(uint32_t action)', 1)[1]
        self.assertLess(body.index('caller->pid != supervisor_desktop_pid()'), body.index('CIUKI_POWER_QUERY'))
        self.assertLess(body.index('flush_storage()'), body.index('kernel_reboot()'))
        self.assertLess(body.index('flush_storage()'), body.index('acpi_power_off()'))
        wrapper = (ROOT / 'sdk/libciuki/power.c').read_text()
        self.assertIn('CU_CALL(POWER, action, 0, 0, 0, 0, 0)', wrapper)
        header = (ROOT / 'sdk/sysroot-overlay/include/ciuki/power.h').read_text()
        self.assertIn('int ciuki_power(uint32_t action);', header)
        docs = (ROOT / 'docs/design/execution-abi.md').read_text()
        self.assertIn('Syscall **73**, `CIUKI_SYS_POWER`', docs)


if __name__ == '__main__':
    unittest.main()
