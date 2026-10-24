"""Original USER facade through the production kernel queues and v3 parser."""
import json
import os
from pathlib import Path
import subprocess
import unittest
from scripts.win32 import user32
ROOT=Path(__file__).resolve().parents[4]
HERE=Path(__file__).resolve().parent

def build_fixture(out):
    """Production USER, gdi32 and DirectDraw (native adapter included) with the
    fixture transports. Returns the ASan/UBSan binary."""
    from scripts.win32 import ddraw, gdi32
    out.mkdir(parents=True,exist_ok=True)
    session=json.loads((HERE/'session.json').read_text())
    vectors=[]
    for layout in ('us','it'):
        rows=[','.join(map(str,row[:-1]))+','+str(int(row[-1],16)) for row in session[layout]]
        vectors.append('static const unsigned '+layout+'[][6]={'+','.join('{'+r+'}' for r in rows)+'};')
    for key in ('scans','us','it'):
        vectors.append('static const unsigned vk_'+key+'[]={'+','.join(map(str,session['virtual_keys'][key]))+'};')
    (out/'session.h').write_text('\n'.join(vectors)+'\n')
    user32.icon_header(out)
    flags=['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror','-fno-pie',
        '-ffunction-sections','-fdata-sections','-fsanitize=address,undefined','-fno-sanitize-recover=all',
        # GetProcAddress binding calls exports through the importer's own
        # prototype (DWORD gdi32 handles), exactly as the PE32 build does.
        '-fno-sanitize=function','-I'+str(ROOT/'src/kernel/include')]
    bridge=out/'kernel.o'
    subprocess.run([*flags,'-c',str(ROOT/'tests/host/win32/user32/kernel_bridge.c'),'-o',str(bridge)],check=True)
    objects=[bridge]
    sources=sorted((ROOT/'sdk/win32/user32').glob('*.c'))+[
        ROOT/'apps/libdeskui/v3.c',ROOT/'apps/libdeskui/clipstore.c',ROOT/'apps/desktop/protocol.c',ROOT/'apps/desktop/keymap.c',
        ROOT/'sdk/win32/services/bytes.c',HERE/'user32_test.c',
        *[ROOT/'apps/w32srv'/n for n in ('registry.c','storage.c','directory.c','defaults.c','protocol.c','posix_io.c','progman.c','restart.c','shell_fs.c')]]
    for i,source in enumerate(sources):
        obj=out/f'{i}.o';objects.append(obj)
        subprocess.run([*flags,'-DW32_HOST_MODEL','-I'+str(ROOT/'sdk/win32/include'),'-I'+str(out),
            '-c',str(source),'-o',str(obj)],check=True)
    # gdi32/ddraw use DWORD handles: separate translation units, and any global
    # name USER also defines (FillRect, FrameRect...) stays private to them.
    engine=[ROOT/'sdk/win32/gdi32'/n for n in gdi32.SOURCES if not n.startswith('compiler')]
    engine+=[ROOT/'sdk/win32/ddraw'/n for n in ddraw.SOURCES]
    engine+=[ROOT/'tests/host/win32/ddraw/shim.c',HERE/'gdi_host.c',HERE.parent/'gdi_support.c',HERE/'ddraw_seam.c']
    defined=set()
    for obj in objects:
        nm=subprocess.run(['llvm-nm','--defined-only','--extern-only','-j',str(obj)],capture_output=True,text=True,check=True)
        defined|=set(nm.stdout.split())
    for i,source in enumerate(engine):
        obj=out/f'engine-{i}.o';objects.append(obj)
        subprocess.run([*flags,'-D_GNU_SOURCE','-DW32_HOST_MODEL','-fno-builtin','-I'+str(ROOT/'sdk/win32/include'),
            '-idirafter',str(ROOT/'sdk/sysroot-overlay/include'),'-c',str(source),'-o',str(obj)],check=True)
        nm=subprocess.run(['llvm-nm','--defined-only','--extern-only','-j',str(obj)],capture_output=True,text=True,check=True)
        clash=sorted(set(nm.stdout.split())&defined)
        if clash:
            subprocess.run(['llvm-objcopy',*[f'--localize-symbol={n}' for n in clash],str(obj)],check=True)
    binary=out/'user32'
    subprocess.run([*flags,'-no-pie','-Wl,--gc-sections',*map(str,objects),
        str(ROOT/'src/kernel/fs/path.c'),str(ROOT/'tests/host/proc/signal_legacy.c'),
        str(ROOT/'tests/host/proc/desktop_legacy.c'),'-pthread','-o',str(binary)],check=True)
    return binary

