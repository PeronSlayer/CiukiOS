"""Exact production hives, RPC, registry facade, version and compression."""
# SPDX-License-Identifier: LGPL-2.1-or-later
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import unittest
import zlib
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT))
from scripts.win32 import facades,stubs,census

class RegistryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.out=ROOT/'build/host/win32/registry'
        cls.out.mkdir(parents=True,exist_ok=True)
        cls.env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0',TMPDIR=str(cls.out))
        cls.program=cls.out/'registry-tests'
        sources=[ROOT/'apps/w32srv'/n for n in ('registry.c','storage.c','defaults.c','directory.c','protocol.c','posix_io.c')]
        sources += [ROOT/'sdk/win32/advapi32'/n for n in ('registry.c','text.c')]
        sources += [ROOT/'sdk/win32/services/bytes.c',ROOT/'sdk/win32/version/version.c']
        sources += [ROOT/'sdk/win32/lz32'/n for n in ('lz32.c','decode.c')]
        sources += [ROOT/'sdk/win32/lz32/mspack'/n for n in ('kwajd.c','szddd.c','lzssd.c','mszipd.c')]
        sources += [ROOT/'tests/host/win32/registry_test.c',ROOT/'tests/host/win32/registry_facades_test.c']
        subprocess.run(['clang','-std=c17','-DW32_HOST_MODEL','-DMSPACK_NO_DEFAULT_SYSTEM','-O1','-g','-Wall','-Wextra','-Werror',
                        '-Wno-unused-parameter','-Wno-sign-compare','-fsanitize=address,undefined','-fno-sanitize-recover=all',
                        '-I'+str(ROOT/'sdk/win32/include'),'-I'+str(ROOT/'sdk/win32/lz32/mspack'),
                        *map(str,sources),'-o',str(cls.program)],check=True,env=cls.env)
        cls.store=facades.build(cls.out/'builtin')
        cls.image=cls.out/'builtin/version.dll'
        szdd=b'SZDD\x88\xf0\x27\x33At'+struct.pack('<I',5)+b'\x1fhello'
        (cls.out/'szdd.bin').write_bytes(szdd)
        (cls.out/'file.tx_').write_bytes(szdd)
        for method in (0,1,2):
            payload=b'hello' if method==0 else bytes(c^255 for c in b'hello') if method==1 else b'\x1fhello'
            (cls.out/f'kwaj{method}.bin').write_bytes(b'KWAJ\x88\xf0\x27\xd1'+struct.pack('<HHHI',method,18,1,5)+payload)
        # Independent MSZIP encoder, raw DEFLATE wrapped in KWAJ blocks.
        z=zlib.compressobj(wbits=-15);payload=b'CK'+z.compress(b'hello')+z.flush()
        (cls.out/'kwaj4.bin').write_bytes(b'KWAJ\x88\xf0\x27\xd1'+struct.pack('<HHHI',4,18,1,5)+struct.pack('<H',len(payload))+payload+b'\0\0')
        # LZH: five canonical fixed-length Huffman tables (type 0), literal
        # run length 5, then five 8-bit literal codes. MSB-first bitstream.
        bits='0000'*6+'0000'+'00100'+''.join(f'{c:08b}' for c in b'hello')
        bits+='0'*(-len(bits)%8)
        payload=int(bits,2).to_bytes(len(bits)//8,'big')
        (cls.out/'kwaj3.bin').write_bytes(b'KWAJ\x88\xf0\x27\xd1'+struct.pack('<HHHI',3,18,1,5)+payload)

    def execute(self,mode,*args):
        p=subprocess.run([str(self.program),mode,*map(str,args)],capture_output=True,text=True,env=self.env,timeout=180)
        self.assertEqual(p.returncode,0,p.stdout+p.stderr)
        self.assertIn('PASS',p.stdout)
        print(p.stdout,end='')

    def test_registry_public_api_errors_and_aliases(self):self.execute('basics')
    def test_compatibility_reader_and_authorized_writer(self):self.execute('compat')
    def test_all_types_names_and_hive_limits(self):self.execute('limits')
    def test_every_torn_write_byte_and_fsync_stage(self):self.execute('faults')
    def test_restart_timer_writer_exit_and_identity_lifetime(self):self.execute('lifecycle')
    def test_two_process_writers(self):self.execute('concurrent')
    def test_malformed_records_and_fragments(self):self.execute('malformed')
    def test_version_resources(self):self.execute('version',self.image)
    def test_szdd_and_all_kwaj_methods(self):self.execute('lz',self.out)

    def test_census_exports_and_builtin_store(self):
        self.assertEqual(stubs.verify_store(self.out/'builtin/builtin-store.json'),3)
        coverage=json.loads((self.out/'builtin/coverage.json').read_text())
        self.assertEqual({k:len(v) for k,v in coverage.items()},{'advapi32.dll':38,'version.dll':6,'lz32.dll':3})
        for dll,rows in coverage.items():
            exports={n for e in census.inspect((self.out/'builtin'/dll).read_bytes())['exports'] for n in e['names']}
            self.assertTrue(set(rows)<=exports)
            self.assertTrue(set(rows.values()) <= {'implemented','W21 stub'})
        self.assertEqual(coverage['version.dll']['VerInstallFileA'],'W21 stub')
        self.assertEqual(coverage['version.dll']['VerFindFileA'],'implemented')
        self.assertTrue(all(v=='implemented' for v in coverage['lz32.dll'].values()))

    def test_upstream_source_pin(self):
        directory=ROOT/'sdk/win32/lz32/mspack'
        pin=json.loads((directory/'UPSTREAM.json').read_text())
        for name,digest in pin['files'].items():self.assertEqual(hashlib.sha256((directory/name).read_bytes()).hexdigest(),digest,name)

if __name__=='__main__':unittest.main()
