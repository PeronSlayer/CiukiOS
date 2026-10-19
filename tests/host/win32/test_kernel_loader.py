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
        self.assertIn("PASS kernel32 VM heap objects waits TLS codepages time environment",self.command("kernel32",self.images/"console.exe"))

    def test_kernel32_create_process_transaction(self):
        builtins.build(self.images/'WINDOWS/SYSTEM')
        self.assertIn("PASS kernel32 kernel CreateProcess",self.command("spawn",self.images/'console.exe'))

    def test_runtime_services_on_kernel_model(self):
        self.assertIn('PASS kernel heap critical waits SEH filter',self.command('runtime',self.images/'console.exe'))
        out=self.command('startup',self.images/'compiled-tls.exe')
        self.assertIn('PASS kernel startup parameters TLS DATAFILE',out)
        self.assertIn('PASS kernel forced relocation COW',out)
        out=self.command('attach-fail',self.images/'consumer.exe')
        self.assertIn('PASS kernel attach failure start status rollback',out)
        self.assertIn('[win32] load check=dllmain-attach-failed',out)

    def test_production_builtin_store_through_kernel_loader(self):
        builtins.build(self.images/'WINDOWS/SYSTEM')
        from scripts.win32 import census
        coverage=json.loads((self.images/'WINDOWS/SYSTEM/kernel32-coverage.json').read_text())
        exports={name for e in census.inspect((self.images/'WINDOWS/SYSTEM/kernel32.dll').read_bytes())['exports'] for name in e['names']}
        expected=set()
        from scripts.win32 import stubs
        for source in ('docs/design/research/win32-layer-2026-10-14.md','docs/design/win32-census/fixtures.txt'):
            expected.update(stubs.imports((ROOT/source).read_text()).get('kernel32.dll',set()))
        self.assertEqual({e['name'] for e in coverage['exports']},expected)
        self.assertTrue(expected<=exports)
        self.assertEqual(set(coverage['deferred_vfs']),{'LockFile','LockFileEx','UnlockFile','UnlockFileEx'})
        self.assertTrue(set(coverage['deferred_vfs'])<=exports)
        self.assertIn('provisional',coverage['scope'])
        self.assertIn('PASS kernel builtin store imports helpers pinning',self.command('store',self.images/'builtin-import.exe'))

    def test_builtin_image_payload_rules_without_image_build(self):
        import importlib.util
        spec=importlib.util.spec_from_file_location('image_builder',ROOT/'scripts/build_image.py')
        image=importlib.util.module_from_spec(spec);spec.loader.exec_module(image)
        with tempfile.TemporaryDirectory(dir=self.out) as temporary:
            folder=Path(temporary);builtins.build(folder)
            sources,directories,meta=image.win32_payloads(folder)
            self.assertIn('/WINDOWS/SYSTEM/ntdll.dll',sources)
            self.assertIn('/WINDOWS/SYSTEM',directories)
            combined=directories|{'/System','/system/licenses'}
            self.assertEqual(len(combined),len({d.casefold() for d in combined}))
            self.assertEqual(len(meta['builtins']),28)
            self.assertIn('/WINDOWS/SYSTEM/imaadp32.acm',sources)
            self.assertIn('/system/licenses/WINE-SOURCE-OFFER.txt',sources)
            self.assertIn('/WINDOWS/SYSTEM/codecs-inventory.json',sources)
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

if __name__=='__main__':unittest.main()
