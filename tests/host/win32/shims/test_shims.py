"""Production shim parser and behavioral hooks, ASan/UBSan; no PE execution."""
from pathlib import Path
import os
import subprocess
import unittest
ROOT=Path(__file__).resolve().parents[4]
class ShimTests(unittest.TestCase):
    def test_public_header_and_stub_conventions(self):
        self.assertEqual((ROOT/'src/kernel/include/ciuki/w32_diagnostics.h').read_bytes(),
                         (ROOT/'sdk/sysroot-overlay/include/ciuki/w32_diagnostics.h').read_bytes())
        from scripts.win32.stub_signatures import signature,body
        cases=[('kernel32.dll','LockFile',0,20),('ole32.dll','CoInitialize',1,4),
               ('winmm.dll','waveOutOpen',2,24),('advapi32.dll','RegOpenKeyA',3,12),
               ('kernel32.dll','FindFirstFileA',4,8),('kernel32.dll','GetThreadPriority',6,4),
               ('kernel32.dll','OpenFile',4,12),('gdi32.dll','RealizePalette',4,4)]
        for dll,name,convention,bytes_ in cases:
            with self.subTest(name=name):
                value=signature(dll,name);self.assertEqual(value['convention'],convention)
                self.assertEqual(sum(value['widths']),bytes_)
                source,target,audit=body(dll,name,'test_stub',0)
                self.assertEqual(target,f'_test_stub@{bytes_}');self.assertIn('w32_shim_stub',source)
                self.assertNotIn('__builtin_trap',source);self.assertIn('failure',audit)
        self.assertIn('__builtin_trap',body('winmm.dll','#2','opaque',0)[0])
        self.assertIn('0x400fu',body('user32.dll','DdeInitializeA','dde',0)[0])
        self.assertIn('274u',body('winmm.dll','mciSendStringA','mci',0)[0])
        for dll,name in [('gdi32.dll','GetDeviceCaps'),('gdi32.dll','SelectObject'),
                         ('user32.dll','CharLowerA'),('imm32.dll','ImmAssociateContext')]:
            self.assertIn('__builtin_trap',body(dll,name,'fatal',0)[0])
    def test_production_shims(self):
        out=ROOT/'build/host/win32/shims';out.mkdir(parents=True,exist_ok=True)
        sources=['tests/host/win32/shims/shims_test.c']+['sdk/win32/ntdll/'+s+'.c' for s in ('shims','shim_start','rtl','heap','critical')]
        sources+=['sdk/win32/kernel32/time.c']
        from scripts.win32.stub_signatures import wide_gate
        gates=out/'wide.c'
        text=['#include <w32/shims.h>','unsigned wide_real_calls;']
        # f5-20C: USER32/GDI32 W exports use the same generated gate.
        for i,(dll,name,bytes_) in enumerate([('advapi32.dll','RegOpenKeyExW',20),
                  ('version.dll','VerQueryValueW',16),('lz32.dll','LZOpenFileW',12),
                  ('user32.dll','GetMessageW',16),('gdi32.dll','TextOutW',20)]):
            params=','.join(f'unsigned a{j}' for j in range(bytes_//4))
            unused=''.join(f'(void)a{j};' for j in range(bytes_//4))
            text.append(f'unsigned __attribute__((stdcall)) w32_real_{name}({params}){{{unused}wide_real_calls++;return 17;}}')
            text.append(wide_gate(dll,name,bytes_,i))
        gates.write_text('\n'.join(text));sources.append(str(gates))
        program=out/'shims'
        subprocess.run(['clang','-std=c17','-O1','-g','-DW32_HOST_MODEL','-Wall','-Wextra','-Werror',
            '-Wno-ignored-attributes','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-fsanitize=address,undefined',
            '-I'+str(ROOT/'sdk/win32/include'),'-I'+str(ROOT/'src/kernel/include'),
            *[str(ROOT/s) for s in sources],'-o',str(program)],check=True)
        result=subprocess.run([program],capture_output=True,text=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('PASS all 12',result.stdout)
