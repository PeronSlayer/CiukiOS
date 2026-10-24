"""G26C full-layout pagination; upstream headers are consumed read-only."""
from pathlib import Path
import os
import subprocess
import unittest
ROOT=Path(__file__).resolve().parents[3]
class BrowserIntegration(unittest.TestCase):
    def test_document_printing(self):
        base=Path(os.environ.get('CIUKI_NETSURF_BASE',ROOT/'build/external/netsurf-host'))
        if not (base/'source/netsurf').is_dir():
            base=Path('/home/peronslayer/Desktop/CiukiOS/build/external/netsurf-host')
        source=base/'source/netsurf'
        self.assertTrue((source/'include/netsurf/browser_window.h').is_file(),'prepared upstream headers required')
        out=ROOT/'build/host/g26c';out.mkdir(parents=True,exist_ok=True)
        command=['clang','-std=c17','-D_POSIX_C_SOURCE=200809L','-O1','-g','-Wall','-Wextra','-Werror',
                 '-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(ROOT),
                 '-I'+str(ROOT/'src/kernel/include'),'-include',str(ROOT/'tests/host/desktop/replay_base.h'),'-I'+str(base/'prefix/include'),'-I'+str(source),'-I'+str(source/'include'),'-I'+str(source/'frontends'),
                 str(ROOT/'tests/host/netsurf/print_document_test.c'),str(ROOT/'sdk/netsurf/port/printing.c'),
                 str(ROOT/'apps/libprint/print.c'),'-Wl,--gc-sections','-ffunction-sections','-fdata-sections','-o',str(out/'printing')]
        subprocess.run(command,check=True)
        result=subprocess.run([out/'printing'],capture_output=True,text=True)
        print(result.stdout,end='');self.assertEqual(result.returncode,0,result.stdout+result.stderr)
    def test_internet_controls(self):
        from tests.host.desktop.test_desktop import load
        out=ROOT/'build/host/g26c';out.mkdir(parents=True,exist_ok=True)
        icons=load('g26c_icons',ROOT/'tests/host/desktop/icon_assets.py')
        ft=load('g26c_freetype',ROOT/'sdk/freetype/build.py');library=ft.host()
        sources=['tests/host/desktop/control_test.c','apps/control/app.c','apps/desktop/power_policy.c',
                 'apps/systools/devctl.c','apps/systools/profiles.c','apps/control/driver_pack_ui.c',
                 'apps/control/display.c','apps/control/fonts.c','sdk/libfont/font.c','apps/libdeskui/text_ttf.c',
                 'apps/desktop/protocol.c']
        sources += ['apps/desktop/'+name+'.c' for name in ('settings','sounds','wallpaper','image','compositor','theme')]
        sources += ['apps/libdeskui/'+name+'.c' for name in ('draw','widgets','list','menu','dialog','controls','commdlg','textarea','icons','icon_map')]
        subprocess.run(['clang','-std=c17','-D_POSIX_C_SOURCE=200809L','-O1','-g','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-sanitize-recover=all',*icons.defines(),
                        '-I'+str(ROOT/'apps/control'),'-I'+str(ROOT/'src/kernel/include'),
                        '-idirafter',str(ROOT/'sdk/sysroot-overlay/include'),*ft.includes(),
                        *[str(ROOT/p) for p in sources],str(library),'-pthread','-lm','-o',str(out/'internet')],check=True)
        result=subprocess.run([out/'internet','--internet-only'],capture_output=True,text=True)
        print(result.stdout,end='');self.assertEqual(result.returncode,0,result.stdout+result.stderr)
if __name__=='__main__':unittest.main()
