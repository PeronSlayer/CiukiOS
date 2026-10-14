"""Reproducible builds and independent LLVM/i386 ABI evidence (no PE execution)."""
# SPDX-License-Identifier: MIT
import json
import hashlib
import re
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from scripts.win32 import census, fixtures, toolchain


class FixtureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        parent = ROOT/'build/host/win32'
        parent.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='repro-',dir=parent)
        cls.addClassCleanup(cls.temp.cleanup)
        cls.first = Path(cls.temp.name)/'first'
        cls.second = Path(cls.temp.name)/'second'
        cls.hashes = fixtures.build(cls.first)
        cls.other = fixtures.build(cls.second)

    def command(self, *args):
        p = subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=30)
        self.assertEqual(p.returncode,0,p.stderr)
        return p.stdout

    def test_every_fixture_builds_twice_with_identical_hash(self):
        self.assertEqual(self.hashes,self.other)
        self.assertGreaterEqual(len(self.hashes),60)
        self.assertEqual((self.first/'hashes.json').read_bytes(),(self.second/'hashes.json').read_bytes())
        self.assertEqual(len(set(self.hashes.values())),len(self.hashes),
                         'Distinct cases must exercise distinct bytes, not duplicate the corpus count')

    def test_source_manifests_have_behavior_and_independent_oracles(self):
        specs = fixtures.manifests()
        self.assertEqual(len(specs),len(self.hashes))
        covered = {rule for spec in specs for rule in spec['rules']}
        for required in ('imports-name-hint','imports-ordinal','bound-imports','forwarder-cycles',
                         'tls-callbacks','resources','virtual-size-zero','low-alignment',
                         'seh-chain','nested-handlers','self-modifying-code','abi-stdcall',
                         'abi-cdecl','abi-varargs','abi-thiscall','abi-return64','abi-nonvolatile',
                         'abi-df','dllmain-failure-rollback','dllmain-detach-order'):
            self.assertIn(required,covered)
        for spec in specs:
            self.assertTrue(spec['runtime'])
            self.assertIn('pending',spec['evidence'])
            if 'error' in spec['census']:
                self.assertTrue(spec['census']['error'])

    def test_independent_llvm_headers_for_all_structurally_valid_fixtures(self):
        specs = {s['id']:s for s in fixtures.manifests()}
        for name in self.hashes:
            spec = specs[Path(name).stem]
            if 'error' in spec['census']:
                continue
            with self.subTest(fixture=name):
                text = self.command('llvm-readobj','--file-headers',self.first/name)
                got = census.inspect((self.first/name).read_bytes())
                self.assertIn('Machine: IMAGE_FILE_MACHINE_I386 (0x14C)',text)
                self.assertIn('Magic: 0x10B',text)
                major = re.search(r'MajorLinkerVersion: (\d+)',text).group(1)
                minor = re.search(r'MinorLinkerVersion: (\d+)',text).group(1)
                self.assertEqual(got['linker_version'],major+'.'+minor)
                self.assertEqual(got['dll'],'IMAGE_FILE_DLL ' in text)
                self.assertIn('IMAGE_SUBSYSTEM_WINDOWS_'+('GUI' if got['subsystem']=='gui' else 'CUI'),text)

    def test_independent_llvm_import_export_resource_tls_decoding(self):
        # Compare actual table records, not just detection of an MZ signature.
        text = self.command('llvm-readobj','--coff-imports',self.first/'consumer.exe')
        symbols = re.findall(r'Symbol: (.*?) \((\d+)\)',text)
        got = census.inspect((self.first/'consumer.exe').read_bytes())
        self.assertEqual(got['imports'][0]['dll'], re.search(r'Name: (.+)',text).group(1))
        self.assertEqual(got['imports'][0]['symbols'],[name or '#'+ordinal for name,ordinal in symbols])
        text = self.command('llvm-readobj','--coff-exports',self.first/'provider.dll')
        exports = re.findall(r'Ordinal: (\d+)\s+Name: (.*?)\s+RVA: (0x[0-9A-Fa-f]+)',text)
        expected = [(int(ordinal),[name] if name else []) for ordinal,name,rva in exports if int(rva,16)]
        got = census.inspect((self.first/'provider.dll').read_bytes())
        self.assertEqual([(e['ordinal'],e['names']) for e in got['exports']],expected)
        text = self.command('llvm-readobj','--coff-resources',self.first/'compiled-resources.exe')
        got = census.inspect((self.first/'compiled-resources.exe').read_bytes())
        self.assertEqual(len(got['resources']),int(re.search(r'Total Number of Resources: (\d+)',text).group(1)))
        self.assertEqual([r['language'] for r in got['resources']],list(map(int,re.findall(r'Language: \(ID (\d+)\)',text))))
        self.assertEqual([r['size'] for r in got['resources']],list(map(int,re.findall(r'DataSize: (\d+)',text))))
        text = self.command('llvm-readobj','--coff-tls-directory',self.first/'compiled-tls.exe')
        got = census.inspect((self.first/'compiled-tls.exe').read_bytes())['tls']
        start,end = [int(re.search(key+r': (0x[\dA-Fa-f]+)',text).group(1),16)
                     for key in ('StartAddressOfRawData','EndAddressOfRawData')]
        self.assertEqual(got['template_size'],end-start)
        self.assertEqual(got['zero_fill'],int(re.search(r'SizeOfZeroFill: (0x[\dA-Fa-f]+)',text).group(1),16))

    def test_i386_abi_disassembly(self):
        dis = self.command('llvm-objdump','-d',self.first/'abi-stdcall.obj')
        def body(symbol):
            return re.search(r'<'+re.escape(symbol)+r'>:\n(.*?)(?=\n[0-9a-f]+ <|\Z)',dis,re.S).group(1)
        self.assertRegex(body('_stdcall_add@8'),r'retl\s+\$0x8')
        self.assertRegex(body('_cdecl_add'),r'retl\s*\n')
        self.assertNotRegex(body('_cdecl_add'),r'retl\s+\$')
        self.assertNotRegex(body('_varargs_sum'),r'retl\s+\$')
        self.assertIn('%ecx',body('_method'))
        self.assertRegex(body('_method'),r'retl\s+\$0x4')
        self.assertRegex(body('_return64@0'),r'0x55667788, %eax')
        self.assertRegex(body('_return64@0'),r'0x11223344, %edx')
        for register in ('ebx','esi','edi','ebp'):
            self.assertRegex(body('_nonvolatile'),r'pushl\s+%'+register)
            self.assertRegex(body('_nonvolatile'),r'popl\s+%'+register)
        df = self.command('llvm-objdump','-d',self.first/'abi-df.obj')
        self.assertRegex(df,r'\bstd\b[\s\S]*\bcld\b')
        seh = self.command('llvm-objdump','-d',self.first/'seh-nested.obj')
        self.assertGreaterEqual(seh.count('%fs:'),4)
        self.assertIn('ud2',seh)
        packed = self.command('llvm-objdump','-dr',self.first/'packed-code.obj')
        self.assertRegex(packed,r'movb\s+\$0x2a')
        self.assertIn('jmp',packed)

    def test_toolchain_verifies_and_reports_pending_download(self):
        lines,errors,pending = toolchain.verify()
        self.assertEqual(errors,[])
        self.assertTrue(any('freestanding COFF' in line for line in lines))
        for message in pending:
            self.assertIn('SHA256',message)

    def test_toolchain_refuses_replacement_and_missing_tool(self):
        with patch.object(toolchain,'sha256',return_value='0'*64):
            _,errors,_ = toolchain.verify(probe=False)
            self.assertTrue(any('SHA-256 mismatch' in e for e in errors))
        with patch.object(toolchain.shutil,'which',return_value=None):
            _,errors,_ = toolchain.verify(probe=False)
            self.assertTrue(any('missing' in e for e in errors))

    def test_archive_size_and_digest_are_both_verified(self):
        root = Path(self.temp.name)/'pin-test'
        (root/'config').mkdir(parents=True)
        archive = root/'archive.tar'
        archive.write_bytes(b'local test archive')
        pin = {'path':'archive.tar','size':archive.stat().st_size,
               'sha256':hashlib.sha256(archive.read_bytes()).hexdigest(),
               'url':'https://example.invalid/archive.tar','status':'pending-download'}
        (root/'config/toolchain.json').write_text(json.dumps({'win32':{'tools':{},'archives':{'test':pin}}}))
        lines,errors,pending = toolchain.verify(root,probe=False)
        self.assertEqual((errors,pending),([],[]))
        self.assertIn('archive verified',lines[0])
        archive.write_bytes(b'x'*pin['size'])
        self.assertTrue(toolchain.verify(root,probe=False)[1])
        archive.write_bytes(b'x')
        self.assertTrue(toolchain.verify(root,probe=False)[1])
        archive.unlink()
        self.assertTrue(toolchain.verify(root,probe=False)[2])

    def test_no_payload_files_in_fixture_sources(self):
        for path in fixtures.SOURCES.iterdir():
            self.assertIn(path.suffix,('.c','.s','.def','.rc','.json','.md'))
            self.assertFalse(path.read_bytes().startswith(b'MZ'))


if __name__ == '__main__':
    unittest.main()
