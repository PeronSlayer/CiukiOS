"""G14 Startup menu, Step-by-step, ScanDisk message and boot-status sector.

The production loader is assembled with NASM and its new stages run in the
bounded host executor (loader_parser_machine.py): BIOS INT 10h/13h/16h and
the BDA tick/IRQ wake are host fixtures, print/menu_echo are observed, and
every result is compared with scripts/test/loader_model.py, the model of
docs/design/startup-recovery.md. No firmware, QEMU or hardware is involved.
"""
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/test'))
sys.path.insert(0, str(ROOT / 'tests/host'))
import loader_model as model
from loader_parser_machine import LoaderParserMachine, SYMBOLS

NAMES = (*SYMBOLS, 'startup_menu', 'startup_apply', 'startup_scan', 'startup_hotkey',
         'bootstat_read', 'bootstat_write', 'autoscan_check', 'disk_read', 'print',
         'menu_echo', 'menu_getc', 'progress_check', 'deadline_start', 'deadline_expired',
         'startup_auto', 'autoscan_mode', 'bootstat_valid', 'sector', 'fat_lba', 'disk_op',
         'startup_choice', 'startup_secs', 'screen_char', 'startup_prompt', 'screen_quiet',
         'menu_tick', 'menu_polls', 'selector_source')
INFO = 0x20000
FLAGS = 8
PART = 2048
FAT_LBA = PART + 160


def bootstat_sector(state=0, disabled=0, skipped=0, request=0, magic=b'CBS1', version=1):
    b = bytearray(512)
    b[:4] = magic
    b[4], b[5], b[6], b[7], b[9] = state, version, disabled, skipped, request
    crc = zlib.crc32(bytes(b[5:508]), zlib.crc32(bytes(b[:4])))
    struct.pack_into('<I', b, 508, crc)
    return b


class Machine(LoaderParserMachine):
    def __init__(self, binary):
        super().__init__(binary, NAMES)

    def setup(self, flags=0, keys=(), ctrl_until=-1, disk=None, progress_expire_at=None,
              serial=()):
        self.reset()
        self.budget = 3000000
        self.out, self.screen, self.writes = [], [], []
        self.tick = 0
        self.keys = list(keys)  # (tick, ascii, scan)
        self.serial = list(serial)  # (tick, byte); UART capacity is not modelled.
        self.port_reads = []
        self.wakes = 0
        self.clock_running = True
        self.iflag = True  # entry enabled IRQs before discovery and the menus.
        self.idle = self.irq_hook
        self.ctrl_until = ctrl_until
        self.disk = dict(disk or {})
        self.progress_expire_at = progress_expire_at
        self.write(f'dword [0x{self.info + FLAGS:x}]', flags, 32)
        self.write(f'dword [0x{self.info + 0x10:x}]', PART, 32)  # partition_lba
        self.write(f'dword [0x{self.info + 0x38:x}]', 0x3f8 if serial else 0, 32)
        self.var('fat_lba', FAT_LBA, 32)
        self.set_tick(0)
        self.hooks[self.symbols['print']] = self.print_hook
        self.hooks[self.symbols['menu_echo']] = lambda: self.out.append(chr(self.read('al')))
        self.hooks[self.symbols['progress_check']] = self.progress_hook
        self.hooks[self.symbols['disk_read']] = self.disk_hook
        self.interrupts[0x10] = self.video
        self.interrupts[0x16] = self.keyboard

    def set_tick(self, tick):
        self.tick = tick
        self.mem[0x46c:0x470] = struct.pack('<I', tick)

    def text(self, address):
        end = self.mem.index(0, address)
        return self.mem[address:end].decode('ascii'), end + 1

    def print_hook(self):
        value, after = self.text(0x20000 + self.read('si'))
        self.out.append(value)
        self.write('si', after - 0x20000)

    def video(self):
        ah = self.read('ah')
        if ah == 0x0e:
            self.screen.append(chr(self.read('al')))
        elif ah == 0x02:
            self.screen.append(f'@{self.read("dh")}')

    def keyboard(self):
        assert self.iflag, 'BIOS keyboard polling needs enabled interrupts'
        ah = self.read('ah')
        if ah == 2:
            self.write('al', 4 if self.tick <= self.ctrl_until else 0)
        else:
            assert ah in (0, 1)
            available = bool(self.keys and self.keys[0][0] <= self.tick)
            self.zero = not available
            if available:
                _, ascii, scan = self.keys[0]
                self.write('al', ascii)
                self.write('ah', scan)
                if ah == 0:
                    self.keys.pop(0)
            else:
                assert ah == 1, 'blocking INT 16h without a buffered key'

    def port_read(self, port):
        self.port_reads.append((self.tick, port))
        assert port in (0x3fd, 0x3f8)
        available = bool(self.serial and self.serial[0][0] <= self.tick)
        if port == 0x3fd:
            return int(available)
        assert available, 'COM1 read without data ready'
        return self.serial.pop(0)[1]

    def irq_hook(self):
        self.wakes += 1
        if self.clock_running:
            self.set_tick((self.tick + 1) % model.BootMenu.DAY_TICKS)

    def progress_hook(self):
        self.carry = self.progress_expire_at is not None and self.tick >= self.progress_expire_at

    def disk_hook(self):
        lba = self.read('eax')
        address = self.regs['es'] * 16 + self.read('bx')
        if self.var('disk_op', bits=8):
            self.writes.append((lba, bytes(self.mem[address:address + 512])))
            self.disk[lba] = bytearray(self.mem[address:address + 512])
        else:
            self.mem[address:address + 512] = self.disk.get(lba, bytes(512))

    def flags(self):
        return self.field(FLAGS)

    def joined(self):
        return ''.join(self.out)


