"""Part C executable golden vectors and real presenter/registry integration."""
import test_display_driver_model as model
import unittest
from pathlib import Path
import subprocess
import tempfile
import os

class DisplayCommandTests(unittest.TestCase):
    run_host = model.DisplayDriverTests.run_host

    def test_commands(self):
        self.run_host(['tests/host/display_commands_test.c'])

    def test_sdk_wrappers(self):
        folder=model.ROOT/'build/host'; folder.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='command-sdk-',dir=folder) as tmp:
            source=(model.ROOT/'sdk/libciuki/posix.c').read_text()
            first=source.index('int ciuki_display_commands(')
            last=source.index('int ciuki_input_read(',first)
            (Path(tmp)/'batch_wrappers.inc').write_text(source[first:last])
            self.run_host(['tests/host/display_batch_sdk_test.c'],extra=['-I',tmp])

    def test_desktop_batches(self):
        root=model.ROOT
        sources=[root/'apps/desktop'/n for n in ('protocol.c','client.c','input.c','compositor.c','theme.c','shell.c','autoplay.c','autoplay_content.c','settings.c','wallpaper.c','keymap.c','folder.c','../files/model.c','../files/transfer.c')]
        sources += [root/'apps/libdeskui'/n for n in ('draw.c','widgets.c','list.c','menu.c','dialog.c','textarea.c','icons.c','icon_map.c')]
        sources += [root/'tests/host/desktop/fake.c',root/'tests/host/desktop/commands_test.c',root/'src/kernel/drivers/video/swdib.c',root/'src/kernel/drivers/video/batch.c']
        folder=root/'build/host'; folder.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='command-client-',dir=folder) as tmp:
            binary=Path(tmp)/'test'
            subprocess.run(['clang','-std=c17','-D_POSIX_C_SOURCE=200809L','-O1','-g','-Wall','-Wextra','-Werror',
                '-fsanitize='+('address,undefined' if os.environ.get('CIUKI_DISPLAY_ASAN') else 'undefined'),'-fno-sanitize-recover=undefined',
                *(['-include',os.environ['CIUKI_TEST_CURSOR_HEADER']] if os.environ.get('CIUKI_TEST_CURSOR_HEADER') else []),'-include',str(root/'tests/host/desktop/replay_base.h'),
                '-include',str(root/'tests/host/desktop/paint_probe.h'),'-I',str(root/'src/kernel/include'),
                '-I',str(root/'apps/desktop'),*map(str,sources),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)


if __name__ == '__main__':
    unittest.main()
