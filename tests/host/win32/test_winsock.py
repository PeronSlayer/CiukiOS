"""Actual PE compile, stdcall exports, legacy ordinals and shared DLL forwarding."""
from pathlib import Path
import subprocess
import sys
import unittest

ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT))
from scripts.win32 import kernel32,stubs,census

class WinsockBuild(unittest.TestCase):
    def test_dlls(self):
        out=ROOT/'build/host/win32/g26-dlls';out.mkdir(parents=True,exist_ok=True)
        stubs.generate([ROOT/'docs/design/research/win32-layer-2026-10-14.md'],out/'generated')
        subprocess.run(['llvm-dlltool','-m','i386','-d',str(ROOT/'sdk/win32/ntdll/ntdll.def'),'-l',str(out/'ntdll.lib')],check=True)
        flags=['--target=i686-pc-windows-msvc','-march=pentiumpro','-ffreestanding','-fno-builtin',
            '-fno-stack-protector','-fno-omit-frame-pointer','-mno-sse','-mno-sse2','-mno-mmx',
            '-O1','-Wall','-Wextra','-Werror','-Wframe-larger-than=4096',
            '-I'+str(ROOT/'sdk/win32/include'),'-I'+str(ROOT/'src/kernel/include')]
        first=kernel32.build_offline(out,0x72000000,flags)
        second=kernel32.build_winsock2(out,0x72010000,flags)
        old=census.inspect((out/'wsock32.dll').read_bytes())
        new=census.inspect((out/'ws2_32.dll').read_bytes())
        oldnames={name:e for e in old['exports'] for name in e['names']}
        newnames={name:e for e in new['exports'] for name in e['names']}
        for name in ('socket','bind','listen','accept','connect','send','recv','sendto','recvfrom','select',
                     'WSAStartup','WSACleanup','gethostbyname','__WSAFDIsSet','WSASend','WSARecv','WSASocketA','getaddrinfo'):
            self.assertIn(name,oldnames);self.assertEqual(newnames[name]['forwarder'],'wsock32.'+name)
        self.assertEqual(oldnames['socket']['ordinal'],23)
        self.assertEqual(oldnames['__WSAFDIsSet']['ordinal'],151)
        self.assertEqual(oldnames['inet_addr']['ordinal'],10)
        self.assertEqual(newnames['inet_addr']['ordinal'],11)
        self.assertEqual(newnames['ioctlsocket']['ordinal'],10)
        self.assertLessEqual(first['size'],65536)  # its prior store slot has this alignment
        saved=[(out/name).read_bytes() for name in ('wsock32.dll','ws2_32.dll')]
        kernel32.build_offline(out,first['base'],flags);kernel32.build_winsock2(out,second['base'],flags)
        self.assertEqual(saved,[(out/name).read_bytes() for name in ('wsock32.dll','ws2_32.dll')])
        print(f"PASS G26 PE: {len(oldnames)} stdcall exports; correct wsock32/ws2_32 ordinals, all forwarders share state; sizes {first['size']}/{second['size']}; reproducible")

if __name__=='__main__':unittest.main()