def run_fixture(out,binary):
    # The fixture's server state lives under build/host/win32/user32 (mkdtemp in
    # user32_test.c run()); create it so callers outside this module do not
    # depend on test discovery order.
    (ROOT/'build/host/win32/user32').mkdir(parents=True,exist_ok=True)
    trace=out/'trace.log'
    with trace.open('w') as log:
        result=subprocess.run([binary],stdout=log,stderr=subprocess.STDOUT,text=True,timeout=60,
            env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0:detect_stack_use_after_return=0'))
    return result.returncode,trace.read_text()

class User32Tests(unittest.TestCase):
    def test_production_user32(self):
        out=ROOT/'build/host/win32/user32'
        code,output=run_fixture(out,build_fixture(out))
        self.assertEqual(code,0,output)
        self.assertIn('PASS USER production kernel queues and teardown',output)
        self.assertIn('[win32] exit pid=',output)
        for line in ('TRACE window order','TRACE display seam','TRACE lifecycle','TRACE SystemParametersInfo',
                     'TRACE CF_BITMAP','TRACE DrawIconEx','TRACE palette callbacks','TRACE ddraw on USER seam'):
            self.assertIn(line,output)
        self.assertIn('PASS USER windows messages input paint clipboard',output)
        print(output,end='')

    def test_public_pe32_layout(self):
        out=ROOT/'build/host/win32/user32';out.mkdir(parents=True,exist_ok=True)
        source=out/'layout.c'
        source.write_text('#include <w32/user_surface.h>\n'
            '_Static_assert(sizeof(MSG)==28,"MSG");\n'
            '_Static_assert(sizeof(WNDCLASSA)==40,"WNDCLASSA");\n'
            '_Static_assert(sizeof(WNDCLASSEXA)==48,"WNDCLASSEXA");\n'
            '_Static_assert(sizeof(CREATESTRUCTA)==48,"CREATESTRUCTA");\n'
            '_Static_assert(sizeof(PAINTSTRUCT)==64,"PAINTSTRUCT");\n'
            '_Static_assert(sizeof(ICONINFO)==20,"ICONINFO");\n'
            '_Static_assert(sizeof(struct u32_surface)==24,"surface");\n'
            '_Static_assert(__builtin_offsetof(struct u32_gdi,window_pixel_format)==36,"GDI v1 prefix");\n'
            '_Static_assert(sizeof(struct u32_gdi)==40,"GDI v2 interface");\n')
        subprocess.run(['clang','--target=i686-pc-windows-msvc','-ffreestanding','-Wall','-Wextra','-Werror',
            '-I'+str(ROOT/'sdk/win32/include'),'-I'+str(ROOT/'src/kernel/include'),
            '-c',str(source),'-o',str(out/'layout.obj')],check=True)

    def test_real_pe_census_and_unicode_exports(self):
        from scripts.win32 import builtins,census,stubs
        out=ROOT/'build/host/win32/user32/store'
        manifest=builtins.build(out)
        self.assertGreater(stubs.verify_store(manifest),0)
        record=json.loads((out/'user32-coverage.json').read_text())
        exports={n for e in census.inspect((out/'user32.dll').read_bytes())['exports'] for n in e['names']}
        self.assertTrue({row['name'] for row in record['exports']}<=exports)
        self.assertTrue(set(record['unicode_stubs'])<=exports)
        self.assertTrue({'GetMessageA','SendMessageA','BeginPaint','CiukiUserSetGdi','w32_user_display_lease',
            'w32_user_display_release','w32_user_display_poll','w32_user_display_clip','w32_user_thread_cleanup'}<=exports)
        # Amendment 1 item 3: USER and kernel32 (advapi32: test_registry) share
        # one service endpoint through ntdll's w32_server_exchange, which holds
        # FastPebLock across the whole BEGIN/WRITE/READ transaction.
        for dll in ('user32','kernel32'):
            imports=census.inspect((out/(dll+'.dll')).read_bytes())['imports']
            self.assertTrue(any(i['dll'].lower()=='ntdll.dll' and 'w32_server_exchange' in i['symbols'] for i in imports),dll)
        self.assertTrue(all(row['status'] in ('implemented','W21 stub') for row in record['exports']))
        # W21 failures must return through their declared ABI; no placeholder trap.
        source=(out/'user32-objects/remainder.c').read_text()
        self.assertNotIn('__builtin_trap',source)
        self.assertIn('SetLastError(120)',source)
        self.assertIn('return (uintptr_t)-1;',source)
        spec=user32.inventory()
        self.assertEqual(spec['GetMessageW'],('stdcall',16))
        self.assertEqual(spec['wsprintfA'],('varargs',8))

        # Execute the generated production stub bodies through their actual
        # native model prototypes. PE32 layout/stack arities are checked above.
        import re
        # Menu APIs are implemented by f5-10C; WaitForInputIdle, GetMessageW
        # and the DDEML by f5-20B. The remaining typed refusals.
        targets={'ChangeDisplaySettingsA':-1,'RegisterHotKey':0,
                 'ExcludeUpdateRgn':0}
        definition=(out/'user32.def').read_text()
        declarations=[];calls=[]
        for i,(name,value) in enumerate(targets.items()):
            match=re.search(r'^ '+name+r'=_?(u32_stub_\d+)@(\d+)$',definition,re.M)
            self.assertIsNotNone(match,name)
            count=int(match[2])//4
            declarations.append('extern uintptr_t '+match[1]+'('+','.join(['uintptr_t']*count)+');')
            call=match[1]+'('+','.join(['0']*count)+')'
            expected='(uintptr_t)-1' if value<0 else str(value)
            calls.append('for(int repeat=0;repeat<2;repeat++){last=0;if('+call+'!='+expected+'||last!=120)return '+str(i+1)+';}')
        driver=out/'typed-w21.c'
        driver.write_text('#include <w32/kernel32.h>\n#include <stdio.h>\n'
            'static unsigned last,diagnostics; void SetLastError(DWORD n){last=n;}\n'
            'void w32_diag(const char *event,const char *dll,const char *name){(void)event;(void)dll;(void)name;diagnostics++;}\n'
            +'\n'.join(declarations)+'\nint main(void){'+''.join(calls)+
            'if(diagnostics!='+str(len(targets))+')return 99;puts("PASS typed USER W21 failure returns");return 0;}\n')
        binary=out/'typed-w21'
        subprocess.run(['clang','-std=c17','-O1','-Wall','-Wextra','-Werror','-DW32_HOST_MODEL',
            '-fsanitize=address,undefined','-fno-sanitize-recover=all',
            '-I'+str(ROOT/'sdk/win32/include'),'-I'+str(ROOT/'src/kernel/include'),
            str(out/'user32-objects/remainder.c'),str(driver),'-o',str(binary)],check=True)
        result=subprocess.run([binary],capture_output=True,text=True,
            env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('PASS typed USER W21 failure returns',result.stdout)
