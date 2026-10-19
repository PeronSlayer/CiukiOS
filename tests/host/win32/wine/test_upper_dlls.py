"""Real PE closure, T1 mutations, reproducibility and authored DirectSetup behavior."""
# SPDX-License-Identifier: LGPL-2.1-or-later
import ctypes
import json
from pathlib import Path
import signal
import subprocess
import tempfile
import unittest

from scripts.win32 import census, stubs, upper_dlls as upper
from tests.host.win32.adapter import build as adapter

ROOT = Path(__file__).resolve().parents[4]


class UpperDLLs(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        parent = ROOT / 'build/host/win32/upper'
        parent.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(dir=parent)
        cls.addClassCleanup(cls.temp.cleanup)
        cls.out = Path(cls.temp.name)
        cls.a, cls.b = cls.out / 'a', cls.out / 'b'
        cls.first = upper.build(cls.a)
        cls.second = upper.build(cls.b)
        cls.manifest = cls.a / 'builtin-store.json'
        upper.dump(cls.manifest, {'schema': 1, 'dlls': cls.first})
        cls.native = cls.out / 'dsetup.so'
        upper.run(['clang', '-shared', '-fPIC', '-DW32_HOST_MODEL', '-O1',
                   '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'sdk/win32/include'),
                   ROOT / 'sdk/win32/upper/dsetup.c', '-o', cls.native])
        cls.library = ctypes.CDLL(str(cls.native))
        cls.graph = cls.out / 'graph'
        cls.graph.mkdir()
        source = cls.graph / 'unit.c'
        source.write_text('int __stdcall Value(void) { return 17; }\n')
        upper.run(['clang', *upper.FLAGS, '-c', source, '-o', cls.graph / 'unit.obj'])

    def dll(self, name, exports, objects=None, extra=()):
        definition = self.graph / (name + '.def')
        definition.write_text('LIBRARY ' + name + '\nEXPORTS\n' + exports)
        upper.run(['lld-link', '/machine:x86', '/dll', '/noentry', '/nodefaultlib',
                   '/safeseh:no', '/timestamp:0', '/incremental:no',
                   '/subsystem:windows,4.0', '/osversion:4.0',
                   '/out:' + str(self.graph / name), '/def:' + str(definition),
                   *(objects or [self.graph / 'unit.obj']), *extra])

    def test_two_run_pe_identity_and_real_version_resource(self):
        self.assertEqual(self.first, self.second)
        self.assertEqual((self.a / upper.INVENTORY).read_bytes(), (self.b / upper.INVENTORY).read_bytes())
        for record in self.first:
            name = record['name']
            raw = (self.a / name).read_bytes()
            self.assertEqual(raw, (self.b / name).read_bytes())
            pe = census.PE(raw)
            self.assertNotEqual(pe.entry, 0)
            self.assertEqual(pe.u32(pe.opt - 16), 0)  # COFF timestamp
            self.assertEqual(stubs.image_version(raw), [4, 7, 0, 716])
            names = {n for e in pe.exports() for n in e['names']}
            self.assertEqual(names, {'DirectXSetup', 'DirectXSetupA', 'DirectXSetupW', 'DirectXSetupGetVersion'})
            self.assertEqual(pe.imports() + pe.imports(True), [])
            self.assertTrue(upper.audit_passed(upper.audit_images(self.a, upper.BUILTINS)))

    def test_native_identity_query_optional_outputs_and_no_copy_success(self):
        query = self.library.DirectXSetupGetVersion
        query.argtypes = [ctypes.POINTER(ctypes.c_uint32)] * 2
        query.restype = ctypes.c_int32
        v, r = ctypes.c_uint32(), ctypes.c_uint32()
        self.assertEqual(query(ctypes.byref(v), ctypes.byref(r)), 1)
        self.assertEqual((v.value, r.value), (0x40007, 716))
        self.assertEqual(query(None, ctypes.byref(r)), 1)
        self.assertEqual(query(ctypes.byref(v), None), 1)
        self.assertEqual(query(None, None), 1)
        # An unreadable root would fault if the no-copy identity touched it.
        for name in ('DirectXSetupA', 'DirectXSetupW'):
            function = getattr(self.library, name)
            function.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32]
            function.restype = ctypes.c_int32
            for flags in (0, 0x10018, 0x30018, 0x30033a7f):
                self.assertEqual(function(None, 1, flags), 0)
            for flags in (0x4000, 0x14018, 0xffffffff):
                self.assertEqual(function(None, 1, flags), -1)
        entry = self.library.CiukiSetupDllMain
        entry.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p]
        for reason in (0, 1, 2, 3):
            self.assertEqual(entry(None, reason, None), 1)

    def test_t1_inventory_license_source_hash_and_unlisted_pe_refusals(self):
        self.assertEqual(stubs.verify_store(self.manifest), 2)
        original = self.manifest.read_bytes()
        inventory = self.a / upper.INVENTORY
        original_inventory = inventory.read_bytes()
        for field, value in (('license', 'Public-Domain'), ('wine_reused', True),
                             ('source_sha256', '0' * 64), ('memory', {})):
            changed = json.loads(original)
            changed['dlls'][0][field] = value
            upper.dump(self.manifest, changed)
            changed_inventory = json.loads(original_inventory)
            changed_inventory['dlls'] = changed['dlls']
            upper.dump(inventory, changed_inventory)
            with self.assertRaises(ValueError):
                stubs.verify_store(self.manifest)
        self.manifest.write_bytes(original)
        inventory.write_bytes(original_inventory)
        extra = self.a / 'unlisted.dll'
        extra.write_bytes((self.a / 'dsetup.dll').read_bytes())
        try:
            with self.assertRaisesRegex(ValueError, 'unlisted builtin PE'):
                stubs.verify_store(self.manifest)
        finally:
            extra.unlink()
        raw = (self.a / 'dsetup.dll').read_bytes()
        (self.a / 'dsetup.dll').write_bytes(raw + b'changed')
        try:
            with self.assertRaisesRegex(ValueError, 'SHA256'):
                stubs.verify_store(self.manifest)
        finally:
            (self.a / 'dsetup.dll').write_bytes(raw)
        self.assertEqual(stubs.verify_store(self.manifest), 2)

    def test_actual_pe_named_ordinal_and_recursive_forwarder_audit(self):
        self.dll('provider.dll', ' Value=_Value@0 @42 NONAME\n')
        self.dll('forward.dll', ' Forward=provider.#42\n')
        self.dll('unresolved.dll', ' Missing=provider.#41\n')
        source = self.graph / 'consumer.c'
        source.write_text('__declspec(dllimport) int Forward(void);\n'
                          'int __stdcall Call(void) { return Forward(); }\n')
        upper.run(['clang', *upper.FLAGS, '-c', source, '-o', self.graph / 'consumer.obj'])
        self.dll('consumer.dll', ' Call=_Call@0\n', [self.graph / 'consumer.obj'],
                 [self.graph / 'forward.lib'])
        audit = upper.audit_images(self.graph)
        self.assertTrue(upper.audit_passed({n: audit[n] for n in ('consumer.dll', 'forward.dll', 'provider.dll')}))
        row = audit['consumer.dll']['imports'][0]
        self.assertEqual(row['present'], ['Forward'])
        self.assertEqual(row['resolutions']['Forward']['chain'], ['forward.dll!Forward', 'provider.dll!#42'])
        self.assertFalse(upper.audit_passed(audit))
        self.assertEqual(audit['unresolved.dll']['forwarders'][0]['error'], 'missing-export')
        self.assertEqual(upper.audit_images(self.graph, ['absent.dll'])['absent.dll']['error'], 'not-built')

    def test_actual_pe_delayed_import_is_audited(self):
        self.dll('delayed-provider.dll', ' Forward=_Value@0\n')
        source = self.graph / 'delayed.c'
        source.write_text('__declspec(dllimport) int Forward(void);\n'
                          'int __stdcall Call(void) { return Forward(); }\n'
                          'void *__stdcall __delayLoadHelper2(void *a, void *b) { '
                          '(void)a; (void)b; return 0; }\n')
        obj = self.graph / 'delayed.obj'
        upper.run(['clang', *upper.FLAGS, '-c', source, '-o', obj])
        definition = self.graph / 'delayed-imports.def'
        definition.write_text('LIBRARY delayed-provider.dll\nEXPORTS\n Forward\n')
        library = self.graph / 'delayed-imports.lib'
        upper.run(['llvm-dlltool', '-m', 'i386', '-k', '-d', definition, '-l', library])
        self.dll('delayed-consumer.dll', ' Call=_Call@0\n', [obj],
                 [library, '/delayload:delayed-provider.dll'])
        row = upper.audit_images(self.graph, ['delayed-consumer.dll'])['delayed-consumer.dll']
        self.assertTrue(row['imports'][0]['delayed'])
        self.assertEqual(row['imports'][0]['present'], ['Forward'], row)
        (self.graph / 'delayed-provider.dll').unlink()
        row = upper.audit_images(self.graph, ['delayed-consumer.dll'])['delayed-consumer.dll']
        self.assertEqual(row['imports'][0]['missing'], ['Forward'])
        self.assertEqual(row['imports'][0]['resolutions']['Forward']['error'], 'missing-dll')

    def test_forwarder_cycles_depth_and_case_sensitive_names(self):
        self.assertFalse(upper.audit_passed({}))
        def entry(forward=None):
            return {'forwarder': forward}
        index = {'a.dll': {'F': entry('b.F')}, 'b.dll': {'F': entry('a.F')}}
        self.assertEqual(upper.resolve(index, 'A.DLL', 'F')['error'], 'forwarder-cycle')
        self.assertEqual(upper.resolve(index, 'a.dll', 'f')['error'], 'missing-export')
        index = {f'f{i}.dll': {'F': entry(f'f{i+1}.F')} for i in range(17)}
        index['f17.dll'] = {'F': entry()}
        self.assertEqual(upper.resolve(index, 'f0.dll', 'F')['error'], 'forwarder-depth')
        self.assertIsNone(upper.resolve(index, 'f1.dll', 'F')['error'])
        index['bad.dll'] = {'F': entry('other.#65536')}
        self.assertEqual(upper.resolve(index, 'bad.dll', 'F')['error'], 'export-forwarder-format')
        index['ordinal.dll'] = {'F': entry('other.#00042')}
        index['other.dll'] = {'#42': entry()}
        self.assertIsNone(upper.resolve(index, 'ordinal.dll', 'F')['error'])

    def test_memory_model_unions_shared_raw_pages_and_cow_promises(self):
        class Image:
            headers, size = 512, 16384
            sections = [{'rva': 4096, 'raw_size': 5000, 'span': 7000, 'flags': 0x80000000},
                        {'rva': 11096, 'raw_size': 100, 'span': 1000, 'flags': 0x80000000}]
        self.assertEqual(upper.memory_model(Image()), {
            'shared_seed_pages': 3, 'private_cow_commitment_pages': 2,
            'startup_committed_pages_model': 5, 'reserved_image_pages': 4})

    def adapter_fixture(self):
        directory = self.out / 'adapter'
        directory.mkdir(exist_ok=True)
        header = directory / 'directsetup_store.h'
        header.write_text('static struct w32_builtin directsetup_store[] = {\n' + ''.join(
            '{' + ','.join([json.dumps(r['name']), json.dumps(str(self.a / r['name'])),
                           json.dumps(r['sha256']), hex(r['base']), str(r['size']), '{4,7,0,716}']) + '},\n'
            for r in self.first) + '};\n')
        main = directory / 'main.c'
        main.write_text((Path(__file__).with_name('directsetup_adapter.c')).read_text())
        return directory, main

    def test_production_loader_h0_binding_and_callback_observation(self):
        directory, main = self.adapter_fixture()
        program = adapter.build_model(directory / 'model', main)
        result = subprocess.run([program, 'observe'], capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('PASS DirectSetup H0 binding attach/detach observation; no PE execution', result.stdout)

    def test_production_loader_h1_real_pe_calls_and_attach_detach(self):
        directory, main = self.adapter_fixture()
        program = adapter.build(directory / 'i386', main, windows=False)
        result = subprocess.run([program, 'execute'], capture_output=True, text=True, timeout=30)
        if result.returncode == -signal.SIGSYS:
            self.skipTest('i386 adapter execution blocked by sandbox seccomp (SIGSYS); H0 is separate')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('PASS DirectSetup H1 PE calls attach/detach', result.stdout)


if __name__ == '__main__':
    unittest.main()
