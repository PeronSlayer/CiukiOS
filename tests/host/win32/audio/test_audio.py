"""Host tests of the production Win32 mixer; no audio device or guest access."""
import os
from pathlib import Path
import subprocess
import unittest
ROOT=Path(__file__).resolve().parents[4]
class Audio(unittest.TestCase):
    def test_pcm_and_com(self):
        out=ROOT/'build/host/win32/audio';out.mkdir(parents=True,exist_ok=True)
        exe=out/'pcm'
        subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror','-DW32_HOST_MODEL','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(ROOT/'sdk/win32/include'),str(ROOT/'tests/host/win32/audio/pcm_test.c'),str(ROOT/'sdk/win32/audio/mix.c'),str(ROOT/'sdk/win32/services/bytes.c'),'-lm','-o',str(exe)],check=True)
        p=subprocess.run([exe],capture_output=True,text=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
        self.assertEqual(p.returncode,0,p.stdout+p.stderr)
        self.assertIn('PASS PCM exact',p.stdout)

    def test_synth(self):
        from .fixture import soundfont
        out=ROOT/'build/host/win32/audio';out.mkdir(parents=True,exist_ok=True)
        fixture=out/'authored.sf2';fixture.write_bytes(soundfont())
        for optimized in (False,True):
            exe=out/('synth-cost' if optimized else 'synth')
            flags=['-O2'] if optimized else ['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all']
            subprocess.run(['clang','-std=c17',*flags,'-Wall','-Wextra','-Werror','-DW32_HOST_MODEL','-I'+str(ROOT/'sdk/win32/include'),str(ROOT/'tests/host/win32/audio/synth_test.c'),str(ROOT/'sdk/win32/services/bytes.c'),'-lm','-o',str(exe)],check=True)
            p=subprocess.run([exe,fixture],capture_output=True,text=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
            self.assertEqual(p.returncode,0,p.stdout+p.stderr)
            self.assertIn('PASS synth',p.stdout)
            if optimized:(ROOT/'build/codex/synth-cost.txt').write_text(p.stdout)

    def test_bank_derivative(self):
        import importlib.util
        from .fixture import soundfont
        spec=importlib.util.spec_from_file_location('banks',ROOT/'assets/audio/soundfonts/prepare.py')
        m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
        full=soundfont();reduced=m.reduce(full)
        self.assertEqual(m.coverage(full)['gm_programs'],128)
        self.assertEqual(m.coverage(reduced)['gm_programs'],128)
        self.assertEqual(reduced,m.reduce(full))
        self.assertLess(len(reduced),len(full))
        self.assertLessEqual(len(reduced),32*1024*1024)
        with self.assertRaises(ValueError):m.coverage(full[:-1])

    def test_runtime_wave_and_timers(self):
        out=ROOT/'build/host/win32/audio';out.mkdir(parents=True,exist_ok=True)
        exe=out/'runtime'
        subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror','-DW32_HOST_MODEL','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(ROOT/'sdk/win32/include'),'-I'+str(ROOT/'src/kernel/include'),str(ROOT/'tests/host/win32/audio/runtime_test.c'),*[str(ROOT/'sdk/win32/audio'/s) for s in ['mix.c','wave.c','timers.c']],str(ROOT/'sdk/win32/services/bytes.c'),'-o',str(exe)],check=True)
        p=subprocess.run([exe],capture_output=True,text=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
        self.assertEqual(p.returncode,0,p.stdout+p.stderr)
        self.assertIn('PASS native audio',p.stdout)

    def test_pe_exports_imports_and_i686(self):
        import json,re
        from scripts.win32 import builtins,census,stubs
        out=ROOT/'build/host/win32/audio/pe'
        manifest=builtins.build(out)
        store=json.loads(manifest.read_text())
        export_sets={d['name']:{name for e in census.inspect((out/d['path']).read_bytes())['exports'] for name in e['names']} for d in store['dlls']}
        for dll in ('dsound.dll','winmm.dll'):
            if dll=='dsound.dll':
                exports=census.inspect((out/dll).read_bytes())['exports']
                self.assertEqual(next(e['ordinal'] for e in exports if 'DirectSoundCreate' in e['names']),1)
                self.assertEqual(next(e['ordinal'] for e in exports if 'DirectSoundCreate8' in e['names']),11)
            coverage=json.loads((out/(dll+'-coverage.json')).read_text())
            required=stubs.imports((ROOT/'docs/design/research/win32-layer-2026-10-14.md').read_text())[dll]
            self.assertEqual(set(coverage),required)
            self.assertTrue(set(coverage.values())<={'implemented','W21 stub'})
            for imported in census.inspect((out/dll).read_bytes())['imports']:
                for symbol in imported.get('symbols',[]):
                    if not symbol.startswith('#'):self.assertIn(symbol,export_sets[imported['dll'].lower()])
        obj=out/'integer-mix.obj'
        subprocess.run(['clang','--target=i686-pc-windows-msvc','-march=pentiumpro','-O2','-ffreestanding','-fno-builtin','-mno-sse','-mno-sse2','-mno-mmx','-I'+str(ROOT/'sdk/win32/include'),'-c',str(ROOT/'sdk/win32/audio/mix.c'),'-o',str(obj)],check=True)
        symbols=subprocess.run(['llvm-nm','--undefined-only',obj],check=True,capture_output=True,text=True).stdout
        self.assertNotRegex(symbols,r'__(?:aull|all|u?divdi|u?moddi|muldi)')
        for unit in ('integer-mix.obj','audio-synth.obj','audio-dsound.obj'):
            text=subprocess.run(['llvm-objdump','-d',out/unit],check=True,capture_output=True,text=True).stdout
            self.assertNotRegex(text,r'\b(?:xmm\d|mm[0-7])\b')

    def test_midi_stream(self):
        out=ROOT/'build/host/win32/audio';out.mkdir(parents=True,exist_ok=True)
        exe=out/'midi-stream'
        subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror','-DW32_HOST_MODEL','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(ROOT/'sdk/win32/include'),'-I'+str(ROOT/'src/kernel/include'),str(ROOT/'tests/host/win32/audio/midi_stream_test.c'),str(ROOT/'sdk/win32/audio/mix.c'),str(ROOT/'sdk/win32/services/bytes.c'),'-o',str(exe)],check=True)
        p=subprocess.run([exe],capture_output=True,text=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
        self.assertEqual(p.returncode,0,p.stdout+p.stderr)
        self.assertIn('PASS MIDI tempo',p.stdout)

    def test_mmio(self):
        out=ROOT/'build/host/win32/audio';out.mkdir(parents=True,exist_ok=True)
        exe=out/'mmio'
        subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror','-DW32_HOST_MODEL','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(ROOT/'sdk/win32/include'),'-I'+str(ROOT/'src/kernel/include'),str(ROOT/'tests/host/win32/audio/mmio_test.c'),str(ROOT/'sdk/win32/services/bytes.c'),'-o',str(exe)],check=True)
        p=subprocess.run([exe],capture_output=True,text=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
        self.assertEqual(p.returncode,0,p.stdout+p.stderr)
        self.assertIn('PASS MMIO',p.stdout)
