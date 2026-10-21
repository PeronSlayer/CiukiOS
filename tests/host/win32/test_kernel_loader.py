"""The 137-image corpus through the real CIUKI_SYS_W32 backend and kernel VM.
H0 structural/callback model only; PE instructions are never executed here.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT))
from scripts.win32 import fixtures,builtins
from tests.host.win32 import test_loader

class KernelLoaderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # The dialogs integration test uses this same kernel-backed loader
        # before handing its returned bytes to the separate USER/GDI model.
        if getattr(cls, '_prepared', False):
            return
        cls.out=ROOT/'build/host/win32/kernel-loader';cls.out.mkdir(parents=True,exist_ok=True)
        cls.env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0')
        cls.images=cls.out/'images';cls.hashes=fixtures.build(cls.images)
        flags=['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror',
            '-ffunction-sections','-fdata-sections','-fsanitize=address,undefined','-fno-sanitize-recover=all',
            '-I'+str(ROOT/'src/kernel/include')]
        cls.bridge=cls.out/'bridge.o'
        subprocess.run([*flags,'-c',str(ROOT/'tests/host/win32/kernel_loader_bridge.c'),'-o',str(cls.bridge)],check=True)
        sdk=ROOT/'sdk/win32';objects=[cls.bridge]
        common=[*flags,'-DW32_HOST_MODEL','-DW32_KERNEL_MODEL','-ffreestanding','-fno-builtin',
            '-I'+str(sdk/'include'),'-I'+str(ROOT/'tests/host/win32/adapter')]
        sources=sorted((sdk/'ldr').glob('*.c'))+sorted((sdk/'ntdll').glob('*.c'))+sorted((sdk/'kernel32').glob('*.c'))+[
            sdk/'services/ciuki.c',sdk/'services/start.c',ROOT/'tests/host/win32/adapter/model.c',
            ROOT/'tests/host/win32/adapter/main.c',ROOT/'tests/host/win32/kernel_loader_main.c',ROOT/'tests/host/win32/kernel32_runtime.c']
        for i,source in enumerate(sources):
            obj=cls.out/f'core-{i}.o';objects.append(obj)
            extra=['-Dmain=adapter_model_main'] if source.name=='model.c' else []
            subprocess.run([*common,*extra,'-c',str(source),'-o',str(obj)],check=True)
        cls.program=cls.out/'kernel-loader'
        subprocess.run([*flags,'-Wl,--gc-sections',*map(str,objects),str(ROOT/'src/kernel/fs/path.c'),
            str(ROOT/'tests/host/proc/signal_legacy.c'),str(ROOT/'tests/host/proc/desktop_legacy.c'),
            '-pthread','-o',str(cls.program)],check=True)
        cls._prepared = True

    def command(self,mode,image,expected=0):
        result=subprocess.run([self.program,self.images,mode,'/'+image.name,'/','observe'],
            capture_output=True,text=True,env=self.env,timeout=30)
        self.assertEqual(result.returncode,expected,result.stdout+result.stderr)
        self.assertIn('KERNEL_MODEL teardown PASS',result.stdout)
        return result.stdout

    def test_entire_corpus_on_kernel_personality_model(self):
        peaks=[];results=[]
        for spec in fixtures.manifests():
            image=next(self.images/n for n in self.hashes if Path(n).stem==spec['id'])
            error=spec['census'].get('error')
            refusal=(error,193) if error else test_loader.LoaderTests.RUNTIME_REFUSALS.get(spec['id'])
            with self.subTest(image=image.name):
                out=self.command('validate',image,1 if error else 0)
                self.assertIn('REFUSE '+error+' 193' if error else 'PASS validate',out)
                out=self.command('load',image,1 if refusal else 0)
                self.assertIn(f'REFUSE {refusal[0]} {refusal[1]}' if refusal else 'PASS load',out)
                if refusal:self.assertIn('[win32] load',out)
                import re
                peak=int(re.search(r'peak_bytes=(\d+)',out)[1]);peaks.append(peak)
                results.append({'fixture':spec['id'],'refusal':refusal,'peak_bytes':peak})
        self.assertEqual(len(results),137)
        (self.out/'corpus-results.json').write_text(json.dumps(results,indent=2)+'\n')
        print(f'kernel personality corpus: {len(results)}/137; peak bytes {min(peaks)}..{max(peaks)}')

    def test_stub_records_have_process_budget_and_keep_fatal_load_diagnostics(self):
        out=self.command('diagnostics',self.images/'console.exe')
        self.assertEqual(out.count('[win32] stub fixture.dll!FixtureMissing'),1)
        self.assertIn('[win32] load check=missing-export',out)
        self.assertIn('pid=2 tid=1',out)

    def test_kernel32_wrappers_on_kernel_model(self):
        out=self.command("kernel32",self.images/"console.exe")
        self.assertIn("PASS kernel32 VM heap objects waits TLS codepages time environment",out)
        self.assertIn("PASS kernel32 f5-20A process queries heap compaction",out)

    def dialogs_fixture_output(self):
        import shutil
        builtins.build(self.images/'System/Win32')
        target=self.images/'System/Win32/Fixtures';target.mkdir(exist_ok=True)
        shutil.copyfile(self.images/'System/Win32/fixtures/dialogs-exercise.exe',target/'dialogs-exercise.exe')
        return self.command('dialogs-startup',target/'dialogs-exercise.exe')

    def test_dialogs_fixture_startup_on_kernel_model(self):
        out=self.dialogs_fixture_output()
        self.assertIn('PASS dialogs startup old result collision=487 new result free commit RW zero GetModuleHandleA',out)
        self.assertIn('PASS dialogs compiled PE FindResource LoadResource LockResource SizeofResource',out)

    def test_upper_and_kernel32_result_pages_are_free_after_actual_imports(self):
        import shutil
        builtins.build(self.images/'System/Win32')
        target = self.images/'System/Win32/Fixtures'
        target.mkdir(exist_ok=True)
        for name in ('upper', 'kernel32'):
            image = name+'-exercise.exe'
            shutil.copyfile(self.images/'System/Win32/fixtures'/image, target/image)
            out = self.command(name+'-startup', target/image)
            self.assertIn('new result free commit RW zero GetModuleHandleA', out)

    def test_core_observer_result_pages_are_free_after_actual_imports(self):
        import shutil
        from scripts.win32 import core_observer_fixtures
        builtins.build(self.images/'System/Win32')
        target = self.images/'System/Win32/Fixtures'
        target.mkdir(exist_ok=True)
        for name in core_observer_fixtures.NAMES:
            with self.subTest(fixture=name):
                image = name+'-exercise.exe'
                shutil.copyfile(self.images/'System/Win32/fixtures'/image, target/image)
                out = self.command(name+'-exercise-startup', target/image)
                self.assertIn('new result free commit RW zero GetModuleHandleA', out)

    def test_fixed_address_virtualalloc_on_kernel_model(self):
        self.assertIn('PASS kernel32 fixed VirtualAlloc exact rounding collision=487 invalid=87 zero RW release',
                      self.command('fixed-alloc',self.images/'console.exe'))

    def test_kernel32_non_ascii_module_paths(self):
        import shutil
        directory=self.images/'café';directory.mkdir(exist_ok=True)
        shutil.copyfile(self.images/'exports.dll',directory/'Euro€.dll')
        self.assertIn('PASS kernel32 ACP1252 UTF8 module path round trip',
                      self.command('unicode-paths',self.images/'console.exe'))

    def test_kernel32_create_process_transaction(self):
        builtins.build(self.images/'System/Win32')
        self.assertIn("PASS kernel32 kernel CreateProcess",self.command("spawn",self.images/'console.exe'))

    def test_kernel32_search_and_spawn_exit_races(self):
        builtins.build(self.images/'System/Win32')
        source=self.out/'native-child.c';source.write_text('void _start(void) { for (;;) {} }\n')
        obj=self.out/'native-child.o'
        subprocess.run(['clang','--target=i686-unknown-elf','-ffreestanding','-fno-stack-protector',
                        '-c',str(source),'-o',str(obj)],check=True)
        subprocess.run(['ld.lld','-m','elf_i386','--entry=_start','-Ttext=0x400000',
                        '-o',str(self.images/'native-child.exe'),str(obj)],check=True)
        self.assertIn('PASS kernel32 search precedence=5',self.command('spawn-edges',self.images/'console.exe'))

    def test_runtime_services_on_kernel_model(self):
        self.assertIn('PASS kernel heap critical waits SEH filter',self.command('runtime',self.images/'console.exe'))
        out=self.command('startup',self.images/'compiled-tls.exe')
        self.assertIn('PASS kernel startup parameters TLS DATAFILE',out)
        self.assertIn('PASS kernel forced relocation COW',out)
        out=self.command('attach-fail',self.images/'consumer.exe')
        self.assertIn('PASS kernel attach failure start status rollback',out)
        self.assertIn('[win32] load check=dllmain-attach-failed',out)

    def test_production_builtin_store_through_kernel_loader(self):
        builtins.build(self.images/'System/Win32')
        from scripts.win32 import census
        coverage=json.loads((self.images/'System/Win32/kernel32-coverage.json').read_text())
        exports={name for e in census.inspect((self.images/'System/Win32/kernel32.dll').read_bytes())['exports'] for name in e['names']}
        expected=set()
        from scripts.win32 import stubs
        for source in ('docs/design/research/win32-layer-2026-10-14.md','docs/design/win32-census/fixtures.txt'):
            expected.update(stubs.imports((ROOT/source).read_text()).get('kernel32.dll',set()))
        self.assertEqual({e['name'] for e in coverage['exports']},expected)
        self.assertTrue(expected<=exports)
        self.assertEqual(coverage['deferred_vfs'],[])
        self.assertEqual(set(coverage['required_vfs']),{'LockFile','LockFileEx','UnlockFile','UnlockFileEx'})
        self.assertTrue(set(coverage['required_vfs'])<=exports)
        self.assertIn('provisional',coverage['scope'])
        self.assertIn('PASS kernel builtin store imports helpers pinning',self.command('store',self.images/'builtin-import.exe'))
        # f5-20A: every core-implement/core-w21-stub decision of the reused Wine
        # DLLs owned by kernel32/ntdll/advapi32/version resolves through the
        # production loader and store (forwarders into LZ32 included).
        decisions=[d for d in json.loads((ROOT/'config/wine-upper.json').read_text())['decisions']
                   if d.get('audit','f5-14b')=='f5-14b']
        owners=('kernel32.dll','ntdll.dll','advapi32.dll','version.dll')
        pairs=sorted({(d['owner'],d['name']) for d in decisions
                      if d['owner'] in owners and d['action'] in ('core-implement','core-w21-stub')})
        self.assertEqual(len({n for o,n in pairs}),len(pairs))
        self.assertEqual(len(pairs),110+24+47+3)
        (self.images/'System/Win32/upper-symbols.txt').write_text(''.join(f'{o} {n}\n' for o,n in pairs))
        out=self.command('upper-core',self.images/'builtin-import.exe')
        self.assertIn(f'PASS kernel f5-20A upper core exports={len(pairs)} loader references resources NT services',out)
        upper={row['name']:row for row in coverage['upper']}
        self.assertEqual(set(upper),{n for o,n in pairs if o=='kernel32.dll'})
        self.assertTrue({n for n,r in upper.items() if r['action']=='core-w21-stub'}==
                        {n for n,r in upper.items() if r['status']=='W21 typed failure'})
        self.assertEqual({n for n,r in upper.items() if r['status'].startswith('forwarder')},
                         {'LZClose','LZCopy','LZOpenFileW','LZRead','LZSeek'})
        facades=json.loads((self.images/'System/Win32/facades-upper-coverage.json').read_text())
        self.assertEqual({k:len(v) for k,v in facades.items()},{'advapi32.dll':47,'version.dll':3})
        # f5-20B: every user32/gdi32 decision and the amendment's kernel32
        # Unicode functions (with their new ANSI cores) resolve too.
        names=sorted({(d['owner'],d['name']) for d in decisions
                      if d['owner'] in ('user32.dll','gdi32.dll') and d['action'] in ('core-implement','core-w21-stub')})
        self.assertEqual(len(names),108+55)
        names+=[('kernel32.dll',n) for n in ('CreateFileW','LoadLibraryW','LoadLibraryExW','GetModuleHandleW',
            'GetModuleFileNameW','CreateEventW','CreateMutexW','GetVersionExW','GetEnvironmentVariableW',
            'GetCommandLineW','CreateProcessW','CompareStringW','CompareStringA','GetLocaleInfoW',
            'GetStringTypeW','LCMapStringW','LCMapStringA')]
        (self.images/'System/Win32/upper-names.txt').write_text(''.join(f'{o} {n}\n' for o,n in names))
        out=self.command('upper-names',self.images/'builtin-import.exe')
        self.assertIn(f'PASS kernel f5-20B upper user32 gdi32 kernel32 exports={len(names)}',out)
        for name in ('user32','gdi32'):
            rows=json.loads((self.images/f'System/Win32/{name}-coverage.json').read_text())['upper']
            self.assertEqual({r['name'] for r in rows},{n for o,n in names if o==name+'.dll'})
            self.assertTrue(all(r['status']!='undecided' for r in rows))
        kernel={row['name']:row for row in coverage['exports']}
        self.assertFalse({'CompareStringW','GetLocaleInfoW','GetStringTypeW','LCMapStringW'}&
                         {n for n,r in kernel.items() if r['status']!='implemented'})

    def test_builtin_image_payload_rules_without_image_build(self):
        import importlib.util
        spec=importlib.util.spec_from_file_location('image_builder',ROOT/'scripts/build_image.py')
        image=importlib.util.module_from_spec(spec);spec.loader.exec_module(image)
        with tempfile.TemporaryDirectory(dir=self.out) as temporary:
            folder=Path(temporary);builtins.build(folder)
            sources,directories,meta=image.win32_payloads(folder)
            self.assertIn('/System/Win32/ntdll.dll',sources)
            self.assertIn('/System/Win32',directories)
            self.assertIn('/System/Win32/BUILTIN.BIN',sources)
            self.assertFalse(any(p.startswith('/WINDOWS/SYSTEM/') for p in sources))
            combined=directories|{'/System','/system/licenses'}
            self.assertEqual(len(combined),len({d.casefold() for d in combined}))
            # f5-20C: 32 + nine absent-DLL W21 stubs (+ mciavi32.dll, f5-12B);
            # f5-20D: plus the qualified reused Wine PEs of the cached upper
            # build, each replacing its census placeholder when one exists.
            upper=json.loads((ROOT/'config/wine-upper.json').read_text())
            staged={n for n,r in upper['dlls'].items() if r['qualified']}
            core=json.loads((folder/'builtin-store.json').read_text())['dlls']
            replaced={d['name'] for d in core if d['name'] in staged}
            self.assertEqual(len(core),43)  # + f5-15A ciukishell.dll
            self.assertEqual(len(meta['builtins']),43-len(replaced)+len(staged))
            self.assertEqual({d['name'] for d in meta['builtins'] if d.get('wine_reused') is True},staged)
            for name in staged:
                self.assertIn('/System/Win32/'+name,sources)
                self.assertIn('/system/licenses/WINE-PATCHES.md',sources)
                self.assertIn('/system/licenses/WINE-musl-COPYRIGHT.txt',sources)
                self.assertIn('/system/licenses/WINE-zlib-NOTICE.txt',sources)
                self.assertEqual(sources['/System/Win32/'+name].parent.name,'staged')
            self.assertIn('/System/Win32/dsetup.dll',sources)
            self.assertIn('/System/Win32/dsetup32.dll',sources)
            self.assertIn('/System/Win32/upper-builtins-inventory.json',sources)
            self.assertIn('/System/Win32/imaadp32.acm',sources)
            self.assertIn('/system/licenses/WINE-SOURCE-OFFER.txt',sources)
            self.assertIn('/System/Win32/codecs-inventory.json',sources)
            manifest=folder/'builtin-store.json';original=manifest.read_text()
            data=json.loads(original);data['dlls'][1]['base']=data['dlls'][0]['base'];manifest.write_text(json.dumps(data))
            with self.assertRaisesRegex(ValueError,'overlap'):image.win32_payloads(folder)
            manifest.write_text(original)
            binary=folder/'BUILTIN.BIN';original_binary=binary.read_bytes()
            binary.write_bytes(original_binary[:-1]+bytes([original_binary[-1]^1]))
            with self.assertRaisesRegex(ValueError,'binary manifest'):image.win32_payloads(folder)
            binary.write_bytes(original_binary)
            raw=bytearray((folder/'ntdll.dll').read_bytes());raw[-1]^=1;(folder/'ntdll.dll').write_bytes(raw)
            with self.assertRaisesRegex(ValueError,'SHA256'):image.win32_payloads(folder)

    def test_upper_directsetup_store_measurements(self):
        import re
        builtins.build(self.images/'WINDOWS/SYSTEM')
        results={}
        for name,mode in (('baseline','upper-store-baseline'),('dsetup.dll','upper-store-dsetup'),
                          ('dsetup32.dll','upper-store-dsetup32')):
            out=self.command(mode,self.images/'console.exe')
            self.assertIn('PASS kernel DirectSetup store H0 '+name,out)
            self.assertIn('bind/callback observation only',out)
            results[name]={'peak_bytes':int(re.search(r'peak_bytes=(\d+)',out)[1]),
                           'peak_cow_commitments':int(re.search(r'cow_credits=(\d+)',out)[1])}
        for name in ('dsetup.dll','dsetup32.dll'):
            self.assertGreater(results[name]['peak_bytes'],results['baseline']['peak_bytes'])
            results[name]['increment_over_baseline_bytes']=results[name]['peak_bytes']-results['baseline']['peak_bytes']
        (self.out/'directsetup-store-results.json').write_text(json.dumps(results,indent=2)+'\n')

if __name__=='__main__':unittest.main()
