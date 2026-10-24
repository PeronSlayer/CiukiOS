"""fx-28 production Log Viewer, shell and toolkit, plus system-string atlas audit."""
import ast
import importlib.util
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from .test_deskui import TOOLKIT
from . import icon_assets

ROOT = Path(__file__).resolve().parents[3]

class ScreenshotReviewTests(unittest.TestCase):
    def test_idle_logview_menu_screenshot_and_delivery_deadline(self):
        parent=ROOT/'build/host'; parent.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='fx-28-',dir=parent) as tmp:
            work=Path(tmp); (work/'include').mkdir()
            (work/'include/ciuki').symlink_to(ROOT/'sdk/sysroot-overlay/include/ciuki',target_is_directory=True)
            sources=[*TOOLKIT,*(f'apps/desktop/{n}.c' for n in
                ('client','protocol','input','shell','autoplay','autoplay_content','folder','settings','wallpaper','keymap','clipboard')),
                'apps/files/model.c','apps/files/transfer.c',
                'apps/libdeskui/clipboard.c','apps/libdeskui/clipstore.c',
                'apps/control/display.c','apps/accessories/logview_model.c','apps/accessories/logview_view.c',
                'tests/host/desktop/resize_transport.c','tests/host/desktop/resize_client.c',
                'tests/host/desktop/screenshot_review_test.c']
            binary=work/'review'
            subprocess.run(['clang','-std=c17','-D_POSIX_C_SOURCE=200809L','-O1','-g','-Wall','-Wextra','-Werror',
                '-fsanitize=address,undefined','-fno-sanitize-recover=all','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-include',str(ROOT/'tests/host/desktop/replay_base.h'),
                '-I',str(ROOT/'src/kernel/include'),'-I',str(work/'include'),*icon_assets.defines(),
                *(str(ROOT/s) for s in sources),'-pthread','-o',str(binary)],check=True)
            result=subprocess.run([binary,work],capture_output=True,text=True,timeout=30)
            print(result.stdout,end=''); self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_all_system_strings_have_atlas_coverage(self):
        spec=importlib.util.spec_from_file_location('ui_fonts',ROOT/'apps/desktop/convert_fonts.py')
        fonts=importlib.util.module_from_spec(spec); spec.loader.exec_module(fonts)
        coverage=set(fonts.CODES); missing=[]; points=set()
        # Strip comments while retaining string/character tokens: URLs and comment
        # delimiters inside strings must survive, and UTF-8 byte escapes decode.
        token=re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|/\*.*?\*/|//[^\n]*',re.S)
        for path in sorted((ROOT/'apps').rglob('*')):
            if path.suffix not in ('.c','.h','.inc') or path.name in ('ui_fonts.h','stb_image.h'): continue
            for match in token.finditer(path.read_text(errors='strict')):
                raw=match.group()
                if not raw.startswith('"'): continue
                # Python interprets C hex/octal escapes. Re-encode raw UTF-8
                # literals first so they and escaped byte strings decode alike.
                value=ast.literal_eval('b'+raw.encode('utf-8').decode('ascii','backslashreplace'))
                try: value=value.decode('utf-8')
                except UnicodeDecodeError: value=value.decode('latin-1')
                for c in value:
                    cp=ord(c)
                    if cp<32 or cp in (127,0xfeff): continue  # encoding signature, never UI ink
                    points.add(cp)
                    if cp not in coverage: missing.append((str(path.relative_to(ROOT)),f'U+{cp:04X}'))
        self.assertFalse(missing,missing[:40])
        self.assertTrue({0x2013,0x2014,0x2026,0x203a}<=points)
        subprocess.run(['python3',str(ROOT/'apps/desktop/convert_fonts.py'),'--check'],check=True)
        print(f'PASS fx-28 all apps C string literals: {len(points)} code points covered (including escaped UTF-8)')

if __name__=='__main__': unittest.main()
