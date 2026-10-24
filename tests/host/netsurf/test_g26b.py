"""Focused G26B storage and script admission; no image or guest."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[3]
class Continuation(unittest.TestCase):
    def build_run(self,name,sources,args=(),env=None):
        out=ROOT/'build/host/g26b';out.mkdir(parents=True,exist_ok=True)
        binary=out/name
        subprocess.run(['clang','-std=c17','-D_POSIX_C_SOURCE=200809L','-D_DEFAULT_SOURCE','-O1','-g','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(ROOT),
                        *map(lambda p:str(ROOT/p),sources),'-o',str(binary)],check=True)
        result=subprocess.run([binary,*args],capture_output=True,text=True,
                              env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1',**(env or {})))
        print(result.stdout,end='')
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
    def test_profile_concurrency_and_clear(self):
        out=ROOT/'build/host/g26b';out.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(dir=out) as path:
            self.build_run('profiles',['tests/host/netsurf/profile_concurrency_test.c','sdk/netsurf/port/profile.c'],[path])
    def test_proxy_handshake(self):
        self.build_run('proxy-handshake',['tests/host/netsurf/proxy_handshake_test.c'])
    def test_js_admission(self):
        self.build_run('js-memory',['tests/host/netsurf/js_memory_test.c','sdk/netsurf/port/js_memory.c'],env={'CIUKI_NETSURF_FREE_BYTES':'8192'})
if __name__=='__main__':unittest.main()
