"""F1-37 host menu replay and production loader stage checks; no QEMU."""
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/test'))
from loader_model import BootMenu


class LoaderMenuTests(unittest.TestCase):
    REQUEST = 'f1:safe run=12345678'
    CONFIGURED = 'all:sweep run=87654321 step=9'

    def enter(self, menu, request=None, tick=60):
        for char in (request or self.REQUEST) + '\r':
            menu.poll(tick, char)

    def assert_safe_menu(self, menu, request=None):
        self.assertEqual(menu.state, 'done')
        self.assertTrue(menu.safe)
        self.assertEqual(menu.request, request)
        self.assertEqual(menu.source, 'menu')
        self.assertFalse(menu.error)
        self.assertIn('L:SAFE=1', menu.output)
        self.assertEqual(menu.output[-1], 'L:SELECT_SOURCE=menu')

    def test_safe_then_probe_cancels_cfg_and_passes_both_flag_and_selector(self):
        for safe_key, probe_key in (('S', 'P'), ('s', 'p')):
            with self.subTest(keys=(safe_key, probe_key)):
                menu = BootMenu(request=self.CONFIGURED)
                menu.poll(50, safe_key)
                self.assertEqual(menu.state, 'menu')
                self.assertIsNone(menu.request)
                self.assertIn(BootMenu.SAFE_CONFIRMATION, menu.output)
                menu.poll(70, probe_key)  # Beyond the original 55-tick window.
                self.enter(menu, tick=80)
                self.assert_safe_menu(menu, self.REQUEST)

    def test_safe_then_normal_keeps_safe_and_cancels_cfg(self):
        menu = BootMenu(request=self.CONFIGURED)
        menu.poll(50, 'S')
        menu.poll(60, 'N')
        self.assert_safe_menu(menu)

    def test_safe_then_expiry_boots_without_probe(self):
        menu = BootMenu(request=self.CONFIGURED)
        menu.poll(50, 'S')
        menu.poll(104)
        self.assertEqual(menu.state, 'menu')
        menu.poll(105)
        self.assert_safe_menu(menu)

    def test_safe_restarts_tick_and_idle_wakeup_budgets(self):
        menu = BootMenu(tick=100)
        menu.poll(154)
        self.assertEqual(menu.polls_left, 2 * BootMenu.MENU_TICKS - 1)
        menu.poll(154, 'S')
        self.assertEqual((menu.start_tick, menu.deadline_ticks, menu.polls_left),
                         (154, 55, 2 * BootMenu.MENU_TICKS))
        menu.poll(155)
        self.assertEqual(menu.state, 'menu')
        menu.poll(208)
        self.assertEqual(menu.state, 'menu')
        menu.poll(209)
        self.assert_safe_menu(menu)

    def test_probe_alone_does_not_add_safe(self):
        menu = BootMenu(request=self.CONFIGURED)
        menu.poll(1, 'P')
        self.enter(menu)
        self.assertFalse(menu.safe)
        self.assertEqual(menu.request, self.REQUEST)
        self.assertFalse(menu.error)
        self.assertNotIn('L:SAFE=1', menu.output)
        self.assertEqual(menu.output[-1], 'L:SELECT_SOURCE=menu')

    def test_safe_log_toggle_then_probe(self):
        menu = BootMenu()
        menu.poll(50, 'S')
        menu.poll(60, 'L')
        self.assertFalse(menu.serial)
        self.assertEqual(menu.start_tick, 50)  # L does not restart the timer.
        menu.poll(70, 'P')
        self.enter(menu, tick=80)
        self.assert_safe_menu(menu, self.REQUEST)

    def test_probe_entry_restarts_its_own_thirty_second_deadline(self):
        menu = BootMenu()
        menu.poll(50, 'S')
        menu.poll(100, 'P')
        self.assertEqual((menu.start_tick, menu.deadline_ticks), (100, 548))
        menu.poll(647)
        self.assertEqual(menu.state, 'entry')
        menu.poll(648)
        self.assertTrue(menu.error)
        self.assertTrue(menu.safe)
        self.assertIsNone(menu.request)
        self.assertEqual(menu.output[-1], 'L:SELECT_SOURCE=menu')

    def test_invalid_hardware_suffix_does_not_clear_safe_or_restore_cfg(self):
        menu = BootMenu(request=self.CONFIGURED)
        menu.poll(1, 'S')
        menu.poll(2, 'P')
        self.enter(menu, self.REQUEST + ' safe=1', tick=3)
        self.assertTrue(menu.error)
        self.assertTrue(menu.safe)
        self.assertIsNone(menu.request)
        self.assertEqual(menu.source, 'menu')

    def test_repeated_safe_and_midnight_restart(self):
        menu = BootMenu(tick=BootMenu.DAY_TICKS - 30)
        menu.poll(BootMenu.DAY_TICKS - 1, 'S')
        menu.poll(20)
        self.assertEqual(menu.state, 'menu')
        menu.poll(30, 's')
        menu.poll(84)
        self.assertEqual(menu.state, 'menu')
        menu.poll(85)
        self.assert_safe_menu(menu)

    def test_stopped_clock_still_expires_after_safe_restart(self):
        menu = BootMenu()
        menu.poll(1, 'S')
        menu.polls_left = 2  # Reach the finite idle-wakeup bound cheaply.
        menu.poll(1)
        self.assertEqual(menu.state, 'menu')
        menu.poll(1)
        self.assert_safe_menu(menu)

    def test_buffered_input_does_not_spend_idle_wakeup_budget(self):
        menu = BootMenu()
        for char in 'x' * 200:
            menu.poll(0, char)
        self.assertEqual(menu.polls_left, 2 * BootMenu.MENU_TICKS)
        self.assertEqual(menu.state, 'menu')
        for _ in range(2 * BootMenu.MENU_TICKS):
            menu.poll(0)  # A wake without a working BDA clock.
        self.assertEqual(menu.state, 'done')

    def test_production_empty_input_halts_and_only_wakeups_spend_the_bound(self):
        source = (ROOT / 'src/boot/ciukldr/menu.inc').read_text()
        getc = source.split('menu_getc:', 1)[1].split('deadline_start:', 1)[0]
        empty = getc.split('.empty:', 1)[1]
        self.assertRegex(empty, r'\bsti\s+hlt\b')
        self.assertLess(empty.index('hlt'), empty.index('dec word [menu_polls]'))
        deadline = source.split('deadline_expired:', 1)[1].split('menu_tick dd', 1)[0]
        self.assertIn('cmp word [menu_polls], 0', deadline)
        self.assertNotIn('dec ', deadline)

    def test_unchanged_cfg_and_fw_cfg_paths_and_positive_safe_sources(self):
        menu = BootMenu(request=self.CONFIGURED, safe=True)
        menu.poll(55)
        self.assertTrue(menu.safe)
        self.assertEqual(menu.request, self.CONFIGURED)
        self.assertEqual(menu.output, ['SELECT_READY', 'L:SELECT_SOURCE=cfg'])
        menu = BootMenu(request=self.REQUEST + ' safe=1', source='fw_cfg')
        menu.poll(1, 'S')  # fw_cfg bypasses the menu.
        self.assertTrue(menu.safe)
        self.assertEqual(menu.output, ['L:SELECT_SOURCE=fw_cfg'])
        menu = BootMenu(safe=True)
        menu.poll(1, 'N')
        self.assertTrue(menu.safe)
        self.assertNotIn('L:SAFE=1', menu.output)

    def test_hardware_check_replaces_cfg_preserves_safe_and_fw_priority(self):
        for key in ('h','H'):
            menu=BootMenu(safe=True,request='f0:core run=12345678')
            menu.poll(1,key)
            self.assertEqual(menu.state,'done')
            self.assertTrue(menu.hardware_check and menu.verbose and menu.safe)
            self.assertIsNone(menu.request)
            self.assertFalse(menu.screen_quiet)
        menu=BootMenu(request='f0:core run=12345678',source='fw_cfg')
        menu.poll(1,'H')
        self.assertFalse(menu.hardware_check)

    def test_self_test_replaces_cfg_preserves_safe_and_fw_priority(self):
        for key in ('t', 'T'):
            menu = BootMenu(safe=True, request='f0:core run=12345678')
            menu.poll(1, key)
            self.assertEqual(menu.state, 'done')
            self.assertTrue(menu.self_test and menu.safe)
            self.assertFalse(menu.hardware_check)
            self.assertIsNone(menu.request)
            self.assertFalse(menu.screen_quiet)
            self.assertEqual(menu.output[-2:], ['L:SELFTEST=1', 'L:SELECT_SOURCE=menu'])
        menu = BootMenu(request='f0:core run=12345678', source='fw_cfg')
        menu.poll(1, 'T')
        self.assertFalse(menu.self_test)
        source = (ROOT / 'src/boot/ciukldr/menu.inc').read_text()
        selftest = source.split('.selftest:', 1)[1].split('.probe:', 1)[0]
        self.assertIn('call .cancel_cfg', selftest)
        self.assertIn('CBI_F_SELF_TEST >> 16', selftest)
        self.assertNotIn('CBI_F_SAFE_MODE', selftest)
        self.assertIn("selftest_selected db 'L:SELFTEST=1',13,10,0", source)

    def test_production_safe_stage_returns_through_full_deadline_restart(self):
        source = (ROOT / 'src/boot/ciukldr/menu.inc').read_text()
        menu = source.split('boot_menu:', 1)[1].split('source_menu db', 1)[0]
        safe = menu.split('.safe:', 1)[1].split('.cancel_cfg:', 1)[0]
        stages = ['call .cancel_cfg', 'or dword [B(flags)], CBI_F_SAFE_MODE',
                  'mov si, safe_confirmation', 'call print',
                  'mov si, safe_selected', 'jmp .restart']
        positions = [safe.index(stage) for stage in stages]
        self.assertEqual(positions, sorted(positions))
        self.assertNotIn('jmp .done', safe)
        restart = menu.split('.restart:', 1)[1].split('.poll:', 1)[0]
        self.assertIn('mov bp, 55', restart)
        self.assertIn('call deadline_start', restart)
        deadline = source.split('deadline_start:', 1)[1].split('deadline_expired:', 1)[0]
        self.assertIn('mov [menu_tick], eax', deadline)
        self.assertIn('mov [menu_polls], bp', deadline)
        self.assertIn('shl word [menu_polls], 1', deadline)
        cancel = menu.split('.cancel_cfg:', 1)[1].split('.log:', 1)[0]
        self.assertIn('~CBI_F_TEST_REQUEST', cancel)
        self.assertIn('mov word [B(test_request_len)], 0', cancel)
        self.assertIn('mov byte [selector_source], 0', cancel)
        self.assertNotIn('CBI_F_SAFE_MODE', cancel)
        self.assertIn("safe_selected db 'L:SAFE=1',13,10,0", source)
        self.assertIn("safe_confirmation db '" + BootMenu.SAFE_CONFIRMATION + "',13,10,0", source)
        entry = (ROOT / 'src/boot/ciukldr.asm').read_text().split('entry:', 1)[1].split('; All errors', 1)[0]
        calls = ['call config_read', 'call boot_menu', 'call video_select', 'call elf_load']
        positions = [entry.index(call) for call in calls]
        self.assertEqual(positions, sorted(positions))


if __name__ == '__main__':
    unittest.main()