@unittest.skipUnless(shutil.which('nasm') and shutil.which('ndisasm'), 'requires NASM tools')
class StartupMenuTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        base = ROOT / 'build/host'
        base.mkdir(parents=True, exist_ok=True)
        cls.folder = tempfile.TemporaryDirectory(prefix='startup-menu-', dir=base)
        source = Path(cls.folder.name) / 'startup.asm'
        binary = source.with_suffix('.bin')
        source.write_text('%include "src/boot/ciukldr.asm"\nsection .startup_symbols align=1\ndw '
                          + ','.join(NAMES) + '\n')
        subprocess.run(['nasm', '-f', 'bin', "-dCIUKI_VERSION='9.9 test'", str(source), '-o', str(binary)],
                       cwd=ROOT, check=True)
        cls.m = Machine(binary)

    @classmethod
    def tearDownClass(cls):
        cls.folder.cleanup()

    def menu(self, keys=(), flags=0, progress_expire_at=None):
        m = self.m
        m.setup(flags=flags, keys=keys, progress_expire_at=progress_expire_at)
        self.assertTrue(m.run('startup_menu'))
        return m

    def replay(self, script, failed=False):
        """Replay (tick, ascii, scan) with one countdown step per 18 ticks."""
        menu = model.StartupMenu(failed)
        tick, keys, mark = 0, list(script), 0
        while not menu.done:
            if keys and keys[0][0] <= tick:
                _, ascii, scan = keys.pop(0)
                menu.key(ascii, scan)
                mark = tick
                continue
            tick += 1
            if tick - mark >= 18:
                menu.second()
                mark = tick
        return menu

    def test_timeout_boots_normal_and_records_the_choice(self):
        m = self.menu()
        self.assertEqual(m.flags(), model.CBI_F_MENU_SHOWN)
        text = m.joined()
        self.assertIn('CiukiOS 9.9 test Startup Menu', text)
        for number, (label, _) in model.STARTUP_CHOICES.items():
            self.assertIn(f'  {number}. {label}\r\n', text)
        self.assertIn('F5=Safe mode  Shift+F5=Command prompt  Shift+F8=Step-by-step confirmation [N]', text)
        self.assertTrue(text.endswith('L:STARTUP=1\r\n'))
        screen = ''.join(m.screen)
        self.assertIn('Enter a choice: 1   Time remaining: 30', screen)
        self.assertIn('Time remaining: 01', screen)
        # 30 one-second steps of 18 ticks each (deadline_expired: elapsed >= 18).
        self.assertEqual(m.tick, 30 * 18)
        self.assertNotIn('did not start', text)

    def test_failed_start_defaults_to_safe_mode_with_disks(self):
        m = self.menu(flags=model.CBI_F_BOOT_FAILED)
        self.assertIn('CiukiOS did not start successfully. Safe mode is selected.', m.joined())
        self.assertEqual(m.flags(), model.CBI_F_BOOT_FAILED | model.CBI_F_MENU_SHOWN |
                         model.CBI_F_SAFE_MODE | model.CBI_F_SAFE_STORAGE)

    def test_every_digit_then_enter_matches_the_model(self):
        for number in range(1, 9):
            if number == 4:
                continue  # Step-by-step has its own prompts (below)
            with self.subTest(choice=number):
                script = [(5, ord(str(number)), 0), (9, 13, 0x1c)]
                m = self.menu(script)
                expected = self.replay(script)
                self.assertEqual(expected.choice, number)
                self.assertEqual(m.flags(), expected.flags())
                self.assertTrue(m.joined().endswith(f'L:STARTUP={number}\r\n'))
                self.assertLess(m.tick, 18)

    def test_function_keys_start_at_once(self):
        for scan, number in ((model.SCAN_F5, 3), (model.SCAN_SHIFT_F5, 6)):
            with self.subTest(scan=hex(scan)):
                m = self.menu([(3, 0, scan)])
                self.assertEqual(m.flags(), model.CBI_F_MENU_SHOWN | model.STARTUP_CHOICES[number][1])
                self.assertEqual(self.replay([(3, 0, scan)]).choice, number)

    def test_arrows_move_and_clamp_and_keys_restart_the_countdown(self):
        script = [(1, 0, model.SCAN_UP), (2, 0, model.SCAN_DOWN), (3, 0, model.SCAN_DOWN),
                  (400, ord('9'), 0)]
        m = self.menu(script)
        expected = self.replay(script)
        self.assertEqual(expected.choice, 3)
        self.assertEqual(m.flags(), expected.flags())
        # The last key (an ignored '9' at tick 400) restarted the full 30 s.
        self.assertEqual(m.tick, 400 + 30 * 18)
        for script in ([(1, 0, model.SCAN_DOWN)] * 9, [(1, 0, model.SCAN_UP)]):
            m = self.menu(script)
            self.assertEqual(m.flags(), self.replay(script).flags())

    def test_menu_deadline_keeps_the_current_choice(self):
        m = self.menu([(2, ord('5'), 0)], progress_expire_at=40)
        self.assertEqual(m.flags(), model.CBI_F_MENU_SHOWN | model.CBI_F_CMD_ONLY)
        self.assertEqual(m.tick, 40)

    def step(self, answers, entry=((5, 0, model.SCAN_SHIFT_F8),)):
        keys, tick = list(entry), 20
        for yes in answers:
            keys.append((tick, 13 if yes else 27, 0x1c if yes else 0x01))
            tick += 5
        return self.menu(keys)

    def test_step_by_step_answers_match_the_model(self):
        prompts = len(model.STEP_PROMPTS)
        cases = [[True] * prompts, [False] * prompts]
        for i in range(prompts):
            answers = [True] * prompts
            answers[i] = False
            cases.append(answers)
        cases.append([i % 2 == 0 for i in range(prompts)])
        for answers in cases:
            with self.subTest(answers=answers):
                m = self.step(answers)
                self.assertEqual(m.flags(), model.CBI_F_MENU_SHOWN | model.CBI_F_STEP | model.step_flags(answers))
                text = m.joined()
                for (_, question), yes in zip(model.STEP_PROMPTS, answers):
                    self.assertIn(question + ' [Enter=Y,Esc=N] ' + ('Y' if yes else 'N') + '\r\n', text)

    def test_f8_inside_the_menu_and_choice_four_are_step_by_step(self):
        prompts = len(model.STEP_PROMPTS)
        for entry in (((5, 0, model.SCAN_F8),), ((5, ord('4'), 0), (6, 13, 0x1c))):
            m = self.step([True] * prompts, entry)
            self.assertEqual(m.flags(), model.CBI_F_MENU_SHOWN | model.CBI_F_STEP | model.step_flags([True] * prompts))

    def test_other_keys_are_ignored_during_step_prompts_and_the_deadline_answers_yes(self):
        m = self.m
        keys = [(5, 0, model.SCAN_SHIFT_F8), (20, ord('x'), 0x2d), (21, 27, 1)]
        m.setup(keys=keys, progress_expire_at=60)
        self.assertTrue(m.run('startup_menu'))
        answers = [False] + [True] * (len(model.STEP_PROMPTS) - 1)
        self.assertEqual(m.flags(), model.CBI_F_MENU_SHOWN | model.CBI_F_STEP | model.step_flags(answers))

    def test_scan_codes_and_ctrl_hotkey(self):
        m = self.m
        for scan, expected in ((model.SCAN_F5, '3'), (model.SCAN_SHIFT_F5, '6'),
                               (model.SCAN_SHIFT_F8, '4'), (model.SCAN_F8, '\0'), (0x3b, None), (0xff, None)):
            m.setup()
            m.write('ah', scan)
            self.assertTrue(m.run('startup_scan'))
            self.assertEqual(m.carry, expected is None)
            if expected is not None:
                self.assertEqual(chr(m.read('al')), expected)
        for held in (True, False):
            m.setup(ctrl_until=10 if held else -1)
            self.assertTrue(m.run('startup_hotkey'))
            self.assertEqual(m.carry, held)

    def boot_menu(self, **options):
        m = self.m
        m.setup(**options)
        self.assertTrue(m.run('boot_menu'))
        return m

    def test_real_empty_input_path_halts_for_each_timer_tick(self):
        m = self.boot_menu()
        self.assertEqual((m.tick, m.wakes, m.var('menu_polls')), (55, 55, 55))
        self.assertTrue(m.iflag)
        self.assertNotIn(m.symbols['menu_getc'], m.hooks)

    def test_idle_enables_interrupts_even_after_bios_clears_if(self):
        m = self.m
        m.setup()
        m.iflag = False  # Exercise menu_getc's explicit STI before BIOS input.
        keyboard = m.keyboard

        def firmware_clears_if():
            keyboard()
            m.iflag = False

        m.interrupts[0x16] = firmware_clears_if
        for reg, value in (('bp', 55), ('di', 0x1234), ('es', 0x3456)):
            m.write(reg, value)
        self.assertTrue(m.run('menu_getc'))
        self.assertTrue(m.carry)
        self.assertTrue(m.iflag)
        self.assertEqual(m.wakes, 1)
        self.assertEqual(m.var('menu_polls'), 0)  # Saturates before any deadline.
        self.assertEqual([m.read(reg) for reg in ('bp', 'di', 'es')],
                         [55, 0x1234, 0x3456])

    def test_stalled_bda_clock_uses_a_tick_sized_idle_bound(self):
        m = self.m
        m.setup()
        m.clock_running = False
        self.assertTrue(m.run('boot_menu'))
        self.assertEqual((m.tick, m.wakes, m.var('menu_polls')), (0, 110, 0))
        self.assertTrue(m.joined().endswith('L:SELECT_SOURCE=menu\r\n'))

    def test_deadline_checks_and_buffered_keys_do_not_count_as_ticks(self):
        m = self.m
        m.setup(keys=[(0, ord('x'), 0)] * 200)
        m.write('bp', 55)
        self.assertTrue(m.run('deadline_start'))
        for _ in range(200):
            self.assertTrue(m.run('menu_getc'))
            self.assertFalse(m.carry)
            self.assertTrue(m.run('deadline_expired'))
            self.assertFalse(m.carry)
        self.assertEqual((m.tick, m.wakes, m.var('menu_polls')), (0, 0, 110))
        self.assertTrue(m.run('menu_getc'))
        self.assertTrue(m.carry)
        for _ in range(200):
            self.assertTrue(m.run('deadline_expired'))
            self.assertFalse(m.carry)
        self.assertEqual((m.tick, m.wakes, m.var('menu_polls')), (1, 1, 109))

    def test_idle_countdown_wraps_at_midnight(self):
        m = self.m
        m.setup()
        m.set_tick(model.BootMenu.DAY_TICKS - 10)
        self.assertTrue(m.run('boot_menu'))
        self.assertEqual((m.tick, m.wakes), (45, 55))

    def test_keyboard_irq_without_a_tick_keeps_the_bda_deadline(self):
        m = self.m
        m.setup()

        def keyboard_then_timer():
            # An extra IRQ1 wake between each pair of timer ticks.
            m.clock_running = bool(m.wakes % 2)
            m.irq_hook()

        m.idle = keyboard_then_timer
        self.assertTrue(m.run('boot_menu'))
        self.assertEqual((m.tick, m.wakes), (55, 110))

    def test_keyboard_precedes_serial_and_serial_has_no_function_scan_code(self):
        m = self.m
        m.setup(keys=[(0, 0, model.SCAN_F8)], serial=[(0, ord('N'))])
        self.assertTrue(m.run('menu_getc'))
        self.assertFalse(m.carry)
        self.assertEqual((m.read('al'), m.read('ah')), (0, model.SCAN_F8))
        self.assertEqual(m.port_reads, [])
        self.assertTrue(m.run('menu_getc'))
        self.assertFalse(m.carry)
        self.assertEqual((m.read('al'), m.read('ah')), (ord('N'), 0xff))
        self.assertEqual(m.wakes, 0)

    def test_serial_selectors_and_safe_log_hardware_normal_keys(self):
        for serial in (False, True):
            for text, expected, ticks in (
                ('N', 0, 1), ('H', model.CBI_F_HW_CHECK | model.CBI_F_VERBOSE, 1),
                ('SLN', model.CBI_F_SAFE_MODE | model.CBI_F_SERIAL_LOG |
                 model.CBI_F_VERBOSE | model.CBI_F_DIAG_VERBOSE, 1),
                ('S', model.CBI_F_SAFE_MODE, 56),
                ('SPf1:safe run=12345678\r', model.CBI_F_SAFE_MODE |
                 model.CBI_F_TEST_REQUEST, 1),
            ):
                with self.subTest(serial=serial, text=text):
                    options = ({'serial': [(1, ord(c)) for c in text]} if serial else
                               {'keys': [(1, ord(c), 0) for c in text]})
                    m = self.boot_menu(**options)
                    self.assertEqual(m.flags(), expected)
                    self.assertEqual((m.tick, m.wakes), (ticks, ticks))
                    self.assertTrue(m.joined().endswith('L:SELECT_SOURCE=menu\r\n'))
        # No key: COM1 LSR is checked once per tick; an empty UART never blocks.
        m = self.boot_menu(serial=[(1000, ord('N'))])
        self.assertEqual(m.port_reads, [(tick, 0x3fd) for tick in range(55)])

    def test_fw_cfg_bypasses_idle_and_cfg_timeout_and_positive_safe_survive(self):
        m = self.m
        m.setup(flags=model.CBI_F_TEST_REQUEST)
        self.assertTrue(m.run('boot_menu'))
        self.assertEqual((m.tick, m.wakes), (0, 0))
        self.assertEqual(m.joined(), 'L:SELECT_SOURCE=fw_cfg\r\n')
        m.setup(flags=model.CBI_F_TEST_REQUEST)
        m.var('selector_source', 2, 8)
        self.assertTrue(m.run('boot_menu'))
        self.assertEqual((m.tick, m.wakes), (55, 55))
        self.assertEqual(m.flags(), model.CBI_F_TEST_REQUEST)
        self.assertTrue(m.joined().endswith('L:SELECT_SOURCE=cfg\r\n'))
        m = self.boot_menu(flags=model.CBI_F_SAFE_MODE, keys=[(1, ord('N'), 0)])
        self.assertEqual(m.flags(), model.CBI_F_SAFE_MODE)

    def test_boot_menu_opens_the_startup_menu_for_ctrl_f8_and_failed_starts(self):
        m = self.boot_menu(ctrl_until=3)
        self.assertTrue(m.flags() & model.CBI_F_MENU_SHOWN)
        self.assertIn('Startup Menu', m.joined())
        self.assertTrue(m.joined().endswith('L:SELECT_SOURCE=menu\r\n'))
        m = self.boot_menu(keys=[(4, 0, model.SCAN_F8)])
        self.assertTrue(m.flags() & model.CBI_F_MENU_SHOWN)
        m = self.boot_menu(keys=[(4, 0, model.SCAN_F5)])
        self.assertEqual(m.flags(), model.CBI_F_SAFE_MODE | model.CBI_F_SAFE_STORAGE)
        self.assertNotIn('Startup Menu', m.joined())
        self.assertIn('L:STARTUP=3', m.joined())
        # Without a key: the earlier quiet 3-second window and the new hint.
        m = self.boot_menu()
        self.assertEqual(m.flags(), 0)
        self.assertNotIn('Startup Menu', m.joined())
        self.assertEqual(m.tick, 55)
        # A failed start (bootstat_read sets startup_auto) needs no key.
        m.setup(flags=model.CBI_F_BOOT_FAILED)
        m.var('startup_auto', 1, 8)
        self.assertTrue(m.run('boot_menu'))
        self.assertTrue(m.flags() & model.CBI_F_SAFE_STORAGE)
        self.assertIn('did not start successfully', m.joined())

    def test_bootstat_read_matches_the_model(self):
        m = self.m
        sectors = [bootstat_sector(), bootstat_sector(state=1), bootstat_sector(disabled=0x05, skipped=0x22),
                   bootstat_sector(request=1), bootstat_sector(request=3), bootstat_sector(request=4),
                   bootstat_sector(state=1, disabled=0xff, skipped=0x80, request=7),
                   bootstat_sector(magic=b'CBS2'), bootstat_sector(version=2), bytearray(512)]
        for sector in sectors:
            for test_request in (False, True):
                with self.subTest(sector=bytes(sector[:10]).hex(), test=test_request):
                    flags = 0x40 if test_request else 0
                    m.setup(flags=flags, disk={PART + 9: sector})
                    self.assertTrue(m.run('bootstat_read'))
                    expected, auto, valid = model.bootstat_loader_flags(sector, test_request)
                    self.assertEqual(m.flags(), flags | expected)
                    self.assertEqual(m.var('startup_auto', bits=8), auto)
                    self.assertEqual(m.var('bootstat_valid', bits=8), valid)

    def test_bootstat_write_marks_only_the_state_byte(self):
        m = self.m
        sector = bootstat_sector(disabled=3, skipped=4, request=1)
        m.setup(disk={PART + 9: sector})
        self.assertTrue(m.run('bootstat_read'))
        self.assertTrue(m.run('bootstat_write'))
        self.assertEqual(len(m.writes), 1)
        lba, data = m.writes[0]
        self.assertEqual(lba, PART + 9)
        expected = bytearray(sector)
        expected[4] = 1
        self.assertEqual(data, bytes(expected))
        self.assertEqual(m.var('disk_op', bits=8), 0)
        m.setup(flags=model.CBI_F_SAFE_MODE | model.CBI_F_SAFE_STORAGE, disk={PART + 9: sector})
        self.assertTrue(m.run('bootstat_read'))
        self.assertTrue(m.run('bootstat_write'))
        self.assertEqual(len(m.writes), 1)           # safe mode with the disks completes normally
        # The CRC excludes byte 4: the record stays valid for the kernel.
        self.assertEqual(struct.unpack_from('<I', data, 508)[0],
                         zlib.crc32(data[5:508], zlib.crc32(data[:4])))
        # No write without a valid record, once a probe was selected, or in
        # safe mode without disks (no mount could complete the start).
        for disk, flags in (({PART + 9: bytes(512)}, 0), ({PART + 9: sector}, 0x40),
                            ({PART + 9: sector}, model.CBI_F_SAFE_MODE)):
            m.setup(disk=disk)
            self.assertTrue(m.run('bootstat_read'))
            m.write(f'dword [0x{m.info + FLAGS:x}]', flags, 32)
            self.assertTrue(m.run('bootstat_write'))
            self.assertEqual(m.writes, [])

    def fat(self, clean=True, error_free=True):
        b = bytearray(512)
        value = 0x0fffffff & ~((0 if clean else 0x08000000) | (0 if error_free else 0x04000000))
        struct.pack_into('<II', b, 0, 0x0ffffff8, value)
        return b

    def autoscan(self, fat, mode=1, flags=0, keys=()):
        m = self.m
        m.setup(flags=flags, keys=keys, disk={FAT_LBA: fat})
        m.var('autoscan_mode', mode, 8)
        self.assertTrue(m.run('autoscan_check'))
        return m

    def test_autoscan_message_skip_and_thorough(self):
        message = 'CiukiOS was not shut down properly. ScanDisk will check your disks.'
        m = self.autoscan(self.fat())
        self.assertEqual((m.flags(), m.out), (0, []))
        m = self.autoscan(self.fat(clean=False))
        self.assertEqual(m.flags(), model.CBI_F_SCANDISK)
        self.assertIn(message, m.joined())
        self.assertEqual(m.tick, 182)  # ten seconds, then ScanDisk runs
        m = self.autoscan(self.fat(error_free=False))
        self.assertEqual(m.flags(), model.CBI_F_SCANDISK | model.CBI_F_SCANDISK_THOROUGH)
        m = self.autoscan(self.fat(clean=False, error_free=False), keys=[(30, 27, 1)])
        self.assertEqual(m.flags(), model.CBI_F_SCANDISK_SKIP)
        self.assertIn('ScanDisk skipped.', m.joined())
        m = self.autoscan(self.fat(clean=False), keys=[(30, 13, 0x1c)])
        self.assertEqual((m.flags(), m.tick), (model.CBI_F_SCANDISK, 30))
        m = self.autoscan(self.fat(clean=False), mode=2)
        self.assertEqual((m.flags(), m.out), (model.CBI_F_SCANDISK, []))
        for mode, flags in ((0, 0), (1, 0x40), (1, model.CBI_F_SAFE_MODE)):
            m = self.autoscan(self.fat(clean=False), mode=mode, flags=flags)
            self.assertEqual((m.flags(), m.out), (flags, []))
        m = self.autoscan(self.fat(clean=False), flags=model.CBI_F_SAFE_MODE | model.CBI_F_SAFE_STORAGE)
        self.assertTrue(m.flags() & model.CBI_F_SCANDISK)

    def test_production_entry_order_and_tokens(self):
        entry = (ROOT / 'src/boot/ciukldr.asm').read_text().split('entry:', 1)[1].split('; All errors', 1)[0]
        calls = ['call config_read', 'call bootstat_read', 'call progress_begin', 'call boot_menu',
                 'call autoscan_check', 'call bootstat_write', 'call progress_end', 'call video_select']
        positions = [entry.index(c) for c in calls]
        self.assertEqual(positions, sorted(positions))
        menu = (ROOT / 'src/boot/ciukldr/menu.inc').read_text()
        for token in ("'menu=1'", "'autoscan=0'", "'autoscan=2'"):
            self.assertIn(token, menu)
        for token in ('menu=1', 'autoscan=0', 'autoscan=2', 'menu=1 autoscan=2\r\nsafe=0'):
            self.assertEqual(model.boot_options(token)['safe'], False)
        with self.assertRaises(ValueError):
            model.boot_options('autoscan=1')

    def test_config_tokens_set_menu_and_autoscan(self):
        m = self.m
        for text, auto, mode in (('', 0, 1), ('menu=1', 1, 1), ('autoscan=0', 0, 0),
                                 ('autoscan=2 menu=1', 1, 2), ('serial=1\nautoscan=0 autoscan=2', 0, 2)):
            with self.subTest(text=text):
                self.assertTrue(m.config(text))
                self.assertEqual(m.var('startup_auto', bits=8), auto)
                self.assertEqual(m.var('autoscan_mode', bits=8), mode)
        for text in ('menu=0', 'menu=11', 'autoscan=1', 'autoscan=20'):
            self.assertFalse(m.config(text), text)


if __name__ == '__main__':
    unittest.main()
