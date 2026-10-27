"""G27B wire and native-command controllers; strictly sequential host tests."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[3]
FLAGS = ['clang', '-std=c17', '-D_POSIX_C_SOURCE=200809L', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-fsanitize=address,undefined', '-fno-sanitize-recover=all']

class G27BTests(unittest.TestCase):
    def test_transfer(self):
        self.run_case('transfer', ['apps/serial/transfer.c', 'apps/serial/xmodem.c',
                                  'tests/host/desktop/transfer_test.c'])

    def test_native_terminal(self):
        self.run_case('terminal-native', ['apps/terminal/native_commands.c', 'apps/terminal/format.c',
                      'tests/host/desktop/terminal_native_test.c', *map(str, sorted((ROOT/'sdk/libreg').glob('*.c')))],
                      ['-include', str(ROOT/'tests/host/desktop/replay_base.h'),
                       '-I'+str(ROOT/'src/kernel/include'), '-idirafter', str(ROOT/'sdk/sysroot-overlay/include')])

    def test_window_rows(self):
        desktop = ['protocol', 'client', 'input', 'compositor', 'theme', 'shell',
                   'access', 'access_desktop', 'autoplay', 'autoplay_content',
                   'autoplay_registry', 'settings', 'power_policy', 'wallpaper',
                   'keymap', 'folder', 'recovery_view']
        toolkit = ['draw', 'widgets', 'list', 'menu', 'dialog', 'controls',
                   'commdlg', 'textarea', 'icons', 'icon_map', 'iconx', 'fileicon']
        sources = [*(f'apps/desktop/{n}.c' for n in desktop),
                   *(f'apps/libdeskui/{n}.c' for n in toolkit),
                   'apps/files/model.c', 'apps/files/transfer.c',
                   'apps/accessories/mixer_view.c', 'tests/host/desktop/fake.c',
                   'src/kernel/drivers/video/swdib.c', 'src/kernel/drivers/video/batch.c',
                   'sdk/libreg/registry.c', 'sdk/libreg/storage.c', 'sdk/libreg/hardware.c',
                   'tests/host/desktop/g27b_window_test.c']
        self.run_case('g27b-window', sources,
                      ['-include', str(ROOT/'tests/host/desktop/replay_base.h'),
                       '-include', str(ROOT/'tests/host/desktop/paint_probe.h'),
                       '-I'+str(ROOT/'src/kernel/include'), '-I'+str(ROOT/'apps/desktop'),
                       '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections'])

    def run_case(self, name, sources, flags=()):
        out = ROOT/'build/host/g27b';out.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=out) as scratch:
            binary = Path(scratch)/name
            include = Path(scratch)/'include'; include.mkdir()
            (include/'ciuki').symlink_to(ROOT/'sdk/sysroot-overlay/include/ciuki', target_is_directory=True)
            extra = ['-I'+str(include)] if name != 'transfer' else []
            subprocess.run([*FLAGS, *extra, *flags, *(str(ROOT/s) for s in sources), '-o', str(binary)], check=True)
            subprocess.run([str(binary), scratch], check=True)

if __name__ == '__main__':
    unittest.main()
