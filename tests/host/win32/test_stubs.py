"""Census coverage, exact explicit ordinals, reproducible sources/PEs and T1 store."""
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright 2026 CiukiOS contributors.
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT))
from scripts.win32 import stubs,census
from tests.host.win32.adapter import build

class StubTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        parent=ROOT/'build/host/win32';parent.mkdir(parents=True,exist_ok=True)
        cls.temp=tempfile.TemporaryDirectory(prefix='stubs-',dir=parent);cls.addClassCleanup(cls.temp.cleanup)
        cls.out=Path(cls.temp.name);cls.inputs=[ROOT/'docs/design/research/win32-layer-2026-10-14.md']
        cls.a=cls.out/'a';cls.b=cls.out/'b'
        cls.first=stubs.generate(cls.inputs,cls.a);cls.second=stubs.generate(cls.inputs,cls.b)
        build.build(cls.out/'core')
        library=cls.out/'core/i686-pc-windows-msvc/ntdll.lib'
        cls.store=stubs.build(cls.a,library);stubs.build(cls.b,library)

    def test_exact_research_union_708_without_prose_placeholders(self):
        self.assertEqual(self.first,self.second)
        self.assertEqual(len(self.first['dlls']),20)
        self.assertEqual(sum(len(d['exports']) for d in self.first['dlls']),708)
        self.assertNotIn('dll.dll',[d['name'] for d in self.first['dlls']])
        for path in self.a.iterdir():
            if path.suffix in ('.c','.def','.rc','.dll','.json'):
                self.assertEqual(path.read_bytes(),(self.b/path.name).read_bytes(),path.name)

    def test_every_named_export_and_ordinal_resolves_in_actual_pe_tables(self):
        for dll in self.first['dlls']:
            pe=census.inspect((self.a/dll['name']).read_bytes())
            names={name:e for e in pe['exports'] for name in e['names']}
            ords={e['ordinal']:e for e in pe['exports']}
            for e in dll['exports']:
                if e['name']: self.assertIn(e['name'],names)
                if e['ordinal']:
                    actual=ords[e['ordinal']]
                    self.assertEqual(actual['names'],[e['name']] if e['name'] else [])
            self.assertEqual([i['dll'].lower() for i in pe['imports']],['ntdll.dll'])
            self.assertTrue(any(r['type']==16 and r['name']==1 for r in pe['resources']))
        for dll,symbol,ordinal in [('wsock32.dll','WSAStartup',115),('wsock32.dll','socket',23),
                                  ('comctl32.dll','InitCommonControls',17),('dplayx.dll','DirectPlayCreate',1),
                                  ('dplayx.dll','DirectPlayEnumerateA',2),('dplayx.dll','DirectPlayLobbyCreateA',4)]:
            exports=census.inspect((self.a/dll).read_bytes())['exports']
            self.assertEqual(next(e['ordinal'] for e in exports if symbol in e['names']),ordinal)
        exports=census.inspect((self.a/'winmm.dll').read_bytes())['exports']
        self.assertEqual(next(e for e in exports if e['ordinal']==2)['names'],[])

    def test_store_digests_ranges_and_corruption_refusals(self):
        path=self.a/'builtin-store.json';self.assertEqual(stubs.verify_store(path),20)
        original=path.read_bytes();store=json.loads(original)
        for field,value in [('sha256','0'*64),('base',0x400000),('size',1),('version',[-1,0,0,0]),('version',[1,0,0,0])]:
            with self.subTest(field=field):
                altered=json.loads(original);altered['dlls'][0][field]=value;path.write_text(json.dumps(altered))
                with self.assertRaises(ValueError): stubs.verify_store(path)
        path.write_bytes(original)
        image=self.a/store['dlls'][0]['name'];raw=image.read_bytes();image.write_bytes(raw+b'changed')
        with self.assertRaises(ValueError): stubs.verify_store(path)
        image.write_bytes(raw)

    def test_text_census_aliases_and_implemented_forwarders(self):
        text=self.out/'input.txt';text.write_text('STATIC WSOCK32.dll -> #115, WSAStartup, #23\nDELAY kernel32.dll -> GetLastError\n')
        out=self.out/'custom';data=stubs.generate([text],out,{'kernel32.dll':{'GetLastError':'ciuki.GetLastError'}})
        ws=next(d for d in data['dlls'] if d['name']=='wsock32.dll')
        self.assertEqual([(e['name'],e['ordinal']) for e in ws['exports']],[('WSAStartup',115),('socket',23)])
        self.assertIn('GetLastError=ciuki.GetLastError',(out/'kernel32.def').read_text())
        self.assertNotIn('w32_stub_0000(void)',(out/'kernel32.c').read_text())
        for invalid in ('STATIC ../x.dll -> Bad','STATIC x.dll -> #65536','STATIC x.dll -> a";injection'):
            with self.assertRaises(ValueError): stubs.imports(invalid)

if __name__=='__main__': unittest.main()
