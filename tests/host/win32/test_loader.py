"""Real production PE walkers, H0 models, and separately labelled H1 execution."""
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright 2026 CiukiOS contributors.
import json
from pathlib import Path
import signal
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT))
from scripts.win32 import fixtures
from tests.host.win32.adapter import build,wine_compare

class LoaderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        parent=ROOT/'build/host/win32';parent.mkdir(parents=True,exist_ok=True)
        cls.temp=tempfile.TemporaryDirectory(prefix='loader-',dir=parent)
        cls.addClassCleanup(cls.temp.cleanup)
        cls.out=Path(cls.temp.name)
        cls.images=cls.out/'images'
        cls.hashes=fixtures.build(cls.images)
        cls.h1=build.build(cls.out/'core')
        cls.h0=build.build_model(cls.out/'model')
        cls.units=build.build_model(cls.out/'units',ROOT/'tests/host/win32/adapter/runtime_tests.c')
        cls.i386_units=build.build(cls.out/'i386-units',ROOT/'tests/host/win32/adapter/runtime_tests.c',windows=False)
        cls.graphs=cls.out/'graphs';cls.graphs.mkdir()
        specs={s['id']:s for s in fixtures.manifests()}
        # The original graph corpus has no relocations and collides at
        # 0x400000. Derive distinct-base layouts; do not overwrite its evidence.
        import shutil
        shutil.copyfile(cls.images/'provider.dll',cls.graphs/'provider.dll')
        names=['cycle-a','cycle-b','forward-cycle','forward-depth-16','forward-depth-17']+[f'fwd{i:02d}' for i in range(1,17)]
        for i,name in enumerate(names):
            raw=bytearray(fixtures.synthetic(specs[name]));struct.pack_into('<I',raw,fixtures.O+28,0x02000000+i*65536)
            (cls.graphs/(name+'.dll')).write_bytes(raw)
        shutil.copyfile(cls.graphs/'forward-cycle.dll',cls.graphs/'fixture.dll')
        for target in ('i686-pc-windows-msvc','i686-w64-mingw32'):
            build.build_shim(cls.out/'core'/target)
        cls.probe=wine_compare.build_fixture(cls.out/'probe','seh_probe')
        probe=subprocess.run([cls.h1,'validate',cls.images/'console.exe'],capture_output=True,text=True,timeout=10)
        cls.i386_available=probe.returncode==0
        cls.i386_reason=f'i386 adapter cannot run here: exit {probe.returncode} (SIGSYS = {-signal.SIGSYS}); H0 is not H1 evidence'
        if not cls.i386_available and probe.returncode not in (-signal.SIGSYS,124):
            raise RuntimeError('i386 adapter failure: '+probe.stdout+probe.stderr)

    def command(self,exe,*args,expected=0):
        p=subprocess.run(list(map(str,[exe,*args])),capture_output=True,text=True,timeout=30)
        self.assertEqual(p.returncode,expected,p.stdout+p.stderr)
        self.assertNotIn('LEAK',p.stdout)
        return p.stdout

    def image(self,spec):
        return next(self.images/name for name in self.hashes if Path(name).stem==spec['id'])

    def test_entire_137_fixture_corpus_against_independent_refusal_manifest(self):
        for spec in fixtures.manifests():
            with self.subTest(fixture=spec['id']):
                error=spec['census'].get('error')
                out=self.command(self.h0,'validate',self.image(spec),expected=1 if error else 0)
                self.assertIn('REFUSE '+error+' 193' if error else 'PASS validate',out)

    def test_every_fixture_mapping_binding_or_exact_runtime_refusal(self):
        overrides={'bound-without-names':('bound-iat-without-names',193),
                   'missing-dll':('missing-dll',126),'missing-export':('missing-export',127),
                   'cycle-a':('relocation-base-collision',193),'cycle-b':('relocation-base-collision',193),
                   'builtin-import':('missing-dll',126)}
        for spec in fixtures.manifests():
            with self.subTest(fixture=spec['id']):
                expected=(spec['census']['error'],193) if 'error' in spec['census'] else overrides.get(spec['id'])
                out=self.command(self.h0,'load',self.image(spec),self.images,'observe',expected=1 if expected else 0)
                self.assertIn(f'REFUSE {expected[0]} {expected[1]}' if expected else 'PASS load',out)

    def test_f5_15c_subsystem_version_record_names_application_image(self):
        """f5-15C: MajorSubsystemVersion is the 16-bit field at optional-header
        offset 48 (Microsoft PE format). Real lld-link headers declare 4.0 and
        load without the record; the corpus image patched to 6.0 is accepted
        with one record naming that image (contract "Accepted images")."""
        for name,major in (('console.exe',4),('gui.exe',4),('new-subsystem-version.exe',6)):
            with self.subTest(image=name):
                raw=(self.images/name).read_bytes()
                optional=struct.unpack_from('<I',raw,60)[0]+24
                self.assertEqual(struct.unpack_from('<H',raw,optional)[0],0x10b)
                self.assertEqual(struct.unpack_from('<HH',raw,optional+48),(major,0))
                out=self.command(self.h0,'load',self.images/name,self.images,'observe')
                self.assertIn('PASS load',out)
                records=[l for l in out.splitlines() if 'newer-subsystem-version' in l]
                self.assertEqual(records,['[win32] load '+name+' newer-subsystem-version'] if major>5 else [])
                self.assertNotIn('[win32] load image newer-subsystem-version',out)

    def test_runtime_models(self):
        for mode in ('heap','critical','strings','seh','loader','relocations','resources','stubs','heap-exceptions'):
            with self.subTest(mode=mode): self.assertIn('PASS ',self.command(self.units,mode,self.images))
        self.assertIn('PASS cyclic-imports-forwarders',self.command(self.units,'graphs',self.graphs))

    def test_search_order_loaded_basename_and_builtin_over_explicit_path(self):
        specs={s['id']:s for s in fixtures.manifests()}
        raw=bytearray(fixtures.synthetic(specs['exports']))
        known=bytearray(raw);struct.pack_into('<I',known,fixtures.O+28,0x70010000)
        for chosen in range(5):
            with self.subTest(chosen=chosen):
                store=self.out/f'search-{chosen}';store.mkdir()
                for i,name in enumerate(('exe','current','system','windows','path')):
                    directory=store/name;directory.mkdir()
                    if i>=chosen: (directory/'ordinary.dll').write_bytes(raw)
                (store/'known.dll').write_bytes(known)
                expected=('exe','current','system','windows','path')[chosen]+'\\ordinary.dll'
                self.assertIn('PASS search-order-builtin',self.command(self.units,'search',store,expected))

    def test_altered_search_uses_top_dll_folder_and_restores_normal_order(self):
        import shutil
        store=self.out/'altered-search'
        for name in ('exe','other'):
            folder=store/name;folder.mkdir(parents=True)
            shutil.copyfile(self.images/'provider.dll',folder/'provider.dll')
        shutil.copyfile(self.images/'consumer.exe',store/'other/consumer.exe')
        self.assertIn('PASS alternate import folder',self.command(self.units,'altered-search',store))

    def test_real_i386_runtime_models(self):
        if not self.i386_available: self.skipTest(self.i386_reason)
        for mode in ('heap','critical','strings','seh','loader','relocations','resources','ciuki-stub'):
            with self.subTest(mode=mode): self.assertIn('PASS ',self.command(self.i386_units,mode,self.images))
        self.assertIn('PASS cyclic-imports-forwarders',self.command(self.i386_units,'graphs',self.graphs))

    def test_real_compiler_fixture_execution_including_fs_seh_tls_and_packer(self):
        if not self.i386_available: self.skipTest(self.i386_reason)
        for name,value in [('seh-chain',0),('seh-nested',0),('compiled-tls',0),('consumer',18),('packed-code',42)]:
            with self.subTest(fixture=name):
                out=self.command(self.h1,'exec',self.images/(name+'.exe'),self.images)
                self.assertIn('RESULT '+str(value),out)
        out=self.command(self.h1,'load',self.images/'provider-fail.dll',self.images,expected=1)
        self.assertIn('REFUSE dllmain-attach-failed 1114',out)

    RUNTIME_REFUSALS={'bound-without-names':('bound-iat-without-names',193),
                      'missing-dll':('missing-dll',126),'missing-export':('missing-export',127),
                      'cycle-a':('relocation-base-collision',193),'cycle-b':('relocation-base-collision',193),
                      'builtin-import':('missing-dll',126)}

    def test_h1_i386_corpus_validate_and_load(self):
        """The whole corpus through the real i386 build, not only the H0 model."""
        if not self.i386_available: self.skipTest(self.i386_reason)
        for spec in fixtures.manifests():
            with self.subTest(fixture=spec['id']):
                error=spec['census'].get('error')
                out=self.command(self.h1,'validate',self.image(spec),expected=1 if error else 0)
                self.assertIn('REFUSE '+error+' 193' if error else 'PASS validate',out)
                expected=(error,193) if error else self.RUNTIME_REFUSALS.get(spec['id'])
                out=self.command(self.h1,'load',self.image(spec),self.images,'observe',expected=1 if expected else 0)
                self.assertIn(f'REFUSE {expected[0]} {expected[1]}' if expected else 'PASS load',out)

    # Executed on i386 through w32_ldr_start_process. Synthetic entries return
    # their HIGHLOW site (0x402000 at the preferred base).
    EXEC_EXCLUDED={'tls':'synthetic TLS callbacks are not stdcall routines (observer only)',
                   'tls-two':'synthetic TLS callbacks are not stdcall routines (observer only)',
                   'entrypoint-header':'host NX: header pages are not executable on Linux',
                   'builtin-import':'needs kernel32; executed by the exec-core shim test'}

    def test_h1_i386_executes_every_loadable_exe(self):
        if not self.i386_available: self.skipTest(self.i386_reason)
        compiled={'consumer':18,'packed-code':42}
        statuses={126:0xc0000135,127:0xc0000139,193:0xc000007b}
        count=0
        for spec in fixtures.manifests():
            image=self.image(spec)
            if image.suffix!='.exe' or spec['id'] in self.EXEC_EXCLUDED: continue
            with self.subTest(fixture=spec['id']):
                error=spec['census'].get('error')
                refusal=(error,193) if error else self.RUNTIME_REFUSALS.get(spec['id'])
                out=self.command(self.h1,'exec',image,self.images,expected=1 if refusal else 0)
                if refusal:
                    self.assertIn(f'REFUSE {refusal[0]} {refusal[1]} STATUS {statuses[refusal[1]]}',out)
                else:
                    value=compiled.get(spec['id'],0 if spec['kind']=='compiled' else 0x402000)
                    self.assertIn(f'RESULT {value}\n',out)
                count+=1
        self.assertGreater(count,100)

    def test_h1_relocated_exe_execution(self):
        """Preferred base occupied (by the adapter ELF): HIGHLOW applied, entry
        returns the relocated pointer."""
        if not self.i386_available: self.skipTest(self.i386_reason)
        raw=bytearray((self.images/'reloc-highlow.exe').read_bytes())
        struct.pack_into('<I',raw,fixtures.O+28,0x60000000)
        path=self.out/'images'/'relocated-exe.exe';path.write_bytes(raw)
        out=self.command(self.h1,'exec',path,self.images)
        # The code holds 0x402000 (linked for 0x400000); HIGHLOW adds the delta
        # from the declared base 0x60000000 to the first-fit base 0x01000000.
        self.assertIn(f'RESULT {(0x402000+0x01000000-0x60000000)&0xffffffff}\n',out)
        struct.pack_into('<H',raw,0x96,struct.unpack_from('<H',raw,0x96)[0]|1)
        path.write_bytes(raw)
        self.assertIn('REFUSE relocation-base-collision 193',self.command(self.h1,'exec',path,self.images,expected=1))
        path.unlink()

    def test_h1_pe_ntdll_executes_fixtures_and_probe_on_both_targets(self):
        """exec-core: the PE ntdll.dll (MSVC and MinGW targets) is the builtin
        the EXE binds to; faults are delivered to its KiUserExceptionDispatcher."""
        if not self.i386_available: self.skipTest(self.i386_reason)
        for target in ('i686-pc-windows-msvc','i686-w64-mingw32'):
            core=self.out/'core'/target
            for name,value in [('seh-chain',0),('seh-nested',0),('compiled-tls',0),('consumer',18),
                               ('packed-code',42),('builtin-import',0)]:
                with self.subTest(target=target,fixture=name):
                    out=self.command(self.h1,'exec-core',self.images/(name+'.exe'),core)
                    self.assertIn(f'RESULT {value}\n',out)
            with self.subTest(target=target,fixture='seh_probe'):
                self.assertIn('RESULT 0\n',self.command(self.h1,'exec-core',self.probe,core))

    def test_both_pe_target_exports_and_i386_machine_code(self):
        for target in ('i686-pc-windows-msvc','i686-w64-mingw32'):
            dll=self.out/'core'/target/'ntdll.dll'
            out=self.command('llvm-readobj','--file-headers','--coff-imports','--coff-exports',dll)
            self.assertIn('IMAGE_FILE_MACHINE_I386',out)
            for name in ('RtlUnwind','LdrLoadDll','NtCurrentTeb','KiUserExceptionDispatcher',
                         'LdrFindResource_U','LdrAccessResource','RtlImageNtHeader','RtlAllocateHeap'):
                self.assertIn('Name: '+name+'\n',out)
            self.assertNotIn('Import {',out,'ntdll must have no CRT or platform imports')
            assembly=self.command('llvm-objdump','-d',self.out/'core'/target/'context.o')
            self.assertIn('%fs',assembly);self.assertIn('fnsave',assembly)
            self.assertRegex(assembly,r'retl?\s+\$0x4')

    def test_directory_holes_and_relocation_modified_tables_fail_closed(self):
        raw=bytearray((self.images/'console.exe').read_bytes())
        struct.pack_into('<II',raw,fixtures.O+96+1*8,0x1800,40)
        path=self.out/'hole.exe';path.write_bytes(raw)
        self.assertIn('REFUSE rva-unmapped 193',self.command(self.h0,'validate',path,expected=1))
        raw=bytearray((self.images/'import-name.exe').read_bytes())
        # A relocation aimed at OriginalFirstThunk must not turn a validated
        # descriptor into an unchecked arbitrary pointer after relocation.
        struct.pack_into('<II',raw,fixtures.O+96+5*8,0x2700,12)
        struct.pack_into('<IIHH',raw,0xb00,0x2000,12,0x3000,0)
        struct.pack_into('<I',raw,fixtures.O+28,0x01000000)
        path=self.out/'reloc-table.exe';path.write_bytes(raw)
        self.assertIn('PASS validate',self.command(self.h0,'validate',path))
        self.assertIn('PASS relocated-invalid-import-refused',self.command(self.units,'invalid-reloc',path))

if __name__=='__main__': unittest.main()
