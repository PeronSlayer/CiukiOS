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

    def test_javascript_in_real_browser(self):
        page=self.out/'g26b-script.html'
        page.write_text('<!doctype html><html><head></head><body><div id="result">before</div><script>'
                        'document.getElementById("result").textContent="after";'
                        'document.write("<title>G26B JavaScript "+'
                        'document.getElementById("result").textContent+"</title>");'
                        '</script></body></html>')
        capture=self.out/'g26b-script.xrgb'
        result=subprocess.run([self.base/'stage/Programs/netsurf','-w','640','-h','480','-f','ram',
            '--ciuki-resources='+str(self.base/'stage/system/netsurf'),'--ciuki-capture='+str(capture),page.as_uri()],
            capture_output=True,text=True,timeout=30)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('NetSurf local title: G26B JavaScript after',result.stderr)
        self.assertEqual(capture.stat().st_size,640*480*4)
        print('PASS G26B real NetSurf Duktape executes authored DOM/title mutation')

    def test_javascript_values_on_i686(self):
        """QEMU 2026-10-25: example.com's script ended NetSurf with a fault at
        0x200 in duk_hobject_find_entry_tval_ptr_stridx. The target's x87
        code copies Duktape's packed duk_tval as a double and quiets the
        signalling-NaN tags (fastint, undefined, null, boolean, pointer,
        lightfunc): a moved integer became an object at the integer's
        address. NetSurf's own Duktape, configuration and the port's
        allocator, built with -m32 and the SDK's code-generation flags; the
        values must keep their kind (patch 0014: unpacked duk_tval)."""
        spec=importlib.util.spec_from_file_location('g26_kernel_flags',ROOT/'scripts/build_kernel.py')
        kernel=importlib.util.module_from_spec(spec);spec.loader.exec_module(kernel)
        # The SDK compiles with the kernel's flags except -mno-80387 (sdk/build_sdk.py).
        codegen=[f for f in kernel.CFLAGS if f.startswith(('-march=','-mno-sse','-mno-mmx','-O'))
                 or f in ('-fno-strict-aliasing','-fno-omit-frame-pointer')]
        self.assertIn('-mno-sse2',codegen)
        probe=self.out/'m32-probe.c';probe.write_text('int main(void){return 0;}\n')
        if subprocess.run(['clang','-m32',str(probe),'-o',str(self.out/'m32-probe')],capture_output=True).returncode:
            self.skipTest('no 32-bit C library for clang -m32 on this host')
        source=self.base/'source/netsurf';duktape=source/'content/handlers/javascript/duktape'
        common=['clang','-m32',*codegen,'-g','-std=c17','-D_POSIX_C_SOURCE=200809L','-I'+str(duktape),'-I'+str(source),'-I'+str(source/'include')]
        engine=self.out/'duktape-i686.o'
        subprocess.run([*common,'-w','-c',str(duktape/'duktape.c'),'-o',str(engine)],check=True)
        binary=self.out/'duktape-values-i686'
        subprocess.run([*common,'-DCIUKI_HOST','-Wall','-Wextra','-Werror','-I'+str(ROOT),
            str(ROOT/'tests/host/netsurf/duktape_values_test.c'),str(ROOT/'sdk/netsurf/port/js_memory.c'),str(engine),'-lm','-o',str(binary)],check=True)
        result=subprocess.run([binary],capture_output=True,text=True,timeout=60)
        print(result.stdout,end='')
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        # The target compiler itself selects the unpacked value.
        sdk=ROOT/'build/tools/ciuki-sdk/bin/ciuki-cc'
        if sdk.exists():
            check=self.out/'tval-target.c'
            check.write_text('#include "duktape.h"\n#if defined(DUK_USE_PACKED_TVAL)\n#error packed duk_tval on the target\n#endif\nint ciuki_tval_probe;\n')
            subprocess.run([str(sdk),'-D__ciuki__','-DCIUKI_NETSURF','-I'+str(duktape),'-I'+str(source),'-I'+str(source/'include'),
                            '-I'+str(source/'frontends'),'-fsyntax-only',str(check)],check=True)
            print('PASS target compiler: unpacked duk_tval')

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
            '-Wl,--gc-sections,--wrap=socket,--wrap=connect,--wrap=fcntl,--wrap=send,--wrap=recv','-o',str(binary)],check=True)
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
