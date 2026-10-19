"""Production input engine with ASan/UBSan, actual v3 parsing and PE32 ABI."""
import json
import os
from pathlib import Path
import subprocess
import unittest
from scripts.win32 import user32
ROOT=Path(__file__).resolve().parents[4]
HERE=Path(__file__).resolve().parent

class InputTests(unittest.TestCase):
    def test_production_input_sessions(self):
        out=ROOT/'build/host/win32/input';out.mkdir(parents=True,exist_ok=True)
        table=json.loads((HERE/'tables.json').read_text())
        session=json.loads((HERE/'sessions.json').read_text())
        vectors=[]
        for name,rows,typ in [('pinned_keys',table['keys'],'unsigned'),('pinned_chars',table['characters'],'unsigned'),('raw_session',session['relative'],'int')]:
            vectors.append('static const '+typ+' '+name+'[]['+str(len(rows[0]))+']={'+','.join('{'+','.join(map(str,r))+'}' for r in rows)+'};')
        (out/'input_tables.h').write_text('\n'.join(vectors)+'\n')
        # Original USER fixture supplies independent scheduler/transport adapters.
        original=json.loads((ROOT/'tests/host/win32/user32/session.json').read_text())
        vectors=[]
        for layout in ('us','it'):
            vectors.append('static const unsigned '+layout+'[][6]={'+','.join('{'+','.join(map(str,r[:-1]))+','+str(int(r[-1],16))+'}' for r in original[layout])+'};')
        for name in ('scans','us','it'):
            vectors.append('static const unsigned vk_'+name+'[]={'+','.join(map(str,original['virtual_keys'][name]))+'};')
        (out/'session.h').write_text('\n'.join(vectors)+'\n');user32.icon_header(out)
        flags=['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror','-fno-pie',
               '-ffunction-sections','-fdata-sections','-fsanitize=address,undefined','-fno-sanitize-recover=all',
               '-I'+str(ROOT/'src/kernel/include')]
        objects=[]
        bridge=out/'kernel.o'
        subprocess.run([*flags,'-c',str(ROOT/'tests/host/win32/user32/kernel_bridge.c'),'-o',str(bridge)],check=True);objects.append(bridge)
        sources=sorted((ROOT/'sdk/win32/user32').glob('*.c'))+sorted((ROOT/'sdk/win32/input').glob('*.c'))+[
            ROOT/'apps/libdeskui/v3.c',ROOT/'apps/desktop/protocol.c',ROOT/'apps/desktop/keymap.c',ROOT/'sdk/win32/services/bytes.c',HERE/'session_test.c',
            *[ROOT/'apps/w32srv'/n for n in ('registry.c','storage.c','directory.c','defaults.c','protocol.c','posix_io.c')]]
        for i,source in enumerate(sources):
            obj=out/f'{i}.o';objects.append(obj)
            subprocess.run([*flags,'-DW32_HOST_MODEL','-I'+str(ROOT/'sdk/win32/include'),'-I'+str(out),'-c',str(source),'-o',str(obj)],check=True)
        binary=out/'input'
        subprocess.run([*flags,'-no-pie','-Wl,--gc-sections',*map(str,objects),str(ROOT/'src/kernel/fs/path.c'),
            str(ROOT/'tests/host/proc/signal_legacy.c'),str(ROOT/'tests/host/proc/desktop_legacy.c'),'-pthread','-o',str(binary)],check=True)
        result=subprocess.run([binary],capture_output=True,text=True,timeout=30,
            env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0:detect_stack_use_after_return=0'))
        (out/'trace.log').write_text(result.stdout+result.stderr)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('PASS INPUT production USER/v3 DirectInput',result.stdout);print(result.stdout,end='')

    def test_pe32_abi(self):
        out=ROOT/'build/host/win32/input';out.mkdir(parents=True,exist_ok=True)
        for target in ('i686-pc-windows-msvc','i686-w64-mingw32'):
            subprocess.run(['clang','--target='+target,'-ffreestanding','-Wall','-Wextra','-Werror',
                '-I'+str(ROOT/'sdk/win32/include'),'-I'+str(ROOT/'src/kernel/include'),'-c',str(HERE/'abi.c'),'-o',str(out/(target+'.obj'))],check=True)

    def test_real_pe_input_fixture_and_census(self):
        import shutil
        from scripts.win32 import builtins,census,stubs
        from tests.host.win32.adapter import build
        out=ROOT/'build/host/win32/input/pe';out.mkdir(parents=True,exist_ok=True)
        store=out/'store';manifest=builtins.build(store)
        self.assertGreater(stubs.verify_store(manifest),0)
        for dll in ('dinput','dinput8'):
            coverage=json.loads((store/(dll+'-coverage.json')).read_text())
            names={n for e in census.inspect((store/(dll+'.dll')).read_bytes())['exports'] for n in e['names']}
            self.assertTrue({e['name'] for e in coverage['exports']}<=names)
            self.assertTrue(all(e['status'] in ('implemented','W21 stub') for e in coverage['exports']))
            shutil.copyfile(store/(dll+'.dll'),out/(dll+'.dll'))
        forward=census.inspect((out/'dinput8.dll').read_bytes())['exports']
        self.assertEqual(next(e['forwarder'] for e in forward if 'DirectInput8Create' in e['names']),'dinput.DirectInput8Create')
        host=build.build(out/'core')
        shutil.copyfile(out/'core/i686-pc-windows-msvc/ntdll.dll',out/'ntdll.dll')
        flags=['--target=i686-pc-windows-msvc','-march=pentiumpro','-ffreestanding','-fno-builtin',
            '-fno-stack-protector','-mno-sse','-mno-sse2','-mno-mmx','-O1','-Wall','-Wextra','-Werror',
            '-I'+str(ROOT/'sdk/win32/include'),'-I'+str(ROOT/'src/kernel/include')]
        def compile(source,name):
            obj=out/name;subprocess.run(['clang',*flags,'-c',str(source),'-o',str(obj)],check=True);return obj
        # The f5-06b loader maps non-builtin images only below the builtin region
        # (0x70000000..); the adapter run loads these copies as plain files, so
        # rebuild dinput/dinput8 at a low base with the same sources and flags.
        from scripts.win32 import input as inputmod
        inputmod.build(store,0x60100000,flags)  # rebuilds in the store directory, which holds the builder's inputs
        for dll in ('dinput','dinput8'):
            shutil.copyfile(store/(dll+'.dll'),out/(dll+'.dll'))
        adapter=compile(HERE/'pe_adapter.c','adapter.obj')
        exports={'GetProcessHeap':0,'HeapAlloc':12,'HeapFree':12,'SetEvent':4,'GetCurrentProcessId':0,'SetLastError':4,
            'CiukiUserInputLock':0,'CiukiUserInputUnlock':0,'CiukiUserInputPump':0,'CiukiUserInputSubscribe':12,
            'CiukiUserInputSnapshot':4,'CiukiUserInputPointer':8,'IsWindow':4,'GetWindowLongA':8,'GetWindowThreadProcessId':8,'GetKeyNameTextA':12,'AdapterInput':16}
        definition=out/'adapter.def';definition.write_text('LIBRARY kernel32.dll\nEXPORTS\n'+''.join(f' {n}=_{n}@{a}\n' for n,a in exports.items()))
        subprocess.run(['lld-link','/machine:x86','/dll','/subsystem:windows,4.0','/osversion:4.0','/noentry','/nodefaultlib','/safeseh:no','/fixed',
            '/base:0x70800000','/timestamp:0','/def:'+str(definition),'/out:'+str(out/'kernel32.dll'),str(adapter),
            str(out/'core/i686-pc-windows-msvc/ntdll.lib'),str(store/'input-objects/bytes.obj')],check=True)
        definition=out/'user-adapter.def';definition.write_text('LIBRARY user32.dll\nEXPORTS\n'+''.join(f' {n}=kernel32.{n}\n' for n in exports if n.startswith('Ciuki') or n in ('IsWindow','GetWindowLongA','GetWindowThreadProcessId','GetKeyNameTextA')))
        # lld-link emits only the import library when no object is given: give the
        # forwarder-only USER shim one empty object so user32.dll really exists and
        # the adapter can register it as a builtin (core names resolve from builtins).
        (out/'user-shim.c').write_text('int ciuki_input_user_shim;\n')
        shim=compile(out/'user-shim.c','user-shim.obj')
        subprocess.run(['lld-link','/machine:x86','/dll','/subsystem:windows,4.0','/osversion:4.0','/noentry','/nodefaultlib','/fixed','/base:0x70900000',
            '/def:'+str(definition),'/out:'+str(out/'user32.dll'),str(shim)],check=True)
        self.assertTrue((out/'user32.dll').is_file())
        objects=[compile(HERE/'fixture.c','fixture.obj'),compile(ROOT/'sdk/win32/input/formats.c','formats.obj')]
        libraries=[]
        for dll,names in [('dinput',{'DirectInputCreateA':16}),('dinput8',{'DirectInput8Create':20}),('kernel32',{'AdapterInput':16})]:
            definition=out/(dll+'-import.def');definition.write_text('LIBRARY '+dll+'.dll\nEXPORTS\n'+''.join(f' {n}@{a}\n' for n,a in names.items()))
            library=out/(dll+'-import.lib');libraries.append(str(library))
            subprocess.run(['llvm-dlltool','-m','i386','-k','-d',str(definition),'-l',str(library)],check=True)
        fixture=out/'input.exe'
        subprocess.run(['lld-link','/machine:x86','/entry:entry','/nodefaultlib','/safeseh:no','/timestamp:0',
            '/subsystem:console,4.0','/base:0x400000','/out:'+str(fixture),*map(str,objects),*libraries],check=True)
        import signal
        probe=subprocess.run([host,'validate',store/'fixtures/console.exe'],capture_output=True,text=True,timeout=10)
        if probe.returncode==-signal.SIGSYS:
            (out/'trace.log').write_text('H1 blocked: baseline f5-02 Linux i386 adapter receives SIGSYS before PE execution.\n')
            self.skipTest('Linux sandbox denies baseline i386 adapter syscall (SIGSYS); PE execution unverified')
        self.assertEqual(probe.returncode,0,probe.stdout+probe.stderr)
        result=subprocess.run([host,'exec-core',fixture,out],capture_output=True,text=True,timeout=30)
        (out/'trace.log').write_text(result.stdout+result.stderr)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('RESULT 0',result.stdout);self.assertIn('PASS load',result.stdout)
        print('PASS authored PE32 input fixture on f5-02 Linux adapter')
