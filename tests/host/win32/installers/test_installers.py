"""Production shell/registry on the Linux file adapter, with independent output."""
# SPDX-License-Identifier: LGPL-2.1-or-later
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[4]

class InstallerShellTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.out = ROOT/'build/host/win32/installers'
        cls.out.mkdir(parents=True, exist_ok=True)
        cls.program = cls.out/'shell-test'
        cls.sanitized = cls.out/'shell-test-asan'
        sources = [ROOT/'sdk/win32/shell'/n for n in
            ('folders.c','pidl.c','link.c','execute.c','fileop.c','strings.c')]
        sources += [ROOT/'sdk/win32/services/bytes.c',
            ROOT/'sdk/win32/kernel32/text.c',ROOT/'apps/desktop/protocol.c']
        sources += [ROOT/'sdk/win32/version'/n for n in ('version.c','install.c')]
        sources += [ROOT/'sdk/win32/advapi32'/n for n in ('registry.c','text.c')]
        sources += [ROOT/'apps/w32srv'/n for n in
            ('registry.c','storage.c','defaults.c','directory.c','posix_io.c')]
        sources += [ROOT/'tests/host/win32/installers'/n for n in ('host.c','shell_test.c')]
        command = ['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror',
            '-DW32_HOST_MODEL','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
            '-fno-sanitize-recover=all',
            '-I'+str(ROOT/'sdk/win32/include'),'-I'+str(ROOT/'src/kernel/include'),
            *map(str,sources)]
        subprocess.run([*command,'-fsanitize=undefined','-o',str(cls.program)],check=True)
        subprocess.run([*command,'-fsanitize=address,undefined','-o',str(cls.sanitized)],check=True)

    def test_address_and_leak_sanitizers(self):
        # Keep leak detection enabled. The sandbox's ptrace denial is an
        # explicit unavailable check, never a sanitizer-options workaround.
        with tempfile.TemporaryDirectory(dir=self.out) as name:
            p=subprocess.run([self.sanitized,name],capture_output=True,text=True,timeout=60)
            if 'LeakSanitizer has encountered a fatal error' in p.stderr:
                self.skipTest('LeakSanitizer cannot inspect threads in this sandbox; leak detection was not disabled')
            self.assertEqual(p.returncode,0,p.stdout+p.stderr)

    def test_shell_flows_and_independent_manifests(self):
        with tempfile.TemporaryDirectory(dir=self.out) as name:
            result = subprocess.run([self.program,name],capture_output=True,text=True,timeout=60)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            print(result.stdout,end='')
            root = Path(name)
            expected = json.loads((ROOT/'tests/host/win32/installers/expected.json').read_text())
            actual = {str(p.relative_to(root/'C')):hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in (root/'C').rglob('*') if p.is_file() and p.suffix!='.lnk'}
            self.assertEqual(actual,expected['files'])
            self.assertEqual((root/'spawn.txt').read_text().splitlines(),expected['spawns'])
            link = (root/'C/WINDOWS/Desktop/Fixture.lnk').read_bytes()
            self.assertEqual(link[:8],b'CIUKLNK3')
            self.assertEqual(struct.unpack_from('<Ii',link,8),(3,-3))
            lengths = struct.unpack_from('<5I',link,16)
            at,fields = 40,[]
            for n in lengths:
                fields.append(link[at:at+n].decode('utf-8'))
                at += n
            self.assertEqual(at,len(link))
            self.assertEqual(fields,expected['link'])

    def test_i386_shell_layout_and_support_pe(self):
        from scripts.win32 import shell,census
        flags = ['--target=i686-pc-windows-msvc','-march=pentiumpro','-ffreestanding',
            '-fno-builtin','-fno-stack-protector','-mno-sse','-mno-sse2','-mno-mmx',
            '-O1','-Wall','-Wextra','-Werror','-I'+str(ROOT/'sdk/win32/include'),
            '-I'+str(ROOT/'src/kernel/include')]
        record = shell.build(self.out,0x73000000,flags)
        pe = census.inspect((self.out/record['path']).read_bytes())
        exports = {name for e in pe['exports'] for name in e['names']}
        for name in ('SHFileOperationA','SHGetFolderPathA','ShellExecuteExA','DllGetClassObject','SHGetMalloc'):
            self.assertIn(name,exports)
        source = self.out/'layout.c'
        source.write_text('#include <w32/shell.h>\n'
            '_Static_assert(sizeof(SHELL_FILEOP)==30,"SHFILEOPSTRUCTA");\n'
            '_Static_assert(offsetof(SHELL_FILEOP,aborted)==18,"pack(1) WORD flags");\n'
            '_Static_assert(sizeof(SHELL_EXECUTE)==60,"SHELLEXECUTEINFOA");\n'
            '_Static_assert(sizeof(struct shell_startup)==68,"STARTUPINFOA");\n')
        subprocess.run(['clang',*flags,'-c',str(source),'-o',str(self.out/'layout.obj')],check=True)

    def test_dxsetup_name_and_validated_pe_identity(self):
        from tests.host.win32.adapter import build
        model=build.build_model(self.out/'intercept',ROOT/'tests/host/win32/installers/intercept.c')
        source=self.out/'dxsetup.c'
        source.write_text('int __attribute__((stdcall)) entry(void){return 99;}\n')
        obj=self.out/'dxsetup.obj'
        subprocess.run(['clang','--target=i686-pc-windows-msvc','-ffreestanding','-c',source,'-o',obj],check=True)
        for ident,company,product,original,expected in (
            ('accepted','Microsoft Corporation','Microsoft DirectX','DXSETUP.EXE',1),
            ('wrong-company','CiukiOS','Microsoft DirectX','DXSETUP.EXE',0),
            ('wrong-product','Microsoft Corporation','Other Setup','DXSETUP.EXE',0),
            ('wrong-original','Microsoft Corporation','Microsoft DirectX','OTHER.EXE',0)):
            folder=self.out/ident
            folder.mkdir(exist_ok=True)
            rc=folder/'identity.rc'
            rc.write_text('1 VERSIONINFO\nFILEVERSION 4,7,0,716\nFILEOS 0x40004\nFILETYPE 1\n'
                'BEGIN\n BLOCK "StringFileInfo"\n BEGIN\n BLOCK "040904E4"\n BEGIN\n'+
                f'VALUE "OriginalFilename", "{original}\\0"\nVALUE "CompanyName", "{company}\\0"\nVALUE "ProductName", "{product}\\0"\n'+
                'END\n END\n END\n')
            res=folder/'identity.res'
            subprocess.run(['llvm-rc','/fo',res,rc],check=True)
            image=folder/'DXSETUP.EXE'
            subprocess.run(['lld-link','/machine:x86','/nodefaultlib','/safeseh:no','/entry:entry@0',
                '/subsystem:windows,4.0','/osversion:4.0','/timestamp:0','/out:'+str(image),obj,res],check=True)
            p=subprocess.run([model,image,str(expected)],capture_output=True,text=True,timeout=20)
            self.assertEqual(p.returncode,0,p.stdout+p.stderr)
            renamed=folder/'OTHER.EXE'
            renamed.write_bytes(image.read_bytes())
            p=subprocess.run([model,renamed,'0'],capture_output=True,text=True,timeout=20)
            self.assertEqual(p.returncode,0,p.stdout+p.stderr)

if __name__=='__main__':
    unittest.main()
