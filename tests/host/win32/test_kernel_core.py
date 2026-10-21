"""Production kernel mechanisms and differential PE validation; no guest execution."""
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[3]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
from scripts.win32 import fixtures, core_fixtures


class KernelCoreTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.out = ROOT/'build/host/win32/kernel'
        cls.out.mkdir(parents=True, exist_ok=True)
        cls.env = dict(os.environ, TMPDIR=str(cls.out), ASAN_OPTIONS='detect_leaks=0')
        cls.program = cls.out/'core'
        subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror',
                        '-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
                        '-fsanitize=address,undefined','-fno-sanitize-recover=all',
                        '-I',str(ROOT/'src/kernel/include'),
                        str(ROOT/'tests/host/win32/kernel_core_test.c'),
                        str(ROOT/'src/kernel/fs/path.c'),
                        str(ROOT/'tests/host/proc/signal_legacy.c'),
                        str(ROOT/'tests/host/proc/desktop_legacy.c'),'-o',str(cls.program)],
                       check=True, env=cls.env)
        cls.corpus = ROOT/'build/host/win32/kernel/corpus'
        fixtures.build(cls.corpus)

    def test_production_mechanisms(self):
        result = subprocess.run([str(self.program)],capture_output=True,text=True,env=self.env,timeout=30)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('w32 core: checks=',result.stdout)
        print(result.stdout,end='')

    def test_f5_02_structural_rules_in_production(self):
        # Import/resource/TLS walker failures belong to f5-05; the core checks
        # structural headers/sections/directories only. Use the census parser's
        # constructor, not a complete import walk, as the reference boundary.
        from scripts.win32.census import PE, PEError
        for path in sorted(self.corpus.glob('*')):
            if path.suffix not in ('.exe','.dll'):
                continue
            with self.subTest(image=path.name):
                try:
                    PE(path.read_bytes()); expected='valid'
                except PEError as error:
                    expected=str(error)
                result=subprocess.run([str(self.program),'--pe',str(path)],
                                      capture_output=True,text=True,env=self.env,timeout=5)
                self.assertEqual(result.returncode,0,result.stderr)
                self.assertEqual(result.stdout.strip(),expected)

    def test_bootstrap_is_freestanding_and_uses_original_nested_seh(self):
        from scripts.win32.census import PE
        info=core_fixtures.build()
        loader=PE(Path(info['loader']).read_bytes())
        self.assertEqual(loader.base,0x70010000)
        self.assertEqual(loader.imports(),[])
        source=next(s for s in fixtures.manifests() if s['id']=='seh-nested')
        independent=fixtures.compiled(source,self.corpus)
        self.assertEqual(Path(info['exe']).read_bytes(),independent.read_bytes())
        for name in ('CoreResult','CoreRelease'):
            self.assertLess(info[name],loader.size)

    def test_i386_elf_and_both_coff_layouts(self):
        source=self.out/'wire.c'
        source.write_text('#include <ciuki/w32_objects.h>\n'
                          '_Static_assert(sizeof(void*)==4,"pointer");\n'
                          '_Static_assert(sizeof(long)==4,"long");\n'
                          '_Static_assert(sizeof(uint16_t)==2,"WCHAR");\n'
                          '_Static_assert(_Alignof(struct ciuki_w32_context)==4,"context alignment");\n'
                          '_Static_assert(__builtin_offsetof(struct ciuki_w32_context,eip)==184,"EIP");\n'
                          '_Static_assert(__builtin_offsetof(struct ciuki_w32_context,extended)==204,"FXSAVE");\n'
                          '_Static_assert(__builtin_offsetof(struct ciuki_w32_teb,tls_slots)==0xe10,"TLS");\n'
                          'unsigned __attribute__((stdcall)) WinGate(unsigned a,unsigned b){return a+b;}\n')
        for target in ('i686-unknown-elf','i686-pc-windows-msvc','i686-w64-mingw32'):
            obj=self.out/(target+'.o')
            subprocess.run(['clang','--target='+target,'-march=pentiumpro','-ffreestanding',
                            '-fno-builtin','-Wall','-Wextra','-Werror',
                            '-I',str(ROOT/'src/kernel/include'),'-c',str(source),'-o',str(obj)],
                           check=True,env=self.env)
            names=subprocess.run(['llvm-nm','--defined-only',str(obj)],check=True,
                                 capture_output=True,text=True).stdout
            self.assertIn('WinGate' if target.endswith('elf') else '_WinGate@8',names)

    def test_runner_and_kernel_selectors(self):
        sys.path.insert(0,str(ROOT/'scripts/test'))
        from loader_model import selector
        suite=json.loads((ROOT/'tests/suites/f5-core.json').read_text())
        self.assertEqual([c['id'] for c in suite['cases']],
                         ['w32-exceptions-qemu-t23','w32-memory-qemu-t23','w32-objects-qemu-t23',
                          'w32-fullscreen-qemu-t23','w32-fixtures-qemu-t23','w32-registry-qemu-t23',
                          'w32-kernel32-qemu-t23','w32-audio-qemu-t23','w32-gdi32-qemu-t23','w32-crash-qemu-t23','w32-user32-qemu-t23','w32-codecs-qemu-t23','w32-media-qemu-t23','w32-cdaudio-qemu-t23','w32-upper-dlls-qemu-t23','w32-ddraw-qemu-t23','w32-input-qemu-t23','w32-dialogs-qemu-t23','w32-installer-nsis-qemu-t23','w32-restart-qemu-t23'])
        for case in suite['cases']:
            if case['probe']=='w32-installer-nsis':
                self.assertIn('authored, unrun',case['integration_pending'])
                self.assertEqual(case['selector'],'f5:w32-installer-nsis run={run_id}')
                self.assertEqual([p['where']['case'] for p in case['expected']['predicates']],
                                 ['installer_install','installer_cancel','installer_restart_request','installer_uninstall'])
                for predicate in case['expected']['predicates']:
                    self.assertEqual(predicate['fields'],{'errors':{'eq':0},'manifest':{'eq':1}})
                continue
            if case['probe']=='w32-restart':
                self.assertIn('f5-21',case['integration_pending'])
                with self.assertRaises(ValueError):
                    selector(case['selector'].format(run_id='12345678'),'fw_cfg',True)
                self.assertIn('authored, unrun',case['integration_pending'])
                self.assertEqual(case['selector'],'f5:w32-restart run={run_id}')
                self.assertTrue(all(p['fields']['errors']=={'eq':0} for p in case['expected']['predicates']))
                continue
            if case['probe']=='w32-upper-dlls':
                self.assertIn('authored, unrun',case['integration_pending'])
                self.assertEqual(case['selector'],'f5:w32-upper-dlls run={run_id}')
                # f5-20D: one record per staged DLL, then the totals and the teardown.
                self.assertEqual([p['where']['case'] for p in case['expected']['predicates']],
                                 ['write-gate','fixture','upper_dll','upper_dlls','upper_teardown'])
                upper=json.loads((ROOT/'config/wine-upper.json').read_text())
                staged=sum(1 for r in upper['dlls'].values() if r['qualified'])
                self.assertEqual(case['expected']['predicates'][2]['exact_count'],staged)
                self.assertEqual(case['expected']['predicates'][3]['fields']['dlls'],{'eq':staged})
                self.assertIn('CIUKI_F5_PROBE("w32-upper-dlls"',(ROOT/'src/kernel/probes/f2_probes_desktop.c').read_text())
                continue
            if case['probe']=='w32-dialogs':
                self.assertIn('authored, unrun',case['integration_pending'])
                self.assertEqual(case['selector'],'f5:w32-dialogs run={run_id}')
                self.assertEqual([p['where']['case'] for p in case['expected']['predicates']],
                                 ['write-gate','dialog_units','dialog_controls','dialog_keyboard','dialog_menu',
                                  'dialog_msgbox','font_bridge','font_budget','dialog_teardown'])
                self.assertIn('CIUKI_F5_PROBE("w32-dialogs"',(ROOT/'src/kernel/probes/f2_probes_desktop.c').read_text())
                continue
            if case['probe']=='w32-ddraw':
                self.assertIn('authored, unrun',case['integration_pending'])
                self.assertEqual(case['selector'],'f5:w32-ddraw run={run_id}')
                continue
            if case['probe']=='w32-input':
                self.assertIn('authored, unrun',case['integration_pending'])
                self.assertEqual(case['selector'],'f5:w32-input run={run_id}')
                self.assertEqual([p['where']['case'] for p in case['expected']['predicates']],
                                 ['input_layouts','input_formats','input_buffering','input_cooperative','input_relative_warp','input_teardown'])
                self.assertTrue(all(p['fields']['errors']=={'eq':0} for p in case['expected']['predicates']))
                continue
            if case['probe']=='w32-codecs':
                self.assertIn('authored, unrun',case['integration_pending'])
                self.assertEqual(case['selector'],'f5:w32-codecs run={run_id}')
                self.assertEqual(case['expected']['predicates'][0]['fields']['dlls'],{'eq':8})
                continue
            if case['probe']=='w32-media':
                self.assertIn('authored, unrun',case['integration_pending'])
                self.assertEqual(case['selector'],'f5:w32-media run={run_id}')
                self.assertEqual(case['expected']['predicates'][0]['fields']['errors'],{'eq':0})
                continue
            if case['probe']=='w32-audio':
                self.assertIn('authored, unrun',case['integration_pending'])
                self.assertEqual(case['expected']['predicates'][0]['fields']['lead'],{'le':512})
                continue
            if case['probe']=='w32-gdi32':
                self.assertIn('authored, unrun',case['integration_pending'])
                self.assertEqual(case['selector'],'f5:w32-gdi32 run={run_id}')
                self.assertEqual([p['where']['case'] for p in case['expected']['predicates']],
                                 ['gdi_dib','gdi_regions_palettes_text','gdi_teardown'])
                continue
            if case['probe']=='w32-crash':
                self.assertIn('authored, unrun',case['integration_pending'])
                self.assertEqual(case['selector'],'f5:w32-crash run={run_id}')
                continue
            if case['probe']=='w32-user32':
                self.assertIn('authored, unrun',case['integration_pending'])
                self.assertEqual(case['selector'],'f5:w32-user32 run={run_id}')
                self.assertEqual([p['where']['case'] for p in case['expected']['predicates']],
                                 ['user32_sends','user32_input','user32_paint','user32_teardown'])
                continue
            if case['probe']=='w32-registry':
                self.assertIn('authored, unrun',case['integration_pending'])
                self.assertEqual(case['selector'],'f5:w32-registry run={run_id}')
                self.assertEqual([p['where']['case'] for p in case['expected']['predicates']],
                                 ['registry_recovery','registry_restart','registry_writers'])
                continue
            if case['probe']=='w32-fullscreen':
                # f5-08 authors the unrun case; f5-09 owns its desktop driver.
                self.assertIn('f5-09',case['integration_pending'])
                self.assertEqual(case['selector'],'f5:w32-fullscreen run={run_id}')
                self.assertEqual([p['where']['case'] for p in case['expected']['predicates']],
                                 ['indexed_640480','indexed_320200','lease'])
                continue
            if case['probe']=='w32-kernel32':
                # f5-17B lead note: the case awaits its owner's guest driver
                # (f5-21b); the host model must reject it just like both real
                # tables. f5-21 registered w32-fixtures in all three tables.
                with self.assertRaises(ValueError):
                    selector(case['selector'].format(run_id='12345678'),'fw_cfg',True)
                continue
            selected=selector(case['selector'].format(run_id='12345678'),'fw_cfg',True)
            self.assertEqual(selected['phase'],5)
            self.assertEqual(selected['probe'],case['probe'])
            if case['probe'] in ('w32-objects','w32-fixtures'):
                continue
            native=[x for x in case['expected']['predicates'] if x['where'].get('case')=='native_sse']
            self.assertEqual(native[0]['fields'],{'vector':{'eq':6},'status':{'eq':4},'ud':{'eq':1}})
        # QEMU TCG never raises #XM (vector 19): nine faults are the TCG floor and the fixture
        # records tcg_fallback=1 for the exact TCG signature; off TCG a missing #XM is an error.
        exceptions=[x for x in suite['cases'][0]['expected']['predicates'] if x['where'].get('case')=='exceptions']
        self.assertEqual(exceptions[0]['fields']['faults'],{'ge':9})
        self.assertEqual(exceptions[0]['fields']['errors'],{'eq':0})
        self.assertIn('tcg_fallback',(ROOT/'tests/host/win32/fixtures/core_loader.c').read_text())
        payload=(ROOT/'tests/host/proc/payload.asm').read_text()
        self.assertIn("je sse_fault",payload)
        self.assertIn('core_spawn(parent,&native_image,"x",&native)',
                      (ROOT/'src/kernel/probes/f5_probes_core.c').read_text())
        with self.assertRaises(ValueError):
            selector('f5:unknown run=12345678','fw_cfg',True)


if __name__=='__main__':
    unittest.main()
