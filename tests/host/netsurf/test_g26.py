"""G26 decoders and their registration in the real browser, no guest."""
import importlib.util
import os
from pathlib import Path
import struct
import subprocess
import unittest

ROOT=Path(__file__).resolve().parents[3]

class BrowserUserTools(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec=importlib.util.spec_from_file_location('g26_netsurf',ROOT/'sdk/netsurf/build.py')
        cls.recipe=importlib.util.module_from_spec(spec);spec.loader.exec_module(cls.recipe)
        cls.recipe.build(host=True)
        cls.base=ROOT/'build/external/netsurf-host'
        cls.out=ROOT/'build/host/g26-browser';cls.out.mkdir(parents=True,exist_ok=True)

    def test_png_jpeg(self):
        binary=self.out/'raster'
        subprocess.run(['clang','-std=c17','-D_POSIX_C_SOURCE=200809L','-O1','-g','-Wall','-Wextra','-Werror',
            '-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(ROOT),
            '-I'+str(self.base/'prefix/include'),str(ROOT/'tests/host/netsurf/raster_test.c'),
            str(ROOT/'sdk/netsurf/port/raster.c'),str(self.base/'prefix/lib/libpng.a'),str(self.base/'prefix/lib/libz.a'),
            '-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc','-lm','-o',str(binary)],check=True)
        result=subprocess.run([binary,self.out,ROOT/'tests/host/netsurf/fixtures/g26-colours.jpg',
            ROOT/'tests/host/netsurf/fixtures/g26-progressive.jpg'],capture_output=True,text=True,
            env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1'))
        print(result.stdout,end='')
        # Run the browser even when the sandbox's leak-check exit is unavailable;
        # report both outcomes, and keep the sanitizer failure as a failed test.
        if 'PASS G26 JPEG' in result.stdout:
            for image in ('colours.png','colours.jpg','progressive.jpg'):
                page=self.out/'images.html'
                page.write_text('<!doctype html><title>G26 PNG/JPEG</title><style>body{margin:0}</style>'
                    '<img src="'+image+'" width="64" height="64">')
                capture=self.out/'images.xrgb'
                browser=subprocess.run([self.base/'stage/Programs/netsurf','-w','640','-h','480','-f','ram',
                    '--ciuki-resources='+str(self.base/'stage/system/netsurf'),'--ciuki-capture='+str(capture),page.as_uri()],
                    capture_output=True,text=True,timeout=30)
                self.assertEqual(browser.returncode,0,browser.stdout+browser.stderr)
                pixels=[p[0]&0xffffff for p in struct.iter_unpack('<I',capture.read_bytes())]
                self.assertGreater(sum((p>>16)>=250 and ((p>>8)&255)<=5 and (p&255)<=5 for p in pixels),1000,image)
                self.assertGreater(sum((p&255)>=250 and ((p>>8)&255)<=5 and (p>>16)<=5 for p in pixels),1000,image)
            print('PASS G26 real browser PNG/JPEG content handlers rendered authored colour fixtures')
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_proxy(self):
        binary=self.out/'proxy';source=self.base/'source/netsurf'
        subprocess.run(['clang','-std=c17','-D_POSIX_C_SOURCE=200809L','-DCIUKI_HOST','-O1','-g','-Wall','-Wextra','-Werror',
            '-ffunction-sections','-fdata-sections','-fsanitize=address,undefined','-fno-sanitize-recover=all',
            '-include',str(ROOT/'sdk/netsurf/port/host_network.h'),'-I'+str(ROOT),
            '-I'+str(self.base/'native-headers'),'-I'+str(self.base/'prefix/include'),'-I'+str(source),'-I'+str(source/'include'),
            str(ROOT/'tests/host/netsurf/proxy_test.c'),str(ROOT/'sdk/netsurf/port/http.c'),str(self.base/'prefix/lib/libz.a'),
            '-Wl,--gc-sections,--wrap=socket,--wrap=connect,--wrap=fcntl','-o',str(binary)],check=True)
        result=subprocess.run([binary],capture_output=True,text=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1'))
        print(result.stdout,end='')
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_profile_storage(self):
        import tempfile
        binary=self.out/'profile'
        subprocess.run(['clang','-std=c17','-D_POSIX_C_SOURCE=200809L','-O1','-g','-Wall','-Wextra','-Werror',
            '-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(ROOT),
            str(ROOT/'tests/host/netsurf/profile_test.c'),str(ROOT/'sdk/netsurf/port/profile.c'),'-o',str(binary)],check=True)
        with tempfile.TemporaryDirectory(dir=self.out) as directory:
            result=subprocess.run([binary,directory],capture_output=True,text=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1'))
            print(result.stdout,end='')
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)

if __name__=='__main__':unittest.main()
