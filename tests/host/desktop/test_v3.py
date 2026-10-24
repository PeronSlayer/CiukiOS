"""Protocol v3 production harness; deterministic frames are host evidence only."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import unittest
from .test_desktop import ROOT, SHELL_SOURCES, load
from .frame_review import capture


class DesktopV3Tests(unittest.TestCase):
    def test_protocol_lifecycle_input_shell_and_frames(self):
        out = ROOT / 'build/codex/f5-09-frames'
        out.mkdir(parents=True, exist_ok=True)
        work = ROOT / 'build/host/desktop-v3'
        work.mkdir(parents=True, exist_ok=True)
        portrait = work / 'portrait.xrgb'
        converter = load('v3_portrait', ROOT / 'apps/desktop/convert_portrait.py')
        portrait.write_bytes(converter.convert(ROOT / 'assets/brand/ciuki-logo.png'))
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0')
        binary = work / 'v3-test'
        sources = [ROOT / 'apps/desktop' / n for n in ('protocol.c', 'client.c', 'input.c', 'compositor.c', 'theme.c')]
        sources += SHELL_SOURCES
        sources += [ROOT / 'apps/libdeskui' / n for n in ('icons.c', 'icon_map.c', 'v3.c')]
        sources += [ROOT / 'tests/host/desktop' / n for n in ('fake.c', 'v3_test.c')]
        subprocess.run(['cc', '-std=c17', '-D_POSIX_C_SOURCE=200809L', '-include',
                        str(ROOT / 'tests/host/desktop/replay_base.h'), '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        '-I', str(ROOT / 'apps/desktop'), '-I', str(ROOT / 'src/kernel/include'),
                        *(str(s) for s in sources), '-o', str(binary)], check=True, env=env)
        result = subprocess.run([str(binary), str(portrait), str(out)], env=env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        (ROOT / 'build/codex/f5-09-v3.log').write_text(result.stdout + result.stderr)
        record = {}
        for theme in ('ciuki-blue', 'platinum'):
            for case in ('overlapped', 'popup', 'tool', 'topmost', 'alt-tab', 'alt-tab-committed', 'alt-enter', 'ctrl-alt-del', 'programs-folder', 'crash', 'crash-details', 'restart',*[f'refuse-{i}' for i in range(1,8)]):
                name = f'{theme}-{case}'
                data = (out / (name + '.xrgb')).read_bytes()
                self.assertEqual(len(data), 800 * 600 * 4)
                record[name] = hashlib.sha256(data).hexdigest()
                capture('f5-09-' + name, data, 800, 600)
        goldens = ROOT / 'tests/host/desktop/v3-goldens.json'
        self.assertEqual(record, json.loads(goldens.read_text()))

    def test_production_presenter_adapter(self):
        out=ROOT/'build/codex/f5-09-frames'
        out.mkdir(parents=True,exist_ok=True)
        work=ROOT/'build/host/desktop-v3';work.mkdir(parents=True,exist_ok=True)
        portrait=work/'portrait.xrgb'
        converter=load('adapter_portrait',ROOT/'apps/desktop/convert_portrait.py')
        portrait.write_bytes(converter.convert(ROOT/'assets/brand/ciuki-logo.png'))
        env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0')
        common=['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror',
                '-fsanitize=address,undefined','-ffunction-sections','-fdata-sections',
                '-DDESKTOP_ADAPTER_FIXTURE','-I'+str(ROOT/'src/kernel/include')]
        obj=work/'presenter.o'
        subprocess.run([*common,'-Dmain=presenter_baseline_main','-c',
                        str(ROOT/'tests/host/display_present_test.c'),'-o',str(obj)],check=True,env=env)
        sources=[ROOT/'apps/desktop'/n for n in ('protocol.c','client.c','input.c','compositor.c','theme.c','win32_presenter.c')]
        sources+=SHELL_SOURCES
        sources+=[ROOT/'apps/libdeskui'/n for n in ('icons.c','icon_map.c','v3.c')]
        sources+=[ROOT/'tests/host/desktop'/n for n in ('fake.c','v3_test.c')]
        sources+=[ROOT/'src/kernel/vm'/n for n in ('vga_presenter.c','virtual_vga.c')]
        binary=work/'presenter-adapter-test'
        subprocess.run([*common,'-D_POSIX_C_SOURCE=200809L','-include',str(ROOT/'tests/host/desktop/replay_base.h'),
                        '-I'+str(ROOT/'apps/desktop'),*(str(p) for p in sources),str(obj),
                        '-pthread','-Wl,--gc-sections','-o',str(binary)],check=True,env=env)
        result=subprocess.run([binary,portrait,out],capture_output=True,text=True,env=env,timeout=30)
        (ROOT/'build/codex/f5-09b2-presenter.log').write_text(result.stdout+result.stderr)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_guest_fixture_and_predicate_dry_run(self):
        import copy
        import sys
        import tempfile
        work=ROOT/'build/host/desktop-v3';work.mkdir(parents=True,exist_ok=True)
        env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0')
        common=['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror',
                '-fsanitize=address,undefined','-ffunction-sections','-fdata-sections',
                '-DDESKTOP_ADAPTER_FIXTURE','-DDESKTOP_GUEST_FIXTURE','-I'+str(ROOT/'src/kernel/include')]
        obj=work/'guest-presenter.o'
        subprocess.run([*common,'-Dmain=presenter_baseline_main','-c',str(ROOT/'tests/host/display_present_test.c'),'-o',str(obj)],check=True,env=env)
        peer=work/'guest-peer.o'
        subprocess.run([*common,'-DDESKTOP_V3_HOST','-Dentry=desktop_v3_peer_entry','-Dmemset=peer_memset','-Dmemcpy=peer_memcpy',
                        '-I'+str(ROOT/'sdk/win32/include'),'-c',str(ROOT/'src/probes/desktop_v3/peer.c'),'-o',str(peer)],check=True,env=env)
        io=work/'guest-io.o'
        subprocess.run([*common,'-I'+str(ROOT/'sdk/win32/include'),'-c',str(ROOT/'apps/w32srv/posix_io.c'),'-o',str(io)],check=True,env=env)
        sources=[ROOT/'apps/desktop'/n for n in ('protocol.c','client.c','input.c','compositor.c','theme.c','win32_presenter.c','v3_fixture.c','win32_registry.c')]
        sources+=SHELL_SOURCES
        sources+=[ROOT/'apps/libdeskui'/n for n in ('icons.c','icon_map.c','v3.c')]
        sources+=[ROOT/'tests/host/desktop'/n for n in ('fake.c','v3_test.c')]
        sources+=[ROOT/'src/kernel/vm'/n for n in ('vga_presenter.c','virtual_vga.c')]
        sources+=[ROOT/'apps/w32srv'/f'{n}.c' for n in ('registry','storage','directory','defaults')]
        binary=work/'guest-fixture-test'
        subprocess.run([*common,'-D_POSIX_C_SOURCE=200809L','-include',str(ROOT/'tests/host/desktop/replay_base.h'),
                        '-I'+str(ROOT/'apps/desktop'),'-I'+str(ROOT/'sdk/win32/include'),
                        '-idirafter',str(ROOT/'sdk/sysroot-overlay/include'),*(str(p) for p in sources),str(obj),str(peer),str(io),
                        '-pthread','-no-pie','-Wl,--gc-sections','-o',str(binary)],check=True,env=env)
        with tempfile.TemporaryDirectory(dir=work) as directory:
            result=subprocess.run([binary,directory],capture_output=True,text=True,env=env,timeout=30)
        (ROOT/'build/codex/f5-09b2-guest-fixture.log').write_text(result.stdout+result.stderr)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        sys.path.insert(0,str(ROOT/'scripts/test'))
        from evidence import Parser, EvidenceError
        case=next(c for c in json.loads((ROOT/'tests/suites/f2-desktop.json').read_text())['cases'] if c['id']=='desktop-v3-integration-qemu-t23')
        # The unchanged isolation battery uses its production kernel controller
        # and deterministic host scheduling; append actual guest-fixture phases.
        kernel=work/'guest-isolation-controller'
        subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
                        '-I'+str(ROOT/'src/kernel/include'),str(ROOT/'tests/host/proc/desktop_test.c'),
                        str(ROOT/'tests/host/proc/signal_legacy.c'),str(ROOT/'src/kernel/lib/sha256.c'),
                        str(ROOT/'src/kernel/lib/fmt.c'),str(ROOT/'src/kernel/probes/selector.c'),
                        '-o',str(kernel)],check=True,env=env)
        baseline=subprocess.run([kernel,'--isolation-records'],capture_output=True,text=True,env=env,timeout=30)
        self.assertEqual(baseline.returncode,0,baseline.stdout+baseline.stderr)
        (ROOT/'build/codex/f5-09b2-isolation-model.log').write_text(baseline.stdout+baseline.stderr)
        parser=Parser('12345678','crash-isolation')
        seq=0
        import re
        for output,excluded in ((baseline.stdout,{'END'}),(result.stdout,{'BEGIN'})):
            for line in output.splitlines():
                if not line.startswith('CIUKI_TEST ') or any(f'event={e} ' in line for e in excluded):continue
                seq+=1
                line=re.sub(r'seq=[0-9]+',f'seq={seq:06d}',line)
                parser.feed((line+'\n').encode())
        expected=copy.deepcopy(case['expected'])
        parser.check(expected)
        self.assertEqual(len([r for r in parser.records if r.get('case')=='desktop-v3']),8)
        for row in [r for r in parser.records if r.get('case')=='desktop-v3']:
            row['errors']='1'
            with self.assertRaises(EvidenceError):parser.check(expected)
            row['errors']='0'

    def test_add_remove_page(self):
        out = ROOT / 'build/codex/f5-09-frames'
        out.mkdir(parents=True, exist_ok=True)
        work = ROOT / 'build/host/desktop-v3'
        work.mkdir(parents=True, exist_ok=True)
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0')
        icons = load('v3_control_icons', ROOT / 'tests/host/desktop/icon_assets.py')
        ft = load('v3_control_ft', ROOT / 'sdk/freetype/build.py')
        library = ft.host()
        sources = ['tests/host/desktop/v3_control_test.c', 'apps/control/app.c', 'apps/control/driver_pack_ui.c', 'apps/control/display.c',
                   'apps/control/fonts.c', 'sdk/libfont/font.c', 'apps/libdeskui/text_ttf.c', 'apps/desktop/protocol.c',
                   *(f'apps/desktop/{n}.c' for n in ('settings', 'sounds', 'wallpaper', 'image', 'compositor', 'theme')),
                   *(f'apps/libdeskui/{n}.c' for n in ('draw', 'widgets', 'list', 'menu', 'dialog', 'controls', 'commdlg', 'textarea', 'icons', 'icon_map'))]
        binary = work / 'v3-control-test'
        subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-D_POSIX_C_SOURCE=200809L',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', *icons.defines(),
                        '-I', str(ROOT / 'apps/control'), '-I', str(ROOT / 'apps/libdeskui'),
                        '-I', str(ROOT / 'src/kernel/include'), '-idirafter', str(ROOT / 'sdk/sysroot-overlay/include'),
                        *ft.includes(), *(str(ROOT / s) for s in sources), str(library), '-pthread', '-lm', '-o', str(binary)],
                       check=True, env=env)
        result = subprocess.run([str(binary), str(out)], capture_output=True, text=True, env=env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        (ROOT / 'build/codex/f5-09-control.log').write_text(result.stdout + result.stderr)
        record = {}
        for theme in ('ciuki-blue', 'platinum'):
            name = theme + '-add-remove'
            data = (out / (name + '.xrgb')).read_bytes()
            self.assertEqual(len(data), 560 * 420 * 4)
            record[name] = hashlib.sha256(data).hexdigest()
            capture('f5-09-' + name, data, 560, 420)
        self.assertEqual(record, json.loads((ROOT / 'tests/host/desktop/v3-control-goldens.json').read_text()))
